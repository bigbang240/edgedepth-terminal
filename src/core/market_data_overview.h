#pragma once
#include "core/stats_snapshot.h"
#include "core/ticker_manager.h"

// Free overview uses public feeds only, never paid history or research receipts.
namespace market_data_overview {
inline bool ticker_fresh(const TickerEntry* ticker, int64_t now) {
    return ticker && ticker->event_time_ms > 0 && ticker->event_time_ms <= now &&
        now - ticker->event_time_ms <= 60000;
}
inline StatFreshness::Field field(const StatsSnapshot& snapshot, int64_t observed, int64_t now) {
    // Legacy receipt time cannot establish whether a proto3 zero was observed.
    // Keep an unstamped value unavailable instead of inventing a live reading.
    if (!snapshot.received_ms || snapshot.received_ms > now || observed > now)
        return {StatFreshness::State::Unavailable, 0};
    return StatFreshness::field(true, observed, now, 600000);
}
}
