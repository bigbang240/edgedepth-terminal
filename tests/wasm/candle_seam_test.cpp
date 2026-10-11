// Live candle continuity around history requests, against the production
// CandleManager and StreamManager (only the WebSocket boundary is stubbed).
//
//   1. Trades that arrive while the initial batch is in flight build candles
//      once it lands, instead of leaving the request's round trip empty.
//   2. The server's live candle (final = false) becomes the building candle
//      even after its second has closed, with the in-flight periods before it.
//   3. A recent-candle refetch never silences live trades.
//   4. A sub-minute refetch fills holes but never trades a local candle for
//      one holding fewer trades (the server's newest captured seconds).
#include <span>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "stream_handler.h"
#include "core/candle_manager.h"

static std::vector<nlohmann::json> sent;
extern "C" EMSCRIPTEN_RESULT emscripten_websocket_send_utf8_text(
    EMSCRIPTEN_WEBSOCKET_T, const char* text) {
    sent.push_back(nlohmann::json::parse(text));
    return EMSCRIPTEN_RESULT_SUCCESS;
}
extern "C" EMSCRIPTEN_RESULT emscripten_websocket_get_ready_state(
    EMSCRIPTEN_WEBSOCKET_T, unsigned short* state) {
    *state = 1;
    return EMSCRIPTEN_RESULT_SUCCESS;
}
static void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}
static size_t history_requests() {
    size_t n = 0;
    for (const auto& m : sent) n += m["method"] == "get_historical_candles";
    return n;
}
static Terminal::Candle candle(int64_t ts, double price, int64_t trades, bool final = true) {
    return {1, price, price, price, price, double(trades), double(trades), 0, trades, 0, ts, final};
}

int main() {
    const Terminal::Pair pair{"binancef", "nilusdt"};
    StreamManager sm(1);
    CandleManager cm(pair, 1, sm);
    const StreamKey trades{pair, Terminal::Stream::Trades, 0};
    const StreamKey candles{pair, Terminal::Stream::Candles, 1};
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t s = now / 1000 * 1000 - 30000;  // 30 s ago, on a second

    cm.initial_load();
    check(history_requests() == 1, "initial load requests history");

    // In flight: two seam seconds before the server's live candle, two trades
    // inside the live candle's second.
    sm.dispatch_trade_impl(trades, {10.0, 1, s + 20100, true});
    sm.dispatch_trade_impl(trades, {10.2, 1, s + 20600, false});
    sm.dispatch_trade_impl(trades, {10.4, 1, s + 21200, true});
    sm.dispatch_trade_impl(trades, {10.5, 1, s + 22100, true});
    sm.dispatch_trade_impl(trades, {10.6, 1, s + 22300, true});

    // Captured history ends at s+10 s; the server's live candle is s+22 s.
    std::vector<Terminal::Candle> batch;
    for (int i = 0; i <= 10; ++i) batch.push_back(candle(s + i * 1000, 10.0, 3));
    Terminal::Candle live = candle(s + 22000, 10.5, 7, false);
    batch.push_back(live);
    sm.dispatch_candles_impl(candles, batch);

    check(cm.is_initial_load_complete(), "batch completes the initial load");
    check(cm.count() == 13, "history plus the two in-flight seam seconds");
    check(cm.candles()[11].timestamp_ms == s + 20000 && cm.candles()[11].tbuy + cm.candles()[11].tsell == 2,
          "the first seam second is built from both in-flight trades");
    check(cm.candles()[12].timestamp_ms == s + 21000 && cm.candles()[12].close == 10.4,
          "the second seam second follows it");
    check(cm.has_building_candle() && cm.building_candle().timestamp_ms == s + 22000,
          "a closed live candle (final = false) is still the building one");
    check(cm.building_candle().tbuy == 7, "trades the live candle already holds are not added twice");

    // Refetch in flight: live trades keep building.
    const size_t before = history_requests();
    cm.request_recent(40);
    check(history_requests() == before + 1, "refetch is requested");
    sm.dispatch_trade_impl(trades, {10.7, 1, s + 25100, true});
    check(cm.has_building_candle() && cm.building_candle().timestamp_ms == s + 25000,
          "a live trade during the refetch builds its candle");
    check(cm.candles().back().timestamp_ms == s + 22000, "the live candle is finalized behind it");

    // The refetch answers with the captured seconds, including a partial one.
    std::vector<Terminal::Candle> recent;
    recent.push_back(candle(s + 15000, 10.1, 4));  // a hole: inserted
    recent.push_back(candle(s + 20000, 10.0, 5));  // fuller than ours: replaces
    recent.push_back(candle(s + 22000, 10.5, 2));  // partial capture edge: kept ours
    sm.dispatch_candles_impl(candles, recent);
    check(cm.count() == 15, "only the hole is inserted");
    bool hole = false, fuller = false, kept = false;
    for (const auto& c : cm.candles()) {
        hole |= c.timestamp_ms == s + 15000;
        fuller |= c.timestamp_ms == s + 20000 && c.tbuy == 5;
        kept |= c.timestamp_ms == s + 22000 && c.tbuy == 7;
    }
    check(hole, "the missing second is filled");
    check(fuller, "a server candle with more trades replaces ours");
    check(kept, "a server candle with fewer trades never replaces ours");
    check(cm.building_candle().timestamp_ms == s + 25000, "the building period stays live");

    // Replay seek: no next-minute tick can establish a future building bar
    // before trades arrive. Exercise two subscribers as in a floating chart.
    const Terminal::Pair replay_pair{"binancef", "metusdt"};
    CandleManager primary(replay_pair, 60, sm), secondary(replay_pair, 60, sm);
    for (auto* replay : {&primary, &secondary}) {
        replay->subscribe();
        replay->mark_ready_for_replay();
        replay->reset_for_seek(180000);
    }
    const StreamKey replay_candles{replay_pair, Terminal::Stream::Candles, 60};
    const StreamKey replay_trades{replay_pair, Terminal::Stream::Trades, 0};
    std::vector<Terminal::Candle> history{candle(60000, 10, 3), candle(120000, 11, 5)};
    sm.dispatch_candles_impl(replay_candles, history);
    sm.dispatch_candle_impl(replay_candles, candle(240000, 99, 100));
    for (auto* replay : {&primary, &secondary})
        check(!replay->has_building_candle(), "ahead tick cannot create a future bar after seek");
    sm.dispatch_trade_impl(replay_trades, {12, 1, 181000, true});
    for (auto* replay : {&primary, &secondary}) {
        check(replay->building_candle().timestamp_ms == 180000, "trade owns the active replay minute");
        replay->reset_for_seek(150000);
    }
    // Late response to the previous position includes the now-future 180/240 bars.
    history.push_back(candle(180000, 98, 100));
    history.push_back(candle(240000, 99, 100));
    sm.dispatch_candles_impl(replay_candles, history);
    for (auto* replay : {&primary, &secondary}) {
        check(replay->count() == 1 && replay->candles().back().timestamp_ms == 60000,
              "late batch cannot restore future finalized candles");
        check(replay->building_candle().timestamp_ms == 120000 && replay->building_candle().volume == 0,
              "late batch adopts and truncates only the playhead period");
        replay->handle_trade({12, 1, 151000, true});
        replay->handle_trade({13, 1, 241000, true});
        replay->trim_candles_after(180000);
        check(!replay->has_building_candle(), "rewind into gap keeps the preceding closed bar finalized");
        check(replay->candles().back().timestamp_ms == 120000 && replay->candles().back().close == 12,
              "rewind preserves fully observed closed OHLC");
        check(replay->tick_times().empty(), "rewind drops future line points");
    }

    std::puts("candle seam passed");
    return 0;
}
