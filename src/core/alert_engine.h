#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// alert_engine.h - threshold alert queues over the feeds the client already
// observes (Phase 2 S2 of docs/UPGRADE-ROADMAP.md, "جنگ افزار لیکوییدیشن").
//
// The chart filters by the user's dollar threshold, feeds qualifying events
// exactly once, and drains the queues once per frame into the shared
// toast + browser Notification + beep path the price alerts and the cascade
// detector (S1) already use.
//
// - Big liquidation: one observed @forceOrder at/above the threshold. Exchanges
//   split large liquidations into prints a few hundred ms apart, so same-side
//   repeats within kDedupeMs stay quiet: a $2M fill must not fire six toasts.
// - Wall pull: rt_walls::Tracker already classifies a pulled wall (>= 70% of
//   its largest size gone within 1s while less than 25% traded). The chart
//   scans the tracker's pull ring with an end-time watermark, so each pull
//   enters the queue exactly once; the first scan after enable/reset
//   fast-forwards the watermark so pre-toggle pulls never burst as stale.
//
// Memory is bounded: both queues cap at kMaxQueued (the per-frame drain keeps
// them at zero anyway). Rewinds and stream unsubscribes reset outright.
// Self-host note (EDGEDEPTH_PTHREADS=OFF): everything runs on the main thread.
// ═══════════════════════════════════════════════════════════════════════════════
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>

struct LiqAlertEvent {
    int64_t ts_ms = 0;
    double price = 0.0;
    double usd = 0.0;
    bool buy = false;  // force-BUY: shorts forced out
};

struct WallAlertEvent {
    int64_t ts_ms = 0;
    double price = 0.0;
    double vanished = 0.0;  // base quantity that left the level
    bool bid = true;
};

class AlertEngine {
public:
    static constexpr int64_t kDedupeMs = 2000;
    static constexpr size_t kMaxQueued = 32;

    void observe_liquidation(const int64_t ts_ms, const double price,
                             const double usd, const bool is_buy) {
        if (ts_ms <= 0) return;
        if (last_liq_ms_ > 0 && is_buy == last_liq_side_ &&
            ts_ms - last_liq_ms_ <= kDedupeMs) return;  // split fill: stay quiet
        last_liq_ms_ = ts_ms;
        last_liq_side_ = is_buy;
        liq_.push_back(LiqAlertEvent{ts_ms, price, usd, is_buy});
        while (liq_.size() > kMaxQueued) liq_.pop_front();
    }

    void observe_wall_pull(const int64_t ts_ms, const double price,
                           const double vanished, const bool bid) {
        if (ts_ms <= 0) return;
        pulls_.push_back(WallAlertEvent{ts_ms, price, vanished, bid});
        while (pulls_.size() > kMaxQueued) pulls_.pop_front();
    }

    bool take_liq_alert(LiqAlertEvent& out) {
        if (liq_.empty()) return false;
        out = liq_.front();
        liq_.pop_front();
        return true;
    }

    bool take_wall_alert(WallAlertEvent& out) {
        if (pulls_.empty()) return false;
        out = pulls_.front();
        pulls_.pop_front();
        return true;
    }

    void reset() {
        liq_.clear();
        pulls_.clear();
        last_liq_ms_ = 0;
        last_liq_side_ = false;
    }

private:
    std::deque<LiqAlertEvent> liq_;
    std::deque<WallAlertEvent> pulls_;
    int64_t last_liq_ms_ = 0;
    bool last_liq_side_ = false;
};
