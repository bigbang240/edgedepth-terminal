// Production usage accounting and browser dispatch with deterministic clocks.
// Run each mode in a fresh process: terminal, lesson, studio, event, demo,
// studio-empty, event-empty, demo-empty, terminal-empty.
#include "core/session_usage.h"
#include "core/education_boot.h"
#include "core/usage_emit.h"
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>

namespace {
using json = nlohmann::json;
double frame_time = 10.0;
int checks = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
json events() {
    EM_ASM({ window.__usage_json = JSON.stringify(window.__usage_events); });
    return json::parse(usage::read_window_string("__usage_json"));
}
void clock_at(double wall_ms, double frame_seconds) {
    EM_ASM({ globalThis.__usage_wall_ms = $0; }, wall_ms);
    frame_time = frame_seconds;
}
} // namespace

// Replace only the UI clock; session code, boot detection and dispatch are real.
namespace ImGui { double GetTime() { return frame_time; } }

int main(int argc, char** argv) {
    const std::string scenario = argc > 1 ? argv[1] : "terminal";
    const auto dash = scenario.find('-');
    const std::string mode = scenario.substr(0, dash);
    const bool empty = dash != std::string::npos;
    check(mode == "terminal" || mode == "lesson" || mode == "studio" ||
          mode == "event" || mode == "demo", "known test mode");
    const std::string route = scenario == "terminal-empty" ? "" : "btcusdt";
    const std::string symbol = !empty && (mode == "studio" || mode == "event" || mode == "demo")
                                 ? "ethusdt" : route;
    const std::string plan = mode == "studio" ? "pro" : "free";
    EM_ASM({
        globalThis.window = { location: { search: "" } };
        globalThis.document = { referrer: 'https://example.invalid/guide' };
        globalThis.CustomEvent = class {
            constructor(type, init) { this.type = type; this.detail = init.detail; }
        };
        window.__usage_events = [];
        window.dispatchEvent = function(event) {
            if (event.type !== 'edgedepth:usage') throw new Error('unexpected event type');
            window.__usage_events.push(event.detail);
        };
        globalThis.__usage_wall_ms = 1000000;
        Date.now = function() { return globalThis.__usage_wall_ms; };
        var mode = UTF8ToString($0);
        var symbol = $1 ? "" : 'ethusdt';
        if (mode === 'studio' || mode === 'lesson') {
            window.__EDGEDEPTH_LESSON__ = ({ mode: mode, symbol: symbol });
        } else if (mode === 'event') {
            window.__EDGEDEPTH_EVENT__ = ({ eventId: 'fixture', symbol: symbol });
        } else if (mode === 'demo') {
            window.__EDGEDEPTH_PACK__ = ({ url: '/fixture.edpack', symbol: symbol });
        }
        if (mode === 'studio') window.__EDGEDEPTH_TIER__ = 'pro';
    }, mode.c_str(), empty);
    EducationBoot::instance().detect();

    usage::tick_session_usage(10.0, true, route);
    usage::maybe_emit_session_start(false, route);
    check(events().empty(), "no session or heartbeat before initialization");
    usage::maybe_emit_session_start(true, route);
    usage::maybe_emit_session_start(true, route);
    const std::string id = mode + "-1000000";
    const json start = {
        {"event", "session_start"}, {"mode", mode}, {"symbol", symbol},
        {"dedupeKey", id + ":start"},
        {"props", {{"session_run_id", id}, {"chrome", mode}, {"symbol", symbol},
                   {"plan", plan}, {"referrer", "https://example.invalid/guide"}}}
    };
    check(events() == json::array({start}), "one exact startup payload, including attribution and fallback");

    usage::tick_session_usage(-5.0, true, route); // negative deltas contribute zero
    usage::tick_session_usage(5.0, true, route);  // one frame contributes at most 0.25 s
    usage::tick_session_usage(0.25, true, route);
    for (int i = 0; i < 5; ++i) usage::tick_session_usage(0.25, false, route);
    clock_at(1029999, 39.999);
    usage::tick_session_usage(0.0, false, route);
    check(events().size() == 1, "no heartbeat before thirty wall seconds");
    clock_at(1030000, 40.0);
    usage::tick_session_usage(0.0, false, route);
    json heartbeat = {
        {"event", "session_heartbeat"}, {"heartbeatFor", "session_end"},
        {"mode", mode}, {"symbol", symbol}, {"umami", false}, {"dedupeKey", id + ":end"},
        {"props", {{"session_run_id", id}, {"chrome", mode}, {"symbol", symbol},
                   {"duration_s", 30}, {"active_s", 1}}}
    };
    const json first_heartbeat = heartbeat;
    check(events() == json::array({start, heartbeat}), "exact heartbeat with clamped visible time and rounding");
    for (int i = 0; i < 4; ++i) usage::tick_session_usage(0.25, true, route);
    check(events().size() == 2, "same wall time cannot emit another heartbeat");
    clock_at(1060000, 70.0);
    usage::tick_session_usage(0.0, false, route);
    heartbeat["props"]["duration_s"] = 60;
    heartbeat["props"]["active_s"] = 2;
    usage::maybe_emit_session_start(true, route);
    check(events() == json::array({start, first_heartbeat, heartbeat}),
          "later heartbeat keeps session identity and cumulative active time");
    std::printf("PASS: %s, %d session usage checks\n", scenario.c_str(), checks);
}
