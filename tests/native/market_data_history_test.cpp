#include "core/market_data_history.h"
#include <cassert>
int main() {
    using J=nlohmann::json;
    const int64_t to=1791245100000LL, from=to-86400000;
    J j={{"v",1},{"venue","binancef"},{"symbol","rlcusdt"},{"source","binance"},{"status","ok"},
        {"from",from},{"to",to},{"step",300000},{"generated_at",to+1000},{"server_now",to+2000},{"points",J::array()}};
    for (int64_t t=from;t<=to;t+=300000) j["points"].push_back(J::array({t,2,100,.44}));
    j["points"][1][1]=nullptr; j["points"][2][3]=0;
    const auto parse=[&](const J& value){return MarketDataHistory::parse(value.dump(),"binancef","rlcusdt");};
    auto h=parse(j); assert(h.available()); assert(h.times.size()==289);
    assert(std::isnan(h.values[0][1]) && std::isnan(h.notional[1]));
    assert(h.values[2][2]==0 && h.notional[0]==200);
    assert(h.nearest(h.times[2]+100)==2 && h.nearest(h.times[2]+151)==3);
    assert(h.nearest(-1,10)==10);
    for (const auto* key : {"venue","symbol","source"}) { auto bad=j; bad[key]="other"; assert(!parse(bad).available()); }
    auto bad=j; bad["points"][1][0]=from; assert(!parse(bad).available());
    bad=j; bad["points"][2][3]=1.1; assert(!parse(bad).available());
    bad=j; bad["points"][2][2]="100"; assert(!parse(bad).available());
    bad=j; bad["to"]=to+300000; assert(!parse(bad).available());
    bad=j; bad["generated_at"]=to-1; assert(!parse(bad).available());
    assert(!MarketDataHistory::parse(j.dump(),"bybit","rlcusdt").available());
    assert(h.values[3].size()==289 && std::isnan(h.values[3][0])); // older API remains usable
    j["series"]={{"oi_market_cap",std::vector<double>(289,1.25)}, {"top_long_accounts",std::vector<double>(289,.6)},
        {"taker_buy_volume",std::vector<double>(289,60)}, {"taker_sell_volume",std::vector<double>(289,40)}};
    j["series"]["oi_market_cap"][1]=nullptr;
    j["series"]["taker_buy_volume"][2]=0;
    h=parse(j); assert(h.available()); assert(h.values[3][0]==125 && std::isnan(h.values[3][1]));
    assert(h.values[4][0]==60 && h.values[5][0]==60 && h.values[6][0]==40 && h.values[5][2]==0);
    bad=j; bad["series"]["top_long_accounts"][0]=1.1; assert(!parse(bad).available());
    bad=j; bad["series"]["oi_market_cap"].erase(0); assert(!parse(bad).available());
    bad=j; bad["series"]["taker_buy_volume"][0]=-1; assert(!parse(bad).available());
    bad=j; bad["series"]["taker_sell_volume"][0]="40"; assert(!parse(bad).available());
    J doc={{"version",1},{"layout",""},{"widgets",J::array({
        J{{"type","chart"},{"title","chart"},{"settings",J::object()}},
        J{{"type","data"},{"title","data"},{"settings",{{"hours",24}}},{"pair",{{"exchange","binancef"},{"symbol","rlcusdt"}}}}
    })}};
    assert(workspace::valid_document(doc));
    doc["widgets"].push_back(doc["widgets"][1]); assert(!workspace::valid_document(doc));
}
