#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace activity_strip {
inline constexpr int64_t minute=60000;
inline constexpr double missing=std::numeric_limits<double>::quiet_NaN();
struct Point { int64_t t=0; double trades=missing,volume=missing,oi=missing,buy=missing,longs=missing,shorts=missing; std::string state; };
struct Line { std::string id,label,source; int64_t known=0,origin=0,touch=0,support_from=0,until=0; double price=0,slope=0,support=0,support_slope=0; bool range=false;
    double top(double t) const { return price+slope*(t-double(origin)); }
    double bottom(double t) const { return support+support_slope*(t-double(support_from)); }
};
struct Marker { int64_t t=0,known=0; double price=0; std::string label; bool setup=false; };
struct Data { std::vector<Point> points; std::vector<Line> lines; std::vector<Marker> markers; std::string status="Loading",alerts_status; };
inline bool number(const nlohmann::json& j,const char* key) { auto i=j.find(key);return i!=j.end()&&i->is_number()&&std::isfinite(i->get<double>()); }
inline double optional(const nlohmann::json& j,const char* key) { return number(j,key)?j[key].get<double>():missing; }
inline Data parse(const nlohmann::json& j,const std::string& symbol,int64_t from,int64_t to,int64_t clock) {
    Data out;out.status="Unavailable";
    if(!j.is_object()||!number(j,"v")||j["v"]!=1||!j.contains("exchange")||j["exchange"]!="binancef"||!j.contains("symbol")||j["symbol"]!=symbol||
       !j.contains("activity_status")||!j["activity_status"].is_string()||
       !number(j,"from")||!number(j,"to")||!number(j,"clock")||j["from"]!=from||j["to"]!=to||j["clock"]!=clock)return out;
    if(!j.contains("points")||!j["points"].is_array()||j["points"].size()>240)return out;
    int64_t previous=0;
    for(const auto& row:j["points"]) {
        if(!row.is_object()||(!number(row,"t")||!row["t"].is_number_integer())||!row.contains("state")||!row["state"].is_string()){out.points.clear();return out;}
        const int64_t t=row["t"];if(t%minute||t<from||t+minute>to||t<=previous){out.points.clear();return out;}previous=t;
        Point p{t,optional(row,"trades"),optional(row,"volume"),optional(row,"oi"),optional(row,"buy"),optional(row,"longs"),optional(row,"shorts"),row["state"].get<std::string>()};
        if(p.trades<0||p.volume<0||p.buy<0||p.buy>1||p.oi<=-1||p.longs<0||p.shorts<0){out.points.clear();return out;}out.points.push_back(p);
    }
    out.status=j.value("activity_status",std::string())=="available"?"":"Unavailable";
    const auto alerts=j.find("alerts");if(alerts==j.end()||!alerts->is_object())return out;
    if(!alerts->contains("status")||!(*alerts)["status"].is_string())return out;
    out.alerts_status=(*alerts)["status"].get<std::string>();
    if(alerts->contains("lines")&&(*alerts)["lines"].is_array()&&(*alerts)["lines"].size()<=8)for(const auto& row:(*alerts)["lines"]){
        if(!row.is_object())continue;
        bool valid=true;for(const char* key:{"known","origin","touch","supportFrom","until","price","slope","support","supportSlope"})valid=valid&&number(row,key);
        for(const char* key:{"id","label","source"})valid=valid&&row.contains(key)&&row[key].is_string();
        for(const char* key:{"known","origin","touch","supportFrom","until"})valid=valid&&row.contains(key)&&row[key].is_number_integer()&&row[key].get<double>()>0&&row[key].get<double>()<=clock;
        if(!valid)continue;
        Line l{row["id"],row["label"],row["source"],row["known"],row["origin"],row["touch"],row["supportFrom"],row["until"],row["price"],row["slope"],row["support"],row["supportSlope"]};
        l.range=l.id.rfind("range/",0)==0;
        if(l.known<=clock&&l.origin>0&&l.origin<l.touch&&l.touch<=l.known&&l.support_from>0&&l.price>0&&l.support>0&&l.slope<=0)out.lines.push_back(l);
    }
    if(alerts->contains("markers")&&(*alerts)["markers"].is_array()&&(*alerts)["markers"].size()<=256)for(const auto& row:(*alerts)["markers"]){
        if(!row.is_object()||!number(row,"t")||!number(row,"known")||!number(row,"price")||!row.contains("label")||!row["label"].is_string()||!row.contains("setup")||!row["setup"].is_boolean())continue;
        if(!row["t"].is_number_integer()||!row["known"].is_number_integer()||row["t"].get<double>()<from||row["t"].get<double>()>clock||row["known"].get<double>()<=0||row["known"].get<double>()>clock)continue;
        Marker m{row["t"],row["known"],row["price"],row["label"],row["setup"]};if(m.t>=from&&m.t<=clock&&m.known<=clock&&m.price>0)out.markers.push_back(m);
    }
    return out;
}
// Observed book samples only. A seek clears these; a stale/missing minute is a
// gap. USD weights make bids/(bids+asks) match the setup journal's depth reading.
class Book {
public:
    std::map<int64_t,double> closed;
    void reset(){closed.clear();last_t=0;last_share=missing;}
    void observe(int64_t at,double share) {
        if(at<=0||!std::isfinite(share)||share<0||share>1)return;
        if(at<last_t)reset();
        const auto bucket=at/minute*minute;
        if(last_t>0&&bucket>last_t/minute*minute&&bucket-last_t<=5000)closed[last_t/minute*minute]=last_share;
        last_t=at;last_share=share;
        while(closed.size()>1440)closed.erase(closed.begin());
    }
private:int64_t last_t=0;double last_share=missing;
};
}
