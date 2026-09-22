#include "ymodem.hpp"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <climits>

namespace rmb::ymodem {
namespace {
constexpr std::uint8_t SOH=1, STX=2, EOT=4, ACK=6, NAK=21, CAN=24;
constexpr unsigned retries=10, startup_retries=30;
bool control(IO& io,std::uint8_t value){return io.write(io.context,&value,1);}
Error input_error(int c){return c==xmodem::cancelled||c==CAN?Error::Cancelled:
    c==xmodem::disconnected?Error::Disconnected:Error::Timeout;}
int drain(IO& io){for(unsigned i=0;i<2048;++i){int c=io.read(io.context,100);if(c<0)return c;}return xmodem::timeout;}
Result result(Error error,std::uint32_t bytes,const char* name=nullptr){
    Result out;out.error=error;out.bytes=bytes;
    if(name)std::snprintf(out.filename,sizeof(out.filename),"%s",name);
    return out;
}
Result fail(IO& io,Error error,std::uint32_t bytes,const char* name=nullptr){
    const std::uint8_t cancel[]={CAN,CAN,CAN};io.write(io.context,cancel,sizeof(cancel));
    return result(error,bytes,name);
}
struct Packet {std::uint8_t sequence=0;std::size_t size=0;std::uint8_t data[kDataSize]={};};
Error read_packet(IO& io,int marker,Packet& packet){
    if(marker!=SOH&&marker!=STX)return Error::Protocol;
    packet.size=marker==SOH?kHeaderSize:kDataSize;
    int seq=io.read(io.context,1000),inverse=io.read(io.context,1000);
    if(seq<0||inverse<0)return input_error(seq<0?seq:inverse);
    if(static_cast<std::uint8_t>(seq^inverse)!=0xff)return Error::Protocol;
    packet.sequence=static_cast<std::uint8_t>(seq);
    for(std::size_t i=0;i<packet.size;++i){
        int c=io.read(io.context,1000);if(c<0)return input_error(c);
        packet.data[i]=static_cast<std::uint8_t>(c);
    }
    int high=io.read(io.context,1000),low=io.read(io.context,1000);
    if(high<0||low<0)return input_error(high<0?high:low);
    auto received=static_cast<std::uint16_t>((high<<8)|low);
    return xmodem::crc16(packet.data,packet.size)==received?Error::None:Error::Crc;
}
bool write_packet(IO& io,std::uint8_t marker,std::uint8_t sequence,
                  const std::uint8_t* data,std::size_t size){
    std::uint8_t prefix[]={marker,sequence,static_cast<std::uint8_t>(~sequence)};
    auto crc=xmodem::crc16(data,size);std::uint8_t suffix[]={static_cast<std::uint8_t>(crc>>8),static_cast<std::uint8_t>(crc)};
    return io.write(io.context,prefix,sizeof(prefix))&&io.write(io.context,data,size)&&io.write(io.context,suffix,sizeof(suffix));
}
Error transmit(IO& io,std::uint8_t marker,std::uint8_t sequence,
               const std::uint8_t* data,std::size_t size){
    Error last=Error::Timeout;
    for(unsigned attempt=0;attempt<retries;++attempt){
        if(!write_packet(io,marker,sequence,data,size))return Error::Disconnected;
        int c=io.read(io.context,3000);
        if(c==ACK)return Error::None;
        if(c==CAN||c==xmodem::cancelled||c==xmodem::disconnected)return input_error(c);
        last=c==NAK?Error::Crc:c<0?Error::Timeout:Error::Protocol;
    }
    return last;
}
}

bool encode_header(const char* filename,std::uint32_t size,std::uint8_t payload[kHeaderSize]){
    if(!filename||!*filename||std::strlen(filename)>=kFilenameSize)return false;
    std::memset(payload,0,kHeaderSize);auto length=std::strlen(filename);
    std::memcpy(payload,filename,length);
    char value[16];int n=std::snprintf(value,sizeof(value),"%lu",static_cast<unsigned long>(size));
    if(length+1+static_cast<std::size_t>(n)+1>kHeaderSize)return false;
    std::memcpy(payload+length+1,value,n);return true;
}
bool decode_header(const std::uint8_t payload[kHeaderSize],Header& header){
    header=Header{};
    std::size_t name=0;while(name<kHeaderSize&&payload[name])++name;
    if(name==kHeaderSize||name>=kFilenameSize)return false;
    if(name==0){header.empty=true;return true;}
    std::memcpy(header.filename,payload,name);header.filename[name]=0;
    std::size_t pos=name+1;if(pos>=kHeaderSize||!std::isdigit(payload[pos]))return false;
    std::uint64_t size=0;
    while(pos<kHeaderSize&&std::isdigit(payload[pos])){
        size=size*10+(payload[pos++]-'0');if(size>UINT32_MAX)return false;
    }
    if(pos<kHeaderSize&&payload[pos]!=0&&payload[pos]!=' ')return false;
    header.size=static_cast<std::uint32_t>(size);return true;
}

Result receive(IO& io){
    Packet packet;Header header;unsigned waiting=0;
    if(!control(io,'C'))return result(Error::Disconnected,0);
    while(true){
        int marker=io.read(io.context,2000);
        if(marker==CAN||marker==xmodem::cancelled||marker==xmodem::disconnected)
            return fail(io,input_error(marker),0);
        if((marker=='\r'||marker=='\n')&&waiting++<startup_retries)continue;
        if(marker<0){if(++waiting>=startup_retries)return fail(io,Error::Timeout,0);control(io,'C');continue;}
        Error error=read_packet(io,marker,packet);
        if(error==Error::Timeout)drain(io);
        if(error!=Error::None||marker!=SOH||packet.sequence!=0||
           !decode_header(packet.data,header)){
            if(++waiting>=retries)return fail(io,error==Error::None?Error::Protocol:error,0);
            control(io,'C');continue;
        }
        break;
    }
    if(header.empty){control(io,ACK);return result(Error::None,0);}
    if(!io.begin_receive(io.context,header.filename,header.size))
        return fail(io,Error::Write,0,header.filename);
    if(!control(io,ACK)||!control(io,'C'))return result(Error::Disconnected,0,header.filename);
    std::uint8_t expected=1;std::uint32_t bytes=0;unsigned errors=0;
    while(true){
        int marker=io.read(io.context,3000);
        if(marker==CAN||marker==xmodem::cancelled||marker==xmodem::disconnected)
            return fail(io,input_error(marker),bytes,header.filename);
        if(marker==EOT){
            if(bytes!=header.size)return fail(io,Error::Protocol,bytes,header.filename);
            if(!control(io,NAK))return result(Error::Disconnected,bytes,header.filename);
            int second=io.read(io.context,3000);
            if(second==CAN||second==xmodem::cancelled||second==xmodem::disconnected)
                return fail(io,input_error(second),bytes,header.filename);
            if(second!=EOT)return fail(io,second<0?Error::Timeout:Error::Protocol,bytes,header.filename);
            if(!io.finish(io.context))return fail(io,Error::Write,bytes,header.filename);
            if(!control(io,ACK)||!control(io,'C'))return result(Error::Disconnected,bytes,header.filename);
            for(unsigned attempt=0;attempt<retries;++attempt){
                int end=io.read(io.context,3000);
                if(end==CAN||end==xmodem::cancelled||end==xmodem::disconnected)
                    return fail(io,input_error(end),bytes,header.filename);
                Error error=read_packet(io,end,packet);Header empty;
                if(error==Error::Timeout)drain(io);
                if(error==Error::None&&end==SOH&&packet.sequence==0&&
                   decode_header(packet.data,empty)&&empty.empty){
                    control(io,ACK);return result(Error::None,bytes,header.filename);
                }
                if(!control(io,'C'))return result(Error::Disconnected,bytes,header.filename);
            }
            return fail(io,Error::Protocol,bytes,header.filename);
        }
        Error error=marker<0?input_error(marker):read_packet(io,marker,packet);
        if(error==Error::Timeout&&marker>=0)drain(io);
        if(error==Error::None&&(marker==SOH||marker==STX)){
            if(packet.sequence==expected){
                if(bytes>=header.size)return fail(io,Error::Protocol,bytes,header.filename);
                std::size_t write=packet.size;
                if(write>header.size-bytes)write=header.size-bytes;
                if(!io.sink(io.context,packet.data,write))return fail(io,Error::Write,bytes,header.filename);
                bytes+=write;++expected;errors=0;
                if(!control(io,ACK))return result(Error::Disconnected,bytes,header.filename);
                continue;
            }
            if(packet.sequence==static_cast<std::uint8_t>(expected-1)){
                if(!control(io,ACK))return result(Error::Disconnected,bytes,header.filename);
                if(++errors>=retries)return fail(io,Error::Protocol,bytes,header.filename);
                continue;
            }
            return fail(io,Error::Protocol,bytes,header.filename);
        }
        if(++errors>=retries)return fail(io,error,bytes,header.filename);
        if(!control(io,NAK))return result(Error::Disconnected,bytes,header.filename);
    }
}

Result send(IO& io,const char* filename,std::uint32_t size){
    bool ready=false;
    for(unsigned i=0;i<startup_retries;++i){
        int c=io.read(io.context,2000);
        if(c=='C'){ready=true;break;}
        if(c==CAN||c==xmodem::cancelled||c==xmodem::disconnected)return fail(io,input_error(c),0,filename);
    }
    if(!ready)return fail(io,Error::Timeout,0,filename);
    std::uint8_t header[kHeaderSize];if(!encode_header(filename,size,header))return fail(io,Error::Protocol,0,filename);
    Error error=transmit(io,SOH,0,header,sizeof(header));if(error!=Error::None)return fail(io,error,0,filename);
    int request=io.read(io.context,3000);
    if(request!='C')return fail(io,request<0?input_error(request):Error::Protocol,0,filename);
    std::uint32_t bytes=0;std::uint8_t sequence=1;
    while(bytes<size){
        std::uint8_t data[kDataSize];std::memset(data,0x1a,sizeof(data));
        std::size_t wanted=size-bytes;if(wanted>sizeof(data))wanted=sizeof(data);
        int n=io.source(io.context,data,wanted);
        if(n<0||static_cast<std::size_t>(n)!=wanted)return fail(io,Error::Read,bytes,filename);
        error=transmit(io,STX,sequence,data,sizeof(data));if(error!=Error::None)return fail(io,error,bytes,filename);
        bytes+=n;++sequence;
    }
    bool acknowledged=false;
    for(unsigned attempt=0;attempt<retries&&!acknowledged;++attempt){
        if(!control(io,EOT))return result(Error::Disconnected,bytes,filename);
        int c=io.read(io.context,3000);
        if(c==ACK){acknowledged=true;break;}
        if(c==NAK){
            if(!control(io,EOT))return result(Error::Disconnected,bytes,filename);
            c=io.read(io.context,3000);if(c==ACK){acknowledged=true;break;}
        }
        if(c==CAN||c==xmodem::cancelled||c==xmodem::disconnected)return fail(io,input_error(c),bytes,filename);
    }
    if(!acknowledged)return fail(io,Error::Timeout,bytes,filename);
    request=io.read(io.context,3000);if(request!='C')return fail(io,request<0?input_error(request):Error::Protocol,bytes,filename);
    std::memset(header,0,sizeof(header));error=transmit(io,SOH,0,header,sizeof(header));
    return error==Error::None?result(Error::None,bytes,filename):fail(io,error,bytes,filename);
}
const char* error_text(Error error){
    switch(error){
        case Error::None:return "TRANSFER COMPLETE";
        case Error::Timeout:return "TIMEOUT";
        case Error::Cancelled:return "CANCELLED";
        case Error::Disconnected:return "DISCONNECTED";
        case Error::Crc:return "CRC ERROR";
        case Error::Read:return "SD READ ERROR";
        case Error::Write:return "SD WRITE ERROR";
        default:return "PROTOCOL ERROR";
    }
}
}
