#include "chart_widget.h"
#include "core/candle_manager.h"
#include "replayer/replay_manager.h"
#include "rendering/theme.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

void ChartWidget::update_reference_context() {
    if (!session_vwap_ && !previous_day_ && !previous_week_ && !vwap_anchor_ms_) return;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const double seconds = std::chrono::duration<double>(now).count();
    const auto& replay = ctx_.replay_mgr();
    const int64_t asof = replay.is_active() ? replay.interpolated_time_ms() :
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    const auto* cm_ptr = &candles();
    const auto tf_now = cm_ptr->timeframe_seconds();
    if (reference_manager_ != cm_ptr || reference_tf_ != tf_now) {
        reference_manager_ = cm_ptr; reference_tf_ = tf_now; reference_update_time_ = -1;
    }
    // Rewind invalidates immediately; regular data corrections refresh at 4 Hz.
    if (seconds - reference_update_time_ < 0.25 && asof >= reference_asof_) return;
    reference_update_time_ = seconds; reference_asof_ = asof;
    const auto& cm = candles();
    const auto tf = cm.timeframe_seconds() * 1000;
    const auto day = reference_context::day_start(asof);
    const auto week = reference_context::week_start(asof);
    // Quiet stretches (no trades, no candle) up to an hour do not break a level (reference_context::kQuietGapMs).
    const int64_t gap = reference_context::kQuietGapMs;
    if (session_vwap_) reference_context::vwap(cm.candles(), day, asof, tf, session_vwap_data_, gap);
    if (vwap_anchor_ms_) {
        // Keep the chosen epoch fixed. A coarser timeframe that cannot express
        // it must not silently include trades from before the anchor.
        reference_context::vwap(cm.candles(), vwap_anchor_ms_, asof, tf, anchored_vwap_data_, gap);
    }
    if (previous_day_) previous_day_data_ = reference_context::levels(
        cm.candles(), day - reference_context::day_ms, day, tf, gap);
    if (previous_week_) previous_week_data_ = reference_context::levels(
        cm.candles(), week - 7 * reference_context::day_ms, week, tf, gap);
    // A level switched on whose period starts before the loaded history asks once for the candles it needs: the
    // same bounded read as the menu's "Load earlier candles", sized to the period, at most 20160 candles (two weeks
    // of 1m). James 2026-10-11: the levels "don't do anything" on a 1m chart, whose first load is two days.
    int64_t need = INT64_MAX;
    if (session_vwap_ && !session_vwap_data_.complete) need = std::min(need, day);
    if (vwap_anchor_ms_ && !anchored_vwap_data_.complete) need = std::min(need, vwap_anchor_ms_);
    if (previous_day_ && !previous_day_data_.complete) need = std::min(need, day - reference_context::day_ms);
    if (previous_week_ && !previous_week_data_.complete) need = std::min(need, week - 7 * reference_context::day_ms);
    const auto& bars = cm.candles();
    if (need != INT64_MAX && tf > 0 && !bars.empty() && bars.front().timestamp_ms > need && !cm.is_loading() &&
        reference_autoload_from_ != need) {
        reference_autoload_from_ = need;
        candles().request_historical(static_cast<size_t>(std::min<int64_t>(20160, (asof - need) / tf + 2)), asof);
    }
}

void ChartWidget::render_reference_context() {
    if (!ct_allows_time_overlays(chart_type_)) return;
    const auto line = [&](const char* label, const reference_context::Series& data, ImVec4 color) {
        if (data.times.empty()) return;
        ImPlotSpec style; style.LineColor = color; style.LineWeight = 1.5f;
        ImPlot::PlotLine(label, data.times.data(), data.values.data(), static_cast<int>(data.times.size()), style);
    };
    // VWAP in amber (the usual VWAP colour, distinct from price); the anchored one neutral.
    if (session_vwap_) line("Session VWAP (closed HLC3)", session_vwap_data_, Theme::Tokens::WARN);
    if (vwap_anchor_ms_) line("Anchored VWAP (closed HLC3)", anchored_vwap_data_, Theme::Tokens::TX2);
    // Previous day / week high, low and close: dashed lines over the current period with a price tag at their right
    // end (design v3, 2026-10-11: the thin grey lines were easy to miss). Tags never overlap; a later one steps aside.
    auto* dl = ImPlot::GetPlotDrawList();
    const auto pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    std::vector<float> tag_ys;
    ImPlot::PushPlotClipRect();
    const auto levels = [&](const char* prefix, const reference_context::Levels& data, int64_t start) {
        if (!data.complete) return;
        const auto bounds = ImPlot::GetPlotLimits();
        const double x0 = std::max(bounds.X.Min, static_cast<double>(start));
        const double x1 = std::min(bounds.X.Max, static_cast<double>(reference_asof_));
        if (x1 < x0) return;
        const double values[] = {data.high, data.low, data.close};
        const char* suffixes[] = {"H", "L", "C"};
        for (int i = 0; i < 3; ++i) {
            const ImVec2 a = ImPlot::PlotToPixels(x0, values[i]), b = ImPlot::PlotToPixels(x1, values[i]);
            const float y = std::round(a.y) + 0.5f;
            if (y < pos.y || y > pos.y + size.y) continue;
            const ImU32 col = Theme::u32(i == 2 ? Theme::Tokens::TX3 : Theme::Tokens::TX2, 0.85f);
            for (float x = a.x; x < b.x; x += 9.0f) dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + 5.0f, b.x), y), col, 1.0f);
            char label[48], price[32];
            snprintf(price, sizeof(price), fmt_.price_fmt, values[i]);
            snprintf(label, sizeof(label), "%s%s %s", prefix, suffixes[i], price);
            ImGui::PushFont(Theme::Fonts::mono_sm());
            const ImVec2 ts = ImGui::CalcTextSize(label);
            float ty = y - ts.y * 0.5f - 2.0f;
            for (const float u : tag_ys)
                if (std::abs(u - ty) < ts.y + 4.0f) ty = u + (ty >= u ? 1.0f : -1.0f) * (ts.y + 4.0f);
            tag_ys.push_back(ty);
            const ImVec2 p0(std::min(b.x, pos.x + size.x) - ts.x - 12.0f, ty), p1(p0.x + ts.x + 10.0f, ty + ts.y + 4.0f);
            dl->AddRectFilled(p0, p1, Theme::u32(Theme::Tokens::PANEL, 0.92f), Theme::Radius::R1);
            dl->AddRect(p0, p1, Theme::u32(Theme::Tokens::BD2), Theme::Radius::R1);
            dl->AddText(ImVec2(p0.x + 5.0f, p0.y + 2.0f), Theme::u32(i == 2 ? Theme::Tokens::TX2 : Theme::Tokens::TX1), label);
            ImGui::PopFont();
        }
    };
    if (previous_day_) levels("PD", previous_day_data_, reference_context::day_start(reference_asof_));
    if (previous_week_) levels("PW", previous_week_data_, reference_context::week_start(reference_asof_));
    ImPlot::PopPlotClipRect();
}
