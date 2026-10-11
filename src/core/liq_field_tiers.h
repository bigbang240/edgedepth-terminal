#pragma once
#include "core/liq_field_brackets.h"
#include "core/liq_field_venue_caps.h"
#include <array>
#include <cstdint>
#include <string_view>

namespace liq_field {
inline constexpr std::string_view kModelVersion = "lf.v2";
inline constexpr std::array<double, 6> kLeverages{5, 10, 25, 50, 75, 100};

template <size_t N>
inline double find_cap(const std::array<LeverageCap, N>& caps, std::string_view symbol) {
    size_t lo = 0, hi = caps.size();
    while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2;
        if (caps[mid].symbol < symbol) lo = mid + 1;
        else hi = mid;
    }
    return lo < caps.size() && caps[lo].symbol == symbol ? caps[lo].leverage : 0;
}

// Binance matches the pinned Go model (July 2026 brackets). Hyperliquid and Bybit use the
// 2026-10-02 venue snapshots, a display rule only: backend research evaluates Binance alone.
// Unknown symbols keep the legacy uncapped assumptions, identified in the layer details.
inline double max_leverage(std::string_view exchange, std::string_view symbol) {
    if (exchange == "binancef") return find_cap(kLeverageCaps, symbol);
    if (exchange == "hl") return find_cap(kHlLeverageCaps, symbol);
    if (exchange == "bybit") return find_cap(kBybitLeverageCaps, symbol);
    return 0;
}
inline const char* cap_snapshot(std::string_view exchange) {
    return exchange == "binancef" ? "July 2026" : exchange == "hl" || exchange == "bybit" ? "October 2026" : "";
}

struct TierSelection { uint8_t enabled = 0; uint8_t floor = 0; };
inline TierSelection select_tiers(uint8_t mask, double cap) {
    TierSelection out;
    for (size_t i = 0; i < kLeverages.size(); ++i)
        if ((mask & (1u << i)) && (cap <= 0 || kLeverages[i] <= cap))
            out.enabled |= static_cast<uint8_t>(1u << i);
    int remaining = 3;
    for (int i = 5; i >= 0 && remaining; --i) {
        if (out.enabled & (1u << i)) {
            out.floor |= static_cast<uint8_t>(1u << i);
            --remaining;
        }
    }
    return out;
}

// Display rule, not the pinned Go statistic: when every selected tier exceeds the market's cap,
// draw every tier the market allows instead of a blank map (228 of 648 Binance markets in the
// 2026-09-29 touch-odds roster cap below the default 25x). Deselecting every tier stays blank.
inline TierSelection display_tiers(uint8_t mask, double cap) {
    const auto t = select_tiers(mask, cap);
    return (t.enabled == 0 && mask != 0 && cap > 0) ? select_tiers(0x3f, cap) : t;
}
}
