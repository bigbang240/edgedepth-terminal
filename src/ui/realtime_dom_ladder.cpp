#include "core/i18n.h"
#include "ui/realtime_dom_ladder.h"
#include "core/trade_at_price.h"
#include "core/refill_levels.h"
#include "core/ui_test_marks.h"
#include "rendering/theme.h"
#include "types/frame_profiler.h"
#include "ui/drawing/drawing_icons.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
void ladder_quantity(double value, bool signed_value, char* out, size_t capacity, float width) {
    const double v = std::abs(value);
    const double scale = v >= 1e9 ? 1e9 : v >= 1e6 ? 1e6 : v >= 1e3 ? 1e3 : 1;
    const char* suffix = scale == 1e9 ? "B" : scale == 1e6 ? "M" : scale == 1e3 ? "K" : "";
    const char* sign = signed_value ? (value >= 0 ? "+" : "-") : "";
    if (scale == 1 && v < 1) snprintf(out, capacity, "%s%.2g", sign, v);
    else for (int decimals = 1; decimals >= 0; --decimals) {
        snprintf(out, capacity, "%s%.*f%s", sign, decimals, v / scale, suffix);
        if (ImGui::CalcTextSize(out).x <= width) break;
    }
}

// The first character of `row` that differs from `current`, keeping at least the last two digits bright: the
// part of a row's price the eye reads (the DOM widget's rule).
size_t bright_from(const char* row, const char* current) {
    const size_t n = std::strlen(row);
    if (n != std::strlen(current)) return 0;
    size_t k = 0;
    while (k < n && row[k] == current[k]) ++k;
    return std::min(k, n >= 2 ? n - 2 : size_t{0});
}

constexpr float kHeaderPx = 13.0f;   // Inter, as the DOM widget's toolbar and headers
}

float RealtimeDomLadder::preferred_width() {
    return ImGui::CalcTextSize("000000.000").x + 4 * ImGui::CalcTextSize("00000.0").x + 64;
}

// Absolute screen Y is intentional: the header takes the top of the panel,
// and rows are clipped to the part of the shared price grid below it. Never
// recenter or scale rows independently of the chart.
void RealtimeDomLadder::render(const RealtimeDOMFrame& frame, const PriceFormatter& fmt, bool* open) {
    ProfileScope profile("RT DOM");
    using namespace Theme;
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float right = org.x + avail.x;
    const bool live = frame.projected(ImGui::GetFrameCount());
    const bool recovering = live && frame.recovery_visible();
    const bool ready = live && (frame.fresh() || recovering);
    // Every exit leaves the cursor where the panel started and claims the panel, as one item.
    const auto finish = [&] { ImGui::SetCursorScreenPos(org); ImGui::Dummy(avail); };

    // ── Toolbar: status, units, delta since reset, spread and close ──
    constexpr float bar_h = 22.0f, bar_top = 4.0f;
    push_font(Fonts::ui(), kHeaderPx);
    const float mid_y = org.y + bar_top + bar_h * 0.5f;
    const ImVec4& dot = !ready || recovering ? Tokens::WARN : frame.paused ? Tokens::TX3 : frame.replay ? Tokens::WARN : Tokens::UP;
    dl->AddCircleFilled(ImVec2(org.x + 8.0f, mid_y), 3.0f, u32(dot), 12);
    const char* status = frame.replay ? (frame.paused ? "Replay, paused" : "Replay")
        : recovering ? "Recovering" : frame.paused ? "Paused" : "Live";
    const ImVec2 status_size = ImGui::CalcTextSize(status);
    dl->AddText(ImVec2(org.x + 16.0f, std::round(mid_y - status_size.y * 0.5f)), u32(recovering ? Tokens::WARN : Tokens::TX2), status);
    if (recovering && ImGui::IsMouseHoveringRect(ImVec2(org.x, org.y), ImVec2(org.x + 16.0f + status_size.x, org.y + bar_top + bar_h)))
        tooltip("Showing the last confirmed book, %.1fs old, until fresh depth arrives.",
                double(frame.clock_ms - frame.book->timestamp_ms) / 1000);
    float x = org.x + 16.0f + status_size.x + 10.0f;
    static const char* const kUnits[] = {"Coin", "USD"};
    ImGui::SetCursorScreenPos(ImVec2(x, org.y + bar_top));
    display_usd_ = segmented("rt_dom_units", kUnits, 2, display_usd_ ? 1 : 0, bar_h) == 1;
    if (ImGui::IsItemHovered()) tooltip("Resting size and traded volume in coins or in dollars");
    x += segmented_width(kUnits, 2, bar_h) + 10.0f;
    float limit = right - (open ? bar_h + 4.0f : 0.0f);
    if (open) {
        ImGui::SetCursorScreenPos(ImVec2(right - bar_h, org.y + bar_top));
        if (icon_button("##rt_dom_close", drawing::UiIcon::Close, "Hide the depth ladder (DOM in the chart toolbar shows it again)", bar_h))
            *open = false;
        ui_test::mark("rt.dom.close");
    }
    if (ready) {
        const double tick = frame.native_tick > 0 ? frame.native_tick : 0;
        const long ticks = tick > 0 ? std::lround((frame.ask() - frame.bid()) / tick) : 0;
        char spread[32];
        if (ticks >= 1 && ticks <= 999) snprintf(spread, sizeof(spread), "%ld tick%s", ticks, ticks == 1 ? "" : "s");
        else snprintf(spread, sizeof(spread), "-");
        const float sw = ImGui::CalcTextSize(spread).x;
        if (limit - sw - 6.0f > x) {
            const ImVec2 at(limit - sw - 6.0f, std::round(mid_y - ImGui::GetFontSize() * 0.5f));
            dl->AddText(at, u32(Tokens::TX3), spread);
            if (ImGui::IsMouseHoveringRect(at, ImVec2(at.x + sw, at.y + ImGui::GetFontSize()))) {
                char bid[32], ask[32];
                snprintf(bid, sizeof(bid), fmt.price_fmt, frame.bid());
                snprintf(ask, sizeof(ask), fmt.price_fmt, frame.ask());
                const double mid = (frame.bid() + frame.ask()) * 0.5;
                tooltip("Spread: best bid %s, best ask %s\n%.2f bp of the mid", bid, ask, mid > 0 ? (frame.ask() - frame.bid()) / mid * 1e4 : 0.0);
            }
            limit = at.x - 10.0f;
        }
        if (frame.flow) {
            // Buy minus sell quantity since the last reset. It stays in base units: a session total times today's
            // price would misstate what actually traded.
            char value[24], label[40];
            const double delta = frame.flow->total_delta();
            ladder_quantity(delta, true, value, sizeof(value), 80);
            snprintf(label, sizeof(label), "CVD %s", value);
            const float cw = ImGui::CalcTextSize(label).x;
            if (x + cw <= limit) {
                const ImVec2 at(x, std::round(mid_y - ImGui::GetFontSize() * 0.5f));
                dl->AddText(at, u32(Tokens::TX3), "CVD");
                dl->AddText(ImVec2(at.x + ImGui::CalcTextSize("CVD ").x, at.y), u32(delta >= 0 ? Tokens::UP : Tokens::DOWN), value);
                if (ImGui::IsMouseHoveringRect(at, ImVec2(at.x + cw, at.y + ImGui::GetFontSize())))
                    tooltip("Buy minus sell quantity at the chart clock, reset every 5 minutes of market time%s.",
                            frame.flow->reset_after_gap() ? "; reset after a gap in received trades" : "");
            }
        }
    }

    // ── Column headings: quiet sentence case over a hairline ──
    const float head_y = org.y + bar_top + bar_h + 6.0f;
    const float header_h = head_y + ImGui::GetFontSize() + 5.0f - org.y;
    ImGui::PopFont();
    if (!live) {
        push_font(Fonts::ui(), kHeaderPx);
        dl->AddText(ImVec2(org.x + 8.0f, org.y + header_h), u32(Tokens::TX2), "Waiting for real-time depth");
        ImGui::PopFont();
        finish();
        return;
    }
    if (!ready) {
        push_font(Fonts::ui(), kHeaderPx);
        dl->AddText(ImVec2(org.x + 8.0f, org.y + header_h), u32(Tokens::TX2),
                    frame.replay ? "Waiting for recorded depth" : "Waiting for synchronized depth");
        ImGui::PopFont();
        finish();
        return;
    }
    if (frame.native_tick <= 0 || frame.bucket_ticks < 1) { finish(); return; }
    const auto& book = *frame.book;
    // Row centres: on the tick grid for an even number of ticks per row, half a tick past it for an odd number.
    const auto center_fmt = PriceFormatter::from_tick_and_step(frame.native_tick * (frame.bucket_ticks % 2 ? 0.1 : 1.0), 1);
    char widest[32];
    center_fmt.format_price(widest, sizeof(widest), frame.price_max);
    const float price_w = ImGui::CalcTextSize(widest).x + 14.0f;
    // A mirrored book: traded volume outside, resting depth beside the price, so sells sit next to the bids they
    // hit and buys next to the asks they lifted.
    const float numbers_w = std::max(4.0f, avail.x - price_w);
    const float flow_w = numbers_w * 0.2f, depth_w = numbers_w * 0.3f;
    const float edges[] = {org.x, org.x + flow_w, org.x + flow_w + depth_w, org.x + flow_w + depth_w + price_w,
        org.x + flow_w + depth_w * 2 + price_w, right};
    {
        push_font(Fonts::ui(), kHeaderPx);
        const char* const names[] = {i18n::tr("Sold", i18n::fa_sold), i18n::tr("Bids", i18n::fa_bids), i18n::tr("Price", i18n::fa_price), i18n::tr("Asks", i18n::fa_asks), i18n::tr("Bought", i18n::fa_bought)};
        for (int c = 0; c < 5; ++c) {
            const float tw = ImGui::CalcTextSize(names[c]).x;
            // Each heading sits where its numbers do: at the edge nearest the price.
            const float hx = c == 2 ? (edges[2] + edges[3] - tw) * 0.5f : c < 2 ? edges[c + 1] - tw - 5.0f : edges[c] + 5.0f;
            dl->AddText(ImVec2(std::round(hx), head_y), u32(Tokens::TX3), names[c]);
        }
        if (ImGui::IsMouseHoveringRect(ImVec2(edges[2], head_y), ImVec2(edges[3], head_y + ImGui::GetFontSize())))
            tooltip("Rows of %d tick%s%s, the heatmap's rows. Each price is its row's centre;\nthe quotes are marked at their exact price.",
                    frame.bucket_ticks, frame.bucket_ticks == 1 ? "" : "s", frame.automatic_grouping ? " (automatic)" : "");
        if (ImGui::IsMouseHoveringRect(ImVec2(edges[0], head_y), ImVec2(edges[1], head_y + ImGui::GetFontSize())) ||
            ImGui::IsMouseHoveringRect(ImVec2(edges[4], head_y), ImVec2(edges[5], head_y + ImGui::GetFontSize())))
            tooltip("Volume traded at each row since the last reset (every 5 minutes of market time):\nsold into the bids on the left, bought from the asks on the right.");
        ImGui::PopFont();
        dl->AddLine(ImVec2(org.x, org.y + header_h - 0.5f), ImVec2(right, org.y + header_h - 0.5f), u32(Tokens::BD1));
    }

    const float text_h = ImGui::GetFontSize();
    float top = std::max(org.y + header_h + 1.0f, frame.top);
    float bottom = std::min(org.y + avail.y, frame.bottom);
    if (bottom <= top || avail.x <= 0) { finish(); return; }
    const double step = frame.bucket_size();
    // Keep the viewport edges on complete bands: no half-cut numeric rows
    // under the header or at the bottom of the panel.
    const auto price_at_y = [&](float y) {
        return frame.price_min + (frame.bottom - y) / (frame.bottom - frame.top) *
            (frame.price_max - frame.price_min);
    };
    top = frame.price_y(std::floor(price_at_y(top) / step + 1e-7) * step);
    bottom = frame.price_y(std::ceil(price_at_y(bottom) / step - 1e-7) * step);
    if (bottom <= top) { finish(); return; }
    const int64_t first_bucket = frame.bucket_index(frame.price_min);
    const int64_t last_bucket = frame.bucket_index(frame.price_max);
    if (last_bucket - first_bucket > 16384) {
        push_font(Fonts::ui(), kHeaderPx);
        dl->AddText(ImVec2(org.x + 8.0f, top + 4.0f), u32(Tokens::TX2), "Zoom the price axis in to show the rows");
        ImGui::PopFont();
        finish();
        return;
    }
    const size_t count = size_t(last_bucket - first_bucket + 1);
    rows_.assign(count, {});
    const auto row_index = [&](double price) { return frame.bucket_index(price) - first_bucket; };
    const auto value = [&](double qty, double price) { return display_usd_ ? qty * price : qty; };
    for (const auto& level : book.levels) {
        const auto idx = row_index(level.price);
        if (idx < 0 || size_t(idx) >= count) continue;
        auto& row = rows_[size_t(idx)];
        (level.price <= book.bid ? row.bid : row.ask) += value(level.size, level.price);
    }
    if (frame.flow) for (const auto& [price, level] : frame.flow->levels()) {
        const auto idx = row_index(price);
        if (idx < 0 || size_t(idx) >= count) continue;
        auto& row = rows_[size_t(idx)];
        row.buy += value(level.buy_volume, price);
        row.sell += value(level.sell_volume, price);
    }
    // Bars against the book on screen: the 90th percentile row reaches 60% of a cell, so ordinary rows keep a
    // readable shape and walls run to the edge instead of shrinking everything else to a sliver.
    double max_flow = 0;
    depth_sizes_.clear();
    for (size_t i = 0; i < count; ++i) {
        const float y = frame.price_y(frame.bucket_center(first_bucket + int64_t(i)));
        if (y < top || y > bottom) continue;
        if (rows_[i].bid > 0) depth_sizes_.push_back(rows_[i].bid);
        if (rows_[i].ask > 0) depth_sizes_.push_back(rows_[i].ask);
        max_flow = std::max({max_flow, rows_[i].buy, rows_[i].sell});
    }
    double depth_ref = 0;
    if (!depth_sizes_.empty()) {
        const auto at = depth_sizes_.begin() + std::ptrdiff_t((depth_sizes_.size() - 1) * 9 / 10);
        std::nth_element(depth_sizes_.begin(), at, depth_sizes_.end());
        depth_ref = *at / 0.36;   // sqrt(0.36) = 0.6
    }
    const float row_h = float(step / (frame.price_max - frame.price_min) * (frame.bottom - frame.top));
    const int64_t mid_row = row_index((frame.bid() + frame.ask()) * 0.5);
    const int64_t bid_row = row_index(frame.bid()), ask_row = row_index(frame.ask());
    char current[32];
    center_fmt.format_price(current, sizeof(current), frame.bucket_center(first_bucket + mid_row));
    dl->PushClipRect(ImVec2(org.x, top), ImVec2(right, bottom), true);
    // Row backgrounds: the mid and refill levels' price cells.
    for (size_t i = 0; i < count; ++i) {
        const float y = frame.price_y(frame.bucket_center(first_bucket + int64_t(i)));
        const float y0 = y - row_h * 0.5f, y1 = y + row_h * 0.5f;
        if (y1 < top || y0 > bottom) continue;
        if (int64_t(i) == mid_row) dl->AddRectFilled(ImVec2(org.x, y0), ImVec2(right, y1), u32(Tokens::ELEV));
    }
    if (frame.refill) for (int k = 0; k < frame.refill->count; ++k) {
        const int64_t idx = row_index(frame.refill->marks[size_t(k)].price);
        if (idx < 0 || size_t(idx) >= count) continue;
        const float y = frame.price_y(frame.bucket_center(first_bucket + idx));
        dl->AddRectFilled(ImVec2(edges[2], y - row_h * 0.5f), ImVec2(edges[3], y + row_h * 0.5f), u32(Tokens::TX1, 0.08f));
    }
    // A bar from the cell's inner edge (toward the price) outward; its number at the inner edge, over the bar.
    const auto bar = [&](int c, float y, double v, double reference, const ImVec4& color, float alpha_low, float alpha_high,
                         const ImVec4& ink, char* label) {
        const float cell = edges[c + 1] - edges[c] - 2.0f;
        const float frac = reference > 0 ? float(std::min(1.0, std::sqrt(v / reference))) : 0.0f;
        const float w = std::max(2.0f, std::round(frac * cell));
        const bool from_right = c < 2;   // left of the price, bars grow leftward
        const float y0 = std::round(y - row_h * 0.5f) + 1.0f, y1 = std::round(y + row_h * 0.5f) - 1.0f;
        const float x0 = from_right ? edges[c + 1] - 1.0f - w : edges[c] + 1.0f;
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y1), u32(color, alpha_low + (alpha_high - alpha_low) * frac), 2.0f);
        const float tw = ImGui::CalcTextSize(label).x;
        const float tx = from_right ? edges[c + 1] - 5.0f - tw : edges[c] + 5.0f;
        dl->AddText(ImVec2(std::round(tx), std::round(y - text_h * 0.5f)), u32(ink), label);
        return frac;
    };
    for (int c = 0; c < 5; ++c) {
        dl->PushClipRect(ImVec2(edges[c], top), ImVec2(edges[c + 1], bottom), true);
        for (size_t i = 0; i < count; ++i) {
            const double price = frame.bucket_center(first_bucket + int64_t(i));
            const float y = frame.price_y(price);
            if (y + row_h * 0.5f < top || y - row_h * 0.5f > bottom) continue;
            const auto& row = rows_[i];
            char label[32];
            if (c == 2) {
                // Price: the digits that differ from the mid's row bright, the rest dim; the best bid and best ask
                // rows in their side colour.
                center_fmt.format_price(label, sizeof(label), price);
                const size_t split = bright_from(label, current);
                const float tw = ImGui::CalcTextSize(label).x, pw = ImGui::CalcTextSize(label, label + split).x;
                const float lx = std::round((edges[2] + edges[3] - tw) * 0.5f), text_y = std::round(y - text_h * 0.5f);
                const ImVec4& bright = int64_t(i) == bid_row ? Tokens::UP : int64_t(i) == ask_row ? Tokens::DOWN : Tokens::TX1;
                dl->AddText(ImVec2(lx, text_y), u32(Tokens::TX3), label, label + split);
                dl->AddText(ImVec2(lx + pw, text_y), u32(bright), label + split);
                continue;
            }
            if (c == 1 || c == 3) {
                // Resting depth, on the heatmap's own blue-to-green ramp.
                const double q = c == 1 ? row.bid : row.ask;
                if (!(q > 0)) continue;
                ladder_quantity(q, false, label, sizeof(label), edges[c + 1] - edges[c] - 10);
                const float frac = depth_ref > 0 ? float(std::min(1.0, std::sqrt(q / depth_ref))) : 0.0f;
                bar(c, y, q, depth_ref, ocean(0.30f + 0.55f * frac), 0.35f, 0.8f, frac >= 0.45f ? Tokens::TX1 : Tokens::TX2, label);
                continue;
            }
            // Traded since the reset: sold (red) beside the bids, bought (green) beside the asks.
            const double v = c == 0 ? row.sell : row.buy;
            if (!(v > 0)) continue;
            ladder_quantity(v, false, label, sizeof(label), edges[c + 1] - edges[c] - 10);
            const ImVec4& side = c == 0 ? Tokens::DOWN : Tokens::UP;
            bar(c, y, v, max_flow, side, 0.14f, 0.32f, side, label);
        }
        dl->PopClipRect();
    }
    // Walls the chart marks at its right edge: a side-coloured bar at the outer end of their depth cell.
    if (frame.walls) for (int k = 0; k < frame.walls->count; ++k) {
        const auto& w = frame.walls->marks[size_t(k)];
        const int64_t idx = row_index(w.price);
        if (idx < 0 || size_t(idx) >= count) continue;
        const float y = frame.price_y(frame.bucket_center(first_bucket + idx));
        const float x0 = w.bid ? edges[1] + 1.0f : edges[4] - 4.0f;
        dl->AddRectFilled(ImVec2(x0, y - row_h * 0.5f + 1.0f), ImVec2(x0 + 3.0f, y + row_h * 0.5f - 1.0f),
                          u32(w.bid ? Tokens::UP : Tokens::DOWN), 1.0f);
    }
    // Holding refill levels (the chart's Refill levels layer): a near-white bar at the price side of their depth cell,
    // as the chart marks them.
    if (frame.refill) {
        const ImU32 ink = u32(Tokens::TX1);
        for (int k = 0; k < frame.refill->count; ++k) {
            const auto& mark = frame.refill->marks[size_t(k)];
            const int64_t idx = row_index(mark.price);
            if (idx < 0 || size_t(idx) >= count) continue;
            const float y = frame.price_y(frame.bucket_center(first_bucket + idx));
            if (y + row_h * 0.5f < top || y - row_h * 0.5f > bottom) continue;
            const float half = std::clamp(row_h * 0.5f - 1.0f, 1.5f, 6.0f);
            const float bx = mark.bid ? edges[2] - 2.0f : edges[3];
            dl->AddRectFilled(ImVec2(bx, y - half), ImVec2(bx + 2.0f, y + half), ink);
            if (ui_test::enabled()) ui_test::mark_rect("rt.ladder.refill." + std::to_string(k), ImVec2(bx, y - half), ImVec2(bx + 2.0f, y + half));
        }
    }
    // The exact quotes: a small side-coloured triangle at each edge of the price column, pointing in.
    for (int side = 0; side < 2; ++side) {
        const float y = std::round(frame.price_y(side ? frame.ask() : frame.bid())) + 0.5f;
        const ImU32 color = u32(side ? Tokens::DOWN : Tokens::UP);
        if (side) dl->AddTriangleFilled(ImVec2(edges[3], y - 3.5f), ImVec2(edges[3], y + 3.5f), ImVec2(edges[3] - 4.0f, y), color);
        else dl->AddTriangleFilled(ImVec2(edges[2], y - 3.5f), ImVec2(edges[2], y + 3.5f), ImVec2(edges[2] + 4.0f, y), color);
    }
    dl->PopClipRect();
    finish();
}
