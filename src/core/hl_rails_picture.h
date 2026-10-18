#pragma once
// What the Hyperliquid rails overlay (rendering/hl_rails_overlay.h) draws, worked out without a renderer, and kept
// across frames while nothing it reads changes (James 2026-10-18: "Hyperliquid positions" took the live chart from 180
// to 120 FPS). The overlay worked the whole picture out every frame: every step of every rail in a band beyond the
// view (V2's segments()), two sorts, sets, the profile's candle scans from each rail's birth. On BTC (290 rails, 25,396
// steps) that was 1.4 to 2.2 ms a frame in WASM at 4 h. Now one in-order pass over the rails in view builds it, and a
// frame reuses it until the clock reaches anything that changes it (Picture::until; the overlay keys everything else).
// The rules (consumption, clear_start, specks, major levels, the typical rail, the profile) are the overlay's own, moved
// here unchanged; tests/native/hl_rails_test.cpp holds the kept picture to the plain per-frame pass at many clocks.
// Proj maps to the plot's pixels: x(t) for a real time t, y(price) unrounded, price_at(y); the overlay's calls ImPlot.
#include "core/hl_rails.h"
#include "core/liqmap2_rails.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace hl_rails {
inline constexpr float kBarPx = 4.0f;       // a rail's bar (liqmap2_rails_overlay::kWidth)
inline constexpr float kRowPitch = 5.0f;    // profile rows
inline constexpr float kMinRunPx = 8.0f;    // a stretch narrower than this joins the next (one colour, no dashes)
inline constexpr float kMinLinePx = 12.0f;  // a whole line shorter than this is a speck: the profile has it
inline constexpr float kMinDrawPx = 3.0f;   // a stretch narrower than this is not drawn

// A rail's colour step by how far it stands out from the typical rail in view (James 2026-10-15: the rails "mostly all
// have the same color ... they don't actually look that significant"). On BTC a rail stands in nearly every 0.25% bin
// near price, most of a few million dollars and a few of tens of millions; scaled to the one largest in view, nearly
// all drew in the lowest blues as an even ladder. Now: at the typical rail (the median in view) the lowest step, then
// up the ramp by doubling, 2x, 4x, and 8x or more at the top.
inline int significance(double v, double typical) {
    if (!(v > 0) || !(typical > 0)) return 0;
    const double steps = std::log2(v/typical)*(liqmap2_rails::kLevels - 1)/3.0;
    return std::clamp(static_cast<int>(std::floor(steps)), 0, liqmap2_rails::kLevels - 1);
}

// The chart's closed candles (open time ms ascending, lows, highs), the forming one, and the plot's time shift
// (candles sit centred on their open, so a real time t is drawn at t - shift). The forming candle consumes too
// (rule 2: no band across a candle the chart draws).
struct Candles {
    const std::vector<double>* t = nullptr;
    const std::vector<double>* lo = nullptr;
    const std::vector<double>* hi = nullptr;
    double shift_ms = 0;
    bool forming = false;
    double forming_t = 0, forming_lo = 0, forming_hi = 0;
};

// The first candle, closed or forming, at or after from_ms (and before until_ms) touching price; INT64_MAX if none.
inline int64_t consumed_at(const Candles& c, bool is_long, double price, int64_t from_ms, int64_t until_ms, double slack) {
    const int64_t closed = c.t ? liqmap2_rails::consumed_at(*c.t, *c.lo, *c.hi, is_long, price, from_ms, until_ms, slack)
                               : INT64_MAX;
    if (closed != INT64_MAX || !c.forming || c.forming_t < from_ms || c.forming_t >= until_ms) return closed;
    return (is_long ? c.forming_lo <= price + slack : c.forming_hi >= price - slack) ? static_cast<int64_t>(c.forming_t)
                                                                                     : INT64_MAX;
}

// Where a line may start without crossing the candle it starts inside (liqmap2_rails::clear_start), the forming one
// included.
inline int64_t clear_start(const Candles& c, double price, int64_t from_ms, int64_t tf_ms, double slack) {
    if (c.forming && from_ms >= c.forming_t && tf_ms > 0) {
        const int64_t open = static_cast<int64_t>(c.forming_t);
        if (from_ms == open || from_ms >= open + tf_ms || price < c.forming_lo - slack || price > c.forming_hi + slack)
            return from_ms;
        return std::max(from_ms, open + static_cast<int64_t>(liqmap2_rails::kPastBody*static_cast<double>(tf_ms)));
    }
    return c.t ? liqmap2_rails::clear_start(*c.t, *c.lo, *c.hi, price, from_ms, tf_ms, slack) : from_ms;
}

// The clock the picture reads at asof. A stale census (no message within kFreshMs) stops every rail a column after the
// last message: what stood since is unknown. latest: null in a replay, which draws the record.
inline int64_t picture_clock(const Latest* latest, int64_t asof, bool& stale) {
    stale = state(latest, asof) == State::stale;
    return stale ? std::min(asof, latest->timestamp_ms + kColumnMs) : asof;
}

// The chart's present column: the end of the forming candle's slot. Standing rails run to it.
inline int64_t now_edge(const Candles& c, int64_t clock, bool stale) {
    int64_t edge = clock;
    if (!stale && c.shift_ms > 0) {
        if (c.forming) edge = std::max(clock, static_cast<int64_t>(c.forming_t + 2*c.shift_ms));
        else if (c.t && c.lo && c.hi && !c.t->empty()) {
            const auto& t = *c.t;
            auto it = std::upper_bound(t.begin(), t.end(), static_cast<double>(clock));
            if (it != t.begin()) edge = std::max(clock, static_cast<int64_t>(*std::prev(it) + 2*c.shift_ms));
        }
    }
    return edge;
}

// The view: its times (real, the shift added back) and chart prices; the plot's top and height in px.
struct Plot {
    double t_lo = 0, t_hi = 0, y_min = 0, y_max = 0;
    float top = 0, height = 0;
};

// A drawn stretch of a rail: from t0 to t1 at its chart price, its dollars and step; its colour step and whether it
// sits under the typical rail (quiet).
struct Line {
    int64_t t0 = 0, t1 = 0;
    double price = 0, v = 0;
    const liqmap2_rails::Rail* rail = nullptr;
    const liqmap2_rails::Step* step = nullptr;
    int level = 0;
    bool quiet = false;
    bool consumed = false;  // its rail ends at a candle in view (the profile leaves it out)
};

struct Row { double v = 0; int rails = 0; };

struct Picture {
    std::vector<Line> lines;          // the shown lines, dimmest first (the brightest draw last)
    std::vector<uint32_t> at_clock;   // lines that end at the clock: they follow it (follow())
    std::vector<uint32_t> labels;     // lines standing at the clock, most dollars first (the dollar labels)
    std::vector<Row> rows;            // the profile (when asked for)
    double row_max = 0;
    double typical = 0, vmax = 0;     // the lower median of the rails' peaks in view; the largest stretch
    size_t selected = 0, available = 0;
    int64_t clock = 0;                // the clock it was built at
    int64_t until = 0;                // the next clock at or after it that can change it (INT64_MAX: none)

    // The runs that end at the clock run on with it, as a fresh pass would draw them.
    void follow(int64_t now) {
        for (uint32_t i : at_clock) lines[i].t1 = now;
    }

    // Scratch, kept for its capacity: a frame that rebuilds allocates nothing once these have grown.
    struct Seg { int64_t t0, t1; double v; const liqmap2_rails::Step* step; };
    std::vector<Seg> segs;
    std::vector<Line> all, mine;
    std::vector<DisplayCandidate> candidates;
    std::vector<uint32_t> first;      // per candidate, its first line in all
    std::vector<char> chosen;
    std::vector<uint32_t> order;
    std::vector<float> ys;
    std::vector<double> peaks;
};

// major_levels (core/hl_rails.h) as flags over the candidates, without allocating once the scratch has grown. order:
// the valid candidates, largest first (its ties as major_levels breaks them). Returns how many are chosen.
inline size_t mark_major_levels(const std::vector<DisplayCandidate>& c, std::vector<char>& chosen,
                                std::vector<uint32_t>& order, std::vector<float>& ys, float spacing = 14.0f) {
    chosen.assign(c.size(), 0);
    order.clear();
    size_t counts[2]{};
    for (uint32_t i = 0; i < c.size(); ++i) {
        if (!std::isfinite(c[i].usd) || c[i].usd <= 0 || !std::isfinite(c[i].y)) continue;
        order.push_back(i);
        ++counts[c[i].is_long];
    }
    // The ids are distinct, so this order is total and equals major_levels' stable sort.
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        if (c[a].usd != c[b].usd) return c[a].usd > c[b].usd;
        if (c[a].observed_ms != c[b].observed_ms) return c[a].observed_ms > c[b].observed_ms;
        return c[a].id < c[b].id;
    });
    const size_t budget[2] = {std::min<size_t>(6, (counts[0] + 4)/5), std::min<size_t>(6, (counts[1] + 4)/5)};
    size_t used[2]{}, n = 0;
    ys.clear();
    for (uint32_t i : order) {
        if (used[c[i].is_long] >= budget[c[i].is_long]) continue;
        if (std::any_of(ys.begin(), ys.end(), [&](float y) { return std::abs(y - c[i].y) < spacing; })) continue;
        chosen[i] = 1;
        ys.push_back(c[i].y);
        ++used[c[i].is_long];
        ++n;
    }
    return n;
}

// Works out the picture at the clock (picture_clock) for the book's rails in view, mapped onto the chart by ratio,
// and the next clock that can change it (p.until). now_edge: the present column at the clock. profile: work out the
// standing rails' profile too. major_only: only the major levels (else every eligible rail).
template <class Proj>
void build(Picture& p, const Book& book, const Candles& candles, int64_t clock, int64_t now_edge, double ratio,
           const Plot& view, const Proj& proj, bool stale, bool profile, bool major_only) {
    using liqmap2_rails::Rail;
    const double t_lo = view.t_lo, t_hi = view.t_hi;
    const auto x_of = [&](double t) { return proj.x(t); };
    const auto y_of = [&](double price) { return std::round(proj.y(price)); };
    const bool have_candles = candles.t && candles.lo && candles.hi;
    p.clock = clock;
    p.lines.clear();
    p.at_clock.clear();
    p.labels.clear();
    p.rows.clear();
    p.row_max = p.typical = p.vmax = 0;
    p.selected = p.available = 0;

    // The next moment the picture can change: anything at or after the clock that the passes below compare it with
    // (conservative: a moment that changes nothing only costs a rebuild).
    int64_t until = INT64_MAX;
    const auto ahead = [&](int64_t t) { if (t >= clock) until = std::min(until, t); };
    // How long a line ending at the clock (and so growing with it) takes to reach px wide from a; never: nothing.
    const auto grows_to = [&](double a, float px) {
        const auto width = [&](double t) { return x_of(std::min(t, t_hi)) - x_of(a); };
        if (!(t_hi > static_cast<double>(clock)) || t_hi >= 9.2e18 || width(t_hi) < px) return;
        int64_t lo = clock, hi = static_cast<int64_t>(std::ceil(t_hi));
        while (hi - lo > 1) {
            const int64_t mid = lo + (hi - lo)/2;
            (width(static_cast<double>(mid)) >= px ? hi : lo) = mid;
        }
        ahead(hi);
    };
    if (std::isfinite(t_lo) && t_lo >= static_cast<double>(clock) && t_lo < 9.2e18)
        ahead(static_cast<int64_t>(std::ceil(t_lo)));
    if (have_candles) {
        const auto& t = *candles.t;
        auto it = std::lower_bound(t.begin(), t.end(), static_cast<double>(clock));
        if (it != t.end() && *it < 9.2e18) ahead(static_cast<int64_t>(std::ceil(*it)));
    }
    if (candles.forming && candles.forming_t >= static_cast<double>(clock) && candles.forming_t < 9.2e18)
        ahead(static_cast<int64_t>(std::ceil(candles.forming_t)));
    if (now_edge > clock) ahead(now_edge);

    // The rails in view, each in time order and the rails in id order (the map's), as V2's segments() sorted back to
    // rail and time gave them. Only rails whose chart price is in view draw (segments' band, 10% wider, held more).
    auto& all = p.all;
    all.clear();
    for (const auto& [key, r] : book.rails) {
        ahead(r.born_ms);
        ahead(liqmap2_rails::start_ms(r));
        if (r.end_ms > 0) {
            ahead(r.end_ms);
            ahead(liqmap2_rails::end_known_ms(r));
        }
        const auto next = std::lower_bound(r.steps.begin(), r.steps.end(), clock,
                                           [](const liqmap2_rails::Step& s, int64_t v) { return s.at_ms < v; });
        if (next != r.steps.end()) ahead(next->at_ms);
        if (r.steps.empty()) continue;
        const double price = r.price*ratio;
        if (price < view.y_min || price > view.y_max) continue;
        // From its start to its end once known at the clock, else the clock; each step to the next (segments()).
        const int64_t from = liqmap2_rails::start_ms(r), end_known = liqmap2_rails::end_at(r, clock);
        const int64_t until_r = end_known > 0 ? end_known : clock;
        if (from > t_hi || until_r < t_lo || until_r <= from) continue;
        auto& segs = p.segs;
        segs.clear();
        for (size_t i = 0; i < r.steps.size(); ++i) {
            const int64_t a = i == 0 ? from : std::max(from, r.steps[i].at_ms);
            const int64_t b = i + 1 < r.steps.size() ? std::min(until_r, r.steps[i + 1].at_ms) : until_r;
            if (b <= a || b < t_lo || a > t_hi) continue;
            segs.push_back({a, b, liqmap2_rails::value(r.steps[i], liqmap2_rails::Colour::dollars), &r.steps[i]});
        }
        if (segs.empty()) continue;
        // Candles consume: the line ends at the first candle touching its row after the line began.
        int64_t end = INT64_MAX, start = segs.front().t0;
        bool pushed = false;
        if (have_candles || candles.forming) {
            const float y = proj.y(price);
            const double slack = std::abs(proj.price_at(y - (kBarPx*0.5f + 1.0f)) - price);
            end = consumed_at(candles, r.is_long, price, segs.front().t0, static_cast<int64_t>(t_hi) + 1, slack);
            start = clear_start(candles, price, segs.front().t0, static_cast<int64_t>(2*candles.shift_ms), slack);
            pushed = start > segs.front().t0;
            ahead(start);
        }
        auto& mine = p.mine;
        mine.clear();
        for (const auto& s : segs) {
            Line l{std::max(s.t0, start), s.t1, price, s.v, &r, s.step, 0, false, end != INT64_MAX};
            if (l.t0 >= end || l.t0 >= std::max(l.t1, l.t1 >= clock ? now_edge : l.t1)) continue;
            l.t1 = std::min(l.t1, end);
            // Still standing at the clock: on to the present column.
            if (l.t1 >= clock && end == INT64_MAX && (r.end_ms == 0 || r.end_ms > clock)) l.t1 = std::max(l.t1, now_edge);
            // One colour per stretch: a stretch narrower than kMinRunPx joins the next and takes its figures.
            if (!mine.empty() && mine.back().t1 == l.t0 && x_of(double(mine.back().t1)) - x_of(double(mine.back().t0)) < kMinRunPx) {
                l.t0 = mine.back().t0;
                mine.pop_back();
            }
            mine.push_back(l);
        }
        // A speck (a sliver at this zoom) is noise beside the candles, unless it is a rail first seen moments ago.
        if (!mine.empty()) {
            const double a = std::max(double(mine.front().t0), t_lo), b = std::min(double(mine.back().t1), t_hi);
            const bool fresh = mine.back().t1 >= clock && !pushed;
            if (fresh || x_of(b) - x_of(a) >= kMinLinePx) all.insert(all.end(), mine.begin(), mine.end());
            else if (mine.back().t1 == clock) grows_to(a, kMinLinePx);  // grows with the clock until it is no speck
        }
    }
    std::erase_if(all, [&](const Line& l) {
        const double a = std::max(double(l.t0), t_lo);
        const bool narrow = x_of(std::min(double(l.t1), t_hi)) - x_of(a) < kMinDrawPx;
        if (narrow && l.t1 == clock) grows_to(a, kMinDrawPx);
        return l.price < view.y_min || l.price > view.y_max || narrow;
    });

    // One candidate per rail: its largest stretch, how long it has been seen, its row.
    auto& cand = p.candidates;
    cand.clear();
    p.first.clear();
    for (size_t i = 0; i < all.size();) {
        const auto& head = all[i];
        double peak = 0;
        int64_t end = head.t0;
        size_t j = i;
        for (; j < all.size() && all[j].rail == head.rail; ++j) {
            peak = std::max(peak, all[j].v);
            end = std::max(end, all[j].t1);
        }
        cand.push_back({head.rail->id, head.rail->is_long, peak,
                        std::max<int64_t>(0, std::min(clock, end) - head.rail->born_ms), static_cast<float>(y_of(head.price))});
        p.first.push_back(static_cast<uint32_t>(i));
        i = j;
    }
    p.available = cand.size();
    if (major_only) {
        p.selected = mark_major_levels(cand, p.chosen, p.order, p.ys);
        // A rail still standing is seen longer each moment; among equal dollars that breaks ties, so where it meets
        // another's fixed time the choice can turn.
        const auto& o = p.order;
        for (size_t g0 = 0; g0 < o.size();) {
            size_t g1 = g0 + 1;
            while (g1 < o.size() && cand[o[g1]].usd == cand[o[g0]].usd) ++g1;
            for (size_t a = g0; g1 - g0 > 1 && a < g1; ++a)
                for (size_t b = g0; b < g1; ++b) {
                    const auto& x = cand[o[a]];
                    const auto& y = cand[o[b]];
                    const Line& xl = all[p.first[o[a]]];
                    const Line& yl = all[p.first[o[b]]];
                    if (x.observed_ms != clock - xl.rail->born_ms || y.observed_ms == clock - yl.rail->born_ms) continue;
                    // x grows with the clock, y holds.
                    const int64_t meet = xl.rail->born_ms + y.observed_ms;
                    ahead(meet);
                    ahead(meet + 1);
                }
            g0 = g1;
        }
    } else {
        p.chosen.assign(cand.size(), 1);
        p.selected = cand.size();
    }
    for (const auto& l : all) p.vmax = std::max(p.vmax, l.v);
    // The typical rail in view: the lower median of each drawn rail's largest stretch. The lower one, so of two rails
    // the larger reads against the smaller.
    auto& peaks = p.peaks;
    peaks.clear();
    for (const auto& c : cand)
        if (c.usd > 0) peaks.push_back(c.usd);
    if (!peaks.empty()) {
        const auto mid = peaks.begin() + static_cast<std::ptrdiff_t>((peaks.size() - 1)/2);
        std::nth_element(peaks.begin(), mid, peaks.end());
        p.typical = *mid;
    }
    // The chosen rails' lines, dimmest first; equal dollars keep rail and time order (as a stable sort would).
    for (size_t k = 0; k < cand.size(); ++k) {
        if (!p.chosen[k]) continue;
        for (size_t i = p.first[k]; i < all.size() && all[i].rail == all[p.first[k]].rail; ++i) p.lines.push_back(all[i]);
    }
    std::sort(p.lines.begin(), p.lines.end(), [](const Line& a, const Line& b) {
        if (a.v != b.v) return a.v < b.v;
        if (a.rail->id != b.rail->id) return a.rail->id < b.rail->id;
        return a.t0 < b.t0;
    });
    for (uint32_t i = 0; i < p.lines.size(); ++i) {
        auto& l = p.lines[i];
        l.level = significance(l.v, p.typical);
        l.quiet = l.v < p.typical;
        if (l.t1 == clock) p.at_clock.push_back(i);
        if (l.t1 >= clock && !stale && (l.rail->end_ms == 0 || l.rail->end_ms > clock) && l.v > 0) p.labels.push_back(i);
    }
    std::sort(p.labels.begin(), p.labels.end(), [&](uint32_t a, uint32_t b) {
        if (p.lines[a].v != p.lines[b].v) return p.lines[a].v > p.lines[b].v;
        return a < b;
    });

    // The profile: every chosen rail standing at the clock and not consumed by the chart's candles, its dollars in the
    // row at its price (a census bin is 0.25% wide, many rows at an intraday zoom: at its price it stays a bar). The
    // rails in id order, as the map gave them, so each row sums in the same order.
    if (profile) {
        auto& rows = p.rows;
        rows.assign(static_cast<size_t>(std::max(1.0f, std::ceil(view.height/kRowPitch))), Row{});
        for (size_t k = 0; k < cand.size(); ++k) {
            if (!p.chosen[k]) continue;
            const Line& head = all[p.first[k]];
            const Rail& r = *head.rail;
            if ((r.end_ms > 0 && r.end_ms <= clock) || r.born_ms > clock || r.steps.empty() || stale) continue;
            const auto* st = liqmap2_rails::step_at(r, clock);
            const double price = r.price*ratio;
            if (!(st->usd > 0) || price < view.y_min || price > view.y_max) continue;
            if (head.consumed || consumed_at(candles, r.is_long, price, r.born_ms, clock + 1, 0) != INT64_MAX) continue;
            const int row = std::clamp(static_cast<int>((proj.y(price) - view.top)/kRowPitch), 0,
                                       static_cast<int>(rows.size()) - 1);
            rows[row].v += st->usd;
            ++rows[row].rails;
        }
        for (const auto& row : rows) p.row_max = std::max(p.row_max, row.v);
    }
    p.until = until;
}

// A picture kept across frames. It holds for an asof in [asof, until), or at asof alone; same_inputs: everything else
// it reads is as when it was built (the overlay's Cache::Key).
struct Kept {
    bool valid = false;
    int64_t asof = 0, until = 0;
    Picture picture;
};

template <class Proj>
const Picture& keep(Kept& k, bool same_inputs, const Book& book, const Latest* latest, const Candles& candles,
                    int64_t asof, double ratio, const Plot& view, const Proj& proj, bool profile, bool major_only) {
    bool stale = false;
    const int64_t clock = picture_clock(latest, asof, stale);
    if (!same_inputs || !k.valid || !(asof == k.asof || (asof > k.asof && asof < k.until))) {
        k.valid = true;
        k.asof = asof;
        build(k.picture, book, candles, clock, now_edge(candles, clock, stale), ratio, view, proj, stale, profile, major_only);
        // Stale, the clock stands at the last message; fresh, it is asof, until the census goes stale.
        k.until = stale ? INT64_MAX
                : latest ? std::min(k.picture.until, latest->timestamp_ms + kFreshMs + 1) : k.picture.until;
    }
    k.picture.follow(clock);
    return k.picture;
}
}  // namespace hl_rails
