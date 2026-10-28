#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// whale_trail.h - [SELF-HOST] link consecutive large prints into visible paths.
//
// Public perp feeds carry no order ids, so a whale cannot be followed by
// identity. What IS visible: a large participant working an entry leaves a
// sequence of big clips on one side within seconds of each other. The trail
// chains those prints - same side, small time gap, small price step - and the
// renderer draws the path over the depth heat, so "is this one big trader or
// many small ones" reads at a glance.
//
// v2 linkage rules (hardened over the v1 greedy first-fit):
//   1. BEST-FIT, not first-fit: a print joins the chain with the lowest
//      continuation score - 0.75 x (price step / max step) + 0.25 x (time gap
//      / window). When two working orders overlap in time, each print now
//      continues the trail it is actually closest to instead of whichever
//      chain happened to be opened first.
//   2. VALUE GATE: a clip only extends a chain when it reaches
//      min_join_fraction of that chain's peak clip. Medium traffic between
//      two whales can no longer hijack their trails (it may still open its
//      own). A working order typically iceberg-clips at comparable sizes;
//      15% of peak keeps the tail of a fading order attached while dropping
//      unrelated flow.
//   3. SPAN CAP: a chain may not grow past max_span_ms between its first and
//      newest node. Without a cap a "trail" could survive minutes by chaining
//      coincidental prints - after the cap it reads as what it is: separate
//      events.
// Chains expose total_value / span_ms so the renderer can scale line weight
// with the trail's quoted volume and fade older segments toward the newest.
//
// Stateless per frame: the visible window's records chain deterministically
// (prints arrive in time order from the archive), so panning cannot reshuffle
// history. Kept allocation-light: a few hundred prints per view, chains in a
// flat vector.
// ═══════════════════════════════════════════════════════════════════════════════
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace whale_trail {

struct Node {
    int64_t ts_ms = 0;
    double  price = 0;
    bool    buy = false;     // taker side of the clip
    double  value = 0;       // quote notional of the grouped clip
};

struct Chain {
    std::vector<Node> nodes;
    double  peak_value = 0;  // largest clip in the chain (halo target)
    double  total_value = 0; // quoted notional of every linked clip (line weight)
    int64_t span_ms = 0;     // first -> newest node age (fade + cap bookkeeping)
    bool    buy = false;
};

struct Options {
    double  min_join_fraction = 0.15;  // clip >= this x chain peak to extend it
    int64_t max_span_ms = 90000;       // hard cap on first->newest chain age
};

// Best-fit same-side linkage: a print joins the chain it scores best against
// (time gap <= window_ms, price step <= max_step_ticks * tick_size, value
// gate, span cap) and otherwise opens a new one. Single-print chains are
// dropped at the end (a lone large print is already drawn as its own bubble).
inline void build(const std::vector<Node>& prints, int64_t window_ms,
                  double max_step_ticks, double tick_size, std::vector<Chain>& out,
                  const Options& opts = {}) {
    out.clear();
    if (prints.empty() || !(tick_size > 0)) return;
    const double max_step = std::max(0.0, max_step_ticks) * tick_size;
    const double inv_window = window_ms > 0 ? 1.0 / double(window_ms) : 0.0;
    out.reserve(prints.size() / 4 + 1);
    for (const auto& p : prints) {
        Chain*  best = nullptr;
        double  best_score = 0;
        if (max_step > 0) {
            for (auto& c : out) {
                if (c.buy != p.buy) continue;
                const Node& last = c.nodes.back();
                const int64_t gap = p.ts_ms - last.ts_ms;
                if (gap <= 0 || gap > window_ms) continue;              // stale or unordered
                if (p.ts_ms - c.nodes.front().ts_ms > opts.max_span_ms) continue;  // span cap
                const double dist = std::fabs(p.price - last.price);
                if (dist > max_step) continue;                          // price step
                if (p.value < opts.min_join_fraction * c.peak_value) continue;     // value gate
                const double score = 0.75 * (dist / max_step) + 0.25 * (double(gap) * inv_window);
                if (!best || score < best_score) { best = &c; best_score = score; }
            }
        }
        if (best) {
            best->nodes.push_back(p);
            best->peak_value = std::max(best->peak_value, p.value);
            best->total_value += p.value;
            best->span_ms = p.ts_ms - best->nodes.front().ts_ms;
        } else {
            Chain c;
            c.nodes.push_back(p);
            c.peak_value = p.value;
            c.total_value = p.value;
            c.buy = p.buy;
            out.push_back(std::move(c));
        }
    }
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](const Chain& c) { return c.nodes.size() < 2; }),
              out.end());
}

}  // namespace whale_trail
