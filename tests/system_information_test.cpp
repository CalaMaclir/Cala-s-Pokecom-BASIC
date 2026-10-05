#include "system_information.hpp"
#include "files_playback_input.hpp"
#include <cassert>
#include <string>
int main() {
    rmb::SystemInformation info;
    info.section("Audio");info.add("Current file", "%s", "dir/song.mp3");
    info.section("Network");info.add("SSID", "%s", "test\x1b\nnetwork");
    std::string out;
    assert(info.render_serial([](const char* s,void* p){*static_cast<std::string*>(p)+=s;return true;},&out));
    assert(out.find("dir/song.mp3")!=std::string::npos);
    assert(out.find("test??network")!=std::string::npos);
    assert(out.find('\x1b')==std::string::npos);
    rmb::SystemInformation wrapped;
    const std::string name(79,'A');wrapped.add("Current file","%s",name.c_str());
    std::string lcd;char line[54];
    for(std::size_t i=0;i<wrapped.screen_line_count();++i){wrapped.screen_line(i,line,sizeof(line));lcd+=line;}
    assert(lcd==wrapped.rows[0].text);
    assert(!info.render_serial([](const char*,void*){return false;},nullptr));
    for(int i=0;i<100;++i)info.add("X","%d",i);
    assert(info.count==info.capacity && info.overflow);
    using namespace rmb::files_input;
    PlaybackInput input;
    assert(input.handle(0xb1,false,true)==Result::Stop);
    assert(input.handle(0xb1,true,false)==Result::Consumed);
    assert(input.handle(0xb6,false,true)==Result::Unhandled);
    assert(input.handle(0xb1,true,false)==Result::Consumed); // Other key does not release it.
    assert(input.handle(0xb1,false,false)==Result::Unhandled);
    assert(input.handle('P',false,true)==Result::Stop);
    assert(input.handle('p',true,false)==Result::Consumed); // Modifier changes during a hold.
    assert(input.handle(0xb6,false,true)==Result::Unhandled);
    assert(input.handle(0xb6,false,true,true)==Result::Stop);
    assert(input.handle(0xb6,true,false,true)==Result::Consumed);
    assert(input.handle(' ',false,true)==Result::TogglePause);
    assert(input.handle(' ',true,true)==Result::Consumed);
}
