#include "ui/stats_widget.h"
#include "core/ticker_manager.h"
#include "core/logo_manager.h"
#include "core/display_time_zone.h"
#include <cstdlib>
#include "rendering/theme.h"
#include "core/entitlements.h"
#include "ui/upsell_modal.h"
#include "core/analytics_manager.h"
#include "core/indicator_series.h"

#include "imgui.h"
#include "implot.h"
#include <algorithm>

#include <cctype>
#include <cfloat>
#include <cstdio>
#include <string>

#include "core/terminal_request.h"
#include "rendering/menu.h"

using namespace Theme;

namespace {

void fmt_compact(double v, char* buf, size_t n);
void fmt_usd(double v, char* buf, size_t n);

inline ImU32 col(const ImVec4& c) { return ImGui::GetColorU32(c); }

inline std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Small pill: optional fill + optional border + centered mono-tiny label. Used for
// the PRO tag and the UNLOCK chip. Returns the pill width.
float draw_tag(ImDrawList* dl, ImVec2 pos, const char* txt,
               ImU32 bg, ImU32 border, ImU32 fg, float pad_x) {
    ImGui::PushFont(Fonts::mono_sm());
    const ImVec2 ts = ImGui::CalcTextSize(txt);
    const float w = ts.x + pad_x * 2.0f;
    const float h = ts.y + 3.0f;
    const ImVec2 b(pos.x + w, pos.y + h);
    if (bg)     dl->AddRectFilled(pos, b, bg, 2.0f);
    if (border) dl->AddRect(pos, b, border, 2.0f, 0, 1.0f);
    dl->AddText(ImVec2(pos.x + pad_x, pos.y + 1.5f), fg, txt);
    ImGui::PopFont();
    return w;
}

}  // namespace

StatsWidget::StatsWidget(const Terminal::Pair& pair, const AppContext& ctx, const PriceFormatter& fmt)
    : pair_(pair)
    , title_("     Stats  " + widget_symbol_label(pair.symbol) + "###Stats " + pair.exchange + " " + pair.symbol)
    , stream_key_{pair, Terminal::Stream::Stats, StatsSnapshot::timeframe_ms}
    , hour_stream_key_{pair, Terminal::Stream::Stats, StatsSnapshot::hour_ms}
    , volume_stream_key_{pair, Terminal::Stream::Volumes, StatsSnapshot::timeframe_ms}
    , ctx_(ctx)
    , fmt_(fmt)
{
    context_id_ = next_terminal_request_id();
    StreamHandler<Terminal::Stat> handler{
        .widget_ptr = this,
        .callback = [](void* ptr, const Terminal::Stat& s) {
            static_cast<StatsWidget*>(ptr)->handle_stat(s);
        }
    };
    subscribed_streams_ = &ctx_.stream_mgr();
    subscribed_streams_->subscribe_stats(stream_key_, handler);
    subscribed_streams_->subscribe_stats(hour_stream_key_, handler);
    subscribed_streams_->subscribe_volume(volume_stream_key_, StreamHandler<Terminal::Volume>{
        .widget_ptr = this,
        .callback = [](void* ptr, const Terminal::Volume& volume) {
            static_cast<StatsWidget*>(ptr)->volume_snapshot_.apply(volume, static_cast<int64_t>(Entitlements::wall_now_ms()));
        }
    });
    if (Entitlements::is_pro()) subscribed_streams_->subscribe_direct({pair_, Terminal::Stream::IcebergState, 0}, this);
    for (auto stream : {Terminal::Stream::PositioningState, Terminal::Stream::VPINState})
        subscribed_streams_->subscribe_direct({pair_, stream, 0}, this);
}

StatsWidget::~StatsWidget() {
#ifdef __EMSCRIPTEN__
    terminal_request_cancel(context_id_);
    terminal_request_cancel(context_id_ + 1);
#endif
    if (!subscribed_streams_) return;
    subscribed_streams_->unsubscribe_stats(stream_key_, this);
    subscribed_streams_->unsubscribe_stats(hour_stream_key_, this);
    subscribed_streams_->unsubscribe_volume(volume_stream_key_, this);
    subscribed_streams_->unsubscribe_direct({pair_, Terminal::Stream::IcebergState, 0}, this);
    for (auto stream : {Terminal::Stream::PositioningState, Terminal::Stream::VPINState})
        subscribed_streams_->unsubscribe_direct({pair_, stream, 0}, this);
}

void StatsWidget::handle_stat(const Terminal::Stat& stat) {
    const auto now = static_cast<int64_t>(Entitlements::wall_now_ms());
    if (ctx_.stream_mgr().is_replay_mode()) return;
    if (stat.historical) {
        if ((stat.timeframe == 60000 || stat.timeframe == 60) && stat.oi_close > 0) {
            const int64_t bucket = stat.timestamp_ms < 100000000000LL ? stat.timestamp_ms * 1000 : stat.timestamp_ms;
            oi_history_.seed(bucket, stat.oi_close, now);
        }
        return;
    }
    const bool minute = snapshot_.apply(stat, now);
    hour_snapshot_.apply(stat, now);
    if (minute) {
        oi_history_.add(stat.open_interest_ms, stat.open_interest_usd, now);
        double change = 0;
        if (history_attempts_ < 3 && (!history_requested_ms_ || now - history_requested_ms_ >= 30000) &&
            !oi_history_.change(4*3600000LL, now, change)) {
            // First live receipt proves the socket is open. Request a bounded,
            // venue-keyed seed using the same endpoint as the OI chart.
            subscribed_streams_->request_historical_oi(pair_, 60, 246);
            history_requested_ms_ = now;
            ++history_attempts_;
        }
    }
}

StatFreshness::Field StatsWidget::stat_field(int64_t clock_ms, int64_t stale_after_ms) const {
    if (!snapshot_.received_ms) return {StatFreshness::State::Unavailable, 0};
    // Legacy messages have no field clocks; age the last received value instead.
    return StatFreshness::field(true,
        StatFreshness::stated(snapshot_.stat) ? clock_ms : snapshot_.received_ms,
        static_cast<int64_t>(Entitlements::wall_now_ms()), stale_after_ms);
}

const PositioningState* StatsWidget::positioning() const {
    if (!ctx_.analytics) return nullptr;
    const auto* pos = ctx_.analytics_mgr().get_positioning(Terminal::pair_key(pair_.exchange, pair_.symbol));
    const auto now = static_cast<int64_t>(Entitlements::wall_now_ms());
    return pos && pos->timestamp_ms > 0 && pos->timestamp_ms <= now &&
        now - pos->timestamp_ms <= 120000 ? pos : nullptr;
}

void StatsWidget::render() {
    if (!is_open) { update_market_context(false); return; }

    ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 360.0f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(500.0f, std::min(960.0f, ImGui::GetIO().DisplaySize.y - 100.0f)), ImGuiCond_FirstUseEver);
    place_new_window();
    const bool vis = ImGui::Begin(title_.c_str(), &is_open, ImGuiWindowFlags_NoScrollbar);
    if (!vis) { update_market_context(false); ImGui::End(); ImGui::PopStyleVar(2); return; }

    // A live ticker and cached live snapshots must never masquerade as replay.
    const bool live = !ctx_.stream_mgr().is_replay_mode();
    update_market_context(live && more_details_ && Entitlements::is_pro());
    render_status_strip(live);
    if (!live) {
        note("Stats summary is unavailable in replay. Open Flow & Positioning for recorded window statistics.");
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }
    render_header_price();

    ImGui::BeginChild("##stats_body", ImVec2(0.0f, 0.0f), false);
    if (render_section_header(SEC_OVERVIEW, "Live overview", false, "Latest")) {
        ImGui::PushID("live_overview");
        render_positioning(true);
        render_order_flow(true);
        render_liq_risk(true);
        ImGui::PopID();
    }
    ImGui::SetCursorPosX(20);
    if (ImGui::Button("Open Data: history & research", ImVec2(ImGui::GetContentRegionAvail().x - 20, 32)))
        Menu::request_market_data(pair_);
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::SetCursorPosX(20);
    ImGui::Checkbox("More details", &more_details_);
    if (more_details_) {
        render_market_context();
        if (render_section_header(SEC_POSITIONING, "Positioning details", false, "Latest"))
            render_positioning();
        if (render_section_header(SEC_ORDER_FLOW, "Order flow details", true, "UTC minute"))
            render_order_flow();
        render_liq_risk();
        render_price_ratio();
    }
    ImGui::Dummy(ImVec2(0, 12));
    ImGui::EndChild();

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void StatsWidget::update_market_context(bool enabled) {
    update_ratio_history(enabled && pair_.exchange == "binancef" && section_open_[SEC_PRICE_RATIO]);
#ifdef __EMSCRIPTEN__
    if (!enabled) {
        terminal_request_cancel(context_id_);
        market_context_ = {};
        context_requested_ms_ = 0;
        return;
    }
    if (char* response = terminal_request_take(context_id_)) {
        context_locked_ = std::string_view(response) == "locked";
        market_context_ = MarketContext::parse(response, pair_.exchange, pair_.symbol);
        std::free(response);
    }
    const auto now = static_cast<int64_t>(Entitlements::wall_now_ms());
    if (!context_requested_ms_ || now - context_requested_ms_ >= 30000) {
        terminal_request_start(context_id_, pair_.exchange.c_str(), pair_.symbol.c_str(), 0);
        context_requested_ms_ = now;
    }
#else
    (void)enabled;
#endif
}

void StatsWidget::update_ratio_history(bool enabled) {
#ifdef __EMSCRIPTEN__
    if (!enabled) {
        terminal_request_cancel(context_id_ + 1);
        ratio_history_ = {}; ratio_requested_ms_ = 0; ratio_loaded_ = false;
        return;
    }
    if (char* response = terminal_request_take(context_id_ + 1)) {
        ratio_history_ = StatsRatioHistory::parse(response, pair_.exchange, pair_.symbol);
        if (ratio_history_.available() && ratio_history_.binance == ratio_archive_) ratio_history_ = {};
        ratio_loaded_ = true;
        std::free(response);
    }
    const auto now = static_cast<int64_t>(Entitlements::wall_now_ms());
    if (!ratio_requested_ms_ || now - ratio_requested_ms_ >= (ratio_archive_ ? 300000 : 60000)) {
        terminal_request_start(context_id_ + 1, pair_.exchange.c_str(), pair_.symbol.c_str(), ratio_archive_ ? 1 : 2);
        ratio_requested_ms_ = now;
    }
#else
    (void)enabled;
#endif
}

void StatsWidget::render_price_ratio() {
    if (!render_section_header(SEC_PRICE_RATIO, "Price & OI ratio", true, ratio_archive_ ? "Archive" : "Binance · 5m")) return;
    if (!Entitlements::is_pro()) { note("Recorded price and market-cap/OI charts are included with Pro and Research."); return; }
    if (pair_.exchange != "binancef") { note("Recorded market-cap/OI history currently covers Binance Futures."); return; }
    const float width = ImGui::GetContentRegionAvail().x - 40.0f;
    ImGui::SetCursorPosX(20);
    ImGui::SetNextItemWidth(width);
    int source_kind = ratio_archive_ ? 1 : 0;
    if (ImGui::Combo("##ratio_source", &source_kind, "Recent Binance (5m)\0Research archive\0")) {
        ratio_archive_ = source_kind == 1;
#ifdef __EMSCRIPTEN__
        terminal_request_cancel(context_id_ + 1);
#endif
        ratio_history_ = {}; ratio_requested_ms_ = 0; ratio_loaded_ = false;
    }
    ImGui::SetCursorPosX(20);
    ImGui::SetNextItemWidth(width);
    int direction = ratio_inverse_ ? 0 : 1;
    if (ImGui::Combo("##ratio_direction", &direction, "Market cap / OI (x)\0OI / market cap (%)\0")) ratio_inverse_ = direction == 0;
    if (ImGui::IsItemHovered()) {
        Theme::begin_tooltip();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(360.0f, ImGui::GetIO().DisplaySize.x - 32.0f));
        ImGui::TextWrapped("Market cap uses circulating supply. The recorded OI/market-cap ratio compares contract OI with supply; market cap/OI is its inverse. It covers this Binance contract, not all exchanges. Recent Binance uses Binance OI snapshots and its CMC circulating supply, paired with completed last-price candles. The research archive uses a separate daily supply provider and can differ; it is not current. The two chart axes have different units: line crossings are not signals.");
        ImGui::PopTextWrapPos(); Theme::end_tooltip();
    }
    ImGui::Dummy(ImVec2(0, 12));
    const auto now = static_cast<int64_t>(Entitlements::wall_now_ms());
    // Current original research deliberately withholds this ratio. Use only a
    // recent exchange snapshot, explicitly labelled, never a substituted archive.
    if (!ratio_archive_ && ratio_history_.binance) {
        const auto& h = ratio_history_;
        size_t latest = h.times.size();
        while (latest > 0 && (!std::isfinite(h.oi_share[latest - 1]) || !std::isfinite(h.prices[latest - 1]))) --latest;
        if (latest > 0) {
            const auto at = static_cast<int64_t>(h.times[latest - 1] * 1000);
            const bool fresh = at <= now && now - at <= 15 * 60000;
            char value[40] = "--", age[24], detail[96];
            const double share = h.oi_share[latest - 1];
            if (fresh) snprintf(value, sizeof(value), ratio_inverse_ ? "%.2fx" : "%.2f%%", ratio_inverse_ ? 1.0 / share : 100 * share);
            StatFreshness::age_label(now - at, age, sizeof(age));
            snprintf(detail, sizeof(detail), "Binance 5m snapshot · %s ago%s", age, fresh ? "" : " · stale");
            metric_row(Row{.label = "Latest snapshot", .value = value, .detail = detail,
                .help = "Binance contract OI divided by Binance's CMC circulating supply. A five-minute observation, not a streaming live ratio."});
        }
    }
    if (!ratio_history_.available()) {
        note(ratio_loaded_ ? "Selected ratio source unavailable. Retry shortly or select the research archive above." : "Loading price and OI/market-cap history...");
        return;
    }
    const auto& h = ratio_history_;
    char through[64], stamp[150];
    DisplayTimeZone::instance().format(h.to, TimeZoneFormat::DateTimeMinutes, through, sizeof(through));
    snprintf(stamp, sizeof(stamp), "%s through %s · %s", ratio_archive_ ? "Archive" : "Binance", through, h.step == 300000 ? "5m" : "1h");
    note(stamp);
    note(ratio_archive_ ? "Historical research archive; not current Binance data." : "Price: completed candle close. Ratio: Binance OI / CMC supply.");
    ImGui::SetCursorPosX(20);
    for (const int days : {1, 3, 7}) {
        const int hours = days == 1 ? 3 : days == 3 ? 12 : 24;
        if (days != 1) ImGui::SameLine(0, 8);
        char label[16]; snprintf(label, sizeof(label), ratio_archive_ ? "%dd" : "%dh", ratio_archive_ ? days : hours);
        const bool active = ratio_archive_ ? ratio_days_ == days : ratio_hours_ == hours;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, Tokens::BD2);
        if (ImGui::Button(label, ImVec2(42, 26))) { if (ratio_archive_) ratio_days_ = days; else ratio_hours_ = hours; }
        if (active) ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, 10));
    const auto begin = std::lower_bound(h.times.begin(), h.times.end(), double(h.to - (ratio_archive_ ? ratio_days_ * 86400000LL : ratio_hours_ * 3600000LL)) / 1000.0);
    const size_t first = static_cast<size_t>(begin - h.times.begin());
    const int count = static_cast<int>(h.times.size() - first);
    std::vector<double> ratios(h.oi_share.size());
    for (size_t i = first; i < ratios.size(); ++i) ratios[i] = ratio_inverse_ ? 1.0 / h.oi_share[i] : h.oi_share[i] * 100;
    size_t selected = h.times.size() - 1;
    ImGui::SetCursorPosX(20);
    int axis_days = ratio_archive_ ? ratio_days_ : 1;
    if (ImPlot::BeginPlot("##stats_price_ratio", ImVec2(width, 210), ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMenus | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_Lock | ImPlotAxisFlags_NoLabel);
        ImPlot::SetupAxisLimits(ImAxis_X1, h.times[first] - (count == 1 ? h.step / 1000.0 : 0), h.times.back(), ImGuiCond_Always);
        const double span = h.times.back() - h.times[first];
        const double ticks[] = {h.times[first] + span * 0.12, h.times[first] + span * (width < 350 ? 0.88 : 0.5), h.times[first] + span * 0.88};
        ImPlot::SetupAxisFormat(ImAxis_X1, [](double t, char* buf, int size, void* data) {
            DisplayTimeZone::instance().format(static_cast<int64_t>(t * 1000), *static_cast<const int*>(data) == 1 ? TimeZoneFormat::TimeMinutes : TimeZoneFormat::MonthDay, buf, size);
            return static_cast<int>(std::char_traits<char>::length(buf));
        }, &axis_days);
        ImPlot::SetupAxisTicks(ImAxis_X1, ticks, width < 350 ? 2 : 3);
        ImPlot::SetupAxis(ImAxis_Y1, nullptr, ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoLabel);
        ImPlot::SetupAxisFormat(ImAxis_Y1, "%.6g");
        ImPlot::SetupAxis(ImAxis_Y2, nullptr, ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_Opposite | ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoLabel);
        ImPlot::SetupAxisFormat(ImAxis_Y2, ratio_inverse_ ? "%.3fx" : "%.2f%%");
        ImPlotSpec price_style; price_style.LineColor = Tokens::WARN; price_style.LineWeight = 2.0f;
        ImPlot::PlotLine("Price (USDT, left)", h.times.data() + first, h.prices.data() + first, count, price_style);
        ImPlot::SetAxis(ImAxis_Y2);
        ImPlotSpec ratio_style; ratio_style.LineColor = Tokens::BRAND; ratio_style.LineWeight = 2.0f;
        ImPlot::PlotLine("Ratio (right)", h.times.data() + first, ratios.data() + first, count, ratio_style);
        if (ImPlot::IsPlotHovered()) {
            const double x = ImPlot::GetPlotMousePos().x;
            selected = std::clamp<size_t>(static_cast<size_t>(std::max(0.0, std::round((x - h.times.front()) * 1000.0 / h.step))), first, h.times.size() - 1);
            const double cursor[] = {h.times[selected]};
            ImPlotSpec marker; marker.LineColor = Tokens::TX3;
            ImPlot::PlotInfLines("##inspect", cursor, 1, marker);
        }
        ImPlot::EndPlot();
    }
    char price[48] = "--", ratio[48] = "--", inspected[64];
    if (std::isfinite(h.prices[selected])) snprintf(price, sizeof(price), "%.8g USDT", h.prices[selected]);
    if (std::isfinite(ratios[selected])) snprintf(ratio, sizeof(ratio), ratio_inverse_ ? "%.2fx" : "%.2f%%", ratios[selected]);
    DisplayTimeZone::instance().format(static_cast<int64_t>(h.times[selected] * 1000), TimeZoneFormat::DateTimeMinutes, inspected, sizeof(inspected));
    summary_pair(Row{.label = "Price · left", .value = price, .value_col = Tokens::WARN},
        Row{.label = ratio_inverse_ ? "Mcap/OI · right" : "OI/Mcap · right", .value = ratio, .value_col = Tokens::BRAND});
    note(inspected);
    note("Hover to inspect a recorded close. Gaps mean missing data.");
    ImGui::Dummy(ImVec2(0, 12));
}

void StatsWidget::render_market_context() {
    if (!render_section_header(SEC_CONTEXT, "Market context", true, "Research")) return;
    if (!Entitlements::is_pro() || context_locked_) {
        note("Regime, forced-flow candidates and cascade tiers are included with Pro and Research. Sign in with an active plan.");
        return;
    }
    if (pair_.exchange != "binancef" || market_context_.status == "unsupported") {
        note("Radar research currently covers Binance Futures. This venue's live statistics are in the overview.");
        return;
    }
    const auto now = static_cast<int64_t>(Entitlements::wall_now_ms());
    const auto& rows = market_context_.items;
    const float width = ImGui::GetContentRegionAvail().x;
    constexpr float gutter = 20.0f, inset = 14.0f;
    auto tint = [](const MarketContext::Item& r) {
        return r.tone == "up" ? Tokens::UP : r.tone == "down" ? Tokens::DOWN : r.tone == "warn" ? Tokens::WARN : Tokens::TX1;
    };
    auto age = [&](const MarketContext::Item& r, const char* source, char* out, size_t size) {
        char elapsed[20]; StatFreshness::age_label(now - r.at, elapsed, sizeof(elapsed));
        snprintf(out, size, "%s · %s ago", source, elapsed);
    };
    // One measured block per reading. A stable inset, wrapped text and explicit
    // gaps separate the conclusion, explanation and source without shrinking type.
    auto block = [&](const char* id, const char* title, const char* stamp, const char* detail,
                     const ImVec4& tone, const char* help,
                     const MarketContext::Item* reading = nullptr) {
        const ImVec2 org = ImGui::GetCursorScreenPos();
        const float x = org.x + gutter + inset, wrap = width - 2 * (gutter + inset);
        float y = org.y + 14.0f;
        auto* dl = ImGui::GetWindowDrawList();
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        auto line = [&](const char* value, ImFont* font, const ImVec4& color, float gap) {
            ImGui::PushFont(font);
            dl->AddText(font, ImGui::GetFontSize(), ImVec2(x, y), col(color), value, nullptr, wrap);
            y += font->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, wrap, value).y + gap;
            ImGui::PopFont();
        };
        line(title, Fonts::heading(), tone, 8.0f);
        if (detail && *detail) line(detail, Fonts::ui(), Tokens::TX2, 8.0f);
        if (stamp && *stamp) line(stamp, Fonts::mono_sm(), Tokens::TX3, 0.0f);
        if (reading) {
            y += 12.0f;
            dl->AddLine(ImVec2(x, y), ImVec2(x + wrap, y), col(Tokens::BD1));
            y += 12.0f;
            if (reading->current(now)) {
                // Keep each label/value together when a narrow card wraps.
                ImGui::PushFont(Fonts::mono_sm());
                const std::string_view values(reading->value), separator(" · ");
                float offset = 0.0f, line_height = ImGui::GetFontSize();
                size_t from = 0, metric = 0;
                while (from < values.size()) {
                    const auto found = values.find(separator, from);
                    const size_t end = found == std::string_view::npos ? values.size() : found;
                    const char* first = values.data() + from;
                    const char* last = values.data() + end;
                    const float part_width = ImGui::CalcTextSize(first, last).x;
                    if (offset > 0 && offset + part_width > wrap) {
                        offset = 0; y += line_height + 6; line_height = ImGui::GetFontSize();
                    }
                    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x + offset, y),
                                col(metric < 2 && std::isfinite(reading->metrics[metric]) && reading->metrics[metric] != 0
                                    ? (reading->metrics[metric] > 0 ? Tokens::UP : Tokens::DOWN) : Tokens::TX2),
                                first, last, wrap - offset);
                    line_height = std::max(line_height, ImGui::GetFont()->CalcTextSizeA(
                        ImGui::GetFontSize(), FLT_MAX, wrap - offset, first, last).y);
                    offset += part_width + 16;
                    if (found == std::string_view::npos) break;
                    from = end + separator.size();
                    metric = std::min(metric + 1, size_t(2));
                }
                y += line_height + 8;
                ImGui::PopFont();
                char clock[80]; age(*reading, "1h readings", clock, sizeof(clock));
                line(clock, Fonts::mono_sm(), Tokens::TX3, 0.0f);
            } else line("Current research readings unavailable", Fonts::ui(), Tokens::TX3, 0.0f);
        }
        y += 14.0f;
        dl->ChannelsSetCurrent(0);
        dl->AddRectFilled(ImVec2(org.x + gutter, org.y), ImVec2(org.x + width - gutter, y), col(Tokens::ELEV));
        dl->AddLine(ImVec2(org.x + gutter, org.y + 14), ImVec2(org.x + gutter, y - 14), col(tone), 2.0f);
        dl->ChannelsMerge();
        ImGui::SetCursorScreenPos(ImVec2(org.x + gutter, org.y));
        ImGui::PushID(id);
        ImGui::InvisibleButton("context_reading", ImVec2(width - 2 * gutter, y - org.y));
        if (ImGui::IsItemHovered()) {
            Theme::begin_tooltip();
            // Auto-sized tooltips need an explicit wrap width. Wrap-at-window
            // uses their initial zero content width and can stack each character.
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(360.0f, ImGui::GetIO().DisplaySize.x - 32.0f));
            ImGui::TextWrapped("%s", help);
            if (reading && reading->current(now)) {
                ImGui::Spacing(); ImGui::TextWrapped("%s", reading->help.c_str());
            }
            ImGui::PopTextWrapPos();
            Theme::end_tooltip();
        }
        ImGui::PopID();
        ImGui::SetCursorScreenPos(ImVec2(org.x, y));
        ImGui::Dummy(ImVec2(width, 12.0f));
    };
    const auto& regime = rows[MarketContext::Regime];
    char regime_age[80] = {};
    if (regime.current(now)) age(regime, "4h background / hourly snapshot", regime_age, sizeof(regime_age));
    block("regime", regime.current(now) ? regime.value.c_str() : regime.at ? "Regime snapshot expired" : "Regime unavailable",
          regime_age, regime.current(now) ? regime.detail.c_str() : nullptr,
          regime.current(now) ? tint(regime) : Tokens::TX3,
          regime.current(now) ? regime.help.c_str() : "No current activity and structure snapshot for this market.", &rows[MarketContext::Readings]);

    const auto& flow = rows[MarketContext::Flow];
    char flow_age[80] = {};
    if (flow.current(now)) age(flow, "Recent event / detected", flow_age, sizeof(flow_age));
    block("flow", flow.current(now) ? flow.value.c_str() : flow.at ? "Forced-flow detector expired" : "Forced-flow detector unavailable",
          flow_age, flow.current(now) ? flow.detail.c_str() : nullptr,
          flow.current(now) ? tint(flow) : Tokens::TX3,
          flow.current(now) ? flow.help.c_str() : "No current forced-flow detector reading for this market.");

    const auto& cascade = rows[MarketContext::Cascade];
    char risk_title[100], window[180] = {};
    if (cascade.current(now)) {
        snprintf(risk_title, sizeof(risk_title), "Cascade risk · %s", cascade.value.c_str());
        char start[64], end[64]; auto& zone = DisplayTimeZone::instance();
        zone.format(cascade.at, TimeZoneFormat::TimeMinutes, start, sizeof(start));
        zone.format(cascade.until, TimeZoneFormat::TimeMinutes, end, sizeof(end));
        snprintf(window, sizeof(window), "Downside · %s to %s · research preview", start, end);
    } else snprintf(risk_title, sizeof(risk_title), "%s", cascade.at ? "Cascade forecast expired" : "Cascade forecast unavailable");
    block("cascade", risk_title, window, cascade.current(now) ? "Relative tier, not a probability" : nullptr,
          cascade.current(now) ? tint(cascade) : Tokens::TX3,
          cascade.current(now) ? cascade.help.c_str() : "No current admitted forecast for this market. Forecasts require supported market coverage and expire at the end of their published hour.");

    float chip_x = gutter;
    bool has_flags = false;
    for (const auto id : {MarketContext::OIAhead, MarketContext::Awake}) {
        const auto& r = rows[id];
        if (!r.current(now)) continue;
        ImGui::PushFont(Fonts::mono_sm());
        const auto size = ImGui::CalcTextSize(r.value.c_str());
        ImGui::PopFont();
        if (has_flags && chip_x + size.x + 12.0f > width - gutter) {
            chip_x = gutter;
            ImGui::Dummy(ImVec2(0, 8));
        } else if (has_flags) ImGui::SameLine(chip_x, 0.0f);
        ImGui::SetCursorPosX(chip_x);
        draw_tag(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), r.value.c_str(),
                 0, col(Tokens::BD2), col(Tokens::TX2), 6.0f);
        ImGui::PushID("context_flag"); ImGui::PushID(static_cast<int>(id));
        ImGui::InvisibleButton("flag", ImVec2(size.x + 12.0f, size.y + 3.0f));
        ImGui::PopID(); ImGui::PopID();
        if (ImGui::IsItemHovered()) {
            char at[64]; DisplayTimeZone::instance().format(r.at, TimeZoneFormat::DateTimeMinutes, at, sizeof(at));
            Theme::tooltip("%s Recorded %s", r.help.c_str(), at);
        }
        chip_x += size.x + 22.0f;
        has_flags = true;
    }
    ImGui::Dummy(ImVec2(0, 20));
}

void StatsWidget::render_status_strip(bool live) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float ww = ImGui::GetContentRegionAvail().x;
    const float H = 32.0f, PADX = 20.0f;
    const float cy = org.y + H * 0.5f;

    dl->AddCircleFilled(ImVec2(org.x + PADX + 3.0f, cy), 3.0f,
                        col(live && snapshot_.active(static_cast<int64_t>(Entitlements::wall_now_ms())) ? Tokens::UP : Tokens::WARN));

    const bool active = live && snapshot_.active(static_cast<int64_t>(Entitlements::wall_now_ms()));
    const char* t = live ? (active ? "Live" : "Waiting for data") : "Replay";
    ImGui::PushFont(Fonts::ui());
    dl->AddText(ImVec2(org.x + PADX + 13.0f, cy - ImGui::GetFontSize() * 0.5f),
                col(Tokens::TX2), t);
    ImGui::PopFont();

    float tier_width = 0.0f;
    if (Entitlements::hosted()) {
        ImGui::PushFont(Fonts::mono_sm());
        tier_width = ImGui::CalcTextSize(Entitlements::tier_label()).x + 8.0f;
        ImGui::PopFont();
    }
    ImGui::PushFont(Fonts::ui());
    const float help_x = org.x + ww - PADX - 48.0f - (tier_width ? tier_width + 12.0f : 0.0f);
    ImGui::SetCursorScreenPos(ImVec2(help_x, org.y + 5.0f));
    if (ImGui::Button("Help##stats_help", ImVec2(48, 22))) ImGui::OpenPopup("##stats_guide");
    if (ImGui::IsItemHovered()) Theme::tooltip("How to read Stats");
    ImGui::PopFont();
    ImGui::SetNextWindowSize(ImVec2(350.0f, 0.0f), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 10.0f));
    if (ImGui::BeginPopup("##stats_guide")) {
        ImGui::TextUnformatted("Reading Stats");
        ImGui::TextWrapped("-- unavailable / $0 fresh window, no reports");
        if (ImGui::TreeNode("Liquidations")) {
            ImGui::TextWrapped("Shorts: forced buys / longs: forced sells");
            ImGui::TextWrapped("Compare price, tape and the stated window");
            ImGui::TextWrapped("Reports alone do not confirm a squeeze");
            ImGui::TextWrapped("Coverage is partial; Binance is sampled");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Positioning")) {
            ImGui::TextWrapped("OI: outstanding contracts at mark price");
            ImGui::TextWrapped("Ratios count accounts, not position size");
            ImGui::TextWrapped("No accumulation claim");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Flow")) {
            ImGui::TextWrapped("CVD: taker buys minus sells this minute");
            ImGui::TextWrapped("VPIN: imbalance, not direction");
            ImGui::TextWrapped("Check the last bucket's age");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Iceberg candidates")) {
            ImGui::TextWrapped("Repeated taker-side trade clusters");
            ImGui::TextWrapped("Heuristic, not confirmed hidden orders");
            ImGui::TextWrapped("Bias: observed buy/sell notional");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Research")) {
            ImGui::TextWrapped("Each published source has its own clock");
            ImGui::TextWrapped("A risk tier is not a probability");
            ImGui::TreePop();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::SetCursorScreenPos(org);

    if (Entitlements::hosted()) {
        const char* tier = Entitlements::tier_label();
        draw_tag(dl, ImVec2(org.x + ww - PADX - tier_width, cy - 7.0f), tier,
                 0, col(Tokens::BD2), col(Tokens::TX2), 4.0f);
    }

    dl->AddLine(ImVec2(org.x, org.y + H), ImVec2(org.x + ww, org.y + H), col(Tokens::BD1), 1.0f);
    ImGui::Dummy(ImVec2(ww, H));
}

void StatsWidget::render_header_price() {
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float ww = ImGui::GetContentRegionAvail().x;
    constexpr float pad = 20.0f;
    const auto* meta = SymbolRegistry::instance().get(pair_.exchange, pair_.symbol);
    const std::string sym = meta ? meta->display_name() : upper(pair_.symbol);
    const std::string& asset = meta && !meta->base_asset.empty() ? meta->base_asset : pair_.symbol;
    const char* venue = pair_.exchange == "binancef" ? "Binance Futures" : pair_.exchange == "bybit" ? "Bybit" :
                        pair_.exchange == "hl" ? "Hyperliquid" : pair_.exchange.c_str();
    auto& logos = LogoManager::instance();
    const ImVec2 coin_pos(org.x + pad, org.y + 16);
    logos.draw_coin(dl, asset, coin_pos, 32.0f);
    const ImVec2 venue_pos(org.x + ww - pad - 20, org.y + 22);
    if (const auto logo = logos.exchange(pair_.exchange))
        dl->AddImage(logo, venue_pos, ImVec2(venue_pos.x + 20, venue_pos.y + 20));
    else draw_logo_monogram(dl, venue, venue_pos, 20.0f);
    ImGui::SetCursorScreenPos(venue_pos);
    ImGui::InvisibleButton("stats_venue_logo", ImVec2(20, 20));
    if (ImGui::IsItemHovered()) Theme::tooltip("%s", venue);

    ImGui::PushFont(Fonts::heading());
    const float title_wrap = ww - 2 * pad - 42 - 32;
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(coin_pos.x + 42, org.y + 22),
                col(Tokens::TX1), sym.c_str(), nullptr, title_wrap);
    const float title_height = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, title_wrap, sym.c_str()).y;
    ImGui::PopFont();
    float y = org.y + 16 + std::max(32.0f, title_height + 6) + 12;

    const auto now = static_cast<int64_t>(Entitlements::wall_now_ms());
    const auto* ticker = ctx_.tickers ? ctx_.ticker_mgr().get(pair_.exchange, pair_.symbol) : nullptr;
    const bool ticker_fresh = ticker && ticker->event_time_ms > 0 &&
        ticker->event_time_ms <= now && now - ticker->event_time_ms <= 60000;
    const auto mark_state = stat_field(snapshot_.stat.mark_price_ms, 30000);
    const bool mark_known = snapshot_.stat.mark_price > 0 &&
        mark_state.state != StatFreshness::State::Unavailable;
    const bool last = ticker_fresh && ticker->last_price > 0;
    char price[40] = "--", change[40] = "24h --";
    if (last || mark_known)
        snprintf(price, sizeof(price), fmt_.price_fmt, last ? ticker->last_price : snapshot_.stat.mark_price);
    if (ticker_fresh) snprintf(change, sizeof(change), "24h %+.2f%%", ticker->change_pct_24h);
    ImGui::PushFont(Fonts::mono_lg());
    dl->AddText(ImVec2(org.x + pad, y), col(Tokens::TX1), price);
    const float pw = ImGui::CalcTextSize(price).x, ph = ImGui::GetFontSize();
    ImGui::PopFont();
    ImGui::PushFont(Fonts::mono_sm());
    const bool stack_change = pw + 16 + ImGui::CalcTextSize(change).x > ww - 2 * pad;
    dl->AddText(ImVec2(org.x + pad + (stack_change ? 0 : pw + 16), y + (stack_change ? ph + 6 : 7)),
        col(ticker_fresh ? (ticker->change_pct_24h >= 0 ? Tokens::UP : Tokens::DOWN) : Tokens::TX3), change);
    y += ph + 12 + (stack_change ? ImGui::GetFontSize() + 6 : 0);
    ImGui::PopFont();

    char volume[32] = "--";
    if (ticker_fresh) fmt_usd(ticker->volume_quote, volume, sizeof(volume));
    ImGui::PushFont(Fonts::ui());
    char volume_label[48]; snprintf(volume_label, sizeof(volume_label), "24h volume  %s", volume);
    dl->AddText(ImVec2(org.x + pad, y), col(Tokens::TX3), volume_label);
    const char* source = last ? "Last trade" : mark_known ?
        (mark_state.state == StatFreshness::State::Stale ? "Stale mark" : "Mark price") : "No price";
    const bool stack_source = ImGui::CalcTextSize(volume_label).x + 16 + ImGui::CalcTextSize(source).x > ww - 2 * pad;
    if (stack_source) y += ImGui::GetFontSize() + 6;
    dl->AddText(ImVec2(stack_source ? org.x + pad : org.x + ww - pad - ImGui::CalcTextSize(source).x, y), col(Tokens::TX3), source);
    y += ImGui::GetFontSize() + 18;
    ImGui::PopFont();
    dl->AddLine(ImVec2(org.x, y), ImVec2(org.x + ww, y), col(Tokens::BD1), 1.0f);
    ImGui::SetCursorScreenPos(org);
    ImGui::Dummy(ImVec2(ww, y - org.y));
}

bool StatsWidget::render_section_header(Section section, const char* name, bool pro_section, const char* scope) {
    ImGui::PushID(static_cast<int>(section));
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = 48.0f;
    const bool locked = pro_section && !Entitlements::is_pro();
    ImGui::InvisibleButton("section", ImVec2(width - (locked ? 68 : 0), height));
    if (ImGui::IsItemClicked()) section_open_[section] = !section_open_[section];
    dl->AddLine(ImVec2(org.x + 20, org.y), ImVec2(org.x + width - 20, org.y), col(Tokens::BD1));
    ImGui::PushFont(Fonts::ui_semibold());
    dl->AddText(ImVec2(org.x + 20, org.y + 17), col(Tokens::TX1), name);
    ImGui::PopFont();
    ImGui::PushFont(Fonts::ui());
    const char* hint = locked ? "Unlock" : scope;
    const float hint_w = ImGui::CalcTextSize(hint).x;
    dl->AddText(ImVec2(org.x + width - 36 - hint_w, org.y + 17), col(Tokens::TX3), hint);
    ImGui::PopFont();
    const float x = org.x + width - 24, y = org.y + 24;
    if (section_open_[section]) dl->AddTriangleFilled(ImVec2(x-3,y-2), ImVec2(x+3,y-2), ImVec2(x,y+2), col(Tokens::TX3));
    else dl->AddTriangleFilled(ImVec2(x-2,y-3), ImVec2(x-2,y+3), ImVec2(x+2,y), col(Tokens::TX3));
    if (locked) {
        ImGui::SetCursorScreenPos(ImVec2(org.x + width - 68, org.y));
        ImGui::InvisibleButton("unlock", ImVec2(68, height));
        if (ImGui::IsItemClicked()) ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Generic);
    }
    ImGui::SetCursorScreenPos(ImVec2(org.x, org.y + height));
    ImGui::PopID();
    return section_open_[section];
}

// ── Section-body primitives (step 2) ────────────────────────────────────────
namespace {

void meter_track(ImDrawList* dl, float x, float y, float w, ImU32 c) {
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + 4.0f), c, 1.0f);
}

// Proportional two-sided: left = a (up), right = 1-a (down), 1px gap.
void meter_proportional(ImDrawList* dl, float x, float y, float w, float a,
                        ImU32 c_up, ImU32 c_down) {
    a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
    const float lw = (w - 1.0f) * a;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + lw, y + 4.0f), c_up, 1.0f);
    dl->AddRectFilled(ImVec2(x + lw + 1.0f, y), ImVec2(x + w, y + 4.0f), c_down, 1.0f);
}

// Directional center-zero: v in [-1,1]; fill from center, up right / down left.
void meter_directional(ImDrawList* dl, float x, float y, float w, float v,
                       ImU32 c_up, ImU32 c_down, ImU32 track, ImU32 tick) {
    v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    meter_track(dl, x, y, w, track);
    const float cxp = x + w * 0.5f;
    if (v >= 0.0f) dl->AddRectFilled(ImVec2(cxp, y), ImVec2(cxp + (w * 0.5f) * v, y + 4.0f), c_up, 1.0f);
    else           dl->AddRectFilled(ImVec2(cxp + (w * 0.5f) * v, y), ImVec2(cxp, y + 4.0f), c_down, 1.0f);
    dl->AddLine(ImVec2(cxp, y - 2.0f), ImVec2(cxp, y + 6.0f), tick, 1.0f);
}

// Fill 0..1.
void meter_fill(ImDrawList* dl, float x, float y, float w, float v, ImU32 track, ImU32 fill) {
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    meter_track(dl, x, y, w, track);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w * v, y + 4.0f), fill, 1.0f);
}

// Amber regime chip; level 0 normal / 1 elevated / 2 high / 3 critical.
void draw_regime_chip(ImDrawList* dl, ImVec2 pos, const char* txt, int level) {
    ImGui::PushFont(Fonts::mono_sm());
    const ImVec2 ts = ImGui::CalcTextSize(txt);
    const ImVec2 b(pos.x + ts.x + 12.0f, pos.y + ts.y + 3.0f);
    ImU32 border = col(Tokens::BD2), bg = 0, fg = col(Tokens::TX3);
    if (level == 1)      { border = col(Tokens::WARN); fg = col(Tokens::WARN); }
    else if (level == 2) { border = col(Tokens::WARN); bg = col(Tokens::WARN_SOFT); fg = col(Tokens::WARN); }
    else if (level >= 3) { border = col(Tokens::WARN); bg = col(Tokens::WARN); fg = col(Tokens::BRAND_INK); }
    if (bg) dl->AddRectFilled(pos, b, bg, 2.0f);
    dl->AddRect(pos, b, border, 2.0f, 0, 1.0f);
    dl->AddText(ImVec2(pos.x + 6.0f, pos.y + 1.5f), fg, txt);
    ImGui::PopFont();
}

// Tiny padlock glyph (~9px) at top-left p: body + shackle.
void draw_lock(ImDrawList* dl, ImVec2 p, ImU32 c) {
    dl->AddRectFilled(ImVec2(p.x, p.y + 4.0f), ImVec2(p.x + 8.0f, p.y + 10.0f), c, 1.0f);
    dl->AddRect(ImVec2(p.x + 1.5f, p.y), ImVec2(p.x + 6.5f, p.y + 5.0f), c, 0.0f, 0, 1.2f);
}

void fmt_compact(double v, char* buf, size_t n) {
    const double a = v < 0.0 ? -v : v;
    if (a >= 1e9)      snprintf(buf, n, "%.2fB", v / 1e9);
    else if (a >= 1e6) snprintf(buf, n, "%.2fM", v / 1e6);
    else if (a >= 1e3) snprintf(buf, n, "%.1fK", v / 1e3);
    else               snprintf(buf, n, "%.0f", v);
}

void fmt_usd(double v, char* buf, size_t n) {
    char t[24];
    fmt_compact(v, t, sizeof(t));
    snprintf(buf, n, "$%s", t);
}

}  // namespace

void StatsWidget::metric_row(const Row& r) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float ww = ImGui::GetContentRegionAvail().x;
    const float PADX = 20.0f;
    const bool locked = r.pro && !Entitlements::is_pro();
    const bool has_meter  = r.meter != 0;
    const bool has_detail = r.detail != nullptr;

    const float top = org.y + 10.0f;

    // Label (always crisp - the value proposition stays readable when locked)
    ImGui::PushFont(Fonts::ui());
    const float label_width = ImGui::CalcTextSize(r.label).x;
    dl->AddText(ImVec2(org.x + PADX, top + 1.0f), col(Tokens::TX2), r.label);
    ImGui::PopFont();

    float y2 = top + (r.prominent ? 32.0f : 22.0f);

    if (locked) {
        ImVec4 bar = Tokens::BD2; bar.w = 0.45f;
        dl->AddRectFilled(ImVec2(org.x + ww - PADX - 96.0f, top),
                          ImVec2(org.x + ww - PADX - 16.0f, top + 12.0f), col(bar), 2.0f);
        draw_lock(dl, ImVec2(org.x + ww - PADX - 9.0f, top + 1.0f), col(Tokens::TX3));
        if (has_meter) {
            ImVec4 mt = Tokens::BD2; mt.w = 0.35f;
            dl->AddRectFilled(ImVec2(org.x + PADX, y2), ImVec2(org.x + ww - PADX, y2 + 4.0f), col(mt), 1.0f);
            y2 += 9.0f;
        }
        if (has_detail) y2 += 13.0f;   // keep the row height stable vs the unlocked state
    } else {
        // Value (right-aligned, mono)
        ImGui::PushFont(r.prominent ? Fonts::mono_lg() : Fonts::mono());
        const float vw = ImGui::CalcTextSize(r.value).x;
        float chip_width = 0;
        if (r.chip) {
            ImGui::PushFont(Fonts::mono_sm());
            chip_width = ImGui::CalcTextSize(r.chip).x + 20.0f;
            ImGui::PopFont();
        }
        const bool stacked = label_width + vw + chip_width + 12 > ww - 2 * PADX;
        const float value_y = stacked ? y2 : top;
        const float vx = stacked ? org.x + PADX + chip_width : org.x + ww - PADX - vw;
        dl->AddText(ImVec2(vx, value_y), col(r.value_col), r.value);
        if (stacked) y2 += ImGui::GetFontSize() + 3.0f;
        ImGui::PopFont();

        // Chip before the value
        if (r.chip) {
            ImGui::PushFont(Fonts::mono_sm());
            const float cw = ImGui::CalcTextSize(r.chip).x + 12.0f;
            ImGui::PopFont();
            draw_regime_chip(dl, ImVec2(vx - cw - 8.0f, value_y - 1.0f), r.chip, r.chip_level);
        }

        // Meter, then detail
        if (has_meter) {
            const float mx = org.x + PADX, mw = ww - PADX * 2.0f;
            if (r.meter == 1)
                meter_proportional(dl, mx, y2, mw, r.meter_a, col(r.meter_c1), col(r.meter_c2));
            else if (r.meter == 2)
                meter_directional(dl, mx, y2, mw, r.meter_a, col(Tokens::UP), col(Tokens::DOWN),
                                  col(Tokens::ELEV), col(Tokens::BD2));
            else
                meter_fill(dl, mx, y2, mw, r.meter_a, col(Tokens::ELEV), col(r.meter_c1));
            y2 += 9.0f;
        }
        if (has_detail) {
            ImGui::PushFont(Fonts::mono_sm());
            ImFont* font = ImGui::GetFont();
            const float size = ImGui::GetFontSize(), wrap = ww - 2 * PADX;
            dl->AddText(font, size, ImVec2(org.x + PADX, y2), col(r.detail_col), r.detail, nullptr, wrap);
            y2 += font->CalcTextSizeA(size, FLT_MAX, wrap, r.detail).y;
            ImGui::PopFont();
        }
    }

    // Submit a real item (not just a cursor move) so the scroll child grows its
    // content size / boundaries - otherwise ImGui warns and can't size/scroll.
    ImGui::Dummy(ImVec2(ww, (y2 + 9.0f) - org.y));
    if (r.help && ImGui::IsItemHovered()) Theme::tooltip("%s", r.help);
}

void StatsWidget::summary_pair(const Row& left, const Row& right, bool cards) {
    ImGui::PushID(left.label); // distinct group scope: liquidation, positioning and flow
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, gap = 12.0f;
    const float cell = (width - 40 - gap) * 0.5f;
    float height = cards ? 90.0f : 76.0f;
    auto* dl = ImGui::GetWindowDrawList();
    const Row* rows[] = {&left, &right};
    ImGui::PushFont(Fonts::mono_sm());
    for (const Row* row : rows) {
        if (row->detail) height = std::max(height, 80.0f + ImGui::GetFont()->CalcTextSizeA(
            ImGui::GetFontSize(), FLT_MAX, cell - (cards ? 24 : 0), row->detail).y);
    }
    ImGui::PopFont();
    for (int i = 0; i < 2; ++i) {
        const Row& row = *rows[i];
        const bool locked = row.pro && !Entitlements::is_pro();
        const ImVec2 a(org.x + 20 + i * (cell + gap), org.y);
        const float inset = cards ? 12 : 0;
        if (cards) {
            dl->AddRectFilled(a, ImVec2(a.x + cell, a.y + height - 4), col(Tokens::ELEV));
            dl->AddRect(a, ImVec2(a.x + cell, a.y + height - 4), col(Tokens::BD1));
        }
        ImGui::PushFont(Fonts::ui());
        dl->AddText(ImVec2(a.x + inset, a.y + 12), col(Tokens::TX2), row.label);
        ImGui::PopFont();
        const char* value = locked ? "Locked" : row.value;
        ImFont* font = locked ? Fonts::ui() : (cards ? Fonts::mono_lg() : Fonts::mono_md());
        ImGui::PushFont(font);
        const float base_size = ImGui::GetFontSize();
        const float size = locked ? base_size : (cards ? 25.0f : 20.0f);
        const float value_size = font->CalcTextSizeA(size, FLT_MAX, 0, value).x > cell - 2 * inset ? base_size : size;
        dl->AddText(font, value_size, ImVec2(a.x + inset, a.y + 38), col(locked ? Tokens::TX3 : row.value_col), value);
        ImGui::PopFont();
        if (row.detail && !locked) {
            ImGui::PushFont(Fonts::mono_sm());
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(a.x + inset, a.y + 68),
                col(Tokens::TX3), row.detail, nullptr, cell - 2 * inset);
            ImGui::PopFont();
        }
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        ImGui::InvisibleButton("summary", ImVec2(cell, height));
        if (row.help && ImGui::IsItemHovered()) Theme::tooltip("%s", locked ? "Pro access required." : row.help);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(org.x, org.y + height));
    ImGui::PopID();
}

void StatsWidget::split_bar(float share, const char* left, const char* right) {
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    auto* dl = ImGui::GetWindowDrawList();
    meter_proportional(dl, org.x + 20, org.y, width - 40, share, col(Tokens::UP), col(Tokens::DOWN));
    ImGui::PushFont(Fonts::ui());
    dl->AddText(ImVec2(org.x + 20, org.y + 11), col(Tokens::TX2), left);
    dl->AddText(ImVec2(org.x + width - 20 - ImGui::CalcTextSize(right).x, org.y + 11), col(Tokens::TX2), right);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(width, 34));
}

void StatsWidget::note(const char* text) {
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float ww = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorScreenPos(ImVec2(org.x + 20.0f, org.y + 6.0f));
    ImGui::PushFont(Fonts::ui());
    ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX3);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ww - 40.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(ww - 40.0f, 6.0f));
    ImGui::SetCursorPosX(0.0f);
}

// ── Order flow ────────────────────────────────────────
void StatsWidget::render_order_flow(bool overview) {
    const auto now = static_cast<int64_t>(Entitlements::wall_now_ms());
    const bool flow = volume_snapshot_.active(now);
    const auto& volume = volume_snapshot_.volume;
    char delta[32] = "--", pressure[64] = "No current minute flow";
    const double buy_usd = volume.buy * volume.vwap_buy, sell_usd = volume.sell * volume.vwap_sell;
    const double total = buy_usd + sell_usd;
    const bool split = flow && std::isfinite(total) && total > 0;
    const float buy_share = split ? static_cast<float>(buy_usd / total) : 0.5f;
    if (flow) {
        char amount[24]; fmt_usd(std::abs(volume.cvd), amount, sizeof(amount));
        snprintf(delta, sizeof(delta), "%s%s", volume.cvd > 0 ? "+" : volume.cvd < 0 ? "-" : "", amount);
        snprintf(pressure, sizeof(pressure), "%s", volume.buy + volume.sell > 0 ? "Taker split unavailable" : "No taker volume this minute");
    }
    const bool active = snapshot_.active(now);
    char trades[32] = "--", trade_detail[80] = "Current minute stats unavailable";
    if (active) {
        const auto buys = snapshot_.stat.trade_buy, sells = snapshot_.stat.trade_sell;
        fmt_compact(static_cast<double>(buys + sells), trades, sizeof(trades));
        snprintf(trade_detail, sizeof(trade_detail), "Buy\xc2\xa0%lld / sell\xc2\xa0%lld", static_cast<long long>(buys), static_cast<long long>(sells));
    }
    if (overview) {
        double change=0;
        char oi_change[24]="--";
        const bool known=oi_history_.change(3600000,now,change);
        if(known)snprintf(oi_change,sizeof(oi_change),"%+.1f%%",change*100);
        summary_pair(Row{.label="Taker delta",.pro=true,.value=delta,
            .value_col=flow?(volume.cvd>=0?Tokens::UP:Tokens::DOWN):Tokens::TX3,
            .detail="Current UTC minute",.help="Observed taker buy minus sell USD notional since this UTC minute began. Resets each minute."},
            Row{.label="OI change",.value=oi_change,
                .value_col=known?(change>=0?Tokens::UP:Tokens::DOWN):Tokens::TX3,
                .detail="1h contract change",.help="Contract quantity change using recorded observations approximately one hour apart. Missing or stale anchors stay unavailable; not mark-priced USD change."});
        return;
    }
    summary_pair(Row{.label = "Taker delta", .pro = true, .value = delta,
        .value_col = flow ? (volume.cvd >= 0 ? Tokens::UP : Tokens::DOWN) : Tokens::TX3,
        .help = "CVD: taker buy minus sell USD notional since this UTC minute began. Resets each minute."},
        Row{.label = "Trades", .value = trades, .detail = trade_detail, .help = "Trade counts since this UTC minute began; the split counts trades, not volume."});
    if (Entitlements::is_pro()) {
        if (split) {
            char buy[32], sell[32];
            snprintf(buy, sizeof(buy), "Buy %.0f%%", buy_share * 100);
            snprintf(sell, sizeof(sell), "Sell %.0f%%", (1 - buy_share) * 100);
            split_bar(buy_share, buy, sell);
            if (ImGui::IsItemHovered()) Theme::tooltip("Share of observed taker USD notional in the current UTC minute.");
        } else note(pressure);
    }
    render_icebergs();

    // VPIN - latest finalized volume-bucket print from the SeriesCache (already
    // parsed for the chart). Value + regime chip + fill gauge (amber). PRO.
    {
        const auto& series = ctx_.series_mgr().vpin(Terminal::pair_key(pair_.exchange, pair_.symbol));
        char val[16] = "--", age_note[48] = "Waiting for a completed volume bucket";
        const char* chip = "--"; int chip_lvl = 0; float mval = 0.0f;
        if (!series.empty() && series.back().ts_ms <= Entitlements::wall_now_ms()) {
            const Series::VPINPoint& p = series.back();
            snprintf(val, sizeof(val), "%.2f", p.vpin);
            mval = p.vpin;
            char age[16];
            StatFreshness::age_label(static_cast<int64_t>(Entitlements::wall_now_ms()) - p.ts_ms, age, sizeof(age));
            snprintf(age_note, sizeof(age_note), "Last bucket %s ago", age);
            static const char* const kRegime[4] = { "NORMAL", "ELEVATED", "HIGH", "CRITICAL" };
            const int r = (p.regime >= 0 && p.regime <= 3) ? p.regime : 0;
            chip = kRegime[r]; chip_lvl = r;
        }
        metric_row(Row{ .label = "VPIN", .pro = true, .value = val, .detail = age_note,
                        .meter = val[0] == '-' ? 0 : 3, .meter_a = mval, .meter_c1 = Tokens::WARN,
                        .chip = val[0] == '-' ? nullptr : chip, .chip_level = chip_lvl });
    }


}

// ── Positioning ─────────────────────────────────────────────────────────────
void StatsWidget::render_positioning(bool overview) {
    const PositioningState* pos = positioning();
    char funding_value[24], funding_detail[48], oi_value[24], oi_detail[48];
    Row funding_row{.label = "Funding", .value = funding_value, .detail = funding_detail,
        .help = "Latest funding rate for the venue's stated cadence. Positive rates mean longs pay shorts. The next-payment row states the venue schedule when available."};
    Row oi_row{.label = "Open interest", .value = oi_value, .detail = oi_detail,
        .help = "Outstanding contract quantity multiplied by the mark price. Changes below use contract quantity; account ratios count accounts."};
    // Funding - LIVE rate + countdown to next funding. FREE, amber. A stated
    // cadence is shown only when stated; nothing assumes eight hours.
    {
        const auto funding_state = stat_field(snapshot_.stat.funding_ms, 600000);
        const auto* metadata = SymbolRegistry::instance().get(pair_.exchange, pair_.symbol);
        const bool no_funding = metadata && !metadata->perpetual;
        char cadence[8]; StatFreshness::cadence_label(snapshot_.stat.funding_interval_minutes > 0,
            snapshot_.stat.funding_interval_minutes, cadence, sizeof(cadence));
        char val[24];
        if (funding_state.state == StatFreshness::State::Unavailable) snprintf(val, sizeof(val), "--");
        else snprintf(val, sizeof(val), "%+.4f%%", snapshot_.stat.funding * 100.0);
        char det[48] = "Unavailable";
        if (no_funding)
            snprintf(det, sizeof(det), "Not applicable");
        else if (snapshot_.stat.next_funding_time > 0) {
            long long ms = snapshot_.stat.next_funding_time
                         - static_cast<long long>(Entitlements::wall_now_ms());
            if (ms < 0) ms = 0;
            const long long s = ms / 1000;
            snprintf(det, sizeof(det), "%02d:%02d:%02d%s%s",
                     static_cast<int>(s / 3600), static_cast<int>((s % 3600) / 60), static_cast<int>(s % 60),
                     cadence[0] ? " \xc2\xb7 " : "", cadence);
        }
        if (funding_state.state == StatFreshness::State::Stale) {
            char age[16]; StatFreshness::age_label(funding_state.age_ms, age, sizeof(age));
            snprintf(det, sizeof(det), "STALE %s", age);
        }
        const ImVec4 fcol = funding_state.state == StatFreshness::State::Stale ? Tokens::TX3 : Tokens::TX1;
        snprintf(funding_value, sizeof(funding_value), "%s",
            funding_state.state == StatFreshness::State::Stale ? "Stale" : val);
        snprintf(funding_detail, sizeof(funding_detail), "%s", det);
        funding_row.value_col = fcol; funding_row.detail_col = fcol;
    }
    // Open interest - LIVE. The stat's open_interest_usd field actually carries
    // CONTRACT QTY (backend note in stat.go), so notional USD = contracts * mark.
    // History changes use contract quantity and remain independent of account ratios.
    {
        const auto oi_state = stat_field(snapshot_.stat.open_interest_ms, 600000);
        const auto mark_state_oi = stat_field(snapshot_.stat.mark_price_ms, 30000);
        const bool oi_known = oi_state.state != StatFreshness::State::Unavailable &&
                              mark_state_oi.state != StatFreshness::State::Unavailable;
        const double oi_usd = oi_known ? snapshot_.stat.open_interest_usd * snapshot_.stat.mark_price : 0.0;
        char val[24];
        if (oi_usd > 0.0) fmt_usd(oi_usd, val, sizeof(val));
        else              snprintf(val, sizeof(val), "--");
        if (oi_state.state == StatFreshness::State::Stale || mark_state_oi.state == StatFreshness::State::Stale) {
            char age[16]; StatFreshness::age_label(std::max(oi_state.age_ms, mark_state_oi.age_ms), age, sizeof(age));
            snprintf(val, sizeof(val), "STALE %s", age);
        }
        // 1H/4H deltas derived client-side from the OI history ring.
        char det[48] = "1H -- \xc2\xb7 4H --";
        bool h1 = false, h4 = false;
        const auto now = static_cast<int64_t>(Entitlements::wall_now_ms());
        double d1 = 0, d4 = 0;
        h1 = oi_history_.change(3600000, now, d1);
        h4 = oi_history_.change(14400000, now, d4);
        if (h1 || h4) {
            char s1[12] = "--", s4[12] = "--";
            if (h1) snprintf(s1, sizeof(s1), "%+.1f%%", d1 * 100.0);
            if (h4) snprintf(s4, sizeof(s4), "%+.1f%%", d4 * 100.0);
            snprintf(det, sizeof(det), "1H %s \xc2\xb7 4H %s", s1, s4);
        }
        snprintf(oi_value, sizeof(oi_value), "%s", val);
        snprintf(oi_detail, sizeof(oi_detail), "%s", det);
    }
    if (overview) {
        oi_row.detail="Latest notional";
        // The funding detail already carries countdown, cadence or stale state.
        summary_pair(oi_row,funding_row);
        return;
    }
    // Keep the headline pair uncluttered; the history/timing rows are always visible.
    oi_row.detail = nullptr; funding_row.detail = nullptr;
    summary_pair(oi_row, funding_row);
    metric_row(Row{.label = "OI change", .value = oi_detail,
        .help = "Change in contract quantity, not mark-priced USD. Uses recorded minute closes and live OI observations. A missing or distant anchor stays unavailable."});
    metric_row(Row{.label = "Next funding", .value = funding_detail, .value_col = funding_row.detail_col});
    if (!snapshot_.stat.mark_price_ms && snapshot_.stat.mark_price > 0 && StatFreshness::stated(snapshot_.stat))
        note("Mark / funding source timestamps missing");
    if (!pos) note("Account ratios unavailable or stale.");
    {
        const auto now = static_cast<int64_t>(Entitlements::wall_now_ms());
        double v = 0;
        const bool has_velocity = oi_history_.change(60000, now, v, true);
        const auto oi_state = stat_field(snapshot_.stat.open_interest_ms, 120000);
        char val[32];
        snprintf(val, sizeof(val), "%s", oi_state.state == StatFreshness::State::Stale ? "Stale" : "Collecting history");
        if (has_velocity) snprintf(val, sizeof(val), "%+.2f%%/m", v * 100.0);
        metric_row(Row{.label = "OI velocity", .pro = true, .value = val,
            .value_col = has_velocity ? (v >= 0 ? Tokens::UP : Tokens::DOWN) : Tokens::TX3,
            .help = "Contract-quantity change per minute over approximately the last minute. Uses observed anchors within 90 seconds of the target; it never interpolates missing history."});
        double unused = 0;
        if (!oi_history_.change(14400000, now, unused))
            note(oi_state.state == StatFreshness::State::Stale ? "OI source stale; changes unavailable" :
                history_attempts_ < 3 ? "Loading available OI history" : "OI history incomplete; collecting live samples");
    }
    // Account ratios are counts, not position sizes or measured accumulation.
    if (pos) {
        char val[24] = "-- / --"; float mval = 0.5f;
        if (pos && pos->global_long_account > 0.0) {
            const float lg = static_cast<float>(pos->global_long_account);
            mval = lg;
            snprintf(val, sizeof(val), "%.0f%% / %.0f%%", lg * 100.0f, (1.0f - lg) * 100.0f);
        }
        metric_row(Row{ .label = "Long / short accounts", .value = val, .meter = pos && pos->global_long_account > 0 ? 1 : 0, .meter_a = mval,
            .help = "Global long/short account share. This does not measure position size." });
        if (pos->top_trader_long_account > 0.0 && pos->top_trader_long_account <= 1.0) {
            char top_accounts[24];
            snprintf(top_accounts, sizeof(top_accounts), "%.0f%% long", pos->top_trader_long_account * 100.0);
            metric_row(Row{.label = "Top-trader accounts", .pro = true, .value = top_accounts,
                .help = "Long-account share among the venue's top traders. The old Accumulation bias row was this fraction minus 50%, not an accumulation detector."});
        }
    }
}

// Both windows are observed venue reports, never estimates of all liquidations.
void StatsWidget::render_liq_risk(bool overview) {
    const auto* pos = positioning();
    const auto now = static_cast<int64_t>(Entitlements::wall_now_ms());
    const bool minute = snapshot_.active(now), hour = hour_snapshot_.active(now);
    const bool known = pos || hour || minute;
    const auto& fallback = hour ? hour_snapshot_ : snapshot_;
    const char* scope = pos ? "Last 1h" : hour ? "UTC hour" : "UTC minute";
    if (!overview && !render_section_header(SEC_LIQ_RISK, "Liquidation details", false, scope)) return;
    const double longs = pos ? pos->long_liq_usd : known ? fallback.stat.liq_long_usd : 0.0;
    const double shorts = pos ? pos->short_liq_usd : known ? fallback.stat.liq_short_usd : 0.0;
    const double total = longs + shorts;
    char short_value[32] = "--", long_value[32] = "--";
    if (known) { fmt_usd(shorts, short_value, sizeof(short_value)); fmt_usd(longs, long_value, sizeof(long_value)); }
    if(overview) {
        char period[64];snprintf(period,sizeof(period),"%s · reported",scope);
        summary_pair(Row{.label="Shorts liquidated",.value=short_value,
            .value_col=known?Tokens::UP:Tokens::TX3,.detail=period,
            .help="Reported forced-buy notional in the labelled window. Binance sends at most one report per second per market (a floor); Bybit sends every liquidation."},
            Row{.label="Longs liquidated",.value=long_value,
                .value_col=known?Tokens::DOWN:Tokens::TX3,.detail=period,
                .help="Reported forced-sell notional in the labelled window. Bybit reports use bankruptcy prices. Not total exchange liquidations."});
        return;
    }
    summary_pair(Row{.label = "Shorts", .value = short_value,
        .value_col = known ? Tokens::UP : Tokens::TX3,
        .help = "Short liquidations are forced buying. Amounts are venue reports within the labelled window, not estimates of all liquidations."},
        Row{.label = "Longs", .value = long_value,
        .value_col = known ? Tokens::DOWN : Tokens::TX3,
        .help = "Long liquidations are forced selling. Reports alone do not confirm a squeeze."}, true);
    char minute_value[24] = "--", current[48];
    if (minute) fmt_usd(snapshot_.stat.liq_long_usd + snapshot_.stat.liq_short_usd, minute_value, sizeof(minute_value));
    snprintf(current, sizeof(current), "This minute %s", minute_value);
    if (known && total > 0) {
        char share[32]; snprintf(share, sizeof(share), "%.1f%% shorts", shorts / total * 100);
        split_bar(static_cast<float>(shorts / total), share, current);
        if (ImGui::IsItemHovered()) Theme::tooltip("Share of reported liquidation notional in the labelled window. The minute total resets at each UTC minute; it is not added to the main total.");
    } else {
        note(known ? "No liquidation reports in this window" : "Liquidation data unavailable");
        if (pos || hour) metric_row(Row{.label = "Current minute", .value = minute_value});
    }
    const char* source = pair_.exchange == "binancef" ? "Sampled reports" :
        pair_.exchange == "bybit" ? "At bankruptcy price" : "Venue reports";
    char source_note[100];
    if (known) {
        char age[16]; StatFreshness::age_label(now - (pos ? pos->timestamp_ms : fallback.received_ms), age, sizeof(age));
        snprintf(source_note, sizeof(source_note), "%s / updated %s ago", source, age);
    } else snprintf(source_note, sizeof(source_note), "%s / -- unavailable", source);
    {
        char detail[80] = "Current minute unavailable";
        if (minute) {
            char l[24], sh[24]; fmt_usd(snapshot_.stat.liq_long_usd, l, sizeof(l)); fmt_usd(snapshot_.stat.liq_short_usd, sh, sizeof(sh));
            snprintf(detail, sizeof(detail), "Shorts %s / longs %s", sh, l);
        }
        metric_row(Row{.label = "This minute", .value = detail});
    }
    note(source_note);
    ImGui::Dummy(ImVec2(0, 8));
}

void StatsWidget::render_icebergs() {
    const auto now = static_cast<int64_t>(Entitlements::wall_now_ms());
    const auto* ice = ctx_.analytics ? ctx_.analytics_mgr().get_iceberg(Terminal::pair_key(pair_.exchange, pair_.symbol)) : nullptr;
    const bool fresh = ice && ice->fresh(now), paid = Entitlements::is_pro();
    const bool volume = fresh && ice->buy_usd + ice->sell_usd > 0;
    char counts[48] = "--", bias[32] = "--";
    if (fresh) snprintf(counts, sizeof(counts), "%d buy / %d sell", ice->buy_clusters, ice->sell_clusters);
    if (volume) snprintf(bias, sizeof(bias), "%+.0f%%", ice->bias() * 100);
    char help[380] = "Repeated taker-side trade clusters, not confirmed hidden limit orders. No feed is not zero detections. Counts cover the last five minutes.";
    if (ice) snprintf(help, sizeof(help), "Repeated taker-side clusters with at least %d bulks of $%.0f and heuristic confidence >= %.2f. Counts are unique qualifying clusters with complete bulks in the last 5m. Bias is (buy - sell) / total observed cluster notional. This does not identify passive hidden orders or accumulation.", ice->min_refreshes, ice->min_bulk_usd, ice->min_confidence);
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(org.x + 20, org.y + 6), ImVec2(org.x + width - 20, org.y + 73), col(Tokens::ELEV));
    ImGui::PushFont(Fonts::ui());
    dl->AddText(ImVec2(org.x + 32, org.y + 16), col(Tokens::TX2), "Iceberg candidates");
    const char* window = "5m heuristic";
    dl->AddText(ImVec2(org.x + width - 32 - ImGui::CalcTextSize(window).x, org.y + 16), col(Tokens::TX3), window);
    ImGui::PopFont();
    ImGui::PushFont(Fonts::mono());
    dl->AddText(ImVec2(org.x + 32, org.y + 43), col(Tokens::TX1), paid ? counts : "Locked");
    ImGui::PopFont();
    char bias_label[48]; snprintf(bias_label, sizeof(bias_label), "Bias %s", paid ? bias : "--");
    ImGui::PushFont(Fonts::ui());
    dl->AddText(ImVec2(org.x + width - 32 - ImGui::CalcTextSize(bias_label).x, org.y + 43),
        col(paid && volume ? (ice->bias() >= 0 ? Tokens::UP : Tokens::DOWN) : Tokens::TX3), bias_label);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(width, 84));
    if (ImGui::IsItemHovered()) Theme::tooltip("%s", paid ? help : "Pro access required.");
    if (paid && (!fresh || ice->warming() || ice->history_limited))
        note(!fresh ? (ice ? "Detector or trade source stale" : "Detector feed unavailable") :
             ice->history_limited ? "Partial detector history" : "Detector warming up");
    if (paid) {
        char detail[96] = "No fresh detector data";
        if (fresh) {
            char age[16]; StatFreshness::age_label(now - ice->timestamp_ms, age, sizeof(age));
            snprintf(detail, sizeof(detail), "Last 5m / updated %s ago%s%s", age,
                ice->warming() ? " / warming" : "", ice->history_limited ? " / partial" : "");
        }
        char buy[24] = "--", sell[24] = "--";
        if (fresh) { fmt_usd(ice->buy_usd, buy, sizeof(buy)); fmt_usd(ice->sell_usd, sell, sizeof(sell)); }
        char amounts[64]; snprintf(amounts, sizeof(amounts), "Buy %s / sell %s", buy, sell);
        metric_row(Row{.label = "Observed", .value = amounts, .detail = detail,
            .help = "Observed taker notional of the candidate clusters, not estimated hidden inventory."});
    }
}
