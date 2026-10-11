#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

// Closed-candle HLC3 VWAP, not a trade-price VWAP. UTC periods are independent
// of the display timezone. Incomplete periods never supply reference levels.
namespace reference_context {
inline constexpr int64_t day_ms = 86400000;
inline int64_t day_start(int64_t t) { return t - ((t % day_ms + day_ms) % day_ms); }
inline int64_t week_start(int64_t t) {
    const auto day = day_start(t) / day_ms;
    return (day - ((day + 3) % 7 + 7) % 7) * day_ms; // Monday
}
struct Series {
    std::vector<double> times, values;
    bool complete = false;
    void clear() { times.clear(); values.clear(); complete = false; }
};
struct Levels { bool complete = false; double high = 0, low = 0, close = 0; };
template<class Candle> bool valid(const Candle& c) {
    return std::isfinite(c.high) && std::isfinite(c.low) && std::isfinite(c.close) &&
           std::isfinite(c.volume) && c.low > 0 && c.high >= c.low &&
           c.close >= c.low && c.close <= c.high && c.volume >= 0;
}
// max_gap_ms: a run of missing candles up to this long inside loaded history is a quiet stretch (no trades, so
// no candle; the exchange's own klines fill such minutes), not missing data. The chart passes kQuietGapMs, the
// backend's quiet-minute rule; 0 keeps the strict contiguity. Either way the loaded history must reach back to the
// period's start (a later first candle is history not loaded, and the period stays incomplete).
inline constexpr int64_t kQuietGapMs = 3600000;
template<class Candles>
bool covers(const Candles& candles, int64_t start) {
    return !candles.empty() && candles.front().timestamp_ms <= start;
}
template<class Candles>
void vwap(const Candles& candles, int64_t start, int64_t asof, int64_t tf,
          Series& out, int64_t max_gap_ms = 0) {
    out.clear();
    if (tf <= 0 || start <= 0 || start % tf || asof <= start) return;
    if (!covers(candles, start)) return;
    int64_t expected = start;
    double volume = 0, weighted = 0;
    for (const auto& c : candles) {
        if (c.timestamp_ms < start) continue;
        if (c.timestamp_ms > asof - tf) break; // A full bucket must have elapsed.
        if (c.timestamp_ms < expected) continue;
        if (c.timestamp_ms - expected > max_gap_ms || !valid(c)) break;
        expected = c.timestamp_ms + tf;
        volume += c.volume;
        weighted += ((c.high + c.low + c.close) / 3.0) * c.volume;
        if (!std::isfinite(volume) || !std::isfinite(weighted)) { out.clear(); return; }
        if (volume > 0) {
            // Values become known at the close, never at the candle's opening.
            out.times.push_back(static_cast<double>(expected));
            out.values.push_back(weighted / volume);
        }
    }
    out.complete = (asof / tf) * tf - expected <= max_gap_ms && expected <= (asof / tf) * tf && volume > 0;
    if (out.complete && !out.times.empty() && out.times.back() < static_cast<double>(asof)) {
        out.times.push_back(static_cast<double>(asof));
        out.values.push_back(out.values.back()); // last completed-bar value
    }
}
template<class Candles>
Levels levels(const Candles& candles, int64_t start, int64_t end, int64_t tf, int64_t max_gap_ms = 0) {
    Levels out;
    if (tf <= 0 || start % tf || end % tf || end <= start) return out;
    if (!covers(candles, start)) return out;
    int64_t expected = start;
    bool any = false;
    for (const auto& c : candles) {
        if (c.timestamp_ms < start) continue;
        if (c.timestamp_ms >= end) break;
        if (c.timestamp_ms < expected) continue;
        if (c.timestamp_ms - expected > max_gap_ms || !valid(c)) return {};
        if (!any) { out.high = c.high; out.low = c.low; any = true; }
        else { out.high = std::max(out.high, c.high); out.low = std::min(out.low, c.low); }
        out.close = c.close; expected = c.timestamp_ms + tf;
    }
    out.complete = any && end - expected <= max_gap_ms;
    return out;
}
}
