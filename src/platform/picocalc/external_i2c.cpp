#include "external_i2c.hpp"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
namespace rmb::picocalc::external_i2c {
namespace { constexpr uint kSda=4,kScl=5; constexpr uint32_t kHz=100000,kTimeout=10000; bool initialized=false,locked=false;
bool valid(std::uint8_t a){return a>=0x08&&a<=0x77;} Result result(int v,int wanted){if(v==wanted)return Result::Ok;return v==PICO_ERROR_TIMEOUT?Result::Timeout:Result::Nack;}
struct Lock { bool held; Lock():held(try_lock()){} ~Lock(){if(held)unlock();} }; }
void init(){if(initialized)return;i2c_init(i2c0,kHz);gpio_set_function(kSda,GPIO_FUNC_I2C);gpio_set_function(kScl,GPIO_FUNC_I2C);gpio_pull_up(kSda);gpio_pull_up(kScl);initialized=true;}
void reconfigure_bus_clock(){if(initialized)i2c_set_baudrate(i2c0,kHz);}
bool try_lock(){if(locked)return false;locked=true;return true;} void unlock(){locked=false;}
Result scan(std::uint8_t* a,int cap,int& count){init();Lock l;if(!l.held)return Result::Busy;count=0;std::uint8_t probe=0;for(int x=0x08;x<=0x77;++x){if(i2c_read_timeout_us(i2c0,x,&probe,1,false,kTimeout)==1){if(a&&count<cap)a[count]=static_cast<std::uint8_t>(x);++count;}}return Result::Ok;}
Result read_registers(std::uint8_t a,std::uint8_t reg,std::uint8_t* data,int n){if(!valid(a))return Result::BadAddress;init();Lock l;if(!l.held)return Result::Busy;int v=i2c_write_timeout_us(i2c0,a,&reg,1,true,kTimeout);if(v!=1)return result(v,1);return result(i2c_read_timeout_us(i2c0,a,data,n,false,kTimeout),n);}
Result read_register(std::uint8_t a,std::uint8_t r,std::uint8_t& v){return read_registers(a,r,&v,1);}
Result write_bytes(std::uint8_t a,const std::uint8_t* d,int n){if(!valid(a))return Result::BadAddress;init();Lock l;if(!l.held)return Result::Busy;return result(i2c_write_timeout_us(i2c0,a,d,n,false,kTimeout),n);}
Result write_register(std::uint8_t a,std::uint8_t r,std::uint8_t v){const std::uint8_t d[2]={r,v};return write_bytes(a,d,2);}
}