#include "ui/chart_widget.h"
#include "core/indicator_series.h"
#include "replayer/replay_manager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <utility>

// Indicators

void ChartWidget::render_indicators() {
    if (subplots_suppressed()) return;
    if (indicator_mgr_.count() == 0) return;
    indicator_mgr_.render_tabbed(stored_x_min_, stored_x_max_, this,
                                 &crosshair_state_);
}

void ChartWidget::populate_volume_data(Indicators::VolumeIndicator* vol_ind) const {
    vol_ind->sync_candles(candles());
}

void ChartWidget::add_volume_indicator() {
    if (indicator_mgr_.has_indicator_of_type<Indicators::VolumeIndicator>()) return;

    auto volume_ind = std::make_unique<Indicators::VolumeIndicator>();
    volume_ind->set_timeframe(candles().timeframe_seconds());
    populate_volume_data(volume_ind.get());
    volume_ind->update();
    indicator_mgr_.add_indicator(std::move(volume_ind));
}

void ChartWidget::update_volume_indicators() {
    auto* vol_ind = indicator_mgr_.get_indicator_of_type<Indicators::VolumeIndicator>();
    if (!vol_ind) return;

    vol_ind->sync_candles(candles());
}

void ChartWidget::add_cvd_indicator() {
    if (indicator_mgr_.has_indicator_of_type<Indicators::CVDIndicator>()) return;

    auto cvd_ind = std::make_unique<Indicators::CVDIndicator>();
    cvd_ind->set_timeframe(candles().timeframe_seconds());
    populate_cvd_data(cvd_ind.get());
    cvd_ind->update();
    indicator_mgr_.add_indicator(std::move(cvd_ind));
}

void ChartWidget::populate_cvd_data(Indicators::CVDIndicator* cvd_ind) const {
    // Safety: ensure we start from empty state.
    // Callers should clear() before calling, but guard here too.
    if (cvd_ind->bar_count() > 0) {
        cvd_ind->clear();
    }
    const auto& history = candles().candles();
    cvd_ind->set_history_unavailable(std::any_of(history.begin(), history.end(),
        [](const auto& c) { return c.trade_stats_unavailable; }));
    for (const auto& candle : history) {
        if (candle.trade_stats_unavailable) continue;
        // Check if we have intra-candle CVD wick data from Volume stream
        auto it = cvd_wick_cache_.find(candle.timestamp_ms);
        if (it != cvd_wick_cache_.end()) {
            // Use Volume stream data with real intra-candle CVD high/low
            const double delta = (candle.vbuy - candle.vsell) * candle.close;
            cvd_ind->add_candle_with_cvd(candle.timestamp_ms, delta,
                                         it->second.cvd_high, it->second.cvd_low);
        } else {
            // Fallback: candle data only (no wicks)
            cvd_ind->add_candle(candle.timestamp_ms,
                                candle.vbuy * candle.close,
                                candle.vsell * candle.close);
        }
    }
}

void ChartWidget::handle_volume(const Terminal::Volume& vol) {
    // Store CVD wick data for the CVD indicator to pick up on next rebuild.
    // cvd/cvd_high/cvd_low are per-candle values from the VolumeActor.
    if (vol.cvd_high != 0 || vol.cvd_low != 0) {
        cvd_wick_cache_[vol.timestamp_ms] = CVDWickData{vol.cvd_high, vol.cvd_low};
    }
}

void ChartWidget::update_cvd_indicator() {
    auto* cvd_ind = indicator_mgr_.get_indicator_of_type<Indicators::CVDIndicator>();
    if (!cvd_ind) return;

    if (candles().has_building_candle()) {
        const auto& bc = candles().building_candle();
        cvd_ind->set_building_unavailable(bc.trade_stats_unavailable);
        if (!bc.trade_stats_unavailable) cvd_ind->set_building_candle(bc.timestamp_ms,
                                     bc.vbuy * bc.close,
                                     bc.vsell * bc.close);
    }
    cvd_ind->update();
}

// ═══ Funding Rate Indicator ═══

void ChartWidget::handle_stat_for_chart(const Terminal::Stat& stat) {
    if (stat.funding != 0) {
        funding_cache_[stat.timestamp_ms] = stat.funding;
        latest_funding_rate_ = stat.funding;
        funding_data_dirty_ = true;
    }
    // Historical OI OHLC (from get_historical_oi batch)
    if (stat.oi_close > 0) {
        oi_ohlc_cache_[stat.timestamp_ms] = {stat.oi_open, stat.oi_high, stat.oi_low, stat.oi_close};
        latest_oi_usd_ = stat.oi_close;
        oi_data_dirty_ = true;
        // Debug: log first few historical OI arrivals
        static int oi_hist_count = 0;
        if (++oi_hist_count <= 5) {
        }
    }
    // Live OI snapshot (from live stat stream)
    if (stat.open_interest_usd > 0) {
        oi_cache_[stat.timestamp_ms] = stat.open_interest_usd;
        latest_oi_usd_ = stat.open_interest_usd;
    }
    // Phase 2 S3: the estimator reads the live mark and open interest only
    // (the historical OI batch above carries a different unit).
    if (stat.mark_price > 0 && std::isfinite(stat.mark_price)) liq_mark_price_ = stat.mark_price;
    if (stat.open_interest_usd > 0) liq_oi_contracts_ = stat.open_interest_usd;
}

void ChartWidget::add_funding_rate_indicator() {
    if (indicator_mgr_.has_indicator_of_type<Indicators::FundingRateIndicator>()) return;

    // Request historical funding data if we haven't already
    if (!funding_history_requested_) {
        funding_history_requested_ = true;
        const int64_t tf_sec = candles().timeframe_seconds();
        const int count = static_cast<int>(candles().count());
        ctx_.stream_mgr().request_historical_funding(pair_, tf_sec, count > 0 ? count : 2880);
    }

    auto ind = std::make_unique<Indicators::FundingRateIndicator>();
    ind->set_timeframe(candles().timeframe_seconds());
    populate_funding_data(ind.get());
    ind->update();
    indicator_mgr_.add_indicator(std::move(ind));
}

void ChartWidget::populate_funding_data(Indicators::FundingRateIndicator* ind) const {
    // Funding data comes from stats stream, keyed by candle timestamp.
    // For candles that don't have a matching stat, use the most recent known funding.
    double last_funding = 0;
    for (const auto& candle : candles().candles()) {
        auto it = funding_cache_.find(candle.timestamp_ms);
        if (it != funding_cache_.end()) {
            last_funding = it->second;
        }
        ind->add_bar(candle.timestamp_ms, last_funding);
    }
}

void ChartWidget::update_funding_indicator() {
    auto* ind = indicator_mgr_.get_indicator_of_type<Indicators::FundingRateIndicator>();
    if (!ind) return;

    if (candles().has_building_candle()) {
        const auto& bc = candles().building_candle();
        ind->set_building_bar(bc.timestamp_ms, latest_funding_rate_);
    }
    ind->update();
}

// ═══ OI Indicator ═══

void ChartWidget::add_oi_indicator() {
    if (indicator_mgr_.has_indicator_of_type<Indicators::OIIndicator>()) return;

    // Request historical OI if we haven't already
    request_historical_oi();

    auto ind = std::make_unique<Indicators::OIIndicator>();
    ind->set_timeframe(candles().timeframe_seconds());
    populate_oi_data(ind.get());
    ind->update();
    indicator_mgr_.add_indicator(std::move(ind));
}

void ChartWidget::request_historical_oi() {
    if (oi_history_requested_) return;
    oi_history_requested_ = true;
    const int64_t tf_sec = candles().timeframe_seconds();
    const int count = static_cast<int>(candles().count());
    ctx_.stream_mgr().request_historical_oi(pair_, tf_sec, count > 0 ? count : 2880);
}

void ChartWidget::populate_oi_data(Indicators::OIIndicator* ind) const {
    double last_close = 0;
    int ohlc_hits = 0, live_hits = 0, fwd_fills = 0, gaps = 0;
    for (const auto& candle : candles().candles()) {
        // Prefer OHLC data from historical OI query
        auto ohlc_it = oi_ohlc_cache_.find(candle.timestamp_ms);
        if (ohlc_it != oi_ohlc_cache_.end()) {
            const auto& ohlc = ohlc_it->second;
            ind->add_candle_ohlc(candle.timestamp_ms, ohlc.open, ohlc.high, ohlc.low, ohlc.close);
            last_close = ohlc.close;
            ohlc_hits++;
            continue;
        }
        // Fallback: live OI snapshot (no wicks)
        auto it = oi_cache_.find(candle.timestamp_ms);
        if (it != oi_cache_.end()) {
            ind->add_oi(candle.timestamp_ms, it->second);
            last_close = it->second;
            live_hits++;
            continue;
        }
        // Forward-fill: use previous close as flat bar to avoid gaps
        if (last_close > 0) {
            ind->add_candle_ohlc(candle.timestamp_ms, last_close, last_close, last_close, last_close);
            fwd_fills++;
        } else {
            gaps++;
        }
    }
}

void ChartWidget::update_oi_indicator() {
    auto* ind = indicator_mgr_.get_indicator_of_type<Indicators::OIIndicator>();
    if (!ind) return;

    if (candles().has_building_candle()) {
        const auto& bc = candles().building_candle();
        if (latest_oi_usd_ > 0) {
            ind->set_building_oi(bc.timestamp_ms, latest_oi_usd_);
        }
    }
    ind->update();
}

// ═══ VPIN / Toxicity Indicator (Indicators V1 S1b - pathfinder) ═══

void ChartWidget::add_vpin_indicator() {
    if (indicator_mgr_.has_indicator_of_type<Indicators::VPINIndicator>()) return;

    auto ind = std::make_unique<Indicators::VPINIndicator>();
    // Populate from whatever the SeriesCache already holds (live accrual /
    // replay seed); the update loop backfills via get_historical_vpin.
    if (ctx_.series) {
        ind->set_points(ctx_.series_mgr().vpin(Terminal::pair_key(pair_.exchange, pair_.symbol)));
        vpin_populated_revision_ = ctx_.series_mgr().vpin_revision(Terminal::pair_key(pair_.exchange, pair_.symbol));
    }
    indicator_mgr_.add_indicator(std::move(ind));
}

void ChartWidget::update_vpin_indicator() {
    auto* ind = indicator_mgr_.get_indicator_of_type<Indicators::VPINIndicator>();
    if (!ind || !ctx_.series) return;
    ind->set_observed_until(ctx_.replay_mgr().is_active()
        ? ctx_.replay_mgr().interpolated_time_ms()
        : std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    const bool is_replay = ctx_.candle_mgr().replay_start_time_ms() > 0;

    // Live STATE_VPIN subscribe - lazy, once, never during replay (the
    // replay StreamManager is subscribe-inert anyway; seeds deliver data).
    if (!vpin_subscribed_ && !is_replay) {
        const StreamKey key{pair_, Terminal::Stream::VPINState, 0};
        ctx_.stream_mgr().send_subscribe(key);
        vpin_subscribed_ = true;
    }

    // Chart-load history (F3 absolute range + chunk-until-covered).
    // The server keeps the MOST RECENT `count` rows of a window, so a wide
    // window truncates its OLD side - coverage must converge by chunking
    // [candle_front, series_oldest) until the gap closes, a chunk lands
    // empty (series floor), or the initial response is still in flight.
    // This also covers scroll-back: candle backfill moves the target left
    // and the same loop keeps chunking. end_ms of the initial request is 0
    // (server end = now; replay sessions clamp to the playback head).
    if (candles().is_initial_load_complete() && !candles().empty()) {
        const int64_t target_ms = candles().candles().front().timestamp_ms;
        if (!vpin_history_requested_) {
            vpin_history_requested_ = true;
            vpin_hist_last_req_ = std::chrono::steady_clock::now();
            ctx_.stream_mgr().request_historical_vpin(pair_, target_ms, 0, 5000);
        } else if (!vpin_hist_exhausted_) {
            const int64_t oldest_ms = ctx_.series_mgr().vpin_oldest_ts(Terminal::pair_key(pair_.exchange, pair_.symbol));
            if (oldest_ms > 0 && oldest_ms > target_ms + 3600'000) {  // >1h uncovered
                const auto now = std::chrono::steady_clock::now();
                if (now - vpin_hist_last_req_ > std::chrono::seconds(2)) {
                    if (vpin_hist_last_oldest_ == 0 ||
                        oldest_ms < vpin_hist_last_oldest_ - 1000) {
                        // First chunk, or the last one made progress → next.
                        vpin_hist_last_oldest_ = oldest_ms;
                        vpin_hist_last_req_ = now;
                        ctx_.stream_mgr().request_historical_vpin(
                            pair_, target_ms, oldest_ms, 5000);
                    } else {
                        // A chunk landed with nothing older - stop asking.
                        vpin_hist_exhausted_ = true;
                    }
                }
            }
        }
    }

    // Repopulate the render cache only when the series actually changed.
    const uint64_t rev = ctx_.series_mgr().vpin_revision(Terminal::pair_key(pair_.exchange, pair_.symbol));
    if (rev != vpin_populated_revision_) {
        ind->set_points(ctx_.series_mgr().vpin(Terminal::pair_key(pair_.exchange, pair_.symbol)));
        vpin_populated_revision_ = rev;
    }
}

void ChartWidget::add_rsi_indicator(int period) {
    if (indicator_mgr_.has_indicator_of_type<Indicators::RSIIndicator>()) return;

    auto rsi_ind = std::make_unique<Indicators::RSIIndicator>(period);
    for (const auto& candle : candles().candles()) {
        rsi_ind->add_candle(candle.timestamp_ms, candle.close);
    }
    rsi_ind->update();
    indicator_mgr_.add_indicator(std::move(rsi_ind));
}

void ChartWidget::add_macd_indicator(int fast, int slow, int signal) {
    if (indicator_mgr_.has_indicator_of_type<Indicators::MACDIndicator>()) return;

    auto macd_ind = std::make_unique<Indicators::MACDIndicator>(fast, slow, signal);
    macd_ind->set_timeframe(candles().timeframe_seconds());
    for (const auto& candle : candles().candles()) {
        macd_ind->add_candle(candle.timestamp_ms, candle.close);
    }
    macd_ind->update();
    indicator_mgr_.add_indicator(std::move(macd_ind));
}
