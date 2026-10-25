// Phase 7 - Persian UX layer (self-host, AGPL mod).
//
// Design constraints that shaped this:
//  - ImGui renders LTR glyph runs and does not shape Arabic script. The
//    Persian strings in i18n_fa.h are therefore PRE-SHAPED into visual order
//    (presentation forms B + bidi reversal) by tools/scripts/gen-i18n-fa.py —
//    the table is data, not logic.
//  - Persian glyphs live in fonts/Vazirmatn-*-Subset.ttf, merged as a second
//    glyph source on every face in theme.cpp (the established CJK pattern).
//  - tr() is a branch, not a map lookup: call sites keep their English
//    literal inline (grep-able) plus the fa_<key> enum from the generated
//    table. Layout stays LTR — this is the layered, <200-string scope the
//    roadmap promises, not a full RTL mirror.
#pragma once

#include <string>

#include "i18n_fa.h"

namespace i18n {

enum class Lang : int { En = 0, Fa = 1 };

// Workspace-persisted globals (loaded in workspace_settings.cpp; written by
// the Appearance panel in app_shell.cpp).
inline Lang g_lang      = Lang::En;
inline bool g_fa_digits = false;

inline bool fa() { return g_lang == Lang::Fa; }

// tr("English literal", i18n::fa_key): English when the session is English,
// pre-shaped Persian when Persian. The English literal is what grep finds.
inline const char* tr(const char* en, FaKey key) {
    return fa() ? fa_table[key][1] : en;
}

// 0-9 → ۰-۹ (U+06F0..U+06F9), ASCII bytes are single-codepoint so the scan is
// byte-wise. Separators stay ASCII: trading surfaces keep parseable shapes,
// only the digit glyphs localize.
inline std::string digits(const std::string& s) {
    if (!g_fa_digits) return s;
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            out += static_cast<char>(0xDB);
            out += static_cast<char>(0xB0 + (c - '0'));
        } else {
            out += c;
        }
    }
    return out;
}

// Persistence: localStorage "edgedepth.i18n.v1" = {"lang":0|1,"digits":bool}.
// load() at boot (main.cpp), save() after each change in the Appearance panel.
void load();
void save();

}  // namespace i18n
