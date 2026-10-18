#pragma once
// Real-time trade bubble radius in pixels, for a value against the size minimum. Pure, so the native tests reach it;
// RealtimeBubble::radius (rendering/realtime_bubble.h) forwards here.
//
// Up to 16x the minimum the radius is 3 * sqrt(value / minimum), 3 px to 12 px: the rule since 2026-09-08, so
// ordinary tape keeps its look. Past 16x each doubling adds 3 px up to `cap`, so a price that keeps getting hit by
// ever larger prints keeps getting visibly bigger instead of stopping at 12 px (James 2026-10-14, the Bookmap
// "bubbles get bigger and bigger" read). A cap of 12 is the old rule exactly.
#include <algorithm>
#include <cmath>

namespace bubble_size {

// The three Max size choices in Layers > Trade bubbles (saved as rt_bubble_cap).
inline constexpr float kCompact = 12.0f, kStandard = 24.0f, kLarge = 32.0f;
// A refill level's absorption marker uses the same curve with room to keep growing.
inline constexpr float kAbsorptionCap = 40.0f;

inline float radius(double value, double minimum, float cap = kStandard) {
    if (!(minimum > 0) || !std::isfinite(value) || value < minimum) return 0;
    const double ratio = value / minimum;
    const double r = ratio <= 16.0 ? 3.0 * std::sqrt(ratio) : 12.0 + 3.0 * std::log2(ratio / 16.0);
    return float(std::min<double>(r, cap));
}

// A saved cap is one of the three; anything else reads as the default.
inline float valid_cap(double cap) {
    return cap == double(kCompact) || cap == double(kLarge) ? float(cap) : kStandard;
}

}  // namespace bubble_size
