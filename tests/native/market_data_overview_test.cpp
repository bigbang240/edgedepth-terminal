#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/market_data_overview.h"
#include <cassert>
int main() {
    constexpr int64_t now = 1790899201000LL;
    TickerEntry ticker{.last_price=.017, .change_pct_24h=21, .volume_quote=12345, .event_time_ms=now};
    assert(!market_data_overview::ticker_fresh(nullptr, now));
    assert(market_data_overview::ticker_fresh(&ticker, now));
    assert(!market_data_overview::ticker_fresh(&ticker, now+60001));
    assert(!market_data_overview::ticker_fresh(&ticker, now-1));
    StatsSnapshot snapshot;
    using State = StatFreshness::State;
    assert(market_data_overview::field(snapshot, now, now).state == State::Unavailable);
    Terminal::Stat stat{}; stat.timestamp_ms=now/60000*60; stat.timeframe=60;
    stat.open_interest_ms=now; stat.funding_ms=now; stat.funding=0; stat.open_interest_usd=0;
    assert(snapshot.apply(stat, now));
    // A timestamped zero is real; a recent message cannot refresh an old field.
    assert(market_data_overview::field(snapshot, stat.funding_ms, now).state == State::Fresh);
    assert(market_data_overview::field(snapshot, stat.open_interest_ms, now+600001).state == State::Stale);
    assert(market_data_overview::field(snapshot, 0, now).state == State::Unavailable);
    assert(market_data_overview::field(snapshot, now+1, now).state == State::Unavailable);
    assert(market_data_overview::field(snapshot, now-1, now-1).state == State::Unavailable);
}
