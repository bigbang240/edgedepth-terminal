#pragma once
// Open an external page in a new tab, leaving the terminal (live socket, replay session, layout) where it
// is. Shared by the Data tab's research and pricing buttons and the V2 map's daily record link.
#include <string>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace ui {
inline void open_url(const std::string& url) {
#ifdef __EMSCRIPTEN__
    EM_ASM({ window.open(UTF8ToString($0), '_blank', 'noopener,noreferrer'); }, url.c_str());
#else
    (void)url;
#endif
}
}  // namespace ui
