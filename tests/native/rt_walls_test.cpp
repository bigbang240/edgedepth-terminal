// Walls and pulled walls (core/rt_walls.h) on synthetic books and trades.
#include "core/rt_walls.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <utility>
#include <vector>

namespace {
using Sample = RealtimeDepthHistory::Sample;

// A book around mid 1000 on a 1.0 tick: bids 999 down to 980, asks 1001 up
// to 1020, 10 each, with `extra` sizes replacing those levels. The touch is
// 999..995 and 1001..1005 (five levels a side).
Sample book(int64_t t, std::vector<std::pair<double, double>> extra = {}, bool segment_start = false,
            double deepest_bid = 980, double bid = 999, double ask = 1001) {
    Sample s;
    s.timestamp_ms = t; s.bid = bid; s.ask = ask; s.segment_start = segment_start;
    auto size_at = [&](double price) {
        for (const auto& [p, q] : extra) if (p == price) return q;
        return 10.0;
    };
    for (double p = bid; p >= deepest_bid; p -= 1) if (size_at(p) > 0) s.levels.push_back({p, size_at(p)});
    for (double p = ask; p <= 1020; p += 1) if (size_at(p) > 0) s.levels.push_back({p, size_at(p)});
    return s;
}

Terminal::Trade trade(int64_t t, double price, double qty, bool buy) {
    Terminal::Trade x{};
    x.timestamp_ms = t; x.price = price; x.qty = qty; x.is_buy = buy;
    return x;
}

// Rows of four ticks: near price is 12 ticks of mid.
const rt_walls::Grid kGrid{1.0, 4};
constexpr double kLow = 970, kHigh = 1030;

struct Counts { size_t live = 0, finished = 0, pulls = 0; };
Counts counts(const rt_walls::Tracker& t) {
    Counts c;
    t.for_each_live([&](const rt_walls::Wall&) { ++c.live; });
    t.for_each_finished([&](const rt_walls::Wall&) { ++c.finished; });
    t.for_each_pull([&](const rt_walls::Pull&) { ++c.pulls; });
    return c;
}

// The same book once a second from `from` to `to` inclusive.
void stand(rt_walls::Tracker& t, int64_t from, int64_t to, std::vector<std::pair<double, double>> extra,
           rt_walls::Grid grid = kGrid) {
    for (int64_t ms = from; ms <= to; ms += 1000) t.observe(book(ms, extra), grid, kLow, kHigh);
}
}

int main() {
    int failures = 0;
    auto expect = [&](bool ok, const char* message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    // Trades exist before every interval below, so the trade record covers it.
    const std::deque<Terminal::Trade> quiet{trade(0, 1001, 0.1, true)};

    {   // Detection: 5x the median level beyond the touch, and at least what the touch holds.
        rt_walls::Tracker t;
        t.observe(book(1000, {{990, 200}, {1010, 40}}), kGrid, kLow, kHigh);
        expect(counts(t).live == 1, "one wall at 20x the median; 4x is not a wall");
        bool found = false;
        t.for_each_live([&](const rt_walls::Wall& w) {
            found = w.bid && w.price == 990 && w.size == 200 && w.multiple == 20 && w.start_ms == 1000;
        });
        expect(found, "the wall keeps its exact price, side, size, multiple and start");
        expect(t.median(true) == 10 && t.median(false) == 10, "medians of the levels beyond the touch");
        expect(t.touch(true) == 10 && t.touch(false) == 10, "the touch holds its largest level");
    }
    {   // The touch is never a wall: size at and next to the best price follows the price.
        rt_walls::Tracker t;
        t.observe(book(1000, {{999, 300}, {998, 120}, {997, 80}, {995, 60}, {1001, 250}}), kGrid, kLow, kHigh);
        expect(counts(t).live == 0, "the five levels each side of the touch never start a wall");
    }
    {   // A level beyond the touch that is large against the deep book but smaller than
        // what the touch holds (a market maker's second layer) is not a wall.
        rt_walls::Tracker t;
        t.observe(book(1000, {{999, 120}, {1001, 120}, {994, 80}, {1006, 70}}), kGrid, kLow, kHigh);
        expect(counts(t).live == 0, "8x the median but under the touch's 120 is not a wall");
        t.observe(book(1100, {{999, 120}, {990, 150}}), kGrid, kLow, kHigh);
        expect(counts(t).live == 1, "beyond the touch and larger than it is a wall");
    }
    {   // A BTC-shaped book (0.1 tick): 10 BTC at the touch thinning to tenths a few
        // dollars out, resting orders of 2-6 BTC and one 44 BTC wall.
        const rt_walls::Grid btc{0.1, 10};
        Sample s;
        s.timestamp_ms = 1000; s.bid = 82960.0; s.ask = 82960.1;
        const double touch[] = {10, 4.5, 4, 3.5, 3, 1.4, 1.2, 1.0};
        for (int d = 0; d < 300; ++d) {
            const double near = d < 8 ? touch[d] : 0.2 + 0.1 * (d % 5);
            const double bid = std::round((82960.0 - d * 0.1) * 10) / 10, ask = std::round((82960.1 + d * 0.1) * 10) / 10;
            s.levels.push_back({bid, near + (d == 150 ? 44 : d % 37 == 0 ? 4 : 0)});
            s.levels.push_back({ask, near + (d % 41 == 0 ? 6 : 0)});
        }
        rt_walls::Tracker t;
        t.observe(s, btc, 82920, 83000);
        bool wall = false;
        t.for_each_live([&](const rt_walls::Wall& w) { wall = w.bid && std::abs(w.price - 82945.0) < 1e-6; });
        expect(t.live_count() == 1 && wall, "BTC: only the 44 BTC level is a wall, not the touch or 2-6 BTC resting orders");
    }
    {   // Shown: only after standing three seconds, at most three per side, largest first.
        rt_walls::Tracker t;
        const std::vector<std::pair<double, double>> walls{{993, 100}, {990, 200}, {987, 150}, {985, 300}, {1008, 90}};
        t.observe(book(1000, walls), kGrid, kLow, kHigh);
        std::array<rt_walls::Wall, rt_walls::kMaxShownPerSide * 2> shown{};
        expect(t.shown(1000, kLow, kHigh, shown) == 0, "a new wall is not shown yet");
        stand(t, 2000, 4000, walls);
        const size_t n = t.shown(4000, kLow, kHigh, shown);
        expect(n == 4 && shown[0].price == 985 && shown[1].price == 990 && shown[2].price == 987 && !shown[3].bid,
               "three largest bids then the ask");
        expect(t.shown(4000, 986, kHigh, shown) == 4 && shown[0].price == 990, "shown walls are inside the visible prices");
    }
    {   // Traded away: the size vanishes, but trades at the price account for it.
        rt_walls::Tracker t;
        std::deque<Terminal::Trade> trades = quiet;
        stand(t, 1000, 4000, {{994, 200}});
        trades.push_back(trade(4050, 994, 120, false));
        trades.push_back(trade(4090, 994, 60, false));
        t.observe(book(4100, {{994, 20}}), kGrid, kLow, kHigh);
        expect(t.pending_count() == 1 && counts(t).live == 0, "a 90% drop near mid waits for trades");
        t.settle(4500, trades, 4500);
        expect(t.pending_count() == 1, "undecided until the trade settle time");
        t.settle(4600, trades, 4600);
        const auto c = counts(t);
        expect(c.pulls == 0 && c.finished == 1, "a wall traded away is not pulled");
    }
    {   // Pulled: a wall that stood, gone near price with no trades at its price.
        rt_walls::Tracker t;
        std::deque<Terminal::Trade> trades = quiet;
        trades.push_back(trade(4050, 1001, 5, true));   // at another price
        stand(t, 1000, 4000, {{994, 200}});
        t.observe(book(4100), kGrid, kLow, kHigh);
        t.settle(4600, trades, 4600);
        const auto c = counts(t);
        expect(c.pulls == 1 && c.finished == 0, "a wall gone near price without trades is pulled");
        t.for_each_pull([&](const rt_walls::Pull& p) {
            expect(p.wall.end_ms == 4100 && p.wall.start_ms == 1000 && p.vanished == 190 && p.traded == 0,
                   "the pull keeps its start, pull time and vanished size");
        });
    }
    {   // A level that came and went within three seconds is a passing quote, never a pull.
        rt_walls::Tracker t;
        t.observe(book(1000, {{994, 200}}), kGrid, kLow, kHigh);
        t.observe(book(2500, {{994, 200}}), kGrid, kLow, kHigh);
        t.observe(book(2600), kGrid, kLow, kHigh);
        t.settle(9000, quiet, 9000);
        const auto c = counts(t);
        expect(t.pending_count() == 0 && c.pulls == 0 && c.finished == 1, "a wall younger than three seconds is not pulled");
    }
    {   // Price walking through the book: each touch empties as the market maker requotes, no prints there.
        rt_walls::Tracker t;
        std::deque<Terminal::Trade> trades = quiet;
        for (int k = 0; k < 40; ++k) {
            const double bid = 999 + k, ask = 1001 + k;
            Sample s;
            s.timestamp_ms = 1000 + k * 200; s.bid = bid; s.ask = ask;
            for (int d = 0; d < 20; ++d) {
                s.levels.push_back({bid - d, d < 3 ? 150.0 - d * 40 : 10.0});
                s.levels.push_back({ask + d, d < 3 ? 150.0 - d * 40 : 10.0});
            }
            t.observe(s, kGrid, 970, 1070);
        }
        t.settle(20000, trades, 20000);
        const auto c = counts(t);
        expect(c.live == 0 && c.pulls == 0, "a moving touch makes no walls and no pulls");
    }
    {   // Trades behind the book (a throttled frame delivers depth before the prints
        // that emptied the level): nothing is decided until the trades catch up.
        rt_walls::Tracker t;
        std::deque<Terminal::Trade> delivered = quiet;
        delivered.push_back(trade(3900, 1001, 1, true));
        stand(t, 1000, 4000, {{994, 200}});
        t.observe(book(4100), kGrid, kLow, kHigh);
        t.settle(6000, delivered, 3900);   // clock well past, trades only through 3.9 s
        expect(t.pending_count() == 1 && counts(t).pulls == 0, "a drop waits while trades lag the book");
        delivered.push_back(trade(4080, 994, 185, false));
        delivered.push_back(trade(5900, 1001, 1, true));
        t.settle(6000, delivered, 6000);
        const auto c = counts(t);
        expect(c.pulls == 0 && c.finished == 1, "once delivered, the prints show the wall was traded");
    }
    {   // The same lag with the guard bypassed would have called it pulled.
        rt_walls::Tracker t;
        std::deque<Terminal::Trade> delivered = quiet;
        delivered.push_back(trade(3900, 1001, 1, true));
        stand(t, 1000, 4000, {{994, 200}});
        t.observe(book(4100), kGrid, kLow, kHigh);
        t.settle(6000, delivered, 6000);
        expect(counts(t).pulls == 1, "without the delivery watermark the undelivered prints read as a pull");
    }
    {   // Trades lost before delivery: waiting drops end as plain walls.
        rt_walls::Tracker t;
        stand(t, 1000, 4000, {{994, 200}});
        t.observe(book(4100), kGrid, kLow, kHigh);
        t.trades_lost();
        t.settle(9000, quiet, 9000);
        const auto c = counts(t);
        expect(t.pending_count() == 0 && c.pulls == 0 && c.finished == 1, "lost trades never make a pull");
    }
    {   // A small share traded still reads as a pull (under 25% of what vanished).
        rt_walls::Tracker t;
        std::deque<Terminal::Trade> trades = quiet;
        stand(t, 1000, 4000, {{1007, 200}});
        trades.push_back(trade(4050, 1007, 30, true));
        t.observe(book(4100, {{1007, 5}}), kGrid, kLow, kHigh);
        t.settle(4700, trades, 4700);
        expect(counts(t).pulls == 1, "an ask wall mostly cancelled with a little traded is pulled");
    }
    {   // Far from price: the same vanishing is not flagged.
        rt_walls::Tracker t;
        stand(t, 1000, 4000, {{985, 200}});
        t.observe(book(4100), kGrid, kLow, kHigh);
        expect(t.pending_count() == 0, "a level 1.5% from mid never waits as a pull");
        t.settle(9000, quiet, 9000);
        const auto c = counts(t);
        expect(c.pulls == 0 && c.finished == 1, "a wall far from price is not pulled");
    }
    {   // Without trades covering the interval, a drop cannot be shown untraded.
        rt_walls::Tracker t;
        const std::deque<Terminal::Trade> late{trade(4050, 1001, 1, true)};
        stand(t, 1000, 4000, {{994, 200}});
        t.observe(book(4100), kGrid, kLow, kHigh);
        t.settle(9000, late, 9000);
        expect(counts(t).pulls == 0, "no pull when the trade record starts after the wall's last second");
    }
    {   // A slow fade over more than a second is not a pull; the exit has hysteresis.
        rt_walls::Tracker t;
        t.observe(book(1000, {{994, 200}}), kGrid, kLow, kHigh);
        t.observe(book(1600, {{994, 120}}), kGrid, kLow, kHigh);
        t.observe(book(2200, {{994, 70}}), kGrid, kLow, kHigh);
        t.observe(book(2800, {{994, 45}}), kGrid, kLow, kHigh);
        expect(counts(t).live == 1, "4.5x the median keeps a wall that formed at 5x");
        t.observe(book(3400, {{994, 25}}), kGrid, kLow, kHigh);
        t.settle(9000, quiet, 9000);
        const auto c = counts(t);
        expect(c.live == 0 && c.finished == 1 && c.pulls == 0 && t.pending_count() == 0,
               "below 3x it ends as a plain wall");
        t.for_each_finished([&](const rt_walls::Wall& w) {
            expect(w.end_ms == 3400 && w.peak == 200, "a finished wall keeps its end and peak");
        });
    }
    {   // A depth gap (new segment) restarts the one-second drop test.
        rt_walls::Tracker t;
        stand(t, 1000, 4000, {{994, 200}});
        t.observe(book(4100, {{994, 40}}, true), kGrid, kLow, kHigh);
        expect(t.pending_count() == 0 && counts(t).live == 1, "no pull across a depth gap");
    }
    {   // A price past the deepest observed level cannot be measured: it ends quietly.
        rt_walls::Tracker t;
        stand(t, 1000, 4000, {{990, 200}});
        t.observe(book(4100, {}, false, 992), kGrid, kLow, kHigh);
        t.settle(9000, quiet, 9000);
        const auto c = counts(t);
        expect(t.pending_count() == 0 && c.pulls == 0 && c.finished == 1,
               "a wall that left the observed depth window is never called pulled");
    }
    {   // Regrouping the display changes nothing: a wall is an exact price.
        rt_walls::Tracker t;
        stand(t, 1000, 2000, {{994, 200}});
        stand(t, 3000, 4000, {{994, 200}}, rt_walls::Grid{1.0, 20});
        t.observe(book(4100, {{994, 210}}), kGrid, kLow, kHigh);
        bool same = false;
        t.for_each_live([&](const rt_walls::Wall& w) { same = w.price == 994 && w.start_ms == 1000 && w.size == 210; });
        expect(same && t.pending_count() == 0 && counts(t).live == 1, "a wall keeps its price and start across regrouping");
    }
    {   // Bounds: live walls, finished walls and pulls stay capped.
        rt_walls::Tracker t;
        // 40 walls (300 to 340) against 60 thin levels a side: 32 tracked, the largest.
        Sample s;
        s.timestamp_ms = 1000; s.bid = 999; s.ask = 1001;
        for (double p = 999; p >= 920; --p) s.levels.push_back({p, p >= 975 && p <= 994 ? 300 + p - 975 : 10});
        for (double p = 1001; p <= 1080; ++p) s.levels.push_back({p, p >= 1006 && p <= 1025 ? 300 + p - 1006 : 10});
        t.observe(s, kGrid, 900, 1100);
        double smallest = 1e9;
        t.for_each_live([&](const rt_walls::Wall& w) { smallest = std::min(smallest, w.size); });
        expect(t.live_count() == rt_walls::kMaxActive && smallest >= 304, "live walls are capped, largest kept");
        rt_walls::Tracker p;
        int64_t clock = 1000;
        for (size_t i = 0; i < rt_walls::kMaxPulls + 24; ++i) {
            stand(p, clock, clock + 3000, {{994, 200}});
            p.observe(book(clock + 3100), kGrid, kLow, kHigh);
            p.settle(clock + 3700, quiet, clock + 3700);
            clock += 5000;
        }
        expect(p.pull_count() == rt_walls::kMaxPulls, "the pull ring keeps a bounded history independent of the glyph budget");
        expect(p.ended_count() <= rt_walls::kMaxEnded, "finished walls are capped");
    }
    {   // A significant older pull survives many small arrivals; freshness still has room.
        rt_walls::Tracker t;
        int64_t clock = 1000;
        for (int i = 0; i < 40; ++i) {
            stand(t, clock, clock + 3000, {{994, i == 0 ? 2000.0 : i % 3 == 0 ? 1000.0 : 100.0}});
            t.observe(book(clock + 3100), kGrid, kLow, kHigh);
            t.settle(clock + 3700, quiet, clock + 3700);
            clock += 5000;
        }
        expect(t.pull_count() == 40, "older pulls remain available beyond the old sixteen-event ring");
        std::array<rt_walls::PullMarker, rt_walls::kMaxPullsDrawn> shown{};
        const auto project = [](const rt_walls::Pull& p) {
            return std::array<float, 2>{float(p.wall.end_ms / 200), 100};
        };
        const auto select = [&](int64_t at, float width, auto projection) {
            return rt_walls::select_pulls(t, at, 0, double(clock), kLow, kHigh, width, projection, shown);
        };
        const size_t n = select(clock, 1200, project);
        bool oldest = false, newest = false, small = false, medium = false;
        for (size_t i = 0; i < n; ++i) {
            const auto& m = shown[i];
            oldest |= m.pull->wall.end_ms == 4100 && m.diameter == 14;
            newest |= m.pull->wall.end_ms == 199100;
            small |= m.diameter == 7;
            medium |= m.diameter > 7 && m.diameter < 14;
            expect(m.alpha >= 0.55f && m.alpha <= 0.95f, "age fades without making retained pulls unreadable");
            for (size_t j = 0; j < i; ++j) {
                const float gap = (m.diameter + shown[j].diameter) * 0.5f + 4;
                expect(std::abs(m.x - shown[j].x) >= gap || std::abs(m.y - shown[j].y) >= gap,
                       "variable-size markers do not overlap");
            }
        }
        expect(n == 12 && oldest && newest && small && medium,
               "twelve glyphs balance a large older pull, recent events and bounded visible size differences");
        expect(select(clock, 280, project) == 6, "compact plots reduce the marker budget");
        expect(select(clock, 1200, [](const rt_walls::Pull&) { return std::array<float, 2>{100, 100}; }) == 1 &&
               shown[0].pull->wall.end_ms == 4100, "overlap keeps the significant event instead of the newest small pull");
        expect(select(5000, 1200, project) == 1 && shown[0].pull->wall.end_ms == 4100 && shown[0].diameter == 14,
               "future events affect neither selection nor size at a past clock");
        expect(select(clock + rt_walls::kPullRetentionMs, 1200, project) == 0,
               "old events leave the display after the session history horizon");
        expect(rt_walls::select_pulls(t, clock, 5000, double(clock), 1000, 1010, 1200, project, shown) == 0,
               "out-of-view events do not consume the display budget");
        expect(select(clock, 0, project) == 0, "a collapsed plot has no markers");
    }
    {   // The deepest observed ask level can hold a wall and be measured.
        rt_walls::Tracker t;
        t.observe(book(1000, {{1020, 200}}), kGrid, kLow, kHigh);
        t.observe(book(1100, {{1020, 210}}), kGrid, kLow, kHigh);
        bool held = false;
        t.for_each_live([&](const rt_walls::Wall& w) { held = !w.bid && w.price == 1020 && w.size == 210; });
        expect(held && t.ended_count() == 0, "a wall on the last observed ask level stays measured");
    }
    {   // Fewer than three non-empty levels beyond the touch in view: no median, no walls.
        rt_walls::Tracker t;
        t.observe(book(1000, {{993, 200}}), kGrid, 992.5, 1003);
        expect(t.median(true) == 0 && counts(t).live == 0, "two bid levels in view are too few for a median");
    }
    {   // A thin market: 0.0001 tick, sparse books and trades, small sizes. Near
        // price comes from rows and the minute's mid range, never a fixed percent.
        const rt_walls::Grid thin{0.0001, 1};
        auto thin_book = [](int64_t t, double mid, double wall_price, double wall_size) {
            Sample s;
            // Quotes on the tick grid, as books report them (a bid of mid - tick in floating point can sit
            // a hair below its own level and misclassify it).
            s.timestamp_ms = t; s.bid = std::round((mid - 0.0001) / 0.0001) * 0.0001; s.ask = std::round((mid + 0.0001) / 0.0001) * 0.0001;
            // Ten sparse bid levels: the touch is the first five.
            const double offsets[] = {1, 3, 4, 6, 9, 11, 13, 16, 18, 21};
            const double sizes[] = {150, 120, 200, 300, 180, 90, 110, 140, 100, 130};
            for (int i = 0; i < 10; ++i) {
                const double p = std::round((mid - 0.0001 * offsets[i]) / 0.0001) * 0.0001;
                const bool wall = std::abs(p - wall_price) < 0.00005;
                s.levels.push_back({p, wall ? wall_size : sizes[i]});
            }
            for (int i = 1; i <= 10; ++i) s.levels.push_back({std::round((mid + 0.0001 * i * 2 - 0.0001) / 0.0001) * 0.0001, 100.0 + 20 * i});
            return s;
        };
        // One small print every 5 s, nowhere near the walls.
        std::deque<Terminal::Trade> sparse;
        for (int64_t t = 0; t <= 120000; t += 5000) sparse.push_back(trade(t, 0.0524, 40, true));
        const double view_low = 0.0500, view_high = 0.0540;

        // Mid swings 0.0521 to 0.0534 over the minute: 13 rows of range, wider than 3 rows.
        auto swing = [&](rt_walls::Tracker& t) {
            const double mids[] = {0.0523, 0.0534, 0.0521, 0.0524, 0.0523};
            for (int i = 0; i < 5; ++i) t.observe(thin_book(40000 + i * 4000, mids[i], 0, 0), thin, view_low, view_high);
        };
        auto hold = [&](rt_walls::Tracker& t) {
            for (int64_t ms = 57000; ms <= 61000; ms += 1000) t.observe(thin_book(ms, 0.0523, 0.0512, 4000), thin, view_low, view_high);
        };
        {   // Pulled: 11 ticks from mid, inside the minute's range, gone with no trades.
            rt_walls::Tracker t;
            swing(t);
            hold(t);
            expect(counts(t).live == 1, "a thin book still has walls relative to its own median and touch");
            expect(std::abs(t.near_distance() - 0.0013) < 1e-9, "near price is the minute's mid range when wider than 3 rows");
            t.observe(thin_book(62000, 0.0523, 0, 0), thin, view_low, view_high);   // a second later, no other book
            t.settle(63000, sparse, 63000);
            expect(counts(t).pulls == 1, "thin market: a wall gone near price without trades is pulled");
        }
        {   // Traded away: the same wall, consumed by a sell at its price.
            rt_walls::Tracker t;
            swing(t);
            hold(t);
            std::deque<Terminal::Trade> trades = sparse;
            trades.push_back(trade(61500, 0.0512, 3800, false));
            std::sort(trades.begin(), trades.end(), [](const auto& a, const auto& b) { return a.timestamp_ms < b.timestamp_ms; });
            t.observe(thin_book(62000, 0.0523, 0, 0), thin, view_low, view_high);
            t.settle(63000, trades, 63000);
            const auto c = counts(t);
            expect(c.pulls == 0 && c.finished == 1, "thin market: a wall traded away is not pulled");
        }
        {   // A quiet minute: near price is 3 rows, so the same wall 11 ticks out is not a pull.
            rt_walls::Tracker t;
            hold(t);
            expect(std::abs(t.near_distance() - 0.0003) < 1e-9, "near price falls back to 3 rows");
            t.observe(thin_book(62000, 0.0523, 0, 0), thin, view_low, view_high);
            t.settle(63000, sparse, 63000);
            expect(counts(t).pulls == 0, "outside the near distance a vanishing wall is not a pull");
        }
    }
    {   // Time going backwards (seek) starts a new traversal.
        rt_walls::Tracker t;
        t.observe(book(5000, {{994, 200}}), kGrid, kLow, kHigh);
        t.observe(book(1000), kGrid, kLow, kHigh);
        expect(counts(t).live == 0 && t.ended_count() == 0, "a rewind clears tracked walls");
    }
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::printf("rt_walls_test passed\n");
    return failures ? 1 : 0;
}
