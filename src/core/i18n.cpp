// Phase 7 - i18n persistence (see i18n.h for the design notes).
#include "i18n.h"

#include <cstdlib>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace i18n {

void load() {
#ifdef __EMSCRIPTEN__
    static char buf[64] = {};
    // NOTE: no object literals inside EM_ASM - the preprocessor splits macro
    // arguments on every top-level comma (braces do not protect), so the
    // packed int avoids a {lang, digits} literal entirely.
    EM_ASM({
        try {
            const raw = localStorage.getItem('edgedepth.i18n.v1');
            if (!raw) return;
            const j = JSON.parse(raw);
            stringToUTF8(JSON.stringify((j.lang | 0) + (j.digits ? 100 : 0)), $0, 64);
        } catch (e) {}
    }, buf);
    const int packed = std::atoi(buf);  // lang + 100*digits flag
    if (packed >= 100) { g_fa_digits = true; }
    if (packed % 100 == 1) { g_lang = Lang::Fa; }
#endif
}

void save() {
#ifdef __EMSCRIPTEN__
    const std::string text = "{\"lang\":" + std::to_string(int(g_lang)) +
                             ",\"digits\":" + (g_fa_digits ? "true" : "false") + "}";
    EM_ASM({ try { localStorage.setItem('edgedepth.i18n.v1', UTF8ToString($0)); } catch (e) {} },
           text.c_str());
#endif
}

}  // namespace i18n
