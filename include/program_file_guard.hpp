#pragma once
#include <cctype>
#include <cstdio>
#include <cstring>
namespace rmb::program_files {
inline char active[80] = {};
inline bool equal(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) if (std::toupper(static_cast<unsigned char>(*a++)) !=
                        std::toupper(static_cast<unsigned char>(*b++))) return false;
    return *a == *b;
}
inline bool reserved(const char* name) {
    if (!name) return false;
    char prefix[5] = {};
    for (int i=0; i<4 && name[i]; ++i) prefix[i]=std::toupper(static_cast<unsigned char>(name[i]));
    return std::strcmp(prefix,"RMBP")==0 || std::strcmp(prefix,"RMBE")==0;
}
inline bool transfer_staging(const char* name) {
    return equal(name, "XMODEM.TMP") || equal(name, "XMODEM.BAK") ||
           equal(name, "YMODEM.TMP") || equal(name, "YMODEM.BAK") ||
           equal(name, "HTTP.TMP") || equal(name, "HTTP.BAK");
}
inline bool session_staging(const char* name) {
    return equal(name, "RMBSES.TMP") || equal(name, "RMBSES.BAK");
}
inline bool visible_for_transfer(const char* name) {
    return name && *name && !reserved(name) && !transfer_staging(name) &&
           !session_staging(name);
}
inline bool visible_in_directory(const char* name) {
    return visible_for_transfer(name);
}
inline bool in_use(const char* name) { return reserved(name) || (*active && equal(name,active)); }
inline void set_active(const char* name) { std::snprintf(active,sizeof(active),"%s",name ? name : ""); }
}
