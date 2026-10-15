#pragma once

#include <string>

namespace usage {

// One session per page mount. Called at the existing main-thread frame boundary;
// readiness and route identity stay owned by application startup.
void maybe_emit_session_start(bool initialized, const std::string& initial_symbol);
void tick_session_usage(double dt_seconds, bool document_visible,
                        const std::string& initial_symbol);

} // namespace usage
