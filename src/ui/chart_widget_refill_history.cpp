// Refill levels on candle charts: recorded levels read from the server's history (get_refill_history) for the
// visible range, each drawn as a pointer at its exact price beside the candle where it qualified. The store, the request
// planner and the selection are in core/refill_history.h; the access rule, the upsell and the hover wording are
// shared with the Real-time layer (core/refill_levels.h, chart_widget_refill_levels.cpp).
#include "ui/chart_widget.h"
#include "core/display_time_zone.h"
#include "core/entitlements.h"
#include "core/footprint_manager.h"
#include "core/ui_test_marks.h"
#include "replayer/replay_manager.h"
#include "rendering/liqmap2_rails_overlay.h"
#include "rendering/theme.h"
#include "stream_handler.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <emscripten.h>

namespace {
// Shortest span the selection treats as drawn: a level shorter than a candle (or this many pixels) still counts.
constexpr float kMinSegmentPx = 8.0f;

// Another overlay (liquidation diamonds, rails, trade bubbles) already explains what is under the pointer.
bool tooltip_taken() {
    const ImGuiContext& g = *GImGui;
    return g.TooltipPreviousWindow && g.TooltipPreviousWindow->Active;
}
}  // namespace

// The server refused this viewer (pro_required): the locked row, never an error on the chart.
bool ChartWidget::refill_history_locked() const {
    const refill::history::Market* m =
        refill::history::store().find(pair_.exchange, pair_.symbol, ctx_.replay_mgr().is_active());
    return m && m->error == refill::history::Error::pro_required;
}

bool ChartWidget::refill_history_active() const {
    return !rt_mode_ && candle_refill_enabled_ && ct_allows_time_overlays(chart_type_) && !refill_pack() &&
        refill::granted(refill_access()) && !refill_history_locked();
}

// Every frame: ask for the visible range once it has rested, one read at a time, never past the clock.
void ChartWidget::update_refill_history() {
    auto& store = refill::history::store();
    ReplayManager& rm = ctx_.replay_mgr();
    const bool replay = rm.is_active();
    const refill::Access access = refill_access();
    const bool want = is_open && !rt_mode_ && candle_refill_enabled_ && ct_allows_time_overlays(chart_type_) &&
        !refill_pack() && refill::granted(access);
    if (!want) {
        refill_history_req_.reset(store);
        refill_history_seeking_ = false;
        return;
    }
    // A seek reads again from the new position (a rewind clears the store through main.cpp's hook).
    if (replay && rm.is_loading()) {
        if (!refill_history_seeking_) store.clear_replay();
        refill_history_seeking_ = true;
        return;
    }
    refill_history_seeking_ = false;
    StreamManager& sm = ctx_.stream_mgr();
    // A replay asks on its own socket once joined, never on the live one while the session is being created.
    if (replay != sm.is_replay_mode() || !sm.connected()) return;
    if (!(last_visible_range_.X.Max > last_visible_range_.X.Min) || last_visible_range_.X.Min <= 0) return;
    // Candles draw at their open time, centred on their slot: the slot [t, t + tf) spans t - tf/2 to t + tf/2.
    const int64_t half = timeframe_seconds() * 500;
    refill::history::Want w;
    w.exchange = pair_.exchange;
    w.symbol = pair_.symbol;
    w.replay = replay;
    w.view_from = int64_t(last_visible_range_.X.Min) + half;
    w.view_to = int64_t(last_visible_range_.X.Max) + half;
    w.now = emscripten_get_now();
    const int64_t now_ms = int64_t(emscripten_date_now());
    if (replay) {
        // The server reads from the replay start to its playhead and no further.
        w.clock = std::min(rm.interpolated_time_ms(), rm.info().confirmed_time_ms);
        w.floor = rm.info().start_time_ms;
    } else {
        w.clock = now_ms - refill::history::kLiveMarginMs;
        if (access == refill::Access::free_taste) {
            int64_t start = 0, end = 0;
            Entitlements::free_window_range(now_ms, start, end);
            w.floor = start;
        } else {
            w.floor = now_ms - Entitlements::pro_lookback_ms();
        }
    }
    if (w.clock <= 0 || w.floor <= 0) return;
    refill::history::Request r;
    if (!refill_history_req_.step(store, w, r)) return;
    sm.send_message(refill::history::request_message(r, pair_.exchange, pair_.symbol, refill_token()));
}

// Layer off, chart closed, context swap: the read in flight is forgotten and a cache nobody uses is dropped.
void ChartWidget::release_refill_history() {
    refill_history_req_.reset(refill::history::store());
    refill_history_seeking_ = false;
    refill_history_hover_ = -1;
}

// Over the candles, under the liquidation diamonds and the drawings. Picks the hovered level for
// render_refill_history_hover, which runs after every other overlay.
//
// Each level is one pointer at its exact price, on the candle where it qualified (James 2026-10-15: the absorption
// bubbles and the grey lines from known to close "need either a rethink or a significant repolish"; on a 4 h chart
// every level lasted minutes, so each drew as a candle-wide grey stub ending in a ring stacked on the candle). A bid
// that refilled (buyers absorbing sells) points in from the candle's left in the up colour; an ask (sellers absorbing
// buys) from its right in the down colour, as a ladder puts bids left and asks right. Size follows what it absorbed
// against the largest drawn, so the few that matter lead. Nothing crosses a candle: the level's life (known to close)
// draws only while it is hovered, with a hairline at its price across the plot.
void ChartWidget::render_refill_history() {
    refill_history_hover_ = -1;
    if (!refill_history_active()) return;
    const bool replay = ctx_.replay_mgr().is_active();
    const refill::history::Market* m = refill::history::store().find(pair_.exchange, pair_.symbol, replay);
    if (!m) return;
    const ImPlotRect limits = ImPlot::GetPlotLimits();
    const ImVec2 plot_pos = ImPlot::GetPlotPos(), plot_size = ImPlot::GetPlotSize();
    const double tf = double(timeframe_seconds()) * 1000.0;
    const double shift = tf * 0.5;
    const double ms_per_px = limits.X.Size() / std::max(1.0, double(plot_size.x));
    refill::history::View view;
    view.t0 = int64_t(limits.X.Min + shift);
    view.t1 = int64_t(limits.X.Max + shift);
    view.p0 = limits.Y.Min;
    view.p1 = limits.Y.Max;
    view.min_len = int64_t(std::max(tf, double(kMinSegmentPx) * ms_per_px));
    // Never ahead of the chart: the replay clock, or now.
    const int64_t clock = replay ? ctx_.replay_mgr().interpolated_time_ms() : int64_t(emscripten_date_now());

    // A short note, only when it changes what the chart means: the Free window, or a read that kept its largest.
    const char* note = nullptr;
    if (refill_access() == refill::Access::free_taste &&
        (refill_history_req_.before_floor() || m->error == refill::history::Error::outside_history_window))
        note = "Free history covers the last 3 days";
    else if (refill::history::truncated_in(*m, view.t0, view.t1))
        note = "Largest refill levels shown";
    if (note) render_refill_history_note(note);

    const size_t n = refill::history::select(*m, clock, view, refill_history_order_);
    if (!n) return;
    const double max_usd = m->levels[refill_history_order_[0]].executed_usd;  // the selection is largest first
    // The candle a time falls in, drawn centred on its open (plot_candles): its open from the chart's own candles,
    // the forming one included, else the timeframe's grid.
    const auto& opens = candles().timestamps();
    const auto candle_open = [&](int64_t t) {
        if (candles().has_building_candle() && candles().building_candle().timestamp_ms <= t &&
            t < candles().building_candle().timestamp_ms + int64_t(tf))
            return double(candles().building_candle().timestamp_ms);
        const auto it = std::upper_bound(opens.begin(), opens.end(), double(t));
        if (it != opens.begin() && double(t) < *std::prev(it) + tf) return *std::prev(it);
        return std::floor(double(t) / tf) * tf;
    };
    // plot_candles' body: 70% of the slot, 15% of that under a footprint.
    const float body_half = float(tf * 0.35 * (ctx_.footprint_mgr().enabled ? 0.15 : 1.0) / ms_per_px);
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool hover_ok = ImPlot::IsPlotHovered() && !drawing_layer_.captures_mouse();
    float hovered_d = 1e9f;
    struct Mark { ImVec2 tip; float w, h; int dir; };  // dir: +1 points right (a bid, left of its candle), -1 left
    Mark hovered_mark{};
    ImPlot::PushPlotClipRect();
    // Largest last, so it sits on top.
    for (size_t k = n; k-- > 0;) {
        const uint16_t index = refill_history_order_[k];
        const refill::Level& l = m->levels[index];
        const bool bid = l.side == refill::Side::bid;
        const float share = float(std::sqrt(max_usd > 0 ? std::clamp(l.executed_usd / max_usd, 0.0, 1.0) : 0.0));
        const float h = std::round(7.0f + 6.0f * share), w = std::round(h * 0.75f);
        const float cx = ImPlot::PlotToPixels(candle_open(l.known_ms), l.price).x;
        const float y = std::round(ImPlot::PlotToPixels(0.0, l.price).y) + 0.5f;
        const int dir = bid ? 1 : -1;
        const ImVec2 tip(std::round(cx - dir * (body_half + 2.0f)), y);
        const ImVec2 a(tip.x - dir * w, y - h * 0.5f), b(tip.x - dir * w, y + h * 0.5f);
        const ImVec4& side = bid ? Theme::Tokens::UP : Theme::Tokens::DOWN;
        const bool pulled = l.status == refill::End::pulled;
        // A dark keyline lifts it off candles and wicks; a pulled level (its liquidity left) is drawn hollow.
        dl->AddTriangle(tip, a, b, Theme::u32(Theme::Tokens::BASE, 0.9f), 2.5f);
        if (pulled) dl->AddTriangle(tip, a, b, Theme::u32(side, 0.75f), 1.3f);
        else dl->AddTriangleFilled(tip, a, b, Theme::u32(side));
        const float x_lo = std::min(tip.x, a.x), x_hi = std::max(tip.x, a.x);
        if (ui_test::enabled())
            ui_test::mark_rect("chart.refill." + std::to_string(k), ImVec2(x_lo, a.y), ImVec2(x_hi, b.y));
        if (hover_ok && mouse.x >= x_lo - 3.0f && mouse.x <= x_hi + 3.0f && std::abs(mouse.y - y) <= h * 0.5f + 3.0f) {
            const float d = std::abs(mouse.y - y) + std::abs(mouse.x - (x_lo + x_hi) * 0.5f) * 0.25f;
            if (d < hovered_d) {
                refill_history_hover_ = index;
                hovered_d = d;
                hovered_mark = {tip, w, h, dir};
            }
        }
    }
    // The hovered level: a hairline at its price across the plot, its life from known to close, and the pointer
    // again on top with a light edge.
    if (refill_history_hover_ >= 0) {
        const refill::Level& l = m->levels[size_t(refill_history_hover_)];
        const bool bid = l.side == refill::Side::bid;
        const ImVec4& side = bid ? Theme::Tokens::UP : Theme::Tokens::DOWN;
        const float y = hovered_mark.tip.y;
        dl->AddLine(ImVec2(plot_pos.x, y), ImVec2(plot_pos.x + plot_size.x, y), Theme::u32(Theme::Tokens::TX1, 0.14f), 1.0f);
        const float x0 = ImPlot::PlotToPixels(double(l.known_ms) - shift, l.price).x;
        const float x1 = std::max(ImPlot::PlotToPixels(double(l.closed_ms) - shift, l.price).x, x0 + 2.0f);
        dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), Theme::u32(Theme::Tokens::BASE, 0.9f), 4.0f);
        dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), Theme::u32(side, 0.9f), 2.0f);
        dl->AddLine(ImVec2(x1, y - 4.0f), ImVec2(x1, y + 4.0f), Theme::u32(side, 0.9f), 1.5f);
        const ImVec2 tip = hovered_mark.tip;
        const ImVec2 a(tip.x - hovered_mark.dir * hovered_mark.w, y - hovered_mark.h * 0.5f);
        const ImVec2 b(tip.x - hovered_mark.dir * hovered_mark.w, y + hovered_mark.h * 0.5f);
        dl->AddTriangle(tip, a, b, Theme::u32(Theme::Tokens::BASE, 0.9f), 2.5f);
        dl->AddTriangleFilled(tip, a, b, Theme::u32(side, l.status == refill::End::pulled ? 0.45f : 1.0f));
        dl->AddTriangle(tip, a, b, Theme::u32(Theme::Tokens::TX1, 0.9f), 1.0f);
    }
    ImPlot::PopPlotClipRect();
    refill_history_hover_rev_ = m->revision;
}

// The level under the pointer, unless another overlay has already said something. The rails' hover grammar
// (liqmap2_rails_overlay::hover): the side and price, the dollars it absorbed as the hero, what showed and how often
// it refilled, then when it qualified, how long it held and how it ended.
void ChartWidget::render_refill_history_hover() {
    const int index = refill_history_hover_;
    refill_history_hover_ = -1;
    if (index < 0 || tooltip_taken() || ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    if (Theme::chart_chip_covers(ImGui::GetIO().MousePos)) return;
    const refill::history::Market* m =
        refill::history::store().find(pair_.exchange, pair_.symbol, ctx_.replay_mgr().is_active());
    if (!m || m->revision != refill_history_hover_rev_ || size_t(index) >= m->levels.size()) return;
    const refill::Level& l = m->levels[size_t(index)];
    const bool bid = l.side == refill::Side::bid;
    char price[32], traded[24], shown[24], detail[96], from[32] = "", life[32] = "", foot[128];
    fmt_.format_price(price, sizeof(price), l.price);
    refill::format_usd(l.executed_usd, traded, sizeof(traded));
    refill::format_usd(l.max_shown_base * l.price, shown, sizeof(shown));
    std::snprintf(detail, sizeof(detail), "vs %s shown \xc2\xb7 refilled %d\xc3\x97", shown, l.refills);
    // When it qualified, in the chart's own time format (seconds only under one-minute candles), and how long it held.
    const TimeZoneFormat style = timeframe_seconds() < 60 ? TimeZoneFormat::TimeSeconds : TimeZoneFormat::TimeMinutes;
    DisplayTimeZone::instance().format(l.known_ms, style, from, sizeof(from));
    const int64_t held = std::max<int64_t>(0, l.closed_ms - l.known_ms);
    if (held < 60000) std::snprintf(life, sizeof(life), "%llds", static_cast<long long>(held / 1000));
    else if (held < 3600000) std::snprintf(life, sizeof(life), "%lld min", static_cast<long long>(held / 60000));
    else std::snprintf(life, sizeof(life), "%lldh %02lld", static_cast<long long>(held / 3600000),
                       static_cast<long long>((held % 3600000) / 60000));
    std::snprintf(foot, sizeof(foot), "%s%sheld %s \xc2\xb7 %s", from, *from ? " \xc2\xb7 " : "", life, refill::end_word(l.status));

    namespace hv = liqmap2_rails_overlay::hover;
    const auto type = hv::begin();
    const ImVec4& tone = bid ? Theme::Tokens::UP : Theme::Tokens::DOWN;
    const hv::Span who{Theme::Fonts::ui_semibold(), type.body, bid ? "BID REFILL" : "ASK REFILL", tone};
    const hv::Span at = type.figure(price);
    const hv::Span why = type.quiet(bid ? "buyers absorbed sells" : "sellers absorbed buys");
    const hv::Span usd{Theme::Fonts::heading(), type.hero, traded, Theme::Tokens::TX1};
    const hv::Span what = type.words("traded into it", Theme::Tokens::TX2);
    const hv::Span more = type.words(detail, Theme::Tokens::TX2);
    const hv::Span tail = type.quiet(foot);
    const float w = std::round(std::max({type.min_w * 0.85f, who.w() + 8.0f + at.w(), usd.w() + 7.0f + what.w(), more.w(),
                                         tail.w(), why.w()}));
    hv::line(w, {{0, who}, {who.w() + 8.0f, at}});
    hv::gap(2.0f);
    hv::line(w, {{0, why}});
    hv::gap(8.0f);
    hv::line(w, {{0, usd}, {usd.w() + 7.0f, what}});
    hv::gap(3.0f);
    hv::line(w, {{0, more}});
    hv::rule(w);
    hv::line(w, {{0, tail}});
    hv::end();
}

// A muted chip at the plot's bottom left, above the liquidation status chip and the scenario caption.
void ChartWidget::render_refill_history_note(const char* note) {
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const ImVec2 pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    float bottom = pos.y + size.y - 6.0f;
    if (scenario_map_shown()) bottom -= 24.0f;
    if (reported_liquidations_enabled() && !liquidation_activity_visible() && liq_status_attention_) bottom -= 26.0f;
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const float px = ImGui::GetFontSize();
    ImGui::PopFont();
    Theme::push_font(Theme::Fonts::ui(), px);
    const ImVec2 text = ImGui::CalcTextSize(note);
    constexpr float h = 20.0f;
    const ImVec2 a(pos.x + 10.0f, bottom - h);
    const ImVec2 b(std::round(a.x + 19.0f + text.x + 8.0f), bottom);
    dl->AddRectFilled(a, b, Theme::u32(Theme::Tokens::PANEL, 0.92f), Theme::Radius::R2);
    dl->AddRect(a, b, Theme::u32(Theme::Tokens::BD2), Theme::Radius::R2, 0, 1.0f);
    dl->AddCircleFilled(ImVec2(a.x + 10.0f, a.y + h * 0.5f), 2.5f, Theme::u32(Theme::Tokens::TX2), 12);
    dl->AddText(ImVec2(a.x + 19.0f, std::round(a.y + (h - text.y) * 0.5f)), Theme::u32(Theme::Tokens::TX3), note);
    ImGui::PopFont();
    Theme::chart_chip(a, b);
}

// Layers > MARKET STRUCTURE row on candle charts. Same access as Real-time: free on live Binance BTC, Pro on every
// other Binance futures market, any market in a replay.
void ChartWidget::render_refill_history_layer_row() {
    const refill::Access access = refill_access();
    const bool pack = refill_pack();
    if (pack || access == refill::Access::unsupported) {
        Theme::menu_layer("Refill levels", false, false, pack ? "not in local replay" : "Binance futures only");
        if (ImGui::IsItemHovered())
            Theme::tooltip(pack ? "Refill levels come from the replay server, not local replays."
                                : "Refill levels are on Binance futures only for now.");
        return;
    }
    if (access == refill::Access::locked || refill_history_locked()) {
        if (Theme::menu_layer("Refill levels", false, true)) open_refill_upsell("click");
        if (ImGui::IsItemHovered())
            Theme::tooltip("Prices that kept refilling as they got hit.\nFree on BTC, Pro on every other market.");
        return;
    }
    const bool free_here = access == refill::Access::free_taste;
    if (Theme::menu_layer("Refill levels", candle_refill_enabled_, false, free_here ? "free here" : nullptr))
        candle_refill_enabled_ = !candle_refill_enabled_;
    const refill::history::Market* m =
        refill::history::store().find(pair_.exchange, pair_.symbol, ctx_.replay_mgr().is_active());
    if (ImGui::IsItemHovered()) {
        char since[32] = "";
        if (m && m->coverage_known && m->coverage_from_ms == 0)
            Theme::tooltip("Prices that kept refilling as they got hit. Not recorded for this market yet.");
        else if (m && m->coverage_known &&
                 DisplayTimeZone::instance().format(m->coverage_from_ms, TimeZoneFormat::ReplayDate, since, sizeof(since)))
            Theme::tooltip("Prices that kept refilling as they got hit. Recorded since %s.", since);
        else
            Theme::tooltip("Prices that kept refilling as they got hit.");
    }
    if (!candle_refill_enabled_ || !m) return;
    const char* note = m->error == refill::history::Error::source_unavailable ? "History unavailable right now"
                     : free_here && refill_history_req_.before_floor() ? "Free history covers the last 3 days"
                     : nullptr;
    if (note) {
        Theme::begin_menu_detail();
        Theme::menu_note(note);
        Theme::end_menu_detail();
    }
}
