#pragma once
// Each surface owns two adjacent IDs (context and history); late results never
// retain a C++ pointer. Cancellation removes the slot before any response lands.
inline int next_terminal_request_id() { static int next = 0; return 2 * ++next; }
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
extern "C" {
void terminal_request_cancel(int id) EM_IMPORT(terminal_request_cancel);
void terminal_request_start(int id, const char* venue, const char* symbol, int history) EM_IMPORT(terminal_request_start);
char* terminal_request_take(int id) EM_IMPORT(terminal_request_take);
}
#endif
