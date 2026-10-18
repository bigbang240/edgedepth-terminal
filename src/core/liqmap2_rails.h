#pragma once
// V2 rails admin preview (hub task liq-map-v2, sprint item 3; backend proto/liqmap2.proto LiqMapV2Rails on stream 40,
// history LiqMapV2RailsHistory on stream 41). A rail is a fixed price picked from the modelled fuel. It carries three
// numbers calibrated against Bybit's liquidation record: the dollars forced out if price reaches its zone, the chance
// of reaching it within 24 h, and the liquidations expected within 24 h, which sets its colour. The numbers change at
// most hourly, so a rail's colour steps and never flickers. Admin only until the rails forward check passes
// (hub outputs/liq-map-v2-20261004/rails/PREREG_RAILS_FORWARD.md) and James releases them.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace liqmap2_rails {
inline constexpr size_t kMaxRails = 6000;                    // per market; the oldest ended rails go first
inline constexpr int64_t kKeepEndedMs = 7*24*3600000LL;      // the history the server serves
inline constexpr int64_t kHistoryMs = 24*3600000LL;          // one history request: a day (each reply stays within
                                                             // the server's record budget)
inline constexpr int kHistoryDays = 7;                       // asked for a day at a time while the view reaches back
inline constexpr size_t kMaxMarkets = 4;

struct Step {
    int64_t at_ms = 0;
    double usd = 0;          // dollars if price reaches the zone
    double chance = -1;      // within 24 h; -1 unknown (nearer than 0.5%, or a week of prices not yet in hand)
    double expected = -1;    // liquidations expected within 24 h; -1 unknown
    double zone_lo = 0, zone_hi = 0;
    // The same within 1 h and 4 h (hub rails/PREREG_RAILS_HORIZONS_FORWARD.md): expected = usd x that horizon's
    // chance; -1 unknown (also when an older publisher sent neither).
    double chance_1h = -1, expected_1h = -1, chance_4h = -1, expected_4h = -1;
};

struct Rail {
    int64_t id = 0;
    bool is_long = false;
    double price = 0;        // fixed for life
    int64_t born_ms = 0;
    int64_t end_ms = 0;      // 0 while live
    std::string end;         // touched | closed
    std::vector<Step> steps; // ascending, one per distinct step time
};

struct Latest {
    int64_t timestamp_ms = 0;
    double mark = 0;
    std::string status, config_id;
    bool chance_ready = false, provisional = false;
};

// The step in force at t: the latest at or before t, or the first when t precedes it.
inline const Step* step_at(const Rail& r, int64_t t) {
    if (r.steps.empty()) return nullptr;
    auto it = std::upper_bound(r.steps.begin(), r.steps.end(), t, [](int64_t v, const Step& s) { return v < s.at_ms; });
    return it == r.steps.begin() ? &r.steps.front() : &*std::prev(it);
}

inline void add_step(Rail& r, const Step& s) {
    auto it = std::lower_bound(r.steps.begin(), r.steps.end(), s.at_ms, [](const Step& a, int64_t v) { return a.at_ms < v; });
    if (it != r.steps.end() && it->at_ms == s.at_ms) *it = s;
    else r.steps.insert(it, s);
}

// What a viewer could know of a rail at a clock (hub task liq-map-v2: a replay shows the rails as published at the
// time). Every drawing reads a rail's start and end through these, so a clock in the past sees what was out then.
// The publisher (backend liqmap2.Rails) stamps a touch with the open of the 1-minute bar that reached the rail, but
// learns of it only when that bar closes and reports it in the next minute's message; a close is stamped with the
// update at a bar's close, so it is known when stamped. An end is therefore known from the close of the bar that made
// it: a touch one bar after its stamp, a close (or any other reason, conservatively a touch) as given. Until then the
// rail stands at the clock, and the chart's own candles end it the moment price reaches it. Live, an end arrives in
// the message after that bar's close (published 5 s past the minute), so it is already known when it lands.
// A rail is first published with its first step; after a publisher warm-up or outage its birth is earlier, so a line
// starts at the later of its birth and the first step held (the history read sends every step from the first). That
// is replay's rule. The live chart draws every published rail back toward its birth (backend
// docs/LIQMAP_VISUAL_RULE.md rule 3: a band starts when its positions opened; line_from, then clear_back past any candle
// that reached it before publication): the record began on 10 Oct and most BTC rails were born in September, so drawn
// from their first step every live line started at one hard edge (2026-10-17).
inline constexpr int64_t kBarMs = 60000;

// The clock from which the rail's end is known; 0 while it has none.
inline int64_t end_known_ms(const Rail& r) {
    return r.end_ms > 0 ? r.end_ms + (r.end == "closed" ? 0 : kBarMs) : 0;
}

// The rail's end as known at the clock asof (its stamp), or 0 while it still stands then.
inline int64_t end_at(const Rail& r, int64_t asof) {
    return r.end_ms > 0 && end_known_ms(r) <= asof ? r.end_ms : 0;
}

// Where the rail's line starts: its birth, or its first step held when that is later.
inline int64_t start_ms(const Rail& r) {
    return r.steps.empty() ? r.born_ms : std::max(r.born_ms, r.steps.front().at_ms);
}

// Published by the clock asof and not known to have ended by then.
inline bool standing(const Rail& r, int64_t asof) {
    return !r.steps.empty() && start_ms(r) <= asof && end_at(r, asof) == 0;
}

// Where the drawn line starts at the clock asof: from_birth (the live chart), once published by then, its birth;
// otherwise start_ms.
inline int64_t line_from(const Rail& r, int64_t asof, bool from_birth) {
    return from_birth && start_ms(r) <= asof ? r.born_ms : start_ms(r);
}

// What the overlay draws (rendering/liqmap2_rails_overlay.h), in time and price so it can be tested without a renderer.
inline constexpr int kLevels = 8;  // colour steps

// What sets a rail's colour (backend docs/LIQMAP_VISUAL_RULE.md, colour meaning): the liquidations expected within
// the chosen horizon (1 h, 4 h or 24 h; expected is 24 h), or by toggle the dollars if price reaches it. While any
// rail in view has no figure for the horizon, the overlay colours every rail by its dollars, so one unknown never
// paints the view in the dimmest step.
enum class Colour { expected_1h, expected_4h, expected, dollars };

// The value that sets a step's colour; -1 while the figure is unknown.
inline double value(const Step& s, Colour c = Colour::expected) {
    const double v = c == Colour::dollars ? s.usd : c == Colour::expected_1h ? s.expected_1h
                   : c == Colour::expected_4h ? s.expected_4h : s.expected;
    return v >= 0 ? v : -1;
}

// Colour step 0..kLevels-1 on the scale of the rails in view (vmax), -1 when the chance is unknown.
inline int level(double v, double vmax) {
    if (!(v >= 0) || !(vmax > 0)) return -1;
    return std::min(kLevels - 1, static_cast<int>(kLevels*std::pow(std::min(1.0, v/vmax), 0.6)));
}

struct Segment {
    int64_t t0, t1;   // a rail at one step: from its start or the step to the next step, its end or the clock
    double price;
    double v;         // value() of the step
    const Rail* rail;
    const Step* step;
    int source = 0;   // which venue's store it came from (the overlay draws the chart's venue and its sibling)
};

// Every segment within [t_lo, t_hi] x [p_lo, p_hi] at the clock asof, and the largest value among them. A rail runs
// from its start (line_from; before its first step it carries that step's figures) to the bar that ended it once that
// end is known (end_at); nothing is drawn after the clock.
inline std::vector<Segment> segments(const std::map<std::pair<std::string, int64_t>, Rail>& rails, int64_t asof,
                                     double t_lo, double t_hi, double p_lo, double p_hi, double& vmax,
                                     Colour colour = Colour::expected, bool from_birth = false) {
    std::vector<Segment> out;
    vmax = 0;
    for (const auto& [key, r] : rails) {
        if (r.steps.empty() || r.price < p_lo || r.price > p_hi) continue;
        const int64_t from = line_from(r, asof, from_birth), end = end_at(r, asof);
        const int64_t until = end > 0 ? end : asof;
        if (from > t_hi || until < t_lo || until <= from) continue;
        for (size_t i = 0; i < r.steps.size(); ++i) {
            const int64_t a = i == 0 ? from : std::max(from, r.steps[i].at_ms);
            const int64_t b = i + 1 < r.steps.size() ? std::min(until, r.steps[i + 1].at_ms) : until;
            if (b <= a || b < t_lo || a > t_hi) continue;
            out.push_back({a, b, r.price, value(r.steps[i], colour), &r, &r.steps[i]});
            vmax = std::max(vmax, out.back().v);
        }
    }
    // Brighter rails last, so a dim neighbour never covers them.
    std::stable_sort(out.begin(), out.end(), [](const Segment& a, const Segment& b) { return a.v < b.v; });
    return out;
}

// The live rails in [p_lo, p_hi] at the clock, largest first by the colour's value, with that value known.
inline std::vector<const Rail*> labelled(const std::map<std::pair<std::string, int64_t>, Rail>& rails, int64_t asof,
                                         double p_lo, double p_hi, Colour colour = Colour::expected) {
    std::vector<const Rail*> out;
    for (const auto& [key, r] : rails)
        if (standing(r, asof) && r.price >= p_lo && r.price <= p_hi && value(*step_at(r, asof), colour) > 0)
            out.push_back(&r);
    std::stable_sort(out.begin(), out.end(), [&](const Rail* a, const Rail* b) {
        return value(*step_at(*a, asof), colour) > value(*step_at(*b, asof), colour);
    });
    return out;
}

// The colour the view uses: the viewer's choice, except an expected figure falls back to dollars while any segment
// in view has none for that horizon (unknown chance), so the scale never mixes known and unknown rails.
inline Colour view_colour(const std::vector<Segment>& chosen_segments, Colour chosen) {
    if (chosen == Colour::dollars) return Colour::dollars;
    for (const auto& s : chosen_segments)
        if (s.v < 0) return Colour::dollars;
    return chosen;
}

// Ladders (James 2026-10-10: "merge the ladders into one rail"; backend docs/LIQMAP_VISUAL_RULE.md rejects an evenly
// spaced ladder across a smooth region). Over each stretch of time with the same live rails, one side's rails are
// sorted by price and a chain is a sequence whose zones touch or sit within kLadderGap of each other. The publisher
// tiles a side's fuel into zones end to end, so one chain often holds every rail on that side (IMX 2026-10-10: 18
// long rails over 9% of price); drawing a chain as one line left a mover with one rail a side. A chain therefore
// splits into hills of dollars: a member is a peak when its dollars stand kProminence above the lowest member between
// it and any larger one (topographic prominence; the chain's largest is always a peak), and each member joins the
// peak on its side of the lowest member between two peaks. A smooth ladder is one hill and draws as one rail; distinct
// piles of positions stay separate rails. A hill draws once: at the price of the member with the most dollars when it
// forms, kept while that member stays in the hill (so the line neither moves nor steps as candles eat into it; a
// member that drew last stretch needs only kProminenceKeep to stay a peak, so hourly steps do not flicker the split),
// valued at the sum of its members, which keeps the dollars. Display only: the record, the scored rails and the
// hover's per-rail numbers are unchanged.
inline constexpr double kLadderGap = 1.0031;       // up to three 10bp grid rows between one zone and the next
inline constexpr double kProminence = 1.25;        // a new peak: 25% more dollars than the col to a larger member
inline constexpr double kProminenceKeep = 1.10;    // a peak that already draws keeps its line down to 10%

// Spacing (James 2026-10-17, AGT's pump zoomed out on 1 h: "too many purple bands"). Prominence is in dollars only, so
// a long chain with uneven dollars keeps a peak every few members (AGT 16 Oct: about thirty over 0.030-0.047), and
// zoomed out those lines sit a pixel or two apart and stack into slabs. With min_gap > 0 (a price distance; the overlay
// passes the price of a few rows at the chart's zoom) a peak nearer than min_gap to a stronger one gives way, strongest
// first, and its hill joins the nearer surviving hill on either side, so hills stay contiguous and keep their dollars.
// Strength is the peak's dollars, kProminence times them for a member that drew last stretch, so near-equal neighbours
// do not trade places as their figures step. Zoomed in, min_gap shrinks and the piles separate again.
// Scratch for hills and space_hills, kept by a caller that splits many chains (runs) so a split allocates nothing.
struct HillScratch {
    std::vector<double> left_col, right_col;
    std::vector<char> left_higher, right_higher;
    std::vector<std::pair<size_t, double>> waiting;
    std::vector<size_t> peaks, order, held;
    std::vector<int> to;
};

inline void space_hills(std::vector<int>& out, const std::vector<size_t>& peaks, const std::vector<double>& u,
                        const std::vector<bool>& kept, const std::vector<double>& price, double min_gap, HillScratch& sc) {
    const size_t np = peaks.size();
    if (!(min_gap > 0) || np < 2 || price.size() != u.size()) return;
    // Prices ascend, so with no two neighbouring peaks nearer than the gap, none are.
    bool close = false;
    for (size_t q = 1; q < np && !close; ++q) close = price[peaks[q]] - price[peaks[q - 1]] < min_gap;
    if (!close) return;
    const auto strength = [&](size_t q) { return u[peaks[q]]*(peaks[q] < kept.size() && kept[peaks[q]] ? kProminence : 1.0); };
    auto& order = sc.order;
    auto& held = sc.held;
    order.resize(np);
    held.clear();
    for (size_t q = 0; q < np; ++q) order[q] = q;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return strength(a) > strength(b); });
    held.reserve(np);
    for (size_t q : order) {
        const auto it = std::lower_bound(held.begin(), held.end(), q);
        const bool near = (it != held.end() && price[peaks[*it]] - price[peaks[q]] < min_gap) ||
                          (it != held.begin() && price[peaks[q]] - price[peaks[*std::prev(it)]] < min_gap);
        if (!near) held.insert(it, q);
    }
    if (held.size() == np) return;
    auto& to = sc.to;
    to.resize(np);
    for (size_t q = 0, h = 0; q < np; ++q) {
        while (h + 1 < held.size() && held[h + 1] <= q) ++h;
        size_t k = h;
        if (held[h] < q && h + 1 < held.size() &&
            price[peaks[held[h + 1]]] - price[peaks[q]] < price[peaks[q]] - price[peaks[held[h]]])
            k = h + 1;
        to[q] = static_cast<int>(k);
    }
    for (auto& x : out) x = to[static_cast<size_t>(x)];
}
inline void space_hills(std::vector<int>& out, const std::vector<size_t>& peaks, const std::vector<double>& u,
                        const std::vector<bool>& kept, const std::vector<double>& price, double min_gap) {
    HillScratch sc;
    space_hills(out, peaks, u, kept, price, min_gap, sc);
}

// Hills of one chain: for dollars u (ascending price), the index of the hill each member joins, numbered from 0.
// kept[i]: member i drew a line in the previous stretch. price, min_gap: the members' prices and the spacing above
// (none while min_gap is 0).
inline void hills(std::vector<int>& out, HillScratch& sc, const std::vector<double>& u, const std::vector<bool>& kept,
                  const std::vector<double>& price = {}, double min_gap = 0) {
    const size_t n = u.size();
    // Walk out to the first larger member each way (equal counts as larger to the right, so a plateau has one peak),
    // noting the lowest member passed; a side with no larger member does not bound the prominence. One pass each way:
    // a stack of the members still waiting for a larger one, each holding the lowest dollars between it and the
    // member under it, so a member's col is the lowest of what it pops (linear, where walking was quadratic on the
    // long chains of a mover).
    auto& left_col = sc.left_col;
    auto& right_col = sc.right_col;
    auto& left_higher = sc.left_higher;
    auto& right_higher = sc.right_higher;
    auto& waiting = sc.waiting;
    left_col.resize(n);
    right_col.resize(n);
    left_higher.resize(n);
    right_higher.resize(n);
    waiting.clear();
    for (size_t i = 0; i < n; ++i) {
        double low = u[i];
        while (!waiting.empty() && !(u[waiting.back().first] > u[i])) {
            low = std::min(low, waiting.back().second);
            waiting.pop_back();
        }
        left_higher[i] = !waiting.empty();
        left_col[i] = low;
        waiting.push_back({i, low});
    }
    waiting.clear();
    for (size_t i = n; i-- > 0;) {
        double low = u[i];
        while (!waiting.empty() && !(u[waiting.back().first] >= u[i])) {
            low = std::min(low, waiting.back().second);
            waiting.pop_back();
        }
        right_higher[i] = !waiting.empty();
        right_col[i] = low;
        waiting.push_back({i, low});
    }
    auto& peaks = sc.peaks;
    peaks.clear();
    for (size_t i = 0; i < n; ++i) {
        const double col = left_higher[i] && right_higher[i] ? std::max(left_col[i], right_col[i])
                         : left_higher[i] ? left_col[i] : right_higher[i] ? right_col[i] : -1;
        const double need = i < kept.size() && kept[i] ? kProminenceKeep : kProminence;
        if (col < 0 || (u[i] > 0 && u[i] >= need*std::max(col, 1e-12))) peaks.push_back(i);
    }
    out.assign(n, 0);
    if (peaks.empty()) return;
    size_t hill = 0, valley = 0, valley_of = static_cast<size_t>(-1);
    for (size_t i = 0; i < n; ++i) {
        if (hill + 1 < peaks.size() && i > peaks[hill]) {
            // Between two peaks: the lowest member (the first, on a tie) goes with the nearer peak.
            const size_t a = peaks[hill], b = peaks[hill + 1];
            if (valley_of != hill) {
                valley = a + 1;
                for (size_t k = a + 1; k < b; ++k)
                    if (u[k] < u[valley]) valley = k;
                valley_of = hill;
            }
            if (i > valley || (i == valley && valley - a > b - valley) || i == b) ++hill;
        }
        out[i] = static_cast<int>(hill);
    }
    space_hills(out, peaks, u, kept, price, min_gap, sc);
}
inline std::vector<int> hills(const std::vector<double>& u, const std::vector<bool>& kept,
                              const std::vector<double>& price = {}, double min_gap = 0) {
    HillScratch sc;
    std::vector<int> out;
    hills(out, sc, u, kept, price, min_gap);
    return out;
}

struct Run {
    int64_t t0 = 0, t1 = 0;
    double price = 0;          // the representative member's price
    double v = 0;              // the sum of the members' values; -1 if any member's is unknown
    const Rail* rep = nullptr; // the representative member
    int members = 1;
    double lo = 0, hi = 0;     // the run's zone extent
    double near = 0;           // the member price nearest the mark side (lowest short, highest long)
    double usd = 0;            // the members' dollars if reached, summed
    double e1 = 0, e4 = 0, e24 = 0;  // the members' expected figures, summed; -1 if any is unknown
    int source = 0;
};

// The sweep below visits every stretch between consecutive segment ends: a wide view (days of hourly steps on both
// venues) holds thousands, each with a hundred or more live rails. Most stretches only move one rail to its next
// step, so it splits only the chains a change reached: a rail starting or ending, its neighbours (a chain can join or
// part there), and a chain whose representatives changed last stretch (its stickiness, and so its split, can change
// once more). Every other chain would split as before, so its runs simply carry on. Same runs, in the same order, as
// splitting every chain afresh each stretch; within a side, rails at one price keep a fixed order (by rail).
// min_gap: the peaks' spacing in price (space_hills; 0 for none); half of it is the widest gap between zones a chain
// spans.
inline std::vector<Run> runs(const std::vector<Segment>& segs, double min_gap = 0) {
    std::vector<Run> out;
    const size_t n = segs.size();
    if (n == 0) return out;
    std::vector<int64_t> ts;
    ts.reserve(n*2);
    for (const auto& sg : segs) { ts.push_back(sg.t0); ts.push_back(sg.t1); }
    std::sort(ts.begin(), ts.end());
    ts.erase(std::unique(ts.begin(), ts.end()), ts.end());
    constexpr size_t kNone = static_cast<size_t>(-1);
    // Each rail as a dense number; a rail has one segment live at a time (its steps follow each other).
    std::unordered_map<const Rail*, size_t> ids;
    ids.reserve(n);
    std::vector<size_t> rail_of(n);
    for (size_t i = 0; i < n; ++i) rail_of[i] = ids.try_emplace(segs[i].rail, ids.size()).first->second;
    std::vector<size_t> starts(n), ends(n);
    for (size_t i = 0; i < n; ++i) starts[i] = ends[i] = i;
    std::sort(starts.begin(), starts.end(),
              [&](size_t a, size_t b) { return segs[a].t0 != segs[b].t0 ? segs[a].t0 < segs[b].t0 : a < b; });
    std::sort(ends.begin(), ends.end(),
              [&](size_t a, size_t b) { return segs[a].t1 != segs[b].t1 ? segs[a].t1 < segs[b].t1 : a < b; });
    const size_t rails = ids.size();
    std::vector<size_t> live(rails, kNone);    // the rail's live segment
    std::vector<size_t> run_of(rails, kNone);  // the run the rail represents, while it represents one
    std::vector<int64_t> rep_at(rails, -1);    // the stretch in which the rail last became or stayed a representative
    std::vector<int64_t> seen_at(rails, -1);   // the stretch in which the rail was last queued to split
    // Each side's live segments by price, then rail: [0] long, [1] short.
    std::vector<size_t> side[2];
    const auto before = [&](size_t x, size_t y) {
        return segs[x].price != segs[y].price ? segs[x].price < segs[y].price : rail_of[x] < rail_of[y];
    };
    const auto side_of = [&](size_t s) { return segs[s].rail->is_long ? 0 : 1; };
    const auto at = [&](size_t s) {
        auto& v = side[side_of(s)];
        return static_cast<size_t>(std::lower_bound(v.begin(), v.end(), s, before) - v.begin());
    };
    // Member p of a side joins member p - 1 in a chain (zones that touch, or nearer than half min_gap: at this zoom such
    // a gap is about a bar's height, and separate chains there stacked as one slab).
    const auto joined = [&](const std::vector<size_t>& v, size_t p) {
        const double lo = segs[v[p]].step->zone_lo, hi = segs[v[p - 1]].step->zone_hi;
        return lo <= hi*kLadderGap || lo - hi < 0.5*min_gap;
    };
    std::vector<size_t> seeds, again, gone;
    const auto seed = [&](size_t s, ptrdiff_t offset) {
        const auto& v = side[side_of(s)];
        const ptrdiff_t p = static_cast<ptrdiff_t>(at(s)) + offset;
        if (p >= 0 && p < static_cast<ptrdiff_t>(v.size())) seeds.push_back(rail_of[v[static_cast<size_t>(p)]]);
    };
    struct Chain { int side; size_t c0, c1, kept; };
    std::vector<Chain> chains;
    std::vector<char> kept;
    std::vector<double> u, at_price;
    std::vector<bool> kept_bits;
    HillScratch scratch;
    std::vector<int> h;
    const auto sum = [](double acc, double x) { return acc < 0 || x < 0 ? -1.0 : acc + x; };
    size_t next_start = 0, next_end = 0;
    for (size_t k = 0; k + 1 < ts.size(); ++k) {
        const int64_t a = ts[k], b = ts[k + 1];
        const int64_t stretch = static_cast<int64_t>(k);
        seeds.swap(again);  // chains whose representatives changed last stretch split once more
        again.clear();
        gone.clear();
        // Segments ending here leave their side; their neighbours' chains may part.
        for (; next_end < n && segs[ends[next_end]].t1 <= a; ++next_end) {
            const size_t s = ends[next_end];
            if (live[rail_of[s]] != s) continue;
            auto& v = side[side_of(s)];
            const size_t p = at(s);
            v.erase(v.begin() + static_cast<ptrdiff_t>(p));
            live[rail_of[s]] = kNone;
            if (p > 0) seeds.push_back(rail_of[v[p - 1]]);
            if (p < v.size()) seeds.push_back(rail_of[v[p]]);
            gone.push_back(rail_of[s]);
        }
        // Segments starting here join theirs (a rail's next step leaves and rejoins at its own price).
        for (; next_start < n && segs[starts[next_start]].t0 <= a; ++next_start) {
            const size_t s = starts[next_start];
            if (segs[s].t1 <= a) continue;
            auto& v = side[side_of(s)];
            v.insert(v.begin() + static_cast<ptrdiff_t>(at(s)), s);
            live[rail_of[s]] = s;
            seed(s, -1);
            seed(s, 0);
            seed(s, 1);
        }
        // The chains the seeds reach, each once, by side and price; the stickiness each member brings.
        chains.clear();
        kept.clear();
        for (size_t id : seeds)
            if (live[id] != kNone && seen_at[id] != stretch) {
                seen_at[id] = stretch;
                const size_t s = live[id];
                const auto& v = side[side_of(s)];
                size_t c0 = at(s), c1 = c0 + 1;
                while (c0 > 0 && joined(v, c0)) --c0;
                while (c1 < v.size() && joined(v, c1)) ++c1;
                chains.push_back({side_of(s), c0, c1, 0});
            }
        std::sort(chains.begin(), chains.end(), [](const Chain& x, const Chain& y) {
            return x.side != y.side ? x.side < y.side : x.c0 < y.c0;
        });
        chains.erase(std::unique(chains.begin(), chains.end(), [](const Chain& x, const Chain& y) {
            return x.side == y.side && x.c0 == y.c0;
        }), chains.end());
        for (auto& c : chains) {
            c.kept = kept.size();
            for (size_t m = c.c0; m < c.c1; ++m) {
                const size_t id = rail_of[side[c.side][m]];
                seen_at[id] = stretch;
                kept.push_back(run_of[id] != kNone);
            }
        }
        // Split each chain into hills, then each hill into one run: a sticky representative, else the member with the
        // most dollars. A representative's run carries on while its figures hold.
        for (const auto& c : chains) {
            const auto& v = side[c.side];
            const bool is_long = c.side == 0;
            u.clear();
            at_price.clear();
            kept_bits.clear();
            for (size_t m = c.c0; m < c.c1; ++m) {
                u.push_back(segs[v[m]].step->usd);
                at_price.push_back(segs[v[m]].price);
                kept_bits.push_back(kept[c.kept + m - c.c0] != 0);
            }
            hills(h, scratch, u, kept_bits, at_price, min_gap);
            bool changed = false;
            for (size_t i = c.c0; i < c.c1;) {
                size_t j = i + 1;
                while (j < c.c1 && h[j - c.c0] == h[i - c.c0]) ++j;
                size_t rep = kNone;
                for (size_t m = i; m < j; ++m)
                    if (kept[c.kept + m - c.c0] && (rep == kNone || segs[v[m]].step->usd > segs[v[rep]].step->usd))
                        rep = m;
                if (rep == kNone)
                    for (size_t m = i; m < j; ++m)
                        if (rep == kNone || segs[v[m]].step->usd > segs[v[rep]].step->usd) rep = m;
                const Segment& rs = segs[v[rep]];
                Run r;
                r.t0 = a; r.t1 = b; r.price = rs.price; r.rep = rs.rail; r.members = static_cast<int>(j - i);
                r.lo = segs[v[i]].step->zone_lo; r.hi = segs[v[j - 1]].step->zone_hi;
                r.near = is_long ? segs[v[j - 1]].price : segs[v[i]].price;
                for (size_t m = i; m < j; ++m) {
                    const Step& st = *segs[v[m]].step;
                    r.v = sum(r.v, segs[v[m]].v);
                    r.usd += st.usd;
                    r.e1 = sum(r.e1, st.expected_1h);
                    r.e4 = sum(r.e4, st.expected_4h);
                    r.e24 = sum(r.e24, st.expected);
                }
                const size_t id = rail_of[v[rep]];
                size_t& open = run_of[id];
                if (open == kNone || out[open].v != r.v || out[open].members != r.members || out[open].lo != r.lo ||
                    out[open].hi != r.hi) {
                    if (open != kNone) out[open].t1 = a;
                    out.push_back(r);
                    open = out.size() - 1;
                }
                rep_at[id] = stretch;
                changed = changed || !kept[c.kept + rep - c.c0];
                i = j;
            }
            // Representatives that no longer draw end here.
            for (size_t m = c.c0; m < c.c1; ++m) {
                const size_t id = rail_of[v[m]];
                if (kept[c.kept + m - c.c0] && rep_at[id] != stretch) {
                    out[run_of[id]].t1 = a;
                    run_of[id] = kNone;
                    changed = true;
                }
            }
            if (changed) again.push_back(rail_of[v[c.c0]]);
        }
        // So do those of rails that ended.
        for (size_t id : gone)
            if (live[id] == kNone && run_of[id] != kNone) {
                out[run_of[id]].t1 = a;
                run_of[id] = kNone;
            }
    }
    for (size_t id = 0; id < rails; ++id)
        if (run_of[id] != kNone) out[run_of[id]].t1 = ts.back();
    std::stable_sort(out.begin(), out.end(), [](const Run& x, const Run& y) { return x.v < y.v; });
    return out;
}

// Candles consume rails (backend liqmap2.Rails.Touch; docs/LIQMAP_VISUAL_RULE.md rule 2): a long rail ends at the
// first candle whose low reaches its price, a short rail at the first whose high does. The publisher applies this to
// closed bars and reports it in the next minute's message; the chart also applies it at once to every candle it
// holds, the forming one included, so no rail is ever drawn through a candle. t, lo, hi: the chart's candles (open
// time ms, ascending). Returns the open of the first candle opening at or after from_ms (and before until_ms) that
// touches price, or INT64_MAX.
// slack: the price half-height of the drawn bar, so a candle touching any part of the bar's row consumes it
// (rule 2: "touches any part of its row").
inline int64_t consumed_at(const std::vector<double>& t, const std::vector<double>& lo, const std::vector<double>& hi,
                           bool is_long, double price, int64_t from_ms, int64_t until_ms, double slack = 0) {
    const size_t n = std::min({t.size(), lo.size(), hi.size()});
    auto it = std::lower_bound(t.begin(), t.begin() + n, static_cast<double>(from_ms));
    for (size_t i = static_cast<size_t>(it - t.begin()); i < n && t[i] < static_cast<double>(until_ms); ++i)
        if (is_long ? lo[i] <= price + slack : hi[i] >= price - slack) return static_cast<int64_t>(t[i]);
    return INT64_MAX;
}

// Where a line may start without crossing the candle it starts inside (James 2026-10-11, STRK 1 D and 4 h). On 4 h
// or 1 D positions open mid-candle, and that candle's range often holds their price because price passed it before
// they opened (the publisher ends a rail only on a touch after its birth). Drawn from its birth the line would cross
// the candle, so it starts just past the candle's body instead (bodies span the open +/- kBodyHalf of the timeframe,
// candles sit centred on their open, so the body's right edge is at open + (0.5 + kBodyHalf) x timeframe in real
// time). A line starting at an open (1 m) or clear of the candle's range starts at its birth. t, lo, hi: the chart's
// candles; tf_ms their timeframe; slack as consumed_at.
inline constexpr double kBodyHalf = 0.35;   // chart_widget: candle bodies span +/- 0.35 of the timeframe
inline constexpr double kPastBody = 0.5 + kBodyHalf + 0.05;  // a sliver of gap after the body
inline int64_t clear_start(const std::vector<double>& t, const std::vector<double>& lo, const std::vector<double>& hi,
                           double price, int64_t from_ms, int64_t tf_ms, double slack = 0) {
    const size_t n = std::min({t.size(), lo.size(), hi.size()});
    auto it = std::upper_bound(t.begin(), t.begin() + n, static_cast<double>(from_ms));
    if (it == t.begin() || tf_ms <= 0) return from_ms;
    const size_t k = static_cast<size_t>(it - t.begin()) - 1;
    const int64_t open = static_cast<int64_t>(t[k]);
    if (from_ms == open || from_ms >= open + tf_ms) return from_ms;
    if (price < lo[k] - slack || price > hi[k] + slack) return from_ms;
    return std::max(from_ms, open + static_cast<int64_t>(kPastBody*static_cast<double>(tf_ms)));
}

// Live, a line is drawn back from its publication toward its birth (line_from). The birth is the publisher's estimate
// and precedes publication (about 30 minutes in steady state, weeks after a warm-up), and price may reach the rail's row
// in between without ending it: drawn from its birth, the chart's candles would consume the line there and a standing
// rail would vanish (AGT 17 Oct, the short at 0.0956 published after the 04:00 high). So the line reaches back from
// publication only to just past the last candle before it that touches the row (as consumed_at reads a touch), never
// before from_ms. t, lo, hi: the chart's candles; tf_ms their timeframe; slack as consumed_at.
inline int64_t clear_back(const std::vector<double>& t, const std::vector<double>& lo, const std::vector<double>& hi,
                          bool is_long, double price, int64_t from_ms, int64_t published_ms, int64_t tf_ms,
                          double slack = 0) {
    const size_t n = std::min({t.size(), lo.size(), hi.size()});
    if (published_ms <= from_ms) return from_ms;
    const auto it = std::lower_bound(t.begin(), t.begin() + n, static_cast<double>(published_ms));
    for (size_t i = static_cast<size_t>(it - t.begin()); i-- > 0;) {
        if (t[i] + static_cast<double>(std::max<int64_t>(tf_ms, 0)) <= static_cast<double>(from_ms)) break;
        if (is_long ? lo[i] <= price + slack : hi[i] >= price - slack)
            return std::max(from_ms, static_cast<int64_t>(t[i]) + (tf_ms > 0 ? static_cast<int64_t>(kPastBody*static_cast<double>(tf_ms)) : 1));
    }
    return from_ms;
}

// The candles that consume rails, as consumed_at and clear_start read them: the chart's closed candles and, last, the
// one still forming (rule 2: no rail is drawn across a candle the chart draws). The chart holds the forming candle
// apart from its closed ones, so without it a rail the live candle trades through ran on across it until the candle
// closed, and live lines stopped at the clock rather than at the end of the forming candle's slot. A forming candle at
// the last closed candle's open joins it; one before it, or with no range yet, is left out. Only the tail is redone
// when only the forming candle moves. revision() changes with every change and is never reused by another instance.
class ChartCandles {
public:
    struct Forming {
        bool present = false;
        int64_t open_ms = 0;
        double low = 0, high = 0;
        bool operator==(const Forming&) const = default;
    };

    // The closed candles (open time ms ascending, lows, highs; closed_revision changes whenever they do) and the
    // forming one. Returns whether the candles changed.
    bool update(const std::vector<double>& t, const std::vector<double>& lo, const std::vector<double>& hi,
                uint64_t closed_revision, const Forming& forming) {
        const size_t n = std::min({t.size(), lo.size(), hi.size()});
        const bool closed_same = source_ == &t && closed_revision_ == closed_revision && closed_ == n;
        if (closed_same && forming_ == forming) return false;
        if (!closed_same) {
            t_.assign(t.begin(), t.begin() + static_cast<ptrdiff_t>(n));
            lo_.assign(lo.begin(), lo.begin() + static_cast<ptrdiff_t>(n));
            hi_.assign(hi.begin(), hi.begin() + static_cast<ptrdiff_t>(n));
            source_ = &t;
            closed_revision_ = closed_revision;
            closed_ = n;
        } else {
            // The closed part stands; undo the previous forming candle (appended, or folded into the last one).
            t_.resize(n);
            lo_.resize(n);
            hi_.resize(n);
            if (n > 0) { lo_[n - 1] = lo[n - 1]; hi_[n - 1] = hi[n - 1]; }
        }
        forming_ = forming;
        const double open = static_cast<double>(forming.open_ms);
        if (forming.present && forming.low > 0 && forming.high >= forming.low && std::isfinite(forming.high)) {
            if (n == 0 || open > t_[n - 1]) {
                t_.push_back(open);
                lo_.push_back(forming.low);
                hi_.push_back(forming.high);
            } else if (open == t_[n - 1]) {
                lo_[n - 1] = std::min(lo_[n - 1], forming.low);
                hi_[n - 1] = std::max(hi_[n - 1], forming.high);
            }
        }
        revision_ = ++revisions_;
        return true;
    }

    const std::vector<double>& t() const { return t_; }
    const std::vector<double>& lo() const { return lo_; }
    const std::vector<double>& hi() const { return hi_; }
    uint64_t revision() const { return revision_; }

private:
    std::vector<double> t_, lo_, hi_;
    const void* source_ = nullptr;
    uint64_t closed_revision_ = 0;
    size_t closed_ = 0;
    Forming forming_;
    uint64_t revision_ = 0;
    inline static uint64_t revisions_ = 0;  // main thread only
};

class Store {
public:
    using Key = std::pair<std::string, int64_t>;  // config id, rail id

    // A live message: each rail it carries with its current step, and the rails that ended since the previous one.
    void apply_live(const Latest& latest, const std::string& config_id, const std::vector<Rail>& rails,
                    const std::vector<Rail>& ended) {
        if (latest.timestamp_ms < latest_.timestamp_ms) return;  // late duplicate of an older message
        latest_ = latest;
        for (const auto& r : rails) merge(config_id, r);
        for (const auto& e : ended) {
            auto it = rails_.find({config_id, e.id});
            if (it == rails_.end()) continue;
            it->second.end_ms = e.end_ms;
            it->second.end = e.end;
        }
        bound();
        revision_ = ++revisions_;
    }

    // A stored record (history): the rail, every stored step, its end.
    void apply_record(const std::string& config_id, const Rail& r) {
        merge(config_id, r);
        bound();
        revision_ = ++revisions_;
    }

    const std::map<Key, Rail>& rails() const { return rails_; }
    const Latest& latest() const { return latest_; }
    // Changes with every change to the store, and is never reused by another store (a market's store can be dropped
    // and a new one made at the same address), so a store and its revision name one state.
    uint64_t revision() const { return revision_; }

private:
    void merge(const std::string& config_id, const Rail& r) {
        auto [it, fresh] = rails_.try_emplace({config_id, r.id});
        Rail& x = it->second;
        if (fresh) {
            x.id = r.id;
            x.is_long = r.is_long;
            x.price = r.price;
            x.born_ms = r.born_ms;
        }
        if (r.end_ms > 0) {
            x.end_ms = r.end_ms;
            x.end = r.end;
        }
        for (const auto& s : r.steps) add_step(x, s);
    }
    // Drop rails ended a week before the newest message, then the oldest ended ones beyond the cap.
    void bound() {
        const int64_t cut = latest_.timestamp_ms - kKeepEndedMs;
        for (auto it = rails_.begin(); it != rails_.end();)
            it = it->second.end_ms > 0 && it->second.end_ms < cut ? rails_.erase(it) : std::next(it);
        while (rails_.size() > kMaxRails) {
            auto victim = rails_.end();
            for (auto it = rails_.begin(); it != rails_.end(); ++it)
                if (it->second.end_ms > 0 && (victim == rails_.end() || it->second.end_ms < victim->second.end_ms)) victim = it;
            if (victim == rails_.end()) break;  // every rail is live: keep them
            rails_.erase(victim);
        }
    }
    std::map<Key, Rail> rails_;
    Latest latest_;
    uint64_t revision_ = 0;
    inline static uint64_t revisions_ = 0;  // main thread only, as every store
};
}  // namespace liqmap2_rails
