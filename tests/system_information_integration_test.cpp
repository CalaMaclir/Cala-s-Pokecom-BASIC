#include "benchmark_platform.hpp"
#include "system_information.hpp"
#include "soak_support.hpp"
int main() {
    rmb::Repl repl;
    rmb::SystemInformation info;
    const auto revision=repl.program_.revision();
    host_audio_active=true;host_audio_paused=true;
    std::strcpy(rmb::platform::host_audio_file,"music/song.mp3");
    const auto stops=host_audio_stop_count;
    repl.collect_system_information(info);
    assert(!rmb::platform::host_keyboard_query);
    assert(repl.program_.revision()==revision&&host_audio_active&&host_audio_stop_count==stops);
    std::string report;
    auto sink=[](const char* s,void* p){*static_cast<std::string*>(p)+=s;return true;};
    assert(info.render_serial(sink,&report));
    for(const char* field:{"Firmware","v0.94","RP2350","Memory","Storage","Network","Serial","Audio","Keyboard","Paused","music/song.mp3","Not attempted"})
        assert(report.find(field)!=std::string::npos);
    assert(!info.overflow);
    // S exports the captured snapshot; a subsequent live audio change does not mutate it.
    editor_keys={'s',0xb7,0xb7,0xb1};editor_key_index=0;output.clear();
    repl.menu_system_info();
    assert(output.find("music/song.mp3")!=std::string::npos);
    assert(host_audio_active&&host_audio_stop_count==stops);
    assert(!rmb::platform::serial_transfer_active());
    const auto resources = cpb_soak::resources();
    for (unsigned i = 0; i < cpb_soak::iterations(); ++i) {
        repl.collect_system_information(info); report.clear();
        assert(!info.overflow && info.render_serial(sink, &report));
        assert(repl.program_.revision() == revision && host_audio_active && host_audio_paused);
        assert(host_audio_stop_count == stops && !rmb::platform::host_keyboard_query);
        output.clear(); editor_keys={'s',0xb1}; editor_key_index=0;
        repl.menu_system_info();
        assert(!rmb::platform::serial_transfer_active());
    }
    cpb_soak::record("diagnostic-serial", cpb_soak::iterations(), resources);
    rmb::platform::audio_stop();repl.collect_system_information(info);report.clear();
    info.render_serial(sink,&report);assert(report.find("music/song.mp3")==std::string::npos);
    std::puts("System Information collector, read-only snapshot, screen and Serial: PASS");
}
