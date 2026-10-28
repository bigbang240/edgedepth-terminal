#include "ui/chart_widget.h"
#include "core/i18n.h"
#include "core/ui_test_marks.h"
#include "core/position_sizing.h"
#include "core/execution_bridge.h"
#include "ui/drawing/drawing_icons.h"
#include "core/candle_bubble_display.h"
#include "ui/realtime_navigation.h"
#include "core/candle_manager.h"
#include "core/display_time_zone.h"
#include "core/trade_at_price.h"
#include "core/orderbook_manager.h"
#include "core/entitlements.h"
#include "core/realtime_preview.h"
#include "core/realtime_archive.h"
#include "core/usage_emit.h"
#include "replayer/replay_manager.h"
#include <nlohmann/json.hpp>
#include "core/education_boot.h"
#include "ui/upsell_modal.h"
#include "rendering/theme.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <emscripten.h>
#include "stream_handler.h"
#include <array>
#include "rendering/realtime_bubble.h"
#include "rendering/observed_liquidations.h"
#include "ui/indicators/volume_indicator.h"
#include "types/frame_profiler.h"

bool ChartWidget::rt_mode_gated() const {
    return Entitlements::hosted() && !Entitlements::is_pro() &&
        !ctx_.replay_mgr().is_active() && !EducationBoot::instance().is_pack() &&
        !Entitlements::btc_preview_market(pair_.exchange, pair_.symbol);
}

void ChartWidget::set_rt_candle_seconds(const int seconds) {
    // The supported grid. Anything else is ignored so a stale workspace or a
    // scripted call cannot put the live view on an unbounded bucket.
    switch (seconds) {
        case 1: case 5: case 15: case 30: case 60: case 300:
            rt_candle_seconds_ = seconds;
            break;
        default: break;
    }
}

bool ChartWidget::rt_mode_locked() const {
    return rt_mode_gated() && !rt_preview::Store::instance().available();
}

int64_t ChartWidget::rt_preview_remaining_ms() const {
    return rt_preview::Store::instance().remaining();
}

// The market as the upsell names it: "OGN", or "Bybit BTC" for a BTC that is
// not the free Binance one.
static std::string rt_subject(const Terminal::Pair& pair) {
    std::string subject = Entitlements::coin_label(pair.symbol);
    if (subject == "BTC") {
        subject = (pair.exchange == "bybit" ? "Bybit " : pair.exchange == "hl" ? "Hyperliquid " : "") + subject;
    }
    return subject;
}

bool ChartWidget::tick_rt_preview() {
    if (!rt_mode_gated()) return false;
    auto& budget = rt_preview::Store::instance();
    budget.bill_frame(ImGui::GetFrameCount(), ImGui::GetIO().DeltaTime);
    if (budget.available()) return false;

    // Spent: back to candles, then the upsell for the market they were watching.
    const std::string subject = rt_subject(pair_);
    usage::dispatch_detail(nlohmann::json{
        {"event", "rt_preview_end"}, {"mode", "terminal"}, {"symbol", pair_.symbol},
        {"props", {{"venue", pair_.exchange}, {"reason", "exhausted"}}}}.dump());
    set_rt_mode(false);
    const std::string detail = "That was today's free 3 minutes of real-time. Pro unlocks live depth on " +
        subject + " and every other market, all day. The free preview resets at 00:00 UTC.";
    auto& modal = ui::UpsellModal::instance();
    modal.open(ui::UpsellModal::Trigger::Layer, detail.c_str(), "realtime_depth", "preview_end");
    modal.set_subject(subject.c_str());
    return true;
}

// True only inside toggle_rt_mode: the viewer clicked the pill. A ?realtime=1
// link, a restored workspace, a recorder view or a context swap re-arming it all
// reach set_rt_mode without a click. Those automatic openings may be what sold
// user 291 (2026-10-10, locks on page load), so they are labelled, not suppressed.
static bool g_rt_toggle_by_viewer = false;

void ChartWidget::toggle_rt_mode() {
    g_rt_toggle_by_viewer = true;
    set_rt_mode(!rt_mode_);
    g_rt_toggle_by_viewer = false;
}

void ChartWidget::set_rt_mode(bool on) {
    if (on && rt_mode_locked()) {
        // Today's preview is spent. Name the market they are on; only Binance BTC
        // is free, so a locked BTC (Bybit, Hyperliquid) is named with its venue,
        // never "free on BTC" beside a lock on BTC.
        const std::string subject = rt_subject(pair_);
        const std::string detail = "You've used today's free 3 minutes of real-time. Live depth is always free on "
            "Binance BTC; Pro unlocks it on " + subject + " and every other market. The free preview resets at 00:00 UTC.";
        auto& modal = ui::UpsellModal::instance();
        modal.open(ui::UpsellModal::Trigger::Layer, detail.c_str(), "realtime_depth",
                   g_rt_toggle_by_viewer ? "click" : "auto");
        modal.set_subject(subject.c_str());
        return;
    }
    if (on && !rt_mode_ && rt_mode_gated()) {
        // A Pro market on the free daily preview: it runs, the pill counts down,
        // and the upsell waits until the time is used (tick_rt_preview).
        const int64_t left = rt_preview::Store::instance().remaining();
        usage::dispatch_detail(nlohmann::json{
            {"event", "rt_preview_start"}, {"mode", "terminal"}, {"symbol", pair_.symbol},
            {"props", {{"venue", pair_.exchange}, {"remaining_ms", left},
                       {"cause", g_rt_toggle_by_viewer ? "click" : "auto"}}}}.dump());
        const std::string msg = "Free real-time preview on " + rt_subject(pair_) + ": " +
            rt_preview::format_left(left) + " left today. Binance BTC is always free.";
        ui::UpsellModal::instance().toast(msg.c_str());
    }
    // heatmap_enabled_ (candle depth heatmap) is left alone in both directions:
    // real-time depth has its own flag, so a round trip keeps the candle choice.
    // The chart type is not: real-time draws Line / 1s candles, so the type the
    // user had is captured on the rising edge and put back on exit (a re-arm
    // while already on, e.g. a replay context swap, keeps the captured type).
    if (on && !rt_mode_ && !ctx_.replay_mgr().is_active() && !EducationBoot::instance().is_embedded() && !EducationBoot::instance().is_pack()) {
        rt_usage_emit_s_ = -1;
        EM_ASM({
            window.dispatchEvent(new CustomEvent('edgedepth:depth-frame', {detail: {
                symbol: UTF8ToString($0), venue: UTF8ToString($1), instance: $2, requested: true, valid: false
            }}));
        }, pair_.symbol.c_str(), pair_.exchange.c_str(), chart_instance_);
    }
    if (on && !rt_mode_) rt_prev_chart_type_ = chart_type_;
    const bool leaving = !on && rt_mode_;
    // Replay, packs and lessons put the scenario map back on leaving real-time, as before; live charts keep
    // the viewer's own liquidation map switch.
    if (leaving && !live_chart()) liq_dense_field_ = true;
    rt_mode_ = on;
    if (leaving) apply_chart_type(rt_prev_chart_type_);
    if (on && !rt_archive_) rt_archive_ = RealtimeArchive::acquire(candles(), ctx_.ob_mgr(), pair_);
    // Keep recording across candle/RT switches. Chart closure or a context
    // change owns archive disposal and depth unsubscription.
    if (on) {
        chart_type_ = ChartType::Candles;
        rt_candles_ = true;
        rt_candle_seconds_ = 1;
        rt_auto_price_ = true;
        rt_price_free_ = false;
        rt_axis_scale_axis_ = -1;
        rt_auto_fit_ = {};
        rt_price_window_ = {};
        rt_effective_multiplier_ = rt_bucket_multiplier_;
        rt_paused_ = false;
        rt_paused_liquidations_.clear();
        rt_span_ms_ = realtime_default_span_ms;
        rt_unhealthy_since_ms_ = 0;
        if (!rt_flow_) {
            rt_flow_ = std::make_unique<TradeAtPriceAccumulator>();
            rt_flow_->init(pair_, ctx_.stream_mgr(), tick_size_, true);
        }
        candles().set_follow_live(true);
        if (!rt_subscribed_) {
            rt_stream_mgr_ = &ctx_.stream_mgr();
            rt_stream_mgr_->subscribe_direct({pair_, Terminal::Stream::Orderbook, 0}, this);
            rt_subscribed_ = true;
        }
    } else {
        rt_flow_.reset();
    }
}

// Navigation and panel visibility only. Overlay switches belong in Layers;
// recording diagnostics stay available without filling the everyday menu.
// ═══════════════════════════════════════════════════════════════════════════
// Phase 3: one-click layer layouts for the real-time view. Toggles only —
// the workspace autosave keeps whatever the user changes afterwards.
//   Scalp    - raw microstructure: 1s hollow candles, walls + pulls, CVD,
//              bubbles, fast alerts, estimates off (they are minutes-scale)
//   Swing    - structure: 1m solid candles, estimated liquidations + resting
//              depth on, noise layers off
//   Research - everything on, including the walls table and peak-hold ghosts
// ═══════════════════════════════════════════════════════════════════════════
void ChartWidget::apply_layer_preset(int preset) {
    rt_depth_enabled_ = true;
    rt_bubbles_ = preset != 1;
    rt_trade_line_ = preset == 2;
    rt_candles_ = true;
    chart_type_ = ChartType::Candles;
    rt_candles_hollow_ = preset != 1;
    rt_heikin_ = false;
    rt_cvd_enabled_ = preset != 1;
    rt_liq_levels_enabled_ = preset != 0;
    rt_cum_depth_enabled_ = preset != 0;
    rt_walls_enabled_ = true;
    rt_pulls_enabled_ = preset != 1;
    rt_walls_table_enabled_ = preset == 2;
    rt_peak_hold_ = preset == 2;
    cascade_alerts_enabled_ = true;
    liq_alerts_enabled_ = preset != 1;
    wall_alerts_enabled_ = preset == 2;
    rt_events_enabled_ = true;
    rt_slip_enabled_ = preset != 1;          // execution aid for scalp + research
    refill_alerts_enabled_ = preset == 2;
    set_rt_candle_seconds(preset == 0 ? 1 : preset == 1 ? 60 : 5);
}

// ─── User layer sets ("MY LAYER SETS" in the Layers menu) ───────────────────
// A set snapshots every real-time layer switch plus the candle bucket size, so
// a trader can flip between e.g. a scalping view and a liquidation view in one
// click without touching the fixed Scalp/Swing/Research presets. Captures are
// read-back tolerant: absent keys keep the current value.
workspace::Json ChartWidget::capture_layer_flags() const {
    return workspace::Json{{"depth", rt_depth_enabled_},      {"bubbles", rt_bubbles_},
        {"trade_line", rt_trade_line_},      {"candles", rt_candles_},
        {"hollow", rt_candles_hollow_},      {"heikin", rt_heikin_},
        {"cvd", rt_cvd_enabled_},            {"est_liq", rt_liq_levels_enabled_},
        {"resting", rt_cum_depth_enabled_},  {"walls", rt_walls_enabled_},
        {"pulls", rt_pulls_enabled_},        {"walls_table", rt_walls_table_enabled_},
        {"peak_hold", rt_peak_hold_},        {"refill", rt_refill_enabled_},
        {"liq_strip", rt_liq_strip_},        {"liq_markers", rt_liq_markers_},
        {"alerts_cascade", cascade_alerts_enabled_}, {"alerts_liq", liq_alerts_enabled_},
        {"alerts_wall", wall_alerts_enabled_},       {"alerts_refill", refill_alerts_enabled_},
        {"events", rt_events_enabled_},              {"slip", rt_slip_enabled_},
        {"tf", rt_candle_seconds_}};
}

void ChartWidget::apply_layer_flags(const workspace::Json& f) {
    const auto flag = [&](const char* key, bool& value) {
        const auto it = f.find(key);
        if (it != f.end() && it->is_boolean()) value = it->get<bool>();
    };
    flag("depth", rt_depth_enabled_);
    flag("bubbles", rt_bubbles_);
    flag("trade_line", rt_trade_line_);
    flag("candles", rt_candles_);
    flag("hollow", rt_candles_hollow_);
    flag("heikin", rt_heikin_);
    flag("cvd", rt_cvd_enabled_);
    flag("est_liq", rt_liq_levels_enabled_);
    flag("resting", rt_cum_depth_enabled_);
    flag("walls", rt_walls_enabled_);
    flag("pulls", rt_pulls_enabled_);
    flag("walls_table", rt_walls_table_enabled_);
    flag("peak_hold", rt_peak_hold_);
    flag("refill", rt_refill_enabled_);
    flag("liq_strip", rt_liq_strip_);
    flag("liq_markers", rt_liq_markers_);
    flag("alerts_cascade", cascade_alerts_enabled_);
    flag("alerts_liq", liq_alerts_enabled_);
    flag("alerts_wall", wall_alerts_enabled_);
    flag("alerts_refill", refill_alerts_enabled_);
    flag("events", rt_events_enabled_);
    flag("slip", rt_slip_enabled_);
    if (rt_mode_) chart_type_ = rt_candles_ ? ChartType::Candles : ChartType::Line;
    const auto tf = f.find("tf");
    if (tf != f.end() && tf->is_number_integer())
        set_rt_candle_seconds(std::clamp(tf->get<int>(), 1, 300));
}

void ChartWidget::render_realtime_settings() {
    using drawing::UiIcon;
    if (!ctx_.replay_mgr().is_active() && Theme::menu_toggle(i18n::tr("Pause display", i18n::fa_pause_display), rt_paused_)) {
        rt_paused_ = !rt_paused_;
        if (rt_paused_) {
            rt_paused_trades_ = candles().realtime_trades().trades();
            rt_paused_liquidations_ = ctx_.liq_heatmap_mgr().observed_events(pair_);
            if (rt_archive_) rt_archive_->cancel_view();
            rt_query_from_ = 0; // A cancelled request is not loaded coverage.
        } else { rt_paused_trades_.clear(); rt_paused_liquidations_.clear(); }
    }
    ui_test::mark("rt.pause");
    if (Theme::menu_toggle(i18n::tr("Follow price", i18n::fa_follow_price), rt_auto_price_)) rt_auto_price_ = !rt_auto_price_;
    if (ImGui::IsItemHovered()) Theme::tooltip("Keep price in view. Drag the price axis to inspect manually.");
    if (Theme::menu_item(UiIcon::Play, i18n::tr("Return live", i18n::fa_return_live))) {
        rt_paused_ = false;
        rt_paused_trades_.clear(); rt_paused_liquidations_.clear();
        rt_span_ms_ = realtime_default_span_ms; rt_auto_price_ = true;
        rt_price_window_ = {}; rt_auto_fit_ = {};
        candles().set_follow_live(true);
    }
    ui_test::mark("rt.live");
    if (rt_archive_ && Theme::menu_item(UiIcon::Range, i18n::tr("Whole session", i18n::fa_whole_session))) {
        rt_span_ms_ = double(RealtimeArchive::target_ms) / 0.88;
        candles().set_follow_live(true);
    }
    ui_test::mark("rt.whole");
    ImGui::TextDisabled("%s", i18n::tr("Heat window", i18n::fa_heat_window));
    if (ImGui::IsItemHovered())
        Theme::tooltip("How much live session the heat and candles show behind the live edge.");
    for (const int mins : {1, 5, 15, 30}) {
        if (mins != 1) ImGui::SameLine();
        ImGui::PushID(mins + 1000);  // distinct from the Candle-size row IDs
        const double want = double(mins) * 60000.0 / 0.88;
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "%dm", mins);
        const bool on = rt_span_ms_ > want - 1500.0 && rt_span_ms_ < want + 1500.0;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
        if (ImGui::SmallButton(lbl)) {
            rt_span_ms_ = want;
            candles().set_follow_live(true);
        }
        if (on) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    Theme::menu_separator();
    if (Theme::menu_toggle(i18n::tr("Depth ladder", i18n::fa_depth_ladder), rt_dom_shown_)) rt_dom_shown_ = !rt_dom_shown_;
    ui_test::mark("rt.ladder");
    if (Theme::menu_toggle(i18n::tr("Side panels", i18n::fa_side_panels), rt_show_standalone_)) rt_show_standalone_ = !rt_show_standalone_;
    if (ImGui::IsItemHovered()) Theme::tooltip("Show the separate DOM and trade panels beside this view.");
    if (Theme::menu_toggle(i18n::tr("Link charts", i18n::fa_link_charts), link_charts_enabled())) set_link_charts_enabled(!link_charts_enabled());
    if (ImGui::IsItemHovered())
        Theme::tooltip("Sync crosshair and zoom across every open chart, any symbols.\nThe chart under the mouse drives the group; real-time views share\nthe zoom span, candle charts share the full axis.");
    if (rt_candles_) {
        ImGui::TextDisabled("%s", i18n::tr("Candle size", i18n::fa_candle_size));
        if (ImGui::IsItemHovered())
            Theme::tooltip("Aggregate the live trade flow into buckets of this length, 1s to 5m.");
        for (const int sec : {1, 5, 15, 30, 60, 300}) {
            if (sec != 1) ImGui::SameLine();
            ImGui::PushID(sec);  // "1m"/"5m" also exist in the Heat window row
            const bool on = rt_candle_seconds_ == sec;
            char lbl[8];
            if (sec >= 60) snprintf(lbl, sizeof(lbl), "%dm", sec / 60);
            else snprintf(lbl, sizeof(lbl), "%ds", sec);
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            if (ImGui::SmallButton(lbl)) set_rt_candle_seconds(sec);
            if (on) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        if (Theme::menu_toggle(i18n::tr("Hollow candles", i18n::fa_l_hollow), rt_candles_hollow_)) rt_candles_hollow_ = !rt_candles_hollow_;
        if (ImGui::IsItemHovered()) Theme::tooltip("Outline candle bodies so the depth heatmap stays visible behind them.");
        {
            // [SELF-HOST] How much live history the browser keeps for scrolling
            // back / review. The zoom ceilings and the IndexedDB worker budget
            // follow RealtimeArchive::target_ms; raising it records from now on.
            const char* const kArchiveLabels[] = {i18n::tr("30 min", i18n::fa_arch_30m), i18n::tr("2 h", i18n::fa_arch_2h),
                                                   i18n::tr("6 h", i18n::fa_arch_6h), i18n::tr("12 h", i18n::fa_arch_12h)};
            static constexpr int64_t kArchiveMs[] = {1800000, 2LL * 3600000, 6LL * 3600000, 12LL * 3600000};
            int idx = 0;
            for (int i = 1; i < 4; ++i) if (RealtimeArchive::target_ms >= kArchiveMs[i]) idx = i;
            const int next = Theme::menu_choice_row(i18n::tr("Archive depth", i18n::fa_archive_depth), "rt_archive_depth", kArchiveLabels, 4, idx);
            if (next != idx) RealtimeArchive::set_target_ms(kArchiveMs[next]);
            if (ImGui::IsItemHovered())
                Theme::tooltip("How much real-time history this browser records: 30 minutes up to 12 hours.\n"
                               "Higher values widen how far you can scroll back (and use more browser storage).\n"
                               "Recording starts from the moment it is raised; the choice persists.");
        }
    }
    ImGui::TextDisabled("%s", i18n::tr("Alerts", i18n::fa_sec_alerts));
    if (Theme::menu_toggle(i18n::tr("Cascade alerts", i18n::fa_cascade_alerts), cascade_alerts_enabled_)) cascade_alerts_enabled_ = !cascade_alerts_enabled_;
    if (ImGui::IsItemHovered()) Theme::tooltip("Notify when 3+ same-side liquidations fire within 2 seconds (cascade chain).");
    if (Theme::menu_toggle(i18n::tr("Big liquidations", i18n::fa_big_liq_alerts), liq_alerts_enabled_)) liq_alerts_enabled_ = !liq_alerts_enabled_;
    if (ImGui::IsItemHovered())
        Theme::tooltip("Notify on a single liquidation at or above the dollar threshold below.");
    if (liq_alerts_enabled_) {
        ImGui::SetNextItemWidth(170.0f);
        if (ImGui::InputDouble(i18n::tr("Threshold ($)", i18n::fa_threshold_usd), &liq_alert_usd_, 50000.0, 250000.0, "$%.0f"))
            liq_alert_usd_ = liq_alert_usd_ < 10000.0 ? 10000.0 : liq_alert_usd_;
    }
    if (Theme::menu_toggle(i18n::tr("Wall pulls", i18n::fa_wall_pulls), wall_alerts_enabled_)) wall_alerts_enabled_ = !wall_alerts_enabled_;
    if (ImGui::IsItemHovered())
        Theme::tooltip("Notify when a tracked depth wall near price is pulled (eaten or cancelled).");
    if (Theme::menu_toggle(i18n::tr("Refill consumed", i18n::fa_refill_consumed), refill_alerts_enabled_)) refill_alerts_enabled_ = !refill_alerts_enabled_;
    if (ImGui::IsItemHovered())
        Theme::tooltip("Notify when a tracked refill level's liquidity is traded through\n(exhausted) and the traded value clears the threshold below.");
    if (refill_alerts_enabled_) {
        ImGui::SetNextItemWidth(170.0f);
        if (ImGui::InputDouble(i18n::tr("Refill threshold ($)", i18n::fa_refill_threshold), &refill_alert_usd_, 50000.0, 250000.0, "$%.0f"))
            refill_alert_usd_ = refill_alert_usd_ < 10000.0 ? 10000.0 : refill_alert_usd_;
    }
    if (Theme::menu_toggle(i18n::tr("Drawing alerts", i18n::fa_drawing_alerts), drawing_alerts_enabled_)) drawing_alerts_enabled_ = !drawing_alerts_enabled_;
    if (ImGui::IsItemHovered())
        Theme::tooltip("Notify when the price crosses a drawn line: horizontal lines and rays fire at\ntheir level, trendlines and rays project to the current time. The line stays armed.");

    // ── Phase 5: execution & risk ──────────────────────────────────────────
    Theme::menu_separator();
    ImGui::TextDisabled("%s", i18n::tr("Execution", i18n::fa_exec_group));
    if (Theme::menu_toggle(i18n::tr("Paper on alerts", i18n::fa_paper_on_alerts), paper_on_alerts_)) paper_on_alerts_ = !paper_on_alerts_;
    if (ImGui::IsItemHovered())
        Theme::tooltip("Open a local paper position whenever a price or drawing alert fires.\nEntry at the alert price, stop at the % below, TP1 = 1R, TP2 = 2R, then a\ntrailing runner. Quantity is risk-based (account x risk% / stop distance).\nSee the Paper Trading widget for the open positions and daily stats.");
    {
        auto& ps = position_sizing::settings();
        ImGui::SetNextItemWidth(170.0f);
        if (ImGui::InputDouble(i18n::tr("Account ($)", i18n::fa_account_usd), &ps.account_usd, 1000.0, 5000.0, "$%.0f"))
            position_sizing::clamp_settings(ps);
        if (ImGui::IsItemHovered()) Theme::tooltip("Account equity used by the size calculator on position drawings\nand by the alert paper orders.");
        ImGui::SetNextItemWidth(170.0f);
        if (ImGui::InputDouble(i18n::tr("Risk per trade (%)", i18n::fa_risk_per_trade), &ps.risk_pct, 0.1, 0.5, "%.2f%%"))
            position_sizing::clamp_settings(ps);
        if (ImGui::IsItemHovered()) Theme::tooltip("Risk per trade as a percentage of the account.");
    }
    if (paper_on_alerts_) {
        ImGui::SetNextItemWidth(170.0f);
        if (ImGui::InputDouble(i18n::tr("Paper stop (%)", i18n::fa_paper_stop), &paper_auto_stop_pct_, 0.1, 0.5, "%.2f%%"))
            paper_auto_stop_pct_ = std::clamp(paper_auto_stop_pct_, 0.05, 20.0);
    }
    {
        auto& wb = exec_bridge::settings();
        if (Theme::menu_toggle(i18n::tr("Webhook bridge", i18n::fa_webhook_bridge), wb.enabled)) wb.enabled = !wb.enabled;
        if (ImGui::IsItemHovered())
            Theme::tooltip("POST a JSON message to your own bot for every alert\n(and paper opens/closes when enabled below). The target must\nallow CORS from this origin - self-hosted users point it at\ntheir own box or a proxy.");
        if (wb.enabled) {
            ImGui::SetNextItemWidth(170.0f);
            ImGui::InputText("##webhook_url", wb.url, sizeof(wb.url));
            if (ImGui::IsItemHovered()) Theme::tooltip("Webhook endpoint URL, e.g. http://your-box:8080/edgedepth");
            if (Theme::menu_toggle(i18n::tr("Include paper fills", i18n::fa_include_fills), wb.paper_fills)) wb.paper_fills = !wb.paper_fills;
            ImGui::TextDisabled("last: %s", exec_bridge::last_status().c_str());
        }
    }

    if (!rt_archive_) return;
    const auto& a = *rt_archive_;
    Theme::menu_separator();
    if (Theme::begin_menu_group(i18n::tr("Session history", i18n::fa_session_history))) {
        ImGui::TextDisabled(i18n::tr("Recorded %.1f min", i18n::fa_recorded_min), double(std::max(int64_t(0), a.last - a.first)) / 60000);
        if (ImGui::IsItemHovered()) Theme::tooltip("Up to 30 minutes in browser storage. Closing the chart clears this history.");
        if (!a.startup_status.empty()) ImGui::TextWrapped("%s", a.startup_status.c_str());
        const auto& b = a.bubbles();
        const unsigned long long skipped = b.approximate_records + b.late_records + b.overflow_records;
        if (skipped) {
            ImGui::TextDisabled(i18n::tr("Bubbles omitted: %llu records", i18n::fa_bubbles_omitted), skipped);
            if (ImGui::IsItemHovered())
                Theme::tooltip("%llu multi-price summaries, %llu late records, %llu over capture capacity.\nOriginal records remain in session history where storage is available.",
                    (unsigned long long)b.approximate_records, (unsigned long long)b.late_records,
                    (unsigned long long)b.overflow_records);
        }
        if (rt_trade_tail_waiting_) ImGui::TextWrapped("Loading older trades...");
        if (ImGui::SmallButton("Clear history")) rt_archive_->reset();
        Theme::end_menu_group();
    }
    // Actual capture failures must remain visible even with details collapsed.
    if (!a.error.empty() || a.dropped) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::WARN);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 250.0f);
        if (!a.error.empty()) ImGui::TextWrapped("%s", a.error.c_str());
        if (a.dropped) ImGui::TextWrapped("Capture overload: %zu records missed; depth gaps kept", a.dropped);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
}

void ChartWidget::on_rewind(int64_t) {
    liq_obs_size_reference_ = {};
    cascade_detector_.reset();  // the observed tail the chains were built from is gone
    alert_engine_.reset();      // S2: the queues belong to the same tails
    liq_estimator_.reset();     // S3: the tape the estimate was built from is gone
    last_pull_alert_ms_ = 0;
    // The replay owner restores/replays depth separately. Never keep future
    // samples or GPU cells from the preceding traversal.
    rt_dom_frame_ = {};
    rt_auto_fit_ = {}; rt_price_window_ = {};
    rt_archive_samples_.clear(); rt_archive_trades_.clear();
    rt_trade_tail_waiting_ = false;
    rt_history_view_ = false; rt_query_from_ = rt_query_to_ = rt_loaded_to_ = 0;
    rt_quote_ = {};
    if (rt_flow_) rt_flow_->reset();
    rt_unhealthy_since_ms_ = 0;
    rt_renderer_.reset();
    rt_latest_.reset();
    rt_last_good_.reset();
    rt_reported_failure_ = 0;
    rt_was_healthy_ = false;
    rt_pending_.clear();
    rt_samples_.clear();
    rt_serial_ = 0;
    rt_paused_ = false;
    rt_paused_trades_.clear();
    rt_book_valid_ = false;
    rt_bubble_scale_ = {};
    // [SELF-HOST P1] peak-hold ghosts belong to the discarded traversal too.
    rt_peak_levels_.clear();
    rt_peak_pruned_at_ms_ = 0;
    // [Phase 2 S5] the resting-depth accumulator belongs to it as well.
    rt_cum_levels_.clear();
    rt_cum_pruned_at_ms_ = 0;
    reset_realtime_extras();
    // Candle-mode large prints belong to the traversal being discarded too.
    candle_bubble_history_.reset();
    flow_history_.reset();
    candle_prints_.clear();
    candle_prints_seen_ms_ = 0; candle_prints_seen_count_ = 0;
    candle_bubble_scale_ = {}; candle_bubble_scale_since_ms_ = 0;
    candle_bubble_auto_floor_ = 0; candle_bubble_size_reference_ = 0;
}

void ChartWidget::update_realtime() {
    // First, before any real-time state is advanced: a spent preview leaves
    // real-time here, and the rest of this frame renders candles.
    if (tick_rt_preview()) return;
    if (rt_generation_ != ctx_.ob_mgr().realtime_generation()) {
        on_rewind(0);
        rt_generation_ = ctx_.ob_mgr().realtime_generation();
    }
    if (rt_paused_) { update_realtime_archive_view(); return; }
    if (!(tick_size_ > 0)) return;
    if (!rt_renderer_) {
        rt_renderer_ = std::make_unique<ShaderHeatmapRenderer>();
        rt_renderer_->configure_realtime(tick_size_);
        rt_pending_.reserve(RealtimeDepthHistory::max_samples);
        rt_prices_.reserve(1024);
    }
    const int64_t clock = ctx_.replay_mgr().is_active()
        ? ctx_.replay_mgr().interpolated_time_ms()
        : std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    if (rt_flow_) {
        rt_flow_->set_tick_size(tick_size_);
        rt_flow_->advance_to(clock);
    }
    if (clock != rt_clock_ms_ || !ctx_.replay_mgr().is_paused()) {
        const auto quote = ctx_.ob_mgr().realtime_quote_as_of(pair_, clock);
        rt_quote_ = quote.quote;
        rt_quote_bid_since_ms_ = quote.bid_since_ms;
        rt_quote_ask_since_ms_ = quote.ask_since_ms;
    }
    rt_clock_ms_ = clock;
    update_realtime_archive_view();
    rt_renderer_->set_observation_clock_ms(clock);
    RealtimeDepthHistory::SyncFailure failure;
    rt_book_valid_ = ctx_.ob_mgr().copy_realtime_since(pair_, rt_serial_, rt_pending_, &failure);
    // Samples past the as-of clock stay pending.
    for (const auto& sample : rt_pending_) {
        if (sample->timestamp_ms > clock) break;
        rt_serial_ = sample->serial;
        if (sample->timestamp_ms <= clock - RealtimeDepthHistory::retention_ms) continue;
        rt_prices_.clear();
        for (const auto& level : sample->levels) rt_prices_[level.price] += float(level.size);
        // ── [SELF-HOST P1] Peak-hold accumulation ────────────────────────
        // Decayed per-bucket maxima keyed by the absolute bucket index, so a
        // wall that pulled keeps a ghost bar for the horizon regardless of
        // the GPU ring's drifting per-column price windows.
        if (rt_peak_hold_ && tick_size_ > 0) {
            const double tau = std::max(1.0, double(rt_peak_horizon_min_) * 60000.0 / 3.0);
            for (const auto& level : sample->levels) {
                const int64_t key = std::llround(level.price / tick_size_);
                auto& entry = rt_peak_levels_[key];
                const double dt = double(sample->timestamp_ms - entry.second);
                entry.first = std::max(entry.first * float(std::exp(-dt / tau)), float(level.size));
                entry.second = sample->timestamp_ms;
            }
            if (clock - rt_peak_pruned_at_ms_ > 5000) {
                const int64_t cutoff = clock - 2 * int64_t(rt_peak_horizon_min_) * 60000;
                for (auto it = rt_peak_levels_.begin(); it != rt_peak_levels_.end();)
                    it = it->second.second < cutoff ? rt_peak_levels_.erase(it) : std::next(it);
                rt_peak_pruned_at_ms_ = clock;
            }
        }
        // ── [Phase 2 S5] Cumulative resting-depth accumulation ────────────
        // An exponential average of the size each level holds (≈10-minute
        // memory): depth that sits - shelves, walls - keeps its value while
        // flicker fades. Always accumulated; the decay of levels absent from
        // a sample happens lazily at draw time from the last-seen stamp.
        if (tick_size_ > 0) {
            for (const auto& level : sample->levels) {
                const int64_t key = std::llround(level.price / tick_size_);
                auto& entry = rt_cum_levels_[key];
                const double dt = std::clamp(double(sample->timestamp_ms - entry.second), 1.0, 5000.0);
                entry.first += (float(level.size) - entry.first) * float(1.0 - std::exp(-dt / kCumTauMs));
                entry.second = sample->timestamp_ms;
            }
            if (clock - rt_cum_pruned_at_ms_ > 5000) {
                const int64_t cutoff = clock - kCumRetentionMs;
                for (auto it = rt_cum_levels_.begin(); it != rt_cum_levels_.end();)
                    it = it->second.second < cutoff ? rt_cum_levels_.erase(it) : std::next(it);
                rt_cum_pruned_at_ms_ = clock;
            }
        }
        if (!rt_history_view_ || realtime_live_edge()) rt_renderer_->finalize_column(sample->timestamp_ms, rt_prices_, sample->segment_start, (sample->bid + sample->ask) * 0.5, sample->source_bucket_ticks);
        rt_latest_ = sample;
        rt_samples_.push_back(sample);
        if (rt_history_view_ && realtime_live_edge() &&
            (rt_archive_samples_.empty() || sample->timestamp_ms > rt_archive_samples_.back()->timestamp_ms))
            append_realtime_depth_sample(rt_archive_samples_, sample, rt_loaded_step_);
        while (rt_samples_.size() > RealtimeDepthHistory::max_samples || rt_samples_.front()->timestamp_ms <= clock - RealtimeDepthHistory::retention_ms)
            rt_samples_.pop_front();
    }
    if (!rt_book_valid_ && rt_latest_) {
        // Discard the final sampling bin on interruption rather than carrying
        // it through an invalid sequence inside that bin.
        if (!rt_history_view_ || realtime_live_edge()) rt_renderer_->invalidate_observation(rt_latest_->timestamp_ms);
        if (!rt_samples_.empty() && rt_samples_.back() == rt_latest_) rt_samples_.pop_back();
        if (!rt_archive_samples_.empty() && rt_archive_samples_.back() == rt_latest_)
            rt_archive_samples_.pop_back();
        rt_latest_.reset();
    }
    const bool healthy = rt_book_valid_ && rt_latest_ && rt_latest_->timestamp_ms <= clock &&
        clock - rt_latest_->timestamp_ms <= 15000;
    if (healthy) rt_last_good_ = rt_latest_;
    if (!ctx_.replay_mgr().is_active() &&
        (failure.count != rt_reported_failure_ || (rt_was_healthy_ && !healthy))) {
        const char* reason = failure.count != rt_reported_failure_ || !rt_book_valid_
            ? failure.reason : "Depth observation stale";
        // A bounded, local diagnostic survives recoveries too brief to capture.
        // Sequence IDs are strings so browser tooling cannot round int64 IDs.
        char ids[192];
        snprintf(ids, sizeof(ids), "%lld/%lld/%lld/%lld", (long long)failure.prior,
            (long long)failure.first, (long long)failure.last, (long long)failure.previous);
        EM_ASM({
            const event = ({symbol: UTF8ToString($0), venue: UTF8ToString($1),
                reason: UTF8ToString($2), observedAt: $3, sourceAt: $4,
                sequencePriorFirstLastPrevious: UTF8ToString($5), bid: $6, ask: $7,
                lastGoodAt: $8, recovered: !!$9});
            const events = Module['depthRecoveryEvents'] || (Module['depthRecoveryEvents'] = []);
            events.push(event);
            if (events.length > 20) events.shift();
            console.warn('Depth recovery', event);
        }, pair_.symbol.c_str(), pair_.exchange.c_str(), reason, double(clock),
            double(failure.timestamp_ms), ids, failure.bid, failure.ask,
            rt_last_good_ ? double(rt_last_good_->timestamp_ms) : 0, healthy);
    }
    rt_reported_failure_ = failure.count;
    rt_was_healthy_ = healthy;
    if (healthy || ctx_.replay_mgr().is_active()) rt_unhealthy_since_ms_ = 0;
    else {
        if (!rt_unhealthy_since_ms_) rt_unhealthy_since_ms_ = clock;
        // A known break needs a seed immediately. The shared stream owner
        // coalesces requests across charts/archive and enforces its cooldown.
        if ((!rt_book_valid_ && rt_last_good_) || clock - rt_unhealthy_since_ms_ >= 3000)
            ctx_.stream_mgr().refresh_orderbook({pair_, Terminal::Stream::Orderbook, 0}, clock);
    }
    rt_renderer_->set_observation_hold((!rt_history_view_ || realtime_live_edge()) && rt_book_valid_ && rt_latest_ &&
        clock - rt_latest_->timestamp_ms <= 15000 ? clock : 0,
        rt_latest_ ? rt_latest_->timestamp_ms : 0);

}

const std::deque<Terminal::Trade>& ChartWidget::realtime_trades() const {
    if (rt_history_view_) return rt_archive_trades_;
    return rt_paused_ ? rt_paused_trades_ : candles().realtime_trades().trades();
}

void ChartWidget::render_realtime() {
    ProfileScope profile("RT overlays");
    // Only the visible chart's render path, at most once per second. A valid
    // book reaches the renderer; this is not a GPU pixel-readback assertion.
    const double usage_now = ImGui::GetTime();
    if (!ctx_.replay_mgr().is_active() && !EducationBoot::instance().is_embedded() && !EducationBoot::instance().is_pack() && usage_now - rt_usage_emit_s_ >= 1.0) {
        rt_usage_emit_s_ = usage_now;
        const bool observing = rt_depth_enabled_ && (!rt_history_view_ || realtime_live_edge()) && !rt_paused_;
        const bool valid = observing &&
            rt_book_valid_ && rt_latest_ && rt_latest_->timestamp_ms <= rt_clock_ms_ &&
            rt_clock_ms_ - rt_latest_->timestamp_ms <= 15000;
        EM_ASM({
            window.dispatchEvent(new CustomEvent('edgedepth:depth-frame', {detail: {
                symbol: UTF8ToString($0), venue: UTF8ToString($1), instance: $2, valid: !!$3, observing: !!$4
            }}));
        }, pair_.symbol.c_str(), pair_.exchange.c_str(), chart_instance_, valid, observing);
    }
    const auto limits = ImPlot::GetPlotLimits();
    if (!candles().follow_live()) rt_span_ms_ = std::clamp(limits.X.Size(), 5000.0, (double(RealtimeArchive::target_ms) / 0.88));
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    const auto& trades = realtime_trades();
    const bool fresh = rt_book_valid_ && rt_latest_ &&
        rt_latest_->timestamp_ms <= rt_clock_ms_ && rt_clock_ms_ - rt_latest_->timestamp_ms <= 15000;
    rt_dom_frame_.book = fresh ? rt_latest_ : rt_last_good_;
    rt_dom_frame_.quote = rt_quote_;
    rt_dom_frame_.frame = ImGui::GetFrameCount();
    rt_dom_frame_.clock_ms = rt_clock_ms_;
    rt_dom_frame_.native_tick = rt_renderer_ ? rt_renderer_->get_native_bucket_size() : 0;
    rt_dom_frame_.automatic_grouping = rt_auto_fit_history_ && rt_dom_linked_;
    rt_dom_frame_.bucket_ticks = rt_renderer_ ? rt_renderer_->get_bucket_multiplier() : rt_bucket_multiplier_;
    rt_dom_frame_.price_min = limits.Y.Min;
    rt_dom_frame_.price_max = limits.Y.Max;
    rt_dom_frame_.top = ImPlot::GetPlotPos().y;
    rt_dom_frame_.bottom = rt_dom_frame_.top + ImPlot::GetPlotSize().y;
    rt_dom_frame_.synchronized = rt_book_valid_;
    rt_dom_frame_.recovery_enabled = rt_bridge_gaps_;
    rt_dom_frame_.replay = ctx_.replay_mgr().is_active();
    rt_dom_frame_.flow = rt_flow_.get();
    rt_dom_frame_.paused = rt_paused_ || ctx_.replay_mgr().is_paused();
    const bool recovering = rt_dom_frame_.recovery_visible();
    if (recovering && rt_renderer_ && rt_depth_enabled_ && (!rt_history_view_ || realtime_live_edge())) {
        rt_prices_.clear();
        for (const auto& level : rt_last_good_->levels) rt_prices_[level.price] += float(level.size);
        rt_renderer_->render_recovery_preview(rt_prices_, (rt_last_good_->bid + rt_last_good_->ask) * 0.5,
            heatmap_sensitivity_, rt_last_good_->timestamp_ms,
            rt_extend_depth_ ? realtime_projection_until(rt_clock_ms_, rt_span_ms_) : double(rt_clock_ms_));
    }
    if (rt_depth_enabled_ && rt_extend_depth_ && (!rt_history_view_ || realtime_live_edge()) && (fresh || recovering) &&
        limits.X.Max > rt_clock_ms_) {
        const float edge = std::max(ImPlot::GetPlotPos().x,
            ImPlot::PlotToPixels(double(rt_clock_ms_), 0).x);
        dl->AddLine(ImVec2(edge, ImPlot::GetPlotPos().y),
            ImVec2(edge, ImPlot::GetPlotPos().y + ImPlot::GetPlotSize().y), Theme::u32(Theme::Tokens::TX2, 0.35f));
        const float room = ImPlot::GetPlotPos().x + ImPlot::GetPlotSize().x - edge - 10;
        const char* full_label = recovering ? "Last known depth" : "Current depth";
        const char* label = room >= ImGui::CalcTextSize(full_label).x ? full_label : recovering ? "Held" : "Now";
        if (room >= ImGui::CalcTextSize(label).x)
            dl->AddText(ImVec2(edge + 5, ImPlot::GetPlotPos().y + 5), Theme::u32(Theme::Tokens::TX2), label);
    }

    // Display bridges get a small marker, not a black stripe through
    // the book. Original source boundaries remain available underneath.
    if (rt_depth_enabled_) {
        const auto& samples = realtime_samples();
        const auto pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
        for (size_t i = 1; i < samples.size(); ++i) {
            const auto& before = samples[i - 1];
            const auto& after = samples[i];
            if (!after->segment_start || after->timestamp_ms > rt_clock_ms_ ||
                after->timestamp_ms < limits.X.Min || before->timestamp_ms > limits.X.Max) continue;
            const float left = std::max(pos.x, ImPlot::PlotToPixels(double(before->timestamp_ms), 0).x);
            const float right = std::min(pos.x + size.x, ImPlot::PlotToPixels(double(after->timestamp_ms), 0).x);
            if (right <= left) continue;
            if (rt_renderer_ && rt_renderer_->bridges_observation(after->timestamp_ms)) {
                dl->AddLine(ImVec2(left, pos.y + size.y - 3), ImVec2(right, pos.y + size.y - 3),
                    Theme::u32(Theme::Tokens::WARN, 0.7f), 2.0f);
                if (realtime_hover_ready() && ImGui::IsMouseHoveringRect(
                    ImVec2(left - 3, pos.y + size.y - 12), ImVec2(right + 3, pos.y + size.y)))
                    Theme::tooltip("Display bridge: %.1fs held from the last confirmed book. No depth was recorded in the gap.",
                        double(after->timestamp_ms - before->timestamp_ms) / 1000);
                continue;
            }
            if (right - left < 24) continue;
            dl->AddRectFilled(ImVec2(left, pos.y), ImVec2(right, pos.y + size.y),
                Theme::u32(Theme::Tokens::TX2, 0.035f));
            const char* label = "Depth unavailable";
            const auto text = ImGui::CalcTextSize(label);
            if (right - left > text.x + 16)
                dl->AddText(ImVec2((left + right - text.x) * 0.5f, pos.y + size.y - text.y - 10),
                    Theme::u32(Theme::Tokens::TX2), label);
            if (ImGui::GetIO().KeyAlt && realtime_hover_ready() && ImGui::GetIO().MousePos.x >= left &&
                ImGui::GetIO().MousePos.x < right) {
                Theme::begin_tooltip();
                ImGui::TextUnformatted("No continuous depth recorded in this interval");
                ImGui::TextUnformatted("Trades may still be available. Missing orders are not reconstructed.");
                Theme::end_tooltip();
            }
        }
    }

    // ── [SELF-HOST P1] Peak-hold ghost bars ──────────────────────────────
    // Subtle horizontal ghosts at the deepest level each visible price row
    // held within the horizon window, drawn under the quote lines so candles
    // and bubbles stay on top.
    if (rt_peak_hold_ && !rt_peak_levels_.empty() && tick_size_ > 0) {
        const ImVec2 pos = ImPlot::GetPlotPos();
        const ImVec2 size = ImPlot::GetPlotSize();
        float visible_max = 0;
        for (const auto& peak : rt_peak_levels_) {
            const double price = double(peak.first) * tick_size_;
            if (price < limits.Y.Min || price > limits.Y.Max) continue;
            visible_max = std::max(visible_max, peak.second.first);
        }
        if (visible_max > 0) {
            const float line_h = std::max(1.0f, size.y / 720.0f);
            int drawn = 0;
            for (const auto& peak : rt_peak_levels_) {
                if (drawn > 600) break;
                const double price = double(peak.first) * tick_size_;
                if (price < limits.Y.Min || price > limits.Y.Max) continue;
                const float intensity = peak.second.first / visible_max;
                if (intensity < 0.02f) continue;
                const ImVec2 p = ImPlot::PlotToPixels(limits.X.Min, price + tick_size_ * 0.5);
                const ImU32 col = ImGui::GetColorU32(
                    ImVec4(0.55f, 0.85f, 0.75f, 0.10f + 0.22f * intensity));
                dl->AddRectFilled(ImVec2(pos.x, p.y), ImVec2(pos.x + size.x, p.y + line_h), col);
                ++drawn;
            }
        }
    }

    // ── [Phase 2 S5] Cumulative resting depth — the second heat layer ────
    render_realtime_cum_depth(dl, limits);

    // ── [SELF-HOST P1] lightweight state bridge for tooling ─────────────
    // window.__ed_state mirrors the live chart twice a second; the snapshot
    // API and the egress bridge read it without touching the render loop.
    static int64_t s_state_pushed_at = 0;
    if (rt_clock_ms_ - s_state_pushed_at >= 500) {
        s_state_pushed_at = rt_clock_ms_;
        EM_ASM({
            const s = Module['edState'] || (Module['edState'] = {});
            s.symbol = UTF8ToString($0); s.venue = UTF8ToString($1);
            s.tf = $2; s.bid = $3; s.ask = $4; s.ts = $5;
        }, pair_.symbol.c_str(), pair_.exchange.c_str(), rt_candle_seconds_,
           double(rt_quote_.best_bid), double(rt_quote_.best_ask), double(rt_clock_ms_));
    }

    ImVec2 previous{};
    int64_t previous_ms = 0;
    // Last point per screen pixel bounds line geometry to the plot width.
    if (rt_trade_line_ && !rt_candles_ && !(rt_history_view_ && rt_archive_->view_trades_grouped)) for (const auto& trade : trades) {
        if (trade.timestamp_ms < limits.X.Min) continue;
        if (trade.timestamp_ms > rt_clock_ms_ || trade.timestamp_ms > limits.X.Max) break;
        const ImVec2 point = ImPlot::PlotToPixels(double(trade.timestamp_ms), trade.price);
        if (rt_trade_line_ && !rt_candles_ && previous_ms && point.x >= previous.x + 1.0f &&
            trade.timestamp_ms - previous_ms <= 1000)
            dl->AddLine(previous, point, Theme::u32(Theme::Tokens::TX1), 1.0f);
        if (!previous_ms || point.x >= previous.x + 1.0f || trade.timestamp_ms - previous_ms > 1000) {
            previous = point; previous_ms = trade.timestamp_ms;
        }
    }
    // Historical best bid/ask are steps at original observation timestamps.
    // Quiet intervals hold the prior quote; a new synchronization epoch breaks it.
    const RealtimeDepthHistory::Sample* previous_book = nullptr;
    auto draw_book = [&](const RealtimeDepthHistory::SamplePtr& sample) {
        if (sample->timestamp_ms < limits.X.Min) { previous_book = sample.get(); return; }
        if (previous_book && !sample->segment_start) {
            for (int side = 0; side < 2; ++side) {
                const double before = side ? previous_book->ask : previous_book->bid;
                const double after = side ? sample->ask : sample->bid;
                const ImVec2 a = ImPlot::PlotToPixels(double(previous_book->timestamp_ms), before);
                const ImVec2 b = ImPlot::PlotToPixels(double(sample->timestamp_ms), before);
                const ImVec2 c = ImPlot::PlotToPixels(double(sample->timestamp_ms), after);
                const ImU32 color = Theme::u32(side ? Theme::Tokens::DOWN : Theme::Tokens::UP);
                const ImU32 halo = Theme::u32(Theme::Tokens::BASE);
                dl->AddLine(a, b, halo, 3.5f);
                dl->AddLine(b, c, halo, 3.5f);
                dl->AddLine(a, b, color, 1.5f);
                dl->AddLine(b, c, color, 1.5f);
            }
        }
        previous_book = sample.get();
    };
    for (const auto& sample : realtime_samples()) {
        if (sample->timestamp_ms > rt_clock_ms_ || sample->timestamp_ms > limits.X.Max) break;
        draw_book(sample);
    }
    // Coarse archive bins omit the newest observations within their last bin.
    // Draw the retained raw tail without changing the bounded archive grid.
    if (rt_history_view_ && realtime_live_edge() && previous_book) {
        if (!rt_samples_.empty() && rt_samples_.front()->timestamp_ms > previous_book->timestamp_ms &&
            rt_samples_.front()->serial > 1) previous_book = nullptr;
        for (const auto& sample : rt_samples_) {
            if (sample->timestamp_ms > rt_clock_ms_ || sample->timestamp_ms > limits.X.Max) break;
            if (previous_book && sample->timestamp_ms <= previous_book->timestamp_ms) continue;
            draw_book(sample);
        }
    }
    if (rt_candles_ && !(rt_history_view_ && rt_archive_->view_trades_grouped)) {
        // OHLC from retained trade records only, aggregated into the selected
        // real-time bucket (1s by default; the toolbar offers 5s..5m). Empty
        // buckets stay absent; a paused frame uses its frozen records and the
        // same as-of clock. Heikin-Ashi (Phase 3) transforms each finished
        // bucket: open = midpoint of the previous HA candle, close = the raw
        // OHLC4 — the forming bucket gets the same treatment, so the last
        // candle eases into place exactly like the historical HA series.
        const int64_t bucket_ms = int64_t(std::max(1, rt_candle_seconds_)) * 1000;
        const double mid = 0.5 * double(bucket_ms), body = 0.35 * double(bucket_ms);
        Terminal::Candle candle{};
        bool have = false;
        double ha_open_prev = 0.0, ha_close_prev = 0.0;
        bool have_prev_ha = false;
        auto flush = [&]() {
            if (!have) return;
            double o = candle.open, h = candle.high, l = candle.low, c = candle.close;
            if (rt_heikin_) {
                const double close = (candle.open + candle.high + candle.low + candle.close) * 0.25;
                const double open = have_prev_ha ? 0.5 * (ha_open_prev + ha_close_prev)
                                                 : 0.5 * (candle.open + candle.close);
                o = open; c = close;
                h = std::max(candle.high, std::max(open, close));
                l = std::min(candle.low, std::min(open, close));
                ha_open_prev = open; ha_close_prev = close; have_prev_ha = true;
            }
            const ImVec2 hi = ImPlot::PlotToPixels(double(candle.timestamp_ms) + mid, h);
            const ImVec2 lo = ImPlot::PlotToPixels(double(candle.timestamp_ms) + mid, l);
            const ImVec2 open = ImPlot::PlotToPixels(double(candle.timestamp_ms) + mid - body, o);
            const ImVec2 close = ImPlot::PlotToPixels(double(candle.timestamp_ms) + mid + body, c);
            const ImU32 color = Theme::u32(c >= o ? Theme::Tokens::UP : Theme::Tokens::DOWN);
            dl->AddLine(hi, lo, color);
            if (rt_candles_hollow_) {
                // Outline only: the depth heatmap stays visible behind the body.
                dl->AddRect(ImVec2(open.x, std::min(open.y, close.y)),
                    ImVec2(close.x, std::max(open.y + 1, close.y)), color, 0.0f, 0, 1.25f);
            } else {
                dl->AddRectFilled(ImVec2(open.x, std::min(open.y, close.y)),
                    ImVec2(close.x, std::max(open.y + 1, close.y)), color);
            }
        };
        for (const auto& trade : trades) {
            if (trade.timestamp_ms < limits.X.Min - double(bucket_ms)) continue;
            if (trade.timestamp_ms > rt_clock_ms_ || trade.timestamp_ms > limits.X.Max) break;
            const int64_t bucket = trade.timestamp_ms / bucket_ms * bucket_ms;
            if (!have || bucket != candle.timestamp_ms) {
                flush(); candle = {}; candle.timestamp_ms = bucket;
                candle.open = candle.high = candle.low = candle.close = trade.price; have = true;
            } else {
                candle.high = std::max(candle.high, trade.price);
                candle.low = std::min(candle.low, trade.price); candle.close = trade.price;
            }
        }
        flush();
    }
    // Size and filter the same exact-price 100ms groups that are drawn. The
    // viewport and lossy archive display summaries never enter calibration.
    if (rt_auto_bubbles_ && rt_archive_) rt_bubble_scale_.update(rt_archive_->bubbles(), rt_clock_ms_);
    const double minimum = rt_auto_bubbles_
        ? (rt_bubble_scale_.settled() ? rt_bubble_scale_.minimum() : 0) : rt_min_notional_;
    // Refill levels' absorption bubbles swell behind the trades that hit them.
    render_refill_lenses(dl, limits, minimum);
    if (rt_bubbles_ && minimum > 0 && rt_archive_) {
        ProfileScope bubbles_profile("RT trade bubbles");
        // Immutable capture publications, not a ranking of the current viewport.
        // Neither archive refreshes nor camera movement can change a marker.
        size_t test_marks = 0;
        rt_archive_->bubbles().for_each(int64_t(limits.X.Min), int64_t(limits.X.Max), rt_clock_ms_,
            [&](const RealtimeBubbleHistory::Bubble& m) {
                if (m.value() < minimum) return;
                const ImVec4& major = m.buy_led() ? Theme::Tokens::UP : Theme::Tokens::DOWN;
                const ImVec4& minor = m.buy_led() ? Theme::Tokens::DOWN : Theme::Tokens::UP;
                const ImVec2 center = ImPlot::PlotToPixels(double(m.timestamp_ms), m.price);
                const float radius = RealtimeBubble::radius(m.value(), minimum, rt_bubble_cap_);
                RealtimeBubble::draw(*dl, center, radius, major, Theme::u32(Theme::Tokens::BASE),
                    RealtimeBubble::Fill::Opaque, float(m.minor()), minor, rt_bubble_alpha_);
                if (ui_test::enabled() && test_marks < 256)
                    ui_test::mark_rect("rt.bubble." + std::to_string(test_marks++),
                        ImVec2(center.x - radius, center.y - radius), ImVec2(center.x + radius, center.y + radius));
            });
    }
    // The signals over the trades: cascade bands, estimated liquidation
    // clusters (S3), wall marks and pulls, then refill marks on top of
    // everything but the quotes.
    render_realtime_cascades(dl, limits);
    render_realtime_liq_levels(dl, limits);
    render_realtime_walls(dl);
    render_refill_levels(dl, limits);
    // [SELF-HOST] flow extras: linked large-print trails, then the cross-venue
    // basis ribbon on top (it owns the plot's bottom band).
    render_realtime_whale_trail(dl, limits);
    render_realtime_basis(dl, limits);
    // [SELF-HOST] execution + events: the slippage cone at the live edge, the
    // layer-events corner feed last (a panel, on top of every overlay).
    render_realtime_slippage(dl, limits);
    render_realtime_event_feed(dl, limits);
    if (fresh && limits.X.Max > rt_clock_ms_) {
        const auto& book = *rt_latest_;
        // The dashed quotes stop where the current-depth projection stops.
        const float right = std::min(ImPlot::GetPlotPos().x + ImPlot::GetPlotSize().x,
            ImPlot::PlotToPixels(realtime_projection_until(rt_clock_ms_, rt_span_ms_), book.bid).x);
        const float edge = ImPlot::PlotToPixels(double(rt_clock_ms_), book.bid).x;
        // A dark halo under the current quotes, as under the quote history above, keeps them readable over any
        // depth colour (a blue bid over the blue end of the ramp vanished). Halos first, so a tight spread keeps
        // both colours.
        const ImU32 halo = Theme::u32(Theme::Tokens::BASE);
        for (int pass = 0; pass < 2; ++pass) for (int side = 0; side < 2; ++side) {
            const double price = side ? book.ask : book.bid;
            const double current_price = side ? rt_dom_frame_.ask() : rt_dom_frame_.bid();
            // The step sits where this side's price changed, not where the
            // newest ticker arrived, so it holds its place in time.
            const int64_t since = side ? rt_quote_ask_since_ms_ : rt_quote_bid_since_ms_;
            const double quote_ms = double(std::clamp(since > 0 ? since : rt_quote_.timestamp_ms, book.timestamp_ms, rt_clock_ms_));
            const ImU32 color = pass ? Theme::u32(side ? Theme::Tokens::DOWN : Theme::Tokens::UP) : halo;
            const float width = pass ? 1.25f : 3.25f;
            dl->AddLine(ImPlot::PlotToPixels(double(book.timestamp_ms), price),
                ImPlot::PlotToPixels(quote_ms, price), color, width);
            dl->AddLine(ImPlot::PlotToPixels(quote_ms, price),
                ImPlot::PlotToPixels(quote_ms, current_price), color, width);
            dl->AddLine(ImPlot::PlotToPixels(quote_ms, current_price),
                ImPlot::PlotToPixels(double(rt_clock_ms_), current_price), color, width);
        }
        for (int pass = 0; pass < 2; ++pass) for (int side = 0; side < 2; ++side) {
            const double price = side ? rt_dom_frame_.ask() : rt_dom_frame_.bid();
            const float y = ImPlot::PlotToPixels(0, price).y;
            const ImU32 color = pass ? Theme::u32(side ? Theme::Tokens::DOWN : Theme::Tokens::UP) : halo;
            for (float x = edge; x < right; x += 10)
                dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + 5, right), y), color, pass ? 1.0f : 3.0f);
        }
    }
    const bool archive_error = rt_archive_ && !rt_archive_->error.empty();
    char recovery_note[80];
    snprintf(recovery_note, sizeof(recovery_note), "Recovering depth / last book %.1fs old",
        recovering ? double(rt_clock_ms_ - rt_last_good_->timestamp_ms) / 1000 : 0);
    const char* note = archive_error ? "History recording unavailable" : rt_paused_ ? "Display paused" :
        recovering ? recovery_note : !fresh ? "Waiting for synchronized depth" : rt_archive_ && rt_archive_->loading ? "Loading history" :
        rt_history_view_ ? "Live depth / session history" : "Live depth";
    if (ctx_.replay_mgr().is_active() && fresh && !rt_paused_ && !archive_error)
        note = "Recorded depth";
    const ImVec2 pos = ImPlot::GetPlotPos();
    // One of the chart chips (status, rails key): a rounded chip with a dot, amber while depth is not in hand.
    const bool depth_warn = archive_error || !fresh;
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const float chip_px = ImGui::GetFontSize();
    ImGui::PopFont();
    Theme::push_font(Theme::Fonts::ui(), chip_px);
    const ImVec2 note_size = ImGui::CalcTextSize(note);
    const ImVec2 note_min(pos.x + 10, pos.y + (ctx_.replay_mgr().is_active() ? 58.0f : 10.0f));
    const ImVec2 note_max(std::round(note_min.x + 9 + 6 + 7 + note_size.x + 9), note_min.y + 22);
    dl->AddRectFilled(note_min, note_max, Theme::u32(Theme::Tokens::PANEL, 0.92f), Theme::Radius::R2);
    dl->AddRect(note_min, note_max, Theme::u32(Theme::Tokens::BD2), Theme::Radius::R2, 0, 1.0f);
    dl->AddCircleFilled(ImVec2(note_min.x + 12, note_min.y + 11), 3.0f,
        Theme::u32(depth_warn ? Theme::Tokens::WARN : fresh && !rt_paused_ ? Theme::Tokens::UP : Theme::Tokens::TX3), 12);
    dl->AddText(ImVec2(note_min.x + 22, std::round(note_min.y + 11 - note_size.y * 0.5f)),
        Theme::u32(depth_warn ? Theme::Tokens::WARN : Theme::Tokens::TX2), note);
    ImGui::PopFont();
    Theme::chart_chip(note_min, note_max);
    render_refill_status(dl);
    if (ImGui::IsMouseHoveringRect(note_min, note_max) && ImPlot::IsPlotHovered()) {
        Theme::begin_tooltip();
        ImGui::TextUnformatted("Depth averages received samples; the right margin holds the current book.");
        ImGui::TextUnformatted("During resync, the last confirmed book stays visible with its age until fresh depth arrives.");
        ImGui::TextUnformatted("Amber marks below the chart identify display bridges; recorded data stays unchanged.");
        ImGui::TextUnformatted("Trade bubbles use fixed 100ms groups, up to eight prices selected once.");
        if (rt_archive_ && !rt_archive_->startup_status.empty())
            ImGui::TextUnformatted(rt_archive_->startup_status.c_str());
        if (archive_error) ImGui::TextUnformatted(rt_archive_->error.c_str());
        ImGui::TextUnformatted("Open Session history in the Real-time menu for recording details.");
        Theme::end_tooltip();
    }
    if (!realtime_samples().empty()) {
        const auto first = realtime_samples().front()->timestamp_ms;
        if (first > limits.X.Min && first <= std::min(limits.X.Max, double(rt_clock_ms_))) {
            const float start = ImPlot::PlotToPixels(double(first), 0).x;
            const char* label = "No earlier depth";
            const auto text = ImGui::CalcTextSize(label);
            if (start - pos.x > text.x + 20) {
                dl->AddLine(ImVec2(start, pos.y), ImVec2(start, pos.y + ImPlot::GetPlotSize().y),
                    Theme::u32(Theme::Tokens::TX2, 0.3f));
                dl->AddText(ImVec2(start - text.x - 8, pos.y + ImPlot::GetPlotSize().y - text.y - 10),
                    Theme::u32(Theme::Tokens::TX2), label);
            }
        }
    }
    render_realtime_walls_table(dl);  // Phase 2 S4: top-right overlay panel
    // Replay priming. Embedded replays (events, lessons, /demo) cover the whole canvas with their own scrim and
    // spinner (main.cpp render_lesson_loading_overlay); a second one here showed through it, off centre.
    if (ctx_.replay_mgr().is_loading()) {
        const ImVec2 size = ImPlot::GetPlotSize();
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), Theme::u32(Theme::Tokens::BASE));
        if (!EducationBoot::instance().is_embedded()) {
            const ImVec2 center(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
            Theme::spinner(dl, center, 18.0f);
            const char* label = "Loading replay";
            const ImVec2 text_size = ImGui::CalcTextSize(label);
            dl->AddText(ImVec2(std::round(center.x - text_size.x * 0.5f), center.y + 32),
                        Theme::u32(Theme::Tokens::TX2), label);
        }
    }
    ImPlot::PopPlotClipRect();
}

void ChartWidget::configure_depth_fidelity(ShaderHeatmapRenderer& renderer) {
    if (rt_mode_) {
        // The chart owns one display grid for both historical depth and DOM.
        // Archive queries retain the selected minimum grouping for later detail.
        renderer.set_bucket_multiplier(rt_effective_multiplier_);
        renderer.set_realtime_auto_contrast(rt_auto_contrast_);
        return;
    }
    const double native_bucket = renderer.get_native_bucket_size();
    const auto limits = ImPlot::GetPlotLimits();
    int mult = heatmap_bucket_multiplier_;
    if (native_bucket > 0 && heatmap_adapt_to_zoom_) {
        // Round UP to readable rows, with no 100x ceiling on volatile markets.
        const double rows = std::clamp(double(ImPlot::GetPlotSize().y) / 2.5, 1.0, 900.0);
        const double needed = std::ceil(limits.Y.Size() / (native_bucket * rows));
        const int groups = int(std::clamp(std::ceil(needed / mult), 1.0, 1000000.0));
        mult *= groups;
        const int current = renderer.get_bucket_multiplier();
        if (mult < current && mult > current * 0.7) mult = current;
    }
    renderer.set_bucket_multiplier(mult);
    renderer.set_candle_price_window(limits.Y.Min, limits.Y.Max);

}


void ChartWidget::capture_realtime_archive() {
    if (!rt_archive_) rt_archive_ = RealtimeArchive::acquire(candles(), ctx_.ob_mgr(), pair_);
    const int64_t clock = ctx_.replay_mgr().is_active() ? ctx_.replay_mgr().interpolated_time_ms() :
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    rt_archive_->update(clock);
    if(!ctx_.replay_mgr().is_active())rt_archive_->request_startup(ctx_.stream_mgr());
}

void ChartWidget::rebuild_realtime_view() {
    if (!rt_renderer_) return;
    rt_renderer_->clear_realtime_view(rt_history_view_ ? rt_loaded_step_ : 100);
    rt_renderer_->set_observation_clock_ms(rt_clock_ms_);
    for (const auto& sample : realtime_samples()) {
        rt_prices_.clear();
        for (const auto& level : sample->levels) rt_prices_[level.price] += float(level.size);
        rt_renderer_->finalize_column(sample->timestamp_ms, rt_prices_, sample->segment_start, (sample->bid + sample->ask)*0.5, sample->source_bucket_ticks);
    }
}

bool ChartWidget::realtime_live_edge() const {
    return realtime_view_has_live_edge(candles().follow_live(), last_visible_range_.X.Max, rt_clock_ms_);
}

void ChartWidget::update_realtime_archive_view() {
    ProfileScope profile("RT archive view");
    if (!rt_archive_ || !rt_renderer_ || rt_clock_ms_<=0) return;
    if (rt_archive_generation_ != rt_archive_->generation) {
        rt_archive_generation_ = rt_archive_->generation;
        rt_archive_samples_.clear(); rt_archive_trades_.clear(); rt_query_from_ = rt_query_to_ = rt_loaded_to_ = 0;
        if (rt_history_view_) rebuild_realtime_view();
    }
    if (rt_archive_view_revision_ != rt_archive_->view_revision) {
        rt_archive_view_revision_ = rt_archive_->view_revision;
        // Startup/recovery added observations to this same source. Its old
        // query was cancelled; retain the displayed snapshot and live tail
        // until a complete replacement arrives, including while paused.
        rt_query_from_ = rt_query_to_ = 0;
    }
    const bool follow = candles().follow_live();
    const int64_t from = follow ? rt_clock_ms_ - int64_t(rt_span_ms_ * 0.88) : int64_t(last_visible_range_.X.Min);
    const int64_t to = follow ? rt_clock_ms_ : std::min(rt_clock_ms_, int64_t(last_visible_range_.X.Max));
    const auto& recent_trades = rt_paused_ ? rt_paused_trades_ : candles().realtime_trades().trades();
    const bool trades_retired = recent_trades.size() == RealtimeTradeHistory::max_trades &&
        recent_trades.front().timestamp_ms > from && rt_archive_->first < recent_trades.front().timestamp_ms;
    const bool seeded = rt_archive_->startup_first>0 && from<rt_archive_->startup_end;
    const bool history = to > from && (seeded || (rt_archive_ && rt_archive_->recovered_in(from, to)) || trades_retired || to-from > 290000 || from < rt_clock_ms_ - 290000);
    if (!history) {
        rt_trade_tail_waiting_ = false;
        // Keep the loaded archive and any pending response while inspecting
        // the recent ring. Zooming back out must not start with empty history.
        if (rt_history_view_) {
            rt_history_view_ = false;
            rebuild_realtime_view();
        }
        return;
    }
    if (!rt_history_view_ && rt_loaded_to_ > 0) {
        rt_history_view_ = true;
        if (realtime_live_edge())
            append_realtime_depth_tail(rt_archive_samples_, rt_samples_, rt_loaded_to_, rt_clock_ms_, rt_loaded_step_);
        rebuild_realtime_view();
    }
    const int64_t step = std::max(int64_t(seeded || rt_archive_->recovered_in(from, to) ? 500 : 100), ((to-from+179999)/180000)*100);
    const int64_t aligned = from / step * step;
    int64_t received_step = 100;
    std::deque<RealtimeDepthHistory::SamplePtr> received_samples;
    std::deque<Terminal::Trade> received_trades;
    const auto displayed_count = rt_archive_->view_trade_count;
    const bool displayed_grouped = rt_archive_->view_trades_grouped;
    if (rt_archive_->take_view(received_samples, received_trades, received_step)) {
        // Navigation may have changed while a worker query was in flight.
        const bool tail_already_retired = realtime_live_edge() &&
            realtime_trade_tail_retired(recent_trades, rt_query_to_);
        if (tail_already_retired || step != rt_query_step_ || rt_query_multiplier_ != rt_bucket_multiplier_ || !realtime_query_start_matches(follow, aligned, rt_query_from_, step)) {
            rt_archive_->view_trade_count = displayed_count;
            rt_archive_->view_trades_grouped = displayed_grouped;
            rt_query_from_ = 0;
        } else {
            // Keep the displayed data until a matching replacement is complete.
            rt_archive_samples_.swap(received_samples);
            rt_archive_trades_.swap(received_trades);
            rt_history_view_ = true;
            rt_loaded_to_ = rt_query_to_; rt_loaded_step_ = received_step;
            // Depth delivered while the query ran has already advanced rt_serial_.
            // Preserve that tail before rebuilding the GPU view.
            if (realtime_live_edge())
                append_realtime_depth_tail(rt_archive_samples_, rt_samples_, rt_loaded_to_, rt_clock_ms_, rt_loaded_step_);
            rebuild_realtime_view();
        }
    }
    // Join the query's as-of snapshot to the recent source by a strict time
    // boundary. Equal timestamps stay together in the archive snapshot; late
    // records at/before its cutoff appear on the next query, never deduplicated.
    bool tail_ready = true;
    if (realtime_live_edge() && rt_loaded_to_ > 0)
        tail_ready = refresh_realtime_trade_tail(rt_archive_trades_, recent_trades,
                                                rt_loaded_to_, rt_clock_ms_);
    rt_trade_tail_waiting_ = !tail_ready;
    if (!tail_ready && bound_realtime_trade_tail(rt_archive_trades_, recent_trades, rt_trade_view_))
        rt_archive_->view_trades_grouped = true;
    const double now = emscripten_get_now();
    if (!rt_archive_->loading && (!tail_ready || rt_query_from_==0 || step!=rt_query_step_ || rt_query_multiplier_!=rt_bucket_multiplier_ ||
        !realtime_query_start_matches(follow, aligned, rt_query_from_, step) || (!rt_paused_ && to>rt_query_to_ && now-rt_query_at_>5000))) {
        if (rt_archive_->query(aligned,to,rt_clock_ms_,step,tick_size_*rt_bucket_multiplier_,tick_size_)) {
            rt_query_from_=aligned;rt_query_to_=to;rt_query_step_=step;rt_query_at_=now;rt_query_multiplier_=rt_bucket_multiplier_;
        }
    }
}


const std::vector<Terminal::Liquidation>& ChartWidget::realtime_liquidations() const {
    return rt_mode_ && rt_paused_ ? rt_paused_liquidations_ : ctx_.liq_heatmap_mgr().observed_events(pair_);
}

// ═══════════════════════════════════════════════════════════════════════════
// Trade bubbles on candles (free)
//
// Real-time mode draws every received trade; this draws only the LARGE ones,
// over any time-based candle view, for every plan. Same marker and
// signed colours. Candle displays use their own compressed reference and
// screen-space selection so wide history cannot become a wall of equal discs.
// ═══════════════════════════════════════════════════════════════════════════

double ChartWidget::candle_bubble_floor() const {
    if (candle_bubble_min_ > 0) return candle_bubble_min_;
    return candle_bubble_auto_floor_ > 0 ? candle_bubble_auto_floor_ : candle_bubble_history_.auto_floor;
}

void ChartWidget::collect_candle_prints() {
    const auto& trades = candles().realtime_trades().trades();
    if (trades.empty()) return;
    const int64_t clock = trades.back().timestamp_ms;
    // Time went backwards (replay seek, context swap): the retained prints
    // belong to a traversal that no longer exists.
    if (clock < candle_prints_seen_ms_) {
        candle_prints_.clear();
        candle_prints_seen_ms_ = 0; candle_prints_seen_count_ = 0;
        candle_bubble_scale_ = {}; candle_bubble_scale_since_ms_ = 0;
        candle_bubble_auto_floor_ = 0; candle_bubble_size_reference_ = 0;
    }
    // Fix the reference once warm. Panning and new history never resize old
    // prints. The user can explicitly recalibrate from the layer controls.
    if (!candle_bubble_scale_since_ms_) candle_bubble_scale_since_ms_ = clock;
    if (!(candle_bubble_auto_floor_ > 0)) {
        if (candle_bubble_history_.auto_floor > 0)
            candle_bubble_auto_floor_ = candle_bubble_history_.auto_floor;
        else {
            candle_bubble_scale_.update(trades, clock);
            if (candle_bubble_scale_.settled() ||
                (candle_bubble_scale_.minimum() > 0 && clock - candle_bubble_scale_since_ms_ >= kCandleBubbleWarmMs)) {
                candle_bubble_auto_floor_ = candle_bubble_scale_.minimum() * kCandleBubbleMult;
                // Recover the still-retained initial sample when auto first warms.
                if (candle_bubble_min_ == 0) {
                    candle_prints_.clear(); candle_prints_seen_ms_ = 0; candle_prints_seen_count_ = 0;
                }
            }
        }
    }
    const double floor = candle_bubble_floor();
    // Fold in every trade newer than the last one scanned. Same-millisecond
    // ties are told apart by arrival position: the ring appends in order for
    // every venue, so the count already scanned at that millisecond is the
    // only identity needed (Bybit prints carry no numeric aggregate ID).
    auto it = std::upper_bound(trades.begin(), trades.end(), candle_prints_seen_ms_,
        [](int64_t ts, const Terminal::Trade& t) { return ts < t.timestamp_ms; });
    if (candle_prints_seen_ms_ > 0) {
        auto tie = std::lower_bound(trades.begin(), it, candle_prints_seen_ms_,
            [](const Terminal::Trade& t, int64_t ts) { return t.timestamp_ms < ts; });
        size_t index = 0;
        for (; tie != it; ++tie, ++index)
            if (index >= candle_prints_seen_count_ && floor > 0 &&
                tie->price * tie->qty >= floor)
                candle_prints_.push_back(*tie);
    }
    for (; it != trades.end(); ++it)
        if (floor > 0 && it->price * it->qty >= floor) candle_prints_.push_back(*it);
    candle_prints_seen_ms_ = clock;
    {
        const auto first = std::lower_bound(trades.begin(), trades.end(), clock,
            [](const Terminal::Trade& a, int64_t ts) { return a.timestamp_ms < ts; });
        const auto last = std::upper_bound(first, trades.end(), clock,
            [](int64_t ts, const Terminal::Trade& a) { return ts < a.timestamp_ms; });
        candle_prints_seen_count_ = size_t(last - first);
    }
    // Same-ms ties appended after the main run can land out of order; the
    // renderer binary-searches by time, so keep the deque sorted.
    if (candle_prints_.size() > 1 &&
        candle_prints_[candle_prints_.size() - 2].timestamp_ms > candle_prints_.back().timestamp_ms)
        std::stable_sort(candle_prints_.begin(), candle_prints_.end(),
            [](const Terminal::Trade& a, const Terminal::Trade& b) { return a.timestamp_ms < b.timestamp_ms; });
    while (candle_prints_.size() > kCandlePrintsMax ||
           (!candle_prints_.empty() && candle_prints_.front().timestamp_ms < clock - kCandlePrintsKeepMs))
        candle_prints_.pop_front();
}

void ChartWidget::render_candle_bubbles() {
    const double floor = candle_bubble_floor();
    const auto limits = ImPlot::GetPlotLimits();
    // A candle bar is drawn CENTRED on its period's open time (draw_single_candle
    // spans x +/- 0.35 tf around timestamps[i]), while the period's trades run
    // from open to open + tf. Drawn at their raw time, the second half of every
    // bar's prints landed over the NEXT bar, at prices that bar never touched.
    // Shifting each print back by half a period puts it inside its own bar.
    const double shift_ms = double(candles().timeframe_seconds()) * 500.0;
    const bool history = candle_bubble_history_enabled_ && Entitlements::hosted() && Entitlements::is_pro() &&
        pair_.exchange == "binancef" && !ctx_.replay_mgr().is_active() && !EducationBoot::instance().is_pack();
    const int64_t from_ms = int64_t(limits.X.Min + shift_ms), to_ms = int64_t(limits.X.Max + shift_ms);
    if (history) {
        auto req = candle_bubble_history_.request(pair_,from_ms,to_ms,int64_t(emscripten_date_now()),emscripten_get_now());
        if (!req.is_null() && !req.empty()) {
            const char* token = emscripten_run_script_string("window.__EDGEDEPTH_REPLAY_TOKEN__ || ''");
            req["data"]["entitlement_token"] = token ? token : "";
            ctx_.stream_mgr().send_message(req.dump());
        }
        int loaded = 0, observed = 0;
        candle_bubble_history_.coverage(from_ms,to_ms,loaded,observed);
        char status[160];
        if (!candle_bubble_history_.error.empty())
            snprintf(status,sizeof(status),"Bubbles: %s",candle_bubble_history_.error.c_str());
        else if (candle_bubble_history_.loading()) snprintf(status,sizeof(status),"Bubbles: loading recent history...");
        else snprintf(status,sizeof(status),"Bubbles: %d/%d loaded minutes have records | recent 6h | max 16/min",observed,loaded);
        auto* draw = ImPlot::GetPlotDrawList();
        const ImVec2 pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
        const ImVec2 text(pos.x+8,pos.y+size.y-ImGui::GetFontSize()-8);
        const ImVec2 text_size = ImGui::CalcTextSize(status);
        ImPlot::PushPlotClipRect();
        draw->AddRectFilled(ImVec2(text.x-3,text.y-2),ImVec2(text.x+text_size.x+3,text.y+text_size.y+2),Theme::u32(Theme::Tokens::BASE,0.9f));
        draw->AddText(text,Theme::u32(Theme::Tokens::TX2),status);
        ImPlot::PopPlotClipRect();
        if (ImPlot::IsPlotHovered() && ImGui::IsMouseHoveringRect(text,ImVec2(text.x+text_size.x,text.y+text_size.y)))
            Theme::tooltip("Historical selection: up to 16 largest original records per minute, filtered by Minimum value.\n"
                "Only the recent six hours are queried; the latest two minutes use live observations.\n"
                "A minute with records is not certified complete. Blank minutes may be quiet, missing or not loaded.\n"
                "Exchange records can aggregate fills. Overlapping smaller records are hidden; zoom for detail. Sizes use a fixed log-compressed reference.");
    }
    if (!(floor > 0)) return;
    const auto first = std::lower_bound(candle_prints_.begin(), candle_prints_.end(), int64_t(limits.X.Min + shift_ms),
        [](const Terminal::Trade& t, int64_t ts) { return t.timestamp_ms < ts; });
    const auto last = std::upper_bound(first, candle_prints_.end(), int64_t(limits.X.Max + shift_ms),
        [](int64_t ts, const Terminal::Trade& t) { return ts < t.timestamp_ms; });
    auto* dl = ImPlot::GetPlotDrawList();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const MixedBubbles::Marker* hovered = nullptr;
    ImPlot::PushPlotClipRect();
    // Qualifying prints keep their identities up to here; the mixed view then
    // merges the ones that share a bar and a price, so a level both takers hit
    // in one bar is one two-tone bubble, and the strongest-first selection
    // below removes crowding among the merged markers.
    static std::vector<const Terminal::Trade*> order;
    order.clear();
    for (auto it = first; it != last; ++it) {
        const double notional = it->price * it->qty;
        if (notional < floor || it->price < limits.Y.Min || it->price > limits.Y.Max) continue;
        if (!history || !candle_bubble_history_.contains(it->agg_trade_id)) order.push_back(&*it);
    }
    if (history) candle_bubble_history_.append_visible(order,from_ms,to_ms);
    order.erase(std::remove_if(order.begin(),order.end(),[&](const auto* t){
        return t->price*t->qty < floor || t->price < limits.Y.Min || t->price > limits.Y.Max;
    }),order.end());
    const size_t qualifying_count=order.size();
    candle_bubble_view_.build(order.begin(), order.end(), int64_t(shift_ms) * 2, tick_size_, 0.0);
    static std::vector<const MixedBubbles::Marker*> ranked;
    ranked.clear();
    for (size_t i = 0; i < candle_bubble_view_.count; ++i) ranked.push_back(&candle_bubble_view_.markers[i]);
    std::sort(ranked.begin(), ranked.end(), [](const MixedBubbles::Marker* a, const MixedBubbles::Marker* b) {
        if (a->value() != b->value()) return a->value() > b->value();
        if (a->timestamp_ms != b->timestamp_ms) return a->timestamp_ms < b->timestamp_ms;
        return a->price < b->price;
    });
    if (!(candle_bubble_size_reference_ > 0)) {
        if (candle_bubble_history_.size_reference > 0) candle_bubble_size_reference_=candle_bubble_history_.size_reference;
        else if (!history || !candle_bubble_history_.error.empty() || (!candle_bubble_history_.loading() && !candle_bubble_history_.tiles.empty())) {
            // Local/replay observations use their warmed market reference.
            candle_bubble_size_reference_=candle_bubble_auto_floor_;
        }
    }
    const double reference = candle_bubble_size_reference_;
    // Warm the market reference even with a manual visibility floor.
    if (!(reference > 0)) { ImPlot::PopPlotClipRect(); return; }
    static std::vector<CandleBubbleDisplay::Marker> selected;
    selected.clear();
    const auto plot_pos = ImPlot::GetPlotPos(), plot_size = ImPlot::GetPlotSize();
    for (size_t i=0;i<ranked.size();++i) {
        const auto* m=ranked[i];
        const auto c=ImPlot::PlotToPixels(double(m->timestamp_ms)-shift_ms,m->price);
        const float radius=CandleBubbleDisplay::radius(m->value(),reference);
        // Offscreen candidates cannot suppress a visible execution.
        if(c.x<plot_pos.x || c.x>plot_pos.x+plot_size.x || c.y<plot_pos.y || c.y>plot_pos.y+plot_size.y) continue;
        CandleBubbleDisplay::retain(selected,{i,c.x,c.y,radius},plot_size.x);
    }
    for (const auto& marker : selected) {
        const MixedBubbles::Marker* m = ranked[marker.index];
        const ImVec2 c = ImPlot::PlotToPixels(double(m->timestamp_ms) - shift_ms, m->price);
        const float r = CandleBubbleDisplay::radius(m->value(), reference);
        const ImVec4& major = m->buy_led() ? Theme::Tokens::UP : Theme::Tokens::DOWN;
        const ImVec4& minor = m->buy_led() ? Theme::Tokens::DOWN : Theme::Tokens::UP;
        RealtimeBubble::draw(*dl, c, r, major, Theme::u32(Theme::Tokens::BASE),
                             RealtimeBubble::Fill::Translucent, float(m->minor()), minor, rt_bubble_alpha_);
        const float dx = mouse.x - c.x, dy = mouse.y - c.y;
        // Last hit is the visible topmost disc, including its circular edge.
        if (dx * dx + dy * dy <= r * r) hovered = m;
    }
    if (hovered && ImPlot::IsPlotHovered()) {
        const auto anchor = ImPlot::PlotToPixels(double(hovered->timestamp_ms)-shift_ms,hovered->price);
        const float right=plot_pos.x+plot_size.x;
        for(float x=anchor.x;x<right;x+=10)
            dl->AddLine(ImVec2(x,anchor.y),ImVec2(std::min(x+5,right),anchor.y),Theme::u32(Theme::Tokens::TX1,0.6f));
    }
    ImPlot::PopPlotClipRect();
    if (hovered && ImPlot::IsPlotHovered()) {
        char time[64] = {}, price[64] = {};
        DisplayTimeZone::instance().format(hovered->timestamp_ms, TimeZoneFormat::DateTimeSeconds, time, sizeof(time));
        snprintf(price, sizeof(price), fmt_.price_fmt, hovered->price);
        Theme::begin_tooltip();
        ImGui::TextColored(hovered->buy_led()?Theme::Tokens::UP:Theme::Tokens::DOWN,
            "%s | %.2f", hovered->buy>0 && hovered->sell>0 ? "Buy / sell prints" : hovered->buy_led()?"Buy prints":"Sell prints",hovered->value());
        ImGui::Text("Price %s | %u record%s",price,hovered->count,hovered->count==1?"":"s");
        ImGui::TextUnformatted(time);
        if(hovered->buy>0 && hovered->sell>0)
            ImGui::Text("Bought %.2f | sold %.2f",hovered->buy,hovered->sell);
        if(ImGui::GetIO().KeyAlt) {
            ImGui::Separator();
            ImGui::Text("%s %s | quote notional",pair_.exchange.c_str(),pair_.symbol.c_str());
            ImGui::Text("%.1fx size reference (%.0f)",hovered->value()/reference,reference);
            ImGui::Text("%zu bubbles / %zu qualifying records",selected.size(),qualifying_count);
            ImGui::TextUnformatted(candle_bubble_history_.size_reference>0
                ? "Reference: initial selection P90, fixed while panning."
                : "Reference: warmed live trade-size threshold.");
            ImGui::TextUnformatted("Log-scaled sizes; zoom reveals hidden prints.");
            ImGui::TextUnformatted("Same bar and price grouped; exchange records may combine fills.");
        } else ImGui::TextDisabled("Quote notional | Alt for details");
        Theme::end_tooltip();
    }
}
