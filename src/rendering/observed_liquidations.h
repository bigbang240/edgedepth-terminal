#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <string_view>
#include <vector>

namespace observed_liquidations {
// Quantized world-space scale: camera translation cannot change membership.
inline double stable_marker_step(double desired, double minimum) {
    if (!(minimum>0) || !std::isfinite(minimum)) minimum=1;
    if (!std::isfinite(desired) || desired<=minimum) return minimum;
    return minimum*std::exp2(std::ceil(std::log2(desired/minimum)));
}

// Fixed-reference logarithmic sizing, not area-proportional sizing. The
// reference is a saved chart setting, never a percentile of the moving viewport.
inline float marker_radius(double value, double reference) {
    if (!(value>0) || !std::isfinite(value)) return 0;
    if (!(reference>0) || !std::isfinite(reference)) reference=25000;
    return float(std::clamp(4.0+2.0*std::log2(1.0+value/(reference/100.0)),4.0,20.0));
}

// Prices and quantity are reported fields, not an inferred match to tape prints.
template<class Event> double price(const Event& e) {
    return std::isfinite(e.avg_price) && e.avg_price > 0 ? e.avg_price : e.price;
}

// What each venue's liquidation feed is, in one line for hovers (James 2026-10-11: "Bybit publishes complete liqs").
// Binance's forceOrder stream sends at most one report per market per second, so its totals are a floor. Bybit's
// allLiquidation topic (the backend subscribes to it) carries every liquidation, priced at the bankruptcy price.
// Hyperliquid's are the liquidation fills on its public trade feed. Gaps in our own recording show as coverage in
// the activity pane; they are not a property of the venue's feed.
inline const char* feed_line(std::string_view exchange) {
    if (exchange == "binancef") return "Binance \xc2\xb7 at most 1 report per second, so a floor";
    if (exchange == "bybit") return "Bybit \xc2\xb7 every liquidation \xc2\xb7 bankruptcy prices";
    if (exchange == "hl") return "Hyperliquid \xc2\xb7 every liquidation fill";
    return "Exchange-reported liquidations";
}
inline bool sampled_feed(std::string_view exchange) { return exchange == "binancef"; }
// Bybit reports the bankruptcy price, often percent away from where the forced order traded.
inline bool bankruptcy_prices(std::string_view exchange) { return exchange == "bybit"; }

// Where a report sits on a real-time tape when its reported price is not a traded price (Bybit): the nearest print
// on the forced side (a forced buy lifts the ask) within window_ms, else the last print before it within 5 s. NaN
// when the tape has neither, so the report stays in the activity pane only. Trades must be in time order; each
// direction scans a bounded number of prints.
template<class Trades> double tape_price(const Trades& trades, int64_t t, bool forced_buy, int64_t window_ms = 2000) {
    constexpr int kScan = 256;
    const auto at = std::lower_bound(trades.begin(), trades.end(), t,
        [](const auto& trade, int64_t v) { return trade.timestamp_ms < v; });
    double best = std::numeric_limits<double>::quiet_NaN();
    int64_t best_gap = window_ms + 1;
    int n = 0;
    for (auto it = at; it != trades.end() && it->timestamp_ms - t <= window_ms && n < kScan; ++it, ++n)
        if (it->is_buy == forced_buy) { best_gap = it->timestamp_ms - t; best = it->price; break; }
    n = 0;
    for (auto it = at; it != trades.begin() && n < kScan; ++n) {
        --it;
        const int64_t gap = t - it->timestamp_ms;
        if (gap > window_ms || gap >= best_gap) break;
        if (it->is_buy == forced_buy) { best = it->price; break; }
    }
    if (std::isfinite(best) && best > 0) return best;
    if (at != trades.begin()) {
        const auto& before = *std::prev(at);
        if (t - before.timestamp_ms <= 5000 && before.price > 0) return before.price;
    }
    return std::numeric_limits<double>::quiet_NaN();
}
template<class Event> double notional(const Event& e) {
    const double p = price(e), n = p * e.qty;
    return e.timestamp_ms > 0 && std::isfinite(p) && p > 0 &&
        std::isfinite(e.qty) && e.qty > 0 && std::isfinite(n) ? n : 0;
}

// One causal scale for the whole chart at its playhead, independent of the
// viewport. Prefix totals let seeks select only this market's preceding hour
// without rescanning history every frame or including future reports.
struct MarketSizeReference {
    struct Reading { double usd = 1000; size_t count = 0; };
    struct Total { double usd = 0; size_t count = 0; };
    std::vector<Total> prefix;
    const void* source = nullptr;
    uint64_t revision = 0;
    template<class Events> Reading update(const Events& events, uint64_t next_revision, int64_t cutoff) {
        if (source != events.data() || revision != next_revision || prefix.size() != events.size() + 1) {
            source = events.data(); revision = next_revision;
            prefix.resize(events.size() + 1);
            prefix[0] = {};
            for (size_t i = 0; i < events.size(); ++i) {
                prefix[i + 1] = prefix[i];
                const double usd = notional(events[i]);
                if (usd > 0) { prefix[i + 1].usd += usd; ++prefix[i + 1].count; }
            }
        }
        if (cutoff <= 0) return {};
        const auto end = std::lower_bound(events.begin(), events.end(), cutoff,
            [](const auto& e, int64_t t) { return e.timestamp_ms < t; });
        const auto start = std::upper_bound(events.begin(), end, cutoff - 3600000,
            [](int64_t t, const auto& e) { return t < e.timestamp_ms; });
        const auto& a = prefix[size_t(start - events.begin())];
        const auto& b = prefix[size_t(end - events.begin())];
        const size_t count = b.count - a.count;
        return {count ? std::max(1000.0, (b.usd - a.usd) / count) : 1000.0, count};
    }
};
inline float relative_marker_radius(double multiple) {
    if (!(multiple > 0) || !std::isfinite(multiple)) return 0;
    return float(std::clamp(4.0 + 4.0 * std::log2(1.0 + multiple), 4.0, 20.0));
}

// Candle charts (James 2026-10-10, option C of three rendered on real BTC and IMX; the diamonds were rejected): a
// candle carries a callout only where its liquidations were unusual for this market: the dollars in a pill at the
// wick tip, shorts liquidated above the high, longs below the low. Every candle's totals stay in the activity pane.
// Unusual is a multiple of the typical candle: the mean of the nonzero long and short totals over the
// kTypicalCandles candles up to the clock (never the viewport, never a future report), floored at $1,000.
inline constexpr int64_t kTypicalCandles = 200;
inline constexpr double kCalloutDefault = 2.0;  // x the typical candle
inline constexpr double kTypicalFloor = 1000.0;

// Reports must be in time order; tf_ms is the chart's candle length. Reports under min_usd are left out.
template<class Events> double typical_candle(const Events& events, int64_t cutoff, int64_t tf_ms, double min_usd = 0) {
    if (tf_ms <= 0) return kTypicalFloor;
    const auto start = std::lower_bound(events.begin(), events.end(), cutoff - kTypicalCandles * tf_ms,
        [](const auto& e, int64_t t) { return e.timestamp_ms < t; });
    double sum = 0, longs = 0, shorts = 0;
    size_t n = 0;
    int64_t bucket = 0;
    bool open = false;
    const auto flush = [&] {
        if (longs > 0) { sum += longs; ++n; }
        if (shorts > 0) { sum += shorts; ++n; }
        longs = shorts = 0;
    };
    for (auto it = start; it != events.end() && it->timestamp_ms <= cutoff; ++it) {
        const double usd = notional(*it);
        if (usd <= 0 || usd < min_usd) continue;
        const int64_t b = it->timestamp_ms / tf_ms;
        if (!open || b != bucket) { flush(); bucket = b; open = true; }
        (it->is_buy ? shorts : longs) += usd;
    }
    flush();
    return n ? std::max(kTypicalFloor, sum / double(n)) : kTypicalFloor;
}

// One side of one candle: its reports' dollars, count, price range and first/last report times.
struct CandleSide {
    int64_t open = 0;
    bool shorts = false;  // forced buys (shorts liquidated); else longs
    double usd = 0, low = 0, high = 0;
    int64_t first = 0, last = 0;
    uint32_t count = 0;
};

// The candle sides in [from, to) whose dollars reach multiple x typical, largest first. open_of maps a report time to
// its candle's open on the chart (monotonic in time).
template<class Events, class OpenOf>
std::vector<CandleSide> callouts(const Events& events, int64_t from, int64_t to, double typical, double multiple,
                                 double min_usd, OpenOf open_of) {
    std::vector<CandleSide> out;
    const double bar = std::max(0.0, typical) * std::max(0.0, multiple);
    CandleSide sides[2];
    bool open = false;
    const auto flush = [&] {
        for (auto& s : sides)
            if (s.count && s.usd >= bar) out.push_back(s);
        sides[0] = sides[1] = CandleSide{};
    };
    const auto first = std::lower_bound(events.begin(), events.end(), from,
        [](const auto& e, int64_t t) { return e.timestamp_ms < t; });
    int64_t current = 0;
    for (auto it = first; it != events.end() && it->timestamp_ms < to; ++it) {
        const double usd = notional(*it), p = price(*it);
        if (usd <= 0 || usd < min_usd) continue;
        const int64_t o = open_of(it->timestamp_ms);
        if (!open || o != current) { flush(); current = o; open = true; }
        CandleSide& s = sides[it->is_buy ? 1 : 0];
        if (!s.count) { s.open = o; s.shorts = it->is_buy; s.first = it->timestamp_ms; s.low = s.high = p; }
        s.last = it->timestamp_ms;
        s.low = std::min(s.low, p);
        s.high = std::max(s.high, p);
        s.usd += usd;
        ++s.count;
    }
    flush();
    std::stable_sort(out.begin(), out.end(), [](const CandleSide& a, const CandleSide& b) { return a.usd > b.usd; });
    return out;
}

// Compact dollars for a pill (its colour and place say what it is): 950, 12.6K, 55.9M, 1.23B.
inline void compact_usd(double v, char* out, size_t n) {
    if (v >= 1e9) std::snprintf(out, n, "%.2fB", v / 1e9);
    else if (v >= 1e6) std::snprintf(out, n, "%.1fM", v / 1e6);
    else if (v >= 1e3) std::snprintf(out, n, "%.1fK", v / 1e3);
    else std::snprintf(out, n, "%.0f", v);
}

// Dollars for a hover: exact under $100K ($2,650.67 reads as a report), compact above ($597.8K, $1.23M).
inline void hover_usd(double v, char* out, size_t n) {
    if (!(v >= 0) || !std::isfinite(v)) { std::snprintf(out, n, "-"); return; }
    if (v >= 1e9) { std::snprintf(out, n, "$%.2fB", v / 1e9); return; }
    if (v >= 1e6) { std::snprintf(out, n, "$%.2fM", v / 1e6); return; }
    if (v >= 1e5) { std::snprintf(out, n, "$%.1fK", v / 1e3); return; }
    const long long cents = static_cast<long long>(std::llround(v * 100.0));
    const long long whole = cents / 100, frac = cents % 100;
    if (whole >= 1000) std::snprintf(out, n, "$%lld,%03lld.%02lld", whole / 1000, whole % 1000, frac);
    else std::snprintf(out, n, "$%lld.%02lld", whole, frac);
}

struct Bin { int64_t start_ms = 0; double longs = 0, shorts = 0; uint32_t count = 0; };
struct View {
    std::array<Bin, 1024> bins{};
    size_t size = 0;
    int64_t step_ms = 1000;
    double longs = 0, shorts = 0, peak = 0;
    uint64_t count = 0;
};
template<class Events> View aggregate(const Events& events, int64_t from, int64_t to, int64_t cutoff, int64_t step_ms = 1000) {
    View view;
    to = std::min(to, cutoff + 1);
    view.step_ms = std::max<int64_t>(1,step_ms);
    if (from < 0 || to <= from) return view;
    while ((to - from) / view.step_ms > 1022) view.step_ms *= 2;
    const int64_t origin = from / view.step_ms * view.step_ms;
    view.size = std::min<size_t>(1024, size_t((to - origin) / view.step_ms + 1));
    for (size_t i = 0; i < view.size; ++i) view.bins[i].start_ms = origin + int64_t(i) * view.step_ms;
    const auto first = std::lower_bound(events.begin(), events.end(), from,
        [](const auto& e, int64_t t) { return e.timestamp_ms < t; });
    for (auto it = first; it != events.end() && it->timestamp_ms < to; ++it) {
        const double n = notional(*it);
        if (n <= 0) continue;
        const size_t i = size_t((it->timestamp_ms - origin) / view.step_ms);
        if (i >= view.size) continue;
        auto& bin = view.bins[i];
        if (it->is_buy) { bin.shorts += n; view.shorts += n; }
        else { bin.longs += n; view.longs += n; }
        ++bin.count; ++view.count;
        view.peak = std::max({view.peak, bin.longs, bin.shorts});
    }
    return view;
}
} // namespace observed_liquidations
