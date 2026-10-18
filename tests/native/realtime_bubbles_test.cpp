#include "core/realtime_bubbles.h"
#include "core/realtime_history.h"
#include "core/realtime_bubble_size.h"
#include <cstdio>
#include <limits>

int main() {
    int failures = 0;
    auto expect = [&](bool ok, const char* message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    using History = RealtimeBubbleHistory;
    auto snapshot = [](const History& history, int64_t from, int64_t to, int64_t as_of) {
        std::vector<History::Bubble> result;
        history.for_each(from, to, as_of, [&](const auto& bubble) { result.push_back(bubble); });
        return result;
    };
    auto equal = [](const auto& a, const auto& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].timestamp_ms != b[i].timestamp_ms || a[i].price != b[i].price ||
                a[i].buy != b[i].buy || a[i].sell != b[i].sell || a[i].count != b[i].count) return false;
        return true;
    };
    auto trade = [](double price, double qty, int64_t timestamp, bool buy,
                    int64_t id = 0, double low = 0, double high = 0, uint64_t count = 0) {
        Terminal::Trade t{};
        t.price = price; t.qty = qty; t.timestamp_ms = timestamp; t.is_buy = buy;
        t.agg_trade_id = id; t.summary_low = low; t.summary_high = high; t.summary_count = count;
        return t;
    };
    History history;
    history.append(trade(100, 2, 1001, true), 1050);
    history.append(trade(100, 1, 1099, false), 1090);
    history.append(trade(101, 1, 1099, true), 1090);
    history.advance(1189);
    expect(history.size() == 0, "wait for the complete window and delivery settling before first display");
    history.advance(1190);
    const auto original = snapshot(history, 0, 2000, 1190);
    expect(original.size() == 2 && original[0].timestamp_ms == 1050 && original[0].price == 100 &&
        original[0].buy == 200 && original[0].sell == 100 && original[0].count == 2 &&
        original[1].price == 101 && original[1].timestamp_ms == 1050,
        "fixed midpoint, exact separate prices and conserved side totals");
    expect(snapshot(history, 0, 2000, 1189).empty(), "paused as-of view cannot expose later publications");
    history.append(trade(100, 100000, 1099, false), 1191);
    history.append(trade(110, 1, 1100, true), 1191);
    history.advance(1291);
    expect(history.late_records == 1 && equal(original, snapshot(history, 0, 1099, 1291)),
        "late larger trades cannot move, resize, recolour or replace completed bubbles");
    expect(history.size() == 3, "adjacent window closes independently");
    expect(equal(original, snapshot(history, 0, 2000, 1190)), "paused chart stays identical while capture continues");
    history.advance(1100);
    history.advance(1291);
    expect(equal(original, snapshot(history, 0, 1099, 1291)), "clock corrections do not destroy frozen groups");

    History dense;
    for (int bin = 0; bin < 600; ++bin) {
        const int64_t start = 10000 + bin * 100;
        // A rapid reversal path with ten prices and both aggressors at each price.
        for (int price = 0; price < 10; ++price) {
            const double p = 100 + (bin % 20 < 10 ? bin % 10 : 19 - bin % 20) * 10 + price;
            dense.append(trade(p, 2, start + price, true), start + 20);
            dense.append(trade(p, 1, start + 99 - price, false), start + 20);
        }
        dense.advance(start + 120);
    }
    const auto before = snapshot(dense, 0, 100000, 100000);
    expect(before.size() == 4800 && dense.filtered_groups == 1200,
        "dense history keeps a fixed per-window budget beyond the former global 1500 cap");
    for (const auto& bubble : before) {
        const int bin = int((bubble.timestamp_ms - 10050) / 100);
        const double base = 100 + (bin % 20 < 10 ? bin % 10 : 19 - bin % 20) * 10;
        expect(bubble.price >= base + 2 && bubble.price <= base + 9 && bubble.buy == 2 * bubble.price &&
            bubble.sell == bubble.price && bubble.count == 2 && bubble.timestamp_ms % 100 == 50,
            "selection is local to each window and never averages across a reversal");
    }
    for (int i = 0; i < 2000; ++i) dense.append(trade(1000.0 + i, 100000, 80000 + i * 100, true), 280000);
    dense.advance(280100);
    expect(equal(before, snapshot(dense, 0, 70000, 280100)), "thousands of new larger bubbles cannot evict old ones");
    const auto zoomed = snapshot(dense, 25000, 29999, 280100);
    std::vector<History::Bubble> expected_zoom;
    for (const auto& bubble : before) if (bubble.timestamp_ms >= 25000 && bubble.timestamp_ms < 30000) expected_zoom.push_back(bubble);
    expect(equal(zoomed, expected_zoom) && equal(before, snapshot(dense, 0, 70000, 280100)),
        "zoom and pan are read-only queries of the identical publications");
    dense.advance(10000 + History::retention_ms + 100);
    expect(snapshot(dense, 0, 10099, 2000000).empty() && !snapshot(dense, 10100, 10199, 2000000).empty(),
        "only the explicit 30 minute time retention expires old publications");

    History summaries;
    summaries.append(trade(100, 20, 1001, true, 0, 98, 103, 10), 1000);
    summaries.append(trade(100, 20, 1001, true, 0, 100, 100, 10), 1000);
    summaries.append(trade(std::numeric_limits<double>::infinity(), 1, 1001, true), 1000);
    summaries.advance(1100);
    const auto exact = snapshot(summaries, 0, 2000, 2000);
    expect(summaries.approximate_records == 10 && exact.size() == 1 && exact[0].price == 100 &&
        exact[0].buy == 2000 && exact[0].count == 10,
        "multi-price summaries cannot create imaginary exact-price bubbles; exact summaries remain valid");
    History bounded;
    for (size_t i = 0; i <= History::max_pending_groups; ++i)
        bounded.append(trade(100.0 + i, 1, 1001, true), 1000);
    expect(bounded.overflow_records == 1, "pending capture has a hard bounded budget");
    bounded.advance(1100);
    expect(bounded.size() == 8 && bounded.filtered_groups == History::max_pending_groups - 8,
        "a pathological burst publishes only its stable local selection");
    bounded.append(trade(100, 1, 1101, true), 1100);
    bounded.advance(1200);
    expect(bounded.size() == 9 && bounded.overflow_records == 1, "publication releases pending capacity");
    bounded.reset();
    bounded.append(trade(100, 1, 1001, true), 1000);
    bounded.advance(1100);
    expect(bounded.size() == 1 && bounded.filtered_groups == 0 && bounded.overflow_records == 0,
        "explicit reset starts a clean traversal, including old timestamps");
    {   // BTC groups many small fills; an alt can have one fill per group.
        History btc, alt;
        std::deque<Terminal::Trade> btc_raw, alt_raw;
        for (int bin = 0; bin < 100; ++bin) {
            const int64_t at = 10000 + bin * 100;
            for (int fill = 0; fill < 200; ++fill) {
                auto t = trade(80000, 0.001 * (bin % 4 + 1), at + 1, fill % 2);
                btc.append(t, at + 20); btc_raw.push_back(t);
            }
            auto t = trade(0.1, 1000 * (bin % 4 + 1), at + 1, true);
            alt.append(t, at + 20); alt_raw.push_back(t);
            btc.advance(at + 120); alt.advance(at + 120);
        }
        RealtimeBubbleScale btc_groups, alt_groups, old_btc, old_alt;
        btc_groups.update(btc, 20100); alt_groups.update(alt, 20100);
        old_btc.update(btc_raw, 20100); old_alt.update(alt_raw, 20100);
        expect(btc_groups.settled() && std::abs(btc_groups.minimum() - 48000) < 0.01 &&
               old_btc.minimum() == 240, "busy BTC calibrates grouped dollars, not individual fill dollars");
        expect(alt_groups.settled() && alt_groups.minimum() == old_alt.minimum() && alt_groups.minimum() == 300,
               "sparse altcoin retains its own smaller reference and existing visibility");
        expect(bubble_size::radius(64000, old_btc.minimum()) == 24 &&
               bubble_size::radius(64000, btc_groups.minimum()) < 4,
               "ordinary busy-market groups no longer saturate the enlarged size cap");
        expect(bubble_size::radius(48000 * 64, btc_groups.minimum()) == 18 &&
               bubble_size::radius(300 * 64, alt_groups.minimum()) == 18,
               "equally exceptional groups stand out equally on BTC and altcoins");
        const auto btc_before = snapshot(btc, 0, 30000, 20100);
        btc_groups.update(btc, 19000);
        expect(btc_groups.minimum() == 48000, "rewind recalibrates using only already published groups");
        expect(equal(btc_before, snapshot(btc, 0, 30000, 20100)), "calibration does not alter captured quantities or publications");
        History late;
        for (int bin = 0; bin < 40; ++bin) late.append(trade(100, 2, 1000 + bin * 100, true), 9000);
        late.advance(9100);
        RealtimeBubbleScale asof;
        asof.update(late, 9000);
        expect(!asof.settled() && asof.minimum() == 0, "future publications cannot calibrate a paused chart");
        asof.update(late, 9100);
        expect(asof.settled() && asof.minimum() == 200, "late groups calibrate once actually published");
        late.append(trade(100, 20000, 9201, true), 9220); late.advance(9320);
        asof.update(late, 9320);
        expect(asof.minimum() == 200, "one new print cannot jump a settled reference");
        RealtimeBubbleScale expired;
        expired.update(late, 80000);
        expect(expired.minimum() == 0, "expired groups do not calibrate a new reference");
        History quiet;
        quiet.append(trade(0.01, 1000, 1001, true), 1050); quiet.advance(1150);
        RealtimeBubbleScale quiet_scale;
        quiet_scale.update(quiet, 1150);
        expect(!quiet_scale.settled(), "sparse market waits for a stable initial sample");
        quiet_scale.update(quiet, 6150);
        expect(quiet_scale.settled() && quiet_scale.minimum() == 10, "sparse market can settle without 32 groups");
    }
    {   // Option A (2026-10-17): a quiet start, a sustained 100x burst, then quiet again.
        History tape;
        RealtimeBubbleScale follow;
        int64_t ms = 1000, clock = 0;
        const auto step = [&](double qty) {   // one $100-price group per 100ms bin, then a chart frame
            tape.append(trade(100, qty, ms + 1, true), ms + 20);
            tape.advance(ms + 120);
            clock = ms + 120;
            follow.update(tape, clock);
            ms += 100;
        };
        while (ms < 7000) step(2);                    // $200 groups
        expect(follow.settled() && follow.minimum() == 200, "a quiet start settles on its own groups");
        while (ms < 11000) step(200);                 // $20,000 groups
        expect(follow.minimum() > 200 && follow.minimum() < 800, "a burst eases the reference instead of jumping it");
        while (ms < 51000) step(200);
        const double eased = follow.minimum();
        expect(bubble_size::radius(20000, eased) < 9, "ordinary burst groups shrink back once the reference follows");
        expect(bubble_size::radius(20000 * 64, eased) > 18, "a 64x outlier still stands out inside the burst");
        follow.update(tape, clock);
        follow.update(tape, clock);
        expect(follow.minimum() == eased, "a paused chart neither measures nor eases");
        while (ms < 120000) step(2);
        expect(follow.minimum() < 2000, "the reference relaxes once the burst leaves the minute");
        follow.update(tape, 60000);
        expect(follow.minimum() > 0 && follow.minimum() <= 20000, "rewind recalibrates on published groups only");
    }
    if (!failures) std::puts("PASS: immutable fixed-window bubbles, an eased market reference, burst selection, pause, zoom, late data and retention");
    return failures ? 1 : 0;
}
