#include "ui/chart_widget.h"
#include "core/footprint_manager.h"
#include "replayer/replay_manager.h"
#include "rendering/theme.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <ranges>

// ═══════════════════════════════════════════════════════════════════════════════
// Footprint Chart Overlay
// ═══════════════════════════════════════════════════════════════════════════════

// Format volume for footprint cells: compact K/M notation
static void format_fp_volume(char* buf, size_t buf_size, double vol) {
    double abs_vol = std::abs(vol);
    if (abs_vol >= 1000000.0) {
        snprintf(buf, buf_size, "%.1fM", vol / 1000000.0);
    } else if (abs_vol >= 1000.0) {
        snprintf(buf, buf_size, "%.0fK", vol / 1000.0);
    } else if (abs_vol >= 1.0) {
        snprintf(buf, buf_size, "%.0f", vol);
    } else {
        snprintf(buf, buf_size, "%.1f", vol);
    }
}

// Shared by cluster and profile: default same-price outlines are unchanged.
static void draw_fp_imbalance(ImDrawList* draw, const FootprintManager& manager,
    const FootprintManager::GroupedLevel& row, float left, float right,
    float top, float bottom) {
    if (!manager.show_imbalances) return;
    if (manager.comparison == FootprintManager::Comparison::SamePrice &&
        manager.stacked_levels == 0) {
        if (row.buy_imbalance || row.sell_imbalance) {
            draw->AddRect(ImVec2(left, top), ImVec2(right, bottom),
                row.buy_imbalance ? IM_COL32(80, 180, 120, 160) : IM_COL32(180, 80, 80, 160),
                0.0f, 0, 1.0f);
        }
        return;
    }
    const float mid = (left + right) * 0.5f;
    if (row.sell_imbalance) draw->AddRect(ImVec2(left, top), ImVec2(mid, bottom),
        ImGui::GetColorU32(Theme::Tokens::DOWN), 0.0f, 0, row.sell_stack ? 2.5f : 1.0f);
    if (row.buy_imbalance) draw->AddRect(ImVec2(mid, top), ImVec2(right, bottom),
        ImGui::GetColorU32(Theme::Tokens::UP), 0.0f, 0, row.buy_stack ? 2.5f : 1.0f);
}

void ChartWidget::render_footprint_overlay(double visible_x_min, double visible_x_max) {
    auto& fp_mgr = ctx_.footprint_mgr();
    const auto& timestamps = candles().timestamps();
    if (timestamps.empty() && !candles().has_building_candle()) return;

    const int64_t tf_sec = candles().timeframe_seconds();
    if (tf_sec < 60) {
        ImPlot::GetPlotDrawList()->AddText(ImPlot::GetPlotPos(),
            ImGui::GetColorU32(Theme::Tokens::TX2), "Footprints require 1m or higher");
        return;
    }
    const int64_t as_of_ms = ctx_.replay_mgr().is_active()
        ? ctx_.replay_mgr().interpolated_time_ms()
        : std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    const double tf_ms = static_cast<double>(tf_sec) * 1000.0;
    const double visible_candles = (visible_x_max - visible_x_min) / tf_ms;
    const int64_t tf_ms_i = static_cast<int64_t>(tf_sec) * 1000;

    // Hard cutoff - too many candles, skip entirely
    if (visible_candles > 500.0) return;

    const double pixels_per_candle = ImPlot::GetPlotSize().x / visible_candles;
    if (pixels_per_candle < 3.0) return;

    // Delta divergence state: the last closed, reconciled candle with footprint data.
    bool div_valid = false;
    double div_high = 0.0, div_low = 0.0, div_delta = 0.0;

    // Zoom modes:
    //   zoomed_out_block: single colored rect per candle (no per-level detail)
    //                     Kicks in when candles are too narrow for text - matches MMT behavior
    //   show_text:        draw text labels inside cells
    const bool zoomed_out_block = (pixels_per_candle < 55.0);
    const bool show_text = !zoomed_out_block;

    // Get visible Y range for vertical culling
    const ImPlotRect plot_limits = ImPlot::GetPlotLimits();
    const double visible_y_min = plot_limits.Y.Min;
    const double visible_y_max = plot_limits.Y.Max;

    // Request historical data if needed (deduplication handled by FootprintManager)
    {
        const int64_t start_ms = static_cast<int64_t>(visible_x_min) - tf_ms_i;
        const int64_t end_ms = static_cast<int64_t>(visible_x_max) + tf_ms_i;
        fp_mgr.request_history(pair_, start_ms, end_ms, &ctx_.stream_mgr());
    }

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    const auto& opens = candles().opens();
    const auto& closes = candles().closes();
    const auto& lows = candles().lows();
    const auto& highs = candles().highs();

    // Font setup (only used when show_text)
    float font_scale = 1.0f;
    if (pixels_per_candle < 80.0f) font_scale = 0.7f;
    else if (pixels_per_candle < 120.0f) font_scale = 0.8f;
    ImFont* font = ImGui::GetFont();
    const float fp_font_size = ImGui::GetFontSize() * font_scale;

    // Tick per row - with minimum pixel height guarantee
    double tick_per_row = fp_mgr.ticks_per_row > 0
        ? fp_mgr.ticks_per_row * tick_size_ : 0.0;

    // Binary search for first visible candle
    size_t start_idx = 0;
    if (!timestamps.empty()) {
        const auto it = std::ranges::lower_bound(timestamps, visible_x_min - tf_ms);
        if (it != timestamps.begin()) {
            start_idx = static_cast<size_t>(std::distance(timestamps.cbegin(), it)) - 1;
        }
    }

    const auto& building = candles().building_candle();
    const bool include_building = candles().has_building_candle() &&
        (timestamps.empty() || building.timestamp_ms > timestamps.back());
    const size_t candle_count = timestamps.size() + (include_building ? 1 : 0);
    for (size_t i = start_idx; i < candle_count; ++i) {
        const bool is_building = i == timestamps.size();
        const int64_t candle_ts = is_building ? building.timestamp_ms : timestamps[i];
        const double candle_open = is_building ? building.open : opens[i];
        const double candle_close = is_building ? building.close : closes[i];
        const double candle_high = is_building ? building.high : highs[i];
        const double candle_low = is_building ? building.low : lows[i];
        const double ts = static_cast<double>(candle_ts);
        if (ts > visible_x_max + tf_ms) break;

        // Early cull: skip candles entirely off-screen vertically
        if (candle_high < visible_y_min || candle_low > visible_y_max) continue;

        const double candle_center = ts + tf_ms * 0.5;

        // ── ZOOMED OUT BLOCK MODE ──────────────────────────────────────
        // Single colored rectangle per candle, delta-colored (blue/pink)
        if (zoomed_out_block) {
            // Use cached merge - no per-frame allocation
            const auto* mc = fp_mgr.get_merged_grouped(
                FootprintManager::market_key(pair_.exchange, pair_.symbol), candle_ts, tf_sec, 0.0, as_of_ms);
            if (!mc) continue;

            double delta = mc->delta;
            float dn = (mc->total_buy + mc->total_sell > 0)
                ? static_cast<float>(std::abs(delta) / (mc->total_buy + mc->total_sell))
                : 0.0f;
            dn = std::sqrt(dn);

            const double body_top_price = std::max(candle_open, candle_close);
            const double body_bot_price = std::min(candle_open, candle_close);
            const double half_w = tf_ms * 0.35;
            const ImVec2 px_top = ImPlot::PlotToPixels(candle_center, body_top_price);
            const ImVec2 px_bot = ImPlot::PlotToPixels(candle_center, body_bot_price);
            const ImVec2 px_left  = ImPlot::PlotToPixels(candle_center - half_w, 0.0);
            const ImVec2 px_right = ImPlot::PlotToPixels(candle_center + half_w, 0.0);

            float top_y = std::min(px_top.y, px_bot.y);
            float bot_y = std::max(px_top.y, px_bot.y);
            if (bot_y - top_y < 2.0f) {
                float cy = (top_y + bot_y) * 0.5f;
                top_y = cy - 1.0f; bot_y = cy + 1.0f;
            }

            ImU32 block_color;
            if (delta >= 0.0) {
                block_color = IM_COL32(
                    static_cast<int>(20 + 30 * dn),
                    static_cast<int>(30 + 70 * dn),
                    static_cast<int>(50 + 140 * dn), 230);
            } else {
                block_color = IM_COL32(
                    static_cast<int>(40 + 140 * dn),
                    static_cast<int>(20 + 25 * dn),
                    static_cast<int>(40 + 70 * dn), 230);
            }
            draw_list->AddRectFilled(ImVec2(px_left.x, top_y), ImVec2(px_right.x, bot_y), block_color);
            continue;
        }

        // ── DETAILED CELL MODE (zoomed in) ─────────────────────────────
        // Auto tick_per_row with minimum pixel height guarantee
        double effective_tpr = tick_per_row;
        if (effective_tpr <= 0.0) {
            double range = candle_high - candle_low;
            if (range <= 0.0) range = 1.0;
            effective_tpr = range / 18.0;
            const ImVec2 px_hi = ImPlot::PlotToPixels(candle_center, candle_high);
            const ImVec2 px_lo = ImPlot::PlotToPixels(candle_center, candle_low);
            float candle_px_height = std::abs(px_lo.y - px_hi.y);
            if (candle_px_height > 0.0f) {
                int max_rows = std::max(3, static_cast<int>(candle_px_height / 12.0f));
                double min_tpr = range / static_cast<double>(max_rows);
                if (effective_tpr < min_tpr) effective_tpr = min_tpr;
            }
        }

        if (tick_size_ > 0.0) effective_tpr = std::max(tick_size_,
            std::ceil(effective_tpr / tick_size_) * tick_size_);

        // Use cached merge+group - no per-frame vector allocations
        const auto* mc = fp_mgr.get_merged_grouped(
            FootprintManager::market_key(pair_.exchange, pair_.symbol), candle_ts, tf_sec, effective_tpr, as_of_ms);
        if (show_text && ((mc && mc->observed_trades) ||
            (as_of_ms >= candle_ts && as_of_ms - candle_ts < tf_ms_i))) {
            const ImVec2 label = ImPlot::PlotToPixels(candle_center - tf_ms * 0.35,
                mc ? std::max(candle_high, mc->high_price) + effective_tpr : candle_high);
            draw_list->AddText(font, fp_font_size * 0.8f,
                ImVec2(label.x, label.y - fp_font_size),
                ImGui::GetColorU32(Theme::Tokens::TX2),
                mc && mc->observed_trades ? "Live observed (partial)" :
                mc ? "Partial (closed minutes)" :
                ctx_.replay_mgr().is_active() ? "Awaiting minute close" : "Awaiting trades");
        }
        if (!mc) continue;
        const auto& grouped = mc->levels;

        // Find max volume for color scaling
        double max_vol = 0.0;
        double max_abs_delta = 0.0;
        for (const auto& gl : grouped) {
            if (gl.total_volume > max_vol) max_vol = gl.total_volume;
            double ad = std::abs(gl.delta);
            if (ad > max_abs_delta) max_abs_delta = ad;
        }
        if (max_vol < 1e-8) continue;

        // Candle pixel bounds - footprint box IS the candle body
        const double half_w = tf_ms * 0.35;
        const ImVec2 px_left  = ImPlot::PlotToPixels(candle_center - half_w, 0.0);
        const ImVec2 px_right = ImPlot::PlotToPixels(candle_center + half_w, 0.0);
        const float cell_width = px_right.x - px_left.x;
        if (cell_width < 2.0f) continue;

        for (const auto& gl : grouped) {
            // Vertical culling - skip levels off-screen
            if (gl.price_hi < visible_y_min || gl.price_lo > visible_y_max) continue;

            const ImVec2 px_top    = ImPlot::PlotToPixels(candle_center, gl.price_hi);
            const ImVec2 px_bottom = ImPlot::PlotToPixels(candle_center, gl.price_lo);
            const float row_top    = px_top.y;
            const float row_bottom = px_bottom.y;
            const float row_height = row_bottom - row_top;
            if (row_height < 1.0f) continue;

            const float left  = px_left.x;
            const float right = px_right.x;

            if (fp_mgr.mode == FootprintManager::Mode::SellsBuys) {
                float mid_x = left + cell_width * 0.5f;

                // MMT-style: dark base, pink-red for sells, blue-teal for buys
                float si = max_vol > 0 ? static_cast<float>(gl.sell_volume / max_vol) : 0.0f;
                float bi = max_vol > 0 ? static_cast<float>(gl.buy_volume / max_vol) : 0.0f;
                si = std::sqrt(si); bi = std::sqrt(bi);

                ImU32 sell_bg = IM_COL32(
                    static_cast<int>(25 + 130 * si),
                    static_cast<int>(14 + 20 * si),
                    static_cast<int>(30 + 60 * si),
                    static_cast<int>(230 + 25 * si));
                draw_list->AddRectFilled(ImVec2(left, row_top), ImVec2(mid_x, row_bottom), sell_bg);

                ImU32 buy_bg = IM_COL32(
                    static_cast<int>(14 + 20 * bi),
                    static_cast<int>(20 + 55 * bi),
                    static_cast<int>(35 + 120 * bi),
                    static_cast<int>(230 + 25 * bi));
                draw_list->AddRectFilled(ImVec2(mid_x, row_top), ImVec2(right, row_bottom), buy_bg);

                // Subtle vertical divider between sell/buy columns
                draw_list->AddLine(ImVec2(mid_x, row_top), ImVec2(mid_x, row_bottom),
                                   IM_COL32(100, 100, 120, 80), 1.0f);

                // Issue 4 fix: lowered threshold from 0.7f to 0.5f
                if (show_text && row_height >= fp_font_size * 0.5f) {
                    char sell_buf[16], buy_buf[16];
                    format_fp_volume(sell_buf, sizeof(sell_buf), gl.sell_volume);
                    format_fp_volume(buy_buf, sizeof(buy_buf), gl.buy_volume);
                    float text_y = row_top + (row_height - fp_font_size) * 0.5f;
                    float half_cell = cell_width * 0.5f;

                    // Center sell text in left half
                    ImVec2 sell_size = font->CalcTextSizeA(fp_font_size, FLT_MAX, 0.0f, sell_buf);
                    float sell_x = left + (half_cell - sell_size.x) * 0.5f;
                    draw_list->AddText(font, fp_font_size,
                        ImVec2(sell_x, text_y),
                        IM_COL32(230, 215, 215, 255), sell_buf);

                    // Center buy text in right half
                    ImVec2 buy_size = font->CalcTextSizeA(fp_font_size, FLT_MAX, 0.0f, buy_buf);
                    float buy_x = mid_x + (half_cell - buy_size.x) * 0.5f;
                    draw_list->AddText(font, fp_font_size,
                        ImVec2(buy_x, text_y),
                        IM_COL32(215, 225, 240, 255), buy_buf);
                }

            } else if (fp_mgr.mode == FootprintManager::Mode::Delta) {
                float dn = max_abs_delta > 0
                    ? static_cast<float>(std::sqrt(std::abs(gl.delta) / max_abs_delta)) : 0.0f;
                ImU32 bg = (gl.delta >= 0.0)
                    ? IM_COL32(14+int(20*dn), 20+int(60*dn), 35+int(110*dn), 230+int(25*dn))
                    : IM_COL32(25+int(125*dn), 14+int(20*dn), 30+int(55*dn), 230+int(25*dn));
                draw_list->AddRectFilled(ImVec2(left, row_top), ImVec2(right, row_bottom), bg);

                if (show_text && row_height >= fp_font_size * 0.5f) {
                    char delta_buf[16];
                    format_fp_volume(delta_buf, sizeof(delta_buf), gl.delta);
                    ImVec2 sz = font->CalcTextSizeA(fp_font_size, FLT_MAX, 0.0f, delta_buf);
                    draw_list->AddText(font, fp_font_size,
                        ImVec2(left + (cell_width - sz.x) * 0.5f,
                               row_top + (row_height - fp_font_size) * 0.5f),
                        IM_COL32(230, 230, 230, 255), delta_buf);
                }

            } else if (fp_mgr.mode == FootprintManager::Mode::Matrix) {
                // Delta matrix: sells | buys | delta | volume as four columns.
                const bool wide = cell_width >= 92.0f;
                const float w_sell  = cell_width * 0.30f;
                const float w_buy   = cell_width * 0.30f;
                const float w_delta = wide ? cell_width * 0.22f : cell_width * 0.40f;
                const float w_vol   = wide ? cell_width - w_sell - w_buy - w_delta : 0.0f;
                const float si = max_vol > 0 ? std::sqrt(static_cast<float>(gl.sell_volume / max_vol)) : 0.0f;
                const float bi = max_vol > 0 ? std::sqrt(static_cast<float>(gl.buy_volume / max_vol)) : 0.0f;
                const float dn = max_abs_delta > 0 ? std::sqrt(static_cast<float>(std::abs(gl.delta) / max_abs_delta)) : 0.0f;
                const float vi = max_vol > 0 ? std::sqrt(static_cast<float>(gl.total_volume / max_vol)) : 0.0f;

                float cx = left;
                const auto cell = [&](float w, ImU32 bg) {
                    if (w > 0.5f)
                        draw_list->AddRectFilled(ImVec2(cx, row_top), ImVec2(cx + w, row_bottom), bg);
                };
                // Dimmer siblings of the Cluster palette so the matrix reads as data, not noise.
                cell(w_sell, IM_COL32(25 + int(100 * si), 14 + int(15 * si), 30 + int(45 * si), 235));
                cell(w_buy,  IM_COL32(14 + int(15 * bi), 20 + int(42 * bi), 35 + int(92 * bi), 235));
                cell(w_delta, gl.delta >= 0.0
                    ? IM_COL32(12 + int(14 * dn), 22 + int(50 * dn), 34 + int(96 * dn), 235)
                    : IM_COL32(26 + int(110 * dn), 14 + int(16 * dn), 30 + int(48 * dn), 235));
                if (wide) cell(w_vol, IM_COL32(14 + int(16 * vi), 20 + int(40 * vi), 40 + int(92 * vi), 235));
                cx += w_sell;
                draw_list->AddLine(ImVec2(cx, row_top), ImVec2(cx, row_bottom), IM_COL32(100, 100, 120, 70), 1.0f);
                cx += w_buy;
                draw_list->AddLine(ImVec2(cx, row_top), ImVec2(cx, row_bottom), IM_COL32(100, 100, 120, 70), 1.0f);
                cx += w_delta;
                if (wide)
                    draw_list->AddLine(ImVec2(cx, row_top), ImVec2(cx, row_bottom), IM_COL32(100, 100, 120, 70), 1.0f);

                if (show_text && row_height >= fp_font_size * 0.5f) {
                    char b_sell[16], b_buy[16], b_delta[16], b_vol[16];
                    format_fp_volume(b_sell, sizeof(b_sell), gl.sell_volume);
                    format_fp_volume(b_buy, sizeof(b_buy), gl.buy_volume);
                    format_fp_volume(b_delta, sizeof(b_delta), gl.delta);
                    const float text_y = row_top + (row_height - fp_font_size) * 0.5f;
                    const auto put = [&](const char* txt, float x, float w, ImU32 ink) {
                        const ImVec2 sz = font->CalcTextSizeA(fp_font_size, FLT_MAX, 0.0f, txt);
                        draw_list->AddText(font, fp_font_size,
                            ImVec2(x + (w - sz.x) * 0.5f, text_y), ink, txt);
                    };
                    put(b_sell, left, w_sell, IM_COL32(230, 215, 215, 255));
                    put(b_buy, left + w_sell, w_buy, IM_COL32(215, 225, 240, 255));
                    put(b_delta, left + w_sell + w_buy, w_delta, IM_COL32(230, 230, 230, 255));
                    if (wide) {
                        format_fp_volume(b_vol, sizeof(b_vol), gl.total_volume);
                        put(b_vol, left + w_sell + w_buy + w_delta, w_vol, IM_COL32(210, 220, 215, 255));
                    }
                }

            } else { // Volume mode
                float vi = std::sqrt(static_cast<float>(gl.total_volume / max_vol));
                ImU32 bg = IM_COL32(14+int(20*vi), 20+int(50*vi), 40+int(115*vi), 230+int(25*vi));
                draw_list->AddRectFilled(ImVec2(left, row_top), ImVec2(right, row_bottom), bg);

                if (show_text && row_height >= fp_font_size * 0.5f) {
                    char vol_buf[16];
                    format_fp_volume(vol_buf, sizeof(vol_buf), gl.total_volume);
                    ImVec2 sz = font->CalcTextSizeA(fp_font_size, FLT_MAX, 0.0f, vol_buf);
                    draw_list->AddText(font, fp_font_size,
                        ImVec2(left + (cell_width - sz.x) * 0.5f,
                               row_top + (row_height - fp_font_size) * 0.5f),
                        IM_COL32(230, 230, 230, 255), vol_buf);
                }
            }

            draw_fp_imbalance(draw_list, fp_mgr, gl, left, right, row_top, row_bottom);

            // POC - subtle horizontal line
            if (fp_mgr.show_poc && gl.is_poc) {
                float poc_y = row_top + row_height * 0.5f;
                draw_list->AddLine(ImVec2(left, poc_y), ImVec2(right, poc_y),
                                   IM_COL32(200, 180, 100, 140), 1.0f);
            }
        }

        // Outer border around the entire footprint box
        {
            const ImVec2 box_top = ImPlot::PlotToPixels(candle_center, mc->high_price);
            const ImVec2 box_bot = ImPlot::PlotToPixels(candle_center, mc->low_price);
            draw_list->AddRect(ImVec2(px_left.x, box_top.y), ImVec2(px_right.x, box_bot.y),
                               IM_COL32(120, 130, 150, 60), 0.0f, 0, 1.0f);
        }

        // V:/D: footer
        if (fp_mgr.show_summary && show_text) {
            const ImVec2 px_low = ImPlot::PlotToPixels(candle_center, candle_low);
            float footer_y = px_low.y + 3.0f;
            float ff = fp_font_size * 0.8f;

            char vf[24], df[24];
            snprintf(vf, sizeof(vf), "V:"); format_fp_volume(vf+2, sizeof(vf)-2, mc->total_volume);
            snprintf(df, sizeof(df), "D:"); format_fp_volume(df+2, sizeof(df)-2, mc->delta);

            ImVec2 vs = font->CalcTextSizeA(ff, FLT_MAX, 0.0f, vf);
            ImVec2 ds = font->CalcTextSizeA(ff, FLT_MAX, 0.0f, df);
            float tw = vs.x + 3.0f + ds.x;
            float sx = px_left.x + (cell_width - tw) * 0.5f;

            draw_list->AddText(font, ff, ImVec2(sx, footer_y),
                               IM_COL32(120, 170, 200, 180), vf);
            draw_list->AddText(font, ff, ImVec2(sx + vs.x + 3.0f, footer_y),
                mc->delta >= 0 ? IM_COL32(100, 180, 130, 180) : IM_COL32(180, 100, 100, 180), df);
        }

        // ── Delta divergence: price makes a new extreme while delta goes the other way ──
        // Classic absorption tell. Closed, reconciled candles only - observed/live cells and
        // the building candle are provisional by definition.
        if (fp_mgr.show_delta_divergence && show_text && mc &&
            !mc->provisional && !mc->observed_trades) {
            if (div_valid) {
                const double scale = std::max(std::abs(mc->delta), std::abs(div_delta));
                const double diff = mc->delta - div_delta;
                if (scale > 0.0 && std::abs(diff) >= 0.25 * scale) {
                    constexpr float s = 4.0f, off = 6.0f;
                    if (candle_high > div_high && diff < 0.0) {
                        // Bearish: higher high on weaker delta.
                        const ImVec2 at = ImPlot::PlotToPixels(candle_center, candle_high);
                        draw_list->AddTriangleFilled(
                            ImVec2(at.x - s, at.y - off - 2.0f * s),
                            ImVec2(at.x + s, at.y - off - 2.0f * s),
                            ImVec2(at.x, at.y - off), IM_COL32(210, 80, 80, 220));
                    } else if (candle_low < div_low && diff > 0.0) {
                        // Bullish: lower low on stronger (rising) delta.
                        const ImVec2 at = ImPlot::PlotToPixels(candle_center, candle_low);
                        draw_list->AddTriangleFilled(
                            ImVec2(at.x - s, at.y + off + 2.0f * s),
                            ImVec2(at.x + s, at.y + off + 2.0f * s),
                            ImVec2(at.x, at.y + off), IM_COL32(80, 190, 120, 220));
                    }
                }
            }
            div_valid = true;
            div_high = candle_high;
            div_low = candle_low;
            div_delta = mc->delta;
        }
    }

    const ImVec2 status_pos = ImPlot::GetPlotPos();
    const char* cadence = ctx_.replay_mgr().is_active()
        ? "Replay footprints: closed minutes"
        : "Live: observed trades; reconciled after minute close";
    const ImVec2 note(status_pos.x + 12.0f, status_pos.y + 60.0f);
    const ImVec2 note_size = ImGui::CalcTextSize(cadence);
    ImPlot::GetPlotDrawList()->AddRectFilled(
        ImVec2(note.x - 4.0f, note.y - 2.0f),
        ImVec2(note.x + note_size.x + 4.0f, note.y + note_size.y + 2.0f),
        ImGui::GetColorU32(Theme::Tokens::PANEL));
    ImPlot::GetPlotDrawList()->AddText(note, ImGui::GetColorU32(Theme::Tokens::TX2), cadence);
    ImPlot::PopPlotClipRect();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Footprint Profile - MMT-style per-candle volume profile bars
//
// Like Cluster mode but with proportional-width bar backgrounds behind the
// sell|buy text. Each row has two columns (sell left, buy right) filling the
// full candle width. Bar width within each half is proportional to that side's
// volume relative to the candle's max. POC row gets a white outline box.
// ═══════════════════════════════════════════════════════════════════════════════

void ChartWidget::render_footprint_profile(double visible_x_min, double visible_x_max) {
    auto& fp_mgr = ctx_.footprint_mgr();
    const auto& timestamps = candles().timestamps();
    if (timestamps.empty() && !candles().has_building_candle()) return;

    const int64_t tf_sec = candles().timeframe_seconds();
    if (tf_sec < 60) {
        ImPlot::GetPlotDrawList()->AddText(ImPlot::GetPlotPos(),
            ImGui::GetColorU32(Theme::Tokens::TX2), "Footprints require 1m or higher");
        return;
    }
    const int64_t as_of_ms = ctx_.replay_mgr().is_active()
        ? ctx_.replay_mgr().interpolated_time_ms()
        : std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    const double tf_ms = static_cast<double>(tf_sec) * 1000.0;
    const int64_t tf_ms_i = static_cast<int64_t>(tf_sec) * 1000;
    const double visible_candles = (visible_x_max - visible_x_min) / tf_ms;

    if (visible_candles > 500.0) return;

    const double pixels_per_candle = ImPlot::GetPlotSize().x / visible_candles;
    if (pixels_per_candle < 3.0) return;

    const bool zoomed_out_block = (pixels_per_candle < 55.0);
    const bool show_text = !zoomed_out_block;

    const ImPlotRect plot_limits = ImPlot::GetPlotLimits();
    const double visible_y_min = plot_limits.Y.Min;
    const double visible_y_max = plot_limits.Y.Max;

    // Request historical data
    {
        const int64_t start_ms = static_cast<int64_t>(visible_x_min) - tf_ms_i;
        const int64_t end_ms = static_cast<int64_t>(visible_x_max) + tf_ms_i;
        fp_mgr.request_history(pair_, start_ms, end_ms, &ctx_.stream_mgr());
    }

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    const auto& opens = candles().opens();
    const auto& closes = candles().closes();
    const auto& lows = candles().lows();
    const auto& highs = candles().highs();

    ImFont* font = ImGui::GetFont();
    float font_scale = 1.0f;
    if (pixels_per_candle < 80.0f) font_scale = 0.7f;
    else if (pixels_per_candle < 120.0f) font_scale = 0.8f;
    const float fp_font_size = ImGui::GetFontSize() * font_scale;

    double tick_per_row = fp_mgr.ticks_per_row > 0
        ? fp_mgr.ticks_per_row * tick_size_ : 0.0;

    // Binary search for first visible candle
    size_t start_idx = 0;
    if (!timestamps.empty()) {
        const auto it = std::ranges::lower_bound(timestamps, visible_x_min - tf_ms);
        if (it != timestamps.begin()) {
            start_idx = static_cast<size_t>(std::distance(timestamps.cbegin(), it)) - 1;
        }
    }

    const auto& building = candles().building_candle();
    const bool include_building = candles().has_building_candle() &&
        (timestamps.empty() || building.timestamp_ms > timestamps.back());
    const size_t candle_count = timestamps.size() + (include_building ? 1 : 0);
    for (size_t i = start_idx; i < candle_count; ++i) {
        const bool is_building = i == timestamps.size();
        const int64_t candle_ts = is_building ? building.timestamp_ms : timestamps[i];
        const double candle_open = is_building ? building.open : opens[i];
        const double candle_close = is_building ? building.close : closes[i];
        const double candle_high = is_building ? building.high : highs[i];
        const double candle_low = is_building ? building.low : lows[i];
        const double ts = static_cast<double>(candle_ts);
        if (ts > visible_x_max + tf_ms) break;
        if (candle_high < visible_y_min || candle_low > visible_y_max) continue;

        const double candle_center = ts + tf_ms * 0.5;

        // ── ZOOMED OUT BLOCK MODE ──────────────────────────────────────
        // Same as cluster: single delta-colored rectangle per candle
        if (zoomed_out_block) {
            const auto* mc = fp_mgr.get_merged_grouped(
                FootprintManager::market_key(pair_.exchange, pair_.symbol), candle_ts, tf_sec, 0.0, as_of_ms);
            if (!mc) continue;

            double delta = mc->delta;
            float dn = (mc->total_buy + mc->total_sell > 0)
                ? static_cast<float>(std::abs(delta) / (mc->total_buy + mc->total_sell))
                : 0.0f;
            dn = std::sqrt(dn);

            const double body_top_price = std::max(candle_open, candle_close);
            const double body_bot_price = std::min(candle_open, candle_close);
            const double half_w = tf_ms * 0.35;
            const ImVec2 px_top = ImPlot::PlotToPixels(candle_center, body_top_price);
            const ImVec2 px_bot = ImPlot::PlotToPixels(candle_center, body_bot_price);
            const ImVec2 px_left  = ImPlot::PlotToPixels(candle_center - half_w, 0.0);
            const ImVec2 px_right = ImPlot::PlotToPixels(candle_center + half_w, 0.0);

            float top_y = std::min(px_top.y, px_bot.y);
            float bot_y = std::max(px_top.y, px_bot.y);
            if (bot_y - top_y < 2.0f) {
                float cy = (top_y + bot_y) * 0.5f;
                top_y = cy - 1.0f; bot_y = cy + 1.0f;
            }

            ImU32 block_color;
            if (delta >= 0.0) {
                block_color = IM_COL32(
                    static_cast<int>(20 + 30 * dn),
                    static_cast<int>(30 + 70 * dn),
                    static_cast<int>(50 + 140 * dn), 230);
            } else {
                block_color = IM_COL32(
                    static_cast<int>(40 + 140 * dn),
                    static_cast<int>(20 + 25 * dn),
                    static_cast<int>(40 + 70 * dn), 230);
            }
            draw_list->AddRectFilled(ImVec2(px_left.x, top_y), ImVec2(px_right.x, bot_y), block_color);
            continue;
        }

        // ── DETAILED PROFILE MODE (zoomed in) ─────────────────────────
        // Auto tick_per_row (same logic as cluster)
        double effective_tpr = tick_per_row;
        if (effective_tpr <= 0.0) {
            double range = candle_high - candle_low;
            if (range <= 0.0) range = 1.0;
            effective_tpr = range / 18.0;
            const ImVec2 px_hi = ImPlot::PlotToPixels(candle_center, candle_high);
            const ImVec2 px_lo = ImPlot::PlotToPixels(candle_center, candle_low);
            float candle_px_height = std::abs(px_lo.y - px_hi.y);
            if (candle_px_height > 0.0f) {
                int max_rows = std::max(3, static_cast<int>(candle_px_height / 12.0f));
                double min_tpr = range / static_cast<double>(max_rows);
                if (effective_tpr < min_tpr) effective_tpr = min_tpr;
            }
        }

        if (tick_size_ > 0.0) effective_tpr = std::max(tick_size_,
            std::ceil(effective_tpr / tick_size_) * tick_size_);

        const auto* mc = fp_mgr.get_merged_grouped(
            FootprintManager::market_key(pair_.exchange, pair_.symbol), candle_ts, tf_sec, effective_tpr, as_of_ms);
        if (show_text && ((mc && mc->observed_trades) ||
            (as_of_ms >= candle_ts && as_of_ms - candle_ts < tf_ms_i))) {
            const ImVec2 label = ImPlot::PlotToPixels(candle_center - tf_ms * 0.35,
                mc ? std::max(candle_high, mc->high_price) + effective_tpr : candle_high);
            draw_list->AddText(font, fp_font_size * 0.8f,
                ImVec2(label.x, label.y - fp_font_size),
                ImGui::GetColorU32(Theme::Tokens::TX2),
                mc && mc->observed_trades ? "Live observed (partial)" :
                mc ? "Partial (closed minutes)" :
                ctx_.replay_mgr().is_active() ? "Awaiting minute close" : "Awaiting trades");
        }
        if (!mc) continue;
        const auto& grouped = mc->levels;

        // Find max volume per side (for proportional bar widths)
        double max_side_vol = 0.0;
        for (const auto& gl : grouped) {
            if (gl.buy_volume > max_side_vol) max_side_vol = gl.buy_volume;
            if (gl.sell_volume > max_side_vol) max_side_vol = gl.sell_volume;
        }
        if (max_side_vol < 1e-8) continue;

        // Candle pixel bounds - same width as cluster mode
        const double half_w = tf_ms * 0.35;
        const ImVec2 px_left  = ImPlot::PlotToPixels(candle_center - half_w, 0.0);
        const ImVec2 px_right = ImPlot::PlotToPixels(candle_center + half_w, 0.0);
        const float cell_width = px_right.x - px_left.x;
        if (cell_width < 2.0f) continue;

        const float left = px_left.x;
        const float right = px_right.x;
        const float mid_x = left + cell_width * 0.5f;
        const float half_cell = cell_width * 0.5f;

        for (const auto& gl : grouped) {
            if (gl.price_hi < visible_y_min || gl.price_lo > visible_y_max) continue;

            const ImVec2 px_top = ImPlot::PlotToPixels(candle_center, gl.price_hi);
            const ImVec2 px_bot = ImPlot::PlotToPixels(candle_center, gl.price_lo);
            const float row_top = px_top.y;
            const float row_bot = px_bot.y;
            const float row_height = row_bot - row_top;
            if (row_height < 1.0f) continue;

            // Volume fractions for proportional bar widths
            float sell_frac = static_cast<float>(gl.sell_volume / max_side_vol);
            float buy_frac  = static_cast<float>(gl.buy_volume / max_side_vol);

            // Bar widths within each half-cell
            float sell_bar_w = sell_frac * half_cell;
            float buy_bar_w  = buy_frac * half_cell;

            // Sell bar - fills from LEFT edge rightward within left half
            if (sell_bar_w > 0.5f) {
                float si = std::sqrt(sell_frac);
                ImU32 sell_color = IM_COL32(
                    static_cast<int>(40 + 150 * si),
                    static_cast<int>(15 + 25 * si),
                    static_cast<int>(35 + 70 * si), 200);
                draw_list->AddRectFilled(
                    ImVec2(mid_x - sell_bar_w, row_top),
                    ImVec2(mid_x, row_bot), sell_color);
            }

            // Buy bar - fills from mid_x rightward within right half
            if (buy_bar_w > 0.5f) {
                float bi = std::sqrt(buy_frac);
                ImU32 buy_color = IM_COL32(
                    static_cast<int>(15 + 25 * bi),
                    static_cast<int>(25 + 65 * bi),
                    static_cast<int>(45 + 140 * bi), 200);
                draw_list->AddRectFilled(
                    ImVec2(mid_x, row_top),
                    ImVec2(mid_x + buy_bar_w, row_bot), buy_color);
            }

            // Text labels: sell vol on left, buy vol on right (always both shown)
            if (show_text && row_height >= fp_font_size * 0.5f) {
                char sell_buf[16], buy_buf[16];
                format_fp_volume(sell_buf, sizeof(sell_buf), gl.sell_volume);
                format_fp_volume(buy_buf, sizeof(buy_buf), gl.buy_volume);
                float text_y = row_top + (row_height - fp_font_size) * 0.5f;

                // Sell text - centered in left half
                ImVec2 sell_size = font->CalcTextSizeA(fp_font_size, FLT_MAX, 0.0f, sell_buf);
                float sell_x = left + (half_cell - sell_size.x) * 0.5f;
                draw_list->AddText(font, fp_font_size,
                    ImVec2(sell_x, text_y),
                    IM_COL32(230, 215, 215, 255), sell_buf);

                // Buy text - centered in right half
                ImVec2 buy_size = font->CalcTextSizeA(fp_font_size, FLT_MAX, 0.0f, buy_buf);
                float buy_x = mid_x + (half_cell - buy_size.x) * 0.5f;
                draw_list->AddText(font, fp_font_size,
                    ImVec2(buy_x, text_y),
                    IM_COL32(215, 225, 240, 255), buy_buf);
            }

            // Subtle vertical divider between sell/buy columns
            draw_list->AddLine(ImVec2(mid_x, row_top), ImVec2(mid_x, row_bot),
                               IM_COL32(100, 100, 120, 60), 1.0f);

            // POC - white outline box around the highest-volume row (MMT style)
            if (fp_mgr.show_poc && gl.is_poc) {
                draw_list->AddRect(ImVec2(left, row_top), ImVec2(right, row_bot),
                                   IM_COL32(220, 220, 220, 200), 0.0f, 0, 1.5f);
            }

            draw_fp_imbalance(draw_list, fp_mgr, gl, left, right, row_top, row_bot);
        }

        // Outer border around entire footprint box
        {
            const ImVec2 box_top = ImPlot::PlotToPixels(candle_center, mc->high_price);
            const ImVec2 box_bot = ImPlot::PlotToPixels(candle_center, mc->low_price);
            draw_list->AddRect(ImVec2(left, box_top.y), ImVec2(right, box_bot.y),
                               IM_COL32(120, 130, 150, 60), 0.0f, 0, 1.0f);
        }

        // V:/D: footer
        if (fp_mgr.show_summary && show_text) {
            const ImVec2 px_low = ImPlot::PlotToPixels(candle_center, candle_low);
            float footer_y = px_low.y + 3.0f;
            float ff = fp_font_size * 0.8f;

            char vf[24], df[24];
            snprintf(vf, sizeof(vf), "V:"); format_fp_volume(vf+2, sizeof(vf)-2, mc->total_volume);
            snprintf(df, sizeof(df), "D:"); format_fp_volume(df+2, sizeof(df)-2, mc->delta);

            ImVec2 vs = font->CalcTextSizeA(ff, FLT_MAX, 0.0f, vf);
            ImVec2 ds = font->CalcTextSizeA(ff, FLT_MAX, 0.0f, df);
            float tw = vs.x + 3.0f + ds.x;
            float sx = left + (cell_width - tw) * 0.5f;

            draw_list->AddText(font, ff, ImVec2(sx, footer_y),
                               IM_COL32(120, 170, 200, 180), vf);
            draw_list->AddText(font, ff, ImVec2(sx + vs.x + 3.0f, footer_y),
                mc->delta >= 0 ? IM_COL32(100, 180, 130, 180) : IM_COL32(180, 100, 100, 180), df);
        }
    }

    const ImVec2 status_pos = ImPlot::GetPlotPos();
    const char* cadence = ctx_.replay_mgr().is_active()
        ? "Replay footprints: closed minutes"
        : "Live: observed trades; reconciled after minute close";
    const ImVec2 note(status_pos.x + 12.0f, status_pos.y + 60.0f);
    const ImVec2 note_size = ImGui::CalcTextSize(cadence);
    ImPlot::GetPlotDrawList()->AddRectFilled(
        ImVec2(note.x - 4.0f, note.y - 2.0f),
        ImVec2(note.x + note_size.x + 4.0f, note.y + note_size.y + 2.0f),
        ImGui::GetColorU32(Theme::Tokens::PANEL));
    ImPlot::GetPlotDrawList()->AddText(note, ImGui::GetColorU32(Theme::Tokens::TX2), cadence);
    ImPlot::PopPlotClipRect();
}

void ChartWidget::render_footprint_settings_popup() {
    footprint_settings_panel_.set_panel_height(340.0f);
    if (footprint_settings_panel_.begin()) {
        auto& fp = ctx_.footprint_mgr();

        if (footprint_settings_panel_.tab("Mode")) {
            const char* fp_modes[] = { "Cluster", "Delta", "Volume", "Profile", "Matrix" };
            int mode_idx = static_cast<int>(fp.mode);
            ImGui::SetNextItemWidth(130);
            if (ImGui::Combo("Mode##fp", &mode_idx, fp_modes, 5)) {
                fp.mode = static_cast<FootprintManager::Mode>(mode_idx);
            }
            if (fp.mode == FootprintManager::Mode::Matrix && ImGui::IsItemHovered())
                Theme::tooltip("Delta matrix: sells | buys | delta | volume per row.");
        }
        if (footprint_settings_panel_.tab("Imbalances")) {
            int comparison = static_cast<int>(fp.comparison);
            const char* comparisons[] = { "Same price", "Diagonal" };
            ImGui::SetNextItemWidth(130);
            if (ImGui::Combo("Comparison##fp", &comparison, comparisons, 2))
                fp.comparison = static_cast<FootprintManager::Comparison>(comparison);
            if (ImGui::IsItemHovered()) Theme::tooltip(
                "Diagonal: buys vs sells one row below; sells vs buys one row above.\n"
                "Missing rows and zero opposing volume do not qualify.");
            ImGui::SetNextItemWidth(100);
            ImGui::SliderFloat("Imbalance Ratio##fp", &fp.imbalance_ratio, 1.5f, 10.0f, "%.1f:1");
            if (ImGui::IsItemHovered()) {
                Theme::tooltip("At least this ratio against the opposing volume\nat the selected comparison price");
            }
            ImGui::Spacing();
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputDouble("Minimum volume##fp", &fp.imbalance_min_volume, 0, 0, "%.4f"))
                fp.imbalance_min_volume = std::isfinite(fp.imbalance_min_volume)
                    ? std::max(0.0, fp.imbalance_min_volume) : 0.0;
            if (ImGui::IsItemHovered()) Theme::tooltip("Minimum qualifying buy or sell volume, in feed volume units.");
            ImGui::SetNextItemWidth(100);
            if (ImGui::SliderInt("Stack levels##fp", &fp.stacked_levels, 0, 10,
                                 fp.stacked_levels == 0 ? "Off" : "%d", ImGuiSliderFlags_AlwaysClamp))
                if (fp.stacked_levels == 1) fp.stacked_levels = 2;
            if (ImGui::IsItemHovered()) Theme::tooltip(
                "Thick outlines mark consecutive same-side imbalances.\n"
                "A missing grouped price breaks a stack. 0 disables stacks.");
            ImGui::TextWrapped("Thin outlines: imbalance. Thick: stack. Sells left, buys right.");
            ImGui::TextWrapped("Live cells use observed trades and may be partial after joining or reconnecting. Closed 1m snapshots replace them; replay uses closed minutes.");
        }
        if (footprint_settings_panel_.tab("Display")) {
            ImGui::Checkbox("Imbalance Highlights##fp", &fp.show_imbalances);
            ImGui::Checkbox("POC Marker##fp", &fp.show_poc);
            ImGui::Checkbox("V:/D: Footer##fp", &fp.show_summary);
            ImGui::Checkbox("Delta divergence##fp", &fp.show_delta_divergence);
            if (ImGui::IsItemHovered()) Theme::tooltip(
                "Mark candles whose price makes a higher high or lower low\n"
                "while delta moves the other way against the previous candle\n"
                "(absorption). Closed, reconciled candles only.");
        }
        if (footprint_settings_panel_.tab("Sizing")) {
            ImGui::Text("Ticks Per Row");
            ImGui::SameLine();
            if (fp.ticks_per_row == 0) {
                ImGui::TextDisabled("(auto)");
            } else if (tick_size_ > 0) {
                char pl[32];
                snprintf(pl, sizeof(pl), "($%.2f)", fp.ticks_per_row * tick_size_);
                ImGui::TextDisabled("%s", pl);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("-##fptpr") && fp.ticks_per_row > 0) {
                fp.ticks_per_row = std::max(0, fp.ticks_per_row - 1);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+##fptpr")) {
                fp.ticks_per_row += 1;
            }
            ImGui::SetNextItemWidth(60);
            if (ImGui::InputInt("##fptpr_input", &fp.ticks_per_row, 0, 0)) {
                fp.ticks_per_row = std::max(0, fp.ticks_per_row);
            }
            if (ImGui::IsItemHovered()) {
                Theme::tooltip("Price ticks per row (0 = auto ~18 rows)");
            }
            ImGui::Spacing();
            ImGui::Separator();
            int cached = fp.candle_count(FootprintManager::market_key(pair_.exchange, pair_.symbol));
            ImGui::TextDisabled("Cached candles: %d", cached);
            if (fp.is_loading(FootprintManager::market_key(pair_.exchange, pair_.symbol))) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "(loading...)");
            }
            if (ImGui::SmallButton("Clear Cache##fp")) {
                fp.clear(FootprintManager::market_key(pair_.exchange, pair_.symbol));
            }
        }
        footprint_settings_panel_.end();
    }
}
