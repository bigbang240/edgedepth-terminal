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
    r.price = 100 - id;
    r.born_ms = born;
    r.steps.push_back({step, usd, 0.2, usd/5, r.price - 0.1, r.price + 0.1});
    return r;
}

static int64_t h_ms(int64_t hours) { return hours*3600000; }

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
        const auto rs = runs(segments(g.rails(), h_ms(1), 0, h_ms(2), 0, 1, vmax, Colour::dollars));
        double total = 0;
        for (const auto& r : rs) total += r.usd;
        CHECK(rs.size() == 2);
        CHECK(std::abs(total - 12060) < 1e-6);
        bool has7 = false, has12 = false;
        for (const auto& r : rs) { has7 = has7 || r.rep->id == 7; has12 = has12 || r.rep->id == 12; }
        CHECK(has7 && has12);
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

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("liqmap2_rails_test: ok\n");
    return 0;
}
