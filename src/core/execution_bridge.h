#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// execution_bridge.h - Optional self-hosted webhook bridge (Phase 5)
//
// POSTs a small JSON message to a user-configured URL (personal bot / exchange
// adapter) whenever a chart alert fires or a local paper order opens/closes.
// Fire-and-forget fetch from the JS side (the WASM link has -sFETCH disabled,
// so all browser I/O rides EM_JS - replay_library_widget pattern).
//
// Settings are process-wide (one bridge per terminal) and persisted through
// the chart workspace rows. The target endpoint must allow CORS from this
// origin - self-hosted users point it at their own box.
// ═══════════════════════════════════════════════════════════════════════════════

#include <string>

namespace exec_bridge {

struct Settings {
    bool   enabled     = false;
    bool   paper_fills = true;   // include local paper open/close events
    char   url[256]    = {0};
};

inline Settings& settings() {
    static Settings s;
    return s;
}

// Build the JSON body and POST it (no-op unless enabled + URL set).
// kind: "alert" | "paper_open" | "paper_close"
// Kinds starting with "paper" are additionally gated by paper_fills.
void notify(const char* kind, const char* exchange, const char* symbol,
            const char* side, double price, const char* msg);

// Human-readable one-line status of the last POST ("ok 200 12:33:05",
// "http 500 ...", "-1 Failed to fetch", "idle"). Menu display only.
[[nodiscard]] std::string last_status();

}  // namespace exec_bridge
