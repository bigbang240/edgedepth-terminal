#include "ui/replay_library_widget.h"

#include "rendering/theme.h"
#include "core/logo_manager.h"
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
        return text;
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
}

void ReplayLibraryWidget::play_pack(const Pack& pack) {
    ctx_.replay_mgr().request_pack_replay(pack.pack_url, pack.symbol, 1.0f);
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
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (Theme::button("Replace and replay", Theme::ButtonKind::Primary, ImVec2(170.0f, 28.0f), drawing::UiIcon::Play)) {
        if (pending_pack_ >= 0 && pending_pack_ < static_cast<int>(packs_.size())) {
            play_pack(packs_[pending_pack_]);
        }
        pending_pack_ = -1;
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
