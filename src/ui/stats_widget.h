#pragma once
// Live market summary with explicit windows and missing-data states.
#include "core/stats_snapshot.h"
#include "core/market_context.h"
#include "core/stats_ratio_history.h"
#include "core/stats_oi_history.h"
#include "ui/widget.h"
#include "types/types.h"
#include "stream_handler.h"
#include "core/symbol_metadata.h"
#include "core/app_context.h"
#include "rendering/theme.h"
#include <string>
#include <cstdint>

struct PositioningState;

class StatsWidget : public Widget {
public:
    workspace::Json save_settings() const override;
    void load_settings(const workspace::Json& settings) override;
    StatsWidget(const Terminal::Pair& pair, const AppContext& ctx, const PriceFormatter& fmt);
    ~StatsWidget() override;

    // Price precision can land after the widget is built. See Widget::refresh_instrument.
    void refresh_instrument() override {
        const PriceFormatter f =
            SymbolRegistry::instance().get_formatter(pair_.exchange, pair_.symbol);
        if (f.resolved) fmt_ = f;
    }
    void render() override;
    void update() override {}
    WidgetType type() const override { return WidgetType::Stats; }
    [[nodiscard]] const Terminal::Pair& pair() const { return pair_; }
    const char* title() const override { return title_.c_str(); }
    void handle_stat(const Terminal::Stat& stat);

private:
    // Persisted section IDs stay stable when the visible order changes.
    enum Section : uint8_t { SEC_ORDER_FLOW = 0, SEC_POSITIONING, SEC_LIQ_RISK, SEC_CONTEXT, SEC_PRICE_RATIO, SEC_OVERVIEW, SEC_COUNT };

    Terminal::Pair    pair_;
    std::string       title_;
    StreamKey         stream_key_;
    StreamKey         hour_stream_key_;
    StreamKey         volume_stream_key_;
    const AppContext& ctx_;
    PriceFormatter    fmt_;
    StatsSnapshot snapshot_;
    MinuteVolumeSnapshot volume_snapshot_;
    StatsSnapshot hour_snapshot_{StatsSnapshot::hour_ms};
    StatFreshness::Field stat_field(int64_t clock_ms, int64_t stale_after_ms) const;
    const PositioningState* positioning() const;

    // Session UI state
    bool more_details_ = false;
    bool section_open_[SEC_COUNT] = { false, false, false, false, false, true };
    MarketContext market_context_;
    int context_id_ = 0;
    int64_t context_requested_ms_ = 0;
    bool context_locked_ = false;
    void update_market_context(bool enabled);
    void render_market_context();
    StatsRatioHistory ratio_history_;
    int64_t ratio_requested_ms_ = 0;
    bool ratio_loaded_ = false;
    bool ratio_inverse_ = false;
    bool ratio_archive_ = false;
    int ratio_hours_ = 3;
    int ratio_days_ = 1;
    void update_ratio_history(bool enabled);
    void render_price_ratio();
    StatsOIHistory oi_history_;
    int64_t history_requested_ms_ = 0;
    int history_attempts_ = 0;

    // Fully expanded readings, with individually collapsible sections.
    void render_status_strip(bool live);   // source activity, help and actual tier
    void render_header_price();            // live last/mark, 24h change and volume
    bool render_section_header(Section s, const char* name, bool pro_section, const char* scope);

    // ── Section bodies (step 2) ─────────────────────────────────────────────
    void render_order_flow(bool overview = false);
    void render_icebergs();
    void note(const char* text);
    void render_positioning(bool overview = false);
    void render_liq_risk(bool overview = false);

    // One metric row: label (left) + value (right) + optional chip / detail /
    // meter. Pro rows are redacted (label kept crisp, value/meter replaced with a
    // muted bar + lock) for free-tier viewers. Numerals use mono; colors use Tokens.
    struct Row {
        const char* label      = "";
        bool        prominent  = false;
        bool        pro        = false;
        const char* value      = "--";
        ImVec4      value_col  = Theme::Tokens::TX1;
        const char* detail     = nullptr;
        ImVec4      detail_col = Theme::Tokens::TX3;
        int         meter      = 0;   // 0 none / 1 proportional / 2 directional / 3 fill
        float       meter_a    = 0.0f;
        ImVec4      meter_c1   = Theme::Tokens::UP;
        ImVec4      meter_c2   = Theme::Tokens::DOWN;
        const char* chip       = nullptr;  // regime chip drawn before the value
        int         chip_level = 0;        // amber escalation 0..3
        const char* help       = nullptr;
    };
    void metric_row(const Row& r);
    void summary_pair(const Row& left, const Row& right, bool cards = false);
    void split_bar(float share, const char* left, const char* right);
};
