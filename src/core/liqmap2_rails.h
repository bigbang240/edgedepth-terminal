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
    int64_t t0, t1;   // a rail at one step: from its birth or the step to the next step, its end or the clock
    double price;
    double v;         // value() of the step
    const Rail* rail;
    const Step* step;
    int source = 0;   // which venue's store it came from (the overlay draws the chart's venue and its sibling)
};

// Every segment within [t_lo, t_hi] x [p_lo, p_hi] at the clock asof, and the largest value among them. A rail ends
// at the bar that ended it; nothing is drawn after the clock.
inline std::vector<Segment> segments(const std::map<std::pair<std::string, int64_t>, Rail>& rails, int64_t asof,
                                     double t_lo, double t_hi, double p_lo, double p_hi, double& vmax,
                                     Colour colour = Colour::expected) {
    std::vector<Segment> out;
    vmax = 0;
    for (const auto& [key, r] : rails) {
        if (r.steps.empty() || r.price < p_lo || r.price > p_hi) continue;
        const int64_t until = r.end_ms > 0 ? std::min(r.end_ms, asof) : asof;
        if (r.born_ms > t_hi || until < t_lo || until <= r.born_ms) continue;
        for (size_t i = 0; i < r.steps.size(); ++i) {
            const int64_t a = i == 0 ? r.born_ms : std::max(r.born_ms, r.steps[i].at_ms);
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
        if (r.end_ms == 0 && !r.steps.empty() && r.price >= p_lo && r.price <= p_hi && r.born_ms <= asof &&
            value(*step_at(r, asof), colour) > 0)
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

// Hills of one chain: for dollars u (ascending price), the index of the hill each member joins, numbered from 0.
// kept[i]: member i drew a line in the previous stretch.
inline std::vector<int> hills(const std::vector<double>& u, const std::vector<bool>& kept) {
    const size_t n = u.size();
    std::vector<size_t> peaks;
    for (size_t i = 0; i < n; ++i) {
        // Walk out to the first larger member each way (equal counts as larger to the right, so a plateau has one
        // peak), noting the lowest member passed; a side with no larger member does not bound the prominence.
        double left_col = u[i], right_col = u[i];
        bool left_higher = false, right_higher = false;
        for (size_t k = i; k-- > 0;) {
            if (u[k] > u[i]) { left_higher = true; break; }
            left_col = std::min(left_col, u[k]);
        }
        for (size_t k = i + 1; k < n; ++k) {
            if (u[k] >= u[i]) { right_higher = true; break; }
            right_col = std::min(right_col, u[k]);
        }
        const double col = left_higher && right_higher ? std::max(left_col, right_col)
                         : left_higher ? left_col : right_higher ? right_col : -1;
        const double need = i < kept.size() && kept[i] ? kProminenceKeep : kProminence;
        if (col < 0 || (u[i] > 0 && u[i] >= need*std::max(col, 1e-12))) peaks.push_back(i);
    }
    std::vector<int> out(n, 0);
    if (peaks.empty()) return out;
    size_t hill = 0;
    for (size_t i = 0; i < n; ++i) {
        if (hill + 1 < peaks.size() && i > peaks[hill]) {
            // Between two peaks: the lowest member (the first, on a tie) goes with the nearer peak.
            const size_t a = peaks[hill], b = peaks[hill + 1];
            size_t valley = a + 1;
            for (size_t k = a + 1; k < b; ++k)
                if (u[k] < u[valley]) valley = k;
            if (i > valley || (i == valley && valley - a > b - valley) || i == b) ++hill;
        }
        out[i] = static_cast<int>(hill);
    }
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

inline std::vector<Run> runs(const std::vector<Segment>& segs) {
    std::vector<int64_t> ts;
    ts.reserve(segs.size()*2);
    for (const auto& sg : segs) { ts.push_back(sg.t0); ts.push_back(sg.t1); }
    std::sort(ts.begin(), ts.end());
    ts.erase(std::unique(ts.begin(), ts.end()), ts.end());
    std::vector<size_t> order(segs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return segs[a].t0 < segs[b].t0; });
    std::vector<Run> out;
    std::map<const Rail*, size_t> open;  // a representative's run segment, to extend while nothing changes
    std::vector<const Rail*> prev_reps;
    std::vector<size_t> active;
    size_t next = 0;
    const auto sum = [](double acc, double x) { return acc < 0 || x < 0 ? -1.0 : acc + x; };
    for (size_t k = 0; k + 1 < ts.size(); ++k) {
        const int64_t a = ts[k], b = ts[k + 1];
        active.erase(std::remove_if(active.begin(), active.end(), [&](size_t i) { return segs[i].t1 <= a; }), active.end());
        for (; next < order.size() && segs[order[next]].t0 <= a; ++next)
            if (segs[order[next]].t1 > a) active.push_back(order[next]);
        std::vector<const Rail*> reps;
        for (const bool is_long : {true, false}) {
            std::vector<const Segment*> side;
            for (size_t i : active)
                if (segs[i].rail->is_long == is_long) side.push_back(&segs[i]);
            std::sort(side.begin(), side.end(), [](const Segment* x, const Segment* y) { return x->price < y->price; });
            // Chains of touching zones, each split into hills of dollars; [i, j) below is one hill.
            std::vector<std::pair<size_t, size_t>> groups;
            for (size_t c0 = 0; c0 < side.size();) {
                size_t c1 = c0 + 1;
                while (c1 < side.size() && side[c1]->step->zone_lo <= side[c1 - 1]->step->zone_hi*kLadderGap) ++c1;
                std::vector<double> u;
                std::vector<bool> kept;
                for (size_t m = c0; m < c1; ++m) {
                    u.push_back(side[m]->step->usd);
                    kept.push_back(std::find(prev_reps.begin(), prev_reps.end(), side[m]->rail) != prev_reps.end());
                }
                const auto h = hills(u, kept);
                for (size_t m = c0; m < c1;) {
                    size_t e = m + 1;
                    while (e < c1 && h[e - c0] == h[m - c0]) ++e;
                    groups.push_back({m, e});
                    m = e;
                }
                c0 = c1;
            }
            for (const auto& [i, j] : groups) {
                // [i, j) is one run: a sticky representative, else the member with the most dollars.
                const Segment* rep = nullptr;
                for (size_t m = i; m < j; ++m)
                    if (std::find(prev_reps.begin(), prev_reps.end(), side[m]->rail) != prev_reps.end() &&
                        (!rep || side[m]->step->usd > rep->step->usd)) rep = side[m];
                if (!rep)
                    for (size_t m = i; m < j; ++m)
                        if (!rep || side[m]->step->usd > rep->step->usd) rep = side[m];
                Run r;
                r.t0 = a; r.t1 = b; r.price = rep->price; r.rep = rep->rail; r.members = static_cast<int>(j - i);
                r.lo = side[i]->step->zone_lo; r.hi = side[j - 1]->step->zone_hi;
                r.near = is_long ? side[j - 1]->price : side[i]->price;
                for (size_t m = i; m < j; ++m) {
                    const Step& st = *side[m]->step;
                    r.v = sum(r.v, side[m]->v);
                    r.usd += st.usd;
                    r.e1 = sum(r.e1, st.expected_1h);
                    r.e4 = sum(r.e4, st.expected_4h);
                    r.e24 = sum(r.e24, st.expected);
                }
                auto it = open.find(rep->rail);
                if (it != open.end() && out[it->second].t1 == a && out[it->second].v == r.v && out[it->second].members == r.members &&
                    out[it->second].lo == r.lo && out[it->second].hi == r.hi) {
                    out[it->second].t1 = b;
                } else {
                    out.push_back(r);
                    open[rep->rail] = out.size() - 1;
                }
                reps.push_back(rep->rail);
            }
        }
        prev_reps.swap(reps);
    }
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
        ++revision_;
    }

    // A stored record (history): the rail, every stored step, its end.
    void apply_record(const std::string& config_id, const Rail& r) {
        merge(config_id, r);
        bound();
        ++revision_;
    }

    const std::map<Key, Rail>& rails() const { return rails_; }
    const Latest& latest() const { return latest_; }
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
};
}  // namespace liqmap2_rails
