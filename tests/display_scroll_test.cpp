// Exercise the production LCD shadow/scroll logic with no-op SPI/GPIO.
#include <cassert>
#include <cstdio>
#include <string>

// display.cpp cooperatively services audio during long PAINT operations.
// The host scroll regression has no audio backend, so provide a no-op stub.
namespace rmb::platform { void audio_service() {} }

#include "../src/platform/picocalc/display.cpp"

int main() {
    using namespace rmb::picocalc::display;
    clear();

    // STANDBY must stop the LCD controller, not merely request backlight zero.
    // Repeated entry/leave calls are idempotent so nested UI paths cannot send
    // an invalid DCS sequence.
    test_spi_begin_capture();
    test_sleep_begin_capture();
    enter_standby();
    enter_standby();
    assert(test_spi_single_write_count == 2);
    assert(test_spi_single_writes[0] == 0x28); // Display OFF
    assert(test_spi_single_writes[1] == 0x10); // Sleep IN
    assert(test_sleep_count == 1);
    assert(test_sleep_values[0] == 120);

    leave_standby();
    leave_standby();
    assert(test_spi_single_write_count == 4);
    assert(test_spi_single_writes[2] == 0x11); // Sleep OUT
    assert(test_spi_single_writes[3] == 0x29); // Display ON
    assert(test_sleep_count == 3);
    assert(test_sleep_values[1] == 120);
    assert(test_sleep_values[2] == 20);
    test_spi_end_capture();
    test_sleep_end_capture();

    assert(scroll_top_pixels() == 24);
    assert(console_bottom_pixels() == 312);
    for (int row = 0; row < 3; ++row)
        draw_text_row(row, "STATUS", 0xffffff, 0);
    draw_text_row(39, "FKEYS", 0xffffff, 0);
    for (int i = 0; i < 150; ++i) {
        set_cursor_position(0, 38);
        put_string("BODY\n");
        for (int row = 0; row < 3; ++row) {
            assert(std::string(text_shadow[row], 6) == "STATUS");
            assert(text_physical_y(row * 8) == row * 8);
        }
        assert(std::string(text_shadow[39], 5) == "FKEYS");
        assert(cursor_row() >= 3 && cursor_row() < 39);
    }
    scroll_text_rows(40);
    assert(std::string(text_shadow[2], 6) == "STATUS");
    assert(std::string(text_shadow[39], 5) == "FKEYS");
    set_status_area_enabled(false);
    assert(scroll_top_pixels() == 0);
    set_status_area_enabled(true);
    assert(scroll_top_pixels() == 24);
    set_function_key_bar_enabled(false);
    assert(console_bottom_pixels() == 320);
    set_function_key_bar_enabled(true);
    assert(console_bottom_pixels() == 312);
    for (int start : {3, 36, 37, 38, 39}) {
        set_function_key_bar_enabled(false);
        clear();
        set_cursor_position(0, start);
        put_string("***\r\n[RUN] 0m 0.112s\r\n");
        set_function_key_bar_enabled(true);
        draw_text_row(39, "FKEYS", 0xffffff, 0);
        clear_to_eol();
        put_string("BASIC> ");
        const int prompt = cursor_row();
        assert(std::string(text_shadow[prompt - 2], 3) == "***");
        assert(std::string(text_shadow[prompt - 1], 15) == "[RUN] 0m 0.112s");
        assert(std::string(text_shadow[prompt], 7) == "BASIC> ");
        assert(std::string(text_shadow[39], 5) == "FKEYS");
    }
    std::puts("display scroll tests passed");
}
