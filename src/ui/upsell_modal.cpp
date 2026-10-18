// ═══════════════════════════════════════════════════════════════════════════════
// upsell_modal.cpp - see upsell_modal.h. The single free→Pro conversion surface.
// ═══════════════════════════════════════════════════════════════════════════════

#include "upsell_modal.h"
#include <algorithm>

#include "imgui.h"
#include <cstdio>
#include <cfloat>
#include <cmath>
#include <nlohmann/json.hpp>
#include "../rendering/theme.h"
#include "drawing/drawing_icons.h"
#include "../core/entitlements.h"
#include "../core/usage_emit.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <GLES3/gl3.h>   // preview clip texture
#endif

namespace ui {

namespace {

// ── Preview clips ────────────────────────────────────────────────────────────
// Every gate shows a short recorded loop of what it unlocks (replay, real-time
// depth, Hyperliquid levels, sub-minute candles), served by the web host from
// public/media/terminal/<name>.webm (VP9) with an .mp4 (H.264) fallback, both
// same-origin so the page's COEP needs nothing extra. A detached, muted,
// looping <video> decodes them; each new frame is uploaded into a GL texture
// between NewFrame and Render (the recorder's cam-bubble pattern, see
// recorder_glue.cpp) and drawn with AddImage. Where the files are missing
// (a local build server, the OSS terminal) the panel says so and the dialog
// works as before.
#ifdef __EMSCRIPTEN__
EM_JS(void, upsell_js_preview_play, (const char* name), {
    var st = Module['__edupsell'] || (Module['__edupsell'] = {});
    var n = UTF8ToString(name);
    if (st.v && st.name === n) { st.v.play().catch(function () {}); return; }
    if (st.v) {
        try { st.v.pause(); while (st.v.firstChild) st.v.removeChild(st.v.firstChild); st.v.load(); } catch (_) {}
    }
    var v = document.createElement('video');
    v.muted = true; v.defaultMuted = true; v.loop = true; v.playsInline = true; v.preload = 'auto';
    v.setAttribute('muted', ''); v.setAttribute('playsinline', '');
    st.v = v; st.name = n; st.t = -1; st.failed = false;
    [['.webm', 'video/webm'], ['.mp4', 'video/mp4']].forEach(function (f) {
        var src = document.createElement('source');
        src.src = '/media/terminal/' + n + f[0];
        src.type = f[1];
        v.appendChild(src);
    });
    // A <source> error never reaches the video's own onerror; the last
    // candidate failing means nothing will play.
    v.lastChild.addEventListener('error', function () { st.failed = true; });
    v.play().catch(function () {});
});

// 1 = a frame is in `tex`, 0 = still loading, -1 = no clip available.
EM_JS(int, upsell_js_preview_frame, (int tex), {
    var st = Module['__edupsell'];
    var v = st && st.v;
    if (!v) return 0;
    if (st.failed) return -1;
    if (v.readyState < 2 || !v.videoWidth) return 0;
    if (v.currentTime !== st.t) {
        var glTex = GL.textures[tex];
        if (!glTex) return 0;
        GLctx.bindTexture(GLctx.TEXTURE_2D, glTex);
        GLctx.pixelStorei(GLctx.UNPACK_FLIP_Y_WEBGL, false);
        GLctx.texImage2D(GLctx.TEXTURE_2D, 0, GLctx.RGBA, GLctx.RGBA, GLctx.UNSIGNED_BYTE, v);
        st.t = v.currentTime;
    }
    return 1;
});

EM_JS(void, upsell_js_preview_stop, (), {
    var st = Module['__edupsell'];
    if (st && st.v) { try { st.v.pause(); } catch (_) {} }
});
#endif

// Which loop a gate shows. Replay-shaped gates show the replay clip; layer
// gates show the layer; everything else shows real-time depth. All are real
// recordings of the terminal (tools/record_upsell_previews.sh, the marketing
// clip project for replay-preview and hyperliquid-preview, and
// tools/record_liq_previews.sh for rails-preview and liquidations-preview).
const char* preview_clip(UpsellModal::Trigger t, const std::string& layer) {
    using T = UpsellModal::Trigger;
    switch (t) {
        case T::Timeframe: return "subminute-preview";
        case T::Layer:
            if (layer == "realtime_depth") return "realtime-preview";
            if (layer == "liqmap2" || layer == "liq_rails") return "rails-preview";
            if (layer == "liq_observed") return "liquidations-preview";
            if (layer.rfind("hl_", 0) == 0 || layer.rfind("liq", 0) == 0) return "hyperliquid-preview";
            return "realtime-preview";
        case T::Range: case T::Preset: case T::Speed: case T::Symbol: case T::ServerTier:
        case T::Daily: case T::Events: case T::Research: case T::Lesson: case T::Auth:
            return "replay-preview";
        default: return "realtime-preview";
    }
}

// Gates where a free account can try the same thing on BTCUSDT right now.
bool offers_btc(UpsellModal::Trigger t, const std::string& layer) {
    using T = UpsellModal::Trigger;
    if (t == T::Layer) return layer == "realtime_depth" || layer == "refill";
    return t == T::Range || t == T::Preset || t == T::Symbol || t == T::ServerTier;
}

const char* trigger_name(UpsellModal::Trigger t) {
    using T = UpsellModal::Trigger;
    switch (t) {
        case T::Range:      return "range";
        case T::Preset:     return "preset";
        case T::Speed:      return "speed";
        case T::Symbol:     return "symbol";
        case T::Layer:      return "layer";
        case T::ServerTier: return "server_tier";
        case T::Events:     return "events";
        case T::Lesson:     return "lesson";
        case T::Daily:      return "daily";
        case T::Auth:       return "auth";
        case T::Research:   return "research";
        case T::Timeframe:  return "timeframe";
        default:            return "generic";
    }
}

// Founder-rate Pro, per month. One place for the dialog's price block and its
// button; the plan records (Plans collection) are the source these mirror.
constexpr const char* kYearlyPerMonth  = "$20";   // billed $240 yearly
constexpr const char* kMonthlyPerMonth = "$29";

// Who opened the dialog currently up (UpsellModal::open's `cause`); empty = unknown.
// File-scope because every event of one opening carries it, including the ones
// emitted from inside render().
std::string g_upsell_cause;

// `layer` is passed explicitly at every call site (not defaulted) so a new event
// cannot quietly ship without deciding whether it carries the slug.
void emit_upsell_usage(const char* event, UpsellModal::Trigger trigger, bool login,
                       const std::string& layer,
                       const nlohmann::json& props = nlohmann::json::object()) {
    nlohmann::json merged = props;
    merged["source"] = "terminal";
    merged["trigger"] = trigger_name(trigger);
    merged["variant"] = login ? "login" : "pro";
    if (!g_upsell_cause.empty()) merged["cause"] = g_upsell_cause;
    // ABSENT rather than empty: `props->>'layer' is null` then means "this row
    // predates the slug or its gate has no layer", which an empty string could
    // not say. Never emit "" here.
    if (!layer.empty()) merged["layer"] = layer;
    usage::dispatch_detail(nlohmann::json{
        {"event", event},
        {"mode", "terminal"},
        {"props", std::move(merged)},
    }.dump());
}

}  // namespace

UpsellModal& UpsellModal::instance() {
    static UpsellModal inst;
    return inst;
}

// Default one-line context for a gate (overridden by open()'s `detail`).
static const char* default_subline(UpsellModal::Trigger t, bool login) {
    using T = UpsellModal::Trigger;
    if (login) return "A free account replays BTC's last 3 days.";
    switch (t) {
        case T::Range:
        case T::Preset:
        case T::ServerTier: return "Free replays BTC's last 3 days. Pro replays every market, 90 days back.";
        case T::Speed:      return "Free replays at 1\xc3\x97. Pro unlocks slow motion and up to 4\xc3\x97.";
        case T::Symbol:     return "Free replays BTC. Pro replays every recorded market.";
        case T::Layer:      return "Pro adds Hyperliquid levels, reported liquidations and leverage filters.";
        case T::Events:     return "This event is older than the free window.";
        case T::Lesson:     return "This lesson is part of Pro.";
        case T::Daily:      return "Free includes 6 replays a day. They reset at 00:00 UTC.";
        case T::Research:   return "Reading a past minute is Pro. The live read stays free.";
        case T::Timeframe:  return "Sub-minute candles show what a 1-minute bar hides.";
        default:            return "Live depth, liquidation layers and 90-day replay on every market.";
    }
}

static const char* modal_headline(UpsellModal::Trigger t, bool login) {
    using T = UpsellModal::Trigger;
    if (login) return "Log in to replay";
    switch (t) {
        case T::Range:
        case T::ServerTier: return "Replay this exact moment";
        case T::Preset:     return "Replay any moment";
        case T::Speed:      return "Choose your replay speed";
        case T::Symbol:     return "Replay every market";
        case T::Daily:      return "Today's free replays are used";
        case T::Events:     return "Replay this event";
        case T::Lesson:     return "Unlock this lesson";
        case T::Research:   return "Investigate past moments";
        case T::Timeframe:  return "Watch the minute form";
        default:            return "Unlock the full terminal";
    }
}

void UpsellModal::open(Trigger t, const char* detail, const char* layer, const char* cause) {
    g_upsell_cause = cause ? cause : "";
    trigger_ = t;
    login_variant_ = (t == Trigger::Auth);
    yearly_billing_ = true;
    detail_ = detail ? detail : "";
    // Assigned unconditionally, like detail_: this is a singleton, so a gate that
    // passes no layer must CLEAR the previous one or a Speed gate would inherit
    // the slug of the last layer pill that fired.
    layer_ = layer ? layer : "";
    subject_.clear();           // set_subject() after open(); never inherited
    preview_ = preview_clip(trigger_, layer_);
    dismiss_redirect_.clear();  // never inherit an event/lesson-boot redirect
    want_open_ = true;
    emit_upsell_usage("locked_action", trigger_, login_variant_, layer_);
}

void UpsellModal::open_login(const char* detail) {
    g_upsell_cause.clear();
    trigger_ = Trigger::Auth;
    login_variant_ = true;
    yearly_billing_ = true;
    detail_ = detail ? detail : "";
    layer_.clear();             // an auth gate is not a layer gate
    subject_.clear();
    preview_ = preview_clip(trigger_, layer_);
    dismiss_redirect_.clear();  // never inherit an event/lesson-boot redirect
    want_open_ = true;
    emit_upsell_usage("locked_action", trigger_, login_variant_, layer_);
}

void UpsellModal::set_subject(const char* subject) {
    subject_ = subject ? subject : "";
}

void UpsellModal::set_dismiss_redirect(const char* symbol) {
    // Built once here, outside the render loop; dismiss() only reads it.
    dismiss_redirect_ = (symbol && *symbol)
        ? std::string("/terminal/") + symbol
        : std::string("/terminal");
}

// Shared dismiss for "Maybe later"/"Not now" and Escape - NOT the primary CTA.
// With a redirect armed (event/lesson boot denial) this leaves the dead embedded
// chrome for the live terminal via a FULL navigation (the embedded page can't be
// repurposed in place); otherwise it just closes.
bool UpsellModal::blocks_replay_shortcuts() const {
    return open_ || want_open_ || dismissed_frame_ == ImGui::GetFrameCount();
}

void UpsellModal::stop_preview() {
    if (!preview_playing_) return;
    preview_playing_ = false;
#ifdef __EMSCRIPTEN__
    upsell_js_preview_stop();
#endif
}

void UpsellModal::dismiss(const char* how) {
    dismissed_frame_ = ImGui::GetFrameCount();
    stop_preview();
    // Closing records nothing until now, so a lock was indistinguishable from a
    // lock the viewer considered and declined. open_ms separates a reflex close
    // from a read.
    if (how && open_) {
        emit_upsell_usage("upsell_dismiss", trigger_, login_variant_, layer_,
                          {{"how", how},
                           {"open_ms", static_cast<int64_t>((ImGui::GetTime() - opened_at_) * 1000.0)}});
    }
#ifdef __EMSCRIPTEN__
    if (!dismiss_redirect_.empty()) {
        EM_ASM({ window.location.assign(UTF8ToString($0)); }, dismiss_redirect_.c_str());
    }
#endif
    ImGui::CloseCurrentPopup();
    open_ = false;
}

void UpsellModal::toast(const char* msg) {
    toast_text_   = msg ? msg : "";
    toast_active_ = true;
    toast_until_  = ImGui::GetTime() + 3.4;
}

// ── Per-frame entry point ────────────────────────────────────────────────────
void UpsellModal::render() {
    using namespace Theme;

    if (want_open_) {
        want_open_ = false;
        // Every explicit locked action opens the same reviewable plan dialog.
        toast_active_ = false;
        ImGui::OpenPopup("##edx_upsell");
        open_ = true;
        opened_at_ = ImGui::GetTime();
        stop_preview();
        if (preview_) {
            preview_playing_ = true;
#ifdef __EMSCRIPTEN__
            upsell_js_preview_play(preview_);
#endif
        }
        emit_upsell_usage("upsell_impression", trigger_, login_variant_, layer_,
                          {{"surface", "modal"}});
    }

    render_toast();

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    const ImVec2 viewport_size = ImGui::GetMainViewport()->Size;
    // A gate with a preview clip gets a second column for it, when the
    // viewport has room; otherwise the dialog stays single-column.
    const bool wide = preview_ && viewport_size.x >= 900.0f;
    const float modal_width = std::min(wide ? 1080.0f : 560.0f, viewport_size.x - 32.0f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(modal_width, 0.0f),
                                       ImVec2(modal_width, viewport_size.y - 32.0f));
    ImGui::SetNextWindowSize(ImVec2(modal_width, 0.0f), ImGuiCond_FirstUseEver);

    ImGui::PushStyleColor(ImGuiCol_PopupBg, Tokens::PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border, Tokens::BD2);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.62f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 24.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Radius::R3 + 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);

    if (ImGui::BeginPopupModal("##edx_upsell", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize)) {
        render_modal_body();
        ImGui::EndPopup();
    } else {
        open_ = false;
        stop_preview();
    }

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
}

// ── Non-blocking session notice (bottom-center, above the transport) ───────────
void UpsellModal::render_toast() {
    if (!toast_active_) return;
    const double now = ImGui::GetTime();
    if (now > toast_until_) { toast_active_ = false; return; }

    using namespace Theme;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::PushFont(Fonts::ui_semibold());
    const ImVec2 ts = ImGui::CalcTextSize(toast_text_.c_str());
    const float padx = 14.0f, pady = 9.0f;
    const float w = ts.x + padx * 2.0f, h = ts.y + pady * 2.0f;
    const float x = vp->Pos.x + (vp->Size.x - w) * 0.5f;
    const float y = vp->Pos.y + vp->Size.y - h - 88.0f;
    float a = 1.0f;
    const double remaining = toast_until_ - now;
    if (remaining < 0.4) a = static_cast<float>(remaining / 0.4);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), u32(Tokens::ELEV, a), Radius::R2);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), u32(Tokens::BRAND_LINE, a), Radius::R2, 0, 1.0f);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x + padx, y + pady),
                u32(Tokens::TX1, a), toast_text_.c_str());
    ImGui::PopFont();
}

// ── Modal body ───────────────────────────────────────────────────────────────
// Design v3 (James 2026-10-11, "improve the CTAs ... full UI/UX improvement"): a badge row with a round close, a
// clear type ladder (headline 24, context 15, benefits 14.5), the price in a rounded card with an Annual / Monthly
// switch and its saving as a chip, one rounded mint action that names what it buys, quiet secondary actions, and
// the clip in a rounded frame with a caption saying what it shows.
namespace {
const char* preview_caption(const char* clip) {
    const std::string c = clip ? clip : "";
    if (c == "subminute-preview") return "1-second candles, recorded in the terminal";
    if (c == "hyperliquid-preview") return "Hyperliquid positions by price, recorded in the terminal";
    if (c == "rails-preview") return "Liquidation rails on STRK/USDT, 10 Oct, recorded in the terminal";
    if (c == "liquidations-preview") return "Reported liquidations on STRK/USDT, 10 Oct, recorded in the terminal";
    if (c == "replay-preview") return "A recorded session, replayed in the terminal";
    return "Real-time depth and trades, recorded in the terminal";
}
// A check in a soft mint disc (the UI atlas has no check glyph).
void check_disc(ImDrawList* dl, ImVec2 c, float r) {
    using namespace Theme;
    dl->AddCircleFilled(c, r, u32(Tokens::LOGO, 0.14f), 20);
    const ImU32 ink = u32(Tokens::LOGO);
    dl->AddLine(ImVec2(c.x - r * 0.45f, c.y + r * 0.02f), ImVec2(c.x - r * 0.12f, c.y + r * 0.34f), ink, 1.6f);
    dl->AddLine(ImVec2(c.x - r * 0.12f, c.y + r * 0.34f), ImVec2(c.x + r * 0.48f, c.y - r * 0.32f), ink, 1.6f);
}
}  // namespace

void UpsellModal::render_modal_body() {
    using namespace Theme;
    const float width = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Badge row: PRO tag and the product name (or the free account), the round close at the right.
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float row_h = 30.0f;
        float x = p.x;
        if (!login_variant_) {
            const ImVec2 tag = pro_tag_size();
            draw_pro_tag(dl, ImVec2(x, p.y + (row_h - tag.y) * 0.5f));
            x += tag.x + 9.0f;
        }
        const char* name = login_variant_ ? "Your free account" : "EdgeDepth Pro";
        const float px = 13.5f;
        dl->AddText(Fonts::ui_semibold(), px, ImVec2(x, p.y + (row_h - px * 1.17f) * 0.5f), u32(Tokens::TX2), name);
        ImGui::SetCursorScreenPos(ImVec2(p.x + width - row_h + 4.0f, p.y));
        if (icon_button("##close_upgrade", drawing::UiIcon::Close, "Close", row_h)) { dismiss("close"); return; }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + row_h));
        ImGui::Dummy(ImVec2(width, 14.0f));
    }

    const bool wide = preview_ && width >= 820.0f;
    if (!wide) {
        // Narrow viewports: the clip goes above the copy when the screen is
        // tall enough to show both without scrolling (the benefit list then
        // makes way for it); otherwise the dialog stays text-only.
        const bool stacked = preview_ && ImGui::GetMainViewport()->Size.y >= 980.0f;
        if (stacked) {
            render_preview(width);
            ImGui::Dummy(ImVec2(0, 14));
        }
        render_copy(width, /*with_benefits=*/!stacked);
    } else {
        // Copy, what Pro includes and the actions on the left; the clip alone
        // on the right, so the two columns carry similar weight.
        constexpr float kCopyW = 420.0f, kGap = 36.0f;
        const float preview_w = width - kCopyW - kGap;
        ImGui::BeginChild("##upsell_copy", ImVec2(kCopyW, 0.0f), ImGuiChildFlags_AutoResizeY);
        render_copy(kCopyW, /*with_benefits=*/true);
        ImGui::EndChild();
        ImGui::SameLine(0.0f, kGap);
        ImGui::BeginChild("##upsell_preview", ImVec2(preview_w, 0.0f), ImGuiChildFlags_AutoResizeY);
        render_preview(preview_w);
        ImGui::EndChild();
    }

    if (open_ && ImGui::IsKeyPressed(ImGuiKey_Escape)) dismiss("escape");
}

void UpsellModal::render_preview(float width) {
    using namespace Theme;
    // Every clip is 1200x800 (3:2).
    const float height = std::round(width * 2.0f / 3.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 q(p.x + width, p.y + height);
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = Radius::R3;
    dl->AddRectFilled(p, q, u32(Tokens::BASE), r);
    int state = -1;
#ifdef __EMSCRIPTEN__
    if (preview_tex_ == 0) {
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        preview_tex_ = tex;
    }
    state = upsell_js_preview_frame(static_cast<int>(preview_tex_));
    if (state == 1) dl->AddImageRounded((ImTextureID)(uintptr_t)preview_tex_, p, q, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, r);
#endif
    if (state != 1) {
        const char* msg = state == 0 ? "Loading preview\xe2\x80\xa6" : "Preview unavailable here";
        const float px = 13.5f;
        const ImVec2 ts = Fonts::ui()->CalcTextSizeA(px, FLT_MAX, 0.0f, msg);
        dl->AddText(Fonts::ui(), px, ImVec2(p.x + (width - ts.x) * 0.5f, p.y + (height - ts.y) * 0.5f), u32(Tokens::TX3), msg);
    }
    dl->AddRect(p, q, u32(Tokens::BD2), r, 0, 1.0f);
    // Caption: what the loop shows, so a still frame is never mistaken for a promise.
    ImGui::Dummy(ImVec2(0, 8));
    const ImVec2 c = ImGui::GetCursorScreenPos();
    const float px = 12.5f;
    dl->AddCircleFilled(ImVec2(c.x + 4.0f, c.y + px * 0.62f), 3.0f, u32(state == 1 ? Tokens::LOGO : Tokens::TX4), 10);
    dl->AddText(Fonts::ui(), px, ImVec2(c.x + 13.0f, c.y), u32(Tokens::TX3), preview_caption(preview_));
    ImGui::Dummy(ImVec2(width, px * 1.3f));
}

void UpsellModal::render_benefits() {
    using namespace Theme;
    char replay[96];
    snprintf(replay, sizeof(replay), "%d-day replay on every market, up to 4\xc3\x97",
             Entitlements::pro_lookback_days());
    const char* items[] = {
        "Real-time depth and sub-minute candles",
        "Hyperliquid levels and reported liquidations",
        replay,
    };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    push_font(Fonts::ui(), 14.5f);
    for (const char* item : items) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight();
        check_disc(dl, ImVec2(p.x + 9.0f, p.y + h * 0.5f), 9.0f);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 28.0f, p.y));
        ImGui::TextColored(Tokens::TX1, "%s", item);
        ImGui::Dummy(ImVec2(0, 6));
    }
    ImGui::PopFont();
}

void UpsellModal::render_copy(float width, bool with_benefits) {
    using namespace Theme;
    // Hierarchy, top to bottom: what you tried to do, one line of why, what Pro adds, the price, then one filled
    // action. Secondary choices are quiet.
    const char* headline = modal_headline(trigger_, login_variant_);
    const char* context = default_subline(trigger_, login_variant_);
    if (!login_variant_ && trigger_ == Trigger::Layer) {
        if (layer_ == "hl_liq_levels" || layer_ == "hl_history") {
            headline = "See where Hyperliquid positions liquidate";
            context = "The significant levels where observed Hyperliquid positions would be forced out, drawn as rails "
                      "with a week of history. Free on BTC; Pro unlocks every coin.";
        } else if (layer_ == "liq_observed") {
            headline = "See the liquidations that printed";
            context = "Reported liquidations beside price and order flow.";
        } else if (layer_ == "liqmap2") {
            headline = "See where liquidations would fire";
            context = "Liquidation rails: the dollars forced out if price reaches each level, ended by the candle that "
                      "does. Free on BTC; Pro unlocks every market. Forward test on live Bybit data from 13 Oct.";
        } else if (layer_ == "realtime_depth") {
            headline = "See every trade meet the book";
            context = "Free on Binance BTC. Pro unlocks every market.";
        } else if (layer_ == "refill") {
            headline = "See where price keeps refilling";
            context = "Refill levels mark prices that keep getting hit and keep refilling. Free on BTC, Pro on every "
                      "other market.";
        } else {
            headline = "Read the market in more detail";
        }
    }
    const float x0 = ImGui::GetCursorPosX();
    push_font(Fonts::heading(), 24.0f);
    ImGui::PushTextWrapPos(x0 + width);
    ImGui::TextColored(Tokens::TX1, "%s", headline);
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 4));
    push_font(Fonts::ui(), 15.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 3.0f));
    ImGui::PushTextWrapPos(x0 + width);
    ImGui::TextColored(Tokens::TX2, "%s", detail_.empty() ? context : detail_.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 18));

    if (with_benefits) {
        render_benefits();
        ImGui::Dummy(ImVec2(0, 12));
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!login_variant_) {
        // The price in a rounded card: the billing switch and its saving on top, the monthly figure large, the
        // billing line under it.
        const float pad = 16.0f;
        const ImVec2 card0 = ImGui::GetCursorScreenPos();
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        ImGui::SetCursorScreenPos(ImVec2(card0.x + pad, card0.y + pad));
        push_font(Fonts::ui(), 14.0f);
        static const char* const kBilling[] = {"Annual", "Monthly"};
        const int sel = segmented("##upsell_billing", kBilling, 2, yearly_billing_ ? 0 : 1, 30.0f);
        yearly_billing_ = sel == 0;
        ImGui::SameLine(0.0f, 10.0f);
        {
            const char* save = "Save $108";
            const ImVec2 sp = ImGui::GetCursorScreenPos();
            const float px = 13.0f, tw = Fonts::ui_semibold()->CalcTextSizeA(px, FLT_MAX, 0.0f, save).x;
            const float ch = 22.0f, cy = sp.y + (30.0f - ch) * 0.5f;
            dl->AddRectFilled(ImVec2(sp.x, cy), ImVec2(sp.x + tw + 16.0f, cy + ch), u32(Tokens::LOGO, yearly_billing_ ? 0.16f : 0.08f), ch * 0.5f);
            dl->AddText(Fonts::ui_semibold(), px, ImVec2(sp.x + 8.0f, cy + (ch - px * 1.17f) * 0.5f),
                        u32(Tokens::LOGO, yearly_billing_ ? 1.0f : 0.6f), save);
            ImGui::Dummy(ImVec2(tw + 16.0f, 30.0f));
        }
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(card0.x + pad, ImGui::GetCursorScreenPos().y + 10.0f));
        const ImVec2 pp = ImGui::GetCursorScreenPos();
        const char* price = yearly_billing_ ? kYearlyPerMonth : kMonthlyPerMonth;
        const float big = 40.0f;
        dl->AddText(Fonts::heading(), big, pp, u32(Tokens::TX1), price);
        const float pw = Fonts::heading()->CalcTextSizeA(big, FLT_MAX, 0.0f, price).x;
        dl->AddText(Fonts::ui(), 15.0f, ImVec2(pp.x + pw + 7.0f, pp.y + big - 15.0f * 1.17f - 6.0f), u32(Tokens::TX2), "/ month");
        ImGui::Dummy(ImVec2(width - pad * 2.0f, big + 2.0f));
        ImGui::SetCursorScreenPos(ImVec2(card0.x + pad, ImGui::GetCursorScreenPos().y));
        push_font(Fonts::ui(), 13.5f);
        ImGui::TextColored(Tokens::TX3, "%s", yearly_billing_ ? "Billed $240 yearly \xc2\xb7 founder rate"
                                                             : "Billed monthly \xc2\xb7 founder rate");
        ImGui::PopFont();
        const float card_y1 = ImGui::GetCursorScreenPos().y + pad - 4.0f;
        dl->ChannelsSetCurrent(0);
        dl->AddRectFilled(card0, ImVec2(card0.x + width, card_y1), u32(Tokens::CARD), Radius::R3);
        dl->AddRect(card0, ImVec2(card0.x + width, card_y1), u32(Tokens::BD1), Radius::R3, 0, 1.0f);
        dl->ChannelsMerge();
        ImGui::SetCursorScreenPos(ImVec2(card0.x, card_y1));
        ImGui::Dummy(ImVec2(width, 16.0f));
    }

    // The one filled control: the marketing site's mint. It says what it buys and for how much, at the interval
    // the switch shows: the coin they hit the lock on when there is one, then the per-month price ("Unlock OGN ·
    // $20/mo, billed yearly"); it goes straight to card checkout in a new tab.
    char cta[96];
    if (login_variant_) {
        snprintf(cta, sizeof(cta), "Log in###upsell_cta");
    } else {
        const char* per_month = yearly_billing_ ? kYearlyPerMonth : kMonthlyPerMonth;
        const char* cadence = yearly_billing_ ? "/mo, billed yearly" : "/mo";
        if (!subject_.empty()) {
            snprintf(cta, sizeof(cta), "Unlock %s \xc2\xb7 %s%s###upsell_cta", subject_.c_str(), per_month, cadence);
        } else {
            snprintf(cta, sizeof(cta), "Get Pro \xc2\xb7 %s%s###upsell_cta", per_month, cadence);
        }
    }
    push_font(Fonts::ui_semibold(), 15.0f);
    const bool cta_clicked = button(cta, ButtonKind::Upgrade, ImVec2(width, 46.0f),
                                    login_variant_ ? drawing::UiIcon(255) : drawing::UiIcon::ArrowUpRight, true);
    ImGui::PopFont();
    if (cta_clicked) {
        if (login_variant_) {
            emit_upsell_usage("login_click", trigger_, true, layer_);
            Entitlements::open_login();
        } else {
            const char* billing = yearly_billing_ ? "yearly" : "monthly";
            nlohmann::json props = {{"plan", "pro"}, {"billing", billing}, {"rail", "card"},
                                    {"destination", "checkout"}};
            if (!subject_.empty()) props["market"] = subject_;
            emit_upsell_usage("upgrade_click", trigger_, false, layer_, props);
            Entitlements::open_checkout(billing);
        }
        ImGui::CloseCurrentPopup();
        open_ = false;
        stop_preview();
    }
    ImGui::Dummy(ImVec2(0, 6));

    // Quiet secondary row: try the free BTC version where one exists, or leave.
    const bool btc = !login_variant_ && !Entitlements::is_pro() && offers_btc(trigger_, layer_);
    const float half = (width - 8.0f) * 0.5f;
    push_font(Fonts::ui(), 14.5f);
    if (btc) {
        if (button("Try BTC free", ButtonKind::Soft, ImVec2(half, 38.0f))) {
            emit_upsell_usage("btc_preview_click", trigger_, false, layer_, {{"market", "binancef:btcusdt"}});
#ifdef __EMSCRIPTEN__
            // Full navigation: the WASM canvas is a singleton. Browser Back
            // returns to the original market; no popup permission is needed.
            if (trigger_ == Trigger::Layer) {
                EM_ASM({ window.location.href = '/terminal/binancef/btcusdt?realtime=1'; });
            } else {
                // The last half hour of BTCUSDT, ending a minute back for ingestion.
                EM_ASM({
                    var n = Date.now();
                    window.location.href = '/terminal?replay=btcusdt&exchange=binancef&from=' +
                        (n - 1800000) + '&to=' + (n - 60000);
                });
            }
#endif
            dismiss(nullptr);   // btc_preview_click above is the record
        }
        ImGui::SameLine(0, 8.0f);
    }
    if (button(login_variant_ ? "Not now" : "Maybe later", ButtonKind::Ghost, ImVec2(btc ? half : width, 38.0f))) dismiss("later");
    ImGui::PopFont();

    if (!login_variant_) {
        // Footer: where the action goes, and the plan page one quiet click away for anyone comparing tiers.
        ImGui::Dummy(ImVec2(0, 8));
        push_font(Fonts::ui(), 12.5f);
        ImGui::PushTextWrapPos(x0 + width);
        ImGui::TextColored(Tokens::TX3, "Card checkout opens in a new tab. Reload the terminal after upgrading.");
        ImGui::PopTextWrapPos();
        const char* link = "Compare plans and Bitcoin payment";
        const ImVec2 lp = ImGui::GetCursorScreenPos();
        const float lw = ImGui::CalcTextSize(link).x, lh = ImGui::GetTextLineHeight();
        const bool link_clicked = ImGui::InvisibleButton("##upsell_plans", ImVec2(lw + 16.0f, lh + 4.0f));
        const bool hov = ImGui::IsItemHovered();
        const ImU32 ink = u32(hov ? Tokens::TX1 : Tokens::TX2);
        dl->AddText(ImVec2(lp.x, lp.y + 2.0f), ink, link);
        if (hov) dl->AddLine(ImVec2(lp.x, lp.y + lh + 2.0f), ImVec2(lp.x + lw, lp.y + lh + 2.0f), ink, 1.0f);
        drawing::draw_ui_icon(dl, drawing::UiIcon::ArrowUpRight, ImVec2(lp.x + lw + 8.0f, lp.y + 2.0f + lh * 0.5f), 3.5f, ink, 1.3f);
        ImGui::PopFont();
        if (link_clicked) {
            const char* billing = yearly_billing_ ? "yearly" : "monthly";
            emit_upsell_usage("upgrade_click", trigger_, false, layer_,
                              {{"plan", "unselected"}, {"billing", billing}, {"rail", "pricing"},
                               {"destination", "plans"}});
            Entitlements::open_pricing(billing);
            ImGui::CloseCurrentPopup();
            open_ = false;
            stop_preview();
        }
    }
}

}  // namespace ui
