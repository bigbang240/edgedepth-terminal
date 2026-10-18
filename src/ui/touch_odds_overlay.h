#pragma once
#include "types/types.h"
#include "core/liq_rails.h"
#include "core/touch_zones.h"
#include <cstdint>
#include <functional>
// Heatmap V2 touch odds and the P9 cascade tier on live Binance charts, admin preview.
// Data comes only from the web host's window.__EDGEDEPTH_REACH__ (see core/touch_odds.h).
namespace touch_odds_overlay {
// True once the web host has published (it mounts the bridge for admins only).
bool available(const Terminal::Pair& pair);
// Whether render() draws the cascade chip (plot y 58 to 80) for this pair, so other notes stack below it.
bool chip_visible(const Terminal::Pair& pair, bool replay);
// End of the current forecast window for this pair, or 0; the chart reserves future margin to it.
int64_t window_end(const Terminal::Pair& pair, int64_t clock);
// observed_allowed: this viewer may switch the rails' fuel to observed Hyperliquid positions (admin, HL chart).
// rails_off: why the touch-odds rails cannot draw on this chart (another layer holds the forward area, or no fuel
// stands under them), or null; the menu then says so instead of offering switches that change nothing.
void menu(const Terminal::Pair& pair, bool& enabled, bool observed_allowed = false, const char* rails_off = nullptr);
// The rails fuel choice from menu(): observed Hyperliquid positions instead of the scenario map.
bool observed_fuel_selected();
// The 20-minute bands, the cascade chip and the cursor readout. rails_drawn: the chart draws the touch-odds rails
// this frame, and the bands give them the forward area.
void render(const Terminal::Pair& pair, int64_t clock, const char* price_fmt, bool replay, bool rails_drawn);
// Whether the liquidation rails layer draws for this pair now; the scenario map then leaves the
// forward area to the rails, and zone chips give way to rail labels.
bool rails_active(const Terminal::Pair& pair, int64_t clock, bool replay);
// Rails, or the locked panel that stands in for them: either way the forward area is the overlay's.
bool owns_forward_area(const Terminal::Pair& pair, int64_t clock, bool replay);
// Liquidation rails: fuel from any source times calibrated touch odds, ahead of now (core/liq_rails.h).
// The caption and tooltip name the source.
void rails(const Terminal::Pair& pair, int64_t clock, const char* price_fmt,
           const liq_rails::FuelSource& fuel, bool replay = false);
// Instead of rails(), when the chart draws the admin V2 rails: no forward columns this frame, and no rail label
// hover left over to hide the cursor readout.
void step_aside();
// Historical rails: recorded touch odds per candle (web /api/terminal/touch-odds/history through the bridge)
// times the fuel standing then, painted inside the candles as a backdrop. Ready when there is something to
// draw for this pair; the scenario map then dims beneath it. `runs` is called only when `source_key` (which
// source, and its cache generation) or the recorded history changed; bw is that source's bucket width.
bool rails_history_ready(const Terminal::Pair& pair, bool replay);
void rails_history(const Terminal::Pair& pair, uint64_t source_key, double bw,
                   const std::function<std::vector<liq_rails::Run>()>& runs, int64_t tf_ms, bool replay);
// Calibrated 1h/4h/24h touch odds on the strongest scenario-map zones (both layers on).
void zone_labels(const Terminal::Pair& pair, int64_t clock, const char* price_fmt,
                 std::vector<touch_zones::Fuel> fuel, bool replay = false);
}
