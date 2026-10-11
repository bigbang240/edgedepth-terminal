#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/activity_strip.h"
#include <cassert>
using nlohmann::json;
int main(){
    const int64_t t=1791390480000;
    json j={{"v",1},{"exchange","binancef"},{"symbol","metusdt"},{"from",t},{"to",t+60000},{"clock",t+60000},{"activity_status","available"},
        {"points",json::array({{{"t",t},{"trades",3.94},{"volume",6.5},{"oi",nullptr},{"buy",.61},{"longs",0},{"shorts",50000},{"state","forced"}}})}};
    auto data=activity_strip::parse(j,"metusdt",t,t+60000,t+60000);assert(data.points.size()==1&&std::isnan(data.points[0].oi));
    assert(activity_strip::parse(j,"other",t,t+60000,t+60000).points.empty());
    assert(activity_strip::parse(j,"metusdt",t,t+60000,t).points.empty());
    j["points"][0]["t"]=t+60000;assert(activity_strip::parse(j,"metusdt",t,t+60000,t+60000).points.empty());
    j["points"][0]["t"]=t;j["points"][0]["buy"]=1.1;assert(activity_strip::parse(j,"metusdt",t,t+60000,t+60000).points.empty());
    for(const char* field:{"v","exchange","symbol","activity_status"}){auto bad=j;bad[field]=json::array();assert(activity_strip::parse(bad,"metusdt",t,t+60000,t+60000).points.empty());}
    activity_strip::Book book;book.observe(t+59000,.6);assert(book.closed.empty());book.observe(t+61000,.4);assert(book.closed.at(t)==.6);
    book.observe(t+180000,.7);assert(book.closed.count(t+60000)==0);book.observe(t-1000,.5);assert(book.closed.empty());
}
