#include <cassert>
#include <cstring>

#include "system_controls.hpp"

int main() {
    using namespace rmb::system_controls;

    assert(cpu_profile_for_mhz(75) == CpuProfile::Eco);
    assert(cpu_profile_for_mhz(100) == CpuProfile::Normal);
    assert(cpu_profile_for_mhz(150) == CpuProfile::Full);
    assert(cpu_profile_for_mhz(200) == CpuProfile::Experimental);
    assert(cpu_profile_experimental(200));
    assert(!cpu_profile_experimental(150));
    assert(!cpu_profile_supported(175));
    assert(!cpu_profile_supported(225));
    assert(!cpu_profile_supported(250));
    assert(std::strcmp(cpu_profile_status_name(200), "EXP") == 0);
    assert(safe_boot_cpu_mhz() == 150);
    assert(safe_boot_cpu_mhz(200) == 150);

    assert(board_led_mode_from_setting("off") == BoardLedMode::Off);
    assert(board_led_mode_from_setting("ON") == BoardLedMode::On);
    assert(board_led_mode_from_setting("Heartbeat") == BoardLedMode::Heartbeat);
    assert(board_led_mode_from_setting("invalid") == BoardLedMode::Off);
    assert(board_led_mode_from_setting(nullptr) == BoardLedMode::Off);

    assert(!board_led_output(BoardLedMode::Off, 0));
    assert(board_led_output(BoardLedMode::On, 999));
    assert(board_led_output(BoardLedMode::Heartbeat, 0));
    assert(board_led_output(BoardLedMode::Heartbeat, 119));
    assert(!board_led_output(BoardLedMode::Heartbeat, 120));
    assert(!board_led_output(BoardLedMode::Heartbeat, 999));
    assert(board_led_output(BoardLedMode::Heartbeat, 1000));
    return 0;
}
