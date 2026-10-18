// ═══════════════════════════════════════════════════════════════════════════════
// research_moment_panel.cpp - see research_moment_panel.h. The floating
// "Investigate this minute" reader over the chart.
//
// Copy rules (the web repo's Tier-1 rules apply to terminal strings too):
// no em dashes, no ALL-CAPS chunk over 6 words, and NO rates or counts of
// what followed - counting lives in Research, where denominators are pinned.
// ═══════════════════════════════════════════════════════════════════════════════

#include "research_moment_panel.h"

#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include "../core/display_time_zone.h"

#include <nlohmann/json.hpp>

#include "../core/research_url.h"
#include "../core/usage_emit.h"
#include "../education/transport_emit.h"
#include "../rendering/theme.h"
#include "drawing/drawing_icons.h"
#include "upsell_modal.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace ui {

namespace {

// ── Frozen panel copy (greppable, one block) ─────────────────────────────────
constexpr const char* kTag = "INVESTIGATE";
constexpr const char* kLoading = "Reading this minute from the record";
constexpr const char* kZeroStandouts =
    "Nothing stood out at this minute. All %d readings sat inside their usual range.";
constexpr const char* kLead = "%d of %d readings stood out at this minute.";
constexpr const char* kFiredOne = "1 rulebook condition was firing at this minute.";
constexpr const char* kFiredMany = "%d rulebook conditions were firing at this minute.";
// Round-2 tightening (2026-08-24, James: "hard to read, verbose"): one
// claim, one pointer. The Pro/Research look-back detail moved out of the
// body; the paywall states it where it gates. 2026-10-13: the claim sits
// under the lead it qualifies, the pointer above the button it describes.
constexpr const char* kHonesty = "They describe this minute, not what followed.";
constexpr const char* kSearchFromMany =
    "Uses the %d marked readings to find similar minutes and what followed. "
    "Nothing runs until you confirm.";
constexpr const char* kSearchFromOne =
    "Uses the marked reading to find similar minutes and what followed. "
    "Nothing runs until you confirm.";
constexpr const char* kSearchFromNone =
    "Finds similar minutes and what followed. Nothing runs until you confirm.";
constexpr const char* kMarkHint = "The search starts from this reading.";
// Group headers replace the per-row NEW/STANDING tags and their legend.
constexpr const char* kGroupNew = "NEW SINCE %s";
constexpr const char* kGroupStanding = "ALREADY STANDING OUT AT %s";
constexpr const char* kGroupNewHint = "Not standing out one hour earlier, at %s.";
constexpr const char* kGroupStandingHint = "Already standing out one hour earlier, at %s.";
constexpr const char* kLiveNote =
    "Live read: the newest closed minute, not the minute you pointed at.";
constexpr const char* kProvisionalNote = "Some readings cover a candle that is still forming.";
constexpr const char* kMsgEdgeFallback =
    "The record has not reached this minute yet. It rebuilds nightly and runs about 27 to 40 "
    "hours behind live.";
constexpr const char* kMsgNoBridge =
    "This read needs the EdgeDepth page around the terminal. Open Research directly instead.";
constexpr const char* kMsgTimeout = "The page did not answer. Open Research directly instead.";
constexpr const char* kMsgBadResult = "The read came back malformed. Open Research directly instead.";
constexpr const char* kCtaSearch = "Find moments like this";
constexpr const char* kCtaLive = "Get a live read";
constexpr const char* kCtaOpen = "Open in Research";
constexpr const char* kCtaRetry = "Retry";
constexpr const char* kCtaClose = "Close";

constexpr float kPanelW = 400.0f;
constexpr float kMaxHeightShare = 0.85f;  // of the viewport
constexpr double kTimeoutS = 12.0;

void emit_moment_usage(const char* event, const std::string& symbol, int64_t minute_ms,
                       bool live, const nlohmann::json& props = nlohmann::json::object()) {
    nlohmann::json merged = props;
    merged["entry"] = "terminal";
    merged["live"] = live;
    merged["minute_ms"] = minute_ms;
    usage::dispatch_detail(nlohmann::json{
        {"event", event},
        {"mode", "terminal"},
        {"symbol", symbol},
        {"props", std::move(merged)},
    }.dump());
}

bool bridge_present() {
#ifdef __EMSCRIPTEN__
    return EM_ASM_INT({ return window.__EDGEDEPTH_RESEARCH_BRIDGE__ === 1 ? 1 : 0; }) == 1;
#else
    return false;
#endif
}

// Parse the UTC wire identity once; format through the shared display zone.
int64_t iso_epoch(const std::string& iso) {
    std::tm value{};int year=0,month=0;
    if(std::sscanf(iso.c_str(),"%d-%d-%dT%d:%d:%d",&year,&month,&value.tm_mday,&value.tm_hour,&value.tm_min,&value.tm_sec)!=6)return 0;
    value.tm_year=year-1900;value.tm_mon=month-1;
    return static_cast<int64_t>(timegm(&value))*1000;
}
std::string minute_header(int64_t epoch) {
    char text[128]{};DisplayTimeZone::instance().format(epoch,TimeZoneFormat::FullInspection,text,sizeof(text));return text;
}
// "2026-09-02 13:00 UTC+00:00": the panel reads whole minutes, so no seconds.
std::string minute_title(int64_t epoch) {
    char text[96]{}, offset[24]{};
    DisplayTimeZone::instance().format(epoch, TimeZoneFormat::DateTimeMinutes, text, sizeof(text));
    if (DisplayTimeZone::instance().offset_label(epoch, offset, sizeof(offset)))
        return std::string(text) + " " + offset;
    return text;
}
std::string minute_clock(int64_t epoch) {
    char text[32]{};DisplayTimeZone::instance().format(epoch,TimeZoneFormat::TimeMinutes,text,sizeof(text));return text;
}

bool primary_button(const char* label, float w, float h = 34.0f) {
    using namespace Theme;
    ImGui::PushStyleColor(ImGuiCol_Button, Tokens::BRAND);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tokens::BRAND_TX);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tokens::BRAND);
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::BRAND_INK);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::R2);
    ImGui::PushFont(Fonts::ui_semibold());
    const bool hit = ImGui::Button(label, ImVec2(w, h));
    ImGui::PopFont();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return hit;
}

bool ghost_button(const char* label, float w) {
    using namespace Theme;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Tokens::HOVER);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tokens::ACTIVE);
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
    const bool hit = ImGui::Button(label, ImVec2(w, 26.0f));
    ImGui::PopStyleColor(4);
    return hit;
}

}  // namespace

ResearchMomentPanel& ResearchMomentPanel::instance() {
    static ResearchMomentPanel inst;
    return inst;
}

void ResearchMomentPanel::open(const std::string& symbol_raw, int64_t minute_ms) {
    symbol_raw_ = symbol_raw;
    symbol_norm_ = research_url::normalize_symbol(symbol_raw);
    symbol_upper_ = symbol_norm_;
    std::transform(symbol_upper_.begin(), symbol_upper_.end(), symbol_upper_.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    minute_ms_ = research_url::floor_minute_ms(minute_ms);
    anchor_pos_ = ImGui::GetMousePos();
    open_ = true;
    live_read_ = false;
    standouts_.clear();
    total_readings_ = 0;
    fired_rules_ = 0;
    any_provisional_ = false;
    tags_note_.clear();
    mfields_.clear();
    marked_count_ = 0;
    bucket_label_.clear();
    message_.clear();
    bridge_available_ = bridge_present();
    emit_moment_usage("research_moment_opened", symbol_norm_, minute_ms_, false,
                      {{"bridge_available", bridge_available_}});
    if (!bridge_available_) {
        state_ = State::Error;
        message_ = kMsgNoBridge;
        return;
    }
    send_request(false);
}

void ResearchMomentPanel::send_request(bool live) {
    ++req_seq_;
    last_request_live_ = live;
    state_ = State::Loading;
    request_wall_ = ImGui::GetTime();
    nlohmann::json req;
    req["reqId"] = req_seq_;
    req["symbol"] = symbol_norm_;
    req["iso"] = research_url::iso_utc(minute_ms_);
    req["live"] = live;
    emit_moment_usage("research_moment_read", symbol_norm_, minute_ms_, live,
                      {{"request_id", req_seq_}});
    edu::transport::dispatch_state("edgedepth:research-moment",
                                   "__EDGEDEPTH_RESEARCH_MOMENT_REQ__", req.dump());
}

void ResearchMomentPanel::consume_pending_result() {
#ifdef __EMSCRIPTEN__
    // Studio set_source pattern in reverse: the bridge staged a JSON string
    // in a window global and poked the void export; copy it in-glue (where
    // lengthBytesUTF8/stringToUTF8/_malloc are always available) and clear
    // the global so a stale result can never be re-read.
    char* raw = reinterpret_cast<char*>(EM_ASM_PTR({
        try {
            var s = window.__EDGEDEPTH_RESEARCH_MOMENT_RESULT__;
            if (!s) return 0;
            delete window.__EDGEDEPTH_RESEARCH_MOMENT_RESULT__;
            s = String(s);
            var len = lengthBytesUTF8(s);
            var buf = _malloc(len + 1);
            stringToUTF8(s, buf, len + 1);
            return buf;
        } catch (e) {
            console.warn('research_moment_result: failed reading result global:', e);
            return 0;
        }
    }));
    if (raw) {
        apply_result_json(raw);
        free(raw);
    }
#endif
}

void ResearchMomentPanel::apply_result_json(const char* json) {
    if (!open_) return;
    // No-throw parse: exceptions are disabled in the WASM build, so a throw
    // would abort. A discarded parse renders the error state instead.
    const auto j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        state_ = State::Error;
        message_ = kMsgBadResult;
        return;
    }
    // Only the NEWEST request may answer; a slow earlier fetch is stale.
    if (!j.contains("reqId") || !j["reqId"].is_number_integer() ||
        j["reqId"].get<int>() != req_seq_) {
        return;
    }
    const std::string state = j.value("state", "");
    const std::string message = j.value("message", "");

    // Auth outcomes funnel into the ONE conversion surface (§8.3) and the
    // panel leaves the screen: rendering a second paywall would fork it.
    if (state == "auth") {
        emit_moment_usage("research_moment_result", symbol_norm_, minute_ms_, last_request_live_,
                          {{"state", "auth"}});
        close();
        UpsellModal::instance().open_login(message.empty() ? nullptr : message.c_str());
        return;
    }
    if (state == "pro") {
        emit_moment_usage("research_moment_result", symbol_norm_, minute_ms_, last_request_live_,
                          {{"state", "pro"}});
        close();
        UpsellModal::instance().open(UpsellModal::Trigger::Research,
                                     message.empty() ? nullptr : message.c_str());
        return;
    }
    if (state == "edge") {
        state_ = State::Edge;
        message_ = message.empty() ? kMsgEdgeFallback : message;
        emit_moment_usage("research_moment_result", symbol_norm_, minute_ms_, last_request_live_,
                          {{"state", "edge"}});
        return;
    }
    if (state != "ok" && state != "ok_live") {
        state_ = State::Error;
        message_ = message.empty() ? kMsgBadResult : message;
        emit_moment_usage("research_moment_result", symbol_norm_, minute_ms_, last_request_live_,
                          {{"state", "error"}});
        return;
    }

    live_read_ = (state == "ok_live");
    bucket_label_ = j.value("bucketTime", "");
    total_readings_ = j.value("totalReadings", 0);
    fired_rules_ = j.value("firedRules", 0);
    standouts_.clear();
    any_provisional_ = false;
    tags_note_ = j.value("tagsNote", "");
    if (j.contains("standouts") && j["standouts"].is_array()) {
        for (const auto& r : j["standouts"]) {
            if (!r.is_object()) continue;
            Row row;
            row.name = r.value("name", "");
            row.id = r.value("id", "");
            row.value = r.value("value", "");
            row.detail = r.value("detail", "");
            row.tag = r.value("tag", "");
            row.is_new = r.value("isNew", false);
            row.provisional = r.value("provisional", false);
            any_provisional_ = any_provisional_ || row.provisional;
            if (!row.name.empty()) standouts_.push_back(std::move(row));
        }
    }
    std::vector<std::string> checked;
    if (j.contains("checked") && j["checked"].is_array()) {
        for (const auto& c : j["checked"])
            if (c.is_string()) checked.push_back(c.get<std::string>());
    }
    for (Row& row : standouts_)
        row.marked = std::find(checked.begin(), checked.end(), row.id) != checked.end();
    marked_count_ = static_cast<int>(checked.size());
    mfields_ = research_url::join_mfields(checked);
    state_ = live_read_ ? State::ReadyLive : State::Ready;
    emit_moment_usage("research_moment_result", symbol_norm_, minute_ms_, live_read_,
                      {{"state", live_read_ ? "ok_live" : "ok"},
                       {"standouts", static_cast<int>(standouts_.size())},
                       {"readings", total_readings_},
                       {"checked", static_cast<int>(checked.size())},
                       {"fired_rules", fired_rules_}});
}

void ResearchMomentPanel::open_handoff(bool live) const {
    const std::string url = live ? research_url::moment_live_url(symbol_norm_, mfields_)
                                 : research_url::moment_url(symbol_norm_, minute_ms_, mfields_);
    emit_moment_usage("research_moment_handoff", symbol_norm_, minute_ms_, live,
                      {{"checked", static_cast<int>(
                           std::count(mfields_.begin(), mfields_.end(), ',') +
                           (mfields_.empty() ? 0 : 1))}});
#ifdef __EMSCRIPTEN__
    // A new tab, never a navigation: the terminal holds live WS state and
    // possibly a replay session, and navigating away throws both away.
    EM_ASM({ window.open(UTF8ToString($0), '_blank', 'noopener'); }, url.c_str());
#else
    (void)url;
#endif
}

void ResearchMomentPanel::close() {
    open_ = false;
    state_ = State::Loading;
}

bool ResearchMomentPanel::blocks_replay_escape() const {
    return open_ || esc_frame_ == ImGui::GetFrameCount();
}

void ResearchMomentPanel::render() {
    if (!open_) return;
    using namespace Theme;

    if (state_ == State::Loading && ImGui::GetTime() - request_wall_ > kTimeoutS) {
        state_ = State::Error;
        message_ = kMsgTimeout;
    }

    // Near the click, clamped so a right-edge or bottom-edge click never
    // pushes the panel off-screen.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 pos = anchor_pos_;
    pos.x = std::min(pos.x, vp->Pos.x + vp->Size.x - kPanelW - 16.0f);
    pos.x = std::max(pos.x, vp->Pos.x + 8.0f);
    pos.y = std::min(pos.y, vp->Pos.y + vp->Size.y * 0.45f);
    pos.y = std::max(pos.y, vp->Pos.y + 8.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(kPanelW, 0.0f),
                                        ImVec2(kPanelW, vp->Size.y * kMaxHeightShare));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, Tokens::PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border, Tokens::BD2);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 16.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Radius::R3);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);

    bool keep = true;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##edx_research_moment", &keep, flags)) {
        // AlwaysAutoResize height is only known AFTER Begin: a tall result
        // (tags + direction lines) anchored at the click can escape the
        // viewport bottom and hide the CTAs. Clamp by shifting up; the
        // correction lands on the frame after the size settles.
        {
            const ImVec2 wp = ImGui::GetWindowPos();
            const ImVec2 ws = ImGui::GetWindowSize();
            const float bottom_limit = vp->Pos.y + vp->Size.y - 12.0f;
            if (wp.y + ws.y > bottom_limit) {
                const float ny = std::max(vp->Pos.y + 8.0f, bottom_limit - ws.y);
                ImGui::SetWindowPos(ImVec2(wp.x, ny));
            }
        }
        // Floating surfaces get the soft shadow (same treatment as ChartCtx).
        {
            ImDrawList* d = ImGui::GetWindowDrawList();
            const ImVec2 wa = ImGui::GetWindowPos();
            const ImVec2 wb(wa.x + ImGui::GetWindowSize().x, wa.y + ImGui::GetWindowSize().y);
            d->PushClipRectFullScreen();
            for (int i = 5; i >= 1; --i) {
                const float e = static_cast<float>(i) * 2.0f;
                d->AddRect(ImVec2(wa.x - e, wa.y - e + 3.0f), ImVec2(wb.x + e, wb.y + e + 3.0f),
                           IM_COL32(0, 0, 0, 14), Radius::R3 + e, 0, 1.6f);
            }
            d->PopClipRect();
        }
        render_body();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    // Esc closes the panel wherever focus sits; blocks_replay_escape() keeps
    // the same press from also stopping an active replay.
    if (open_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        esc_frame_ = ImGui::GetFrameCount();
        keep = false;
    }
    if (!keep) close();
}

void ResearchMomentPanel::render_body() {
    using namespace Theme;
    const float w = ImGui::GetContentRegionAvail().x;

    // ── Header: tag + close, then symbol + minute ────────────────────────
    const ImVec2 head = ImGui::GetCursorScreenPos();
    ImGui::PushFont(Fonts::label());
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX3);
    ImGui::TextUnformatted(kTag);
    ImGui::PopStyleColor();
    const float tag_h = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    const ImVec2 after_tag = ImGui::GetCursorScreenPos();
    constexpr float kCloseSz = 22.0f;
    ImGui::SetCursorScreenPos(ImVec2(head.x + w - kCloseSz, head.y + (tag_h - kCloseSz) * 0.5f));
    if (Theme::icon_button("##moment_close", drawing::UiIcon::Close, "Close (Esc)", kCloseSz)) {
        close();
        return;
    }
    ImGui::SetCursorScreenPos(ImVec2(after_tag.x, after_tag.y + 2.0f));

    ImGui::PushFont(Fonts::ui_semibold());
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX1);
    ImGui::TextUnformatted(symbol_upper_.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::PushFont(Fonts::mono_sm());
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
    const int64_t at_ms = bucket_label_.empty() ? minute_ms_ : iso_epoch(bucket_label_);
    ImGui::TextWrapped("%s", minute_title(at_ms).c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    Theme::hairline(w, 16.0f);

    // ── Body per state ───────────────────────────────────────────────────
    if (state_ == State::Loading) {
        const int dots = static_cast<int>(ImGui::GetTime() * 2.0) % 4;
        char line[96];
        snprintf(line, sizeof(line), "%s%.*s", kLoading, dots, "...");
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
        ImGui::TextUnformatted(line);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        return;
    }

    if (state_ == State::Edge || state_ == State::Error) {
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(message_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, 12.0f));
        if (state_ == State::Edge) {
            if (primary_button(kCtaLive, w)) send_request(true);
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            if (ghost_button(kCtaOpen, w)) open_handoff(false);
        } else if (bridge_available_) {
            if (primary_button(kCtaRetry, w)) send_request(last_request_live_);
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            if (ghost_button(kCtaOpen, w)) open_handoff(false);
        } else {
            if (primary_button(kCtaOpen, w)) open_handoff(false);
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            if (ghost_button(kCtaClose, w)) close();
        }
        return;
    }

    // Ready / ReadyLive.
    if (state_ == State::ReadyLive) {
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::WARN);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(kLiveNote);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
    }

    // ── The answer, and the one claim that qualifies it ──────────────────
    char lead[128];
    if (standouts_.empty()) {
        snprintf(lead, sizeof(lead), kZeroStandouts, total_readings_);
    } else {
        snprintf(lead, sizeof(lead), kLead, static_cast<int>(standouts_.size()), total_readings_);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX1);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(lead);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    if (!standouts_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX3);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(kHonesty);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    // ── Readings ─────────────────────────────────────────────────────────
    // Reader name over the registry id, value right-aligned; names, values,
    // directions and ids are bridge strings rendered verbatim, in the
    // bridge's rarest-first order. With an honest -1h baseline the rows are
    // grouped by the bridge's isNew flag (order kept within each group): what
    // is new this hour leads, what was already standing out (the afternoon's
    // weather, not this minute's event) follows under its own header. The
    // headers replace per-row NEW/STANDING tags and their legend.
    //
    // Columns: a slot marking the readings the search starts from (the
    // bridge's checked set, only when there is one), the name wrapping in
    // the space left of the value, and the value sized to the widest (capped
    // at 40% of the list; a wider value drops under its name).
    //
    // The list scrolls in its own region when the panel would outgrow the
    // viewport, so the action under it never falls out of view.
    constexpr float kColGap = 10.0f;
    const float mark_w = marked_count_ > 0 ? 18.0f : 0.0f;
    const float above = ImGui::GetCursorScreenPos().y - ImGui::GetWindowPos().y;
    const float list_max = std::max(120.0f, ImGui::GetMainViewport()->Size.y * kMaxHeightShare - above -
                                                footer_h_ - ImGui::GetStyle().WindowPadding.y);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, list_max));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild("##moment_rows", ImVec2(w, 0.0f), ImGuiChildFlags_AutoResizeY);
    const float lw = ImGui::GetContentRegionAvail().x;
    float value_col = 0.0f;
    ImGui::PushFont(Fonts::mono());
    for (const Row& row : standouts_)
        value_col = std::max(value_col, ImGui::CalcTextSize(row.value.c_str()).x);
    ImGui::PopFont();
    value_col = std::min(value_col, lw * 0.4f);
    const float line_h = ImGui::GetTextLineHeight();

    auto draw_row = [&](const Row& row) {
        ImGui::PushFont(Fonts::mono());
        const ImVec2 vsz = ImGui::CalcTextSize(row.value.c_str());
        ImGui::PopFont();
        const bool beside = vsz.x <= value_col;
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float x0 = ImGui::GetCursorPosX();
        if (row.marked) {
            drawing::draw_ui_icon(ImGui::GetWindowDrawList(), drawing::UiIcon::Search,
                                  ImVec2(start.x + 6.0f, start.y + line_h * 0.5f), 5.0f,
                                  ImGui::GetColorU32(Tokens::TX2), 1.4f);
            if (ImGui::IsWindowHovered() &&
                ImGui::IsMouseHoveringRect(start, ImVec2(start.x + mark_w, start.y + line_h)))
                Theme::tooltip("%s", kMarkHint);
        }
        ImGui::SetCursorPosX(x0 + mark_w);
        ImGui::PushTextWrapPos(x0 + (beside ? lw - value_col - kColGap : lw));
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX1);
        ImGui::TextUnformatted(row.name.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        const float name_bottom = ImGui::GetCursorScreenPos().y;

        ImGui::PushFont(Fonts::mono());
        ImGui::SetCursorScreenPos(ImVec2(start.x + lw - vsz.x,
                                         (beside ? start.y : name_bottom) + (line_h - vsz.y) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX1);
        ImGui::TextUnformatted(row.value.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(start.x, std::max(name_bottom, ImGui::GetCursorScreenPos().y)));

        if (!row.detail.empty()) {
            ImGui::SetCursorPosX(x0 + mark_w);
            ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
            ImGui::PushTextWrapPos(x0 + lw);
            ImGui::TextUnformatted(row.detail.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        ImGui::SetCursorPosX(x0 + mark_w);
        ImGui::PushFont(Fonts::mono_xs());
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX3);
        if (row.provisional) {
            char idbuf[96];
            snprintf(idbuf, sizeof(idbuf), "%s (forming)", row.id.c_str());
            ImGui::TextUnformatted(idbuf);
        } else {
            ImGui::TextUnformatted(row.id.c_str());
        }
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    };

    const int64_t base_ms = at_ms - 3600000;
    auto draw_group = [&](const char* title_fmt, const char* hint_fmt, bool accent, int kind) {
        int count = 0;
        for (const Row& row : standouts_) {
            const int k = row.tag.empty() ? 2 : row.is_new ? 0 : 1;
            if (k == kind) ++count;
        }
        if (count == 0) return;
        if (title_fmt) {
            char title[64], hint[128];
            snprintf(title, sizeof(title), title_fmt, minute_clock(base_ms).c_str());
            snprintf(hint, sizeof(hint), hint_fmt, minute_header(base_ms).c_str());
            ImGui::PushFont(Fonts::label());
            ImGui::PushStyleColor(ImGuiCol_Text, accent ? Tokens::BRAND : Tokens::TX3);
            ImGui::TextUnformatted(title);
            ImGui::PopStyleColor();
            ImGui::PopFont();
            if (ImGui::IsItemHovered()) Theme::tooltip("%s", hint);
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
        }
        for (const Row& row : standouts_) {
            const int k = row.tag.empty() ? 2 : row.is_new ? 0 : 1;
            if (k == kind) draw_row(row);
        }
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
    };
    draw_group(kGroupNew, kGroupNewHint, true, 0);
    draw_group(kGroupStanding, kGroupStandingHint, false, 1);
    draw_group(nullptr, nullptr, false, 2);  // untagged: no honest baseline
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ── Footnotes ────────────────────────────────────────────────────────
    const float footer_top = ImGui::GetCursorScreenPos().y;
    if (fired_rules_ > 0) {
        char fired[96];
        if (fired_rules_ == 1) {
            snprintf(fired, sizeof(fired), "%s", kFiredOne);
        } else {
            snprintf(fired, sizeof(fired), kFiredMany, fired_rules_);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(fired);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    }

    if (any_provisional_) {
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX3);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(kProvisionalNote);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    }

    // ── The one action, with what it does directly above it ──────────────
    Theme::hairline(w, 14.0f);
    char scope[192];
    if (marked_count_ > 1) snprintf(scope, sizeof(scope), kSearchFromMany, marked_count_);
    else snprintf(scope, sizeof(scope), "%s", marked_count_ == 1 ? kSearchFromOne : kSearchFromNone);
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX3);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(scope);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    // The flagship CTA (2026-08-24): the tallest button in the terminal on
    // purpose - "find similar" is the whole journey, so it reads as the
    // panel's one obvious next step. Close is the header x and Esc.
    const bool search = primary_button(kCtaSearch, w, 44.0f);
    footer_h_ = ImGui::GetCursorScreenPos().y - footer_top;
    if (search) open_handoff(live_read_);
}

}  // namespace ui

// ═══════════════════════════════════════════════════════════════════════════════
// KEEPALIVE result callback - the bridge calls Module.__research_moment_result()
// after staging window.__EDGEDEPTH_RESEARCH_MOMENT_RESULT__. Mirrors the
// _studio_cmd_set_source convention: defined WITH a leading underscore,
// exported as __research_moment_result in BOTH CMake EXPORTED_FUNCTIONS lists.
// ═══════════════════════════════════════════════════════════════════════════════

#ifdef __EMSCRIPTEN__
extern "C" {

EMSCRIPTEN_KEEPALIVE void _research_moment_result(void) {
    ui::ResearchMomentPanel::instance().consume_pending_result();
}

}  // extern "C"
#endif
