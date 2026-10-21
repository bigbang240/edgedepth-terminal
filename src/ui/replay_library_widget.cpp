#include "ui/replay_library_widget.h"

#include "rendering/theme.h"
#include "core/logo_manager.h"
#include "core/ui_test_marks.h"
#include "ui/drawing/drawing_icons.h"
#include "replayer/replay_manager.h"
#include "core/entitlements.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <utility>

#include "imgui.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using json = nlohmann::json;

namespace {

constexpr const char* kDefaultManifestUrl =
    "https://replays.edgedepth.com/library/v1/manifest.json";
constexpr size_t kMaxPacks = 64;

#ifdef __EMSCRIPTEN__

// Resolve once at widget creation. The query parameter wins so a developer can
// test a private/local catalog without rebuilding the terminal.
EM_JS(char*, edrl_resolve_manifest_url, (), {
    var fallback = 'https://replays.edgedepth.com/library/v1/manifest.json';
    var candidate = String();
    try {
        var query = new URLSearchParams(window.location.search).get('replayLibrary');
        candidate = query || window['__EDGEDEPTH_REPLAY_LIBRARY_URL__'] || fallback;
        var resolved = new URL(candidate, window.location.href);
        if (resolved.protocol !== 'https:' && resolved.protocol !== 'http:') {
            resolved = new URL(fallback);
        }
        candidate = resolved.href;
    } catch (_) {
        candidate = fallback;
    }
    var n = lengthBytesUTF8(candidate) + 1;
    var p = _malloc(n);
    stringToUTF8(candidate, p, n);
    return p;
});

// One browser-side catalog state is shared across widget reopenings. A cached
// manifest paints immediately, while every normal open still refreshes it.
EM_JS(void, edrl_fetch_begin, (const char* url_ptr, int force), {
    var url = UTF8ToString(url_ptr);
    var state = Module['__edReplayLibrary'];
    if (!state || state.url !== url) {
        state = {
            url: url,
            payload: String(),
            revision: 0,
            loading: false,
            error: String(),
            cacheTried: false
        };
        Module['__edReplayLibrary'] = state;
    }
    if (state.loading) return;

    var validate = function(text) {
        if (!text || text.length > 1024 * 1024) {
            throw new Error('Manifest is empty or exceeds 1 MB');
        }
        var parsed = JSON.parse(text);
        if (!parsed || parsed.schema !== 'edgedepth.replay-library.v1' ||
            !Array.isArray(parsed.packs)) {
            throw new Error('Unsupported replay-library manifest');
        }
        // Self-hosted catalogs (the recorder daemon's manifest, a NAS, a LAN
        // box) may ship host-agnostic RELATIVE pack URLs ("/packs/x.edpack"),
        // which the C++ side rejects by design (is_http_url). Resolve them
        // against this manifest's URL so every pack_url the widget sees is a
        // plain absolute http(s) URL. Absolute entries pass through untouched.
        var base = requestUrl ? requestUrl.href : window.location.href;
        for (var i = 0; i < parsed.packs.length; i++) {
            var p = parsed.packs[i];
            if (p && typeof p.pack_url === 'string' && p.pack_url.charAt(0) === '/') {
                try { p.pack_url = new URL(p.pack_url, base).href; } catch (_) {}
            }
        }
        return JSON.stringify(parsed);
    };

    var cacheKey = 'edgedepth.replay_library.v1:' + url;
    if (!force && !state.cacheTried) {
        state.cacheTried = true;
        try {
            var cached = localStorage.getItem(cacheKey);
            if (cached) {
                state.payload = validate(cached);
                state.revision += 1;
            }
        } catch (_) {
            // Cache failures never prevent a network request.
        }
    }

    state.loading = true;
    state.error = String();
    // Cloudflare installations often give all public R2 objects one long edge
    // TTL. A five-minute query bucket keeps the mutable catalog fresh even if
    // that broad rule also catches /library/*. Manual Refresh is immediate.
    var requestUrl = new URL(url);
    requestUrl.searchParams.set(
        '_edrl', force ? String(Date.now()) : String(Math.floor(Date.now() / 300000)));
    fetch(requestUrl.href, {
        method: 'GET', cache: force ? 'reload' : 'no-cache', credentials: 'omit'
    })
        .then(function(response) {
            if (!response.ok) throw new Error('HTTP ' + response.status);
            return response.text();
        })
        .then(function(text) {
            state.payload = validate(text);
            state.revision += 1;
            state.loading = false;
            try { localStorage.setItem(cacheKey, text); } catch (_) {}
        })
        .catch(function(error) {
            state.loading = false;
            state.error = error && error.message ? error.message : 'Catalog request failed';
        });
});

EM_JS(int, edrl_status, (), {
    var state = Module['__edReplayLibrary'];
    if (!state) return 0;
    if (state.payload) return 2;
    if (state.loading) return 1;
    if (state.error) return 3;
    return 0;
});

EM_JS(int, edrl_loading, (), {
    var state = Module['__edReplayLibrary'];
    return state && state.loading ? 1 : 0;
});

EM_JS(int, edrl_revision, (), {
    var state = Module['__edReplayLibrary'];
    return state ? state.revision : 0;
});

EM_JS(char*, edrl_copy_payload, (), {
    var state = Module['__edReplayLibrary'];
    var value = state && state.payload ? state.payload : String();
    var n = lengthBytesUTF8(value) + 1;
    var p = _malloc(n);
    stringToUTF8(value, p, n);
    return p;
});

EM_JS(char*, edrl_copy_error, (), {
    var state = Module['__edReplayLibrary'];
    var value = state && state.error ? state.error : String();
    var n = lengthBytesUTF8(value) + 1;
    var p = _malloc(n);
    stringToUTF8(value, p, n);
    return p;
});

// At most one header request per widget. Exact range responses only, bounded
// before body consumption. Closing/refreshing cancels ownership, including late
// callbacks; no C++ pointer crosses an asynchronous JS callback.
EM_JS(int, edrl_preview_begin, (const char* url_ptr), {
    var jobs = Module['__edReplayPreviews'] || (Module['__edReplayPreviews'] = {next: 1, jobs: {}});
    var id = jobs.next++;
    var controller = new AbortController();
    var job = {status: 0, data: null, controller: controller};
    jobs.jobs[id] = job;
    var url = UTF8ToString(url_ptr);
    var timeout = setTimeout(function() { controller.abort(); }, 20000);
    var range = async function(start, end) {
        var response = await fetch(url, {headers: {'Range': 'bytes=' + start + '-' + end},
            credentials: 'omit', signal: controller.signal});
        var expected = end - start + 1;
        var match = new RegExp('^bytes ([0-9]+)-([0-9]+)/([0-9]+)$').exec(response.headers.get('Content-Range') || String());
        if (response.status !== 206 || !match || Number(match[1]) !== start || Number(match[2]) !== end) {
            if (response.body) await response.body.cancel();
            throw new Error('Range unavailable');
        }
        var reader = response.body.getReader();
        var data = new Uint8Array(expected), offset = 0;
        while (true) {
            var part = await reader.read();
            if (part.done) break;
            if (offset + part.value.length > expected) { await reader.cancel(); throw new Error('Oversized range'); }
            data.set(part.value, offset); offset += part.value.length;
        }
        if (offset !== expected) throw new Error('Truncated range');
        return data;
    };
    (async function() {
        try {
            var prefix = await range(0, 8);
            if (prefix[0] !== 69 || prefix[1] !== 68 || prefix[2] !== 80 || prefix[3] !== 75 ||
                (prefix[4] !== 1 && prefix[4] !== 2)) throw new Error('Invalid pack');
            var length = new DataView(prefix.buffer).getUint32(5, true);
            if (!length || length > 4 * 1024 * 1024) throw new Error('Header too large');
            var data = await range(9, length + 8);
            if (jobs.jobs[id] === job) { job.data = data; job.status = 1; }
        } catch (_) {
            if (jobs.jobs[id] === job) job.status = -1;
        } finally { clearTimeout(timeout); }
    })();
    return id;
});
EM_JS(int, edrl_preview_status, (int id), {
    var state = Module['__edReplayPreviews']; var job = state && state.jobs[id];
    return job ? job.status : -1;
});
EM_JS(void*, edrl_preview_take, (int id, int* size), {
    var state = Module['__edReplayPreviews']; var job = state && state.jobs[id];
    if (!job || !job.data) return 0;
    var data = job.data, ptr = _malloc(data.length);
    if (ptr) { HEAPU8.set(data, ptr); HEAP32[size >> 2] = data.length; }
    delete state.jobs[id];
    return ptr;
});
EM_JS(void, edrl_preview_cancel, (int id), {
    var state = Module['__edReplayPreviews']; var job = state && state.jobs[id];
    if (job) { delete state.jobs[id]; job.controller.abort(); }
});

EM_JS(void, edrl_copy_url, (const char* url_ptr), {
    var value = UTF8ToString(url_ptr);
    var fallback = function() {
        var area = document.createElement('textarea');
        area.value = value;
        area.style.position = 'fixed';
        area.style.opacity = '0';
        document.body.appendChild(area);
        area.select();
        try { document.execCommand('copy'); } catch (_) {}
        document.body.removeChild(area);
    };
    if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(value).catch(fallback);
    } else {
        fallback();
    }
});

EM_JS(void, edrl_open_url, (const char* url_ptr), {
    window.open(UTF8ToString(url_ptr), '_blank', 'noopener');
});

// ── Event finder feed (the recorder daemon's /events endpoint) ────────────────
// The endpoint lives NEXT TO the catalog manifest on the same server, so the
// query reuses the manifest URL's directory + query string (a self-hosted
// setup's XTransformPort routing suffix rides along untouched). One browser
// side state, same shape as the catalog fetch above.
EM_JS(void, ed_ev_fetch_begin, (const char* base_ptr, const char* path_ptr), {
    var base = UTF8ToString(base_ptr);
    var path = UTF8ToString(path_ptr);
    var state = Module['__edEventFinder'] || (Module['__edEventFinder'] = {
        payload: String(), revision: 0, loading: false, error: String()});
    if (state.loading) return;
    var url;
    try {
        url = new URL(path, base ? base : window.location.href);
    } catch (_) {
        state.error = 'Bad event feed URL';
        return;
    }
    state.loading = true;
    state.error = String();
    fetch(url.href, {method: 'GET', cache: 'no-cache', credentials: 'omit'})
        .then(function(response) {
            if (!response.ok) throw new Error('HTTP ' + response.status);
            return response.text();
        })
        .then(function(text) {
            var parsed = JSON.parse(text);
            if (!parsed || parsed.schema !== 'edgedepth.events.v1' ||
                !Array.isArray(parsed.events)) {
                throw new Error('Unsupported event feed');
            }
            if (text.length > 4 * 1024 * 1024) throw new Error('Event feed too large');
            state.payload = JSON.stringify(parsed);
            state.revision += 1;
            state.loading = false;
        })
        .catch(function(error) {
            state.loading = false;
            state.error = error && error.message ? error.message : 'Event search failed';
        });
});

EM_JS(int, ed_ev_loading, (), {
    var state = Module['__edEventFinder'];
    return state && state.loading ? 1 : 0;
});

EM_JS(int, ed_ev_revision, (), {
    var state = Module['__edEventFinder'];
    return state ? state.revision : 0;
});

EM_JS(int, ed_ev_failed, (), {
    var state = Module['__edEventFinder'];
    return state && state.error ? 1 : 0;
});

EM_JS(char*, ed_ev_copy_payload, (), {
    var state = Module['__edEventFinder'];
    var value = state && state.payload ? state.payload : String();
    var n = lengthBytesUTF8(value) + 1;
    var p = _malloc(n);
    stringToUTF8(value, p, n);
    return p;
});

EM_JS(char*, ed_ev_copy_error, (), {
    var state = Module['__edEventFinder'];
    var value = state && state.error ? state.error : String();
    var n = lengthBytesUTF8(value) + 1;
    var p = _malloc(n);
    stringToUTF8(value, p, n);
    return p;
});

#else

static char* edrl_resolve_manifest_url() { return nullptr; }
static void edrl_fetch_begin(const char*, int) {}
static int edrl_status() { return 3; }
static int edrl_loading() { return 0; }
static int edrl_revision() { return 0; }
static char* edrl_copy_payload() { return nullptr; }
static char* edrl_copy_error() { return nullptr; }
static int edrl_preview_begin(const char*) { return 0; }
static int edrl_preview_status(int) { return -1; }
static void* edrl_preview_take(int, int*) { return nullptr; }
static void edrl_preview_cancel(int) {}
static void edrl_copy_url(const char*) {}
static void edrl_open_url(const char*) {}
static void ed_ev_fetch_begin(const char*, const char*) {}
static int ed_ev_loading() { return 0; }
static int ed_ev_revision() { return 0; }
static int ed_ev_failed() { return 0; }
static char* ed_ev_copy_payload() { return nullptr; }
static char* ed_ev_copy_error() { return nullptr; }

#endif

std::string upper_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

} // namespace

ReplayLibraryWidget::ReplayLibraryWidget(const AppContext& ctx)
    : ctx_(ctx), manifest_url_(kDefaultManifestUrl) {
#ifdef __EMSCRIPTEN__
    if (char* raw = edrl_resolve_manifest_url()) {
        if (raw[0] != '\0') manifest_url_ = raw;
        std::free(raw);
    }
#endif
    begin_fetch(false);
}

ReplayLibraryWidget::~ReplayLibraryWidget() { edrl_preview_cancel(preview_request_); }

void ReplayLibraryWidget::update_preview() {
    if (!is_open) { edrl_preview_cancel(preview_request_); preview_request_ = 0; return; }
    if (preview_request_) {
        const int status = edrl_preview_status(preview_request_);
        if (!status) return;
        if (preview_pack_ >= 0 && preview_pack_ < static_cast<int>(packs_.size())) {
            auto& pack = packs_[preview_pack_];
            pack.preview_done = true;
            int size = 0;
            if (void* data = edrl_preview_take(preview_request_, &size)) {
                pack.preview = PackPreview::decode(data, size, pack.exchange_id, pack.symbol, pack.event_id);
                std::free(data);
            }
        }
        edrl_preview_cancel(preview_request_);
        preview_request_ = 0;
    }
    for (size_t i = 0; i < packs_.size(); ++i) {
        auto& pack = packs_[i];
        if (!pack.preview_requested || pack.preview_done) continue;
        preview_pack_ = static_cast<int>(i);
        preview_request_ = edrl_preview_begin(pack.pack_url.c_str());
        if (!preview_request_) pack.preview_done = true;
        break;
    }
}

void ReplayLibraryWidget::begin_fetch(bool force) {
    error_.clear();
    edrl_fetch_begin(manifest_url_.c_str(), force ? 1 : 0);
    loading_ = true;
    // Explicit refresh is also the retry action for failed previews.
    if (force) for (auto& pack : packs_) if (pack.preview.bars.empty()) pack.preview_done = false;
}

bool ReplayLibraryWidget::is_http_url(const std::string& value) {
    if (!(value.rfind("https://", 0) == 0 || value.rfind("http://", 0) == 0)) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    });
}

std::string ReplayLibraryWidget::format_duration(int64_t seconds) {
    char out[32];
    if (seconds < 60) {
        std::snprintf(out, sizeof(out), "%llds", static_cast<long long>(seconds));
    } else if (seconds < 3600) {
        std::snprintf(out, sizeof(out), "%lldm", static_cast<long long>(seconds / 60));
    } else {
        const auto hours = seconds / 3600;
        const auto minutes = (seconds % 3600) / 60;
        if (minutes == 0) {
            std::snprintf(out, sizeof(out), "%lldh", static_cast<long long>(hours));
        } else {
            std::snprintf(out, sizeof(out), "%lldh %lldm",
                          static_cast<long long>(hours), static_cast<long long>(minutes));
        }
    }
    return out;
}

std::string ReplayLibraryWidget::format_size(int64_t bytes) {
    char out[32];
    if (bytes >= 1024LL * 1024LL * 1024LL) {
        std::snprintf(out, sizeof(out), "%.1f GB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024LL * 1024LL) {
        std::snprintf(out, sizeof(out), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024LL) {
        std::snprintf(out, sizeof(out), "%.1f KB",
                      static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(out, sizeof(out), "%lld B", static_cast<long long>(bytes));
    }
    return out;
}

bool ReplayLibraryWidget::parse_manifest(
    const std::string& payload, std::string& parse_error) {
    const auto doc = json::parse(payload, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        parse_error = "Catalog returned invalid JSON";
        return false;
    }
    try {
    if (doc.value("schema", std::string()) != "edgedepth.replay-library.v1") {
        parse_error = "Catalog schema is not supported by this terminal build";
        return false;
    }
    const auto it = doc.find("packs");
    if (it == doc.end() || !it->is_array()) {
        parse_error = "Catalog has no packs array";
        return false;
    }

    std::vector<Pack> parsed;
    parsed.reserve(std::min(kMaxPacks, it->size()));
    for (const auto& item : *it) {
        if (parsed.size() >= kMaxPacks) break;
        if (!item.is_object()) continue;

        Pack pack;
        pack.id = item.value("id", std::string());
        pack.title = item.value("title", std::string());
        pack.summary = item.value("summary", std::string());
        pack.exchange = item.value("exchange", std::string());
        pack.symbol = item.value("symbol", std::string());
        pack.display_symbol = item.value("display_symbol", std::string());
        pack.captured_at = item.value("captured_at", std::string());
        pack.pack_url = item.value("pack_url", std::string());
        pack.duration_seconds = item.value("duration_seconds", int64_t{0});
        pack.size_bytes = item.value("size_bytes", int64_t{0});
        pack.featured = item.value("featured", false);
        pack.exchange_id = item.value("exchange_id", std::string());
        pack.event_id = item.value("event_id", std::string());
        pack.fingerprint = item.value("sha256", std::string());

        if (pack.id.empty() || pack.title.empty() || !is_http_url(pack.pack_url)) {
            continue;
        }
        if (pack.display_symbol.empty()) pack.display_symbol = upper_ascii(pack.symbol);
        if (pack.exchange.empty()) pack.exchange = "Recorded market";

        pack.base_asset = pack.display_symbol.substr(0, pack.display_symbol.find('/'));
        // The venue is drawn as its mark beside the pair (James, 2026-10-17), so the
        // line carries only when, how long and how big: "9 Aug 2026 · 06:50 UTC · 27m · 74.8 MB".
        static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        const auto add = [&pack](const std::string& part) {
            if (part.empty()) return;
            if (!pack.metadata_line.empty()) pack.metadata_line += " \xC2\xB7 ";
            pack.metadata_line += part;
        };
        int year = 0, month = 0, day = 0, hour = -1, minute = -1;
        if (std::sscanf(pack.captured_at.c_str(), "%d-%d-%dT%d:%d", &year, &month, &day, &hour, &minute) >= 3 &&
            month >= 1 && month <= 12) {
            char date[32];
            std::snprintf(date, sizeof(date), "%d %s %d", day, kMonths[month - 1], year);
            add(date);
            if (hour >= 0 && minute >= 0) {
                char time[16];
                std::snprintf(time, sizeof(time), "%02d:%02d UTC", hour, minute);
                add(time);
            }
        } else if (pack.captured_at.size() >= 10) {
            add(pack.captured_at.substr(0, 10));
        }
        if (pack.duration_seconds > 0) add(format_duration(pack.duration_seconds));
        if (pack.size_bytes > 0) add(format_size(pack.size_bytes));

        if (const auto tags = item.find("tags"); tags != item.end() && tags->is_array()) {
            for (const auto& tag : *tags) {
                if (!tag.is_string() || pack.tags.size() >= 6) continue;
                std::string words = tag.get<std::string>();
                std::replace(words.begin(), words.end(), '-', ' ');
                pack.tags.push_back(std::move(words));
            }
        }
        // Preserve only previews of the exact same catalog object on refresh.
        for (auto& old : packs_) {
            if (old.id == pack.id && old.pack_url == pack.pack_url && old.fingerprint == pack.fingerprint &&
                old.symbol == pack.symbol && old.exchange_id == pack.exchange_id && old.event_id == pack.event_id) {
                pack.preview = old.preview;
                pack.preview_done = old.preview_done;
                pack.details_open = old.details_open;
                break;
            }
        }
        parsed.push_back(std::move(pack));
    }

    edrl_preview_cancel(preview_request_);
    preview_request_ = 0;
    pending_pack_ = -1;
    packs_ = std::move(parsed);
    catalog_ready_ = true;
    parse_error.clear();
    return true;
    } catch (const json::exception&) {
        parse_error = "Catalog contains an invalid field type";
        return false;
    }
}

void ReplayLibraryWidget::update() {
    update_preview();
    loading_ = edrl_loading() != 0;
    const int revision = edrl_revision();
    if (revision != manifest_revision_ && revision > 0) {
        manifest_revision_ = revision;
        if (char* payload = edrl_copy_payload()) {
            std::string parse_error;
            const bool ok = parse_manifest(payload, parse_error);
            std::free(payload);
            if (ok) error_.clear();
            else error_ = std::move(parse_error);
        }
    }

    if (edrl_status() == 3 && error_.empty()) {
        if (char* error = edrl_copy_error()) {
            error_ = error[0] ? error : "Could not load the replay catalog";
            std::free(error);
        }
    }

    // Event finder feed: apply new revisions, surface failures once.
    const int ev_revision = ed_ev_revision();
    if (ev_revision != event_revision_ && ev_revision > 0) {
        event_revision_ = ev_revision;
        if (char* payload = ed_ev_copy_payload()) {
            apply_events(payload);
            std::free(payload);
        }
    }
    if (event_requested_ && event_error_.empty() && ed_ev_failed() != 0) {
        if (char* error = ed_ev_copy_error()) {
            event_error_ = error[0] ? error : "Event search failed";
            std::free(error);
        }
    }
}

void ReplayLibraryWidget::play_pack(const Pack& pack, int64_t seek_ms) {
    ctx_.replay_mgr().request_pack_replay(pack.pack_url, pack.symbol, 1.0f, seek_ms);
    is_open = false;
}

bool ReplayLibraryWidget::render_preview(Pack& pack, ImVec2 size) {
    using namespace Theme;
    pack.preview_requested = true;
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + size.x, a.y + size.y);
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, ImGui::GetColorU32(Tokens::BASE), Radius::R2);
    const auto& bars = pack.preview.bars;
    ImGui::PushFont(Fonts::mono_sm());
    dl->AddText(ImVec2(a.x + 10, a.y + 8), ImGui::GetColorU32(Tokens::TX3), "1m candles");
    if (bars.empty()) {
        dl->AddText(ImVec2(a.x + 10, a.y + size.y * .5f), ImGui::GetColorU32(Tokens::TX3),
            pack.preview_done ? "Preview unavailable" : "Loading market preview...");
    } else {
        double lo = bars.front().low, hi = bars.front().high;
        for (const auto& bar : bars) { lo = std::min(lo, bar.low); hi = std::max(hi, bar.high); }
        const double pad = std::max((hi - lo) * .1, hi * .0001);
        lo -= pad; hi += pad;
        const float left = a.x + 10, right = b.x - 10, top = a.y + 29, bottom = b.y - 25;
        auto y = [&](double price) { return bottom - static_cast<float>((price - lo) / (hi - lo)) * (bottom - top); };
        auto x = [&](int64_t ts) { return left + static_cast<float>(double(ts - pack.preview.start_ms) /
            double(pack.preview.end_ms - pack.preview.start_ms)) * (right - left); };
        for (int i = 0; i <= 2; ++i) {
            const float gy = top + (bottom - top) * i * .5f;
            dl->AddLine(ImVec2(left, gy), ImVec2(right, gy), ImGui::GetColorU32(Tokens::BD1));
        }
        const float half = std::clamp((x(pack.preview.start_ms + 60000) - left) * .3f, 1.0f, 5.0f);
        for (const auto& bar : bars) {
            const float cx = x(bar.time_ms + 30000);
            const ImU32 color = ImGui::GetColorU32(bar.close >= bar.open ? Tokens::UP : Tokens::DOWN);
            dl->AddLine(ImVec2(cx, y(bar.high)), ImVec2(cx, y(bar.low)), color);
            const float yt = y(std::max(bar.open, bar.close)), yb = y(std::min(bar.open, bar.close));
            dl->AddRectFilled(ImVec2(cx-half, yt), ImVec2(cx+half, std::max(yt + 1.0f, yb)), color);
        }
        char change[32];
        snprintf(change, sizeof(change), "Clip net %+.1f%%", (bars.back().close / bars.front().open - 1) * 100);
        dl->AddText(ImVec2(b.x - ImGui::CalcTextSize(change).x - 10, a.y + 8),
            ImGui::GetColorU32(bars.back().close >= bars.front().open ? Tokens::UP : Tokens::DOWN), change);
        auto utc = [](int64_t ms, char* out) {
            const auto seconds = static_cast<std::time_t>(ms / 1000);
            const auto* tm = std::gmtime(&seconds);
            if (tm) std::strftime(out, 16, "%H:%M", tm); else snprintf(out, 16, "--:--");
        };
        char first[16], last[16], window[48];
        utc(bars.front().time_ms, first); utc(bars.back().time_ms + 60000, last);
        snprintf(window, sizeof(window), "%s - %s UTC", first, last);
        dl->AddText(ImVec2(left, b.y - 17), ImGui::GetColorU32(Tokens::TX3), window);
    }
    ImGui::PopFont();
    // The preview is also the play button: under the pointer it dims and shows a round play mark.
    const bool clicked = ImGui::InvisibleButton("##preview", size);
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 120), Radius::R2);
        const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const float r = ImGui::IsItemActive() ? 19.0f : 20.0f;
        dl->AddCircleFilled(c, r, ImGui::GetColorU32(Tokens::TX1), 32);
        drawing::draw_ui_icon(dl, drawing::UiIcon::Play, ImVec2(c.x + 1.5f, c.y), 8.0f, ImGui::GetColorU32(Tokens::BRAND_INK), 1.6f);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay))
        Theme::tooltip("Click to replay. Actual complete 1-minute candles inside this recording.\nPre-roll and bars crossing the end are excluded. Gaps stay gaps.\nThe change is for the displayed bars, not the whole event.");
    return clicked;
}

void ReplayLibraryWidget::request_play(int index) {
    if (index < 0 || index >= static_cast<int>(packs_.size())) return;
    if (ctx_.replay_mgr().is_active()) { pending_pack_ = index; replace_popup_requested_ = true; }
    else play_pack(packs_[index]);
}

// ── Event finder (roadmap Phase 4) ────────────────────────────────────────────
// "همهٔ لحظه‌هایی که cascade>500k$ بود" - every cascade, liquidation, outsized
// fill and volume spike the 24/7 recorder captured, searchable and replayable
// straight into its .edpack recording.

void ReplayLibraryWidget::split_manifest_url(std::string& base, std::string& query) const {
    const auto qpos = manifest_url_.find('?');
    const std::string noq = qpos == std::string::npos ? manifest_url_ : manifest_url_.substr(0, qpos);
    query = qpos == std::string::npos ? std::string() : manifest_url_.substr(qpos + 1);
    const auto slash = noq.rfind('/');
    base = slash == std::string::npos ? std::string() : noq.substr(0, slash + 1);
}

std::string ReplayLibraryWidget::event_endpoint_url(const char* endpoint) const {
    std::string base, query;
    split_manifest_url(base, query);
    return base + endpoint + (query.empty() ? "" : "?" + query);
}

void ReplayLibraryWidget::begin_event_fetch() {
    event_error_.clear();
    event_hits_.clear();  // a new search replaces the old results
    std::string base, query;
    split_manifest_url(base, query);
    static const char* const kKinds[] = {"all", "cascade", "liq", "trade", "vol"};
    std::string symbol;
    for (const unsigned char c : std::string(event_symbol_))
        if (std::isalnum(c)) symbol += static_cast<char>(std::tolower(c));
    std::string path = std::string("events?kind=") + kKinds[event_kind_] +
                       "&min_usd=" + std::to_string(static_cast<long long>(std::max(0.0, event_min_usd_))) +
                       "&limit=300";
    if (!symbol.empty()) path += "&symbol=" + symbol;
    if (!query.empty()) path += "&" + query;
    ed_ev_fetch_begin(base.c_str(), path.c_str());
    event_requested_ = true;
}

void ReplayLibraryWidget::apply_events(const std::string& payload) {
    const auto doc = json::parse(payload, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        event_error_ = "Event feed returned invalid JSON";
        return;
    }
    // NOTE: this WASM build runs with exception catching disabled, so a
    // throwing nlohmann accessor (item.value on a null/foreign-typed field)
    // would abort the whole terminal instead of unwinding. Every access below
    // goes through non-throwing find() + explicit type checks - the same
    // reason parse_manifest's value() calls only touch fields the manifest
    // always ships.
    auto get_i64 = [](const json& o, const char* key, int64_t dflt) -> int64_t {
        const auto f = o.find(key);
        if (f == o.end() || f->is_null() || !f->is_number_integer()) return dflt;
        return f->get<int64_t>();
    };
    auto get_f64 = [](const json& o, const char* key, double dflt) -> double {
        const auto f = o.find(key);
        if (f == o.end() || f->is_null() || !(f->is_number_float() || f->is_number_integer()))
            return dflt;
        return f->get<double>();
    };
    auto get_str = [](const json& o, const char* key) -> std::string {
        const auto f = o.find(key);
        return f != o.end() && f->is_string() ? f->get<std::string>() : std::string();
    };
    try {
        if (get_str(doc, "schema") != "edgedepth.events.v1") {
            event_error_ = "This catalog does not serve the event finder";
            return;
        }
        const auto it = doc.find("events");
        if (it == doc.end() || !it->is_array()) {
            event_error_ = "Event feed has no events array";
            return;
        }
        std::vector<EventHit> hits;
        for (const auto& item : *it) {
            if (!item.is_object()) continue;
            EventHit h;
            h.ts_ms = get_i64(item, "ts_ms", 0);
            h.end_ms = get_i64(item, "end_ms", 0);
            const auto kind = get_str(item, "kind");
            h.kind = kind == "cascade" ? 0 : kind == "liq" ? 1 : kind == "trade" ? 2 : 3;
            const auto side = get_str(item, "side");
            h.side = (side == "short" || side == "buy") ? 1 : 0;
            h.usd = get_f64(item, "usd", 0.0);
            h.price = get_f64(item, "price", 0.0);
            h.count = get_i64(item, "count", 0);
            h.detail = get_str(item, "detail");
            h.pack_file = get_str(item, "pack");
            h.symbol = get_str(item, "symbol");
            {
                const auto f = item.find("live");
                h.live = f != item.end() && f->is_boolean() && f->get<bool>();
            }
            if (h.ts_ms <= 0) continue;
            // Resolve the containing pack inside the loaded catalog: absolute
            // pack URLs carry /packs/<file> (plus a routing query suffix, so
            // this is a substring match, not an ends-with). Missing = the
            // recording was pruned since the moment happened.
            h.pack_index = -1;
            if (!h.pack_file.empty()) {
                const std::string needle = "/packs/" + h.pack_file;
                for (std::size_t i = 0; i < packs_.size(); ++i) {
                    if (packs_[i].pack_url.find(needle) != std::string::npos) {
                        h.pack_index = static_cast<int>(i);
                        break;
                    }
                }
            }
            hits.push_back(std::move(h));
            if (hits.size() >= 300) break;
        }
        event_hits_ = std::move(hits);
    } catch (const json::exception&) {
        // Belt and braces: nothing above throws by construction any more.
        event_error_ = "Event feed contains an invalid field type";
    }
}

void ReplayLibraryWidget::play_event(int hit_index) {
    if (hit_index < 0 || hit_index >= static_cast<int>(event_hits_.size())) return;
    const EventHit& hit = event_hits_[hit_index];
    if (hit.pack_index < 0 || hit.pack_index >= static_cast<int>(packs_.size())) return;
    // Pre-roll 45s so book and tape are already flowing when the moment hits.
    // The seek latch lives in ReplayManager (apply_pending_pack_seek): the
    // widget closes itself here, long before the session primes.
    const int64_t want = std::max<int64_t>(hit.ts_ms - 45000, 0);
    if (ctx_.replay_mgr().is_active()) {
        pending_pack_ = hit.pack_index;
        pending_event_seek_ms_ = want;
        replace_popup_requested_ = true;
    } else {
        play_pack(packs_[hit.pack_index], want);
    }
}

// Mirrored into ReplayManager::apply_pending_pack_seek (runs per frame for
// every replay, regardless of this widget's open state).

namespace {
std::string format_usd_compact(double value) {
    char out[32];
    const double a = value < 0 ? -value : value;
    if (a >= 1e9) std::snprintf(out, sizeof(out), "$%.2fB", value / 1e9);
    else if (a >= 1e6) std::snprintf(out, sizeof(out), "$%.2fM", value / 1e6);
    else if (a >= 1e3) std::snprintf(out, sizeof(out), "$%.1fK", value / 1e3);
    else std::snprintf(out, sizeof(out), "$%.0f", value);
    return out;
}
} // namespace

void ReplayLibraryWidget::render_event_finder() {
    using namespace Theme;
    using drawing::UiIcon;

    const float top = ImGui::GetCursorPosY();
    ImGui::PushFont(Fonts::heading());
    const float head_h = ImGui::GetTextLineHeight();
    ImGui::TextUnformatted("Event finder");
    ImGui::PopFont();
    ImGui::SameLine(0.0f, 9.0f);
    ImGui::SetCursorPosY(top + (head_h - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::TextColored(Tokens::TX3, "search the 24/7 personal archive");
    if (ImGui::IsItemHovered())
        Theme::tooltip(
            "Cascades, liquidations, outsized fills and volume spikes captured by the recorder daemon. "
            "Play opens the recording 45s before the moment, paused at it.");
    // The recorder daemon's live HTML report for today (same server as the feed).
    constexpr float kReportW = 108.0f;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - kReportW);
    ImGui::SetCursorPosY(top + (head_h - 24.0f) * 0.5f);
    if (Theme::button("Daily report", ButtonKind::Ghost, ImVec2(kReportW, 24.0f), UiIcon::ArrowUpRight))
        edrl_open_url(event_endpoint_url("report").c_str());

    static const char* const kKinds[] = {"All", "Cascades", "Liquidations", "Big fills", "Volume spikes"};
    event_kind_ = segmented("##event_kind", kKinds, 5, event_kind_);
    if (ImGui::IsItemHovered())
        Theme::tooltip("What counts as a moment: cascade chains (3+ same-side liquidations within 2s), single liquidations, fills over the threshold, and 1m candles trading at 5x the hour's average.");

    ImGui::SetNextItemWidth(190.0f);
    if (ImGui::InputDouble("##event_min_usd", &event_min_usd_, 50000.0, 250000.0, "Min $%.0f"))
        event_min_usd_ = std::max(0.0, event_min_usd_);
    ImGui::SameLine(0.0f, 10.0f);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputTextWithHint("##event_symbol", "any symbol", event_symbol_, sizeof(event_symbol_));
    ImGui::SameLine(0.0f, 10.0f);
    const bool searching = ed_ev_loading() != 0;
    if (Theme::button(searching ? "Searching" : "Search", ButtonKind::Primary, ImVec2(96.0f, 0.0f)) && !searching)
        begin_event_fetch();

    if (!event_error_.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(Tokens::DOWN, "%s", event_error_.c_str());
        if (event_error_.find("HTTP 404") != std::string::npos)
            ImGui::TextColored(Tokens::TX3,
                "The event feed lives on the self-hosted recorder. Open the library with "
                "?replayLibrary=/manifest.json?XTransformPort=9002 (or your recorder's address).");
    } else if (searching) {
        ImGui::Spacing();
        ImGui::TextColored(Tokens::TX3, "Searching the archive...");
    } else if (!event_requested_) {
        ImGui::Spacing();
        ImGui::TextColored(Tokens::TX3, "Pick a kind, a dollar floor and search every recorded moment.");
    } else if (event_hits_.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(Tokens::TX3, "No matching moments in the archive yet.");
    } else {
        char found[64];
        std::snprintf(found, sizeof(found), "%d moment%s found", static_cast<int>(event_hits_.size()),
                      event_hits_.size() == 1 ? "" : "s");
        ImGui::Spacing();
        Theme::chip(found, Tokens::TX2, Tokens::CONTROL);
    }

    if (!event_hits_.empty()) {
        const float list_h = std::min(280.0f, std::max(160.0f, ImGui::GetContentRegionAvail().y * 0.45f));
        ImGui::PushFont(Fonts::mono_sm());
        if (ImGui::BeginTable("##event_hits", 5,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoBordersInBody,
                              ImVec2(0.0f, list_h))) {
            ImGui::TableSetupScrollFreeze(0, 0);
            ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthFixed, 122.0f);
            ImGui::TableSetupColumn("kind", ImGuiTableColumnFlags_WidthFixed, 104.0f);
            ImGui::TableSetupColumn("side", ImGuiTableColumnFlags_WidthFixed, 62.0f);
            ImGui::TableSetupColumn("usd", ImGuiTableColumnFlags_WidthFixed, 84.0f);
            ImGui::TableSetupColumn("detail", ImGuiTableColumnFlags_WidthStretch);
            static const char* const kKindLabels[] = {"cascade", "liquidation", "big fill", "vol spike"};
            for (int i = 0; i < static_cast<int>(event_hits_.size()); ++i) {
                const EventHit& h = event_hits_[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                // UTC stamp (the archive's clock), one row per moment.
                const auto seconds = static_cast<std::time_t>(h.ts_ms / 1000);
                char tbuf[24];
                if (const auto* tm = std::gmtime(&seconds)) std::strftime(tbuf, sizeof(tbuf), "%m-%d %H:%M:%S", tm);
                else std::snprintf(tbuf, sizeof(tbuf), "--");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(tbuf);
                ImGui::TableNextColumn();
                const bool bullish = h.side != 0;
                const ImVec4& fg = h.kind == 0 ? Tokens::WARN
                                 : h.kind == 2 ? Tokens::TX2
                                 : bullish ? Tokens::UP : Tokens::DOWN;
                Theme::chip(kKindLabels[h.kind], fg, Tokens::CONTROL);
                ImGui::TableNextColumn();
                if (h.kind == 2) ImGui::TextColored(bullish ? Tokens::UP : Tokens::DOWN, "%s", bullish ? "buy" : "sell");
                else ImGui::TextColored(bullish ? Tokens::UP : Tokens::DOWN, "%s", bullish ? "shorts" : "longs");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(format_usd_compact(h.usd).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(h.detail.c_str());
                if (ImGui::IsItemHovered()) {
                    const auto full = static_cast<std::time_t>(h.ts_ms / 1000);
                    char iso[40] = "";
                    if (const auto* tm = std::gmtime(&full)) std::strftime(iso, sizeof(iso), "%Y-%m-%d %H:%M:%S UTC", tm);
                    if (h.pack_index >= 0)
                        Theme::tooltip("%s\n%s\nClick Play to open this recording 45s before the moment.",
                                       iso, h.detail.c_str());
                    else if (h.live)
                        Theme::tooltip("%s\n%s\nThe episode holding this moment is still being recorded -\nit becomes replayable when the hour rolls over.",
                                       iso, h.detail.c_str());
                    else
                        Theme::tooltip("%s\n%s\nThe containing recording has been pruned from the archive.",
                                       iso, h.detail.c_str());
                }
                const bool playable = h.pack_index >= 0 && h.pack_index < static_cast<int>(packs_.size());
                if (playable) {
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 54.0f);
                    if (Theme::button("Play", ButtonKind::Ghost, ImVec2(50.0f, 20.0f), UiIcon::Play))
                        play_event(i);
                } else if (h.live) {
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 82.0f);
                    ImGui::TextColored(Tokens::TX3, "recording");
                } else {
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 56.0f);
                    ImGui::TextColored(Tokens::TX3, "pruned");
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopFont();
    }
}

// One recording as a card (design v3, 2026-10-17; James: the library looked "slightly low quality"):
// a rounded surface whose hairline brightens under the pointer; the coin and venue as their marks;
// Featured as a badge; tags as chips; one filled Replay with its glyph, a quiet Details and the rest
// behind a three-dot menu. The card sizes to its content (AutoResizeY), so no height arithmetic.
void ReplayLibraryWidget::render_pack(Pack& pack, int index) {
    using namespace Theme;
    using drawing::UiIcon;
    ImGui::PushID(pack.id.c_str());
    constexpr float kPad = 14.0f, kGap = 18.0f, kPreviewH = 138.0f;
    const float width = ImGui::GetContentRegionAvail().x;
    const bool wide = width >= 640.0f;
    const float preview_w = wide ? 252.0f : width - kPad * 2.0f;
    const float text_w = wide ? width - preview_w - kPad * 2.0f - kGap : width - kPad * 2.0f;
    std::string name = pack.title;
    if (auto comma = name.find(','); comma != std::string::npos) name.resize(comma);
    const auto base = pack.base_asset + " ";
    if (name.rfind(base, 0) == 0) name.erase(0, base.size());
    if (!name.empty()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, Tokens::CARD);
    ImGui::PushStyleColor(ImGuiCol_Border, hovered_pack_ == index ? Tokens::BD2 : Tokens::BD1);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Radius::R3);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPad, kPad));
    bool play = false;
    const bool visible = ImGui::BeginChild("##pack", ImVec2(0, 0),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (visible) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorPos();
        play |= render_preview(pack, ImVec2(preview_w, kPreviewH));
        if (wide) ImGui::SetCursorPos(ImVec2(origin.x + preview_w + kGap, origin.y + 2.0f));
        else ImGui::Dummy(ImVec2(0.0f, 4.0f));
        const float col_x = ImGui::GetCursorPosX();
        ImGui::BeginGroup();

        // Identity: coin mark, pair, venue mark, Featured.
        ImGui::PushFont(Fonts::heading());
        const float line_h = ImGui::GetTextLineHeight();
        const ImVec2 row = ImGui::GetCursorScreenPos();
        constexpr float kCoin = 22.0f, kVenue = 15.0f;
        LogoManager::instance().draw_coin(dl, pack.base_asset, ImVec2(row.x, row.y + (line_h - kCoin) * 0.5f), kCoin);
        ImGui::SetCursorScreenPos(ImVec2(row.x + kCoin + 9.0f, row.y));
        ImGui::TextUnformatted(pack.display_symbol.c_str());
        ImGui::PopFont();
        if (!pack.exchange_id.empty()) {
            ImGui::SameLine(0.0f, 9.0f);
            const ImVec2 v = ImGui::GetCursorScreenPos();
            LogoManager::instance().draw_exchange(dl, pack.exchange_id, ImVec2(v.x, row.y + (line_h - kVenue) * 0.5f), kVenue);
            ImGui::Dummy(ImVec2(kVenue, line_h));
            if (ImGui::IsItemHovered()) Theme::tooltip("%s", pack.exchange.c_str());
        }
        if (pack.featured) {
            ImGui::SameLine(0.0f, 10.0f);
            push_font(Fonts::ui(), 11.5f);
            const char* label = "Featured";
            const ImVec2 ts = ImGui::CalcTextSize(label);
            const float h = ts.y + 6.0f, glyph = 4.5f, w = 8.0f + glyph * 2.0f + 5.0f + ts.x + 9.0f;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float y = row.y + (line_h - h) * 0.5f;
            dl->AddRectFilled(ImVec2(at.x, y), ImVec2(at.x + w, y + h), ImGui::GetColorU32(Tokens::CONTROL), h * 0.5f);
            drawing::draw_ui_icon(dl, UiIcon::Star, ImVec2(at.x + 8.0f + glyph, y + h * 0.5f), glyph,
                                  ImGui::GetColorU32(Tokens::WARN), 1.2f);
            dl->AddText(ImVec2(at.x + 8.0f + glyph * 2.0f + 5.0f, y + 3.0f), ImGui::GetColorU32(Tokens::TX1), label);
            ImGui::Dummy(ImVec2(w, line_h));
            ImGui::PopFont();
            if (ImGui::IsItemHovered()) Theme::tooltip("Picked for how clearly it shows the order flow behind a move.");
        }

        // What it is, then when, how long and how big.
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        ImGui::TextColored(Tokens::TX1, "%s", name.c_str());
        ImGui::PushFont(Fonts::mono_sm());
        ImGui::PushTextWrapPos(col_x + text_w);
        ImGui::TextColored(Tokens::TX3, "%s", pack.metadata_line.c_str());
        ImGui::PopTextWrapPos();

        // Tags as chips, wrapped to the column.
        if (!pack.tags.empty()) {
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            const float right = ImGui::GetCursorScreenPos().x + text_w;
            for (size_t i = 0; i < pack.tags.size(); ++i) {
                const float w = ImGui::CalcTextSize(pack.tags[i].c_str()).x + 14.0f;
                if (i > 0) {
                    ImGui::SameLine(0.0f, 6.0f);
                    if (ImGui::GetCursorScreenPos().x + w > right) ImGui::NewLine();
                }
                chip(pack.tags[i].c_str(), Tokens::TX2, Tokens::CONTROL);
            }
        }
        ImGui::PopFont();

        // Actions: Replay, Details, and the rest behind the three dots at the column's right.
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        constexpr float kButtonH = 28.0f;
        play |= button("Replay", ButtonKind::Soft, ImVec2(0.0f, kButtonH), UiIcon::Play);
        if (ui_test::enabled()) ui_test::mark("library.play." + pack.id);
        ImGui::SameLine(0.0f, 6.0f);
        if (button(pack.details_open ? "Hide details" : "Details", ButtonKind::Ghost, ImVec2(0.0f, kButtonH)))
            pack.details_open = !pack.details_open;
        ImGui::SameLine(text_w - kButtonH);  // inside the group, SameLine counts from the group's start
        if (icon_button("##more", UiIcon::More, "More", kButtonH)) ImGui::OpenPopup("##pack_actions");
        if (begin_menu("##pack_actions", 210.0f)) {
            if (menu_item(UiIcon::Copy, "Copy link")) {
                edrl_copy_url(pack.pack_url.c_str());
                notice_ = "Recording link copied"; notice_until_ = ImGui::GetTime() + 2.0;
            }
            if (menu_item(UiIcon::ArrowUpRight, "Download .edpack")) edrl_open_url(pack.pack_url.c_str());
            ImGui::EndPopup();
        }
        if (pack.details_open) {
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            ImGui::PushTextWrapPos(col_x + text_w);
            ImGui::TextColored(Tokens::TX2, "%s", pack.summary.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndGroup();
        // A wide card is as tall as its taller column.
        if (wide) {
            const float bottom = std::max(ImGui::GetCursorPosY(), origin.y + kPreviewH);
            ImGui::SetCursorPosY(bottom);
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
        }
    }
    ImGui::EndChild();
    // After EndChild the card is the item; hovering anything inside it hovers the card.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)) hovered_pack_ = index;
    else if (hovered_pack_ == index) hovered_pack_ = -1;
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::PopID();
    if (play) request_play(index);
}

void ReplayLibraryWidget::render_replace_confirmation() {
    ImGui::SetNextWindowSize(ImVec2(390.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Replace current replay?", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextWrapped("Starting this recording will stop the replay that is currently open.");
    ImGui::Spacing();
    if (Theme::button("Cancel", Theme::ButtonKind::Ghost, ImVec2(100.0f, 28.0f))) {
        pending_pack_ = -1;
        pending_event_seek_ms_ = 0;  // event replay cancelled with the popup
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (Theme::button("Replace and replay", Theme::ButtonKind::Primary, ImVec2(170.0f, 28.0f), drawing::UiIcon::Play)) {
        if (pending_pack_ >= 0 && pending_pack_ < static_cast<int>(packs_.size())) {
            play_pack(packs_[pending_pack_], pending_event_seek_ms_);
        }
        pending_pack_ = -1;
        pending_event_seek_ms_ = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ReplayLibraryWidget::render() {
    if (!is_open) return;

    place_new_window();  // before our own size, so it wins
    // Roomy, but never wider or taller than the screen: an 850 px window opened half off a narrow one.
    const ImVec2 work = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSize(ImVec2(std::min(850.0f, work.x - 40.0f), std::min(850.0f, work.y - 40.0f)),
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 360.0f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    if (!ImGui::Begin(title(), &is_open)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    // Title, a count chip, and Refresh as an icon at the right.
    const float top = ImGui::GetCursorPosY();
    ImGui::PushFont(Theme::Fonts::heading());
    const float title_h = ImGui::GetTextLineHeight();
    ImGui::TextUnformatted("Replay Library");
    ImGui::PopFont();
    if (catalog_ready_ && !packs_.empty()) {
        ImGui::SameLine(0.0f, 10.0f);
        char count[32];
        std::snprintf(count, sizeof(count), "%zu recording%s", packs_.size(), packs_.size() == 1 ? "" : "s");
        ImGui::SetCursorPosY(top + (title_h - (ImGui::GetTextLineHeight() + 4.0f)) * 0.5f);
        Theme::chip(count, Theme::Tokens::TX2, Theme::Tokens::CONTROL);
    }
    constexpr float kIcon = 26.0f;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - kIcon);
    ImGui::SetCursorPosY(top + (title_h - kIcon) * 0.5f);
    if (Theme::icon_button("##refresh", drawing::UiIcon::Refresh, loading_ ? "Refreshing..." : "Refresh the library", kIcon) && !loading_)
        begin_fetch(true);
    ImGui::SetCursorPosY(top + std::max(title_h, kIcon) + 4.0f);

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(
        Theme::Tokens::TX3,
        "Short excerpts of larger moves. Clip net shows the preview's change, not the full event's move.");
    ImGui::PopTextWrapPos();
    if (ImGui::IsItemHovered()) {
        Theme::tooltip("Manifest: %s", manifest_url_.c_str());
    }

    // Event finder: only meaningful once a catalog context exists, but the
    // feed derives purely from the manifest URL, so render it regardless.
    render_event_finder();
    Theme::hairline(ImGui::GetContentRegionAvail().x, 8.0f);

    if (!notice_.empty() && ImGui::GetTime() < notice_until_) {
        ImGui::TextColored(Theme::Tokens::BRAND_TX, "%s", notice_.c_str());
    }

    if (!catalog_ready_ && loading_) {
        ImGui::Spacing();
        ImGui::TextColored(Theme::Tokens::TX2, "Loading curated recordings...");
    } else if (!catalog_ready_ && !error_.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(Theme::Tokens::DOWN, "Could not load the replay library");
        ImGui::TextWrapped("%s", error_.c_str());
        ImGui::Spacing();
        if (Theme::button("Try again", Theme::ButtonKind::Soft, ImVec2(0.0f, 28.0f), drawing::UiIcon::Refresh)) begin_fetch(true);
        ImGui::SameLine();
        if (Theme::button("Copy manifest URL", Theme::ButtonKind::Ghost, ImVec2(0.0f, 28.0f), drawing::UiIcon::Copy)) {
            edrl_copy_url(manifest_url_.c_str());
            notice_ = "Manifest URL copied";
            notice_until_ = ImGui::GetTime() + 2.0;
        }
    } else if (packs_.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(Theme::Tokens::TX2, "No recordings are published yet.");
    } else {
        const float footer_height = Entitlements::is_pro() ? 44.0f : 82.0f;
        const float list_height = std::max(
            120.0f, ImGui::GetContentRegionAvail().y - footer_height);
        ImGui::BeginChild("##replay_library_list", ImVec2(0.0f, list_height), false);
        for (int i = 0; i < static_cast<int>(packs_.size()); ++i) {
            render_pack(packs_[i], i);
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
        }
        ImGui::EndChild();
        if (replace_popup_requested_) {
            ImGui::OpenPopup("Replace current replay?");
            replace_popup_requested_ = false;
        }
        render_replace_confirmation();

        Theme::hairline(ImGui::GetContentRegionAvail().x, 8.0f);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(Theme::Tokens::TX3, "Pause, slow down and scrub to inspect how the move unfolded.");
        ImGui::PopTextWrapPos();
        if (!Entitlements::is_pro()) {
            ImGui::TextColored(Theme::Tokens::TX3, "These recordings are free. Explore more historical markets with Pro.");
            if (Theme::button("Explore replay plans", Theme::ButtonKind::Ghost, ImVec2(0.0f, 26.0f), drawing::UiIcon::ArrowUpRight, true))
                edrl_open_url("https://edgedepth.com/pricing?utm_source=terminal&utm_medium=replay_library");
        }

    }

    ImGui::End();
    ImGui::PopStyleVar();
}
