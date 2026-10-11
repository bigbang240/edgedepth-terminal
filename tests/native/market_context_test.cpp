#include "core/market_context.h"
#include <cassert>
int main() {
    constexpr int64_t at = 1790992800000LL;
    nlohmann::json j = {{"v",1},{"venue","binancef"},{"symbol","aaveusdt"},{"server_now",at+1000},{"status","ok"},
        {"items", nlohmann::json::array({{{"id","cascade"},{"value","Top 5% tier"},{"detail","Research preview"},{"help","Relative tier, not probability"},{"tone","warn"},{"at",at},{"until",at+3600000}}})}};
    auto parse = [&](const nlohmann::json& x) { return MarketContext::parse(x.dump(), "binancef", "aaveusdt"); };
    auto valid = parse(j);
    assert(valid.status == "ok");
    assert(valid.items[MarketContext::Cascade].current(at+3599999));
    assert(!valid.items[MarketContext::Cascade].current(at+3600000));
    assert(!valid.items[MarketContext::Cascade].current(at-1));
    assert(MarketContext::parse(j.dump(), "bybit", "aaveusdt").status == "unavailable");
    assert(MarketContext::parse(j.dump(), "binancef", "btcusdt").status == "unavailable");
    auto x = j; x["items"].push_back(x["items"][0]); assert(parse(x).status == "unavailable");
    for (const auto& bad : {nlohmann::json(nullptr), nlohmann::json("future"), nlohmann::json(1e100), nlohmann::json(at+1001)}) {
        x=j; x["items"][0]["at"]=bad; assert(parse(x).status == "unavailable");
    }
    x=j; x["items"][0]["until"]=at+3600001; assert(parse(x).status == "unavailable");
    x=j; x["items"][0]["until"]=at+1000; assert(parse(x).status == "unavailable");
    x=j; x["items"][0]["id"]="secret"; assert(parse(x).status == "unavailable");
    x=j; x["items"][0]["value"]=std::string(161,'x'); assert(parse(x).status == "unavailable");
    x=j; x["items"][0]["help"]="bad\ncopy"; assert(parse(x).status == "unavailable");
    x=j; x["status"]="unsupported"; assert(parse(x).status == "unavailable");
    x["items"]=nlohmann::json::array(); assert(parse(x).status == "unsupported");
    x=j; x["items"][0]["id"]="readings"; x["items"][0]["until"]=at+600000;
    x["items"][0]["metrics"]={0.175,-0.01,0.04};
    auto readings=parse(x).items[MarketContext::Readings];
    assert(readings.metrics[0] > 0 && readings.metrics[1] < 0 && readings.metrics[2] > 0);
    x["items"][0]["metrics"][1]=nullptr; assert(std::isnan(parse(x).items[MarketContext::Readings].metrics[1]));
    x["items"][0]["metrics"][2]=-0.01; assert(parse(x).status=="unavailable");
    x["items"][0]["metrics"]={"bad",0,0}; assert(parse(x).status=="unavailable");
    x["items"][0]["metrics"]={0.175,-0.01,0.04};
    x["items"][0]["oi_mcap_ratio"]=0.025; assert(parse(x).items[MarketContext::Readings].oi_mcap_ratio==0.025);
    x["items"][0]["oi_mcap_ratio"]=0; assert(parse(x).items[MarketContext::Readings].oi_mcap_ratio==0);
    x["items"][0]["oi_mcap_ratio"]=-1; assert(parse(x).status=="unavailable");
    x["items"][0].erase("oi_mcap_ratio");
    x["items"][0].erase("metrics"); assert(parse(x).status=="ok"); // old server remains compatible
    assert(MarketContext::parse("broken", "binancef", "aaveusdt").status == "unavailable");
}
