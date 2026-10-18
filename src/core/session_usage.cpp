#include "core/session_usage.h"
#include "core/education_boot.h"
#include "core/usage_emit.h"
#include "imgui.h"
#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace usage {
using json = nlohmann::json;

// ═══════════════════════════════════════════════════════════════════════════════
// Usage: session lifecycle (design §3). session_start once at boot; a ~30s
// session_heartbeat the bridge holds as the pending session_end (materialized on
// tab-close). Replay watch-seconds are ReplayManager's job (tick_usage); session
// tracks time-in-terminal. Symbol switches are full navigations (fresh mount), so
// a session ~= one page mount here.
// ═══════════════════════════════════════════════════════════════════════════════
namespace {
std::string g_usage_session_id;
double      g_usage_session_start_wall = 0.0;   // ImGui::GetTime() at session_start
double      g_usage_session_active_s = 0.0;     // wall seconds while visible
int64_t     g_usage_session_last_hb_ms = 0;
bool        g_usage_session_started = false;

int64_t usage_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

const char* usage_chrome() {
    switch (EducationBoot::instance().boot_mode()) {
        case EducationBoot::Mode::Lesson: return "lesson";
        case EducationBoot::Mode::Studio: return "studio";
        case EducationBoot::Mode::Event:  return "event";
        case EducationBoot::Mode::Pack:   return "demo";
        default:                          return "terminal";
    }
}

std::string usage_session_symbol(const std::string& initial_symbol) {
    const auto& edu = EducationBoot::instance();
    if (edu.is_event() && !edu.event_symbol().empty())  return edu.event_symbol();
    if (edu.is_pack()  && !edu.pack_symbol().empty())   return edu.pack_symbol();
    if (edu.is_studio() && !edu.hosted_research() && !edu.studio_symbol().empty()) return edu.studio_symbol();
    if (!initial_symbol.empty()) return initial_symbol;
    return "";
}
}  // namespace

void maybe_emit_session_start(bool initialized, const std::string& initial_symbol) {
    if (g_usage_session_started) return;
    if (!initialized) return;
    g_usage_session_started = true;

    g_usage_session_id = std::string(usage_chrome()) + "-" + std::to_string(usage_now_ms());
    g_usage_session_start_wall = ImGui::GetTime();
    g_usage_session_active_s = 0.0;
    g_usage_session_last_hb_ms = usage_now_ms();

    const std::string sym = usage_session_symbol(initial_symbol);
    std::string plan = usage::read_window_string("__EDGEDEPTH_TIER__");
    if (plan.empty()) plan = "free";
    const std::string ref = usage::read_referrer();

    json detail = {
        {"event", "session_start"},
        {"mode", usage_chrome()},
        {"symbol", sym},
        {"dedupeKey", g_usage_session_id + ":start"},
        {"props", {
            {"session_run_id", g_usage_session_id},
            {"chrome", usage_chrome()},
            {"symbol", sym},
            {"plan", plan},
            {"referrer", ref},
        }},
    };
    usage::dispatch_detail(detail.dump());
}

void tick_session_usage(double dt_seconds, bool document_visible,
                        const std::string& initial_symbol) {
    if (!g_usage_session_started) return;
    if (document_visible) {
        double d = dt_seconds;
        if (d < 0.0) d = 0.0;
        if (d > 0.25) d = 0.25;
        g_usage_session_active_s += d;
    }
    const int64_t noww = usage_now_ms();
    if (noww - g_usage_session_last_hb_ms < 30000) return;
    g_usage_session_last_hb_ms = noww;

    const double duration_s = ImGui::GetTime() - g_usage_session_start_wall;
    json detail = {
        {"event", "session_heartbeat"},
        {"heartbeatFor", "session_end"},
        {"mode", usage_chrome()},
        {"symbol", usage_session_symbol(initial_symbol)},
        {"umami", false},
        {"dedupeKey", g_usage_session_id + ":end"},
        {"props", {
            {"session_run_id", g_usage_session_id},
            {"chrome", usage_chrome()},
            {"symbol", usage_session_symbol(initial_symbol)},
            {"duration_s", static_cast<int64_t>(duration_s + 0.5)},
            {"active_s", static_cast<int64_t>(g_usage_session_active_s + 0.5)},
        }},
    };
    usage::dispatch_detail(detail.dump());
}

} // namespace usage
