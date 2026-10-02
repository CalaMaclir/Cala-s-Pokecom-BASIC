#include "program_file_guard.hpp"

#include <cassert>
#include <fstream>
#include <iterator>
#include <string>

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

    std::ifstream repl_source("src/core/repl.cpp");
    assert(repl_source);
    const std::string source(
        (std::istreambuf_iterator<char>(repl_source)),
        std::istreambuf_iterator<char>()
    );
    const auto control_center = source.find("void Repl::show_system_menu()");
    const auto file_transfer = source.find("void Repl::menu_file_transfer()");
    const auto serial_config =
        source.find("void Repl::menu_transfer_performance()");
    assert(control_center != std::string::npos);
    assert(file_transfer != std::string::npos);
    assert(serial_config != std::string::npos);
    const auto files = source.find("{\"Files\", ControlAction::Files", control_center);
    const auto editor = source.find("{\"Editor\", ControlAction::Editor", control_center);
    const auto program = source.find("{\"Program\", ControlAction::None", control_center);
    const auto save = source.find("{\"Save Program\", ControlAction::SaveProgram", control_center);
    assert(files < editor && editor < program && program < save);
    assert(source.find("{\"Storage\", ControlAction::None", control_center) < file_transfer);
    assert(source.find("{\"Display & Audio\", ControlAction::None", control_center) < file_transfer);
    assert(source.find("{\"Network\", ControlAction::None", control_center) < file_transfer);
    assert(source.find("{\"Serial\", ControlAction::None", control_center) < file_transfer);
    assert(source.find("{\"System\", ControlAction::None", control_center) < file_transfer);
    assert(source.find("{\"Diagnostics\", ControlAction::None", control_center) < file_transfer);
    assert(source.find("{\"Serial Config\", ControlAction::SerialConfig", control_center) < file_transfer);
    assert(source.find("{\"File Transfer\", ControlAction::FileTransfer", control_center) < file_transfer);
    assert(source.find("selectable(items[scroll.selected])", control_center) < file_transfer);
    assert(source.substr(control_center, file_transfer - control_center).find("...") ==
           std::string::npos);
    assert(source.find("menu_transfer_performance();", control_center) <
           file_transfer);
    assert(source.find("\"Transfer Performance...\"", file_transfer) ==
           std::string::npos);
    assert(source.find("return \"Micro-USB (USB CDC)\";", file_transfer) <
           serial_config);
    assert(source.find("return \"USB-C (UART0)\";", file_transfer) <
           serial_config);
    assert(source.find("\"SERIAL CONFIG\"", serial_config) !=
           std::string::npos);
    assert(source.find("\"Micro-USB CDC Bulk: %s\"", serial_config) !=
           std::string::npos);
    assert(source.find("\"Micro-USB: USB CDC (baud ignored)\"",
                       serial_config) != std::string::npos);
    assert(source.find("\"USB-C: UART0 (PC baud must match)\"",
                       serial_config) != std::string::npos);
}
