#pragma once
#include "core/stat_freshness.h"
#include "types/types.h"
#include <cmath>

// The backend publishes UTC window snapshots, with a seconds bucket timestamp
// despite the wire field's timestamp_ms name. Never difference these cumulative
// counts as if the timestamp were a sampling clock, or sum repeated partials.
struct StatsSnapshot {
    static constexpr int64_t timeframe_ms = 60000;
    static constexpr int64_t hour_ms = 3600000;
    explicit StatsSnapshot(int64_t width_ms = timeframe_ms) : window_ms(width_ms) {}
    const int64_t window_ms;
    Terminal::Stat stat{};
    int64_t received_ms = 0;
    int64_t bucket_ms = 0;

    bool apply(const Terminal::Stat& incoming, int64_t now_ms) {
        if (incoming.historical) return false; // OI/funding history cannot replace live aggregates
        if (incoming.timeframe != window_ms / 1000 && incoming.timeframe != window_ms) return false;
        if (incoming.timestamp_ms <= 0) return false;
        const int64_t bucket = incoming.timestamp_ms < 100000000000LL ?
            incoming.timestamp_ms * 1000 : incoming.timestamp_ms;
        if (bucket > now_ms || bucket < bucket_ms) return false;
        if (bucket == bucket_ms && received_ms &&
            ((stat.final && !incoming.final) || incoming.trade_buy < stat.trade_buy ||
             incoming.trade_sell < stat.trade_sell || incoming.liq_long_usd < stat.liq_long_usd ||
             incoming.liq_short_usd < stat.liq_short_usd)) return false;
        const auto previous = stat;
        stat = incoming;
        if (!StatFreshness::stated(stat)) {
            if (stat.mark_price == 0) stat.mark_price = previous.mark_price;
            if (stat.open_interest_usd == 0) stat.open_interest_usd = previous.open_interest_usd;
            if (stat.next_funding_time == 0) stat.next_funding_time = previous.next_funding_time;
        }
        bucket_ms = bucket;
        received_ms = now_ms;
        return true;
    }

    bool active(int64_t now_ms) const {
        return received_ms > 0 && now_ms >= received_ms && now_ms - received_ms <= (window_ms == hour_ms ? 60000 : 15000) &&
            bucket_ms <= now_ms && now_ms < bucket_ms + window_ms;
    }
};

// CVD uses the venue's direct volume sampler, independently of account-ratio
// availability. Snapshots replace; only base quantities are monotonic, not CVD.
struct MinuteVolumeSnapshot {
    Terminal::Volume volume{};
    int64_t bucket_ms = 0, received_ms = 0;
    bool apply(const Terminal::Volume& incoming, int64_t now) {
        if ((incoming.timeframe != 60 && incoming.timeframe != 60000) || incoming.timestamp_ms <= 0 ||
            !std::isfinite(incoming.buy) || !std::isfinite(incoming.sell) || !std::isfinite(incoming.cvd) ||
            !std::isfinite(incoming.vwap_buy) || !std::isfinite(incoming.vwap_sell) ||
            incoming.buy < 0 || incoming.sell < 0 || incoming.vwap_buy < 0 || incoming.vwap_sell < 0) return false;
        const int64_t bucket = incoming.timestamp_ms < 100000000000LL ? incoming.timestamp_ms * 1000 : incoming.timestamp_ms;
        if (bucket > now || bucket < bucket_ms || bucket % 60000 != 0) return false;
        if (received_ms && bucket == bucket_ms && ((volume.final && !incoming.final) ||
            incoming.buy < volume.buy || incoming.sell < volume.sell)) return false;
        volume = incoming; bucket_ms = bucket; received_ms = now; return true;
    }
    bool active(int64_t now) const {
        return received_ms > 0 && now >= received_ms && now - received_ms <= 15000 &&
            now >= bucket_ms && now - bucket_ms < 60000;
    }
};
