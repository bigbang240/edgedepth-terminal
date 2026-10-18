#pragma once
// What a refill level has absorbed as of the chart clock: the aggressive value printed into its exact price since
// its episode started (sells into a bid, buys into an ask), summed from the real-time bubble publications, and where
// the latest of those prints sits. Real-time draws one swelling marker per level from this (James 2026-10-14: the
// marker grows as the hits add up and hops right with each new hit).
//
// Publications are immutable 100 ms windows (core/realtime_bubbles.h), published once settled, so a running total
// over windows that are certainly published never needs revising. Each read folds everything the history reports
// stable (RealtimeBubbleHistory::stable_through) and at least kSettledMs behind the clock into the level's settled
// total, and rescans only the rest. The kSettledMs margin covers a print that reaches an empty window late. A clock
// that moves back past what was settled (seek, rewind), a new start, side or price, or another archive or archive
// generation starts again.
//
// Main thread only. No ImGui, so the native test compiles it as is.
#include "core/realtime_bubbles.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace refill {

class Absorption {
public:
    // Prints reach a window up to a few hundred ms after it ends; this leaves headroom for a slow feed.
    static constexpr int64_t kSettledMs = 2000;
    static constexpr size_t kMaxEntries = 64;

    struct Reading {
        double value = 0;        // aggressive quote value into the price, from the start through the clock
        int64_t last_ms = 0;     // the latest window with such a print (its fixed midpoint), 0 when none
        double last_value = 0;   // that window's value into the price
    };

    // `source` and `generation` name the bubble history (the archive and its reset count); `stable` is its
    // stable_through(clock) this frame. `bid`: the level rests on the bid, so sells hit it. `from`/`to`: the episode
    // as drawn at the clock (refill::Span start and end). Half of `tick` is the exact-price tolerance.
    Reading read(const RealtimeBubbleHistory& history, const void* source, uint64_t generation, int64_t stable,
                 const std::string& id, bool bid, double price, double tick, int64_t from, int64_t to, int64_t clock) {
        if (source != source_ || generation != generation_) {
            count_ = 0;
            source_ = source;
            generation_ = generation;
        }
        ++reads_;
        const int64_t end = std::min(to, clock);
        if (!(tick > 0) || end < from) return {};
        Entry* e = nullptr;
        for (size_t i = 0; i < count_ && !e; ++i)
            if (entries_[i].id == id) e = &entries_[i];
        if (e && (e->from != from || e->bid != bid || e->price != price || e->through > end)) e->reset(from, bid, price);
        if (!e) {
            if (count_ < kMaxEntries) {
                e = &entries_[count_++];
            } else {
                e = &entries_[0];
                for (size_t i = 1; i < count_; ++i)
                    if (entries_[i].used < e->used) e = &entries_[i];
            }
            e->id = id;
            e->reset(from, bid, price);
        }
        e->used = reads_;
        const double half = tick * 0.5;
        const auto fold = [&](Reading& into, int64_t after, int64_t through) {
            history.for_each(after + 1, through, clock, [&](const RealtimeBubbleHistory::Bubble& b) {
                const double v = bid ? b.sell : b.buy;
                if (!(v > 0) || std::abs(b.price - price) >= half) return;
                into.value += v;
                if (b.timestamp_ms >= into.last_ms) {
                    into.last_ms = b.timestamp_ms;
                    into.last_value = v;
                }
            });
        };
        const int64_t settle = std::min({end, clock - kSettledMs, stable});
        if (settle > e->through) {
            fold(e->settled, e->through, settle);
            e->through = settle;
        }
        Reading out = e->settled;
        if (end > e->through) fold(out, e->through, end);
        return out;
    }

    void clear() { count_ = 0; }
    size_t size() const { return count_; }

private:
    struct Entry {
        std::string id;
        bool bid = true;
        double price = 0;
        int64_t from = 0, through = 0;   // `settled` covers windows in [from, through]
        Reading settled;
        uint64_t used = 0;
        void reset(int64_t start, bool on_bid, double at) {
            bid = on_bid;
            price = at;
            from = start;
            through = start - 1;
            settled = {};
        }
    };
    std::array<Entry, kMaxEntries> entries_{};
    size_t count_ = 0;
    const void* source_ = nullptr;
    uint64_t generation_ = 0, reads_ = 0;
};

}  // namespace refill
