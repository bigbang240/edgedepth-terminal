#pragma once
// Library-only candle preview. Never enters CandleManager or the replay clock.
#include "pb/messages.pb.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace PackPreview {
constexpr size_t max_header_bytes = 4 * 1024 * 1024;
struct Bar { int64_t time_ms; double open, high, low, close; };
struct Chart {
    std::vector<Bar> bars;
    int64_t start_ms = 0, end_ms = 0;
};
inline Chart decode(const void* data, size_t size, const std::string& exchange,
                    const std::string& symbol, const std::string& event_id) {
    Chart out;
    if (!data || !size || size > max_header_bytes || exchange.empty() || symbol.empty()) return out;
    pb::PackHeader h;
    if (!h.ParseFromArray(data, static_cast<int>(size)) ||
        (h.format_version() != 1 && h.format_version() != 2) ||
        h.exchange() != exchange || h.symbol() != symbol ||
        (!event_id.empty() && h.event_id() != event_id) ||
        h.start_ts_ms() <= 0 || h.end_ts_ms() <= h.start_ts_ms()) return out;
    out.start_ms = h.start_ts_ms(); out.end_ms = h.end_ts_ms();
    for (const auto& seed : h.candle_seeds()) {
        if (seed.timeframe_sec() != 60) continue;
        pb::Candles candles;
        if (!candles.ParseFromString(seed.candles()) || candles.timeframe() != 60) return {};
        for (const auto& c : candles.values()) {
            const auto ts = c.timestamp_ms();
            // Whole one-minute bars only: excludes pre-roll and a final candle
            // whose close can contain trades after this recording ended.
            if (ts < out.start_ms || ts > out.end_ms - 60000) continue;
            if (!std::isfinite(c.open()) || !std::isfinite(c.high()) ||
                !std::isfinite(c.low()) || !std::isfinite(c.close()) ||
                c.low() <= 0 || c.low() > std::min(c.open(), c.close()) ||
                c.high() < std::max(c.open(), c.close())) continue;
            out.bars.push_back({ts, c.open(), c.high(), c.low(), c.close()});
            if (out.bars.size() > 240) return {}; // bounded library thumbnail
        }
        std::sort(out.bars.begin(), out.bars.end(), [](const Bar& a, const Bar& b) { return a.time_ms < b.time_ms; });
        // A conflicting/duplicated timestamp is not a trustworthy price path.
        for (size_t i = 1; i < out.bars.size(); ++i)
            if (out.bars[i].time_ms == out.bars[i-1].time_ms) return {};
        if (out.bars.size() < 2) return {};
        return out;
    }
    return {};
}
} // namespace PackPreview
