#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// rt_event_feed.h - the Layer-events widget's data (self-host): a bounded ring
// of the real-time layer events the chart already detects. Cascade chains,
// threshold liquidations, pulled walls, consumed refill levels.
//
// The feed is passive: it collects whatever the toast/Notification alert
// toggles hold, so the widget stays a complete recent-history view. Newest
// first, capped at kMax; every push copies a short pre-formatted line.
//
// Rewinds and stream changes clear it outright (reset_realtime_extras).
// Self-host note (EDGEDEPTH_PTHREADS=OFF): everything runs on the main thread,
// so the feed needs no lock.
// ═══════════════════════════════════════════════════════════════════════════════
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>

struct RtEvent {
    enum class Kind : uint8_t { cascade, liq, wall, refill };
    int64_t ts_ms = 0;
    Kind kind = Kind::liq;
    bool bull = false;      // colour direction: squeeze up / resistance gone
    double usd = 0.0;       // headline notional; 0 when not meaningful
    char text[96] = "";     // description without the symbol prefix
};

class RtEventFeed {
public:
    static constexpr size_t kMax = 24;

    void push(const int64_t ts_ms, const RtEvent::Kind kind, const bool bull,
              const double usd, const char* text) {
        if (ts_ms <= 0 || !text || !*text) return;
        RtEvent e;
        e.ts_ms = ts_ms;
        e.kind = kind;
        e.bull = bull;
        e.usd = usd > 0 ? usd : 0.0;
        std::snprintf(e.text, sizeof(e.text), "%s", text);
        events_.push_front(e);
        while (events_.size() > kMax) events_.pop_back();
    }

    template <class F> void for_each(F&& f) const {
        for (const auto& e : events_) f(e);
    }
    [[nodiscard]] bool empty() const { return events_.empty(); }
    void reset() { events_.clear(); }

private:
    std::deque<RtEvent> events_;
};
