#include "wifi_profiles.hpp"
#include <cassert>
#include <string>
using namespace rmb::wifi_profiles;
int main() {
    static_assert(kProfileActionCount == 4);
    static_assert(static_cast<int>(ProfileAction::ConnectNow) == 0);
    static_assert(static_cast<int>(ProfileAction::ToggleEnabled) == 1);
    static_assert(static_cast<int>(ProfileAction::Delete) == 2);
    static_assert(static_cast<int>(ProfileAction::Back) == 3);
    Profiles ps{};
    assert(count(ps) == 0 && count(ps, true) == 0);
    assert(upsert(ps,"HOME","secret") == 0 && count(ps) == 1);
    ps[0].enabled = false;
    assert(upsert(ps,"HOME","changed") == 0 && !ps[0].enabled);
    assert(std::strcmp(ps[0].password,"changed") == 0);
    for (int i=1;i<5;++i) assert(upsert(ps, ("AP"+std::to_string(i)).c_str(),"") == i);
    const auto before = ps[0];
    assert(upsert(ps,"SIX","pw") < 0 && count(ps) == 5);
    assert(std::memcmp(&before,&ps[0],sizeof(before)) == 0);
    erase(ps,2); assert(!ps[2].used && !ps[2].enabled && !ps[2].password[0]);
    assert(upsert(ps,"OPEN","") == 2 && ps[2].enabled && !ps[2].password[0]);
    erase(ps,2); assert(upsert(ps,"SECURE","pw") == 2);
    assert(upsert(ps,std::string(33,'X').c_str(),"pw") < 0);
    assert(upsert(ps,"HOME",std::string(64,'X').c_str()) < 0);
    assert(upsert(ps,"BAD\nSSID","pw") < 0);
    Profiles legacy{};
    migrate(legacy,false,"OLD","old-password");
    assert(count(legacy) == 1 && legacy[0].enabled && std::strcmp(legacy[0].password,"old-password") == 0);
    Profiles fresh{};
    assert(parse_line(fresh,"wifi_profile3_ssid=NEW"));
    assert(parse_line(fresh,"wifi_profile3_password=new-secret"));
    assert(parse_line(fresh,"wifi_profile3_enabled=off"));
    migrate(fresh,true,"OLD","old-password");
    assert(count(fresh) == 1 && fresh[2].used && !fresh[2].enabled && !fresh[0].used);
    Profiles empty{}; migrate(empty,false,"",""); assert(count(empty)==0);
    migrate(empty,true,"OLD","pw"); assert(count(empty)==0);
    Profiles max{};
    for(int i=0;i<5;++i) {
        std::string ssid(32,'S'); ssid[0]='1'+i; ssid[31]=' ';
        const std::string password=" "+std::string(61,'=')+" ";
        assert(upsert(max,ssid.c_str(),password.c_str())==i); max[i].enabled=(i%2)==0;
    }
    char config[3072]; const int size=serialize(max,config,sizeof(config));
    assert(size>0 && size<900);
    Profiles reload{};
    char* cursor=config;
    while(*cursor) {
        char* line=cursor; while(*cursor && *cursor!='\n')++cursor;
        if(*cursor)*cursor++=0;
        assert(parse_line(reload,line));
    }
    normalize(reload);
    assert(std::memcmp(max,reload,sizeof(max))==0);
    char small[8]; assert(serialize(max,small,sizeof(small))<0);
    Profiles select{}; upsert(select,"HOME","pw"); upsert(select,"OFFICE","pw");
    upsert(select,"MOBILE","pw"); select[2].enabled=false;
    rmb::network::AccessPoint aps[4]{};
    std::strcpy(aps[0].ssid,"HOME");aps[0].rssi=-65;
    std::strcpy(aps[1].ssid,"OFFICE");aps[1].rssi=-40;
    std::strcpy(aps[2].ssid,"MOBILE");aps[2].rssi=-30;
    std::strcpy(aps[3].ssid,"HOME");aps[3].rssi=-80;
    Candidate c[kCount]; assert(candidates(select,aps,4,c)==2 && c[0].slot==1 && c[1].slot==0);
    auto plan=make_plan(select,aps,4); Candidate next{};
    // A failed strongest candidate advances to the next candidate; exhaustion is deterministic.
    assert(next_candidate(plan,next) && next.slot==1);
    assert(next_candidate(plan,next) && next.slot==0);
    assert(!next_candidate(plan,next));
    aps[0].rssi=-40;assert(candidates(select,aps,4,c)==2 && c[0].slot==0);
    assert(candidates(select,aps,1,c)==1 && c[0].slot==0);
    assert(candidates(select,aps,0,c)==0);
    select[0].enabled=select[1].enabled=false; assert(candidates(select,aps,4,c)==0);
    std::puts("Profiles, transactional updates, legacy authority, maximum roundtrip and RSSI selection: PASS");
}
