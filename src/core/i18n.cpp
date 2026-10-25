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
    EM_ASM({
        try {
            const raw = localStorage.getItem('edgedepth.i18n.v1');
            if (!raw) return;
            const j = JSON.parse(raw);
            const lang = j.lang | 0, dig = j.digits ? 1 : 0;
            stringToUTF8(JSON.stringify({lang: lang, digits: dig}), $0, 64);
        } catch (e) {}
    }, buf);
    const std::string raw(buf);
    if (raw.find("\"lang\":1") != std::string::npos) g_lang = Lang::Fa;
    if (raw.find("\"digits\":true") != std::string::npos) g_fa_digits = true;
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
