#pragma once
// Browser-test marks (2026-10-10): with window.__EDGEDEPTH_TEST_UI__ set before boot, named controls publish their
// on-screen rects (CSS px) each frame to window.__EDGEDEPTH_UI_RECTS__ = {name: [x0, y0, x1, y1]}, so browser
// tests click controls by name instead of by coordinates that every redesign moves. Off by default: one cached
// flag check per mark, nothing published.
#include "imgui.h"
#include <cstdio>
#include <string>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace ui_test {
inline bool enabled() {
#ifdef __EMSCRIPTEN__
    static const bool on = emscripten_run_script_int("window.__EDGEDEPTH_TEST_UI__ ? 1 : 0") != 0;
    return on;
#else
    return false;
#endif
}
inline std::string& frame_json() { static std::string s; return s; }
// The last submitted item's rect under name.
inline void mark(const char* name) {
    if (!enabled()) return;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    char entry[256];
    std::snprintf(entry, sizeof(entry), "%s\"%s\":[%.1f,%.1f,%.1f,%.1f]", frame_json().empty() ? "" : ",", name, a.x, a.y, b.x, b.y);
    frame_json() += entry;
}
inline void mark(const std::string& name) { mark(name.c_str()); }
// An explicit rect (drawn text that is not an item).
inline void mark_rect(const std::string& name, ImVec2 a, ImVec2 b) {
    if (!enabled()) return;
    char entry[256];
    std::snprintf(entry, sizeof(entry), "%s\"%s\":[%.1f,%.1f,%.1f,%.1f]", frame_json().empty() ? "" : ",", name.c_str(), a.x, a.y, b.x, b.y);
    frame_json() += entry;
}
// Once per frame, after every widget has drawn.
inline void publish() {
    if (!enabled()) return;
#ifdef __EMSCRIPTEN__
    const std::string script = "window.__EDGEDEPTH_UI_RECTS__={" + frame_json() + "};";
    emscripten_run_script(script.c_str());
#endif
    frame_json().clear();
}
}  // namespace ui_test
