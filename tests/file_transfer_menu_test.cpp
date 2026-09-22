#include "program_file_guard.hpp"

#include <cassert>

int main() {
    using rmb::program_files::visible_for_transfer;

    assert(visible_for_transfer("CPB_LARGE_400.BAS"));
    assert(visible_for_transfer("SCREEN0012.BMP"));
    assert(visible_for_transfer("RMBASIC.CFG"));
    assert(visible_for_transfer("RMBASIC.BAK"));

    assert(!visible_for_transfer("RMBP0001.BAS"));
    assert(!visible_for_transfer("RMBEDIT.TMP"));
    assert(!visible_for_transfer("XMODEM.TMP"));
    assert(!visible_for_transfer("XMODEM.BAK"));
    assert(!visible_for_transfer("YMODEM.TMP"));
    assert(!visible_for_transfer("YMODEM.BAK"));
    assert(!visible_for_transfer("HTTP.TMP"));
    assert(!visible_for_transfer("HTTP.BAK"));
}
