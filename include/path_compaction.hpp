#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include "file_path.hpp"
namespace rmb::file_paths {
constexpr std::size_t display_capacity = capacity + 1; // 79-byte path + slash + NUL
// Empty file/unassigned stays empty; an explicitly requested root directory is "/".
inline void format_root_path(const char* input,char* output,std::size_t size,bool directory=false) {
    if(!output||!size)return;
    output[0]=0;
    if(!input)return;
    while(*input=='/')++input;
    if(!*input&&!directory)return;
    std::snprintf(output,size,"/%s",input);
}
enum class CompactPathPolicy { FullName, ProgramName, BasenameOnly };
// Display-only; input is never modified. Width and output capacity both bound the result.
inline void compact_path(const char* input,std::size_t width,CompactPathPolicy policy,
                         char* output,std::size_t size) {
    if(!output||!size)return;
    width=std::min(width,size-1);output[0]=0;
    if(!input||!width)return;
    const char* path=policy==CompactPathPolicy::BasenameOnly?basename(input):input;
    const auto length=std::strlen(path);
    if(length<=width){std::memmove(output,path,length+1);return;}
    if(*path=='/' && policy!=CompactPathPolicy::BasenameOnly) {
        output[0]='/';output[1]=0;
        compact_path(path+1,width-1,policy,output+1,size-1);return;
    }
    char name[display_capacity]={};std::snprintf(name,sizeof(name),"%s",path);
    if(policy==CompactPathPolicy::ProgramName) {
        const auto n=std::strlen(name);
        if(n>=4&&same(name+n-4,".BAS"))name[n-4]=0;
    }
    const char* base=basename(name);const std::size_t base_length=std::strlen(base);
    if(base_length>width) {
        if(width<=2){std::memset(output,'.',width);output[width]=0;return;}
        const auto left=(width-2+1)/2,right=width-2-left;
        std::memcpy(output,base,left);std::memcpy(output+left,"..",2);
        std::memcpy(output+left+2,base+base_length-right,right);output[width]=0;return;
    }
    if(base==name) {std::memcpy(output,base,base_length+1);return;}
    // Keep as many complete trailing components as fit behind "../".
    const char* suffix=base;
    while(suffix>name) {
        const char* previous=suffix-2;
        while(previous>name&&previous[-1]!='/')--previous;
        if(previous==name||std::strlen(previous)+3>width)break;
        suffix=previous;
    }
    const auto suffix_length=std::strlen(suffix);
    if(suffix_length+3>width) {std::memcpy(output,base,base_length+1);return;}
    const char* slash=std::strchr(name,'/');
    const auto first_length=static_cast<std::size_t>(slash-name);
    const auto prefix=std::min(first_length>0?first_length-1:0,width-3-suffix_length);
    std::memcpy(output,name,prefix);std::memcpy(output+prefix,"../",3);
    std::memcpy(output+prefix+3,suffix,suffix_length+1);
}
} // namespace rmb::file_paths
namespace rmb::file_paths {
inline void compact_root_path(const char* input,std::size_t width,CompactPathPolicy policy,
                              char* output,std::size_t size,bool directory=false) {
    char rooted[display_capacity]={};format_root_path(input,rooted,sizeof(rooted),directory);
    compact_path(rooted,width,policy,output,size);
}
}
