#include "xmodem.hpp"
#include "transfer_file.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <unistd.h>

using namespace rmb::xmodem;
struct Peer {
    std::deque<int> input;
    std::vector<std::uint8_t> output, data, source;
    std::size_t pos = 0;
    bool committed = false, write_ok = true, finish_ok = true, read_ok = true;
    unsigned finishes = 0;
    rmb::TransferFile* file = nullptr;
    IO io() {
        return {this,
            [](void* p, unsigned) { auto& s = *static_cast<Peer*>(p);
                if (s.input.empty()) return timeout;
                int c = s.input.front(); s.input.pop_front(); return c; },
            [](void* p, const std::uint8_t* b, std::size_t n) {
                auto& s = *static_cast<Peer*>(p); s.output.insert(s.output.end(), b, b+n); return true; },
            [](void* p, std::uint8_t* b, std::size_t n) {
                auto& s = *static_cast<Peer*>(p);
                if (!s.read_ok) return -1;
                n = std::min(n, s.source.size() - s.pos);
                if (n) std::memcpy(b, s.source.data() + s.pos, n);
                s.pos += n; return int(n); },
            [](void* p, const std::uint8_t* b, std::size_t n) {
                auto& s = *static_cast<Peer*>(p);
                if (!s.write_ok) return false;
                s.data.insert(s.data.end(), b, b+n);
                return !s.file || s.file->write(b,n); },
            [](void* p) { auto& s = *static_cast<Peer*>(p); ++s.finishes;
                s.committed = s.finish_ok && (!s.file || s.file->commit()); return s.committed; }};
    }
    void block(unsigned seq, bool bad_crc = false, bool bad_seq = false) {
        std::uint8_t b[128];
        for (unsigned i=0; i<128; ++i) b[i] = i; // includes NUL, CAN, ESC
        input.push_back(1); input.push_back(seq & 255);
        input.push_back((~seq & 255) ^ (bad_seq ? 1 : 0));
        for (auto v : b) input.push_back(v);
        auto crc = crc16(b,128) ^ (bad_crc ? 1 : 0);
        input.push_back(crc >> 8); input.push_back(crc & 255);
    }
};
std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
int main() {
    assert(crc16(reinterpret_cast<const std::uint8_t*>("123456789"),9) == 0x31c3);
    { Peer p; p.input.push_back('\n'); p.block(1,true); p.block(1); p.block(1); p.block(2);
      p.input.push_back(4); p.input.push_back(4); auto io=p.io(); auto r=receive(io);
      assert(r.error==Error::None && r.bytes==256 && p.data.size()==256 && p.finishes==1);
      assert(p.output[0]=='C' && p.output[1]==21); }
    { Peer p; for(unsigned i=1;i<=260;++i) p.block(i); p.input.push_back(4);
      auto io=p.io(); assert(receive(io).bytes==260*128); }
    for(auto failure : {Error::Crc,Error::Protocol,Error::Timeout,Error::Cancelled,Error::Disconnected,Error::Write}) {
      Peer p;
      if(failure==Error::Crc || failure==Error::Protocol)
        for(int i=0;i<10;++i) p.block(1,failure==Error::Crc,failure==Error::Protocol);
      else if(failure==Error::Cancelled) p.input.push_back(24);
      else if(failure==Error::Disconnected) p.input.push_back(disconnected);
      else if(failure==Error::Write) {p.block(1); p.write_ok=false;}
      auto io=p.io(); assert(receive(io).error==failure); assert(!p.committed);
    }
    { Peer p; p.block(0); auto io=p.io(); assert(receive(io).error==Error::Protocol); }
    { Peer p; p.input={1,1,254,0,timeout,timeout}; p.block(1); p.input.push_back(4);
      auto io=p.io(); assert(receive(io).error==Error::None); }
    { Peer p; p.block(1); p.input.push_back(4); p.finish_ok=false;
      auto io=p.io(); assert(receive(io).error==Error::Write); assert(p.output.back()==24); }
    { Peer p; p.source.resize(129); for(unsigned i=0;i<129;++i) p.source[i]=i;
      p.input={'C',21,6,6,21,6}; auto io=p.io(); auto r=send(io);
      assert(r.error==Error::None && r.bytes==129);
      assert(p.output.size()==133*3+2);
      assert(std::equal(p.output.begin(),p.output.begin()+133,p.output.begin()+133));
      Peer receiver; receiver.input.insert(receiver.input.end(),p.output.begin(),p.output.begin()+399);
      receiver.input.push_back(4); auto rio=receiver.io(); assert(receive(rio).error==Error::None);
      assert(std::equal(p.source.begin(),p.source.end(),receiver.data.begin()));
      assert(receiver.data.back()==0x1a); }
    { Peer p; p.input={'C',6}; auto io=p.io(); assert(send(io).error==Error::None); assert(p.output[0]==4); }
    for(auto failure : {Error::Timeout,Error::Cancelled,Error::Disconnected,Error::Read}) {
      Peer p; p.input.push_back('C'); p.source.resize(1);
      if(failure==Error::Cancelled) p.input.push_back(cancelled);
      if(failure==Error::Disconnected) p.input.push_back(disconnected);
      if(failure==Error::Read) p.read_ok=false;
      auto io=p.io(); assert(send(io).error==failure);
    }
    char dir[]="/tmp/rmb-xmodem-XXXXXX"; assert(mkdtemp(dir));
    std::string root=std::string(dir)+"/";
    { std::ofstream f(root+"TEST.BAS"); f << "10 PRINT 1\n"; }
    // An aborted transfer must not replace the original, nor leave a TMP.
    { rmb::TransferFile f; assert(f.open("TEST.BAS",true,root.c_str()));
      Peer p; p.file=&f; p.block(1); p.input.push_back(24); auto io=p.io();
      assert(receive(io).error==Error::Cancelled); }
    assert(read_file(root+"TEST.BAS")=="10 PRINT 1\n");
    assert(!std::filesystem::exists(root+"XMODEM.TMP"));
    std::uint8_t block[128]; std::memset(block,0x1a,128);
    std::memcpy(block,"10 PRINT 2\n",11);
    { rmb::TransferFile f; assert(f.open("TEST.BAS",true,root.c_str()));
      assert(f.write(block,128)); assert(f.commit()); assert(f.bytes()==11); }
    assert(read_file(root+"TEST.BAS")=="10 PRINT 2\n");
    assert(!std::filesystem::exists(root+"XMODEM.BAK"));
    { rmb::TransferFile f; assert(f.open("TEST.BIN",true,root.c_str()));
      assert(f.write(block,128)); assert(f.commit()); assert(f.bytes()==128); }
    assert(read_file(root+"TEST.BIN").size()==128);
    { rmb::TransferFile f; assert(!f.open("../BAD",true,root.c_str()));
      assert(!f.open("XMODEM.BAK",true,root.c_str())); assert(!f.open("MISSING",false,root.c_str())); }
    { std::ofstream f(root+"XMODEM.BAK"); f << "keep"; }
    { rmb::TransferFile f; assert(!f.open("TEST.BAS",true,root.c_str())); }
    assert(read_file(root+"XMODEM.BAK")=="keep");
    std::filesystem::remove_all(root); // only the mkdtemp directory above
    std::puts("XMODEM CRC, retry, cancel, wrap, file safety tests passed");
}
