// ═══════════════════════════════════════════════════════════════════════════════
// execution_bridge.cpp - webhook POST implementation (see execution_bridge.h)
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/execution_bridge.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <nlohmann/json.hpp>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// Fire-and-forget POST. The JS-side state object records the outcome so the
// menu can render a one-line status without any C++↔JS polling machinery.
EM_JS(void, edwh_post, (const char* url, const char* body), {
    try {
        var st = Module['__edWebhook'] ||
                 (Module['__edWebhook'] = { code: 0, at: 0, status: 'idle' });
        st.status = 'sending';
        fetch(UTF8ToString(url), {
            method: 'POST',
            mode: 'cors',
            credentials: 'omit',
            headers: { 'Content-Type': 'application/json' },
            body: UTF8ToString(body)
        }).then(function (r) {
            st.code = r.status;
            st.at = Date.now();
            st.status = r.ok ? 'ok' : 'http ' + r.status;
        }).catch(function (e) {
            st.code = -1;
            st.at = Date.now();
            st.status = String((e && e.message) || 'failed');
        });
    } catch (e) {}
});

// Last status rendered as "code status HH:MM:SS" (caller frees).
EM_JS(char*, edwh_status, (), {
    var st = Module['__edWebhook'];
    if (!st || (!st.at && st.status === 'idle')) return 0;
    var t = st.at ? new Date(st.at).toISOString().slice(11, 19) : '';
    var s = st.code + ' ' + st.status + (t ? ' ' + t : '');
    var n = lengthBytesUTF8(s) + 1;
    var p = _malloc(n);
    stringToUTF8(s, p, n);
    return p;
});

#else
static void edwh_post(const char*, const char*) {}
static char* edwh_status() { return nullptr; }
#endif

namespace exec_bridge {

void notify(const char* kind, const char* exchange, const char* symbol,
            const char* side, double price, const char* msg) {
    Settings& s = settings();
    if (!s.enabled || !s.url[0]) return;
    if (std::strncmp(kind, "paper", 5) == 0 && !s.paper_fills) return;

    nlohmann::json j;
    j["kind"]    = kind ? kind : "";
    j["exchange"] = exchange ? exchange : "";
    j["symbol"]  = symbol ? symbol : "";
    j["side"]    = side ? side : "";
    j["price"]   = price;
    j["msg"]     = msg ? msg : "";
    j["ts_ms"]   = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const std::string body = j.dump();

    // JS-side fetch keeps its own copy; the temporaries are safe to free.
    char url_copy[256];
    std::snprintf(url_copy, sizeof(url_copy), "%s", s.url);
    edwh_post(url_copy, body.c_str());
}

std::string last_status() {
    char* raw = edwh_status();
    if (!raw) return "idle";
    std::string out(raw);
    std::free(raw);
    return out;
}

}  // namespace exec_bridge
