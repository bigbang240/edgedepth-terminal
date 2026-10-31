#include "core/liqmap2_scorecard.h"
#include "core/realtime_archive.h"
#include "core/refill_history.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_hints.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "ui/stats_widget.h"
#include "ui/data_widget.h"
#include "implot.h"
#include "implot_internal.h"

#define IMGUI_IMPL_OPENGL_LOADER_CUSTOM

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_opengl3.h"

#include <emscripten/websocket.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <GLES3/gl3.h>
#include <array>
#include <imgui_impl_sdl3.h>
#include <zstd.h>
#include <nlohmann/json.hpp>

#include "core/message_handler.h"
#include "core/message_context.h"
#include "core/message_parser.h"
#include "core/data_thread.h"
#include "core/websocket.h"
#include "core/websocket_endpoints.h"
#include "types/frame_profiler.h"
#include "core/orderbook_manager.h"
#include "core/logo_manager.h"
#include "core/liquidation_heatmap_manager.h"
#include "core/url_router.h"
#include "core/education_boot.h"
#include "core/workspace_manager.h"
#include "core/usage_emit.h"
#include "core/session_usage.h"
#include "core/entitlements.h"
#include "core/i18n.h"
#include "core/display_time_zone.h"
#include "core/recorder_glue.h"
#include "education/lesson_runtime.h"
#include "education/studio_runtime.h"
#include "education/event_runtime.h"
#include "education/recorder_runtime.h"
#include "core/symbol_metadata.h"
#include "core/debug_manager.h"
#include "core/volume_profile_manager.h"
#include "core/tpo_manager.h"
#include "core/footprint_manager.h"
#include "core/indicator_series.h"
#include "core/analytics_manager.h"
#include "core/paper_trading_manager.h"
#include "core/local_paper_engine.h"
#include "core/ticker_manager.h"
#include "core/scanner_manager.h"
#include "core/drawing_manager.h"
#include "ui/drawing/drawing_style_editor.h"
#include "rendering/layout.h"
#include "rendering/theme.h"
#include "core/ui_test_marks.h"
#include "rendering/menu.h"
#include "rendering/app_shell.h"
#include "rendering/performance_diagnostics.h"
#include "rendering/shader_heatmap_resources.h"
#include "stream_handler.h"
#include "replayer/replay_manager.h"
#include "core/data_context.h"


#include "ui/debug_widget.h"
#include "ui/chart_widget.h"
#include "ui/upsell_modal.h"
#include "ui/research_moment_panel.h"
#include "ui/dom_widget.h"
#include "ui/orderbook_widget.h"
#include "ui/trades_widget.h"
#include "ui/watchlist_widget.h"
#include "ui/widget.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/eventloop.h>
#endif

struct Timeframe {
    const char* label;
    int64_t seconds;
};

struct AppState {
    bool done = false;
    SDL_Window* window = nullptr;
    SDL_GLContext gl_context = nullptr;
    // Owned managers (RAII via unique_ptr)
    std::unique_ptr<WebSocketClient> ws_client;
    // The replay lane's socket, non-null ONLY when a separate replay origin is
    // configured (see resolve_replay_ws_url). Live and replay share one /ws
    // handler on the box and the lane is chosen after the handshake, so the
    // split has to happen at the socket, not at a path. When it is null the
    // replay lane rides ws_client exactly as it always has.
    std::unique_ptr<WebSocketClient> replay_ws_client;
    std::unique_ptr<StreamManager> stream_mgr;
    std::unique_ptr<HeatmapManager> heatmap_mgr;
    std::unique_ptr<LiquidationHeatmapManager> liq_heatmap_mgr;
    std::unique_ptr<MessageHandler> msg_handler;
    std::unique_ptr<OrderbookManager> ob_mgr;
    std::unique_ptr<CandleManager> candle_mgr;
    std::unique_ptr<ReplayManager> replay_mgr;
    std::unique_ptr<DebugManager> debug_mgr;
    std::unique_ptr<VolumeProfileManager> vpvr_mgr;
    std::unique_ptr<TPOManager> tpo_mgr;
    std::unique_ptr<FootprintManager> footprint_mgr;
    std::unique_ptr<PaperTradingManager> paper_trading_mgr;
    std::unique_ptr<IndicatorSeriesManager> series_mgr;  // Indicators V1 SeriesCache
    std::unique_ptr<AnalyticsManager> analytics_mgr;     // Positioning/Contagion state
    std::unique_ptr<DrawingManager> drawing_mgr;         // Chart drawing tools (annotations)
    std::unique_ptr<LocalPaperEngine> local_paper_mgr;   // Client-side paper engine (Phase 5)
    std::unique_ptr<DataThread> data_thread;
    // pthread-free builds (EDGEDEPTH_PTHREADS=OFF): pthread_create fails, the
    // WS bytes parse inline on the main thread, and the dispatch queue would
    // never be drained - it must be dropped everywhere (stream manager +
    // message context) so StreamManager applies widget state directly.
    bool inline_dispatch = false;
    // Message context (rebuilt on connect)
    MessageContext msg_ctx{};
    // App context - single dependency injection point for widgets/menus
    AppContext app_ctx{};
    std::vector<std::unique_ptr<Widget>> widgets;
    std::vector<std::string> debug_log;
    void build_message_context() {
        msg_ctx = MessageContext{
            stream_mgr.get(),
            ob_mgr.get(),
            heatmap_mgr.get(),
            liq_heatmap_mgr.get(),
            debug_mgr.get(),
            vpvr_mgr.get(),
            tpo_mgr.get(),
            footprint_mgr.get(),
            paper_trading_mgr.get(),
            &TickerManager::instance(),
            &ScannerManager::instance(),
            data_thread && !inline_dispatch ? &data_thread->dispatch_queue() : nullptr
        };
        msg_ctx.series = series_mgr.get();
        msg_ctx.analytics = analytics_mgr.get();
    }
    void build_app_context() {
        app_ctx = AppContext{
            stream_mgr.get(),
            ob_mgr.get(),
            heatmap_mgr.get(),
            liq_heatmap_mgr.get(),
            candle_mgr.get(),
            replay_mgr.get(),
            debug_mgr.get(),
            vpvr_mgr.get(),
            tpo_mgr.get(),
            footprint_mgr.get(),
            paper_trading_mgr.get(),
            &TickerManager::instance(),
            series_mgr.get(),
            analytics_mgr.get(),
            drawing_mgr.get(),
            local_paper_mgr.get()
        };
    }
};

AppState g_app;
Route g_initial_route;

void init_main_chart();

// ─── WebSocket message routing ───────────────────────────────────────────────
// Extracted from connect_websocket() for clarity. Handles JSON control messages
// on the main thread, routes binary protobuf to the data thread.

// Which socket a frame arrived on. Only meaningful once the replay lane has its
// own socket; with a single socket everything is Live and the guard below is
// inert.
enum class WsLane : uint8_t { Live, Replay };

static void on_ws_message(const uint8_t* data, size_t len, WsLane lane) {
    if (len == 0) return;

    // ── THE LANE INVARIANT ──────────────────────────────────────────────────
    // A frame that arrived on the LIVE socket must never be written into replay
    // state. While a replay context is active the AppContext managers ARE the
    // replay managers, so a live depth update lands in the replay orderbook and
    // the DOM renders today's book against a 2026-08-19 chart.
    //
    // This regressed on 2026-08-26 when replay moved to its own socket. Before
    // that, join_replay landed on the SAME box session that held the live
    // subscriptions, and the server paused those consumers ("Paused live
    // consumers for replay"). That server-side pause was doing the real work.
    // Splitting the socket split the session, so hub is never told, and the
    // client-side pause_live_subscriptions() alone did not stop it.
    //
    // Guarded on replay_ws_client so it CANNOT fire in the single-socket
    // configuration, where replay frames legitimately arrive on the live lane.
    // Only binary market data is dropped; JSON control frames (errors, auth,
    // status) still need to reach their handlers on both lanes.
    //
    // The `active_socket` test is what keeps ARCHIVE replay working: those
    // sessions ride hub's LIVE socket by design, because only hub has
    // /data/archives. Dropping live-lane frames during one would throw away the
    // replay's own data. Drop only when the replay is demonstrably NOT on this
    // lane.
    if (lane == WsLane::Live && g_app.replay_ws_client && data[0] != '{' &&
        g_app.replay_mgr && g_app.replay_mgr->replay_context() != nullptr &&
        g_app.replay_mgr->active_socket() != g_app.ws_client.get()) {
        static uint64_t dropped = 0;
        if (++dropped % 2000 == 1) {
            SDL_Log("Dropped %llu live-lane frames during replay (lane invariant)",
                    static_cast<unsigned long long>(dropped));
        }
        return;
    }

    // JSON text frames start with '{', binary protobuf never does
    if (data[0] == '{') {
        try {
            auto parsed = nlohmann::json::parse(
                reinterpret_cast<const char*>(data),
                reinterpret_cast<const char*>(data) + len);
            std::string type = parsed.value("type", "");

#ifdef EDGEDEPTH_EXPOSURE_V2_DEV
            if (type == "exposure_v2" || type == "exposure_v2_summary") {
                const bool replay=g_app.replay_mgr && g_app.replay_mgr->is_active();
                if ((lane==WsLane::Replay && replay && g_app.replay_mgr->active_socket()==g_app.replay_ws_client.get()) ||
                    (lane==WsLane::Live && (!replay || g_app.replay_mgr->active_socket()==g_app.ws_client.get())))
                {
                    if (type=="exposure_v2_summary") exposure::SummaryHistory::receive(parsed,replay);
                    else exposure::History::receive(parsed,replay);
                }
                return;
            }
#endif
            if (type == "liquidity_response") {
                const bool replay = g_app.replay_mgr && g_app.replay_mgr->is_active();
                if ((lane == WsLane::Replay && replay && g_app.replay_mgr->active_socket() == g_app.replay_ws_client.get()) ||
                    (lane == WsLane::Live && (!replay || g_app.replay_mgr->active_socket() == g_app.ws_client.get())))
                    liquidity_response::History::receive(parsed);
                return;
            }
            if (type == "refill_levels") {
                // Same lane rule as liquidity_response; the store also drops the other context's mode.
                const bool replay = g_app.replay_mgr && g_app.replay_mgr->is_active();
                if ((lane == WsLane::Replay && replay && g_app.replay_mgr->active_socket() == g_app.replay_ws_client.get()) ||
                    (lane == WsLane::Live && (!replay || g_app.replay_mgr->active_socket() == g_app.ws_client.get())))
                    refill::receive(parsed);
                return;
            }
            if (type == "refill_history") {
                // Recorded levels for candle charts: the same lane rule; the store matches the request id and mode.
                const bool replay = g_app.replay_mgr && g_app.replay_mgr->is_active();
                if ((lane == WsLane::Replay && replay && g_app.replay_mgr->active_socket() == g_app.replay_ws_client.get()) ||
                    (lane == WsLane::Live && (!replay || g_app.replay_mgr->active_socket() == g_app.ws_client.get())))
                    refill::history::receive(parsed, emscripten_get_now());
                return;
            }
            if (type == "flow_positioning") {
                const bool replay = g_app.replay_mgr && g_app.replay_mgr->is_active();
                if ((lane == WsLane::Replay && replay && g_app.replay_mgr->active_socket() == g_app.replay_ws_client.get()) ||
                    (lane == WsLane::Live && (!replay || g_app.replay_mgr->active_socket() == g_app.ws_client.get())))
                    flow_positioning::History::receive(parsed);
                return;
            }
            if (type == "reported_liquidations") {
                if (lane == WsLane::Live && !(g_app.replay_mgr && g_app.replay_mgr->is_active()))
                    LiquidationHeatmapManager::ReportHistory::receive(parsed);
                return;
            }
            if (type == "liqmap2_scorecard") {
                if (lane == WsLane::Live) liqmap2::Scorecards::receive(parsed);
                return;
            }
            if (type == "candle_bubble_history") {
                if (lane == WsLane::Live && !(g_app.replay_mgr && g_app.replay_mgr->is_active()))
                    CandleBubbleHistory::receive(parsed);
                return;
            }
            if(type=="rt_history") {
                if(lane==WsLane::Live && !(g_app.replay_mgr && g_app.replay_mgr->is_active()))
                    RealtimeArchive::receive_startup(parsed);
                return;
            }
            if (g_app.replay_mgr && g_app.replay_mgr->handle_ws_message(type, &parsed))
                return;

            if (type == "debug_range_start" && g_app.debug_mgr) {
                g_app.debug_mgr->handle_range_start(
                    parsed.value("start_time", int64_t(0)),
                    parsed.value("end_time",   int64_t(0)));
                return;
            }
            if (type == "debug_range_complete" && g_app.debug_mgr) {
                g_app.debug_mgr->handle_range_complete(
                    parsed.value("start_time",  int64_t(0)),
                    parsed.value("end_time",    int64_t(0)),
                    parsed.value("msg_count",   0),
                    parsed.value("entry_count", 0));
                return;
            }
        } catch (...) {
            // Malformed JSON - fall through to binary path
        }
    }

    // An errored replay retains its frozen context until explicitly closed.
    if (g_app.replay_mgr && g_app.replay_mgr->state() == ReplayManager::State::Error) return;

    // Binary protobuf path - route to replay or live context
    bool replay_active = g_app.replay_mgr && g_app.replay_mgr->is_active();
    bool has_replay_ctx = replay_active && g_app.replay_mgr->replay_context();
    
    static bool logged_replay_routing = false;
    if (replay_active && !logged_replay_routing) {
        logged_replay_routing = true;
    }
    if (!replay_active && logged_replay_routing) {
        logged_replay_routing = false;  // Reset for next session
    }

    if (has_replay_ctx) {
        // ── Drip-feed gate ────────────────────────────────────────────────
        // During a client-only rewind, the drip-feed dispatches buffered data
        // at the correct pace. Incoming frames from the backend (still streaming
        // from the old position) should be captured into the history buffer
        // but NOT routed to managers - that would mix future data with the
        // drip-feed's past data.
        if (g_app.replay_mgr->is_drip_feeding()) {
            // Decompress + parse to get event_ts for buffer capture
            std::string msg_data(reinterpret_cast<const char*>(data), len);
            auto decomp = MessageParser::decompress_zstd(msg_data);
            if (!decomp.success) return;
            auto parsed = MessageParser::parse_payload(decomp.data);
            if (!parsed.success) return;
            int64_t event_ts = parsed.payload.event_time_ms();
            auto* hist = g_app.replay_mgr->history_buffer();
            if (hist) {
                int64_t ts = (event_ts > 0) ? event_ts : MessageHandler::last_timestamp_ms;
                if (ts > 0) hist->push(ts, Terminal::Stream::Unknown, 0, data, len);
            }
            return;  // Don't route - drip-feed handles dispatch
        }

        // ── Backend rewind gate (out-of-buffer rewind via backend) ────────
        // When the rewind was too far back for the buffer, the backend is
        // reconnecting consumers. Drop stale frames until replay_seeked.
        if (g_app.replay_mgr->is_rewind_pending()) {
            return;  // Stale pre-rewind data - drop
        }

        // During active replay, binary frames are replay data from the Replayer actor.
        // Parse the WSPayload ONCE - used for routing and history capture.
        std::string msg_data(reinterpret_cast<const char*>(data), len);
        auto decomp = MessageParser::decompress_zstd(msg_data);
        if (!decomp.success) return;
        auto parsed = MessageParser::parse_payload(decomp.data);
        if (!parsed.success) return;

        int64_t event_ts = parsed.payload.event_time_ms();

        // Route the pre-parsed payload (no double decompress/parse)
        MessageContext replay_ctx = g_app.replay_mgr->replay_message_context();
        static int replay_msg_count = 0;
        if (++replay_msg_count % 100 == 1) {
        }

        // Safety: warn if incoming frame timestamp is far from the client clock.
        // This detects leaks where the backend sends data from the wrong position.
        if (event_ts > 0) {
            int64_t clock = g_app.replay_mgr->interpolated_time_ms();
            int64_t drift_s = (event_ts - clock) / 1000;
            if (drift_s > 30 || drift_s < -30) {
                static int drift_warn_count = 0;
                if (++drift_warn_count <= 20) {
                }
            }
        }

        MessageHandler::route_parsed(parsed.payload, replay_ctx);

        // Capture into history buffer using event_time_ms from the WSPayload.
        auto* hist = g_app.replay_mgr->history_buffer();
        if (hist) {
            int64_t ts = (event_ts > 0) ? event_ts : MessageHandler::last_timestamp_ms;

            if (ts > 0) {
                hist->push(ts, Terminal::Stream::Unknown, 0, data, len);
            }

            // Log first 10 captures + every 500th
            static int capture_count = 0;
            capture_count++;
            if (capture_count <= 10 || capture_count % 500 == 0) {
            }

            // Periodic OB snapshot for instant rewind support
            if (ts > 0) {
                auto* rctx = g_app.replay_mgr->replay_context();
                if (rctx && rctx->orderbooks && !g_app.replay_mgr->info().symbols.empty()) {
                    hist->snapshot_orderbook(ts, *rctx->orderbooks, g_app.replay_mgr->primary_pair());
                }
            }
        }
    } else if (g_app.data_thread && g_app.data_thread->is_running()) {
        g_app.data_thread->enqueue(data, len);
    } else if (g_app.msg_handler) {
        std::string msg_data(reinterpret_cast<const char*>(data), len);
        MessageHandler::handle_message(msg_data, g_app.msg_ctx);
    }
}

// After a planned hub restart the live feed skipped the outage: charts miss
// a period (or hold a partial one on each edge). Re-fetch the newest candles
// of every open chart and merge them in place - once right away for what the
// new hub has built since it came up, and once more a few minutes later,
// after the server has rebuilt the outage minutes from exchange klines
// (consumer_recovery_candles in redeploy.sh, ~2-3 min after ready).
static double g_refetch_at_ms[2] = {0, 0};
static double g_refetch_outage_ms = 0;

static void refetch_recent_candles(double outage_ms) {
    for (auto& w : g_app.widgets) {
        if (!w->is_open || w->type() != WidgetType::Chart) continue;
        auto* chart = static_cast<ChartWidget*>(w.get());
        const double tf_ms = static_cast<double>(chart->timeframe_seconds()) * 1000.0;
        // Sized in candles, so the cap must be too: the old 40 covered 40
        // minutes of a 1m chart but only 40 seconds of a 1s one.
        const size_t count = std::clamp(static_cast<size_t>(outage_ms / std::max(tf_ms, 1.0)) + 3,
                                        static_cast<size_t>(3), CandleManager::INITIAL_PRELOAD_CANDLES);
        chart->candles().request_recent(count);
    }
}

static void tick_restart_refetch() {
    if (!g_app.ws_client || !g_app.ws_client->is_connected()) return;
    const double now = emscripten_get_now();
    for (double& at : g_refetch_at_ms) {
        if (at > 0 && now >= at) {
            at = 0;
            refetch_recent_candles(g_refetch_outage_ms);
        }
    }
    if (g_refetch_at_ms[0] == 0 && g_refetch_at_ms[1] == 0) g_refetch_outage_ms = 0;
}

static void on_ws_status(const std::string& status) {
    liquidity_response::History::invalidate_all("Transport changed; waiting for observations");
    refill::on_transport_changed();
    refill::history::on_transport_changed();
#ifdef EDGEDEPTH_EXPOSURE_V2_DEV
            exposure::History::invalidate_all("V2 transport changed; reload required");
            exposure::SummaryHistory::invalidate_all("V2 transport changed; reload required");
#endif
    if (status == "Reconnecting") {
        if (g_app.ob_mgr) g_app.ob_mgr->set_realtime_transport_open(false);
        g_app.stream_mgr->update_websocket_handle(0);
        if (g_app.replay_mgr) g_app.replay_mgr->on_transport_interrupted(g_app.ws_client.get());
    }
    if (status == "Connected") {
        if (g_app.ob_mgr) g_app.ob_mgr->set_realtime_transport_open(true);
        g_app.stream_mgr->update_websocket_handle(g_app.ws_client->get_handle());
        if (g_app.ws_client->take_reconnected_after_restart()) {
            const double now = emscripten_get_now();
            g_refetch_outage_ms = g_app.ws_client->last_outage_ms();
            g_refetch_at_ms[0] = now + 2000;
            g_refetch_at_ms[1] = now + 240000;
        }

        // Embedded lesson/studio mode: the socket is still needed (replay frames
        // ride this same WS via the Replayer actor), but the LIVE auto-subscribes
        // below are pure waste in a lesson - and ticker24h alone is ~150KB/s. Skip
        // them; a replay session is driven separately from EducationBoot's source.
        // A boot decision: a live terminal hosting a research replay in place
        // (EducationBoot::hosted_research) still returns to live on exit, so a
        // reconnect during that replay must restore the live feeds too.
        if (EducationBoot::instance().boot_embedded()) {
            return;
        }

        // [2026-04-24] Explicit paper trading subscribe - server no longer auto-subscribes.
        StreamKey paper_key{
            Terminal::Pair{"binancef", "global"},
            Terminal::Stream::PaperTrading,
            0
        };
        g_app.stream_mgr->send_subscribe(paper_key);

        // Subscribe to global 24h ticker - always on, data is ~150KB/sec
        // TODO: make on-demand once confirmed working
        StreamKey ticker24h_key{
            Terminal::Pair{"binancef", "global"},
            Terminal::Stream::Ticker24h,
            0
        };
        g_app.stream_mgr->send_subscribe(ticker24h_key);

        // HL 24h ticker - the venue-parameterized global feed (ticker24h.hl.global,
        // ~0.7KB/sec). Always-on so the statsbar + picker LAST/24H%/VOLUME stay live
        // whichever venue is active. Keyed by exchange in TickerManager.
        StreamKey ticker24h_hl_key{
            Terminal::Pair{"hl", "global"},
            Terminal::Stream::Ticker24h,
            0
        };
        g_app.stream_mgr->send_subscribe(ticker24h_hl_key);

        // Bybit 24h ticker - ticker24h.bybit.global, aggregated by the hub's Bybit
        // consumer from the exchange's own tickers stream (~10 s cadence, ~1KB/s).
        StreamKey ticker24h_bybit_key{
            Terminal::Pair{"bybit", "global"},
            Terminal::Stream::Ticker24h,
            0
        };
        g_app.stream_mgr->send_subscribe(ticker24h_bybit_key);

        // OKX 24h ticker - ticker24h.okx.global, REST-polled by the gateway's
        // OKX adapter (no venue push exists; ~30s cadence). Also the seed of
        // the OKX watchlist universe: entries for venues the metadata API does
        // not carry are upserted into SymbolRegistry as they arrive.
        StreamKey ticker24h_okx_key{
            Terminal::Pair{"okx", "global"},
            Terminal::Stream::Ticker24h,
            0
        };
        g_app.stream_mgr->send_subscribe(ticker24h_okx_key);

        // Binance SPOT 24h ticker - ticker24h.binancespot.global, the gateway's
        // ticker-only spot adapter (!ticker@arr, ~1/s). Seeds the Spot-Perp
        // basis layer: spot last vs this chart's perp mid in bps. Keyed by
        // exchange in TickerManager like every other venue.
        StreamKey ticker24h_spot_key{
            Terminal::Pair{"binancespot", "global"},
            Terminal::Stream::Ticker24h,
            0
        };
        g_app.stream_mgr->send_subscribe(ticker24h_spot_key);
    }
}

void connect_websocket() {
    if (g_app.ws_client) {
        g_app.ws_client->disconnect();
        g_app.ws_client.reset();
    }
    g_app.ws_client = std::make_unique<WebSocketClient>();
    g_app.ws_client->set_message_callback(
        [](const uint8_t* d, size_t n) { on_ws_message(d, n, WsLane::Live); });
    g_app.ws_client->set_status_callback(on_ws_status);

    if (!g_app.ws_client->connect(websocket_endpoints::resolve_ws_url())) {
    }
}

// The socket for NATS/cold-parquet replay: the dedicated one when it exists,
// otherwise the live one. Every such call site goes through this so there is
// exactly one place that knows the lane split exists.
WebSocketClient* replay_lane_socket() {
    return g_app.replay_ws_client ? g_app.replay_ws_client.get() : g_app.ws_client.get();
}

// The socket for ARCHIVE replay of curated events, which is always hub. Those
// sessions stream the absolute market_events.archive_path under /data/archives,
// a 232 GB tree that exists ONLY on hub; server 2 has no such directory. The
// session would create there and then fail opening metadata.json.
WebSocketClient* archive_lane_socket() {
    return g_app.ws_client.get();
}

// Status callback for the REPLAY socket only. Deliberately does almost nothing:
// it must not touch stream_mgr's websocket handle and must not fire the live
// auto-subscribes, or a replay connection would start paying for ticker24h and
// paper-trading traffic on a socket that serves neither.
static void on_replay_ws_status(const std::string& status) {
    liquidity_response::History::invalidate_all("Replay transport changed; waiting for observations");
    refill::on_transport_changed();
    refill::history::on_transport_changed();
#ifdef EDGEDEPTH_EXPOSURE_V2_DEV
            exposure::History::invalidate_all("V2 replay transport changed; reload required");
            exposure::SummaryHistory::invalidate_all("V2 replay transport changed; reload required");
#endif
    if (status == "Reconnecting" && g_app.replay_mgr)
        g_app.replay_mgr->on_transport_interrupted(g_app.replay_ws_client.get());
    if (status == "Connected") {
        SDL_Log("Replay lane socket connected");
    }
}

// Opens the replay lane's own socket, but ONLY when a separate origin is
// configured. With no origin set this is a no-op and nothing about the app
// changes, so the split is inert until the host page opts in.
void connect_replay_websocket() {
    const std::string replay_url = websocket_endpoints::resolve_replay_ws_url();
    if (replay_url.empty() || replay_url == websocket_endpoints::resolve_ws_url()) {
        // Same endpoint as live: opening a second socket to it would double
        // every user's connection count for no benefit.
        g_app.replay_ws_client.reset();
        return;
    }

    if (g_app.replay_ws_client) {
        g_app.replay_ws_client->disconnect();
        g_app.replay_ws_client.reset();
    }
    g_app.replay_ws_client = std::make_unique<WebSocketClient>();
    // Same handler as live, but TAGGED, so the lane invariant at the top of
    // on_ws_message can tell replay frames from live ones. The routing inside
    // is content-based and shared; only the source needs distinguishing.
    g_app.replay_ws_client->set_message_callback(
        [](const uint8_t* d, size_t n) { on_ws_message(d, n, WsLane::Replay); });
    g_app.replay_ws_client->set_status_callback(on_replay_ws_status);

    // Connected eagerly rather than on first replay request. The join path
    // already has a pending_join_ latch for "socket not up yet", but the
    // replay DataContext grabs get_handle() during context creation, and a
    // lazily-opened socket can still be mid-handshake at that moment.
    if (!g_app.replay_ws_client->connect(replay_url)) {
    }
}

void render_debug() {
    ImGui::Begin("WebSocket Debug");
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("Presentation: %.1f FPS (%.3f ms interval, smoothed)",
                io.Framerate,
                io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);
    ImGui::Separator();
    ImGui::Text("Display: %.0f x %.0f (Scale: %.2fx%.2f)",
                io.DisplaySize.x, io.DisplaySize.y,
                io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);

#ifdef __EMSCRIPTEN__
    double dpr = EM_ASM_DOUBLE({
        return window.devicePixelRatio || 1.0;
    });
    ImGui::Text("Device Pixel Ratio: %.2f", dpr);
    int window_w = EM_ASM_INT({ return window.innerWidth; });
    int window_h = EM_ASM_INT({ return window.innerHeight; });
    ImGui::Text("Window Size: %d x %d", window_w, window_h);
    int canvas_w, canvas_h;
    emscripten_get_canvas_element_size("#canvas", &canvas_w, &canvas_h);
    ImGui::Text("Canvas Resolution: %d x %d", canvas_w, canvas_h);
    double zoom = EM_ASM_DOUBLE({
        return (window.outerWidth / window.innerWidth) * 100;
    });
    ImGui::Text("Browser Zoom: %.0f%%", zoom);
#endif

    ImGui::Separator();
    if (ImGui::Button("Reconnect WebSocket")) {
        connect_websocket();
        // connect_websocket() destroys and recreates g_app.ws_client, so any
        // replay lane riding the live socket is now holding a freed pointer.
        if (g_app.replay_mgr) {
            g_app.replay_mgr->set_lanes(replay_lane_socket(), archive_lane_socket());
        }
    }
    ImGui::Separator();
    ImGui::Text("WebSocket: %s",
                g_app.ws_client && g_app.ws_client->is_connected() ? "Connected" : "Disconnected");
    ImGui::End();
}

// File-scope so maybe_start_lesson_replay() (and other helpers) can observe when
// the terminal has finished its first-time initialization. check_initialization
// sets this once its widget/chart setup completes.
static bool g_init_complete = false;

void check_initialization() {
    static bool initialization_complete = false;
    if (initialization_complete) return;

    static bool chart_initialized = false;
    static bool widgets_created = false;
    static double metadata_wait_start_s = 0.0;

    const Terminal::Pair pair{g_initial_route.exchange, g_initial_route.symbol};

    // Pack mode boots with NO WebSocket (the .edpack + metadata fetch are the
    // only network I/O - the box stays out of the per-viewer loop), so init
    // proceeds on the pack path without a socket.
    // Embedded tick replays use the replay lane; a slow/restarting live hub
    // must not hold their chart initialization. Curated archives still use hub.
    const auto& boot = EducationBoot::instance();
    WebSocketClient* boot_socket = boot.is_embedded() && !boot.is_event()
        ? replay_lane_socket() : g_app.ws_client.get();
    const bool comms_ready = boot.is_pack() ||
        (boot_socket && boot_socket->is_connected());
    if (!chart_initialized && comms_ready) {
        // Wait for metadata, but only to avoid a visible placeholder: widgets
        // built without it rebind through Widget::refresh_instrument() when the
        // registry lands, so timing out is no longer destructive.
        //
        // The budget is WALL CLOCK, not frames. The old "120 frames ≈ 2 seconds
        // at 60fps" was 0.66s on a 180fps client, and in pack mode the counter
        // starts at frame one (comms_ready is immediate with no socket) while
        // the 500 KB metadata fetch races the pack's own range requests: the
        // /demo terminal lost that race consistently and drew every price at
        // placeholder precision for the whole session.
        const double now_s = ImGui::GetTime();
        if (metadata_wait_start_s == 0.0) metadata_wait_start_s = now_s;
        bool metadata_ready = SymbolRegistry::instance().is_loaded();
        if (!metadata_ready && (now_s - metadata_wait_start_s) < 3.0) {
            return;  // Try again next frame
        }
        // 0 = unknown. No invented tick anywhere: ChartWidget draws on
        // provisional precision and rebinds, the DOM says so and waits.
        double tick_size = SymbolRegistry::instance().tick_or_zero(pair.exchange, pair.symbol);
        // In embedded lesson mode the replay is the ONLY data source - skip the
        // live initial candle load (otherwise the chart floods with current
        // market data before the replay swaps in). The replay session populates
        // candles for the lesson window via the normal pipeline.
        if (!EducationBoot::instance().is_embedded() &&
            !EducationBoot::instance().is_pack()) {
            g_app.candle_mgr->initial_load();
        }
        auto initial_chart = std::make_unique<ChartWidget>(pair, g_app.app_ctx, tick_size);
        if ((EducationBoot::instance().is_pack() && EducationBoot::instance().pack_realtime()) ||
            (!Entitlements::hosted() && !boot.is_embedded() && !boot.is_pack()))
            initial_chart->set_rt_mode(true);
#ifdef __EMSCRIPTEN__
        // Recent-BTC links continue the welcome demo's recorded depth view.
        // This only sets the first chart's view; replay admission stays unchanged.
        if (EducationBoot::instance().is_research_replay() &&
            EM_ASM_INT({ return new URLSearchParams(location.search).get('realtime') === '1'; }))
            initial_chart->set_rt_mode(true);
#endif
        g_app.widgets.push_back(std::move(initial_chart));
        // The native shell (topbar + stats strip) is suppressed in embedded
        // lesson mode (React owns it), so skip its init too - it subscribes the
        // global ticker24h feed, which would be a live leak in a replay lesson.
        // Pack mode also skips it: no WS means the strip would render empty.
        if (!EducationBoot::instance().is_embedded() &&
            !EducationBoot::instance().is_pack()) {
            AppShell::init(g_app.app_ctx, pair);
        }
        chart_initialized = true;
        return;
    }
    if (chart_initialized && !widgets_created && LayoutManager::is_initialized) {
        static int frames_waited = 0;
        if (frames_waited++ > 3) {
            auto fmt = SymbolRegistry::instance().get_formatter(pair.exchange, pair.symbol);
            double dom_tick = SymbolRegistry::instance().tick_or_zero(pair.exchange, pair.symbol);

            if (EducationBoot::instance().is_embedded() ||
                EducationBoot::instance().is_pack()) {
                // Lesson layout: the chart (already created above) carries the
                // heatmap; the right side is the Order Flow read (DOM ladder +
                // tape). NO Watchlist, NO separate Depth/Orderbook widget - a
                // lesson is a focused single-symbol study, not a scanning desk.
                // Pack (/demo) mode uses the same focused set: the watchlist is
                // a live-data widget and there is no WS in a pack session.
                g_app.widgets.push_back(std::make_unique<DOMWidget>(
                    pair, g_app.app_ctx, dom_tick, 20
                ));
                g_app.widgets.push_back(std::make_unique<TradesWidget>(
                    pair, g_app.app_ctx, fmt
                ));
            } else {
                // Phase E: Depth (OrderbookWidget) is no longer docked by default -
                // the right column is the Order Flow read (DOM ladder + tape). Depth
                // stays available on demand via the +Widget menu.
                g_app.widgets.push_back(std::make_unique<DOMWidget>(
                    pair, g_app.app_ctx, dom_tick, 20
                ));
                g_app.widgets.push_back(std::make_unique<TradesWidget>(
                    pair, g_app.app_ctx, fmt
                ));
                g_app.widgets.push_back(std::make_unique<WatchlistWidget>(
                    pair, g_app.app_ctx
                ));
            }
            for (auto& w : g_app.widgets)
                if (w->type() == WidgetType::DOM || w->type() == WidgetType::Trades) {
                    w->mode_companion = true;
                    w->follows_route = true;
                }
            widgets_created = true;
            initialization_complete = true;
            g_init_complete = true;
        }
    }
}

// ─── Instrument metadata rebind ──────────────────────────────────────────────
// Tick size and price precision are not available at a fixed point in the boot:
// the metadata fetch is ~500 KB over the network, a /demo pack carries its own
// tick in a header that arrives later still, and widgets are built in between.
// Every widget that caches either one re-reads it here when the registry moves.
// Driven from the frame loop rather than from the fetch callback on purpose:
// the callback re-enters wasm from a browser event and would rebuild widget
// state mid-frame, between an ImGui Begin and its End. Same safe-point rule as
// the +widget drain and the live-flow rebuild above.
void resolve_instrument_metadata_rebind() {
    static uint32_t seen_epoch = 0;
    const uint32_t epoch = SymbolRegistry::instance().epoch();
    if (epoch == seen_epoch) return;
    seen_epoch = epoch;
    for (auto& w : g_app.widgets) {
        if (w) w->refresh_instrument();
    }
}

// A replay of another market replaces the primary chart. Rebuild it only at
// the safe widget-add point, once the replay-owned identity has been retired.
namespace {
struct LiveFlowRebuild {
    bool armed = false;
    bool want_chart = false;   // set when a replay of another symbol replaced it
    Terminal::Pair pair{};
};
LiveFlowRebuild g_live_flow_rebuild;
}  // namespace

void resolve_live_flow_widget_rebuild() {
    if (!g_live_flow_rebuild.armed) return;
    // Wait for the sweep to actually retire the replay-owned pair. Building now
    // would put two widgets on one ImGui docking identity, and that does not
    // stack, it corrupts the frame (see the BeginCount guard in
    // ChartWidget::render).
    const bool replay_widgets_gone = std::none_of(
        g_app.widgets.begin(), g_app.widgets.end(),
        [](const auto& w) { return w && w->is_replay_widget; });
    if (!replay_widgets_gone) return;

    g_live_flow_rebuild.armed = false;
    const Terminal::Pair pair = g_live_flow_rebuild.pair;
    if (pair.symbol.empty()) return;

    const double dom_tick = SymbolRegistry::instance().tick_or_zero(pair.exchange, pair.symbol);

    // A replay of a different symbol replaced the chart, so the dock tree in
    // place is keyed on the REPLAY pair's titles. Re-key it for the live pair
    // before the windows are submitted, or they come up floating for the same
    // reason the library launch used to float its DOM. render_dockspace picks
    // the queued pair up on the very next call, which is the line after this.
    if (g_live_flow_rebuild.want_chart) {
        LayoutManager::reset_layout_for(pair.exchange, pair.symbol);
        g_app.widgets.push_back(
            std::make_unique<ChartWidget>(pair, g_app.app_ctx, dom_tick));
        g_live_flow_rebuild.want_chart = false;
    }
}

// Embedded lesson mode: start the replay for the lesson's window exactly once,
// as soon as BOTH the terminal is initialized AND the lesson doc has parsed.
// These race - the lesson arrives via async XHR that can land before or after
// check_initialization - so this is a standalone per-frame latch rather than
// inlined in check_initialization (where it would miss if the doc came later).
void pause_live_for_historical_replay() {
    if (g_app.stream_mgr) {
        g_app.stream_mgr->pause_live_subscriptions();
    }
    if (g_app.debug_mgr) {
        g_app.debug_mgr->unsubscribe();
    }
}

void maybe_start_lesson_replay() {
    static bool started = false;
    if (started) return;
    if (!g_init_complete) return;
    // Lesson-only: is_embedded() now also covers studio + event (which start their
    // replays via their own paths), so gate on the lesson mode specifically.
    if (!EducationBoot::instance().is_lesson()) return;
    if (!edu::LessonRuntime::instance().loaded()) return;
    if (!g_app.replay_mgr) return;

    const auto& src = edu::LessonRuntime::instance().lesson().source;
    if (src.symbol.empty() || src.startMs <= 0 || src.endMs <= src.startMs) return;

    // Case per the session venue (fixed at boot; lessons are binancef today).
    const std::string sym = normalize_symbol_case(g_app.replay_mgr->exchange(), src.symbol);
    // Historical pages fail closed. Stop live delivery before the session POST,
    // including the Creating and Joining states where no replay context exists.
    pause_live_for_historical_replay();
    g_app.replay_mgr->request_replay({sym}, src.startMs, src.endMs, 1.0f);
    started = true;
}

// Event (archive) replay: start the archived market-event replay exactly once,
// as soon as the terminal is initialized. Mirrors maybe_start_lesson_replay but
// uses the archive session path (request_archive_replay → POST
// /replay/session/archive → join). The backend resolves event_id → archive_path
// and streams the event window. Transport is owned by the React EventReplayBar
// (event mode is now is_embedded(), so the native control bar is suppressed) -
// EventRuntime mirrors this session's state to it. seekToStart is implicit: the
// archive plays from the start of the event window.
void maybe_start_event_replay() {
    static bool started = false;
    if (started) return;
    if (!g_init_complete) return;
    if (!EducationBoot::instance().is_event()) return;
    if (!g_app.replay_mgr) return;

    const std::string& eid = EducationBoot::instance().event_id();
    if (eid.empty()) return;

    std::string sym = EducationBoot::instance().event_symbol();
    std::transform(sym.begin(), sym.end(), sym.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    pause_live_for_historical_replay();
    g_app.replay_mgr->request_archive_replay(eid, sym, 1.0f);
    started = true;
}

// Pack (CDN .edpack) replay: start exactly once, as soon as the terminal is
// initialized. Mirrors maybe_start_event_replay but there is no session POST
// and no WebSocket - ReplayManager::request_pack_replay boots the
// PackReplayEngine, which fetches the pack header and drives the same replay
// state machine (Hot Replay Path B, slice 2).
void maybe_start_pack_replay() {
    static bool started = false;
    if (started) return;
    if (!g_init_complete) return;
    if (!EducationBoot::instance().is_pack()) return;
    if (!g_app.replay_mgr) return;

    const std::string& url = EducationBoot::instance().pack_url();
    if (url.empty()) return;

    g_app.replay_mgr->request_pack_replay(url, EducationBoot::instance().pack_symbol(), 1.0f);
    g_app.replay_mgr->set_pack_checkpoint(EducationBoot::instance().pack_checkpoint_ms());
    started = true;
}

// STANDALONE pack start-at (?packt=<epoch-ms>): the embedded chrome applies
// __EDGEDEPTH_PACK__.seekToMs via EventRuntime's one-shot deep-link seek, but
// EventRuntime never runs standalone. Mirror the exact same contract here:
// ONE deliberate ReplayManager::seek once the session is live and primed
// (Playing or Paused - joined AND past the buffering gate) so the chart's
// re-request cascade and the seeded book behave identically to a user seek -
// then the latch closes.
void maybe_apply_pack_start_at() {
    static bool done = false;
    if (done) return;
    const auto& boot = EducationBoot::instance();
    if (!boot.is_pack() || boot.is_embedded()) { done = true; return; }
    const int64_t want = boot.pack_seek_to_ms();
    if (want <= 0) { done = true; return; }
    if (!g_app.replay_mgr) return;
    ReplayManager& rm = *g_app.replay_mgr;
    // Playing/Paused ONLY (same fix as EventRuntime's deep-link latch): the old
    // gate admitted Creating/Joining (is_active() counts them; is_loading() is
    // Buffering-only), firing the seek before the engine had a session to serve.
    if (rm.state() != ReplayManager::State::Playing &&
        rm.state() != ReplayManager::State::Paused) {
        return;  // not primed yet - retry next frame
    }
    const int64_t start = rm.info().start_time_ms;
    const int64_t end   = rm.info().end_time_ms;
    if (end <= start) return;
    rm.seek(std::clamp<int64_t>(want, start, end), /*deliberate=*/true);
    done = true;
}

// ── Lesson loading gate ──────────────────────────────────────────────────────
// The actual clock-hold now lives in ReplayManager::tick_buffering_gate() so it
// applies to EVERY replay (lessons AND the live terminal's "replay from here") -
// no replay starts ticking against empty buffers. Here we only mirror that into
// g_lesson_loading so the lesson canvas spinner + the React 'loading' emit track
// it. (Trades aren't a separate manager; candles+OB is the readiness bar - see
// ReplayManager::context_primed.)
bool g_lesson_loading = false;

void tick_lesson_loading_gate() {
    // Cover the entire asynchronous boot, plus post-seek buffering, so an event
    // or lesson never exposes the underlying live canvas while replay is pending.
    g_lesson_loading = false;
    if (!EducationBoot::instance().is_embedded() || !g_app.replay_mgr) return;
    const ReplayManager::State state = g_app.replay_mgr->state();
    const auto& boot = EducationBoot::instance();
    g_lesson_loading =
        ((boot.is_event() || boot.is_lesson()) && state == ReplayManager::State::Idle) ||
        state == ReplayManager::State::Creating ||
        state == ReplayManager::State::Joining ||
        state == ReplayManager::State::Buffering ||
        state == ReplayManager::State::Seeking;
}

// Canvas loading overlay - masks the empty-chart window while the lesson holds
// the clock. Drawn on the foreground draw list so it sits above all widgets.
// A later React chrome can show its own polished overlay (driven by the emitted
// 'loading' flag); this is the zero-gap, no-dependency canvas fallback.
void render_lesson_loading_overlay() {
    if (!g_lesson_loading) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* fg = ImGui::GetForegroundDrawList(vp);
    const ImVec2 c = ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
                            vp->Pos.y + vp->Size.y * 0.5f);

    // Full-viewport scrim so the half-built chart doesn't flash through. The one spinner while it is up: the
    // real-time chart skips its own in embedded mounts (chart_widget_realtime.cpp).
    fg->AddRectFilled(vp->Pos,
                      ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                      Theme::u32(Theme::Tokens::BASE, 0.96f));
    const float r = 22.0f;
    Theme::spinner(fg, c, r, 3.0f);
    const char* msg = "Loading replay";
    const ImVec2 ts = ImGui::CalcTextSize(msg);
    fg->AddText(ImVec2(std::round(c.x - ts.x * 0.5f), c.y + r + 16.0f), Theme::u32(Theme::Tokens::TX2), msg);
}

// Keep failed historical data frozen and capture chart input. Recovery actions
// explicitly re-seed the retained session; a lost session requires a reload.
void render_historical_replay_error_overlay() {
    const auto& boot = EducationBoot::instance();
    if ((!boot.is_event() && !boot.is_lesson()) || !g_app.replay_mgr) return;
    auto& replay = *g_app.replay_mgr;
    if (replay.state() != ReplayManager::State::Error) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);
    ImVec4 veil = Theme::Tokens::BASE;
    veil.w = 0.58f;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, veil);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##historical_replay_error", nullptr, flags | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    const float width = std::min(520.0f, std::max(180.0f, vp->Size.x - 32.0f));
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
                                 vp->Pos.y + vp->Size.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, vp->Size.y - 24.0f));
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Tokens::MENU);
    ImGui::PushStyleColor(ImGuiCol_Border, Theme::Tokens::BD2);
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::TX1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 22.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 12.0f));
    bool retry = false, restart = false, reload = false;
    if (ImGui::Begin("##replay_recovery_card", nullptr, flags | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(Theme::Tokens::WARN, "REPLAY PAUSED");
        ImGui::PushFont(Theme::get_bold_font());
        ImGui::TextWrapped("Couldn't load this replay position");
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::TX2);
        const bool can_retry = replay.can_retry_historical_position();
        ImGui::TextWrapped(can_retry
            ? "Try this position again, or play from the beginning of the recording."
            : "Reload the page to reconnect to this recording.");
        if (replay.info().current_time_ms > 0) {
            char when[80]{}, zone[64]{};
            auto& tz = DisplayTimeZone::instance();
            tz.format(replay.info().current_time_ms, TimeZoneFormat::DateTimeSeconds, when, sizeof(when));
            tz.visible_label(replay.info().current_time_ms, true, zone, sizeof(zone));
            ImGui::TextWrapped("%s  %s", when, zone);
        }
        ImGui::PopStyleColor();
        const bool stacked = width < 440.0f;
        const float button_width = stacked ? ImGui::GetContentRegionAvail().x
            : (ImGui::GetContentRegionAvail().x - 10.0f) * 0.5f;
        if (can_retry) {
            ImGui::PushStyleColor(ImGuiCol_Button, Theme::Tokens::BRAND);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Tokens::BRAND);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Tokens::BRAND);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::BRAND_INK);
            retry = ImGui::Button("Retry position", ImVec2(button_width, 38.0f));
            ImGui::PopStyleColor(4);
            if (!stacked) ImGui::SameLine();
            restart = ImGui::Button("Play from start", ImVec2(button_width, 38.0f));
        } else {
            reload = ImGui::Button("Reload replay", ImVec2(-1.0f, 38.0f));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::TX3);
        if (!replay.info().error_message.empty() && ImGui::TreeNode("Details")) {
            ImGui::TextWrapped("%s", replay.info().error_message.c_str());
            ImGui::TreePop();
        }
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(3);
    if (retry) replay.seek(replay.info().current_time_ms, true);
    if (restart) replay.seek(replay.info().start_time_ms, true);
#ifdef __EMSCRIPTEN__
    if (reload) EM_ASM({ window.location.reload(); });
#else
    (void)reload;
#endif
}

void drain_data_dispatches(double budget_ms, bool profile) {
    // Drain queued dispatches from data thread → fire widget callbacks on main
    // thread. TIME-BUDGETED: ingest storms (replay catch-up, reconnect bursts)
    // used to execute unbounded in one frame - the classic 90-FPS dip. The
    // remainder stays queued in order and resumes next frame.
    // Unconditional on the data thread object: the pthread-free build parses
    // inline, but a queue could still hold early-boot dispatches (the queue is
    // only dropped after start() has failed) and the drain must not skip it.
    if (g_app.data_thread) {
        if (!g_app.data_thread->is_running()) budget_ms = std::max(budget_ms, 6.0);
        if (profile) g_profiler.begin("Dispatch");
        const size_t n = g_app.data_thread->drain_dispatches(*g_app.stream_mgr,
                                                             budget_ms);
        if (profile) {
            g_profiler.end("Dispatch");
            g_profiler.add_count("Dispatch", static_cast<long>(n));
        }
        if (const size_t backlog = g_app.data_thread->carry_backlog(); profile && backlog > 0) {
            g_profiler.add_count("DispBacklog", static_cast<long>(backlog));
        }
        // The queue is bounded (DispatchQueue::kMaxPending): a window hidden
        // long enough drops its oldest trades and candle updates. Repair the
        // charts the same way a planned hub restart does, with a recent-candle
        // refetch reaching back to where the window stopped draining; RT trade
        // archives keep the gap. The second pass picks up the newest dropped
        // seconds, which sub-minute history only serves once the server's
        // trade capture (10 s buckets) has reached its database.
        std::uint64_t dropped = 0;
        double gap_start_ms = 0.0;
        if (g_app.data_thread->dispatch_queue().take_drops(dropped, gap_start_ms)) {
#ifdef EDGEDEPTH_EXPOSURE_V2_DEV
            exposure::History::invalidate_all("V2 dispatch gap; reload required");
            exposure::SummaryHistory::invalidate_all("V2 dispatch gap; reload required");
#endif
            if (profile) g_profiler.add_count("DispDropped", static_cast<long>(dropped));
            const double span_ms = std::max(0.0, DispatchQueue::steady_ms() - gap_start_ms);
            g_refetch_outage_ms = std::max(g_refetch_outage_ms, span_ms);
            const double now = emscripten_get_now();
            g_refetch_at_ms[0] = now + 2000;
            if (g_refetch_at_ms[1] == 0) g_refetch_at_ms[1] = now + 30000;
        }
    }
}

#ifdef __EMSCRIPTEN__
extern "C" EMSCRIPTEN_KEEPALIVE void capture_background_live() {
    if (g_app.done || !EM_ASM_INT({ return document.hidden; }) ||
        !g_app.replay_mgr || g_app.replay_mgr->is_active()) return;
    // Browsers throttle timers, but even a minute cadence is inside the depth
    // owner's five-minute ring. A frozen/discarded page still leaves real gaps.
    drain_data_dispatches(12.0, false);
    const int64_t clock = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    RealtimeArchive::update_live(*g_app.ob_mgr, *g_app.stream_mgr, clock);
}
#endif

void resolve_mode_companions() {
    for (auto& w : g_app.widgets) {
        w->hidden_by_chart = false;
        if (!w->is_open || !w->mode_companion || w->suspended_for_replay) continue;
        const Terminal::Pair* pair = nullptr;
        if (w->type() == WidgetType::DOM) pair = &static_cast<DOMWidget*>(w.get())->pair();
        if (w->type() == WidgetType::Trades) pair = &static_cast<TradesWidget*>(w.get())->pair();
        if (!pair) continue;
        bool owner_rt = false, needed = false;
        for (const auto& candidate : g_app.widgets) {
            if (!candidate->is_open || candidate->type() != WidgetType::Chart || candidate->suspended_for_replay) continue;
            const auto& chart = *static_cast<const ChartWidget*>(candidate.get());
            if (chart.pair().exchange != pair->exchange || chart.pair().symbol != pair->symbol) continue;
            if (chart.chart_instance() == 1) owner_rt = chart.rt_mode();
            needed |= !chart.rt_mode() || chart.show_standalone_panels();
        }
        w->hidden_by_chart = owner_rt && !needed;
    }
}

namespace {
enum class HostedRequest { None, Begin, End };
HostedRequest g_hosted_request = HostedRequest::None;
std::string   g_hosted_payload;          // __EDGEDEPTH_HOSTED_REPLAY__ (JSON) at begin
bool          g_hosted_returning = false; // exited; live workspace not re-applied yet

bool hosted_research_panel(WidgetType type) {
    return type == WidgetType::Chart || type == WidgetType::DOM || type == WidgetType::Trades;
}

// The live-only panels stay out of the frame while hosted, and after the exit
// until workspace::tick has re-applied the live workspace (that tick re-docks
// them; submitting them earlier floats them over the research layout's tree).
bool hosted_focus_layout() {
    return EducationBoot::instance().hosted_research() ||
           (g_hosted_returning && !workspace::enabled());
}

}  // namespace

void update_and_render_widgets() {
    resolve_mode_companions();
    drain_data_dispatches(3.0, true);
    // Swap the ACTIVE orderbook manager's buffers (write→read).
    // During replay, app_ctx.orderbooks points to the replay OB manager.
    // During live, it points to the live OB manager. Either way, this
    // swaps the one the widgets are actually reading from.
    if (g_app.app_ctx.orderbooks) {
        g_profiler.begin("OBSwap");
        g_app.app_ctx.orderbooks->swap_buffers();
        g_profiler.end("OBSwap");
    }
    g_profiler.begin("WidgetsUpd");
    for (const auto& widget: g_app.widgets) {
        if (widget->is_open && !widget->suspended_for_replay && !widget->hidden_by_chart)
            widget->update();
    }
    g_profiler.end("WidgetsUpd");
    // REC focus layout: don't SUBMIT the watchlist while a focused clip records -
    // its dock node collapses and the chart takes the width; on stop the window
    // re-docks into its remembered node. (Render-skip only: the widget object,
    // subscriptions and is_open are untouched - is_open=false would ERASE it.)
    const bool rec_focus_widgets = ClipRecorder::focus_active() ||
                                   edu::RecorderRuntime::instance().active();
    // Hosted research replay: the research viewer's panel set only (chart, DOM,
    // tape). Render-skip, never is_open=false, for the reason given above.
    const bool hosted_focus = hosted_focus_layout();
    for (const auto& widget: g_app.widgets) {
        if (!widget->is_open || widget->hidden_by_chart || widget->suspended_for_replay) continue;
        if (rec_focus_widgets && widget->type() == WidgetType::Watchlist) continue;
        if (hosted_focus && !hosted_research_panel(widget->type())) continue;
        // Per-widget render timing - labels by widget type so the once/sec
        // console profile shows exactly where frame time goes.
        const char* sec = "Widget";
        switch (widget->type()) {
            case WidgetType::Chart:        sec = "Chart";     break;
            case WidgetType::DOM:          sec = "DOM";       break;
            case WidgetType::Orderbook:    sec = "Orderbook"; break;
            case WidgetType::Trades:       sec = "Trades";    break;
            case WidgetType::Watchlist:    sec = "Watchlist"; break;
            case WidgetType::Data:         sec = "Data";      break;
            case WidgetType::Stats:        sec = "Stats";     break;
            case WidgetType::PaperTrading: sec = "Paper";     break;
            case WidgetType::ReplayLibrary: sec = "ReplayLib"; break;
            default: break;
        }
        // An undocked panel is floating chrome: rounded like the menus and cards, with their shadow (drawn below);
        // docked panels stay square. Decided from the window's state last frame, so a new window is square for one.
        char wname[512];
        std::snprintf(wname, sizeof(wname), "%s%s", widget->title(), widget->title_suffix().c_str());
        const ImGuiWindow* last = ImGui::FindWindowByName(wname);
        const bool floating = last && !last->DockIsActive && last->WasActive;
        if (floating) ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::Radius::R3);
        g_profiler.begin(sec);
        widget->render();
        g_profiler.end(sec);
        if (floating) ImGui::PopStyleVar();
    }
    // Floating panels: the menus' shadow outside the frame, and the focus hairline on the focused one's title bar,
    // as on the focused panel's tab.
    {
        const ImGuiWindow* nav = GImGui->NavWindow ? GImGui->NavWindow->RootWindow : nullptr;
        for (const auto& widget : g_app.widgets) {
            char wname[512];
            std::snprintf(wname, sizeof(wname), "%s%s", widget->title(), widget->title_suffix().c_str());
            ImGuiWindow* w = ImGui::FindWindowByName(wname);
            if (!w || w->DockIsActive || w->LastFrameActive != ImGui::GetFrameCount() || w->Hidden) continue;
            const ImVec2 a = w->Pos, b(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
            Theme::float_shadow(w->DrawList, a, b, w->WindowRounding);
            if (w == nav && !(w->Flags & ImGuiWindowFlags_NoTitleBar)) {
                // The same 1 px grey as a focused dock tab's line (theme.cpp), inside the rounded corners.
                const float r = w->WindowRounding;
                w->DrawList->PushClipRectFullScreen();
                w->DrawList->AddRectFilled(ImVec2(a.x + r, a.y), ImVec2(b.x - r, a.y + 1.0f), Theme::u32(Theme::Tokens::BD3));
                w->DrawList->PopClipRect();
            }
        }
    }
    // Draw venue images in the reserved leading space of native dock tabs
    // and floating title bars. Keep stable ### IDs and native tab interactions.
    for (const auto& widget : g_app.widgets) {
        const Terminal::Pair* pair = nullptr;
        if (widget->type() == WidgetType::Chart) pair = &static_cast<ChartWidget*>(widget.get())->pair();
        if (widget->type() == WidgetType::DOM) pair = &static_cast<DOMWidget*>(widget.get())->pair();
        if (widget->type() == WidgetType::Trades) pair = &static_cast<TradesWidget*>(widget.get())->pair();
        if (widget->type() == WidgetType::Orderbook) pair = &static_cast<OrderbookWidget*>(widget.get())->pair();
        if (widget->type() == WidgetType::Data) pair = &static_cast<DataWidget*>(widget.get())->pair();
        if (widget->type() == WidgetType::Stats) pair = &static_cast<StatsWidget*>(widget.get())->pair();
        if (!pair) continue;
        char name[512];
        std::snprintf(name, sizeof(name), "%s%s", widget->title(), widget->title_suffix().c_str());
        auto* window = ImGui::FindWindowByName(name);
        if (!window || window->LastFrameActive != ImGui::GetFrameCount()) continue;
        ImDrawList* draw = window->DrawList;
        ImRect rect = window->TitleBarRect();
        if (window->DockNode) {
            auto* bar = window->DockNode->TabBar;
            auto* tab = bar ? ImGui::TabBarFindTabByID(bar, window->TabId) : nullptr;
            if (!tab || !window->DockNode->HostWindow || window->DockNode->IsHiddenTabBar()) continue;
            rect.Min = ImVec2(bar->BarRect.Min.x + tab->Offset - bar->ScrollingAnim, bar->BarRect.Min.y);
            rect.Max = ImVec2(rect.Min.x + tab->Width, bar->BarRect.Max.y);
            draw = window->DockNode->HostWindow->DrawList;
        }
        const float size = std::min(16.0f, rect.GetHeight() - 4.0f);
        if (size <= 0) continue;
        const ImVec2 pos(rect.Min.x + ImGui::GetStyle().FramePadding.x, rect.GetCenter().y - size * 0.5f);
        draw->PushClipRect(rect.Min, rect.Max, true);
        const auto texture = LogoManager::instance().exchange(pair->exchange);
        if (texture) draw->AddImage(texture, pos, ImVec2(pos.x + size, pos.y + size));
        else draw_logo_monogram(draw, pair->exchange == "hl" ? "HL" : pair->exchange == "bybit" ? "BY" : pair->exchange == "okx" ? "OK" : "B", pos, size);
        draw->PopClipRect();
        if (ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size, pos.y + size)))
            Theme::tooltip("%s", widget_venue_label(pair->exchange));
    }
    // Dim the tabs a panel is not showing (label and venue image), so the selected one reads at a glance; hovering
    // one lifts it. ImGui draws every tab label in one text colour, so this veils them with the strip's own tone.
    {
        ImGuiContext& g = *GImGui;
        const ImU32 veil = Theme::u32(Theme::Tokens::BASE, 0.52f);
        for (int n = 0; n < g.DockContext.Nodes.Data.Size; ++n) {
            auto* node = static_cast<ImGuiDockNode*>(g.DockContext.Nodes.Data[n].val_p);
            if (!node || !node->TabBar || !node->HostWindow || node->IsHiddenTabBar() ||
                node->HostWindow->LastFrameActive != ImGui::GetFrameCount()) continue;
            ImGuiTabBar* bar = node->TabBar;
            if (bar->Tabs.Size < 2) continue;
            ImDrawList* draw = node->HostWindow->DrawList;
            for (const ImGuiTabItem& tab : bar->Tabs) {
                if (tab.ID == bar->SelectedTabId) continue;
                const ImVec2 a(bar->BarRect.Min.x + tab.Offset - bar->ScrollingAnim, bar->BarRect.Min.y);
                const ImVec2 b(a.x + tab.Width, bar->BarRect.Max.y);
                if (ImGui::IsMouseHoveringRect(a, b)) continue;
                draw->PushClipRect(bar->BarRect.Min, bar->BarRect.Max, true);
                draw->AddRectFilled(a, b, veil);
                draw->PopClipRect();
            }
        }
    }
    std::erase_if(g_app.widgets,
                  [](const auto& w) { return !w->is_open; });
}


// (render_replay_overlay removed - the amber full-viewport border + top-left
//  REPLAY badge were redundant with the restyled control bar + topbar indicator.)

// ── Embedded-mode canvas sizing ───────────────────────────────────────────────
// In the Next host (/studio, /lesson) the canvas is NOT the full window - it lives
// in a flex box (symbol tree + inspector around it). Polling window.innerWidth
// there is wrong on two counts: (1) ImGui lays the dock out over a virtual
// full-window space, so the right column (DOM/Depth) lands off-screen right; and
// (2) the client forces canvas.width/height to full-window, fighting
// TerminalEmbed's ResizeObserver (which sizes the backing buffer to the CSS box ×
// dpr) → the squished render. The React side pushes the real CSS box here via
// _edu_set_viewport (instant); main_loop also polls the canvas box as a fallback
// seed (the RO can fire before the wasm exports are callable).
static int    g_embed_view_w     = 0;
static int    g_embed_view_h     = 0;
static double g_embed_view_dpr   = 1.0;
static bool   g_embed_view_dirty = false;
// Deferred dock reflow: during a live resize (devtools drag, host panel drag)
// the RO pushes a new size every few ms. Rebuilding the dock per step cleared
// the canvas + burned a full DockBuilder pass per frame; instead the size is
// applied instantly (ImGui rescales the dock tree proportionally) and ONE
// reflow snaps the 248/flex/300 design widths after the size settles.
static bool   g_embed_reflow_pending = false;
static double g_embed_last_resize_t  = 0.0;
// Last size each viewport path applied. File scope (they were function
// statics) so a hosted research replay can invalidate them: the live terminal
// switches from the window-sized path to the canvas-box path on entry and back
// on exit, and a path that still remembers the size it applied last time would
// skip the resize the switch needs (SDL keeps the other path's window size and
// ImGui lays out over the wrong box).
static int    g_embed_applied_w   = -1;
static int    g_embed_applied_h   = -1;
static double g_embed_applied_dpr = -1.0;
static bool   g_embed_first_apply = true;
static int    g_standalone_w      = 0;
static int    g_standalone_h      = 0;
static double g_standalone_dpr    = 0.0;
static double g_standalone_poll_at = 0.0;

#ifdef __EMSCRIPTEN__
// Apply a corrected embedded canvas size to ImGui + SDL and re-flow the dock.
// Dedupes on (w,h,dpr), so calling it every frame (push) or at 10Hz (poll) is cheap.
static void apply_embedded_viewport(int css_w, int css_h, double dpr) {
    if (css_w <= 0 || css_h <= 0) return;
    if (dpr <= 0.0) dpr = 1.0;
    // Floor the applied size: a devtools pane / host flexbox can squeeze the CSS
    // box to near-zero mid-drag. Layout + widgets never see a degenerate
    // viewport; the canvas may letterbox for a moment, which beats a dead frame.
    css_w = std::max(css_w, 320);
    css_h = std::max(css_h, 240);
    if (css_w == g_embed_applied_w && css_h == g_embed_applied_h &&
        std::abs(dpr - g_embed_applied_dpr) <= 0.01) return;
    g_embed_applied_w = css_w; g_embed_applied_h = css_h; g_embed_applied_dpr = dpr;

    // Logical CSS-px window size drives ImGui layout. SDL3's NewFrame recomputes
    // io.DisplaySize from SDL_GetWindowSize and the framebuffer scale from
    // GetWindowSizeInPixels/GetWindowSize, so the SDL window MUST track the CSS box
    // or NewFrame clobbers io.DisplaySize back to the stale boot size.
    SDL_SetWindowSize(g_app.window, css_w, css_h);

    // SDL_SetWindowSize reset the canvas backing buffer to the logical size (no
    // dpr), so re-apply the dpr scale for a crisp hi-dpi render - exactly what the
    // standalone path does. This is the SAME value TerminalEmbed's ResizeObserver
    // sets (CSS box × dpr), so the two don't fight: writing the width/height attrs
    // can't re-trigger the RO (it observes the CSS box, not the attrs). The old bug
    // was writing innerWidth × dpr (full window) here vs the RO's CSS box → squish.
    EM_ASM({
        var canvas = Module.canvas;
        canvas.width  = $0;
        canvas.height = $1;
    }, (int)(css_w * dpr + 0.5), (int)(css_h * dpr + 0.5));

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)css_w, (float)css_h);
    io.DisplayFramebufferScale = ImVec2((float)dpr, (float)dpr);

    // The dock layout is built ONCE from GetMainViewport()->Size; if that was the
    // stale boot size the DOM/Depth column is off-screen right. First real size →
    // reflow NOW (kills the boot squish). Later sizes → defer ONE reflow until the
    // size settles (see g_embed_reflow_pending). Embedded mode always uses the
    // default layout, so nothing the user arranged by hand is lost.
    if (g_embed_first_apply) {
        g_embed_first_apply = false;
        LayoutManager::relayout();
    } else {
        g_embed_reflow_pending = true;
        g_embed_last_resize_t  = ImGui::GetTime();
    }
}
#endif


#ifdef __EMSCRIPTEN__
// With more than one terminal window open, present every refresh only where
// it can be seen: the focused window, or one the pointer is over or that took
// input in the last second. Any other window presents every Nth refresh, N
// from the measured refresh rate, so it lands near 60 Hz. Measured on a 180 Hz + 155 Hz X11 desktop (2026-09-17): two
// full-rate terminals split across the monitors stalled each other in Firefox
// to 133/114 FPS with 20-46 ms P99 spikes and 1-2 ms of CPU per frame; the
// same pair with the second window at every 3rd refresh held 180 and 59.
// ?vsync=N pins the interval for measurement and disables the automatic rule.
namespace {
struct PresentationCadence {
    int fixed = 0;                 // ?vsync=N; 0 = automatic
    int current = 1;               // interval SDL was last given
    int full_rate_frames = 0;      // consecutive frames presented at interval 1
    double refresh_hz = 0.0;       // from the P50 interval of a full-rate window
    double last_poll = 0.0;
    double last_input = 0.0;
    bool pointer_over = false;     // canvas mouseenter/mousemove vs mouseleave, read each poll
} g_cadence;

void note_presentation_input() { g_cadence.last_input = ImGui::GetTime(); }

// Every terminal document holds the same shared Web Lock the web notice uses
// (edgedepth:terminal-tab:v1); the browser releases it when the document
// goes away, so counting distinct holders is an honest "other terminal
// windows" signal with nothing stored. Polled every 3 s into a JS global;
// browsers without locks report zero and keep the full rate.
void start_window_census() {
    EM_ASM({
        Module.__edgedepthOtherWindows = 0;
        // Pointer presence from the canvas's own events: SDL reports mouse
        // focus at boot whether or not the pointer is anywhere near the
        // window, so its enter/leave pair cannot be trusted for this.
        Module.__edgedepthPointerOver = 0;
        var canvas = Module.canvas;
        if (canvas) {
            var over = function() { Module.__edgedepthPointerOver = 1; };
            canvas.addEventListener('mouseenter', over);
            canvas.addEventListener('mousemove', over);
            canvas.addEventListener('mouseleave', function() { Module.__edgedepthPointerOver = 0; });
        }
        if (!navigator.locks) return;
        var name = 'edgedepth:terminal-tab:v1';
        var count = function() {
            navigator.locks.query().then(function(snapshot) {
                var ids = {};
                (snapshot.held || []).forEach(function(lock) {
                    if (lock.name === name) ids[lock.clientId] = 1;
                });
                Module.__edgedepthOtherWindows = Math.max(0, Object.keys(ids).length - 1);
            }).catch(function() {});
        };
        navigator.locks.request(name, { mode: 'shared' }, function() {
            count();
            setInterval(count, 3000);
            return new Promise(function() {});  // held for the document's lifetime
        }).catch(function() {});
    });
}

int other_terminal_windows() {
    return EM_ASM_INT({ return Module.__edgedepthOtherWindows | 0; });
}

bool pointer_over_canvas() {
    return EM_ASM_INT({ return Module.__edgedepthPointerOver | 0; }) != 0;
}

void tick_presentation_cadence() {
    if (g_cadence.fixed > 0) return;
    // The refresh rate is the median full-rate interval over a whole tracker
    // window: the minimum is fooled by rAF jitter (Firefox reported intervals
    // down to 4 ms on a 180 Hz panel), dropped frames only lengthen the P50.
    if (g_cadence.current == 1) {
        if (++g_cadence.full_rate_frames >= static_cast<int>(FrameTimeTracker::HISTORY_SIZE)) {
            g_cadence.full_rate_frames = 0;
            const auto stats = g_frame_tracker.compute();
            if (stats.p50 > 0.0) g_cadence.refresh_hz = 1000.0 / stats.p50;
        }
    } else {
        g_cadence.full_rate_frames = 0;
    }
    const double t = ImGui::GetTime();
    if (t - g_cadence.last_poll < 0.1) return;  // JS boundary, 10 Hz like the other polls
    g_cadence.last_poll = t;
    const bool focused = EM_ASM_INT({ return document.hasFocus() ? 1 : 0; }) != 0;
    g_cadence.pointer_over = pointer_over_canvas();
    const bool active = focused || g_cadence.pointer_over || t - g_cadence.last_input < 1.0;
    // A lone window keeps every refresh even while idle: the reduced cadence
    // exists to stop terminal windows contending with each other, not to
    // change what a single window looks like on a high-refresh display.
    const int others = other_terminal_windows();
    int target = 1;
    if (!active && g_cadence.refresh_hz > 0.0 && others > 0) {
        target = std::clamp(static_cast<int>(std::lround(g_cadence.refresh_hz / 60.0)), 1, 4);
    }
    if (target != g_cadence.current) {
        g_cadence.current = target;
        SDL_GL_SetSwapInterval(target);
    }
    if (PerformanceDiagnostics::enabled()) {
        char note[160];
        std::snprintf(note, sizeof(note),
                      "Cadence: %s%s%s, %d other window%s, refresh %.0f Hz, every %d",
                      focused ? "focused" : "unfocused",
                      g_cadence.pointer_over ? ", pointer" : "",
                      t - g_cadence.last_input < 1.0 ? ", input" : "",
                      others, others == 1 ? "" : "s", g_cadence.refresh_hz, target);
        PerformanceDiagnostics::set_cadence_note(note);
    }
}
}  // namespace
#endif

// ═══════════════════════════════════════════════════════════════════════════════
// Hosted research replay (2026-10-13)
//
// "Replay from here" on the live terminal used to navigate to /terminal?replay=,
// which reloaded the page and booted a second WASM module. The live page now
// hosts the research replay chrome around THIS canvas instead (web
// LiveTerminalHost): it validates the window on the server, pushes the replay
// URL into history, and calls Module.__hosted_replay_begin; Back or Exit calls
// Module.__hosted_replay_end. Both only RECORD the request (they arrive from a
// browser event, between frames); main_loop applies it at the top of the next
// frame so the whole frame sees one mode.
//
// While hosted, EducationBoot reports a research replay (mode Studio, chrome
// "research"), so every per-frame branch already does what the research viewer
// boot does: no native shell or control bar, StudioRuntime drains the source the
// React shell pushes, the viewport follows the canvas box. The live-only panels
// (watchlist, Data, Stats, depth, library, positions) are kept but not
// submitted, the same render-skip the clip recorder's focus layout uses. On exit
// the replay stops (the context swap returns every manager to live), and
// workspace::tick re-applies the live workspace checkpointed on entry.
// ═══════════════════════════════════════════════════════════════════════════════
namespace {
void apply_hosted_replay_transition() {
    if (g_hosted_request == HostedRequest::None) return;
    // Hold the request until the live terminal exists; the host page cannot
    // offer "Replay from here" before then, but Forward into a hosted entry can
    // race the boot.
    if (!g_init_complete || !g_app.replay_mgr) return;
    const HostedRequest req = g_hosted_request;
    g_hosted_request = HostedRequest::None;
    auto& boot = EducationBoot::instance();
    if (req == HostedRequest::Begin) {
        std::string symbol, exchange;
        bool realtime = false;
        try {
            const nlohmann::json j = nlohmann::json::parse(g_hosted_payload);
            symbol   = j.value("symbol", "");
            exchange = j.value("exchange", "");
            realtime = j.value("realtime", false);
        } catch (const nlohmann::json::exception&) {
            return;
        }
        std::transform(exchange.begin(), exchange.end(), exchange.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!is_known_exchange(exchange)) exchange = g_initial_route.exchange;
        symbol = normalize_symbol_case(exchange, symbol);
        if (symbol.empty()) return;
        const bool was_hosted = boot.hosted_research();
        // Checkpoint BEFORE the switch: the tick that follows runs disabled, and
        // the checkpoint is what the exit restores.
        if (!was_hosted) workspace::checkpoint_live();
        if (!boot.begin_hosted_research(symbol, exchange)) return;
        edu::StudioRuntime::instance().begin_hosted();
        // Every session plays one venue; a chart of another venue replays on
        // its own. Restored to the route's venue on exit.
        g_app.replay_mgr->set_exchange(exchange);
        if (!was_hosted) {
            // First canvas-box size docks the research layout immediately,
            // exactly as a research viewer boot does.
            g_embed_applied_w = -1;
            g_embed_applied_h = -1;
            g_embed_applied_dpr = -1.0;
            g_embed_first_apply = true;
            g_embed_reflow_pending = false;
            // Measure the canvas box afresh (the poll reads its rect) rather
            // than consume a push the ResizeObserver sent at boot.
            g_embed_view_dirty = false;
        }
        g_hosted_returning = false;
        if (realtime) {
            // ?realtime=1 (the recent-BTC shortcut): the same first-chart view
            // a research viewer boot applies.
            for (auto& w : g_app.widgets) {
                if (!w || w->type() != WidgetType::Chart) continue;
                auto* chart = static_cast<ChartWidget*>(w.get());
                if (chart->chart_instance() == 1 && !chart->is_secondary()) {
                    chart->set_rt_mode(true);
                    break;
                }
            }
        }
        return;
    }
    if (!boot.hosted_research()) return;
    ReplayManager& rm = *g_app.replay_mgr;
    // Same pair the native "Close replay" button uses: stop() retires the
    // context (the swap returns every manager to live and resumes the live
    // subscriptions), reset() clears the session for the next replay.
    rm.close_replay();
    edu::StudioRuntime::instance().reset_hosted();
    boot.end_hosted_research();
    rm.set_exchange(g_initial_route.exchange);
    // Back to the window-sized viewport: forget the size that path applied
    // before the replay, so the next poll resizes SDL and the canvas buffer.
    g_standalone_w = 0;
    g_standalone_h = 0;
    g_standalone_dpr = 0.0;
    g_standalone_poll_at = 0.0;
    g_embed_reflow_pending = false;
    // Baseline live dock for the route pair; workspace::tick replaces it with
    // the checkpoint taken on entry (normally in this same frame).
    LayoutManager::reset_layout_for(g_initial_route.exchange, g_initial_route.symbol);
    g_hosted_returning = true;
}
}  // namespace

void main_loop() {
    apply_hosted_replay_transition();
    if (g_app.ws_client) g_app.ws_client->tick();
    if (g_app.replay_ws_client) g_app.replay_ws_client->tick();
    tick_restart_refetch();
    static std::chrono::steady_clock::time_point last_frame{};
    static bool has_previous_frame = false;
    const auto now = std::chrono::steady_clock::now();
    [[maybe_unused]] ProfileFrameScope frame_cpu_scope(g_profiler, now);

    // Record frame time for percentile tracking (interval between loop entries -
    // includes browser idle between rAF ticks; what the FPS counter reflects)
    float frame_ms = 0.0f;
    if (has_previous_frame) {
        frame_ms = std::chrono::duration<float, std::milli>(now - last_frame).count();
        g_frame_tracker.record(frame_ms);
    }
    last_frame = now;
    has_previous_frame = true;

#ifdef __EMSCRIPTEN__
    tick_presentation_cadence();
    // Viewport/DPR sync. Crosses the JS↔WASM boundary, so we throttle.
    if (EducationBoot::instance().is_embedded()) {
        // Embedded host: the canvas is a flex-box child, not the window. Consume a
        // ResizeObserver push immediately (no poll latency → no squished first
        // frames); fall back to measuring the canvas box at ~10Hz if no push has
        // landed yet (the RO can fire before the wasm exports are callable).
        if (g_embed_view_dirty) {
            g_embed_view_dirty = false;
            apply_embedded_viewport(g_embed_view_w, g_embed_view_h, g_embed_view_dpr);
        } else {
            static double last_poll = 0.0;
            const double t_now = ImGui::GetTime();
            if (t_now - last_poll >= 0.1) {
                last_poll = t_now;
                int    cw  = EM_ASM_INT({ var r = Module.canvas.getBoundingClientRect(); return Math.round(r.width); });
                int    ch  = EM_ASM_INT({ var r = Module.canvas.getBoundingClientRect(); return Math.round(r.height); });
                double dpr = EM_ASM_DOUBLE({ return window.devicePixelRatio || 1.0; });
                apply_embedded_viewport(cw, ch, dpr);
            }
        }
        // Size settled (no new push for 250ms) → snap the dock back to the
        // 248/flex/300 design widths in one reflow.
        if (g_embed_reflow_pending &&
            ImGui::GetTime() - g_embed_last_resize_t > 0.25) {
            g_embed_reflow_pending = false;
            LayoutManager::relayout();
        }
    } else {
        // Standalone terminal: the canvas IS the full window and there's no React
        // ResizeObserver - poll innerWidth at ~10Hz and own the backing buffer.
        const double t_now = ImGui::GetTime();
        if (g_standalone_poll_at == 0.0 || t_now - g_standalone_poll_at >= 0.1) {
            g_standalone_poll_at = t_now;

            int window_w = EM_ASM_INT({ return window.innerWidth; });
            int window_h = EM_ASM_INT({ return window.innerHeight; });
            double dpr = EM_ASM_DOUBLE({ return window.devicePixelRatio || 1.0; });

            // Check if window size or DPI changed
            if (window_w != g_standalone_w || window_h != g_standalone_h ||
                std::abs(dpr - g_standalone_dpr) > 0.01) {
                g_standalone_w = window_w;
                g_standalone_h = window_h;
                g_standalone_dpr = dpr;
                // CRITICAL: Resize the actual SDL window
                SDL_SetWindowSize(g_app.window, window_w, window_h);
                // Update canvas resolution
                EM_ASM({
                    var canvas = Module.canvas;
                    var dpr = window.devicePixelRatio || 1;
                    canvas.width = $0 * dpr;
                    canvas.height = $1 * dpr;
                }, window_w, window_h);
                // Update ImGui
                ImGuiIO& io = ImGui::GetIO();
                io.DisplaySize = ImVec2((float)window_w, (float)window_h);
                io.DisplayFramebufferScale = ImVec2((float)dpr, (float)dpr);
            }
        }
    }
#endif
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
#ifdef __EMSCRIPTEN__
        switch (event.type) {
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_WHEEL:
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_TEXT_INPUT:
                note_presentation_input();
                break;
            default: break;
        }
#endif
        // if (event.type == SDL_EVENT_QUIT) {
        //     g_app.done = true;
        // }
        // if (event.type == SDL_EVENT_WINDOW_RESIZED) {
        //     printf("SDL resize event: %d x %d\n",
        //            event.window.data1, event.window.data2);
        // }
    }
    if (!g_app.window) {
        return;
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    // Navigation closes popups during NewFrame. Retain their keyboard ownership
    // through this frame so Escape/Space/arrows cannot also control replay.
    const bool popup_owned_keyboard = ImGui::IsPopupOpen(nullptr,
        ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    ImGui::NewFrame();
    // Drain freshly-decoded coin/exchange logos into GL textures (safe here:
    // between NewFrame and Render, no draw in flight) + run the LRU eviction.
    LogoManager::instance().tick();
    // REC focus layout (CLIP_FACTORY P1): while a focused clip records, hide the
    // perf overlay + app shell (below) + watchlist (update_and_render_widgets) so
    // the capture frames chart+DOM+tape only. Everything returns on stop.
    // Recorder mode (CLIP_FACTORY P2) takes the same chrome-free path for the
    // whole session - the harness captures the X display, so the shell/status
    // bar must never enter a frame.
    const bool rec_focus = ClipRecorder::focus_active() ||
                           edu::RecorderRuntime::instance().active();
    check_initialization();
    maybe_start_lesson_replay();
    maybe_start_event_replay();
    maybe_start_pack_replay();
    maybe_apply_pack_start_at();
    tick_lesson_loading_gate();
    // Usage instrumentation (design §3): session_start once, per-frame session +
    // replay watch-clock accounting. Visibility computed once (used by both).
    usage::maybe_emit_session_start(g_init_complete, g_initial_route.symbol);
    // Visibility gates the watch clock. Throttle the JS<->WASM read to ~10Hz (the
    // codebase convention for boundary crossings); 100ms staleness is immaterial
    // to watch-second accuracy.
    static double s_usage_vis_poll = 0.0;
    static bool s_usage_visible = true;
    if (ImGui::GetTime() - s_usage_vis_poll >= 0.1) {
        s_usage_vis_poll = ImGui::GetTime();
        s_usage_visible = usage::is_document_visible();
    }
    const bool g_doc_visible = s_usage_visible;
    usage::tick_session_usage(static_cast<double>(frame_ms) / 1000.0, g_doc_visible,
                              g_initial_route.symbol);
    // Native fixed chrome (topbar + stats strip). In embedded lesson mode the
    // React host owns the top nav + chart-header strip, so skip the ImGui shell
    // and give the dockspace the full height (no top reserve). Same branch while
    // a focused clip records - the shell must not be in the capture.
    g_profiler.begin("Shell+Dock");
    // Pack mode never ran AppShell::init (no WS → empty strip), so it takes
    // the no-shell branch even though it renders the native transport bar.
    const auto& edu = EducationBoot::instance();
    const bool full_shell = !edu.is_embedded() && !edu.is_pack() && !rec_focus;
    // Event (archive) + demo (pack) chromes suppress the native topbar/statsbar
    // (the React host owns the top nav) but still want the live terminal's bottom
    // telemetry strip (symbol · WS · FPS · present interval · UTC). Draw the status bar only.
    const bool status_bar_only = !rec_focus && !full_shell &&
                                 (edu.is_event() || edu.is_pack());
    if (!rec_focus) drawing::render_style_editor(g_app.app_ctx);
    if (full_shell) {
        LayoutManager::top_reserve    = AppShell::total_height();
        LayoutManager::status_reserve = Theme::Layout::STATUSBAR_H;  // bottom telemetry bar
        // Drawing rail moved INTO ChartWidget (2026-08-06) - no left reserve.
        LayoutManager::left_reserve   = 0.0f;
        AppShell::render(g_app.widgets, g_app.app_ctx, g_app.ws_client.get());
    } else {
        LayoutManager::top_reserve    = 0.0f;
        LayoutManager::left_reserve   = 0.0f;
        LayoutManager::status_reserve = status_bar_only ? Theme::Layout::STATUSBAR_H : 0.0f;
        if (status_bar_only) {
            // Symbol from the education boot globals (g_pair is unset without
            // AppShell::init); connection pill = live WS for an archive event,
            // or the pack engine running for the box-free /demo replay.
            const std::string sym = edu.is_event() ? edu.event_symbol() : edu.pack_symbol();
            AppShell::render_statusbar(g_app.app_ctx, sym, g_app.ws_client.get(), edu.is_pack());
        }
    }
    // Drain a pending chart-toolbar "+ widget" add (embedded chromes file a
    // request here since their native topbar picker never renders). Runs in every
    // host mode; no-op when nothing is queued. Must be OUTSIDE the widget render
    // loop below (it mutates g_app.widgets).
    Menu::resolve_widget_add_request(g_app.widgets, g_app.app_ctx);
    resolve_live_flow_widget_rebuild();  // same safety requirement
    resolve_instrument_metadata_rebind();
    workspace::tick(g_app.widgets, g_app.app_ctx,
        {g_initial_route.exchange, g_initial_route.symbol},
        g_init_complete && !EducationBoot::instance().is_embedded() &&
        !EducationBoot::instance().is_pack() && !g_app.replay_mgr->is_active() &&
        !g_live_flow_rebuild.armed && !ClipRecorder::focus_active());
#ifdef __EMSCRIPTEN__
    // Apply explicit navigation intent AFTER the initial workspace restore.
    // Once only: later mode changes and named workspaces remain the user's choice.
    static bool realtime_link_applied = false;
    if ((!realtime_link_applied || Menu::market_view_navigation_pending) && g_init_complete && !EducationBoot::instance().is_embedded() &&
        !EducationBoot::instance().is_pack() && !g_app.replay_mgr->is_active() &&
        !g_live_flow_rebuild.armed && !ClipRecorder::focus_active()) {
        Menu::market_view_ready=false;
        const bool returning=Menu::market_view_navigation_pending;
        Menu::market_view_navigation_pending=false;
        realtime_link_applied = true;
        if (EM_ASM_INT({return new URLSearchParams(location.search).get('view')==='data';})) {
            const int hours=EM_ASM_INT({return new URLSearchParams(location.search).get('range')==='24h'?24:4;});
            Menu::request_market_data({g_initial_route.exchange,g_initial_route.symbol},hours);
            Menu::resolve_widget_add_request(g_app.widgets,g_app.app_ctx);
        } else if (returning) {
            for (const auto& widget : g_app.widgets) {
                if (widget->type()!=WidgetType::Chart) continue;
                const auto& chart=*static_cast<ChartWidget*>(widget.get());
                if (chart.chart_instance()==1) { ImGui::SetWindowFocus(chart.title()); break; }
            }
        }
        if (!returning && EM_ASM_INT({ return new URLSearchParams(window.location.search).get('realtime') === '1'; })) {
            for (auto& widget : g_app.widgets) {
                if (widget->type() != WidgetType::Chart) continue;
                auto* chart = static_cast<ChartWidget*>(widget.get());
                if (chart->chart_instance() == 1) { chart->set_rt_mode(true); break; }
            }
        }
    }
#endif
#ifdef __EMSCRIPTEN__
    // The replay host can extend the canvas across its HTML rail. Only the
    // dock workspace is inset; floating windows retain the whole canvas.
    LayoutManager::left_reserve += std::clamp(float(EM_ASM_DOUBLE({
        return Number(Module['__edgedepthReplayInset']) || 0;
    })),0.0f,std::max(0.0f,ImGui::GetIO().DisplaySize.x-100.0f));
#endif
    LayoutManager::render_dockspace(nullptr,
        g_initial_route.exchange, g_initial_route.symbol);
    g_profiler.end("Shell+Dock");
    if (g_app.drawing_mgr) g_app.drawing_mgr->tick();  // debounced persist flush
    if (LayoutManager::is_initialized) {
        update_and_render_widgets();
#ifdef __EMSCRIPTEN__
        Menu::market_view_ready = realtime_link_applied && !EducationBoot::instance().is_embedded() && !EducationBoot::instance().is_pack();
#endif
    }
    if (g_app.replay_mgr) {
        // Pack engine first: it processes fetch responses, serves queued
        // candle requests, and drips due frames - the gate + skip logic below
        // then observe the freshest state (no-op outside pack mode).
        g_app.replay_mgr->tick_pack_engine();
        g_app.replay_mgr->tick_buffering_gate();
        // Usage: accrue replay watch-seconds (playing AND visible) + ~15s heartbeat.
        g_app.replay_mgr->tick_usage(static_cast<double>(frame_ms) / 1000.0, g_doc_visible);
        g_app.replay_mgr->flush_pending_skip();
        g_app.replay_mgr->flush_pending_join();
        // Recreate a replay whose socket dropped, once that socket is back.
        g_app.replay_mgr->tick_reconnect();
        g_profiler.begin("Drip");
        g_app.replay_mgr->drip_feed_pending_replay();
        g_profiler.end("Drip");
        // A drawing-tool Esc (cancel placement/deselect, consumed by the chart's
        // DrawingLayer during the widget pass above) must not ALSO stop a
        // running replay - the replay Esc handler reads the raw key.
        if (!popup_owned_keyboard &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
            (!g_app.drawing_mgr ||
             !g_app.drawing_mgr->escape_consumed(ImGui::GetFrameCount())) &&
            !ChartWidget::selection_escape_consumed(ImGui::GetFrameCount()))
            g_app.replay_mgr->process_keyboard_shortcuts();
        // In embedded lesson mode the React chrome owns the transport - don't
        // render the native ImGui control bar (it would stack under the React one).
        if (!EducationBoot::instance().is_embedded()) {
            g_app.replay_mgr->render_control_bar();
            g_app.replay_mgr->render_replay_launcher();
        }
        // LAST in the frame, after every stop() site: the widget sweep above has
        // run, so a context retired on an earlier frame has no subscribers left
        // and its managers can go.
        g_app.replay_mgr->release_retired_context();
        // Clip recorder (CLIP_FACTORY P1): authoritative 3:00 cap, auto-stop when
        // the replay session dies, and the burned-in watermark badge (foreground
        // draw list). No-op unless recording - and v1 recordings can only start
        // from the native transport bar, so embedded lesson/studio never record.
        ClipRecorder::tick_and_render(g_app.replay_mgr->is_active());
    }
    // The upsell modal is the ONE funnel every free-tier gate routes into (locked
    // range/preset/speed/symbol/layer pill, TIER_* server errors, login prompts) -
    // render it in ALL host modes so a gate hit anywhere reaches the same surface.
    ui::UpsellModal::instance().render();
    // The floating "Investigate this minute" reader (right-click on a chart).
    // Rendered in all host modes too: the web bridge exists wherever
    // TerminalEmbed does, and without it the panel shows its own fallback.
    ui::ResearchMomentPanel::instance().render();
    // (Replay is signaled by the restyled bottom control bar + the topbar REPLAY
    //  indicator. The old full-viewport amber border + top-left REPLAY badge were
    //  redundant with those, so they've been removed - see render_replay_overlay's
    //  deletion.)

    // Education runtimes. Lesson mode: gate the playhead + draw the spotlight +
    // emit state. Studio mode: free scrub (no gate) + drain the picker's source
    // into a replay (+ draw/capture overlay, next slice). They are mutually
    // exclusive - one host chrome per mount.
    if (EducationBoot::instance().is_lesson()) {
        edu::LessonRuntime::instance().update(g_app.app_ctx);
        edu::LessonRuntime::instance().render_overlay(g_app.app_ctx);
        // Push lesson state to the React chrome (dirty-checked, emits on change).
        edu::LessonRuntime::instance().emit_state(g_app.app_ctx);
        // Loading scrim sits ABOVE the spotlight while the clock is held.
        render_lesson_loading_overlay();
        render_historical_replay_error_overlay();
    } else if (EducationBoot::instance().is_studio()) {
        edu::StudioRuntime::instance().update(g_app.app_ctx);
        // Export session (CLIP_FACTORY P3-v1): while the studio exports a lesson,
        // the LessonRuntime drives the gate loop and burns the spotlight + cards
        // into the canvas (export-render mode - native card, no button). Outside
        // an export the studio stays a free scrub with no lesson gates.
        if (edu::StudioRuntime::instance().export_session_active()) {
            edu::LessonRuntime::instance().update(g_app.app_ctx);
            edu::LessonRuntime::instance().render_overlay(g_app.app_ctx);
        } else {
            // Draw/capture overlay (phase 1): armed from the React inspector, the
            // author drags a region/band on the chart and the inverse-projected
            // bounds are emitted back as 'edgedepth:capture'. Never during an
            // export take (it would burn into the video + steal the mouse).
            edu::StudioRuntime::instance().render_capture_overlay(g_app.app_ctx);
        }
        // Push transport state (clock/progress/paused/speed + export) to React.
        edu::StudioRuntime::instance().emit_state(g_app.app_ctx);
        // Export badge + cam bubble + 15:00 cap (no-op unless export recording).
        ClipRecorder::export_tick_and_render(
            g_app.replay_mgr && g_app.replay_mgr->is_active());
        // Loading scrim while the chosen window buffers, same as lesson mode.
        render_lesson_loading_overlay();
    } else if (EducationBoot::instance().is_event() ||
               (EducationBoot::instance().is_pack() &&
                EducationBoot::instance().is_embedded())) {
        // Event = embedded archive replay; embedded Pack (/demo) reuses the SAME
        // chrome against the pack engine. The React EventReplayShell owns the chrome
        // + transport; EventRuntime mirrors the replay transport state to it
        // (edgedepth:event) and drains its seek/pause/speed commands - all through
        // ReplayManager, which routes to the box session (event) or the pack engine
        // (pack) identically. No gate loop, no spotlight - the key_moments rail is
        // built on the web (EventRecord / showcase catalog).
        edu::EventRuntime::instance().update(g_app.app_ctx);
        edu::EventRuntime::instance().emit_state(g_app.app_ctx);
        // Loading scrim while the archive window primes (OB seed + candles), same as
        // lesson/studio. A failed historical session remains covered and paused.
        render_lesson_loading_overlay();
        render_historical_replay_error_overlay();
    }
    // Recorder driver (CLIP_FACTORY P2) - ORTHOGONAL to the session mode: layered
    // over whichever event/pack replay booted above, it walks the injected
    // RecorderScript's shots[] against ReplayManager (skip/seek/speed/tf/hold),
    // emits progress to the render harness (CustomEvent 'edgedepth:recorder'),
    // and burns the EDGEDEPTH badge. No-op unless a script global was injected.
    if (edu::RecorderRuntime::instance().active()) {
        edu::RecorderRuntime::instance().update(g_app.app_ctx);
        edu::RecorderRuntime::instance().emit_state(g_app.app_ctx);
        edu::RecorderRuntime::instance().render_overlay(g_app.app_ctx);
    }
    if (!rec_focus && PerformanceDiagnostics::enabled()) {
        QueueBacklogSnapshot queues = g_app.data_thread
            ? g_app.data_thread->queue_metrics()
            : QueueBacklogSnapshot{};
        if (g_app.data_thread) queues.dropped_dispatches = g_app.data_thread->dispatch_queue().dropped_total();
        PerformanceDiagnostics::render(g_profiler, g_frame_tracker, queues);
    }
    // render_debug();
    ui_test::publish();
    g_profiler.begin("ImGui::Render");
    ImGui::Render();
    g_profiler.end("ImGui::Render");
#ifdef __EMSCRIPTEN__
    if (EM_ASM_INT({ return typeof Module['__edgedepthFloatingWindows'] === 'function'; })) {
        // Root floating surfaces only. Docked charts stay beside the HTML rail;
        // child windows are already contained by their root. The same clip path
        // controls browser hit testing, so uncovered markers remain clickable.
        float rects[512]; int count=0;
        for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
            if (!window->Active || window->Hidden || window->RootWindow!=window || window->DockIsActive) continue;
            if ((window->Flags & ImGuiWindowFlags_NoMove) &&
                !(window->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip))) continue;
            if (count+4>512) break;
            rects[count++]=window->Pos.x; rects[count++]=window->Pos.y;
            rects[count++]=window->Pos.x+window->Size.x;
            rects[count++]=window->Pos.y+window->Size.y;
        }
        EM_ASM({
            const rects = Array.from(HEAPF32.subarray($0/4, $0/4+$1));
            Module['__edgedepthFloatingWindows'](rects);
        },rects,count);
    }
#endif
    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f ||
        std::isnan(io.DisplaySize.x) || std::isnan(io.DisplaySize.y)) {
        return;
    }
    const int display_w = static_cast<int>(io.DisplaySize.x * io.DisplayFramebufferScale.x);
    const int display_h = static_cast<int>(io.DisplaySize.y * io.DisplayFramebufferScale.y);
    if (display_w <= 0 || display_h <= 0) {
        return;
    }
    glViewport(0, 0, display_w, display_h);
    glClearColor(Theme::Tokens::BASE.x, Theme::Tokens::BASE.y, Theme::Tokens::BASE.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    g_profiler.begin("GL Draw");
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    g_profiler.end("GL Draw");
    // Submission shape for ?perf=1: each draw command is a scissor plus a
    // glDrawElements through the browser's GPU process, and each list is two
    // buffer uploads. Compare these before blaming the GPU for a slow frame.
    if (const ImDrawData* dd = ImGui::GetDrawData(); dd && PerformanceDiagnostics::enabled()) {
        long commands = 0;
        for (int i = 0; i < dd->CmdListsCount; ++i) commands += dd->CmdLists[i]->CmdBuffer.Size;
        g_profiler.add_count("GL Draw", commands);
        g_profiler.add_count("GLCmdLists", dd->CmdListsCount);
        g_profiler.add_count("GLVertices", dd->TotalVtxCount);
    }
    g_profiler.begin("SwapWindow");
    SDL_GL_SwapWindow(g_app.window);
    g_profiler.end("SwapWindow");
}

extern "C" {
    // A modifier may have been pressed while a surrounding web control had
    // focus. Browser mouse events carry its current state even when SDL never
    // received that keydown. Queue it before SDL's mouse press is processed.
    EMSCRIPTEN_KEEPALIVE
    void sync_pointer_modifiers(int ctrl, int shift, int alt, int super) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, ctrl != 0);
        io.AddKeyEvent(ImGuiMod_Shift, shift != 0);
        io.AddKeyEvent(ImGuiMod_Alt, alt != 0);
        io.AddKeyEvent(ImGuiMod_Super, super != 0);
    }

    // Set the primary chart's timeframe (seconds). Called by the embedded host
    // (lesson Explore / studio) via Module.__set_chart_timeframe. Runs on the main
    // thread between frames - same change_timeframe path the live topbar uses.
    EMSCRIPTEN_KEEPALIVE
    int _set_chart_timeframe(int sec) {
        if (sec <= 0) return 0;
        for (auto& w : g_app.widgets)
            if (w && w->type() == WidgetType::Chart) {
                static_cast<ChartWidget*>(w.get())->change_timeframe(sec);
                return 1;
            }
        return 0; // Host can retry while the first chart is still booting.
    }

    // The web guide's "Back to real-time" / "Show candles" pair: turn the primary
    // chart's Real-time view on or off through the same toggle as the toolbar
    // pill (so the usual gate and upsell apply). Returns 1 when a chart answered.
    EMSCRIPTEN_KEEPALIVE
    int _set_chart_realtime(int on) {
        for (auto& w : g_app.widgets)
            if (w && w->type() == WidgetType::Chart) {
                auto* chart = static_cast<ChartWidget*>(w.get());
                if (chart->rt_mode() != (on != 0)) chart->toggle_rt_mode();
                return 1;
            }
        return 0;
    }

    // The web guide's "Add a candle chart": the same request as the toolbar's
    // "+ Widget > Chart" for the primary chart's market, drained next frame.
    EMSCRIPTEN_KEEPALIVE
    int _add_chart_widget() {
        for (auto& w : g_app.widgets)
            if (w && w->type() == WidgetType::Chart) {
                static_cast<ChartWidget*>(w.get())->request_chart_of_this_market();
                return 1;
            }
        return 0;
    }

    EMSCRIPTEN_KEEPALIVE
    void on_popstate() {
        // Embedded in the Next host: Next owns navigation. Do nothing - a
        // reload here would tear down the React route and the canvas with it.
        if (EducationBoot::instance().is_embedded()) return;

        Route route = parse_route(url_get_current_path(), url_get_current_search());
        if (route.symbol.empty()) return;

        if (route.exchange==g_initial_route.exchange && route.symbol==g_initial_route.symbol) {
            Menu::market_view_navigation_pending=true;
            return;
        }
        // Market changes still restore their workspace through the normal reload.
        // For now: full reload on back/forward.
        // A proper implementation would tear down widgets and recreate,
        // but that requires a "switch_symbol" flow you don't have yet.
        EM_ASM({ location.reload(); });
    }

    // Pushed by TerminalEmbed's ResizeObserver with the canvas CSS box (logical px)
    // + milli-dpr (1000 = 1.0×). dpr crosses as an INT to dodge the f64→f32
    // mis-marshal that bit the lesson speed command (see _edu_cmd_set_speed). This
    // only RECORDS the size - main_loop applies it (never touch ImGui/SDL from a JS
    // callback that lands mid-frame). Mirrors the _edu_cmd_* deferral.
    EMSCRIPTEN_KEEPALIVE
    void _edu_set_viewport(int css_w, int css_h, int milli_dpr) {
        if (css_w <= 0 || css_h <= 0) return;
        g_embed_view_w     = css_w;
        g_embed_view_h     = css_h;
        g_embed_view_dpr   = (milli_dpr > 0) ? (milli_dpr / 1000.0) : 1.0;
        g_embed_view_dirty = true;
    }

    // Hosted research replay (see apply_hosted_replay_transition). The live
    // page sets window.__EDGEDEPTH_HOSTED_REPLAY__ = '{"symbol","exchange",
    // "realtime"}' and calls Module.__hosted_replay_begin() once the server has
    // admitted the window; Back or Exit calls Module.__hosted_replay_end().
    // Both only record the request. Returns 1 when the request was recorded, 0
    // when this mount cannot host (it booted as a lesson, event, pack or Course
    // Studio, which never switch mode), so the page navigates instead.
    EMSCRIPTEN_KEEPALIVE
    int _hosted_replay_begin(void) {
        if (EducationBoot::instance().boot_mode() != EducationBoot::Mode::None) return 0;
        char* raw = reinterpret_cast<char*>(EM_ASM_PTR({
            try {
                var s = window.__EDGEDEPTH_HOSTED_REPLAY__;
                if (!s) return 0;
                s = String(s);
                var len = lengthBytesUTF8(s);
                var buf = _malloc(len + 1);
                stringToUTF8(s, buf, len + 1);
                return buf;
            } catch (e) {
                return 0;
            }
        }));
        if (!raw) return 0;
        g_hosted_payload.assign(raw);
        free(raw);
        g_hosted_request = HostedRequest::Begin;
        return 1;
    }

    EMSCRIPTEN_KEEPALIVE
    int _hosted_replay_end(void) {
        if (EducationBoot::instance().boot_mode() != EducationBoot::Mode::None) return 0;
        g_hosted_request = HostedRequest::End;
        return 1;
    }
}

int main(int, char**) {
    // ── [SELF-HOST P1] RT archive retention override ─────────────────────
    // window.__EDGEDEPTH_RT_ARCHIVE_MS__ (optionally set from the boot
    // script/config) raises the browser-local archive horizon from the
    // shipped 30 minutes up to 12h; the worker budget scales with it.
    if (const double rt_ms = EM_ASM_DOUBLE({
            return Number(window.__EDGEDEPTH_RT_ARCHIVE_MS__ || 0);
        }); rt_ms >= double(RealtimeArchive::kDefaultTargetMs))
        RealtimeArchive::set_target_ms(int64_t(rt_ms));
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return -1;
    }
    // Setup OpenGL ES 3.0 context for WebGL2
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    constexpr auto window_flags = static_cast<SDL_WindowFlags>(
        SDL_WINDOW_OPENGL |
        SDL_WINDOW_HIGH_PIXEL_DENSITY
        // SDL_WINDOW_RESIZABLE |
        // SDL_WINDOW_ALLOW_HIGHDPI
    );
    int window_width = 1920;
    int window_height = 1080;
#ifdef __EMSCRIPTEN__
    // Disable SDL's window event handling (it's broken)
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
    window_width = EM_ASM_INT({ return window.innerWidth || 1920; });
    window_height = EM_ASM_INT({ return window.innerHeight || 1080; });
#endif
    g_app.window = SDL_CreateWindow(
        "EdgeDepth Terminal",
        // SDL_WINDOWPOS_CENTERED,
        // SDL_WINDOWPOS_CENTERED,
        window_width, window_height,
        window_flags
    );
    if (!g_app.window) {
        SDL_Quit();
        return -1;
    }
    g_app.gl_context = SDL_GL_CreateContext(g_app.window);
    if (!g_app.gl_context) {
        SDL_DestroyWindow(g_app.window);
        SDL_Quit();
        return -1;
    }
    g_app.stream_mgr = std::make_unique<StreamManager>(0);  // Dummy handle
    g_app.ob_mgr = std::make_unique<OrderbookManager>();
    g_app.msg_handler = std::make_unique<MessageHandler>();
    g_app.heatmap_mgr = std::make_unique<HeatmapManager>();
    g_app.liq_heatmap_mgr = std::make_unique<LiquidationHeatmapManager>();
    // Detect embedded education mode (window.__EDGEDEPTH_LESSON__) BEFORE any
    // URL writes - when hosted inside the Next app, Next owns the URL and the
    // client must not pushState over it (see EducationBoot).
    EducationBoot::instance().detect();
    // Read the signed-in user's plan (window.__EDGEDEPTH_TIER__) once, for replay
    // gating UX. Backend still enforces the real cap. Defaults to Pro if unset.
    Entitlements::detect();
    // Display-only IANA preference. Browser Intl remains the authority for
    // named zones and historical DST; no epoch or replay state changes here.
    DisplayTimeZone::instance().initialize();
    // Recorder script (CLIP_FACTORY P2): inline in the window global, so it
    // parses synchronously right here - no fetch, no ready-latch. On a parse
    // failure the runtime stays inert and the boot is a plain event/pack replay.
    if (EducationBoot::instance().has_recorder_script()) {
        edu::RecorderRuntime::instance().load(EducationBoot::instance().recorder_json());
    }
    const bool embedded = EducationBoot::instance().is_embedded();

    g_initial_route = parse_route(url_get_current_path(), url_get_current_search());
    if (g_initial_route.symbol.empty()) {
        g_initial_route.symbol = "btcusdt";
    }
    // Studio mode: the picker already chose the symbol (carried in the studio
    // global, read by EducationBoot::detect). Use it as the initial route so the
    // widgets + dock layout build for the RIGHT symbol at boot - the client has no
    // in-place symbol switch, so the symbol must be correct from frame one (a
    // different symbol = a fresh canvas mount, driven by the studio shell).
    if (EducationBoot::instance().is_studio() &&
        !EducationBoot::instance().studio_symbol().empty()) {
        // The venue rides the same global (research replay viewer). An older
        // host that sends none leaves the route's venue in place, which for
        // a /terminal?replay= page is whatever ?exchange= said or binancef.
        const std::string& studio_ex = EducationBoot::instance().studio_exchange();
        if (is_known_exchange(studio_ex)) g_initial_route.exchange = studio_ex;
        g_initial_route.symbol = normalize_symbol_case(
            g_initial_route.exchange, EducationBoot::instance().studio_symbol());
    }
    // Event (archive replay) mode: boot the BARE terminal but for the event's
    // symbol, so widgets + dock layout build for the right symbol from frame one
    // (the client has no in-place symbol switch). The archive replay itself is
    // started from maybe_start_event_replay() once WS + metadata are ready.
    if (EducationBoot::instance().is_event() &&
        !EducationBoot::instance().event_symbol().empty()) {
        g_initial_route.symbol = EducationBoot::instance().event_symbol();
    }
    // Pack (CDN replay) mode: same rule as event mode - widgets + dock layout
    // must build for the pack's symbol from frame one. The replay itself is
    // started from maybe_start_pack_replay().
    if (EducationBoot::instance().is_pack() &&
        !EducationBoot::instance().pack_symbol().empty()) {
        g_initial_route.symbol = EducationBoot::instance().pack_symbol();
    }
    // Only own the URL when running standalone. Embedded lesson/studio leaves the
    // host's /terminal?lesson=N path untouched; event mode is hosted by Next at
    // /terminal?event=<id> (Next owns that URL too), so don't push over it either.
    // Pack mode keeps its ?pack= query params (dev) / host URL (embedded).
    if (!embedded && !EducationBoot::instance().is_event() &&
        !EducationBoot::instance().is_pack()) {
        url_push(build_terminal_path(g_initial_route.exchange, g_initial_route.symbol));
    }

    // Lesson mode: fetch the LessonDoc (credentialed → paywalled) so the replay
    // runtime can consume it. Lesson-only - studio has no doc (picker-driven) and
    // event has no doc (the archive id + window ride __EDGEDEPTH_EVENT__), and
    // is_embedded() now covers both, so gate on is_lesson() specifically.
    if (EducationBoot::instance().is_lesson()) {
        EducationBoot::instance().fetch_lesson([]() {
            auto& eb = EducationBoot::instance();
            if (eb.lesson_ready()) {
                // Parse now (no replay dependency). The archive replay is started
                // from check_initialization() once WS + metadata are ready, to
                // avoid racing replay_mgr construction.
                edu::LessonRuntime::instance().load(eb.lesson_json());
            }
        });
    }

    // Set active symbol for menu bar display
    {
        Menu::g_symbol_picker.active_symbol = g_initial_route.symbol;
        Menu::g_symbol_picker.active_exchange = g_initial_route.exchange;
        // Build display label - uppercase base, try registry first
        std::string sym = g_initial_route.symbol;
        std::transform(sym.begin(), sym.end(), sym.begin(),
            [](unsigned char c) { return std::toupper(c); });
        if (sym.size() > 4 && sym.substr(sym.size() - 4) == "USDT")
            sym = sym.substr(0, sym.size() - 4) + "/USDT";
        std::string exch = (g_initial_route.exchange == "binancef") ? "BINANCEF" : g_initial_route.exchange;
        Menu::g_symbol_picker.active_symbol_label = sym + "@" + exch;
    }

    const Terminal::Pair initial_pair{g_initial_route.exchange, g_initial_route.symbol};
    g_app.candle_mgr = std::make_unique<CandleManager>(
        initial_pair,
        300,
        *g_app.stream_mgr
    );
    g_app.debug_mgr = std::make_unique<DebugManager>(*g_app.stream_mgr, initial_pair);
    g_app.vpvr_mgr = std::make_unique<VolumeProfileManager>();
    g_app.tpo_mgr = std::make_unique<TPOManager>();
    g_app.footprint_mgr = std::make_unique<FootprintManager>();
    g_app.paper_trading_mgr = std::make_unique<PaperTradingManager>();
    g_app.series_mgr = std::make_unique<IndicatorSeriesManager>();
    g_app.analytics_mgr = std::make_unique<AnalyticsManager>();
    g_app.drawing_mgr = std::make_unique<DrawingManager>();
    g_app.drawing_mgr->init(g_initial_route.exchange, g_initial_route.symbol);
    g_app.local_paper_mgr = std::make_unique<LocalPaperEngine>();

    // Create and start data thread BEFORE build_message_context so it can
    // set the dispatch_queue pointer in the context. The queue is installed
    // optimistically: the worker's copy of the context needs it for the
    // cross-thread hand-off. If the thread could not start (self-host
    // pthreads-free builds), drop the queue again - otherwise every widget
    // dispatch would sit in a queue nothing drains (no tape, no CVD, history
    // batches never applied).
    g_app.data_thread = std::make_unique<DataThread>();
    g_app.stream_mgr->set_dispatch_queue(&g_app.data_thread->dispatch_queue());
    g_app.build_message_context();

    // Start data thread with the fully-built context
    g_app.data_thread->start(g_app.msg_ctx);
    if (!g_app.data_thread->is_running()) {
        g_app.inline_dispatch = true;
        g_app.stream_mgr->set_dispatch_queue(nullptr);
        g_app.build_message_context();
    }

    // Pack mode: NO WebSocket at all - the pack + symbol metadata are the only
    // network I/O, so N viewers cost the box zero streaming sessions.
    if (!EducationBoot::instance().is_pack()) {
        connect_websocket();
        // Pack mode is excluded for the same reason live is: a pack session
        // does zero network I/O, so it must not open a replay socket either.
        connect_replay_websocket();
    }
    g_app.replay_mgr = std::make_unique<ReplayManager>(replay_lane_socket(), archive_lane_socket());
    // Every session this canvas creates plays the boot route's venue (the
    // canvas is single-pair, a session is single-venue). Set before any entry
    // point can POST: the lesson/studio starts below run off callbacks.
    g_app.replay_mgr->set_exchange(g_initial_route.exchange);
    if (EducationBoot::instance().is_event() || EducationBoot::instance().is_lesson()) {
        // Arm the persistent pause before the socket-open callback can replay
        // subscriptions and before a lesson document finishes loading.
        pause_live_for_historical_replay();
    }

    // Wire context-swap callback: when replay starts, swap AppContext pointers
    // to replay managers. When replay stops, swap back to live managers.
    g_app.replay_mgr->set_context_swap_callback([](DataContext* replay_ctx) {
#ifdef EDGEDEPTH_EXPOSURE_V2_DEV
            exposure::History::invalidate_all("V2 replay context changed; reload required");
            exposure::SummaryHistory::invalidate_all("V2 replay context changed; reload required");
#endif
        if (replay_ctx) {
            workspace::checkpoint_live();
            // Stop live data from the backend
            g_app.stream_mgr->pause_live_subscriptions();
            // Also unsubscribe live debug (managed separately from StreamManager)
            if (g_app.debug_mgr) {
                g_app.debug_mgr->unsubscribe();
            }
            // Swap AppContext to replay managers
            g_app.app_ctx.streams      = replay_ctx->streams;
            g_app.app_ctx.orderbooks   = replay_ctx->orderbooks;
            g_app.app_ctx.heatmaps     = replay_ctx->heatmaps;
            g_app.app_ctx.liq_heatmaps = replay_ctx->liq_heatmaps;
            g_app.app_ctx.candles      = replay_ctx->candles;
            g_app.app_ctx.debug        = replay_ctx->debug;
            g_app.app_ctx.vpvr         = replay_ctx->vpvr;
            g_app.app_ctx.tpo          = replay_ctx->tpo;
            g_app.app_ctx.footprint    = replay_ctx->footprint;
            g_app.app_ctx.series       = replay_ctx->series;
            g_app.app_ctx.analytics    = replay_ctx->analytics;

            // Secondary charts do not cross a context swap: a live one's
            // manager is subscribed to the LIVE StreamManager and would keep
            // building live candles beside the replay, and one opened in a
            // previous replay belongs to the context being retired. Close
            // them; the end-of-frame sweep erases them and the manager's
            // destructor unsubscribes through the manager it subscribed to.
            // The primary chart is untouched. A second chart opened from
            // "+ widget" during the replay builds its own manager in the new
            // context (ChartWidget constructor).
            for (auto& w : g_app.widgets) {
                if (w && w->type() == WidgetType::Chart &&
                    static_cast<ChartWidget*>(w.get())->is_secondary()) {
                    w->is_open = false;
                }
            }

            // Liq heatmap data is now delivered by the Replayer's DB drip-feed
            // (fetchLiqHeatmapTimeline in replay_seeds.go), NOT by a client-side
            // historical request. This ensures data arrives progressively at the
            // correct timestamps, preventing "future leak".

            // Reset chart widget overlay state so subscriptions are re-established
            // on the replay StreamManager, and stale heatmap data is cleared.
            // A chart parked in real-time mode also leaves it here: replays are
            // historical views, and when the replay pair EQUALS the chart's pair
            // the widget is reused rather than rebuilt, so its live RT latch
            // would otherwise survive the swap. An RT chart over a replay keeps
            // the replay's readiness gate unsatisfied (the replay candle context
            // is never consumed) and the session stalls in Buffering until the
            // 20s watchdog fires - seen launching a recorder pack for the same
            // symbol the live RT chart was showing. Candle mode on replay is
            // what a rebuilt chart would do anyway.
            for (auto& w : g_app.widgets) {
                if (w && w->type() == WidgetType::Chart) {
                    auto* chart = static_cast<ChartWidget*>(w.get());
                    chart->reset_overlay_subscriptions();
                    // Standalone only: self-hosted packs are the only replays
                    // this swap serves there, and none of them carry the live
                    // RT stream. Hosted research replays keep their RT option.
                    if (chart->rt_mode() && !Entitlements::hosted())
                        chart->set_rt_mode(false);
                }
            }

            // Keep live flow objects and their settings/subscriptions intact.
            // Their live StreamManager survives replay; suspended widgets neither
            // update nor submit windows against the replay context.
            if (!g_live_flow_rebuild.armed) g_live_flow_rebuild.want_chart = false;
            for (auto& w : g_app.widgets) {
                if (!w || (w->type() != WidgetType::DOM && w->type() != WidgetType::Trades)) continue;
                if (w->is_replay_widget) w->is_open = false;
                else if (w->is_open) w->suspended_for_replay = true;
            }

            // Create replay widgets subscribed to the replay StreamManager.
            if (!replay_ctx->symbols.empty()) {
                // The session venue, never a literal: frames route by the wire
                // pair, and a Bybit session's chart keyed on binancef sees none.
                const Terminal::Pair replay_pair = replay_ctx->primary_pair();
                auto fmt = SymbolRegistry::instance().get_formatter(replay_pair.exchange, replay_pair.symbol);
                double dom_tick = SymbolRegistry::instance().tick_or_zero(replay_pair.exchange, replay_pair.symbol);
                // Compare markets (symbols[1..]) stack under the primary. A
                // change in the compare set is a layout change: the dock tree
                // has no nodes for the new charts, so it is rebuilt like a
                // pair change. ImGui prunes empty dock nodes, so the nodes can
                // only be built once the charts exist, i.e. here.
                std::vector<std::string> compare(replay_ctx->symbols.begin() + 1,
                                                 replay_ctx->symbols.end());
                const bool compare_changed = compare != LayoutManager::compare_symbols();

                // The chart has to BELONG to the replay pair, not merely exist.
                // The old guard asked "is any chart open", which is symbol-blind,
                // and every library pack is a different symbol from the live boot
                // symbol. So a library launch kept the live btcusdt chart and drew
                // TUT into it: the panel title said btcusdt, the on-chart badge
                // said BTCUSDT, and the dock tree stayed keyed on
                // "DOM binancef btcusdt" while the swap created
                // "DOM binancef tutusdt", which is why the DOM came up floating
                // over the watchlist with no tape at all. The `?pack=` path looked
                // fine only because main.cpp rewrites the route symbol before any
                // widget is built, so its layout already matched.
                //
                // Ask the layout what pair it was BUILT for.
                if (!LayoutManager::layout_matches(replay_pair.exchange,
                                                   replay_pair.symbol) || compare_changed) {
                    // Retire charts built for another pair. This DESTROYS them
                    // (erase_if(!is_open) at the end of the frame) rather than
                    // hiding them, which is what is wanted here: they are being
                    // replaced. Record the pair so the exit rebuild can restore
                    // the chart that was live.
                    for (auto& w : g_app.widgets) {
                        if (!w || w->type() != WidgetType::Chart) continue;
                        if (!w->is_replay_widget) {
                            g_live_flow_rebuild.want_chart = true;
                            g_live_flow_rebuild.pair =
                                static_cast<ChartWidget*>(w.get())->pair();
                        }
                        w->is_open = false;
                    }
                    LayoutManager::set_compare_symbols(compare);
                    LayoutManager::reset_layout_for(replay_pair.exchange, replay_pair.symbol);
                    auto chart_w = std::make_unique<ChartWidget>(
                        replay_pair, g_app.app_ctx, dom_tick);
                    chart_w->is_replay_widget = true;
                    g_app.widgets.push_back(std::move(chart_w));
                }
                // One compare chart per extra market: a replay widget that
                // reads the context's own manager for its pair (no live
                // subscription, no history request of its own) and shares the
                // primary's time axis. The primary is linked too while the
                // session has company, and unlinked again on exit below.
                for (const auto& sym : compare) {
                    const Terminal::Pair cpair{replay_pair.exchange, sym};
                    const double ctick = SymbolRegistry::instance().tick_or_zero(cpair.exchange, cpair.symbol);
                    auto cw = std::make_unique<ChartWidget>(cpair, g_app.app_ctx, ctick, 1, /*compare=*/true);
                    cw->is_replay_widget = true;
                    g_app.widgets.push_back(std::move(cw));
                }
                for (auto& w : g_app.widgets) {
                    if (!w || w->type() != WidgetType::Chart) continue;
                    auto* chart = static_cast<ChartWidget*>(w.get());
                    if (chart->pair().exchange == replay_pair.exchange &&
                        chart->pair().symbol == replay_pair.symbol)
                        chart->set_time_linked(!compare.empty());
                }

                // Mirror only panels that actually existed, with their preferences.
                // Parked live objects retain their data and settings for return-live.
                for (const auto type : {WidgetType::Trades, WidgetType::DOM}) {
                    Widget* source = nullptr;
                    for (const auto& w : g_app.widgets) {
                        if (!w->is_open || !w->suspended_for_replay || w->type() != type) continue;
                        const auto& pair = type == WidgetType::DOM ? static_cast<DOMWidget*>(w.get())->pair()
                                                                  : static_cast<TradesWidget*>(w.get())->pair();
                        if (w->mode_companion || (pair.exchange == replay_pair.exchange && pair.symbol == replay_pair.symbol)) {
                            source = w.get(); break;
                        }
                    }
                    if (!source) continue; // Never resurrect a panel closed before replay.
                    std::unique_ptr<Widget> flow;
                    if (type == WidgetType::DOM) flow = std::make_unique<DOMWidget>(replay_pair, g_app.app_ctx, dom_tick, 20);
                    else flow = std::make_unique<TradesWidget>(replay_pair, g_app.app_ctx, fmt);
                    flow->load_settings(source->save_settings());
                    flow->mode_companion = source->mode_companion;
                    flow->is_replay_widget = true;
                    g_app.widgets.push_back(std::move(flow));
                }
            }
        } else {
            // Restore parked live panels; retire replay-owned widgets before
            // the next render so stable window IDs are never submitted twice.
            bool closed_replay_chart = false;
            for (auto& w : g_app.widgets) {
                if (!w) continue;
                w->suspended_for_replay = false;
                if (w->is_replay_widget) {
                    closed_replay_chart = closed_replay_chart ||
                                          w->type() == WidgetType::Chart;
                    w->is_open = false;  // Will be erased by cleanup loop
                }
            }
            // A compare layout has no place in the live terminal: drop the
            // stacked split and re-key the dock for the live pair, and unlink
            // the surviving primary chart's time axis.
            if (!LayoutManager::compare_symbols().empty()) {
                LayoutManager::set_compare_symbols({});
                g_live_flow_rebuild.want_chart = true;
                for (auto& w : g_app.widgets) {
                    if (!w || w->type() != WidgetType::Chart) continue;
                    auto* chart = static_cast<ChartWidget*>(w.get());
                    chart->set_time_linked(false);
                    if (!w->is_replay_widget) {
                        g_live_flow_rebuild.pair = chart->pair();
                        w->is_open = false;  // rebuilt below, keyed on the live pair
                    }
                }
                closed_replay_chart = true;
            }
            g_live_flow_rebuild.armed = g_live_flow_rebuild.want_chart;
            // No blind reset_layout() here. It cleared the dock tree without
            // queueing a pair, so the rebuild had nothing to key on; the chart
            // arm below re-docks for the pair that was live instead.
            g_live_flow_rebuild.want_chart =
                g_live_flow_rebuild.want_chart && closed_replay_chart;
            g_app.build_app_context();
            // Reset chart overlay subscriptions so they re-subscribe on live
            for (auto& w : g_app.widgets) {
                if (w && w->type() == WidgetType::Chart) {
                    auto* chart = static_cast<ChartWidget*>(w.get());
                    chart->reset_overlay_subscriptions();
                }
            }
            // Historical event/lesson mounts never fall back to live delivery.
            // Navigation creates a fresh terminal mount when the user leaves.
            const auto& boot = EducationBoot::instance();
            if (!boot.is_event() && !boot.is_lesson()) {
                g_app.stream_mgr->resume_live_subscriptions();
                // Re-subscribe live debug
                if (g_app.debug_mgr) {
                    g_app.debug_mgr->subscribe(g_initial_route.symbol);
                }
            }
        }
    });

    // Wire rewind callback: when << is pressed, clear widget data
    g_app.replay_mgr->set_rewind_callback([](int64_t cutoff_ms) {
        liquidity_response::History::invalidate_all("Replay rewound; waiting for observations");
        refill::on_rewind();
        refill::history::on_rewind();
#ifdef EDGEDEPTH_EXPOSURE_V2_DEV
            exposure::History::invalidate_all("V2 rewind; reload required");
            exposure::SummaryHistory::invalidate_all("V2 rewind; reload required");
#endif
        for (auto& w : g_app.widgets) {
            if (w && w->is_replay_widget) {
                w->on_rewind(cutoff_ms);
            }
        }
        // Clear debug manager (it's a manager, not a widget)
        auto* replay_ctx = g_app.replay_mgr->replay_context();
        if (replay_ctx && replay_ctx->debug) {
            replay_ctx->debug->clear_entries();
        }
        // WS4 Observed markers: drop @forceOrder events past the rewind target -
        // they're the replay's future now (would paint ahead of the playback head).
        if (replay_ctx && replay_ctx->liq_heatmaps) {
            replay_ctx->liq_heatmaps->trim_observed_after(cutoff_ms);
        }
        // Indicator SeriesCache (VPIN et al): same rule - points past the
        // rewind target are the replay's future; idempotent re-delivery
        // from the buffer refills up to the head.
        if (replay_ctx && replay_ctx->series) {
            replay_ctx->series->trim_after(cutoff_ms);
        }
        if (replay_ctx && replay_ctx->analytics) {
            replay_ctx->analytics->trim_after(cutoff_ms);
        }
    });

    // Build app context AFTER all managers are constructed
    g_app.build_app_context();

    // Fetch symbol metadata (tick_size, step_size) from API
    SymbolRegistry::instance().fetch_metadata([]() {
    });

    SDL_GL_MakeCurrent(g_app.window, g_app.gl_context);
    // Present once per display refresh. On Emscripten SDL maps interval 1 to a
    // requestAnimationFrame main loop and interval 0 to setTimeout(0), which
    // redraws as fast as the CPU and GPU allow (the old 180 FPS counter). Frames
    // above the monitor rate are never shown; they only burn CPU and GPU time,
    // and every terminal window shares the browser's single GPU process, so two
    // uncapped windows halved each other. rAF gives each window its own
    // monitor's cadence and stops entirely while a tab is hidden.
    int swap_interval = 1;
#ifdef __EMSCRIPTEN__
    // ?vsync=N (1..4, dev only) pins every Nth refresh; see PresentationCadence.
    g_cadence.fixed = EM_ASM_INT({
        var v = parseInt(new URLSearchParams(window.location.search).get('vsync') || '0', 10);
        return (v >= 1 && v <= 4) ? v : 0;
    });
    if (g_cadence.fixed > 0) swap_interval = g_cadence.fixed;
    g_cadence.current = swap_interval;
    start_window_census();
#endif
    SDL_GL_SetSwapInterval(swap_interval);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // The renderer owns its capability flags. WebGL cannot apply base-vertex
    // offsets; the application uses 32-bit indices for dense chart geometry.
    static_assert(sizeof(ImDrawIdx) == 4);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

#ifdef __EMSCRIPTEN__
    // Keep native menu navigation, Escape and keyboard focus in the browser.
    io.IniFilename = nullptr; // Disable ini file in browser
    // EM_ASM({
    //     // Remove any keyboard event blocking
    //     Module.canvas.onkeydown = null;
    //     Module.canvas.onkeyup = null;
    //     Module.canvas.onkeypress = null;
    //     // Allow F12 and dev tools shortcuts
    //     document.addEventListener('keydown', function(e) {
    //         // F12
    //         if (e.key === 'F12') {
    //             e.stopPropagation();
    //             return;
    //         }
    //         // Ctrl+Shift+I or Cmd+Opt+I
    //         if ((e.ctrlKey && e.shiftKey && e.key === 'I') ||
    //             (e.metaKey && e.altKey && e.key === 'I')) {
    //             e.stopPropagation();
    //             return;
    //         }
    //     }, true);
    // });
#endif
    Theme::apply_dark_theme();
    Theme::apply_trading_colors();
    Theme::load_preferences();
    i18n::load();  // Phase 7: session language + Persian-numerals toggle
    if (!Theme::load_fonts()) {
    }
    ImGui_ImplSDL3_InitForOpenGL(g_app.window, g_app.gl_context);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Initialize shared GPU resources for shader-based heatmap rendering
    if (!ShaderHeatmapResources::instance().init()) {
    }
#ifdef __EMSCRIPTEN__
    EM_ASM({
        var canvas = Module.canvas;
        // Focus immediately
        canvas.focus();
        // Only canvas clicks focus the terminal. Surrounding web inputs keep
        // their focus, and mouse modifiers repair keydowns missed while away.
        var syncModifiers = function(event) {
            Module['_sync_pointer_modifiers'](
                event.ctrlKey, event.shiftKey, event.altKey, event.metaKey);
        };
        canvas.addEventListener('mousedown', function(event) {
            syncModifiers(event);
            canvas.focus();
        }, true);
        canvas.addEventListener('mousemove', syncModifiers, true);
        canvas.addEventListener('mouseup', syncModifiers, true);
    });
    // Clip recorder support probe - once at boot. Negotiates the MediaRecorder
    // container (vp9 → vp8 → webm → mp4); a failed probe renders the transport's
    // record button disabled instead of letting a click fail silently.
    ClipRecorder::probe_support();
    url_register_popstate();
    emscripten_set_interval([](void*) { capture_background_live(); }, 1000, nullptr);
    emscripten_set_main_loop(main_loop, 0, 1);
#else
    while (!g_app.done) {
        main_loop();
    }
#endif
    // Cleanup - unique_ptrs auto-destroy, just clear widgets first
    g_app.widgets.clear();
    if (g_app.ws_client) {
        g_app.ws_client->disconnect();
    }
    if (g_app.replay_ws_client) {
        g_app.replay_ws_client->disconnect();
    }
    // Stop data thread before destroying managers it references
    if (g_app.data_thread) {
        g_app.data_thread->stop();
        g_app.data_thread.reset();
    }
    // Reset in reverse order of construction
    g_app.replay_mgr.reset();
    g_app.debug_mgr.reset();
    g_app.candle_mgr.reset();
    g_app.liq_heatmap_mgr.reset();
    g_app.heatmap_mgr.reset();
    ShaderHeatmapResources::instance().destroy();
    g_app.msg_handler.reset();
    g_app.ob_mgr.reset();
    g_app.stream_mgr.reset();
    // replay_mgr holds a raw pointer to whichever socket is the replay lane, so
    // it is already reset above; drop both sockets after it, never before.
    g_app.replay_ws_client.reset();
    g_app.ws_client.reset();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(g_app.gl_context);
    SDL_DestroyWindow(g_app.window);
    SDL_Quit();

    return 0;
}
