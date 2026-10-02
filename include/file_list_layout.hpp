#pragma once
#include "path_compaction.hpp"
#include "storage.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
namespace rmb::file_list {
constexpr std::size_t attribute_columns=6;
constexpr std::size_t size_columns=10;
// Every cell, including trailing blank cells, is painted with the row colors.
inline void format_row(const storage::DirectoryEntry* entry,bool show_size,
                       char* out,std::size_t size,std::size_t columns=53,
                       const char* empty=nullptr) {
    if(!out||!size)return;
    columns=std::min(columns,size-1);
    std::memset(out,' ',columns);out[columns]=0;
    if(!entry) {
        if(empty)std::memcpy(out,empty,std::min(columns,std::strlen(empty)));
        return;
    }
    if(entry->directory)std::memcpy(out,"[DIR] ",std::min(columns,attribute_columns));
    const auto reserved=show_size&&!entry->directory?size_columns+1:0;
    if(columns<attribute_columns+reserved)return;
    const auto name_columns=columns-attribute_columns-reserved;
    char name[80]={};
    file_paths::compact_path(entry->name,name_columns,file_paths::CompactPathPolicy::BasenameOnly,name,sizeof(name));
    std::memcpy(out+attribute_columns,name,std::strlen(name));
    if(reserved) {
        char digits[size_columns+1]={};
        std::snprintf(digits,sizeof(digits),"%10lu",static_cast<unsigned long>(entry->size));
        std::memcpy(out+columns-size_columns,digits,size_columns);
    }
}
}
