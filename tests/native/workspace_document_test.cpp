#include "core/workspace_document.h"
#include <cstdio>
#include <cstdlib>
using workspace::Json;
static void check(bool ok) { if (!ok) { std::fputs("workspace check failed\n",stderr); std::exit(1); } }
int main() {
    for (const std::string source : {"chart_binancef_btcusdt", "###chart_binancef_btcusdt",
                                    "Chart BTC 1m###chart_binancef_btcusdt"}) {
        std::string ini = "[Window][" + source + "]\nDockId=0x00000003,0\n\n[Window][Watchlist]\nDockId=0x00000001\n";
        workspace::remap_window_title(ini, "Chart BTC 1m###chart_binancef_btcusdt",
                                     "Chart 牛来 5m###chart_binancef_牛来usdt");
        if (ini != "[Window][chart_binancef_牛来usdt]\nDockId=0x00000003,0\n\n[Window][Watchlist]\nDockId=0x00000001\n") {
            std::fprintf(stderr, "FAIL symbol-switch docking remap\n");
            return 1;
        }
    }

    Json doc = {{"version",1},{"layout","[Window][Chart]\nPos=0,0\n"},
                {"widgets",Json::array({{{"type","chart"},{"title","Chart"},{"settings",Json::object()}}})}};
    check(workspace::parse_document(doc.dump()) == doc);
    auto multi = doc;
    multi["widgets"][0]["settings"]["rt_mode"] = true;
    multi["widgets"].push_back({{"type","dom"},{"title","DOM"},{"settings",{{"mode_companion",true},{"group_mult",10}}}});
    multi["widgets"].push_back({{"type","trades"},{"title","Tape"},{"settings",{{"mode_companion",false}}}});
    multi["widgets"].push_back({{"type","chart"},{"title","Secondary"},{"instance",2},
        {"pair",{{"exchange","binancef"},{"symbol","btcusdt"}}},{"settings",{{"rt_mode",false}}}});
    multi["widgets"].push_back({{"type","dom"},{"title","Other market"},
        {"pair",{{"exchange","bybit"},{"symbol","ethusdt"}}},{"settings",{{"mode_companion",false}}}});
    multi["widgets"][1]["route_pair"] = {{"exchange","binancef"},{"symbol","btcusdt"}};
    check(workspace::parse_document(multi.dump()) == multi);
    auto invalid_route = multi; invalid_route["widgets"][1]["route_pair"]["symbol"] = 42;
    check(!workspace::valid_document(invalid_route));
    auto duplicate = multi; duplicate["widgets"].push_back(multi["widgets"].back());
    check(!workspace::valid_document(duplicate));
    duplicate["widgets"].back()["pair"]["extra"] = "cannot bypass identity";
    check(!workspace::valid_document(duplicate));
    for (const Json& invalid : {Json(0), Json(1001), Json(2.5), Json("2")}) {
        auto broken = multi; broken["widgets"][3]["instance"] = invalid;
        check(!workspace::valid_document(broken));
    }
    auto broken = multi; broken["widgets"].back()["pair"]["symbol"] = "bad\n[Window]";
    check(!workspace::valid_document(broken));
    broken = multi; broken["widgets"].back()["pair"]["exchange"] = 3;
    check(!workspace::valid_document(broken));
    const Json bybit = {{"exchange","bybit"},{"symbol","rlcusdt"}};
    const Json binance = {{"exchange","binancef"},{"symbol","rlcusdt"}};
    const Json eth = {{"exchange","binancef"},{"symbol","ethusdt"}};
    const Json hl = {{"exchange","hl"},{"symbol","BTC"}};
    auto legacy = doc;
    legacy["widgets"][0]["title"] = "Chart RLC###chart_bybit_rlcusdt";
    legacy["widgets"].push_back({{"type","data"},{"title","Data RLC###data_bybit_rlcusdt"},
        {"pair",bybit},{"settings",{{"hours",24}}}});
    legacy["widgets"].push_back({{"type","chart"},{"title","RLC second"},{"instance",2},
        {"pair",bybit},{"settings",{{"rt_mode",true}}}});
    legacy["widgets"].push_back({{"type","chart"},{"title","BTC comparison"},
        {"pair",hl},{"settings",Json::object()}});
    check(workspace::document_market(legacy) == bybit);
    auto restored = workspace::for_market(legacy,binance);
    check(workspace::valid_document(restored));
    check(restored["market"] == binance);
    check(restored["widgets"][1]["pair"] == binance);
    check(restored["widgets"][1]["settings"]["hours"] == 24);
    check(restored["widgets"][2]["pair"] == binance);
    check(restored["widgets"][3]["pair"] == hl);
    restored = workspace::for_market(restored,eth);
    check(restored["widgets"][1]["pair"] == eth);
    check(restored["widgets"][2]["pair"] == eth);
    check(workspace::for_market(restored,eth) == restored);
    check(workspace::for_market(restored,bybit)["widgets"][1]["pair"] == bybit);
    auto closed = legacy;
    closed["widgets"].erase(closed["widgets"].begin());
    auto reopened = workspace::for_market(closed,binance);
    check(workspace::valid_document(reopened));
    check(reopened["widgets"][0]["type"] == "chart");
    check(!reopened["widgets"][0].contains("pair"));
    check(reopened["widgets"][0]["instance"] == 1);
    check(reopened["widgets"][1]["pair"] == binance);
    auto collision = legacy;
    auto destination = legacy["widgets"][1]; destination["pair"] = binance;
    collision["widgets"].push_back(destination);
    auto merged = workspace::for_market(collision,binance);
    check(workspace::valid_document(merged));
    check(merged["widgets"].size() == legacy["widgets"].size());
    // A target comparison ordered before the primary cannot steal instance 1.
    auto reordered = legacy;
    auto comparison = reordered["widgets"][3]; comparison["pair"] = binance;
    reordered["widgets"].erase(reordered["widgets"].begin()+3);
    reordered["widgets"].insert(reordered["widgets"].begin(),comparison);
    auto ordered = workspace::for_market(reordered,binance);
    check(!ordered["widgets"][0].contains("pair"));
    check(ordered["widgets"][0]["instance"] == 1);
    check(ordered["widgets"][1]["instance"] == 2);
    check(workspace::valid_document(ordered));
    collision["widgets"].back()["title"] = "Destination###data_binancef_rlcusdt";
    collision["layout"] = "[Window][data_bybit_rlcusdt]\nDockId=0x01\n\n"
        "[Window][data_binancef_rlcusdt]\nDockId=0x02\n\n[Docking][Data]\n";
    merged = workspace::for_market(collision,binance);
    check(merged["layout"].get<std::string>().find("data_binancef_rlcusdt") == std::string::npos);
    check(merged["layout"].get<std::string>().find("[Docking][Data]") != std::string::npos);
    auto stale_data = doc;
    stale_data["widgets"][0]["title"] = "Chart ETH###chart_binancef_ethusdt";
    stale_data["widgets"].push_back(legacy["widgets"][1]);
    check(workspace::for_market(stale_data,eth)["widgets"][1]["pair"] == eth);
    closed["market"] = eth; // Prior route already differs from the remaining chart.
    check(workspace::for_market(closed,binance)["widgets"][1]["pair"] == binance);
    auto malformed = legacy; malformed["market"] = {{"exchange",12},{"symbol","rlcusdt"}};
    check(!workspace::valid_document(malformed));

    auto bad=doc; bad["version"]=2; check(workspace::parse_document(bad.dump()).is_null());
    bad=doc; bad["widgets"].push_back(bad["widgets"][0]); check(!workspace::valid_document(bad));
    bad=doc; bad["widgets"][0]["type"]="execute"; check(!workspace::valid_document(bad));
    check(workspace::parse_document(std::string(300000,'x')).is_null());
    check(workspace::parse_document(std::string(10000,'[')).is_null());
    check(workspace::parse_document("{broken").is_null());
    int n=3; bool b=true; double x=1;
    workspace::read(Json{{"n",1e100}},"n",n,0,10); check(n==3);
    workspace::read(Json{{"n",2.5}},"n",n,0,10); check(n==3);
    workspace::read(Json{{"n","4"}},"n",n,0,10); check(n==3);
    workspace::read(Json{{"b",0}},"b",b); check(b);
    workspace::read(Json{{"x",2.0}},"x",x,0,10); check(x==2);
    std::puts("workspace document validation passed");
}
