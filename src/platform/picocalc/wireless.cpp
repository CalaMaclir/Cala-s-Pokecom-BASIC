#include "wireless.hpp"
#include <cstdio>
#include "pico/cyw43_arch.h"
namespace rmb::wireless {
namespace { bool ready=false; char error[64]="NOT INITIALIZED"; }
bool init(){if(ready)return true;if(cyw43_arch_init()!=0){std::snprintf(error,sizeof(error),"CYW43 INIT FAILED");return false;}ready=true;std::snprintf(error,sizeof(error),"READY");return true;}
bool initialized(){return ready;}
const char* last_error(){return error;}
} // namespace rmb::wireless
