#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// realtime_dom_ladder.h - the real-time chart's attached depth ladder.
//
// Owned by the chart and drawn beside its plot, from the RealtimeDOMFrame the
// plot publishes that same frame: one immutable sampled book, one as-of clock
// and the absolute price-to-screen transform, so every row sits on the chart's
// own price grid. It never reads the live book. Because it is part of the
// chart window, it is shown and hidden with the chart; a separate DOM widget
// used to follow a docked, hidden real-time chart and show nothing.
// ═══════════════════════════════════════════════════════════════════════════════
#include "ui/realtime_dom_frame.h"
#include "core/symbol_metadata.h"
#include <unordered_map>
#include <vector>

class RealtimeDomLadder {
public:
    // Call after the chart's plot has rendered this frame, with the mono font
    // pushed. A close button in the header clears `open`.
    void render(const RealtimeDOMFrame& frame, const PriceFormatter& fmt, bool* open);
    // Width that keeps the price and four compact quantity columns readable.
    static float preferred_width();

    // Flash rows whose resting size just jumped or was eaten (the depth-diff
    // animation). Chart-owned toggle, persisted in the workspace ("rt_dom_diff").
    bool diff_animation = true;

private:
    struct Row { double bid = 0, ask = 0, buy = 0, sell = 0; };
    std::vector<Row> rows_;
    std::vector<double> depth_sizes_;   // visible rows' depth, for the bar scale
    bool display_usd_ = false;

    // ── Depth-diff animation state, per ladder bucket ────────────────────
    struct CellDiff {
        double bid = 0, ask = 0;             // last resting sizes seen this bucket
        float bid_flash = 0, ask_flash = 0;  // 1 → 0 decay, seconds of life left
        float bid_kind = 0, ask_kind = 0;    // +1 added, -1 eaten
        double bid_diff = 0, ask_diff = 0;   // signed size change that fired
        bool bid_slow = false, ask_slow = false; // eaten walls fade slower
    };
    std::unordered_map<int64_t, CellDiff> diffs_;
    double diffs_bucket_size_ = 0;           // regrouping invalidates the map
    int sweep_frame_ = 0;                    // periodic GC of scrolled-out buckets
};
