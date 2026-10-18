// Replay draws the V2 rails as published at its clock (hub task liq-map-v2; core/liqmap2_rails.h start_ms, end_at,
// standing). The picture at a clock built from the whole record must equal the picture built from the record cut to
// what was known then: each rail's steps at or before the clock (a rail with none was not yet published), and its end
// only once known, a touch one bar after its stamp (the touching bar's open) and a close at its stamp (the bar's close
// it was made at). A touched rail, a rail born before its first step (a publisher warm-up), stepping rails, a closed
// rail, a ladder whose member is touched and an end with no reason are seen every 5 s and around each event; random
// records are then seen at random clocks.
#include "core/liqmap2_rails.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

using namespace liqmap2_rails;

namespace {
constexpr int64_t kM = 60000, kH = 3600000;

// The record as a viewer had it at asof, from the publisher's timing written out apart from the code under test.
Store cut(const Store& full, int64_t asof) {
    Store out;
    for (const auto& [key, r] : full.rails()) {
        Rail x = r;
        x.steps.clear();
        for (const auto& s : r.steps)
            if (s.at_ms <= asof) x.steps.push_back(s);
        if (x.steps.empty()) continue;  // not published yet
        const int64_t known = r.end == "closed" ? r.end_ms : r.end_ms + kM;
        if (!(r.end_ms > 0 && known <= asof)) {
            x.end_ms = 0;
            x.end.clear();
        }
        out.apply_record(key.first, x);
    }
    return out;
}

struct View {
    double t_lo, t_hi, p_lo, p_hi;
    Colour colour;
};

// What the overlay reads from the record at a clock, by rail id rather than address.
struct Seg {
    int64_t id, t0, t1, step, hover_step;
    double price, v;
    bool operator==(const Seg&) const = default;
};
struct RunRow {
    int64_t rep, t0, t1;
    int members;
    double price, v, lo, hi, near, usd, e1, e4, e24;
    bool operator==(const RunRow&) const = default;
};
struct RailRow {  // the profile, the hover and "data in hand" (rails published by the clock)
    int64_t id, start, end, step;
    bool up;
    std::string reason;
    double usd, expected;
    bool operator==(const RailRow&) const = default;
};
struct Picture {
    double vmax = 0;
    std::vector<Seg> segs;
    std::vector<RunRow> runs;
    std::vector<int64_t> labels;
    std::vector<RailRow> rails;
    bool operator==(const Picture&) const = default;
};

Picture picture(const Store& st, int64_t asof, const View& v) {
    Picture p;
    const auto segs = segments(st.rails(), asof, v.t_lo, v.t_hi, v.p_lo, v.p_hi, p.vmax, v.colour);
    for (const auto& s : segs) {
        // The hover reads the step in force at the hovered time, never past the clock.
        const int64_t mid = s.t0 + (s.t1 - s.t0)/2;
        p.segs.push_back({s.rail->id, s.t0, s.t1, s.step->at_ms, step_at(*s.rail, std::min(asof, mid))->at_ms, s.price, s.v});
    }
    for (const auto& r : runs(segs))
        p.runs.push_back({r.rep->id, r.t0, r.t1, r.members, r.price, r.v, r.lo, r.hi, r.near, r.usd, r.e1, r.e4, r.e24});
    for (const Rail* r : labelled(st.rails(), asof, v.p_lo, v.p_hi, v.colour)) p.labels.push_back(r->id);
    for (const auto& [key, r] : st.rails()) {
        if (r.steps.empty() || start_ms(r) > asof) continue;
        const int64_t end = end_at(r, asof);
        const Step* s = step_at(r, asof);
        p.rails.push_back({r.id, start_ms(r), end, s->at_ms, standing(r, asof), end ? r.end : std::string(), s->usd, s->expected});
    }
    return p;
}

Step step(int64_t at, double usd, double expected, double price) {
    Step s;
    s.at_ms = at;
    s.usd = usd;
    s.chance = expected >= 0 ? 0.2 : -1;
    s.expected = expected;
    s.expected_1h = expected >= 0 ? expected/4 : -1;
    s.expected_4h = expected >= 0 ? expected/2 : -1;
    s.zone_lo = price*0.998;
    s.zone_hi = price*1.002;
    return s;
}

Rail make(int64_t id, bool is_long, double price, int64_t born, const std::vector<std::pair<int64_t, double>>& steps,
          int64_t end_ms = 0, const char* end = "") {
    Rail r;
    r.id = id;
    r.is_long = is_long;
    r.price = price;
    r.born_ms = born;
    for (const auto& [at, usd] : steps) add_step(r, step(at, usd, usd/10, price));
    r.end_ms = end_ms;
    r.end = end;
    return r;
}

// Compares the two pictures at asof over every view; counts the clocks seen and those that differ.
void compare(const Store& full, int64_t asof, const std::vector<View>& views, int& seen, int& differ) {
    const Store known = cut(full, asof);
    for (const auto& v : views) {
        ++seen;
        if (!(picture(full, asof, v) == picture(known, asof, v))) {
            if (differ < 5) std::fprintf(stderr, "differs at %lld\n", static_cast<long long>(asof));
            ++differ;
        }
    }
}
}  // namespace

int main() {
    const int64_t t0 = 10*kH;
    // 1: a long rail touched by the bar opening at 11:37 (known at its close, 11:38).
    // 2: a short rail born at 08:00 in a publisher warm-up, first published at 10:05.
    // 3: a long rail stepping hourly, its later expected unknown (the view falls back to dollars).
    // 4: a short rail closed at 12:15 (stamped at the bar close it was made at).
    // 5, 6: a short ladder (touching zones); 6, its representative, touched at 12:42.
    // 7: a rail born in the warm-up and touched at 13:01. 8: an end with no reason (read as a touch).
    Store full;
    full.apply_record("cfg", make(1, true, 95, t0, {{t0, 400}, {t0 + kH, 500}}, t0 + kH + 37*kM, "touched"));
    full.apply_record("cfg", make(2, false, 106, 8*kH, {{t0 + 5*kM, 300}, {t0 + kH + 5*kM, 350}, {t0 + 2*kH + 5*kM, 200}}));
    {
        Rail r = make(3, true, 90, t0 + 10*kM, {{t0 + 10*kM, 100}, {t0 + kH + 10*kM, 250}, {t0 + 2*kH + 10*kM, 150}});
        Step s = step(t0 + 3*kH + 10*kM, 180, -1, 90);
        add_step(r, s);
        full.apply_record("cfg", r);
    }
    full.apply_record("cfg", make(4, false, 110, t0, {{t0, 600}, {t0 + kH, 700}}, t0 + 2*kH + 15*kM, "closed"));
    full.apply_record("cfg", make(5, false, 103, t0 + 20*kM, {{t0 + 20*kM, 200}, {t0 + kH + 20*kM, 260}}));
    full.apply_record("cfg", make(6, false, 103.4, t0 + 20*kM, {{t0 + 20*kM, 500}, {t0 + kH + 20*kM, 520}},
                                  t0 + 2*kH + 42*kM, "touched"));
    full.apply_record("cfg", make(7, true, 97, 9*kH, {{t0, 800}, {t0 + kH, 900}, {t0 + 2*kH, 950}}, t0 + 3*kH + kM, "touched"));
    full.apply_record("cfg", make(8, true, 92, t0 + 30*kM, {{t0 + 30*kM, 120}}, t0 + 2*kH + 20*kM));
    CHECK(full.rails().size() == 8);

    const std::vector<View> views = {
        {static_cast<double>(7*kH), static_cast<double>(16*kH), 0, 1000, Colour::expected},
        {static_cast<double>(7*kH), static_cast<double>(16*kH), 0, 1000, Colour::dollars},
        {static_cast<double>(t0 + kH), static_cast<double>(t0 + 3*kH), 100, 1000, Colour::expected_1h},
        {static_cast<double>(t0 + 2*kH + 30*kM), static_cast<double>(t0 + 2*kH + 50*kM), 0, 1000, Colour::expected_4h},
    };
    int seen = 0, differ = 0;
    for (int64_t asof = 7*kH; asof <= 16*kH; asof += 5000) compare(full, asof, views, seen, differ);
    // Around every event: a birth, a step, a stamp and the moment it is known, to the millisecond.
    for (const auto& [key, r] : full.rails()) {
        std::vector<int64_t> at{r.born_ms};
        for (const auto& s : r.steps) at.push_back(s.at_ms);
        if (r.end_ms > 0) {
            at.push_back(r.end_ms);
            at.push_back(r.end_ms + kM);
        }
        for (int64_t t : at)
            for (int64_t d : {-1000, -1, 0, 1, 1000, 30000}) compare(full, t + d, views, seen, differ);
    }
    CHECK(seen > 7000);
    CHECK(differ == 0);

    // What the equality means, at a few clocks.
    const auto& rails = full.rails();
    const Rail& touched = rails.at({"cfg", 1});
    const Rail& warm = rails.at({"cfg", 2});
    const Rail& closed = rails.at({"cfg", 4});
    const int64_t touch = t0 + kH + 37*kM;
    // Inside the touching minute the touch is not known: the rail stands and draws to the clock (the chart's own
    // candles end it the moment price reaches it); once the bar has closed it ends at the bar's open.
    CHECK(standing(touched, touch + 30000) && end_at(touched, touch + 30000) == 0);
    CHECK(standing(touched, touch + kM - 1) && !standing(touched, touch + kM));
    CHECK(end_at(touched, touch + kM) == touch && end_known_ms(touched) == touch + kM);
    double vmax = 0;
    const auto inside = segments(rails, touch + 30000, 0, 1e18, 94, 96, vmax);
    CHECK(inside.size() == 2 && inside.back().t1 == touch + 30000);
    const auto after = segments(rails, touch + 2*kM, 0, 1e18, 94, 96, vmax);
    CHECK(after.size() == 2 && after.back().t1 == touch);
    // Born in the warm-up: nothing before its first step, then a line from that step, never from its birth.
    CHECK(start_ms(warm) == t0 + 5*kM && !standing(warm, t0) && standing(warm, t0 + 5*kM));
    CHECK(segments(rails, t0 + 4*kM, 0, 1e18, 105, 107, vmax).empty());
    const auto first = segments(rails, t0 + kH, 0, 1e18, 105, 107, vmax);
    CHECK(first.size() == 1 && first[0].t0 == t0 + 5*kM && first[0].t1 == t0 + kH);
    // A close is known when stamped: the line stops at the stamp, as the publisher's next message showed it.
    const int64_t close_at = t0 + 2*kH + 15*kM;
    CHECK(end_known_ms(closed) == close_at && standing(closed, close_at - 1) && !standing(closed, close_at));
    const auto shut = segments(rails, close_at + 1000, 0, 1e18, 109, 111, vmax);
    CHECK(shut.size() == 2 && shut.back().t1 == close_at);
    // An end with no reason waits a bar, as a touch.
    CHECK(end_known_ms(rails.at({"cfg", 8})) == t0 + 2*kH + 21*kM);
    // Labels: only rails standing at the clock (the touched one is still labelled inside its touching minute).
    bool labelled_inside = false;
    for (const Rail* r : labelled(rails, touch + 30000, 0, 1000, Colour::dollars)) labelled_inside |= r->id == 1;
    CHECK(labelled_inside);
    for (const Rail* r : labelled(rails, touch + kM, 0, 1000, Colour::dollars)) CHECK(r->id != 1);
    for (const Rail* r : labelled(rails, t0, 0, 1000, Colour::dollars)) CHECK(r->id != 2);

    // Random records: births on the 5-minute grid, a third first published later (warm-up), steps on the minute grid,
    // touches stamped at a minute's open from the first step on, closes at a 5-minute bar close, some ends with no
    // reason; seen at random clocks and around random rails' events, over random views and colours.
    uint64_t seed = 0x2545f4914f6cdd1dULL;
    const auto rnd = [&seed]() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
    const Colour colours[] = {Colour::expected_1h, Colour::expected_4h, Colour::expected, Colour::dollars};
    int rseen = 0, rdiffer = 0;
    for (int trial = 0; trial < 40; ++trial) {
        Store st;
        std::vector<int64_t> events;
        const int n = 10 + static_cast<int>(rnd() % 70);
        for (int i = 0; i < n; ++i) {
            Rail r;
            r.id = i + 1;
            r.is_long = rnd() % 2;
            r.price = 100.0*(1.0 + (r.is_long ? -1.0 : 1.0)*(0.002 + static_cast<double>(rnd() % 800)/10000.0));
            r.born_ms = t0 + static_cast<int64_t>(rnd() % 120)*5*kM;
            const int64_t first_at = rnd() % 3 ? r.born_ms : r.born_ms + static_cast<int64_t>(1 + rnd() % 40)*5*kM;
            int64_t at = first_at;
            for (int k = 0, steps = 1 + static_cast<int>(rnd() % 8); k < steps; ++k) {
                const double usd = rnd() % 7 == 0 ? 0.0 : static_cast<double>(1 + rnd() % 9)*1000.0;
                Step s = step(at, usd, rnd() % 5 ? usd/10 : -1, r.price);
                add_step(r, s);
                at += static_cast<int64_t>(1 + rnd() % 90)*kM;
            }
            switch (rnd() % 4) {
                case 0:
                    r.end_ms = first_at + static_cast<int64_t>(rnd() % 400)*kM;
                    r.end = "touched";
                    break;
                case 1:
                    r.end_ms = (first_at/(5*kM) + 1 + static_cast<int64_t>(rnd() % 80))*5*kM;
                    r.end = "closed";
                    break;
                case 2:
                    if (rnd() % 2) r.end_ms = first_at + static_cast<int64_t>(rnd() % 400)*kM;  // no reason
                    break;
                default:
                    break;
            }
            events.push_back(r.born_ms);
            events.push_back(first_at);
            if (r.end_ms > 0) {
                events.push_back(r.end_ms);
                events.push_back(r.end_ms + kM);
            }
            st.apply_record("cfg", r);
        }
        for (int c = 0; c < 60; ++c) {
            const int64_t asof = c % 2 ? t0 - kH + static_cast<int64_t>(rnd() % (24*3600))*1000
                                       : events[rnd() % events.size()] + static_cast<int64_t>(rnd() % 121000) - 60000;
            const double lo = static_cast<double>(t0) + static_cast<double>(rnd() % (20*60))*kM - 2.0*kH;
            const std::vector<View> v = {
                {lo, lo + static_cast<double>(1 + rnd() % (16*60))*kM, 0, 1000, colours[rnd() % 4]},
                {lo, lo + static_cast<double>(1 + rnd() % (16*60))*kM, rnd() % 2 ? 100.0 : 0.0, rnd() % 2 ? 100.0 : 1000.0,
                 colours[rnd() % 4]},
            };
            compare(st, asof, v, rseen, rdiffer);
        }
    }
    CHECK(rseen == 40*60*2);
    CHECK(rdiffer == 0);

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("liqmap2_rails_asof_test: ok (%d hand-made and %d random pictures)\n", seen, rseen);
    return 0;
}
