#include "terminal_request.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// Requests belong to widget IDs, never C++ pointers. Closing/replay cancels them;
// late completions cannot land on a recreated widget or a different market.
EM_JS(void, terminal_request_cancel, (int id), {
    var slots = Module['terminalRequests'];
    var slot = slots && slots[id];
    if (slot) { clearTimeout(slot.timer); slot.controller.abort(); delete slots[id]; }
});
EM_JS(void, terminal_request_start, (int id, const char* venue, const char* symbol, int history), {
    var slots = Module['terminalRequests'] || (Module['terminalRequests'] = {});
    var old = slots[id];
    if (old) { clearTimeout(old.timer); old.controller.abort(); }
    var slot = {controller: new AbortController(), result: null, timer: null};
    slots[id] = slot;
    slot.timer = setTimeout(function() { slot.controller.abort(); }, 8000);
    var url = '/api/terminal/' + (history === 4 ? 'research' : history === 3 ? 'market-data' : history ? 'ratio-history' : 'market-context') + '?venue=' + encodeURIComponent(UTF8ToString(venue)) + '&symbol=' + encodeURIComponent(UTF8ToString(symbol));
    if (history === 2) url += '&source=binance';
    fetch(url, {credentials: 'same-origin', cache: 'no-store', signal: slot.controller.signal})
        .then(async function(response) {
            if (response.status === 401 || response.status === 403) return 'locked';
            if (!response.ok) return 'unavailable';
            var body = await response.text();
            return body.length <= (history ? 262144 : 16384) ? body : 'unavailable';
        }).catch(function() { return 'unavailable'; })
        .then(function(body) {
            clearTimeout(slot.timer);
            if (slots[id] === slot) slot.result = body;
        });
});
EM_JS(char*, terminal_request_take, (int id), {
    var slot = Module['terminalRequests'] && Module['terminalRequests'][id];
    if (!slot || slot.result === null) return 0;
    var result = slot.result; slot.result = null;
    var size = lengthBytesUTF8(result) + 1, ptr = _malloc(size);
    if (ptr) stringToUTF8(result, ptr, size);
    return ptr;
});
#endif
