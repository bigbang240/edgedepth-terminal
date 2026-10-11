#include "rendering/observed_liquidations.h"
#include <cstdlib>
#include <cstdio>
#define check(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (false)
#include <limits>
#include <string>
#include <vector>
struct Event { double price, avg_price, qty; bool is_buy; int64_t timestamp_ms; };
int main() {
    // Tiny scale changes and camera translation must not regroup settled reports.
    check(observed_liquidations::stable_marker_step(101,1)==128);
    check(observed_liquidations::stable_marker_step(120,1)==128);
    check(observed_liquidations::stable_marker_step(129,1)==256);
    check(observed_liquidations::stable_marker_step(0,0.01)==0.01);

    const auto small=observed_liquidations::marker_radius(200,25000);
    const auto medium=observed_liquidations::marker_radius(3172,25000);
    check(small>5 && small<6);
    check(medium>11 && medium<12 && medium>1.9f*small);
    float previous=0;
    for(double amount : {1.,50.,200.,1000.,3172.,10000.,25000.,50000.}) {
        const float radius=observed_liquidations::marker_radius(amount,25000);
        check(radius>previous && radius<=20); previous=radius;
    }
    check(observed_liquidations::marker_radius(1e15,25000)==20);
    check(observed_liquidations::marker_radius(0,25000)==0);
    check(observed_liquidations::marker_radius(std::numeric_limits<double>::infinity(),25000)==0);
    check(observed_liquidations::marker_radius(200,0)==small);
    check(observed_liquidations::marker_radius(200,10000)>small);

    // Every diamond uses the same reference at the playhead, so dollars cannot
    // reverse their size ordering as the report count or market activity changes.
    std::vector<Event> btc, orca;
    for(int i=0;i<25;++i){
        btc.push_back({1,0,20000,false,1000+i*1000});
        orca.push_back({1,0,300,false,1000+i*1000});
    }
    observed_liquidations::MarketSizeReference btc_scale, orca_scale;
    auto btc_ref=btc_scale.update(btc,1,21000);
    auto orca_ref=orca_scale.update(orca,1,21000);
    check(btc_ref.usd==20000 && btc_ref.count==20);
    check(orca_ref.usd==1000 && orca_ref.count==20);
    check(observed_liquidations::relative_marker_radius(20000/btc_ref.usd)==
          observed_liquidations::relative_marker_radius(1000/orca_ref.usd));
    const auto ordinary=observed_liquidations::relative_marker_radius(1);
    check(ordinary==8 && observed_liquidations::relative_marker_radius(10)>17);
    // Cold/sparse ORCA no longer borrows a $25k reference for its first 20 reports.
    for(int count : {0,1,19,20,25}){
        orca_ref=orca_scale.update(orca,1,1000+count*1000);
        check(orca_ref.usd==1000 && orca_ref.count==size_t(count));
        const auto radius=[&](double dollars){return observed_liquidations::relative_marker_radius(dollars/orca_ref.usd);};
        check(radius(5000)>2.5f*radius(300));
        check(radius(8000)>radius(2382.37));
        check(radius(8262.55)>radius(5000));
    }
    // BTC learns its own larger baseline as soon as earlier reports exist.
    check(btc_scale.update(btc,1,2000).usd==20000);
    btc.push_back({1,0,200000,false,26000}); btc.push_back({1,0,9000000,false,26000});
    check(btc_scale.update(btc,2,26000).usd==20000); // excludes current timestamp
    check(btc_scale.update(btc,2,21000).usd==20000); // future spike cannot enter a rewind
    check(btc_scale.update(btc,2,26001).count==27); // both simultaneous reports included together
    btc.resize(21); btc_ref=btc_scale.update(btc,3,22000);
    check(btc_ref.count==21 && btc_ref.usd==20000); // source truncated on rewind
    btc[0].qty=40000; btc_ref=btc_scale.update(btc,4,21000);
    check(btc_ref.usd==21000); // correction with identical count
    btc_ref=btc_scale.update(btc,4,3601000);
    check(btc_ref.count==20 && btc_ref.usd==20000); // hour expires even without a data revision
    btc_ref=btc_scale.update(btc,4,7200000);
    check(btc_ref.count==0 && btc_ref.usd==1000);
    auto snapshot=orca;
    for(auto& e:snapshot)e.qty=4000;
    check(orca_scale.update(snapshot,1,21000).usd==4000); // paused source changes with same revision
    for(auto& e:orca)e.qty=1;
    check(orca_scale.update(orca,2,21000).usd==1000); // floor prevents dust amplification
    orca[0].qty=std::numeric_limits<double>::infinity();
    orca[1].qty=-1;
    check(orca_scale.update(orca,3,21000).count==18);
    check(orca_scale.update(std::vector<Event>{},4,21000).count==0);

    std::vector<Event> events{{100,101,2,true,1000},{100,0,3,false,1500},
        {100,100,4,true,2500},{100,100,999,false,3000}};
    auto paused = observed_liquidations::aggregate(events,1000,10000,2500);
    check(paused.count==3 && paused.shorts==602 && paused.longs==300);
    auto rewind = observed_liquidations::aggregate(events,1000,10000,1500);
    check(rewind.count==2 && rewind.shorts==202 && rewind.longs==300);
    auto narrow = observed_liquidations::aggregate(events,1500,2500,2500);
    check(narrow.count==1 && narrow.shorts==0 && narrow.longs==300); // [from,to), matching candle ownership
    auto wide = observed_liquidations::aggregate(events,0,1800000,1800000);
    check(wide.size<=1024 && wide.count==4 && wide.shorts==602 && wide.longs==100200);
    auto invalid=events.front(); invalid.qty=std::numeric_limits<double>::infinity();
    check(observed_liquidations::notional(invalid)==0);
    invalid.qty=-1; check(observed_liquidations::notional(invalid)==0);
    invalid.qty=1; invalid.price=invalid.avg_price=std::numeric_limits<double>::quiet_NaN();
    check(observed_liquidations::notional(invalid)==0);
    check(observed_liquidations::aggregate(events,3000,1000,1000).count==0);
    // Candle charts: callouts only where a candle's liquidations were unusual for this market.
    {
        namespace ol = observed_liquidations;
        const int64_t tf = 60000, T = 29000000 * tf;  // a real epoch, candle aligned (notional() refuses time 0)
        // Four 1m candles: longs 2000 / 3000 / 0 / 20000, shorts 1000 in candle 1 only; one future report.
        std::vector<Event> ev{{100,100,10,false,T},{100,100,10,false,T+30000},     // candle 0: longs 2000
                              {100,100,30,false,T+60000},{100,100,10,true,T+61000}, // candle 1: longs 3000, shorts 1000
                              {100,100,200,false,T+180000},                         // candle 3: longs 20000
                              {100,100,1000,false,T+400000}};                       // after the clock
        // Typical: mean of nonzero side totals up to the clock (2000, 3000, 1000, 20000) = 6500; the future is out.
        check(ol::typical_candle(ev, T+200000, tf) == 6500);
        check(ol::typical_candle(ev, T+59999, tf) == 2000);
        check(ol::typical_candle(std::vector<Event>{}, T+200000, tf) == ol::kTypicalFloor);
        // Small reports filtered out of the typical as they are out of the callouts.
        check(ol::typical_candle(ev, T+200000, tf, 1500) == (3000.0 + 20000.0) / 2);
        // The window is the last kTypicalCandles candles: an old huge candle stops counting.
        std::vector<Event> old{{100,100,1000000,false,T},{100,100,10,false,T+(ol::kTypicalCandles + 5) * tf}};
        check(ol::typical_candle(old, T+(ol::kTypicalCandles + 5) * tf, tf) == 1000);
        check(ol::typical_candle(old, T+ol::kTypicalCandles * tf - 1, tf) == 1e8);
        const auto open_of = [&](int64_t t) { return t / tf * tf; };
        // At 2x typical (13000) only candle 3's longs qualify; at 0.3x (1950) the largest come first.
        auto c = ol::callouts(ev, T, T+200001, 6500, 2.0, 0, open_of);
        check(c.size() == 1 && c[0].open == T+180000 && !c[0].shorts && c[0].usd == 20000 && c[0].count == 1);
        c = ol::callouts(ev, T, T+200001, 6500, 0.3, 0, open_of);
        check(c.size() == 3 && c[0].usd == 20000 && c[1].usd == 3000 && c[2].usd == 2000);
        check(c[2].first == T && c[2].last == T+30000 && c[2].count == 2);
        // Sides stay apart in one candle; [from, to) bounds the view and the clock.
        c = ol::callouts(ev, T+60000, T+120000, 1000, 0.5, 0, open_of);
        check(c.size() == 2 && c[0].usd == 3000 && !c[0].shorts && c[1].shorts && c[1].usd == 1000);
        check(ol::callouts(ev, T, T+400000, 1, 1, 0, open_of).size() == 4);  // the report at the clock's edge is out
        check(ol::callouts(ev, T, T+200001, 1, 1, 2500, open_of).size() == 2); // reports under the filter are ignored
        char buf[16];
        ol::compact_usd(55880000, buf, sizeof(buf)); check(std::string(buf) == "55.9M");
        ol::compact_usd(12600, buf, sizeof(buf)); check(std::string(buf) == "12.6K");
        ol::compact_usd(950, buf, sizeof(buf)); check(std::string(buf) == "950");
        ol::compact_usd(1.234e9, buf, sizeof(buf)); check(std::string(buf) == "1.23B");
    }
}
