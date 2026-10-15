#include "core/websocket_endpoints.h"

#include <cstdlib>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace websocket_endpoints {

// Backend endpoint resolution, first match wins:
//   1. ?ws=<url> query param        (dev / self-hosters pointing at a local gateway)
//   2. window.__EDGEDEPTH_WS_URL__  (host page, set before the glue loads)
//   3. wss://api.edgedepth.com/ws   (production default)
// Only ws:// and wss:// schemes are accepted; anything else falls through.
//
// Origin guard: when the PAGE is served from edgedepth.com, an override is
// honoured only if it points at an *.edgedepth.com host. This is where a real
// signed-in user's entitlement tokens exist, and both resolvers send the Bearer
// token to whatever host they return, so a crafted ?ws= link must not be able
// to redirect those tokens to an attacker's server. Off edgedepth.com (local
// dev, a self-hoster's own domain) the override is unrestricted exactly as
// before, so the open-source self-host build is unaffected.
std::string resolve_ws_url() {
    static const char* kDefaultWsUrl = "wss://api.edgedepth.com/ws";
#ifdef __EMSCRIPTEN__
    char* raw = reinterpret_cast<char*>(EM_ASM_PTR({
        try {
            var url = new URLSearchParams(window.location.search).get('ws') ||
                      window.__EDGEDEPTH_WS_URL__ || "";
            url = String(url);
            if (url.indexOf('ws://') !== 0 && url.indexOf('wss://') !== 0) return 0;
            var page = String(window.location.hostname || "");
            if (page === "edgedepth.com" || page.endsWith(".edgedepth.com")) {
                var h = new URL(url).hostname;
                if (h !== "edgedepth.com" && !h.endsWith(".edgedepth.com")) return 0;
            }
            var len = lengthBytesUTF8(url);
            var buf = _malloc(len + 1);
            stringToUTF8(url, buf, len + 1);
            return buf;
        } catch (e) {
            return 0;
        }
    }));
    if (raw) {
        std::string url(raw);
        free(raw);
        return url;
    }
#endif
    return kDefaultWsUrl;
}

// Replay-lane endpoint resolution, first match wins:
//   1. ?replay_ws=<url> query param   (dev / pointing at a local replay-server)
//   2. window.__EDGEDEPTH_REPLAY_WS_URL__  (host page, before the glue loads)
//   3. "" = no separate replay origin, so the replay lane rides the live socket
//
// Returning "" by default is the whole safety property: shipping this build
// changes nothing until the host page sets the variable, exactly like
// RESEARCH_STORE_ENGINE_URL did for the research offload.
//
// WHY A SECOND SOCKET AT ALL. The box mounts ONE /ws handler and the client
// picks its lane after the handshake, so no path-based proxy rule can separate
// live from replay. Splitting them means a second hostname in front of a second
// process, which means a second socket here.
std::string resolve_replay_ws_url() {
#ifdef __EMSCRIPTEN__
    char* raw = reinterpret_cast<char*>(EM_ASM_PTR({
        try {
            var url = new URLSearchParams(window.location.search).get('replay_ws') ||
                      window.__EDGEDEPTH_REPLAY_WS_URL__ || "";
            url = String(url);
            if (url.indexOf('ws://') !== 0 && url.indexOf('wss://') !== 0) return 0;
            // Same origin guard as resolve_ws_url: on edgedepth.com, only an
            // *.edgedepth.com target may receive the entitlement token.
            var page = String(window.location.hostname || "");
            if (page === "edgedepth.com" || page.endsWith(".edgedepth.com")) {
                var h = new URL(url).hostname;
                if (h !== "edgedepth.com" && !h.endsWith(".edgedepth.com")) return 0;
            }
            var len = lengthBytesUTF8(url);
            var buf = _malloc(len + 1);
            stringToUTF8(url, buf, len + 1);
            return buf;
        } catch (e) {
            return 0;
        }
    }));
    if (raw) {
        std::string url(raw);
        free(raw);
        return url;
    }
#endif
    return std::string();
}

} // namespace websocket_endpoints
