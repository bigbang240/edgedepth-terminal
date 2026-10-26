#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// whale_trail.h - [SELF-HOST] link consecutive large prints into visible paths.
//
// Public perp feeds carry no order ids, so a whale cannot be followed by
// identity. What IS visible: a large participant working an entry leaves a
// sequence of big clips on one side within seconds of each other. The trail
// chains those prints greedily - same side, small time gap, small price step -
// and the renderer draws the path over the depth heat, so "is this one big
// trader or many small ones" reads at a glance.
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
    double peak_value = 0;   // largest clip in the chain (halo target)
    bool  buy = false;
};

// Greedy same-side linkage: a print joins the most recent chain it fits
// (time gap <= window_ms, price step <= max_step_ticks * tick_size) and
// otherwise opens a new one. Single-print chains are dropped by the caller's
// render (a lone large print is already drawn as its own bubble).
inline void build(const std::vector<Node>& prints, int64_t window_ms,
                  double max_step_ticks, double tick_size, std::vector<Chain>& out) {
    out.clear();
    if (prints.empty() || !(tick_size > 0)) return;
    const double max_step = std::max(0.0, max_step_ticks) * tick_size;
    out.reserve(prints.size() / 4 + 1);
    for (const auto& p : prints) {
        bool linked = false;
        if (max_step > 0) {
            for (auto& c : out) {
                const Node& last = c.nodes.back();
                if (c.buy == p.buy && p.ts_ms > last.ts_ms &&
                    p.ts_ms - last.ts_ms <= window_ms &&
                    std::fabs(p.price - last.price) <= max_step) {
                    c.nodes.push_back(p);
                    c.peak_value = std::max(c.peak_value, p.value);
                    linked = true;
                    break;
                }
            }
        }
        if (!linked) {
            Chain c;
            c.nodes.push_back(p);
            c.peak_value = p.value;
            c.buy = p.buy;
            out.push_back(std::move(c));
        }
    }
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](const Chain& c) { return c.nodes.size() < 2; }),
              out.end());
}

}  // namespace whale_trail
