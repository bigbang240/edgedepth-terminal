#include "core/stats_ratio_history.h"
#include <cassert>
int main() {
    const int64_t t = 1790812800000LL;
    nlohmann::json j = {{"v",1},{"venue","binancef"},{"symbol","sandusdt"},{"status","ok"},
        {"from",t},{"to",t+600000},{"step",300000},{"generated_at",t+900000},{"server_now",t+86400000},
        {"points", {{t, 0.07, 0.025}, {t+300000, nullptr, nullptr}, {t+600000, 0.08, 0.05}}}};
    auto parse=[&](const nlohmann::json& x){return StatsRatioHistory::parse(x.dump(),"binancef","sandusdt");};
    const auto h=parse(j); assert(h.available() && h.times.size()==3);
    assert(1/h.oi_share[0]==40 && h.oi_share[0]*100==2.5);
    assert(std::isnan(h.prices[1]) && std::isnan(h.oi_share[1])); // gaps are never zero or forward-filled
    assert(!StatsRatioHistory::parse(j.dump(),"bybit","sandusdt").available());
    assert(!StatsRatioHistory::parse(j.dump(),"binancef","btcusdt").available());
    auto live=j; live["source"]="binance"; live["points"][0][2]=12;
    assert(parse(live).binance && parse(live).oi_share[0]==12);
    auto x=j; x["points"][1][0]=t; assert(!parse(x).available());
    x=j; x["points"].erase(1); assert(!parse(x).available());
    x=j; x["generated_at"]=t; assert(!parse(x).available());
    x=j; x["server_now"]=t; assert(!parse(x).available());
    x=j; x["from"]=t-8*86400000LL; assert(!parse(x).available());
    for(const auto& bad:{nlohmann::json(0),nlohmann::json(-1),nlohmann::json("0.025"),nlohmann::json(11),nlohmann::json(1e-320)}) {
        x=j; x["points"][0][2]=bad; assert(!parse(x).available());
    }
    x=j; x["points"][2][1]=nullptr; assert(std::isnan(parse(x).prices.back())); // no last-value substitution
    assert(!StatsRatioHistory::parse(std::string(262145,'x'),"binancef","sandusdt").available());
}
