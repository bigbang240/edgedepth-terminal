// V2 rails admin preview store (core/liqmap2_rails.h): live messages and history merge into one record per rail,
// steps stay ordered and unique, ends stick, and the store stays bounded.
#include "core/liqmap2_rails.h"

#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

using namespace liqmap2_rails;

static Rail rail(int64_t id, int64_t born, int64_t step, double usd) {
    Rail r;
    r.id = id;
    r.is_long = true;
    r.price = 100.0 - static_cast<double>(id);
    r.born_ms = born;
    r.steps.push_back({step, usd, 0.2, usd/5, r.price - 0.1, r.price + 0.1});
    return r;
}

static int64_t h_ms(int64_t hours) { return hours*3600000; }

// The split as first written (2026-10-10): every member walks out to its first larger member each way. hills() does
// this in one pass each way and must agree. The spacing (space_hills) is applied to the peaks found here.
static std::vector<int> plain_hills(const std::vector<double>& u, const std::vector<bool>& kept,
                                    const std::vector<double>& price = {}, double min_gap = 0) {
    const size_t n = u.size();
    std::vector<size_t> peaks;
    for (size_t i = 0; i < n; ++i) {
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
            const size_t a = peaks[hill], b = peaks[hill + 1];
            size_t valley = a + 1;
            for (size_t k = a + 1; k < b; ++k)
                if (u[k] < u[valley]) valley = k;
            if (i > valley || (i == valley && valley - a > b - valley) || i == b) ++hill;
        }
        out[i] = static_cast<int>(hill);
    }
    space_hills(out, peaks, u, kept, price, min_gap);
    return out;
}

// The sweep as first written: every chain of every stretch split afresh. runs() skips the chains nothing reached and
// must give the same runs in the same order.
static std::vector<Run> plain_runs(const std::vector<Segment>& segs, double min_gap = 0) {
    std::vector<int64_t> ts;
    for (const auto& sg : segs) { ts.push_back(sg.t0); ts.push_back(sg.t1); }
    std::sort(ts.begin(), ts.end());
    ts.erase(std::unique(ts.begin(), ts.end()), ts.end());
    std::vector<size_t> order(segs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return segs[x].t0 < segs[y].t0; });
    std::vector<Run> out;
    std::map<const Rail*, size_t> open;
    std::vector<const Rail*> prev_reps, reps;
    std::vector<size_t> active;
    size_t next = 0;
    const auto sum = [](double acc, double x) { return acc < 0 || x < 0 ? -1.0 : acc + x; };
    const auto was_rep = [&](const Rail* r) { return std::find(prev_reps.begin(), prev_reps.end(), r) != prev_reps.end(); };
    for (size_t k = 0; k + 1 < ts.size(); ++k) {
        const int64_t a = ts[k], b = ts[k + 1];
        active.erase(std::remove_if(active.begin(), active.end(), [&](size_t i) { return segs[i].t1 <= a; }), active.end());
        for (; next < order.size() && segs[order[next]].t0 <= a; ++next)
            if (segs[order[next]].t1 > a) active.push_back(order[next]);
        reps.clear();
        for (const bool is_long : {true, false}) {
            std::vector<const Segment*> side;
            for (size_t i : active)
                if (segs[i].rail->is_long == is_long) side.push_back(&segs[i]);
            std::sort(side.begin(), side.end(), [](const Segment* x, const Segment* y) { return x->price < y->price; });
            std::vector<std::pair<size_t, size_t>> groups;
            for (size_t c0 = 0; c0 < side.size();) {
                size_t c1 = c0 + 1;
                while (c1 < side.size() && (side[c1]->step->zone_lo <= side[c1 - 1]->step->zone_hi*kLadderGap ||
                                            side[c1]->step->zone_lo - side[c1 - 1]->step->zone_hi < 0.5*min_gap)) ++c1;
                std::vector<double> u, price;
                std::vector<bool> kept;
                for (size_t m = c0; m < c1; ++m) {
                    u.push_back(side[m]->step->usd);
                    price.push_back(side[m]->price);
                    kept.push_back(was_rep(side[m]->rail));
                }
                const auto hs = plain_hills(u, kept, price, min_gap);
                for (size_t m = c0; m < c1;) {
                    size_t e = m + 1;
                    while (e < c1 && hs[e - c0] == hs[m - c0]) ++e;
                    groups.push_back({m, e});
                    m = e;
                }
                c0 = c1;
            }
            for (const auto& [i, j] : groups) {
                const Segment* rep = nullptr;
                for (size_t m = i; m < j; ++m)
                    if (was_rep(side[m]->rail) && (!rep || side[m]->step->usd > rep->step->usd)) rep = side[m];
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
                if (it != open.end() && out[it->second].t1 == a && out[it->second].v == r.v &&
                    out[it->second].members == r.members && out[it->second].lo == r.lo && out[it->second].hi == r.hi) {
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

static Latest at(int64_t ts) {
    Latest l;
    l.timestamp_ms = ts;
    l.mark = 101;
    l.status = "ready";
    l.config_id = "cfg";
    return l;
}

int main() {
    const int64_t h = 3600000;
    Store s;
    s.apply_live(at(10*h), "cfg", {rail(1, 9*h, 10*h, 100), rail(2, 9*h, 10*h, 50)}, {});
    s.apply_live(at(10*h + 60000), "cfg", {rail(1, 9*h, 10*h, 100), rail(2, 9*h, 10*h, 50)}, {});  // same step: no copy
    s.apply_live(at(11*h), "cfg", {rail(1, 9*h, 11*h, 120)}, {});
    CHECK(s.rails().size() == 2);
    const Rail& one = s.rails().at({"cfg", 1});
    CHECK(one.steps.size() == 2 && one.steps[0].usd == 100 && one.steps[1].usd == 120);
    CHECK(step_at(one, 9*h)->usd == 100);         // before the first step: its first numbers
    CHECK(step_at(one, 10*h + 30*60000)->usd == 100);
    CHECK(step_at(one, 11*h)->usd == 120);

    Rail end;
    end.id = 2;
    end.end_ms = 11*h + 5*60000;
    end.end = "touched";
    s.apply_live(at(11*h + 6*60000), "cfg", {}, {end});
    CHECK(s.rails().at({"cfg", 2}).end_ms == 11*h + 5*60000 && s.rails().at({"cfg", 2}).end == "touched");

    // History fills older steps of a live rail and brings rails the session never saw.
    Rail old = rail(1, 9*h, 9*h + 30*60000, 90);
    s.apply_record("cfg", old);
    CHECK(s.rails().at({"cfg", 1}).steps.size() == 3 && s.rails().at({"cfg", 1}).steps[0].usd == 90);
    Rail gone = rail(7, 2*h, 2*h, 10);
    gone.end_ms = 3*h;
    gone.end = "closed";
    s.apply_record("cfg", gone);
    CHECK(s.rails().count({"cfg", 7}) == 1);
    CHECK(s.rails().size() == 3);

    // A message older than the latest is ignored.
    const auto rev = s.revision();
    s.apply_live(at(5*h), "cfg", {rail(9, 4*h, 5*h, 1)}, {});
    CHECK(s.revision() == rev && s.rails().count({"cfg", 9}) == 0);

    // Rails ended more than a week before the newest message are dropped; live rails always stay.
    s.apply_live(at(3*h + kKeepEndedMs + h), "cfg", {}, {});
    CHECK(s.rails().count({"cfg", 7}) == 0 && s.rails().count({"cfg", 1}) == 1);

    // Drawing geometry: a rail runs from its birth, splits at each step, ends at its end (or the clock), never later.
    {
        Store g;
        Rail a = rail(1, 0, 0, 100);
        a.steps.push_back({2*h, 300, 0.5, 150, 98.9, 99.1});
        Rail b = rail(2, h, h, 40);
        b.steps[0].expected = -1;  // chance unknown
        g.apply_live(at(4*h), "cfg", {a, b}, {});
        Rail e;
        e.id = 1;
        e.end_ms = 3*h;
        e.end = "touched";
        g.apply_live(at(4*h + 60000), "cfg", {}, {e});
        double vmax = 0;
        const auto segs = segments(g.rails(), 4*h, 0, 5*h, 0, 1000, vmax);
        CHECK(vmax == 150);
        CHECK(segs.size() == 3);
        for (const auto& s2 : segs) {
            CHECK(s2.t1 <= 4*h);
            if (s2.rail->id == 1) CHECK(s2.t1 <= 3*h);   // ended at the touching bar
        }
        CHECK(segs.back().v == 150 && segs.back().t0 == 2*h && segs.back().t1 == 3*h);  // brightest last
        CHECK(segs.front().v == -1 && segs.front().rail->id == 2 && segs.front().t1 == 4*h);
        CHECK(level(150, 150) == kLevels - 1 && level(-1, 150) == -1 && level(20, 150) >= 0 && level(20, 150) < level(150, 150));
        // A view that ends before the step leaves out the later segment; a price outside the view leaves out the rail.
        CHECK(segments(g.rails(), 4*h, 0, 90*60000, 0, 1000, vmax).size() == 2);
        const auto upper = segments(g.rails(), 4*h, 0, 5*h, 98.5, 1000, vmax);  // rail 1 at 99 in view, rail 2 at 98 not
        CHECK(upper.size() == 2 && upper[0].rail->id == 1 && upper[1].rail->id == 1);
        // Only live rails with a known expected figure are labelled, largest first.
        Rail c = rail(3, h, h, 500);
        g.apply_live(at(4*h + 120000), "cfg", {c}, {});
        const auto lab = labelled(g.rails(), 4*h + 120000, 0, 1000);
        CHECK(lab.size() == 1 && lab[0]->id == 3);
        // One unknown chance in view switches the whole view to dollars if reached (never the dimmest step for all);
        // a view where every rail has an expected figure keeps it unless the viewer prefers dollars.
        CHECK(view_colour(segs, Colour::expected) == Colour::dollars);
        CHECK(view_colour(upper, Colour::expected) == Colour::expected && view_colour(upper, Colour::dollars) == Colour::dollars);
        // The short horizons are unknown on these steps (-1 by default): a 4 h view falls back to dollars.
        CHECK(view_colour(segments(g.rails(), 4*h, 0, 5*h, 98.5, 1000, vmax, Colour::expected_4h), Colour::expected_4h) == Colour::dollars);
        const auto by_usd = segments(g.rails(), 4*h, 0, 5*h, 0, 1000, vmax, Colour::dollars);
        CHECK(vmax == 500 && by_usd.size() == 4);  // rails 1 (two steps), 2 and 3 by dollars
        for (const auto& s2 : by_usd) CHECK(s2.v >= 0);
        const auto lab_usd = labelled(g.rails(), 4*h + 120000, 0, 1000, Colour::dollars);  // rail 1 ended
        CHECK(lab_usd.size() == 2 && lab_usd[0]->id == 3 && lab_usd[1]->id == 2);
    }

    {
        // Ladders merge into one rail: three short rails whose zones touch, and one apart.
        Store g;
        const auto mk = [](int64_t id, double price, double lo, double hi, double usd) {
            Rail r;
            r.id = id;
            r.is_long = false;
            r.price = price;
            r.born_ms = 0;
            r.steps.push_back({0, usd, 0.2, usd/5, lo, hi});
            return r;
        };
        // 101-102 and 102-103.2 touch; 103.25 is within three grid rows of 103.2; 110 stands apart.
        Rail a = mk(1, 101.5, 101, 102, 100), b = mk(2, 102.5, 102, 103.2, 300), c = mk(3, 103.6, 103.25, 104, 200),
             d = mk(4, 110, 109.5, 110.5, 50);
        b.steps.push_back({2*h, 100, 0.2, 20, 102, 103.2});  // b shrinks later: its place as representative holds
        c.steps.push_back({2*h, 900, 0.2, 180, 103.25, 104}); // c grows past it
        g.apply_live(at(4*h), "cfg", {a, b, c, d}, {});
        double vmax = 0;
        const auto segs = segments(g.rails(), 4*h, 0, 5*h, 0, 1000, vmax, Colour::dollars);
        const auto rs = runs(segs);
        const Run* ladder = nullptr;
        int lone = 0;
        for (const auto& r : rs) {
            CHECK(r.t1 <= 4*h);
            if (r.members == 3 && r.t0 == 0) ladder = &r;
            if (r.members == 1) { ++lone; CHECK(r.rep->id == 4 && r.price == 110 && r.v == 50); }
        }
        CHECK(ladder && ladder->rep->id == 2 && ladder->price == 102.5 && ladder->v == 600 && ladder->usd == 600);
        CHECK(ladder && ladder->lo == 101 && ladder->hi == 104 && ladder->near == 101.5 && ladder->e24 == 120);
        // After the 2 h step c holds more, but b stays the line: one rail at 102.5 for the whole 4 h.
        int at_b = 0;
        for (const auto& r : rs)
            if (r.members == 3) { CHECK(r.rep->id == 2 && r.price == 102.5); ++at_b; }
        CHECK(at_b == 2);  // the value steps at 2 h (600 to 1100), the price never moves
        CHECK(lone >= 1);
        // When the representative ends, the run's line moves to the member with the most dollars then.
        Rail e;
        e.id = 2;
        e.end_ms = 3*h;
        e.end = "touched";
        g.apply_live(at(4*h + 60000), "cfg", {}, {e});
        const auto rs2 = runs(segments(g.rails(), 4*h, 0, 5*h, 0, 1000, vmax, Colour::dollars));
        bool moved = false;
        for (const auto& r : rs2)
            if (r.t0 >= 3*h && r.rep->id == 3 && r.price == 103.6) moved = true;
        CHECK(moved);
    }

    {
        // A chain of tiled zones splits into hills of dollars (IMX 2026-10-10: the publisher tiles a side end to end, so
        // contiguity alone made one rail of 18). The live IMX long side, by price: two distinct piles stay two rails.
        const std::vector<double> imx{607, 574, 543, 494, 644, 764, 850, 759, 615, 565, 464, 1022, 782, 959, 605, 663, 582, 568};
        const auto hl = hills(imx, {});
        CHECK(hl.front() == 0 && hl.back() == 1);
        CHECK(hl[6] == 0 && hl[11] == 1);         // the 850 and 1022 piles lead their hills
        CHECK(hl[10] == 1);                       // the valley (464) goes with the nearer peak
        for (size_t i = 1; i < hl.size(); ++i) CHECK(hl[i] >= hl[i - 1] && hl[i] - hl[i - 1] <= 1);  // hills are contiguous runs
        // A smooth ladder is one hill; a plateau is one peak.
        CHECK(hills({100, 110, 120, 115, 105}, {}) == std::vector<int>(5, 0));
        CHECK(hills({100, 300, 300, 100}, {}) == std::vector<int>(4, 0));
        // A small bump stays merged, unless it already draws (hysteresis keeps hourly steps from flickering the split).
        const std::vector<double> bump{1000, 500, 580, 500, 1000};
        CHECK(hills(bump, {}) == std::vector<int>({0, 0, 1, 1, 1}));                       // 580 is 16% over its col
        CHECK(hills(bump, {false, false, true, false, false}) == std::vector<int>({0, 0, 1, 1, 2}));  // a tied valley goes left
        CHECK(hills({}, {}).empty() && hills({5}, {}) == std::vector<int>({0}));

        // Spacing (AGT 2026-10-17, zoomed out on 1 h): peaks nearer than min_gap to a stronger one give way, and their
        // hills join the nearer surviving one; peaks further apart keep their lines; no gap, no change.
        const std::vector<double> three{100, 50, 120, 50, 100}, px{1, 2, 3, 4, 5};
        CHECK(hills(three, {}, px, 0) == std::vector<int>({0, 0, 1, 1, 2}));
        CHECK(hills(three, {}, px, 1.5) == std::vector<int>({0, 0, 1, 1, 2}));
        CHECK(hills(three, {}, px, 2.5) == std::vector<int>(5, 0));
        CHECK(hills({100, 10, 90, 10, 100}, {}, {0, 1, 1.5, 9, 10}, 2) == std::vector<int>({0, 0, 0, 0, 1}));  // 90 joins 0
        CHECK(hills({100, 10, 90, 10, 100}, {}, {0, 8.5, 9, 9.5, 10}, 2) == std::vector<int>({0, 0, 1, 1, 1}));  // 90 joins 10
        CHECK(hills({100, 10, 110}, {true, false, false}, {0, 1, 2}, 5) == std::vector<int>(3, 0));
        // A peak that drew last stretch holds against a slightly larger neighbour (it counts kProminence times its dollars).
        {
            std::vector<int> o{0, 0, 1, 1, 2};
            space_hills(o, {0, 2, 4}, {100, 10, 110, 10, 100}, {true}, {0, 1, 2, 9, 10}, 3);
            CHECK(o == std::vector<int>({0, 0, 0, 0, 1}));  // 110 gave way to the sticky 100 at 0 and joined it
        }

        // Through runs(): the two IMX piles draw as two rails, each at its largest member, the dollars kept.
        Store g;
        double lo = 0.1812;
        for (size_t i = 0; i < imx.size(); ++i) {
            Rail r;
            r.id = static_cast<int64_t>(i) + 1;
            r.is_long = true;
            r.price = lo*1.002;
            r.born_ms = 0;
            r.steps.push_back({0, imx[i], 0.3, imx[i]*0.3, lo, lo*1.004});
            lo *= 1.004;  // tiled end to end
            g.apply_live(at(h_ms(1)), "cfg", {r}, {});
        }
        double vmax = 0;
        const auto rs = runs(segments(g.rails(), h_ms(1), 0, static_cast<double>(h_ms(2)), 0, 1, vmax, Colour::dollars));
        double total = 0;
        for (const auto& r : rs) total += r.usd;
        CHECK(rs.size() == 2);
        CHECK(std::abs(total - 12060) < 1e-6);
        bool has7 = false, has12 = false;
        for (const auto& r : rs) { has7 = has7 || r.rep->id == 7; has12 = has12 || r.rep->id == 12; }
        CHECK(has7 && has12);
        // Zoomed out so the two piles' leaders stand nearer than the gap: one line at the larger (1022), dollars kept.
        const auto near = runs(segments(g.rails(), h_ms(1), 0, static_cast<double>(h_ms(2)), 0, 1, vmax, Colour::dollars), 0.01);
        CHECK(near.size() == 1 && near[0].rep->id == 12 && near[0].members == 18 && std::abs(near[0].usd - 12060) < 1e-6);
        CHECK(runs(segments(g.rails(), h_ms(1), 0, static_cast<double>(h_ms(2)), 0, 1, vmax, Colour::dollars), 0.001).size() == 2);
        // Rails whose zones do not touch are separate chains, until the gap between them is narrower than half min_gap
        // (AGT 2026-10-15: five singletons 0.5% apart stacked into one slab zoomed out).
        Store apart;
        for (int i = 0; i < 3; ++i) {
            Rail r;
            r.id = i + 1;
            r.is_long = true;
            r.price = 10.0 + i;
            r.steps.push_back({0, 100.0 + i, 0.3, 30, r.price - 0.2, r.price + 0.2});
            apart.apply_live(at(h_ms(1)), "cfg", {r}, {});
        }
        const auto seg3 = segments(apart.rails(), h_ms(1), 0, static_cast<double>(h_ms(2)), 0, 100, vmax, Colour::dollars);
        CHECK(runs(seg3).size() == 3 && runs(seg3, 1.0).size() == 3);
        const auto joined3 = runs(seg3, 1.5);
        CHECK(joined3.size() == 1 && joined3[0].members == 3 && joined3[0].rep->id == 3 && joined3[0].usd == 303);
    }

    {
        // Live draws a published rail from its birth (2026-10-17: the record began 10 Oct, most BTC rails were born in
        // September, and drawn from their first step every line started at one edge); replay keeps the first step.
        Store w;
        w.apply_record("cfg", rail(1, h_ms(2), h_ms(5), 100));
        const Rail& r = w.rails().begin()->second;
        double vmax = 0;
        const auto live = segments(w.rails(), h_ms(6), 0, static_cast<double>(h_ms(10)), 0, 1000, vmax, Colour::dollars, true);
        CHECK(live.size() == 1 && live[0].t0 == h_ms(2) && live[0].t1 == h_ms(6) && live[0].v == 100);
        const auto replay = segments(w.rails(), h_ms(6), 0, static_cast<double>(h_ms(10)), 0, 1000, vmax, Colour::dollars);
        CHECK(replay.size() == 1 && replay[0].t0 == h_ms(5));
        CHECK(segments(w.rails(), h_ms(4), 0, static_cast<double>(h_ms(10)), 0, 1000, vmax, Colour::dollars, true).empty());
        CHECK(line_from(r, h_ms(6), true) == h_ms(2) && line_from(r, h_ms(4), true) == h_ms(5) && line_from(r, h_ms(6), false) == h_ms(5));
        // Price reached the rail's row between its birth and its publication without ending it: the line reaches back
        // only past that candle (else the chart's candles would consume a standing rail there). 1 h candles from 0 h; a
        // long rail at 95 born at 1 h, published at 5 h; the 3 h candle's low touches 95.
        const std::vector<double> ct{0, double(h_ms(1)), double(h_ms(2)), double(h_ms(3)), double(h_ms(4)), double(h_ms(5))};
        const std::vector<double> clo{94, 97, 96, 94.9, 96, 97}, chi{99, 99, 99, 99, 99, 99};
        const int64_t back = clear_back(ct, clo, chi, true, 95, h_ms(1), h_ms(5), h_ms(1));
        CHECK(back == h_ms(3) + static_cast<int64_t>(kPastBody*double(h_ms(1))));
        CHECK(consumed_at(ct, clo, chi, true, 95, back, h_ms(10)) == INT64_MAX);              // then it stands
        CHECK(consumed_at(ct, clo, chi, true, 95, h_ms(1), h_ms(10)) == h_ms(3));             // from birth it vanished
        CHECK(clear_back(ct, clo, chi, true, 93, h_ms(1), h_ms(5), h_ms(1)) == h_ms(1));       // untouched: from birth
        CHECK(clear_back(ct, clo, chi, true, 95, h_ms(5), h_ms(5), h_ms(1)) == h_ms(5));       // born at publication
        CHECK(clear_back(ct, clo, chi, false, 98.5, h_ms(1), h_ms(5), h_ms(1)) == h_ms(4) + static_cast<int64_t>(kPastBody*double(h_ms(1))));
    }

    {
        // Candles consume rails: a long rail ends at the first candle (opening at or after its line began) whose low
        // reaches its price, a short rail at the first whose high does; candles before the line, or at or past the
        // bound, do not count.
        const std::vector<double> t{0, 60000, 120000, 180000, 240000};
        const std::vector<double> lo{99, 98, 96, 94.5, 97};
        const std::vector<double> hi{101, 102, 103, 104, 106};
        CHECK(consumed_at(t, lo, hi, true, 95, 0, 1 << 30) == 180000);       // the low first reaches 95 at 180000
        CHECK(consumed_at(t, lo, hi, true, 94, 0, 1 << 30) == INT64_MAX);    // never reached
        CHECK(consumed_at(t, lo, hi, false, 103.5, 0, 1 << 30) == 180000);   // the high reaches 103.5 at 180000
        CHECK(consumed_at(t, lo, hi, false, 105, 200000, 1 << 30) == 240000);
        CHECK(consumed_at(t, lo, hi, true, 98.5, 61000, 1 << 30) == 120000); // a candle before the line does not count
        CHECK(consumed_at(t, lo, hi, true, 95, 0, 180000) == INT64_MAX);     // nor one at the bound
        // Slack: a candle reaching any part of the bar's row consumes it.
        CHECK(consumed_at(t, lo, hi, true, 94, 0, 1 << 30) == INT64_MAX);
        CHECK(consumed_at(t, lo, hi, true, 94, 0, 1 << 30, 0.6) == 180000);
        CHECK(consumed_at(t, lo, hi, false, 106.5, 0, 1 << 30, 0.6) == 240000);
        // A line starting inside a candle whose range holds its price starts past that candle's body (0.9 of the
        // timeframe after its open); clear of the range, at an open, or past the last candle, at its birth.
        CHECK(clear_start(t, lo, hi, 97, 130000, 60000) == 120000 + 54000);
        CHECK(clear_start(t, lo, hi, 95, 130000, 60000) == 130000);   // the candle's low is 96: clear of it
        CHECK(clear_start(t, lo, hi, 95.5, 130000, 60000, 0.6) == 174000);  // within the bar's row of it
        CHECK(clear_start(t, lo, hi, 97, 120000, 60000) == 120000);   // at the open (1 m): consumption decides
        CHECK(clear_start(t, lo, hi, 97, 310000, 60000) == 310000);   // past the last candle
        CHECK(clear_start(t, lo, hi, 97, 178000, 60000) == 178000);   // already past the body
    }

    {
        // The candles rails meet: the closed ones and the forming one last (rule 2: no rail across a drawn candle).
        const std::vector<double> t{0, 60000, 120000}, lo{99, 98, 97}, hi{101, 102, 103};
        ChartCandles cc;
        using F = ChartCandles::Forming;
        CHECK(cc.update(t, lo, hi, 7, F{}));
        CHECK(cc.t() == t && cc.lo() == lo && cc.hi() == hi);
        const uint64_t r0 = cc.revision();
        CHECK(!cc.update(t, lo, hi, 7, F{}) && cc.revision() == r0);  // nothing changed: same arrays, same revision
        // Without the forming candle nothing reaches 95.5; the forming candle (open 180000, low 95) does.
        CHECK(consumed_at(cc.t(), cc.lo(), cc.hi(), true, 95.5, 0, 1 << 30) == INT64_MAX);
        CHECK(cc.update(t, lo, hi, 7, F{true, 180000, 95, 100}));
        CHECK(cc.t().size() == 4 && cc.t()[3] == 180000 && cc.lo()[3] == 95 && cc.hi()[3] == 100);
        CHECK(cc.revision() != r0);
        CHECK(consumed_at(cc.t(), cc.lo(), cc.hi(), true, 95.5, 0, 1 << 30) == 180000);
        // Its range grows: only the tail moves.
        const uint64_t r1 = cc.revision();
        CHECK(cc.update(t, lo, hi, 7, F{true, 180000, 94, 104}));
        CHECK(cc.t().size() == 4 && cc.lo()[3] == 94 && cc.hi()[3] == 104 && cc.revision() != r1);
        CHECK(consumed_at(cc.t(), cc.lo(), cc.hi(), false, 103.5, 150000, 1 << 30) == 180000);
        // At the last closed candle's open it joins that candle, and leaves it as it was once it moves on.
        CHECK(cc.update(t, lo, hi, 7, F{true, 120000, 96, 104}));
        CHECK(cc.t().size() == 3 && cc.lo()[2] == 96 && cc.hi()[2] == 104);
        CHECK(cc.update(t, lo, hi, 7, F{true, 180000, 99, 100}));
        CHECK(cc.t().size() == 4 && cc.lo()[2] == 97 && cc.hi()[2] == 103 && cc.lo()[3] == 99);
        // No range yet (low 0 would eat every long rail), or one before the last closed candle: left out.
        CHECK(cc.update(t, lo, hi, 7, F{true, 180000, 0, 0}) && cc.t() == t && cc.lo() == lo);
        CHECK(cc.update(t, lo, hi, 7, F{true, 180000, 101, 99}) && cc.t() == t);
        CHECK(cc.update(t, lo, hi, 7, F{true, 60000, 90, 110}) && cc.t() == t && cc.lo() == lo && cc.hi() == hi);
        // The closed candles change (the forming one closed): copied afresh, the next forming one after them.
        const std::vector<double> t2{0, 60000, 120000, 180000}, lo2{99, 98, 97, 95}, hi2{101, 102, 103, 100};
        CHECK(cc.update(t2, lo2, hi2, 8, F{true, 240000, 100, 100.5}));
        CHECK(cc.t().size() == 5 && cc.t()[3] == 180000 && cc.lo()[3] == 95 && cc.t()[4] == 240000);
        // Another chart's candles never share a revision.
        ChartCandles other;
        other.update(t, lo, hi, 7, F{});
        CHECK(other.revision() != cc.revision() && other.revision() != r0);
    }

    {
        // The fast split and sweep (2026-10-14: the overlay redid them every frame, 20 FPS zoomed out) against the
        // plain ones: random chains with plateaus, zeros and stickiness; random stores of touching ladders on both
        // sides with staggered hourly steps, births, ends and unknown figures, seen over random windows and clocks.
        uint64_t seed = 0x9e3779b97f4a7c15ULL;
        const auto rnd = [&seed]() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
        const auto uni = [&rnd]() { return static_cast<double>(rnd() % 1000000)/1e6; };
        int split_mismatch = 0;
        for (int it = 0; it < 20000; ++it) {
            const size_t n = 1 + rnd() % 40;
            const int levels = 1 + static_cast<int>(rnd() % 12);
            std::vector<double> u(n);
            std::vector<bool> kept(n);
            for (size_t i = 0; i < n; ++i) {
                u[i] = rnd() % 5 == 0 ? 0.0 : static_cast<double>(rnd() % static_cast<uint64_t>(levels))*(it % 2 ? 1.0 : 1.17);
                kept[i] = rnd() % 3 == 0;
            }
            if (it % 7 == 0) kept.resize(rnd() % (n + 1));
            if (hills(u, kept) != plain_hills(u, kept)) ++split_mismatch;
            std::vector<double> price(n);
            for (size_t i = 0; i < n; ++i) price[i] = (i ? price[i - 1] : 100.0) + uni();
            const double gap = uni()*4;
            const auto spaced = hills(u, kept, price, gap);
            if (spaced != plain_hills(u, kept, price, gap)) ++split_mismatch;
            for (size_t i = 1; i < spaced.size(); ++i)
                if (spaced[i] < spaced[i - 1] || spaced[i] - spaced[i - 1] > 1) ++split_mismatch;  // contiguous hills
        }
        CHECK(split_mismatch == 0);
        const Colour colours[] = {Colour::expected_1h, Colour::expected_4h, Colour::expected, Colour::dollars};
        int sweeps = 0, sweep_mismatch = 0;
        for (int trial = 0; trial < 30; ++trial) {
            Store st;
            const int rails = 20 + static_cast<int>(rnd() % 160);
            for (int i = 0; i < rails; ++i) {
                Rail r;
                r.id = i;
                r.is_long = rnd() % 2;
                r.price = 100.0*(1.0 + (r.is_long ? -1.0 : 1.0)*(0.002 + uni()*0.08));
                r.born_ms = static_cast<int64_t>(rnd() % 200)*60000;
                r.end_ms = rnd() % 3 ? 0 : r.born_ms + static_cast<int64_t>(1 + rnd() % 400)*60000;
                const double half = r.price*(0.0005 + uni()*0.004);
                int64_t t = r.born_ms - static_cast<int64_t>(rnd() % 3)*60000;
                for (int k = 0, steps = 1 + static_cast<int>(rnd() % 30); k < steps; ++k) {
                    Step step;
                    step.at_ms = t;
                    step.usd = rnd() % 6 == 0 ? 0.0 : static_cast<double>(1 + rnd() % 9)*1000.0;
                    step.expected = rnd() % 5 ? step.usd*0.1 : -1;
                    step.expected_1h = step.expected;
                    step.expected_4h = step.expected;
                    step.zone_lo = r.price - half;
                    step.zone_hi = r.price + half;
                    add_step(r, step);
                    t += static_cast<int64_t>(1 + rnd() % 60)*60000;
                }
                st.apply_record("cfg", r);
            }
            for (int view = 0; view < 20; ++view) {
                double vmax = 0;
                const int64_t asof = static_cast<int64_t>(rnd() % 700)*60000;
                const double t_lo = static_cast<double>(rnd() % 600)*60000 - 3e6, t_hi = t_lo + 1e6 + uni()*4e7;
                const auto segs = segments(st.rails(), asof, t_lo, t_hi, 0, 1000, vmax, colours[view % 4]);
                const double gap = view % 3 == 0 ? 0 : uni()*3;
                const auto fast = runs(segs, gap), plain = plain_runs(segs, gap);
                bool same = fast.size() == plain.size();
                for (size_t i = 0; same && i < fast.size(); ++i) {
                    const Run &x = fast[i], &y = plain[i];
                    same = x.t0 == y.t0 && x.t1 == y.t1 && x.price == y.price && x.v == y.v && x.rep == y.rep &&
                           x.members == y.members && x.lo == y.lo && x.hi == y.hi && x.near == y.near && x.usd == y.usd &&
                           x.e1 == y.e1 && x.e4 == y.e4 && x.e24 == y.e24;
                }
                ++sweeps;
                if (!same) ++sweep_mismatch;
            }
        }
        CHECK(sweeps == 600 && sweep_mismatch == 0);
    }

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("liqmap2_rails_test: ok\n");
    return 0;
}
