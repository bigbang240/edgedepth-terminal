#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// watchlist_widget.h - Symbol watchlist sidebar (design v3, 2026-10-11)
//
// Docked left rail. A non-scrolling header stack (title with count chip and round
// icon controls · rounded search · category + venue dropdowns · quiet sort header)
// sits above a scrolling list of rows: [star] logo (venue badge) pair / volume,
// price / change pill. Compact rows are one line (pair, price, pill).
//
// Data: last / 24h% / 24h-volume come from the always-on TickerManager stream
// (owned by AppShell - no subscription here); categories + price formatting from
// SymbolRegistry. Row click switches the terminal symbol (page-reload flow, same
// as the picker). Sort: Pair / Volume / Price / 24h, each asc/desc. Star
// favourites are in-memory only (no persistence layer yet).
// ═══════════════════════════════════════════════════════════════════════════════
#include "ui/widget.h"
#include "core/app_context.h"
#include "core/symbol_metadata.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <array>

class WatchlistWidget : public Widget {
public:
    workspace::Json save_settings() const override;
    void load_settings(const workspace::Json& settings) override;
    WatchlistWidget(const Terminal::Pair& active_pair, const AppContext& ctx);

    void update() override;
    void render() override;
    WidgetType type() const override { return WidgetType::Watchlist; }
    const char* title() const override { return "Watchlist"; }
    UpdateFrequency update_frequency() const override { return UpdateFrequency::Slow; }

private:
    enum class Sort : uint8_t { Symbol, Change, Volume, Price };

    Terminal::Pair active_pair_;
    const AppContext& ctx_;

    // Filter / sort state
    char search_buf_[48] = {};
    int selected_category_ = -1;     // -1 = All
    std::string selected_exchange_;  // empty = all venues
    Sort sort_ = Sort::Volume;
    bool sort_desc_ = true;          // Symbol defaults A->Z; others biggest first
    std::vector<std::string> categories_;
    std::vector<int> category_counts_;   // pair count per category (aligned to categories_)
    int all_count_ = 0;                  // active-symbol total (the "All" count)
    int scoped_count_ = 0;               // active count after the exchange filter
    std::array<int, 3> exchange_counts_{}; // Binance Futures, Hyperliquid, Bybit
    bool categories_loaded_ = false;

    // Row extras: starred favourites (in-memory), compact mode, and the
    // category-popover search box.
    std::unordered_set<std::string> favorites_;
    bool compact_ = false;               // user compact toggle (auto when rail is narrow)
    char cat_search_[48] = {};           // category-popover substring filter

    // Cached visible list (rebuilt when filter/sort/ticker changes)
    std::vector<const SymbolMetadata*> visible_;
    int64_t prev_ticker_ms_ = 0;
    int64_t last_resort_ms_ = 0;
    char prev_search_[48] = {};
    int prev_category_ = -2;
    std::string prev_exchange_ = "__unset";
    Sort prev_sort_ = Sort::Symbol;
    bool prev_sort_desc_ = true;

    void rebuild_visible();
    void build_category_counts();
    void render_title_bar();
    void render_filter_bar();         // full-width symbol filter input
    void render_category_selector();  // category + venue selectors and popovers
    void render_sort_header();        // SYMBOL / LAST / 24H% sort chips
    void render_rows();
};
