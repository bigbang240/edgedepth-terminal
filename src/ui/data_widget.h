#pragma once
#include "ui/widget.h"
#include "core/app_context.h"
#include "core/market_data_view.h"
#include "core/research_readings.h"
#include "core/stats_snapshot.h"

class DataWidget final : public Widget {
public:
    DataWidget(const Terminal::Pair& pair, const AppContext& ctx);
    ~DataWidget() override;
    void render() override;
    void update() override;
    WidgetType type() const override { return WidgetType::Data; }
    const char* title() const override { return title_.c_str(); }
    const Terminal::Pair& pair() const { return pair_; }
    workspace::Json save_settings() const override { return {{"hours", hours_}, {"oi_usd", oi_usd_}, {"readings_view", readings_view_}}; }
    void load_settings(const workspace::Json& j) override {
        int hours = hours_; workspace::read(j, "hours", hours, 4, 24);
        set_hours(hours);
        workspace::read(j, "oi_usd", oi_usd_);
        workspace::read(j, "readings_view", readings_view_);
        fit_dirty_ = true;
    }
    void set_hours(int hours) { hours_ = hours == 24 ? 24 : 4; fit_dirty_ = true; inspection_.clear_hover(); }
    std::string initial_dock_target;
private:
    Terminal::Pair pair_;
    const AppContext& ctx_;
    std::string title_;
    int request_id_, readings_request_id_, hours_ = 4;
    ResearchReadings readings_;
    ImGuiTextFilter readings_filter_;
    bool readings_loaded_ = false, readings_failed_ = false, readings_missing_ = false, readings_view_ = false;
    std::string selected_reading_;
    bool oi_usd_ = false, loaded_ = false, locked_ = false, failed_ = false;
    bool fit_dirty_ = true, context_loaded_ = false, clocks_open_ = false, scroll_to_inspector_ = false;
    bool method_open_ = false, inspector_clocks_open_ = false, touch_controls_ = false;
    int expanded_ = -1, condition_ = -1, keyboard_plot_ = -1;
    int64_t requested_ = 0;
    size_t first_ = 0;
    float unit_ = 1;
    market_data_view::Inspection inspection_;
    std::array<market_data_view::ChartFit, market_data_view::chart_count> fits_{};
    MarketDataHistory history_;
    MarketContext context_, last_context_;
    StreamManager* basic_streams_ = nullptr;
    StatsSnapshot basic_snapshot_;
    void basic_subscription(bool enabled);
    void basic_overview();
    void pro_preview();
    void cancel();
    void poll();
    void page_header(bool tabs);
    void toolbar();
    void charts();
    void history_actions();
    void research_actions(int64_t at, const char* fields = "");
    void context_row();
    void research();
    void research_readings();
    void inspector(size_t id);
    void select_condition(int id);
    void source_clock(size_t id);
    const MarketContext::Item& context_item(size_t id) const;
    market_data_view::ContextState context_state(size_t id) const;
    void context_age(size_t id, char* out, size_t size) const;
};
