// Exercise the production EM_ASM resolvers with browser inputs in Node.
#include "core/websocket_endpoints.h"
#include <emscripten.h>
#include <cstdio>

namespace {
int failures = 0;
int checks = 0;
constexpr const char* kLiveDefault = "wss://api.edgedepth.com/ws";

void expect(const char* name, const char* lane, const std::string& actual,
            const char* expected) {
    ++checks;
    if (actual != expected) {
        std::fprintf(stderr, "FAIL %s (%s): expected '%s', got '%s'\n",
                     name, lane, expected, actual.c_str());
        ++failures;
    }
}

struct Case {
    const char* name;
    const char* page;
    const char* query;
    const char* global;
    const char* expected; // nullptr means this lane's default.
};
} // namespace

int main() {
    const Case cases[] = {
        {"defaults", "localhost", nullptr, nullptr, nullptr},
        {"global override", "localhost", nullptr, "ws://localhost:9010/ws", "ws://localhost:9010/ws"},
        {"query override", "localhost", "wss://gateway.example/ws", nullptr, "wss://gateway.example/ws"},
        {"query precedes global", "localhost", "ws://localhost:9011/ws", "ws://localhost:9010/ws", "ws://localhost:9011/ws"},
        {"empty query uses global", "localhost", "", "ws://localhost:9010/ws", "ws://localhost:9010/ws"},
        {"invalid query masks global", "localhost", "https://gateway.example/ws", "ws://localhost:9010/ws", nullptr},
        {"invalid global", "localhost", nullptr, "https://gateway.example/ws", nullptr},
        {"uppercase scheme rejected", "localhost", "WSS://gateway.example/ws", nullptr, nullptr},
        {"relative URL rejected", "localhost", "//gateway.example/ws", nullptr, nullptr},
        {"script URL rejected", "localhost", "javascript:void(0)", nullptr, nullptr},
        {"self-hosted external target", "terminal.example", nullptr, "wss://gateway.example/ws", "wss://gateway.example/ws"},
        {"similar page is self-hosted", "notedgedepth.com", "wss://gateway.example/ws", nullptr, "wss://gateway.example/ws"},
        {"hosted apex target", "edgedepth.com", "wss://edgedepth.com/ws", nullptr, "wss://edgedepth.com/ws"},
        {"hosted subdomain target", "app.edgedepth.com", nullptr, "wss://replay-api.edgedepth.com/ws", "wss://replay-api.edgedepth.com/ws"},
        {"hosted target with port", "edgedepth.com", "ws://api.edgedepth.com:9010/ws", nullptr, "ws://api.edgedepth.com:9010/ws"},
        {"hosted external query rejected", "edgedepth.com", "wss://outside.invalid/ws", "wss://api.edgedepth.com/ws", nullptr},
        {"hosted external global rejected", "app.edgedepth.com", nullptr, "wss://outside.invalid/ws", nullptr},
        {"host suffix spoof rejected", "edgedepth.com", "wss://api.edgedepth.com.outside.invalid/ws", nullptr, nullptr},
        {"host prefix spoof rejected", "app.edgedepth.com", "wss://notedgedepth.com/ws", nullptr, nullptr},
        {"userinfo spoof rejected", "edgedepth.com", "wss://api.edgedepth.com@outside.invalid/ws", nullptr, nullptr},
        {"malformed hosted URL rejected", "edgedepth.com", "wss://[", nullptr, nullptr},
        {"URL bytes preserved", "localhost", "ws://localhost:9010/ws?label=caf%C3%A9&lane=1", nullptr, "ws://localhost:9010/ws?label=caf%C3%A9&lane=1"},
    };
    for (bool replay : {false, true}) {
        const char* lane = replay ? "replay" : "live";
        const char* fallback = replay ? "" : kLiveDefault;
        for (const auto& c : cases) {
            EM_ASM({
                var query = new URLSearchParams();
                if ($1) query.set($3 ? 'replay_ws' : 'ws', UTF8ToString($1));
                globalThis.window = ({
                    location: { hostname: UTF8ToString($0), search: '?' + query.toString() }
                });
                if ($2) window[$3 ? '__EDGEDEPTH_REPLAY_WS_URL__' : '__EDGEDEPTH_WS_URL__'] = UTF8ToString($2);
            }, c.page, c.query, c.global, replay);
            expect(c.name, lane, replay ? websocket_endpoints::resolve_replay_ws_url()
                                        : websocket_endpoints::resolve_ws_url(),
                   c.expected ? c.expected : fallback);
            expect("other lane remains independent", lane,
                   replay ? websocket_endpoints::resolve_ws_url()
                          : websocket_endpoints::resolve_replay_ws_url(),
                   replay ? kLiveDefault : "");
        }
    }
    EM_ASM({ delete globalThis.window; });
    expect("missing browser globals", "live", websocket_endpoints::resolve_ws_url(), kLiveDefault);
    expect("missing browser globals", "replay", websocket_endpoints::resolve_replay_ws_url(), "");
    if (failures) return 1;
    std::printf("PASS: %d websocket endpoint checks (precedence, defaults, lane isolation, hosted origin guard)\n", checks);
}
