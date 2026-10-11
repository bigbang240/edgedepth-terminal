#include "data_widget.h"
#include "core/terminal_request.h"
#include "core/market_data_overview.h"
#include "ui/upsell_modal.h"
#include "core/stat_freshness.h"
#include "core/entitlements.h"
#include "core/display_time_zone.h"
#include "core/symbol_metadata.h"
#include "core/logo_manager.h"
#include "core/research_url.h"
#include "rendering/theme.h"
#include "rendering/menu.h"
#include "ui/drawing/drawing_icons.h"
#include "ui/open_url.h"
#include "core/ui_test_marks.h"
#include "implot.h"
#include "imgui_internal.h"
#include <cstdlib>
#include <cstdio>

// Data tab, design v3 (James 2026-10-10: "a solid repolishing / redesign to look sleek and clean"; no square
// buttons). A page header with the view switch, then cards: market context, six chart cards with round icon
// controls, a pin bar, research observation cards and an inspector card. Readings are grouped cards with a
// search field. Content sits in rounded cards on the panel; controls are rounded (Theme::button, segmented,
// icon_button). All data, access, pin and keyboard behaviour is unchanged.
using namespace Theme;
namespace {
using ContextState = market_data_view::ContextState;
using drawing::UiIcon;
constexpr const char* context_names[]{"Activity / structure", "Last hour", "Forced-flow detector", "Cascade tier", "OI ahead of price", "Volume awakening"};
constexpr const char* qualifiers[]{"Descriptive", "1h readings", "15m detector", "Preview / rank, not odds", "Research preview", "Activity only"};
struct ChartInfo { const char* title; const char* detail; const char* help; };
constexpr ChartInfo charts[]{
    {"Price", "5m close", "Completed 5m candle closes."},
    {"Open interest", "Contracts", "Open contracts. Quote value = contracts x matching 5m close."},
    {"OI / market cap", "Price (left) / ratio (right)", "Binance contracts / CMC circulating supply.\nCan exceed 100%; axes are independent.\nNot leverage or liquidation risk."},
    {"Long / short accounts", "Long share", "Share of accounts that are long, not position size."},
    {"Top-trader accounts", "Long share / top 20% by margin", "Long share among Binance's top 20% by margin balance. Accounts, not position size."},
    {"Taker buy / sell", "5m volume / sells below zero", "5m aggressive volume in base units, ending at the shown time. Sells plot below zero."}
};
void stamp(int64_t time, char* out, size_t size, TimeZoneFormat format = TimeZoneFormat::TimeMinutes) {
    DisplayTimeZone::instance().format(time, format, out, size);
}
void metric(double value, bool percent, char* out, size_t size) {
    if (!std::isfinite(value)) snprintf(out, size, "No observation");
    else if (percent) snprintf(out, size, "%.2f%%", value);
    else if (std::abs(value) >= 1e9) snprintf(out, size, "%.2fB", value / 1e9);
    else if (std::abs(value) >= 1e6) snprintf(out, size, "%.2fM", value / 1e6);
    else if (std::abs(value) >= 1e3) snprintf(out, size, "%.2fK", value / 1e3);
    else snprintf(out, size, "%.6g", value);
}

// ── Type scale (logical px at the default UI text size; K follows the viewer's text size) ──
float K = 1.0f;
void font(ImFont* f, float px) { Theme::push_font(f, px * K); }
void text(const char* t, const ImVec4& c = Tokens::TX1, ImFont* f = nullptr, float px = 15.5f) {
    font(f ? f : Fonts::ui(), px); ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextWrapped("%s", t);
    ImGui::PopStyleColor(); ImGui::PopFont();
}
void secondary(const char* t) { text(t, Tokens::TX2, Fonts::ui(), 14.5f); }
void meta(const char* t, const ImVec4& c = Tokens::TX3) { text(t, c, Fonts::ui(), 13.5f); }
void heading(const char* t) { text(t, Tokens::TX1, Fonts::ui_semibold(), 17); }
void space(float px) { ImGui::Dummy(ImVec2(0, px * K)); }
void draw_text(ImDrawList* draw, ImFont* font, float size, ImVec2 pos, const ImVec4& color, const char* text, float width) {
    draw->AddText(font, size, pos, ImGui::GetColorU32(color), text, nullptr, width);
}
// Numeric fields own the sign; never infer it from the API's display copy.
// The same wrapping is used in the summary and inspector.
float reading_text(const MarketContext::Item& item, ImFont* font, float size,
                   ImVec2 pos, float width, ImDrawList* draw = nullptr) {
    width = std::max(20.0f, width);
    if (std::none_of(item.metrics.begin(), item.metrics.end(), [](double v) { return std::isfinite(v); })) {
        if (draw) draw_text(draw, font, size, pos, Tokens::TX1, item.value.c_str(), width);
        return font->CalcTextSizeA(size, FLT_MAX, width, item.value.c_str()).y;
    }
    constexpr const char* labels[]{"Price ", "OI ", "RV "};
    float x = 0, y = 0, line_height = size;
    for (size_t i = 0; i < item.metrics.size(); ++i) {
        const double v = item.metrics[i];
        char value[64], part[80];
        // Avoid showing a signed/coloured zero after one-decimal rounding.
        const double displayed = std::round(v * 1000) / 10;
        if (!std::isfinite(v)) snprintf(value, sizeof(value), "--");
        else snprintf(value, sizeof(value), i < 2 && displayed != 0 ? "%+.1f%%" : "%.1f%%", displayed == 0 ? 0.0 : displayed);
        snprintf(part, sizeof(part), "%s%s", labels[i], value);
        const float label_width = font->CalcTextSizeA(size, FLT_MAX, 0, labels[i]).x;
        const float part_width = font->CalcTextSizeA(size, FLT_MAX, 0, part).x;
        if (x > 0 && x + part_width > width) { x = 0; y += line_height + 4; line_height = size; }
        const auto color = i < 2 && std::isfinite(v) && displayed != 0
            ? (displayed > 0 ? Tokens::UP : Tokens::DOWN) : Tokens::TX1;
        if (draw) {
            if (ui_test::enabled()) {
                const float lx = part_width <= width ? pos.x + x : pos.x;
                ui_test::mark_rect(std::string("data.reading_part.") + std::to_string(i), ImVec2(lx, pos.y + y), ImVec2(lx + part_width, pos.y + y + size));
            }
            if (part_width <= width) {
                draw_text(draw, font, size, ImVec2(pos.x + x, pos.y + y), Tokens::TX3, labels[i], label_width + 1);
                draw_text(draw, font, size, ImVec2(pos.x + x + label_width, pos.y + y), color, value, width - x - label_width);
            } else draw_text(draw, font, size, ImVec2(pos.x, pos.y + y), color, part, width);
        }
        line_height = std::max(line_height, font->CalcTextSizeA(size, FLT_MAX, width, part).y);
        x += part_width + size;
    }
    return y + line_height;
}
void reading(const MarketContext::Item& item, float width, float px = 17) {
    font(Fonts::mono(), px);
    const float height = reading_text(item, ImGui::GetFont(), ImGui::GetFontSize(),
        ImGui::GetCursorScreenPos(), width, ImGui::GetWindowDrawList());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(width, height));
}
const ImVec4& tone_of(const std::string& tone) { return tone == "up" ? Tokens::UP : tone == "down" ? Tokens::DOWN : tone == "warn" ? Tokens::WARN : Tokens::TX1; }

// Cards in a grid: each card's content is a group at its padded origin; every card in a row gets one rounded
// surface as tall as the row's tallest, drawn beneath the content (splitter channel 0) once the row is done.
class CardGrid {
public:
    CardGrid(int columns, float gap = 14.0f, ImVec2 pad = ImVec2(20, 18))
        : columns_(std::max(1, columns)), gap_(gap * K), pad_(pad.x * K, pad.y * K) {
        dl_ = ImGui::GetWindowDrawList();
        origin_ = ImGui::GetCursorScreenPos();
        width_ = ImGui::GetContentRegionAvail().x;
        card_w_ = std::floor((width_ - gap_ * (columns_ - 1)) / columns_);
        row_y_ = origin_.y;
    }
    // Starts the next card; returns its inner width.
    float next() {
        close_card();
        if (!row_open_ || col_ + 1 >= columns_) { if (row_open_) finish_row(); start_row(); }
        ++col_;
        const float x0 = origin_.x + col_ * (card_w_ + gap_);
        cards_.push_back(x0);
        ImGui::SetCursorScreenPos(ImVec2(x0 + pad_.x, row_y_ + pad_.y));
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(x0 + card_w_ - pad_.x - ImGui::GetWindowPos().x + ImGui::GetScrollX());
        card_open_ = true;
        return inner();
    }
    float inner() const { return card_w_ - pad_.x * 2; }
    ImVec2 card_min() const { return ImVec2(cards_.back(), row_y_); }
    void end() {
        close_card();
        if (row_open_) finish_row();
        ImGui::SetCursorScreenPos(ImVec2(origin_.x, row_y_ - gap_));
        ImGui::Dummy(ImVec2(width_, 0));
    }
private:
    void close_card() {
        if (!card_open_) return;
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        row_bottom_ = std::max(row_bottom_, ImGui::GetItemRectMax().y + pad_.y);
        card_open_ = false;
    }
    void start_row() {
        split_.Split(dl_, 2);
        split_.SetCurrentChannel(dl_, 1);
        row_open_ = true; col_ = -1; cards_.clear(); row_bottom_ = row_y_;
    }
    void finish_row() {
        split_.SetCurrentChannel(dl_, 0);
        for (const float x0 : cards_) {
            dl_->AddRectFilled(ImVec2(x0, row_y_), ImVec2(x0 + card_w_, row_bottom_), ImGui::GetColorU32(Tokens::CARD), Radius::R3);
            dl_->AddRect(ImVec2(x0, row_y_), ImVec2(x0 + card_w_, row_bottom_), ImGui::GetColorU32(Tokens::BD1), Radius::R3);
        }
        split_.Merge(dl_);
        row_y_ = row_bottom_ + gap_;
        row_open_ = false;
    }
    int columns_, col_ = -1;
    float gap_, width_, card_w_, row_y_, row_bottom_ = 0;
    ImVec2 pad_, origin_;
    ImDrawList* dl_;
    ImDrawListSplitter split_;
    std::vector<float> cards_;
    bool row_open_ = false, card_open_ = false;
};

// A section title (and optional caption) on the left; right-aligned controls follow at right_x().
void section(const char* title, const char* caption = nullptr) {
    font(Fonts::ui_semibold(), 18); ImGui::TextUnformatted(title); ImGui::PopFont();
    if (caption) { space(3); meta(caption); }
}
// Moves the cursor to the top of the line just drawn, at the right edge less w (call at the start of a line).
void right_of_line(float line_top, float w) {
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - w, line_top));
}
// A ghost button whose label lines up with the text above it (its hover fill reaches into the padding).
bool link(const char* label, UiIcon icon = UiIcon::ChevronRight, bool on = false) {
    const float h = 28 * K;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - std::round(h * 0.46f));
    return Theme::button(label, on ? ButtonKind::Soft : ButtonKind::Ghost, ImVec2(0, h), icon, true);
}
// A ghost button flush with the right edge of the line (label aligned to the content edge).
bool right_link(float line_top, const char* label, UiIcon icon, bool on = false) {
    const float h = 28 * K, w = Theme::button_width(label, h, static_cast<uint8_t>(icon) != 255);
    right_of_line(line_top, w - std::round(h * 0.46f));
    return Theme::button(label, on ? ButtonKind::Soft : ButtonKind::Ghost, ImVec2(w, h), icon, true);
}
// A card's title row: title on the left, then up to two round icon buttons on the right.
float card_title(const char* title, float width) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    font(Fonts::ui_semibold(), 15.5f);
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x, p.y + 4 * K), ImGui::GetColorU32(Tokens::TX1), title);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(width, 28 * K));
    return p.y;
}
using ui::open_url;
}

DataWidget::DataWidget(const Terminal::Pair& pair, const AppContext& ctx)
    : pair_(pair), ctx_(ctx), title_("     Data " + widget_symbol_label(pair.symbol) + "###Data " + pair.exchange + " " + pair.symbol), request_id_(next_terminal_request_id()), readings_request_id_(next_terminal_request_id()) {
#ifdef __EMSCRIPTEN__
    touch_controls_ = EM_ASM_INT({ return navigator.maxTouchPoints > 0 ? 1 : 0; }) != 0;
#endif
}
DataWidget::~DataWidget() { cancel(); basic_subscription(false); }
void DataWidget::basic_subscription(bool enabled) {
    auto* wanted = enabled ? ctx_.streams : nullptr;
    if (wanted == basic_streams_) return;
    const StreamKey key{pair_, Terminal::Stream::Stats, StatsSnapshot::timeframe_ms};
    if (basic_streams_) basic_streams_->unsubscribe_stats(key, this);
    basic_streams_ = wanted;
    basic_snapshot_.stat = {}; basic_snapshot_.received_ms = 0; basic_snapshot_.bucket_ms = 0;
    if (basic_streams_) basic_streams_->subscribe_stats(key, StreamHandler<Terminal::Stat>{
        .widget_ptr = this,
        .callback = [](void* ptr, const Terminal::Stat& stat) {
            auto* self = static_cast<DataWidget*>(ptr);
            if (!self->ctx_.stream_mgr().is_replay_mode())
                self->basic_snapshot_.apply(stat, static_cast<int64_t>(Entitlements::wall_now_ms()));
        }
    });
}
void DataWidget::cancel() {
#ifdef __EMSCRIPTEN__
    terminal_request_cancel(request_id_); terminal_request_cancel(request_id_ + 1); terminal_request_cancel(readings_request_id_);
#endif
    requested_ = 0; keyboard_plot_ = -1; inspection_.clear_hover();
}
void DataWidget::update() {
    // Preserve local range, pin and inspector while replay suppresses live reads.
    if (ctx_.stream_mgr().is_replay_mode()) { cancel(); basic_subscription(false); }
}
void DataWidget::poll() {
#ifdef __EMSCRIPTEN__
    if (char* response = terminal_request_take(request_id_)) {
        context_ = MarketContext::parse(response, pair_.exchange, pair_.symbol);
        for (size_t i = 0; i < MarketContext::Count; ++i)
            if (context_.items[i].at) last_context_.items[i] = context_.items[i];
        context_loaded_ = true;
        std::free(response);
    }
    if (char* response = terminal_request_take(request_id_ + 1)) {
        locked_ = std::string_view(response) == "locked";
        auto next = MarketDataHistory::parse(response, pair_.exchange, pair_.symbol);
        failed_ = !next.available(); loaded_ = true;
        if (next.available()) { history_ = std::move(next); fit_dirty_ = true; }
        if (locked_) { history_ = {}; context_ = {}; last_context_ = {}; readings_ = {}; inspection_.unpin(); }
        std::free(response);
    }
    if (char* response = terminal_request_take(readings_request_id_)) {
        auto next = ResearchReadings::parse(response, pair_.exchange, pair_.symbol);
        readings_loaded_ = true; readings_failed_ = !next.available();
        if (next.available()) readings_ = std::move(next);
        if (std::string_view(response) == "locked") { locked_ = true; readings_ = {}; history_ = {}; context_ = {}; last_context_ = {}; }
        std::free(response);
    }
    const auto now = static_cast<int64_t>(Entitlements::wall_now_ms());
    if (!requested_ || now - requested_ >= 60000) {
        terminal_request_start(request_id_, pair_.exchange.c_str(), pair_.symbol.c_str(), 0);
        terminal_request_start(request_id_ + 1, pair_.exchange.c_str(), pair_.symbol.c_str(), 3);
        terminal_request_start(readings_request_id_, pair_.exchange.c_str(), pair_.symbol.c_str(), 4);
        requested_ = now;
    }
#endif
}

void DataWidget::render() {
    if (!is_open) { cancel(); basic_subscription(false); return; }
    ImGuiWindowClass window_class;
    window_class.ClassId = ImHashStr("ChartWindow");
    ImGui::SetNextWindowClass(&window_class);
    if (!initial_dock_target.empty()) {
        if (const auto* chart = ImGui::FindWindowByName(initial_dock_target.c_str()); chart && chart->DockId) {
            ImGui::SetNextWindowDockID(chart->DockId, ImGuiCond_Always);
            initial_dock_target.clear(); open_centered = false; ImGui::SetNextWindowFocus();
        }
    }
    ImGui::SetNextWindowSize(ImVec2(960, 760), ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool visible = begin_render(title());
    ImGui::PopStyleVar();
    if (!visible) { cancel(); basic_subscription(false); end_render(); return; }
    ImGui::PushFont(Fonts::ui());
    // Logical text size includes the user's font scale, never the atlas DPI factor.
    unit_ = ImGui::GetFontSize() / 13.0f;
    K = ImGui::GetFontSize() / 18.4f;
    font(Fonts::ui(), 15.5f);  // body text
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * K, 22 * K));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * K, (touch_controls_ ? 11 : 7) * K));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * K, 6 * K));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    const float available = ImGui::GetContentRegionAvail().x;
    const float body_width = std::min(available, 1680 * K);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available - body_width) * .5f);
    ImGui::BeginChild("data_body", ImVec2(body_width, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    auto finish = [&]() { space(24); ImGui::EndChild(); ImGui::PopStyleVar(4); ImGui::PopFont(); ImGui::PopFont(); end_render(); };
    // An empty or blocked state: one card with a title, a line and an optional action.
    auto state_card = [&](const char* title, const char* line, const char* action = nullptr) {
        CardGrid g(1);
        g.next();
        heading(title); space(4); secondary(line);
        bool clicked = false;
        if (action) { space(12); clicked = Theme::button(action, ButtonKind::Primary); }
        g.end();
        return clicked;
    };
    if (ctx_.stream_mgr().is_replay_mode()) {
        cancel(); basic_subscription(false);
        page_header(false);
        state_card("Data paused during Replay", "Return to Live to resume.");
        finish(); return;
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) Menu::sync_market_view(pair_, true, hours_);
    if (!Entitlements::is_pro()) {
        cancel(); history_ = {}; context_ = {}; last_context_ = {}; readings_ = {}; inspection_.unpin();
        locked_ = false; loaded_ = false; readings_loaded_ = false; context_loaded_ = false;
        basic_subscription(true); page_header(false); basic_overview(); pro_preview();
        finish(); return;
    }
    if (pair_.exchange != "binancef") {
        cancel(); basic_subscription(true); page_header(false); basic_overview();
        char line[96]; snprintf(line, sizeof(line), "%s history unavailable.", widget_venue_label(pair_.exchange));
        // Do not suggest that a same-symbol contract exists on a different venue.
        state_card("History: Binance Futures only", line);
        finish(); return;
    }
    poll();
    basic_subscription(locked_);
    if (locked_) {
        page_header(false); basic_overview();
        if (state_card("Sign in to restore Data", "Pro / Research access unconfirmed.", "Sign in again")) Entitlements::open_login();
        finish(); return;
    }
    page_header(true);
    if (readings_view_) { research_readings(); finish(); return; }
    context_row();
    space(28);
    toolbar();
    if (failed_) {
        char line[96] = "No history loaded.";
        if (history_.available()) {
            char clock[64]; stamp(history_.to, clock, sizeof(clock), TimeZoneFormat::DateTimeMinutes);
            snprintf(line, sizeof(line), "Last close: %s", clock);
        }
        if (state_card("History refresh failed", line, "Retry history")) {
#ifdef __EMSCRIPTEN__
            terminal_request_start(request_id_ + 1, pair_.exchange.c_str(), pair_.symbol.c_str(), 3);
#endif
            failed_ = false; loaded_ = false;
        }
    } else { charts(); history_actions(); }
    space(32);
    research();
    finish();
}

// The page header: coin logo, pair, then the venue (its logo) and contract; for full Data, the Overview / Readings
// switch at the right.
void DataWidget::page_header(bool tabs) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    const std::string name = widget_symbol_label(pair_.symbol);
    const auto* meta_info = SymbolRegistry::instance().get(pair_.exchange, pair_.symbol);
    const float logo = 40 * K;
    LogoManager::instance().draw_coin(draw, meta_info && !meta_info->base_asset.empty() ? meta_info->base_asset : name, p, logo);
    const float tx = p.x + logo + 14 * K;
    font(Fonts::ui_semibold(), 24);
    draw->AddText(ImVec2(tx, p.y - 1 * K), ImGui::GetColorU32(Tokens::TX1), name.c_str());
    const float name_w = ImGui::CalcTextSize(name.c_str()).x;
    ImGui::PopFont();
    // Venue line: exchange logo, venue name, contract type.
    font(Fonts::ui(), 14);
    const float vy = p.y + 26 * K, ex = 15 * K;
    float vx = tx;
    if (LogoManager::instance().exchange(pair_.exchange)) {
        LogoManager::instance().draw_exchange(draw, pair_.exchange, ImVec2(vx, vy + 1 * K), ex);
        vx += ex + 6 * K;
    }
    char venue[96];
    snprintf(venue, sizeof(venue), "%s / %s", widget_venue_label(pair_.exchange), meta_info && !meta_info->perpetual ? "Dated" : "Perpetual");
    draw->AddText(ImVec2(vx, vy), ImGui::GetColorU32(Tokens::TX2), venue);
    const float venue_w = vx - tx + ImGui::CalcTextSize(venue).x;
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(logo + 14 * K + std::max(name_w, venue_w), logo));
    const float after = ImGui::GetCursorScreenPos().y;
    if (tabs) {
        static const char* const views[] = {"Overview", "Readings"};
        const float h = 34 * K, w = Theme::segmented_width(views, 2, h);
        const float title_w = logo + 14 * K + std::max(name_w, venue_w) + 24 * K;
        // Beside the title when it fits, else on its own line below it.
        const bool beside = ImGui::GetContentRegionAvail().x >= title_w + w;
        if (beside) right_of_line(p.y + (logo - h) * 0.5f, w);
        else space(12);
        const int v = Theme::segmented("data_view", views, 2, readings_view_ ? 1 : 0, h);
        if (v != (readings_view_ ? 1 : 0)) { readings_view_ = v == 1; ImGui::SetScrollY(0); }
        if (beside) ImGui::SetCursorScreenPos(ImVec2(p.x, std::max(after, p.y + logo)));
    }
    space(24);
}

void DataWidget::toolbar() {
    const float top = ImGui::GetCursorScreenPos().y;
    section("Exchange history", oi_usd_ ? "5m closes / Binance Futures / OI = contracts x close" : "5m closes / Binance Futures");
    const float below = ImGui::GetCursorScreenPos().y;
    static const char* const ranges[] = {"4h", "24h"};
    static const char* const units[] = {"Contracts", "Quote value"};
    const float h = 30 * K, gap = 10 * K;
    const float wr = Theme::segmented_width(ranges, 2, h), wu = Theme::segmented_width(units, 2, h);
    const float avail = ImGui::GetContentRegionAvail().x;
    const bool inline_controls = avail > wr + wu + gap + 360 * K;
    if (inline_controls) right_of_line(top, wr + wu + gap);
    else { space(6); }
    const int r = Theme::segmented("range", ranges, 2, hours_ == 24 ? 1 : 0, h);
    if ((r == 1 ? 24 : 4) != hours_) set_hours(r == 1 ? 24 : 4);
    ImGui::SameLine(0, gap);
    const int u = Theme::segmented("units", units, 2, oi_usd_ ? 1 : 0, h);
    if ((u == 1) != oi_usd_) { oi_usd_ = u == 1; fit_dirty_ = true; }
    if (inline_controls) ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, std::max(below, ImGui::GetCursorScreenPos().y)));
    space(12);
    // Range and units can change in the toolbar, before this frame's plots.
    if (history_.available() && fit_dirty_) {
        first_ = market_data_view::first_observation(history_, hours_);
        fits_ = market_data_view::fit(history_, first_, oi_usd_);
        inspection_.reconcile(history_, first_); fit_dirty_ = false;
    }
    if (inspection_.released) {
        char clock[48], line[96]; stamp(inspection_.released, clock, sizeof(clock));
        snprintf(line, sizeof(line), "Unpinned %s: outside the %dh window.", clock, hours_);
        meta(line, Tokens::WARN); space(6);
    }
}

void DataWidget::charts() {
    const auto& h = history_;
    const bool ready = loaded_ && h.available();
    const float width = ImGui::GetContentRegionAvail().x;
    const int columns = market_data_view::columns(width, 18.4f * K);
    struct Area { ImVec2 pos{}, size{}, header{}, clip_min{}, clip_max{}; bool visible = false; float header_width = 0; };
    std::array<Area, market_data_view::chart_count> areas{};
    const auto* meta_info = SymbolRegistry::instance().get(pair_.exchange, pair_.symbol);
    const char* quote = meta_info && !meta_info->quote_asset.empty() ? meta_info->quote_asset.c_str() : "quote units";
    const char* base = meta_info && !meta_info->base_asset.empty() ? meta_info->base_asset.c_str() : "base units";
    const auto& formatter = SymbolRegistry::instance().get_formatter(pair_.exchange, pair_.symbol);
    const float mono_size = 22 * K, small_size = 13.5f * K;
    inspection_.clear_hover();
    int focused = -1;
    auto panel = [&](size_t n, bool expanded, float inner) {
        ImGui::PushID(static_cast<int>(n));
        const bool has = ready && fits_[n].available;
        // Title row: title, then the info and expand icon buttons at the right.
        const float row_top = card_title(::charts[n].title, inner);
        const float icon = (touch_controls_ ? 36 : 26) * K;
        const ImVec2 after_title = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(after_title.x + inner - icon * 2 - 4 * K, row_top));
        Theme::icon_button("##info", UiIcon::Info, ::charts[n].help, icon);
        ui_test::mark("data.chart." + std::to_string(n) + ".info");
        ImGui::SameLine(0, 4 * K);
        ImGui::BeginDisabled(!has && !expanded);
        if (Theme::icon_button("##expand", expanded ? UiIcon::Collapse : UiIcon::Expand, expanded ? "Back to the grid" : "Expand chart", icon))
            expanded_ = expanded ? -1 : static_cast<int>(n);
        ui_test::mark("data.chart." + std::to_string(n) + ".expand");
        ImGui::EndDisabled();
        ImGui::SetCursorScreenPos(after_title);
        const char* detail = n == 1 && oi_usd_ ? "Contracts x close" : ::charts[n].detail;
        draw_text(ImGui::GetWindowDrawList(), Fonts::ui(), small_size, ImGui::GetCursorScreenPos(), Tokens::TX3, detail, inner);
        ImGui::Dummy(ImVec2(inner, small_size * 1.3f + 10 * K));
        // The value and its clock are drawn after every plot has seen the pointer (one shared selection).
        auto& area = areas[n];
        area.header = ImGui::GetCursorScreenPos(); area.header_width = inner;
        area.clip_min = ImGui::GetWindowDrawList()->GetClipRectMin(); area.clip_max = ImGui::GetWindowDrawList()->GetClipRectMax();
        ImGui::Dummy(ImVec2(inner, mono_size * 1.2f + small_size * 1.3f + 14 * K));
        const float height = expanded ? 380 * K : std::clamp(inner * .46f, 170 * K, 250 * K);
        font(Fonts::mono_sm(), 12.5f);
        ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(0, 4 * K));
        ImPlot::PushStyleVar(ImPlotStyleVar_LabelPadding, ImVec2(6 * K, 4 * K));
        if (ImPlot::BeginPlot("##history", ImVec2(inner, height), ImPlotFlags_CanvasOnly | ImPlotFlags_NoInputs)) {
            const double from = ready ? h.times[first_] : 0, to = ready ? h.times.back() : 14400;
            ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_Lock | ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickMarks);
            ImPlot::SetupAxisLimits(ImAxis_X1, from, to, ImGuiCond_Always);
            ImPlot::SetupAxisFormat(ImAxis_X1, [](double t, char* out, int size, void*) {
                stamp(static_cast<int64_t>(t * 1000), out, size); return static_cast<int>(strlen(out));
            });
            double ticks[24]; int nt = 0;
            const double interval = hours_ == 4 ? 3600 : 21600;
            for (double t = std::ceil(from / interval) * interval; t < to && nt < 24; t += interval)
                if (t > from + (to - from) * .04 && t < to - (to - from) * .03) ticks[nt++] = t;
            ImPlot::SetupAxisTicks(ImAxis_X1, ticks, nt);
            auto setup_y = [&](ImAxis axis, const market_data_view::Bounds& bounds, bool price, bool percent, bool right) {
                ImPlot::SetupAxis(axis, nullptr, ImPlotAxisFlags_Lock | ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks | (right ? ImPlotAxisFlags_Opposite : 0));
                ImPlot::SetupAxisLimits(axis, bounds.low, bounds.high, ImGuiCond_Always);
                if (price) ImPlot::SetupAxisFormat(axis, [](double v, char* out, int size, void* ptr) {
                    static_cast<const PriceFormatter*>(ptr)->format_price(out, size, v); return static_cast<int>(strlen(out));
                }, const_cast<PriceFormatter*>(&formatter));
                else if (percent) ImPlot::SetupAxisFormat(axis, "%.1f%%");
                else ImPlot::SetupAxisFormat(axis, [](double v, char* out, int size, void*) {
                    metric(std::abs(v), false, out, size); return static_cast<int>(strlen(out));
                });
            };
            const market_data_view::Bounds empty{0, 1};
            setup_y(ImAxis_Y1, ready ? (n == 2 ? fits_[n].price : fits_[n].primary) : empty, n == 0 || n == 2, n == 3 || n == 4, n != 2);
            if (n == 2) setup_y(ImAxis_Y2, ready ? fits_[n].primary : empty, false, true, true);
            if (has) {
                if ((n == 3 || n == 4) && fits_[n].primary.low <= 50 && fits_[n].primary.high >= 50) {
                    const double parity[] = {50}; ImPlotSpec ref;
                    ref.LineColor = Tokens::BD3; ref.Flags = ImPlotInfLinesFlags_Horizontal;
                    ImPlot::PlotInfLines("50%", parity, 1, ref);
                }
                const auto& values = n == 1 && oi_usd_ ? h.notional : h.values[market_data_view::series[n]];
                const int count = static_cast<int>(h.times.size() - first_);
                ImPlotSpec line; line.LineWeight = 1.6f; line.LineColor = n == 0 ? Tokens::TX1 : Tokens::REST;
                if (n == 5) {
                    struct Bars { const double* t; const double* v; double sign; };
                    auto point = [](int i, void* ptr) { const auto& b = *static_cast<Bars*>(ptr); return ImPlotPoint(b.t[i], b.v[i] * b.sign); };
                    Bars buy{h.times.data() + first_, h.values[5].data() + first_, 1}, sell{h.times.data() + first_, h.values[6].data() + first_, -1};
                    ImPlotSpec b; b.FillColor = b.LineColor = Tokens::UP;
                    ImPlot::PlotBarsG("Buy", point, &buy, count, 186, b);
                    b.FillColor = b.LineColor = Tokens::DOWN;
                    ImPlot::PlotBarsG("Sell", point, &sell, count, 186, b);
                } else {
                    if (n == 2) {
                        line.LineColor = Tokens::TX1;
                        ImPlot::PlotLine("Price", h.times.data() + first_, h.values[0].data() + first_, count, line);
                        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2); line.LineColor = Tokens::REST;
                    } else if (std::all_of(values.begin() + first_, values.end(), [](double v) { return std::isfinite(v); })) {
                        // A soft fill under single-series lines gives the card weight without a second colour (only
                        // without gaps: a fill must never bridge one).
                        ImPlotSpec fill; fill.FillColor = ImVec4(line.LineColor.x, line.LineColor.y, line.LineColor.z, 0.07f);
                        ImPlot::PlotShaded("##fill", h.times.data() + first_, values.data() + first_, count, fits_[n].primary.low, fill);
                    }
                    ImPlot::PlotLine(::charts[n].title, h.times.data() + first_, values.data() + first_, count, line);
                }
            }
            area.pos = ImPlot::GetPlotPos(); area.size = ImPlot::GetPlotSize();
            area.clip_min = ImGui::GetWindowDrawList()->GetClipRectMin(); area.clip_max = ImGui::GetWindowDrawList()->GetClipRectMax();
            area.visible = ImGui::IsRectVisible(area.pos, ImVec2(area.pos.x + area.size.x, area.pos.y + area.size.y));
            ImPlot::EndPlot();
            if (!has) {
                auto* draw = ImGui::GetWindowDrawList();
                const char* empty_text = !ready ? "Loading 5m history..." : "No observations";
                draw_text(draw, Fonts::ui(), small_size, ImVec2(area.pos.x + 8, area.pos.y + area.size.y * .4f), Tokens::TX3, empty_text, area.size.x - 16);
            }
            // NoInputs deliberately bypasses ImPlot's hit testing. One native
            // focusable button owns pin/keyboard input; wheel is left to the body.
            if (ready && area.visible) {
                const auto after = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos(area.pos);
                const bool clicked = ImGui::InvisibleButton("Inspect plot", area.size, ImGuiButtonFlags_EnableNav);
                ui_test::mark("data.chart." + std::to_string(n) + ".plot");
                const bool touch = ImGui::GetIO().MouseSource == ImGuiMouseSource_TouchScreen;
                if (ImGui::IsItemFocused()) {
                    focused = static_cast<int>(n);
                    ImGui::SetKeyOwner(ImGuiKey_LeftArrow, ImGui::GetItemID());
                    ImGui::SetKeyOwner(ImGuiKey_RightArrow, ImGui::GetItemID());
                    if (inspection_.pin) ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetItemID());
                }
                const bool hovered = ImGui::IsItemHovered();
                const double t = from + std::clamp((ImGui::GetIO().MousePos.x - area.pos.x) / area.size.x, 0.0f, 1.0f) * (to - from);
                if (hovered && !inspection_.pin && !touch) {
                    inspection_.hover = h.nearest(t, first_); inspection_.hover_chart = static_cast<int>(n); inspection_.released = 0;
                }
                if (clicked) {
                    const bool navigation = GImGui->NavActivateId == ImGui::GetItemID();
                    if (navigation || ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 36)
                        inspection_.point(h, navigation ? inspection_.effective(h, first_) : h.nearest(t, first_), static_cast<int>(n));
                }
                if (touch && ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 6)) ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
                ImGui::SetCursorScreenPos(after);
                ImGui::Dummy(ImVec2(0, 0));
            }
        }
        ImPlot::PopStyleVar(2); ImGui::PopFont(); ImGui::PopID();
    };
    // Plot chrome: no frame or border, faint grid, quiet tick labels.
    ImPlot::PushStyleColor(ImPlotCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImPlot::PushStyleColor(ImPlotCol_PlotBg, ImVec4(0, 0, 0, 0));
    ImPlot::PushStyleColor(ImPlotCol_PlotBorder, ImVec4(0, 0, 0, 0));
    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, ImVec4(Tokens::BD1.x, Tokens::BD1.y, Tokens::BD1.z, 0.9f));
    ImPlot::PushStyleColor(ImPlotCol_AxisText, Tokens::TX3);
    ImPlot::PushStyleColor(ImPlotCol_AxisBg, ImVec4(0, 0, 0, 0));
    ImPlot::PushStyleColor(ImPlotCol_AxisBgHovered, ImVec4(0, 0, 0, 0));
    ImPlot::PushStyleColor(ImPlotCol_AxisBgActive, ImVec4(0, 0, 0, 0));
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotMinSize, ImVec2(100, 80));
    const int expanded = expanded_; // Layout changes apply next frame.
    if (expanded >= 0) { CardGrid one(1); panel(static_cast<size_t>(expanded), true, one.next()); one.end(); space(12); }
    {
        CardGrid grid(columns);
        for (size_t n = 0; n < market_data_view::chart_count; ++n) {
            if (static_cast<int>(n) == expanded) continue;
            panel(n, false, grid.next());
        }
        grid.end();
    }
    ImPlot::PopStyleVar(); ImPlot::PopStyleColor(8);
    if (!ready) { keyboard_plot_ = -1; return; }
    const bool keyboard = focused >= 0 && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    // A popup can close during NewFrame and restore plot focus. Require the
    // same plot to have owned focus last frame so its dismissal cannot unpin.
    if (keyboard && keyboard_plot_ == focused) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) inspection_.step(h, first_, -1, focused);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) inspection_.step(h, first_, 1, focused);
        if (inspection_.pin && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetID("data_escape")); inspection_.unpin();
        }
    }
    keyboard_plot_ = keyboard ? focused : -1;
    const size_t selected = inspection_.effective(h, first_);
    const int active = inspection_.pin ? inspection_.chart : inspection_.hover_chart;
    const auto at = static_cast<int64_t>(h.times[selected] * 1000);
    char clock[48], start[48], readout[100]; stamp(at, clock, sizeof(clock)); stamp(at - 300000, start, sizeof(start));
    snprintf(readout, sizeof(readout), "%s%s", inspection_.pin ? "Pinned " : active < 0 ? "Latest " : "", clock);
    auto format = [&](size_t s, bool percent, bool price, char* out, size_t size) {
        const auto& values = s == 1 && oi_usd_ ? h.notional : h.values[s];
        metric(values[selected], percent, out, size);
        if (price && std::isfinite(values[selected])) formatter.format_price(out, size, values[selected]);
    };
    for (size_t n = 0; n < areas.size(); ++n) {
        const auto& a = areas[n];
        if (a.header_width <= 0) continue;
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(a.clip_min, a.clip_max, true);
        const float value_width = a.header_width;
        char value[64], text_buf[180]; format(market_data_view::series[n], n >= 2 && n <= 4, n == 0, value, sizeof(value));
        if (n == 2) {
            draw_text(draw, Fonts::mono_md(), mono_size, a.header, Tokens::TX1, value, a.header_width);
        } else if (n == 5) {
            char sell[64]; format(6, false, false, sell, sizeof(sell));
            snprintf(text_buf, sizeof(text_buf), "%s / ", value);
            char total[180]; snprintf(total, sizeof(total), "%s / %s %s", value, sell, base);
            const float needed = Fonts::mono_md()->CalcTextSizeA(mono_size, FLT_MAX, 0, total).x;
            const float value_size = std::clamp(mono_size * value_width / std::max(1.0f, needed), small_size, mono_size);
            const float buy_width = Fonts::mono_md()->CalcTextSizeA(value_size, FLT_MAX, 0, text_buf).x;
            draw_text(draw, Fonts::mono_md(), value_size, a.header, Tokens::UP, text_buf, value_width);
            snprintf(text_buf, sizeof(text_buf), "%s %s", sell, base);
            draw_text(draw, Fonts::mono_md(), value_size, ImVec2(a.header.x + buy_width, a.header.y), Tokens::DOWN, text_buf, std::max(20.0f, value_width - buy_width));
        } else {
            const bool finite = std::isfinite((n == 1 && oi_usd_ ? h.notional : h.values[market_data_view::series[n]])[selected]);
            draw_text(draw, Fonts::mono_md(), mono_size, a.header, Tokens::TX1, value, value_width);
            if (finite) {
                const char* unit = n == 0 ? quote : n == 1 ? (oi_usd_ ? quote : "contracts") : "long";
                const float vw = Fonts::mono_md()->CalcTextSizeA(mono_size, FLT_MAX, 0, value).x;
                draw_text(draw, Fonts::ui(), 14 * K, ImVec2(a.header.x + vw + 7 * K, a.header.y + mono_size - 14 * K * 1.05f), Tokens::TX2, unit, std::max(20.0f, value_width - vw));
            }
        }
        draw_text(draw, Fonts::ui(), small_size, ImVec2(a.header.x, a.header.y + mono_size * 1.2f + 4 * K), inspection_.pin ? Tokens::INSPECT : Tokens::TX3, readout, a.header_width);
        draw->PopClipRect();
        if (!a.visible || active < 0 || !fits_[n].available) continue;
        draw->PushClipRect(a.pos, ImVec2(a.pos.x + a.size.x, a.pos.y + a.size.y), true);
        const float x = a.pos.x + float((h.times[selected] - h.times[first_]) / (h.times.back() - h.times[first_])) * a.size.x;
        const auto cursor = ImGui::GetColorU32(inspection_.pin ? Tokens::INSPECT : Tokens::TX3);
        if (inspection_.pin) draw->AddLine(ImVec2(x, a.pos.y), ImVec2(x, a.pos.y + a.size.y), cursor);
        else for (float y = a.pos.y; y < a.pos.y + a.size.y; y += 7) draw->AddLine(ImVec2(x, y), ImVec2(x, std::min(y + 3, a.pos.y + a.size.y)), cursor);
        auto marker = [&](double v, const market_data_view::Bounds& bounds, const ImVec4& color) {
            if (!std::isfinite(v)) return;
            const float y = a.pos.y + a.size.y * float(1 - (v - bounds.low) / (bounds.high - bounds.low));
            draw->AddCircleFilled(ImVec2(x, y), 4.0f, ImGui::GetColorU32(Tokens::CARD));
            draw->AddCircle(ImVec2(x, y), 4.0f, ImGui::GetColorU32(color), 0, 1.75f);
        };
        marker((n == 1 && oi_usd_ ? h.notional : h.values[market_data_view::series[n]])[selected], fits_[n].primary, n == 0 ? Tokens::TX1 : n == 5 ? Tokens::UP : Tokens::REST);
        if (n == 2) marker(h.values[0][selected], fits_[n].price, Tokens::TX1);
        if (n == 5) marker(-h.values[6][selected], fits_[n].primary, Tokens::DOWN);
        if (static_cast<int>(n) == active) {
            // Compact, bounded tooltip in only the active plot. Source clock is
            // identical to headers, including when pointer moves away from a pin.
            char lines[4][192]; int rows = 0;
            snprintf(lines[rows++], sizeof(lines[0]), "%s%s%s%s / %s", inspection_.pin ? "Pinned / " : "", n == 5 ? start : "", n == 5 ? "-" : "", clock, DisplayTimeZone::instance().zone_name());
            snprintf(lines[rows++], sizeof(lines[0]), "%s", research_url::minute_header_utc(at).c_str());
            if (n == 2) {
                char price[64]; format(0, false, true, price, sizeof(price));
                snprintf(lines[rows++], sizeof(lines[0]), "Price %s %s", price, quote);
                snprintf(lines[rows++], sizeof(lines[0]), "OI / MC %s", value);
            } else if (n == 5) {
                char sell[64]; format(6, false, false, sell, sizeof(sell));
                snprintf(lines[rows++], sizeof(lines[0]), "Buy %s %s", value, base);
                snprintf(lines[rows++], sizeof(lines[0]), "Sell %s %s", sell, base);
            } else snprintf(lines[rows++], sizeof(lines[0]), "%s %s", value, n == 0 ? quote : n == 1 ? (oi_usd_ ? quote : "contracts") : "long accounts");
            const float tip_size = 13 * K;
            const float tw = std::min(a.size.x - 8, 255 * K), pad = 9 * K;
            float th = pad * 2;
            for (int i = 0; i < rows; ++i) th += Fonts::mono_sm()->CalcTextSizeA(tip_size, FLT_MAX, tw - pad * 2, lines[i]).y + 3;
            float tx = x > a.pos.x + a.size.x * .55f ? x - tw - 12 : x + 12;
            tx = std::clamp(tx, a.pos.x + 4, a.pos.x + a.size.x - tw - 4);
            ImVec2 p(tx, a.pos.y + 6);
            draw->AddRectFilled(p, ImVec2(p.x + tw, p.y + th), ImGui::GetColorU32(Tokens::ELEV), 8.0f);
            draw->AddRect(p, ImVec2(p.x + tw, p.y + th), ImGui::GetColorU32(Tokens::BD2), 8.0f);
            float y = p.y + pad;
            for (int i = 0; i < rows; ++i) {
                draw_text(draw, Fonts::mono_sm(), tip_size, ImVec2(p.x + pad, y), i < 2 ? Tokens::TX3 : n == 5 ? (i == 2 ? Tokens::UP : Tokens::DOWN) : Tokens::TX1, lines[i], tw - pad * 2);
                y += Fonts::mono_sm()->CalcTextSizeA(tip_size, FLT_MAX, tw - pad * 2, lines[i]).y + 3;
            }
        }
        draw->PopClipRect();
    }
    size_t available_from = first_;
    while (available_from < h.times.size() && !std::isfinite(h.values[0][available_from])) ++available_from;
    if (available_from > first_ && available_from < h.times.size()) {
        stamp(static_cast<int64_t>(h.times[available_from] * 1000), clock, sizeof(clock));
        char line[96]; snprintf(line, sizeof(line), "Price history available from %s.", clock);
        space(6); meta(line);
    }
    if (Entitlements::wall_now_ms() - h.to > 900000) { space(6); meta("History stale", Tokens::WARN); }
}
const MarketContext::Item& DataWidget::context_item(size_t id) const {
    return context_.items[id].at ? context_.items[id] : last_context_.items[id];
}
ContextState DataWidget::context_state(size_t id) const {
    return market_data_view::context_state(context_.items[id], last_context_.items[id], static_cast<int64_t>(Entitlements::wall_now_ms()));
}
void DataWidget::context_age(size_t id, char* out, size_t size) const {
    const auto& item = context_item(id);
    const auto state = context_state(id);
    char time[48], age[32];
    if (state == ContextState::Missing) { snprintf(out, size, "%s", context_loaded_ ? "Not supplied" : "Loading research..."); return; }
    if (state == ContextState::Expired) {
        stamp(item.until, time, sizeof(time)); snprintf(out, size, "Expired %s", time); return;
    }
    stamp(item.at, time, sizeof(time));
    StatFreshness::age_label(std::max<int64_t>(0, static_cast<int64_t>(Entitlements::wall_now_ms()) - item.at), age, sizeof(age));
    snprintf(out, size, "%s / %s ago%s", time, age, state == ContextState::Withdrawn ? " / not current" : "");
}
void DataWidget::select_condition(int id) {
    condition_ = condition_ == id ? -1 : id;
    method_open_ = inspector_clocks_open_ = false;
    scroll_to_inspector_ = condition_ >= 0;
}
void DataWidget::source_clock(size_t id) {
    const auto& item = context_item(id);
    font(Fonts::ui_semibold(), 13.5f); ImGui::TextWrapped("%s", context_names[id]); ImGui::PopFont();
    char at[64], until[64], age[96]; context_age(id, age, sizeof(age)); meta(age);
    if (!item.at) return;
    stamp(item.at, at, sizeof(at), TimeZoneFormat::DateTimeMinutes);
    stamp(item.until, until, sizeof(until), TimeZoneFormat::DateTimeMinutes);
    space(4);
    char line[160];
    snprintf(line, sizeof(line), "Observed %s / %s", at, DisplayTimeZone::instance().zone_name()); secondary(line);
    meta(research_url::minute_header_utc(item.at).c_str());
    snprintf(line, sizeof(line), "Valid until %s", until); secondary(line);
    meta(research_url::minute_header_utc(item.until).c_str());
    // at is a source observation/bucket, NOT a guaranteed publication clock.
}

void DataWidget::context_row() {
    const float top = ImGui::GetCursorScreenPos().y;
    section("Market context");
    if (right_link(top - 4 * K, "Source clocks", clocks_open_ ? UiIcon::ChevronUp : UiIcon::ChevronDown, clocks_open_)) clocks_open_ = !clocks_open_;
    ui_test::mark("data.source_clocks");
    space(10);
    const float width = ImGui::GetContentRegionAvail().x;
    const int cols = width >= 900 * K ? 3 : 1;
    CardGrid grid(cols);
    for (const auto id : {MarketContext::Readings, MarketContext::Regime}) {
        const float inner = grid.next();
        ImGui::PushID(static_cast<int>(id));
        meta(context_names[id]);
        space(6);
        const auto& item = context_item(id);
        const auto state = context_state(id);
        if (state == ContextState::Current && id == MarketContext::Readings) reading(item, inner);
        else if (state == ContextState::Current) text(item.value.c_str(), Tokens::TX1, Fonts::ui_semibold(), 18);
        else text(state == ContextState::Missing ? "Unavailable" : "Not current", Tokens::TX2, Fonts::ui_semibold(), 18);
        space(6);
        char age[96]; context_age(id, age, sizeof(age)); meta(age);
        space(10);
        if (link("Details", UiIcon::ChevronRight, condition_ == static_cast<int>(id))) select_condition(static_cast<int>(id));
        ui_test::mark("data.context." + std::to_string(static_cast<int>(id)) + ".details");
        ImGui::PopID();
    }
    grid.next();
    meta("Research snapshot");
    space(6);
    if (readings_.available()) {
        size_t present = 0; for (const auto& item : readings_.items) if (item.present()) ++present;
        char count[48]; snprintf(count, sizeof(count), "%zu / %zu", present, readings_.items.size());
        font(Fonts::mono_md(), 18); ImGui::TextUnformatted(count); ImGui::PopFont();
        ImGui::SameLine(0, 8 * K); secondary("readings available");
        space(6);
        char observed[64], line[96]; stamp(readings_.observed, observed, sizeof(observed));
        const bool stale = readings_.stale(static_cast<int64_t>(Entitlements::wall_now_ms()));
        snprintf(line, sizeof(line), "%s / %s", observed, stale ? "Stale" : "Original minute");
        meta(line, stale ? Tokens::WARN : Tokens::TX3);
    } else {
        text(readings_loaded_ ? "Unavailable" : "Loading...", Tokens::TX2, Fonts::ui_semibold(), 18);
        space(6); meta("Separate source times");
    }
    space(10);
    if (link("View readings")) { readings_view_ = true; ImGui::SetScrollY(0); }
    ui_test::mark("data.view_readings");
    grid.end();
    if (clocks_open_) {
        space(12);
        CardGrid clocks(cols);
        for (size_t i = 0; i < MarketContext::Count; ++i) { clocks.next(); ImGui::PushID(static_cast<int>(i)); source_clock(i); ImGui::PopID(); }
        clocks.end();
    }
}

void DataWidget::inspector(size_t id) {
    ImGui::PushID("inspector");
    if (scroll_to_inspector_) { ImGui::SetScrollHereY(0); scroll_to_inspector_ = false; }
    CardGrid card(1);
    const float inner = card.next();
    const float top = card_title(context_names[id], inner);
    const ImVec2 after = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(after.x + inner - 26 * K, top));
    const bool close = Theme::icon_button("##close", UiIcon::Close, "Close", 26 * K);
    ui_test::mark("data.inspector.close");
    ImGui::SetCursorScreenPos(after);
    const auto& item = context_item(id);
    const auto state = context_state(id);
    char age[96]; context_age(id, age, sizeof(age));
    font(Fonts::ui(), 13); Theme::chip(qualifiers[id], Tokens::TX2, Tokens::CONTROL); ImGui::PopFont();
    space(10);
    if (state == ContextState::Current) {
        if (id == MarketContext::Readings) reading(item, inner, 20);
        else text(item.value.c_str(), tone_of(item.tone), Fonts::mono_md(), 20);
    } else {
        heading(state == ContextState::Expired ? "Expired" : "Unavailable");
        if (item.at) { space(4); meta("Previous reading"); secondary(item.value.c_str()); }
        if (state == ContextState::Withdrawn) meta("Withdrawn; status unknown.");
        if (state == ContextState::Missing) meta("No reading; status unknown.");
    }
    space(4); meta(age);
    if (id == MarketContext::Cascade) { space(10); meta("Unvalidated preview: a rank, not a loss probability.", Tokens::WARN); }
    space(14);
    if (Theme::choice_button("Method", method_open_)) method_open_ = !method_open_;
    ImGui::SameLine(0, 6 * K);
    if (Theme::choice_button("Clocks", inspector_clocks_open_)) inspector_clocks_open_ = !inspector_clocks_open_;
    if (item.at) {
        ImGui::SameLine(0, 6 * K);
        if (Theme::button("Investigate", ButtonKind::Soft, ImVec2(0, 0), UiIcon::Search)) open_url(research_url::moment_url(pair_.symbol, item.at));
    }
    if (method_open_) {
        space(10);
        if (!item.detail.empty()) secondary(item.detail.c_str());
        secondary(item.help.empty() ? "Method unavailable." : item.help.c_str());
    }
    if (inspector_clocks_open_) { space(10); source_clock(id); }
    card.end();
    if (close) condition_ = -1;
    ImGui::PopID();
}

void DataWidget::research() {
    const float top = ImGui::GetCursorScreenPos().y;
    constexpr std::array<size_t, 4> conditions{MarketContext::Awake, MarketContext::Flow, MarketContext::OIAhead, MarketContext::Cascade};
    int current = 0;
    for (const auto id : conditions) if (context_state(id) == ContextState::Current) ++current;
    section("Research observations", current ? "Separate source times; conditions may overlap." : nullptr);
    if (right_link(top - 4 * K, "Saved research", UiIcon::ArrowUpRight)) open_url("https://edgedepth.com/research/workbench?tab=saved");
    ui_test::mark("data.saved_research");
    space(current ? 10 : 4);
    const float width = ImGui::GetContentRegionAvail().x;
    if (current) {
        const int columns = std::min(current, market_data_view::columns(width, 18.4f * K));
        CardGrid grid(columns);
        for (const auto id : conditions) {
            if (context_state(id) != ContextState::Current) continue;
            const auto& item = context_item(id);
            grid.next(); ImGui::PushID(static_cast<int>(id));
            meta(context_names[id]);
            space(6);
            text(item.value.c_str(), tone_of(item.tone), Fonts::ui_semibold(), 18);
            space(8);
            // These qualifiers change the meaning of the reading and stay visible.
            font(Fonts::ui(), 13); Theme::chip(qualifiers[id], Tokens::TX2, Tokens::CONTROL); ImGui::PopFont();
            space(6);
            char age[96]; context_age(id, age, sizeof(age)); meta(age);
            space(12);
            if (link("Details", UiIcon(255), condition_ == static_cast<int>(id))) select_condition(static_cast<int>(id));
            ui_test::mark("data.research." + std::to_string(id) + ".details");
            ImGui::SameLine(0, 6 * K);
            if (Theme::button("Investigate", ButtonKind::Soft, ImVec2(0, 28 * K), UiIcon::Search)) open_url(research_url::moment_url(pair_.symbol, item.at));
            ui_test::mark("data.research." + std::to_string(id) + ".investigate");
            ImGui::PopID();
        }
        grid.end();
    } else meta(context_loaded_ ? "No current observations." : "Loading research...");
    // Coverage: a disclosure row, then one line per condition with a status dot.
    space(12);
    char coverage[80];
    snprintf(coverage, sizeof(coverage), "Coverage: %d of %zu current", current, conditions.size());
    bool& open = *ImGui::GetStateStorage()->GetBoolRef(ImGui::GetID("research_coverage"), false);
    if (link(coverage, open ? UiIcon::ChevronUp : UiIcon::ChevronDown)) open = !open;
    ui_test::mark("data.coverage");
    if (open) {
        space(6);
        for (const auto id : conditions) {
            ImGui::PushID(static_cast<int>(id));
            const auto state = context_state(id);
            const ImVec4& dot = state == ContextState::Current ? Tokens::UP : state == ContextState::Expired ? Tokens::WARN : Tokens::TX4;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float lh = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 12 * K, p.y + lh * 0.5f + 4 * K), 3.5f * K, ImGui::GetColorU32(dot));
            ImGui::SetCursorScreenPos(ImVec2(p.x + 24 * K, p.y + 4 * K));
            char age[96], line[200]; context_age(id, age, sizeof(age));
            snprintf(line, sizeof(line), "%s", context_names[id]);
            ImGui::TextUnformatted(line);
            ImGui::SameLine(0, 10 * K);
            font(Fonts::ui(), 12.5f);
            ImGui::TextColored(Tokens::TX3, "%s", state == ContextState::Current ? age : state == ContextState::Expired ? "Expired" : state == ContextState::Withdrawn ? "Not current" : "Not supplied");
            ImGui::PopFont();
            if (context_item(id).at) {
                ImGui::SameLine(0, 10 * K);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4 * K);
                if (Theme::button("Inspect record", ButtonKind::Ghost, ImVec2(0, 24 * K))) select_condition(static_cast<int>(id));
            }
            ImGui::PopID();
        }
        space(4); meta("Missing means unknown, not inactive.");
    }
    // Summary Details buttons also land here, without repeating their rows.
    if (condition_ >= 0) { space(16); inspector(static_cast<size_t>(condition_)); }
}

void DataWidget::research_actions(int64_t at, const char* fields) {
    if (at <= 0) return;
    if (Theme::button("Investigate", ButtonKind::Soft, ImVec2(0, 0), UiIcon::Search)) open_url(research_url::moment_url(pair_.symbol, at, fields));
    ui_test::mark("data.actions.investigate");
    if (ImGui::IsItemHovered()) Theme::tooltip("Opens this minute. Reading windows vary.");
    ImGui::SameLine(0, 8 * K);
    if (Theme::button("Compare lead-up", ButtonKind::Soft, ImVec2(0, 0), UiIcon::ArrowUpRight, true)) open_url(research_url::leadup_url(pair_.symbol, at, fields));
    ui_test::mark("data.actions.leadup");
}

// The pin bar under the charts: the selected close on the left (pinned in the inspect colour, with step and
// unpin controls), the research actions on the right.
void DataWidget::history_actions() {
    if (!history_.available()) return;
    space(14);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float right = p.x + ImGui::GetContentRegionAvail().x;
    const float row_h = ImGui::GetFrameHeight();
    const auto at = inspection_.pin ? inspection_.pin : static_cast<int64_t>(history_.times.back() * 1000);
    char time[80], line[160]; stamp(at, time, sizeof(time), TimeZoneFormat::DateTimeMinutes);
    if (inspection_.pin) {
        if (Theme::icon_button("##prev", UiIcon::ChevronLeft, "Previous close", row_h)) inspection_.step(history_, first_, -1, inspection_.chart);
        ui_test::mark("data.pin.prev");
        ImGui::SameLine(0, 2 * K);
        if (Theme::icon_button("##next", UiIcon::ChevronRight, "Next close", row_h)) inspection_.step(history_, first_, 1, inspection_.chart);
        ui_test::mark("data.pin.next");
        ImGui::SameLine(0, 8 * K);
    }
    snprintf(line, sizeof(line), "%s %s / %s", inspection_.pin ? "Pinned" : "Latest close", time, DisplayTimeZone::instance().zone_name());
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row_h - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::TextColored(inspection_.pin ? Tokens::INSPECT : Tokens::TX1, "%s", line);
    ImGui::SameLine(0, 12 * K);
    if (inspection_.pin) {
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p.y));
        if (Theme::button("Unpin", ButtonKind::Ghost)) inspection_.unpin();
        ui_test::mark("data.unpin");
    } else {
        font(Fonts::ui(), 12.5f); ImGui::TextColored(Tokens::TX3, "Click a chart to pin"); ImGui::PopFont();
    }
    const int chart = inspection_.pin ? inspection_.chart : 0;
    const char* fields = chart >= 0 && chart < static_cast<int>(market_data_view::chart_count) ? market_data_view::research_fields[chart] : "";
    const float actions_w = Theme::button_width("Investigate", 0, true) + 8 * K + Theme::button_width("Compare lead-up", 0, true);
    if (right - actions_w > ImGui::GetItemRectMax().x + 16 * K) ImGui::SetCursorScreenPos(ImVec2(right - actions_w, p.y));
    else space(8);
    research_actions(at, fields);
}

void DataWidget::research_readings() {
    const float top = ImGui::GetCursorScreenPos().y, left = ImGui::GetCursorScreenPos().x;
    if (!readings_.available()) {
        section("Research readings");
        space(8); meta(readings_loaded_ ? "Readings unavailable." : "Loading readings...");
        return;
    }
    const auto now = static_cast<int64_t>(Entitlements::wall_now_ms());
    char observed[80], caption[160]; stamp(readings_.observed, observed, sizeof(observed), TimeZoneFormat::DateTimeMinutes);
    snprintf(caption, sizeof(caption), "Snapshot %s / %s / independent of the chart pin", observed, DisplayTimeZone::instance().zone_name());
    section("Research readings", caption);
    size_t present = 0;
    for (const auto& item : readings_.items) if (item.present()) ++present;
    char count[48]; snprintf(count, sizeof(count), "%zu / %zu available", present, readings_.items.size());
    font(Fonts::ui(), 12.5f);
    const float cw = ImGui::CalcTextSize(count).x + 14;
    right_of_line(top + 2 * K, cw);
    Theme::chip(count, Tokens::TX2, Tokens::CONTROL);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(left, std::max(ImGui::GetCursorScreenPos().y, top + 40 * K)));
    if (readings_.stale(now)) meta("Snapshot stale", Tokens::WARN);
    if (readings_failed_) meta("Refresh failed; last snapshot shown.", Tokens::WARN);
    space(10);
    // Search: a rounded field with a magnifier in its left padding.
    {
        const float fw = std::min(340.0f * K, std::max(120.0f, ImGui::GetContentRegionAvail().x - 220 * K));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(32 * K, 8 * K));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Tokens::CARD);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Tokens::CONTROL);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Tokens::CONTROL);
        ImGui::SetNextItemWidth(fw);
        if (ImGui::InputTextWithHint("##research_readings_search", "Search readings", readings_filter_.InputBuf, IM_ARRAYSIZE(readings_filter_.InputBuf)))
            readings_filter_.Build();
        ui_test::mark("data.readings.search");
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRect(a, b, ImGui::GetColorU32(ImGui::IsItemActive() ? Tokens::BD3 : Tokens::BD1), Radius::R2);
        drawing::draw_ui_icon(ImGui::GetWindowDrawList(), UiIcon::Search, ImVec2(p.x + 16 * K, (a.y + b.y) * 0.5f), 6 * K, ImGui::GetColorU32(Tokens::TX3), 1.4f);
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        ImGui::SameLine(0, 16 * K);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (b.y - a.y - ImGui::GetFrameHeight()) * 0.5f);
        ImGui::Checkbox("Include unavailable", &readings_missing_);
        ui_test::mark("data.readings.missing");
    }
    space(16);
    const auto selected = std::find_if(readings_.items.begin(), readings_.items.end(), [&](const auto& item) { return item.id == selected_reading_; });
    if (selected != readings_.items.end()) {
        CardGrid card(1);
        const float inner = card.next();
        const float title_top = card_title(selected->name.c_str(), inner);
        const ImVec2 after = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(after.x + inner - 26 * K, title_top));
        const bool close = Theme::icon_button("##close_reading", UiIcon::Close, "Close reading", 26 * K);
        ui_test::mark("data.reading.close");
        ImGui::SetCursorScreenPos(after);
        text(selected->present() ? selected->display.c_str() : selected->availability == "history_only" ? "Historical only" : "Unavailable",
             selected->tone > 0 ? Tokens::UP : selected->tone < 0 ? Tokens::DOWN : Tokens::TX1, Fonts::mono_lg(), 26);
        space(6);
        secondary(selected->description.c_str());
        char unit[160]; snprintf(unit, sizeof(unit), "Unit: %s / %s", selected->unit.c_str(), observed); meta(unit);
        if (!selected->present()) meta(selected->availability == "history_only" ? "History only; no live reading." : "Missing, not zero.");
        space(12);
        research_actions(readings_.observed, selected->id.c_str());
        card.end();
        if (close) selected_reading_.clear();
        space(16);
    }
    const auto visible = [&](const ResearchReadings::Item& item) {
        return (readings_missing_ || item.present()) && readings_filter_.PassFilter(item.search.c_str());
    };
    const int cols = market_data_view::columns(ImGui::GetContentRegionAvail().x, 18.4f * K);
    size_t shown = 0;
    CardGrid grid(cols, 14.0f, ImVec2(16, 16));
    for (const auto& group : readings_.groups) {
        size_t n = 0; for (const auto i : group.items) if (visible(readings_.items[i])) ++n;
        if (!n) continue;
        const float width = grid.next(); ImGui::PushID(group.name.c_str());
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            font(Fonts::ui_semibold(), 16); ImGui::TextUnformatted(group.name.c_str()); ImGui::PopFont();
            char c[16]; snprintf(c, sizeof(c), "%zu", n);
            font(Fonts::ui(), 13.5f);
            ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + width - ImGui::CalcTextSize(c).x - 4 * K, p.y + 2 * K), ImGui::GetColorU32(Tokens::TX3), c);
            ImGui::PopFont();
        }
        space(6);
        const float label_width = width * .62f - 12 * K, value_width = width * .38f;
        const float size = 15 * K;
        bool first = true;
        for (const auto i : group.items) {
            const auto& item = readings_.items[i]; if (!visible(item)) continue;
            ++shown; ImGui::PushID(item.id.c_str());
            if (!first) {
                const ImVec2 lp = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddLine(ImVec2(lp.x + 8 * K, lp.y), ImVec2(lp.x + width - 8 * K, lp.y), ImGui::GetColorU32(ImVec4(Tokens::BD1.x, Tokens::BD1.y, Tokens::BD1.z, 0.8f)));
            }
            first = false;
            const auto pos = ImGui::GetCursorScreenPos();
            const char* value = item.present() ? item.display.c_str() : item.availability == "history_only" ? "Historical only" : "Unavailable";
            const float height = std::max({Fonts::ui()->CalcTextSizeA(size, FLT_MAX, label_width, item.name.c_str()).y,
                Fonts::mono_md()->CalcTextSizeA(size + 0.5f * K, FLT_MAX, value_width, value).y, (touch_controls_ ? 36.f : 22.f) * K}) + 12 * K;
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
            const bool picked = selected_reading_ == item.id;
            if (ImGui::Selectable("##reading", picked, 0, ImVec2(width, height))) { selected_reading_ = item.id; ImGui::SetScrollY(0); }
            ui_test::mark("data.reading." + item.id);
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered() || picked)
                ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(picked ? Tokens::CONTROL_HOVER : Tokens::CONTROL), Radius::R2);
            draw_text(ImGui::GetWindowDrawList(), Fonts::ui(), size, ImVec2(pos.x + 8 * K, pos.y + 6 * K), Tokens::TX1, item.name.c_str(), label_width);
            // Medium mono a half step up: Roboto Mono's figures read smaller than Inter at one size.
            const float vsize = size + 0.5f * K;
            const float value_size = Fonts::mono_md()->CalcTextSizeA(vsize, FLT_MAX, 0, value).x;
            draw_text(ImGui::GetWindowDrawList(), Fonts::mono_md(), vsize,
                ImVec2(pos.x + width - 8 * K - std::min(value_size, value_width), pos.y + 6 * K + (size - vsize) * 0.5f),
                item.tone > 0 ? Tokens::UP : item.tone < 0 ? Tokens::DOWN : item.present() ? Tokens::TX1 : Tokens::TX3, value, value_width);
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    grid.end();
    if (!shown) { space(8); meta("No matches."); }
}

void DataWidget::basic_overview() {
    const auto now = static_cast<int64_t>(Entitlements::wall_now_ms());
    const auto* ticker = ctx_.tickers ? ctx_.ticker_mgr().get(pair_.exchange, pair_.symbol) : nullptr;
    const bool fresh = market_data_overview::ticker_fresh(ticker, now);
    const auto* meta_info = SymbolRegistry::instance().get(pair_.exchange, pair_.symbol);
    const auto& formatter = SymbolRegistry::instance().get_formatter(pair_.exchange, pair_.symbol);
    const char* quote = meta_info && !meta_info->quote_asset.empty() ? meta_info->quote_asset.c_str() : "quote units";
    const auto& stat = basic_snapshot_.stat;
    char values[5][64]{}, details[5][96]{};
    const char* labels[]{"Last price", "24h change", "24h volume", "Open interest", "Funding rate"};
    const char* help[]{"Latest venue trade.",
        "Trailing 24h price change.",
        "Trailing 24h volume in quote units.",
        "Open contracts, not dollars.",
        "Rate per shown interval. Positive: longs pay shorts."};
    for (int i = 0; i < 5; ++i) snprintf(values[i], sizeof(values[i]), "Unavailable");
    for (int i = 0; i < 3; ++i) {
        if (ticker && ticker->event_time_ms > 0 && ticker->event_time_ms <= now) {
            char clock[48]; stamp(ticker->event_time_ms, clock, sizeof(clock), TimeZoneFormat::DateTimeMinutes);
            snprintf(details[i], sizeof(details[i]), "%s / %s", fresh ? "Observed" : "Stale", clock);
        } else snprintf(details[i], sizeof(details[i]), "No current ticker");
    }
    if (fresh) {
        if (std::isfinite(ticker->last_price) && ticker->last_price > 0)
            snprintf(values[0], sizeof(values[0]), formatter.price_fmt, ticker->last_price);
        if (std::isfinite(ticker->change_pct_24h))
            snprintf(values[1], sizeof(values[1]), "%+.2f%%", ticker->change_pct_24h);
        if (std::isfinite(ticker->volume_quote) && ticker->volume_quote >= 0) {
            char number[32]; metric(ticker->volume_quote, false, number, sizeof(number));
            snprintf(values[2], sizeof(values[2]), "%s %s", number, quote);
        }
    }
    const int64_t clocks[]{stat.open_interest_ms, stat.funding_ms};
    for (int i = 3; i < 5; ++i) {
        const auto state = market_data_overview::field(basic_snapshot_, clocks[i-3], now);
        if (state.state == StatFreshness::State::Fresh || state.state == StatFreshness::State::Stale) {
            char clock[48]; stamp(clocks[i-3], clock, sizeof(clock), TimeZoneFormat::DateTimeMinutes);
            snprintf(details[i], sizeof(details[i]), "%s / %s", state.state == StatFreshness::State::Fresh ? "Observed" : "Stale", clock);
        } else snprintf(details[i], sizeof(details[i]), "No timestamp");
        if (state.state != StatFreshness::State::Fresh) continue;
        if (i == 3 && std::isfinite(stat.open_interest_usd) && stat.open_interest_usd >= 0) {
            char number[32]; metric(stat.open_interest_usd, false, number, sizeof(number));
            snprintf(values[i], sizeof(values[i]), "%s contracts", number);
        }
        if (i == 4 && std::isfinite(stat.funding)) {
            char cadence[8]; StatFreshness::cadence_label(stat.funding_interval_minutes > 0, stat.funding_interval_minutes, cadence, sizeof(cadence));
            snprintf(values[i], sizeof(values[i]), "%+.4f%%%s%s", stat.funding * 100, cadence[0] ? " / " : "", cadence);
        }
    }
    if (meta_info && !meta_info->perpetual) { snprintf(values[4], sizeof(values[4]), "Not applicable"); snprintf(details[4], sizeof(details[4]), "Dated contract"); }
    section("Market overview");
    space(10);
    const int columns = std::clamp(static_cast<int>(ImGui::GetContentRegionAvail().x / (250 * K)), 1, 3);
    CardGrid grid(columns);
    for (int i = 0; i < 5; ++i) {
        grid.next(); ImGui::PushID(i);
        meta(labels[i]);
        if (ImGui::IsItemHovered()) Theme::tooltip("%s", help[i]);
        space(6);
        const bool signed_change = i == 1 && fresh && std::isfinite(ticker->change_pct_24h) && ticker->change_pct_24h != 0;
        text(values[i], signed_change ? (ticker->change_pct_24h > 0 ? Tokens::UP : Tokens::DOWN) : Tokens::TX1, Fonts::mono_lg(), 22);
        space(6); meta(details[i]);
        ImGui::PopID();
    }
    grid.end();
    space(24);
}

void DataWidget::pro_preview() {
    if (pair_.exchange != "binancef") {
        CardGrid g(1); g.next();
        heading("History: Binance Futures only");
        char line[96]; snprintf(line, sizeof(line), "%s history unavailable on all plans.", widget_venue_label(pair_.exchange));
        space(4); secondary(line);
        g.end();
        return;
    }
    CardGrid g(1, 12.0f, ImVec2(24, 22));
    g.next();
    font(Fonts::label(), 11.5f); Theme::chip("PRO / RESEARCH", Tokens::LOGO, ImVec4(Tokens::LOGO.x, Tokens::LOGO.y, Tokens::LOGO.z, 0.12f)); ImGui::PopFont();
    space(10);
    char title[96]; snprintf(title, sizeof(title), "Explore %s history", widget_symbol_label(pair_.symbol).c_str());
    text(title, Tokens::TX1, Fonts::ui_semibold(), 20);
    space(10);
    for (const char* line : {"6 linked charts / 4h or 24h", "Price, OI, positioning and taker flow", "Research readings / 5, 15 or 30m comparisons"}) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        drawing::draw_ui_icon(ImGui::GetWindowDrawList(), UiIcon::Check, ImVec2(p.x + 6 * K, p.y + ImGui::GetTextLineHeight() * 0.5f), 5 * K, ImGui::GetColorU32(Tokens::LOGO), 1.6f);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 20 * K, p.y));
        secondary(line);
        space(2);
    }
    space(14);
    if (Theme::button("Explore Pro", ButtonKind::Upgrade, ImVec2(0, 34 * K))) ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Research,
        "Linked charts and research readings for Binance Futures.", "market_data");
    space(8);
    meta("Research adds study capacity and replay depth.");
    g.end();
}
