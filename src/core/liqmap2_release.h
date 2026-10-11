#pragma once
// Who sees the V2 map layer (hub task liq-map-v2). Releasing it is a terminal deploy: while kPublic is false
// only admins see it (a preview while production frames are checked); set it true and ship the terminal,
// and everyone gets the free markets (BTC on every venue), Pro gets every market, and a free viewer
// elsewhere gets a locked row into the upsell. The backend enforces the same split on the data (stream 38).
#include <string>

namespace liqmap2 {

// Released 2026-10-11 (James: "Release now ... happy to state that we are forward testing"): customers get the
// V2 rails (rendering/liqmap2_rails_overlay.h), the same view as the admin preview, labelled as a forward test.
// This overrides the 2026-10-08 hold on the rejected heatmap, whose customer draw path is gone.
inline constexpr bool kPublic = true;
// The wording every customer surface uses for the forward test (hub outputs/liq-map-v2-20261004/rails/
// PREREG_RAILS_FORWARD.md, look date 2026-11-16). No accuracy claim beyond development.
inline constexpr const char* kForwardTest = "Forward test on live Bybit data from 13 Oct; results from 16 Nov";

// The free markets, in hub naming, matching server_session liqmap2Free.
inline bool free_market(const std::string& exchange, const std::string& symbol) {
    if (exchange == "hl") return symbol == "BTC";
    return (exchange == "binancef" || exchange == "bybit") && symbol == "btcusdt";
}

enum class Access { hidden, preview, released, free_taste, locked };

inline Access access(const std::string& exchange, const std::string& symbol, bool admin, bool pro, bool is_public = kPublic) {
    if (admin) return Access::preview;
    if (!is_public) return Access::hidden;
    if (pro) return Access::released;
    return free_market(exchange, symbol) ? Access::free_taste : Access::locked;
}

}  // namespace liqmap2
