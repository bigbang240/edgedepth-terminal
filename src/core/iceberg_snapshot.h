#pragma once
#include <cmath>
#include <cstdint>

// Live display only: taker-side repeated-trade clusters, not hidden inventory.
struct IcebergSnapshot {
    int64_t timestamp_ms = 0, source_trade_ms = 0, start_ms = 0, end_ms = 0, detector_start_ms = 0;
    int buy_clusters = 0, sell_clusters = 0;
    double buy_usd = 0, sell_usd = 0;
    bool history_limited = false;
    double min_confidence = 0, min_bulk_usd = 0;
    int min_refreshes = 0;
    bool apply(const IcebergSnapshot& incoming, int64_t now) {
        if (incoming.timestamp_ms <= 0 || incoming.timestamp_ms > now || incoming.timestamp_ms < timestamp_ms ||
            incoming.end_ms != incoming.timestamp_ms || incoming.start_ms <= 0 || incoming.end_ms - incoming.start_ms != 300000 ||
            incoming.detector_start_ms <= 0 || incoming.detector_start_ms > incoming.end_ms ||
            incoming.source_trade_ms < 0 || incoming.source_trade_ms > incoming.timestamp_ms ||
            incoming.buy_clusters < 0 || incoming.sell_clusters < 0 ||
            !std::isfinite(incoming.buy_usd) || !std::isfinite(incoming.sell_usd) || incoming.buy_usd < 0 || incoming.sell_usd < 0 ||
            !std::isfinite(incoming.buy_usd + incoming.sell_usd) ||
            (incoming.buy_clusters == 0) != (incoming.buy_usd == 0) ||
            (incoming.sell_clusters == 0) != (incoming.sell_usd == 0) ||
            !std::isfinite(incoming.min_confidence) || incoming.min_confidence <= 0 || incoming.min_confidence > 1 ||
            !std::isfinite(incoming.min_bulk_usd) || incoming.min_bulk_usd <= 0 || incoming.min_refreshes < 2) return false;
        *this = incoming; return true;
    }
    bool fresh(int64_t now) const {
        return timestamp_ms > 0 && timestamp_ms <= now && now - timestamp_ms <= 90000 &&
            source_trade_ms > 0 && source_trade_ms <= now && now - source_trade_ms <= 90000;
    }
    bool warming() const { return detector_start_ms > start_ms; }
    double bias() const { const double total = buy_usd + sell_usd; return total > 0 ? (buy_usd - sell_usd) / total : 0; }
};
