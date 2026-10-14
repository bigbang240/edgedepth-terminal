#include "ui/chart_widget.h"
#include "core/tpo_manager.h"
#include "rendering/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// ═══════════════════════════════════════════════════════════════════════════════
// TPO / Market Profile Renderer
// ═══════════════════════════════════════════════════════════════════════════════

void ChartWidget::render_tpo(double visible_x_min, double visible_x_max) {
    auto& tpo = ctx_.tpo_mgr();

    // Build sessions from current candle data
    const auto& timestamps = candles().timestamps();
    const auto& highs = candles().highs();
    const auto& lows = candles().lows();
    if (timestamps.empty()) return;

    const int64_t tf_sec = candles().timeframe_seconds();
    double tick_per_row = tpo.ticks_per_row_setting > 0
        ? tpo.ticks_per_row_setting * tick_size_ : 0.0;

    tpo.build_sessions(pair_.symbol,
                       timestamps.data(), highs.data(), lows.data(),
                       timestamps.size(), tf_sec, tick_per_row);

    const auto* sessions = tpo.get_sessions(pair_.symbol);
    if (!sessions || sessions->empty()) return;

    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize() * 0.75f;

    const ImPlotRect plot_limits = ImPlot::GetPlotLimits();
    const double vis_y_min = plot_limits.Y.Min;
    const double vis_y_max = plot_limits.Y.Max;

    for (size_t si = 0; si < sessions->size(); ++si) {
        const auto& sess = (*sessions)[si];
        double sess_start = static_cast<double>(sess.session_start_ms);
        double sess_end = static_cast<double>(sess.session_end_ms);

        // Skip sessions outside visible range
        if (sess_end < visible_x_min || sess_start > visible_x_max) continue;
        if (sess.rows.empty() || sess.max_block_count == 0) continue;

        // Session pixel bounds on x-axis
        const ImVec2 px_sess_start = ImPlot::PlotToPixels(sess_start, 0.0);
        const ImVec2 px_sess_end = ImPlot::PlotToPixels(sess_end, 0.0);
        const float sess_px_width = px_sess_end.x - px_sess_start.x;
        if (sess_px_width < 2.0f) continue;

        // Block dimensions: make blocks roughly square
        // Row height in pixels determines block width for square appearance
        double sample_row_mid = (sess.session_high + sess.session_low) * 0.5;
        const ImVec2 px_row_top = ImPlot::PlotToPixels(sess_start, sample_row_mid + sess.tick_per_row * 0.5);
        const ImVec2 px_row_bot = ImPlot::PlotToPixels(sess_start, sample_row_mid - sess.tick_per_row * 0.5);
        float row_px_height = std::abs(px_row_bot.y - px_row_top.y);
        if (row_px_height < 1.0f) row_px_height = 1.0f;

        // Block width: scale so widest row fills ~82% of session (18% gap for labels/lines)
        const float spacing = static_cast<float>(tpo.profile_spacing);
        float usable_width = sess_px_width * 0.82f - spacing;
        if (usable_width < 2.0f) usable_width = 2.0f;
        float block_px_width = 1.0f;
        if (sess.max_block_count > 0) {
            block_px_width = usable_width / static_cast<float>(sess.max_block_count);
        }
        // Cap: never wider than 3x row height (reasonable max when zoomed way in)
        float max_block_w = row_px_height * 3.0f;
        if (max_block_w < 4.0f) max_block_w = 4.0f;
        if (block_px_width > max_block_w) block_px_width = max_block_w;
        if (block_px_width < 1.0f) block_px_width = 1.0f;
        float profile_left_x = px_sess_start.x;

        // Draw blocks per row
        for (const auto& row : sess.rows) {
            if (row.blocks.empty()) continue;
            if (row.price_hi < vis_y_min || row.price_lo > vis_y_max) continue;

            const ImVec2 px_top = ImPlot::PlotToPixels(sess_start, row.price_hi);
            const ImVec2 px_bot = ImPlot::PlotToPixels(sess_start, row.price_lo);
            float row_top = px_top.y;
            float row_bot = px_bot.y;
            float row_height = row_bot - row_top;
            if (row_height < 1.0f) row_height = 1.0f;

            int block_count = static_cast<int>(row.blocks.size());

            for (int b = 0; b < block_count; ++b) {
                float bx_left = profile_left_x + b * block_px_width;
                float bx_right = bx_left + block_px_width - 1.0f;
                if (bx_right < bx_left + 1.0f) bx_right = bx_left + 1.0f;

                // Color based on value area / period
                ImU32 color;
                int period_idx = row.blocks[b].period_idx;

                if (row.is_poc) {
                    // POC row - white (MMT style)
                    color = IM_COL32(220, 220, 230, 230);
                } else if (row.is_value_area) {
                    // Value area - blue with brightness by period
                    float brightness = 0.4f + 0.5f *
                        (static_cast<float>(period_idx) / std::max(1, sess.total_periods - 1));
                    color = IM_COL32(
                        static_cast<int>(30 + 80 * brightness),
                        static_cast<int>(60 + 90 * brightness),
                        static_cast<int>(140 + 80 * brightness), 220);
                } else {
                    // Outside value area - grey with brightness by period
                    float brightness = 0.25f + 0.5f *
                        (static_cast<float>(period_idx) / std::max(1, sess.total_periods - 1));
                    int grey = static_cast<int>(60 + 120 * brightness);
                    color = IM_COL32(grey, grey, grey + 5, 200);
                }

                // Highlight start/end periods (overrides above)
                if (tpo.highlight_start_end) {
                    if (period_idx == 0) {
                        color = IM_COL32(50, 90, 200, 230); // blue for first period
                    } else if (period_idx == sess.total_periods - 1) {
                        color = IM_COL32(200, 70, 110, 230); // pink for last period
                    }
                }

                // Single print highlight (overrides above)
                if (row.is_single_print && tpo.show_single_prints) {
                    color = IM_COL32(80, 160, 210, 170); // light blue
                }

                // Poor High/Low row - white blocks (MMT style)
                if (tpo.show_poor_high_low) {
                    if (sess.has_poor_high && &row == &sess.rows.back()) {
                        color = IM_COL32(220, 220, 230, 230);
                    }
                    if (sess.has_poor_low && &row == &sess.rows.front()) {
                        color = IM_COL32(220, 220, 230, 230);
                    }
                }

                draw_list->AddRectFilled(
                    ImVec2(bx_left, row_top + 0.5f),
                    ImVec2(bx_right, row_bot - 0.5f), color);
            }
        }

        // ── Profile right edge (for line endpoints) ─────────────
        float profile_right_x = profile_left_x +
            sess.max_block_count * block_px_width;
        float line_extend = block_px_width * 2.0f; // extend a bit past profile
        float line_end_x = profile_right_x + line_extend;
        // Clamp lines to session boundary
        float max_line_x = px_sess_end.x - 2.0f;
        if (line_end_x > max_line_x) line_end_x = max_line_x;
        if (line_end_x < profile_right_x) line_end_x = profile_right_x;

        // Labels positioned just past line end (always shown, may overlap next session like MMT)
        float label_x = line_end_x + 3.0f;

        // ── POC line (solid white, short - from profile edge) ────
        if (tpo.show_poc_ray && sess.poc_row_idx >= 0) {
            ImVec2 poc_px = ImPlot::PlotToPixels(sess_start, sess.poc_price);
            draw_list->AddLine(
                ImVec2(profile_right_x, poc_px.y),
                ImVec2(line_end_x, poc_px.y),
                IM_COL32(255, 255, 255, 255), 1.5f);
            draw_list->AddText(font, font_size,
                ImVec2(label_x, poc_px.y - font_size * 0.5f),
                IM_COL32(255, 255, 255, 255), "POC");
        }

        // ── VAH / VAL lines (dashed) ─────────────────────────────
        if (tpo.show_vah_val_rays) {
            ImVec2 vah_px = ImPlot::PlotToPixels(sess_start, sess.vah);
            for (float x = profile_right_x; x < line_end_x; x += 6.0f) {
                float x2 = std::min(x + 3.0f, line_end_x);
                draw_list->AddLine(ImVec2(x, vah_px.y), ImVec2(x2, vah_px.y),
                                   IM_COL32(100, 150, 210, 180), 1.0f);
            }
            draw_list->AddText(font, font_size,
                ImVec2(label_x, vah_px.y - font_size * 0.5f),
                IM_COL32(100, 150, 210, 200), "VAH");

            ImVec2 val_px = ImPlot::PlotToPixels(sess_start, sess.val);
            for (float x = profile_right_x; x < line_end_x; x += 6.0f) {
                float x2 = std::min(x + 3.0f, line_end_x);
                draw_list->AddLine(ImVec2(x, val_px.y), ImVec2(x2, val_px.y),
                                   IM_COL32(100, 150, 210, 180), 1.0f);
            }
            draw_list->AddText(font, font_size,
                ImVec2(label_x, val_px.y - font_size * 0.5f),
                IM_COL32(100, 150, 210, 200), "VAL");
        }

        // ── Poor High / Low (solid white line + label) ───────────
        if (tpo.show_poor_high_low) {
            if (sess.has_poor_high) {
                ImVec2 ph = ImPlot::PlotToPixels(sess_start, sess.session_high);
                draw_list->AddLine(
                    ImVec2(profile_right_x, ph.y),
                    ImVec2(line_end_x, ph.y),
                    IM_COL32(255, 255, 255, 220), 1.0f);
                draw_list->AddText(font, font_size,
                    ImVec2(label_x, ph.y - font_size * 0.5f),
                    IM_COL32(255, 255, 255, 255), "PH");
            }
            if (sess.has_poor_low) {
                ImVec2 pl = ImPlot::PlotToPixels(sess_start, sess.session_low);
                draw_list->AddLine(
                    ImVec2(profile_right_x, pl.y),
                    ImVec2(line_end_x, pl.y),
                    IM_COL32(255, 255, 255, 220), 1.0f);
                draw_list->AddText(font, font_size,
                    ImVec2(label_x, pl.y - font_size * 0.5f),
                    IM_COL32(255, 255, 255, 255), "PL");
            }
        }

        // ── Session boundary line ─────────────────────────────────
        {
            const ImVec2 plot_pos = ImPlot::GetPlotPos();
            const ImVec2 plot_size = ImPlot::GetPlotSize();
            draw_list->AddLine(
                ImVec2(px_sess_start.x, plot_pos.y),
                ImVec2(px_sess_start.x, plot_pos.y + plot_size.y),
                IM_COL32(80, 80, 100, 100), 1.0f);
        }

        // ── Hover detection ──────────────────────────────────────
        // Profile bounding box: from session start to rightmost block edge
        {
            ImVec2 box_tl = ImPlot::PlotToPixels(sess_start, sess.session_high);
            box_tl.x = profile_left_x;
            ImVec2 box_br = ImPlot::PlotToPixels(sess_start, sess.session_low);
            box_br.x = profile_right_x;

            ImVec2 mouse = ImGui::GetMousePos();
            if (mouse.x >= box_tl.x && mouse.x <= box_br.x &&
                mouse.y >= box_tl.y && mouse.y <= box_br.y) {
                tpo_hovered_session_ = static_cast<int>(si);

                // White outline around hovered session
                draw_list->AddRect(
                    ImVec2(box_tl.x - 1.0f, box_tl.y - 1.0f),
                    ImVec2(box_br.x + 1.0f, box_br.y + 1.0f),
                    IM_COL32(220, 220, 220, 160), 0.0f, 0, 1.5f);

                // Right-click opens context menu
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
                    ImPlot::IsPlotHovered()) {
                    tpo_context_session_ = static_cast<int>(si);
                    ImGui::OpenPopup("tpo_session_context");
                }
            }
        }
    }

    ImPlot::PopPlotClipRect();

    // ── TPO Session Context Menu (must be outside clip rect) ─────
    if (Theme::begin_popup("tpo_session_context")) {
        int ctx_si = tpo_context_session_;
        if (ctx_si >= 0 && ctx_si < static_cast<int>(sessions->size())) {
            const auto& ctx_sess = (*sessions)[ctx_si];

            if (ImGui::Selectable("Expand")) {
                tpo.toggle_expand(pair_.symbol, ctx_si);
            }

            // Merge right - greyed out if rightmost session
            bool can_merge_right = (ctx_si + 1 < static_cast<int>(sessions->size()));
            if (!can_merge_right) ImGui::BeginDisabled();
            if (ImGui::Selectable("Merge right \xe2\x86\x92|")) {
                // TODO: Phase 2 - merge sessions
            }
            if (!can_merge_right) ImGui::EndDisabled();

            // Merge left - greyed out if leftmost session
            bool can_merge_left = (ctx_si > 0);
            if (!can_merge_left) ImGui::BeginDisabled();
            if (ImGui::Selectable("Merge left |\xe2\x86\x90")) {
                // TODO: Phase 2 - merge sessions
            }
            if (!can_merge_left) ImGui::EndDisabled();

            if (ImGui::BeginMenu("Prices")) {
                char buf[64];
                snprintf(buf, sizeof(buf), "POC: %.1f", ctx_sess.poc_price);
                ImGui::TextDisabled("%s", buf);
                snprintf(buf, sizeof(buf), "VAH: %.1f", ctx_sess.vah);
                ImGui::TextDisabled("%s", buf);
                snprintf(buf, sizeof(buf), "VAL: %.1f", ctx_sess.val);
                ImGui::TextDisabled("%s", buf);
                snprintf(buf, sizeof(buf), "High: %.1f", ctx_sess.session_high);
                ImGui::TextDisabled("%s", buf);
                snprintf(buf, sizeof(buf), "Low: %.1f", ctx_sess.session_low);
                ImGui::TextDisabled("%s", buf);
                ImGui::EndMenu();
            }

            if (ImGui::Selectable("Reset chart")) {
                tpo.clear(pair_.symbol);
            }
        }
        ImGui::EndPopup();
    }

    // Reset hover if mouse not over any session this frame
    if (tpo_hovered_session_ >= 0) {
        // Will be set again next frame if still hovering
        tpo_hovered_session_ = -1;
    }
}
