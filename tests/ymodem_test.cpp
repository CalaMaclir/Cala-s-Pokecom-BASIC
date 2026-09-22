#include "ymodem.hpp"
#include "transfer_file.hpp"
#include "program_file_guard.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

using namespace rmb::ymodem;
namespace {
constexpr std::uint8_t SOH=1,STX=2,EOT=4,ACK=6,NAK=21,CAN=24;
struct Peer {
    std::deque<int> input;
    std::vector<std::uint8_t> output,source,data;
    std::size_t position=0;
    std::string name;
    std::uint32_t expected=0;
    bool begin_ok=true,write_ok=true,finish_ok=true,finished=false;
    IO io(){return {this,
        [](void* p,unsigned){auto& s=*static_cast<Peer*>(p);if(s.input.empty())return rmb::xmodem::timeout;
            int c=s.input.front();s.input.pop_front();return c;},
        [](void* p,const std::uint8_t* b,std::size_t n){auto& s=*static_cast<Peer*>(p);s.output.insert(s.output.end(),b,b+n);return true;},
        [](void* p,std::uint8_t* b,std::size_t n){auto& s=*static_cast<Peer*>(p);n=std::min(n,s.source.size()-s.position);
            if(n)std::memcpy(b,s.source.data()+s.position,n);
            s.position+=n;return static_cast<int>(n);},
        [](void* p,const std::uint8_t* b,std::size_t n){auto& s=*static_cast<Peer*>(p);if(!s.write_ok)return false;
            s.data.insert(s.data.end(),b,b+n);return true;},
        [](void* p){auto& s=*static_cast<Peer*>(p);s.finished=s.finish_ok&&s.data.size()==s.expected;return s.finished;},
        [](void* p,const char* name,std::uint32_t size){auto& s=*static_cast<Peer*>(p);s.name=name;s.expected=size;return s.begin_ok;}
    };}
    void packet(std::uint8_t marker,std::uint8_t sequence,const std::uint8_t* data,std::size_t size,bool bad_crc=false){
        input.push_back(marker);input.push_back(sequence);input.push_back(static_cast<std::uint8_t>(~sequence));
        for(std::size_t i=0;i<size;++i)input.push_back(data[i]);
        auto crc=rmb::xmodem::crc16(data,size)^(bad_crc?1:0);input.push_back(crc>>8);input.push_back(crc&255);
    }
    void header(const char* filename,std::uint32_t size,bool bad_crc=false){
        std::uint8_t data[kHeaderSize]={};if(filename&&*filename)assert(encode_header(filename,size,data));
        packet(SOH,0,data,sizeof(data),bad_crc);
    }
    void transfer(const char* filename,const std::vector<std::uint8_t>& bytes,bool soh=false,bool duplicate=false){
        header(filename,bytes.size());std::uint8_t sequence=1;
        for(std::size_t offset=0;offset<bytes.size();){
            std::size_t block=soh?128:1024;std::vector<std::uint8_t> payload(block,0x1a);
            auto count=std::min(block,bytes.size()-offset);std::copy_n(bytes.data()+offset,count,payload.data());
            packet(soh?SOH:STX,sequence,payload.data(),block);
            if(duplicate&&sequence==1)packet(soh?SOH:STX,sequence,payload.data(),block);
            offset+=count;++sequence;
        }
        input.push_back(EOT);input.push_back(EOT);header("",0);
    }
};
std::vector<std::uint8_t> bytes(std::size_t size){std::vector<std::uint8_t> out(size);for(std::size_t i=0;i<size;++i)out[i]=(i*37+11)&255;return out;}
std::string read_file(const std::string& name){std::ifstream file(name,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
}
int main(){
    assert(rmb::xmodem::crc16(reinterpret_cast<const std::uint8_t*>("123456789"),9)==0x31c3);
    std::uint8_t payload[kHeaderSize];Header header;
    assert(encode_header("CPB_LARGE_400.BAS",6255,payload));assert(decode_header(payload,header));
    assert(!std::strcmp(header.filename,"CPB_LARGE_400.BAS")&&header.size==6255&&!header.empty);
    std::memset(payload,0,sizeof(payload));assert(decode_header(payload,header)&&header.empty);
    assert(!encode_header("",1,payload));
    std::memset(payload,'A',sizeof(payload));assert(!decode_header(payload,header));
    std::memset(payload,0,sizeof(payload));std::memcpy(payload,"TEST.BAS\0x",10);assert(!decode_header(payload,header));

    for(auto size:{0u,1u,127u,128u,129u,1023u,1024u,1025u,6255u}){
        Peer peer;auto original=bytes(size);peer.transfer("TEST.BIN",original);auto io=peer.io();auto r=receive(io);
        assert(r.error==Error::None&&r.bytes==size&&peer.name=="TEST.BIN"&&peer.expected==size);
        assert(peer.data==original&&peer.finished);assert(peer.output.front()=='C'&&peer.output.back()==ACK);
    }
    {Peer peer;auto original=bytes(129);peer.transfer("SMALL.BIN",original,true);auto io=peer.io();assert(receive(io).error==Error::None);assert(peer.data==original);}
    {Peer peer;auto original=bytes(1025);peer.transfer("DUP.BIN",original,false,true);auto io=peer.io();assert(receive(io).error==Error::None);assert(peer.data==original);}
    {Peer peer;auto original=bytes(10);peer.header("CRC.BIN",10,true);peer.header("CRC.BIN",10);std::vector<std::uint8_t> block(1024,0x1a);
     std::copy(original.begin(),original.end(),block.begin());peer.packet(STX,1,block.data(),block.size());peer.input.push_back(EOT);peer.input.push_back(EOT);peer.header("",0);
     auto io=peer.io();assert(receive(io).error==Error::None);assert(std::find(peer.output.begin(),peer.output.end(),'C')!=peer.output.end());}
    {Peer peer;peer.input.push_back(CAN);auto io=peer.io();assert(receive(io).error==Error::Cancelled);}
    {Peer peer;peer.header("../BAD",4);peer.begin_ok=false;auto io=peer.io();assert(receive(io).error==Error::Write);}
    {Peer peer;peer.header("SHORT.BIN",129);std::uint8_t block[128]={};peer.packet(SOH,1,block,128);peer.input.push_back(EOT);
     auto io=peer.io();assert(receive(io).error==Error::Protocol&&!peer.finished);}
    {Peer peer;peer.header("STOP.BIN",1);peer.input.push_back(CAN);auto io=peer.io();assert(receive(io).error==Error::Cancelled&&!peer.finished);}

    for(auto size:{0u,1u,127u,128u,129u,1023u,1024u,1025u,6255u}){
        Peer peer;peer.source=bytes(size);peer.input.push_back('C');peer.input.push_back(ACK);peer.input.push_back('C');
        auto blocks=(size+1023)/1024;for(std::size_t i=0;i<blocks;++i)peer.input.push_back(ACK);
        peer.input.push_back(NAK);peer.input.push_back(ACK);peer.input.push_back('C');peer.input.push_back(ACK);
        auto io=peer.io();auto r=send(io,"TEST.BIN",size);assert(r.error==Error::None&&r.bytes==size);
        assert(peer.output.size()==133+blocks*1029+2+133);
        assert(peer.output[0]==SOH&&peer.output[1]==0);Header sent;assert(decode_header(peer.output.data()+3,sent));assert(sent.size==size);
        std::vector<std::uint8_t> restored;std::size_t pos=133;
        for(std::size_t i=0;i<blocks;++i,pos+=1029)restored.insert(restored.end(),peer.output.begin()+pos+3,peer.output.begin()+pos+3+1024);
        restored.resize(size);assert(restored==peer.source);assert(peer.output[pos]==EOT&&peer.output[pos+1]==EOT);
        Header terminal;assert(decode_header(peer.output.data()+pos+5,terminal)&&terminal.empty);
    }
    {Peer peer;peer.source=bytes(1);peer.input={'C',NAK,ACK,'C',ACK,NAK,ACK,'C',ACK};auto io=peer.io();assert(send(io,"RETRY.BIN",1).error==Error::None);}
    {Peer peer;peer.source=bytes(1);peer.input={'C',ACK,'C',CAN};auto io=peer.io();assert(send(io,"CANCEL.BIN",1).error==Error::Cancelled);}

    char directory[]="/tmp/rmb-ymodem-XXXXXX";assert(mkdtemp(directory));std::string root=std::string(directory)+"/";
    {std::ofstream file(root+"KEEP.BIN",std::ios::binary);file<<"original";}
    {rmb::TransferFile file;assert(file.open("KEEP.BIN",true,root.c_str(),"YMODEM"));auto value=bytes(129);
     assert(file.write_exact(value.data(),value.size()));assert(!file.commit_exact(130));}
    assert(read_file(root+"KEEP.BIN")=="original");assert(!std::filesystem::exists(root+"YMODEM.TMP"));
    {rmb::TransferFile file;auto value=bytes(6255);assert(file.open("KEEP.BIN",true,root.c_str(),"YMODEM"));
     assert(file.write_exact(value.data(),1024));assert(file.write_exact(value.data()+1024,value.size()-1024));assert(file.commit_exact(value.size()));assert(file.bytes()==6255);}
    assert(read_file(root+"KEEP.BIN").size()==6255);
    {rmb::TransferFile file;assert(!file.open("../BAD",true,root.c_str(),"YMODEM"));assert(!file.open("RMBP0000.BAS",true,root.c_str(),"YMODEM"));}
    rmb::program_files::set_active("ACTIVE.BAS");
    {rmb::TransferFile file;assert(!file.open("active.bas",true,root.c_str(),"YMODEM"));assert(!std::strcmp(file.error(),"FILE IN USE"));}
    rmb::program_files::set_active(nullptr);
    std::filesystem::remove_all(root);
    std::puts("YMODEM header, exact size, 128/1K, retry, duplicate, cancel and file safety tests passed");
}
