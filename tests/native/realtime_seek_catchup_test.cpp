// Pack seek catch-up and real-time bubbles.
//
// A full pack seek replays recorded trades from the opening seed to the target
// while the clock holds at the target. Every record then "arrives" at the
// target, so no 100 ms window settles by arrival and the whole catch-up waits
// in pending capture. On the TUT welcome excerpt a seek to 07:10:45 queues
// about 90,000 exact-price groups; past the 65,536 cap every new group was
// dropped, which drew a gap from 07:10:34.4 to the target (founder report,
// 2026-10-18). The engine now publishes each window as soon as delivery has
// passed it (RealtimeBubbleHistory::publish_through); this test drives the
// history the way PackReplayEngine::deliver_due_frames does.
#include "core/realtime_bubbles.h"
#include <cstdio>
#include <map>
#include <vector>

namespace {

using History = RealtimeBubbleHistory;

Terminal::Trade trade(double price, double qty, int64_t timestamp, bool buy) {
    Terminal::Trade t{};
    t.price = price; t.qty = qty; t.timestamp_ms = timestamp; t.is_buy = buy;
    return t;
}

std::vector<History::Bubble> snapshot(const History& history, int64_t from, int64_t to, int64_t as_of) {
    std::vector<History::Bubble> result;
    history.for_each(from, to, as_of, [&](const auto& bubble) { result.push_back(bubble); });
    return result;
}

bool equal(const std::vector<History::Bubble>& a, const std::vector<History::Bubble>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].timestamp_ms != b[i].timestamp_ms || a[i].price != b[i].price ||
            a[i].buy != b[i].buy || a[i].sell != b[i].sell || a[i].count != b[i].count) return false;
    return true;
}

// The TUT excerpt's shape: 75 s from the pack start to the target, ten windows
// a second, about 125 exact prices per window (measured mean 125.4).
constexpr int64_t kStart = 1786259370000;   // 2026-08-09 07:09:30 UTC
constexpr int64_t kTarget = 1786259445000;  // 07:10:45 UTC
constexpr int kPricesPerWindow = 125;

std::vector<Terminal::Trade> tut_catch_up() {
    std::vector<Terminal::Trade> trades;
    for (int64_t bin = kStart; bin < kTarget; bin += History::interval_ms) {
        const int w = int((bin - kStart) / History::interval_ms);
        for (int i = 0; i < kPricesPerWindow; ++i) {
            // In time order inside the window; sizes vary so the top-8 selection is real.
            const double price = 0.80 + 0.0001 * ((w * 7 + i) % 400);
            trades.push_back(trade(price, 1 + (i * 37 + w) % 97, bin + i * History::interval_ms / kPricesPerWindow, i % 3 != 0));
        }
    }
    return trades;
}

// Mirrors deliver_due_frames during seek priming: the clock (cutoff) holds at
// the target, every 32 frames prepare_replay runs update(clock) (advance) and,
// with the fix, publish_through(next undelivered frame less the settle margin).
void deliver_catch_up(History& history, const std::vector<Terminal::Trade>& trades, bool publish) {
    for (size_t i = 0; i < trades.size(); ++i) {
        if ((i & 31) == 0) {
            history.advance(kTarget);
            if (publish) history.publish_through(trades[i].timestamp_ms - History::settle_ms, kTarget);
        }
        history.append(trades[i], kTarget);  // arrival is the held clock
    }
}

}  // namespace

int main() {
    int failures = 0;
    auto expect = [&](bool ok, const char* message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };

    const auto trades = tut_catch_up();
    const size_t windows = size_t((kTarget - kStart) / History::interval_ms);
    expect(windows * kPricesPerWindow > History::max_pending_groups,
           "the fixture queues more exact-price groups than pending capture holds");

    {   // The reported failure, kept as a control: a held clock with no catch-up
        // publication overflows the cap and drops every later window.
        History held;
        deliver_catch_up(held, trades, false);
        held.advance(kTarget + History::interval_ms);
        held.advance(kTarget + 2 * History::interval_ms);
        expect(held.overflow_records > 0, "control: held catch-up overflows pending capture");
        expect(snapshot(held, kTarget - 1000, kTarget, kTarget + 1000).empty(),
               "control: the last second before the target has no bubbles");
    }

    {   // The fix: a seek to 07:10:45 draws every recorded window.
        History seek;
        deliver_catch_up(seek, trades, true);
        expect(seek.overflow_records == 0, "seek catch-up drops no recorded trade over capacity");
        expect(seek.late_records == 0, "in-order catch-up never publishes a window early");
        // The clock leaves the target; the last windows settle as they always did.
        seek.advance(kTarget + History::interval_ms);
        seek.advance(kTarget + 2 * History::interval_ms);

        // Reference: the same trades captured at normal speed, each window
        // published by advance() once the clock has passed it.
        History paced;
        size_t i = 0;
        for (int64_t bin = kStart; bin < kTarget; bin += History::interval_ms) {
            for (; i < trades.size() && trades[i].timestamp_ms < bin + History::interval_ms; ++i)
                paced.append(trades[i], trades[i].timestamp_ms);
            paced.advance(bin + History::interval_ms + History::settle_ms);
        }
        const auto drawn = snapshot(seek, kStart, kTarget, kTarget + 1000);
        const auto expected = snapshot(paced, kStart, kTarget, kTarget + 1000);
        expect(drawn.size() == windows * History::groups_per_bin,
               "every window from the pack start to the target keeps its eight largest groups");
        expect(equal(drawn, expected),
               "catch-up publications are identical to normal-speed capture (same top-8 selection)");
        expect(seek.filtered_groups == paced.filtered_groups,
               "the per-window selection filters exactly the same groups");

        // Windows completed during catch-up are visible at the target itself.
        const auto at_target = snapshot(seek, kStart, kTarget, kTarget);
        expect(at_target.size() >= (windows - 3) * History::groups_per_bin,
               "windows delivered before the target are visible as of the held clock");
        bool covered = true;
        std::map<int64_t, int> per_second;
        for (const auto& b : drawn) per_second[b.timestamp_ms / 1000]++;
        for (int64_t s = kStart / 1000; s < kTarget / 1000; ++s)
            if (per_second[s] != 10 * int(History::groups_per_bin)) covered = false;
        expect(covered, "no second between 07:09:30 and 07:10:45 is missing bubbles");
    }

    {   // publish_through never closes a window that is not fully delivered.
        History h;
        h.advance(5000);  // the held clock
        h.append(trade(100, 1, 1000, true), 5000);
        h.append(trade(101, 1, 1050, true), 5000);
        h.publish_through(1099, 5000);
        expect(h.size() == 0, "a window is open while its last millisecond is undelivered");
        h.append(trade(100, 2, 1099, false), 5000);
        expect(h.late_records == 0, "the window's final millisecond still lands in it");
        h.publish_through(1100, 5000);
        const auto first = snapshot(h, 0, 10000, 5000);
        expect(first.size() == 2 && first[0].price == 100 && first[0].buy == 100 && first[0].sell == 200 &&
               first[0].count == 2 && first[1].price == 101 && first[1].count == 1,
               "the window closes complete once the next undelivered record is at its end");

        h.append(trade(102, 1, 1100, true), 5000);
        h.append(trade(102, 1, 1150, true), 5000);
        h.publish_through(1100, 5000);
        h.publish_through(1199, 5000);
        expect(h.size() == 2, "a record at the boundary opens the next window, which stays open");
        h.append(trade(102, 1, 1199, true), 5000);
        h.publish_through(1200, 5000);
        const auto second = snapshot(h, 1100, 1199, 5000);
        expect(h.late_records == 0 && second.size() == 1 && second[0].count == 3,
               "the next window closes with all three of its records");

        // Never past the clock: frames after it are not delivered while it holds.
        History clocked;
        clocked.advance(1150);
        clocked.append(trade(100, 1, 1120, true), 1150);
        clocked.publish_through(9000, 1150);
        expect(clocked.size() == 0, "a window that ends after the clock stays open");
        clocked.advance(1199);
        clocked.publish_through(9000, 1199);
        expect(clocked.size() == 1, "a window ending at the clock's next millisecond is complete");

        // Out-of-order data after publication is counted, never redrawn.
        History late;
        late.advance(5000);
        late.append(trade(100, 1, 1000, true), 5000);
        late.publish_through(1200, 5000);
        const auto before = snapshot(late, 0, 10000, 5000);
        late.append(trade(100, 50, 1010, true), 5000);
        late.advance(6000);
        expect(late.late_records == 1 && equal(before, snapshot(late, 0, 10000, 6000)),
               "a record for a closed window is counted late and cannot change it");

        // A window genuinely over capacity keeps its overflow accounting.
        History burst;
        burst.advance(5000);
        for (size_t p = 0; p <= History::max_pending_groups; ++p)
            burst.append(trade(100.0 + double(p), 1, 1001, true), 5000);
        burst.publish_through(1100, 5000);
        expect(burst.overflow_records == 1 && burst.size() == History::groups_per_bin,
               "one window wider than the cap still counts its overflow and keeps eight groups");
    }

    if (!failures) std::puts("PASS: seek catch-up publishes every complete window, nothing early, cap and selection kept");
    return failures ? 1 : 0;
}
