#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// cascade_detector.h - liquidation cascade chains from the observed @forceOrder
// tail (Phase 2 S1 of docs/UPGRADE-ROADMAP.md, "جنگ افزار لیکوییدیشن").
//
// A cascade chain is a run of SAME-SIDE observed liquidations whose consecutive
// events are at most kCascadeGapMs (2s) apart; chains of kMinEvents (3+) are
// cascades. The detector is incremental: the chart feeds every observed
// liquidation exactly once, in arrival order, and the finished chains come out
// through take_finished() (alerting) and chains() (amber band rendering).
//
// Memory is bounded: finished chains older than kRetentionMs (10min) fall out
// of the ring, so panning around a long session can keep drawing bands without
// the list growing with the session. Rewinds reset the detector outright —
// the observed-events tail it was built from is gone at that point anyway.
//
// Self-host note (EDGEDEPTH_PTHREADS=OFF): this runs on the main thread, fed
// from the StreamManager liquidation dispatch, so no locking is needed.
// ═══════════════════════════════════════════════════════════════════════════════
#include <algorithm>
#include <cstdint>
#include <deque>

struct CascadeChain {
    int64_t start_ms = 0, end_ms = 0;
    double lo_price = 0.0, hi_price = 0.0;
    int count = 0;
    bool buy_side = false;  // force-BUY liquidations (shorts forced out)
    bool open = true;       // still accepting events
    bool alerted = false;
    double span_s() const { return double(end_ms - start_ms) / 1000.0; }
};

class CascadeDetector {
public:
    static constexpr int64_t kCascadeGapMs = 2000;
    static constexpr int kMinEvents = 3;
    static constexpr int64_t kRetentionMs = 10 * 60 * 1000;

    // Feed one observed liquidation (arrival order). Same-side events within
    // the gap extend the open chain; anything else (opposite side, gap, or a
    // first event) closes what was open and starts a new chain. Closed chains
    // with kMinEvents+ events become finished cascades: queued for the next
    // take_finished() and retained for rendering.
    void observe(int64_t ts_ms, double price, bool is_buy) {
        if (ts_ms <= 0) return;
        if (open_ && is_buy == open_chain_.buy_side &&
            ts_ms - open_chain_.end_ms <= kCascadeGapMs) {
            open_chain_.end_ms = std::max(open_chain_.end_ms, ts_ms);
            open_chain_.hi_price = std::max(open_chain_.hi_price, price);
            open_chain_.lo_price = open_chain_.lo_price > 0.0
                ? std::min(open_chain_.lo_price, price) : price;
            ++open_chain_.count;
            return;
        }
        close_open();
        open_chain_ = CascadeChain{ts_ms, ts_ms, price, price, 1, is_buy, true, false};
        open_ = true;
    }

    // Close the open chain when it has gone quiet for longer than the gap.
    // Call once per frame with the display's as-of clock so a cascade that
    // simply ended (no follow-up event) still alerts and renders.
    void sweep(int64_t now_ms) {
        if (open_ && now_ms - open_chain_.end_ms > kCascadeGapMs) close_open();
    }

    // One closed cascade per call, in the order they closed.
    bool take_finished(CascadeChain& out) {
        if (alerts_.empty()) return false;
        out = alerts_.front();
        alerts_.pop_front();
        return true;
    }

    // Finished cascades (retention-bounded, oldest first) for the band render.
    const std::deque<CascadeChain>& chains() const { return chains_; }

    // The chain still accepting events (live band while it builds up).
    const CascadeChain* open_chain() const { return open_ ? &open_chain_ : nullptr; }

    void reset() {
        chains_.clear();
        alerts_.clear();
        open_ = false;
        open_chain_ = {};
    }

private:
    void close_open() {
        if (!open_) return;
        open_chain_.open = false;
        if (open_chain_.count >= kMinEvents) {
            chains_.push_back(open_chain_);
            if (!open_chain_.alerted) alerts_.push_back(open_chain_);
            const int64_t oldest_allowed = open_chain_.end_ms - kRetentionMs;
            while (!chains_.empty() && chains_.front().end_ms < oldest_allowed)
                chains_.pop_front();
        }
        open_ = false;
    }

    std::deque<CascadeChain> chains_;  // finished cascades, for the bands
    std::deque<CascadeChain> alerts_;  // closed cascades not yet alerted
    CascadeChain open_chain_{};
    bool open_ = false;
};
