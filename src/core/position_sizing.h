#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// position_sizing.h - Risk-based position size calculator (Phase 5)
//
// Global account/risk settings shared by:
//   - the Long/Short Position drawing readout chip ("size X · $Y @ risk%")
//   - the local paper engine's auto-quantity for alert-triggered orders
//   - the RT menu Execution group and the drawing style editor inputs
//
// Deliberately a process-wide singleton (not per chart): account equity and
// risk-per-trade are account-level concepts. Persisted through the chart
// workspace rows (every chart writes the same values, link_charts pattern).
// ═══════════════════════════════════════════════════════════════════════════════

#include <algorithm>

namespace position_sizing {

struct Settings {
    double account_usd   = 10000.0;  // account equity used for sizing
    double risk_pct      = 1.0;      // risk per trade, % of account
    bool   show_on_chart = true;     // draw the size line on position drawings
};

inline Settings& settings() {
    static Settings s;
    return s;
}

inline void clamp_settings(Settings& s) {
    s.account_usd = std::clamp(s.account_usd, 10.0, 1000000000.0);
    s.risk_pct    = std::clamp(s.risk_pct, 0.05, 100.0);
}

struct SizeResult {
    bool   valid     = false;
    double qty       = 0.0;   // base-asset units
    double notional  = 0.0;   // qty * entry
    double risk_usd  = 0.0;   // qty * stop distance (actual, after the cap)
};

// Size a position from the stop distance. Caps the notional at 20x account so
// a hair-thin stop cannot print an absurd size.
inline SizeResult compute(double entry, double stop) {
    SizeResult out;
    const Settings& s = settings();
    if (!(entry > 0.0) || !(stop > 0.0)) return out;
    const double stop_dist = entry > stop ? entry - stop : stop - entry;
    if (!(stop_dist > 0.0)) return out;
    const double risk_usd = s.account_usd * s.risk_pct / 100.0;
    double qty = risk_usd / stop_dist;
    double notional = qty * entry;
    const double cap = s.account_usd * 20.0;
    if (notional > cap) {
        qty = cap / entry;
        notional = cap;
    }
    out.valid = qty > 0.0;
    out.qty = qty;
    out.notional = notional;
    out.risk_usd = qty * stop_dist;
    return out;
}

}  // namespace position_sizing
