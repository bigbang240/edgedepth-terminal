#pragma once
#include "market_data_history.h"
#include "market_context.h"

// Pure view rules shared by rendering and native regressions. No requests or ImGui.
namespace market_data_view {
inline constexpr size_t chart_count = 6;
inline constexpr std::array<size_t, chart_count> series{0, 1, 3, 2, 4, 5};
// A chart may lead to a related research reading, never silently equate
// different windows or populations. Top-trader account history has no exact
// counterpart in the registry's position-weighted top-trader reading.
inline constexpr std::array<const char*, chart_count> research_fields{
    "feature.ret_15m,feature.ret_1h", "feature.oi_contract_change_1h",
    "feature.oi_mcap_ratio", "feature.global_long_ratio", "", "feature.taker_buy_ratio_15m"};
inline size_t first_observation(const MarketDataHistory& h, int hours) {
    return std::lower_bound(h.times.begin(), h.times.end(), double(h.to - hours * 3600000LL) / 1000) - h.times.begin();
}
// Baseline terminal UI is 16 * 1.15 logical pixels, independent of raster DPI.
inline float text_scale(float ui_pixels) { return ui_pixels / 18.4f; }
inline int columns(float width, float ui_pixels) {
    const float scale = text_scale(ui_pixels);
    return width >= 1200 * scale ? 3 : width >= 760 * scale ? 2 : 1;
}
struct Inspection {
    int64_t pin = 0, released = 0;
    int chart = -1, hover_chart = -1;
    size_t hover = 0;
    void clear_hover() { hover_chart = -1; }
    void unpin() { pin = 0; clear_hover(); released = 0; }
    void point(const MarketDataHistory& h, size_t index, int owner) {
        pin = static_cast<int64_t>(h.times[index] * 1000);
        chart = owner; clear_hover(); released = 0;
    }
    bool reconcile(const MarketDataHistory& h, size_t first) {
        if (!pin) return false;
        const auto it = std::lower_bound(h.times.begin() + first, h.times.end(), double(pin) / 1000);
        if (it != h.times.end() && *it == double(pin) / 1000) return false;
        released = pin; pin = 0; clear_hover(); return true;
    }
    size_t effective(const MarketDataHistory& h, size_t first) const {
        if (pin) return h.nearest(double(pin) / 1000, first);
        return hover_chart >= 0 ? std::clamp(hover, first, h.times.size() - 1) : h.times.size() - 1;
    }
    void step(const MarketDataHistory& h, size_t first, int direction, int owner) {
        const auto index = effective(h, first);
        point(h, direction < 0 ? (index > first ? index - 1 : first) : std::min(index + 1, h.times.size() - 1), owner);
    }
};
struct Bounds {
    double low = INFINITY, high = -INFINITY;
    bool available() const { return std::isfinite(low) && std::isfinite(high); }
    void add(double value) {
        if (std::isfinite(value)) { low = std::min(low, value); high = std::max(high, value); }
    }
    void pad(bool account = false) {
        if (!available()) { low = 0; high = 1; return; }
        const double span = std::max(high - low, std::max(std::abs(high) * .01, 1e-9));
        if (account && low - span * .6 <= 50 && high + span * .6 >= 50) add(50);
        low -= span * .12; high += span * .12;
        if (account) { low = std::max(0.0, low); high = std::min(100.0, high); }
    }
};
struct ChartFit {
    Bounds primary, price;
    bool available = false;
};
inline std::array<ChartFit, chart_count> fit(const MarketDataHistory& h, size_t first, bool quote_oi) {
    std::array<ChartFit, chart_count> result{};
    for (size_t chart = 0; chart < chart_count; ++chart) {
        auto& f = result[chart];
        const auto& values = chart == 1 && quote_oi ? h.notional : h.values[series[chart]];
        for (size_t i = first; i < h.times.size(); ++i) {
            f.primary.add(values[i]);
            if (chart == 2) f.price.add(h.values[0][i]);
            if (chart == 5) f.primary.add(h.values[6][i]);
        }
        f.available = f.primary.available() || (chart == 2 && f.price.available());
        if (chart == 5 && f.primary.available()) {
            const double max = std::max(f.primary.high * 1.12, 1.0);
            f.primary = {-max, max};
        } else f.primary.pad(chart == 3 || chart == 4);
        f.price.pad();
    }
    return result;
}
// Missing/withdrawn is distinct from an expired previously supplied observation.
// A new response may withhold an otherwise unexpired item after a parity failure.
enum class ContextState { Current, Expired, Withdrawn, Missing };
inline ContextState context_state(const MarketContext::Item& latest, const MarketContext::Item& last, int64_t now) {
    if (latest.current(now)) return ContextState::Current;
    const auto& item = latest.at ? latest : last;
    if (!item.at) return ContextState::Missing;
    return now >= item.until ? ContextState::Expired : ContextState::Withdrawn;
}
}
