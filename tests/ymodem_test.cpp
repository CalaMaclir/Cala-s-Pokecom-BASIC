#include "ymodem.hpp"
#include "transfer_file.hpp"
#include "program_file_guard.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

using namespace rmb::ymodem;
namespace {
constexpr std::uint8_t SOH=1,STX=2,EOT=4,ACK=6,NAK=21,CAN=24;

struct ReceivedFile {
    std::string name;
    std::uint32_t expected=0;
    std::vector<std::uint8_t> data;
};

struct Peer {
    std::deque<int> input;
    std::vector<std::uint8_t> output,source,data;
    std::vector<ReceivedFile> completed;
    std::size_t position=0;
    std::string name;
    std::uint32_t expected=0;
    unsigned finish_calls=0;
    bool begin_ok=true,write_ok=true,finish_ok=true;
    bool finished=false,active=false;
    std::function<void(Peer&,std::uint8_t)> on_output;

    IO io(){return {this,
        [](void* p,unsigned){
            auto& s=*static_cast<Peer*>(p);
            if(s.input.empty())return rmb::xmodem::timeout;
            int c=s.input.front();s.input.pop_front();return c;
        },
        [](void* p,const std::uint8_t* b,std::size_t n){
            auto& s=*static_cast<Peer*>(p);
            s.output.insert(s.output.end(),b,b+n);
            if(s.on_output)
                for(std::size_t i=0;i<n;++i)s.on_output(s,b[i]);
            return true;
        },
        [](void* p,std::uint8_t* b,std::size_t n){
            auto& s=*static_cast<Peer*>(p);
            n=std::min(n,s.source.size()-s.position);
            if(n)std::memcpy(b,s.source.data()+s.position,n);
            s.position+=n;return static_cast<int>(n);
        },
        [](void* p,const std::uint8_t* b,std::size_t n){
            auto& s=*static_cast<Peer*>(p);
            if(!s.write_ok||!s.active)return false;
            s.data.insert(s.data.end(),b,b+n);return true;
        },
        [](void* p){
            auto& s=*static_cast<Peer*>(p);
            ++s.finish_calls;
            s.finished=s.finish_ok&&s.active&&
                s.data.size()==s.expected;
            if(s.finished)
                s.completed.push_back({s.name,s.expected,s.data});
            s.active=false;
            return s.finished;
        },
        [](void* p,const char* name,std::uint32_t size){
            auto& s=*static_cast<Peer*>(p);
            if(!s.begin_ok)return false;
            s.name=name;s.expected=size;s.data.clear();
            s.finished=false;s.active=true;return true;
        }
    };}

    void packet(std::uint8_t marker,std::uint8_t sequence,
                const std::uint8_t* bytes,std::size_t size,
                bool bad_crc=false){
        input.push_back(marker);
        input.push_back(sequence);
        input.push_back(static_cast<std::uint8_t>(~sequence));
        for(std::size_t i=0;i<size;++i)input.push_back(bytes[i]);
        auto crc=rmb::xmodem::crc16(bytes,size)^(bad_crc?1:0);
        input.push_back(crc>>8);input.push_back(crc&255);
    }

    void header_block(std::uint8_t marker,const char* filename,
                      std::uint32_t size,bool bad_crc=false){
        const std::size_t block=marker==SOH?kHeaderSize:kDataSize;
        std::vector<std::uint8_t> bytes(block,0);
        if(filename&&*filename)
            assert(encode_header(filename,size,bytes.data()));
        packet(marker,0,bytes.data(),bytes.size(),bad_crc);
    }

    void header(const char* filename,std::uint32_t size,
                bool bad_crc=false){
        header_block(SOH,filename,size,bad_crc);
    }

    void file(const char* filename,
              const std::vector<std::uint8_t>& bytes,
              bool soh=false,bool duplicate=false,
              bool bad_first_data=false){
        header(filename,bytes.size());
        std::uint8_t sequence=1;
        for(std::size_t offset=0;offset<bytes.size();){
            const std::size_t block=soh?128:1024;
            std::vector<std::uint8_t> payload(block,0x1a);
            const auto count=std::min(block,bytes.size()-offset);
            std::copy_n(bytes.data()+offset,count,payload.data());
            if(bad_first_data&&sequence==1)
                packet(soh?SOH:STX,sequence,
                       payload.data(),block,true);
            packet(soh?SOH:STX,sequence,payload.data(),block);
            if(duplicate&&sequence==1)
                packet(soh?SOH:STX,sequence,payload.data(),block);
            offset+=count;
            ++sequence;
        }
        input.push_back(EOT);
        input.push_back(EOT);
    }

    void finish_batch(){header("",0);}

    void transfer(const char* filename,
                  const std::vector<std::uint8_t>& bytes,
                  bool soh=false,bool duplicate=false){
        file(filename,bytes,soh,duplicate);
        finish_batch();
    }
};

std::vector<std::uint8_t> bytes(std::size_t size){
    std::vector<std::uint8_t> out(size);
    for(std::size_t i=0;i<size;++i)
        out[i]=static_cast<std::uint8_t>((i*37+11)&255);
    return out;
}

void assert_file(const Peer& peer,std::size_t index,
                 const char* name,
                 const std::vector<std::uint8_t>& expected){
    assert(index<peer.completed.size());
    assert(peer.completed[index].name==name);
    assert(peer.completed[index].expected==expected.size());
    assert(peer.completed[index].data==expected);
}

enum class ReceiveFault {
    None, HeaderCrc, HeaderTimeout, DataCrc, DataTimeout
};

Result interactive_receive(
    Peer& peer,
    const std::vector<std::uint8_t>& original,
    bool one_eot,
    std::uint8_t header_marker=SOH,
    std::uint8_t empty_marker=SOH,
    bool duplicate=false,
    ReceiveFault fault=ReceiveFault::None
) {
    int stage=0;
    bool header_fault_sent=false;
    auto queue_data=[&](Peer& p,bool bad_crc,bool partial){
        const std::size_t block=1024;
        std::vector<std::uint8_t> payload(block,0x1a);
        std::uint8_t sequence=1;
        for(std::size_t offset=0;offset<original.size();){
            const auto count=std::min(block,original.size()-offset);
            std::fill(payload.begin(),payload.end(),0x1a);
            std::copy_n(
                original.data()+offset,count,payload.data());
            if(partial){
                p.input.push_back(STX);
                p.input.push_back(sequence);
                p.input.push_back(
                    static_cast<std::uint8_t>(~sequence));
                for(int i=0;i<16;++i)
                    p.input.push_back(payload[i]);
                p.input.push_back(rmb::xmodem::timeout);
                return;
            }
            p.packet(STX,sequence,payload.data(),
                     payload.size(),bad_crc);
            if(duplicate&&sequence==1)
                p.packet(STX,sequence,payload.data(),
                         payload.size());
            if(bad_crc)return;
            offset+=count;
            ++sequence;
        }
        p.input.push_back(EOT);
    };

    peer.on_output=[&](Peer& p,std::uint8_t value){
        if(stage==0&&value=='C'){
            if(!header_fault_sent&&
               fault==ReceiveFault::HeaderCrc){
                p.header_block(
                    header_marker,"LIVE.BIN",
                    original.size(),true);
                header_fault_sent=true;
                return;
            }
            if(!header_fault_sent&&
               fault==ReceiveFault::HeaderTimeout){
                p.input.push_back(header_marker);
                p.input.push_back(0);
                p.input.push_back(0xff);
                for(int i=0;i<8;++i)p.input.push_back(0);
                p.input.push_back(rmb::xmodem::timeout);
                header_fault_sent=true;
                return;
            }
            p.header_block(
                header_marker,"LIVE.BIN",original.size());
            stage=1;
            return;
        }
        if(stage==1&&value=='C'){
            if(fault==ReceiveFault::DataCrc){
                queue_data(p,true,false);
                stage=2;
            }else if(fault==ReceiveFault::DataTimeout){
                queue_data(p,false,true);
                stage=2;
            }else{
                queue_data(p,false,false);
                stage=3;
            }
            return;
        }
        if(stage==2&&value==NAK){
            queue_data(p,false,false);
            stage=3;
            return;
        }
        if(stage==3&&value==NAK){
            if(!one_eot)p.input.push_back(EOT);
            stage=4;
            return;
        }
        if(stage==4&&value=='C'){
            p.header_block(empty_marker,"",0);
            stage=5;
        }
    };

    auto io=peer.io();
    auto result=receive(io);
    peer.on_output={};
    return result;
}

std::string read_file(const std::string& name){
    std::ifstream file(name,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),{}};
}
}

int main(){
    assert(rmb::xmodem::crc16(
        reinterpret_cast<const std::uint8_t*>("123456789"),9)
        ==0x31c3);

    std::uint8_t payload[kHeaderSize];
    Header header;
    assert(encode_header("CPB_LARGE_400.BAS",6255,payload));
    assert(decode_header(payload,header));
    assert(!std::strcmp(
        header.filename,"CPB_LARGE_400.BAS"));
    assert(header.size==6255&&!header.empty);
    std::memset(payload,0,sizeof(payload));
    assert(decode_header(payload,header)&&header.empty);
    assert(!encode_header("",1,payload));
    std::memset(payload,'A',sizeof(payload));
    assert(!decode_header(payload,header));
    std::memset(payload,0,sizeof(payload));
    std::memcpy(payload,"TEST.BAS\0x",10);
    assert(!decode_header(payload,header));

    // Single-file receive compatibility and exact-size handling.
    for(auto size:{0u,1u,127u,128u,129u,1023u,
                   1024u,1025u,4110u,6255u}){
        Peer peer;
        auto original=bytes(size);
        peer.transfer("TEST.BIN",original);
        auto io=peer.io();
        auto r=receive(io);
        assert(r.error==Error::None&&r.bytes==size&&
               r.files==1&&peer.name=="TEST.BIN"&&
               peer.expected==size);
        assert(peer.data==original&&peer.finished);
        assert(peer.completed.size()==1);
        assert_file(peer,0,"TEST.BIN",original);
        assert(peer.output.front()=='C'&&
               peer.output.back()==ACK);
    }

    {Peer peer;auto original=bytes(129);
     peer.transfer("SMALL.BIN",original,true);
     auto io=peer.io();
     assert(receive(io).error==Error::None);
     assert(peer.data==original);}

    {Peer peer;auto original=bytes(1025);
     peer.transfer("DUP.BIN",original,false,true);
     auto io=peer.io();
     assert(receive(io).error==Error::None);
     assert(peer.data==original);}

    {Peer peer;auto original=bytes(2028);
     auto r=interactive_receive(
         peer,original,true,SOH,SOH);
     assert(r.error==Error::None&&
            r.bytes==original.size()&&r.files==1&&
            peer.data==original&&peer.finished);
     assert(std::find(peer.output.begin(),peer.output.end(),NAK)
            !=peer.output.end());}

    {Peer peer;auto original=bytes(2028);
     auto r=interactive_receive(
         peer,original,false,STX,STX);
     assert(r.error==Error::None&&peer.data==original&&
            peer.finished);}

    {Peer peer;auto original=bytes(129);
     auto r=interactive_receive(
         peer,original,true,SOH,STX,false,
         ReceiveFault::HeaderCrc);
     assert(r.error==Error::None&&peer.data==original);}

    {Peer peer;auto original=bytes(129);
     auto r=interactive_receive(
         peer,original,true,STX,SOH,false,
         ReceiveFault::HeaderTimeout);
     assert(r.error==Error::None&&peer.data==original);}

    {Peer peer;auto original=bytes(1025);
     auto r=interactive_receive(
         peer,original,true,SOH,SOH,true);
     assert(r.error==Error::None&&peer.data==original);}

    // A Windows/Tera Term sender can retransmit Block 0 if the
    // receiver's first ACK+C transition is not observed.  Before Data #1,
    // repeated copies of the same header must be answered with ACK+C again,
    // not treated as duplicate data sequence 0.
    {
        Peer peer;
        auto original=bytes(4110);
        int stage=0;
        unsigned repeated_headers=0;

        auto queue_data=[&](Peer& p){
            std::vector<std::uint8_t> payload(1024,0x1a);
            std::uint8_t sequence=1;
            for(std::size_t offset=0;offset<original.size();){
                const auto count=std::min<std::size_t>(
                    payload.size(),original.size()-offset);
                std::fill(payload.begin(),payload.end(),0x1a);
                std::copy_n(original.data()+offset,count,payload.data());
                p.packet(STX,sequence,payload.data(),payload.size());
                offset+=count;
                ++sequence;
            }
            p.input.push_back(EOT);
        };

        peer.on_output=[&](Peer& p,std::uint8_t value){
            if(stage==0&&value=='C'){
                p.header("REHEAD.BIN",original.size());
                stage=1;
                return;
            }
            if(stage==1&&value=='C'){
                if(repeated_headers<3){
                    p.header("REHEAD.BIN",original.size());
                    ++repeated_headers;
                    return;
                }
                queue_data(p);
                stage=2;
                return;
            }
            if(stage==2&&value==NAK){
                p.input.push_back(EOT);
                stage=3;
                return;
            }
            if(stage==3&&value=='C'){
                p.header("",0);
                stage=4;
            }
        };

        auto io=peer.io();
        auto r=receive(io);
        peer.on_output={};
        assert(r.error==Error::None&&r.files==1);
        assert(r.bytes==original.size());
        assert(repeated_headers==3);
        assert_file(peer,0,"REHEAD.BIN",original);
    }

    // If the peer misses the initial Block-0 ACK+C entirely and
    // does not start Data #1, the receiver sees a marker timeout. Re-state
    // ACK+C (not NAK); a sender waiting for CRC mode can then start Data #1.
    {
        Peer peer;
        auto original=bytes(4110);
        int stage=0;
        bool injected_timeout=false;

        auto queue_data=[&](Peer& p){
            std::vector<std::uint8_t> payload(1024,0x1a);
            std::uint8_t sequence=1;
            for(std::size_t offset=0;offset<original.size();){
                const auto count=std::min<std::size_t>(
                    payload.size(),original.size()-offset);
                std::fill(payload.begin(),payload.end(),0x1a);
                std::copy_n(original.data()+offset,count,payload.data());
                p.packet(STX,sequence,payload.data(),payload.size());
                offset+=count;
                ++sequence;
            }
            p.input.push_back(EOT);
        };

        peer.on_output=[&](Peer& p,std::uint8_t value){
            if(stage==0&&value=='C'){
                p.header("WAITDATA.BIN",original.size());
                stage=1;
                return;
            }
            if(stage==1&&value=='C'){
                if(!injected_timeout){
                    p.input.push_back(rmb::xmodem::timeout);
                    injected_timeout=true;
                    return;
                }
                queue_data(p);
                stage=2;
                return;
            }
            if(stage==2&&value==NAK){
                p.input.push_back(EOT);
                stage=3;
                return;
            }
            if(stage==3&&value=='C'){
                p.header("",0);
                stage=4;
            }
        };

        auto io=peer.io();
        auto r=receive(io);
        peer.on_output={};
        assert(r.error==Error::None&&r.files==1);
        assert(r.bytes==original.size());
        assert(injected_timeout);
        assert_file(peer,0,"WAITDATA.BIN",original);

        // The pre-data timeout recovery is ACK+C. NAK is reserved for data
        // phase retry/EOT handshake, so the first retry transition must
        // contain ACK immediately followed by C.
        bool saw_ack_c=false;
        for(std::size_t i=1;i<peer.output.size();++i)
            if(peer.output[i-1]==ACK&&peer.output[i]=='C')
                saw_ack_c=true;
        assert(saw_ack_c);
    }

    {Peer peer;auto original=bytes(129);
     auto r=interactive_receive(
         peer,original,true,SOH,SOH,false,
         ReceiveFault::DataTimeout);
     assert(r.error==Error::None&&peer.data==original);}

    {Peer peer;auto original=bytes(129);
     auto r=interactive_receive(
         peer,original,false,STX,STX,false,
         ReceiveFault::DataCrc);
     assert(r.error==Error::None&&peer.data==original);}

    {Peer peer;auto original=bytes(10);
     peer.header("CRC.BIN",10,true);
     peer.header("CRC.BIN",10);
     std::vector<std::uint8_t> block(1024,0x1a);
     std::copy(original.begin(),original.end(),block.begin());
     peer.packet(STX,1,block.data(),block.size());
     peer.input.push_back(EOT);
     peer.input.push_back(EOT);
     peer.header("",0);
     auto io=peer.io();
     assert(receive(io).error==Error::None);
     assert(std::find(peer.output.begin(),peer.output.end(),'C')
            !=peer.output.end());}

    // Three-file batch, including the reported 4110-byte/five-block case.
    {
        Peer peer;
        auto first=bytes(4110);
        auto second=bytes(129);
        auto third=bytes(1025);
        peer.file("FILE1.BAS",first);
        peer.file("FILE2.BIN",second);
        peer.file("FILE3.WAV",third);
        peer.finish_batch();
        auto io=peer.io();
        auto r=receive(io);
        assert(r.error==Error::None&&r.files==3);
        assert(r.bytes==first.size()+second.size()+third.size());
        assert(!std::strcmp(r.filename,"FILE3.WAV"));
        assert(peer.finish_calls==3&&peer.completed.size()==3);
        assert_file(peer,0,"FILE1.BAS",first);
        assert_file(peer,1,"FILE2.BIN",second);
        assert_file(peer,2,"FILE3.WAV",third);
        assert(peer.output.back()==ACK);
    }

    // Mixed sizes verify exact padding removal for every file in a batch.
    {
        Peer peer;
        const std::size_t sizes[]={1,128,129,1024,1025,4110};
        const char* names[]={
            "S1.BIN","S128.BIN","S129.BIN",
            "S1024.BIN","S1025.BIN","S4110.BIN"
        };
        std::vector<std::vector<std::uint8_t>> originals;
        std::uint32_t total=0;
        for(std::size_t i=0;i<6;++i){
            originals.push_back(bytes(sizes[i]));
            peer.file(names[i],originals.back());
            total+=static_cast<std::uint32_t>(sizes[i]);
        }
        peer.finish_batch();
        auto io=peer.io();
        auto r=receive(io);
        assert(r.error==Error::None&&r.files==6&&
               r.bytes==total);
        assert(peer.finish_calls==6);
        for(std::size_t i=0;i<6;++i)
            assert_file(peer,i,names[i],originals[i]);
    }

    // File #2 CRC retry and duplicate block never double-write.
    {
        Peer peer;
        auto first=bytes(128);
        auto second=bytes(1025);
        peer.file("FIRST.BIN",first);
        peer.file("RETRY.BIN",second,false,false,true);
        peer.finish_batch();
        auto io=peer.io();
        auto r=receive(io);
        assert(r.error==Error::None&&r.files==2);
        assert_file(peer,0,"FIRST.BIN",first);
        assert_file(peer,1,"RETRY.BIN",second);
        assert(std::find(peer.output.begin(),peer.output.end(),NAK)
               !=peer.output.end());
    }
    {
        Peer peer;
        auto first=bytes(1);
        auto second=bytes(1025);
        peer.file("FIRST.BIN",first);
        peer.file("DUP2.BIN",second,false,true);
        peer.finish_batch();
        auto io=peer.io();
        auto r=receive(io);
        assert(r.error==Error::None&&r.files==2);
        assert_file(peer,1,"DUP2.BIN",second);
    }

    // Cancelling file #2 preserves the already finished file #1 and never
    // calls finish for the incomplete file.
    {
        Peer peer;
        auto first=bytes(129);
        peer.file("DONE.BIN",first);
        peer.header("CANCEL.BIN",1024);
        peer.input.push_back(CAN);
        auto io=peer.io();
        auto r=receive(io);
        assert(r.error==Error::Cancelled&&r.files==1);
        assert(peer.finish_calls==1&&peer.completed.size()==1);
        assert_file(peer,0,"DONE.BIN",first);
        assert(peer.active);
    }

    // A malformed next Block 0 ends with PROTOCOL ERROR while file #1
    // remains committed.
    {
        Peer peer;
        auto first=bytes(129);
        peer.file("DONE.BIN",first);
        std::vector<std::uint8_t> malformed(128,'A');
        for(unsigned i=0;i<10;++i)
            peer.packet(SOH,0,malformed.data(),malformed.size());
        auto io=peer.io();
        auto r=receive(io);
        assert(r.error==Error::Protocol&&r.files==1);
        assert(peer.completed.size()==1);
        assert_file(peer,0,"DONE.BIN",first);
    }

    {Peer peer;peer.input.push_back(CAN);
     auto io=peer.io();
     assert(receive(io).error==Error::Cancelled);}

    {Peer peer;peer.header("../BAD",4);peer.begin_ok=false;
     auto io=peer.io();
     assert(receive(io).error==Error::Write);}

    {Peer peer;peer.header("SHORT.BIN",129);
     std::uint8_t block[128]={};
     peer.packet(SOH,1,block,128);
     peer.input.push_back(EOT);
     auto io=peer.io();
     assert(receive(io).error==Error::Protocol&&
            !peer.finished);}

    {Peer peer;peer.header("STOP.BIN",1);
     peer.input.push_back(CAN);
     auto io=peer.io();
     assert(receive(io).error==Error::Cancelled&&
            !peer.finished);}

    // Sender regressions remain single-file and unchanged.
    for(auto size:{0u,1u,127u,128u,129u,1023u,
                   1024u,1025u,6255u}){
        Peer peer;
        peer.source=bytes(size);
        peer.input.push_back('C');
        peer.input.push_back(ACK);
        peer.input.push_back('C');
        auto blocks=(size+1023)/1024;
        for(std::size_t i=0;i<blocks;++i)
            peer.input.push_back(ACK);
        peer.input.push_back(NAK);
        peer.input.push_back(ACK);
        peer.input.push_back('C');
        peer.input.push_back(ACK);
        auto io=peer.io();
        auto r=send(io,"TEST.BIN",size);
        assert(r.error==Error::None&&r.bytes==size&&r.files==1);
        assert(peer.output.size()==
               133+blocks*1029+2+133);
        assert(peer.output[0]==SOH&&peer.output[1]==0);
        Header sent;
        assert(decode_header(peer.output.data()+3,sent));
        assert(sent.size==size);
        std::vector<std::uint8_t> restored;
        std::size_t pos=133;
        for(std::size_t i=0;i<blocks;++i,pos+=1029)
            restored.insert(
                restored.end(),
                peer.output.begin()+pos+3,
                peer.output.begin()+pos+3+1024);
        restored.resize(size);
        assert(restored==peer.source);
        assert(peer.output[pos]==EOT&&
               peer.output[pos+1]==EOT);
        Header terminal;
        assert(decode_header(
            peer.output.data()+pos+5,terminal)&&
            terminal.empty);
    }

    {Peer peer;peer.source=bytes(1);
     peer.input={'C',NAK,ACK,'C',ACK,NAK,ACK,'C',ACK};
     auto io=peer.io();
     assert(send(io,"RETRY.BIN",1).error==Error::None);}

    {Peer peer;peer.source=bytes(1);
     peer.input={'C',ACK,'C',CAN};
     auto io=peer.io();
     assert(send(io,"CANCEL.BIN",1).error==
            Error::Cancelled);}

    // Existing transactional file safety remains intact.
    char directory[]="/tmp/rmb-ymodem-XXXXXX";
    assert(mkdtemp(directory));
    std::string root=std::string(directory)+"/";
    {std::ofstream file(root+"KEEP.BIN",std::ios::binary);
     file<<"original";}
    {rmb::TransferFile file;
     assert(file.open("KEEP.BIN",true,root.c_str(),"YMODEM"));
     auto value=bytes(129);
     assert(file.write_exact(value.data(),value.size()));
     assert(!file.commit_exact(130));}
    assert(read_file(root+"KEEP.BIN")=="original");
    assert(!std::filesystem::exists(root+"YMODEM.TMP"));
    {rmb::TransferFile file;
     auto value=bytes(6255);
     assert(file.open("KEEP.BIN",true,root.c_str(),"YMODEM"));
     assert(file.write_exact(value.data(),1024));
     assert(file.write_exact(
         value.data()+1024,value.size()-1024));
     assert(file.commit_exact(value.size()));
     assert(file.bytes()==6255);}
    assert(read_file(root+"KEEP.BIN").size()==6255);

    // The Repl keeps one TransferFile in its YMODEM context. Verify that the
    // same object can commit three exact files in sequence, then abort an
    // incomplete fourth file without disturbing the completed files.
    {
        rmb::TransferFile file;
        const char* names[]={
            "BATCH1.BIN","BATCH2.BIN","BATCH3.BIN"
        };
        const std::size_t sizes[]={1,129,4110};
        for(std::size_t i=0;i<3;++i){
            auto value=bytes(sizes[i]);
            assert(file.open(
                names[i],true,root.c_str(),"YMODEM"));
            assert(file.write_exact(value.data(),value.size()));
            assert(file.commit_exact(value.size()));
            assert(read_file(root+names[i])==
                   std::string(
                       reinterpret_cast<const char*>(value.data()),
                       value.size()));
        }
        auto partial=bytes(64);
        assert(file.open(
            "BATCH4.BIN",true,root.c_str(),"YMODEM"));
        assert(file.write_exact(partial.data(),partial.size()));
        file.abort();
        assert(!std::filesystem::exists(root+"BATCH4.BIN"));
        assert(!std::filesystem::exists(root+"YMODEM.TMP"));
        assert(read_file(root+"BATCH1.BIN").size()==1);
        assert(read_file(root+"BATCH2.BIN").size()==129);
        assert(read_file(root+"BATCH3.BIN").size()==4110);
    }

    {rmb::TransferFile file;
     assert(!file.open(
         "../BAD",true,root.c_str(),"YMODEM"));
     assert(!file.open(
         "RMBP0000.BAS",true,root.c_str(),"YMODEM"));}
    rmb::program_files::set_active("ACTIVE.BAS");
    {rmb::TransferFile file;
     assert(!file.open(
         "active.bas",true,root.c_str(),"YMODEM"));
     assert(!std::strcmp(file.error(),"FILE IN USE"));}
    rmb::program_files::set_active(nullptr);
    std::filesystem::remove_all(root);

    std::puts(
        "YMODEM single/batch, exact size, retry, duplicate, "
        "cancel and file safety tests passed"
    );
}
