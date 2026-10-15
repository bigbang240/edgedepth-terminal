#pragma once

#include <string>

namespace websocket_endpoints {

// Read browser overrides on the main thread. Live defaults to the hosted API;
// an empty replay URL tells the caller to reuse its live connection.
std::string resolve_ws_url();
std::string resolve_replay_ws_url();

} // namespace websocket_endpoints
