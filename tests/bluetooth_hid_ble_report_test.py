#!/usr/bin/env python3
"""Exercise the production report adapter with the pinned SDK's real HID parser."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
btstack = Path(sys.argv[1]).resolve()
source = (root / 'src/platform/picocalc/bluetooth_hid_ble_keyboard.cpp').read_text()
start = source.index('void handle_report_protocol(')
opening = source.index('{', start)
depth, end = 1, opening + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
adapter = source[start:end]
prelude = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
extern "C" {
#include "btstack_hid_parser.h"
}
namespace bluetooth_hid { struct KeyboardCore { static constexpr unsigned kMaxKeys = 6; }; }
struct Keyboard {
    unsigned calls = 0, modifiers = 0;
    std::vector<std::uint8_t> keys;
    void handle_report(std::uint8_t mod, const std::uint8_t* data, std::size_t count, unsigned) {
        ++calls; modifiers = mod; keys.assign(data, data + count);
    }
} keyboard;
unsigned get_absolute_time() { return 0; }
unsigned to_ms_since_boot(unsigned value) { return value; }
std::uint16_t hids_cid = 1;
std::vector<std::uint8_t> descriptor;
const std::uint8_t* hids_host_descriptor_storage_get_descriptor_data(unsigned, unsigned) { return descriptor.data(); }
std::uint16_t hids_host_descriptor_storage_get_descriptor_len(unsigned, unsigned) { return descriptor.size(); }
'''
tests = r'''
int main() {
    const std::uint8_t boot[] = {
        0x05,0x01,0x09,0x06,0xa1,0x01,0x05,0x07,
        0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,
        0x75,0x08,0x95,0x01,0x81,0x01,
        0x19,0x00,0x29,0x65,0x15,0x00,0x25,0x65,0x75,0x08,0x95,0x06,0x81,0x00,0xc0
    };
    descriptor.assign(boot, boot + sizeof(boot));
    std::uint8_t report[9] = {0, 2, 0, 4, 0, 0, 0, 0, 0};
    handle_report_protocol(0, report, sizeof(report));
    assert(keyboard.calls == 1 && keyboard.modifiers == 2);
    assert(keyboard.keys.size() == 1 && keyboard.keys[0] == 4);
    handle_report_protocol(0, report, 4);
    assert(keyboard.calls == 1); // truncated packet must not release keys
    descriptor.insert(descriptor.begin() + 6, {0x85, 0x07});
    report[0] = 7;
    handle_report_protocol(0, report, sizeof(report));
    assert(keyboard.calls == 2 && keyboard.modifiers == 2 && keyboard.keys[0] == 4);
    report[0] = 8;
    handle_report_protocol(0, report, sizeof(report));
    assert(keyboard.calls == 2); // unrelated report ID
    report[0] = 7; report[1] = 0; report[3] = 0;
    handle_report_protocol(0, report, sizeof(report));
    assert(keyboard.calls == 3 && keyboard.modifiers == 0 && keyboard.keys.empty());
    // Bitmap keyboard: only the set bit is a press; zero-valued usages are not keys.
    descriptor = {0x05,0x01,0x09,0x06,0xa1,0x01,0x05,0x07,
                  0x19,0x04,0x29,0x0b,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,0xc0};
    std::uint8_t bitmap[] = {0, 4};
    handle_report_protocol(0, bitmap, sizeof(bitmap));
    assert(keyboard.calls == 4 && keyboard.keys.size() == 1 && keyboard.keys[0] == 6);
    // Consumer controls must not clear a held keyboard key.
    descriptor = {0x05,0x0c,0x09,0x01,0xa1,0x01,0x09,0xe9,
                  0x15,0,0x25,1,0x75,1,0x95,1,0x81,2,0xc0};
    handle_report_protocol(0, bitmap, sizeof(bitmap));
    assert(keyboard.calls == 4 && keyboard.keys[0] == 6);
}
'''
with tempfile.TemporaryDirectory(prefix='cpb-report-test-') as tmp:
    tmp = Path(tmp)
    test = tmp / 'test.cpp'
    test.write_text(prelude + adapter + tests)
    flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
             '-ffunction-sections', '-fdata-sections',
             '-I' + str(root / 'tests/ble_stubs'), '-I' + str(btstack / 'src')]
    objects = []
    for name in ['btstack_hid_parser', 'btstack_util']:
        obj = tmp / (name + '.o')
        subprocess.run(['gcc', '-std=c11', *flags, '-c', str(btstack / 'src' / (name + '.c')), '-o', str(obj)], check=True)
        objects.append(str(obj))
    binary = tmp / 'test'
    subprocess.run(['g++', '-std=c++17', *flags, str(test), *objects, '-Wl,--gc-sections', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('BLE report adapter with real BTstack parser: PASS')
