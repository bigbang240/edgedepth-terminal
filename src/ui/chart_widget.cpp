#include "core/liq_field_tiers.h"
#include "ui/compression_overlay.h"
#include "ui/touch_odds_overlay.h"
// ═══════════════════════════════════════════════════════════════════════════════
// chart_widget.cpp - REFACTORED: Rendering + UI only
//
// ChartWidget no longer owns candle data. It reads from CandleManager via
// const accessors. All candle lifecycle (build, finalize, batch, scroll-load)
// is in CandleManager.
//
// Removed: handle_trade, handle_candle, handle_candle_batch,
//          build_candle_from_trade, finalize_current_candle,
//          get_candle_timestamp, initial_load, request_historical_candles,
//          check_and_load_more, should_load_more_candles, rebuild_cache,
//          plot_candlesticks, plot_candles_simple (replaced by plot_candles)
//
// Removed dependency: custom_implot.h/.cpp (draw_single_candle is inline)
// ═══════════════════════════════════════════════════════════════════════════════

#include "ui/chart_widget.h"
#include "core/terminal_request.h"
#include "rendering/observed_liquidations.h"
#include <type_traits>
#include "core/census_profile.h"
#include "rendering/hl_rails_overlay.h"
#include "rendering/liqmap2_rails_overlay.h"
#include "ui/open_url.h"
#include "core/liqmap2_scorecard.h"
#include "ui/realtime_navigation.h"
#include "core/drawing_manager.h"
#include "core/local_paper_engine.h"
#include "core/position_sizing.h"
#include "core/execution_bridge.h"
#include "ui/price_profile_renderer.h"
#include "ui/custom_implot.h"
#include "core/footprint_manager.h"
#include "core/heatmap_colormap.h"
#include "core/entitlements.h"
#include "core/stream_presence.h"
#include "core/display_time_zone.h"
#include "ui/upsell_modal.h"
#include "ui/research_moment_panel.h"  // right-click "Investigate this minute"
#include "core/research_url.h"         // the 60000 minute floor + UTC labels
#include "core/usage_emit.h"           // research_outcome_first_handoff
#include "education/recorder_runtime.h"  // suppress the status chip in produced clips
#include "rendering/theme.h"
#include "core/ui_test_marks.h"
#include "rendering/app_shell.h"
#include "rendering/menu.h"            // "+ widget" toolbar menu → picker / add-request
#include "core/education_boot.h"       // is_embedded()/is_pack() gate the add path
#include "core/recorder_glue.h"        // ClipRecorder::focus_active gates the rail
#include "ui/drawing/drawing_toolbar.h"  // in-chart rail + Draw dropdown
#include "ui/drawing/drawing_icons.h"
#include "implot.h"
#include <cmath>
#include "implot_internal.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <limits>
#include <ctime>
#include <ranges>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include "indicators/volume_indicator.h"
#include "replayer/replay_manager.h"
#include "core/preview_candle_store.h"  // scrub-preview (ghost candles) y-fit
#include "core/ticker_manager.h"        // HL market registry for the census pill grey-out
#include "types/frame_profiler.h"
#include "education/chart_projection.h"


// Single global chart-coordinate snapshot for the lesson spotlight overlay.
namespace edu {
ChartProjection& chart_projection() {
    static ChartProjection p;
    return p;
}
}

// The outcome-first handoff: open /research in a NEW tab for the dragged move.
// Never a navigation - the terminal holds live WS state and possibly a replay
// session, and navigating away throws both away (same rule as
// ResearchMomentPanel::open_handoff, which this mirrors).
static void open_outcome_first_handoff(const Terminal::Pair& pair,
                                       const research_url::MoveSnap& move,
                                       int64_t range_minutes) {
    const std::string url = research_url::investigation_url(pair.symbol, move, pair.exchange);
    if (url.empty()) return;
    // Hand-built JSON rather than nlohmann here: every value is either a
    // literal from the closed ladder/horizon lists or a number, and this is
    // the heaviest translation unit in the build.
    char detail[320];
    snprintf(detail, sizeof(detail),
             "{\"event\":\"research_outcome_first_handoff\",\"mode\":\"terminal\","
             "\"symbol\":\"%s\",\"props\":{\"direction\":\"%s\",\"magnitude\":%g,"
             "\"horizon\":\"%s\",\"range_minutes\":%lld}}",
             research_url::normalize_symbol(pair.symbol).c_str(), move.direction, move.magnitude,
             move.horizon, static_cast<long long>(range_minutes));
    usage::dispatch_detail(detail);
#ifdef __EMSCRIPTEN__
    EM_ASM({ window.open(UTF8ToString($0), '_blank', 'noopener'); }, url.c_str());
#else
    (void)url;
#endif
}

// The two research reads - shared by the candle (##ChartCtx) and Renko
// (##RenkoCtx) context menus, placed ABOVE the replay block in both (they are
// reads, not replays). Enabled only for Binance USDT-M: that is all the C++
// side can honestly know - tier, record edge and absent readings belong to
// the web surface, which already says the right thing for each.
static void render_investigate_menu_item(const Terminal::Pair& pair, int64_t minute_ms,
                                         const research_url::MoveSnap& move,
                                         int64_t range_minutes) {
    const bool on_record = (pair.exchange == "binancef");
    const bool enabled = on_record && minute_ms > 0;
    std::string shortcut;
    if (enabled) shortcut = research_url::minute_label_utc(minute_ms);
    // "Find moments like this" (renamed 2026-08-24, James): the flagship
    // journey is see something -> right-click -> find similar -> replay,
    // and the menu item names the outcome, not the mechanism.
    if (Theme::menu_item(drawing::UiIcon::Search, "Find moments like this",
                         shortcut.empty() ? nullptr : shortcut.c_str(), enabled)) {
        ui::ResearchMomentPanel::instance().open(pair.symbol, minute_ms);
        ImGui::CloseCurrentPopup();
    }
    if (!on_record && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        Theme::tooltip("The record covers Binance USDT-M");
    }

    // "Investigate this move" - the same journey read the other way round: the
    // shift-dragged range IS the outcome, and the door asks what preceded moves
    // like it across the sector. Needs a range that snapped onto the ladder, so
    // the shortcut column states the target the click will actually open.
    const bool move_enabled = on_record && move.end_minute_ms > move.start_minute_ms && move.start_minute_ms > 0;
    const std::string move_shortcut = move_enabled ? "Through " + research_url::minute_label_utc(move.end_minute_ms) : "";
    if (Theme::menu_item(drawing::UiIcon::Move, "Investigate this move",
                         move_shortcut.empty() ? nullptr : move_shortcut.c_str(),
                         move_enabled)) {
        open_outcome_first_handoff(pair, move, range_minutes);
        ImGui::CloseCurrentPopup();
    }
    if (!move_enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        Theme::tooltip("%s", on_record ? "Shift-drag at least one minute of loaded candles.\n"
                                        "Use Select move on the investigation page to type a range."
                                       : "The record covers Binance USDT-M");
    }
}


// Read the shift-drag selection as a move: the anchor close, the extremes it
// reached and where it finished, at the chart's own timeframe. Timeframe
// resolution is fine here - the snap rounds the magnitude down and the horizon
// up, and the engine recomputes the population from one-minute data.
ChartWidget::SelectedMove ChartWidget::read_selected_move() const {
    SelectedMove out;
    const int64_t a = replay_selection_.start_ms;
    const int64_t b = replay_selection_.end_ms;
    if (a <= 0 || b <= 0 || a == b) return out;  // no range: nothing was dragged
    const int64_t from = std::min(a, b);
    const int64_t tf_ms = candles().timeframe_seconds() * 1000;
    if (tf_ms <= 0) return out;
    int64_t loaded_end = candles().candles().empty() ? 0 : candles().candles().back().timestamp_ms + tf_ms;
    if (candles().has_building_candle())
        loaded_end = std::max(loaded_end, candles().building_candle().timestamp_ms + tf_ms);
    const int64_t clock_ms = ctx_.replay_mgr().is_active()
        ? ctx_.replay_mgr().interpolated_time_ms()
        : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t to = research_url::observed_move_end(std::max(a, b), loaded_end, clock_ms);
    if (to <= research_url::floor_minute_ms(from) || to - from > 7 * 86400000LL) return out;

    double start_close = 0.0, end_close = 0.0, max_high = 0.0, min_low = 0.0;
    bool have_range = false;
    const auto fold = [&](const Terminal::Candle& c) {
        if (c.timestamp_ms <= from) start_close = c.close;  // the candle holding the drag start
        if (c.timestamp_ms + tf_ms <= from) return;         // closed before the range opened
        if (c.timestamp_ms >= to) return;                   // opened after the range closed
        if (!have_range) {
            max_high = c.high;
            min_low = c.low;
            have_range = true;
        } else {
            max_high = std::max(max_high, c.high);
            min_low = std::min(min_low, c.low);
        }
        end_close = c.close;
    };
    for (const auto& c : candles().candles()) {
        if (c.timestamp_ms >= to) break;  // deque is ascending by timestamp
        fold(c);
    }
    // The forming candle lives outside the deque, so a drag that runs to the
    // live edge would otherwise stop one candle short of what the user saw.
    if (candles().has_building_candle()) fold(candles().building_candle());

    if (!have_range) return out;
    out.snap = research_url::snap_move(from, to, start_close, end_close, max_high, min_low);
    out.snap.start_minute_ms = research_url::floor_minute_ms(from);
    out.snap.end_minute_ms = research_url::floor_minute_ms(to);
    out.range_minutes = (to - from) / research_url::kMinuteMs;
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Construction / Destruction
// ═══════════════════════════════════════════════════════════════════════════════

// P2e census: resolve the HL census pair ({"hl", UNDERLYING}) for any chart pair.
// The census feed is keyed by underlying coin, so any venue's chart of the same
// underlying subscribes the same subject. HL-native pairs are their own underlying
// (uppercase coins, "BTC"); binancef perps are lowercase "<base><quote>" - strip the
// stable-quote suffix and uppercase. Binance's 1000-bundled tickers map to HL's
// k-prefix ("1000pepe" → "kPEPE"; both price 1000 units, so USD scales align).
static Terminal::Pair hl_census_pair_for(const Terminal::Pair& pair) {
    if (pair.exchange == "hl") return pair;
    std::string base = pair.symbol;
    for (const char* q : {"usdt", "usdc", "fdusd", "busd"}) {
        const std::string_view qv{q};
        if (base.size() > qv.size() && std::string_view(base).ends_with(qv)) {
            base.resize(base.size() - qv.size());
            break;
        }
    }
    std::string sym;
    sym.reserve(base.size() + 1);
    if (base.rfind("1000", 0) == 0) { sym += 'k'; base.erase(0, 4); }
    for (char c : base)
        sym += (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
    return {"hl", sym};
}

// Link-group state shared by every time-linked chart. One group: a canvas
// plays one clock. Frame stamps, not flags, so a chart that stops rendering
// (closed, hidden tab) drops out of the group without a teardown hook.
namespace {
struct TimeLink {
    bool   enabled = false;    // user toggle ("Link charts", workspace link_charts)
    // Candle-family owner: candle charts sync their full axis verbatim (or
    // just the follow latch when the owner follows live, both landing on the
    // same clock at the same timeframe).
    int    owner = 0;            // link_id_ of the chart that last drove the axis
    int    owner_frame = -1;
    double x_min = 0.0, x_max = 0.0;
    bool   follow = true;        // the owner's follow-live latch
    // Real-time family owner: RT charts follow the live edge from the shared
    // wall clock, so they sync the owner's zoom SPAN while live (each chart
    // keeps its own right edge, honouring a paused view) and the full axis
    // verbatim once the owner is scrolled back into history. The families
    // never take each other's ranges; the crosshair mirror crosses both.
    int    rt_owner = 0;
    int    rt_owner_frame = -1;
    double rt_x_min = 0.0, rt_x_max = 0.0;
    bool   rt_follow = true;
    int    hover_owner = 0;      // link_id_ of the chart under the mouse
    int    hover_frame = -1;
    double hover_x = 0.0;
    int    tf_seconds = 0;       // last timeframe broadcast
    int    tf_frame = -1;
    int    tf_owner = 0;
    int    next_id = 1;
};
TimeLink g_time_link;
}  // namespace

bool ChartWidget::link_charts_enabled() { return g_time_link.enabled; }
void ChartWidget::set_link_charts_enabled(bool on) { g_time_link.enabled = on; }
bool ChartWidget::link_active() const { return time_linked_ || g_time_link.enabled; }

CandleManager& ChartWidget::candles() const {
    if (owned_candles_) return *owned_candles_;
    if (compare_ && ctx_.replayer) {
        if (const DataContext* rc = ctx_.replay_mgr().replay_context()) {
            if (CandleManager* cm = rc->candles_for(pair_)) return *cm;
        }
    }
    return ctx_.candle_mgr();
}

ChartWidget::ChartWidget(
    const Terminal::Pair& pair,
    const AppContext& ctx,
    double tick_size,
    int instance,
    bool compare)
    : pair_(pair)
    , ctx_(ctx)
    , chart_instance_(std::max(1, instance))
    , compare_(compare)
    , tick_size_(tick_size)
    , last_heatmap_update_ms_(std::chrono::steady_clock::now())
    , liq_field_(ctx)
{
    link_id_ = g_time_link.next_id++;
    // Self-hosted charts start with depth only. Saved workspace choices win.
    rt_bubbles_ = Entitlements::hosted();
    if (compare_) time_linked_ = true;
    // A secondary chart cannot share the app-level manager: that holds ONE
    // timeframe and ONE follow-live latch, which is exactly what the two
    // charts must disagree on. Mirror main.cpp's construction of the app
    // manager and DataContext's owned one, starting at the primary's current
    // timeframe so the new chart opens looking like the one beside it. The
    // StreamManager keeps a handler list per key, so a second trade
    // subscriber for the pair and a candle subscriber for another timeframe
    // both coexist with the primary's; the server subscription is shared and
    // only released when the last handler leaves.
    //
    // The same applies to a chart of a DIFFERENT pair than the app-level
    // manager's (opened from the picker in the live terminal): the app
    // manager builds one market's candles, so such a chart must own its own
    // or it draws the primary's candles under its own title. A compare chart
    // is the exception: its manager already exists in the replay context.
    // Live only, like the secondary rule above: during a replay the
    // StreamManager in the context is the session's and is retired on exit,
    // so a manager subscribed to it would dangle; such a chart keeps the old
    // behaviour (reads the app-level manager).
    const bool in_replay = ctx_.replayer && ctx_.replay_mgr().is_active();
    const bool other_pair = !compare_ && !in_replay &&
        (ctx_.candle_mgr().pair().exchange != pair_.exchange ||
         ctx_.candle_mgr().pair().symbol   != pair_.symbol);
    DataContext* replay_ctx = in_replay && !compare_ ? ctx_.replay_mgr().replay_context() : nullptr;
    if (chart_instance_ > 1 && replay_ctx) {
        // A secondary chart opened during a replay: its own manager on the
        // replay StreamManager, built the way DataContext builds the compare
        // managers, then brought to the playhead exactly as a seek would. The
        // context lists it so later seeks, trims and suppressions reach it;
        // the chart is a replay widget, so it closes with the replay and is
        // erased a frame before the context (and this StreamManager) is freed.
        owned_candles_ = std::make_unique<CandleManager>(
            pair_, candles().timeframe_seconds(), ctx_.stream_mgr());
        owned_candles_->subscribe();
        owned_candles_->mark_ready_for_replay();
        int64_t playhead_ms = ctx_.replay_mgr().interpolated_time_ms();
        if (playhead_ms <= 0 && replay_ctx->candles)
            playhead_ms = replay_ctx->candles->replay_start_time_ms();
        owned_candles_->reset_for_seek(playhead_ms);
        replay_ctx->chart_candles.push_back(owned_candles_.get());
        replay_candles_ctx_ = replay_ctx;
        liq_field_.set_candles(owned_candles_.get());
    } else if (!in_replay && (chart_instance_ > 1 || other_pair)) {
        owned_candles_ = std::make_unique<CandleManager>(
            pair_, candles().timeframe_seconds(), ctx_.stream_mgr());
        // Subscribes and requests the preload. The primary's initial_load runs
        // on socket open (main.cpp); a secondary is only ever created from the
        // UI of a connected live terminal, so the socket is already up.
        owned_candles_->initial_load();
        liq_field_.set_candles(owned_candles_.get());
    }
    // TUT demo depth is easier to read with ten-tick rows. This is only
    // an initial pack default; the Depth settings still own user changes.
    if (EducationBoot::instance().is_pack() && pair_.exchange == "binancef" &&
        pair_.symbol == "tutusdt") rt_bucket_multiplier_ = 10;
    title_tf_seconds_ = candles().timeframe_seconds();
    timeframe_label_ = timeframe_to_string(title_tf_seconds_);
    rebuild_title();
    // tick_size_ == 0 means the registry has not answered for this pair yet.
    // The axis still has to print something, so it runs on price-magnitude
    // precision until refresh_instrument() binds the exchange's own.
    fmt_ = tick_size_ > 0.0 ? PriceFormatter::from_tick_and_step(tick_size_, 0.001)
                            : PriceFormatter{};

    // P2e: the census layer defaults ON for HL-native pairs - there the census IS
    // the ground-truth predictive layer (the modelled heatmap has no HL publisher).
    // On other venues it's the opt-in cross-venue overlay (the pill).
    liq_census_pair_ = hl_census_pair_for(pair_);
    liq_census_enabled_ = (pair_.exchange == "hl");
    // Candle-chart defaults only, within paid/replay/standalone access. A fresh
    // hosted free candle chart must not start with the locked Pro layer already drawing.
    liq_observed_enabled_ = Entitlements::is_pro() || !Entitlements::hosted() ||
        ctx_.replay_mgr().is_active() || EducationBoot::instance().is_pack();
    // Every pack replay opens with its recorded liquidation reports in the
    // Real-time view and the strip under it: packs carry those reports, and a
    // demo of a cascade without them hides the point (2026-10-18; until then
    // this was a TUTUSDT-only default, so the ARCUSDT welcome opened without).
    if (EducationBoot::instance().is_pack()) {
        rt_liq_observed_enabled_ = true;
        rt_liq_strip_ = true;
    }

    subscribe_chart_streams();

    // Research rollout only. This is an explicit UI gate; the dedicated stream id
    // remains separate from ordinary pattern traffic and the server retains
    // authority over delivery.
    if (Entitlements::is_research()) {
        const StreamKey pattern_key{pair_, Terminal::Stream::PatternAdmin, 0};
        StreamHandler<Terminal::PatternOverlay> pattern_handler{
            .widget_ptr = this,
            .callback = [](void* ptr, const Terminal::PatternOverlay& pattern) {
                static_cast<ChartWidget*>(ptr)->handle_pattern_overlay(pattern);
            }
        };
        pattern_stream_mgr_ = &ctx_.stream_mgr();
        pattern_stream_mgr_->subscribe_patterns(pattern_key, pattern_handler);
        pattern_subscribed_ = true;
    }
}

ChartWidget::~ChartWidget() {
    if(activity_request_id_)terminal_request_cancel(activity_request_id_);
    if (replay_candles_ctx_) std::erase(replay_candles_ctx_->chart_candles, owned_candles_.get());
    if (rt_stream_mgr_)
        rt_stream_mgr_->unsubscribe_direct({pair_, Terminal::Stream::Orderbook, 0}, this);
    release_refill_levels();
    release_refill_history();
    if (heatmap_stream_mgr_)
        heatmap_stream_mgr_->unsubscribe_direct({pair_, Terminal::Stream::Heatmap, 0}, this);
    if (footprint_stream_mgr_)
        footprint_stream_mgr_->unsubscribe_direct(
            {pair_, Terminal::Stream::TickVolume, 60}, this);
    unsubscribe_chart_streams();
    if (pattern_subscribed_ && pattern_stream_mgr_) {
        const StreamKey pattern_key{pair_, Terminal::Stream::PatternAdmin, 0};
        pattern_stream_mgr_->unsubscribe_patterns(pattern_key, this);
    }
    if (liq_census_subscribed_) {
        const StreamKey key{liq_census_pair_, Terminal::Stream::LiquidationLevels, 0};
        ctx_.stream_mgr().send_unsubscribe(key);
    }
    if (liqmap2_stream_mgr_) liqmap2_stream_mgr_->unsubscribe_direct({pair_, Terminal::Stream::LiqMapV2, 0}, this);
    if (hl_rails_mgr_) hl_rails_mgr_->unsubscribe_direct({liq_census_pair_, Terminal::Stream::HLRails, 0}, this);
    if (liqmap2_rails_mgr_) {
        liqmap2_rails_mgr_->unsubscribe_direct({pair_, Terminal::Stream::LiqMapV2Rails, 0}, this);
        if (const auto sib = liqmap2_rails_sibling(); !sib.exchange.empty())
            liqmap2_rails_mgr_->unsubscribe_direct({sib, Terminal::Stream::LiqMapV2Rails, 0}, this);
    }
}

// Callback ownership follows the manager used for registration, even when the
// shared AppContext has already switched between live and replay managers.
void ChartWidget::subscribe_chart_streams() {
    chart_stream_mgr_ = &ctx_.stream_mgr();
    // Subscribe to Volume stream for CVD intra-candle wicks
    {
        volume_sub_tf_ms_ = candles().timeframe_seconds() * 1000;
        StreamKey vol_key{pair_, Terminal::Stream::Volumes, volume_sub_tf_ms_};
        StreamHandler<Terminal::Volume> vol_handler{
            .widget_ptr = this,
            .callback = [](void* ptr, const Terminal::Volume& v) {
                static_cast<ChartWidget*>(ptr)->handle_volume(v);
            }
        };
        chart_stream_mgr_->subscribe_volume(vol_key, vol_handler);
        volume_subscribed_ = true;
    }

    // Subscribe to Stats stream for funding rate data
    {
        stats_sub_tf_ms_ = candles().timeframe_seconds() * 1000;
        StreamKey stat_key{pair_, Terminal::Stream::Stats, stats_sub_tf_ms_};
        StreamHandler<Terminal::Stat> stat_handler{
            .widget_ptr = this,
            .callback = [](void* ptr, const Terminal::Stat& s) {
                static_cast<ChartWidget*>(ptr)->handle_stat_for_chart(s);
            }
        };
        chart_stream_mgr_->subscribe_stats(stat_key, stat_handler);
        stats_subscribed_ = true;
    }

    // Subscribe to the discrete @forceOrder liquidation stream (WS4 Observed
    // markers). TF-independent (timeframe 0); live frames come from the
    // LIQUIDATIONS NATS stream, replay frames from the archive bundle / the
    // liquidation_events DB seed. Events land in LiquidationHeatmapManager.
    {
        StreamKey liq_key{pair_, Terminal::Stream::Liquidations, 0};
        StreamHandler<Terminal::Liquidation> liq_handler{
            .widget_ptr = this,
            .callback = [](void* ptr, const Terminal::Liquidation& l) {
                auto* w = static_cast<ChartWidget*>(ptr);
                w->ctx_.liq_heatmap_mgr().add_observed_event(w->pair_, l);
                w->on_liquidation_observed(l);  // Phase 2 S1: cascade chains
            }
        };
        chart_stream_mgr_->subscribe_liquidations(liq_key, liq_handler);
        liq_events_subscribed_ = true;
    }

}

void ChartWidget::unsubscribe_chart_streams() {
    if (!chart_stream_mgr_) return;
    if (volume_subscribed_) {
        StreamKey vol_key{pair_, Terminal::Stream::Volumes, volume_sub_tf_ms_};
        chart_stream_mgr_->unsubscribe_volume(vol_key, this);
    }
    if (stats_subscribed_) {
        StreamKey stat_key{pair_, Terminal::Stream::Stats, stats_sub_tf_ms_};
        chart_stream_mgr_->unsubscribe_stats(stat_key, this);
    }
    if (liq_events_subscribed_) {
        StreamKey liq_key{pair_, Terminal::Stream::Liquidations, 0};
        chart_stream_mgr_->unsubscribe_liquidations(liq_key, this);
    }
    volume_subscribed_ = stats_subscribed_ = liq_events_subscribed_ = false;
    cascade_detector_.reset();  // chains are per-symbol observed tails
    alert_engine_.reset();      // S2: threshold queues are per-symbol too
    liq_estimator_.reset();     // S3: the estimate is per-symbol as well
    last_pull_alert_ms_ = 0;
    chart_stream_mgr_ = nullptr;
}

// Visible prefix carries the TF; identity after "###" is TF-independent so the
// chart stays docked across TF changes (matches layout.cpp's chart dock id).
// The primary's string is unchanged from before secondary charts existed, so
// saved layouts and the default dock tree still match it. A secondary appends
// its instance to both halves: the label so the tab reads as a second chart,
// the identity so ImGui gives it a window of its own.
void ChartWidget::rebuild_title() {
    // Real-time has no timeframe; the tab names the view, so a docked
    // real-time chart reads as that and not as another 1m chart.
    title_ = rt_mode_ ? std::string("     Real-time  ") + widget_symbol_label(pair_.symbol)
                      : std::string("     Chart  ") + widget_symbol_label(pair_.symbol) + " " + timeframe_label_;
    if (chart_instance_ > 1) title_ += " (" + std::to_string(chart_instance_) + ")";
    title_ += "###chart_" + pair_.exchange + "_" + pair_.symbol;
    if (chart_instance_ > 1) title_ += "_" + std::to_string(chart_instance_);
}

bool ChartWidget::is_loading() const {
    return candles().is_loading();
}

std::string ChartWidget::timeframe_to_string(int64_t seconds) {
    if (seconds < 60) return std::to_string(seconds) + "s";
    if (seconds < 3600) return std::to_string(seconds / 60) + "m";
    if (seconds < 86400) return std::to_string(seconds / 3600) + "h";
    return std::to_string(seconds / 86400) + "d";
}


// Frame Update
void ChartWidget::refresh_instrument() {
    const auto& reg = SymbolRegistry::instance();
    const double tick = reg.tick_or_zero(pair_.exchange, pair_.symbol);
    if (tick <= 0.0 || tick == tick_size_) return;
    tick_size_ = tick;
    fmt_ = reg.get_formatter(pair_.exchange, pair_.symbol);
    // Renko bricks are counted in TICKS and frozen once resolved, so a brick
    // resolved against no grid has to be re-resolved against the real one.
    renko_resolved_ticks_ = 0;
    renko_sig_size_ = -1.0;
}

void ChartWidget::set_chart_type(ChartType type) {
    if (rt_mode_) set_rt_mode(false);   // puts the pre-real-time type back first
    apply_chart_type(type);
}

void ChartWidget::apply_chart_type(ChartType type) {
    const ChartType previous = chart_type_;
    chart_type_ = type;
    if (previous != type) flow_history_.reset();
    auto& fp = ctx_.footprint_mgr();
    fp.enabled = type == ChartType::FootprintCluster || type == ChartType::FootprintProfile;
    if (type == ChartType::FootprintCluster) fp.mode = FootprintManager::Mode::SellsBuys;
    if (type == ChartType::FootprintProfile) fp.mode = FootprintManager::Mode::Profile;
    if (type == ChartType::TPO) { tpo_zoom_pending_ = true; tpo_zoom_frames_ = 3; }
    if (type == ChartType::Renko || previous == ChartType::Renko) {
        candles().set_follow_live(true);
        last_visible_range_.X.Min = last_visible_range_.X.Max = 0.0;
        last_visible_range_.Y.Min = last_visible_range_.Y.Max = 0.0;
        renko_view_t0_ms_ = renko_view_t1_ms_ = 0;
    }
}

// Browser-side side effects shared by every crossing alert (Phase 3): a
// desktop notification when permitted plus a short beep at the given tone.
#ifdef __EMSCRIPTEN__
static void alert_notify_beep(const char* msg, double freq) {
    EM_ASM({
        try {
            var msg = UTF8ToString($0);
            if (window.Notification && Notification.permission === 'granted')
                new Notification('EdgeDepth alert', { body: msg });
            var AC = window.AudioContext || window.webkitAudioContext;
            if (AC) { var c = new AC(); var o = c.createOscillator();
                var g = c.createGain(); o.frequency.value = $1;
                o.connect(g); g.connect(c.destination); g.gain.value = 0.06;
                o.start(); o.stop(c.currentTime + 0.18); }
        } catch (e) {}
    }, msg, freq);
}
#else
static void alert_notify_beep(const char*, double) {}
#endif

// Phase 5: auto-open a local paper position on an alert crossing (price alert
// or drawn-line crossing). Entry at the trigger price, stop = stop%% away,
// TP1 = 1R / TP2 = 2R, quantity risk-based from position_sizing. Rejections
// (cooldown, duplicate, sizing) fail silently - the toast only reports opens.
void ChartWidget::maybe_paper_on_alert(bool long_side, double price,
                                       const char* source) {
    auto* eng = ctx_.local_paper;
    if (!eng || !(price > 0.0)) return;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const bool ok = eng->open(pair_.exchange.c_str(), pair_.symbol.c_str(),
                              long_side, price,
                              std::clamp(paper_auto_stop_pct_, 0.05, 20.0),
                              source, now) != nullptr;
    if (!ok) return;
    char pbuf[32];
    fmt_.format_price(pbuf, sizeof(pbuf), price);
    std::string sym = pair_.symbol;
    for (char& c : sym) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    alert_toast_msg_ = std::string("Paper ") + (long_side ? "long" : "short") +
                       " opened at " + pbuf;
    alert_toast_until_ = ImGui::GetTime() + 5.0;
    alert_notify_beep(alert_toast_msg_.c_str(), 600);
    eng->tick(now);
}

void ChartWidget::update() {
    ProfileScope _ps("ChartUpd");
    time_link_adopt_timeframe();
    update_reference_context();
    update_flow_positioning();
    update_activity();
    update_liquidity_response();
    update_refill_levels();
    update_refill_history();
    update_liqmap2();
    update_hl_rails();
#ifdef EDGEDEPTH_EXPOSURE_V2_DEV
    update_exposure_v2();
#endif
    if (rt_archive_) capture_realtime_archive();
    // Real-time mode draws sampled depth from its own subscription, so the
    // candle heatmap stream is dropped for the duration and re-requested on exit.
    if (heatmap_stream_mgr_ &&
        (rt_mode_ || !heatmap_enabled_ || !ct_allows_time_overlays(chart_type_) ||
         heatmap_stream_mgr_ != &ctx_.stream_mgr())) {
        heatmap_stream_mgr_->unsubscribe_direct({pair_, Terminal::Stream::Heatmap, 0}, this);
        heatmap_stream_mgr_ = nullptr;
        heatmap_data_requested_ = false;
    }
    // Replay can replace the data context after the view was first applied.
    // Keep the scripted footprint mode on the current manager as well.
    if (const auto* view = edu::RecorderRuntime::instance().view()) {
        if (view->chart_type == static_cast<int>(ChartType::FootprintCluster))
            ctx_.footprint_mgr().mode = FootprintManager::Mode::SellsBuys;
        else if (view->chart_type == static_cast<int>(ChartType::FootprintProfile))
            ctx_.footprint_mgr().mode = FootprintManager::Mode::Profile;
    }
    const bool wants_footprint = chart_type_ == ChartType::FootprintCluster ||
                                 chart_type_ == ChartType::FootprintProfile;
    if (footprint_stream_mgr_ &&
        (!wants_footprint || footprint_stream_mgr_ != &ctx_.stream_mgr())) {
        footprint_stream_mgr_->unsubscribe_direct(
            {pair_, Terminal::Stream::TickVolume, 60}, this);
        footprint_stream_mgr_ = nullptr;
    }
    if (wants_footprint && !footprint_stream_mgr_) {
        footprint_stream_mgr_ = &ctx_.stream_mgr();
        footprint_stream_mgr_->subscribe_direct(
            {pair_, Terminal::Stream::TickVolume, 60}, this);
    }
    // Provisional precision while the instrument is unknown: better a chart
    // axis at price-magnitude precision than one printing 0.24 three rows
    // running. Replaced wholesale by refresh_instrument().
    if (!fmt_.resolved) {
        const double px = candles().last_close_price();
        if (px > 0.0) fmt_ = PriceFormatter::provisional_for_price(px);
    }
    if (chart_type_ == ChartType::Renko) {
        // Renko's last_visible_range_.X is a BRICK-INDEX domain - never feed that
        // to CandleManager (its scroll-load thresholds are timestamps). Use the
        // visible bricks' TIME window published by render_chart_renko instead.
        if (renko_view_t1_ms_ == 0) return;
        candles().update(static_cast<double>(renko_view_t0_ms_),
                                 static_cast<double>(renko_view_t1_ms_));
    } else {
        if (last_visible_range_.X.Max == 0 || last_visible_range_.Y.Max == 0 ||
            last_visible_range_.X.Min == 0) {
            return;
        }
        candles().update(last_visible_range_.X.Min, last_visible_range_.X.Max);
    }

    // CLIP_FACTORY P2: one-shot view overrides from the recorder script.
    // Layer state is the produced clip's visual grammar; scripts pin it
    // explicitly instead of inheriting whatever this build's defaults are.
    if (!recorder_view_applied_ && edu::RecorderRuntime::instance().active()) {
        if (const auto* v = edu::RecorderRuntime::instance().view()) {
            if (v->chart_type >= 0) set_chart_type(static_cast<ChartType>(v->chart_type));
            if (v->realtime) set_rt_mode(true);
            if (v->liq_field >= 0 && liq_dense_field_ != (v->liq_field != 0)) {
                liq_dense_field_ = v->liq_field != 0;
                liq_shelf_cache_ts_ = -1;
            }
            // v->liq_levels is accepted and IGNORED: the Liq Levels (rails) layer was
            // retired 2026-09-05 (no live producer since 2026-07-22). Saved views and
            // lesson recordings still carry the field, so keep parsing it rather than
            // failing to load them.
            if (v->liq_profile >= 0) liq_profile_enabled_ = v->liq_profile != 0;
            if (v->liq_observed >= 0) reported_liquidations_enabled() = v->liq_observed != 0;
            if (v->ob_depth >= 0) {
                heatmap_enabled_ = v->ob_depth != 0;
                if (v->realtime) rt_depth_enabled_ = heatmap_enabled_;   // recorded RT views used one flag
            }
            if (v->vpvr >= 0 && vpvr_enabled_ != (v->vpvr != 0)) {
                vpvr_enabled_ = v->vpvr != 0;
                ctx_.vpvr_mgr().set_enabled(vpvr_enabled_);
                if (vpvr_enabled_) { vpvr_last_request_start_ = 0; vpvr_last_request_end_ = 0; }
            }
        }
        recorder_view_applied_ = true;
    }

    // ── Client-side price alerts ──────────────────────────────────────────
    // Fire a browser notification + in-app toast + beep when the live price
    // crosses a user-set level (added from the chart right-click menu, live
    // mode only). One-shot: the level clears once it fires. Session-scoped.
    if (!alerts_.empty()) {
        const double px = candles().last_close_price();
        if (px > 0.0 && alert_last_price_ > 0.0) {
            for (size_t i = 0; i < alerts_.size();) {
                const PriceAlert& a = alerts_[i];
                const bool crossed = a.arm_above
                    ? (px >= a.price && alert_last_price_ < a.price)
                    : (px <= a.price && alert_last_price_ > a.price);
                if (crossed) {
                    char pbuf[32];
                    fmt_.format_price(pbuf, sizeof(pbuf), a.price);
                    std::string sym = pair_.symbol;
                    for (char& c : sym) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
                    alert_toast_msg_ = sym + " crossed " + pbuf;
                    alert_toast_until_ = ImGui::GetTime() + 6.0;
                    alert_notify_beep(alert_toast_msg_.c_str(), 880);
                    if (paper_on_alerts_)
                        maybe_paper_on_alert(a.arm_above, px, "alert");
                    if (exec_bridge::settings().enabled)
                        exec_bridge::notify("alert", pair_.exchange.c_str(),
                            pair_.symbol.c_str(), a.arm_above ? "long" : "short",
                            px, alert_toast_msg_.c_str());
                    alerts_.erase(alerts_.begin() + static_cast<long>(i));
                } else {
                    ++i;
                }
            }
        }
        if (px > 0.0) alert_last_price_ = px;
    } else {
        const double px = candles().last_close_price();
        if (px > 0.0) alert_last_price_ = px;
    }

    // ── Phase 3: drawn-shape crossing alerts ─────────────────────────────
    // Every HLine/HRay acts as a horizontal level and every two-anchor line
    // (Trendline/Ray/ExtendedLine/Arrow) as a projected level at the current
    // time. Same edge + toast + beep machinery as the price alerts above;
    // the level survives the fire so a drawn line keeps guarding until it is
    // deleted or the toggle goes off.
    {
        double px = candles().last_close_price();
        if (rt_mode_) {
            const auto& tr = candles().realtime_trades().trades();
            if (!tr.empty()) px = tr.back().price;
            else if (rt_quote_.best_bid > 0.0 && rt_quote_.best_ask > 0.0)
                px = 0.5 * (rt_quote_.best_bid + rt_quote_.best_ask);
        }
        const int64_t now_ms = rt_mode_ ? rt_clock_ms_
            : std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        // Phase 5: mark-to-market feed for the local paper engine - this chart's
        // price path (RT tick / quote mid / candle close) drives every open
        // position on this symbol, including stop/TP/trail exits.
        if (px > 0.0 && ctx_.local_paper) {
            ctx_.local_paper->on_price(pair_.exchange.c_str(), pair_.symbol.c_str(),
                                       px, now_ms);
            ctx_.local_paper->tick(now_ms);   // 1s persistence debounce
        }
        if (drawing_alerts_enabled_ && px > 0.0 && drawing_alert_last_price_ > 0.0 &&
            !ctx_.drawing_mgr().hidden_all()) {
            const double before = drawing_alert_last_price_;
            for (const auto& d : ctx_.drawing_mgr().items()) {
                if (d.hidden || d.anchors.empty()) continue;
                double level = 0.0;
                const drawing::Tool t = d.tool;
                if (t == drawing::Tool::HLine || t == drawing::Tool::HRay)
                    level = d.anchors[0].price;
                else if ((t == drawing::Tool::Trendline || t == drawing::Tool::Ray ||
                          t == drawing::Tool::ExtendedLine || t == drawing::Tool::Arrow) &&
                    d.anchors.size() >= 2) {
                    // Projected price at the current time (extrapolates past
                    // the anchors, like a ray would continue).
                    const int64_t span = d.anchors[1].t_ms - d.anchors[0].t_ms;
                    const double f = span != 0
                        ? double(now_ms - d.anchors[0].t_ms) / double(span) : 0.0;
                    level = d.anchors[0].price + (d.anchors[1].price - d.anchors[0].price) * f;
                } else {
                    continue;
                }
                if (!(level > 0.0)) continue;
                const double before_gap = before - level, now_gap = px - level;
                if ((before_gap >= 0.0 && now_gap < 0.0) || (before_gap <= 0.0 && now_gap > 0.0)) {
                    char pbuf[32];
                    fmt_.format_price(pbuf, sizeof(pbuf), level);
                    std::string sym = pair_.symbol;
                    for (char& c : sym) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
                    const bool flat = t == drawing::Tool::HLine || t == drawing::Tool::HRay;
                    alert_toast_msg_ = sym + " crossed your ";
                    alert_toast_msg_ += flat ? "line" : "trendline";
                    alert_toast_msg_ += " at ";
                    alert_toast_msg_ += pbuf;
                    alert_toast_until_ = ImGui::GetTime() + 6.0;
                    alert_notify_beep(alert_toast_msg_.c_str(), 740);
                    if (paper_on_alerts_)
                        maybe_paper_on_alert(now_gap > 0.0, px, "drawing");
                    if (exec_bridge::settings().enabled)
                        exec_bridge::notify("alert", pair_.exchange.c_str(),
                            pair_.symbol.c_str(), now_gap > 0.0 ? "long" : "short",
                            px, alert_toast_msg_.c_str());
                }
            }
        }
        if (px > 0.0) drawing_alert_last_price_ = px;
    }

    update_volume_indicators();
    if (!candles().empty()) {

        // CVD: populate on first data availability, update building candle each frame
        auto* cvd_ind = indicator_mgr_.get_indicator_of_type<Indicators::CVDIndicator>();
        if (cvd_ind && cvd_ind->is_visible() && candles().is_initial_load_complete()) {
            // Repopulate when candle count changes (new candle finalized or after clear)
            const size_t current_count = candles().count();
            if (current_count > 0 && cvd_ind->bar_count() != current_count) {
                cvd_ind->clear();
                populate_cvd_data(cvd_ind);
            }
            update_cvd_indicator();
        }

        // RSI: populate on first data, repopulate when candle count changes
        auto* rsi_ind = indicator_mgr_.get_indicator_of_type<Indicators::RSIIndicator>();
        if (rsi_ind && rsi_ind->is_visible() && candles().is_initial_load_complete()) {
            const size_t current_count = candles().count();
            if (current_count > 0 && rsi_ind->bar_count() != current_count) {
                rsi_ind->clear();
                for (const auto& candle : candles().candles())
                    rsi_ind->add_candle(candle.timestamp_ms, candle.close);
            }
            if (candles().has_building_candle()) {
                const auto& bc = candles().building_candle();
                rsi_ind->set_building_candle(bc.timestamp_ms, bc.close);
            }
            rsi_ind->update();
        }

        // MACD: same pattern
        auto* macd_ind = indicator_mgr_.get_indicator_of_type<Indicators::MACDIndicator>();
        if (macd_ind && macd_ind->is_visible() && candles().is_initial_load_complete()) {
            const size_t current_count = candles().count();
            if (current_count > 0 && macd_ind->bar_count() != current_count) {
                macd_ind->clear();
                for (const auto& candle : candles().candles())
                    macd_ind->add_candle(candle.timestamp_ms, candle.close);
            }
            if (candles().has_building_candle()) {
                const auto& bc = candles().building_candle();
                macd_ind->set_building_candle(bc.timestamp_ms, bc.close);
            }
            macd_ind->update();
        }

        // Funding Rate: populate from stats stream cache
        auto* funding_ind = indicator_mgr_.get_indicator_of_type<Indicators::FundingRateIndicator>();
        if (funding_ind && funding_ind->is_visible() && candles().is_initial_load_complete()) {
            const size_t current_count = candles().count();
            const bool have_funding_data = !funding_cache_.empty();
            if (current_count > 0 && have_funding_data &&
                (funding_ind->bar_count() != current_count || funding_data_dirty_)) {
                funding_ind->clear();
                populate_funding_data(funding_ind);
                funding_data_dirty_ = false;
            }
            update_funding_indicator();
        }

        // OI: populate from stats stream cache, repopulate when candles change or history arrives
        auto* oi_ind = indicator_mgr_.get_indicator_of_type<Indicators::OIIndicator>();
        if (oi_ind && oi_ind->is_visible() && candles().is_initial_load_complete()) {
            const size_t current_count = candles().count();
            // Only populate when we have OI data to work with (avoid spamming empty populate
            // while waiting for the async historical OI batch to arrive)
            const bool have_oi_data = !oi_ohlc_cache_.empty() || !oi_cache_.empty();
            if (current_count > 0 && have_oi_data &&
                (oi_ind->bar_count() != current_count || oi_data_dirty_)) {
                oi_ind->clear();
                populate_oi_data(oi_ind);
                oi_data_dirty_ = false;
            }
            update_oi_indicator();
        }

    }
    // Keep the observation clock current even during empty seek/load windows.
    if (auto* ind = indicator_mgr_.get_indicator_of_type<Indicators::VPINIndicator>();
        ind && ind->is_visible()) update_vpin_indicator();
    // Heatmap - request once candles are loaded (WS guaranteed connected).
    // Skipped in Renko: the time-keyed overlays do not draw there, and
    // update_heatmap() reads last_visible_range_ as TIME (brick indices in Renko).
    if (!rt_mode_ && heatmap_enabled_ && ct_allows_time_overlays(chart_type_) &&
        candles().is_initial_load_complete()) {
        request_heatmap_data();  // First-time request, deferred from constructor

        update_heatmap();
        update_live_heatmap_from_orderbook();
    }
    // Liq Levels (rails) retired 2026-09-05: no subscribe, no historical request.
    const bool is_replay = ctx_.candle_mgr().replay_start_time_ms() > 0;
    // The raw census (stream 34) now feeds only the admins' observed touch-odds fuel on Hyperliquid charts; the
    // Hyperliquid positions layer draws the server's rails (update_hl_rails).
    if (observed_rails_allowed() && ct_allows_time_overlays(chart_type_) &&
        candles().is_initial_load_complete() &&
        !liq_census_subscribed_ && !is_replay) {
        const StreamKey key{liq_census_pair_, Terminal::Stream::LiquidationLevels, 0};
        ctx_.stream_mgr().send_subscribe(key);
        liq_census_subscribed_ = true;
    }
    if (observed_rails_allowed() && liq_history_requested_ == 0 &&
        ct_allows_time_overlays(chart_type_) && candles().is_initial_load_complete() &&
        !ctx_.replay_mgr().is_pack_mode()) {
        const int64_t end = is_replay ? ctx_.replay_mgr().interpolated_time_ms()
            : static_cast<int64_t>(emscripten_date_now());
        // This returns false without sending while the socket/token is not ready.
        // A successfully sent request is one-shot; the settings button owns retries.
        if (end > 0 && ctx_.stream_mgr().request_historical_liq_levels(liq_census_pair_, end - 6*3600000LL, end))
            liq_history_requested_ = 1;
    }
    indicator_mgr_.update_all();
}

// Timeframe Change
void ChartWidget::set_liq_opacity(float v) {
    liq_opacity_ = std::clamp(v, 0.0f, 1.0f);
    if (auto* recon = ctx_.liq_heatmap_mgr().get_timeline_reconstructor(pair_))
        recon->set_opacity(liq_opacity_);
}

void ChartWidget::request_chart_of_this_market()
{
    using PW = Menu::SymbolPickerState::PendingWidget;
    Menu::g_widget_add_request.type = PW::Charts;
    Menu::g_widget_add_request.pair = pair_;
    Menu::g_widget_add_request.fmt = fmt_;
    Menu::g_widget_add_request.tick_size = tick_size_;
    Menu::g_widget_add_request.pending = true;
}

void ChartWidget::change_timeframe(const int new_tf_seconds)
{
    if (rt_mode_) set_rt_mode(false);   // back to the pre-real-time chart type
    candles().change_timeframe(new_tf_seconds);
    // The previous viewport is expressed in the old timeframe's domain. Reusing
    // it for even one frame makes the new timeframe's Y fit sample the wrong
    // candle window and can leave volatile symbols vertically compressed.
    last_visible_range_ = {};
    x_axis_min_ = 0.0;
    x_axis_max_ = 0.0;
    stored_x_min_ = 0.0;
    stored_x_max_ = 0.0;
    // Update ReplayManager's timeframe so scrubber ticks, candle-boundary
    // snapping, and skip amounts use the correct timeframe. The replay clock
    // follows the app-level manager, which only the primary chart drives: a
    // secondary chart's timeframe is its own and must not re-time the replay.
    if (ctx_.replayer && !owned_candles_ && !compare_) {
        ctx_.replay_mgr().set_timeframe_ms(static_cast<int64_t>(new_tf_seconds) * 1000);
    }
    // Linked charts read one time axis, so they read one timeframe too.
    if (link_active() && !rt_mode_) {
        g_time_link.tf_seconds = new_tf_seconds;
        g_time_link.tf_frame = ImGui::GetFrameCount();
        g_time_link.tf_owner = link_id_;
        link_tf_seen_frame_ = g_time_link.tf_frame;
    }
    ctx_.heatmap_mgr().set_timeframe(pair_, heatmap_mode_, new_tf_seconds);
    heatmap_data_requested_ = false;
    heatmap_loaded_timeframe_ = 0;
    // Liq map is TIMEFRAME-INDEPENDENT. Snapshots are price-level bands keyed by
    // {exchange,symbol}+timestamp (raw_timeline_protos_) at a fixed ~5-min cadence -
    // NOT bucketed by candle TF. The chart TF only changes the x-axis mapping at
    // render; the GPU timeline reconstructs from the cached protos via the same path
    // set_leverage_mask() uses. So do NOT clear or re-request on TF change.

    // Clear and reset indicators for new timeframe
    auto* vol_ind = indicator_mgr_.get_indicator_of_type<Indicators::VolumeIndicator>();
    if (vol_ind) {
        vol_ind->clear();
        vol_ind->set_timeframe(new_tf_seconds);
    }
    auto* cvd_ind = indicator_mgr_.get_indicator_of_type<Indicators::CVDIndicator>();
    if (cvd_ind) {
        cvd_ind->clear();
        cvd_ind->set_timeframe(new_tf_seconds);
    }
    // Rebind timeframe-dependent callbacks through their original owner.
    cvd_wick_cache_.clear();
    funding_cache_.clear();
    unsubscribe_chart_streams();
    subscribe_chart_streams();
    // Clear and reset funding indicator for new timeframe
    funding_history_requested_ = false;
    funding_data_dirty_ = false;
    auto* funding_ind = indicator_mgr_.get_indicator_of_type<Indicators::FundingRateIndicator>();
    if (funding_ind) {
        funding_ind->clear();
        funding_ind->set_timeframe(new_tf_seconds);
        // Re-request historical funding for new timeframe
        funding_history_requested_ = true;
        const int count = static_cast<int>(candles().count());
        ctx_.stream_mgr().request_historical_funding(pair_, new_tf_seconds, count > 0 ? count : 2880);
    }
    // Clear and reset OI indicator for new timeframe
    oi_ohlc_cache_.clear();
    oi_cache_.clear();
    oi_history_requested_ = false;
    oi_data_dirty_ = false;
    auto* oi_ind = indicator_mgr_.get_indicator_of_type<Indicators::OIIndicator>();
    if (oi_ind) {
        oi_ind->clear();
        oi_ind->set_timeframe(new_tf_seconds);
        request_historical_oi(); // Re-request for new timeframe
    }
}

// Main Render
void ChartWidget::render() {
    cursor_reference_price_ = 0;
    rt_dom_frame_ = {}; // A hidden/collapsed plot must not publish a stale transform.
    if (!is_open) return;

    // Keep the tab's visible TF and mode in sync (catches every TF-change and
    // real-time path, not just the toolbar). Cheap compares; the string is only
    // rebuilt on an actual change. The docking identity (after "###") is unchanged.
    {
        const int64_t tf = candles().timeframe_seconds();
        if (tf != title_tf_seconds_ || rt_mode_ != title_rt_) {
            title_tf_seconds_ = tf;
            title_rt_ = rt_mode_;
            timeframe_label_  = timeframe_to_string(tf);
            rebuild_title();
        }
    }

    ImGuiWindowClass window_class;
    window_class.ClassId = ImHashStr("ChartWindow");
    window_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_CentralNode;

    if (hints.use_hints) {
        ImGui::SetNextWindowPos(ImVec2(hints.pos_x, hints.pos_y), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(hints.size_x, hints.size_y), ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowClass(&window_class);

    place_new_window();
    if (!ImGui::Begin(title_.c_str(), &is_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (chart_instance_ == 1 && !ctx_.stream_mgr().is_replay_mode() &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) Menu::sync_market_view(pair_,false);

    // Two ChartWidgets on one pair resolve to ONE ImGui window: the docking
    // identity is "###chart_<ex>_<sym>", so a collision means the pair matches
    // and the first submission of the frame loses nothing by owning it. Drawing
    // twice does not stack, it corrupts: the second BeginChild sees BeginCount
    // != 1, so it neither seeds the parent cursor from the child's position nor
    // reports the child's size back through ItemSize. That is what leaves the
    // toolbar painted at the window's bottom edge over a body with no height,
    // and what strands DC.IsSetPos long enough for End() to fire the
    // SetCursorPos/SetCursorScreenPos boundary error. Same BeginCount > 1 guard
    // ImGui uses for its own re-entrant debug windows.
    //
    // The duplicate itself is a widget-lifetime defect one layer up, in
    // whoever pushed the second widget. This guard keeps the frame honest; it
    // does not excuse creating the second widget, so it says so once.
    if (ImGui::GetCurrentWindow()->BeginCount > 1) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::printf("[chart] duplicate ChartWidget for %s: '%s' begun twice "
                        "in one frame, second submission skipped\n",
                        pair_.symbol.c_str(), title_.c_str());
        }
        ImGui::End();
        return;
    }

    render_controls();
    // Acquire one as-of clock after pause controls, before chart and DOM render.
    if (rt_mode_) update_realtime();
    // Fold new large prints out of the trade ring every frame, in every mode,
    // so leaving real-time for candles shows the prints that arrived meanwhile.
    collect_candle_prints();
    crosshair_state_ = CrosshairState();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

    // In-chart drawing rail (2026-08-06: moved off the viewport edge so it
    // sits directly against the chart). It reserves its width from the plot;
    // the body child keeps the indicator panes right of the rail too. Gated
    // in live and replay; recording keeps its clean capture surface.
    const bool show_rail = !ClipRecorder::focus_active() &&
                           !edu::RecorderRuntime::instance().active();
    if (show_rail) {
        drawing::render_chart_rail(ctx_.drawing_mgr(),
                                   ImGui::GetContentRegionAvail().y);
        ImGui::SameLine(0.0f, 0.0f);
    }
    // Real-time owns its depth ladder: a column right of the chart body, so it
    // is on screen exactly when the plot it aligns to is. Set before the plot,
    // whose price scale keeps rows readable for it.
    rt_dom_linked_ = rt_mode_ && rt_dom_shown_;
    float ladder_width = 0.0f;
    if (rt_dom_linked_) {
        ImGui::PushFont(Theme::Fonts::mono_sm());
        ladder_width = std::min(RealtimeDomLadder::preferred_width(),
                                ImGui::GetContentRegionAvail().x * 0.45f);
        ImGui::PopFont();
    }
    ImGui::BeginChild("##chart_body", ImVec2(ladder_width > 0.0f ? -ladder_width : 0.0f, 0), false,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);

    if (chart_type_ != ChartType::Renko) {
        const bool selected = replay_selection_.start_ms != replay_selection_.end_ms;
        if (selected) {
            if (pair_.exchange == "binancef") {
                const auto move = read_selected_move();
                const bool valid = move.snap.end_minute_ms > move.snap.start_minute_ms;
                ImGui::BeginDisabled(!valid);
                if (ImGui::SmallButton("Investigate selection"))
                    open_outcome_first_handoff(pair_, move.snap, move.range_minutes);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (valid) Theme::tooltip("Through %s. Empty space beyond available chart time is excluded.",
                        research_url::minute_label_utc(move.snap.end_minute_ms).c_str());
                    else Theme::tooltip("Select one minute to seven days of loaded candles.");
                }
            }
            // Stack at narrow widths rather than colliding with the hint.
            if (ImGui::GetContentRegionAvail().x > 490.0f) ImGui::SameLine(0, 12);
            if (ImGui::SmallButton("Clear selection")) replay_selection_ = {};
            ImGui::SameLine(0, 6);
            ImGui::TextDisabled("Esc");
        }
    }
    // The "scroll back to the newest bar" affordance. It only appears once the
    // user has panned away from the edge, and it says what it does. It used to
    // read "Follow live" / "Following live" at all times, which looked like a
    // second live mode next to the Real-time pill in the toolbar; the resting
    // state now draws nothing (the chart IS at the edge).
    if (chart_type_ != ChartType::TPO && !candles().follow_live()) {
        if (replay_selection_.start_ms != replay_selection_.end_ms &&
            ImGui::GetContentRegionAvail().x > 650.0f)
            ImGui::SameLine(0, 16);
        if (ImGui::SmallButton(ctx_.replay_mgr().is_active() ? "Jump to playhead" : "Jump to latest")) {
            candles().set_follow_live(true);
            if (rt_mode_) {
                rt_auto_price_ = true;
                rt_auto_fit_ = {}; rt_price_window_ = {};
            }
        }
    }
    if (rt_mode_ && !rt_auto_price_) {
        if (!candles().follow_live()) ImGui::SameLine(0, 12);
        if (ImGui::SmallButton("Follow price")) {
            rt_auto_price_ = true;
            rt_price_window_ = {};
            rt_auto_fit_ = {};
        }
        if (!rt_auto_price_) {
            ImGui::SameLine(0, 6);
            ImGui::TextDisabled("Price following off");
        }
    }
    const float total_height = ImGui::GetContentRegionAvail().y;
    // Indicator pane (tabbed, design .indi-pane): 0 / header-only / INDI_PANE_H.
    // Real-time and Renko skip the pane (render_indicators early-returns), so
    // reserve no height for it - the chart takes the full area.
    const float indicator_total_height =
        (chart_type_ == ChartType::FlowPositioning) ? std::min(flow_history_.ready && flow_history_.assessment.raw_count > 0 ? 520.0f : 430.0f, total_height * 0.65f) :
        subplots_suppressed() ? 0.0f : indicator_mgr_.pane_height();
    const float chart_height = std::max(100.0f, total_height - indicator_total_height - activity_height() - realtime_cvd_lane_height() - (liquidation_activity_visible() ? ((rt_mode_ ? rt_liq_collapsed_ : candle_liq_collapsed_) ? 28.0f : std::min(rt_mode_ ? rt_liq_height_ : candle_liq_height_, std::max(60.0f, total_height - indicator_total_height - 180.0f)) + (ImGui::GetContentRegionAvail().x >= 620 ? 38.0f : 58.0f)) : 0.0f));

    chart_allocated_height_ = chart_height;
    indicator_allocated_height_ = indicator_total_height;
    crosshair_state_.is_active = false;

    // SPEC §0.2: the chart + every stacked pane share ONE right-gutter
    // column - ImPlot equalizes the y-axis widths across the group, so all
    // plots have identical x-extents. This is also what makes the crosshair
    // math exact: render_crosshair maps time→pixels with a single transform
    // spanning chart-left → last-pane-right, which is only valid when the
    // extents match (mixed per-pane axis label widths caused the 2026-07-04
    // "mouse vs time-pointer horizontal gap" once panes stacked by default).
    const bool plots_aligned = ImPlot::BeginAlignedPlots("##chart_pane_align");
    const ImPlotInputMap chart_input_map = ImPlot::GetInputMap();
    render_chart();
    // Restore the ImPlot input map before the indicator subplots BeginPlot -
    // they must never see the drawing layer's overrides. Idempotent.
    drawing_layer_.end_frame();
    ImPlot::GetInputMap() = chart_input_map;
    render_realtime_cvd_lane();
    render_activity_strip();
    if (liquidation_activity_visible()) render_realtime_liquidation_strip();
    {
        ProfileScope _ps("Indics");
        if (chart_type_ == ChartType::FlowPositioning) render_flow_positioning();
        else render_indicators();
    }
    if (plots_aligned) ImPlot::EndAlignedPlots();

    if (crosshair_state_.is_active) {
        render_crosshair(crosshair_state_.plot_pos);
    }

    ImGui::EndChild();  // ##chart_body
    ImGui::PopStyleVar();
    if (ladder_width > 0.0f) {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushFont(Theme::Fonts::mono_sm());
        ImGui::BeginChild("##rt_dom_ladder", ImVec2(0, 0), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        rt_dom_ladder_.render(rt_dom_frame_, fmt_, &rt_dom_shown_);
        ImGui::EndChild();
        ImGui::PopFont();
    }

    ImGui::End();
}


// Chart Rendering - reads from CandleManager
// The chart's empty and loading states: centred in the chart's own space (the panes below keep their place), a
// quiet title, one line of detail, and the shared spinner while something is on its way.
void ChartWidget::render_chart_empty(const char* title, const char* detail, bool busy) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float h = std::max(120.0f, chart_allocated_height_);
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(std::round(p.x + w * 0.5f), std::round(p.y + h * 0.5f));
    float y = c.y - (detail ? 18.0f : 9.0f);
    if (busy) {
        Theme::spinner(dl, ImVec2(c.x, y - 30.0f), 14.0f);
        y += 6.0f;
    }
    Theme::push_font(Theme::Fonts::ui(), ImGui::GetFontSize());
    const ImVec2 ts = ImGui::CalcTextSize(title);
    dl->AddText(ImVec2(std::round(c.x - ts.x * 0.5f), y), Theme::u32(Theme::Tokens::TX2), title);
    if (detail) {
        const ImVec2 ds = ImGui::CalcTextSize(detail);
        dl->AddText(ImVec2(std::round(c.x - ds.x * 0.5f), y + ts.y + 6.0f), Theme::u32(Theme::Tokens::TX3), detail);
    }
    ImGui::PopFont();
}

void ChartWidget::render_chart() {
    Theme::chart_chips_reset();  // this chart's chips, read by render_crosshair after the panes
    // Renko abandons the time X-axis for a brick-index domain; it is a fully
    // self-contained render path (own BeginPlot/EndPlot) so the time-based path
    // below is untouched. See render_chart_renko.
    if (chart_type_ == ChartType::Renko) { render_chart_renko(); return; }

    const auto& registry = SymbolRegistry::instance();
    const bool unlisted = registry.is_loaded() && !registry.has(pair_.exchange, pair_.symbol);
    char unlisted_title[96];
    snprintf(unlisted_title, sizeof(unlisted_title), "%s is not listed on %s", widget_symbol_label(pair_.symbol).c_str(),
             widget_venue_label(pair_.exchange));
    if (rt_mode_ && !rt_history_view_ && !rt_latest_ && realtime_trades().empty() && rt_samples_.empty()) {
        if (unlisted) render_chart_empty(unlisted_title, "Choose another market from the pair menu.", false);
        else render_chart_empty("Waiting for depth and trades", "Real-time starts with a synchronized order book.", true);
        return;
    }
    const auto& timestamps = candles().timestamps();
    if (!rt_mode_ && timestamps.empty() && !candles().has_building_candle()) {
        if (unlisted) render_chart_empty(unlisted_title, "Choose another market from the pair menu.", false);
        else if (candles().is_loading()) render_chart_empty("Loading candles", nullptr, true);
        else if (ctx_.replay_mgr().is_active()) render_chart_empty("No candles at the playhead yet", "They build as the replay plays.", false);
        else render_chart_empty("No candles yet", "Waiting for this market's first trade.", true);
        return;
    }
    const int64_t tf_sec = candles().timeframe_seconds();
    // Calculate X range. When no finalized candles exist yet (fresh/volatile
    // symbol still loading history, or replay before the first drip) the old
    // code anchored x_min at 0.0, so the axis spanned 1970 to now with a single
    // dot at the live edge: the "no history / broken" look on RT load. Anchor the
    // window to the live edge instead. Reached identically by live and replay,
    // which both arrive here with only a building candle / ticks at first.
    double x_min, x_max;
    if (!timestamps.empty()) {
        x_min = timestamps.front();
        x_max = timestamps.back();
    } else {
        int64_t anchor_ms;
        if (candles().has_building_candle()) {
            anchor_ms = candles().building_candle().timestamp_ms;
        } else if (!candles().tick_times().empty()) {
            anchor_ms = candles().tick_times().back();
        } else if (ctx_.replay_mgr().is_active()) {
            anchor_ms = ctx_.replay_mgr().interpolated_time_ms();
        } else {
            anchor_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }
        const int64_t span_ms =
            static_cast<int64_t>(candles().visible_candles_for_timeframe()) *
            tf_sec * 1000LL;
        x_max = static_cast<double>(anchor_ms);
        x_min = static_cast<double>(anchor_ms - span_ms);
    }
    if (candles().has_building_candle()) {
        x_max = std::max(x_max, static_cast<double>(candles().building_candle().timestamp_ms));
    }
    double x_padding = static_cast<double>(tf_sec) * 1000.0 * 3.0;
    // The rails' profile sits at the right edge (about a tenth of the plot) with the largest rails' dollar labels
    // beside it: keep the candles clear of both (the leading layer's profile when the V2 and Hyperliquid rails both
    // draw). The touch-odds forward columns step aside while the V2 rails draw.
    if (hl_rails_drawn() && hl_leads() && hl_profile_) {
        x_padding = std::max(x_padding, 0.16 * static_cast<double>(candles().visible_candles_for_timeframe()) *
                                            static_cast<double>(tf_sec) * 1000.0);
    } else if (liqmap2_rails_drawn()) {
        if (liqmap2_rails_profile_ && !hl_leads())
            x_padding = std::max(x_padding, 0.16 * static_cast<double>(candles().visible_candles_for_timeframe()) *
                                                static_cast<double>(tf_sec) * 1000.0);
    } else if (touch_odds_enabled_ && !rt_mode_ && !ctx_.replay_mgr().is_active()) {
        // Touch odds (admin preview) are drawn in the future: reserve their window,
        // at least a readable column and at most 15% of the default view.
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (const int64_t until = touch_odds_overlay::window_end(pair_, now_ms)) {
            const double span = static_cast<double>(candles().visible_candles_for_timeframe()) *
                                static_cast<double>(tf_sec) * 1000.0;
            const double need = std::max(static_cast<double>(until) - x_max, 0.07 * span);
            const double cap = touch_odds_overlay::owns_forward_area(pair_, now_ms, false) ? 0.22 : 0.15;  // rails want room
            x_padding = std::max(x_padding, std::min(need, cap * span));
        }
    }
    x_max += x_padding;

    // During replay, clamp x_max so heatmaps/overlays don't render past the
    // current playback position. Allows a small padding (1 candle) for the
    // building candle, but prevents data from leaking into "future" chart area.
    if (ctx_.replay_mgr().is_active()) {
        const double replay_time = static_cast<double>(ctx_.replay_mgr().interpolated_time_ms());
        const double replay_max = replay_time + static_cast<double>(tf_sec) * 1000.0 * 2.0;
        x_max = std::min(x_max, replay_max);
    }
    // Scrub preview (ghost candles): while the user aims a scrub on the
    // transport, extend x_max PAST the playhead clamp to the aimed-at time
    // plus a small lead, bounded by the replay window end. The view grows as
    // the user sweeps right and snaps back to the clamp when the preview
    // disarms - the clamp above stays authoritative whenever no preview is up.
    const int64_t scrub_preview_ms = ctx_.replay_mgr().scrub_preview_ms();
    if (scrub_preview_ms > 0) {
        const double preview_lead = static_cast<double>(tf_sec) * 1000.0 * 6.0;
        const double window_end = static_cast<double>(ctx_.replay_mgr().info().end_time_ms)
                                  + x_padding;
        const double preview_max = std::min(
            static_cast<double>(scrub_preview_ms) + preview_lead, window_end);
        x_max = std::max(x_max, preview_max);
    }

    if (candles().follow_live()) {
        const size_t visible_candles = candles().visible_candles_for_timeframe();
        if (candles().count() > visible_candles) {
            const auto& series = candles().candles();
            x_min = static_cast<double>(series[series.size() - visible_candles].timestamp_ms);
        }
    }

    // CLIP_FACTORY: script-pinned visible span. The post-seek batch fit spans
    // however much history the seed happened to serve - a produced clip must
    // not inherit that. Pin the X window to the last viewSpanMinutes behind
    // the live edge - but ONLY from shot 0 onward (in_shot): pinning during
    // boot/seek/prime races the initial candle batch (malformed building
    // candle + request thrash), so those phases keep default behavior.
    if (edu::RecorderRuntime::instance().in_shot()) {
        if (const auto* rv = edu::RecorderRuntime::instance().view();
            rv && rv->view_span_min > 0) {
            x_min = x_max - static_cast<double>(rv->view_span_min) * 60000.0;
        }
    }
    // Last-resort guard on a degenerate X range. Every branch above assumes
    // x_min < x_max, and a feed that lands a bogus candle at the back of the
    // series breaks that: an all-zero candle at timestamp 0 takes x_max to 0
    // against a real epoch x_min. ImPlot then draws an empty plot on an
    // inverted axis, and nothing clears it except a timeframe change, so the
    // terminal looks broken for as long as the user leaves it alone. The candle
    // manager now rejects that candle at the door; this is the backstop that
    // keeps ANY such feed from blanking the chart permanently. Written as
    // !(x_max > x_min) so a NaN from the same class of bug is caught too.
    if (!(x_max > x_min)) {
        const int64_t anchor_ms = ctx_.replay_mgr().is_active()
            ? ctx_.replay_mgr().interpolated_time_ms()
            : std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
        const double span_ms =
            static_cast<double>(candles().visible_candles_for_timeframe()) *
            static_cast<double>(tf_sec) * 1000.0;
        x_max = static_cast<double>(anchor_ms) + x_padding;
        x_min = x_max - span_ms;
    }

    // Real-time view (RT), any chart type: engage follow-live so the chart
    // streams and keeps the live edge current. We re-arm follow-live ONLY on the
    // rising edge of rt_mode_ (rt_was_on_ edge-detect) - not every frame - so the
    // normal follow_live() X/Y path (above / below) takes over. Wheel zoom keeps
    // following; deliberate pan clears the latch in handle_plot_interaction.
    if (rt_mode_ && !rt_was_on_) {
        candles().set_follow_live(true);
    }
    rt_was_on_ = rt_mode_;
    if (rt_mode_) {
        // The requested viewport is independent of collected coverage. Leave
        // pre-join space empty rather than overriding zoom on a quiet market.
        const double span = rt_span_ms_;
        x_max = realtime_projection_until(rt_clock_ms_, span);
        x_min = x_max - span;
    }
    // Drawing tools: the ImPlot input-map override must be in place BEFORE
    // BeginPlot (ImPlot reads the map at setup-lock, i.e. the first plot
    // item - not at EndPlot). Restored by end_frame() after render_chart.
    drawing_layer_.begin_frame(ctx_, ct_allows_time_overlays(chart_type_));
    // Shift-left belongs to the time-range selector, including the first
    // frame and a release outside the plot. SetupFinish consumes input before
    // handle_plot_interaction can claim it. Keep drawing overrides intact.
    if (ImGui::GetIO().KeyShift || replay_selection_.active)
        ImPlot::GetInputMap().PanMod = ImGuiMod_Ctrl | ImGuiMod_Shift |
                                       ImGuiMod_Alt | ImGuiMod_Super;
    // TradingView-style axis scaling: while an axis drag is latched, ImPlot's
    // own pan must stand down or it fights the stretch. The map is read at
    // setup-lock (here), so the latch the previous frame set applies now.
    if (rt_axis_scale_axis_ >= 0 && !rt_axis_scale_map_override_) {
        rt_axis_scale_saved_map_ = ImPlot::GetInputMap();
        rt_axis_scale_map_override_ = true;
        ImPlotInputMap& axis_map = ImPlot::GetInputMap();
        axis_map.PanMod = ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt | ImGuiMod_Super;
        axis_map.SelectMod = axis_map.PanMod;
    } else if (rt_axis_scale_axis_ < 0 && rt_axis_scale_map_override_) {
        ImPlot::GetInputMap() = rt_axis_scale_saved_map_;
        rt_axis_scale_map_override_ = false;
    }
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(10, 10));
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotBorderSize, 1.0f);
    if (ImPlot::BeginPlot("##Price", ImVec2(-1, chart_allocated_height_),
                           ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
        // Hide x-axis labels on main chart when the indicator pane shows a
        // plot below (the pane owns the time axis then)
        const bool has_subplots = chart_type_ == ChartType::FlowPositioning ||
            (!subplots_suppressed() && indicator_mgr_.pane_expanded()) ||
            (activity_visible() && !activity_collapsed_) ||
            (liquidation_activity_visible() && !(rt_mode_ ? rt_liq_collapsed_ : candle_liq_collapsed_)) ||
            realtime_cvd_lane_height() > 0;
        // TPO mode always shows date labels on main chart x-axis
        const bool hide_x_labels = has_subplots && chart_type_ != ChartType::TPO;
        // No tick marks either: the pane below is flush against this plot now
        // (its controls ride inside its top plot), so the marks only added a
        // band of dead space at the seam.
        ImPlotAxisFlags x_flags = hide_x_labels
            ? (ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoTickMarks) : ImPlotAxisFlags_None;
        ImPlot::SetupAxis(ImAxis_X1, nullptr, x_flags);
        // Plot-area wheel gestures already own RT time zoom. Price zoom is
        // explicit on the price axis, so time navigation cannot change rows.
        // Use the preceding hit rectangle before setup consumes this input.
        const bool time_wheel = rt_mode_ && rt_dom_linked_ && ImGui::GetIO().MouseWheel != 0 &&
            !ImPlot::GetCurrentPlot()->Axes[ImAxis_Y1].HoverRect.Contains(ImGui::GetIO().MousePos);
        if (rt_mode_ && rt_dom_linked_ && rt_auto_fit_history_ &&
            ImGui::GetIO().MouseWheel != 0 && !time_wheel)
            rt_auto_price_ = false;
        const ImVec2 price_drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
        const bool deliberate_price_drag = !ImGui::GetIO().KeyShift &&
            realtime_price_pan_detaches(price_drag.x, price_drag.y);
        // A time pan may contain vertical jitter. Do not move the linked price
        // axis until the complete gesture deliberately requests a price pan.
        const bool keep_price_on_drag = rt_mode_ && rt_dom_linked_ && rt_auto_price_ &&
            ImGui::IsMouseDown(ImGuiMouseButton_Left) && !deliberate_price_drag;
        ImPlot::SetupAxis(ImAxis_Y1, nullptr, ImPlotAxisFlags_Opposite |
            ((rt_mode_ && rt_auto_price_ && !rt_dom_linked_) || time_wheel || keep_price_on_drag
                ? ImPlotAxisFlags_Lock : ImPlotAxisFlags_None));
        ImPlot::SetupAxisFormat(ImAxis_Y1, fmt_.price_fmt);

        // Beyond this density candles and liquidation cells collapse below a
        // useful pixel width. ImPlot applies this constraint during wheel zoom,
        // while the explicit clamp below also repairs an already-oversized view.
        constexpr double kMinVisibleCandles = 8.0;
        constexpr double kMaxVisibleCandles = 1440.0;
        const double timeframe_ms = chart_type_ == ChartType::FlowPositioning ? 60000.0 : static_cast<double>(tf_sec) * 1000.0;
        if (chart_type_ != ChartType::TPO) {
            ImPlot::SetupAxisZoomConstraints(
                ImAxis_X1,
                rt_mode_ ? 5000.0 : kMinVisibleCandles * timeframe_ms,
                rt_mode_ ? (double(RealtimeArchive::target_ms) / 0.88) : kMaxVisibleCandles * timeframe_ms);
        }

        if (rt_mode_) ImPlot::SetupAxisLimitsConstraints(ImAxis_X1,
            -std::numeric_limits<double>::max(), realtime_projection_until(rt_clock_ms_, rt_span_ms_));

        // TPO mode: override x-axis formatter to show dates only (no scientific notation)
        if (chart_type_ == ChartType::TPO) {
            ImPlot::SetupAxisFormat(ImAxis_X1, [](double value, char* buff, int size, void*) -> int {
                if (!DisplayTimeZone::instance().format(static_cast<int64_t>(value),
                        TimeZoneFormat::MonthDay, buff, static_cast<size_t>(size))) {
                    return snprintf(buff, size, "--/--");
                }
                return static_cast<int>(std::strlen(buff));
            }, nullptr);
        }

        // Keep the viewport between 8 and 1,440 candles. When it is already
        // outside those bounds, force the repaired range for this frame.
        const double min_x_span = rt_mode_ ? 5000.0 : kMinVisibleCandles * timeframe_ms;
        const double max_x_span = rt_mode_ ? (double(RealtimeArchive::target_ms) / 0.88) : kMaxVisibleCandles * timeframe_ms;
        bool zoom_clamped = false;
        if (!candles().follow_live() && chart_type_ != ChartType::TPO) {
            const double span = last_visible_range_.X.Max - last_visible_range_.X.Min;
            if (span > 0.0 && (span < min_x_span || span > max_x_span)) {
                const double clamped_span = std::clamp(span, min_x_span, max_x_span);
                const double center = (last_visible_range_.X.Min + last_visible_range_.X.Max) * 0.5;
                last_visible_range_.X.Min = center - clamped_span * 0.5;
                last_visible_range_.X.Max = center + clamped_span * 0.5;
                zoom_clamped = true;
            }
        }

        // TPO zoom-out: apply for multiple frames to ensure ImPlot processes
        if (tpo_zoom_pending_ && chart_type_ == ChartType::TPO && !timestamps.empty()) {
            double latest = timestamps.back();
            double span_ms = 5.0 * 24.0 * 3600.0 * 1000.0;
            double tpo_x_min = latest - span_ms;
            double tpo_x_max = latest + span_ms * 0.05;
            ImPlot::SetupAxisLimits(ImAxis_X1, tpo_x_min, tpo_x_max, ImPlotCond_Always);
            last_visible_range_.X.Min = tpo_x_min;
            last_visible_range_.X.Max = tpo_x_max;
            if (--tpo_zoom_frames_ <= 0) {
                tpo_zoom_pending_ = false;
            }
        } else if (chart_type_ == ChartType::TPO) {
            // TPO mode: don't follow live - let user control zoom freely
            ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max, ImPlotCond_Once);
        } else {
            // A linked follower takes the group's axis verbatim (see
            // time_link_apply); otherwise the chart's own follow/zoom rules.
            double lx_min = zoom_clamped ? last_visible_range_.X.Min : x_min;
            double lx_max = zoom_clamped ? last_visible_range_.X.Max : x_max;
            bool force_always = candles().follow_live() || zoom_clamped;
            time_link_apply(lx_min, lx_max, force_always);
            ImPlot::SetupAxisLimits(ImAxis_X1, lx_min, lx_max,
                                    force_always ? ImPlotCond_Always : ImPlotCond_Once);
        }
        // Y-axis from VISIBLE candles only.
        // timestamps are sorted ascending → binary-search the visible index
        // range instead of scanning ALL loaded candles (up to 200k after
        // scroll-back) every frame. FPS item, 2026-07-05.
        const bool use_follow_range = candles().follow_live() &&
                                      chart_type_ != ChartType::TPO;
        const double visible_x_min = use_follow_range || last_visible_range_.X.Min <= 0
            ? x_min : last_visible_range_.X.Min;
        const double visible_x_max = use_follow_range || last_visible_range_.X.Max <= 0
            ? x_max : last_visible_range_.X.Max;
        double y_min = std::numeric_limits<double>::infinity();
        double y_max = -std::numeric_limits<double>::infinity();
        const auto& lows = candles().lows();
        const auto& highs = candles().highs();
        {
            ProfileScope _ps("YLimits");
            const auto lo_it = std::lower_bound(timestamps.begin(), timestamps.end(), visible_x_min);
            const auto hi_it = std::upper_bound(lo_it, timestamps.end(), visible_x_max);
            const size_t i0 = static_cast<size_t>(lo_it - timestamps.begin());
            const size_t i1 = static_cast<size_t>(hi_it - timestamps.begin());
            for (size_t i = i0; i < i1; i++) {
                y_min = std::min(y_min, lows[i]);
                y_max = std::max(y_max, highs[i]);
            }
        }
        // Include building candle if visible
        if (candles().has_building_candle()) {
            const auto& bc = candles().building_candle();
            if (bc.timestamp_ms >= visible_x_min && bc.timestamp_ms <= visible_x_max) {
                y_min = std::min(y_min, bc.low);
                y_max = std::max(y_max, bc.high);
            }
        }
        // Heikin Ashi: frame the DRAWN HA candles, not the real ones - HA bodies
        // sit inside the real range, so real extents would leave dead space (or,
        // for the building HA candle, clip). Recompute purely from HA extents;
        // if HA has no data in range this leaves y at infinity and the fallback
        // below covers it. Real-candle modes are untouched.
        if (chart_type_ == ChartType::HeikinAshi) {
            y_min = std::numeric_limits<double>::infinity();
            y_max = -std::numeric_limits<double>::infinity();
            ha_visible_extents(visible_x_min, visible_x_max, y_min, y_max);
        }
        // Scrub preview: the ghost candles ahead of the playhead must join the
        // y-fit, or the very move being previewed clips off-screen. Scans the
        // preview store over [playhead, right edge]; right edge is this frame's
        // extended x_max under follow-live, the user's own range otherwise.
        if (scrub_preview_ms > 0) {
            if (const auto* pv = ctx_.replay_mgr().preview_candles();
                pv && pv->ready()) {
                const double ghost_from =
                    static_cast<double>(ctx_.replay_mgr().interpolated_time_ms());
                const double ghost_to =
                    candles().follow_live() ? x_max : visible_x_max;
                double plo = 0.0, phi = 0.0;
                if (ghost_to > ghost_from &&
                    pv->minmax_in_range(ghost_from, ghost_to, plo, phi)) {
                    y_min = std::min(y_min, plo);
                    y_max = std::max(y_max, phi);
                }
            }
        }
        bool rt_empty_prices = false;
        if (rt_mode_) {
            y_min = std::numeric_limits<double>::infinity();
            y_max = -std::numeric_limits<double>::infinity();
            for (const auto& trade : realtime_trades()) {
                if (trade.timestamp_ms < visible_x_min) continue;
                if (trade.timestamp_ms > visible_x_max || trade.timestamp_ms > rt_clock_ms_) break;
                y_min = std::min(y_min, trade.price);
                y_max = std::max(y_max, trade.price);
            }
            // Published bubbles outlive the raw trade ring and can contain
            // extremes absent from a grouped archive query. Fit what we draw.
            if (rt_bubbles_ && rt_archive_) rt_archive_->bubbles().for_each(
                int64_t(visible_x_min), int64_t(visible_x_max), rt_clock_ms_,
                [&](const RealtimeBubbleHistory::Bubble& bubble) {
                    y_min = std::min(y_min, bubble.price);
                    y_max = std::max(y_max, bubble.price);
                });
            for (size_t i = 0; i < realtime_samples().size(); ++i) {
                const auto& sample = realtime_samples()[i];
                if (sample->timestamp_ms > rt_clock_ms_ || sample->timestamp_ms > visible_x_max) break;
                const int64_t until = i + 1 < realtime_samples().size() && !realtime_samples()[i + 1]->segment_start
                    ? realtime_samples()[i + 1]->timestamp_ms : sample->timestamp_ms;
                if (until < visible_x_min) continue;
                y_min = std::min(y_min, sample->bid);
                y_max = std::max(y_max, sample->ask);
            }
            if (rt_book_valid_ && rt_latest_ && rt_latest_->timestamp_ms <= rt_clock_ms_ &&
                rt_clock_ms_ - rt_latest_->timestamp_ms <= 15000 && visible_x_max >= rt_clock_ms_) {
                y_min = std::min(y_min, rt_latest_->bid);
                y_max = std::max(y_max, rt_latest_->ask);
            }
            if (rt_quote_.timestamp_ms > 0 && visible_x_min <= rt_clock_ms_ && visible_x_max >= rt_clock_ms_) {
                y_min = std::min(y_min, rt_quote_.best_bid);
                y_max = std::max(y_max, rt_quote_.best_ask);
            }
            // Empty navigation keeps the last RT price frame; historical
            // candle extremes are not observations in this view.
            if (!std::isfinite(y_min) && last_visible_range_.Y.Min > 0 && rt_was_on_) {
                rt_empty_prices = true;
                y_min = last_visible_range_.Y.Min;
                y_max = last_visible_range_.Y.Max;
            }
        }
        const double rt_observed_low = y_min, rt_observed_high = y_max;
        // Fallback
        if (y_min == std::numeric_limits<double>::infinity()) {
            const size_t candle_count = std::min({timestamps.size(), lows.size(), highs.size()});
            if (candle_count > 0) {
                // A scroll can briefly outrun the history request. Fit a small
                // neighbourhood around the nearest loaded candle instead of the
                // entire store, which made one old spike flatten the current view.
                const double center = (visible_x_min + visible_x_max) * 0.5;
                const auto nearest = std::lower_bound(
                    timestamps.begin(), timestamps.begin() + candle_count, center);
                size_t pivot = static_cast<size_t>(nearest - timestamps.begin());
                if (pivot == candle_count) pivot = candle_count - 1;
                constexpr size_t kFallbackRadius = 8;
                const size_t i0 = pivot > kFallbackRadius ? pivot - kFallbackRadius : 0;
                const size_t i1 = std::min(candle_count, pivot + kFallbackRadius + 1);
                for (size_t i = i0; i < i1; ++i) {
                    y_min = std::min(y_min, lows[i]);
                    y_max = std::max(y_max, highs[i]);
                }
            } else {
                // No candles at all yet: anchor a thin band around the best known
                // price so a fresh/volatile symbol never shows a degenerate 0..1
                // axis on RT load. Building close, else last close, else newest tick.
                double ref = candles().last_close_price();
                if (ref <= 0.0 && candles().has_building_candle())
                    ref = candles().building_candle().close;
                if (ref <= 0.0 && !candles().tick_prices().empty())
                    ref = candles().tick_prices().back();
                if (ref > 0.0) { y_min = ref * 0.995; y_max = ref * 1.005; }
                else { y_min = 0; y_max = 1; }
            }
        }
        if (rt_mode_ && std::isfinite(y_min) && std::isfinite(y_max)) {
            const double center = (y_min + y_max) * 0.5;
            const double half = realtime_price_half_span(y_min, y_max, tick_size_, rt_bucket_multiplier_);
            y_min = center - half;
            y_max = center + half;
        }
#ifdef EDGEDEPTH_EXPOSURE_V2_DEV
        // Fit only currently accepted display data, and only when the user asked
        // for it: the scenario bands sit about 7% from price, so chasing them by
        // default squashes the candles into a line. Exact and average histories
        // remain separate. Summary bin edges use the unchanged source grid.
        if (exposure_enabled_ && exposure_fit_bands_ && !rt_mode_ && chart_type_==ChartType::Candles) {
            if (exposure_averages_) {
                if (const auto* summary=exposure_summary_.view(exposure_clock_)) for (const auto& column:summary->columns) {
                    if (column.end<visible_x_min || column.start>visible_x_max) continue;
                    for (const auto& cell:column.cells) if (exposure::shown(cell.band,exposure_display_)) {
                        y_min=std::min(y_min,cell.band.low);y_max=std::max(y_max,cell.band.high);
                    }
                }
            } else if (const auto* timeline=exposure_history_.view())
                exposure::rectangles(*timeline,exposure_clock_,[&](auto&,const exposure::Band& band,int64_t begin,int64_t end) {
                    if (end<visible_x_min || begin>visible_x_max || !exposure::shown(band,exposure_display_)) return;
                    y_min=std::min(y_min,band.low);y_max=std::max(y_max,band.high);
                });
        }
#endif
        const double y_span = y_max - y_min;
        const double y_padding = y_span > 0.0
            ? y_span * 0.08
            : std::max(std::abs(y_min) * 0.001, tick_size_ * 4.0);
        if (!rt_mode_ || (rt_auto_price_ && !rt_dom_linked_)) ImPlot::SetupAxisLimits(ImAxis_Y1,
            rt_empty_prices ? last_visible_range_.Y.Min : y_min - y_padding,
            rt_empty_prices ? last_visible_range_.Y.Max : y_max + y_padding, ImPlotCond_Always);
        if (!hide_x_labels) {
            // In TPO mode, always use last_visible_range_ for tick generation
            // to avoid stale visible_x_min/max on initial frames
            if (chart_type_ == ChartType::TPO && last_visible_range_.X.Min > 0) {
                setup_time_axis_ticks(last_visible_range_.X.Min, last_visible_range_.X.Max);
            } else {
                setup_time_axis_ticks(visible_x_min, visible_x_max);
            }
        }
        if (rt_mode_ && rt_dom_linked_ && tick_size_ > 0) {
            // The plot has one untitled X axis and explicit time ticks. Match
            // ImPlot's padding calculation before SetupFinish draws the axes,
            // including multiline labels and the current frame's resize.
            auto& plot = *ImPlot::GetCurrentPlot();
            const auto& style = ImPlot::GetStyle();
            const auto& x_axis = plot.Axes[ImAxis_X1];
            const double axis_height = x_axis.HasTickLabels()
                ? std::max(ImGui::GetTextLineHeight(), x_axis.Ticker.MaxSize.y) + style.LabelPadding.y : 0;
            const double height = std::max(1.0, double(plot.FrameRect.GetHeight()) -
                2 * style.PlotPadding.y - axis_height);
            const double native_tick = rt_renderer_ ? rt_renderer_->get_native_bucket_size() : tick_size_;
            if (!rt_auto_fit_history_) rt_effective_multiplier_ = rt_bucket_multiplier_;
            if (rt_auto_fit_history_ && rt_auto_price_ && !rt_empty_prices) {
                rt_auto_fit_.update(rt_observed_low, rt_observed_high, native_tick,
                    rt_bucket_multiplier_, height, ImGui::GetFontSize() + 4.0, rt_clock_ms_);
                if (rt_auto_fit_.grouping > 0) rt_effective_multiplier_ = rt_auto_fit_.grouping;
            }
            const double step = native_tick * rt_effective_multiplier_;
            if (rt_auto_price_) rt_price_free_ = false;
            if (step > 0 && !rt_price_free_) {
                const double row_height = ImGui::GetFontSize() + 4.0;
                const auto& current = plot.Axes[ImAxis_Y1].Range;
                double focus = std::numeric_limits<double>::quiet_NaN();
                if (rt_book_valid_ && rt_latest_ && rt_latest_->timestamp_ms <= rt_clock_ms_ &&
                    rt_clock_ms_ - rt_latest_->timestamp_ms <= 15000)
                    focus = (rt_latest_->bid + rt_latest_->ask) * 0.5;
                if (rt_quote_.timestamp_ms > 0) focus = (rt_quote_.best_bid + rt_quote_.best_ask) * 0.5;
                const bool fit_history = rt_auto_fit_history_ && rt_auto_price_ && rt_auto_fit_.grouping > 0;
                const auto range = fit_history ? rt_auto_fit_.range : rt_price_window_.update(
                    current.Min, current.Max, step, height, row_height, focus,
                    rt_auto_price_ && !rt_paused_ && !ctx_.replay_mgr().is_paused());
                if (fit_history) { rt_price_window_.bucket = step; rt_price_window_.height = height; }
                // SetRange preserves ImPlot input; SetupAxisLimits(Always)
                // would lock wheel zoom and dragging for the whole frame.
                ImPlot::SetupAxisZoomConstraints(ImAxis_Y1, step,
                    std::max(1.0, std::floor(height / row_height)) * step);
                ImPlot::SetupAxisLimits(ImAxis_Y1, range.low, range.high, ImPlotCond_Once);
                plot.Axes[ImAxis_Y1].SetRange(range.low, range.high);
            }
        } else { rt_price_window_ = {}; rt_effective_multiplier_ = rt_bucket_multiplier_; }
        stored_x_min_ = visible_x_min;
        stored_x_max_ = visible_x_max;

        // Publish the chart's coordinate system for the lesson spotlight overlay.
        // GetPlotPos/GetPlotSize give the plot-area rect that PlotToPixels maps
        // into; ranges match the axes set above. Read later in the frame by the
        // foreground-drawlist lesson overlay (linear projection, axes are linear).
        {
            const ImVec2 ppos = ImPlot::GetPlotPos();
            const ImVec2 psz  = ImPlot::GetPlotSize();
            edu::ChartProjection& cp = edu::chart_projection();
            cp.plot_min = ppos;
            cp.plot_max = ImVec2(ppos.x + psz.x, ppos.y + psz.y);
            cp.x_min_ms = visible_x_min;
            cp.x_max_ms = visible_x_max;
            cp.y_min = ImPlot::GetPlotLimits().Y.Min;
            cp.y_max = ImPlot::GetPlotLimits().Y.Max;
            cp.valid = true;
        }
        if (rt_mode_ && rt_dom_linked_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
            deliberate_price_drag && (ImPlot::IsAxisHovered(ImAxis_Y1) || ImPlot::IsPlotHovered()))
            rt_auto_price_ = false;
        // ── Render layers (back to front)
        // 1. Heatmap (background)
        // Determine replay cutoff for heatmap rendering
        const int64_t heatmap_replay_cutoff = ctx_.replay_mgr().is_active()
            ? ctx_.replay_mgr().interpolated_time_ms() : 0;

        if (!rt_mode_ && heatmap_enabled_ && ct_allows_time_overlays(chart_type_)) {
            ProfileScope _ps("OBHeat");
            auto* reconstructor = ctx_.heatmap_mgr().get_reconstructor(pair_, heatmap_mode_);
            if (reconstructor && reconstructor->has_data()) {

                configure_depth_fidelity(*reconstructor);

                reconstructor->set_replay_cutoff_ms(heatmap_replay_cutoff);
                const int64_t candle_ms = tf_sec * 1000;
                const double visible_span = visible_x_max - visible_x_min;
                const double vis_candles = visible_span / static_cast<double>(candle_ms);
                const bool show_labels = (vis_candles < 45);
                reconstructor->render_cells(candle_ms, heatmap_sensitivity_, show_labels);
                const int64_t coverage_end = heatmap_replay_cutoff > 0 ? heatmap_replay_cutoff :
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                if (reconstructor->has_missing_columns(static_cast<int64_t>(visible_x_min),
                        std::min(static_cast<int64_t>(visible_x_max), coverage_end))) {
                    const ImVec2 pos = ImPlot::GetPlotPos();
                    const ImVec2 note(pos.x + 12.0f, pos.y + 82.0f);
                    const char* text = "Depth gaps: no observation loaded";
                    const ImVec2 size = ImGui::CalcTextSize(text);
                    ImPlot::GetPlotDrawList()->AddRectFilled(
                        ImVec2(note.x - 4.0f, note.y - 2.0f),
                        ImVec2(note.x + size.x + 4.0f, note.y + size.y + 2.0f),
                        ImGui::GetColorU32(Theme::Tokens::PANEL));
                    ImPlot::GetPlotDrawList()->AddText(note,
                        ImGui::GetColorU32(Theme::Tokens::TX2), text);
                }
            }
            render_heatmap_tooltip();
        }
        if (rt_mode_ && rt_renderer_ && rt_depth_enabled_) {
            configure_depth_fidelity(*rt_renderer_);
            rt_renderer_->set_realtime_gap_bridge(rt_bridge_gaps_ && !ctx_.replay_mgr().is_active());
            rt_renderer_->render_cells(rt_renderer_->get_column_interval_ms(), heatmap_sensitivity_, false, rt_extend_depth_ && (!rt_history_view_ || realtime_live_edge()),
                realtime_projection_until(rt_clock_ms_, rt_span_ms_));
        }
#ifdef EDGEDEPTH_EXPOSURE_V2_DEV
        render_exposure_v2(heatmap_replay_cutoff>0?heatmap_replay_cutoff:int64_t(emscripten_date_now()));
#endif
        // 1.5 Liquidation timeline heatmap (the predictive shader map) -- ENABLED 2026-06-27.
        //     Re-enabled on the L1 footprint-located estimator field (peaky + persistent on the
        //     scoreboard + offline render). Per-level brightness is the per-tier USD with a
        //     sqrt→pow(0.6)→decaying-max contrast curve + viewport-adaptive Low/Peak
        //     (convert_to_heatmap_snapshot / render_liq_timeline); GL_NEAREST + extend-levels give
        //     the crisp MMT-style streaks. The discrete rails draw on top as the decision layer.
        // 1.55 Liq Heatmap = dense candle×leverage projection Field (flow-weighted, MMT-style).
        //      Background layer: drawn BEFORE rails + candles so it's the predictive-map backdrop.
        //      Client-computed from CandleManager → universal + deterministic on every symbol/TF.
        //      Never under the V2 map: one liquidation map at a time (scenario_map_shown).
        if (!rt_mode_ && scenario_map_shown() && ct_allows_time_overlays(chart_type_)) {
            ProfileScope _ps("LiqField");
            // Liquidation rails own the forward area when they draw: the scenario's own forward
            // projection steps aside for this frame (the user's knob is restored right after).
            const bool replay = ctx_.replay_mgr().is_active();
            const int64_t clock = replay ? ctx_.replay_mgr().interpolated_time_ms()
                : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            const bool extend = liq_field_.knobs().extend;
            const float opacity = liq_field_.knobs().opacity;
            if (touch_odds_enabled_ && touch_odds_overlay::owns_forward_area(pair_, clock, replay)) liq_field_.knobs().extend = false;
            // Historical rails stand on this same fuel: the plain map dims beneath them.
            const bool history = touch_odds_enabled_ && touch_odds_overlay::rails_history_ready(pair_, replay);
            if (history) liq_field_.knobs().opacity = opacity*0.35f;
            liq_field_.render();
            liq_field_.knobs().extend = extend;
            liq_field_.knobs().opacity = opacity;
            if (touch_odds_enabled_)
                touch_odds_overlay::rails_history(pair_, static_cast<uint64_t>(liq_field_.generation()), liq_field_.bucket_width_log(),
                    [this] { return liq_rails::runs_from_segments(liq_field_.segments(), LiqFieldRenderer::kSegPending); },
                    candles().timeframe_seconds()*1000, replay);
            // Always say what the picture is: brightness reads as a magnet otherwise (registered
            // tests found no touch, direction or after-touch signal beyond volatility).
            const ImVec2 pp = ImPlot::GetPlotPos(), ps = ImPlot::GetPlotSize();
            ImGui::PushFont(Theme::Fonts::mono_sm());
            const char* caption = "SCENARIO \xc2\xb7 assumed leverage \xc2\xb7 not positions or odds";
            const ImVec2 cs = ImGui::CalcTextSize(caption), at(pp.x + 10.0f, pp.y + ps.y - cs.y - 8.0f);
            ImPlot::GetPlotDrawList()->AddRectFilled(ImVec2(at.x - 4.0f, at.y - 2.0f), ImVec2(at.x + cs.x + 4.0f, at.y + cs.y + 2.0f), Theme::u32(Theme::Tokens::PANEL, 0.80f));
            ImPlot::GetPlotDrawList()->AddText(at, Theme::u32(Theme::Tokens::TX3), caption);
            ImGui::PopFont();
        }
        // 1.62 Scenario profile - smoothed price-marginal of the Field; independent
        //      toggle, candle-derived (needs no liq subscription or snapshot). Drawn
        //      before the census so the model never covers observed positions. Part of the scenario
        //      map (its setting in Layers), so it draws only with that map.
        if (!rt_mode_ && liq_profile_enabled_ && scenario_map_shown() && ct_allows_time_overlays(chart_type_)) {
            ProfileScope _ps("LiqProf");
            render_liq_profile();
        }
        // 1.65 Hyperliquid positions (HL census, as rails) and the V2 rails (estimated $ from this venue's open
        //      interest). Both are rails; when both draw, one leads (hl_leads, the V2 rails by default): the follower
        //      draws first, fainter, and the leader on top with its profile, labels and hover. Underlying-keyed, the
        //      census also overlays non-HL charts of the same coin.
        const bool hl_lead = hl_leads();
        if (hl_rails_drawn() && !hl_lead) {
            ProfileScope _ps("LiqCensus");
            render_liq_census(true);
        }
        if (liqmap2_rails_drawn()) {
            ProfileScope _ps("LiqMapV2");
            render_liqmap2(hl_lead);
        }
        if (hl_lead) {
            ProfileScope _ps("LiqCensus");
            render_liq_census(false);
        }
        // 1.7 VPVR profile overlay (sidebar histogram bars)
        if (!rt_mode_ && vpvr_enabled_) {
            // Request profile data when visible range changes
            const int64_t tf_ms = static_cast<int64_t>(tf_sec) * 1000;
            const int64_t start_ms = static_cast<int64_t>(visible_x_min);
            const int64_t end_ms = static_cast<int64_t>(visible_x_max) + tf_ms;
            ctx_.vpvr_mgr().request_profile(
                pair_.symbol, start_ms, end_ms,
                compute_vpvr_tick_per_row(
                    ImPlot::GetPlotLimits().Y.Max - ImPlot::GetPlotLimits().Y.Min,
                    ImPlot::GetPlotSize().y),
                &ctx_.stream_mgr());
            render_vpvr_profile();
        }
        // 2. Foreground price geometry - dispatch over ChartType.
        //    Candles / Footprint draw the REAL OHLC arrays; Heikin Ashi draws the
        //    derived HA arrays through the same plot_candles tiers; Line strokes a
        //    close-price polyline; TPO renders market-profile columns.
        {
            ProfileScope _ps("Candles");
            // Real building-candle OHLC (used by Candles / FP).
            BuildingOHLC bld{0, 0, 0, 0, false};
            if (candles().has_building_candle()) {
                const auto& bc = candles().building_candle();
                bld = {bc.open, bc.high, bc.low, bc.close, true};
            }
            if (!rt_mode_) switch (chart_type_) {
                case ChartType::TPO:
                    render_tpo(visible_x_min, visible_x_max);
                    break;
                case ChartType::Line:
                    plot_line(visible_x_min, visible_x_max);
                    break;
                case ChartType::HeikinAshi: {
                    // Heikin Ashi: same plot_candles tiers, HA arrays as the source.
                    // The building HA candle is derived per frame from the last
                    // finalized HA pair (falls back to the seed when none exists).
                    const auto& ha_o = candles().ha_opens();
                    const auto& ha_c = candles().ha_closes();
                    BuildingOHLC ha_bld{0, 0, 0, 0, false};
                    if (candles().has_building_candle()) {
                        const auto& bc = candles().building_candle();
                        const double hb_close = (bc.open + bc.high + bc.low + bc.close) * 0.25;
                        const double hb_open  = (!ha_o.empty())
                            ? (ha_o.back() + ha_c.back()) * 0.5
                            : (bc.open + bc.close) * 0.5;
                        const double hb_high = std::max({bc.high, hb_open, hb_close});
                        const double hb_low  = std::min({bc.low,  hb_open, hb_close});
                        ha_bld = {hb_open, hb_high, hb_low, hb_close, true};
                    }
                    plot_candles(visible_x_min, visible_x_max,
                                 candles().ha_opens(), candles().ha_highs(),
                                 candles().ha_lows(),  candles().ha_closes(),
                                 ha_bld);
                    break;
                }
                case ChartType::Candles:
                case ChartType::FootprintCluster:
                case ChartType::FootprintProfile:
                case ChartType::Renko:
                default:
                    plot_candles(visible_x_min, visible_x_max,
                                 candles().opens(), candles().highs(),
                                 candles().lows(),  candles().closes(), bld);
                    break;
            }
        }
        // 2.05 Ghost scrub-preview candles (replay only, while the user aims a
        //      scrub on the transport). Drawn over the real candles, under the
        //      Observed markers; reads ONLY the PreviewCandleStore. Skipped for
        //      Line (polyline ghost is a follow-up) and TPO/Renko (no time axis).
        if (rt_mode_) render_realtime();
        if (!rt_mode_ && scrub_preview_ms > 0 && chart_type_ != ChartType::Line &&
            ct_allows_time_overlays(chart_type_)) {
            ProfileScope _ps("ScrubPreview");
            render_scrub_preview(visible_x_min, std::max(visible_x_max, x_max));
        }
        // 1.99 The Hyperliquid positions' key, over the candles (render_liq_census filled it this frame).
        if (hl_rails_drawn()) hl_rails_overlay::draw_key(hl_key_);
        // 2.0 Trade bubbles on candles (free): large prints over the candles, the
        //     same marker real-time mode uses for every trade. Below the observed
        //     liquidation diamonds so a report never hides behind a print.
        if (!rt_mode_ && candle_bubbles_ && ct_allows_time_overlays(chart_type_)) {
            ProfileScope _ps("CandleBubbles");
            render_candle_bubbles();
        }
        // 2.02 Recorded refill levels (candles): exact prices that kept refilling, over the candles and under the
        //      liquidation diamonds and drawings. Its hover waits until every other overlay has had its say.
        if (!rt_mode_ && ct_allows_time_overlays(chart_type_)) {
            ProfileScope _ps("RefillHist");
            render_refill_history();
        }
        // 2.1 Observed - real @forceOrder liquidation markers (WS4). Drawn OVER the
        //     candles: liquidations fire at traded prices, so an under-candle layer
        //     would be occluded by the very candles that consumed them.
        if (reported_liquidations_enabled() && (rt_mode_ || ct_allows_time_overlays(chart_type_))) {
            ProfileScope _ps("LiqObs");
            prepare_liquidations();
            if (rt_mode_ ? rt_liq_markers_ : liq_markers_) render_liq_observed();
            if (liq_report_profile_) render_liquidation_profile();
            // Without the activity subplot, a word on the chart only while history loads or fails.
            if (!liquidation_activity_visible() && liq_status_attention_) {
                const auto pos=ImPlot::GetPlotPos(), size=ImPlot::GetPlotSize();
                auto* draw=ImPlot::GetPlotDrawList();
                const ImVec2 at(pos.x+10,pos.y+size.y-26);
                const float w=ImGui::CalcTextSize(liq_status_).x;
                draw->AddRectFilled(at,ImVec2(at.x+w+16,at.y+20),Theme::u32(Theme::Tokens::PANEL,0.92f),Theme::Radius::R2);
                draw->AddText(ImVec2(at.x+8,at.y+(20-ImGui::GetFontSize())*0.5f),Theme::u32(Theme::Tokens::TX3),liq_status_);
                Theme::chart_chip(at,ImVec2(at.x+w+16,at.y+20));
            }
        }
        if (rt_mode_ || ct_allows_time_overlays(chart_type_)) render_activity_alerts();
        if (!rt_mode_) draw_ohlc_readout();
        // v2 3b: the LIVE status pill is redundant (top-bar Live toggle + status bar).
        // Keep the chip only in replay, where it shows the playback position.
        // No status chip while the clip recorder drives: it would burn the
        // CAPTURE speed (pre-retime) and the wall-position clock into the
        // produced video (CLIP_FACTORY P2).
        if (ctx_.replay_mgr().is_active() && !edu::RecorderRuntime::instance().active())
            draw_status_chip();
        // 2.5 Footprint overlay (per-candle volume grid / profile sidebar)
        if (!rt_mode_ && ctx_.footprint_mgr().enabled) {
            ProfileScope _ps("Footprint");
            if (ctx_.footprint_mgr().mode == FootprintManager::Mode::Profile) {
                render_footprint_profile(visible_x_min, visible_x_max);
            } else {
                render_footprint_overlay(visible_x_min, visible_x_max);
            }
        }
        // 3. Admin pattern context. Live-only and already gated at subscription.
        if (!rt_mode_ && !ctx_.replay_mgr().is_active() && ct_allows_time_overlays(chart_type_)) {
            ProfileScope _ps("Patterns");
            render_pattern_overlay(visible_x_min, visible_x_max);
        }
        if (!rt_mode_ && ct_allows_time_overlays(chart_type_)) {
            const int64_t clock = ctx_.replay_mgr().is_active() ? ctx_.replay_mgr().interpolated_time_ms()
                : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            compression::render(pair_, clock);
            if (touch_odds_enabled_) {
                const bool replay = ctx_.replay_mgr().is_active();
                const bool hosted = touch_odds_overlay::available(pair_);
                // One fuel source per chart: observed HL positions when chosen and usable, else the scenario map,
                // which then says why it is standing in. The rails draw only where nothing else holds the forward
                // area (touch_rails_off), and only then do the 20-minute bands give way to them.
                liq_rails::FuelSource fuel;
                const bool scenario = scenario_map_shown();
                bool rails = false;
                if (hosted) {
                    if (observed_rails_allowed() && touch_odds_overlay::observed_fuel_selected()) fuel = observed_rails_fuel(clock);
                    if (fuel.kind == liq_rails::FuelKind::Scenario && scenario)
                        for (const auto& s : liq_field_.standing()) fuel.standing.push_back({s.price, s.fuel});
                    rails = !touch_rails_off() && (fuel.kind != liq_rails::FuelKind::Scenario || scenario) &&
                            touch_odds_overlay::rails_active(pair_, clock, replay);
                }
                touch_odds_overlay::render(pair_, clock, fmt_.price_fmt, replay, rails);
                if (hosted) {
                    if (rails) touch_odds_overlay::rails(pair_, clock, fmt_.price_fmt, fuel, replay);
                    else if (liqmap2_rails_drawn() || (hl_leads() && hl_profile_)) touch_odds_overlay::step_aside();
                    else if (scenario && fuel.kind == liq_rails::FuelKind::Scenario)
                        touch_odds_overlay::zone_labels(pair_, clock, fmt_.price_fmt, std::move(fuel.standing), replay);
                    else touch_odds_overlay::step_aside();
                }
            }
        }
        // 3.5 Current-price axis tag (green if current candle bullish, red if bearish).
        //     Uses the REAL candle close (never HA) so the tag tracks true price in
        //     every non-TPO mode.
        if (chart_type_ != ChartType::TPO && !ctx_.replay_mgr().is_loading()) {
            double cur_price = 0.0;
            bool have_price = false, bullish = true;
            if (candles().has_building_candle()) {
                const auto& bc = candles().building_candle();
                cur_price = bc.close;
                bullish = bc.close >= bc.open;
                have_price = true;
            } else if (!candles().closes().empty()) {
                const auto& cl = candles().closes();
                const auto& op = candles().opens();
                cur_price = cl.back();
                bullish = cl.back() >= op.back();
                have_price = true;
            }
            if (rt_mode_) {
                have_price = false;
                for (auto it = realtime_trades().rbegin(); it != realtime_trades().rend(); ++it) {
                    if (it->timestamp_ms > rt_clock_ms_) continue;
                    cur_price = it->price; bullish = it->is_buy; have_price = true;
                    break;
                }
            }
            if (have_price) {
                cursor_reference_price_ = cur_price;
                char price_buf[32];
                fmt_.format_price(price_buf, sizeof(price_buf), cur_price);
                const ImU32 tag_col = bullish ? Theme::get_buy_color_u32(255)
                                              : Theme::get_sell_color_u32(255);
                CustomImPlot::DrawAxisValueTag(cur_price, price_buf, tag_col, true);
            }
        }
        // Drawing tools: placement/hit-test/drag + render. Runs BEFORE
        // handle_plot_interaction so captures_mouse() gates the chart's own
        // handlers the same frame. (The input-map override is applied in
        // begin_frame, before BeginPlot - too late to matter from here.)
        if (!rt_mode_) render_refill_history_hover();
        render_liquidity_response();
        render_reference_context();
        drawing_layer_.render_in_plot(ctx_, fmt_,
                                      ct_allows_time_overlays(chart_type_),
                                      tf_sec);
        // Real-time hover readout last: any other overlay's tooltip wins.
        if (rt_mode_) render_realtime_hover();
        // 4. Interaction
        handle_plot_interaction();
        // Update viewport state for next frame
        last_visible_range_ = ImPlot::GetPlotLimits();
        x_axis_min_ = last_visible_range_.X.Min;
        x_axis_max_ = last_visible_range_.X.Max;
        // Linked charts: mirror the other chart's crosshair time, and publish
        // this chart's axis + hover for the rest of the group. Works for the
        // compare link and the user "Link charts" toggle, in both real-time
        // and candle mode (TPO has its own axis semantics and stays out).
        if (link_active() && chart_type_ != ChartType::TPO) {
            time_link_draw_mirror();
            time_link_publish();
        }
        ImPlot::EndPlot();
    }
    if (rt_axis_scale_map_override_) {
        ImPlot::GetInputMap() = rt_axis_scale_saved_map_;
        rt_axis_scale_map_override_ = false;
    }
    ImPlot::PopStyleVar(2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Time link (compare replay): one axis, one crosshair time, one timeframe
// ═══════════════════════════════════════════════════════════════════════════════

// Follower side, before BeginPlot's X setup. The owner is whichever linked
// chart the mouse is over (it drives the axis); when nothing is hovered the
// last owner keeps publishing, so a follower keeps tracking it. Ownership
// is a frame stamp: an owner that stopped rendering releases the group.
void ChartWidget::time_link_apply(double& x_min, double& x_max, bool& force_always) {
    if (!link_active() || chart_type_ == ChartType::TPO) return;
    const int frame = ImGui::GetFrameCount();
    const TimeLink& l = g_time_link;
    if (rt_mode_) {
        if (l.rt_owner == 0 || l.rt_owner == link_id_ || l.rt_owner_frame < frame - 2) return;
        const double span = l.rt_x_max - l.rt_x_min;
        if (l.rt_follow) {
            // Both ride the same wall clock: match the owner's zoom span and
            // let each chart keep its own right edge (a paused view stays put).
            if (span > 0) rt_span_ms_ = std::clamp(span, 5000.0, 1800000.0 / 0.88);
            if (!candles().follow_live()) candles().set_follow_live(true);
            return;
        }
        // Owner is parked in history: take its axis verbatim, like candles.
        if (candles().follow_live()) candles().set_follow_live(false);
        if (span > 0) {
            x_min = l.rt_x_min;
            x_max = l.rt_x_max;
            last_visible_range_.X.Min = l.rt_x_min;
            last_visible_range_.X.Max = l.rt_x_max;
            force_always = true;
        }
        return;
    }
    if (l.owner == 0 || l.owner == link_id_ || l.owner_frame < frame - 2) return;
    if (l.follow) {
        // Both follow the same clock at the same timeframe: the chart's own
        // follow range already lands on the owner's. Match the latch only.
        if (!candles().follow_live()) candles().set_follow_live(true);
        return;
    }
    if (candles().follow_live()) candles().set_follow_live(false);
    if (l.x_max > l.x_min) {
        x_min = l.x_min;
        x_max = l.x_max;
        last_visible_range_.X.Min = l.x_min;
        last_visible_range_.X.Max = l.x_max;
        force_always = true;
    }
}

// Owner side, after the plot: the hovered chart takes ownership; the current
// owner re-stamps every frame it renders so followers keep tracking. Each
// family (candle / real-time) keeps its own owner so a mixed desktop syncs
// within both without either stealing the other's axis.
void ChartWidget::time_link_publish() {
    const int frame = ImGui::GetFrameCount();
    TimeLink& l = g_time_link;
    const bool hovered = ImPlot::IsPlotHovered() || ImPlot::IsAxisHovered(ImAxis_X1);
    if (rt_mode_) {
        const bool owner_gone = l.rt_owner == 0 || l.rt_owner_frame < frame - 2;
        if (hovered || owner_gone || l.rt_owner == link_id_) {
            l.rt_owner = link_id_;
            l.rt_owner_frame = frame;
            l.rt_x_min = last_visible_range_.X.Min;
            l.rt_x_max = last_visible_range_.X.Max;
            l.rt_follow = candles().follow_live();
        }
    } else {
        const bool owner_gone = l.owner == 0 || l.owner_frame < frame - 2;
        if (hovered || owner_gone || l.owner == link_id_) {
            l.owner = link_id_;
            l.owner_frame = frame;
            l.x_min = last_visible_range_.X.Min;
            l.x_max = last_visible_range_.X.Max;
            l.follow = candles().follow_live();
        }
    }
    if (hovered) {
        l.hover_owner = link_id_;
        l.hover_frame = frame;
        l.hover_x = ImPlot::GetPlotMousePos().x;
    }
}

// A dashed vertical at the other chart's crosshair time. Drawn inside the
// plot (clip rect), so it never leaks over the axes.
void ChartWidget::time_link_draw_mirror() {
    const TimeLink& l = g_time_link;
    const int frame = ImGui::GetFrameCount();
    if (l.hover_owner == 0 || l.hover_owner == link_id_ || l.hover_frame < frame - 1) return;
    const ImPlotRect lim = ImPlot::GetPlotLimits();
    if (l.hover_x < lim.X.Min || l.hover_x > lim.X.Max) return;
    ImPlot::PushPlotClipRect();
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const float x = ImPlot::PlotToPixels(l.hover_x, lim.Y.Min).x;
    const ImVec2 top(x, ImPlot::GetPlotPos().y);
    const float bottom = ImPlot::GetPlotPos().y + ImPlot::GetPlotSize().y;
    const ImU32 col = Theme::u32(Theme::Tokens::BRAND, 0.55f);
    // 6 px dash, 4 px gap.
    for (float y = top.y; y < bottom; y += 10.0f)
        dl->AddLine(ImVec2(x, y), ImVec2(x, std::min(y + 6.0f, bottom)), col, 1.0f);
    ImPlot::PopPlotClipRect();
}

// Per frame (update): adopt a timeframe another linked chart switched to.
void ChartWidget::time_link_adopt_timeframe() {
    if (!link_active() || rt_mode_) return;  // RT buckets are per-chart by design
    const TimeLink& l = g_time_link;
    if (l.tf_seconds <= 0 || l.tf_owner == link_id_ || l.tf_frame <= link_tf_seen_frame_) return;
    link_tf_seen_frame_ = l.tf_frame;
    if (candles().timeframe_seconds() != l.tf_seconds) change_timeframe(l.tf_seconds);
}


// ═══════════════════════════════════════════════════════════════════════════════
// Renko (ChartType::Renko) - price-driven bricks on a brick-index X-axis.
// Self-contained render path; the time-axis render_chart() is untouched.
// See core/renko_builder.h.
// ═══════════════════════════════════════════════════════════════════════════════

// Resolved brick size in price units. Manual override wins; otherwise the auto
// size resolves ONCE from the latest price and freezes (recomputing per frame as
// price moves would repaint the whole brick sequence - the plan's determinism /
// "store the resolved size" requirement).
double ChartWidget::renko_resolved_size() {
    if (renko_brick_ticks_ > 0) {                       // manual override
        renko_resolved_ticks_ = renko_brick_ticks_;
        return static_cast<double>(renko_resolved_ticks_) * tick_size_;
    }
    if (renko_resolved_ticks_ > 0)                       // auto, already frozen
        return static_cast<double>(renko_resolved_ticks_) * tick_size_;
    // Auto = volatility-adaptive ATR(14) of the loaded candles - the TradingView /
    // Binance "Renko [ATR(14), x]" default. Resolved ONCE and frozen so it stays
    // replay-stable (the deferred-ATR concern was CONTINUOUS recompute/repaint;
    // freezing avoids that). A fixed %-of-price default was ~6x too small on a
    // volatile symbol, which is what produced the wall of tiny, noisy bricks.
    const double atr = renko_atr(14);
    if (atr > 0.0 && tick_size_ > 0.0) {
        renko_resolved_ticks_ = std::max(1, static_cast<int>(std::llround(atr / tick_size_)));
        return static_cast<double>(renko_resolved_ticks_) * tick_size_;
    }
    // ATR not ready yet (too few candles) → temporary %-of-price size, NOT frozen,
    // so it re-resolves to ATR next frame once candles have loaded.
    double ref = candles().last_close_price();
    if (ref <= 0.0 && !candles().closes().empty())
        ref = candles().closes().back();
    const int tmp = RenkoBuilder::auto_brick_ticks(ref, tick_size_);
    return static_cast<double>(std::max(1, tmp)) * tick_size_;
}

// Average True Range over the most recent `period` CLOSED candles (price units).
// 0 when there are too few candles.
double ChartWidget::renko_atr(int period) const {
    const auto& highs  = candles().highs();
    const auto& lows   = candles().lows();
    const auto& closes = candles().closes();
    const size_t n = std::min({highs.size(), lows.size(), closes.size()});
    if (period < 1 || n < static_cast<size_t>(period) + 1) return 0.0;
    double sum = 0.0;
    for (size_t i = n - static_cast<size_t>(period); i < n; ++i) {
        const double tr = std::max({highs[i] - lows[i],
                                    std::fabs(highs[i] - closes[i - 1]),
                                    std::fabs(lows[i]  - closes[i - 1])});
        sum += tr;
    }
    return sum / static_cast<double>(period);
}

// Rebuild the committed bricks only when the closed-candle signature / brick
// size / timeframe change, then fold the building candle in per frame.
void ChartWidget::ensure_renko() {
    const double bsize = renko_resolved_size();
    const auto& closes = candles().closes();
    const auto& ts     = candles().timestamps();
    const int64_t tf   = candles().timeframe_seconds();
    const size_t  n    = std::min(closes.size(), ts.size());
    // Key on the last CLOSED candle's TIMESTAMP, never its close. The client
    // mutates a closed candle's close during replay (finalize-replace, late
    // trades, 15s-tick merges); keying on close would re-run build() and - with
    // the append-only builder ignoring the tail anyway - do nothing useful, while
    // keying on close in a full-rebuild world was what un-printed bricks. The
    // timestamp only changes when a genuinely new candle closes (append) or the
    // set is reloaded (seek / timeframe), which is exactly when we must fold.
    const int64_t last_t = n ? static_cast<int64_t>(ts[n - 1]) : 0;

    const bool dirty = (n != renko_sig_count_) || (last_t != renko_sig_time_) ||
                       (bsize != renko_sig_size_) || (tf != renko_sig_tf_);
    if (dirty && bsize > 0.0) {
        // close_times parallel to closes (both from the same candle deque). The
        // rebuild is infrequent (candle finalize / scroll-load), so the transient
        // int64 copy of the timestamps is fine. build() is append-only - it folds
        // only the candles newer than the ones already consumed.
        static thread_local std::vector<int64_t> times;
        times.clear();
        times.reserve(n);
        for (size_t i = 0; i < n; ++i) times.push_back(static_cast<int64_t>(ts[i]));
        renko_.build(closes, times, bsize);
        renko_sig_count_ = n;
        renko_sig_time_  = last_t;
        renko_sig_size_  = bsize;
        renko_sig_tf_    = tf;
    }

    // Live leading edge: fold the building (unfinished) candle into a SEPARATE
    // provisional list. These bricks repaint as live price moves - a retrace can
    // remove the rightmost one, then it re-forms. That IS correct Renko: Binance's
    // live Renko (and others) repaint the forming brick the same way. They render
    // SOLID (see plot_renko), identical to committed bricks - the only earlier
    // mistake was drawing them hollow. Crucially they live in building_, NOT
    // bricks_, so a repaint here can never touch a PUBLISHED (closed-candle) brick;
    // those stay append-only and permanent. On candle close they hand off to
    // committed. fold_building() clears the list first, so with no building candle
    // (passing the last committed close) it simply stays empty.
    if (candles().has_building_candle()) {
        const auto& bc = candles().building_candle();
        renko_.fold_building(bc.close, bc.timestamp_ms);
    } else {
        renko_.fold_building(n > 0 ? closes[n - 1] : 0.0,
                             n > 0 ? static_cast<int64_t>(ts[n - 1]) : 0);
    }
}

// Filled brick rectangles over the visible brick-index slice [i0, i1).
void ChartWidget::plot_renko(size_t i0, size_t i1) {
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const size_t n_total = renko_.total_count();
    if (n_total == 0) return;
    i1 = std::min(i1, n_total);
    if (i0 >= i1) return;

    // Pixels per brick-index unit → adaptive tier. Zoomed out, a per-brick gap +
    // sub-pixel width render bricks as dotted slivers; the compressed tier drops
    // the gap and floors each brick at 1px so the staircase stays a solid band.
    const double px_per_brick = ImPlot::PlotToPixels(1.0, 0.0).x
                              - ImPlot::PlotToPixels(0.0, 0.0).x;
    const bool compressed = px_per_brick < 4.0;
    const double kGap = compressed ? 0.0 : 0.08;

    ImPlot::PushPlotClipRect();
    // All bricks draw SOLID - committed (closed-candle) AND the live provisional
    // ones (building candle, indices >= renko_.bricks().size()). No hollow: the
    // live leading edge looks identical to the rest; it just repaints as price
    // moves (correct Renko), while committed bricks stay permanent (append-only).
    for (size_t i = i0; i < i1; ++i) {
        const RenkoBrick& b = renko_.at(i);
        const double xl = static_cast<double>(i) + kGap;
        const double xr = static_cast<double>(i) + 1.0 - kGap;
        const double y_top = std::max(b.open, b.close);
        const double y_bot = std::min(b.open, b.close);
        const ImVec2 p_tl = ImPlot::PlotToPixels(xl, y_top);  // higher price → smaller y
        const ImVec2 p_br = ImPlot::PlotToPixels(xr, y_bot);
        float top_y = p_tl.y, bot_y = p_br.y;
        if (bot_y - top_y < 1.0f) bot_y = top_y + 1.0f;  // never let a brick vanish (thin)
        float left_x = p_tl.x, right_x = p_br.x;
        if (right_x - left_x < 1.0f) right_x = left_x + 1.0f;  // …or (narrow) at zoom-out
        const ImU32 fill = b.up ? Theme::get_buy_color_u32(255)
                                : Theme::get_sell_color_u32(255);
        dl->AddRectFilled(ImVec2(left_x, top_y), ImVec2(right_x, bot_y), fill);
    }
    ImPlot::PopPlotClipRect();
}

// X ticks EVENLY spaced by brick index (the TradingView Renko look): uniform
// on-screen spacing, each label showing that brick's actual close time. Renko
// time is non-uniform, so the labels are NOT round clock values - but the SPACING
// is consistent, which is what matters (the earlier wall-clock scheme gave clean
// times but wildly uneven gaps, since a quiet stretch packs few bricks into a
// long time and a busy one packs many into a short time).
void ChartWidget::setup_renko_axis_ticks(double vis_i_min, double vis_i_max) const {
    const size_t n_total = renko_.total_count();
    if (n_total == 0) return;
    size_t i0 = (vis_i_min <= 0.0) ? 0 : std::min(n_total - 1, static_cast<size_t>(std::floor(vis_i_min)));
    size_t i1 = (vis_i_max <= 0.0) ? n_total : std::min(n_total, static_cast<size_t>(std::ceil(vis_i_max)));
    if (i1 <= i0) return;
    const size_t span = i1 - i0;

    // Stride by a round number of BRICKS → ~8 evenly-spaced labels.
    size_t stride = std::max<size_t>(1, span / 8);
    static constexpr size_t nice[] = {1, 2, 3, 4, 5, 8, 10, 15, 20, 25, 30, 40,
                                      50, 75, 100, 150, 200, 300, 500, 1000, 2000};
    for (size_t v : nice) { if (v >= stride) { stride = v; break; } }

    renko_tick_pos_.clear();
    renko_tick_lbls_.clear();
    renko_tick_ptrs_.clear();
    size_t start = ((i0 + stride - 1) / stride) * stride;  // first multiple of stride >= i0
    char prev_day[16]{};
    for (size_t i = start; i < i1; i += stride) {
        const RenkoBrick& b = renko_.at(i);
        renko_tick_pos_.push_back(static_cast<double>(i) + 0.5);
        char day[16]{};
        DisplayTimeZone::instance().format(b.close_time_ms, TimeZoneFormat::DateOnly,
                                            day, sizeof(day));
        char lbl[32];
        const bool day_changed = prev_day[0] && std::strcmp(day, prev_day) != 0;
        DisplayTimeZone::instance().format(b.close_time_ms,
            (day_changed || !prev_day[0]) ? TimeZoneFormat::MonthDayTime
                                           : TimeZoneFormat::TimeMinutes,
            lbl, sizeof(lbl));
        std::snprintf(prev_day, sizeof(prev_day), "%s", day);
        renko_tick_lbls_.emplace_back(lbl);
    }
    // Second pass: storage is final → c_str() pointers are stable.
    for (const auto& s : renko_tick_lbls_) renko_tick_ptrs_.push_back(s.c_str());
    if (!renko_tick_pos_.empty())
        ImPlot::SetupAxisTicks(ImAxis_X1, renko_tick_pos_.data(),
            static_cast<int>(renko_tick_pos_.size()), renko_tick_ptrs_.data(), false);
}

void ChartWidget::render_chart_renko() {
    const bool follow = candles().follow_live();

    // ── Scroll-load view stability (capture BEFORE ensure_renko may rebuild) ──
    // A historical prepend shifts every brick index right; anchor the view to the
    // left-edge brick's TIME so we can shift the axis back afterwards and keep the
    // same bricks on screen (otherwise the view jumps + the loader keeps re-firing).
    int64_t anchor_time = 0;
    double  anchor_frac = 0.0;
    if (!follow && renko_.total_count() > 0) {
        const double xm = last_visible_range_.X.Min;
        size_t ai; double frac;
        if (xm <= 0.0) { ai = 0; frac = xm; }  // at / left-of the oldest brick
        else {
            ai   = std::min(renko_.total_count() - 1, static_cast<size_t>(std::floor(xm)));
            frac = xm - std::floor(xm);
        }
        anchor_time = renko_.at(ai).close_time_ms;
        anchor_frac = frac;
    }

    ensure_renko();
    const size_t n_total = renko_.total_count();
    const double bsize   = renko_.brick_size();

    // brick-index X cannot be time-projected → mark the lesson/studio projection
    // invalid so the spotlight + capture never misproject in Renko (v1).
    edu::chart_projection().valid = false;

    if (n_total == 0 || bsize <= 0.0) {
        ImGui::Text("Renko: waiting for candle data...");
        return;
    }

    // Re-anchor after a possible prepend: find the anchored brick's NEW index and
    // shift the stored view by the delta so it lands back on the same bricks.
    bool renko_reanchor = false;
    if (anchor_time > 0 && !follow) {
        const auto& br = renko_.bricks();
        size_t lo = 0, hi = br.size();  // first committed brick with time >= anchor_time
        while (lo < hi) { const size_t mid = (lo + hi) / 2;
            if (br[mid].close_time_ms < anchor_time) lo = mid + 1; else hi = mid; }
        const size_t new_ai = br.empty() ? 0 : std::min(lo, br.size() - 1);
        const double delta = (static_cast<double>(new_ai) + anchor_frac) - last_visible_range_.X.Min;
        if (delta > 0.5) {  // bricks were prepended → shift the view right to match
            last_visible_range_.X.Min += delta;
            last_visible_range_.X.Max += delta;
            renko_reanchor = true;
        }
    }

    // ── X range: brick-index domain; follow-live pins the newest brick.
    constexpr double kRenkoVisibleBricks = 140.0;
    const double x_pad = 2.0;
    const double x_max = static_cast<double>(n_total) + x_pad;
    double x_min = 0.0;
    if (follow && static_cast<double>(n_total) > kRenkoVisibleBricks)
        x_min = static_cast<double>(n_total) - kRenkoVisibleBricks;

    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(10, 10));
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotBorderSize, 1.0f);
    if (ImPlot::BeginPlot("##Price", ImVec2(-1, chart_allocated_height_),
                          ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_None);
        ImPlot::SetupAxis(ImAxis_Y1, nullptr, ImPlotAxisFlags_Opposite);
        ImPlot::SetupAxisFormat(ImAxis_Y1, fmt_.price_fmt);

        // Min zoom: never collapse below ~8 bricks.
        const double min_x_span = 8.0;
        bool zoom_clamped = false;
        if (!follow && last_visible_range_.X.Max - last_visible_range_.X.Min > 0 &&
            last_visible_range_.X.Max - last_visible_range_.X.Min < min_x_span) {
            const double center = (last_visible_range_.X.Min + last_visible_range_.X.Max) * 0.5;
            last_visible_range_.X.Min = center - min_x_span * 0.5;
            last_visible_range_.X.Max = center + min_x_span * 0.5;
            zoom_clamped = true;
        }
        if (renko_reanchor) {
            // Prepend just happened - force the shifted range so the view stays on
            // the same bricks (one frame; ImPlot keeps it afterwards under Once).
            ImPlot::SetupAxisLimits(ImAxis_X1, last_visible_range_.X.Min,
                                    last_visible_range_.X.Max, ImPlotCond_Always);
        } else if (follow || zoom_clamped) {
            ImPlot::SetupAxisLimits(ImAxis_X1,
                zoom_clamped ? last_visible_range_.X.Min : x_min,
                zoom_clamped ? last_visible_range_.X.Max : x_max,
                ImPlotCond_Always);
        } else {
            ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max, ImPlotCond_Once);
        }

        // Visible brick-index slice.
        const double vis_i_min = last_visible_range_.X.Min > 0 ? last_visible_range_.X.Min : x_min;
        const double vis_i_max = last_visible_range_.X.Max > 0 ? last_visible_range_.X.Max : x_max;
        size_t i0 = (vis_i_min <= 0.0) ? 0 : std::min(n_total, static_cast<size_t>(std::floor(vis_i_min)));
        size_t i1 = (vis_i_max <= 0.0) ? n_total : std::min(n_total, static_cast<size_t>(std::ceil(vis_i_max)) + 1);
        if (i1 < i0) i1 = i0;

        // Live price (REAL close, never a brick level) - drives the forming brick,
        // the current-price tag, AND the Y-limits (so the forming tip never clips).
        double cur_price = 0.0; bool have_price = false, bullish = true;
        if (candles().has_building_candle()) {
            const auto& bc = candles().building_candle();
            cur_price = bc.close; bullish = bc.close >= bc.open; have_price = true;
        } else if (!candles().closes().empty()) {
            const auto& cl = candles().closes();
            const auto& op = candles().opens();
            cur_price = cl.back(); bullish = cl.back() >= op.back(); have_price = true;
        }

        // Y-limits from the visible bricks' price extents (fallback: whole series).
        double y_min = std::numeric_limits<double>::infinity();
        double y_max = -std::numeric_limits<double>::infinity();
        for (size_t i = i0; i < i1; ++i) {
            const RenkoBrick& b = renko_.at(i);
            y_min = std::min(y_min, std::min(b.open, b.close));
            y_max = std::max(y_max, std::max(b.open, b.close));
        }
        if (!std::isfinite(y_min)) {
            for (size_t i = 0; i < n_total; ++i) {
                const RenkoBrick& b = renko_.at(i);
                y_min = std::min(y_min, std::min(b.open, b.close));
                y_max = std::max(y_max, std::max(b.open, b.close));
            }
            if (!std::isfinite(y_min)) { y_min = 0.0; y_max = 1.0; }
        }
        if (have_price) { y_min = std::min(y_min, cur_price); y_max = std::max(y_max, cur_price); }
        const double y_pad = (y_max - y_min) * 0.08 + bsize * 0.5;
        ImPlot::SetupAxisLimits(ImAxis_Y1, y_min - y_pad, y_max + y_pad, ImPlotCond_Always);

        setup_renko_axis_ticks(vis_i_min, vis_i_max);

        // Publish the visible bricks' TIME window so update() feeds CandleManager
        // a time range (never brick indices) for scroll-load / history.
        if (i1 > i0) {
            // Left-edge brick's TIME drives the historical loader (its threshold is
            // candle-time based). When scrolled to the oldest bricks this sits near
            // the oldest candle, so one batch loads; the re-anchor above then keeps
            // the view on the same bricks, moving the left edge far enough from the
            // (now much older) oldest candle that the loader stops - one batch per
            // scroll gesture, exactly like the candle chart. NO oldest-candle
            // override (that reported "at the edge" forever → back-loaded one tiny
            // batch per cooldown, thrashing the view + ticks).
            renko_view_t0_ms_ = renko_.at(i0).close_time_ms;
            renko_view_t1_ms_ = renko_.at(i1 - 1).close_time_ms;
        }

        plot_renko(i0, i1);

        // Current-price axis tag.
        if (have_price) {
            cursor_reference_price_ = cur_price;
            char pb[32]; fmt_.format_price(pb, sizeof(pb), cur_price);
            const ImU32 tc = bullish ? Theme::get_buy_color_u32(255)
                                     : Theme::get_sell_color_u32(255);
            CustomImPlot::DrawAxisValueTag(cur_price, pb, tc, true);
        }

        // Minimal Renko readout (top-left): brick size in price + resolved ticks.
        {
            ImDrawList* dl = ImPlot::GetPlotDrawList();
            ImVec2 pos = ImPlot::GetPlotPos(); pos.x += 10.0f; pos.y += 8.0f;
            const ImU32 lbl = Theme::u32(Theme::Tokens::TX4);
            const ImU32 val = Theme::u32(Theme::Tokens::TX1);
            ImGui::PushFont(Theme::Fonts::mono_sm());
            auto field = [&](const char* label, const char* v) {
                dl->AddText(pos, lbl, label);
                pos.x += ImGui::CalcTextSize(label).x + 4.0f;
                dl->AddText(pos, val, v);
                pos.x += ImGui::CalcTextSize(v).x + 12.0f;
            };
            char bs[32]; fmt_.format_price(bs, sizeof(bs), bsize);
            char tk[48];
            if (renko_brick_ticks_ == 0)
                snprintf(tk, sizeof(tk), "ATR(14) \xc2\xb7 %d ticks", renko_resolved_ticks_);
            else
                snprintf(tk, sizeof(tk), "%d ticks", renko_resolved_ticks_);
            field("RENKO", bs);
            dl->AddText(pos, lbl, tk);
            pos.x += ImGui::CalcTextSize(tk).x + 10.0f;
            // Gear affordance → opens the brick-size config (also right-clickable
            // on the chart-type dropdown's Renko row).
            const float gr = 6.0f;
            const ImVec2 gc(pos.x + gr, pos.y + ImGui::GetFontSize() * 0.5f);
            const bool gear_hov = ImGui::IsMouseHoveringRect(
                ImVec2(gc.x - gr - 3, gc.y - gr - 3), ImVec2(gc.x + gr + 3, gc.y + gr + 3));
            const ImU32 gcol = gear_hov ? Theme::u32(Theme::Tokens::BRAND)
                                        : Theme::u32(Theme::Tokens::TX3);
            for (int t = 0; t < 8; ++t) {  // cog teeth
                const float ang = static_cast<float>(t) / 8.0f * 6.2831853f;
                dl->AddLine(ImVec2(gc.x + cosf(ang) * gr * 0.68f, gc.y + sinf(ang) * gr * 0.68f),
                            ImVec2(gc.x + cosf(ang) * gr,         gc.y + sinf(ang) * gr), gcol, 1.6f);
            }
            dl->AddCircle(gc, gr * 0.60f, gcol, 12, 1.4f);
            renko_gear_pos_ = ImVec2(gc.x - 30.0f, gc.y + gr + 8.0f);  // popup anchor
            if (gear_hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                open_renko_settings_ = true;
            ImGui::PopFont();
        }

        if (ctx_.replay_mgr().is_active() && !edu::RecorderRuntime::instance().active())
            draw_status_chip();

        // ── Interaction: follow-live latch + price crosshair + REPLAY-FROM.
        //    Renko has no time X-axis, but each brick carries its source candle's
        //    close_time_ms, so "replay from" maps the brick under the cursor back
        //    to a real time and reuses the exact candle-chart entry points (R key
        //    + right-click menu). Measure / alerts / shift-drag range-select stay
        //    off in Renko v1.
        auto brick_time_at = [&](double px) -> int64_t {
            const size_t nt = renko_.total_count();
            if (nt == 0) return 0;
            double xi = std::floor(px);
            if (xi < 0.0) xi = 0.0;
            size_t bi = static_cast<size_t>(xi);
            if (bi >= nt) bi = nt - 1;
            return renko_.at(bi).close_time_ms;
        };
        if (ImPlot::IsPlotHovered()) {
            if (ImGui::GetIO().MouseWheel != 0 ||
                ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                candles().set_follow_live(false);
            }
            const ImPlotPoint m = ImPlot::GetPlotMousePos();
            // R key → replay from the brick under the cursor → focused view.
            if (ImGui::IsKeyPressed(ImGuiKey_R, false) &&
                !ImGui::GetIO().WantTextInput && !ctx_.replay_mgr().is_active()) {
                const int64_t t = brick_time_at(m.x);
                if (t > 0)
                    ctx_.replay_mgr().open_focused_replay_at(pair_.exchange, pair_.symbol, t);
            }
            // Right-click → context menu (replay actions) anchored at the brick time.
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                context_menu_time_ms_ = brick_time_at(m.x);
                // Renko bricks carry their source-candle epoch time, so the
                // research minute floors THAT (never the brick index m.x).
                context_menu_minute_ms_ = research_url::floor_minute_ms(context_menu_time_ms_);
                context_menu_price_   = m.y;
                // Renko has no shift-drag selection (bricks carry no time
                // axis), so the move read is cleared rather than left stale
                // from a candle-chart drag.
                context_menu_move_ = SelectedMove{};
                ImGui::OpenPopup("##RenkoCtx");
            }
            ImDrawList* dl = ImPlot::GetPlotDrawList();
            const ImVec2 pmin = ImPlot::GetPlotPos();
            const ImVec2 psz  = ImPlot::GetPlotSize();
            const ImVec2 mp   = ImGui::GetMousePos();
            const ImU32 ch = Theme::u32(Theme::Tokens::TX3, 0.45f);
            dl->AddLine(ImVec2(pmin.x, mp.y), ImVec2(pmin.x + psz.x, mp.y), ch, 1.0f);
            dl->AddLine(ImVec2(mp.x, pmin.y), ImVec2(mp.x, pmin.y + psz.y), ch, 1.0f);
            char pb[32]; fmt_.format_price(pb, sizeof(pb), m.y);
            CustomImPlot::DrawAxisValueTag(m.y, pb, Theme::u32(Theme::Tokens::TX3), true);
            // X-axis TIME tag: the brick under the cursor → its source-candle time
            // (Renko has no time axis, so map via close_time_ms). Matches the candle
            // chart's crosshair time pill; drawn on the foreground list so it shows
            // below the plot rect.
            const int64_t bt = brick_time_at(m.x);
            if (bt > 0) {
                char tl[48]{};
                if (!DisplayTimeZone::instance().format(bt, TimeZoneFormat::DateTimeSeconds,
                                                         tl, sizeof(tl))) {
                    snprintf(tl, sizeof(tl), "?");
                }
                ImDrawList* fdl = ImGui::GetForegroundDrawList();
                const ImVec2 tsz = ImGui::CalcTextSize(tl);
                constexpr float tp = 6.0f;
                const float by = pmin.y + psz.y;
                ImVec2 bmin(mp.x - tsz.x * 0.5f - tp, by + 4.0f);
                ImVec2 bmax(mp.x + tsz.x * 0.5f + tp, by + 4.0f + tsz.y + tp * 2.0f);
                bmin.x = std::max(bmin.x, pmin.x + 2.0f);
                bmax.x = std::min(bmax.x, pmin.x + psz.x - 2.0f);
                fdl->AddRectFilled(bmin, bmax, IM_COL32(45, 45, 48, 240), 2.0f);
                fdl->AddRect(bmin, bmax, IM_COL32(100, 100, 105, 255), 2.0f, 0, 1.0f);
                fdl->AddText(ImVec2((bmin.x + bmax.x - tsz.x) * 0.5f, bmin.y + tp),
                             IM_COL32(230, 230, 235, 255), tl);
            }
        }
        // Renko context menu (rendered every frame so it stays open once opened) -
        // replay actions keyed to the brick's source-candle time, plus copy price.
        // Same rows as the candle menu, and like it the menu sizes to them.
        if (Theme::begin_menu("##RenkoCtx", 280.0f)) {
            // The research reads - same placement rule as the candle menu:
            // they sit above the replay block.
            render_investigate_menu_item(pair_, context_menu_minute_ms_, context_menu_move_.snap,
                                         context_menu_move_.range_minutes);
            Theme::menu_separator();
            // Replay actions (entitlement-gated inside). No range: shift-drag
            // select is off in Renko.
            ctx_.replay_mgr().render_chart_context_menu(
                pair_.exchange, pair_.symbol, context_menu_time_ms_,
                candles().timeframe_seconds(), 0, 0);
            Theme::menu_separator();
            char pbc[32]; fmt_.format_price(pbc, sizeof(pbc), context_menu_price_);
            if (Theme::menu_item(drawing::UiIcon::Copy, "Copy price", pbc)) ImGui::SetClipboardText(pbc);
            ImGui::EndPopup();
        }

        last_visible_range_ = ImPlot::GetPlotLimits();
        x_axis_min_ = last_visible_range_.X.Min;
        x_axis_max_ = last_visible_range_.X.Max;
        ImPlot::EndPlot();
    }
    ImPlot::PopStyleVar(2);
}

// Renko brick-size config (opened by right-clicking the Renko chart-type row).
void ChartWidget::render_renko_settings_popup() {
    if (renko_gear_pos_.x > 0.0f || renko_gear_pos_.y > 0.0f)
        ImGui::SetNextWindowPos(renko_gear_pos_, ImGuiCond_Appearing);
    if (Theme::begin_popup("renko_settings")) {
        ImGui::PushFont(Theme::Fonts::ui());
        ImGui::TextUnformatted("Renko brick size");
        ImGui::PopFont();
        ImGui::Spacing();
        bool is_auto = (renko_brick_ticks_ == 0);
        if (ImGui::Checkbox("Auto (ATR-14, per-symbol)", &is_auto)) {
            if (is_auto) { renko_brick_ticks_ = 0; renko_resolved_ticks_ = 0; }
            else         { renko_brick_ticks_ = std::max(1, renko_resolved_ticks_); }
            renko_sig_size_ = -1.0;  // force a committed rebuild next frame
        }
        if (!is_auto) {
            int ticks = renko_brick_ticks_;
            ImGui::SetNextItemWidth(160);
            if (ImGui::InputInt("brick (ticks)", &ticks, 1, 10)) {
                renko_brick_ticks_ = std::max(1, ticks);
                renko_resolved_ticks_ = renko_brick_ticks_;
                renko_sig_size_ = -1.0;
            }
        }
        char pb[32];
        const double sz = static_cast<double>(renko_resolved_ticks_) * tick_size_;
        fmt_.format_price(pb, sizeof(pb), sz);
        ImGui::TextDisabled("Brick = %s  (%d ticks)", pb, renko_resolved_ticks_);
        ImGui::EndPopup();
    }
}


// Candle Drawing - reads SoA from CandleManager
// (replaces both plot_candles_simple and CustomImPlot::PlotCandlestick)

// OHLCV readout - top-left overlay, hovered candle (or latest when idle)
void ChartWidget::draw_ohlc_readout() const {
    const auto& ts = candles().timestamps();
    if (ts.empty()) return;
    const auto& opens  = candles().opens();
    const auto& highs  = candles().highs();
    const auto& lows   = candles().lows();
    const auto& closes = candles().closes();
    const auto& vols   = candles().volumes();

    // hovered candle, else most recent
    size_t idx = ts.size() - 1;
    if (ImPlot::IsPlotHovered()) {
        const double mx = ImPlot::GetPlotMousePos().x;
        const auto it = std::ranges::upper_bound(ts, mx);
        if (it != ts.begin()) {
            idx = static_cast<size_t>(std::distance(ts.cbegin(), it)) - 1;
        }
    }
    if (idx >= ts.size()) return;

    const bool up = closes[idx] >= opens[idx];
    const ImU32 val_col = up ? Theme::get_buy_color_u32(255)
                             : Theme::get_sell_color_u32(255);
    const ImU32 lbl_col = Theme::u32(Theme::Tokens::TX4);

    char num[32];
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImVec2 pos = ImPlot::GetPlotPos();
    pos.x += 10.0f;
    pos.y += 8.0f;

    ImGui::PushFont(Theme::Fonts::mono_sm());
    // Two passes: measure, back the row with the panel tone (overlay lines never run through the
    // numbers), then draw. Volume reads compactly (12.27M, not 12272619).
    float width = 0.0f;
    auto format = [&](double value, bool is_vol) {
        if (!is_vol) snprintf(num, sizeof(num), fmt_.price_fmt, value);
        else if (value >= 1e9) snprintf(num, sizeof(num), "%.2fB", value / 1e9);
        else if (value >= 1e6) snprintf(num, sizeof(num), "%.2fM", value / 1e6);
        else if (value >= 1e4) snprintf(num, sizeof(num), "%.1fK", value / 1e3);
        else snprintf(num, sizeof(num), "%.0f", value);
    };
    bool draw = false;
    auto field = [&](const char* label, double value, bool is_vol) {
        format(value, is_vol);
        const float lw = ImGui::CalcTextSize(label).x + 4.0f, nw = ImGui::CalcTextSize(num).x + 10.0f;
        if (draw) {
            dl->AddText(pos, lbl_col, label);
            dl->AddText(ImVec2(pos.x + lw, pos.y), val_col, num);
            pos.x += lw + nw;
        } else {
            width += lw + nw;
        }
    };
    auto fields = [&] {
        if (chart_type_ == ChartType::Line) {
            // Line mode shows Close only (the real close - the line IS the close).
            field("C", closes[idx], false);
        } else {
            field("O", opens[idx], false);
            field("H", highs[idx], false);
            field("L", lows[idx], false);
            field("C", closes[idx], false);
            field("V", vols[idx], true);
        }
    };
    fields();
    const float th = ImGui::GetTextLineHeight();
    dl->AddRectFilled(ImVec2(pos.x - 5.0f, pos.y - 3.0f), ImVec2(pos.x + width - 5.0f, pos.y + th + 3.0f),
                      Theme::u32(Theme::Tokens::BASE, 0.82f));
    Theme::chart_chip(ImVec2(pos.x - 5.0f, pos.y - 3.0f), ImVec2(pos.x + width - 5.0f, pos.y + th + 3.0f));
    draw = true;
    fields();
    ImGui::PopFont();
}

// Replay status, drawn just under the OHLC readout (top-left of plot): one of the chart chips (the rails key and the
// cascade chip), a rounded chip in Inter at the mono figures' size with a dot, the pair, the clock and the speed.
void ChartWidget::draw_status_chip() const {
    using namespace Theme;
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const ImVec2 plot = ImPlot::GetPlotPos();
    const bool replay = ctx_.replay_mgr().is_active();
    char clk[16], text[128];
    if (replay) {
        DisplayTimeZone::instance().format(ctx_.replay_mgr().interpolated_time_ms(), TimeZoneFormat::TimeSeconds, clk, sizeof(clk));
        char spd[16];
        ReplayManager::format_speed(ctx_.replay_mgr().current_speed(), spd, sizeof(spd));
        snprintf(text, sizeof(text), "%s \xc2\xb7 %s \xc2\xb7 %s", widget_symbol_label(pair_.symbol).c_str(), clk, spd);
    } else {
        DisplayTimeZone::instance().format(static_cast<int64_t>(time(nullptr)) * 1000, TimeZoneFormat::TimeSeconds, clk, sizeof(clk));
        snprintf(text, sizeof(text), "%s \xc2\xb7 %s", widget_symbol_label(pair_.symbol).c_str(), clk);
    }
    const char* label = replay ? "Replay" : "Live";
    ImGui::PushFont(Fonts::mono_sm());
    const float px = ImGui::GetFontSize();
    ImGui::PopFont();
    push_font(Fonts::ui(), px);
    const ImVec2 ls = ImGui::CalcTextSize(label), ts = ImGui::CalcTextSize(text);
    constexpr float pad = 9.0f, h = 22.0f, dotr = 3.0f, gap = 7.0f;
    const ImVec2 a(plot.x + 10.0f, plot.y + 30.0f);
    const ImVec2 b(std::round(a.x + pad + dotr * 2.0f + gap + ls.x + gap + ts.x + pad), a.y + h);
    dl->AddRectFilled(a, b, u32(Tokens::PANEL, 0.92f), Radius::R2);
    dl->AddRect(a, b, u32(Tokens::BD2), Radius::R2, 0, 1.0f);
    const float cy = a.y + h * 0.5f;
    float x = a.x + pad + dotr;
    dl->AddCircleFilled(ImVec2(x, cy), dotr, u32(replay ? Tokens::WARN : Tokens::UP), 12);
    x += dotr + gap;
    dl->AddText(ImVec2(x, std::round(cy - ls.y * 0.5f)), u32(Tokens::TX1), label);
    x += ls.x + gap;
    dl->AddText(ImVec2(x, std::round(cy - ts.y * 0.5f)), u32(Tokens::TX2), text);
    ImGui::PopFont();
    chart_chip(a, b);
}

void ChartWidget::plot_candles(double visible_x_min, double visible_x_max,
                               const std::vector<double>& src_opens,
                               const std::vector<double>& src_highs,
                               const std::vector<double>& src_lows,
                               const std::vector<double>& src_closes,
                               const BuildingOHLC& bld) {
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    const int64_t tf_sec = candles().timeframe_seconds();
    const double timeframe_ms = chart_type_ == ChartType::FlowPositioning ? 60000.0 : static_cast<double>(tf_sec) * 1000.0;
    const double half_width = timeframe_ms * 0.35;  // ~70% body, ~30% gap (was 0.49 = no gap)
    const bool fp_thin = ctx_.footprint_mgr().enabled;  // Thin candle body when FP active
    const ImVec2 plot_min = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    const auto plot_max = ImVec2(plot_min.x + plot_size.x, plot_min.y + plot_size.y);

    // Three rendering tiers based on zoom level:
    //   Full candle mode: >=5px per candle (body + wick, normal width)
    //   Thin candle mode: 2-5px per candle (1px wick + thin body, preserves H/L)
    //   Bar mode: <2px per candle (vertical H/L line per pixel column, OHLC style)
    const double visible_candles = (visible_x_max - visible_x_min) / timeframe_ms;
    const double pixels_per_candle = plot_size.x / visible_candles;
    enum class CandleRenderMode { Full, Thin, Bar };
    const CandleRenderMode mode = (pixels_per_candle >= 5.0) ? CandleRenderMode::Full
                                : (pixels_per_candle >= 2.0) ? CandleRenderMode::Thin
                                :                              CandleRenderMode::Bar;

    ImPlot::PushPlotClipRect();
    // X axis is always the REAL timestamps; OHLC comes from the passed source
    // spans (real arrays for Candles/FP, derived HA arrays for Heikin Ashi).
    const auto& timestamps = candles().timestamps();
    const auto& opens = src_opens;
    const auto& closes = src_closes;
    const auto& lows = src_lows;
    const auto& highs = src_highs;
    // §3C - prefer the live building candle over any finalized candle sharing its
    // timestamp. A transient finalized+building coexistence (e.g. the wall-clock
    // finalizer racing a late server partial on 5m/15m) otherwise leaves the STALE
    // finalized candle drawn while the live price tag (= building close) floats above
    // it. Skip the stale finalized one in the loops below and ALWAYS draw the building
    // candle, so the rendered live candle's close sits on the price tag.
    const double building_x = candles().has_building_candle()
        ? static_cast<double>(candles().building_candle().timestamp_ms) : -1.0;
    // Safety: OHLC spans must be parallel to the timestamps (HA cache is built
    // from the same deque, so this holds; guard against a transient mismatch).
    const size_t n_draw = std::min({timestamps.size(), opens.size(),
                                    highs.size(), lows.size(), closes.size()});
    // Binary search for first visible candle
    size_t start_idx = 0;
    if (!timestamps.empty()) {
        const auto it = std::ranges::lower_bound(timestamps, visible_x_min);
        if (it != timestamps.begin()) {
            start_idx = static_cast<size_t>(std::distance(timestamps.cbegin(), it)) - 1;
        }
    }

    if (mode == CandleRenderMode::Bar) {
        // Bar mode: aggregate candles that share the same pixel column.
        // Draw a vertical line from min(low) to max(high), colored by net direction.
        int last_px_col = -1;
        double col_high = -1e18, col_low = 1e18;
        double col_open = 0, col_close = 0;
        bool col_started = false;
        for (size_t i = start_idx; i < n_draw; i++) {
            if (timestamps[i] > visible_x_max) break;
            if (timestamps[i] == building_x) continue;  // live building candle drawn below
            const ImVec2 px = ImPlot::PlotToPixels(timestamps[i], closes[i]);
            const int px_col = static_cast<int>(px.x);
            if (px_col != last_px_col && col_started) {
                // Flush previous pixel column
                const ImU32 col = (col_close >= col_open)
                    ? Theme::get_buy_color_u32(255)
                    : Theme::get_sell_color_u32(255);
                const ImVec2 top = ImPlot::PlotToPixels(0, col_high);
                const ImVec2 bot = ImPlot::PlotToPixels(0, col_low);
                draw_list->AddLine(
                    ImVec2(static_cast<float>(last_px_col), top.y),
                    ImVec2(static_cast<float>(last_px_col), bot.y),
                    col, 1.0f);
                col_high = -1e18; col_low = 1e18;
                col_started = false;
            }
            if (!col_started) {
                col_open = opens[i];
                col_started = true;
            }
            col_high = std::max(col_high, highs[i]);
            col_low = std::min(col_low, lows[i]);
            col_close = closes[i];
            last_px_col = px_col;
        }
        // Flush final column
        if (col_started) {
            const ImU32 col = (col_close >= col_open)
                ? Theme::get_buy_color_u32(255)
                : Theme::get_sell_color_u32(255);
            const ImVec2 top = ImPlot::PlotToPixels(0, col_high);
            const ImVec2 bot = ImPlot::PlotToPixels(0, col_low);
            draw_list->AddLine(
                ImVec2(static_cast<float>(last_px_col), top.y),
                ImVec2(static_cast<float>(last_px_col), bot.y),
                col, 1.0f);
        }
    } else {
        // Full or Thin candle mode
        const bool force_thin = (mode == CandleRenderMode::Thin) || fp_thin;
        for (size_t i = start_idx; i < n_draw; i++) {
            if (timestamps[i] > visible_x_max) break;
            if (timestamps[i] == building_x) continue;  // live building candle drawn below
            draw_single_candle(draw_list,
                               timestamps[i], opens[i], closes[i], lows[i], highs[i],
                               half_width, plot_min, plot_max, force_thin);
        }
    }
    // Draw building candle. Any finalized candle sharing this timestamp was skipped in
    // the loops above (see building_x), so we ALWAYS draw the live building candle here -
    // it owns the right edge and its close sits on the price tag. (Previously this was
    // skipped whenever a finalized candle shared the ts, which left the STALE finalized
    // candle showing while the live price floated above it - the 5m/15m "price not
    // pinned / building candle invisible" bug.)
    if (bld.valid && building_x >= 0.0) {
        const double live_x = building_x;
        if (live_x >= visible_x_min && live_x <= visible_x_max) {
            if (mode == CandleRenderMode::Bar) {
                // Draw as vertical line matching bar mode
                const ImU32 col = (bld.close >= bld.open)
                    ? Theme::get_buy_color_u32(255)
                    : Theme::get_sell_color_u32(255);
                const ImVec2 top = ImPlot::PlotToPixels(live_x, bld.high);
                const ImVec2 bot = ImPlot::PlotToPixels(live_x, bld.low);
                draw_list->AddLine(top, bot, col, 1.0f);
            } else {
                const bool force_thin = (mode == CandleRenderMode::Thin) || fp_thin;
                draw_single_candle(draw_list,
                                   live_x, bld.open, bld.close, bld.low, bld.high,
                                   half_width, plot_min, plot_max, force_thin);
            }
        }
    }
    ImPlot::PopPlotClipRect();
}

// Line chart (ChartType::Line) - a close-price polyline vs time. Real-time
// resolution (§5.1): the recent-tick ring buffer (CandleManager::tick_times /
// tick_prices) supplies points across the window it covers, so the line moves
// tick-by-tick; for the bars OLDER than that window we fall back to finalized
// candle closes. A candle bar is drawn CENTRED on its period's open time
// (draw_single_candle spans timestamps[i] +/- 0.35 tf) and its close vertex
// sits there too, while the period's trades run from open to open + tf, so
// ticks are shifted back by half a period - the candle-bubble convention
// (render_candle_bubbles) - and each period's ticks land inside its own bar.
// The newest tick is the live point; the building candle's close stands in
// only until its first trade arrives (a close at the period's OPEN time after
// newer ticks sent the stroke back in time and drew an arrowhead at the live
// end). When points-per-pixel is high, the stream is decimated per pixel
// column to first / min / max / last, so a fast market stays a clean, cheap
// line that still ends on the newest tick. Stroked with the up/accent token.
void ChartWidget::plot_line(double visible_x_min, double visible_x_max) {
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    const auto& timestamps = candles().timestamps();
    const auto& closes     = candles().closes();
    const auto& tick_times  = candles().tick_times();
    const auto& tick_prices = candles().tick_prices();
    const double shift_ms = static_cast<double>(candles().timeframe_seconds()) * 500.0;

    // Oldest retained tick in bar coordinates: candle closes cover only the bars
    // strictly left of it, so the two sources never double-plot a region and the
    // seam stays monotone in X (the bar holding that tick is tick-owned).
    const bool have_ticks = !tick_times.empty();
    const double tick_floor_x = have_ticks
        ? static_cast<double>(tick_times.front()) - shift_ms
        : std::numeric_limits<double>::max();

    // Per-pixel-column decimation: as we walk points left→right we keep one
    // column open at a time and emit its first point, its two extremes in the
    // order they occurred, and its last point. The stroke enters and leaves
    // every column where the data did, so the live end IS the newest tick.
    line_pts_.clear();
    int    cur_col = std::numeric_limits<int>::min();
    ImVec2 col_first{}, col_last{}, col_min{}, col_max{};
    int    col_seq = 0, col_min_seq = 0, col_max_seq = 0;
    bool   col_open = false;

    auto flush_col = [&]() {
        if (!col_open) return;
        const size_t base = line_pts_.size();
        auto push = [&](const ImVec2& p) {
            // The same pixel row twice in one column adds nothing to the stroke.
            if (line_pts_.size() > base && line_pts_.back().y == p.y) return;
            line_pts_.push_back(p);
        };
        push(col_first);
        if (col_min_seq <= col_max_seq) { push(col_min); push(col_max); }
        else                            { push(col_max); push(col_min); }
        push(col_last);
        col_open = false;
    };
    auto add_point = [&](double x_ms, double price) {
        const ImVec2 px = ImPlot::PlotToPixels(x_ms, price);
        const int col = static_cast<int>(px.x);
        if (col != cur_col) {
            flush_col();
            cur_col   = col;
            col_first = col_last = col_min = col_max = px;
            col_seq   = col_min_seq = col_max_seq = 0;
            col_open  = true;
        } else {
            ++col_seq;
            col_last = px;
            if (px.y < col_min.y) { col_min = px; col_min_seq = col_seq; }  // lower pixel y = higher price
            if (px.y > col_max.y) { col_max = px; col_max_seq = col_seq; }
        }
    };
    // One point past each edge of the window so the stroke enters and leaves
    // off-plot instead of starting inside it; PushPlotClipRect clips the rest.

    // 1) Candle closes, at the bar centre, for the bars left of the tick region.
    if (!timestamps.empty()) {
        const auto it = std::ranges::lower_bound(timestamps, visible_x_min);
        size_t i0 = static_cast<size_t>(std::distance(timestamps.cbegin(), it));
        if (i0 > 0) --i0;
        const size_t n = std::min(timestamps.size(), closes.size());
        for (size_t i = i0; i < n; i++) {
            if (timestamps[i] >= tick_floor_x) break;  // ticks own this bar
            add_point(timestamps[i], closes[i]);
            if (timestamps[i] > visible_x_max) break;
        }
    }

    // 2) Ticks, shifted into their bars (newest, high-resolution region).
    if (have_ticks) {
        const auto it = std::lower_bound(tick_times.begin(), tick_times.end(),
                                         static_cast<int64_t>(visible_x_min + shift_ms));
        size_t j0 = static_cast<size_t>(std::distance(tick_times.begin(), it));
        if (j0 > 0) --j0;
        const size_t n = std::min(tick_times.size(), tick_prices.size());
        for (size_t j = j0; j < n; j++) {
            const double x = static_cast<double>(tick_times[j]) - shift_ms;
            add_point(x, tick_prices[j]);
            if (x > visible_x_max) break;
        }
    }

    // 3) Building candle close, at its bar centre, only while no tick has
    //    reached its period (fresh load, or server candles without trades).
    //    Once one has, the newest tick already is the live point.
    if (candles().has_building_candle()) {
        const auto& bc = candles().building_candle();
        if (!have_ticks || tick_times.back() < bc.timestamp_ms)
            add_point(static_cast<double>(bc.timestamp_ms), bc.close);
    }

    flush_col();

    if (line_pts_.size() >= 2) {
        ImPlot::PushPlotClipRect();
        draw_list->AddPolyline(line_pts_.data(), static_cast<int>(line_pts_.size()),
                               Theme::u32(Theme::Tokens::UP), 0, 1.5f);
        ImPlot::PopPlotClipRect();
    }
}

// Heikin Ashi visible extents (Phase 2): min HA low / max HA high over the
// visible window (plus the per-frame building HA candle), so the Y-limits frame
// the drawn HA candles and nothing clips. Leaves out params untouched when the
// HA cache has no data in range (caller keeps its real-candle extents then).
void ChartWidget::ha_visible_extents(double visible_x_min, double visible_x_max,
                                     double& y_min, double& y_max) const {
    const auto& ts      = candles().timestamps();
    const auto& ha_low  = candles().ha_lows();
    const auto& ha_high = candles().ha_highs();
    if (ts.empty()) return;
    const size_t n = std::min({ts.size(), ha_low.size(), ha_high.size()});
    const auto lo_it = std::lower_bound(ts.begin(), ts.end(), visible_x_min);
    const auto hi_it = std::upper_bound(lo_it, ts.end(), visible_x_max);
    size_t i0 = static_cast<size_t>(lo_it - ts.begin());
    size_t i1 = std::min(n, static_cast<size_t>(hi_it - ts.begin()));
    for (size_t i = i0; i < i1; i++) {
        y_min = std::min(y_min, ha_low[i]);
        y_max = std::max(y_max, ha_high[i]);
    }
    // Building HA candle (computed per frame in render_chart; recompute the pair
    // here from the last finalized HA values to include it in the frame).
    if (candles().has_building_candle() && n > 0) {
        const auto& bc = candles().building_candle();
        if (bc.timestamp_ms >= visible_x_min && bc.timestamp_ms <= visible_x_max) {
            const auto& ha_open  = candles().ha_opens();
            const auto& ha_close = candles().ha_closes();
            const size_t last = n - 1;
            const double prev_o = ha_open[last];
            const double prev_c = ha_close[last];
            const double hb_close = (bc.open + bc.high + bc.low + bc.close) * 0.25;
            const double hb_open  = (prev_o + prev_c) * 0.5;
            const double hb_high  = std::max({bc.high, hb_open, hb_close});
            const double hb_low   = std::min({bc.low,  hb_open, hb_close});
            y_min = std::min(y_min, hb_low);
            y_max = std::max(y_max, hb_high);
        }
    }
}

void ChartWidget::draw_single_candle(
    ImDrawList* draw_list,
    const double x, const double open, const double close,
    const double low, const double high,
    const double half_width, const ImVec2& plot_min, const ImVec2& plot_max,
    bool thin_body, uint8_t alpha)
{
    const bool bullish = close >= open;
    const ImU32 body_color = bullish
        ? Theme::get_buy_color_u32(alpha)
        : Theme::get_sell_color_u32(alpha);
    const ImU32 wick_color = body_color;

    const ImVec2 high_px = ImPlot::PlotToPixels(x, high);
    const ImVec2 low_px = ImPlot::PlotToPixels(x, low);
    const ImVec2 open_px = ImPlot::PlotToPixels(x, open);
    const ImVec2 close_px = ImPlot::PlotToPixels(x, close);

    // When footprint is active, use a thin candle body (~30% width)
    const double effective_half = thin_body ? half_width * 0.15 : half_width;
    const ImVec2 left_px = ImPlot::PlotToPixels(x - effective_half, 0);
    const ImVec2 right_px = ImPlot::PlotToPixels(x + effective_half, 0);

    float body_top = std::min(open_px.y, close_px.y);
    float body_bottom = std::max(open_px.y, close_px.y);

    // Minimum height for doji/flat candles
    if (body_bottom - body_top < 2.0f) {
        float center_y = (body_top + body_bottom) * 0.5f;
        body_top = center_y - 1.0f;
        body_bottom = center_y + 1.0f;
    }
    // Cull off-screen candles
    if (right_px.x < plot_min.x || left_px.x > plot_max.x) return;
    // Wick first (behind body)
    draw_list->AddLine(high_px, low_px, wick_color, 1.0f);
    // Body on top
    draw_list->AddRectFilled(
        ImVec2(left_px.x, body_top),
        ImVec2(right_px.x, body_bottom),
        body_color
    );
}


// Controls / Toolbar

void ChartWidget::render_controls() {
    // v2 chart toolbar (Component 3a): one 44px bar directly above the chart.
    // Left to right: chart-type icon button (single-select menu), layers icon
    // button (multi-select checklist + active-count badge), a vertical divider,
    // then a right-aligned Indicators + Settings group. Timeframes arrive in 3b.
    // Accent teal is the only on-state hue; controls are bg-1 with line-2 borders
    // (accent border on hover/open). Numbers use JetBrains Mono, chrome uses Hanken.
    // The two style pushes below are kept only to balance the trailing
    // PopStyleVar(2); the bar itself is drawn via the window draw list.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 4));

    using namespace Theme;

    // Bar geometry: a 44px band (8 top + 28 control + 8 bottom), bg-1 fill, with a
    // line-1 hairline along the bottom (drawn last) against the chart canvas.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    const float  ww = ImGui::GetContentRegionAvail().x;
    float bar_h = 36.0f;
    float ctrl_y = bp.y + (bar_h - 28.0f) * 0.5f;    // centre 28px controls in the band
    dl->AddRectFilled(bp, ImVec2(bp.x + ww, bp.y + bar_h), Theme::u32(Theme::Tokens::PANEL));

    // Toolbar draw helpers ------------------------------------------------------
    // Small down-caret (a filled triangle), centred on (cx, cy).
    auto draw_caret = [](ImDrawList* d, float cx, float cy, ImU32 col) {
        d->AddTriangleFilled(ImVec2(cx - 3.0f, cy - 1.5f), ImVec2(cx + 3.0f, cy - 1.5f),
                             ImVec2(cx, cy + 2.5f), col);
    };
    // Chart-type glyph: 1:1 port of the design SVGs into a 14x14 box at (ox, oy).
    // chart_type_ index: 0 Candles, 1 FP Cluster, 2 FP Profile, 3 Heikin, 4 Line, 5 TPO.
    auto draw_ctype_glyph = [](ImDrawList* d, int t, float ox, float oy, ImU32 col) {
        const float sw = 1.2f;
        switch (t) {
            case 0:  // Candles: filled body + wick, hollow body + wick
                d->AddLine(ImVec2(ox + 4.0f, oy + 1.5f), ImVec2(ox + 4.0f, oy + 12.5f), col, sw);
                d->AddRectFilled(ImVec2(ox + 2.3f, oy + 4.0f), ImVec2(ox + 5.7f, oy + 10.0f), col);
                d->AddLine(ImVec2(ox + 10.0f, oy + 2.5f), ImVec2(ox + 10.0f, oy + 11.5f), col, sw);
                d->AddRect(ImVec2(ox + 8.3f, oy + 5.5f), ImVec2(ox + 11.7f, oy + 9.5f), col, 0.0f, 0, 1.1f);
                break;
            case 3:  // Heikin Ashi: single centred wick + wide body
                d->AddLine(ImVec2(ox + 7.0f, oy + 1.5f), ImVec2(ox + 7.0f, oy + 12.5f), col, sw);
                d->AddRectFilled(ImVec2(ox + 4.3f, oy + 4.0f), ImVec2(ox + 9.7f, oy + 10.0f), col);
                break;
            case 4: {  // Line: zig-zag polyline
                const ImVec2 pts[5] = {
                    ImVec2(ox + 1.0f, oy + 10.0f), ImVec2(ox + 4.5f, oy + 6.0f),
                    ImVec2(ox + 7.0f, oy + 8.5f),  ImVec2(ox + 10.0f, oy + 3.5f),
                    ImVec2(ox + 13.0f, oy + 5.5f) };
                d->AddPolyline(pts, 5, col, 0, 1.4f);
                break;
            }
            case 1:  // Footprint Cluster: outlined 2x3 grid of cells
                d->AddRect(ImVec2(ox + 1.5f, oy + 2.0f), ImVec2(ox + 12.5f, oy + 12.0f), col, 0.0f, 0, 1.0f);
                d->AddLine(ImVec2(ox + 7.0f, oy + 2.0f), ImVec2(ox + 7.0f, oy + 12.0f), col, 1.0f);
                d->AddLine(ImVec2(ox + 1.5f, oy + 5.3f), ImVec2(ox + 12.5f, oy + 5.3f), col, 1.0f);
                d->AddLine(ImVec2(ox + 1.5f, oy + 8.7f), ImVec2(ox + 12.5f, oy + 8.7f), col, 1.0f);
                break;
            case 7:  // Flow & Positioning: aligned evidence lanes
            case 2:  // Footprint Profile: left-anchored horizontal histogram
                d->AddRectFilled(ImVec2(ox + 2.0f, oy + 2.5f),  ImVec2(ox + 10.0f, oy + 4.0f),  col);
                d->AddRectFilled(ImVec2(ox + 2.0f, oy + 5.25f), ImVec2(ox + 8.0f,  oy + 6.75f), col);
                d->AddRectFilled(ImVec2(ox + 2.0f, oy + 8.0f),  ImVec2(ox + 12.0f, oy + 9.5f),  col);
                d->AddRectFilled(ImVec2(ox + 2.0f, oy + 10.75f),ImVec2(ox + 6.5f,  oy + 12.25f),col);
                break;
            case 5:  // TPO: market-profile bell of stacked columns
                d->AddRectFilled(ImVec2(ox + 2.0f,  oy + 6.0f), ImVec2(ox + 4.0f,  oy + 8.0f),  col);
                d->AddRectFilled(ImVec2(ox + 4.5f,  oy + 4.0f), ImVec2(ox + 6.5f,  oy + 10.0f), col);
                d->AddRectFilled(ImVec2(ox + 7.0f,  oy + 2.5f), ImVec2(ox + 9.0f,  oy + 11.5f), col);
                d->AddRectFilled(ImVec2(ox + 9.5f,  oy + 5.0f), ImVec2(ox + 11.5f, oy + 9.0f),  col);
                break;
            case 6:  // Renko: stepped bricks (staircase - filled, hollow, filled)
                d->AddRectFilled(ImVec2(ox + 1.0f,  oy + 8.0f),  ImVec2(ox + 5.0f,  oy + 11.5f), col);
                d->AddRect(ImVec2(ox + 5.5f,  oy + 4.5f), ImVec2(ox + 9.5f,  oy + 8.0f), col, 0.0f, 0, 1.1f);
                d->AddRectFilled(ImVec2(ox + 10.0f, oy + 1.0f),  ImVec2(ox + 14.0f, oy + 4.5f),  col);
                break;
        }
    };
    // Layers stack glyph: three stacked diamonds in a 15x15 box at (ox, oy).
    auto draw_layers_glyph = [](ImDrawList* d, float ox, float oy, ImU32 col) {
        const float k = 15.0f / 16.0f;
        auto P = [&](float x, float y) { return ImVec2(ox + x * k, oy + y * k); };
        const ImVec2 top[4] = { P(8, 2), P(14.5f, 5), P(8, 8), P(1.5f, 5) };
        d->AddPolyline(top, 4, col, ImDrawFlags_Closed, 1.2f);
        const ImVec2 mid[3] = { P(1.5f, 8), P(8, 11), P(14.5f, 8) };
        d->AddPolyline(mid, 3, col, 0, 1.2f);
        const ImVec2 bot[3] = { P(1.5f, 11), P(8, 14), P(14.5f, 11) };
        d->AddPolyline(bot, 3, col, 0, 1.2f);
    };

    // Layer on-count drives the badge and the layers-menu header. Real-time
    // mode counts its own layer set (the menu lists the same set).
    const bool absorption_supported = liquidity_response::supported(pair_.exchange, pair_.symbol);
    const int layers_on = (activity_visible() ? 1 : 0) + (Entitlements::is_admin() && pair_.exchange == "binancef" ? int(alert_lines_) + int(alert_markers_) : 0) + (!rt_mode_ && absorption_supported && liquidity_response_enabled_ && !absorption_locked() ? 1 : 0) + (refill_history_active() ? 1 : 0) + (rt_mode_
        ? (rt_depth_enabled_ ? 1 : 0) + (rt_bubbles_ ? 1 : 0) + (rt_trade_line_ ? 1 : 0) + (refill_active() ? 1 : 0)
            + (rt_candles_ ? 1 : 0) + (reported_liquidations_enabled() ? 1 : 0)
            + realtime_extra_layers_on()
        : (session_vwap_ ? 1 : 0) + (previous_day_ ? 1 : 0) +
        (previous_week_ ? 1 : 0) + (vwap_anchor_ms_ ? 1 : 0)
                        + (liqmap2_shown() || scenario_map_shown() ? 1 : 0)
                        + (candle_bubbles_ ? 1 : 0) + (reported_liquidations_enabled() ? 1 : 0)
                        + (liq_census_enabled_ ? 1 : 0)
                        + (touch_odds_enabled_ && touch_odds_overlay::available(pair_) ? 1 : 0)
                        + (heatmap_enabled_ ? 1 : 0) + (vpvr_enabled_ ? 1 : 0));

    const int liquidations_on = rt_mode_ ? int(reported_liquidations_enabled())
        : int(liqmap2_shown() || scenario_map_shown()) + int(reported_liquidations_enabled() && Entitlements::is_pro())
          + int(liq_census_enabled_ && hl_rails_allowed() && !hl_coin_missing());
    const int chart_layers_on = std::max(0, layers_on - (rt_mode_ ? int(reported_liquidations_enabled())
        : int(liqmap2_shown() || scenario_map_shown()) + int(reported_liquidations_enabled()) + int(liq_census_enabled_)));

    auto chart_type_label = [](ChartType type) -> const char* {
        switch (type) {
            case ChartType::Candles:          return "Candles";
            case ChartType::HeikinAshi:       return "Heikin Ashi";
            case ChartType::Line:             return "Line";
            case ChartType::FootprintCluster: return "Footprint cluster";
            case ChartType::FootprintProfile: return "Footprint profile";
            case ChartType::TPO:              return "TPO";
            case ChartType::Renko:            return "Renko";
            case ChartType::FlowPositioning:  return "Flow & Positioning";
        }
        return "Chart";
    };

    // The full labels are deliberately visible at normal terminal widths. The
    // previous icon-only controls hid seven working chart views behind an
    // unlabeled candlestick glyph. Compact docks keep the old glyph-only shape.
    const bool show_control_labels = ww >= 980.0f;
    const bool compact_tools = ww < 1100.0f;

    // Left group: chart-view button, layers button, divider ---------------------
    const float caret_w = 9.0f;
    float cx = bp.x + 12.0f;   // left gutter (bar padding 0 12)
    ImVec2 ct_anchor, liq_anchor, ly_anchor, widget_anchor;
    const auto wrap_control = [&](float width) {
        if (cx + width <= bp.x + ww - 12.0f || cx == bp.x + 12.0f) return;
        cx = bp.x + 12.0f;
        ctrl_y += 36.0f;
        bar_h += 36.0f;
        dl->AddRectFilled(ImVec2(bp.x, bp.y + bar_h - 36.0f),
                          ImVec2(bp.x + ww, bp.y + bar_h), Theme::u32(Theme::Tokens::PANEL));
    };

    // Timeframe segment (favourites bar + caret -> grouped dropdown), reusing the
    // shell control so the bare terminal matches the /terminal?event= chrome. Sits
    // left of the chart-type button; render_chart_tf draws it + returns its width.
    {
        ImGui::SetCursorScreenPos(ImVec2(cx, bp.y + (bar_h - 30.0f) * 0.5f));
        cx += AppShell::render_chart_tf(this) + 12.0f;
    }

    // Vertical divider between the timeframe group and the chart-type/layers group.
    {
        const float dy = bp.y + (bar_h - 22.0f) * 0.5f;
        dl->AddLine(ImVec2(cx, dy), ImVec2(cx, dy + 22.0f), Theme::u32(Theme::Tokens::BD1), 1.0f);
        cx += 1.0f + 12.0f;
    }

    // Chart-view button (active glyph + current view name + caret).
    {
        const char* active_label = chart_type_label(chart_type_);
        ImGui::PushFont(Theme::Fonts::ui());
        const float label_w = show_control_labels ? ImGui::CalcTextSize(active_label).x : 0.0f;
        ImGui::PopFont();
        const float label_span = show_control_labels ? (8.0f + label_w) : 0.0f;
        const float w = 9.0f + 14.0f + label_span + 6.0f + caret_w + 9.0f;
        wrap_control(w);
        ImGui::SetCursorScreenPos(ImVec2(cx, ctrl_y));
        const bool clicked = ImGui::InvisibleButton("##ct_btn", ImVec2(w, 28.0f));
        const bool open = ImGui::IsPopupOpen("chart_type_popup");
        const bool hot  = ImGui::IsItemHovered() || open;
        if (hot) dl->AddRectFilled(ImVec2(cx, ctrl_y), ImVec2(cx + w, ctrl_y + 28.0f),
                    Theme::u32(open ? Theme::Tokens::CONTROL_HOVER : Theme::Tokens::CONTROL), Theme::Radius::R2);
        draw_ctype_glyph(dl, static_cast<int>(chart_type_), cx + 9.0f, ctrl_y + 7.0f,
                         Theme::u32(hot ? Theme::Tokens::BRAND_TX : Theme::Tokens::TX2));
        float caret_x = cx + 9.0f + 14.0f;
        if (show_control_labels) {
            ImGui::PushFont(Theme::Fonts::ui());
            dl->AddText(ImVec2(caret_x + 8.0f, ctrl_y + (28.0f - ImGui::GetFontSize()) * 0.5f),
                        Theme::u32(hot ? Theme::Tokens::BRAND_TX : Theme::Tokens::TX2),
                        active_label);
            ImGui::PopFont();
            caret_x += 8.0f + label_w;
        }
        draw_caret(dl, caret_x + 6.0f + caret_w * 0.5f, ctrl_y + 14.0f,
                   Theme::u32(Theme::Tokens::TX3));
        if (ImGui::IsItemHovered()) Theme::tooltip("Chart view: %s", active_label);
        if (clicked && !open) ImGui::OpenPopup("chart_type_popup");
        ct_anchor = ImVec2(cx, ctrl_y + 28.0f);
        cx += w + 12.0f;
    }

    // Keep the paired ladder discoverable after its close button hides it.
    if (rt_mode_) {
        ImGui::PushFont(Theme::Fonts::ui());
        const float w = ImGui::CalcTextSize("DOM").x + 20.0f;
        wrap_control(w);
        ImGui::SetCursorScreenPos(ImVec2(cx, ctrl_y));
        if (Theme::choice_button("DOM##rt_dom", rt_dom_shown_, ImVec2(w, 28.0f)))
            rt_dom_shown_ = !rt_dom_shown_;
        ui_test::mark("rt.dom.toolbar");
        if (ImGui::IsItemHovered())
            Theme::tooltip(rt_dom_shown_ ? "Hide the paired depth ladder" : "Show the paired depth ladder");
        ImGui::PopFont();
        cx += w + 12.0f;
    }

    // Layers button (stack glyph + plain-language label + count + caret).
    const auto overlay_button = [&](const char* label, const char* popup, const char* test_id, int count, ImVec2& anchor, std::string& detail) {
        char cb[8]; snprintf(cb, sizeof(cb), "%d", count);
        ImGui::PushFont(Theme::Fonts::mono_sm());
        const float num_w = ImGui::CalcTextSize(cb).x;
        ImGui::PopFont();
        ImGui::PushFont(Theme::Fonts::ui());
        const float layers_label_w = ImGui::CalcTextSize(label).x;
        ImGui::PopFont();
        const float layers_label_span = 8.0f + layers_label_w;
        const float chip_w = num_w + 10.0f;                          // badge chip (padding ~1 5)
        const float badge_span = (count > 0) ? (chip_w + 6.0f) : 0.0f;
        const float w = 9.0f + 15.0f + layers_label_span + 6.0f + badge_span + caret_w + 9.0f;
        wrap_control(w);
        ImGui::SetCursorScreenPos(ImVec2(cx, ctrl_y));
        const bool clicked = ImGui::InvisibleButton(test_id, ImVec2(w, 28.0f));
        ui_test::mark(test_id);
        const bool open = ImGui::IsPopupOpen(popup);
        const bool hot  = ImGui::IsItemHovered() || open;
        if (hot) dl->AddRectFilled(ImVec2(cx, ctrl_y), ImVec2(cx + w, ctrl_y + 28.0f),
                    Theme::u32(open ? Theme::Tokens::CONTROL_HOVER : Theme::Tokens::CONTROL), Theme::Radius::R2);
        draw_layers_glyph(dl, cx + 9.0f, ctrl_y + 6.5f,
                          Theme::u32(hot ? Theme::Tokens::BRAND_TX : Theme::Tokens::TX2));
        float bx = cx + 9.0f + 15.0f + 6.0f;
        {
            ImGui::PushFont(Theme::Fonts::ui());
            dl->AddText(ImVec2(bx, ctrl_y + (28.0f - ImGui::GetFontSize()) * 0.5f),
                        Theme::u32(hot ? Theme::Tokens::BRAND_TX : Theme::Tokens::TX2),
                        label);
            ImGui::PopFont();
            bx += layers_label_w + 8.0f;
        }
        if (count > 0) {
            // The count as a soft chip, as on Indicators.
            const float chip_h = 15.0f, chip_y = ctrl_y + (28.0f - chip_h) * 0.5f;
            dl->AddRectFilled(ImVec2(bx, chip_y), ImVec2(bx + chip_w, chip_y + chip_h),
                              Theme::u32(Theme::Tokens::CONTROL_ACTIVE), Theme::Radius::R1);
            ImGui::PushFont(Theme::Fonts::mono_sm());
            dl->AddText(ImVec2(bx + (chip_w - num_w) * 0.5f, chip_y + (chip_h - ImGui::GetFontSize()) * 0.5f),
                        Theme::u32(Theme::Tokens::TX1), cb);
            ImGui::PopFont();
            bx += chip_w + 6.0f;
        }
        draw_caret(dl, bx + caret_w * 0.5f, ctrl_y + 14.0f, Theme::u32(Theme::Tokens::TX3));
        if (ImGui::IsItemHovered()) Theme::tooltip("%d chart layer%s active",
                                                       count, count == 1 ? "" : "s");
        if (clicked && !open) { detail.clear(); ImGui::OpenPopup(popup); }
        anchor = ImVec2(cx, ctrl_y + 28.0f);
        cx += w + 12.0f;
    };
    overlay_button("Liquidations", "liquidations_popup", "chart.liquidations", liquidations_on, liq_anchor, liquidation_menu_detail_);
    overlay_button("Layers", "layers_popup", "chart.layers", chart_layers_on, ly_anchor, layers_menu_detail_);

    // "+ widget" button (beside the layers control). This is the ONLY widget-
    // creation entry now (the old topbar "+" was removed): living in the chart
    // toolbar means it renders in EVERY chrome, including the embedded /demo +
    // event replays where the native topbar is suppressed.
    {
        const char* label = compact_tools ? "Tools" : "+ Widget";
        const float tw = ImGui::CalcTextSize(label).x;
        const float w = 10.0f + tw + 10.0f;
        wrap_control(w);
        ImGui::SetCursorScreenPos(ImVec2(cx, ctrl_y));
        widget_anchor = ImVec2(cx, ctrl_y + 28.0f);
        const bool clicked = ImGui::InvisibleButton("##add_widget_btn", ImVec2(w, 28.0f));
        ui_test::mark("chart.add_widget");
        const bool open = ImGui::IsPopupOpen(compact_tools ? "##chart_tools" : "add_widget_popup");
        const bool hot  = ImGui::IsItemHovered() || open;
        if (hot) dl->AddRectFilled(ImVec2(cx, ctrl_y), ImVec2(cx + w, ctrl_y + 28.0f),
                    Theme::u32(open ? Theme::Tokens::CONTROL_HOVER : Theme::Tokens::CONTROL), Theme::Radius::R2);
        dl->AddText(ImVec2(cx + 10.0f, ctrl_y + (28.0f - ImGui::GetFontSize()) * 0.5f),
                    Theme::u32(hot ? Theme::Tokens::BRAND_TX : Theme::Tokens::TX2), label);
        if (ImGui::IsItemHovered()) Theme::tooltip(compact_tools ? "Indicators, drawing and chart settings" : "Add a widget");
        if (clicked && !open) ImGui::OpenPopup(compact_tools ? "##chart_tools" : "add_widget_popup");
        cx += w + 12.0f;
    }

    if (Entitlements::hosted() && ctx_.replay_mgr().is_active()) {
        const float w = ImGui::CalcTextSize("Bookmark").x + ImGui::GetStyle().FramePadding.x * 2;
        wrap_control(w);
        ImGui::SetCursorScreenPos(ImVec2(cx, ctrl_y));
        render_activity_bookmark();
        cx += w + 12.0f;
    }

    const char* requested_popup = nullptr;
    if (Theme::begin_menu("##chart_tools")) {
        using drawing::UiIcon;
        if (Theme::menu_item(UiIcon::Pulse, "Indicators")) requested_popup = "indicators_popup";
        if (Theme::menu_item(UiIcon::Gear, "Chart settings")) {
            if (rt_mode_) requested_popup = "rt_chart_settings";
            else liq_settings_panel_.open();
        }
        if (Theme::menu_item(UiIcon::Plus, "Add widget")) requested_popup = "add_widget_popup";
        ui_test::mark("chart.tools.add_widget");
        if (!ClipRecorder::focus_active() && !edu::RecorderRuntime::instance().active() &&
            Theme::menu_item(UiIcon::Pencil, "Drawing tools")) requested_popup = "chart_draw_popup";
        Theme::menu_separator();
        Theme::menu_section("SHIFT + DRAG SELECTS A MOVE");
        ImGui::EndPopup();
    }
    if (requested_popup) ImGui::OpenPopup(requested_popup);


    // Draw button - the rail's tool list as a dropdown (icons left of labels).
    // Match the rail in live and replay; hide only during recording.
    {
        const bool show_draw_menu = !ClipRecorder::focus_active() &&
                                    !edu::RecorderRuntime::instance().active();
        if (show_draw_menu) {
            if (!compact_tools) {
            const char* label = "Draw";
            const float tw = ImGui::CalcTextSize(label).x;
            const float w = 10.0f + 15.0f + 6.0f + tw + 10.0f;
            wrap_control(w);
            ImGui::SetCursorScreenPos(ImVec2(cx, ctrl_y));
            const bool clicked =
                ImGui::InvisibleButton("##draw_menu_btn", ImVec2(w, 28.0f));
            const bool open = ImGui::IsPopupOpen("chart_draw_popup");
            const bool hot  = ImGui::IsItemHovered() || open;
            if (hot) dl->AddRectFilled(ImVec2(cx, ctrl_y), ImVec2(cx + w, ctrl_y + 28.0f),
                    Theme::u32(open ? Theme::Tokens::CONTROL_HOVER : Theme::Tokens::CONTROL), Theme::Radius::R2);
            drawing::draw_ui_icon(
                dl, drawing::UiIcon::Pencil,
                ImVec2(cx + 10.0f + 7.5f, ctrl_y + 14.0f), 6.0f,
                Theme::u32(hot ? Theme::Tokens::BRAND_TX : Theme::Tokens::TX2),
                1.4f);
            dl->AddText(ImVec2(cx + 10.0f + 15.0f + 6.0f,
                               ctrl_y + (28.0f - ImGui::GetFontSize()) * 0.5f),
                        Theme::u32(hot ? Theme::Tokens::BRAND_TX
                                       : Theme::Tokens::TX2),
                        label);
            if (ImGui::IsItemHovered()) Theme::tooltip("Drawing tools");
            if (clicked && !open) ImGui::OpenPopup("chart_draw_popup");
            cx += w + 12.0f;

            }
            if (Theme::begin_menu("chart_draw_popup", 220.0f)) {
                drawing::render_tool_menu_rows(ctx_.drawing_mgr());
                ImGui::EndPopup();
            }
        }
    }



    // A chart-local action inherits this chart's market, including its venue.
    // The menu sizes to its rows: it used to be pinned at 260px, and the
    // "Chart  TAKE/USDT" hint ran past the right edge because the label
    // column is set by the widest label ("Chart of another market...").
    ImGui::SetNextWindowPos(ImVec2(widget_anchor.x, widget_anchor.y + 4.0f), ImGuiCond_Appearing);
    if (Theme::begin_menu("add_widget_popup", 250.0f)) {
        using drawing::UiIcon;
        Theme::menu_section("ADD WIDGET");
        using PW = Menu::SymbolPickerState::PendingWidget;
        const bool embedded = EducationBoot::instance().is_embedded()
                           || EducationBoot::instance().is_pack();
        auto add = [&](PW type) {
            Menu::g_widget_add_request.type = type;
            Menu::g_widget_add_request.pair = pair_;
            Menu::g_widget_add_request.fmt = fmt_;
            Menu::g_widget_add_request.tick_size = tick_size_;
            Menu::g_widget_add_request.pending = true;
        };
        // Chart names its market: this row inherits ONE/USDT, the next opens
        // the finder for a chart of a different market in the same layout.
        // Until 2026-09-20 nothing opened the finder in add mode (the header
        // and Ctrl-K both navigate), so a two-market layout was unreachable
        // even though a chart of another pair owns its own manager. The
        // finder renders only in the full shell, and a fresh chart of another
        // pair during a replay has no session data to draw, so the row is
        // disabled there rather than inert. A second chart of THIS market
        // works in a replay: it builds its own manager in the replay context.
        const std::string this_market = widget_symbol_label(pair_.symbol);
        if (Theme::menu_item(UiIcon::Candles, "Chart", this_market.c_str())) add(PW::Charts);
        ui_test::mark("widget_add.chart");
        {
            const bool other_ok = !embedded && !ctx_.replay_mgr().is_active();
            if (Theme::menu_item(UiIcon::Globe, "Chart of another market\xe2\x80\xa6", nullptr, other_ok)) {
                auto& pk = Menu::g_symbol_picker;
                pk.pending           = PW::Charts;
                pk.replace_mode      = false;
                pk.compare_mode      = false;
                pk.selected_exchange = pair_.exchange;
                pk.search_buf[0]     = '\0';
                pk.open              = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (other_ok)
                    Theme::tooltip("Add a chart of any market beside this one.\nStarts on %s; switch venue in the finder.", pair_.exchange.c_str());
                else if (embedded)
                    Theme::tooltip("The finder is available in the live terminal.");
                else
                    Theme::tooltip("A replay plays its session markets only.\nUse the chart menu: Replay from here with\xe2\x80\xa6");
            }
        }
        Theme::menu_separator();
        if (Theme::menu_item(UiIcon::Book, "Orderbook")) add(PW::Orderbook);
        if (Theme::menu_item(UiIcon::Ladder, "Depth of market", "DOM")) add(PW::DOM);
        if (Theme::menu_item(UiIcon::Tape, "Trades")) add(PW::Trades);
        ui_test::mark("widget_add.trades");
        if (Theme::menu_item(UiIcon::Bars, "Market data & research")) Menu::request_market_data(pair_);
        if (Theme::menu_item(UiIcon::Bars, "Market statistics")) add(PW::Stats);
        Theme::menu_separator();
        if (Theme::menu_item(UiIcon::Wallet, "Paper trading")) add(PW::PaperTrading);
        if (!embedded && Theme::menu_item(UiIcon::Star, "Watchlist")) add(PW::Watchlist);
        if (!embedded && Theme::menu_item(UiIcon::Library, "Replay library")) add(PW::ReplayLibrary);
        if (Theme::menu_item(UiIcon::Terminal, "Debug log")) add(PW::Debug);
        ImGui::EndPopup();
    }

    // (RT toggle relocated to the TIMEFRAME dropdown - app_shell render_tf_menu.
    //  It now applies to every chart type, not just the Line, so the old Line-
    //  only toolbar pill was removed.)


    // Layer toggles + LIQ LEV now live inside the layers menu (defined below).

    // Open outside the Layers popup so the settings panel shares its ID scope.
    if (open_heatmap_settings_) {
        ImGui::OpenPopup("hm_settings");
        open_heatmap_settings_ = false;
    }
    // Depth settings: the shared menu chrome, grouping as chips (like the
    // leverage chips), toggles as rows and the explanations as notes.
    if (Theme::begin_menu("hm_settings", 300.0f)) {
        using drawing::UiIcon;
        Theme::menu_section(rt_mode_ ? "REAL-TIME DEPTH \xc2\xb7 PRICE GROUPING" : "ORDER BOOK DEPTH \xc2\xb7 PRICE GROUPING");
        const int multipliers[] = {1, 2, 5, 10, 20};
        const char* labels[] = {"UHD", "HD", "SD", "LD", "ULD"};
        int& multiplier = rt_mode_ ? rt_bucket_multiplier_ : heatmap_bucket_multiplier_;
        int selected = 0;
        for (int i = 0; i < 5; ++i)
            if (multiplier == multipliers[i]) selected = i;
        bool changed = false;
        {
            ImGui::Indent(6.0f);
            const float gap = 4.0f;
            const float chip_w = std::max(46.0f, (ImGui::GetContentRegionAvail().x - 6.0f - gap * 4.0f) / 5.0f);
            for (int i = 0; i < 5; ++i) {
                if (i > 0) ImGui::SameLine(0.0f, gap);
                ImGui::PushID(700 + i);
                if (Theme::choice_button(labels[i], selected == i, ImVec2(chip_w, 26.0f)) && selected != i) {
                    selected = i;
                    changed = true;
                }
                if (ImGui::IsItemHovered())
                    Theme::tooltip("%d native tick%s per row", multipliers[i], multipliers[i] == 1 ? "" : "s");
                ImGui::PopID();
            }
            ImGui::Unindent(6.0f);
        }
        multiplier = multipliers[selected];
        if (changed && rt_mode_) { rt_auto_fit_ = {}; rt_effective_multiplier_ = multiplier; }
        auto* recon = rt_mode_ ? rt_renderer_.get() : ctx_.heatmap_mgr().get_reconstructor(pair_, heatmap_mode_);
        if (recon) {
            char bucket[48], line[96];
            snprintf(bucket, sizeof(bucket), fmt_.price_fmt, recon->get_display_bucket_size());
            snprintf(line, sizeof(line), "Each row now covers %s.", bucket);
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            Theme::menu_note(line);
        }
        Theme::menu_separator();
        // ── [SELF-HOST P1] Depth palette + brightness curve ──────────────
        // One picker drives every depth path at once (history orderbook and
        // real-time cool/warm); the brightness curve shapes the mids without
        // touching the liquidation field's tuned curve.
        {
            static const char* const kDepthMapLabels[] = {
                "EdgeDepth (default)", "Teal-Gold", "Blue-Red", "Mono-Ice", "Viridis", "Inferno"};
            const int cur_map = int(HeatmapColormap::depth_map());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##depth_palette", kDepthMapLabels[cur_map])) {
                for (int i = 0; i < int(std::size(kDepthMapLabels)); ++i) {
                    if (ImGui::Selectable(kDepthMapLabels[i], i == cur_map) && i != cur_map)
                        HeatmapColormap::set_depth_map(HeatmapColormap::DepthMap(i));
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                Theme::tooltip("Colour ramp of the depth heatmap.\nApplies to history and real-time at once and survives reloads.");
            int gamma = HeatmapColormap::depth_gamma_pct();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##depth_gamma", &gamma, 50, 250, "Brightness %d%%"))
                HeatmapColormap::set_depth_gamma_pct(gamma);
            if (ImGui::IsItemHovered())
                Theme::tooltip("Below 100 darkens the mid-tones, above 100 lifts them.\nPeaks and floors stay pinned; the liquidation field is untouched.");
        }
        if (rt_mode_) {
            Theme::menu_note("Hold Alt over depth to inspect a price row.");
            if (recon) {
                const bool warm = recon->realtime_warm();
                if (Theme::menu_toggle("Cool-to-warm palette", warm)) recon->set_realtime_warm(!warm);
            }
            if (Theme::menu_toggle("Auto contrast", rt_auto_contrast_)) rt_auto_contrast_ = !rt_auto_contrast_;
            if (ImGui::IsItemHovered())
                Theme::tooltip("Colours ease toward the visible book every few seconds.\nOff keeps them fixed until Recalibrate colors.");
            // ── [SELF-HOST P1] Peak-hold: pulled walls leave a ghost ─────
            if (Theme::menu_toggle("Peak hold", rt_peak_hold_)) rt_peak_hold_ = !rt_peak_hold_;
            if (ImGui::IsItemHovered())
                Theme::tooltip("Ghost bar for the deepest level each price row held\nwithin the window - a pulled wall keeps its trace.");
            if (rt_peak_hold_) {
                static const char* const kHorizons[] = {"5 min", "10 min", "30 min"};
                static const int kHorizonMin[] = {5, 10, 30};
                const int idx = rt_peak_horizon_min_ <= 5 ? 0 : rt_peak_horizon_min_ <= 10 ? 1 : 2;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##peak_horizon", kHorizons[idx])) {
                    for (int i = 0; i < 3; ++i) {
                        if (ImGui::Selectable(kHorizons[i], i == idx) && i != idx) {
                            rt_peak_horizon_min_ = kHorizonMin[i];
                            rt_peak_levels_.clear();
                        }
                    }
                    ImGui::EndCombo();
                }
            }
        } else {
            if (Theme::menu_toggle("Adapt to zoom", heatmap_adapt_to_zoom_)) {
                heatmap_adapt_to_zoom_ = !heatmap_adapt_to_zoom_;
                changed = true;
            }
            if (ImGui::IsItemHovered())
                Theme::tooltip("Group more price rows when zoomed out.\nTurn off for the exact selected grouping.");
        }
        if (recon && Theme::menu_item(UiIcon::Refresh, "Recalibrate colors", nullptr, true, /*keep_open=*/true)) {
            if (rt_mode_) recon->recalibrate_realtime_colors();
            else recon->recalibrate_candle_colors();
        }
        if (changed && recon) recon->set_bucket_multiplier(rt_mode_ ? rt_effective_multiplier_ : multiplier);
        Theme::menu_separator();
        if (rt_mode_) {
            Theme::menu_note(rt_auto_fit_history_ ? "Auto-fit uses this grouping as its minimum. Changing it recalibrates the colours."
                                                  : "Real-time keeps the selected grouping fixed. Changing it recalibrates the colours.");
        } else if (recon) {
            char sampling[320];
            snprintf(sampling, sizeof(sampling),
                     "Colours stay fixed until regrouping or recalibration. Each %lld-second candle slot shows the latest recorded book snapshot in that interval, not summed liquidity. Choose a shorter timeframe for finer sampling. Blank areas may be outside the loaded depth range.",
                     (long long)recon->get_column_interval_ms() / 1000);
            Theme::menu_note(sampling);
        }
        Theme::menu_note("Grouping changes price rows, not tile duration.");
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::EndPopup();
    }
    // ═══════════════════════════════════════════════════════════════════════
    // Chart View dropdown: one base rendering mode at a time.
    // ═══════════════════════════════════════════════════════════════════════
    // Shared floating-chrome style for both menus: bg-1, 1px line-2 border,
    // radius 6, padding 4, plus a soft drop shadow (floating menus are the one
    // place rounding + shadow are allowed).
    // Chart-view menu: price views first, then order-flow views. Each row says
    // what changes, so the menu works as a compact feature inventory too.
    ImGui::SetNextWindowPos(ImVec2(ct_anchor.x, ct_anchor.y + 4.0f));
    int settings_view = -1;
    if (Theme::begin_menu("chart_type_popup", 280.0f)) {
        struct CtRow { const char* label; const char* detail; int idx; };
        const CtRow rows[8] = {
            {"Candles", "Standard OHLC bars", 0},
            {"Heikin Ashi", "Smoothed candles for trend structure", 3},
            {"Line", "Close price without candle noise", 4},
            {"Renko", "Price movement without fixed time bars", 6},
            {"Footprint cluster", "Bid and ask volume at every price", 1},
            {"Footprint profile", "Traded-volume shape inside each bar", 2},
            {"Flow & Positioning", "Aligned aggression, raw OI and evidence", 7},
            {"TPO market profile", "Time spent at each price", 5} };
        ImDrawList* d = ImGui::GetWindowDrawList();
        constexpr float kGearW = 26.0f, kCheckW = 18.0f;
        for (int r = 0; r < 8; ++r) {
            if (r == 0 || r == 4) {
                if (r == 4) Theme::menu_separator();
                Theme::menu_section(r == 0 ? "PRICE" : "ORDER FLOW");
            }
            const int idx = rows[r].idx;
            const bool active = (chart_type_ == static_cast<ChartType>(idx));
            const bool has_settings = idx == 1 || idx == 2 || idx == 5 || idx == 6;
            // The row keeps room for the active check and, where the view has
            // settings, a gear submitted over the row's right end.
            ImGui::PushStyleColor(ImGuiCol_Text, active ? Theme::Tokens::TX1 : Theme::Tokens::TX2);
            const Theme::MenuRow row = Theme::menu_row(rows[r].label, nullptr, true, false,
                                                       kCheckW + (has_settings ? kGearW : 0.0f));
            ImGui::PopStyleColor();
            draw_ctype_glyph(d, idx, row.icon.x - 7.0f, row.icon.y - 7.0f, row.icon_col);
            if (row.hovered) Theme::tooltip("%s", rows[r].detail);
            const float gear_x = row.max.x - 10.0f - (has_settings ? kGearW : 0.0f);
            if (active)
                drawing::draw_ui_icon(d, drawing::UiIcon::Check,
                                      ImVec2(gear_x - kCheckW * 0.5f - 2.0f, row.icon.y), 5.0f,
                                      Theme::u32(Theme::Tokens::BRAND_TX), 1.6f);
            if (row.clicked) set_chart_type(static_cast<ChartType>(idx));
            bool settings_clicked = false;
            if (has_settings) {
                ImGui::PushID(r);
                const ImVec2 gp(gear_x, row.min.y + (row.max.y - row.min.y - 24.0f) * 0.5f);
                ImGui::SetCursorScreenPos(gp);
                settings_clicked = ImGui::InvisibleButton("##ct_gear", ImVec2(kGearW, 24.0f));
                const bool gear_hov = ImGui::IsItemHovered();
                if (gear_hov) {
                    d->AddRectFilled(gp, ImVec2(gp.x + kGearW, gp.y + 24.0f), Theme::u32(Theme::Tokens::HOVER));
                    Theme::tooltip("%s settings", rows[r].label);
                }
                drawing::draw_ui_icon(d, drawing::UiIcon::Gear, ImVec2(gp.x + kGearW * 0.5f, gp.y + 12.0f), 6.0f,
                                      Theme::u32(gear_hov ? Theme::Tokens::TX1 : Theme::Tokens::TX3), 1.3f);
                ImGui::PopID();
                ImGui::SetCursorScreenPos(ImVec2(row.min.x, row.max.y));
            }
            if (has_settings && (settings_clicked || row.right_clicked)) {
                ImGui::CloseCurrentPopup();
                settings_view = idx;
            }
        }
        Theme::menu_separator();
        Theme::menu_section("RIGHT-CLICK A VIEW FOR ITS SETTINGS");
        ImGui::EndPopup();
    }

    // Open at the same window/ID scope as the settings panel, not inside the menu.
    if (settings_view == 1 || settings_view == 2) footprint_settings_panel_.open();
    if (settings_view == 5) tpo_settings_panel_.open();
    if (settings_view == 6) ImGui::OpenPopup("renko_settings");

    // Renko brick-size config popup (opened by right-clicking the Renko row OR
    // the on-chart gear; the gear defers OpenPopup to here so it runs at window
    // scope rather than inside BeginPlot).
    if (open_renko_settings_) { ImGui::OpenPopup("renko_settings"); open_renko_settings_ = false; }
    render_renko_settings_popup();

    render_overlay_menu(true, liq_anchor);
    render_overlay_menu(false, ly_anchor);

    // ═══════════════════════════════════════════════════════════════════════
    // Indicators popup: subplots only. Anything drawn on the main chart belongs
    // in Layers, so one capability has one home and one active count.
    // ═══════════════════════════════════════════════════════════════════════
    // Right group: Indicators + Settings, right-aligned to the bar's right edge.
    // The button carries an active-count chip and the menu stays open for
    // multi-selection.
    // The chip and the menu describe what is on screen. In a view without
    // subplots the count is zero and the menu says why instead of offering
    // toggles that would change nothing visible.
    const bool indi_suppressed = subplots_suppressed();
    int indi_active_n = liquidation_activity_visible() ? 1 : 0;
    if (!indi_suppressed) {
        if (indicator_mgr_.has_indicator_of_type<Indicators::VolumeIndicator>())      ++indi_active_n;
        if (indicator_mgr_.has_indicator_of_type<Indicators::AbsorptionIndicator>()) ++indi_active_n;
        if (indicator_mgr_.has_indicator_of_type<Indicators::CVDIndicator>())         ++indi_active_n;
        if (indicator_mgr_.has_indicator_of_type<Indicators::RSIIndicator>())         ++indi_active_n;
        if (indicator_mgr_.has_indicator_of_type<Indicators::MACDIndicator>())        ++indi_active_n;
        if (indicator_mgr_.has_indicator_of_type<Indicators::FundingRateIndicator>()) ++indi_active_n;
        if (indicator_mgr_.has_indicator_of_type<Indicators::OIIndicator>())          ++indi_active_n;
        if (indicator_mgr_.has_indicator_of_type<Indicators::VPINIndicator>())        ++indi_active_n;
    }
    char indi_nbuf[8];
    snprintf(indi_nbuf, sizeof(indi_nbuf), "%d", indi_active_n);

    ImGui::PushFont(Theme::Fonts::ui());
    const float rg_indic_txt = ImGui::CalcTextSize("Indicators").x;
    const float rg_set_txt   = ImGui::CalcTextSize("Settings").x;
    ImGui::PopFont();
    float indi_chip_w = 0.0f;
    if (indi_active_n > 0) {
        ImGui::PushFont(Theme::Fonts::mono_sm());
        indi_chip_w = 7.0f + ImGui::CalcTextSize(indi_nbuf).x + 10.0f;  // gap + chip
        ImGui::PopFont();
    }
    const float rg_indic_w = rg_indic_txt + 11.0f + 13.0f + 7.0f + indi_chip_w + 11.0f;
    const float rg_set_w   = rg_set_txt + 12.0f;       // borderless text button
    if (!compact_tools) wrap_control(rg_indic_w + 12.0f + rg_set_w);
    const float rg_right   = bp.x + ww - 12.0f;        // right gutter 12
    const float rg_set_x   = rg_right - rg_set_w;
    const float rg_indic_x = rg_set_x - 12.0f - rg_indic_w;
    {
        if (!compact_tools) {
        ImGui::SetCursorScreenPos(ImVec2(rg_indic_x, ctrl_y));
        const bool clicked = ImGui::InvisibleButton("##indic_btn", ImVec2(rg_indic_w, 28.0f));
        const bool open = ImGui::IsPopupOpen("indicators_popup");
        const bool hot  = ImGui::IsItemHovered() || open;
        if (hot)
            dl->AddRectFilled(ImVec2(rg_indic_x, ctrl_y), ImVec2(rg_indic_x + rg_indic_w, ctrl_y + 28.0f),
                              Theme::u32(open ? Theme::Tokens::CONTROL_HOVER : Theme::Tokens::CONTROL), Theme::Radius::R2);
        // Mini bar-chart glyph (1k) + the label.
        const ImU32 icol = Theme::u32(hot ? Theme::Tokens::BRAND_TX : Theme::Tokens::TX2);
        {
            const float ix = rg_indic_x + 11.0f, base = ctrl_y + 20.5f;   // bottom baseline
            const float bars[4] = {6.0f, 11.0f, 8.0f, 13.0f};
            for (int b = 0; b < 4; ++b) {
                const float bx = ix + b * 3.5f;
                dl->AddRectFilled(ImVec2(bx, base - bars[b]), ImVec2(bx + 2.2f, base), icol);
            }
        }
        ImGui::PushFont(Theme::Fonts::ui());
        dl->AddText(ImVec2(rg_indic_x + 11.0f + 13.0f + 7.0f, ctrl_y + (28.0f - ImGui::GetFontSize()) * 0.5f),
                    icol, "Indicators");
        ImGui::PopFont();
        if (indi_active_n > 0) {
            // The count as a soft chip, as on Layers.
            ImGui::PushFont(Theme::Fonts::mono_sm());
            const ImVec2 cts = ImGui::CalcTextSize(indi_nbuf);
            const float chx = rg_indic_x + 11.0f + 13.0f + 7.0f + rg_indic_txt + 7.0f;
            const float chh = 15.0f, chy = ctrl_y + (28.0f - chh) * 0.5f;
            dl->AddRectFilled(ImVec2(chx, chy), ImVec2(chx + cts.x + 10.0f, chy + chh),
                              Theme::u32(Theme::Tokens::CONTROL_ACTIVE), Theme::Radius::R1);
            dl->AddText(ImVec2(chx + 5.0f, chy + (chh - cts.y) * 0.5f),
                        Theme::u32(Theme::Tokens::TX1), indi_nbuf);
            ImGui::PopFont();
        }
        if (ImGui::IsItemHovered()) Theme::tooltip("Add or remove indicator subplots");
        if (clicked && !open) ImGui::OpenPopup("indicators_popup");

        }
        const auto* popup_viewport=ImGui::GetMainViewport();
        const float popup_x=std::max(popup_viewport->WorkPos.x,
            std::min(rg_indic_x,popup_viewport->WorkPos.x+popup_viewport->WorkSize.x-276.0f));
        ImGui::SetNextWindowPos(ImVec2(popup_x, ctrl_y + 28.0f + 4.0f));
        // Indicators: the subplots under the chart, in the Layers grammar (sections, check-box rows, tags) so the
        // two menus read as one system. Rows flip live and the menu stays open; the button carries the count.
        if (Theme::begin_menu("indicators_popup", 264.0f, popup_viewport->WorkSize.y - 32.0f)) {
            bool first_section = true;
            auto section = [&first_section](const char* label) {
                if (!first_section) Theme::menu_separator();
                first_section = false;
                Theme::menu_section(label);
            };
            // One subplot row: on while the chart has an indicator of that type; a click adds or removes it.
            auto subplot = [&](auto type, const char* label, auto&& add, const char* tip = nullptr, const char* tag = nullptr) {
                using T = typename decltype(type)::type;
                const bool active = indicator_mgr_.has_indicator_of_type<T>();
                if (Theme::menu_layer(label, active, false, tag)) {
                    if (active) indicator_mgr_.remove_indicator_of_type<T>();
                    else add();
                }
                if (tip && ImGui::IsItemHovered()) Theme::tooltip("%s", tip);
            };

            if (rt_mode_ || ct_allows_time_overlays(chart_type_)) {
                section("LIQUIDATIONS");
                const bool can = Entitlements::is_pro() || ctx_.replay_mgr().is_active() || !Entitlements::hosted();
                bool& activity = rt_mode_ ? rt_liq_strip_ : candle_liq_strip_;
                const bool on = can && reported_liquidations_enabled() && activity;
                if (Theme::menu_layer("Reported liquidations", on, !can)) {
                    if (can) {
                        activity = !on;
                        if (activity) reported_liquidations_enabled() = true;
                    } else ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Layer, nullptr, "liq_observed");
                }
                if (can && ImGui::IsItemHovered())
                    Theme::tooltip("Shorts liquidated above the line, longs below, per %s.\n%s.",
                                   rt_mode_ ? "second" : "candle", observed_liquidations::feed_line(pair_.exchange));
            }

            if (indi_suppressed) {
                section(rt_mode_ ? "CANDLE INDICATORS" : "INDICATORS");
                Theme::menu_note(rt_mode_ ? "Saved separately; they return with a candle timeframe."
                                          : "Not shown on Renko charts; they return with a time-based view.");
            } else {
                section("VOLUME AND FLOW");
                subplot(std::type_identity<Indicators::VolumeIndicator>{}, "Volume", [&] { add_volume_indicator(); });
                subplot(std::type_identity<Indicators::CVDIndicator>{}, "CVD", [&] { add_cvd_indicator(); },
                        "Cumulative volume delta: taker buys minus taker sells.");
                subplot(std::type_identity<Indicators::VPINIndicator>{}, "VPIN toxicity", [&] { add_vpin_indicator(); },
                        "Flow toxicity (VPIN, volume clock) with HMM regime colouring.\nQuiet markets hold their last reading.");
                // A restored indicator on an unsupported market remains removable.
                if (absorption_supported && absorption_locked() &&
                    !indicator_mgr_.has_indicator_of_type<Indicators::AbsorptionIndicator>()) {
                    if (Theme::menu_layer("Absorption activity", false, true)) open_absorption_upsell();
                } else if (absorption_supported || indicator_mgr_.has_indicator_of_type<Indicators::AbsorptionIndicator>())
                    subplot(std::type_identity<Indicators::AbsorptionIndicator>{}, "Absorption activity",
                            [&] { add_absorption_indicator(); }, "Binance BTC and SOL only. Candles: 1m or longer.",
                            absorption_supported ? "pilot" : "unavailable");
                section("MOMENTUM");
                subplot(std::type_identity<Indicators::RSIIndicator>{}, "RSI", [&] { add_rsi_indicator(); });
                subplot(std::type_identity<Indicators::MACDIndicator>{}, "MACD", [&] { add_macd_indicator(); });
                section("DERIVATIVES");
                subplot(std::type_identity<Indicators::FundingRateIndicator>{}, "Funding rate", [&] { add_funding_rate_indicator(); },
                        "Funding per interval: positive, longs pay; negative, shorts pay.");
                subplot(std::type_identity<Indicators::OIIndicator>{}, "Open interest", [&] { add_oi_indicator(); },
                        "Open interest as candles: green when it rose, red when it fell.");
            }
            ImGui::EndPopup();
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Right-aligned: Indicators + Settings (borderless). LOW->HIGH legend dropped in v2.
    // ═══════════════════════════════════════════════════════════════════════
    {
        // Settings follows the active chart mode.
        if (!compact_tools) {
        ImGui::SetCursorScreenPos(ImVec2(rg_set_x, ctrl_y));
        const bool clicked = ImGui::InvisibleButton("##settings_btn", ImVec2(rg_set_w, 28.0f));
        ui_test::mark("chart.settings");
        const bool hot = ImGui::IsItemHovered();
        ImGui::PushFont(Theme::Fonts::ui());
        dl->AddText(ImVec2(rg_set_x + 6.0f, ctrl_y + (28.0f - ImGui::GetFontSize()) * 0.5f),
                    Theme::u32(hot ? Theme::Tokens::BRAND_TX : Theme::Tokens::TX3), "Settings");
        ImGui::PopFont();
        if (ImGui::IsItemHovered())
            Theme::tooltip(rt_mode_ ? "View controls and appearance"
                                    : "Chart settings: appearance, colors and heatmap");
        if (clicked) {
            if (rt_mode_) ImGui::OpenPopup("rt_chart_settings");
            else liq_settings_panel_.open();
        }
        }
        if (Theme::begin_menu("rt_chart_settings", 260.0f, ImGui::GetMainViewport()->WorkSize.y - 80)) {
            render_realtime_settings();
            Theme::menu_separator();
            if (Theme::begin_menu_group("Appearance")) {
                Theme::render_appearance_controls(); Theme::end_menu_group();
            }
            ImGui::EndPopup();
        }
    }

    // ── Deferred settings popup opens (from right-click in Indicators) ──
    if (open_liq_settings_) {
        liq_settings_panel_.open();
        open_liq_settings_ = false;
    }
    if (open_vpvr_settings_) {
        vpvr_settings_panel_.open();
        open_vpvr_settings_ = false;
    }
    // ── Settings popups (opened by right-clicking indicator items) ────────
    // These must be at toolbar scope, not inside the Indicators popup.
    // Liq heatmap settings - the design floating panel (Style / Intensity),
    // wired straight to the Field's chart_widget.h knobs. Low/Peak/tick-per-row/
    // half-life are baked into the cache at build → invalidate on change.
    liq_settings_panel_.set_panel_height(300.0f);
    if (liq_settings_panel_.begin()) {
        if (liq_settings_panel_.tab("Appearance")) Theme::render_appearance_controls();
        if (liq_settings_panel_.tab("Style")) {
            ImGui::Text("Colormap");
            auto cmap_btn = [&](const char* label, HeatmapColormap::LiqMap m, bool same_line) {
                if (same_line) ImGui::SameLine(0.0f, 5.0f);
                const bool on = (HeatmapColormap::liq_map() == m);
                ImGui::PushStyleColor(ImGuiCol_Button, on ? Theme::Tokens::BRAND_SOFT
                                                          : Theme::Tokens::INPUT);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? Theme::Tokens::BRAND_SOFT
                                                                 : Theme::Tokens::HOVER);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Tokens::ACTIVE);
                ImGui::PushStyleColor(ImGuiCol_Text, on ? Theme::Tokens::BRAND_TX
                                                        : Theme::Tokens::TX2);
                if (ImGui::Button(label)) { HeatmapColormap::set_liq_map(m); Theme::save_preferences(); }
                ImGui::PopStyleColor(4);
            };
            cmap_btn("Ember",   HeatmapColormap::LiqMap::Ember,   false);
            cmap_btn("Inferno", HeatmapColormap::LiqMap::Inferno, true);
            cmap_btn("Magma",   HeatmapColormap::LiqMap::Magma,   true);
            cmap_btn("Viridis", HeatmapColormap::LiqMap::Viridis, true);
            ImGui::Spacing();
            ImGui::SetNextItemWidth(180);
            ImGui::SliderFloat("Opacity", &liq_field_.knobs().opacity, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
                Theme::tooltip("Field opacity - the alpha curve handles the melt\n"
                                  "(design 0.90)");
        }
        if (liq_settings_panel_.tab("Intensity")) {
            bool rebuild = false;
            ImGui::SetNextItemWidth(180);
            ImGui::SliderFloat("Gamma", &liq_field_.knobs().gamma, 0.5f, 4.0f, "%.2f");
            if (ImGui::IsItemHovered())
                Theme::tooltip("Shaping on the log-mapped intensity - higher darkens\n"
                                  "the mid-band, deepens the black toe (design 2.2)");
            ImGui::SetNextItemWidth(180);
            rebuild |= ImGui::SliderFloat("Low", &liq_field_.knobs().lo_pct, 0.0f, 0.90f, "p%.2f");
            if (ImGui::IsItemHovered())
                Theme::tooltip("Intensity-Low clip percentile - higher culls weak plots\n"
                                  "into black gaps; lower fills movers denser");
            ImGui::SetNextItemWidth(180);
            rebuild |= ImGui::SliderFloat("Peak", &liq_field_.knobs().hi_pct, 0.950f, 1.000f, "p%.4f");
            if (ImGui::IsItemHovered())
                Theme::tooltip("Intensity-Peak clip percentile - higher makes yellow\n"
                                  "ignition rarer (design 0.9985)");
            ImGui::SetNextItemWidth(180);
            ImGui::SliderFloat("Noise floor", &liq_field_.knobs().floor_t, 0.0f, 0.10f, "%.3f");
            if (ImGui::IsItemHovered())
                Theme::tooltip("Rendered-intensity floor - culls sub-visible fuel (design 0.02)");
            ImGui::SetNextItemWidth(180);
            rebuild |= ImGui::SliderFloat("Tick-per-row", &liq_field_.knobs().bps, 2.0f, 20.0f, "%.0f bps");
            if (ImGui::IsItemHovered())
                Theme::tooltip("Price-row height in bps of its own price (log grid)");
            ImGui::SetNextItemWidth(180);
            rebuild |= ImGui::SliderFloat("Half-life", &liq_field_.knobs().halflife_h, 0.0f, 72.0f, "%.0f h");
            if (ImGui::IsItemHovered())
                Theme::tooltip("Age-decay half-life on standing fuel - stale far-history\n"
                                  "dims sooner; 0 disables (design 16h, replay-safe)");
            if (rebuild) liq_field_.invalidate();  // Low/Peak/bps/half-life bake at cache build
        }
        liq_settings_panel_.end();
    }
    // VPVR settings (tabbed panel)
    vpvr_settings_panel_.set_panel_height(280.0f);
    if (vpvr_settings_panel_.begin()) {
        if (vpvr_settings_panel_.tab("Display")) {
            const char* vp_modes[] = { "Standard", "Total Volume", "Total Delta" };
            int vp_mode_idx = static_cast<int>(ctx_.vpvr_mgr().mode());
            ImGui::SetNextItemWidth(130);
            if (ImGui::Combo("Mode##vp", &vp_mode_idx, vp_modes, 3)) {
                ctx_.vpvr_mgr().set_mode(static_cast<VolumeProfileManager::Mode>(vp_mode_idx));
            }
            ImGui::Spacing();
            bool vp_poc = ctx_.vpvr_mgr().show_poc();
            if (ImGui::Checkbox("POC Line##vp", &vp_poc)) ctx_.vpvr_mgr().set_show_poc(vp_poc);
            bool vp_va = ctx_.vpvr_mgr().show_vah_val();
            if (ImGui::Checkbox("VAH/VAL Lines##vp", &vp_va)) ctx_.vpvr_mgr().set_show_vah_val(vp_va);
            bool vp_vals = ctx_.vpvr_mgr().show_values();
            if (ImGui::Checkbox("Show Values##vp", &vp_vals)) ctx_.vpvr_mgr().set_show_values(vp_vals);
        }
        if (vpvr_settings_panel_.tab("Sizing")) {
            int vp_width_pct = static_cast<int>(ctx_.vpvr_mgr().width_pct() * 100.0f + 0.5f);
            ImGui::SetNextItemWidth(130);
            if (ImGui::SliderInt("Width##vp", &vp_width_pct, 5, 50, "%d%%")) {
                ctx_.vpvr_mgr().set_width_pct(static_cast<float>(vp_width_pct) / 100.0f);
            }
            ImGui::Spacing();
            ImGui::Text("Ticks Per Row");
            ImGui::SameLine();
            if (vpvr_ticks_per_row_ == 0) {
                ImGui::TextDisabled("(auto)");
            } else if (tick_size_ > 0) {
                char pl[32];
                snprintf(pl, sizeof(pl), "($%.2f)", vpvr_ticks_per_row_ * tick_size_);
                ImGui::TextDisabled("%s", pl);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("-##vptpr") && vpvr_ticks_per_row_ > 0) {
                vpvr_ticks_per_row_ = std::max(0, vpvr_ticks_per_row_ - 5);
                vpvr_last_request_start_ = 0;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+##vptpr")) {
                vpvr_ticks_per_row_ += 5;
                vpvr_last_request_start_ = 0;
            }
            ImGui::SetNextItemWidth(60);
            int vptpr = vpvr_ticks_per_row_;
            if (ImGui::InputInt("##vptpr_input", &vptpr, 0, 0)) {
                vpvr_ticks_per_row_ = std::max(0, vptpr);
                vpvr_last_request_start_ = 0;
            }
            if (ImGui::IsItemHovered()) {
                Theme::tooltip("Price levels per row (0 = auto)");
            }
        }
        vpvr_settings_panel_.end();
    }
    // ── Footprint settings (always available) ──────────────────────────
    render_footprint_settings_popup();
    // ── TPO settings ────────────────────────────────────────────────────
    tpo_settings_panel_.set_panel_height(300.0f);
    if (tpo_settings_panel_.begin()) {
        auto& tpo = ctx_.tpo_mgr();
        if (tpo_settings_panel_.tab("General")) {
            const char* periods[] = { "Daily (24h)", "Weekly (168h)" };
            int period_idx = (tpo.session_period_hours == 168) ? 1 : 0;
            ImGui::SetNextItemWidth(150);
            if (ImGui::Combo("Session Period", &period_idx, periods, 2)) {
                tpo.session_period_hours = (period_idx == 1) ? 168 : 24;
                tpo.clear(pair_.symbol);
            }
            ImGui::Checkbox("Session Header", &tpo.show_session_header);
            ImGui::Checkbox("Highlight Start/End", &tpo.highlight_start_end);
            ImGui::SetNextItemWidth(60);
            ImGui::InputInt("Profile Spacing", &tpo.profile_spacing, 1, 5);
            tpo.profile_spacing = std::max(0, std::min(20, tpo.profile_spacing));
        }
        if (tpo_settings_panel_.tab("Session")) {
            ImGui::Text("Ticks Per Row");
            ImGui::SameLine();
            if (tpo.ticks_per_row_setting == 0) {
                ImGui::TextDisabled("(auto)");
            } else if (tick_size_ > 0) {
                char pl[32];
                snprintf(pl, sizeof(pl), "($%.1f)", tpo.ticks_per_row_setting * tick_size_);
                ImGui::TextDisabled("%s", pl);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("-##tpotpr") && tpo.ticks_per_row_setting > 0) {
                tpo.ticks_per_row_setting = std::max(0, tpo.ticks_per_row_setting - 50);
                tpo.clear(pair_.symbol);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+##tpotpr")) {
                tpo.ticks_per_row_setting += 50;
                tpo.clear(pair_.symbol);
            }
            ImGui::SetNextItemWidth(80);
            int tpr = tpo.ticks_per_row_setting;
            if (ImGui::InputInt("##tpotpr_input", &tpr, 0, 0)) {
                tpo.ticks_per_row_setting = std::max(0, tpr);
                tpo.clear(pair_.symbol);
            }
            ImGui::Spacing();
            ImGui::Checkbox("POC Ray", &tpo.show_poc_ray);
            ImGui::Checkbox("VAH/VAL Rays", &tpo.show_vah_val_rays);
            ImGui::Checkbox("Poor High/Low", &tpo.show_poor_high_low);
            ImGui::Checkbox("Initial Balance", &tpo.show_initial_balance);
        }
        if (tpo_settings_panel_.tab("Value Area")) {
            ImGui::SetNextItemWidth(80);
            ImGui::SliderFloat("Value Area %", &tpo.value_area_pct,
                               0.50f, 0.90f, "%.2f");
            if (ImGui::IsItemHovered()) {
                Theme::tooltip("Percentage of total blocks for value area\n"
                                  "Standard: 0.70 (70%%)");
            }
        }
        if (tpo_settings_panel_.tab("Single Prints")) {
            ImGui::Checkbox("Show Single Prints", &tpo.show_single_prints);
            if (ImGui::IsItemHovered()) {
                Theme::tooltip("Highlight rows with exactly 1 block\n"
                                  "where adjacent rows have >1 block.\n"
                                  "Indicates inefficient price movement.");
            }
        }
        tpo_settings_panel_.end();
    }
    // Bottom hairline (line-1) against the chart, then drop the layout cursor to
    // the bar's bottom so the chart begins directly below the 44px band.
    dl->AddLine(ImVec2(bp.x, bp.y + bar_h - 0.5f), ImVec2(bp.x + ww, bp.y + bar_h - 0.5f),
                Theme::u32(Theme::Tokens::BD1), 1.0f);
    // Claim the band as a real item BEFORE dropping the cursor to its bottom.
    // The bar is painted straight to the window draw list, so nothing has grown
    // the window's content extent over it, and the cursor move below is then a
    // bare SetCursorScreenPos past CursorMaxPos. ImGui only tolerates that while
    // some LATER item grows the boundary; when none does, End() fires "Code uses
    // SetCursorPos()/SetCursorScreenPos() to extend window/parent boundaries".
    // That happens whenever this window is begun twice in one frame (two chart
    // widgets sharing "###chart_<ex>_<sym>"): the second BeginChild/EndChild
    // pair sees BeginCount != 1 and skips the ItemSize() that would have cleared
    // DC.IsSetPos. The Dummy makes the extent honest, so the cursor move is a
    // move inside known bounds rather than a request to extend them.
    ImGui::SetCursorScreenPos(bp);
    ImGui::Dummy(ImVec2(ww, bar_h));
    ImGui::SetCursorScreenPos(ImVec2(bp.x, bp.y + bar_h));

    ImGui::PopStyleVar(2);
}

void ChartWidget::handle_plot_interaction() {
    // While the drawing layer owns the mouse (armed tool, placement, drag, or
    // hover over a drawing), the chart's own click/drag handlers stand down.
    const bool draw_cap = drawing_layer_.captures_mouse();

    // Capture the chart plot rect EVERY frame, not only when the chart is
    // hovered: the indicator subplots reuse first_plot_min/last_plot_max for
    // the shared crosshair, and hovering a pane while these still held the
    // frame-start reset (0,0) dragged the vertical line to the window's
    // left edge. render_tabbed overwrites last_plot_max with the bottom
    // pane's rect right after this.
    crosshair_state_.first_plot_min = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    crosshair_state_.first_plot_max = ImVec2(
        crosshair_state_.first_plot_min.x + plot_size.x,
        crosshair_state_.first_plot_min.y + plot_size.y
    );
    crosshair_state_.last_plot_max = crosshair_state_.first_plot_max;
    ui_test::mark_rect("chart.plot", crosshair_state_.first_plot_min, crosshair_state_.first_plot_max);
    if (ui_test::enabled()) {
        ui_test::mark_rect("chart.market." + pair_.exchange + "." + pair_.symbol + "." +
                           std::to_string(chart_instance_), crosshair_state_.first_plot_min,
                           crosshair_state_.first_plot_max);
    }

    // Acquire at the click's origin within this visible window, independently
    // of ImPlot's hover bookkeeping. The cursor may already have moved since
    // the press by the time this frame is rendered.
    const ImVec2 press = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left];
    if (!draw_cap && ImGui::GetIO().KeyShift &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        press.x >= crosshair_state_.first_plot_min.x && press.x <= crosshair_state_.first_plot_max.x &&
        press.y >= crosshair_state_.first_plot_min.y && press.y <= crosshair_state_.first_plot_max.y) {
        replay_selection_.active = true;
        replay_selection_.dragged = false;
        replay_selection_.cancelled = false;
        replay_selection_.press_ms = static_cast<int64_t>(ImPlot::PixelsToPlot(press).x);
    }

    // Finish even outside the plot, and keep ownership if Shift is released
    // before the mouse. Clamp the endpoint to the visible chart boundary.
    if (replay_selection_.active) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const float x = std::clamp(mouse.x, crosshair_state_.first_plot_min.x,
                                   crosshair_state_.first_plot_max.x);
        if (!replay_selection_.cancelled &&
            (replay_selection_.dragged || ImGui::IsMouseDragging(ImGuiMouseButton_Left))) {
            replay_selection_.dragged = true;
            replay_selection_.start_ms = replay_selection_.press_ms;
            replay_selection_.end_ms = static_cast<int64_t>(
                ImPlot::PixelsToPlot(ImVec2(x, mouse.y)).x);
            candles().set_follow_live(false);
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            replay_selection_.active = false;
            if (chart_type_ == ChartType::FlowPositioning && replay_selection_.dragged) {
                const int64_t start = (std::min(replay_selection_.start_ms,replay_selection_.end_ms)+59999)/60000*60000;
                const int64_t end = std::max(replay_selection_.start_ms,replay_selection_.end_ms)/60000*60000;
                if (end > start) { replay_selection_.start_ms=start; replay_selection_.end_ms=end; }
            }
        }
    }

    const bool popup_open = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    // Menu/dialog Escape closes that surface first. A selected chart consumes
    // the press so it never also stops the replay in the later shell pass.
    if ((replay_selection_.active || replay_selection_.start_ms != replay_selection_.end_ms) &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::GetIO().WantTextInput &&
        !ctx_.drawing_mgr().escape_consumed(ImGui::GetFrameCount()) &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        selection_escape_frame_ = ImGui::GetFrameCount();
        if (!draw_cap && !popup_open && !selection_popup_was_open_) {
            replay_selection_.start_ms = replay_selection_.end_ms = 0;
            replay_selection_.cancelled = true;
        }
    }
    selection_popup_was_open_ = popup_open;

    if (replay_selection_.start_ms != replay_selection_.end_ms) {
        const float a = ImPlot::PlotToPixels(
            static_cast<double>(replay_selection_.start_ms), 0).x;
        const float b = ImPlot::PlotToPixels(
            static_cast<double>(replay_selection_.end_ms), 0).x;
        ImPlot::PushPlotClipRect();
        ImPlot::GetPlotDrawList()->AddRectFilled(
            ImVec2(std::min(a, b), crosshair_state_.first_plot_min.y),
            ImVec2(std::max(a, b), crosshair_state_.first_plot_max.y),
            Theme::u32(Theme::Tokens::BRAND, 0.15f));
        ImPlot::PopPlotClipRect();
    }

    if (ImPlot::IsPlotHovered()) {
        crosshair_state_.is_active = true;
        crosshair_state_.plot_pos = ImPlot::GetPlotMousePos();
        crosshair_state_.chart_hovered = true;
        crosshair_state_.indicator_hovered = false;

        crosshair_state_.hovered_plot_min = crosshair_state_.first_plot_min;
        crosshair_state_.hovered_plot_max = crosshair_state_.first_plot_max;

        const ImPlotRect limits = ImPlot::GetPlotLimits();
        crosshair_state_.hovered_y_min = limits.Y.Min;
        crosshair_state_.hovered_y_max = limits.Y.Max;

        // Panning owns history inspection. Wheel zoom keeps an existing follow
        // latch; zoom out may resume it only while the display is running.
        const float wheel = ImGui::GetIO().MouseWheel;
        if (!draw_cap && ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
            (!rt_mode_ || realtime_pan_detaches(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x,
                                                ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y))) {
            candles().set_follow_live(false);
            if (rt_mode_) rt_auto_fit_.navigate();
        } else if (!draw_cap && rt_mode_ && wheel != 0) {
            const auto zoom = realtime_zoom(limits.X.Size(), wheel,
                ImPlot::GetInputMap().ZoomRate, candles().follow_live(),
                rt_paused_ || ctx_.replay_mgr().is_paused());
            rt_span_ms_ = zoom.span_ms;
            candles().set_follow_live(zoom.follow);
            if (rt_auto_fit_history_) rt_auto_price_ = true;
            rt_auto_fit_.navigate();   // the new window's history fits at once
        } else if (!draw_cap && wheel != 0) {
            candles().set_follow_live(false);
        }

        // ─── R key: replay from crosshair → focused view ───────────────
        if (ImGui::IsKeyPressed(ImGuiKey_R, false) &&
            !ImGui::GetIO().WantTextInput &&
            !ctx_.replay_mgr().is_active()) {
            // Snap to candle boundary so replay starts at exact candle start
            int64_t raw_time = static_cast<int64_t>(crosshair_state_.plot_pos.x);
            int64_t tf_ms = candles().timeframe_seconds() * 1000;
            int64_t cursor_time_ms = (raw_time / tf_ms) * tf_ms;
            if (cursor_time_ms > 0)
                ctx_.replay_mgr().open_focused_replay_at(pair_.exchange, pair_.symbol, cursor_time_ms);
        }

        // ─── Right-click → open context menu ────────────────────
        // In TPO mode, right-click is handled by render_tpo() for session context
        if (!draw_cap && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
            chart_type_ != ChartType::TPO) {
            // Capture timestamp NOW while plot is still hovered.
            // Once the popup opens, ImPlot no longer reports hovered and
            // crosshair_state_ gets zeroed on the next frame.
            // Snap to candle boundary (floor) so replay starts at exact candle start
            {
                int64_t raw_time = static_cast<int64_t>(crosshair_state_.plot_pos.x);
                int64_t tf_ms = candles().timeframe_seconds() * 1000;
                context_menu_time_ms_ = (raw_time / tf_ms) * tf_ms;
                // Research reads a MINUTE: floor the SAME capture to 60000, in
                // this block, while the plot is still hovered. Reusing the
                // timeframe floor above would research a minute up to hours
                // away from the click on a 4H chart (the spec's named trap).
                context_menu_minute_ms_ = research_url::floor_minute_ms(raw_time);
                context_menu_price_ = crosshair_state_.plot_pos.y;
                // The shift-drag selection, read and snapped once here rather
                // than every frame the popup is open.
                context_menu_move_ = read_selected_move();
            }
            ImGui::OpenPopup("##ChartCtx");
        }
    }

    // ─── Measure ruler, price-alert lines + toast (drawn in the plot) ───────
    {
        ImDrawList* pdl = ImPlot::GetPlotDrawList();
        const ImVec2 p_min = ImPlot::GetPlotPos();
        const ImVec2 p_sz  = ImPlot::GetPlotSize();
        const float  p_r   = p_min.x + p_sz.x;
        const double x_mid = (x_axis_min_ + x_axis_max_) * 0.5;

        // Alert level lines (dashed) with a right-edge price pill + ✕ to remove.
        for (size_t i = 0; i < alerts_.size();) {
            const double ap = alerts_[i].price;
            const ImVec2 pt = ImPlot::PlotToPixels(ImPlotPoint(x_mid, ap));
            const float ly = pt.y;
            if (ly >= p_min.y && ly <= p_min.y + p_sz.y) {
                const ImU32 acol = Theme::u32(Theme::Tokens::WARN);
                for (float x = p_min.x; x < p_r - 58.0f; x += 8.0f)
                    pdl->AddLine(ImVec2(x, ly), ImVec2(std::min(x + 4.0f, p_r - 58.0f), ly), acol, 1.0f);
                char apb[32]; fmt_.format_price(apb, sizeof(apb), ap);
                char lbl[40]; snprintf(lbl, sizeof(lbl), "\xe2\x97\x89 %s", apb);
                const ImVec2 ts = ImGui::CalcTextSize(lbl);
                const ImVec2 bx0(p_r - ts.x - 34.0f, ly - ts.y * 0.5f - 3.0f);
                const ImVec2 bx1(p_r - 16.0f, ly + ts.y * 0.5f + 3.0f);
                pdl->AddRectFilled(bx0, bx1, Theme::u32(Theme::Tokens::PANEL), 3.0f);
                pdl->AddRect(bx0, bx1, acol, 3.0f, 0, 1.0f);
                pdl->AddText(ImVec2(bx0.x + 6.0f, ly - ts.y * 0.5f), acol, lbl);
                const ImVec2 xc(p_r - 8.0f, ly);
                pdl->AddText(ImVec2(xc.x - 4.0f, ly - ts.y * 0.5f), Theme::u32(Theme::Tokens::TX3), "\xc3\x97");
                const bool over_x = ImGui::IsMouseHoveringRect(ImVec2(xc.x - 8.0f, ly - 8.0f),
                                                               ImVec2(xc.x + 8.0f, ly + 8.0f));
                if (over_x && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    alerts_.erase(alerts_.begin() + static_cast<long>(i));
                    continue;
                }
            }
            ++i;
        }

        // Measure ruler: anchor → cursor, live Δ readout. Click/Esc clears it.
        if (measure_.active) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyShift &&
                 ImPlot::IsPlotHovered())) {
                measure_.active = false;
            } else {
                const double t1 = crosshair_state_.plot_pos.x;
                const double q1 = crosshair_state_.plot_pos.y;
                const double dprice = q1 - measure_.p0;
                const double dpct = measure_.p0 != 0.0 ? (dprice / measure_.p0) * 100.0 : 0.0;
                const int64_t tf_ms = candles().timeframe_seconds() * 1000;
                const int64_t dt_ms = std::llabs(static_cast<int64_t>(t1) - measure_.t0_ms);
                const long bars = tf_ms > 0 ? static_cast<long>(dt_ms / tf_ms) : 0;
                const ImVec2 a = ImPlot::PlotToPixels(ImPlotPoint((double)measure_.t0_ms, measure_.p0));
                const ImVec2 b = ImPlot::PlotToPixels(ImPlotPoint(t1, q1));
                const bool up = dprice >= 0.0;
                const ImU32 col  = up ? Theme::u32(Theme::Tokens::UP) : Theme::u32(Theme::Tokens::DOWN);
                const ImU32 fill = Theme::u32(up ? Theme::Tokens::UP : Theme::Tokens::DOWN, 34.0f / 255.0f);
                const ImVec2 r0(std::min(a.x, b.x), std::min(a.y, b.y));
                const ImVec2 r1(std::max(a.x, b.x), std::max(a.y, b.y));
                pdl->AddRectFilled(r0, r1, fill);
                pdl->AddRect(r0, r1, col, 0.0f, 0, 1.0f);
                char l1[64], l2[72], pb[32];
                fmt_.format_price(pb, sizeof(pb), std::fabs(dprice));
                snprintf(l1, sizeof(l1), "%s%.2f%%  %s%s", up ? "+" : "-", std::fabs(dpct),
                         up ? "+" : "-", pb);
                const long long s = static_cast<long long>(dt_ms / 1000);
                snprintf(l2, sizeof(l2), "%ld bars  \xc2\xb7  %02lld:%02lld:%02lld", bars,
                         s / 3600, (s % 3600) / 60, s % 60);
                const ImVec2 ts1 = ImGui::CalcTextSize(l1);
                const ImVec2 ts2 = ImGui::CalcTextSize(l2);
                const float bw = std::max(ts1.x, ts2.x) + 16.0f;
                const float bh = ts1.y + ts2.y + 12.0f;
                ImVec2 lb(b.x + 12.0f, b.y - bh - 8.0f);
                if (lb.x + bw > p_r) lb.x = b.x - bw - 12.0f;
                if (lb.y < p_min.y) lb.y = b.y + 12.0f;
                pdl->AddRectFilled(lb, ImVec2(lb.x + bw, lb.y + bh), Theme::u32(Theme::Tokens::PANEL), 4.0f);
                pdl->AddRect(lb, ImVec2(lb.x + bw, lb.y + bh), col, 4.0f, 0, 1.0f);
                pdl->AddText(ImVec2(lb.x + 8.0f, lb.y + 5.0f), col, l1);
                pdl->AddText(ImVec2(lb.x + 8.0f, lb.y + 5.0f + ts1.y + 2.0f),
                             Theme::u32(Theme::Tokens::TX2), l2);
            }
        }

        // Alert toast (top-center of the plot, ~6s).
        if (alert_toast_until_ > ImGui::GetTime() && !alert_toast_msg_.empty()) {
            const ImVec2 ts = ImGui::CalcTextSize(alert_toast_msg_.c_str());
            const float bw = ts.x + 44.0f, bh = ts.y + 14.0f;
            const ImVec2 t0(p_min.x + (p_sz.x - bw) * 0.5f, p_min.y + 12.0f);
            pdl->AddRectFilled(t0, ImVec2(t0.x + bw, t0.y + bh), Theme::u32(Theme::Tokens::ELEV), 5.0f);
            pdl->AddRect(t0, ImVec2(t0.x + bw, t0.y + bh), Theme::u32(Theme::Tokens::WARN), 5.0f, 0, 1.0f);
            pdl->AddCircleFilled(ImVec2(t0.x + 16.0f, t0.y + bh * 0.5f), 3.5f, Theme::u32(Theme::Tokens::WARN));
            pdl->AddText(ImVec2(t0.x + 28.0f, t0.y + 7.0f), Theme::u32(Theme::Tokens::TX1),
                         alert_toast_msg_.c_str());
        }
    }

    // ─── TradingView-style axis scaling (outside the IsPlotHovered scope: the
    // axis gutters sit OUTSIDE the plot rect, so IsPlotHovered is false exactly
    // where this gesture lives). A drag that starts on the TIME axis
    // stretches/shrinks the visible span; on the PRICE axis it stretches/
    // shrinks the price range. Real-time only: the candle view refits its Y
    // from visible candles every frame, and with indicator panes open the
    // strip below the plot belongs to them. The gutters are geometric -
    // ImPlot's own axis HoverRect collapses for the pane-owned x axis. The
    // gesture latches on the axis mouse-down so the input map can suppress
    // ImPlot's axis pan before the next BeginPlot.
    if (!draw_cap && !replay_selection_.active && rt_mode_ && chart_type_ != ChartType::TPO) {
        const ImVec2 ppos = ImPlot::GetPlotPos();
        const ImVec2 psz = ImPlot::GetPlotSize();
        const ImVec2 mouse_pos = ImGui::GetIO().MousePos;
        const bool cvd_lane_open = realtime_cvd_lane_height() > 0;
        const bool x_gutter = !cvd_lane_open &&
            mouse_pos.y >= ppos.y + psz.y - 2.0f && mouse_pos.y <= ppos.y + psz.y + 40.0f &&
            mouse_pos.x >= ppos.x - 6.0f && mouse_pos.x <= ppos.x + psz.x + 6.0f;
        const bool y_gutter =
            mouse_pos.x >= ppos.x + psz.x - 2.0f && mouse_pos.x <= ppos.x + psz.x + 82.0f &&
            mouse_pos.y >= ppos.y - 6.0f && mouse_pos.y <= ppos.y + psz.y + 6.0f;
        if (rt_axis_scale_axis_ < 0) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (x_gutter || ImPlot::IsAxisHovered(ImAxis_X1)) {
                    rt_axis_scale_axis_ = ImAxis_X1;
                    rt_axis_scale_grab_px_ = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left];
                    rt_axis_scale_anchor_ = ImPlot::PixelsToPlot(rt_axis_scale_grab_px_).x;
                    const ImPlotRange xg = ImPlot::GetPlotLimits().X;
                    rt_axis_scale_grab_span_ = rt_mode_ ? rt_span_ms_ : xg.Size();
                    rt_axis_scale_grab_min_ = xg.Min;
                    rt_axis_scale_grab_max_ = xg.Max;
                    // A detached view scales about the grab point; a following
                    // view keeps its latch (wheel zoom does too).
                    if (!rt_mode_) candles().set_follow_live(false);
                } else if (y_gutter || ImPlot::IsAxisHovered(ImAxis_Y1)) {
                    rt_axis_scale_axis_ = ImAxis_Y1;
                    rt_axis_scale_grab_px_ = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left];
                    rt_axis_scale_anchor_ = ImPlot::PixelsToPlot(rt_axis_scale_grab_px_).y;
                    const ImPlotRange yg = ImPlot::GetPlotLimits().Y;
                    rt_axis_scale_grab_min_ = yg.Min;
                    rt_axis_scale_grab_max_ = yg.Max;
                    // Same detachment a deliberate price drag causes, plus a
                    // release from the RT price window so its rows*step clamp
                    // cannot snap the stretched range back mid-gesture.
                    if (rt_auto_price_) rt_auto_price_ = false;
                    rt_price_free_ = true;
                    rt_auto_fit_.navigate();
                }
            }
        } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            rt_axis_scale_axis_ = -1;
        } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            ImPlotPlot* plot = ImPlot::GetCurrentPlot();
            if (plot) {
                if (rt_axis_scale_axis_ == ImAxis_X1) {
                    const float dx = mouse_pos.x - rt_axis_scale_grab_px_.x;
                    if (std::abs(dx) >= 0.5f) {
                        // Drag right = stretch the ruler = zoom in. The factor
                        // maps the TOTAL displacement from the grab, so the
                        // result is idempotent per frame (frame-rate free).
                        const double factor = std::exp(-double(dx) / 260.0);
                        const double span = std::clamp(rt_axis_scale_grab_span_ * factor,
                            5000.0, double(RealtimeArchive::target_ms) / 0.88);
                        if (rt_mode_) {
                            rt_span_ms_ = span;
                            if (!candles().follow_live()) {
                                // Detached: scale the grab range about the grab
                                // point so the history under it stays put.
                                const double a = std::clamp(rt_axis_scale_anchor_,
                                    rt_axis_scale_grab_min_, rt_axis_scale_grab_max_);
                                double mn = a - (a - rt_axis_scale_grab_min_) * factor;
                                double mx = a + (rt_axis_scale_grab_max_ - a) * factor;
                                const double proj = realtime_projection_until(rt_clock_ms_, span);
                                if (mx > proj) { mn -= mx - proj; mx = proj; }
                                if (mx > mn) ImPlot::GetCurrentPlot()->Axes[ImAxis_X1].SetRange(mn, mx);
                            }
                        } else {
                            const double a = std::clamp(rt_axis_scale_anchor_,
                                rt_axis_scale_grab_min_, rt_axis_scale_grab_max_);
                            const double mn = a - (a - rt_axis_scale_grab_min_) * factor;
                            const double mx = a + (rt_axis_scale_grab_max_ - a) * factor;
                            if (mx > mn) ImPlot::GetCurrentPlot()->Axes[ImAxis_X1].SetRange(mn, mx);
                        }
                    }
                } else {
                    const float dy = mouse_pos.y - rt_axis_scale_grab_px_.y;
                    if (std::abs(dy) >= 0.5f) {
                        // Drag down = pull the scale apart = zoom out.
                        const double factor = std::exp(double(dy) / 260.0);
                        const double a = std::clamp(rt_axis_scale_anchor_,
                            rt_axis_scale_grab_min_, rt_axis_scale_grab_max_);
                        const double mn = a - (a - rt_axis_scale_grab_min_) * factor;
                        const double mx = a + (rt_axis_scale_grab_max_ - a) * factor;
                        if (mx > mn) ImPlot::GetCurrentPlot()->Axes[ImAxis_Y1].SetRange(mn, mx);
                    }
                }
            }
        }
        // Cursor affordance on the axis gutters.
        if (rt_axis_scale_axis_ < 0) {
            if (x_gutter) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            else if (y_gutter) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
    }

    // ─── Context menu popup (must be outside IsPlotHovered block) ────
    // Grouped as chart tools, research reads, replay, copy. The menu sizes to
    // its rows (it was pinned at 450px so the replay datetime would fit).
    if (Theme::begin_menu("##ChartCtx", 280.0f)) {
        using drawing::UiIcon;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            selection_escape_frame_ = ImGui::GetFrameCount();
            ImGui::CloseCurrentPopup();
        }

        const bool replaying = ctx_.replay_mgr().is_active();
        char price_buf[32];
        fmt_.format_price(price_buf, sizeof(price_buf), context_menu_price_);

        // Add alert - live chart only (an alert on a historical replay is meaningless).
        if (!replaying) {
            if (Theme::menu_item(UiIcon::Bell, "Add alert", price_buf)) {
                PriceAlert al;
                al.price = context_menu_price_;
                al.arm_above = (context_menu_price_ > candles().last_close_price());
                alerts_.push_back(al);
                alert_last_price_ = candles().last_close_price();
#ifdef __EMSCRIPTEN__
                EM_ASM({
                    if (window.Notification && Notification.permission === 'default')
                        Notification.requestPermission();
                });
#endif
            }
        }

        if (Theme::menu_item(UiIcon::Anchor, "Anchor VWAP here")) {
            vwap_anchor_ms_ = context_menu_time_ms_; reference_update_time_ = -1;
        }
        // Measure ruler - anchor here; readout follows the cursor (click/Esc clears).
        if (Theme::menu_item(UiIcon::Ruler, "Measure from here")) {
            measure_.active = true;
            measure_.t0_ms  = context_menu_time_ms_;
            measure_.p0     = context_menu_price_;
        }

        Theme::menu_separator();
        // The research reads - READS, not replays, so they sit above the replay
        // block. Both use the MINUTE capture (context_menu_minute_ms_), never
        // the timeframe-floored replay timestamp beside it.
        render_investigate_menu_item(pair_, context_menu_minute_ms_, context_menu_move_.snap,
                                     context_menu_move_.range_minutes);

        if (replay_selection_.start_ms != replay_selection_.end_ms &&
            Theme::menu_item(UiIcon::Close, "Clear selection", "Esc")) {
            replay_selection_ = {};
            context_menu_move_ = {};
        }

        Theme::menu_separator();

        // Replay actions (entitlement-gated inside): Replay from here <time>,
        // Replay with..., Replay range, Stop replay Esc.
        ctx_.replay_mgr().render_chart_context_menu(
            pair_.exchange,
            pair_.symbol,
            context_menu_time_ms_,
            candles().timeframe_seconds(),
            replay_selection_.start_ms,
            replay_selection_.end_ms
        );

        Theme::menu_separator();

        if (Theme::menu_item(UiIcon::Copy, "Copy price", price_buf)) {
            ImGui::SetClipboardText(price_buf);
        }

        ImGui::EndPopup();
    }
}

// Crosshair

void ChartWidget::render_crosshair(const ImPlotPoint& mouse_pos) const {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const int64_t tf_sec = candles().timeframe_seconds();
    // Snap to nearest candle timestamp
    const double timeframe_ms = chart_type_ == ChartType::FlowPositioning ? 60000.0 : static_cast<double>(tf_sec) * 1000.0;
    const double snapped_time = rt_mode_ ? mouse_pos.x : std::round(mouse_pos.x / timeframe_ms) * timeframe_ms;
    const float line_top = crosshair_state_.first_plot_min.y;
    const float line_bottom = crosshair_state_.last_plot_max.y;
    const double x_range = x_axis_max_ - x_axis_min_;
    if (x_range <= 0) return;
    const double x_normalized = (snapped_time - x_axis_min_) / x_range;
    const float plot_width = crosshair_state_.last_plot_max.x - crosshair_state_.first_plot_min.x;
    const float snap_x = crosshair_state_.first_plot_min.x + static_cast<float>(x_normalized * plot_width);
    // Y position based on hovered plot
    float mouse_y = 0.0f;
    double y_range = crosshair_state_.hovered_y_max - crosshair_state_.hovered_y_min;
    if (y_range > 0) {
        const double y_normalized = (mouse_pos.y - crosshair_state_.hovered_y_min) / y_range;
        const float plot_height = crosshair_state_.hovered_plot_max.y - crosshair_state_.hovered_plot_min.y;
        mouse_y = crosshair_state_.hovered_plot_max.y - static_cast<float>(y_normalized * plot_height);
    }
    ImU32 crosshair_color = IM_COL32(128, 128, 128, 180);
    float dash_length = 4.0f;
    float gap_length = 4.0f;
    // Dashes pass under the chart's chips (readout, status, layer keys) rather than through their text.
    // Vertical dashed line
    for (float y = line_top; y < line_bottom; y += dash_length + gap_length) {
        float y_end = std::min(y + dash_length, line_bottom);
        if (Theme::chart_chip_covers(ImVec2(snap_x, (y + y_end) * 0.5f))) continue;
        draw_list->AddLine(ImVec2(snap_x, y), ImVec2(snap_x, y_end), crosshair_color, 1.0f);
    }
    // Horizontal dashed line (within hovered plot only)
    float h_left = crosshair_state_.hovered_plot_min.x;
    float h_right = crosshair_state_.hovered_plot_max.x;
    for (float x = h_left; y_range > 0 && x < h_right; x += dash_length + gap_length) {
        float x_end = std::min(x + dash_length, h_right);
        if (Theme::chart_chip_covers(ImVec2((x + x_end) * 0.5f, mouse_y))) continue;
        draw_list->AddLine(ImVec2(x, mouse_y), ImVec2(x_end, mouse_y), crosshair_color, 1.0f);
    }
    // Price label on Y-axis (main chart)
    if (crosshair_state_.chart_hovered && y_range > 0) {
        char price_label[80];
        fmt_.format_price(price_label, sizeof(price_label), mouse_pos.y);
        if (std::isfinite(cursor_reference_price_) && cursor_reference_price_ > 0 && std::isfinite(mouse_pos.y)) {
            const size_t used = std::strlen(price_label);
            const double percent = (mouse_pos.y / cursor_reference_price_ - 1.0) * 100.0;
            snprintf(price_label + used, sizeof(price_label) - used, "\n%+.2f%%", percent);
        }
        // Crosshair tags (design v3, 2026-10-11): one solid rounded chip, no hard border; the change from the
        // current price on a second line in its sign colour.
        const char* nl = std::strchr(price_label, '\n');
        const ImVec2 p_size = ImGui::CalcTextSize(price_label, nl);
        ImVec2 c_size(0, 0);
        if (nl) c_size = ImGui::CalcTextSize(nl + 1);
        constexpr float px = 7.0f, py = 4.0f;
        const float w = std::max(p_size.x, c_size.x) + px * 2, h = p_size.y + (nl ? c_size.y + 1.0f : 0.0f) + py * 2;
        const ImVec2 price_bg_min(crosshair_state_.hovered_plot_max.x + 2.0f,
            std::clamp(mouse_y - h * 0.5f, crosshair_state_.hovered_plot_min.y,
                       std::max(crosshair_state_.hovered_plot_min.y, crosshair_state_.hovered_plot_max.y - h)));
        const ImVec2 price_bg_max(price_bg_min.x + w, price_bg_min.y + h);
        draw_list->AddRectFilled(price_bg_min, price_bg_max, Theme::u32(Theme::Tokens::CONTROL_ACTIVE), Theme::Radius::R1);
        draw_list->AddText(ImVec2(price_bg_min.x + px, price_bg_min.y + py), Theme::u32(Theme::Tokens::TX1), price_label, nl);
        if (nl) {
            const double percent = (mouse_pos.y / cursor_reference_price_ - 1.0) * 100.0;
            draw_list->AddText(ImVec2(price_bg_min.x + px, price_bg_min.y + py + p_size.y + 1.0f),
                               Theme::u32(percent > 0 ? Theme::Tokens::UP : percent < 0 ? Theme::Tokens::DOWN : Theme::Tokens::TX2), nl + 1);
        }
    }

    // Y-axis value label on indicator subplots
    if (crosshair_state_.indicator_hovered && y_range > 0) {
        char val_label[32];
        // Use the hovered indicator's own Y-axis formatter if available
        if (crosshair_state_.indicator_y_formatter) {
            crosshair_state_.indicator_y_formatter(mouse_pos.y, val_label, sizeof(val_label), nullptr);
        } else {
            // Fallback: generic abbreviated format
            const double abs_val = std::abs(mouse_pos.y);
            if (abs_val >= 1e9)       snprintf(val_label, sizeof(val_label), "%.2fB", mouse_pos.y / 1e9);
            else if (abs_val >= 1e6)  snprintf(val_label, sizeof(val_label), "%.2fM", mouse_pos.y / 1e6);
            else if (abs_val >= 1e3)  snprintf(val_label, sizeof(val_label), "%.1fK", mouse_pos.y / 1e3);
            else                      snprintf(val_label, sizeof(val_label), "%.1f", mouse_pos.y);
        }
        const ImVec2 val_text_size = ImGui::CalcTextSize(val_label);
        constexpr float px = 7.0f, py = 4.0f;
        const ImVec2 val_bg_min(crosshair_state_.hovered_plot_max.x + 2.0f, mouse_y - val_text_size.y * 0.5f - py);
        const ImVec2 val_bg_max(val_bg_min.x + val_text_size.x + px * 2, mouse_y + val_text_size.y * 0.5f + py);
        draw_list->AddRectFilled(val_bg_min, val_bg_max, Theme::u32(Theme::Tokens::CONTROL_ACTIVE), Theme::Radius::R1);
        draw_list->AddText(ImVec2(val_bg_min.x + px, val_bg_min.y + py), Theme::u32(Theme::Tokens::TX1), val_label);
    }

    // Time label on X-axis: the weekday and date, then the time (seconds only on sub-minute charts).
    char time_label[64], date_part[32], clock_part[24];
    const int64_t t_ms = static_cast<int64_t>(snapped_time);
    const bool sub_minute = candles().timeframe_seconds() < 60;
    if (DisplayTimeZone::instance().format(t_ms, TimeZoneFormat::WeekdayMonthDay, date_part, sizeof(date_part)) &&
        DisplayTimeZone::instance().format(t_ms, sub_minute ? TimeZoneFormat::TimeSeconds : TimeZoneFormat::TimeMinutes,
                                           clock_part, sizeof(clock_part))) {
        snprintf(time_label, sizeof(time_label), "%s  %s", date_part, clock_part);
    } else if (!DisplayTimeZone::instance().format(t_ms, TimeZoneFormat::DateTimeSeconds, time_label, sizeof(time_label))) {
        snprintf(time_label, sizeof(time_label), "Invalid Time");
    }

    const ImVec2 time_text_size = ImGui::CalcTextSize(time_label);
    constexpr float t_px = 8.0f, t_py = 4.0f;
    ImVec2 time_bg_min(snap_x - time_text_size.x * 0.5f - t_px, line_bottom + 4.0f);
    ImVec2 time_bg_max(snap_x + time_text_size.x * 0.5f + t_px, line_bottom + 4.0f + time_text_size.y + t_py * 2);
    // Keep the whole chip inside the plots, shifting rather than squashing it.
    const float left_lim = crosshair_state_.first_plot_min.x + 2.0f, right_lim = crosshair_state_.last_plot_max.x - 2.0f;
    if (time_bg_min.x < left_lim) { time_bg_max.x += left_lim - time_bg_min.x; time_bg_min.x = left_lim; }
    if (time_bg_max.x > right_lim) { time_bg_min.x -= time_bg_max.x - right_lim; time_bg_max.x = right_lim; }
    draw_list->AddRectFilled(time_bg_min, time_bg_max, Theme::u32(Theme::Tokens::CONTROL_ACTIVE), Theme::Radius::R1);
    draw_list->AddText(ImVec2((time_bg_min.x + time_bg_max.x - time_text_size.x) * 0.5f, time_bg_min.y + t_py),
                       Theme::u32(Theme::Tokens::TX1), time_label);
}


int64_t ChartWidget::depth_timeframe_seconds() const {
    // One sampled book column per candle slot. Zoom must not clear/reload depth
    // by switching sampling cadence under a fixed candle timeframe.
    return candles().timeframe_seconds();
}

void ChartWidget::request_heatmap_data() {
    const int64_t tf_sec = depth_timeframe_seconds();
    if (heatmap_data_requested_ && heatmap_loaded_timeframe_ == tf_sec) return;
    const auto requested_at = std::chrono::steady_clock::now();
    if (heatmap_data_requested_ && requested_at - last_heatmap_rebuild_ < std::chrono::seconds(2)) return;
    // Heatmap delivery is timeframe-independent on the server. Keep it in the
    // managed registry so reconnect and replay pause/resume restore it.
    if (!heatmap_stream_mgr_) {
        heatmap_stream_mgr_ = &ctx_.stream_mgr();
        heatmap_stream_mgr_->subscribe_direct({pair_, Terminal::Stream::Heatmap, 0}, this);
    }

    // During replay, use replay start time as anchor instead of wall clock
    int64_t anchor_ms;
    if (ctx_.candle_mgr().replay_start_time_ms() > 0) {
        anchor_ms = ctx_.candle_mgr().replay_start_time_ms();
    } else {
        const auto now = std::chrono::system_clock::now();
        anchor_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();
    }

    if (last_visible_range_.X.Size() > 0)
        anchor_ms = std::min(anchor_ms, static_cast<int64_t>(last_visible_range_.X.Max));
    // Bound decoded observations at the selected candle cadence.
    const int64_t lookback_ms = std::min({
        candles().timeframe_seconds() * 1000LL * 500,
        tf_sec * 1000LL * 2000,
        5LL * 24 * 60 * 60 * 1000
    });
    const int64_t start_time_ms = anchor_ms - lookback_ms;
    ctx_.heatmap_mgr().set_timeframe(pair_, heatmap_mode_, tf_sec);
    ctx_.stream_mgr().request_historical_heatmap(
        pair_, heatmap_mode_, start_time_ms, anchor_ms, tf_sec
    );
    heatmap_data_requested_ = true;
    heatmap_loaded_timeframe_ = tf_sec;
    last_heatmap_rebuild_ = requested_at;
}

void ChartWidget::update_heatmap() {
    if (!heatmap_enabled_ || candles().empty()) return;

    const ImPlotRect limits = last_visible_range_;
    if (limits.X.Size() <= 0) return;
    if (heatmap_loaded_timeframe_ != depth_timeframe_seconds()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_heatmap_rebuild_ < std::chrono::seconds(2)) return;
        request_heatmap_data();
        last_heatmap_rebuild_ = now;
        return;
    }

    auto* reconstructor = ctx_.heatmap_mgr().get_reconstructor(pair_, heatmap_mode_);
    if (!reconstructor) return;

    const int64_t available_min = reconstructor->get_min_time();
    const int64_t available_max = reconstructor->get_max_time();
    if (available_min == 0 || available_max == 0) return;

    const auto display_time_start = static_cast<int64_t>(limits.X.Min);
    const auto display_time_end = static_cast<int64_t>(limits.X.Max);

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    constexpr int64_t threshold_ms = 30LL * 60 * 1000;

    // Zoom-aware threshold: when zoomed out far, the 30-minute threshold
    // is too small - the viewport edge can be hours past the data boundary.
    // Scale threshold to 20% of visible range or 30 min, whichever is larger.
    const int64_t vis_span = display_time_end - display_time_start;
    const int64_t dynamic_threshold = std::max(threshold_ms, vis_span / 5);

    // Max lookback per timeframe to keep memory bounded.
    const int64_t tf_ms_ob = candles().timeframe_seconds() * 1000LL;
    const int64_t max_lookback_ob = (tf_ms_ob <= 60000)
        ? 3LL * 24 * 60 * 60 * 1000    // 3 days for 1m
        : 10LL * 24 * 60 * 60 * 1000;  // 10 days for 5m+
    const int64_t earliest_ob = now_ms - max_lookback_ob;

    bool missing_left = (display_time_start < available_min + dynamic_threshold)
                        && (available_min > earliest_ob);
    bool missing_right = (display_time_end > available_max - dynamic_threshold)
                         && (available_max < now_ms - 60000);

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_heatmap_rebuild_).count();

    // Faster cooldown, but prevent duplicate requests for the same range
    if ((missing_left || missing_right) && elapsed > 2000) {
        // An old minute-detail window cannot be prepended to a full recent
        // timeline: oldest-first eviction would immediately discard the reply.
        // Re-anchor the bounded cache around the inspected viewport instead.
        if (display_time_end < available_min || display_time_start > available_max ||
            (missing_left && reconstructor->get_snapshot_count() >= 4000)) {
            ctx_.heatmap_mgr().clear(pair_, heatmap_mode_);
            heatmap_data_requested_ = false;
            request_heatmap_data();
            return;
        }
        const int64_t tf_sec = depth_timeframe_seconds();
        // Cap chunk at 1000 observations to stay within memory budget.
        // The timeline caps at 5000 entries, so loading 1000 at a time
        // fills progressively without wasting bandwidth on data that gets evicted.
        const int64_t chunk_candles = std::clamp(
            vis_span / (tf_sec * 1000LL), 500LL, 1000LL);
        const int64_t max_chunk = tf_sec * 1000LL * chunk_candles;
        int64_t req_start, req_end;

        if (missing_left) {
            req_end = available_min;
            req_start = std::max(req_end - max_chunk, earliest_ob);
        } else {
            req_start = available_max;
            req_end = std::min(req_start + max_chunk, now_ms);
        }

        // Skip tiny/zero-length requests
        if (req_end - req_start < tf_sec * 1000LL * 5) return;

        ctx_.stream_mgr().request_historical_heatmap(
            pair_, heatmap_mode_, req_start, req_end, tf_sec);
        last_heatmap_rebuild_ = now;
    }
}

void ChartWidget::update_live_heatmap_from_orderbook() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_heatmap_update_ms_
    ).count();
    if (elapsed_ms < HEATMAP_UPDATE_INTERVAL_MS) return;
    last_heatmap_update_ms_ = now;

    auto* reconstructor = ctx_.heatmap_mgr().get_reconstructor(pair_, heatmap_mode_);
    if (!reconstructor || !reconstructor->has_data()) return;

    // Skip expensive OB iteration when user has scrolled away from live.
    // The live column would be outside the viewport grid and silently dropped anyway.
    if (last_visible_range_.X.Max > 0) {
        auto now_ms = ctx_.replay_mgr().is_active()
            ? ctx_.replay_mgr().interpolated_time_ms()
            : std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        const auto visible_right = static_cast<int64_t>(last_visible_range_.X.Max);
        const int64_t tf_ms = candles().timeframe_seconds() * 1000LL;
        // If the right edge of viewport is more than 5 candles behind "now", skip
        if (now_ms - visible_right > tf_ms * 5) return;
    }

    const auto* orderbook = ctx_.ob_mgr().get_orderbook(pair_);
    if (!orderbook || !orderbook->is_synchronized()) return;

    std::unordered_map<double, float> price_qty;
    price_qty.reserve(orderbook->bids.size() + orderbook->asks.size());

    for (const auto& [price, qty] : orderbook->bids) {
        price_qty[price] += static_cast<float>(qty);
    }
    for (const auto& [price, qty] : orderbook->asks) {
        price_qty[price] += static_cast<float>(qty);
    }

    int64_t timestamp_ms = orderbook->timestamp_ms;
    if (timestamp_ms == 0) {
        timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
    // Deep outliers must not move the finite GPU row window off the market.
    const double center = !orderbook->bids.empty() && !orderbook->asks.empty()
        ? (orderbook->bids.begin()->first + orderbook->asks.begin()->first) * 0.5
        : orderbook->last_price;
    reconstructor->update_live_column(timestamp_ms, price_qty, center);
}

void ChartWidget::render_heatmap_tooltip() {
    auto* reconstructor = ctx_.heatmap_mgr().get_reconstructor(pair_, heatmap_mode_);
    if (!reconstructor || !ImPlot::IsPlotHovered()) return;

    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
    const double bucket_size = reconstructor->get_display_bucket_size();
    const int64_t timeframe_ms = reconstructor->get_column_interval_ms();
    if (bucket_size <= 0) return;

    const double center_price = std::floor(mouse.y / bucket_size) * bucket_size;
    // +half a step before flooring: heatmap columns are center-anchored on their bucket
    // time (see shader_heatmap_renderer data_time_start), so the cell under the cursor is
    // the bucket whose CENTER is nearest x, not the one whose left edge is left of x.
    const int64_t center_time = reconstructor->display_time_to_bucket(mouse.x);

    const float center_value = reconstructor->get_value_at_price_and_time(center_price, center_time);
    const auto coverage = reconstructor->candle_coverage(center_time);
    if (center_value < 0.001f && !coverage.timestamp_ms) return;

    Theme::begin_tooltip();
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6, 4));

    ImGui::Text("Sampled depth: %s", format_time_hms(coverage.timestamp_ms).c_str());
    ImGui::TextUnformatted("Loaded price range:");
    ImGui::SameLine(); ImGui::Text(fmt_.price_fmt, coverage.low);
    ImGui::SameLine(); ImGui::TextUnformatted("to");
    ImGui::SameLine(); ImGui::Text(fmt_.price_fmt, coverage.high);
    if (coverage.clipped) ImGui::TextDisabled("Some loaded depth is hidden at this detail. Pan, zoom in or choose coarser detail.");
    if (center_value < 0.001f) {
        ImGui::TextDisabled(mouse.y < coverage.low || mouse.y >= coverage.high
            ? "Outside loaded depth range" : "No quantity displayed in this bucket");
    }
    ImGui::Separator();

    if (ImGui::BeginTable("HeatmapGrid", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("T-1", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("T", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("T+1", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableHeadersRow();

        for (int row = 0; row < 3; row++) {
            const double price = center_price + (1 - row) * bucket_size;
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text(fmt_.price_fmt, price);

            for (int col = 0; col < 3; col++) {
                ImGui::TableSetColumnIndex(col + 1);
                const int64_t time = center_time + (col - 1) * timeframe_ms;
                const float qty = reconstructor->get_value_at_price_and_time(price, time);

                if (qty > 0.001f) {
                    float normalized = reconstructor->candle_intensity(qty, heatmap_sensitivity_);
                    ImVec4 bg_color = get_tooltip_cell_color(normalized);
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                                           ImGui::ColorConvertFloat4ToU32(bg_color));
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        normalized > 0.5f ? ImVec4(0,0,0,1) : ImVec4(1,1,1,1));
                    ImGui::Text("%.2f", qty);
                    ImGui::PopStyleColor();
                } else {
                    ImGui::TextDisabled("-");
                }
            }
        }
        ImGui::EndTable();
    }

    ImGui::PopStyleVar();
    Theme::end_tooltip();
}

void ChartWidget::calculate_heatmap_price_range(double& min_price, double& max_price) {
    if (candles().empty()) {
        min_price = candles().last_close_price() * 0.90;
        max_price = candles().last_close_price() * 1.10;
        return;
    }

    double data_min = std::numeric_limits<double>::max();
    double data_max = std::numeric_limits<double>::lowest();
    for (const auto& candle : candles().candles()) {
        data_min = std::min(data_min, candle.low);
        data_max = std::max(data_max, candle.high);
    }

    double range = data_max - data_min;
    min_price = data_min - (range * 0.05);
    max_price = data_max + (range * 0.05);

    auto* recon = ctx_.heatmap_mgr().get_reconstructor(pair_, heatmap_mode_);
    if (recon) {
        const double bucket_size = recon->get_display_bucket_size();
        min_price = std::floor(min_price / bucket_size) * bucket_size;
        max_price = std::ceil(max_price / bucket_size) * bucket_size;
    }
}

ImPlotColormap ChartWidget::get_colormap_for_type(HeatmapType type) const {
    switch (type) {
        case HeatmapType::OrderbookDepth:  return ImPlotColormap_Viridis;
        case HeatmapType::VolumeDelta:     return ImPlotColormap_RdBu;
        case HeatmapType::TradeIntensity:  return ImPlotColormap_Hot;
        case HeatmapType::Liquidations:    return ImPlotColormap_Plasma;
        case HeatmapType::VWAPDeviation:   return ImPlotColormap_RdBu;
        default:                           return ImPlotColormap_Viridis;
    }
}

std::string ChartWidget::format_time_hms(int64_t timestamp_ms) {
    char buffer[40]{};
    DisplayTimeZone::instance().format(timestamp_ms, TimeZoneFormat::DateTimeSeconds,
                                        buffer, sizeof(buffer));
    return std::string(buffer);
}

ImVec4 ChartWidget::get_tooltip_cell_color(float normalized) const {
    if (normalized < 0.01f) {
        return {0.06f, 0.10f, 0.18f, 1.0f};
    } else if (normalized < 0.15f) {
        const float s = (normalized - 0.01f) / 0.14f;
        return {
            0.06f + s * 0.04f,
            0.10f + s * 0.37f,
            0.18f + s * 0.41f,
            1.0f
        };
    } else if (normalized < 0.35f) {
        const float s = (normalized - 0.15f) / 0.20f;
        return {
            0.10f + s * 0.14f,
            0.47f + s * 0.16f,
            0.59f + s * 0.21f,
            1.0f
        };
    } else if (normalized < 0.55f) {
        const float s = (normalized - 0.35f) / 0.20f;
        return {
            0.24f + s * 0.47f,
            0.63f - s * 0.31f,
            0.80f + s * 0.12f,
            1.0f
        };
    } else if (normalized < 0.75f) {
        const float s = (normalized - 0.55f) / 0.20f;
        return {
            0.71f + s * 0.25f,
            0.32f + s * 0.39f,
            0.92f - s * 0.61f,
            1.0f
        };
    } else {
        const float s = (normalized - 0.75f) / 0.25f;
        return {
            0.96f + s * 0.04f,
            0.71f + s * 0.29f,
            0.31f + s * 0.69f,
            1.0f
        };
    }
}

void ChartWidget::toggle_heatmap() {
    heatmap_enabled_ = !heatmap_enabled_;
    if (heatmap_enabled_ && !heatmap_data_requested_) {
        request_heatmap_data();
    }
}

// =============================================================================
// Liquidation Heatmap Overlay
// =============================================================================



// ═══════════════════════════════════════════════════════════════════════════════
// VPVR (Volume Profile Visible Range) - sidebar histogram
// ═══════════════════════════════════════════════════════════════════════════════

double ChartWidget::compute_vpvr_tick_per_row(double price_range, double plot_height) const {
    if (vpvr_ticks_per_row_ > 0) {
        // Manual override
        return vpvr_ticks_per_row_ * tick_size_;
    }
    // Auto: target ~150-250 visible rows for good density.
    // This gives a reasonable bar height (2-4px per row at typical chart heights).
    constexpr double kTargetRows = 200.0;
    if (price_range <= 0 || plot_height <= 0 || tick_size_ <= 0) return tick_size_;

    const double ideal_row_price = price_range / kTargetRows;
    // Round to nearest multiple of tick_size
    const double ticks = std::max(1.0, std::round(ideal_row_price / tick_size_));
    return ticks * tick_size_;
}

void ChartWidget::render_vpvr_profile() {
    const auto* profile = ctx_.vpvr_mgr().get_profile(pair_.symbol);
    if (!profile || profile->levels.empty()) return;

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    const ImPlotRect limits = ImPlot::GetPlotLimits();
    const ImVec2 plot_pos = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();

    const auto mode = ctx_.vpvr_mgr().mode();

    // Find max volume for normalization
    double max_vol = 0.001;
    for (const auto& lvl : profile->levels) {
        double v = 0;
        switch (mode) {
            case VolumeProfileManager::Mode::Standard:
                v = lvl.total_volume; break;
            case VolumeProfileManager::Mode::TotalVolume:
                v = lvl.total_volume; break;
            case VolumeProfileManager::Mode::TotalDelta:
                v = lvl.total_volume; break;
        }
        if (v > max_vol) max_vol = v;
    }

    // Compute band width from adjacent levels (or use tick_per_row)
    double band_width = tick_size_;
    if (profile->levels.size() >= 2) {
        band_width = std::abs(profile->levels[1].price - profile->levels[0].price);
        if (band_width < tick_size_) band_width = tick_size_;
    }

    // Token → renderer color helper (design system: UP/DOWN split, WARN POC)
    auto tok = [](const ImVec4& c) {
        return Profile::Color{static_cast<uint8_t>(c.x * 255.0f),
                              static_cast<uint8_t>(c.y * 255.0f),
                              static_cast<uint8_t>(c.z * 255.0f)};
    };

    // Build ProfileBand vector
    std::vector<Profile::ProfileBand> bands;
    bands.reserve(profile->levels.size());

    for (int i = 0; i < static_cast<int>(profile->levels.size()); ++i) {
        const auto& lvl = profile->levels[i];

        // Skip levels outside visible Y range (with small margin)
        if (lvl.price < limits.Y.Min - band_width || lvl.price > limits.Y.Max + band_width)
            continue;

        const float norm = static_cast<float>(lvl.total_volume / max_vol);
        if (norm < 0.001f) continue;

        // Design rule: value-area bins at full alpha, outside dimmed (~0.42)
        const float va_alpha = lvl.in_value_area ? 0.90f : 0.42f;

        Profile::ProfileBand pb{};
        pb.price_mid = lvl.price;
        pb.band_width = band_width;

        switch (mode) {
        case VolumeProfileManager::Mode::Standard: {
            // Primary = total bar (sell base, DOWN), secondary = buy overlay (UP)
            pb.primary_value = norm;
            pb.primary_color = tok(Theme::Tokens::DOWN);
            pb.primary_alpha = va_alpha;

            const float buy_ratio = (lvl.total_volume > 0)
                ? static_cast<float>(lvl.buy_volume / lvl.total_volume) : 0.5f;
            pb.secondary_value = norm * buy_ratio;
            pb.secondary_color = tok(Theme::Tokens::UP);
            pb.secondary_alpha = va_alpha;
            break;
        }
        case VolumeProfileManager::Mode::TotalVolume: {
            // Single neutral bar - brand-tinted, VA-dimmed
            pb.primary_value = norm;
            pb.primary_color = tok(Theme::Tokens::BRAND);
            pb.primary_alpha = va_alpha * 0.75f;
            pb.secondary_value = 0;
            break;
        }
        case VolumeProfileManager::Mode::TotalDelta: {
            // 3-layer: faint neutral total → dominant side in UP/DOWN
            pb.primary_value = norm;
            pb.primary_color = tok(Theme::Tokens::TX4);
            pb.primary_alpha = va_alpha * 0.45f;

            const float buy_ratio = (lvl.total_volume > 0)
                ? static_cast<float>(lvl.buy_volume / lvl.total_volume) : 0.5f;
            const float dominant_ratio = std::max(buy_ratio, 1.0f - buy_ratio);
            pb.secondary_value = norm * dominant_ratio;
            pb.secondary_color = (buy_ratio >= 0.5f)
                ? tok(Theme::Tokens::UP)
                : tok(Theme::Tokens::DOWN);
            pb.secondary_alpha = va_alpha;
            break;
        }
        }

        // POC highlight - WARN amber edge
        if (lvl.is_poc) {
            pb.show_edge = true;
            pb.edge_color = tok(Theme::Tokens::WARN);
            pb.edge_alpha = 0.85f;
        }

        // Value area wash
        if (lvl.in_value_area) {
            pb.wash_alpha = 0.04f;
        }

        // Labels (show volume on significant levels)
        if (ctx_.vpvr_mgr().show_values() && norm > 0.15f) {
            const double vol = lvl.total_volume;
            if (vol >= 1e9)
                snprintf(pb.label_buf, sizeof(pb.label_buf), "%.1fB", vol / 1e9);
            else if (vol >= 1e6)
                snprintf(pb.label_buf, sizeof(pb.label_buf), "%.1fM", vol / 1e6);
            else if (vol >= 1e3)
                snprintf(pb.label_buf, sizeof(pb.label_buf), "%.0fK", vol / 1e3);
            else
                snprintf(pb.label_buf, sizeof(pb.label_buf), "%.0f", vol);
            pb.label = pb.label_buf;
        }

        pb.has_tooltip = true;
        pb.source_index = i;
        bands.push_back(pb);
    }

    if (bands.empty()) return;

    // Price markers: POC (WARN solid), VAH/VAL (TX3 dashed)
    std::vector<Profile::PriceMarker> markers;
    if (ctx_.vpvr_mgr().show_poc()) {
        Profile::PriceMarker poc{};
        poc.price = profile->poc;
        poc.color = Theme::u32(Theme::Tokens::WARN, 0.75f);
        poc.dashed = false;
        snprintf(poc.label_buf, sizeof(poc.label_buf), "POC");
        poc.label = poc.label_buf;
        markers.push_back(poc);
    }
    if (ctx_.vpvr_mgr().show_vah_val()) {
        Profile::PriceMarker vah{};
        vah.price = profile->vah;
        vah.color = Theme::u32(Theme::Tokens::TX3, 0.80f);
        vah.dashed = true;
        snprintf(vah.label_buf, sizeof(vah.label_buf), "VAH");
        vah.label = vah.label_buf;
        markers.push_back(vah);

        Profile::PriceMarker val{};
        val.price = profile->val;
        val.color = Theme::u32(Theme::Tokens::TX3, 0.80f);
        val.dashed = true;
        snprintf(val.label_buf, sizeof(val.label_buf), "VAL");
        val.label = val.label_buf;
        markers.push_back(val);
    }

    // Render config
    Profile::ProfileConfig cfg;
    cfg.max_bar_width_pct = ctx_.vpvr_mgr().width_pct();
    cfg.right_aligned = true;
    cfg.draw_wash = true;
    cfg.label_min_band_height = 10.0f;

    // Tooltip
    Profile::render_profile_with_tooltip(
        draw_list, limits, plot_pos, plot_size,
        bands, markers, cfg,
        [](const Profile::ProfileBand& band, int idx, void* ud) {
            auto* prof = static_cast<const VolumeProfileManager::ProfileData*>(ud);
            const int si = band.source_index;
            if (si < 0 || si >= static_cast<int>(prof->levels.size())) return;
            const auto& lvl = prof->levels[si];
            Theme::begin_tooltip();
            ImGui::Text("Price: %g", lvl.price);
            ImGui::Separator();
            const double buy_pct = (lvl.total_volume > 0.001) ? (lvl.buy_volume / lvl.total_volume * 100.0) : 0.0;
            const double sell_pct = (lvl.total_volume > 0.001) ? (lvl.sell_volume / lvl.total_volume * 100.0) : 0.0;
            ImGui::TextColored(Theme::Tokens::UP,
                "Buy:   %.2f (%.1f%%)", lvl.buy_volume, buy_pct);
            ImGui::TextColored(Theme::Tokens::DOWN,
                "Sell:  %.2f (%.1f%%)", lvl.sell_volume, sell_pct);
            ImGui::Text("Total: %.2f (%.2f%% of profile)", lvl.total_volume, lvl.volume_pct);
            ImGui::Text("Delta: %+.2f", lvl.delta);
            if (lvl.is_poc) {
                ImGui::TextColored(Theme::Tokens::WARN, "** POC **");
            }
            if (lvl.in_value_area) {
                ImGui::TextDisabled("Inside Value Area");
            }
            Theme::end_tooltip();
        },
        const_cast<void*>(static_cast<const void*>(profile))
    );
}

void ChartWidget::render_liq_timeline() {
    auto* reconstructor = ctx_.liq_heatmap_mgr().get_timeline_reconstructor(pair_);
    if (!reconstructor || !reconstructor->has_data()) return;

    const int64_t tf_sec = candles().timeframe_seconds();
    const int64_t candle_ms = tf_sec * 1000;

    // Adaptive tick-per-row: auto-adjust bucket_multiplier so the visible
    // price range has a reasonable number of rows (~80-200).
    // Prevents micro-cap coins with tiny tick_size from generating
    // thousands of sub-pixel rows that look like static noise.
    const double native_bucket = reconstructor->get_native_bucket_size();
    if (native_bucket > 0) {
        int target_multiplier;
        if (liq_ticks_per_row_ > 0) {
            // Manual override: user-specified ticks per row
            // Convert ticks to price range, then to bucket multiplier
            // tick_size_ is from symbol metadata (e.g. $0.10 for BTC)
            if (tick_size_ > 0) {
                const double row_price_range = liq_ticks_per_row_ * tick_size_;
                target_multiplier = std::max(1, static_cast<int>(std::round(row_price_range / native_bucket)));
            } else {
                target_multiplier = liq_ticks_per_row_;
            }
        } else {
            // Auto: pixel-space adaptive - ensure each liq band is at least
            // ~3px tall. This works for both BTC (tight range, many rows fit)
            // and LABUSDT on 4h (300%+ range, needs heavy aggregation).
            // The old fixed target of 200 rows broke on volatile alts where
            // 200 rows × tiny cells = sub-pixel noise with no visible bands.
            const ImPlotRect limits = ImPlot::GetPlotLimits();
            const double visible_range = limits.Y.Max - limits.Y.Min;
            const ImVec2 plot_size = ImPlot::GetPlotSize();
            const float plot_height_px = plot_size.y;
            constexpr float min_cell_px = 2.5f;  // Minimum pixels per liq band
            const int native_rows = static_cast<int>(visible_range / native_bucket);
            const int max_visible_rows = std::max(1, static_cast<int>(plot_height_px / min_cell_px));
            target_multiplier = std::max(1, native_rows / max_visible_rows);
        }
        target_multiplier = std::clamp(target_multiplier, 1, 100);
        const int current = reconstructor->get_bucket_multiplier();
        // Only update if significantly different to avoid constant grid rebuilds
        if (target_multiplier != current &&
            (target_multiplier > current * 1.3 || target_multiplier < current * 0.7 || current <= 1)) {
            reconstructor->set_bucket_multiplier(target_multiplier);
        }
    }

    // Push opacity and color scale to the reconstructor
    reconstructor->set_opacity(liq_opacity_);
    // Historical field: never cone-fade by the CURRENT mark (that erases past bands far from
    // price - preserving them is the whole point). GL_NEAREST + extend-levels give MMT's crisp
    // horizontal streaks rather than a smeared gaussian cloud.
    reconstructor->set_reach_modulation(false);
    reconstructor->set_extend_levels(true);
    reconstructor->set_linear_filtering(false);

    // Auto-compute Low/Peak from viewport data distribution.
    // The Bayesian estimator spreads liquidation estimates via Gaussians,
    // producing smooth non-zero values across all visible bands. Fixed
    // low/peak can't create visual contrast because the values cluster
    // in a narrow range (e.g., all between 1.2 and 2.0). Viewport-adaptive
    // normalization uses the actual mean ± stddev of visible data to stretch
    // that narrow range across the full colormap.
    if (liq_color_auto_) {
        const ImPlotRect limits = ImPlot::GetPlotLimits();
        auto stats = reconstructor->get_viewport_stats(limits.Y.Min, limits.Y.Max);
        if (stats.count > 10 && stats.stddev > 0.001f) {
            // V7d: Final tuning - show more of the purple/magenta mid-range so
            // clusters feel substantial with visible halos, not just isolated dots.
            // Low = mean - 0.2σ: slightly below mean so the upper ~60% of data shows.
            // Peak = mean + 2.5σ: tighter peak brings more values into orange/yellow range.
            // Combined with pow(t, 1.3) in shader for gentler contrast curve.
            // V8 render spike: PEAKY floor. The field is dense/low-contrast, so a floor just
            // below the mean lets ~half the bands clear the shader discard → green fog. Push
            // the floor well above the mean so only the strong tail (the real levels) survives;
            // extend-levels then carries those few bands across columns as clean horizontal
            // lines. (Diagnostic: if survivors form lines, the data has persistent levels and
            // this was a render problem; if they scatter, the levels jitter → estimator work.)
            // V9: dense floor (was the V8 "peaky" mean+1σ diagnostic, which discarded
            // everything but the strong tail → rails-only). That diagnostic passed -
            // survivors form clean horizontal lines = render problem, data good - so
            // restore the dense setting: low just below the mean renders the field as
            // the purple/magenta background; peak at +2.5σ keeps dominant levels bright.
            liq_color_low_ = std::max(0.0f, stats.mean - 0.5f * stats.stddev);
            liq_color_peak_ = stats.mean + 2.5f * stats.stddev;
        } else {
            // Fallback: not enough data yet
            liq_color_low_ = 0.0f;
            liq_color_peak_ = 2.0f;
        }
    }
    reconstructor->set_color_low(liq_color_low_);
    reconstructor->set_color_peak(liq_color_peak_);

    reconstructor->render_cells(candle_ms, 1.0f, false);
}


// The Hyperliquid positions layer, on the UNDERLYING-keyed HL pair (not pair_: a cross-venue overlay). The rails'
// subscription follows it in update_hl_rails; the raw census the admins' fuel reads subscribes in
// subscribe_chart_streams and is dropped here when the layer goes off.
void ChartWidget::toggle_liq_census() {
    liq_census_enabled_ = !liq_census_enabled_;
    if (!liq_census_enabled_ && liq_census_subscribed_) {
        const StreamKey key{liq_census_pair_, Terminal::Stream::LiquidationLevels, 0};
        ctx_.stream_mgr().send_unsubscribe(key);
        liq_census_subscribed_ = false;
    }
}

void ChartWidget::reset_overlay_subscriptions() {
    if (chart_stream_mgr_ != &ctx_.stream_mgr()) {
        unsubscribe_chart_streams();
        if (is_open) subscribe_chart_streams();
    }
    // The old context is still alive here. Release its callbacks before a
    // replay context can be retired, then bind both depth and flow to the new one.
    if (rt_stream_mgr_)
        rt_stream_mgr_->unsubscribe_direct({pair_, Terminal::Stream::Orderbook, 0}, this);
    rt_stream_mgr_ = nullptr;
    rt_subscribed_ = false;
    release_refill_levels();  // re-subscribed on the new context by update_refill_levels
    release_refill_history();  // read again on the new context by update_refill_history
    rt_flow_.reset();
    rt_archive_.reset();
    on_rewind(0);
    if (rt_mode_) set_rt_mode(true);
    // Reset subscription flags so overlays re-subscribe on the new context.
    // Called when replay starts/stops (context swap).
    liq_census_subscribed_ = false;
    liq_history_requested_ = 0;
    heatmap_data_requested_ = false;
    // Force VPVR re-request
    vpvr_last_request_start_ = 0;
    vpvr_last_request_end_ = 0;
    // VPIN pane: fresh SeriesCache on the new context (replay ctx starts
    // empty and is seed-fed; live ctx refills via re-request) - force
    // re-subscribe + re-request + repopulate.
    vpin_subscribed_ = false;
    vpin_history_requested_ = false;
    vpin_populated_revision_ = 0;
    vpin_hist_last_oldest_ = 0;
    vpin_hist_exhausted_ = false;
    // Admin patterns are live context only. Never carry a pre-replay candidate
    // into historical playback or restore it stale on replay exit; the next
    // backend forming update repopulates the fixed slots.
    pattern_overlay_active_.fill(false);
}

// ── Shared liquidation-level SELECTION - used by BOTH the rail render (replay) and the standing-
// shelf fallback (live). Floors at max*min_frac, sorts by USD desc, merges near-duplicate prices,
// then caps: per SIDE when per_side_cap>0 (rails), else a single TOTAL cap (shelves). Pure
// selection - the caller keeps its OWN styling (rails draw LINEAR brightness, shelves SQRT - a
// deliberate difference, see the LIQMAP handoff). lx/rx (the resolved pixel x-span) ride through
// untouched. out_max_usd returns the pre-floor max for the caller's brightness normalization.
namespace {
struct LiqDrawLine { double price; double usd; bool is_long; float lx; float rx; bool swept = false; };
static std::vector<LiqDrawLine> select_liq_lines(
        const std::vector<LiqDrawLine>& in, float min_frac, double merge_abs,
        int per_side_cap, int total_cap, double& out_max_usd) {
    out_max_usd = 0.0;
    for (const auto& l : in) if (l.usd > out_max_usd) out_max_usd = l.usd;
    std::vector<LiqDrawLine> kept;
    if (out_max_usd <= 0.0) return kept;
    const double floor_usd = out_max_usd * static_cast<double>(min_frac);
    std::vector<LiqDrawLine> floored;
    floored.reserve(in.size());
    for (const auto& l : in) if (l.price > 0.0 && l.usd >= floor_usd) floored.push_back(l);
    std::sort(floored.begin(), floored.end(),
        [](const LiqDrawLine& a, const LiqDrawLine& b) { return a.usd > b.usd; });
    kept.reserve(floored.size());
    int n_long = 0, n_short = 0, n_total = 0;
    for (const auto& l : floored) {
        bool dup = false;
        if (merge_abs > 0.0)
            for (const auto& r : kept)
                if (std::abs(r.price - l.price) < merge_abs) { dup = true; break; }
        if (dup) continue;
        if (per_side_cap > 0) {
            if (l.is_long) { if (n_long >= per_side_cap) continue; ++n_long; }
            else           { if (n_short >= per_side_cap) continue; ++n_short; }
        } else if (total_cap > 0 && n_total >= total_cap) {
            continue;
        }
        ++n_total;
        kept.push_back(l);
    }
    return kept;
}
} // namespace

// The stateful per-level discharge tracker (update_liq_discharge_tracker) and liq_level_taken were
// REMOVED with the rails pivot (2026-06-29). "Taken" is no longer inferred from candle [low,high] on
// the client - it is the backend RailTracker's consume_ms (the first LAST-PRICE wick-through),
// delivered per rail on the wire and surfaced via BandTrack.consume_ms. No client candle-scan
// inference.
// ── Liq Heatmap projection Field (V1) ────────────────────────────────────────────────────
// MMT-style dense liquidation heatmap, computed CLIENT-SIDE from candles (universal +
// deterministic - renders on every symbol/timeframe, unlike the OI estimator).
//
// Sentinel end time for a segment that's still PENDING (never consumed) → extends to the live/replay edge.
// WS4 - Observed layer (§7): discrete REAL @forceOrder liquidations as side-tinted dots,
// area ∝ USD. The fact layer against the estimated Field - deliberately a different visual
// grammar (glyphs, not bands). Forced BUY = a SHORT was liquidated (tint Tokens::UP, the
// buy-side force); forced SELL = a LONG was liquidated (Tokens::DOWN). Events come from
// LiquidationHeatmapManager::observed_events - live stream, archive bundle, or the replay
// liquidation_events DB seed - always time-sorted, so the visible window binary-searches.
// Replay correctness needs no special casing here: events are delivered in playback order
// and trimmed on rewind, so nothing past the playback head can exist in the store.
// Hyperliquid positions as rails (core/hl_rails.h): the coin's rails from the server, through the chosen real leverage
// and the minimum dollars. On another venue's chart Hyperliquid prices map onto the chart by today's basis: the chart's
// candle at the latest message over that message's mark (1 in a replay, which has no live mark: BTC's basis between
// the two venues is a few hundredths of a percent). This is a sampled census, and a level can move with collateral.
void ChartWidget::render_liq_census(bool follow) {
    const bool replay = ctx_.candle_mgr().replay_start_time_ms() > 0;
    const int64_t asof = replay ? ctx_.replay_mgr().interpolated_time_ms() : static_cast<int64_t>(emscripten_date_now());
    static const hl_rails::Store kEmpty;
    const auto* store = ctx_.liq_heatmap_mgr().get_hl_rails(liq_census_pair_);
    const auto& book = hl_rails_view_.sync(store ? *store : kEmpty, {hl_groups_, static_cast<double>(liq_census_min_usd_)});
    const hl_rails::Latest* latest = store && store->latest().timestamp_ms > 0 ? &store->latest() : nullptr;
    // On another venue's chart the rails sit at Hyperliquid's prices times the basis: this chart's price as each
    // message arrives over the message's mark, averaged (hl_rails::Basis), so the rails hold still between messages.
    double ratio = 1;
    if (pair_.exchange != "hl" && !replay && latest && latest->mark > 0) {
        char key[96];  // a frame allocates nothing
        std::snprintf(key, sizeof(key), "%s/%s/%s", pair_.exchange.c_str(), pair_.symbol.c_str(), liq_census_pair_.symbol.c_str());
        if (hl_basis_key_ != key) {
            hl_basis_.reset();
            hl_basis_key_ = key;
        }
        const double price = candles().has_building_candle() ? candles().building_candle().close
                           : candles().closes().empty() ? 0.0 : candles().closes().back();
        if (price > 0) hl_basis_.update(latest->timestamp_ms, price/latest->mark);
        ratio = hl_basis_.ratio();
    }
    hl_rails_overlay::Candles cv{&candles().timestamps(), &candles().lows(), &candles().highs(),
                                 double(candles().timeframe_seconds())*500.0};
    liqmap2_rails::ChartCandles::Forming forming;
    if (candles().has_building_candle()) {
        const auto& b = candles().building_candle();
        cv.forming = true;
        cv.forming_t = static_cast<double>(b.timestamp_ms);
        cv.forming_lo = b.low;
        cv.forming_hi = b.high;
        forming = {true, b.timestamp_ms, b.low, b.high};
    }
    // The candles' revision, the forming candle included (as the V2 rails read them), keys the kept picture.
    auto& cc = liqmap2_rails_candles_;
    cc.update(candles().timestamps(), candles().lows(), candles().highs(), candles().history_revision(), forming);
    // The key stacks under the V2 rails' key when both draw.
    const bool stack_keys = ImPlot::GetPlotSize().x < 380.0f;
    const float top = (touch_odds_enabled_ ? 86.0f : 58.0f) + (liqmap2_rails_drawn() && stack_keys ? 24.0f : 0.0f);
    hl_rails_view_lo_ = static_cast<int64_t>(ImPlot::GetPlotLimits().X.Min + cv.shift_ms);
    hl_rails_overlay::render(hl_rails_cache_, book, latest, cv, cc.revision(), asof, ratio, liq_census_pair_.symbol.c_str(), fmt_.price_fmt,
        top, follow ? hl_rails_overlay::Role::follow : liqmap2_rails_drawn() ? hl_rails_overlay::Role::lead : hl_rails_overlay::Role::alone,
        hl_profile_, hl_groups_, replay, hl_key_, hl_major_levels_, rail_value_labels_);
    if (liqmap2_rails_drawn() && !stack_keys) hl_key_.at.x += 150.0f;
}

// Observed rails fuel is an admin choice on Hyperliquid charts with the census subscribed. The touch odds
// there are Hyperliquid's own (trade price), so positions and odds come from one venue: never one venue's
// reach times another venue's size.
bool ChartWidget::observed_rails_allowed() const {
    return Entitlements::is_admin() && pair_.exchange == "hl" && liq_census_enabled_;
}

// The census frame known at the clock as rails fuel, only while fresh and located
// (census_profile::State::ready). Otherwise the scenario map stands in and the caption says why.
liq_rails::FuelSource ChartWidget::observed_rails_fuel(int64_t clock) const {
    liq_rails::FuelSource out;
    const auto* history = ctx_.liq_heatmap_mgr().get_census_history(liq_census_pair_);
    const auto* frame = history ? history->at(clock) : nullptr;
    switch (census_profile::state(frame, clock)) {
        case census_profile::State::ready: break;
        case census_profile::State::stale: out.fallback = "HL positions stale"; return out;
        case census_profile::State::missing: out.fallback = "no HL census yet"; return out;
        default: out.fallback = "HL census unavailable"; return out;
    }
    const auto& q = frame->census_quality;
    const double coverage = q.coverage_denominator_usd > 0 ? std::min(1.0, q.usable_notional_usd/q.coverage_denominator_usd) : -1.0;
    // Age from the oldest wallet receipt behind the frame, not its publication time.
    return liq_rails::observed(frame->bands, frame->census_observed_at_ms > 0 ? frame->census_observed_at_ms : frame->timestamp_ms, coverage);
}

// V2 modelled map access (core/liqmap2_release.h): admin preview until liqmap2::kPublic ships true; the
// server enforces the same free-BTC / Pro split on stream 38.
// One map per coin across venues (James 2026-10-10): the rails run on Bybit and Binance, and a chart on either shows
// the coin's rails from both. Hub naming is the same symbol on both venues; a coin listed under another name on the
// other venue simply gets no sibling rails.
Terminal::Pair ChartWidget::liqmap2_rails_sibling() const {
    Terminal::Pair p = pair_;
    p.exchange = pair_.exchange == "bybit" ? "binancef" : pair_.exchange == "binancef" ? "bybit" : "";
    return p;
}

liqmap2::Access ChartWidget::liqmap2_access() const {
    return liqmap2::access(pair_.exchange, pair_.symbol, Entitlements::is_admin(), Entitlements::is_pro());
}

bool ChartWidget::live_chart() const {
    return ctx_.candle_mgr().replay_start_time_ms() == 0 && !EducationBoot::instance().is_embedded() &&
           !EducationBoot::instance().is_pack();
}

bool ChartWidget::rails_replay() const {
    const auto& boot = EducationBoot::instance();
    return ctx_.candle_mgr().replay_start_time_ms() > 0 && !ctx_.replay_mgr().is_pack_mode() && !boot.is_pack() &&
           !boot.is_lesson();
}

bool ChartWidget::liqmap2_allowed() const {
    const auto a = liqmap2_access();
    return a == liqmap2::Access::preview || a == liqmap2::Access::released || a == liqmap2::Access::free_taste;
}

// Subscribe on the current manager when the layer is on, move with the data context, and ask once per
// subscription for the previous six hours (the live frame paints as soon as it arrives either way).
void ChartWidget::update_liqmap2() {
    const StreamKey key{pair_, Terminal::Stream::LiqMapV2, 0};
    // Live only: replay, packs and lessons carry no V2 frames, and a refused history request there reads
    // as a replay error.
    const bool want = liqmap2_shown() && !rt_mode_ && ct_allows_time_overlays(chart_type_);
    // The frames feed only the admins' touch-odds rails fuel now; customers draw the rails.
    const bool want_frames = want && liqmap2_access() == liqmap2::Access::preview && live_chart();
    if (liqmap2_stream_mgr_ && (!want_frames || liqmap2_stream_mgr_ != &ctx_.stream_mgr())) {
        liqmap2_stream_mgr_->unsubscribe_direct(key, this);
        liqmap2_stream_mgr_ = nullptr;
        liqmap2_history_requested_ = 0;
    }
    if (!want || !candles().is_initial_load_complete()) return;
    if (want_frames && !liqmap2_stream_mgr_) {
        liqmap2_stream_mgr_ = &ctx_.stream_mgr();
        liqmap2_stream_mgr_->subscribe_direct(key, this);
    }
    if (want_frames && liqmap2_history_requested_ == 0 && !ctx_.replay_mgr().is_pack_mode()) {
        const int64_t end = ctx_.candle_mgr().replay_start_time_ms() > 0 ? ctx_.replay_mgr().interpolated_time_ms()
                                                                          : static_cast<int64_t>(emscripten_date_now());
        // False without sending while the socket is not open; the next frame tries again.
        if (end > 0 && ctx_.stream_mgr().request_historical_liqmap2(pair_, end - 6*3600000LL, end))
            liqmap2_history_requested_ = 1;
    }
    // Rails: every viewer with V2 here (free on BTC, Pro elsewhere, admins), their own subscription and the
    // stored history. One map per coin across venues: the coin's rails on the other rails venue too (same
    // subscription lifetime; the sibling of a free BTC market is free BTC too).
    const StreamKey rails_key{pair_, Terminal::Stream::LiqMapV2Rails, 0};
    const auto sib = liqmap2_rails_sibling();
    const StreamKey sib_key{sib, Terminal::Stream::LiqMapV2Rails, 0};
    // Live: the rails stream and the stored days. Replay: no subscription (a replay socket serves no live streams),
    // only the stored rails for the replay window.
    const bool replay = rails_replay();
    const bool want_rails = want && !replay;
    const int64_t window_key = replay ? ctx_.replay_mgr().info().start_time_ms : 0;
    if (window_key != liqmap2_rails_window_) {
        liqmap2_rails_window_ = window_key;
        liqmap2_rails_history_requested_ = 0;
    }
    if (liqmap2_rails_mgr_ && (!want_rails || liqmap2_rails_mgr_ != &ctx_.stream_mgr())) {
        liqmap2_rails_mgr_->unsubscribe_direct(rails_key, this);
        if (!sib.exchange.empty()) liqmap2_rails_mgr_->unsubscribe_direct(sib_key, this);
        liqmap2_rails_mgr_ = nullptr;
        liqmap2_rails_history_requested_ = 0;
    }
    if (want_rails && !liqmap2_rails_mgr_) {
        liqmap2_rails_mgr_ = &ctx_.stream_mgr();
        liqmap2_rails_mgr_->subscribe_direct(rails_key, this);
        if (!sib.exchange.empty()) liqmap2_rails_mgr_->subscribe_direct(sib_key, this);
    }
    // History: the last day when the layer opens, then a day further back at a time while the view reaches before
    // what is loaded, up to the server's seven days (4 h and 1 D views; one day per request keeps each reply within
    // the server's record budget, so a busy coin's older day failing never costs the recent one).
    if (want_rails && liqmap2_rails_history_requested_ < liqmap2_rails::kHistoryDays) {
        const int64_t now = static_cast<int64_t>(emscripten_date_now());
        const int k = liqmap2_rails_history_requested_;
        const int need = k == 0 || liqmap2_rails_view_lo_ <= 0 ? 1
            : std::clamp(static_cast<int>(std::ceil(double(now - liqmap2_rails_view_lo_)/double(liqmap2_rails::kHistoryMs))),
                         1, liqmap2_rails::kHistoryDays);
        const int64_t end = now - k*liqmap2_rails::kHistoryMs, start = end - liqmap2_rails::kHistoryMs;
        if (k < need && ctx_.stream_mgr().request_historical_liqmap2_rails(pair_, start, end)) {
            liqmap2_rails_history_requested_ = k + 1;
            if (!sib.exchange.empty()) ctx_.stream_mgr().request_historical_liqmap2_rails(sib, start, end);
        }
    }
    // Replay: the stored rails alive within the window, a day per request from its start (at most 14 days; the
    // server bounds each read to seven days and refuses one ending after now). Rails born before the window and
    // still standing come with it; nothing after the clock is drawn (the overlay reads steps, ends and candles up
    // to it, a rail from its first step and an end only once known: liqmap2_rails::start_ms, end_at), so a seek
    // simply redraws at the new clock.
    if (want && replay && liqmap2_rails_history_requested_ < 14) {
        const auto& info = ctx_.replay_mgr().info();
        const int64_t now = static_cast<int64_t>(emscripten_date_now()) - 1000;
        const int64_t w0 = info.start_time_ms, w1 = std::min(info.end_time_ms > 0 ? info.end_time_ms : now, now);
        const int k = liqmap2_rails_history_requested_;
        const int64_t start = w0 + k*liqmap2_rails::kHistoryMs;
        if (w0 > 0 && start < w1) {
            const int64_t end = std::min(w1, start + liqmap2_rails::kHistoryMs);
            if (ctx_.stream_mgr().request_historical_liqmap2_rails(pair_, start, end)) {
                liqmap2_rails_history_requested_ = k + 1;
                if (!sib.exchange.empty()) ctx_.stream_mgr().request_historical_liqmap2_rails(sib, start, end);
            }
        }
    }
    // The scorecard changes once a day; ask on first sight and hourly after (live only).
    const int64_t wall = static_cast<int64_t>(emscripten_date_now());
    if (!ctx_.replay_mgr().is_active() && (liqmap2_scorecard_ms_ == 0 || wall - liqmap2_scorecard_ms_ > 3600000) &&
        ctx_.stream_mgr().request_liqmap2_scorecard(pair_))
        liqmap2_scorecard_ms_ = wall;
}

bool ChartWidget::hl_coin_missing() const {
    return TickerManager::instance().has_data() && TickerManager::instance().get("hl", liq_census_pair_.symbol) == nullptr;
}

// Hyperliquid rails (stream 42 live, 43 stored), keyed by the coin, whatever chart asks. Live: the subscription (its
// last message paints at once: it carries every standing rail) and the last day of the record, then a day further back
// at a time while the view reaches before what is loaded, up to the server's seven days. Replay: no subscription (a
// replay socket serves no live streams), the record for the window a day per request; nothing after the clock is drawn.
void ChartWidget::update_hl_rails() {
    const StreamKey key{liq_census_pair_, Terminal::Stream::HLRails, 0};
    const bool want = hl_rails_drawn() && !hl_coin_missing() && candles().is_initial_load_complete();
    const bool replay = rails_replay();
    const bool want_live = want && live_chart();
    const int64_t window_key = replay ? ctx_.replay_mgr().info().start_time_ms : 0;
    if (window_key != hl_rails_window_) {
        hl_rails_window_ = window_key;
        hl_rails_history_requested_ = 0;
    }
    if (hl_rails_mgr_ && (!want_live || hl_rails_mgr_ != &ctx_.stream_mgr())) {
        hl_rails_mgr_->unsubscribe_direct(key, this);
        hl_rails_mgr_ = nullptr;
        hl_rails_history_requested_ = 0;
    }
    if (want_live && !hl_rails_mgr_) {
        hl_rails_mgr_ = &ctx_.stream_mgr();
        hl_rails_mgr_->subscribe_direct(key, this);
    }
    if (want_live && hl_rails_history_requested_ < hl_rails::kHistoryDays) {
        const int64_t now = static_cast<int64_t>(emscripten_date_now());
        const int k = hl_rails_history_requested_;
        const int need = k == 0 || hl_rails_view_lo_ <= 0 ? 1
            : std::clamp(static_cast<int>(std::ceil(double(now - hl_rails_view_lo_)/double(hl_rails::kHistoryMs))), 1,
                         hl_rails::kHistoryDays);
        const int64_t end = now - k*hl_rails::kHistoryMs, start = end - hl_rails::kHistoryMs;
        if (k < need && ctx_.stream_mgr().request_historical_hl_rails(liq_census_pair_, start, end))
            hl_rails_history_requested_ = k + 1;
    }
    if (want && replay && hl_rails_history_requested_ < 14) {
        const auto& info = ctx_.replay_mgr().info();
        const int64_t now = static_cast<int64_t>(emscripten_date_now()) - 1000;
        const int64_t w0 = info.start_time_ms, w1 = std::min(info.end_time_ms > 0 ? info.end_time_ms : now, now);
        const int k = hl_rails_history_requested_;
        const int64_t start = w0 + k*hl_rails::kHistoryMs;
        if (w0 > 0 && start < w1 &&
            ctx_.stream_mgr().request_historical_hl_rails(liq_census_pair_, start, std::min(w1, start + hl_rails::kHistoryMs)))
            hl_rails_history_requested_ = k + 1;
    }
}

void ChartWidget::render_liqmap2(bool follow) {
    const int64_t asof = ctx_.candle_mgr().replay_start_time_ms() > 0
        ? ctx_.replay_mgr().interpolated_time_ms() : static_cast<int64_t>(emscripten_date_now());
    // Every viewer with V2 sees the rails (released 2026-10-11): backend docs/LIQMAP_VISUAL_RULE.md bans the
    // heatmap cloud (rule 7) and the cumulative step lines ("What was rejected"), whose customer path is gone.
    {
        // This chart's venue and the coin's other rails venue; the other's prices map onto this chart by the ratio of
        // the two venues' latest marks (1 until both are in hand).
        auto& sources = liqmap2_rails_sources_;  // kept, so a frame allocates nothing
        sources.resize(1);
        sources[0].store = ctx_.liq_heatmap_mgr().get_liqmap2_rails(pair_);
        sources[0].exchange = pair_.exchange;
        sources[0].ratio = 1.0;
        if (const auto sib = liqmap2_rails_sibling(); !sib.exchange.empty()) {
            const auto* other = ctx_.liq_heatmap_mgr().get_liqmap2_rails(sib);
            const double own_mark = sources[0].store ? sources[0].store->latest().mark : 0;
            const double sib_mark = other ? other->latest().mark : 0;
            sources.push_back({other, sib.exchange, own_mark > 0 && sib_mark > 0 ? own_mark/sib_mark : 1.0});
        }
        static constexpr liqmap2_rails::Colour kColours[] = {liqmap2_rails::Colour::expected_1h, liqmap2_rails::Colour::expected_4h,
                                                             liqmap2_rails::Colour::expected, liqmap2_rails::Colour::dollars};
        // The candles the rails meet: the closed ones and the one still forming, which the chart draws too.
        liqmap2_rails::ChartCandles::Forming forming;
        if (candles().has_building_candle()) {
            const auto& b = candles().building_candle();
            forming = {true, b.timestamp_ms, b.low, b.high};
        }
        auto& cc = liqmap2_rails_candles_;
        cc.update(candles().timestamps(), candles().lows(), candles().highs(), candles().history_revision(), forming);
        const liqmap2_rails_overlay::Candles cv{&cc.t(), &cc.lo(), &cc.hi(),
                                                rt_mode_ ? 0.0 : double(candles().timeframe_seconds())*500.0};
        liqmap2_rails_view_lo_ = static_cast<int64_t>(ImPlot::GetPlotLimits().X.Min + cv.shift_ms);
        // Behind the Hyperliquid positions, from where their record starts: where both have data.
        int64_t follow_from = 0;
        if (follow)
            if (const auto* h = ctx_.liq_heatmap_mgr().get_hl_rails(liq_census_pair_)) follow_from = h->from_ms();
        const auto role = follow ? liqmap2_rails_overlay::Role::follow
                        : hl_rails_drawn() ? liqmap2_rails_overlay::Role::lead : liqmap2_rails_overlay::Role::alone;
        liqmap2_rails_overlay::render(liqmap2_rails_cache_, sources, cv, cc.revision(), asof,
            pair_.symbol.c_str(), fmt_.price_fmt, touch_odds_enabled_ ? 86.0f : 58.0f, kColours[rails_colour()], liqmap2_rails_profile_,
            liqmap2_access() == liqmap2::Access::preview, rails_replay(), role, follow_from, rail_value_labels_);
    }
}

// Why the touch-odds rails cannot draw on this chart, or null. The V2 rails, or Hyperliquid rails leading with their
// profile, are then the chart's one liquidation picture; elsewhere the rails need fuel under them: the scenario map
// (replays, packs, lessons) or observed positions on a Hyperliquid chart. The V2 map was a third fuel, but it only
// existed where the V2 rails draw and hold the forward area, so its switch never changed the chart (removed
// 2026-10-15).
const char* ChartWidget::touch_rails_off() const {
    if (liqmap2_rails_drawn()) return "Touch-odds rails step aside while the V2 rails draw.";
    if (hl_leads() && hl_profile_) return "Touch-odds rails step aside while the Hyperliquid rails draw.";
    if (!scenario_map_shown() && !observed_rails_allowed()) return "Touch-odds rails draw over the scenario map, in replays.";
    return nullptr;
}

// Liquidation Profile - the price-marginal of STANDING fuel (WS1 Option A, 2026-07-03; replaces the
// time-averaged rendered-brightness marginal, which was window-length-dependent and needed an
// arbitrary width gain to be visible at all). Each visible price row takes the MAX of the field's
// own rendered brightness (fixed dual-percentile log map + gamma - the exact tv the forward cascade
// projects with) across the PENDING segments covering it: the sidebar answers "which prices hold the
// most fuel RIGHT NOW", agrees with the cascade by construction (a zone's cascade length IS its tv),
// and is zoom/scroll/window-stable. NO adaptive re-normalization (the round-3/4 "orange wall"), NO
// width gain. Consumed history doesn't count - the field itself already shows it. Buckets the live
// building candle is trading through are carved (same grid band as the field's live carve). Style =
// design `profile` spec: slim right-edge silhouette, LUT tint at FLAT alpha, gaussian sigma = 3
// field rows, 1px outline, baseline hairline. Candle-derived → universal + replay-identical;
// independent toggle (liq_profile_enabled_). §8.
void ChartWidget::render_liq_profile() {
    // The profile is the price-marginal of the Field's cache, not a second
    // computation: same segments, same fixed log map, so the two layers can
    // never disagree about where fuel is standing.
    liq_field_.ensure_cache();
    const std::vector<LiqFieldSeg>& segs = liq_field_.segments();
    const float  norm_lo  = liq_field_.norm_lo();
    const float  norm_hi  = liq_field_.norm_hi();
    const double field_bw = liq_field_.bucket_width_log();
    if (segs.empty() || norm_hi <= 0.0f || field_bw <= 0.0) return;

    const ImPlotRect lims = ImPlot::GetPlotLimits();
    const double y_min = lims.Y.Min, y_max = lims.Y.Max;
    if (y_max <= y_min || y_min <= 0.0) return;
    const ImVec2 plot_pos  = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    if (plot_size.x <= 0.0f || plot_size.y <= 0.0f) return;
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const double bw = field_bw;

    // The field's FIXED log map (set at cache build) - the profile inherits Low/Peak/gamma directly
    // and never re-normalizes.
    const float nlo = norm_lo;
    if (nlo <= 0.0f || norm_hi <= nlo) return;
    const float inv_lr = 1.0f / std::log(norm_hi / nlo);
    const float pgamma = std::max(0.1f, liq_field_.knobs().gamma);

    // Live building-candle carve - the same bucket-grid band as the Field render: fuel the
    // current candle is trading through is being consumed NOW, so it no longer stands.
    double carve_glo = 0.0, carve_ghi = -1.0;
    {
        auto& cm = candles();
        if (cm.has_building_candle()) {
            const auto& b = cm.building_candle();
            if (b.high >= b.low && b.low > 0.0) {
                const double inv_lbw = 1.0 / bw;
                carve_glo = std::exp(std::ceil (std::log(b.low)  * inv_lbw - 1.0) * bw);
                carve_ghi = std::exp(std::floor(std::log(b.high) * inv_lbw + 1.0) * bw);
            }
        }
    }
    const double r_eps_m = 1.0 - bw * 0.25;
    const double r_eps_p = 1.0 + bw * 0.25;

    // 1) MAX-raster the standing fuel into N price rows. Each bucket paints its full ± half-bucket
    //    span (clamped to liq_field_.knobs().min_row_px, mirroring the field's emit_rect row clamp) so a
    //    lone shelf enters the smoothing pass as a small plateau, not a 1px spike it would flatten.
    //    MAX (not sum) across buckets sharing a row keeps the reading zoom-stable: zooming out can
    //    never inflate a row past the strongest shelf actually standing there.
    const int N = std::clamp(static_cast<int>(plot_size.y), 64, 1200);
    liq_profile_scratch_.assign(static_cast<size_t>(N), 0.0f);
    const double rows_per_price = static_cast<double>(N) / (y_max - y_min);
    const double e_half = std::exp(bw * 0.5);
    const double half_row_min = 0.5 * static_cast<double>(std::max(1.0f, liq_field_.knobs().min_row_px))
                                * (static_cast<double>(N) / plot_size.y);   // px → rows
    // LOG grid: bw is the bucket width in log-price (see LiqFieldRenderer::rebuild).
    const double y_lo_margin = y_min * std::exp(-bw * (LiqFieldRenderer::kMaxMergeRun + 1));
    const double y_hi_cut = y_max * std::exp(bw);
    const double y_lo_cut = y_min * std::exp(-bw);
    const LiqFieldSeg key{ static_cast<float>(y_lo_margin), 0.0f, 0.0f, 0, 0 };
    auto sit = std::lower_bound(segs.begin(), segs.end(), key,
                   [](const LiqFieldSeg& a, const LiqFieldSeg& b) { return a.price_lo < b.price_lo; });
    for (; sit != segs.end(); ++sit) {
        const LiqFieldSeg& sg = *sit;
        if (sg.price_lo > y_hi_cut) break;
        if (sg.price_hi < y_lo_cut) continue;                   // merged run entirely below view
        if (sg.end_ms != LiqFieldRenderer::kSegPending) continue;             // STANDING fuel only (Option A)
        if (sg.intensity <= nlo) continue;                      // same Intensity-Low clip as the field
        const float tl = std::min(std::log(sg.intensity / nlo) * inv_lr, 1.0f);
        const float tv = std::pow(tl, pgamma);                  // the field's rendered brightness
        if (tv <= 0.0f) continue;
        // A merged segment spans several buckets - test/deposit per bucket, exactly like pre-merge.
        const int nb = (sg.price_hi > sg.price_lo && sg.price_lo > 0.0f)
            ? 1 + static_cast<int>(std::lround(std::log(static_cast<double>(sg.price_hi) /
                                                        static_cast<double>(sg.price_lo)) / bw))
            : 1;
        for (int bi = 0; bi < nb; ++bi) {
            const double pr = static_cast<double>(sg.price_lo) * std::exp(bi * bw);
            if (pr < y_lo_cut || pr > y_hi_cut) continue;
            if (carve_ghi >= carve_glo && pr >= carve_glo * r_eps_m && pr <= carve_ghi * r_eps_p)
                continue;                                       // being traded through right now
            const double rc = (pr - y_min) * rows_per_price;
            const double half = std::max(half_row_min,
                                         pr * (e_half - 1.0 / e_half) * rows_per_price * 0.5);
            int r0 = std::max(0,     static_cast<int>(std::floor(rc - half)));
            int r1 = std::min(N - 1, static_cast<int>(std::ceil (rc + half)));
            for (int r = r0; r <= r1; ++r) {
                float& cell = liq_profile_scratch_[static_cast<size_t>(r)];
                if (tv > cell) cell = tv;
            }
        }
    }

    // 2) Gaussian smooth - design `profile.smoothingGaussianSigmaRows` = 3 FIELD price rows.
    //    The marginal is sampled at ~1 row/pixel, so the sigma is converted from field rows to
    //    samples via the current zoom (field row height in px = plot_h · bw / ln(y_span)) - the
    //    profile's smoothness tracks the field's visible row size. True gaussian, kernel on the stack.
    {
        const double span_l = (y_min > 0.0 && y_max > y_min) ? std::log(y_max / y_min) : 0.0;
        float sigma = (span_l > 0.0)
            ? static_cast<float>(static_cast<double>(liq_profile_sigma_rows_) * bw / span_l
                                 * static_cast<double>(N))
            : 3.0f;
        // Min clamp 2.5px: on very wide loaded ranges (a 40% mover fully zoomed out) 3 field rows
        // shrink sub-pixel and the silhouette went spiky/jagged (2026-07-03 MAGMA screenshots).
        sigma = std::clamp(sigma, 2.5f, 40.0f);
        const int R = std::min(96, static_cast<int>(std::ceil(sigma * 2.5f)));
        float kw[97];
        float ksum = 0.0f;
        for (int i = 0; i <= R; ++i) {
            kw[i] = std::exp(-0.5f * static_cast<float>(i * i) / (sigma * sigma));
            ksum += (i ? 2.0f : 1.0f) * kw[i];
        }
        for (int i = 0; i <= R; ++i) kw[i] /= ksum;
        liq_profile_scratch2_.assign(liq_profile_scratch_.begin(), liq_profile_scratch_.end());
        for (int i = 0; i < N; ++i) {
            float acc = liq_profile_scratch2_[static_cast<size_t>(i)] * kw[0];
            for (int j = 1; j <= R; ++j) {
                if (i - j >= 0) acc += liq_profile_scratch2_[static_cast<size_t>(i - j)] * kw[j];
                if (i + j < N)  acc += liq_profile_scratch2_[static_cast<size_t>(i + j)] * kw[j];
            }
            liq_profile_scratch_[static_cast<size_t>(i)] = acc;
        }
    }

    // 3) Design render (export `profile` spec): a slim silhouette growing left from the right plot
    //    edge - LUT-tinted per row at a FLAT liq_profile_alpha_, one 1px outline at
    //    kLiqProfileOutline, and a 1px baseline hairline at the plot edge. Width AND tint are the
    //    SAME bounded standing-fuel brightness (no re-normalization, no gain) → the silhouette can
    //    never read hotter or wider than the fuel the field/cascade actually draw: quiet prices hug
    //    the baseline, magnet shelves bulge and glow through.
    ImPlot::PushPlotClipRect();
    const float x_right = plot_pos.x + plot_size.x;
    const float max_w   = plot_size.x * std::clamp(liq_profile_width_, 0.05f, 0.45f);
    const float row_h   = plot_size.y / static_cast<float>(N);
    const int   fill_a  = static_cast<int>(std::clamp(liq_profile_alpha_, 0.0f, 1.0f) * 255.0f);
    const float v_floor = std::max(0.004f, liq_field_.knobs().floor_t);   // same noise floor as the field
    liq_profile_pts_.clear();
    liq_profile_pts_.reserve(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
        const float v = std::clamp(liq_profile_scratch_[static_cast<size_t>(i)], 0.0f, 1.0f);
        const float py = plot_pos.y + plot_size.y - (static_cast<float>(i) + 0.5f) * row_h;
        const float xc = x_right - v * max_w;
        if (v > v_floor) {
            uint8_t cr, cg, cb;
            HeatmapColormap::apply(HeatmapColormap::Type::Liquidation, v, cr, cg, cb);
            dl->AddRectFilled(ImVec2(xc, py - row_h * 0.5f - 0.5f),
                              ImVec2(x_right, py + row_h * 0.5f + 0.5f),
                              IM_COL32(cr, cg, cb, fill_a));
        }
        liq_profile_pts_.push_back(ImVec2(xc, py));
    }
    if (liq_profile_pts_.size() >= 2)
        dl->AddPolyline(liq_profile_pts_.data(), static_cast<int>(liq_profile_pts_.size()),
                        kLiqProfileOutline, 0, 1.0f);
    // 1px baseline hairline at the plot edge (design `profile.baseline`).
    dl->AddLine(ImVec2(x_right - 0.5f, plot_pos.y),
                ImVec2(x_right - 0.5f, plot_pos.y + plot_size.y), kLiqProfileBaseline, 1.0f);
    ImPlot::PopPlotClipRect();
}


// ═══════════════════════════════════════════════════════════════════════════════
// Research pattern overlay
// ═══════════════════════════════════════════════════════════════════════════════

void ChartWidget::handle_pattern_overlay(const Terminal::PatternOverlay& pattern) {
    size_t slot = pattern_overlays_.size();
    for (size_t i = 0; i < pattern_overlays_.size(); ++i) {
        if (pattern_overlay_active_[i] &&
            pattern_overlays_[i].timeframe_ms == pattern.timeframe_ms) {
            slot = i;
            break;
        }
    }

    // This surface is for live candidates only. Expired is the ordinary death
    // state; treating every other terminal state as removal also prevents a
    // breakout update from leaving a stale line behind indefinitely.
    if (pattern.state != "forming") {
        if (slot < pattern_overlay_active_.size() &&
            (pattern.pattern_id.empty() ||
             pattern_overlays_[slot].pattern_id == pattern.pattern_id)) {
            pattern_overlay_active_[slot] = false;
        }
        return;
    }

    if (slot == pattern_overlays_.size()) {
        for (size_t i = 0; i < pattern_overlay_active_.size(); ++i) {
            if (!pattern_overlay_active_[i]) {
                slot = i;
                break;
            }
        }
    }
    if (slot == pattern_overlays_.size()) return;

    // One current candidate per timeframe: a new pattern id replaces the old
    // slot, so a newly-added terminal swing cannot strand the prior segment.
    pattern_overlays_[slot] = pattern;
    pattern_overlay_active_[slot] = true;
}

void ChartWidget::render_pattern_overlay(double visible_x_min, double visible_x_max) {
    if (!Entitlements::is_research()) return;

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    const ImVec2 plot_min = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    const ImVec2 plot_max{plot_min.x + plot_size.x, plot_min.y + plot_size.y};
    const ImU32 resistance_color = Theme::u32(Theme::Tokens::DOWN, 0.78f);
    const ImU32 support_color = Theme::u32(Theme::Tokens::UP, 0.78f);
    const ImU32 label_bg = Theme::u32(Theme::Tokens::PANEL, 0.94f);
    const ImU32 label_border = Theme::u32(Theme::Tokens::BD2, 0.92f);
    const ImU32 label_text = Theme::u32(Theme::Tokens::TX2);

    int rendered = 0;
    ImPlot::PushPlotClipRect();
    for (size_t i = 0; i < pattern_overlays_.size(); ++i) {
        if (!pattern_overlay_active_[i]) continue;
        const auto& pattern = pattern_overlays_[i];
        if (pattern.state != "forming" || pattern.end_ms <= pattern.start_ms) continue;
        if (static_cast<double>(pattern.end_ms) < visible_x_min ||
            static_cast<double>(pattern.start_ms) > visible_x_max) {
            continue;
        }

        const double x0 = std::max(visible_x_min, static_cast<double>(pattern.start_ms));
        const double x1 = std::min(visible_x_max, static_cast<double>(pattern.end_ms));
        if (x1 <= x0) continue;

        const double resistance_y0 = pattern.resistance_at(static_cast<int64_t>(x0));
        const double resistance_y1 = pattern.resistance_at(static_cast<int64_t>(x1));
        const double support_y0 = pattern.support_at(static_cast<int64_t>(x0));
        const double support_y1 = pattern.support_at(static_cast<int64_t>(x1));
        if (!std::isfinite(resistance_y0) || !std::isfinite(resistance_y1) ||
            !std::isfinite(support_y0) || !std::isfinite(support_y1)) {
            continue;
        }

        const ImVec2 resistance_px0 = ImPlot::PlotToPixels(x0, resistance_y0);
        const ImVec2 resistance_px1 = ImPlot::PlotToPixels(x1, resistance_y1);
        const ImVec2 support_px0 = ImPlot::PlotToPixels(x0, support_y0);
        const ImVec2 support_px1 = ImPlot::PlotToPixels(x1, support_y1);
        draw_list->AddLine(resistance_px0, resistance_px1, resistance_color, 1.8f);
        draw_list->AddLine(support_px0, support_px1, support_color, 1.5f);

        const double support_start = pattern.support_at(pattern.start_ms);
        const double support_end = pattern.support_at(pattern.end_ms);
        const double support_delta = support_end - support_start;
        const double support_scale = std::max(std::abs(support_start), std::abs(support_end));
        const double flat_epsilon = std::max(tick_size_, support_scale * 1e-8);
        const char* support_regime = pattern.convergence < 0.0 ? "none"
            : std::abs(support_delta) <= flat_epsilon ? "flat"
            : support_delta > 0.0 ? "rising" : "falling";

        char timeframe[12];
        if (pattern.timeframe_ms % 86400000LL == 0) {
            snprintf(timeframe, sizeof(timeframe), "%lldD",
                     static_cast<long long>(pattern.timeframe_ms / 86400000LL));
        } else if (pattern.timeframe_ms % 3600000LL == 0) {
            snprintf(timeframe, sizeof(timeframe), "%lldH",
                     static_cast<long long>(pattern.timeframe_ms / 3600000LL));
        } else {
            snprintf(timeframe, sizeof(timeframe), "%lldM",
                     static_cast<long long>(pattern.timeframe_ms / 60000LL));
        }

        char label[96];
        snprintf(label, sizeof(label), "%s  %dT  %.0fD  SUP %s",
                 timeframe, pattern.touch_count, pattern.span_days, support_regime);
        const ImVec2 text_size = ImGui::CalcTextSize(label);
        const ImVec2 pad{5.0f, 3.0f};
        float label_x = resistance_px1.x + 5.0f;
        if (label_x + text_size.x + pad.x * 2.0f > plot_max.x - 3.0f) {
            label_x = resistance_px1.x - text_size.x - pad.x * 2.0f - 5.0f;
        }
        label_x = std::clamp(label_x, plot_min.x + 3.0f,
                             plot_max.x - text_size.x - pad.x * 2.0f - 3.0f);
        float label_y = resistance_px1.y -
            static_cast<float>(rendered + 1) * (text_size.y + pad.y * 2.0f + 3.0f);
        label_y = std::clamp(label_y, plot_min.y + 3.0f,
                             plot_max.y - text_size.y - pad.y * 2.0f - 3.0f);
        const ImVec2 label_min{label_x, label_y};
        const ImVec2 label_max{label_x + text_size.x + pad.x * 2.0f,
                               label_y + text_size.y + pad.y * 2.0f};
        draw_list->AddRectFilled(label_min, label_max, label_bg, 3.0f);
        draw_list->AddRect(label_min, label_max, label_border, 3.0f, 0, 1.0f);
        draw_list->AddText(ImVec2(label_x + pad.x, label_y + pad.y), label_text, label);
        ++rendered;
    }
    ImPlot::PopPlotClipRect();
}
