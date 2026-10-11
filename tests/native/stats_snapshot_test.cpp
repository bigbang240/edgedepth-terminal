#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/stats_snapshot.h"
#include <cassert>
#include <cstdio>
int main() {
    {
        MinuteVolumeSnapshot flow;
        const int64_t clock = 1800000010000LL;
        Terminal::Volume sample{};
        sample.timeframe = 60; sample.timestamp_ms = (clock / 60000 * 60000) / 1000;
        sample.buy = 10; sample.sell = 5; sample.vwap_buy = 2; sample.vwap_sell = 2; sample.cvd = 10;
        assert(!flow.active(clock));
        assert(flow.apply(sample, clock) && flow.active(clock));
        sample.sell = 20; sample.cvd = -20;
        assert(flow.apply(sample, clock+1) && flow.volume.cvd == -20);
        sample.sell = 5; assert(!flow.apply(sample, clock+2));
        sample.sell = 20; sample.final = true; assert(flow.apply(sample, clock+3));
        sample.final = false; assert(!flow.apply(sample, clock+4));
        assert(!flow.active(clock+60000));
        sample.timestamp_ms += 60; sample.buy = 0; sample.sell = 0; sample.cvd = 0;
        assert(flow.apply(sample, clock+60000) && flow.active(clock+60000));
    }

    StatsSnapshot historical_guard;
    Terminal::Stat historical{};
    historical.historical = true; historical.timeframe = 60;
    historical.timestamp_ms = 1790899200; historical.oi_close = 100;
    assert(!historical_guard.apply(historical, 1790899201000LL));

    StatsSnapshot snapshot;
    constexpr int64_t minute = 1790899200000LL;
    assert(StatsSnapshot::timeframe_ms == 60000);
    assert(!snapshot.active(minute));
    Terminal::Stat stat{};
    stat.timeframe = 60;
    stat.timestamp_ms = minute / 1000;
    stat.mark_price = 3.1;
    stat.funding = 0.00005;
    stat.trade_buy = 12;
    stat.liq_short_usd = 55700;
    assert(snapshot.apply(stat, minute + 1000));
    assert(snapshot.active(minute + 1000));
    assert(snapshot.stat.liq_short_usd == 55700);
    assert(snapshot.apply(stat, minute + 2000)); // a repeated partial replaces, never adds
    assert(snapshot.stat.liq_short_usd == 55700);
    stat.trade_buy = 11;
    assert(!snapshot.apply(stat, minute + 3000));
    stat.trade_buy = 15;
    assert(snapshot.apply(stat, minute + 4000));
    assert(!snapshot.active(minute + 20000)); // old data is not a live zero
    assert(!snapshot.active(minute + 60000)); // minute rollover
    stat.timestamp_ms += 60;
    stat.trade_buy = 0;
    stat.liq_short_usd = 0;
    assert(snapshot.apply(stat, minute + 61000));
    assert(snapshot.active(minute + 61000));
    assert(snapshot.stat.trade_buy == 0 && snapshot.stat.liq_short_usd == 0);
    stat.timestamp_ms -= 60;
    stat.final = true;
    assert(!snapshot.apply(stat, minute + 62000)); // old final cannot overwrite new minute
    stat.timestamp_ms = minute + 60000; // milliseconds variant
    stat.timeframe = 60000;
    assert(snapshot.apply(stat, minute + 63000));
    stat.final = false;
    assert(!snapshot.apply(stat, minute + 64000));
    stat.timestamp_ms += 60000;
    stat.mark_price = 0;
    stat.funding_ms = minute + 121000;
    assert(snapshot.apply(stat, minute + 121000));
    assert(snapshot.stat.mark_price == 0); // authoritative missing field never carried
    stat.timeframe = 3600;
    assert(!snapshot.apply(stat, minute + 122000)); // wrong window never mixes
    StatsSnapshot hour(StatsSnapshot::hour_ms);
    stat.timestamp_ms = minute / 1000;
    stat.timeframe = 3600;
    stat.liq_short_usd = 9000;
    assert(hour.apply(stat, minute + 1000));
    assert(hour.active(minute + 59000));
    assert(!hour.active(minute + 62000)); // slow hourly stream still ages out
    stat.timestamp_ms = minute / 1000 + 3600;
    stat.liq_short_usd = 0;
    assert(hour.apply(stat, minute + 3601000));
    assert(hour.stat.liq_short_usd == 0); // UTC-hour reset, never a rolling claim
    stat.timeframe = 60;
    assert(!hour.apply(stat, minute + 3601000));
    std::puts("PASS: stats minute window, duplicate/late updates, stale/missing and rollover");
}
