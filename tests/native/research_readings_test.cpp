#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/research_readings.h"
#include <cassert>
int main() {
    using J = nlohmann::json;
    const int64_t t = 1791417600000LL;
    J f={{"id","feature.coil_state_1h"},{"name","Coiling line state (1h)"},{"group","Coiling line"},
        {"unit","label"},{"description","State on completed hourly candles."},{"source","original_live"},
        {"dtype","enum"},{"value",nullptr},{"display",nullptr},{"availability","history_only"}};
    J j={{"v",1},{"venue","binancef"},{"symbol","btcusdt"},{"feature_version","feature_defs.v1"},
        {"feature_edition","original_live.v1"},{"dataset_revision",std::string(64,'a')},{"observed_at",t},
        {"calculated_at",t+65000},{"server_now",t+70000},{"valid_until",t+180000},{"features",J::array({f})}};
    auto parse = [](const J& body) { return ResearchReadings::parse(body.dump(),"binancef","btcusdt"); };
    auto result=parse(j); assert(result.available() && !result.items[0].present());
    assert(!result.stale(t+70000) && result.stale(t+180000));
    j["features"][0]["value"]="none"; j["features"][0]["display"]="none"; j["features"][0]["availability"]="present";
    assert(parse(j).items[0].label=="none");
    j["features"][0]["dtype"]="number"; j["features"][0]["value"]=0; j["features"][0]["display"]="0";
    assert(parse(j).items[0].value==0);
    assert(parse(j).groups.size()==1 && parse(j).groups[0].items[0]==0);
    j["features"][0]["tone"]="down"; assert(parse(j).items[0].tone==-1);
    auto grouped=j; auto extra=j["features"][0]; extra["id"]="feature.ret_15m"; extra["group"]="Price and volatility"; extra["tone"]="up";
    grouped["features"].push_back(extra);
    extra["id"]="feature.coil_age_1h"; extra["group"]="Coiling line"; extra["tone"]=nullptr; grouped["features"].push_back(extra);
    auto groups=parse(grouped); assert(groups.groups.size()==2 && groups.groups[0].items==std::vector<size_t>({0,2}));
    assert(groups.items[1].tone==1 && groups.items[2].tone==0);
    auto invalidTone=j; invalidTone["features"][0]["tone"]="bullish"; assert(!parse(invalidTone).available());
    invalidTone=j; invalidTone["features"][0]=f; invalidTone["features"][0]["tone"]="up"; assert(!parse(invalidTone).available());
    for (const auto* key : {"symbol","venue","feature_edition","feature_version","v"}) { auto bad=j; bad[key]="wrong"; assert(!parse(bad).available()); }
    auto bad=j; bad["features"].push_back(bad["features"][0]); assert(!parse(bad).available());
    bad=j; bad["features"][0]["source"]="nightly_archive"; assert(!parse(bad).available());
    bad=j; bad["features"][0]["value"]="0"; assert(!parse(bad).available());
    bad=j; bad["observed_at"]=t+1; assert(!parse(bad).available());
    bad=j; bad["calculated_at"]=t-1; assert(!parse(bad).available());
    assert(!ResearchReadings::parse(j.dump(),"bybit","btcusdt").available());
}
