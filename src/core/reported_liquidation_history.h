#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>
#include <nlohmann/json.hpp>

// Main-thread, per-market recorded-report cache. Report time remains the
// publication clock shared with Stats and existing archives. Price/quantity
// identity is normalized to the DB's eight-decimal storage precision; this is
// report deduplication, not an exchange order ID or a count of liquidated users.
template<class Event> class ReportedLiquidationHistory {
public:
    static constexpr int64_t tile_ms = 15 * 60 * 1000;
    // A candle chart reads its whole visible window (and the callouts' typical-candle window) within the plan's
    // lookback, so coverage is kept for 92 days of tiles. Markets run about 500 to 1,100 reports a day (Bybit and
    // Binance BTC, ETH, SOL, STRK, Oct 2026), so 200,000 reports hold months of an ordinary market.
    static constexpr size_t max_events = 200000, max_tiles = 92 * 96;
    static constexpr int day_tiles = 96, legacy_batch_tiles = 24;
    std::vector<Event> events;
    struct Coverage { int64_t end = 0, read_at = 0; };
    std::map<int64_t,Coverage> loaded;
    bool limited = false;
    std::string error;
    int64_t subscribed_at = 0;
    uint64_t revision = 0;
    bool connected = false;
    uint64_t connection_generation = 0;
    ReportedLiquidationHistory() = default;
    ReportedLiquidationHistory(const ReportedLiquidationHistory&) = delete;
    ReportedLiquidationHistory& operator=(const ReportedLiquidationHistory&) = delete;
    ~ReportedLiquidationHistory() { cancel(); }
    bool loading() const { return !pending_id.empty(); }
    void cancel() { pending.erase(pending_id); pending_id.clear(); }
    void reset_requests() { cancel(); loaded.clear(); error.clear(); retry_at=0; limited=false; settle(); }
    // Explicit reload may reclaim an exhausted cache so an older, narrower
    // viewport can be loaded. Reconnects retain received events instead.
    void reload() {
        if(limited) { events.clear(); ++revision; }
        reset_requests();
    }
    void connection(bool open,int64_t now,uint64_t generation=0) {
        if (generation != connection_generation) {
            connection_generation=generation; connected=false; cancel();
        }
        if(open && !connected) { subscribed_at=now; reset_requests(); }
        if(!open && connected) cancel();
        if(open != connected) settle();
        connected=open;
    }
    static auto key(const Event& e) {
        auto stored=[](double v){return std::round(v*1e8)/1e8;};
        return std::tuple(e.timestamp_ms,e.is_buy,stored(e.price),stored(e.avg_price),stored(e.qty));
    }
    static bool valid(const Event& e) {
        const double p=e.avg_price>0?e.avg_price:e.price;
        return e.timestamp_ms>0 && std::isfinite(e.price) && e.price>=0 &&
            std::isfinite(e.avg_price) && e.avg_price>=0 && p>0 && e.qty>0 &&
            std::isfinite(p*e.qty) && std::isfinite(e.qty) &&
            std::isfinite(e.filled_qty) && e.filled_qty>=0 && e.trade_timestamp_ms>=0;
    }
    void add(const Event& e) {
        if(!valid(e))return;
        auto at=std::lower_bound(events.begin(),events.end(),key(e),[](const Event& a,const auto& k){return key(a)<k;});
        if(at!=events.end() && key(*at)==key(e)) {
            if(e.trade_timestamp_ms>0)at->trade_timestamp_ms=e.trade_timestamp_ms;
            if(e.filled_qty>0)at->filled_qty=e.filled_qty;
            return;
        }
        events.insert(at,e); ++revision;
        enforce_limit();
    }
    void trim_after(int64_t cutoff) {
        cancel();loaded.clear();
        events.erase(std::upper_bound(events.begin(),events.end(),cutoff,[](int64_t t,const Event& e){return t<e.timestamp_ms;}),events.end());
        ++revision;
    }
    bool covers(int64_t from,int64_t to) const {
        if(to<=from)return false;
        int64_t cursor=from;
        for(const auto& [start,c]:loaded){
            if(c.end<=cursor)continue;
            if(start>cursor)return false;
            cursor=c.end;if(cursor>=to)return true;
        }
        return false;
    }
    nlohmann::json request(const std::string& exchange,const std::string& symbol,
                          int64_t from,int64_t to,int64_t now,int64_t lookback,double steady) {
        if(loading() && steady-sent_at>10000){cancel();error="History unavailable on this connection";retry_at=steady+30000;}
        if(!connected || loading() || steady<retry_at || limited)return {};
        // The whole window within the lookback, newest first, a day per request (six hours on older servers).
        // A window older than the lookback stays uncovered, and covers() says so; nothing is clipped silently.
        from=std::max({int64_t(1),from,now-lookback+1000});
        to=std::min(to,now-2000);
        if(to<=from)return {};
        const int64_t last=(to-1)/tile_ms*tile_ms, first=from/tile_ms*tile_ms;
        // A settled window is rescanned at most twice a second: a wide view holds thousands of tiles. Two are
        // remembered, as a chart asks for its view and then the window behind its callouts' typical candle.
        for(const auto& w:idle)if(steady<w.until && w.first==first && w.last==last)return {};
        // Load settled/missing sections before refreshing the moving tail.
        for(int pass=0;pass<2;++pass)for(int64_t start=last;start>=first;start-=tile_ms){
            int64_t a=std::max(start,now-lookback+1000);
            const int64_t b=std::min(start+tile_ms,to);
            auto it=loaded.find(a);
            const bool missing=it==loaded.end();
            if((pass==0)!=missing)continue;
            // Re-read recent tiles to recover delayed DB writes and reconnect
            // overlap. A successful DB read does not assert source completeness.
            if(!missing && (it->second.end>=b && b<now-120000))continue;
            if(!missing && now-it->second.read_at<10000)continue;
            if(b<=a)continue;
            // Coalesce only adjacent missing tiles; retain small tail refreshes.
            if(missing)for(int n=1;n<batch_tiles && start-int64_t(n)*tile_ms>=first;++n){
                const int64_t previous=std::max(start-int64_t(n)*tile_ms,now-lookback+1000);
                if(loaded.find(previous)!=loaded.end())break;
                a=previous;
            }
            pending_from=a;pending_to=b;pair_exchange=exchange;pair_symbol=symbol;
            pending_id="reported-liq-"+std::to_string(++serial);pending[pending_id]=this;
            sent_at=steady;error.clear();
            return {{"method","get_reported_liquidations"},{"data",{{"request_id",pending_id},{"pair",{{"exchange",exchange},{"symbol",symbol}}},{"from_ms",a},{"to_ms",b}}}};
        }
        idle[idle_next]={first,last,steady+500};idle_next^=1;
        return {};
    }
    static void receive(const nlohmann::json& j) {
        if(!j.is_object() || !j.contains("request_id") || !j["request_id"].is_string())return;
        auto it=pending.find(j["request_id"].template get<std::string>());
        if(it!=pending.end())it->second->accept(j);
    }
private:
    inline static uint64_t serial=0;
    inline static std::map<std::string,ReportedLiquidationHistory*> pending;
    std::string pending_id,pair_exchange,pair_symbol;
    int64_t pending_from=0,pending_to=0;
    double sent_at=0,retry_at=0;
    struct Idle { int64_t first=0,last=0; double until=0; } idle[2];
    int idle_next=0;
    void settle(){idle[0]=idle[1]=Idle{};}
    int batch_tiles=day_tiles; // A day; shrink on dense ranges or older servers.
    void enforce_limit(){
        if(events.size()<=max_events)return;
        events.erase(events.begin(),events.begin()+static_cast<ptrdiff_t>(events.size()-max_events));
        const auto floor=events.front().timestamp_ms;
        for(auto it=loaded.begin();it!=loaded.end();) {if(it->first<floor)it=loaded.erase(it);else ++it;}
        limited=true;
        error="Report cache limit reached; reload a narrower window";
    }
    void accept(const nlohmann::json& j) {
        cancel();retry_at=sent_at+300;settle();
        auto fail=[&](const char* message){error=message;retry_at=sent_at+30000;};
        if(j.contains("error")) {
            if(pending_to-pending_from>tile_ms &&
               (j["error"]=="report_limit" || j["error"]=="outside_history_window")) {
                // Servers before 2026-10-11 accept six hours, the oldest only 15 minutes. Dense batches split
                // without recording coverage or discarding received reports.
                const int asked=int((pending_to-pending_from+tile_ms-1)/tile_ms);
                batch_tiles=j["error"]=="outside_history_window"?(asked>legacy_batch_tiles?legacy_batch_tiles:1):
                    std::max(1,asked/2);
                error.clear();return;
            }
            if(j["error"]=="busy"){error="History busy; retrying";retry_at=sent_at+1500;}
            else if(j["error"]=="pro_required")fail("Recorded history requires Pro");
            else if(j["error"]=="outside_history_window")fail("Outside available history window");
            else if(j["error"]=="report_limit")fail("History report limit reached");
            else if(j["error"]=="unsupported_market")fail("History unavailable for this market");
            else fail("History unavailable; received reports remain visible");
            return;
        }
        if(!j.contains("exchange") || j["exchange"]!=pair_exchange || !j.contains("symbol") || j["symbol"]!=pair_symbol ||
           !j.contains("from_ms") || j["from_ms"]!=pending_from || !j.contains("to_ms") || j["to_ms"]!=pending_to ||
           !j.contains("coverage") || j["coverage"]!="recorded_reports_only" || !j.contains("retrieved_at_ms") || !j["retrieved_at_ms"].is_number_integer() ||
           !j.contains("reports") || !j["reports"].is_array() || j["reports"].size()>32768){fail("Invalid report history response");return;}
        std::vector<Event> incoming;incoming.reserve(j["reports"].size());
        for(const auto& v:j["reports"]){
            if(!v.is_object()){fail("Invalid report history response");return;}
            bool fields=true;
            for(const char* name:{"price","avg_price","qty","filled_qty"}) fields=fields && v.contains(name) && v[name].is_number();
            if(!fields || !v.contains("timestamp_ms") || !v["timestamp_ms"].is_number_integer() || !v.contains("trade_timestamp_ms") ||
               !v["trade_timestamp_ms"].is_number_integer() || !v.contains("is_buy") || !v["is_buy"].is_boolean()){fail("Invalid report history response");return;}
            Event e{};e.timestamp_ms=v["timestamp_ms"].template get<int64_t>();e.price=v["price"].template get<double>();e.avg_price=v["avg_price"].template get<double>();
            e.qty=v["qty"].template get<double>();e.is_buy=v["is_buy"].template get<bool>();e.filled_qty=v["filled_qty"].template get<double>();e.trade_timestamp_ms=v["trade_timestamp_ms"].template get<int64_t>();
            if(!valid(e) || e.timestamp_ms<pending_from || e.timestamp_ms>=pending_to){fail("Invalid report history response");return;}
            incoming.push_back(e);
        }
        // Validate the whole response before mutation. Live observations win
        // numeric precision; history may enrich their optional source fields.
        std::sort(incoming.begin(),incoming.end(),[](const auto& a,const auto& b){return key(a)<key(b);});
        std::vector<Event> merged;merged.reserve(events.size()+incoming.size());
        size_t i=0,k=0;
        auto append=[&](const Event& e){
            if(!merged.empty() && key(merged.back())==key(e)) {
                if(e.trade_timestamp_ms>0)merged.back().trade_timestamp_ms=e.trade_timestamp_ms;
                if(e.filled_qty>0)merged.back().filled_qty=e.filled_qty;
            } else merged.push_back(e);
        };
        while(i<events.size() || k<incoming.size()) {
            if(k==incoming.size() || (i<events.size() && key(events[i])<=key(incoming[k])))append(events[i++]);
            else append(incoming[k++]);
        }
        events.swap(merged);++revision;
        // Keep coverage tile-granular so zoom/pan and moving-tail refreshes
        // reuse the existing cache rather than re-reading a six-hour batch.
        for(int64_t start=pending_from;start<pending_to;){
            const int64_t end=std::min((start/tile_ms+1)*tile_ms,pending_to);
            loaded[start]={end,j["retrieved_at_ms"].template get<int64_t>()};
            start=end;
        }
        while(loaded.size()>max_tiles)loaded.erase(loaded.begin());
        enforce_limit();
    }
};
