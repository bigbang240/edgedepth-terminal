#pragma once
// Liquidation rails: liquidation fuel weighted by the calibrated chance price reaches it.
// Each rail is the fuel standing in one log-price bin (summed, at its fuel-weighted price) times the
// frozen 1h/4h/24h touch odds of that price (core/touch_odds.h). The odds are tested; the fuel comes
// from a FuelSource (below): the scenario model (assumed leverage, which in the registered Binance
// test did not change the odds) or positions observed on Hyperliquid with their real liquidation
// prices. Either way a bright rail means "a lot of fuel within likely reach", never a magnet.
// Bins nearer than the model's 0.5% use the 0.5% chance on that side, a floor: closer levels are
// touched at least as often. Fuel past the model's 10% is left out.
#include "core/touch_zones.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

namespace liq_rails {

struct Rail { double price = 0, fuel = 0; int side = 0; double p[3]{}, w[3]{}; };

// prob(level, k) is the touch chance of level by horizon k (0 = 1h, 1 = 4h, 2 = 24h), negative
// outside the model's range. bin is the rail width in log price.
template <class Prob>
std::vector<Rail> build(const std::vector<touch_zones::Fuel>& fuel, double mark, double dmin, double dmax,
                        double bin, Prob prob) {
    std::vector<Rail> out;
    if (!(mark > 0) || !(bin > 0) || !(dmin > 0) || !(dmax > dmin)) return out;
    struct Acc { double mass = 0, weighted = 0; };
    std::map<long long, Acc> bins;
    const double lm = std::log(mark);
    for (const auto& f : fuel) {
        if (!(f.price > 0) || !(f.mass > 0) || !std::isfinite(f.mass)) continue;
        const double d = f.price/mark - 1.0, dist = d >= 0 ? d : -d;
        if (dist > dmax || d == 0) continue;
        // Bins never straddle the mark: each side counts from the mark outward.
        const long long k = d > 0 ? static_cast<long long>(std::floor((std::log(f.price) - lm)/bin)) + 1
                                  : -(static_cast<long long>(std::floor((lm - std::log(f.price))/bin)) + 1);
        auto& acc = bins[k];
        acc.mass += f.mass;
        acc.weighted += f.mass*f.price;
    }
    for (const auto& [k, acc] : bins) {
        Rail r;
        r.price = acc.weighted/acc.mass;
        r.fuel = acc.mass;
        r.side = k > 0 ? 1 : -1;
        const double dist = std::abs(r.price/mark - 1.0);
        const double level = dist < dmin ? mark*(1.0 + r.side*dmin) : r.price;
        bool ok = true;
        for (int h = 0; h < 3 && ok; ++h) {
            r.p[h] = prob(level, h);
            ok = r.p[h] >= 0;
            r.w[h] = r.fuel*r.p[h];
        }
        if (ok) out.push_back(r);
    }
    return out;  // ascending price: the map orders bins below the mark, then above
}

// The strongest rails on one side at horizon h, at least sep apart in log price, strongest first.
inline std::vector<Rail> strongest(const std::vector<Rail>& rails, int side, int h, int n, double sep) {
    std::vector<Rail> sorted;
    for (const auto& r : rails) if (r.side == side && r.w[h] > 0) sorted.push_back(r);
    std::sort(sorted.begin(), sorted.end(), [h](const Rail& a, const Rail& b) { return a.w[h] > b.w[h]; });
    std::vector<Rail> out;
    for (const auto& r : sorted) {
        if (static_cast<int>(out.size()) >= n) break;
        bool near = false;
        for (const auto& o : out) near = near || std::abs(std::log(o.price/r.price)) < sep;
        if (!near) out.push_back(r);
    }
    return out;
}

// Historical rails: at each recorded forecast point, the scenario fuel standing then (field runs whose
// [start, end) holds the point's time, each spread evenly over its buckets as the merge averaged it), summed
// on a fixed log-price grid of width `bin` (so a level stays on one row across time and reads as a streak)
// and weighted by that point's own recorded touch chance at horizon h. The 0.5% floor and 10% cut are
// build()'s, measured from the point's own mark. A run that never ended has end INT64_MAX. Points must be
// ascending in time.
struct Run { double lo = 0, hi = 0, fuel = 0; int64_t start = 0, end = 0; };
struct Point { int64_t t = 0; double mark = 0; };
struct Cell { int64_t t = 0; long long row = 0; double price = 0, w = 0; int side = 0; };

template <class ProbAt>
std::vector<Cell> history_cells(const std::vector<Run>& runs, double bw, const std::vector<Point>& points,
                                double dmin, double dmax, double bin, ProbAt prob) {
    std::vector<Cell> out;
    if (!(bw > 0) || !(bin > 0)) return out;
    std::vector<size_t> order(runs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return runs[a].start < runs[b].start; });
    std::vector<size_t> active;
    size_t next = 0;
    struct Acc { double mass = 0, weighted = 0; };
    std::map<long long, Acc> rows;
    for (size_t k = 0; k < points.size(); ++k) {
        const auto& pt = points[k];
        while (next < order.size() && runs[order[next]].start <= pt.t) active.push_back(order[next++]);
        active.erase(std::remove_if(active.begin(), active.end(), [&](size_t i) { return runs[i].end <= pt.t; }), active.end());
        if (!(pt.mark > 0)) continue;
        const double lo = pt.mark*(1.0 - dmax), hi = pt.mark*(1.0 + dmax);
        rows.clear();
        for (size_t i : active) {
            const auto& r = runs[i];
            if (r.hi < lo || r.lo > hi || !(r.fuel > 0) || !(r.lo > 0)) continue;
            const int n = std::max(1, static_cast<int>(std::lround(std::log(r.hi/r.lo)/bw)) + 1);
            for (int b = 0; b < n; ++b) {
                const double price = r.lo*std::exp(b*bw);
                if (price < lo || price > hi) continue;
                auto& a = rows[static_cast<long long>(std::floor(std::log(price)/bin))];
                a.mass += r.fuel; a.weighted += r.fuel*price;
            }
        }
        for (const auto& [row, a] : rows) {
            const double price = a.weighted/a.mass, d = price/pt.mark - 1.0, dist = std::abs(d);
            if (dist > dmax || d == 0) continue;
            const int side = d > 0 ? 1 : -1;
            const double p = prob(k, dist < dmin ? pt.mark*(1.0 + side*dmin) : price);
            if (p > 0) out.push_back({pt.t, row, price, a.mass*p, side});
        }
    }
    return out;
}

// Scenario-map segments as historical runs. A segment still standing (end == pending) never ended.
template <class Seg>
std::vector<Run> runs_from_segments(const std::vector<Seg>& segs, int64_t pending) {
    std::vector<Run> out;
    out.reserve(segs.size());
    for (const auto& s : segs)
        out.push_back({s.price_lo, s.price_hi, s.intensity, s.start_ms, s.end_ms == pending ? INT64_MAX : s.end_ms});
    return out;
}

// Where the rails' fuel comes from. The rails only weight it by touch odds; what the fuel IS belongs to
// the source, and the caption and tooltip name it. Scenario: the candle-derived map, relative units,
// built at the clock. Observed: Hyperliquid positions with their real liquidation prices, in USD, as a
// sampled census taken at asof_ms (only part of the venue's exposure is located; coverage says how much).
// Modelled: the V2 map of the chart's own venue (core/liqmap2_store.h), estimated USD from that venue's
// open interest, as published at asof_ms; claim names what its tests allow.
enum class FuelKind { Scenario, Observed, Modelled };
struct FuelSource {
    FuelKind kind = FuelKind::Scenario;
    std::vector<touch_zones::Fuel> standing;  // (price, mass) standing now
    std::vector<signed char> side;            // observed and modelled, parallel to standing: -1 long, +1 short
    int64_t asof_ms = 0;                      // observed: when the census was taken; modelled: the frame clock
    double coverage = -1;                     // observed: located share of exposure, 0..1; negative unknown
    const char* fallback = nullptr;           // scenario standing in for a chosen source: why
    const char* claim = nullptr;              // modelled: "tested", "untested on this venue", ...
};

// A census frame's bands as observed fuel: each band's long notional (liquidated if price falls to it)
// and short notional (if price rises to it), kept with its side.
template <class Band>
FuelSource observed(const std::vector<Band>& bands, int64_t asof_ms, double coverage) {
    FuelSource s;
    s.kind = FuelKind::Observed;
    s.asof_ms = asof_ms;
    s.coverage = coverage;
    for (const auto& b : bands) {
        if (!(b.price_mid > 0)) continue;
        if (b.est_long_usd > 0 && std::isfinite(b.est_long_usd)) { s.standing.push_back({b.price_mid, b.est_long_usd}); s.side.push_back(-1); }
        if (b.est_short_usd > 0 && std::isfinite(b.est_short_usd)) { s.standing.push_back({b.price_mid, b.est_short_usd}); s.side.push_back(1); }
    }
    return s;
}

// A V2 map frame's levels as modelled fuel, each side kept (long below the frame's mark, short above).
template <class Level>
FuelSource modelled(const std::vector<Level>& levels, int64_t asof_ms, const char* claim) {
    FuelSource s;
    s.kind = FuelKind::Modelled;
    s.asof_ms = asof_ms;
    s.claim = claim;
    for (const auto& l : levels) {
        if (!(l.price > 0)) continue;
        if (l.long_usd > 0 && std::isfinite(l.long_usd)) { s.standing.push_back({l.price, l.long_usd}); s.side.push_back(-1); }
        if (l.short_usd > 0 && std::isfinite(l.short_usd)) { s.standing.push_back({l.price, l.short_usd}); s.side.push_back(1); }
    }
    return s;
}

// The fuel that can still be liquidated from `mark`. Scenario fuel has no side of its own: all of it.
// Observed and modelled fuel keep a long only below the mark and a short only above it; the rest is on
// the wrong side (price already traded through it, or the frame predates the move) and is counted in
// `dropped` rather than drawn as the other side's liquidations.
inline std::vector<touch_zones::Fuel> standing_at(const FuelSource& s, double mark, double* dropped = nullptr) {
    if (dropped) *dropped = 0;
    if (s.side.size() != s.standing.size()) return s.kind == FuelKind::Scenario ? s.standing : std::vector<touch_zones::Fuel>{};
    std::vector<touch_zones::Fuel> out;
    out.reserve(s.standing.size());
    for (size_t i = 0; i < s.standing.size(); ++i) {
        const auto& f = s.standing[i];
        if ((s.side[i] < 0 && f.price < mark) || (s.side[i] > 0 && f.price > mark)) out.push_back(f);
        else if (dropped) *dropped += f.mass;
    }
    return out;
}

// The rails caption: what the brightness is, naming the fuel, always "not a magnet". `extra` is appended
// (history and access notes, each starting with its own separator).
inline void caption(const FuelSource& s, int64_t clock, const char* extra, char* out, size_t n) {
    constexpr const char* dot = " \xc2\xb7 ";
    char fuel[112];
    if (s.kind == FuelKind::Observed) {
        char cov[24], age[24];
        if (s.coverage >= 0) std::snprintf(cov, sizeof(cov), "%.0f%% located", 100.0*std::min(1.0, s.coverage));
        else std::snprintf(cov, sizeof(cov), "coverage unknown");
        const int64_t a = clock > s.asof_ms ? (clock - s.asof_ms)/1000 : 0;
        if (a < 120) std::snprintf(age, sizeof(age), "%llds old", static_cast<long long>(a));
        else std::snprintf(age, sizeof(age), "%lldm old", static_cast<long long>(a/60));
        std::snprintf(fuel, sizeof(fuel), "Hyperliquid positions (sampled, %s, %s)", cov, age);
    } else if (s.kind == FuelKind::Modelled) {
        const int64_t a = clock > s.asof_ms ? (clock - s.asof_ms)/1000 : 0;
        std::snprintf(fuel, sizeof(fuel), "V2 modelled $ (%s, %llds old)", s.claim ? s.claim : "untested",
                      static_cast<long long>(a));
    } else if (s.fallback) {
        std::snprintf(fuel, sizeof(fuel), "scenario fuel (%s)", s.fallback);
    } else {
        std::snprintf(fuel, sizeof(fuel), "scenario fuel");
    }
    std::snprintf(out, n, "RAILS%sbrightness = touch chance x %s%snot a magnet%s", dot, fuel, dot, extra ? extra : "");
}

}  // namespace liq_rails
