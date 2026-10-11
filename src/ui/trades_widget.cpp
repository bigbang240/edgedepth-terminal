#include "ui/trades_widget.h"
#include "core/display_time_zone.h"
#include "core/stream_presence.h"
#include "rendering/theme.h"
#include "ui/drawing/drawing_icons.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

TradesWidget::TradesWidget(const Terminal::Pair& pair, const AppContext& ctx, const PriceFormatter& fmt)
    : pair_(pair)
    , title_(std::string("     Trades  ") + widget_symbol_label(pair.symbol) + "###trades_" + pair.exchange + "_" + pair.symbol)
    , stream_key_{pair, Terminal::Stream::Trades, 0}
    , ctx_(ctx)
    , fmt_(fmt)
{
    mode_companion = true; // A normal standalone panel follows its market chart.
    trade_flash_times_.fill(0.0f);
    trade_alphas_.fill(0.0f);
    StreamHandler<Terminal::Trade> handler{
        .widget_ptr = this,
        .callback = [](void* ptr, const Terminal::Trade& t) {
            static_cast<TradesWidget*>(ptr)->handle_trade(t);
        }
    };
    subscribed_streams_ = &ctx_.stream_mgr();
    subscribed_streams_->subscribe_trades(stream_key_, handler);
}

TradesWidget::~TradesWidget() {
    if (subscribed_streams_) subscribed_streams_->unsubscribe_trades(stream_key_, this);
}

void TradesWidget::refresh_instrument() {
    const PriceFormatter f =
        SymbolRegistry::instance().get_formatter(pair_.exchange, pair_.symbol);
    if (!f.resolved) return;
    if (f.price_precision == fmt_.price_precision &&
        f.qty_precision   == fmt_.qty_precision &&
        fmt_.resolved) return;
    fmt_ = f;
    // The tape holds the raw trades, so the visible rows can be re-printed at
    // the real precision instead of being thrown away.
    const size_t count = std::min(trade_count_, MAX_TRADES);
    for (size_t i = 0; i < count; ++i) {
        RowText& txt = row_text_[i];
        snprintf(txt.price, sizeof(txt.price), fmt_.price_fmt, trades_[i].price);
        snprintf(txt.qty,   sizeof(txt.qty),   fmt_.qty_fmt,   trades_[i].qty);
    }
}

// For data-driven events.
void TradesWidget::handle_trade(const Terminal::Trade& trade) {
    // Provisional precision until the registry answers: the tape is the one
    // widget that always has a price in hand, and printing every print of a
    // sub-cent perp as "0.24" is worse than a magnitude-derived guess.
    if (!fmt_.resolved && trade.price > 0.0) {
        fmt_ = PriceFormatter::provisional_for_price(trade.price);
    }
    const size_t slot = trade_count_ % MAX_TRADES;
    trades_[slot] = trade;
    // Format ONCE at insert - trades are immutable (see RowText).
    RowText& txt = row_text_[slot];
    snprintf(txt.price, sizeof(txt.price), fmt_.price_fmt, trade.price);
    snprintf(txt.qty, sizeof(txt.qty), fmt_.qty_fmt, trade.qty);
    {
        const double v = trade.qty * trade.price, a = std::abs(v);
        if (a >= 1e9)      snprintf(txt.usd, sizeof(txt.usd), "%.2fB", v / 1e9);
        else if (a >= 1e6) snprintf(txt.usd, sizeof(txt.usd), "%.2fM", v / 1e6);
        else if (a >= 1e3) snprintf(txt.usd, sizeof(txt.usd), "%.1fK", v / 1e3);
        else               snprintf(txt.usd, sizeof(txt.usd), "%.0f", v);
    }
    format_trade_time(slot);
    trade_flash_times_[slot] = static_cast<float>(ImGui::GetTime());
    formatted_time_zone_generation_ = DisplayTimeZone::instance().generation();
    trade_count_++;
    // Rolling qty EMA for "big print" detection (slow alpha - stable baseline)
    qty_ema_ = (qty_ema_ <= 0.0) ? trade.qty : qty_ema_ * 0.98 + trade.qty * 0.02;
}

void TradesWidget::update() {  // Calculate statistics if enabled
    auto& time_zone = DisplayTimeZone::instance();
    if (formatted_time_zone_generation_ != time_zone.generation()) {
        const size_t count = std::min(trade_count_, MAX_TRADES);
        for (size_t i = 0; i < count; ++i) {
            const size_t slot = (trade_count_ - 1 - i) % MAX_TRADES;
            format_trade_time(slot);
        }
        formatted_time_zone_generation_ = time_zone.generation();
    }
    // Track new trades (newest rendered at top, no scroll needed)
    if (trade_count_ > last_rendered_count_) {
        last_rendered_count_ = trade_count_;
    }
    // Over the buffered prints (at most 64): the size bars' scale and the buy/sell pressure.
    max_notional_ = 0.0; buy_notional_ = 0.0; sell_notional_ = 0.0;
    const size_t count = std::min(trade_count_, MAX_TRADES);
    for (size_t i = 0; i < count; ++i) {
        const auto& t = trades_[(trade_count_ - 1 - i) % MAX_TRADES];
        const double v = t.qty * t.price;
        max_notional_ = std::max(max_notional_, v);
        (t.is_buy ? buy_notional_ : sell_notional_) += v;
    }
}

void TradesWidget::format_trade_time(size_t slot) {
    if (!DisplayTimeZone::instance().format(trades_[slot].timestamp_ms,
                                             TimeZoneFormat::TimeSeconds,
                                             row_text_[slot].time,
                                             sizeof(row_text_[slot].time))) {
        snprintf(row_text_[slot].time, sizeof(row_text_[slot].time), "--:--:--");
    }
}

void TradesWidget::calculate_statistics() {
    float current_time = ImGui::GetTime();
    constexpr float WINDOW_SECONDS = 60.0f;
    volume_buy_1m_ = 0.0;
    volume_sell_1m_ = 0.0;
    size_t recent_trades = 0;
    size_t count_to_check = std::min(trade_count_, MAX_TRADES);
    for (size_t i = 0; i < count_to_check; ++i) {
        size_t idx = (trade_count_ - 1 - i) % MAX_TRADES;
        const auto& trade = trades_[idx];
        // Check if trade is within time window
        float trade_time = static_cast<float>(trade.timestamp_ms) / 1000.0f;
        float age = current_time - trade_time;
        if (age > WINDOW_SECONDS) break;
        recent_trades++;
        if (trade.is_buy) {
            volume_buy_1m_ += trade.qty;
        } else {
            volume_sell_1m_ += trade.qty;
        }
    }
    // Calculate buy pressure (0.0 = all sells, 1.0 = all buys)
    double total_volume = volume_buy_1m_ + volume_sell_1m_;
    if (total_volume > 0.0) {
        buy_pressure_ = static_cast<float>(volume_buy_1m_ / total_volume);
    } else {
        buy_pressure_ = 0.5f;
    }
    // Trades per second
    trades_per_second_ = static_cast<size_t>(recent_trades / WINDOW_SECONDS);
}

void TradesWidget::render_header() {
    using namespace Theme;
    using drawing::UiIcon;
    // A slim toolbar (design v3, 2026-10-11): the buy/sell pressure over the buffered prints on the left; units
    // and settings on the right.
    const float K = ImGui::GetFontSize() / 18.4f;
    const float pad = 10.0f * K, h = 26.0f * K, top = 6.0f * K, ib = 26.0f * K;
    const float ww = ImGui::GetWindowWidth();
    const ImVec2 origin = ImGui::GetCursorPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    push_font(Fonts::ui(), 13.5f * K);
    static const char* const kUnits[] = {"Coin", "USD"};
    const float seg_w = segmented_width(kUnits, 2, h);
    const float right_x = ww - pad - ib - 4.0f * K - seg_w;
    const double total = buy_notional_ + sell_notional_;
    if (show_stats_ && total > 0.0) {
        const float share = static_cast<float>(buy_notional_ / total);
        char a[16], b[16];
        snprintf(a, sizeof(a), "%.0f%%", share * 100.0f);
        snprintf(b, sizeof(b), "%.0f%%", (1.0f - share) * 100.0f);
        const ImVec2 p = ImGui::GetWindowPos();
        const float y = p.y + origin.y + top + h * 0.5f;
        const float x = p.x + origin.x + pad;
        const float aw = ImGui::CalcTextSize(a).x, bw = ImGui::CalcTextSize(b).x;
        const float bar_w = std::clamp(right_x - pad - (aw + bw + 20.0f * K) - 12.0f * K, 0.0f, 160.0f * K);
        if (bar_w > 30.0f * K) {
            const float ty = y - ImGui::GetFontSize() * 0.55f;
            dl->AddText(ImVec2(x, ty), u32(Tokens::UP), a);
            const float bx = x + aw + 8.0f * K, bh = 6.0f * K;
            const float split = std::round(bx + bar_w * share);
            dl->AddRectFilled(ImVec2(bx, y - bh * 0.5f), ImVec2(bx + bar_w, y + bh * 0.5f), u32(Tokens::DOWN, 0.85f), bh * 0.5f);
            dl->AddRectFilled(ImVec2(bx, y - bh * 0.5f), ImVec2(split, y + bh * 0.5f), u32(Tokens::UP, 0.85f), bh * 0.5f,
                              split >= bx + bar_w - 1.0f ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft);
            dl->AddText(ImVec2(bx + bar_w + 8.0f * K, ty), u32(Tokens::DOWN), b);
            ImGui::SetCursorPos(ImVec2(origin.x + pad, origin.y + top));
            ImGui::Dummy(ImVec2(aw + bar_w + bw + 16.0f * K, h));
            if (ImGui::IsItemHovered())
                tooltip("Bought vs sold, by quote value, over the last %zu prints", std::min(trade_count_, MAX_TRADES));
        }
    }
    ImGui::SetCursorPos(ImVec2(right_x, origin.y + top));
    const int units = segmented("tape_units", kUnits, 2, display_usd_ ? 1 : 0, h);
    display_usd_ = units == 1;
    ImGui::SameLine(0.0f, 4.0f * K);
    if (icon_button("##tape_settings_btn", UiIcon::Gear, "Settings", ib)) ImGui::OpenPopup("##tape_settings");
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(origin.x, origin.y + top + h + top));
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(p.x, p.y - 0.5f), ImVec2(p.x + ww, p.y - 0.5f), u32(Tokens::BD1));
    }
    if (begin_menu("##tape_settings", 250.0f)) {
        if (menu_toggle("Buy / sell pressure", show_stats_)) show_stats_ = !show_stats_;
        if (menu_toggle("Highlight large prints", highlight_large_, false, "8\xc3\x97 typical")) highlight_large_ = !highlight_large_;
        bool pinned = !mode_companion;
        if (menu_toggle("Keep visible in real-time", pinned)) mode_companion = pinned;
        ImGui::EndPopup();
    }
}

void TradesWidget::render_table() {
    // A tape that has never printed looks identical to a broken app: column
    // headers over nothing, with no clue whose fault it is. The terminal
    // already knows the difference, and knows it about five seconds before the
    // gateway logs its own warning. Four other surfaces already say so from the
    // same detector; say it here too, because this is the panel a stranger
    // stares at first. Keyed on frame absence rather than entitlements, since
    // bare mode defaults to Pro and cannot tell the feeds apart.
    //
    // Replaces the table rather than adding a row to it: the note needs the
    // panel width, and inside the table it would wrap to the PRICE column and
    // be clipped at that column's edge. Column headers over nothing carry no
    // information anyway, and the moment one trade arrives absent() goes false
    // and the table comes back.
    if (trade_count_ == 0 &&
        StreamPresence::instance().absent(static_cast<uint32_t>(Terminal::Stream::Trades))) {
        const float ww = ImGui::GetContentRegionAvail().x;
        const float pad = 10.0f;
        ImGui::PushFont(Theme::Fonts::label());
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Tokens::TX3);
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + pad, ImGui::GetCursorPosY() + pad));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ww - pad * 2.0f);
        ImGui::TextUnformatted(
            "No trades received. The feed is connected but its trade stream has "
            "not delivered anything, which some networks block. The tape is "
            "waiting, not broken.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        return;
    }

    // Mono numerics in 20 px rows under a quiet sentence-case header.
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const float row_h = 20.0f;
    const float pad_y = std::max(0.0f, (row_h - ImGui::GetFontSize()) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(10.0f, pad_y));
    ImGui::PushStyleColor(ImGuiCol_TableRowBg, ImVec4(0, 0, 0, 0));

    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("TradesTable", 3, flags)) {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::PopFont();
        return;
    }

    ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Size",  ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("Time",  ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00:00:00").x + 4.0f);
    ImGui::TableSetupScrollFreeze(0, 1);

    {
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers, 24.0f);
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, Theme::u32(Theme::Tokens::PANEL));
        Theme::push_font(Theme::Fonts::ui(), 12.5f);
        float x0 = 0.0f, x1 = 0.0f, yb = 0.0f;
        auto head = [&](int col, const char* t, bool right) {
            ImGui::TableSetColumnIndex(col);
            const ImVec2 cur = ImGui::GetCursorScreenPos();
            if (col == 0) { x0 = cur.x - 10.0f; yb = cur.y - pad_y + 24.0f; }
            x1 = cur.x + ImGui::GetContentRegionAvail().x + 10.0f;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f);
            if (right) {
                const float w = ImGui::GetContentRegionAvail().x;
                const float tw = ImGui::CalcTextSize(t).x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w > tw ? w - tw : 0.0f));
            }
            ImGui::TextColored(Theme::Tokens::TX3, "%s", t);
        };
        head(0, "Price", false);
        head(1, display_usd_ ? "Size (USD)" : "Size", true);
        head(2, "Time", true);
        ImGui::PopFont();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(x0, yb - 0.5f), ImVec2(x1, yb - 0.5f), Theme::u32(Theme::Tokens::BD1));
    }

    // Render trades (most recent first)
    const size_t display_count = std::min(trade_count_, MAX_TRADES);
    for (size_t i = 0; i < display_count; ++i) {
        const size_t idx = (trade_count_ - 1 - i) % MAX_TRADES;
        render_trade_row(trades_[idx], row_text_[idx], idx);
    }

    ImGui::EndTable();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::PopFont();
}

void TradesWidget::render_trade_row(const Terminal::Trade& trade, const RowText& txt, size_t slot) {
    using namespace Theme;
    const float row_h = 20.0f;
    ImGui::TableNextRow(ImGuiTableRowFlags_None, row_h);
    const ImVec4& side = trade.is_buy ? Tokens::UP : Tokens::DOWN;
    const bool large = highlight_large_ && qty_ema_ > 0.0 && trade.qty > qty_ema_ * 8.0;

    // A new print flashes its side colour briefly; a large one keeps a soft wash.
    const float age = static_cast<float>(ImGui::GetTime()) - trade_flash_times_[slot];
    const float flash = age >= 0.0f && age < 0.35f ? (1.0f - age / 0.35f) * 0.12f : 0.0f;
    const float wash = std::max(flash, large ? 0.09f : 0.0f);
    if (wash > 0.0f) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, u32(side, wash));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::TableSetColumnIndex(0);
    {
        const ImVec2 cur = ImGui::GetCursorScreenPos();
        const float pad_y = (row_h - ImGui::GetFontSize()) * 0.5f;
        const float y0 = cur.y - pad_y;
        // Price: side-coloured; a large print gets a side bar at the row's left edge.
        if (large) dl->AddRectFilled(ImVec2(cur.x - 10.0f, y0 + 2.0f), ImVec2(cur.x - 8.0f, y0 + row_h - 2.0f), u32(side), 1.0f);
        ImGui::TextColored(side, "%s", txt.price);
    }

    // Size: a side-coloured bar from the column's right edge that always holds the whole number and runs on by
    // quote value (square root, so ordinary prints still show beside a block); large prints read bright.
    ImGui::TableSetColumnIndex(1);
    {
        const char* size = display_usd_ ? txt.usd : txt.qty;
        const ImVec2 cur = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float tw = ImGui::CalcTextSize(size).x;
        const float pad_y = (row_h - ImGui::GetFontSize()) * 0.5f, inset = 5.0f;
        if (max_notional_ > 0.0) {
            const float frac = static_cast<float>(std::sqrt(std::min(1.0, trade.qty * trade.price / max_notional_)));
            const float bw = std::min(w, std::max(tw + inset * 2.0f, std::round(frac * w)));
            dl->AddRectFilled(ImVec2(cur.x + w - bw, cur.y - pad_y + 2.0f), ImVec2(cur.x + w, cur.y - pad_y + row_h - 2.0f),
                              u32(side, large ? 0.30f : 0.10f + 0.12f * frac), 3.0f);
        }
        dl->AddText(ImVec2(std::round(cur.x + w - inset - tw), cur.y), u32(large ? Tokens::TX1 : Tokens::TX2), size);
    }

    // Time: quiet, right.
    ImGui::TableSetColumnIndex(2);
    {
        const float w = ImGui::GetContentRegionAvail().x;
        const float tw = ImGui::CalcTextSize(txt.time).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w > tw ? w - tw : 0.0f));
        ImGui::TextColored(Tokens::TX3, "%s", txt.time);
    }
}

const char* TradesWidget::format_timestamp(int64_t unix_ms) const {
    static char buf[16];
    DisplayTimeZone::instance().format(unix_ms, TimeZoneFormat::TimeSeconds,
                                       buf, sizeof(buf));
    return buf;
}

void TradesWidget::render() {
    if (!is_open) return;
    // Use title_suffix_ for ImGui ID uniqueness (e.g. "##replay" during replay)
    std::string window_title = title_suffix_.empty() ? title_ : title_ + title_suffix_;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    place_new_window();
    if (!ImGui::Begin(window_title.c_str(), &is_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::PopStyleVar();
        ImGui::End();
        return;
    }
    ImGui::PopStyleVar();
    render_header();
    render_table();
    ImGui::End();
}
