#pragma once
#include "rendering/observed_liquidations.h"
#include "core/liquidity_response.h"
#include "core/refill_levels.h"
#include "core/refill_history.h"
#include "core/refill_absorption.h"
#include "core/realtime_bubble_size.h"
#include "core/liq_rails.h"
#include "core/alert_engine.h"
#include "core/entitlements.h"
#include "core/hl_rails.h"
#include "rendering/hl_rails_overlay.h"
#include "core/liqmap2_release.h"
#ifdef EDGEDEPTH_EXPOSURE_V2_DEV
#include "core/exposure_history.h"
#include "core/exposure_summary_history.h"
#include "rendering/exposure_summary_overlay.h"
#include "rendering/exposure_overlay.h"
#endif
#include "rendering/liqmap2_rails_overlay.h"
#include "core/candle_bubble_history.h"
#include "core/flow_positioning.h"
#include "core/activity_strip.h"
#include "core/reference_context.h"
#include "core/rt_walls.h"
#include "core/rt_cvd.h"
#include "core/cascade_detector.h"
#include "core/liq_levels.h"
#include "ui/realtime_dom_frame.h"
#include "ui/realtime_dom_ladder.h"
#include "ui/realtime_navigation.h"
#include "core/trade_at_price.h"
#include "core/realtime_history.h"
#include "core/realtime_archive.h"
#include "rendering/realtime_trade_view.h"
#include "rendering/mixed_bubbles.h"

#include <implot.h>
#include "ui/widget.h"
#include "types/types.h"
#include "stream_handler.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <deque>
#include <array>
#include <utility>
class ReplayManager;
struct DataContext;

#include "indicators/indicator_manager.h"
#include "indicators/volume_indicator.h"
#include "indicators/absorption_indicator.h"
#include "indicators/cvd_indicator.h"
#include "indicators/rsi_indicator.h"
#include "indicators/macd_indicator.h"
#include "indicators/funding_rate_indicator.h"
#include "indicators/oi_indicator.h"
#include "indicators/vpin_indicator.h"
#include "core/app_context.h"
#include "core/heatmap_manager.h"
#include "core/liquidation_heatmap_manager.h"
#include "core/orderbook_manager.h"
#include "core/candle_manager.h"
#include "core/renko_builder.h"
#include "core/volume_profile_manager.h"
#include "core/tpo_manager.h"
#include "core/symbol_metadata.h"
#include "core/research_url.h"  // MoveSnap: the shift-drag snapped to the outcome ladder
#include "rendering/liq_field_renderer.h"
#include "ui/drawing/drawing_layer.h"
#include "ui/settings_panel.h"

// ═══════════════════════════════════════════════════════════════════════════════
// ChartWidget - RENDERING + UI ONLY
//
// Does NOT own candle data. Reads from CandleManager via const accessors.
// Responsible for: ImPlot chart, candle drawing, heatmap overlays,
// crosshair, indicators, toolbar.
//
// CandleManager owns: candle deque, SoA cache, trade→candle building,
// server candle merging, historical batch loading, scroll-load.
// ═══════════════════════════════════════════════════════════════════════════════

enum class HeatmapType {
    OrderbookDepth,
    VolumeDelta,
    TradeIntensity,
    Liquidations,
    VWAPDeviation
};

// ── Chart type (was a bare int; the underlying values are frozen so the
//    chart-type popup rows and the tick-cache key can round-trip via int) ──
//    0=Candles, 1=FP Cluster, 2=FP Profile, 3=Heikin Ashi, 4=Line, 5=TPO, 6=Renko.
enum class ChartType : int {
    Candles          = 0,
    FootprintCluster = 1,
    FootprintProfile = 2,
    HeikinAshi       = 3,
    Line             = 4,
    TPO              = 5,
    Renko            = 6,
    FlowPositioning  = 7,
};

// True when the chart's X-axis is time in ms (everything except Renko).
constexpr bool ct_is_time_axis(ChartType t) { return t != ChartType::Renko; }
// True when the type draws from the real OHLC candle geometry (Candles, FP, TPO).
constexpr bool ct_uses_real_candles(ChartType t) {
    return t == ChartType::Candles || t == ChartType::FootprintCluster
        || t == ChartType::FootprintProfile || t == ChartType::TPO || t == ChartType::FlowPositioning;
}
// True when time-keyed overlays (heatmap, liq, VPVR, footprint, patterns) may draw.
constexpr bool ct_allows_time_overlays(ChartType t) {
    return t != ChartType::TPO && t != ChartType::Renko;
}

class ChartWidget : public Widget {
public:
    workspace::Json save_settings() const override;
    void load_settings(const workspace::Json& settings) override;
    static bool selection_escape_consumed(int frame) { return selection_escape_frame_ == frame; }
    // The pair this chart draws. A replay of a different symbol replaces the
    // chart, and the exit rebuild has to restore the pair that was live.
    [[nodiscard]] const Terminal::Pair& pair() const { return pair_; }

    // Bind the real tick + price precision when the registry answers after the
    // chart was built. Until then the axis runs on provisional precision (see
    // PriceFormatter::provisional_for_price), never on a guessed tick.
    void refresh_instrument() override;

    // instance 1 is the primary chart: it reads the app-level CandleManager
    // and keeps the "###chart_<ex>_<sym>" identity saved layouts dock by.
    // instance >= 2 is a SECONDARY chart for a pair that already has one: it
    // owns a CandleManager of its own (own timeframe, own follow-live latch,
    // own trade ring, so its own RealtimeArchive) and takes the identity
    // "###chart_<ex>_<sym>_<instance>", so a real-time depth view and a
    // candle chart of one market can sit side by side.
    //
    // compare = true builds a COMPARE chart: a replay widget for a session
    // market other than the primary (DataContext::symbols[1..]). It owns no
    // manager; its candles are the replay context's manager for the pair,
    // resolved on every read (the context is retired a frame after the
    // replay ends, and this widget with it). Time-linked to the primary.
    ChartWidget(const Terminal::Pair& pair,
                const AppContext& ctx,
                double tick_size,
                int instance = 1,
                bool compare = false);
    ~ChartWidget() override;

    // The candle source this chart draws. Every candle read in the chart
    // sources goes through here. The one deliberate exception is
    // replay_start_time_ms(): that is app state (which context is live), so it
    // is still read from ctx_.candle_mgr(), which a replay swaps.
    //
    // Resolution: an owned manager (a secondary chart, or a live chart of a
    // pair other than the app-level manager's), else the replay context's
    // manager for this pair (a compare chart), else the app-level manager.
    // A compare chart whose context is already gone falls back to the
    // app-level manager for the one frame before the sweep erases it.
    [[nodiscard]] CandleManager& candles() const;
    [[nodiscard]] int  chart_instance() const { return chart_instance_; }
    [[nodiscard]] bool is_secondary()  const { return owned_candles_ != nullptr; }
    [[nodiscard]] bool is_compare()    const { return compare_; }

    // ─── Time link ──────────────────────────────────────────────────────
    // Linked charts share ONE time axis: pan/zoom on any of them moves all,
    // the hovered chart's crosshair time is mirrored on the others, and a
    // timeframe change on one is adopted by the rest. Used by the compare
    // replay (two markets, one clock) where reading vertical alignment is
    // the whole point. Off by default; main.cpp links the session's charts.
    void set_time_linked(bool on) { time_linked_ = on; }
    [[nodiscard]] bool time_linked() const { return time_linked_; }

    void render() override;
    void update() override;

    WidgetType type() const override { return WidgetType::Chart; }
    const char* title() const override { return title_.c_str(); }

    void change_timeframe(int new_tf_seconds);
    void set_chart_type(ChartType type);

    // Current chart timeframe in seconds - live source of truth is the
    // CandleManager (the title's tf field lags a frame). Lets the topbar TF
    // segment both highlight the active TF and drive timeframe changes.
    int64_t timeframe_seconds() const { return candles().timeframe_seconds(); }

    // Real-time view (RT). Applies regardless of chart type: the chart follows
    // the live edge (follow-live streaming) and draws its own observed-depth /
    // trade view (render_realtime). Toggled from the Real-time pill beside the
    // timeframe bar (see app_shell render_tf_control).
    bool rt_mode() const { return rt_mode_; }
    bool show_standalone_panels() const { return rt_show_standalone_; }
    // Views with no indicator subplots. Real-time draws observed depth on a
    // two-minute axis where timeframe series have nothing to say; Renko uses
    // a brick-index axis the time-aligned pane cannot follow. The pane's
    // indicators are kept, so returning to candles restores them.
    bool subplots_suppressed() const { return rt_mode_ || chart_type_ == ChartType::Renko; }
    void set_rt_mode(bool v);
    // Real-time candle size: the OHLC bucket the live view aggregates trades
    // into (1s / 5s / 15s / 30s / 1m / 5m). Only meaningful while rt_mode() is
    // on; set_rt_mode(true) resets it to one second.
    void set_rt_candle_seconds(int seconds);
    int rt_candle_seconds() const { return rt_candle_seconds_; }
    // The viewer's own toggle (the toolbar pill). Every other set_rt_mode(true)
    // is automatic, and the upsell records which one opened it.
    void toggle_rt_mode();
    // Files the same request as "+ Widget > Chart" for this chart's market (the
    // main loop drains it next frame). Used by the web guide's "Add a candle
    // chart" button through Module.__add_chart_widget.
    void request_chart_of_this_market();
    // True when turning real-time on would open the upsell instead: a Free
    // viewer on the hosted LIVE feed of a Pro market with today's free preview
    // spent (core/realtime_preview.h). Replay sessions and local packs are open
    // to everyone. The toolbar pill reads this to draw its padlock; set_rt_mode
    // is still the one place that enforces it.
    bool rt_mode_locked() const;
    // This market needs Pro for real-time (whatever the preview has left), so
    // real-time here runs on the free daily preview.
    bool rt_mode_gated() const;
    int64_t rt_preview_remaining_ms() const;
    void render_realtime_settings();
    void on_rewind(int64_t cutoff_ms) override;

    // Phase 2 S3: estimated liquidation clusters (OI + mark + the recent tape).
    void render_realtime_liq_levels(ImDrawList* dl, const ImPlotRect& limits);
    // Phase 2 S4: the tracked active walls as an always-visible table.
    void render_realtime_walls_table(ImDrawList* dl);
    // Phase 2 S5: cumulative resting depth - the second heat layer.
    void render_realtime_cum_depth(ImDrawList* dl, const ImPlotRect& limits);

    float liq_opacity() const { return liq_opacity_; }   // liq-heatmap opacity (Tweaks panel)
    void  set_liq_opacity(float v);                      // set + push to the reconstructor
    // WS2 A/B: Field render path - texture quad (GPU) vs per-rect (CPU). The
    // rect path stays selectable until the texture pass is screenshot-approved.
    bool liq_field_use_texture() const { return liq_field_.knobs().use_texture; }
    void set_liq_field_use_texture(bool v) { liq_field_.knobs().use_texture = v; }

    void add_volume_indicator();
    void add_absorption_indicator();
    void add_cvd_indicator();
    void add_rsi_indicator(int period = 14);
    void add_macd_indicator(int fast = 12, int slow = 26, int signal = 9);
    void add_funding_rate_indicator();
    void add_oi_indicator();
    void add_vpin_indicator();

    // Reset overlay subscription state (called on replay context swap)
    void reset_overlay_subscriptions();

    template<typename IndicatorType, typename... Args>
    void add_indicator(Args&&... args) {
        auto indicator = std::make_unique<IndicatorType>(std::forward<Args>(args)...);
        if (auto* vol_ind = dynamic_cast<Indicators::VolumeIndicator*>(indicator.get())) {
            populate_volume_data(vol_ind);
        }
        indicator->update();
        indicator_mgr_.add_indicator(std::move(indicator));
    }

    Indicators::IndicatorManager& get_indicator_manager() { return indicator_mgr_; }

    static std::string timeframe_to_string(int64_t seconds);
    bool is_loading() const;

    struct CrosshairState {
        bool is_active = false;
        ImPlotPoint plot_pos;
        ImVec2 first_plot_min;
        ImVec2 first_plot_max;
        ImVec2 last_plot_max;
        ImVec2 hovered_plot_min;
        ImVec2 hovered_plot_max;
        double hovered_y_min = 0.0;
        double hovered_y_max = 0.0;
        bool chart_hovered = false;
        bool indicator_hovered = false;
        ImPlotFormatter indicator_y_formatter = nullptr;  // Y-axis formatter from hovered indicator
    };

    CrosshairState crosshair_state_;
    double cursor_reference_price_ = 0; // Same as the displayed current-price tag, including paused RT/replay.
    float chart_allocated_height_ = 0.0f;
    float indicator_allocated_height_ = 0.0f;
    // Replay time-range selection (Shift+drag on chart)
    struct ReplaySelection {
        bool active = false;
        bool dragged = false;
        bool cancelled = false;
        int64_t press_ms = 0;
        int64_t start_ms = 0;
        int64_t end_ms = 0;
    };
    ReplaySelection replay_selection_;
    bool selection_popup_was_open_ = false;
    static inline int selection_escape_frame_ = -1;
    int64_t context_menu_time_ms_ = 0;  // Captured at right-click, used by popup
    // The right-clicked MINUTE (60000-floored), captured in the same block as
    // context_menu_time_ms_. NEVER reuse context_menu_time_ms_ for research:
    // it floors to the chart timeframe (a 4H chart floors to a 4h boundary),
    // which is right for replay and silently wrong by up to hours for a
    // minute-addressed research read (the spec's named trap).
    int64_t context_menu_minute_ms_ = 0;
    double  context_menu_price_ = 0.0;  // Price under cursor, captured at right-click

    // The shift-drag selection read as a MOVE and snapped to the outcome
    // ladder, captured at right-click beside the two timestamps above so the
    // menu says the same thing for as long as it is open. Renko has no
    // selection and clears it. See research_url::snap_move.
    struct SelectedMove {
        research_url::MoveSnap snap{};
        int64_t range_minutes = 0;  // the dragged length, for the usage event
    };
    SelectedMove context_menu_move_;
    SelectedMove read_selected_move() const;

    // Measure tool (right-click "Measure from here"): anchor + live readout that
    // follows the cursor (dPrice, d%, elapsed, candle count) until click/Esc.
    struct MeasureState { bool active = false; int64_t t0_ms = 0; double p0 = 0.0; };
    MeasureState measure_;

    // Client-side price alerts (live chart only, session-scoped): a level drawn
    // on the chart that fires a browser notification + in-app toast + beep when
    // the live price crosses it, then clears (one-shot). Not persisted.
    struct PriceAlert { double price = 0.0; bool arm_above = false; };
    std::vector<PriceAlert> alerts_;
    double      alert_last_price_ = 0.0;   // last live price seen (crossing edge)
    double      alert_toast_until_ = 0.0;  // ImGui::GetTime() deadline for the toast
    std::string alert_toast_msg_;

    struct DateBoundary {
        double timestamp;
        bool show_date;
    };
    mutable std::vector<DateBoundary> date_boundaries_;

    void setup_time_axis_ticks(double visible_x_min, double visible_x_max) const;

private:
    bool session_vwap_ = false, previous_day_ = false, previous_week_ = false;
    int64_t vwap_anchor_ms_ = 0;
    reference_context::Series session_vwap_data_, anchored_vwap_data_;
    reference_context::Levels previous_day_data_, previous_week_data_;
    const CandleManager* reference_manager_ = nullptr;
    int64_t reference_tf_ = 0;
    double reference_update_time_ = -1;
    int64_t reference_asof_ = 0;
    int64_t reference_autoload_from_ = 0;   // the period start an automatic history read was last made for
    void update_reference_context();
    void render_reference_context();
    // ─── External Dependencies (not owned) ───────────────────────────────
    Terminal::Pair pair_;
    const AppContext& ctx_;
    // Secondary charts only (see the constructor). Declared right after ctx_ so
    // it outlives every member that may still point at it during teardown
    // (rt_archive_ is keyed by this manager, liq_field_ reads it). Its
    // destructor unsubscribes through the StreamManager it subscribed to.
    std::unique_ptr<CandleManager> owned_candles_;
    // The replay context owned_candles_ is registered with (a secondary chart
    // opened during a replay), so the destructor can leave its seek fan-out.
    DataContext* replay_candles_ctx_ = nullptr;
    int chart_instance_ = 1;
    bool compare_ = false;
    bool time_linked_ = false;
    int  link_id_ = 0;              // this chart's identity in the link group
    int  link_tf_seen_frame_ = -1;  // last TF broadcast this chart adopted
    // Apply the group's axis before BeginPlot's X setup (follower) and publish
    // this chart's axis + hover after the plot (owner). Candle path only.
    void time_link_apply(double& x_min, double& x_max, bool& force_always);
    void time_link_publish();
    void time_link_draw_mirror();
    void time_link_adopt_timeframe();

    // ─── Chart Identity ──────────────────────────────────────────────────
    // title_ = "Chart <ex> <sym> <tf>###chart_<ex>_<sym>" for the primary. The
    // visible prefix carries the live timeframe; the part after "###" is the
    // TF-independent docking identity (must match
    // LayoutManager::setup_default_layout), so the chart stays docked when the
    // TF changes. A secondary chart appends "_<instance>" to the identity (and
    // " (<instance>)" to the visible label) so two charts of one pair never
    // resolve to one ImGui window. title_tf_seconds_ and title_rt_ track what
    // the title was last built for, so render() can refresh it lazily.
    void rebuild_title();
    std::string title_;
    std::string timeframe_label_;
    int64_t title_tf_seconds_ = -1;
    bool title_rt_ = false;
    double tick_size_;

    // ─── Viewport ────────────────────────────────────────────────────────
    ImPlotRect last_visible_range_{};
    double x_axis_min_ = 0.0;
    double x_axis_max_ = 0.0;
    double stored_x_min_ = 0.0;
    double stored_x_max_ = 0.0;

    // ─── Chart Type ───────────────────────────────────────────────────────
    // See ChartType above. Stored as the enum; the popup rows / tick-cache key
    // round-trip through static_cast<int> where an int is genuinely needed.
    ChartType chart_type_ = ChartType::Candles;
    // The type the chart showed before real-time mode replaced it with Line /
    // 1s candles; leaving real-time (pill, timeframe click or a type pick)
    // restores it, and it is what the workspace persists while real-time is on.
    ChartType rt_prev_chart_type_ = ChartType::Candles;

    // ─── Renko (ChartType::Renko) ─────────────────────────────────────────
    // Price-driven bricks on a brick-index X-axis (not time). The builder is a
    // pure, deterministic transform over the candle close series (replay-safe);
    // see core/renko_builder.h.
    RenkoBuilder renko_;
    // Brick size in ticks: 0 = auto (resolved once from price into
    // renko_resolved_ticks_ and then frozen so the sequence does not repaint as
    // price moves); >0 = manual override (config popup). Resolved price size =
    // renko_resolved_ticks_ * tick_size_.
    int  renko_brick_ticks_    = 0;   // user setting (0 = auto)
    int  renko_resolved_ticks_ = 0;   // frozen resolved tick count (0 = unresolved)
    // Committed-cache signature: rebuild the committed bricks only when the
    // closed-candle set / brick size / timeframe change (the building candle
    // folds in per frame, cheaply).
    mutable size_t  renko_sig_count_ = 0;
    mutable int64_t renko_sig_time_  = 0;   // last closed candle's TIMESTAMP (not close:
                                            // a close mutation must NOT force a rebuild)
    mutable double  renko_sig_size_  = 0.0;
    mutable int64_t renko_sig_tf_    = 0;
    // Time window of the currently visible bricks, published by
    // render_chart_renko so update() can feed CandleManager a TIME range (never
    // brick indices) for scroll-load. 0 until the first Renko frame renders.
    int64_t renko_view_t0_ms_ = 0;
    int64_t renko_view_t1_ms_ = 0;
    // On-chart brick-size affordance: a gear next to the "RENKO …" readout opens
    // the config popup (deferred one frame so OpenPopup runs at window scope, not
    // inside BeginPlot). renko_gear_pos_ anchors the popup under the gear.
    bool   open_renko_settings_ = false;
    ImVec2 renko_gear_pos_{};
    // ─── Real-time view (RT) ──────────────────────────────────────────────
    // RT mode (any chart type): engage follow-live so the chart streams and
    // keeps the live edge current, while normal ImPlot zoom/pan stay fully
    // interactive (the follow-live latch clears on the first user pan/scroll -
    // handle_plot_interaction). No per-frame axis pin. For the Line, plot_line
    // also draws the live-edge dot. Toggled from the Real-time pill beside the
    // timeframe bar (app_shell render_tf_control). rt_was_on_ edge-detects the toggle so RT
    // re-arms follow-live exactly ONCE on the rising edge (not every frame).
    RealtimeDOMFrame rt_dom_frame_;
    std::unique_ptr<TradeAtPriceAccumulator> rt_flow_;
    int64_t rt_unhealthy_since_ms_ = 0;
    bool    rt_mode_    = false;
    bool    rt_was_on_  = false;
    bool rt_candles_ = false, rt_bubbles_ = true, rt_book_valid_ = false;
    bool rt_candles_hollow_ = false;  // outline bodies so the heat shows through
    bool cascade_alerts_enabled_ = true;  // toast + notification on liquidation cascades
    CascadeDetector cascade_detector_;    // Phase 2 S1: same-side liquidation chains
    AlertEngine alert_engine_;            // Phase 2 S2: threshold alert queues
    bool rt_liq_levels_enabled_ = true;   // Phase 2 S3: estimated liquidation clusters
    liq_levels::Estimator liq_estimator_; // ...the model behind them
    double liq_mark_price_ = 0;           // live mark price (stats stream)
    double liq_oi_contracts_ = 0;         // live open interest in contracts (stats stream)
    bool rt_walls_table_enabled_ = false; // Phase 2 S4: active walls table overlay
    bool rt_cum_depth_enabled_ = false;   // Phase 2 S5: resting-depth heat layer
    bool liq_alerts_enabled_ = false;     // big single liquidations
    double liq_alert_usd_ = 250000.0;     // alert at/above this notional
    bool wall_alerts_enabled_ = false;    // tracked walls pulled near price
    int64_t last_pull_alert_ms_ = 0;      // watermark: each pull alerts once
    bool rt_paused_ = false, rt_trade_line_ = false;
    bool rt_extend_depth_ = true;
    bool rt_bridge_gaps_ = true;
    // Real-time depth cells. Separate from heatmap_enabled_ (the candle-chart
    // depth heatmap) so entering/leaving real-time mode keeps each view's choice.
    bool rt_depth_enabled_ = true;
    bool rt_auto_price_ = true;
    // The depth ladder attached to the right of the real-time plot, drawn from
    // rt_dom_frame_ after the plot publishes it. rt_dom_linked_ is this frame's
    // "ladder on screen": the price scale then keeps its rows readable.
    bool rt_dom_shown_ = true;
    bool rt_dom_linked_ = false;
    RealtimeDomLadder rt_dom_ladder_;
    RealtimePriceWindow rt_price_window_;
    bool rt_show_standalone_ = false;
    bool rt_auto_fit_history_ = true;
    RealtimeAutoFit rt_auto_fit_;
    int rt_effective_multiplier_ = 5;
    Terminal::BookTicker rt_quote_{};
    // When each side of rt_quote_ last changed price: the live step is drawn
    // there, pinned in time, so it scrolls with the history instead of
    // freezing on screen while tickers keep arriving at an unchanged price.
    int64_t rt_quote_bid_since_ms_ = 0, rt_quote_ask_since_ms_ = 0;
    std::deque<RealtimeDepthHistory::SamplePtr> rt_samples_;
    std::deque<Terminal::Trade> rt_paused_trades_;
    std::shared_ptr<RealtimeArchive> rt_archive_;
    std::deque<RealtimeDepthHistory::SamplePtr> rt_archive_samples_;
    std::deque<Terminal::Trade> rt_archive_trades_;
    RealtimeTradeView rt_trade_view_;    // side-grouped compaction for the archive tail
    MixedBubbles::View candle_bubble_view_;  // per-bar mixed markers on candle charts
    bool realtime_live_edge() const;
    bool rt_history_view_ = false;
    uint64_t rt_archive_generation_ = 0, rt_archive_view_revision_ = 0;
    int64_t rt_query_from_ = 0, rt_query_to_ = 0, rt_query_step_ = 0;
    int64_t rt_loaded_to_ = 0, rt_loaded_step_ = 100;
    bool rt_trade_tail_waiting_ = false;
    double rt_query_at_ = 0;
    int rt_query_multiplier_ = 5;
    void capture_realtime_archive();
    void update_realtime_archive_view();
    void rebuild_realtime_view();
    const std::deque<RealtimeDepthHistory::SamplePtr>& realtime_samples() const {
        return rt_history_view_ ? rt_archive_samples_ : rt_samples_;
    }
    const std::deque<Terminal::Trade>& realtime_trades() const;
    bool rt_subscribed_ = false;
    StreamManager* rt_stream_mgr_ = nullptr;
    bool rt_auto_bubbles_ = true;
    RealtimeBubbleScale rt_bubble_scale_;
    float rt_min_notional_ = 10000.0f;
    // Layers > Trade bubbles > Max size (12 / 24 / 32 px), saved as rt_bubble_cap.
    float rt_bubble_cap_ = bubble_size::kStandard;
    double rt_span_ms_ = realtime_default_span_ms;
    int rt_candle_seconds_ = 1;
    // TradingView-style axis scaling: a drag that starts on a chart axis
    // stretches that axis instead of panning it (latched until mouse-up;
    // the ImPlot input map stands down for the gesture's lifetime).
    int rt_axis_scale_axis_ = -1;              // -1 none, else ImAxis_X1/Y1
    ImVec2 rt_axis_scale_grab_px_{0, 0};
    double rt_axis_scale_anchor_ = 0.0;        // value (time/price) under the grab
    double rt_axis_scale_grab_span_ = 0.0;     // X: span at grab (idempotent scale)
    double rt_axis_scale_grab_min_ = 0.0, rt_axis_scale_grab_max_ = 0.0;
    bool rt_axis_scale_map_override_ = false;
    ImPlotInputMap rt_axis_scale_saved_map_{};
    // A Y-axis scale drag releases the price axis from the RT window manager
    // (its rows*step clamp would fight the stretch every frame). Cleared when
    // price following re-engages.
    bool rt_price_free_ = false;

    // ── [SELF-HOST P1] Peak-hold ──────────────────────────────────────────
    // Decayed per-price-bucket depth maxima keyed by the absolute bucket
    // index (price / tick_size), independent of the GPU ring's drifting
    // per-column windows. A pulled wall keeps a ghost bar for the horizon.
    std::unordered_map<int64_t, std::pair<float, int64_t>> rt_peak_levels_;
    bool rt_peak_hold_ = false;
    int  rt_peak_horizon_min_ = 10;   // 5 / 10 / 30
    int64_t rt_peak_pruned_at_ms_ = 0;

    // ── [Phase 2 S5] Cumulative resting depth ─────────────────────────────
    // Time-weighted resting size per exact price (an exponential average with
    // ≈10-minute memory, lazily decayed when a level leaves), keyed by the
    // absolute bucket index like the peak-hold ghosts. Always accumulated:
    // one map pass per delivered sample, so the layer is warm on toggle.
    static constexpr double kCumTauMs = 200000.0;             // 200 s ≈ 10-min memory
    static constexpr int64_t kCumRetentionMs = 20 * 60000;    // prune unseen entries
    std::unordered_map<int64_t, std::pair<float, int64_t>> rt_cum_levels_;
    int64_t rt_cum_pruned_at_ms_ = 0;

    double rt_usage_emit_s_ = -1;
    int64_t rt_clock_ms_ = 0;
    uint64_t rt_serial_ = 0, rt_generation_ = 0;
    std::unique_ptr<ShaderHeatmapRenderer> rt_renderer_;
    std::vector<RealtimeDepthHistory::SamplePtr> rt_pending_;
    RealtimeDepthHistory::SamplePtr rt_latest_, rt_last_good_;
    uint64_t rt_reported_failure_ = 0;
    bool rt_was_healthy_ = false;
    std::unordered_map<double, float> rt_prices_;
    void update_realtime();
    // Bills this frame to the free preview while real-time runs on a Pro market;
    // true when the budget just ran out and real-time was switched off.
    bool tick_rt_preview();
    void render_realtime();
    void configure_depth_fidelity(ShaderHeatmapRenderer& renderer);
    // Deferred popup opens (set inside Indicators popup, opened next frame)
    bool open_liq_settings_ = false;
    bool open_vpvr_settings_ = false;

    // ─── Heatmap ─────────────────────────────────────────────────────────
    std::chrono::steady_clock::time_point last_heatmap_update_ms_;
    static constexpr int64_t HEATMAP_UPDATE_INTERVAL_MS = 100;
    bool heatmap_enabled_ = false;
    std::string heatmap_mode_ = "UHD";
    HeatmapType heatmap_type_ = HeatmapType::OrderbookDepth;
    bool heatmap_data_requested_ = false;
    bool open_heatmap_settings_ = false;
    bool heatmap_adapt_to_zoom_ = true;
    int heatmap_bucket_multiplier_ = 1;
    int rt_bucket_multiplier_ = 5;
    int64_t heatmap_loaded_timeframe_ = 0;
    float heatmap_sensitivity_ = 1.0f;
    std::chrono::steady_clock::time_point last_heatmap_rebuild_;
    ImPlotRect last_rebuilt_range_{0, 0, 0, 0};
    bool viewport_initialized_ = false;

    // ─── Real-time extras (chart_widget_rt_extras.cpp) ──────────────────
    // Hover readout, walls and pulled walls, the CVD lane; the auto contrast
    // switch is passed to the RT renderer (core/rt_contrast.h).
    bool rt_walls_enabled_ = true, rt_pulls_enabled_ = true, rt_cvd_enabled_ = false, rt_auto_contrast_ = true;
    rt_walls::Tracker rt_walls_;
    uint64_t rt_walls_serial_ = 0, rt_walls_archive_generation_ = 0, rt_walls_dropped_ = 0;
    const RealtimeArchive* rt_walls_archive_ = nullptr;
    struct RtWallMark {   // a wall or pull drawn this frame, kept for its hover
        double price = 0, low = 0, high = 0, size = 0, multiple = 0, traded = 0;
        int64_t start_ms = 0, end_ms = 0;
        ImVec2 at{};          // a pull's glyph
        float half_px = 0;    // actual pull extent, shared by drawing and hover
        bool bid = true, pulled = false;
    };
    std::vector<RtWallMark> rt_wall_marks_;
    RealtimeWallMarks rt_wall_ladder_;
    RealtimeCvdSeries rt_cvd_;
    std::vector<ImVec2> rt_cvd_points_;
    char rt_base_asset_[24] = {};
    const char* realtime_base_asset();
    void update_realtime_walls();
    void render_realtime_walls(ImDrawList* dl);
    void on_liquidation_observed(const Terminal::Liquidation& l);
    void pump_cascade_alerts();
    void pump_rt_alerts();               // S2: big-liq + wall-pull queues
    void scan_new_wall_pulls();          // S2: watermark feed into alert_engine_
    void render_realtime_cascades(ImDrawList* dl, const ImPlotRect& limits);
    bool realtime_hover_ready() const;
    void render_realtime_hover();
    void render_realtime_cvd_lane();
    float realtime_cvd_lane_height() const;
    void render_realtime_wall_layer();
    void render_realtime_cvd_layer();
    int realtime_extra_layers_on() const;
    void reset_realtime_extras();

    // ─── Admin Pattern Overlay ───────────────────────────────────────────
    // Fixed slots avoid allocations in the render loop. The backend currently
    // publishes one live candidate per detector timeframe (4h and 1d); extra
    // capacity keeps the client tolerant of additive admin timeframes.
    std::array<Terminal::PatternOverlay, 4> pattern_overlays_{};
    std::array<bool, 4> pattern_overlay_active_{};
    StreamManager* heatmap_stream_mgr_ = nullptr;
    StreamManager* footprint_stream_mgr_ = nullptr;
    StreamManager* pattern_stream_mgr_ = nullptr;
    bool pattern_subscribed_ = false;

    // ─── Liquidation Heatmap Overlay ─────────────────────────────────────
    bool recorder_view_applied_ = false;  // CLIP_FACTORY: script view overrides, one-shot
    bool liq_extend_levels_ = false;     // V3: OFF - backend EMA provides persistence, forward-fill smears bands

    // ─── Liq Heatmap Visual Controls (MMT-style) ────────────────────────
    float liq_opacity_ = 0.85f;          // Global opacity multiplier (0.0-1.0)
    float liq_color_low_ = 0.0f;         // Low cutoff: values below → transparent (noise filter)
    float liq_color_peak_ = 100000.0f;   // Peak: values at/above → max intensity
    bool liq_color_auto_ = true;         // Auto-compute Low/Peak from data distribution
    int liq_ticks_per_row_ = 0;          // 0 = auto, >0 = manual override
    // Per-leverage toggles (6 tiers matching backend)
    bool liq_lev_5x_ = false;    // Off by default (too far for short-term BTC/ETH)
    bool liq_lev_10x_ = false;   // Off by default
    bool liq_lev_25x_ = true;
    bool liq_lev_50x_ = true;
    bool liq_lev_75x_ = true;
    bool liq_lev_100x_ = true;

    // PERF: cached live standing-shelf selection (price/usd/side), recomputed ONLY when the snapshot
    // ts or the leverage mask changes - the agg-over-~800-bands + sort/merge/cap was running EVERY
    // frame (the liq-map FPS hit). Per-frame now only resolves pixel x for the ~N kept shelves + draws.
    struct ShelfSel { double price = 0.0; double usd = 0.0; bool is_long = false; };
    std::vector<ShelfSel> liq_shelf_cache_;
    int64_t liq_shelf_cache_ts_      = -1;
    uint8_t liq_shelf_cache_mask_    = 0xFF;
    double  liq_shelf_cache_max_usd_ = 0.0;
    bool    liq_shelf_cache_dense_   = false;  // rebuild the cache when liq_dense_field_ is toggled

    // (MMT rail-behavior flags - extend_to_candle / truncate_at_take / min_frac / show_labels - were
    //  removed with the rails pivot 2026-06-29; consume + truncation are backend-baked now.)
    // (The stateful per-level discharge tracker - LiqLevelState, liq_levels_, liq_track_*,
    //  liq_discharge_* - was REMOVED with the rails pivot 2026-06-29. "Taken" is now the backend
    //  RailTracker's consume_ms, delivered per rail on the wire; no client candle-scan inference.)
    // ── Historical line rendering (MMT "rich chart": show past levels, not just the live ones) ──
    // Every tracked level draws as a horizontal line from where it first appeared. A consumed
    // level stops at the take candle and dims to a remnant (KEPT, not deleted) so the chart reads
    // as a cohesive record of where liquidity built and where price took it.
    bool    liq_hist_bands_ = true;             // historical rails ON - backend-baked consume (rails list, RailTracker); LIQMAP_RAILS_DECISION_2026-06-29
    bool    liq_show_lines_ = true;             // standing-shelf overlay - now the LIVE fallback (drawn only when no backend rails) + rollback
    // (liq_consume_cache_ + its candle-set keys were removed with the rails pivot 2026-06-29 - the
    //  per-rail consume time is now BandTrack.consume_ms, baked by the backend RailTracker.)
    int     liq_hist_max_lines_ = 0;            // max drawn lines (0 = unlimited; strongest kept)
    int     liq_hist_max_lines_cap_ = 400;      // hard safety cap (perf), applied even when unlimited
    float   liq_hist_min_frac_ = 0.06f;         // a rail must be >= this fraction of the dominant rail to show (declutter)
    int     liq_hist_per_side_cap_ = 12;        // keep only the strongest N rails per side (long/short) so the dominant magnet reads
    float   liq_hist_merge_pct_ = 0.10f;        // merge lines within this % of a stronger one (0 = off)
    // Census declutter is scoped, not global: the floor is taken per SIDE over bands within
    // this fraction of mark, so a lone whale parked far from price cannot erase every level
    // near it. Measured 2026-07-28 on KAITO: 12 real census levels, all suppressed because a
    // single $305k position at +81% set an $18k floor. See render_liquidation_census().
    float   liq_census_rel_window_ = 0.30f;     // +/- fraction of mark that defines "near price" for the floor

    // liq_dense_field_ is the one "Liquidation map" switch (workspace key "liq_dense_field", so a saved V1
    // "on" reads as map on): on live charts it shows V2's milestone rails where the viewer has V2; in replay,
    // packs and lessons (no V2 frames there) the dense projection Field, the scenario map. §12 cleanup
    // 2026-07-03: the old kLiqDense* constants (pre-Field "dense mode" that flipped the SHELF
    // floor/contrast) are deleted - the Field toggle no longer re-tunes the Levels shelf.
    // Default ON: the liquidation map is a headline layer, so the bare terminal boots with it active.
    bool liq_dense_field_ = true;

    // ── Liq Heatmap projection Field ──────────────────────────────────────
    // The dense candle×leverage projection Field (the "Liq Heatmap" pill).
    // Owns its own segment cache, rebuild signature, tuning knobs and both
    // render paths; see rendering/liq_field_renderer.h. liq_dense_field_ above
    // toggles whether it draws. The Liq Profile below is the price-marginal of
    // this same cache and reads it through the renderer's accessors.
    LiqFieldRenderer liq_field_;
    // §8 Liq Profile - price-marginal of STANDING fuel (WS1 Option A, 2026-07-03: per visible
    // price row, the MAX of the field's rendered brightness across PENDING segments - the same tv
    // the forward cascade projects with. No time-averaging → no window-length dependence; no
    // adaptive re-norm; no width gain - the old liq_profile_gain_ 2.75 hack is deleted).
    // Style = the design system's `profile` spec (liq-heatmap-colormap.json):
    // width 0.12 of plot, LUT-tinted fill at FLAT 0.26 alpha, gaussian sigma = 3 field rows,
    // 1px outline rgba(154,164,178,.50), 1px baseline hairline @ 8% white at the plot edge.
    std::vector<float>  liq_profile_scratch_;
    std::vector<float>  liq_profile_scratch2_;
    std::vector<ImVec2> liq_profile_pts_;
    float liq_profile_width_      = 0.12f;  // sidebar width as a fraction of plot width
    float liq_profile_alpha_      = 0.26f;  // FLAT fill opacity (LUT tint carries the intensity)
    float liq_profile_sigma_rows_ = 3.0f;   // gaussian sigma in FIELD price rows (converted to
                                            // marginal samples via the current zoom, so smoothing
                                            // tracks the field's visible row size)
    static constexpr ImU32 kLiqProfileOutline  = IM_COL32(154, 164, 178, 128);  // 1px outline @ .50
    static constexpr ImU32 kLiqProfileBaseline = IM_COL32(255, 255, 255, 20);   // 1px hairline @ 8%


    // Legend ignition notch - the ramp position where the rare orange→yellow
    // "ignition" top begins (the Ember-K 0.945 stop; see the annotated mockup).
    static constexpr float kLiqLegendNotchT = 0.945f;

    // ─── VPVR Overlay ─────────────────────────────────────────────────────
    bool vpvr_enabled_ = false;
    int  vpvr_ticks_per_row_ = 0;          // 0 = auto
    int64_t vpvr_last_request_start_ = 0;  // track range for re-request
    int64_t vpvr_last_request_end_ = 0;

    // ─── Liquidation Profile Overlay (right-edge sidebar histogram, mirrors VPVR) ────────
    // Reuses the shared Profile:: renderer (price_profile_renderer.h) - same one VPVR uses -
    // fed from the current liq heatmap snapshot instead of volume levels. Opt in from the
    // scenario map settings; the default chart keeps the right edge clear.
    bool liq_profile_enabled_ = false;
    // Touch odds + P9 cascade chip (Heatmap V2, admin preview, live Binance only).
    // Data comes from the web host bridge; see ui/touch_odds_overlay.h.
    bool touch_odds_enabled_ = false;

    // ─── Observed layer (WS4 §7) - REAL @forceOrder liquidation markers ──────────────────
    // Discrete dots at (time, price), area ∝ USD, side-tinted (forced BUY = short liquidated
    // → Tokens::UP; forced SELL = long liquidated → Tokens::DOWN). The fact layer next to
    // the estimated Field - deliberately a different visual grammar (glyphs, not bands).
    bool  liq_observed_enabled_ = true;
    bool  rt_liq_observed_enabled_ = false;
    bool& reported_liquidations_enabled() {
        return rt_mode_ ? rt_liq_observed_enabled_ : liq_observed_enabled_;
    }
    bool reported_liquidations_enabled() const {
        return rt_mode_ ? rt_liq_observed_enabled_ : liq_observed_enabled_;
    }
    bool rt_liq_strip_ = false;
    bool candle_liq_strip_ = true;
    // Candle callouts and real-time diamonds switch separately (they were one flag, which Bybit's default turned off
    // in real-time with no control left to turn it back on). Diamonds sit on the tape (tape_price) where a venue
    // reports bankruptcy prices, so no venue starts with them off.
    bool liq_markers_ = false;
    bool rt_liq_markers_ = true;
    bool liq_report_profile_ = false;
    float rt_liq_height_ = 100.0f, candle_liq_height_ = 100.0f;
    bool rt_liq_collapsed_ = false, candle_liq_collapsed_ = false;
    int64_t liq_view_from_ = 0, liq_view_to_ = 0, liq_cutoff_ = 0;
    char liq_status_[160] = {};
    bool liq_status_attention_ = false;
    void render_chart_empty(const char* title, const char* detail, bool busy);  // loading, interrupted or failed: worth a word on the chart
    bool liquidation_activity_visible() const;
    void render_liquidation_settings();
    void prepare_liquidations();
    void render_liquidation_profile();
    std::vector<Terminal::Liquidation> rt_paused_liquidations_;
    float liq_obs_min_usd_ = 0.0f;       // dust filter (USD); 0 = show all
    bool liq_obs_auto_size_ = true;
    observed_liquidations::MarketSizeReference liq_obs_size_reference_;
    float liq_obs_ref_usd_ = 25000.0f;   // Saved logarithmic size reference; independent of viewport
    static constexpr int kLiqObsMaxDraw = 4000;  // per-frame draw cap → nth_element USD cutoff

    // ─── Trade bubbles on candles (free) ─────────────────────────────────
    // Large prints drawn as bubbles over time-based candle views: the free
    // taste of real-time mode, which draws EVERY trade. The chart retains the
    // prints itself because the candle manager's trade ring keeps only five
    // minutes, which would erase bubbles on anything coarser than 1m.
    // "Large" = quote value >= kCandleBubbleMult x the same 60s 75th-percentile
    // market scale RT uses. Hold the reference until explicit recalibration
    // so panning recorded history cannot change the size of an existing print.
    bool    candle_bubbles_ = false;
    bool    candle_bubble_history_enabled_ = true;
    CandleBubbleHistory candle_bubble_history_;
    float   candle_bubble_min_ = 0.0f;           // manual quote-value floor; 0 = auto
    RealtimeBubbleScale candle_bubble_scale_;
    int64_t candle_bubble_scale_since_ms_ = 0;   // trade clock when the scale last (re)opened
    double  candle_bubble_size_reference_ = 0;  // independent of visibility floor
    double  candle_bubble_auto_floor_ = 0;       // last warm auto floor; holds through a re-warm
    std::deque<Terminal::Trade> candle_prints_;  // retained large prints, time-ordered
    int64_t candle_prints_seen_ms_ = 0;          // newest trade already scanned
    size_t candle_prints_seen_count_ = 0;        // ... and how many prints at that ms were scanned
    static constexpr double  kCandleBubbleMult    = 8.0;
    static constexpr int64_t kCandleBubbleWarmMs  = 2LL * 60 * 1000;
    static constexpr int64_t kCandlePrintsKeepMs  = 24LL * 3600 * 1000;
    static constexpr size_t  kCandlePrintsMax     = 4000;
    // Draw cap: MixedBubbles::View::capacity (largest markers kept).
    double candle_bubble_floor() const;   // current quote-value floor (0 = not yet known)
    void   collect_candle_prints();       // per frame: fold new large prints out of the trade ring
    void   render_candle_bubbles();       // drawn over the candles, inside the plot clip

    // ─── "Liq Levels HL" (P2e) - REAL predictive liq levels (HL census) ──────────────────
    // Ground-truth clusters from the HL clearinghouseState census (stream 34,
    // LiquidationHeatmapManager census store) - real open positions, NOT the modelled
    // heatmap. Keyed by UNDERLYING ({"hl","BTC"}), so it overlays cross-venue: a Binance
    // btcusdt chart subscribes the same HL BTC feed (headline feature - real liq levels
    // on a CEX chart). Distinct colormap (violet longs / mint shorts vs the modelled
    // amber/cyan) + an always-on coverage badge: the census is hot-set-only (the at-risk
    // leveraged tail, ~1-10% of OI notional) and must never read as a complete liq map.
    // The LIQ LEV chips filter this layer by REAL leverage tier (est_Nx = actual
    // position leverage) - same control as the modelled layer, venue-aware meaning.
    // ─── V2 liquidation map (hub task liq-map-v2) ─────────────────────────────────────────
    // What every viewer with V2 sees is the rails (stream 40, history 41; core/liqmap2_rails.h). The publisher's
    // frames (stream 38, history 39) are still subscribed for admins only: the touch-odds rails' modelled fuel
    // reads them. Both live in the managed direct-subscription registry so reconnects and replay transitions
    // restore them.
    int liqmap2_history_requested_ = 0;   // 0 not yet sent on this manager, 1 sent
    int64_t liqmap2_scorecard_ms_ = 0;    // when this chart last asked for its scorecard (wall clock)
    StreamManager* liqmap2_stream_mgr_ = nullptr;
    // V2 rails (public since 2026-10-11, free on BTC, Pro elsewhere; admins as before): their own direct
    // subscription and the history requests (a day at a time, see update_liqmap2).
    int liqmap2_rails_colour_ = 1;  // rails colour: expected in 1 h, 4 h (admin default), 24 h, or $ if reached
    // The colour drawn: customers get 24 h or dollars only (1 h / 4 h unvalidated until their forward look; a saved 1 h
    // or 4 h choice reads as 24 h for them), admins any of the four.
    int rails_colour() const {
        const int c = std::clamp(liqmap2_rails_colour_, 0, 3);
        return liqmap2_access() == liqmap2::Access::preview || c >= 2 ? c : 2;
    }
    bool liqmap2_rails_profile_ = false;  // rails: the live rails' profile at the right edge (ref-6)
    Terminal::Pair liqmap2_rails_sibling() const;  // the coin on the other rails venue (Bybit, Binance)
    int liqmap2_rails_history_requested_ = 0;   // days of rails history asked for (each request one day)
    int64_t liqmap2_rails_view_lo_ = 0;
    int64_t liqmap2_rails_window_ = 0;   // the replay window start the history counter belongs to (0: live)         // the plot's left edge in real time, last frame (0 before one)
    StreamManager* liqmap2_rails_mgr_ = nullptr;
    liqmap2_rails_overlay::Cache liqmap2_rails_cache_;  // the rails picture, kept while its inputs hold
    std::vector<liqmap2_rails_overlay::Source> liqmap2_rails_sources_;  // this frame's venues, kept for their capacity
    liqmap2_rails::ChartCandles liqmap2_rails_candles_;  // closed candles and the forming one, as the rails meet them

    float liq_census_min_usd_ = 1000.0f;
    int liq_history_requested_ = 0;
    bool liq_census_enabled_ = false;      // ctor: defaults ON for HL-native pairs
    bool liq_census_subscribed_ = false;
    Terminal::Pair liq_census_pair_;       // {"hl", UNDERLYING} - resolved at construction
    // PERF: cached band selection (price/usd/side), recomputed only when the census
    // snapshot ts or the leverage mask changes (mirrors liq_shelf_cache_).
    struct CensusSel { double price = 0.0; double usd = 0.0; bool is_long = false; };
    std::vector<CensusSel> liq_census_cache_;
    int64_t liq_census_cache_ts_      = -1;
    uint8_t liq_census_cache_mask_    = 0xFF;
    double  liq_census_cache_max_usd_ = 0.0;
    // Hyperliquid positions as rails, built on the server (stream 42, history 43; core/hl_rails.h).
    hl_rails::View hl_rails_view_;             // the coin's rails through the viewer's leverage and Minimum $
    hl_rails_overlay::Cache hl_rails_cache_;   // the rails picture, kept while its inputs hold
    StreamManager* hl_rails_mgr_ = nullptr;    // the manager holding the live subscription
    int hl_rails_history_requested_ = 0;       // days of history asked for (one request a day)
    int64_t hl_rails_window_ = 0;              // the replay window start the history counter belongs to (0: live)
    int64_t hl_rails_view_lo_ = 0;             // the plot's left edge in real time, last frame (0 before one)
    uint8_t hl_groups_ = hl_rails::kAllGroups; // the real leverage shown (workspace "hl_groups")
    bool hl_profile_ = false;                   // the standing rails' profile at the right edge ("hl_profile")
    // With the V2 rails on too: Hyperliquid leads them ("hl_over_v2"). The V2 rails lead by default (James 2026-10-14:
    // following Hyperliquid they drew thin and dim and lost the map); the older "hl_lead" is not read.
    bool hl_over_v2_ = false;
    hl_rails_overlay::Key hl_key_;             // this frame's key, drawn over the candles
    hl_rails::Basis hl_basis_;                 // this chart's price over Hyperliquid's, one sample a census message
    std::string hl_basis_key_;                 // the chart market and coin the basis belongs to

    // ─── Indicators ──────────────────────────────────────────────────────
    Indicators::IndicatorManager indicator_mgr_;

    // ─── Display Config ──────────────────────────────────────────────────
    PriceFormatter fmt_;

    // ─── Drawing tools ───────────────────────────────────────────────────
    // All logic lives in drawing::DrawingLayer (ui/drawing/) + DrawingManager
    // (ctx_.drawing_mgr()) - this widget only calls render_in_plot()/
    // end_frame() and gates its own handlers on captures_mouse().
    drawing::DrawingLayer drawing_layer_;

    // ─── Settings Panels (reusable tabbed popups) ────────────────────────
    // Liq settings = the design floating panel: Style (colormap, opacity) +
    // Intensity (gamma, Low/Peak, noise floor, tick-per-row, half-life), wired
    // to the liq_field_* members above.
    SettingsPanel liq_settings_panel_{"liq_heatmap_settings",
        {"Appearance", "Style", "Intensity"}};
    SettingsPanel vpvr_settings_panel_{"vpvr_settings",
        {"Display", "Sizing"}};
    SettingsPanel footprint_settings_panel_{"footprint_settings",
        {"Mode", "Imbalances", "Display", "Sizing"}};
    SettingsPanel tpo_settings_panel_{"tpo_settings",
        {"General", "Session", "Value Area", "Single Prints"}};

    // ─── Rendering ───────────────────────────────────────────────────────
    void render_controls();
    void render_overlay_menu(bool liquidations, ImVec2 anchor);
    std::string liquidation_menu_detail_, layers_menu_detail_;
    bool hl_major_levels_ = true;
    bool rail_value_labels_ = false;
    void render_chart();
    void render_indicators();
#ifdef EDGEDEPTH_EXPOSURE_V2_DEV
    void update_exposure_v2();
    void render_exposure_v2_settings();
    void render_exposure_v2(int64_t clock);
    bool exposure_enabled_=false;
    bool exposure_fit_bands_=false; // Off: the price axis follows candles; bands 7% away are counted, not chased.
    int exposure_detail_=0; // Automatic, exact, averaged.
    bool exposure_averages_=false;
    exposure::SummaryHistory exposure_summary_;
    char exposure_scenario_[96]="";
    int64_t exposure_clock_=0,exposure_view_to_=0;
    exposure::History exposure_history_;
    exposure::Display exposure_display_;
#endif
    bool liquidity_response_enabled_=false;
    bool liquidity_response_evidence_open_=false;
    liquidity_response::History liquidity_response_history_;
    std::vector<liquidity_response::MarkerGroup> liquidity_response_markers_;
    uint64_t liquidity_response_marker_revision_=~uint64_t(0);
    int64_t liquidity_response_marker_tf_=0;
    void update_liquidity_response();
    // V2 liquidation map: the rails for every viewer with V2, the frames for admins' touch-odds fuel.
    void update_liqmap2();
    bool hl_coin_missing() const;  // the chart's coin has no Hyperliquid market (once the HL ticker feed is in)
    void render_liquidity_response();
    void render_liquidity_response_settings();
    // Candle-mode absorption history is a Pro read live (the server refuses Free); replay is open.
    bool absorption_locked() const;
    void open_absorption_upsell();
    // Refill levels (Real-time): chart_widget_refill_levels.cpp, core/refill_levels.h.
    bool rt_refill_enabled_ = true;
    StreamManager* refill_stream_mgr_ = nullptr;
    refill::LadderMarks refill_ladder_;
    std::array<uint16_t, refill::kMaxLevels> refill_order_{};
    std::vector<refill::Message> refill_outbox_;
    // One swelling absorption bubble per drawn level, behind the trades: what each has absorbed so far, and where
    // this frame's markers sit (for the hover in render_refill_levels).
    refill::Absorption refill_absorption_;
    struct RefillLens { uint16_t index = 0; ImVec2 center; float r = 0; };
    std::array<RefillLens, refill::kMaxDrawn> refill_lenses_{};
    int refill_lens_count_ = 0;
    bool refill_pack() const;
    refill::Access refill_access() const;
    bool refill_server_locked() const;
    bool refill_active() const;
    void open_refill_upsell(const char* cause);
    void update_refill_levels();
    void release_refill_levels();
    void render_refill_lenses(ImDrawList* dl, const ImPlotRect& limits, double minimum);
    void render_refill_levels(ImDrawList* dl, const ImPlotRect& limits);
    void render_refill_status(ImDrawList* dl);
    void render_refill_layer_row();
    // The entitlement token the host seeds and refreshes (window.__EDGEDEPTH_REPLAY_TOKEN__), read per request.
    static const char* refill_token();
    // Refill levels on candle charts (recorded history): chart_widget_refill_history.cpp, core/refill_history.h.
    bool candle_refill_enabled_ = false;
    bool refill_history_seeking_ = false;
    refill::history::Requester refill_history_req_;
    std::array<uint16_t, refill::history::kMaxLevels> refill_history_order_{};
    int refill_history_hover_ = -1;
    uint64_t refill_history_hover_rev_ = 0;
    bool refill_history_locked() const;
    bool refill_history_active() const;
    void update_refill_history();
    void release_refill_history();
    void render_refill_history();
    void render_refill_history_hover();
    void render_refill_history_note(const char* note);
    void render_refill_history_layer_row();
    void update_flow_positioning();
    void render_flow_positioning();
    flow_positioning::History flow_history_;
    void update_activity();
    void render_activity_settings();
    void render_bookmark_action();
    void render_activity_strip();
    void render_activity_alerts();
    void render_activity_bookmark();
    bool activity_visible() const;
    float activity_height() const;
    bool activity_enabled_=false, activity_collapsed_=true;
    bool activity_trades_=true,activity_volume_=true,activity_oi_=true,activity_buy_=true,activity_shading_=true,activity_book_enabled_=false;
    bool alert_lines_=false,alert_markers_=false,activity_replay_=false,activity_pending_=false;
    int activity_request_id_=0;
    int64_t activity_clock_=0,activity_from_=0,activity_to_=0,activity_requested_clock_=0,bookmark_time_=0,activity_rewind_cutoff_=0;
    uint64_t activity_book_generation_=0;
    double activity_next_read_=0,activity_request_sent_=0;
    float activity_header_height_=28.f;
    std::string activity_request_key_,activity_market_;
    activity_strip::Data activity_data_;
    activity_strip::Book activity_book_;
    char bookmark_note_[1024]={};
    int64_t flow_from_ = 0, flow_to_ = 0;
    bool flow_export_failed_ = false;
    void render_crosshair(const ImPlotPoint& mouse_pos) const;

    // Candle drawing via ImDrawList (replaces custom_implot dependency).
    // The four OHLC source spans are passed in so Candles draws the REAL arrays
    // and Heikin Ashi draws the derived HA arrays through the exact same tiers.
    // The live building candle's OHLC is passed separately (bld_*) because HA
    // needs it transformed per frame; bld_valid=false skips the right-edge candle.
    struct BuildingOHLC { double open; double high; double low; double close; bool valid; };
    void plot_candles(double visible_x_min, double visible_x_max,
                      const std::vector<double>& src_opens,
                      const std::vector<double>& src_highs,
                      const std::vector<double>& src_lows,
                      const std::vector<double>& src_closes,
                      const BuildingOHLC& bld);
    // The chart-type switch proper (footprint/TPO/Renko side effects).
    // set_chart_type wraps it with the real-time exit; set_rt_mode(false) calls
    // it directly to put the pre-real-time type back.
    void apply_chart_type(ChartType type);
    // Close-price polyline (ChartType::Line). Uses the recent-tick ring buffer
    // where available and falls back to candle closes for older regions.
    void plot_line(double visible_x_min, double visible_x_max);
    // Preallocated pixel-space path for plot_line (reused each frame - no STL
    // churn in the render loop).
    std::vector<ImVec2> line_pts_;

    // ─── Renko (ChartType::Renko) ─────────────────────────────────────────
    // Dedicated brick-index render path (self-contained BeginPlot/EndPlot),
    // delegated from render_chart() so the time-axis path is untouched.
    void render_chart_renko();
    // (Re)build the committed bricks if the closed-candle signature / brick size
    // changed, resolve the auto brick size on first use, and fold the building
    // candle in per frame. Cheap on the common path (signature unchanged).
    void ensure_renko();
    double renko_resolved_size();  // resolved brick size in price units (0 if unresolved)
    double renko_atr(int period) const;  // ATR(period) of loaded candles (price units), 0 if too few
    // Filled brick rectangles over the visible brick-index slice [i0, i1).
    void plot_renko(size_t i0, size_t i1);
    // Brick-index -> close-time X ticks (non-uniform, TradingView style).
    void setup_renko_axis_ticks(double vis_i_min, double vis_i_max) const;
    void render_renko_settings_popup();  // brick-size config (right-click the Renko row)
    mutable std::vector<double>      renko_tick_pos_;     // reused tick scratch
    mutable std::vector<std::string> renko_tick_lbls_;
    mutable std::vector<const char*> renko_tick_ptrs_;
    // Heikin Ashi extents over the visible window (drawn HA candles never clip);
    // out params stay untouched when there is no HA data in range.
    void ha_visible_extents(double visible_x_min, double visible_x_max,
                            double& y_min, double& y_max) const;
    void draw_ohlc_readout() const;  // OHLCV overlay, top-left of plot
    void draw_status_chip() const;   // LIVE/REPLAY status pill under the OHLC badge
    void draw_single_candle(ImDrawList* draw_list,
                            double x, double open, double close,
                            double low, double high,
                            double half_width,
                            const ImVec2& plot_min, const ImVec2& plot_max,
                            bool thin_body = false,
                            uint8_t alpha = 255);

    // Ghost scrub-preview pass (replay only): while the user aims a scrub on
    // the transport, renders the PreviewCandleStore's future candles ahead of
    // the playhead at reduced alpha (two tiers: playhead→aim stronger, beyond
    // the aim faint) plus a vertical line at the aimed-at time. Reads ONLY the
    // preview store - never CandleManager. Defined in
    // chart_widget_scrub_preview.cpp (keep it out of this God file).
    void render_scrub_preview(double visible_x_min, double visible_x_max);

    // Heatmap
    int64_t depth_timeframe_seconds() const;
    void request_heatmap_data();
    void update_heatmap();
    void update_live_heatmap_from_orderbook();
    void render_heatmap_tooltip();
    void toggle_heatmap();
    void calculate_heatmap_price_range(double& min_price, double& max_price);
    ImPlotColormap get_colormap_for_type(HeatmapType type) const;
    std::string format_time_hms(int64_t timestamp_ms);
    ImVec4 get_tooltip_cell_color(float normalized) const;

    // Admin-scoped live descending-resistance context.
    void handle_pattern_overlay(const Terminal::PatternOverlay& pattern);
    void render_pattern_overlay(double visible_x_min, double visible_x_max);

    // Liquidation heatmap overlay
    void render_liq_timeline();
    // The Field itself lives in liq_field_ (rendering/liq_field_renderer.h).
    void render_liq_profile();   // right-edge sidebar histogram (Profile:: renderer, mirrors VPVR)
    const std::vector<Terminal::Liquidation>& realtime_liquidations() const;
    void render_realtime_liquidation_strip();
    void render_liq_observed();  // WS4 Observed markers - real @forceOrder dots (drawn over candles)
    // Candle charts: callouts on the candles with unusual liquidations (observed_liquidations::callouts).
    void render_liq_callouts();
    struct LiqTypical { const void* data = nullptr; size_t n = 0; int64_t tf = 0, cutoff_bucket = 0; float min_usd = 0; double usd = 0; };
    LiqTypical liq_typical_;  // the typical candle, recomputed when reports, timeframe, clock candle or filter change
    // The callouts in view and their labels, recomputed only when what they read changes: the reports and which of
    // them the view holds, the typical candle, the threshold and filter, and the candles they group into (the pass
    // reads every report in view: thousands a day on a busy market, every frame otherwise).
    struct LiqCallouts {
        bool valid = false;
        const void* events = nullptr; const void* data = nullptr; size_t size = 0; uint64_t revision = 0;
        size_t first = 0, last = 0;
        double typical = 0; float multiple = 0, min_usd = 0;
        int64_t tf = 0, building = 0; const void* candles = nullptr; uint64_t candles_revision = 0;
        std::vector<observed_liquidations::CandleSide> marks;
        std::vector<std::array<char, 24>> labels;
        const ImFont* font = nullptr; float font_size = 0;  // the label widths' font
        std::vector<ImVec2> sizes;
    };
    LiqCallouts liq_callouts_;
    float liq_callout_x_ = static_cast<float>(observed_liquidations::kCalloutDefault);  // callout at this x typical
    void toggle_liq_census();    // the Hyperliquid positions layer on or off
    // Hyperliquid positions as rails in the V2 grammar (core/hl_rails.h, rendering/hl_rails_overlay.h). follow: the V2
    // rails lead, so these draw fainter beneath them.
    void render_liq_census(bool follow);
    // The live subscription (stream 42) and the stored days (stream 43), or the replay window's record.
    void update_hl_rails();
    // Free on BTC on Binance and Hyperliquid charts (James 2026-10-15), Pro elsewhere: the server serves HL BTC to anyone
    // and other coins to Pro tokens.
    bool hl_rails_free() const {
        return (pair_.exchange == "binancef" && pair_.symbol == "btcusdt") || (pair_.exchange == "hl" && pair_.symbol == "BTC");
    }
    bool hl_rails_allowed() const { return Entitlements::is_pro() || hl_rails_free(); }
    bool hl_rails_drawn() const {
        return !rt_mode_ && liq_census_enabled_ && hl_rails_allowed() && ct_allows_time_overlays(chart_type_);
    }
    // Hyperliquid's rails lead: alone, or over the V2 rails by choice (hl_over_v2_). The leader keeps its profile, labels
    // and hover; the other follows.
    bool hl_leads() const { return hl_rails_drawn() && (!liqmap2_rails_drawn() || hl_over_v2_); }
    // Liquidation rails fuel from observed HL positions (admin choice on HL charts, census subscribed).
    bool observed_rails_allowed() const;
    liq_rails::FuelSource observed_rails_fuel(int64_t clock) const;
    // V2 modelled map: who sees it (core/liqmap2_release.h, the web's release list); its rails fuel stays an
    // admin choice and needs the layer on.
    liqmap2::Access liqmap2_access() const;
    bool liqmap2_allowed() const;
    // Live data on this chart: not a replay context (research replay, archive, event, pack) and not an
    // embedded lesson, event or pack before its replay starts.
    bool live_chart() const;
    // One liquidation map (Layers > Liquidation map, liq_dense_field_). Live: V2's milestone rails while this
    // viewer has V2 here, else nothing (a locked PRO row); the scenario map (V1) never draws live. Replay,
    // packs and lessons: the scenario map, since V2 has no frames there.
    bool liqmap2_shown() const { return liq_dense_field_ && (live_chart() || rails_replay()) && liqmap2_allowed(); }
    // A box replay (research and studio replays, archive events, the terminal's own replays) shows the rails as
    // they were published at the time: the stored rails history is read for the replay window and drawn at the
    // replay clock (James 2026-10-11). Packs and lessons have no backend or predate the record, and keep the
    // scenario map.
    bool rails_replay() const;
    // The V2 rails draw on this chart: then they are the chart's one liquidation picture and the touch-odds rails'
    // forward columns step aside (the touch readout at the cursor stays).
    bool liqmap2_rails_drawn() const {
        return liqmap2_shown() && !rt_mode_ && ct_allows_time_overlays(chart_type_);
    }
    bool scenario_map_shown() const { return liq_dense_field_ && !live_chart() && !(rails_replay() && liqmap2_allowed()); }
    void render_liqmap2(bool follow);
    const char* touch_rails_off() const;

    // VPVR overlay
    void render_vpvr_profile();
    double compute_vpvr_tick_per_row(double price_range, double plot_height) const;

    // Footprint overlay
    void render_footprint_overlay(double visible_x_min, double visible_x_max);
    void render_footprint_profile(double visible_x_min, double visible_x_max);
    void render_footprint_settings_popup();

    // TPO / Market Profile
    void render_tpo(double visible_x_min, double visible_x_max);
    int  tpo_hovered_session_ = -1;  // index of session under mouse, -1 = none
    int  tpo_context_session_ = -1;  // session index for context menu
    bool tpo_zoom_pending_    = false; // zoom-out on TPO activation
    int  tpo_zoom_frames_     = 0;    // apply zoom for N frames to ensure ImPlot processes it

    // Interaction
    void handle_plot_interaction();

    // Time axis
    void generate_time_ticks(double visible_x_min, double visible_x_max) const;
    int64_t calculate_tick_interval(double visible_candles) const;
    int64_t align_to_interval(int64_t timestamp_s, int64_t interval_seconds) const;

    // Volume indicator helpers
    void populate_volume_data(Indicators::VolumeIndicator* vol_ind) const;
    void update_volume_indicators();

    // CVD indicator helpers
    void populate_cvd_data(Indicators::CVDIndicator* cvd_ind) const;
    void update_cvd_indicator();

    // Volume stream for CVD wicks (cvd_high/cvd_low from intra-candle tracking)
    void handle_volume(const Terminal::Volume& vol);
    struct CVDWickData { double cvd_high; double cvd_low; };
    std::unordered_map<int64_t, CVDWickData> cvd_wick_cache_;
    void subscribe_chart_streams();
    void unsubscribe_chart_streams();
    StreamManager* chart_stream_mgr_ = nullptr;
    bool volume_subscribed_ = false;
    int64_t volume_sub_tf_ms_ = 0;  // Timeframe (ms) of current Volume subscription

    // Stats stream for funding rate indicator
    void handle_stat_for_chart(const Terminal::Stat& stat);
    std::unordered_map<int64_t, double> funding_cache_;  // timestamp → funding rate
    double latest_funding_rate_ = 0;
    bool stats_subscribed_ = false;
    int64_t stats_sub_tf_ms_ = 0;

    // Liquidation event stream (WS4 Observed markers) - TF-independent (timeframe 0),
    // subscribed once in the ctor, events stored in LiquidationHeatmapManager.
    bool liq_events_subscribed_ = false;
    bool funding_history_requested_ = false;
    bool funding_data_dirty_ = false;

    // Funding rate helpers
    void populate_funding_data(Indicators::FundingRateIndicator* ind) const;
    void update_funding_indicator();

    // VPIN / Toxicity indicator helpers (Indicators V1 S1b - pathfinder).
    // Data lives in the shared IndicatorSeriesManager (ctx_.series_mgr());
    // the indicator's render cache repopulates when the series revision
    // moves. Live sub = STATE_VPIN (tf 0), lazy on first add; history =
    // get_historical_vpin with an F3 absolute range from the loaded candle
    // window (replay end-clamp is server-side).
    void update_vpin_indicator();
    bool vpin_subscribed_ = false;
    bool vpin_history_requested_ = false;
    uint64_t vpin_populated_revision_ = 0;
    // F3 chunk-until-covered history loop: the server returns the MOST
    // RECENT `count` rows of a window, so one request never guarantees
    // coverage - chunk toward the candle window's start until the gap
    // closes or a chunk brings nothing older (series floor / retention).
    int64_t vpin_hist_last_oldest_ = 0;   // oldest at last chunk (progress check)
    bool vpin_hist_exhausted_ = false;    // no older data - stop chunking
    std::chrono::steady_clock::time_point vpin_hist_last_req_{};

    // OI indicator helpers
    void populate_oi_data(Indicators::OIIndicator* ind) const;
    void update_oi_indicator();
    void request_historical_oi();
    struct OI_OHLC { double open, high, low, close; };
    std::unordered_map<int64_t, OI_OHLC> oi_ohlc_cache_; // timestamp → OHLC (from historical query)
    std::unordered_map<int64_t, double> oi_cache_;  // timestamp → OI USD (from live stat stream)
    double latest_oi_usd_ = 0;
    bool oi_data_dirty_ = false; // Set when new OI data arrives, triggers repopulate
    bool oi_history_requested_ = false;

    static const char* format_timeframe(int64_t seconds);
};
