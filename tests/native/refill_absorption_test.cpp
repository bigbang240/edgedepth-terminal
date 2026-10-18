// Absorption bubbles: the trade bubble size curve and the running total a refill level has absorbed.
#include "core/realtime_bubble_size.h"
#include "core/refill_absorption.h"
#include <cmath>
#include <cstdio>
#include <string>

int main() {
    int failures = 0;
    auto expect = [&](bool ok, const char* message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    auto near = [](double a, double b) { return std::abs(a - b) < 1e-4; };

    // ── Size curve ──────────────────────────────────────────────────────────
    using bubble_size::radius;
    expect(radius(99, 100) == 0, "below the minimum draws nothing");
    expect(radius(100, 0) == 0 && radius(100, -1) == 0, "no minimum draws nothing");
    expect(radius(NAN, 100) == 0 && radius(INFINITY, 100) == 0, "non-finite values draw nothing");
    expect(near(radius(100, 100), 3) && near(radius(400, 100), 6) && near(radius(1600, 100), 12),
           "up to 16x the minimum the 2026-09-08 rule holds");
    expect(near(radius(3200, 100), 15) && near(radius(6400, 100), 18) && near(radius(12800, 100), 21),
           "past 16x each doubling adds 3 px");
    expect(near(radius(25600, 100), 24) && near(radius(1e9, 100), 24), "the standard cap is 24 px");
    expect(near(radius(1e9, 100, bubble_size::kCompact), 12) && near(radius(1600, 100, bubble_size::kCompact), 12),
           "the compact cap is the old 12 px rule");
    expect(near(radius(1e9, 100, bubble_size::kLarge), 32), "the large cap is 32 px");
    expect(near(radius(1e9, 100, bubble_size::kAbsorptionCap), 40), "absorption markers stop at 40 px");
    bool monotonic = true, continuous = true;
    float previous = 0;
    for (double v = 100; v < 1e7; v *= 1.01) {
        const float r = radius(v, 100, bubble_size::kAbsorptionCap);
        if (r < previous) monotonic = false;
        if (previous > 0 && r - previous > 0.1f) continuous = false;
        previous = r;
    }
    expect(monotonic, "radius never shrinks as value grows");
    expect(continuous, "no jump where the curve changes at 16x");
    expect(bubble_size::valid_cap(12) == 12 && bubble_size::valid_cap(32) == 32 && bubble_size::valid_cap(24) == 24,
           "saved caps read back");
    expect(bubble_size::valid_cap(17) == 24 && bubble_size::valid_cap(-1) == 24 && bubble_size::valid_cap(NAN) == 24,
           "anything else reads as the default");

    // ── Absorption ──────────────────────────────────────────────────────────
    using History = RealtimeBubbleHistory;
    auto trade = [](double price, double qty, int64_t timestamp, bool buy) {
        Terminal::Trade t{};
        t.price = price; t.qty = qty; t.timestamp_ms = timestamp; t.is_buy = buy;
        return t;
    };
    // A bid at 100 hit by sells every 500 ms from 10 s, $1,000 more each time; buys at the same price and sells one
    // tick away never count. Prints arrive 40 ms after they happen.
    const int64_t start = 10000;
    auto feed = [&](History& h) {
        for (int i = 0; i < 40; ++i) {
            const int64_t t = start + i * 500;
            h.append(trade(100, 10 + i * 10, t + 10, false), t + 50);  // $1,000 .. $40,000 sold into the bid
            h.append(trade(100, 5, t + 20, true), t + 60);             // buys at the price: not into a bid
            h.append(trade(100.1, 50, t + 30, false), t + 70);         // a tick away
        }
    };
    auto full = [&](const History& h, int64_t from, int64_t to, int64_t clock) {
        refill::Absorption::Reading r;
        h.for_each(from, std::min(to, clock), clock, [&](const History::Bubble& x) {
            if (x.sell > 0 && std::abs(x.price - 100) < 0.05) {
                r.value += x.sell;
                if (x.timestamp_ms >= r.last_ms) { r.last_ms = x.timestamp_ms; r.last_value = x.sell; }
            }
        });
        return r;
    };
    auto same = [](const refill::Absorption::Reading& x, const refill::Absorption::Reading& y) {
        return std::abs(x.value - y.value) < 1e-6 && x.last_ms == y.last_ms && std::abs(x.last_value - y.last_value) < 1e-6;
    };
    const int a = 0, b = 0, c = 0;

    // Publications keep up with the clock, as they do live: every read equals a full rescan.
    History history;
    feed(history);
    refill::Absorption absorption;
    bool incremental_matches = true, grows = true;
    double previous_value = 0;
    for (int64_t clock = start; clock <= start + 21000; clock += 100) {
        history.advance(clock);
        const auto got = absorption.read(history, &a, 1, history.stable_through(clock), "bid-100", true, 100, 0.1,
                                         start, INT64_MAX, clock);
        if (!same(got, full(history, start, INT64_MAX, clock))) incremental_matches = false;
        if (got.value < previous_value) grows = false;
        previous_value = got.value;
    }
    expect(incremental_matches, "the running total equals a full rescan at every clock");
    expect(grows, "the total only grows while the clock moves forward");
    const int64_t done = start + 21000;
    auto read = [&](refill::Absorption& r, const History& h, const void* src, uint64_t gen, const char* id, bool bid,
                    int64_t from, int64_t to, int64_t clock) {
        return r.read(h, src, gen, h.stable_through(clock), id, bid, 100, 0.1, from, to, clock);
    };
    const auto end = read(absorption, history, &a, 1, "bid-100", true, start, INT64_MAX, done);
    expect(near(end.value, 820000), "sells into the bid sum to $820,000 (1k + 2k + ... + 40k)");
    expect(end.last_ms == start + 39 * 500 + 50 && near(end.last_value, 40000),
           "the latest hit is the last window, so the marker hops right with each hit");

    // Publications lag the clock by seconds (a stalled main thread): nothing is settled before it is published.
    History lagging;
    feed(lagging);
    refill::Absorption lagged;
    bool lag_matches = true;
    for (int64_t clock = start; clock <= done; clock += 100) {
        if (clock % 5000 == 0) lagging.advance(clock);
        const auto got = read(lagged, lagging, &c, 1, "bid-100", true, start, INT64_MAX, clock);
        if (!same(got, full(lagging, start, INT64_MAX, clock))) lag_matches = false;
    }
    lagging.advance(done + 1000);
    const auto caught_up = read(lagged, lagging, &c, 1, "bid-100", true, start, INT64_MAX, done + 1000);
    expect(lag_matches, "late publications still match a full rescan");
    expect(near(caught_up.value, 820000), "late publications are counted once they arrive");

    // Paused: a reader behind the history's clock does not see later publications, and a clock moved back reads as
    // of that clock.
    const auto paused = read(absorption, history, &a, 1, "bid-100", true, start, INT64_MAX, start + 5000);
    expect(same(paused, full(history, start, INT64_MAX, start + 5000)), "a clock moved back reads as of that clock");
    expect(paused.last_ms <= start + 5000, "nothing after the clock is counted");
    expect(history.stable_through(start + 5000) < start + 5000, "publications after the reader's clock are not stable");

    // A level that ended: its total stops at its end, wherever the clock is. Windows are dated by their midpoint,
    // so the hit at end + 10 ms (window midpoint end + 50) is after it.
    const auto ended = read(absorption, history, &a, 1, "bid-ended", true, start, start + 2000, done);
    expect(same(ended, full(history, start, start + 2000, done)) && near(ended.value, 10000),
           "an ended level stops at its end (1k + .. + 4k)");

    // An ask at 100 counts buys only.
    const auto ask = read(absorption, history, &a, 1, "ask-100", false, start, INT64_MAX, done);
    expect(near(ask.value, 40 * 5 * 100), "an ask counts the buys into it");

    // A restarted episode (new start) counts from that start only.
    const auto later = read(absorption, history, &a, 1, "bid-100", true, start + 10000, INT64_MAX, done);
    expect(same(later, full(history, start + 10000, INT64_MAX, done)), "a new start counts from that start only");

    // An archive reset (new generation) or another archive starts every level again.
    const auto reset = read(absorption, history, &a, 2, "bid-100", true, start, INT64_MAX, done);
    expect(same(reset, full(history, start, INT64_MAX, done)) && absorption.size() == 1,
           "a new generation drops the old totals");
    read(absorption, history, &b, 2, "bid-100", true, start, INT64_MAX, done);
    expect(absorption.size() == 1, "another archive drops the old totals");

    // Bounded: the least recently read level makes room.
    for (int i = 0; i < 70; ++i)
        read(absorption, history, &b, 2, ("level-" + std::to_string(i)).c_str(), true, start, INT64_MAX, done);
    expect(absorption.size() == refill::Absorption::kMaxEntries, "at most kMaxEntries levels are kept");

    // Nothing to read: no tick, or a clock before the start.
    expect(absorption.read(history, &b, 2, done, "x", true, 100, 0, start, INT64_MAX, done).value == 0,
           "no tick reads nothing");
    expect(read(absorption, history, &b, 2, "y", true, start, INT64_MAX, start - 1).value == 0,
           "a clock before the start reads nothing");

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::printf("refill_absorption_test: ok\n");
    return failures ? 1 : 0;
}
