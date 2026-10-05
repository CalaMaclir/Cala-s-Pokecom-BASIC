"""Execute production standby entry/wake code with bounded peripheral stubs."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/platform/picocalc/platform.cpp").read_text()
start = source.index("void enter_sleep_mode(bool refresh_status)")
opening = source.index("{", start)
depth, end = 1, opening + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
production = source[start:end]

template = r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include "storage.hpp"
std::vector<std::string> events;
bool bluetooth_on = true, serial_on = true, serial_wake = false;
int wake_polls = 0, prepare_calls = 0, low_power_calls = 0;
unsigned char lcd_level = 160, keyboard_level = 80;
namespace rmb::storage {
Owner current = Owner::Firmware;
Owner owner() { return current; }
}
namespace rmb::bluetooth_manager {
bool enabled() { return bluetooth_on; }
bool enable() { bluetooth_on = true; events.push_back("bluetooth restore"); return true; }
}
namespace rmb::wireless {
void suspend_board_led() { events.push_back("led suspend"); }
void resume_board_led() { events.push_back("led resume"); }
}
namespace rmb::picocalc::keyboard {
bool get_lcd_backlight(unsigned char& v) { v=lcd_level; return true; }
bool get_keyboard_backlight(unsigned char& v) { v=keyboard_level; return true; }
bool set_lcd_backlight(unsigned char v) { lcd_level=v; events.push_back(v?"lcd restore":"lcd dark"); return true; }
bool set_keyboard_backlight(unsigned char v) { keyboard_level=v; events.push_back(v?"kbd restore":"kbd dark"); return true; }
}
namespace rmb::picocalc::display {
void enter_standby() { events.push_back("display sleep"); }
void leave_standby() { events.push_back("display wake"); }
}
namespace rmb::unified_keyboard {
int read_key() { return !serial_wake && ++wake_polls >= 3 ? 'w' : -1; }
}
void sleep_ms(std::uint32_t) {}
int low_power_sleep_for_ms(int, void*, bool exclusive) {
    assert(exclusive); ++low_power_calls; return -1; // timer allocation fallback
}
namespace rmb::platform {
using Callback = void(*)(void*);
void prepare(void*) { ++prepare_calls; bluetooth_on=false; events.push_back("prepare"); }
Callback sleep_prepare_callback=prepare, status_refresh_callback=nullptr;
void* sleep_prepare_context=nullptr; void* status_refresh_context=nullptr;
void put_string(const char*) { events.push_back("refused"); }
void serial_put_string_raw(const char* s) { events.push_back(s); }
bool console_uses_serial() { return serial_on; }
bool status_area_enabled() { return false; }
void console_local_input() { events.push_back("local wake"); }
int console_serial_read(unsigned) { return serial_wake ? 13 : -1; }
// @PRODUCTION@
}
int main() {
    using namespace rmb;
    // Guard must precede callbacks, darkness, LCD sleep and low-power entry.
    for (auto owner : {storage::Owner::UsbHost, storage::Owner::Transition}) {
        storage::current=owner; events.clear();
        platform::enter_sleep_mode(true);
        assert(events == std::vector<std::string>{"refused"});
        assert(prepare_calls==0 && low_power_calls==0 && bluetooth_on);
        assert(lcd_level==160 && keyboard_level==80);
    }
    for (int cycle=0; cycle<10; ++cycle) {
        storage::current=storage::Owner::Firmware; events.clear(); wake_polls=0;
        serial_wake=cycle%2; bluetooth_on=true;
        const auto before=prepare_calls;
        platform::enter_sleep_mode(cycle%2);
        assert(prepare_calls==before+1 && bluetooth_on);
        assert(lcd_level==160 && keyboard_level==80);
        assert(events[0]=="led suspend" && events[1]=="prepare");
        assert(events[3]=="lcd dark" && events[4]=="kbd dark");
        assert(events[5]=="display sleep");
        auto find=[](const char* s) {
            for (unsigned i=0;i<events.size();++i) if(events[i]==s)return i;
            assert(false); return 0u;
        };
        assert(find("display wake")<find("lcd restore"));
        assert(find("lcd restore")<find("kbd restore"));
        assert(find("kbd restore")<find("bluetooth restore"));
    }
    assert(low_power_calls>0);
}
'''
with tempfile.TemporaryDirectory(prefix="cpb-power-") as folder:
    cpp = Path(folder) / "power.cpp"
    exe = Path(folder) / "power-test"
    cpp.write_text(template.replace("// @PRODUCTION@", production))
    subprocess.run(["g++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I" + str(ROOT / "include"), str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"},
                   timeout=10, check=True)
print("Production standby guard / local and serial wake / restore order: PASS")
