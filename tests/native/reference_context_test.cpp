#include "core/reference_context.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
struct Candle { int64_t timestamp_ms; double high, low, close, volume; };
void check(bool ok) { if (!ok) { std::fputs("reference context check failed\n",stderr); std::exit(1); } }
int main() {
    using namespace reference_context;
    const int64_t t = 1788739200000LL; // Monday 2026-09-07 UTC
    check(week_start(t + 6 * day_ms) == t);
    check(day_start(t + day_ms - 1) == t);
    std::vector<Candle> bars{{t,12,8,10,2},{t+60000,24,16,20,1},{t+120000,1000,1,900,100}};
    Series s;
    vwap(bars,t,t+120000,60000,s);
    check(s.complete && s.values.size()==2 && std::abs(s.values.back()-40.0/3)<1e-9);
    check(s.times.front()==static_cast<double>(t+60000));
    vwap(bars,t,t+90000,60000,s); // unfinished future OHLC excluded
    check(s.complete && s.values.back()==10);
    vwap(bars,t,t+30000,60000,s); check(!s.complete && s.values.empty());
    vwap(bars,t+60000,t+120000,60000,s); check(s.complete && s.values.back()==20);
    vwap(bars,t+30000,t+120000,60000,s); check(!s.complete && s.values.empty());
    auto l=levels(bars,t,t+120000,60000); check(l.complete && l.high==24 && l.low==8 && l.close==20);
    bars.erase(bars.begin()+1);
    vwap(bars,t,t+180000,60000,s); check(!s.complete && s.values.size()==1 && s.values.back()==10);
    check(!levels(bars,t,t+180000,60000).complete);
    bars[0].volume=0; vwap(bars,t,t+60000,60000,s); check(!s.complete && s.values.empty());
    bars[0].volume=std::numeric_limits<double>::quiet_NaN();
    check(!levels(bars,t,t+60000,60000).complete);
    bars[0].volume=1; bars[0].high=std::numeric_limits<double>::infinity();
    vwap(bars,t,t+60000,60000,s); check(s.values.empty());
    {
        // Quiet minutes (no trades, so no candle) inside loaded history: tolerated up to max_gap, a longer hole is
        // missing data, and history that starts after the period start never completes it.
        std::vector<Candle> q{{t,12,8,10,2},{t+180000,30,20,25,2},{t+240000,40,30,35,1}};
        Series v;
        vwap(q,t,t+300000,60000,v); check(!v.complete && v.values.size()==1);           // strict: the gap stops it
        vwap(q,t,t+300000,60000,v,120000); check(v.complete && v.values.size()>=3);     // two quiet minutes pass
        vwap(q,t,t+300000,60000,v,60000); check(!v.complete);                           // a 2-minute hole does not
        auto lq=levels(q,t,t+300000,60000,120000); check(lq.complete && lq.high==40 && lq.low==8 && lq.close==35);
        check(!levels(q,t,t+300000,60000).complete);
        check(!levels(q,t,t+360000,60000,0).complete);
        check(levels(q,t,t+360000,60000,120000).complete);                              // the last minute was quiet
        std::vector<Candle> late{{t+60000,12,8,10,2},{t+120000,12,8,10,2}};
        check(!levels(late,t,t+180000,60000,3600000).complete);                         // history starts after t
        vwap(late,t,t+180000,60000,v,3600000); check(!v.complete && v.values.empty());
    }
    std::puts("UTC boundaries, weighted VWAP, anchor, rewind, missing-data and quiet-gap checks passed");
}
