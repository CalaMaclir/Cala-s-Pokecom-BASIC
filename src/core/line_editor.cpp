#include "line_editor.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "platform.hpp"

namespace rmb {

namespace {

constexpr int kBackspace = 0x08;
constexpr int kEnter = 0x0a;
constexpr int kCarriageReturn = 0x0d;
constexpr int kLeft = 0xb4;
constexpr int kRight = 0xb7;
constexpr int kHome = 0xd2;
constexpr int kDelete = 0xd4;
constexpr int kEnd = 0xd5;
constexpr int kEscape = 0xb1;

int special_key = 0;

bool is_function_key(int c) {
    return (c >= 0x81 && c <= 0x89) || c == 0x90;
}

bool lcd_console_enabled() {
    return platform::get_console_mode() != platform::ConsoleMode::Serial;
}

bool serial_console_enabled() {
    return platform::get_console_mode() != platform::ConsoleMode::Lcd;
}

void serial_position(std::size_t index) {
    if (!serial_console_enabled()) return;

    platform::serial_put_string_raw("\x1b[u");

    if (index != 0) {
        char seq[24] = {};
        std::snprintf(
            seq,
            sizeof(seq),
            "\x1b[%luC",
            static_cast<unsigned long>(index)
        );
        platform::serial_put_string_raw(seq);
    }
}

void ensure_visible(
    int origin_col,
    int& origin_row,
    std::size_t index
) {
    if (!lcd_console_enabled()) return;

    const int cols = platform::text_columns();
    const int rows = platform::text_rows();
    const int linear = origin_col + static_cast<int>(index);
    const int target_row = origin_row + linear / cols;

    if (target_row >= rows) {
        const int scroll = target_row - rows + 1;
        platform::scroll_text_rows(scroll);
        origin_row -= scroll;
        if (origin_row < 0) origin_row = 0;
    }
}

void position_cursor(
    int origin_col,
    int origin_row,
    std::size_t index
) {
    if (lcd_console_enabled()) {
        const int cols = platform::text_columns();
        const int linear = origin_col + static_cast<int>(index);

        platform::set_cursor_position(
            linear % cols,
            origin_row + linear / cols
        );
    }

    serial_position(index);
}

void redraw(
    const char* buffer,
    std::size_t length,
    std::size_t previous_length,
    std::size_t cursor,
    int origin_col,
    int& origin_row
) {
    const std::size_t visible_length =
        length > previous_length ? length : previous_length;
    ensure_visible(origin_col, origin_row, visible_length);
    if (lcd_console_enabled()) {
        const int cols = platform::text_columns();
        const int linear = origin_col;

        platform::set_cursor_position(
            linear % cols,
            origin_row + linear / cols
        );

        for (std::size_t i = 0; i < length; ++i) {
            platform::screen_put_char(buffer[i]);
        }

        for (std::size_t i = length; i < previous_length; ++i) {
            platform::screen_put_char(' ');
        }
    }

    if (serial_console_enabled()) {
        platform::serial_put_string_raw("\x1b[u");
        for (std::size_t i = 0; i < length; ++i) {
            platform::serial_put_char_raw(buffer[i]);
        }
        for (std::size_t i = length; i < previous_length; ++i) {
            platform::serial_put_char_raw(' ');
        }
    }

    position_cursor(origin_col, origin_row, cursor);
}

} // namespace

int LineEditor::last_special_key() {
    return special_key;
}

std::size_t LineEditor::read(char* buffer, std::size_t capacity) {
    special_key = 0;

    if (!buffer || capacity == 0) {
        return 0;
    }

    buffer[0] = '\0';

    if (serial_console_enabled()) {
        // Prompt has already been printed. Save the terminal cursor position
        // so redraws can restore this input origin without knowing prompt text.
        platform::serial_put_string_raw("\x1b[s");
    }

    const int origin_col = platform::cursor_column();
    int origin_row = platform::cursor_row();

    // The editor scrolls the console only when the typed text actually wraps
    // past the last console row, so the REPL no longer has to reserve four
    // blank rows below every prompt.
    const std::size_t max_length = capacity - 1;

    std::size_t length = 0;
    std::size_t cursor = 0;
    std::size_t rendered_length = 0;

    while (true) {
        const int c = platform::get_char();

        if (c == kEnter || c == kCarriageReturn) {
            position_cursor(origin_col, origin_row, length);
            platform::put_string("\r\n");
            buffer[length] = '\0';
            return length;
        }

        if (c == kEscape || c == 0x1b || c == 0x03) {
            const std::size_t old_length = rendered_length;
            length = 0;
            cursor = 0;
            buffer[0] = '\0';
            redraw(
                buffer,
                length,
                old_length,
                cursor,
                origin_col,
                origin_row
            );
            platform::put_string("\r\n");
            return 0;
        }

        if (c == kLeft) {
            if (cursor > 0) {
                --cursor;
                position_cursor(origin_col, origin_row, cursor);
            }
            continue;
        }

        if (c == kRight) {
            if (cursor < length) {
                ++cursor;
                position_cursor(origin_col, origin_row, cursor);
            }
            continue;
        }

        if (c == kHome) {
            if (length == 0) {
                special_key = c;
                buffer[0] = '\0';
                // Menus repaint only the LCD. End the serial prompt too, so
                // returning without a command cannot append another BASIC>.
                platform::put_string("\r\n");
                return 0;
            }

            cursor = 0;
            position_cursor(origin_col, origin_row, cursor);
            continue;
        }

        if (is_function_key(c) && length == 0) {
            special_key = c;
            buffer[0] = '\0';
            return 0;
        }

        if (c == kEnd) {
            cursor = length;
            position_cursor(origin_col, origin_row, cursor);
            continue;
        }

        if (c == kBackspace) {
            if (cursor == 0) {
                continue;
            }

            const std::size_t old_length = rendered_length;
            std::memmove(
                buffer + cursor - 1,
                buffer + cursor,
                length - cursor + 1
            );

            --cursor;
            --length;
            redraw(
                buffer,
                length,
                old_length,
                cursor,
                origin_col,
                origin_row
            );
            rendered_length = length;
            continue;
        }

        if (c == kDelete) {
            if (cursor >= length) {
                continue;
            }

            const std::size_t old_length = rendered_length;
            std::memmove(
                buffer + cursor,
                buffer + cursor + 1,
                length - cursor
            );

            --length;
            redraw(
                buffer,
                length,
                old_length,
                cursor,
                origin_col,
                origin_row
            );
            rendered_length = length;
            continue;
        }

        if (c < 0x20 || c > 0x7e || length >= max_length) {
            continue;
        }

        const std::size_t old_length = rendered_length;

        std::memmove(
            buffer + cursor + 1,
            buffer + cursor,
            length - cursor + 1
        );

        buffer[cursor] = static_cast<char>(c);
        ++cursor;
        ++length;

        redraw(
            buffer,
            length,
            old_length,
            cursor,
            origin_col,
            origin_row
        );
        rendered_length = length;
    }
}

} // namespace rmb
