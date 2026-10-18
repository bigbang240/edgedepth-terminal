#include "types/frame_profiler.h"
#include "ui/dom_widget.h"
#include "rendering/theme.h"
#include "ui/drawing/drawing_icons.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/orderbook_manager.h"
#include "core/symbol_metadata.h"
#include "replayer/replay_manager.h"

// ═══════════════════════════════════════════════════════════════════════════════
// File-local helpers
// ═══════════════════════════════════════════════════════════════════════════════
namespace {

// Right-aligned text within the current table cell
void cell_text_right(const char* txt) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float tw = ImGui::CalcTextSize(txt).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w > tw ? w - tw : 0.0f));
    ImGui::TextUnformatted(txt);
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// Lifecycle / data
// ═══════════════════════════════════════════════════════════════════════════════

DOMWidget::DOMWidget(const Terminal::Pair& pair, const AppContext& ctx,
                     const double tick_size,
                     const size_t levels_per_side)
    : pair_(pair)
    , ctx_(ctx)
    , stream_key_{pair, Terminal::Stream::Orderbook, 0}
    , title_(std::string("     DOM  ") + widget_symbol_label(pair.symbol) + "###dom_" + pair.exchange + "_" + pair.symbol)
    , tick_size_(tick_size)
    , levels_per_side_(levels_per_side)
    , fmt_(SymbolRegistry::instance().get_formatter(pair.exchange, pair.symbol))
{
    mode_companion = true; // A normal standalone panel follows its market chart.
    subscribed_streams_ = &ctx_.stream_mgr();
    subscribed_streams_->subscribe_orderbook(stream_key_);
    trade_accumulator_.init(pair, ctx_.stream_mgr(), tick_size);
}

DOMWidget::~DOMWidget() {
    if (subscribed_streams_) subscribed_streams_->unsubscribe_orderbook(stream_key_, this);
}

// Late-bind the instrument. A DOM can outlive the moment its tick was unknown:
// the metadata XHR is ~500 KB and in a /demo session it races the pack's own
// range fetches, so the widget is routinely built first. Everything the ladder
// derives from the tick (row grid, trade buckets, price text) is rebuilt here.
void DOMWidget::refresh_instrument() {
    const auto& reg = SymbolRegistry::instance();
    const double tick = reg.tick_or_zero(pair_.exchange, pair_.symbol);
    if (tick <= 0.0 || tick == tick_size_) return;

    tick_size_ = tick;
    fmt_       = reg.get_formatter(pair_.exchange, pair_.symbol);
    trade_accumulator_.set_tick_size(tick);

    // Grid-dependent view state: the old center/scroll were expressed in the
    // placeholder grid's units, and every cached row string was formatted at
    // placeholder precision.
    ladder_center_ = 0.0;
    scroll_offset_ = 0;
    group_mult_    = 1;
    max_bid_size_  = 0.0;
    max_ask_size_  = 0.0;
    cache_ob_ts_   = -1;
    cache_ob_uid_  = -1;
    cache_center_  = -1.0;
}

void DOMWidget::update() {
    // No tick means no ladder: every row price is center + n * tick, so an
    // invented tick invents the whole grid. render() draws the pending state.
    if (tick_size_ <= 0.0) return;

    const Terminal::Orderbook* ob = ctx_.ob_mgr().get_orderbook(pair_);
    if (!ob || !ob->snapshot) return;

    if (auto_center_) {
        // Boot transient: the (seeded) book lands before the first trade, so
        // last_price is still 0 - centering there rendered a placeholder
        // 0.000-price ladder until playback's first trade. Fall back to the
        // book mid so the seeded depth is on-screen from frame one.
        double center = ob->last_price;
        if (center <= 0.0 && !ob->bids.empty() && !ob->asks.empty()) {
            center = (ob->bids.begin()->first + ob->asks.begin()->first) * 0.5;
        }
        if (center > 0.0) {
            ladder_center_ = std::round(center / tick_size_) * tick_size_;
        }
    } else if (manual_center_price_ > 0.0) {
        ladder_center_ = std::round(manual_center_price_ / tick_size_) * tick_size_;
    }

    handle_keyboard_input();
    trade_accumulator_.check_auto_reset();

    // Rebuild the row models ONLY when an input actually changed - the book
    // read-buffer moves at server cadence, not at render FPS.
    if (ob->timestamp_ms != cache_ob_ts_ ||
        ob->last_update_id != cache_ob_uid_ ||
        trade_accumulator_.revision() != cache_acc_rev_ ||
        ladder_center_ != cache_center_ ||
        scroll_offset_ != cache_scroll_ ||
        group_mult_ != cache_group_ ||
        display_usd_ != cache_usd_ ||
        show_trade_columns_ != cache_trade_cols_) {
        cache_ob_ts_      = ob->timestamp_ms;
        cache_ob_uid_     = ob->last_update_id;
        cache_acc_rev_    = trade_accumulator_.revision();
        cache_center_     = ladder_center_;
        cache_scroll_     = scroll_offset_;
        cache_group_      = group_mult_;
        cache_usd_        = display_usd_;
        cache_trade_cols_ = show_trade_columns_;
        build_row_models(*ob);
    }
}

// Sum book depth across a grouped band of `mult` sub-ticks. With mult == 1 this is
// exactly the single-tick lookup, so the default (ungrouped) ladder is unchanged.
static double dom_band_size(const Terminal::Orderbook& ob, double base, bool ask,
                            double tick, int mult) {
    double s = 0.0;
    for (int k = 0; k < mult; ++k) {
        const double p = std::round((base + (ask ? 1.0 : -1.0) * k * tick) / tick) * tick;
        if (ask) {
            const auto it = ob.asks.find(p);
            if (it != ob.asks.end()) s += it->second;
        } else {
            const auto it = ob.bids.find(p);
            if (it != ob.bids.end()) s += it->second;
        }
    }
    return s;
}

void DOMWidget::update_max_sizes(const Terminal::Orderbook& ob) {
    max_bid_size_ = 0.0;
    max_ask_size_ = 0.0;
    const double et = tick_size_ * static_cast<double>(group_mult_);
    const double adjusted_center = ladder_center_ + (scroll_offset_ * et);

    for (int i = 0; i < static_cast<int>(levels_per_side_); i++) {
        const double ask_price = std::round((adjusted_center + (i + 1) * et) / et) * et;
        max_ask_size_ = std::max(max_ask_size_,
                                 dom_band_size(ob, ask_price, true, tick_size_, group_mult_));
        const double bid_price = std::round((adjusted_center - (i + 1) * et) / et) * et;
        max_bid_size_ = std::max(max_bid_size_,
                                 dom_band_size(ob, bid_price, false, tick_size_, group_mult_));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Row-model cache build - ALL formatting + book/tape lookups happen here, on
// input change only. render_ladder() just draws these each frame.
// ═══════════════════════════════════════════════════════════════════════════════

void DOMWidget::build_row_models(const Terminal::Orderbook& ob) {
    update_max_sizes(ob);
    const double max_size = std::max(max_bid_size_, max_ask_size_);
    const double et = tick_size_ * static_cast<double>(group_mult_);
    const double adjusted_center = ladder_center_ + (scroll_offset_ * et);

    rows_ask_.resize(levels_per_side_);
    rows_bid_.resize(levels_per_side_);
    // The current price's text: a row's digits that differ from it are what the eye reads.
    char current_txt[24];
    snprintf(current_txt, sizeof(current_txt), fmt_.price_fmt, ob.last_price);
    const double best_bid = ob.bids.empty() ? 0.0 : ob.bids.begin()->first;
    const double best_ask = ob.asks.empty() ? 0.0 : ob.asks.begin()->first;
    const double half_tick = tick_size_ * 0.5;

    auto fill = [&](RowModel& rm, double price, bool is_ask) {
        rm = RowModel{};
        rm.price = price;
        snprintf(rm.price_txt, sizeof(rm.price_txt), fmt_.price_fmt, price);
        {
            const size_t n = std::strlen(rm.price_txt);
            size_t k = 0;
            while (k < n && rm.price_txt[k] == current_txt[k]) ++k;
            rm.bright_from = static_cast<uint8_t>(std::min(k, n >= 2 ? n - 2 : size_t{0}));
        }
        // A grouped ask band runs up from its price, a bid band down from it.
        rm.is_best = is_ask
            ? best_ask > 0.0 && best_ask > price - half_tick && best_ask < price + et - half_tick
            : best_bid > 0.0 && best_bid < price + half_tick && best_bid > price - et + half_tick;

        const double size = dom_band_size(ob, price, is_ask, tick_size_, group_mult_);
        if (size > 0.0 && max_size > 0.0) {
            rm.has_size = true;
            rm.depth_frac = static_cast<float>(size / max_size);
            fmt_value(size, price, rm.size_txt, sizeof(rm.size_txt));
        }

        if (show_trade_columns_) {
            double agg_buy = 0.0, agg_sell = 0.0;
            bool agg_any = false;
            for (int k = 0; k < group_mult_; ++k) {
                const double p = std::round((price + (is_ask ? 1.0 : -1.0) * k * tick_size_) / tick_size_) * tick_size_;
                if (const TradeAtPriceLevel* t = trade_accumulator_.get(p)) {
                    agg_buy += t->buy_volume; agg_sell += t->sell_volume; agg_any = true;
                }
            }
            if (agg_any && agg_buy > 0.0) {
                rm.has_buy = true;
                fmt_value(agg_buy, price, rm.buy_txt, sizeof(rm.buy_txt));
            }
            if (agg_any && agg_sell > 0.0) {
                rm.has_sell = true;
                fmt_value(agg_sell, price, rm.sell_txt, sizeof(rm.sell_txt));
            }
            if (agg_any && (agg_buy + agg_sell) > 0.0) {
                rm.has_delta = true;
                const double delta = agg_buy - agg_sell;
                rm.delta_pos = delta >= 0.0;
                fmt_signed(delta, price, rm.delta_txt, sizeof(rm.delta_txt));
            }
        }
    };

    for (int i = 0; i < static_cast<int>(levels_per_side_); i++) {
        const double ask_price = std::round((adjusted_center + (i + 1) * et) / et) * et;
        fill(rows_ask_[static_cast<size_t>(i)], ask_price, true);
        const double bid_price = std::round((adjusted_center - (i + 1) * et) / et) * et;
        fill(rows_bid_[static_cast<size_t>(i)], bid_price, false);
    }

    // Current-price row texts
    row_current_ = CurrentRowModel{};
    snprintf(row_current_.price_txt, sizeof(row_current_.price_txt), fmt_.price_fmt, ob.last_price);
    if (show_trade_columns_) {
        if (trade_accumulator_.total_buy_volume() > 0.0) {
            row_current_.has_buy = true;
            fmt_value(trade_accumulator_.total_buy_volume(), ob.last_price,
                      row_current_.buy_txt, sizeof(row_current_.buy_txt));
        }
        if (trade_accumulator_.total_sell_volume() > 0.0) {
            row_current_.has_sell = true;
            fmt_value(trade_accumulator_.total_sell_volume(), ob.last_price,
                      row_current_.sell_txt, sizeof(row_current_.sell_txt));
        }
        const double total_delta = trade_accumulator_.total_delta();
        row_current_.delta_pos = total_delta >= 0.0;
        fmt_signed(total_delta, ob.last_price,
                   row_current_.delta_txt, sizeof(row_current_.delta_txt));
    }
}

void DOMWidget::handle_keyboard_input() {
    if (!ImGui::IsWindowFocused()) return;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        scroll_offset_++;
        auto_center_ = false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        scroll_offset_--;
        auto_center_ = false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
        auto_center_ = true;
        scroll_offset_ = 0;
    }
}

void DOMWidget::handle_mouse_input() {
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
        scroll_offset_ += static_cast<int>(ImGui::GetIO().MouseWheel);
        auto_center_ = false;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Value formatting - COIN qty vs compact USD notional
// ═══════════════════════════════════════════════════════════════════════════════

void DOMWidget::fmt_value(double qty, double price, char* buf, size_t n) const {
    if (display_usd_) {
        const double usd = qty * price;
        const double a = std::abs(usd);
        if (a >= 1e9)      snprintf(buf, n, "%.2fB", usd / 1e9);
        else if (a >= 1e6) snprintf(buf, n, "%.2fM", usd / 1e6);
        else if (a >= 1e3) snprintf(buf, n, "%.1fK", usd / 1e3);
        else               snprintf(buf, n, "%.0f", usd);
    } else {
        // Compact large COIN quantities too (else 5-6 digit sizes for low-priced
        // coins overflow the slim trade/Δ columns and clip past the panel edge).
        const double a = std::abs(qty);
        if (a >= 1e9)      snprintf(buf, n, "%.2fB", qty / 1e9);
        else if (a >= 1e6) snprintf(buf, n, "%.2fM", qty / 1e6);
        else if (a >= 1e3) snprintf(buf, n, "%.1fK", qty / 1e3);
        else               snprintf(buf, n, fmt_.qty_fmt, qty);
    }
}

void DOMWidget::fmt_signed(double v, double price, char* buf, size_t n) const {
    char tmp[24];
    fmt_value(std::abs(v), price, tmp, sizeof(tmp));
    snprintf(buf, n, "%s%s", v >= 0.0 ? "+" : "-", tmp);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Main render
// ═══════════════════════════════════════════════════════════════════════════════

void DOMWidget::render() {
    if (!is_open) return;

    std::string window_title = title_suffix_.empty() ? title_ : title_ + title_suffix_;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    place_new_window();
    if (!ImGui::Begin(window_title.c_str(), &is_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::PopStyleVar();
        ImGui::End();
        return;
    }
    ImGui::PopStyleVar();

    if (tick_size_ <= 0.0) {
        // Deliberately NOT a ladder. A grid drawn on a guessed tick is a lie
        // that looks like data: at 0.10 on a $0.24 perp the entire book folds
        // into one row and the DOM reads as empty. refresh_instrument() swaps
        // this out for the real grid as soon as the tick lands.
        const auto& reg = SymbolRegistry::instance();
        // Distinguish "not answered yet" from "answered, and this instrument is
        // not in it": the second is permanent and a spinner would read as a hang.
        const bool absent = reg.is_loaded() && !reg.has(pair_.exchange, pair_.symbol);
        ImGui::PushFont(Theme::Fonts::ui());
        if (absent) {
            ImGui::TextColored(Theme::Tokens::TX3, "  No tick size for %s %s",
                               pair_.symbol.c_str(), pair_.exchange.c_str());
        } else {
            ImGui::TextColored(Theme::Tokens::TX3, "  Loading %s tick size...",
                               pair_.symbol.c_str());
        }
        ImGui::PopFont();
        ImGui::End();
        return;
    }

    const Terminal::Orderbook* ob = ctx_.ob_mgr().get_orderbook(pair_);
    if (!ob || !ob->snapshot || ob->asks.empty() || ob->bids.empty()) {
        ImGui::PushFont(Theme::Fonts::ui());
        ImGui::TextColored(Theme::Tokens::TX3, "  No DOM data for %s %s",
                           pair_.symbol.c_str(), pair_.exchange.c_str());
        ImGui::PopFont();
        ImGui::End();
        return;
    }

    render_controls(*ob);
    render_ladder(*ob);
    handle_mouse_input();

    ImGui::End();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Controls bar - grouping · Auto/Reset · USD/COIN · flow hint (window via r-click)
// ═══════════════════════════════════════════════════════════════════════════════

void DOMWidget::render_controls(const Terminal::Orderbook& ob) {
    using namespace Theme;
    using drawing::UiIcon;
    // One slim toolbar (design v3, 2026-10-11): units, grouping and the flow window on the left; the spread and
    // the centre / settings icons on the right. The menus hold the rest.
    const float K = ImGui::GetFontSize() / 18.4f;
    const float pad = 10.0f * K, h = 26.0f * K, top = 6.0f * K, ib = 26.0f * K;
    const float ww = ImGui::GetWindowWidth();
    const ImVec2 origin = ImGui::GetCursorPos();
    push_font(Fonts::ui(), 13.5f * K);
    ImGui::SetCursorPos(ImVec2(origin.x + pad, origin.y + top));
    static const char* const kUnits[] = {"Coin", "USD"};
    const int units = segmented("dom_units", kUnits, 2, display_usd_ ? 1 : 0, h);
    display_usd_ = units == 1;
    ImGui::SameLine(0.0f, 6.0f * K);
    char group_label[32];
    snprintf(group_label, sizeof(group_label), fmt_.price_fmt, tick_size_ * group_mult_);
    if (button(group_label, ButtonKind::Ghost, ImVec2(0, h), UiIcon::ChevronDown, true)) ImGui::OpenPopup("##dom_group");
    if (ImGui::IsItemHovered()) tooltip("Price grouping");
    const auto reset_mode = trade_accumulator_.reset_mode();
    const int64_t interval = trade_accumulator_.reset_interval_sec();
    const int flow = reset_mode == AccumulatorResetMode::Manual ? 4 : reset_mode == AccumulatorResetMode::Session ? 3 :
                     interval <= 300 ? 0 : interval <= 900 ? 1 : 2;
    static const char* const kFlowShort[] = {"Flow 5m", "Flow 15m", "Flow 1h", "Flow session", "Flow manual"};
    ImGui::SameLine(0.0f, 2.0f * K);
    if (show_trade_columns_) {
        if (button(kFlowShort[flow], ButtonKind::Ghost, ImVec2(0, h), UiIcon::ChevronDown, true)) ImGui::OpenPopup("##dom_flow");
        if (ImGui::IsItemHovered()) tooltip("When the Buys, Sells and Delta columns start counting again");
        ImGui::SameLine(0.0f, 0.0f);
    }
    const float left_end = ImGui::GetCursorPosX();

    // Right: settings, then centre, then the spread when there is room.
    const float icons_x = ww - pad - ib * 2.0f - 2.0f * K;
    ImGui::SetCursorPos(ImVec2(icons_x, origin.y + top));
    if (icon_button("##dom_center", UiIcon::Target, auto_center_ ? "Following the price (Home). Scroll to look away." :
                                                                  "Back to the price (Home)", ib, auto_center_)) {
        auto_center_ = !auto_center_;
        if (auto_center_) scroll_offset_ = 0;
    }
    ImGui::SameLine(0.0f, 2.0f * K);
    if (icon_button("##dom_settings", UiIcon::Gear, "Settings", ib)) ImGui::OpenPopup("##dom_settings");
    if (!ob.bids.empty() && !ob.asks.empty() && tick_size_ > 0.0) {
        const double bid = ob.bids.begin()->first, ask = ob.asks.begin()->first, spread = ask - bid;
        const long ticks = std::lround(spread / tick_size_);
        const double mid = (ask + bid) * 0.5;
        char spread_txt[64];
        if (ticks >= 1 && ticks <= 999) snprintf(spread_txt, sizeof(spread_txt), "Spread %ld tick%s", ticks, ticks == 1 ? "" : "s");
        else snprintf(spread_txt, sizeof(spread_txt), "Spread -");
        float sw = ImGui::CalcTextSize(spread_txt).x;
        if (icons_x - 10.0f * K - sw <= left_end + 12.0f * K) {   // tight: drop the word, the tooltip says it
            std::memmove(spread_txt, spread_txt + 7, std::strlen(spread_txt + 7) + 1);
            sw = ImGui::CalcTextSize(spread_txt).x;
        }
        const float sx = icons_x - 10.0f * K - sw;
        if (sx > left_end + 12.0f * K) {
            ImGui::SetCursorPos(ImVec2(sx, origin.y + top + (h - ImGui::GetFontSize()) * 0.5f));
            ImGui::TextColored(Tokens::TX3, "%s", spread_txt);
            if (ImGui::IsItemHovered() && mid > 0.0) {
                char a[32], b[32];
                snprintf(a, sizeof(a), fmt_.price_fmt, bid);
                snprintf(b, sizeof(b), fmt_.price_fmt, ask);
                tooltip("Spread: best bid %s, best ask %s\n%.2f bp of the mid", a, b, spread / mid * 1e4);
            }
        }
    }
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(origin.x, origin.y + top + h + top));
    // The toolbar's bottom hairline.
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - 0.5f), ImVec2(p.x + ww, p.y - 0.5f), u32(Tokens::BD1));
    }

    if (begin_menu("##dom_group", 200.0f)) {
        menu_section("PRICE GROUPING");
        const int multipliers[] = {1, 10, 100};
        static const char* const kHints[] = {"1 tick", "10 ticks", "100 ticks"};
        for (int i = 0; i < 3; ++i) {
            char label[32];
            snprintf(label, sizeof(label), fmt_.price_fmt, tick_size_ * multipliers[i]);
            if (menu_item(group_mult_ == multipliers[i] ? UiIcon::Check : UiIcon::None, label, kHints[i])) {
                group_mult_ = multipliers[i]; auto_center_ = true; scroll_offset_ = 0;
            }
        }
        ImGui::EndPopup();
    }
    if (begin_menu("##dom_flow", 240.0f)) {
        menu_section("BUYS, SELLS AND DELTA RESET");
        static const char* const kFlow[] = {"Every 5 minutes", "Every 15 minutes", "Every hour", "Each session", "Only when I reset"};
        for (int i = 0; i < 5; ++i) {
            if (!menu_item(flow == i ? UiIcon::Check : UiIcon::None, kFlow[i])) continue;
            if (i == 4) trade_accumulator_.set_reset_mode(AccumulatorResetMode::Manual);
            else if (i == 3) trade_accumulator_.set_reset_mode(AccumulatorResetMode::Session);
            else trade_accumulator_.set_reset_mode(AccumulatorResetMode::Periodic,
                i == 0 ? ResetPresets::FIVE_MIN : i == 1 ? ResetPresets::FIFTEEN_MIN : ResetPresets::ONE_HOUR);
        }
        menu_separator();
        if (menu_item(UiIcon::Refresh, "Reset now")) trade_accumulator_.reset();
        ImGui::EndPopup();
    }
    if (begin_menu("##dom_settings", 260.0f)) {
        if (menu_toggle("Buys, sells and delta columns", show_trade_columns_)) show_trade_columns_ = !show_trade_columns_;
        bool pinned = !mode_companion;
        if (menu_toggle("Keep visible in real-time", pinned)) mode_companion = pinned;
        menu_separator();
        if (menu_item(UiIcon::Target, "Back to the price", "Home")) { auto_center_ = true; scroll_offset_ = 0; }
        ImGui::EndPopup();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Ladder table - buys · bids · PRICE · asks · sells · Δ
// ═══════════════════════════════════════════════════════════════════════════════

void DOMWidget::render_ladder(const Terminal::Orderbook& ob) {
    using namespace Theme;
    ImGui::PushFont(Fonts::mono_sm());
    const float row_h = row_h_dense();   // DOM/tape density (18px @ default dial)
    const float pad_y = std::max(0.0f, (row_h - ImGui::GetFontSize()) * 0.5f);

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, pad_y));

    const int num_columns = show_trade_columns_ ? 6 : 3;
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                 ImGuiTableFlags_NoHostExtendX;
    char measured_price[32];
    fmt_.format_price(measured_price, sizeof(measured_price), ob.last_price);
    const float number_w = ImGui::CalcTextSize("+00.000").x;
    const float inner_width = std::max(
        ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize,
        ImGui::CalcTextSize(measured_price).x + 24 + (num_columns - 1) * (number_w + 12));

    const ImVec2 tbl_size(0.0f, std::max(60.0f, ImGui::GetContentRegionAvail().y));
    ImGui::PushStyleColor(ImGuiCol_TableRowBg, ImVec4(0, 0, 0, 0));
    if (!ImGui::BeginTable("DOMTable", num_columns, flags, tbl_size, inner_width)) {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::PopFont();
        return;
    }

    if (show_trade_columns_)
        ImGui::TableSetupColumn("Buys", ImGuiTableColumnFlags_WidthFixed, number_w);
    ImGui::TableSetupColumn("Bids",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
    char price_label[32];
    fmt_.format_price(price_label, sizeof(price_label), ob.last_price);
    ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize(price_label).x + 16);
    ImGui::TableSetupColumn("Asks",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
    if (show_trade_columns_) {
        ImGui::TableSetupColumn("Sells", ImGuiTableColumnFlags_WidthFixed, number_w);
        ImGui::TableSetupColumn("Delta", ImGuiTableColumnFlags_WidthFixed, number_w);
    }
    ImGui::TableSetupScrollFreeze(0, 1);

    // Header: quiet sentence-case labels over a hairline.
    {
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers, 24.0f);
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, u32(Tokens::PANEL));
        push_font(Fonts::ui(), 12.5f);
        int c = 0;
        float y_bottom = 0.0f, x0 = 0.0f, x1 = 0.0f;
        auto head = [&](const char* t, int align) {
            ImGui::TableSetColumnIndex(c++);
            const ImVec2 cur = ImGui::GetCursorScreenPos();
            if (c == 1) { x0 = cur.x - 6.0f; y_bottom = cur.y - pad_y + 24.0f; }
            x1 = cur.x + ImGui::GetContentRegionAvail().x + 6.0f;
            const float w = ImGui::GetContentRegionAvail().x;
            const float tw = ImGui::CalcTextSize(t).x;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f);
            if (align == 2)      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w > tw ? w - tw : 0.0f));
            else if (align == 1) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w > tw ? (w - tw) * 0.5f : 0.0f));
            ImGui::TextColored(Tokens::TX3, "%s", t);
        };
        if (show_trade_columns_) head("Buys", 2);
        head("Bids", 2);
        head("Price", 1);
        head("Asks", 0);
        if (show_trade_columns_) {
            head("Sells", 2);
            head("Delta", 2);
        }
        ImGui::PopFont();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(x0, y_bottom - 0.5f), ImVec2(x1, y_bottom - 0.5f), u32(Tokens::BD1));
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Rows come from the row-model cache (rebuilt in update() on change only).
    // Guard: first frames can render before update() built the models.
    if (rows_ask_.size() == levels_per_side_ && rows_bid_.size() == levels_per_side_) {
        for (int i = static_cast<int>(levels_per_side_) - 1; i >= 0; i--)
            render_level_row(rows_ask_[static_cast<size_t>(i)], /*is_ask=*/true, dl, pad_y);
        render_current_row(ob, dl, pad_y);
        if (auto_center_) ImGui::SetScrollHereY(0.5f);
        for (int i = 0; i < static_cast<int>(levels_per_side_); i++)
            render_level_row(rows_bid_[static_cast<size_t>(i)], /*is_ask=*/false, dl, pad_y);
    }

    ImGui::EndTable();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::PopFont();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Level row - one price band (ask or bid side)
// ═══════════════════════════════════════════════════════════════════════════════

namespace {
// A depth cell: a bar from the price side that always holds its whole number (never half under it) and runs on
// past it in proportion to the band's size, so the cell reads as a bar chart with the numbers at its base at any
// panel width. Larger bands are brighter on the OCEAN ramp (kept short of its palest end so the number stays
// readable on it); a wall ten times the rest still leaves ordinary bands visible (gentle power).
void depth_cell(ImDrawList* dl, const char* txt, float frac, bool bid_side, float row_h, float pad_y) {
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    const float cell_w = ImGui::GetContentRegionAvail().x;
    const float y0 = std::round(cur.y - pad_y + 2.0f), y1 = std::round(cur.y - pad_y + row_h - 2.0f);
    Theme::depth_bar(dl, ImVec2(cur.x, y0), ImVec2(cur.x + cell_w, y1), txt, frac, bid_side, cur.y);
}
}  // namespace

void DOMWidget::render_level_row(const RowModel& rm, bool is_ask,
                                 ImDrawList* dl, float pad_y) {
    using namespace Theme;
    const float row_h = row_h_dense();
    ImGui::TableNextRow(ImGuiTableRowFlags_None, row_h);

    int col = 0;
    ImGui::TableSetColumnIndex(0);
    {
        // Hover: a soft row fill, so the eye can follow a price across the columns.
        const float y0 = ImGui::GetCursorScreenPos().y - pad_y;
        const ImVec2 m = ImGui::GetIO().MousePos;
        if (ImGui::IsWindowHovered() && m.y >= y0 && m.y < y0 + row_h)
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, u32(Tokens::CONTROL));
    }

    // ── Buys ──
    if (show_trade_columns_) {
        ImGui::TableSetColumnIndex(col++);
        if (rm.has_buy) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(Tokens::UP.x, Tokens::UP.y, Tokens::UP.z, 0.78f));
            cell_text_right(rm.buy_txt);
            ImGui::PopStyleColor();
        }
    }

    // ── Bids ──
    ImGui::TableSetColumnIndex(col++);
    if (!is_ask && rm.has_size) depth_cell(dl, rm.size_txt, rm.depth_frac, true, row_h, pad_y);

    // ── Price: centred; the digits that differ from the current price bright, best bid / ask in the side colour ──
    ImGui::TableSetColumnIndex(col++);
    {
        const ImVec2 cur = ImGui::GetCursorScreenPos();
        const float cell_w = ImGui::GetContentRegionAvail().x;
        const char* split = rm.price_txt + rm.bright_from;
        const float tw = ImGui::CalcTextSize(rm.price_txt).x;
        const float pw = ImGui::CalcTextSize(rm.price_txt, split).x;
        const float x = std::round(cur.x + std::max(0.0f, (cell_w - tw) * 0.5f));
        const ImVec4& bright = rm.is_best ? (is_ask ? Tokens::DOWN : Tokens::UP) : Tokens::TX1;
        dl->AddText(ImVec2(x, cur.y), u32(Tokens::TX3), rm.price_txt, split);
        dl->AddText(ImVec2(x + pw, cur.y), u32(bright), split);
    }

    // ── Asks ──
    ImGui::TableSetColumnIndex(col++);
    if (is_ask && rm.has_size) depth_cell(dl, rm.size_txt, rm.depth_frac, false, row_h, pad_y);

    if (!show_trade_columns_) return;

    // ── Sells ──
    ImGui::TableSetColumnIndex(col++);
    if (rm.has_sell) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(Tokens::DOWN.x, Tokens::DOWN.y, Tokens::DOWN.z, 0.78f));
        cell_text_right(rm.sell_txt);
        ImGui::PopStyleColor();
    }

    // ── Delta ──
    ImGui::TableSetColumnIndex(col++);
    if (rm.has_delta) {
        ImGui::PushStyleColor(ImGuiCol_Text, rm.delta_pos ? Tokens::UP : Tokens::DOWN);
        cell_text_right(rm.delta_txt);
        ImGui::PopStyleColor();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Current-price row - ELEV fill, inset hairlines, BRAND price chip,
// cumulative buy/sell/Δ totals in the trade columns
// ═══════════════════════════════════════════════════════════════════════════════

void DOMWidget::render_current_row(const Terminal::Orderbook& ob, ImDrawList* dl, float pad_y) {
    (void)ob;
    using namespace Theme;
    const float row_h = row_h_dense() + 4.0f;
    ImGui::TableNextRow(ImGuiTableRowFlags_None, row_h);
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, u32(Tokens::ELEV));

    const CurrentRowModel& cr = row_current_;
    int col = 0;
    float row_y0 = 0.0f, x_left = 0.0f;
    const float lift = 2.0f;   // the taller row: centre its text

    if (show_trade_columns_) {
        ImGui::TableSetColumnIndex(col++);
        const ImVec2 c0 = ImGui::GetCursorScreenPos();
        row_y0 = c0.y - pad_y;
        x_left = c0.x - 6.0f;
        if (cr.has_buy) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + lift);
            ImGui::PushStyleColor(ImGuiCol_Text, Tokens::UP);
            cell_text_right(cr.buy_txt);
            ImGui::PopStyleColor();
        }
    }

    ImGui::TableSetColumnIndex(col++);
    if (!show_trade_columns_) {
        const ImVec2 c0 = ImGui::GetCursorScreenPos();
        row_y0 = c0.y - pad_y;
        x_left = c0.x - 6.0f;
    }

    // ── Price chip ──
    ImGui::TableSetColumnIndex(col++);
    {
        const ImVec2 cur = ImGui::GetCursorScreenPos();
        const float cell_w = ImGui::GetContentRegionAvail().x;
        const float tw = ImGui::CalcTextSize(cr.price_txt).x;
        const float chip_w = std::min(cell_w + 10.0f, tw + 16.0f);
        const float cx0 = std::round(cur.x + (cell_w - chip_w) * 0.5f);
        const float cy0 = std::round(row_y0 + 2.0f);
        dl->AddRectFilled(ImVec2(cx0, cy0), ImVec2(cx0 + chip_w, cy0 + row_h - 4.0f), u32(Tokens::BRAND), Radius::R1);
        dl->AddText(ImVec2(std::round(cur.x + (cell_w - tw) * 0.5f), cur.y + lift), u32(Tokens::BRAND_INK), cr.price_txt);
    }

    ImGui::TableSetColumnIndex(col++);
    float x_right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x + 6.0f;

    if (show_trade_columns_) {
        ImGui::TableSetColumnIndex(col++);
        if (cr.has_sell) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + lift);
            ImGui::PushStyleColor(ImGuiCol_Text, Tokens::DOWN);
            cell_text_right(cr.sell_txt);
            ImGui::PopStyleColor();
        }
        ImGui::TableSetColumnIndex(col++);
        x_right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x + 6.0f;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + lift);
        ImGui::PushStyleColor(ImGuiCol_Text, cr.delta_pos ? Tokens::UP : Tokens::DOWN);
        cell_text_right(cr.delta_txt);
        ImGui::PopStyleColor();
    }

    const ImU32 hairline = u32(Tokens::BD2);
    dl->AddLine(ImVec2(x_left, row_y0 + 0.5f),         ImVec2(x_right, row_y0 + 0.5f),         hairline);
    dl->AddLine(ImVec2(x_left, row_y0 + row_h - 0.5f), ImVec2(x_right, row_y0 + row_h - 0.5f), hairline);
}
