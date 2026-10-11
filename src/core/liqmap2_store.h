#pragma once
// V2 modelled liquidation map (hub task liq-map-v2; backend docs/LIQMAP2_LIVE.md): one frame per market
// per minute from the backend publisher (stream 38, history stream 39). Each frame is the standing
// long liquidations below its mark and short liquidations above it, in USD, modelled from that venue's
// open interest and candles with a liquidation-distance profile learned from Hyperliquid positions.
// An estimate, never observed positions (the HL census is census_history) and never a magnet.
//
// What a frame may claim follows the registered tests (hub outputs/liq-map-v2-20261004): Hyperliquid
// (Test 1) and Bybit (Test 2) passed for the markets they covered; Binance has no outcome test and
// Bybit symbols outside Test 2's universe are published provisional (James 2026-10-05). claim() and
// caption() are the only place that wording is chosen.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace liqmap2 {
inline constexpr int64_t kColumnMs = 60000;
inline constexpr int64_t kFreshMs = 120000;   // a frame older than this at the clock is stale
inline constexpr size_t kMaxFrames = 361;     // six hours of minute frames
inline constexpr size_t kMaxLevels = 256;     // the publisher's cap
inline constexpr size_t kMaxMarkets = 4;
inline constexpr size_t kV22Bands = 40;        // V2.2's 0.5% bands per side
inline constexpr double kV22BandWidth = 0.005;

struct Level { double price = 0, long_usd = 0, short_usd = 0; };

struct Frame {
    int64_t timestamp_ms = 0, warm_since_ms = 0, last_bar_ms = 0;
    double mark_price = 0, band_width_pct = 0, total_long_usd = 0, total_short_usd = 0, usd_scale = 0;
    std::string status, model_class, config_id;
    bool provisional = false;
    uint32_t dropped_levels = 0;
    std::vector<Level> levels;  // ascending price
    // V2.2 (hub v21/PREREG_V2_2A): expected liquidation USD if price reaches band k within 30 minutes, index k - 1;
    // both empty when the frame carries no V2.2.
    std::vector<double> expected_long, expected_short;
    std::string regime, v22_config_id;
    bool hot = false;
    double day_move = 0;
};

inline bool has_v22(const Frame& f) { return f.expected_long.size() == kV22Bands; }

// Band k's price range on its side of mark (long bands below, short above).
inline void v22_band(bool is_long, int k, double mark, double& lo, double& hi) {
    if (is_long) {
        lo = mark*(1 - k*kV22BandWidth);
        hi = mark*(1 - (k - 1)*kV22BandWidth);
    } else {
        lo = mark*(1 + (k - 1)*kV22BandWidth);
        hi = mark*(1 + k*kV22BandWidth);
    }
}

// A frame the publisher could have sent: known status, levels on their side of the mark in ascending
// price, finite non-negative dollars, totals equal to the level sums (to float32 rounding, which a
// history read applies to levels but not totals).
inline bool valid(const Frame& f) {
    if (f.timestamp_ms <= 0 || !std::isfinite(f.mark_price) || f.mark_price < 0 ||
        !std::isfinite(f.band_width_pct) || f.band_width_pct <= 0 || f.levels.size() > kMaxLevels)
        return false;
    if (f.status != "ready" && f.status != "warming" && f.status != "stale") return false;
    if (f.mark_price == 0 && !f.levels.empty()) return false;
    double longs = 0, shorts = 0, prev = 0;
    for (const auto& l : f.levels) {
        if (!std::isfinite(l.price) || !(l.price > prev) || !std::isfinite(l.long_usd) || l.long_usd < 0 ||
            !std::isfinite(l.short_usd) || l.short_usd < 0 || (l.long_usd > 0 && !(l.price < f.mark_price)) ||
            (l.short_usd > 0 && !(l.price > f.mark_price)))
            return false;
        prev = l.price;
        longs += l.long_usd;
        shorts += l.short_usd;
    }
    const auto near = [](double sum, double total) {
        return std::isfinite(total) && total >= 0 && std::abs(sum - total) <= 1e-6*std::max(1.0, total);
    };
    if (!f.expected_long.empty() || !f.expected_short.empty()) {
        if (f.expected_long.size() != kV22Bands || f.expected_short.size() != kV22Bands || !(f.mark_price > 0)) return false;
        for (size_t i = 0; i < kV22Bands; ++i)
            if (!std::isfinite(f.expected_long[i]) || f.expected_long[i] < 0 || !std::isfinite(f.expected_short[i]) || f.expected_short[i] < 0)
                return false;
        if (f.regime != "calm" && f.regime != "normal" && f.regime != "active" && f.regime != "extreme") return false;
        if (!std::isfinite(f.day_move)) return false;
    }
    return near(longs, f.total_long_usd) && near(shorts, f.total_short_usd);
}

// Bounded, source-timestamped frames for one market. No interpolation over gaps.
class History {
public:
    bool insert(Frame f) {
        if (!valid(f)) return false;
        frames_.insert_or_assign(f.timestamp_ms, std::move(f));
        revision_ = next_revision_.fetch_add(1, std::memory_order_relaxed) + 1;
        while (frames_.size() > kMaxFrames) frames_.erase(frames_.begin());
        return true;
    }
    const Frame* at(int64_t asof) const {
        auto it = frames_.upper_bound(asof);
        return it == frames_.begin() ? nullptr : &std::prev(it)->second;
    }
    // A column paints until the next frame, at most one minute, and only the newest known frame is held
    // to the clock; an older one never paints through a gap.
    int64_t display_end(int64_t ts, int64_t asof) const {
        const auto it = frames_.find(ts);
        if (it == frames_.end() || ts > asof) return ts;
        const auto next = std::next(it);
        if (next == frames_.end() || next->first > asof) return std::min(asof, ts + kColumnMs);
        return std::min({asof, ts + kColumnMs, next->first});
    }
    uint64_t revision() const { return revision_; }
    const std::map<int64_t, Frame>& frames() const { return frames_; }
private:
    std::map<int64_t, Frame> frames_;
    uint64_t revision_ = 0;
    // Unique across histories: replacing/evicting a market cannot reuse a cached revision.
    inline static std::atomic<uint64_t> next_revision_{0};
};

enum class State { missing, stale, warming, ready };

inline State state(const Frame* f, int64_t asof) {
    if (!f || f->timestamp_ms > asof) return State::missing;
    if (asof - f->timestamp_ms > kFreshMs || f->status == "stale") return State::stale;
    if (f->status == "warming") return State::warming;
    return State::ready;
}

// The claim a frame may carry. Tested: the venue's registered test covered this market. Hyperliquid
// small coins carry no tested wording (Test 1's small-coin split did not support it).
enum class Claim { tested, untested_small, provisional_bybit, provisional_venue };

inline Claim claim(const std::string& exchange, const Frame& f) {
    if (f.provisional) return exchange == "bybit" ? Claim::provisional_bybit : Claim::provisional_venue;
    if (exchange == "hl" && f.model_class == "small") return Claim::untested_small;
    return Claim::tested;
}

inline const char* venue_name(const std::string& exchange) {
    if (exchange == "hl") return "Hyperliquid";
    if (exchange == "bybit") return "Bybit";
    if (exchange == "binancef") return "Binance";
    return "this venue";
}

// The registered record behind a tested frame (hub outputs/liq-map-v2-20261004, one look each):
// reduction of the baseline's deviance in realized liquidation dollars, with its 95% day-block CI.
// Null when the frame carries no tested claim.
inline const char* record(const std::string& exchange, const Frame& f) {
    const Claim c = claim(exchange, f);
    if (c == Claim::untested_small)
        return "Hyperliquid test 2026-10-04 passed overall; its small-coin split did not (-11.2% at 24h)";
    if (c != Claim::tested) return nullptr;
    if (exchange == "hl")
        return "Hyperliquid test 2026-10-04: 9.2% better than baseline at 24h (95% CI 3.8-13.4), 18.0% at 1h, 16.0% at 4h";
    if (exchange == "bybit")
        return "Bybit test 2026-10-04: 6.2% better than baseline at 24h (95% CI 4.9-7.7), 14.0% at 1h, 13.7% at 4h";
    return nullptr;
}

// How far the dollars can be read as dollars, from the same tests (realized / estimated by decile of
// the estimate). Hyperliquid's top deciles fell outside [0.5, 2], so its dollars are not calibrated.
inline const char* dollars(const std::string& exchange, const Frame& f) {
    if (claim(exchange, f) != Claim::tested) return "Dollar sizes are untested for this market";
    if (exchange == "hl")
        return "Dollar sizes are not calibrated: the largest estimated levels realized about 0.4-0.6x in the test";
    if (exchange == "bybit")
        return "Dollar sizes held within 0.5-2x for the largest levels at 4h and 24h in the test, not at 1h (0.48x)";
    return "Dollar sizes are untested for this market";
}

// The claim in a few words, for captions that name the fuel (the rails).
inline const char* claim_short(const std::string& exchange, const Frame& f) {
    switch (claim(exchange, f)) {
        case Claim::tested: return "tested";
        case Claim::untested_small: return "small coins untested";
        case Claim::provisional_bybit: return "not covered by the Bybit test";
        case Claim::provisional_venue: return "untested on this venue";
    }
    return "untested";
}

// One-line caption: what the picture is, its claim, always "not a magnet".
inline void caption(const std::string& exchange, const Frame& f, char* out, size_t n) {
    constexpr const char* dot = " \xc2\xb7 ";
    const char* what = nullptr;
    switch (claim(exchange, f)) {
        case Claim::tested: what = "modelled from this venue's open interest, tested"; break;
        case Claim::untested_small: what = "modelled from this venue's open interest, small coins untested"; break;
        case Claim::provisional_bybit: what = "modelled, not covered by the Bybit test"; break;
        case Claim::provisional_venue: what = "modelled, untested on this venue"; break;
    }
    std::snprintf(out, n, "V2 MAP%sestimated liquidations in $%s%s%snot a magnet", dot, dot, what, dot);
}

// Dollars, compactly.
inline void money(double value, char* out, size_t size) {
    if (value >= 1e9) std::snprintf(out, size, "$%.2fB", value/1e9);
    else if (value >= 1e6) std::snprintf(out, size, "$%.2fM", value/1e6);
    else if (value >= 1e3) std::snprintf(out, size, "$%.1fK", value/1e3);
    else std::snprintf(out, size, "$%.0f", value);
}

// V2.2's record for a venue (hub v21/RESULT_V2_2A_TEST.md and RESULT_V2_2.md), null where it has none.
inline const char* v22_record(const std::string& exchange) {
    if (exchange == "bybit")
        return "V2.2 test 2026-10-06: 2.0 points better than V2 at 30 min (95% CI 1.5-2.6, Bybit Apr-May 2026); June +1.0 (0.3-1.7)";
    if (exchange == "binancef") return "V2.2 on Binance uses the model fitted on Bybit: untested on this venue";
    return nullptr;
}

// The panel's V2.2 line: regime, hot state and the expected dollars within 3% of the mark on each side.
inline void v22_line(const std::string& exchange, const Frame& f, char* out, size_t n) {
    if (!has_v22(f)) {
        out[0] = 0;
        return;
    }
    double longs = 0, shorts = 0;
    for (size_t i = 0; i < 6; ++i) {  // bands 1..6: within 3%
        longs += f.expected_long[i];
        shorts += f.expected_short[i];
    }
    char a[32], b[32], hot[48] = "";
    money(longs, a, sizeof(a));
    money(shorts, b, sizeof(b));
    if (f.hot) std::snprintf(hot, sizeof(hot), " | HOT %+.0f%% in 24h", 100*(std::exp(f.day_move) - 1));
    std::snprintf(out, n, "V2.2 next 30 min | %s volatility%s | if reached within 3%%: longs %s, shorts %s%s", f.regime.c_str(), hot, a, b,
                  exchange == "binancef" ? " | untested on Binance" : "");
}

// One market's public scorecard over the backend's rolling window (backend cmd/liqmap2-scorecard,
// server_session get_liqmap2_scorecard): in the minutes scored, the dollars the published map had at
// the levels price traded through, beside the liquidations the venue reported then.
struct Scorecard {
    int window_days = 0, days = 0, minutes_scored = 0, reached_long = 0, reached_short = 0;
    int events_reached = 0, events_all = 0;
    std::string from, to, source;  // source: complete | lower_bound
    bool provisional = false;
    double pred_long = 0, pred_short = 0, real_long_reached = 0, real_short_reached = 0;
    double real_long_all = 0, real_short_all = 0;
};

// The panel's scorecard line, or why there is none. frame: the frame at the clock (may be null); sc: the
// received scorecard (null until it arrives).
inline void scorecard_line(const std::string& exchange, const Frame* f, const Scorecard* sc, char* out, size_t n) {
    if (exchange == "hl") {
        std::snprintf(out, n, "Scorecard: Hyperliquid not scored yet (its full liquidation record is not in the hub)");
    } else if (exchange == "bybit" && f && f->provisional) {
        std::snprintf(out, n, "Scorecard: not scored (held out of the Bybit test)");
    } else if (!sc) {
        std::snprintf(out, n, "Scorecard: loading");
    } else if (sc->days == 0) {
        std::snprintf(out, n, "Scorecard: no scored day yet (a market scores once its map is ready all day)");
    } else {
        char pred[32], real[32];
        money(sc->pred_long + sc->pred_short, pred, sizeof(pred));
        money(sc->real_long_reached + sc->real_short_reached, real, sizeof(real));
        const double predicted = sc->pred_long + sc->pred_short;
        char ratio[40] = "";
        if (predicted > 0)
            std::snprintf(ratio, sizeof(ratio), " (%.2fx)", (sc->real_long_reached + sc->real_short_reached)/predicted);
        std::snprintf(out, n, "Scorecard %d day%s: map %s where price went, %s reported there%s%s", sc->days,
                      sc->days == 1 ? "" : "s", pred, real, ratio, sc->source == "lower_bound" ? " (a lower bound)" : "");
    }
}

// The market's public daily record on the web (every Binance and Bybit market has one, locked or not):
// https://edgedepth.com/research/liquidation-map/scorecard/{binance|bybit}/{symbol}. False, with out
// empty, where there is none (Hyperliquid is not scored).
inline bool scorecard_url(const std::string& exchange, const std::string& symbol, char* out, size_t n) {
    const char* venue = exchange == "binancef" ? "binance" : exchange == "bybit" ? "bybit" : nullptr;
    if (n) out[0] = 0;
    if (!venue || symbol.empty()) return false;
    std::string lower(symbol);
    for (auto& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    std::snprintf(out, n, "https://edgedepth.com/research/liquidation-map/scorecard/%s/%s", venue, lower.c_str());
    return true;
}

// Tooltip detail for a scored market: up to four lines; returns how many it wrote.
inline int scorecard_detail(const Scorecard& sc, char lines[4][200]) {
    if (sc.days == 0) return 0;
    int k = 0;
    std::snprintf(lines[k++], 200, "Scorecard: %d scored day%s (%s to %s) in the last %d, %d scored minutes",
                  sc.days, sc.days == 1 ? "" : "s", sc.from.c_str(), sc.to.c_str(), sc.window_days, sc.minutes_scored);
    std::snprintf(lines[k++], 200, "Price reached mapped long levels in %d minutes and short levels in %d", sc.reached_long,
                  sc.reached_short);
    const double all = sc.real_long_all + sc.real_short_all;
    if (all > 0) {
        char a[32];
        money(all, a, sizeof(a));
        std::snprintf(lines[k++], 200, "Of %s reported in scored minutes, %.0f%% came where the map had levels", a,
                      100*(sc.real_long_reached + sc.real_short_reached)/all);
    }
    std::snprintf(lines[k++], 200, "%s", sc.source == "lower_bound"
        ? "This venue throttles its liquidation feed: reported dollars are a lower bound. A daily record, not a test."
        : "A daily record, not a test.");
    return k;
}

}  // namespace liqmap2
