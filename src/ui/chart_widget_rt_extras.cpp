// Real-time extras: the hover readout, walls and pulled walls, and the CVD
// lane. The logic lives in core/rt_walls.h and core/rt_cvd.h; this file reads
// the chart's real-time state and draws. Automatic heatmap contrast lives in
// the renderer (core/rt_contrast.h); the chart only passes its switch.
#include "ui/chart_widget.h"
#include "core/i18n.h"
#include "core/candle_manager.h"
#include "core/display_time_zone.h"
#include "core/entitlements.h"
#include "core/symbol_metadata.h"
#include "core/ui_test_marks.h"
#include "core/ticker_manager.h"
#include "rendering/realtime_bubble.h"
#include "rendering/theme.h"
#include "replayer/replay_manager.h"
#include "imgui_internal.h"
#include "types/frame_profiler.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
// The CVD lane's height, axis included.
constexpr float kCvdLaneHeight = 72.0f;
// Wall depth samples folded per frame: a long backlog (entering RT with five
// minutes retained) spreads over a few frames instead of one long one.
constexpr int kWallSamplesPerFrame = 256;
// Room a bubble hit-test looks around the pointer, past the 12px largest radius.
constexpr float kBubbleReachPx = 14.0f;

// Base quantity, short: 0.42, 3.2, 12.4, 640, 1,240, 2.35M.
void format_base(double v, char* out, size_t n, bool sign = false) {
    const double a = std::abs(v);
    const char* s = !sign || a == 0 ? "" : v > 0 ? "+" : "-";
    if (!std::isfinite(a)) { std::snprintf(out, n, "-"); return; }
    const long long whole = std::llround(a);
    if (a >= 999999.5) std::snprintf(out, n, "%s%.2fM", s, a / 1e6);
    else if (a >= 999.5) std::snprintf(out, n, "%s%lld,%03lld", s, whole / 1000, whole % 1000);
    else if (a >= 99.95) std::snprintf(out, n, "%s%lld", s, whole);
    else if (a >= 1) std::snprintf(out, n, "%s%.1f", s, a);
    else if (a > 0) std::snprintf(out, n, "%s%.2g", s, a);
    else std::snprintf(out, n, "0");
}

// Dollars, short: $950, $12.4K, $324K, $1.25M, $2.10B.
void format_usd(double v, char* out, size_t n) {
    if (!(v >= 0) || !std::isfinite(v)) { std::snprintf(out, n, "-"); return; }
    if (v >= 1e9) std::snprintf(out, n, "$%.2fB", v / 1e9);
    else if (v >= 1e6) std::snprintf(out, n, "$%.2fM", v / 1e6);
    else if (v >= 99950) std::snprintf(out, n, "$%.0fK", v / 1e3);
    else if (v >= 999.5) std::snprintf(out, n, "$%.1fK", v / 1e3);
    else std::snprintf(out, n, "$%.0f", v);
}

// Row prices as the RT ladder prints them: centres, one decimal past the tick.
void format_row_price(double native_tick, double price, char* out, size_t n) {
    PriceFormatter::from_tick_and_step(native_tick * 0.1, 1).format_price(out, n, price);
}

}

float ChartWidget::realtime_cvd_lane_height() const {
    return rt_mode_ && rt_cvd_enabled_ ? kCvdLaneHeight : 0.0f;
}

int ChartWidget::realtime_extra_layers_on() const {
    return (rt_walls_enabled_ ? 1 : 0) + (rt_cvd_enabled_ ? 1 : 0);
}

void ChartWidget::reset_realtime_extras() {
    rt_walls_.reset();
    rt_walls_serial_ = 0;
    rt_wall_marks_.clear();
    rt_event_feed_.reset();
    last_refill_alert_ms_ = 0;
}

// Layers rows: Walls (with Pulled walls under it) in ORDER BOOK SIGNALS, CVD in REAL-TIME VIEW. Free wherever
// real-time itself is available.
void ChartWidget::render_realtime_wall_layer() {
    if (Theme::menu_layer(i18n::tr("Walls", i18n::fa_l_walls), rt_walls_enabled_, false, "Alt+0")) rt_walls_enabled_ = !rt_walls_enabled_;
    if (ImGui::IsItemHovered())
        Theme::tooltip("The largest resting orders away from the touch: at least 5x the usual level\nand larger than what the touch holds, standing 3 s or more. Marked at the right edge.");
    if (!rt_walls_enabled_) return;
    Theme::begin_menu_detail();
    if (Theme::menu_toggle(i18n::tr("Pulled walls", i18n::fa_l_pulled_walls), rt_pulls_enabled_)) rt_pulls_enabled_ = !rt_pulls_enabled_;
    if (ImGui::IsItemHovered())
        Theme::tooltip("A wall that stood, then mostly vanished within a second near price\nwithout trading there. Larger squares mean more removed depth.\nSignificant and recent pulls stay visible, up to 30 minutes or 256 retained events.");
    if (Theme::menu_toggle(i18n::tr("Active walls table", i18n::fa_l_walls_table), rt_walls_table_enabled_)) rt_walls_table_enabled_ = !rt_walls_table_enabled_;
    if (ImGui::IsItemHovered())
        Theme::tooltip("A compact table of the walls being tracked, top-right inside the plot:\nprice, size, multiple, age, distance from mid and how often the price traded into them.");
    Theme::end_menu_detail();
}

void ChartWidget::render_realtime_cvd_layer() {
    if (Theme::menu_layer("CVD", rt_cvd_enabled_, false, "Alt+6")) rt_cvd_enabled_ = !rt_cvd_enabled_;
    if (ImGui::IsItemHovered())
        Theme::tooltip("Buy minus sell volume over the visible time, from zero at the left edge.");
}

const char* ChartWidget::realtime_base_asset() {
    if (!rt_base_asset_[0]) {
        const auto* meta = SymbolRegistry::instance().get(pair_.exchange, pair_.symbol);
        const std::string base = meta && !meta->base_asset.empty() ? meta->base_asset : Entitlements::coin_label(pair_.symbol);
        std::snprintf(rt_base_asset_, sizeof(rt_base_asset_), "%s", base.c_str());
    }
    return rt_base_asset_;
}

// Fold new depth samples into the wall tracker. Same samples, same order in
// live and replay; the visible prices pick the median levels.
void ChartWidget::update_realtime_walls() {
    // Clear history (a new generation of the same archive) starts walls again
    // from new samples. A newly acquired archive is not a reset: its context
    // change already went through on_rewind.
    const uint64_t generation = rt_archive_ ? rt_archive_->generation : 0;
    if (rt_archive_.get() != rt_walls_archive_) {
        rt_walls_archive_ = rt_archive_.get();
        rt_walls_archive_generation_ = generation;
    } else if (generation != rt_walls_archive_generation_) {
        reset_realtime_extras();
        rt_walls_archive_generation_ = generation;
        if (!rt_samples_.empty()) rt_walls_serial_ = rt_samples_.back()->serial;
    }
    // The working window restarted (rewind, context swap): start again.
    if (!rt_samples_.empty() && rt_samples_.back()->serial < rt_walls_serial_) reset_realtime_extras();
    const double native = rt_renderer_ && rt_renderer_->get_native_bucket_size() > 0
        ? rt_renderer_->get_native_bucket_size() : tick_size_;
    const rt_walls::Grid grid{native, std::max(1, rt_effective_multiplier_)};
    const auto limits = ImPlot::GetPlotLimits();
    auto it = std::upper_bound(rt_samples_.begin(), rt_samples_.end(), rt_walls_serial_,
        [](uint64_t serial, const RealtimeDepthHistory::SamplePtr& s) { return serial < s->serial; });
    for (int folded = 0; it != rt_samples_.end() && folded < kWallSamplesPerFrame; ++it, ++folded) {
        if ((*it)->timestamp_ms > rt_clock_ms_) break;
        rt_walls_.observe(**it, grid, limits.Y.Min, limits.Y.Max);
        rt_walls_serial_ = (*it)->serial;
    }
    // Depth is written as it arrives; trades reach the chart through the main
    // thread's dispatch queue, which a slow or throttled frame leaves behind. A
    // drop is decided only once the trades that could explain it are delivered:
    // through the clock when nothing is queued, else through the newest one in.
    StreamManager& sm = ctx_.stream_mgr();
    if (const uint64_t dropped = sm.dispatch_dropped(); dropped != rt_walls_dropped_) {
        rt_walls_dropped_ = dropped;
        rt_walls_.trades_lost();
    }
    const auto& trades = candles().realtime_trades().trades();
    const int64_t delivered = !sm.dispatch_backlogged() ? rt_clock_ms_
        : trades.empty() ? 0 : std::min(rt_clock_ms_, trades.back().timestamp_ms);
    rt_walls_.settle(rt_clock_ms_, trades, delivered);
}

// The largest few walls per side away from the touch, marked at the plot's right
// edge (the ladder beside it marks the same rows with their size); pulls as a
// small hollow square where the wall went. Above trade bubbles, under refill
// rails and the quotes. Called inside the plot clip rect.
void ChartWidget::render_realtime_walls(ImDrawList* dl) {
    ProfileScope profile("RT walls");
    update_realtime_walls();
    rt_wall_marks_.clear();
    rt_wall_ladder_.count = 0;
    rt_dom_frame_.walls = nullptr;
    if (!rt_walls_enabled_) return;
    const auto limits = ImPlot::GetPlotLimits();
    const ImVec2 pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    const double native = rt_renderer_ && rt_renderer_->get_native_bucket_size() > 0
        ? rt_renderer_->get_native_bucket_size() : tick_size_;
    const rt_walls::Grid grid{native, std::max(1, rt_effective_multiplier_)};
    if (size.x <= 0 || size.y <= 0 || !grid.valid()) return;
    constexpr size_t capacity = rt_walls::kMaxShownPerSide * 2 + rt_walls::kMaxPullsDrawn;
    static_assert(std::tuple_size_v<decltype(RealtimeWallMarks::marks)> == rt_walls::kMaxShownPerSide * 2);
    if (rt_wall_marks_.capacity() < capacity) rt_wall_marks_.reserve(capacity);
    const ImU32 halo = Theme::u32(Theme::Tokens::BASE, 0.9f);
    const auto row_of = [&](RtWallMark& m) {
        const int64_t row = grid.index(m.price);
        m.low = grid.low(row); m.high = grid.high(row);
    };

    // Walls: a side-coloured bar at the right edge on the wall's row, its size beside
    // it when the ladder is hidden. The hover outlines the time it has stood.
    std::array<rt_walls::Wall, rt_walls::kMaxShownPerSide * 2> shown{};
    const size_t n = rt_walls_.shown(rt_clock_ms_, limits.Y.Min, limits.Y.Max, shown);
    const float right = pos.x + size.x;
    std::array<float, rt_walls::kMaxShownPerSide * 2> placed{};
    size_t placed_count = 0;
    const float line = ImGui::GetTextLineHeight();
    for (size_t i = 0; i < n; ++i) {
        const auto& w = shown[i];
        RtWallMark m;
        m.price = w.price; m.size = w.size; m.multiple = w.multiple;
        m.start_ms = w.start_ms; m.end_ms = rt_clock_ms_; m.bid = w.bid;
        row_of(m);
        rt_wall_marks_.push_back(m);
        rt_wall_ladder_.marks[size_t(rt_wall_ladder_.count++)] = {w.price, w.bid};
        // A wall stands now: its edge mark only while the view reaches the clock (the ladder always shows now).
        if (limits.X.Max < double(rt_clock_ms_)) continue;
        const ImU32 ink = Theme::u32(w.bid ? Theme::Tokens::UP : Theme::Tokens::DOWN);
        float y0 = ImPlot::PlotToPixels(0, m.high).y, y1 = ImPlot::PlotToPixels(0, m.low).y;
        if (y1 - y0 < 4.0f) { const float c = (y0 + y1) * 0.5f; y0 = c - 2.0f; y1 = c + 2.0f; }
        dl->AddRectFilled(ImVec2(right - 5.0f, y0 - 1.0f), ImVec2(right, y1 + 1.0f), halo);
        dl->AddRectFilled(ImVec2(right - 4.0f, y0), ImVec2(right, y1), ink);
        if (ui_test::enabled()) ui_test::mark_rect(std::string("rt.wall.") + (w.bid ? "bid." : "ask.") + std::to_string(i),
                                                   ImVec2(right - 4.0f, y0), ImVec2(right, y1));
        if (rt_dom_linked_) continue;
        char qty[32], label[48];
        format_base(w.size, qty, sizeof(qty));
        std::snprintf(label, sizeof(label), "%s %s", qty, realtime_base_asset());
        const ImVec2 text = ImGui::CalcTextSize(label);
        const float x = right - 8.0f - text.x;
        const float y = std::round((y0 + y1 - text.y) * 0.5f);
        if (x < pos.x + size.x * 0.5f || y < pos.y || y + text.y > pos.y + size.y) continue;
        bool overlaps = false;
        for (size_t k = 0; k < placed_count; ++k) overlaps |= std::abs(placed[k] - y) < line + 2;
        if (overlaps) continue;
        placed[placed_count++] = y;
        dl->AddRectFilled(ImVec2(x - 4, y - 1), ImVec2(x + text.x + 3, y + text.y + 1), Theme::u32(Theme::Tokens::BASE, 0.82f), Theme::Radius::R1);
        dl->AddText(ImVec2(x, y), Theme::u32(Theme::Tokens::TX1), label);
    }
    if (rt_wall_ladder_.count) rt_dom_frame_.walls = &rt_wall_ladder_;

    // Bounded, size-aware pulls: preserve the significant events and leave
    // room for fresh ones. Pixel collisions include each glyph's actual size.
    if (!rt_pulls_enabled_) return;
    std::array<rt_walls::PullMarker, rt_walls::kMaxPullsDrawn> glyphs{};
    const size_t glyph_count = rt_walls::select_pulls(rt_walls_, rt_clock_ms_, limits.X.Min, limits.X.Max,
        limits.Y.Min, limits.Y.Max, size.x, [&](const rt_walls::Pull& p) {
            const int64_t row = grid.index(p.wall.price);
            const ImVec2 at = ImPlot::PlotToPixels(double(p.wall.end_ms), (grid.low(row) + grid.high(row)) * 0.5);
            return std::array<float, 2>{std::round(at.x), std::round(at.y)};
        }, glyphs);
    for (size_t i = 0; i < glyph_count; ++i) {
        const auto& g = glyphs[i];
        const auto& p = *g.pull;
        RtWallMark m;
        m.price = p.wall.price; m.size = p.vanished; m.multiple = p.wall.peak_multiple; m.traded = p.traded;
        m.start_ms = p.wall.start_ms; m.end_ms = p.wall.end_ms; m.bid = p.wall.bid; m.pulled = true;
        row_of(m);
        m.at = ImVec2(g.x, g.y); m.half_px = g.diameter * 0.5f;
        rt_wall_marks_.push_back(m);
        const ImU32 ink = Theme::u32(m.bid ? Theme::Tokens::UP : Theme::Tokens::DOWN, g.alpha);
        const ImVec2 a(g.x - m.half_px, g.y - m.half_px), b(g.x + m.half_px, g.y + m.half_px);
        dl->AddRect(a, b, halo, 0, 0, 3.0f);
        dl->AddRect(a, b, ink, 0, 0, 1.25f);
        if (ui_test::enabled()) ui_test::mark_rect("rt.pull." + std::to_string(i), a, b);
    }
}

// Deliberate marker inspection only: stop briefly, release pan/zoom buttons,
// and let the first overlay under the pointer own this frame's tooltip.
bool ChartWidget::realtime_hover_ready() const {
    const ImGuiContext& g = *GImGui;
    if (!ImPlot::IsPlotHovered() || ctx_.replay_mgr().is_loading() || drawing_layer_.captures_mouse()) return false;
    for (bool down : ImGui::GetIO().MouseDown) if (down) return false;
    if (g.TooltipPreviousWindow && g.TooltipPreviousWindow->Active) return false;
    if (Theme::chart_chip_covers(ImGui::GetIO().MousePos)) return false;
    return ImGui::GetIO().KeyAlt || g.MouseStationaryTimer >= 0.20f;
}

// Visible trade/pull/wall marks get concise details. Plain depth is opt-in (Alt).
void ChartWidget::render_realtime_hover() {
    ProfileScope profile("RT hover");
    if (!realtime_hover_ready()) return;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const auto limits = ImPlot::GetPlotLimits();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    const ImPlotPoint at = ImPlot::GetPlotMousePos();
    const char* base = realtime_base_asset();
    char qty[32], minor_qty[32], usd[32], price[48], line[128];

    // Trade bubbles: the largest one the pointer is on, the nearest on a tie.
    const double minimum = rt_auto_bubbles_
        ? (rt_bubble_scale_.settled() ? rt_bubble_scale_.minimum() : 0) : rt_min_notional_;
    if (!ImGui::GetIO().KeyAlt && rt_bubbles_ && rt_archive_ && minimum > 0 && plot_size.x > 0) {
        const double reach = std::max(kBubbleReachPx, rt_bubble_cap_ + 2.0f) * limits.X.Size() / plot_size.x;
        RealtimeBubbleHistory::Bubble best{};
        float best_d2 = 0;
        bool found = false;
        rt_archive_->bubbles().for_each(int64_t(at.x - reach), int64_t(at.x + reach) + 1, rt_clock_ms_,
            [&](const RealtimeBubbleHistory::Bubble& b) {
                if (b.value() < minimum) return;
                const float r = RealtimeBubble::radius(b.value(), minimum, rt_bubble_cap_) + 1.0f;
                const ImVec2 c = ImPlot::PlotToPixels(double(b.timestamp_ms), b.price);
                const float dx = mouse.x - c.x, dy = mouse.y - c.y, d2 = dx * dx + dy * dy;
                if (d2 > r * r) return;
                if (!found || b.value() > best.value() || (b.value() == best.value() && d2 < best_d2)) {
                    best = b; best_d2 = d2; found = true;
                }
            });
        if (found) {
            const bool buy = best.buy_led();
            format_base((buy ? best.buy : best.sell) / best.price, qty, sizeof(qty));
            format_usd(best.value(), usd, sizeof(usd));
            if (best.buy > 0 && best.sell > 0) {
                format_base((buy ? best.sell : best.buy) / best.price, minor_qty, sizeof(minor_qty));
                std::snprintf(line, sizeof(line), "%s %s / %s %s %s \xc2\xb7 %s", buy ? "Buy" : "Sell", qty,
                    buy ? "sell" : "buy", minor_qty, base, usd);
            } else std::snprintf(line, sizeof(line), "%s %s %s \xc2\xb7 %s", buy ? "Buy" : "Sell", qty, base, usd);
            char time[40] = {}, when[48];
            DisplayTimeZone::instance().format(best.timestamp_ms, TimeZoneFormat::TimeSeconds, time, sizeof(time));
            std::snprintf(when, sizeof(when), "%s.%d", time, int(best.timestamp_ms % 1000 / 100));
            Theme::begin_tooltip();
            ImGui::TextColored(buy ? Theme::Tokens::UP : Theme::Tokens::DOWN, "%s", line);
            ImGui::TextDisabled("%s", when);
            Theme::end_tooltip();
            return;
        }
    }

    // Pull squares and visible wall edge marks only, largest first.
    const double native = rt_dom_frame_.native_tick > 0 ? rt_dom_frame_.native_tick : tick_size_;
    // Estimated liquidation bands (S3): the first band under the pointer.
    if (!ImGui::GetIO().KeyAlt && rt_liq_levels_enabled_) {
        for (const auto& c : liq_estimator_.clusters()) {
            if (c.price < limits.Y.Min || c.price > limits.Y.Max) continue;
            const float strength = float(std::min(1.0, c.share * 4.0));
            const float half = 2.5f + 9.0f * strength;
            const float y = ImPlot::PlotToPixels(0, c.price).y;
            if (std::abs(mouse.y - y) > half + 2) continue;
            char usd[24], price[48];
            format_usd(c.usd, usd, sizeof(usd));
            fmt_.format_price(price, sizeof(price), c.price);
            Theme::begin_tooltip();
            ImGui::TextColored(c.longs ? Theme::Tokens::DOWN : Theme::Tokens::UP, "Estimated %s liquidations ~%s",
                c.longs ? "long" : "short", usd);
            ImGui::TextDisabled("near %s \xc2\xb7 %.0fx dominant \xc2\xb7 %.1f%% of OI", price, double(c.tier), 100.0 * c.share);
            ImGui::TextDisabled("An estimate from open interest + the recent tape, not real orders.");
            Theme::end_tooltip();
            return;
        }
    }
    if (!ImGui::GetIO().KeyAlt && rt_walls_enabled_ && !rt_wall_marks_.empty()) {
        const RtWallMark* hit = nullptr;
        for (const auto& m : rt_wall_marks_) {
            if (m.pulled) {
                if (std::abs(mouse.x - m.at.x) <= m.half_px + 2 && std::abs(mouse.y - m.at.y) <= m.half_px + 2) { hit = &m; break; }
                continue;
            }
            if (limits.X.Max < double(rt_clock_ms_)) continue; // No edge mark in a past view.
            const float right = ImPlot::GetPlotPos().x + plot_size.x;
            const float y0 = ImPlot::PlotToPixels(0, m.high).y, y1 = ImPlot::PlotToPixels(0, m.low).y;
            const float center = (y0 + y1) * 0.5f, half = std::max(2.0f, (y1 - y0) * 0.5f);
            if (mouse.x < right - 7 || mouse.x > right || std::abs(mouse.y - center) > half + 2) continue;
            if (!hit || (!hit->pulled && m.size > hit->size)) hit = &m;
        }
        if (hit) {
            format_base(hit->size, qty, sizeof(qty));
            fmt_.format_price(price, sizeof(price), hit->price);
            char stood[32];
            const int64_t seconds = std::max<int64_t>(0, (hit->end_ms - hit->start_ms) / 1000);
            if (seconds >= 60) std::snprintf(stood, sizeof(stood), "%lldm %02llds", (long long)(seconds / 60), (long long)(seconds % 60));
            else std::snprintf(stood, sizeof(stood), "%llds", (long long)seconds);
            Theme::begin_tooltip();
            ImGui::TextColored(hit->bid ? Theme::Tokens::UP : Theme::Tokens::DOWN, "%s %s %s at %s",
                hit->pulled ? (hit->bid ? "Bid pulled" : "Ask pulled") : "Wall", qty, base, price);
            if (hit->pulled) {
                char when[40];
                DisplayTimeZone::instance().format(hit->end_ms, TimeZoneFormat::TimeSeconds, when, sizeof(when));
                format_usd(hit->size * hit->price, usd, sizeof(usd));
                ImGui::TextDisabled("%s removed at %s", usd, when);
                if (hit->traded > 0) {
                    format_base(hit->traded, minor_qty, sizeof(minor_qty));
                    ImGui::TextDisabled("Stood %s, gone within a second; %s traded there", stood, minor_qty);
                } else ImGui::TextDisabled("Stood %s, gone within a second without trading", stood);
            } else ImGui::TextDisabled("%.1f\xc3\x97 the usual level \xc2\xb7 standing %s", hit->multiple, stood);
            Theme::end_tooltip();
            if (!hit->pulled) {
                // The time it has stood, outlined on its row.
                ImDrawList* dl = ImPlot::GetPlotDrawList();
                ImPlot::PushPlotClipRect();
                const ImVec2 a = ImPlot::PlotToPixels(double(hit->start_ms), hit->high);
                const ImVec2 b = ImPlot::PlotToPixels(double(hit->end_ms), hit->low);
                dl->AddRect(a, b, Theme::u32(Theme::Tokens::BASE, 0.9f), 0, 0, 3.0f);
                dl->AddRect(a, b, Theme::u32(hit->bid ? Theme::Tokens::UP : Theme::Tokens::DOWN, 0.85f), 0, 0, 1.25f);
                ImPlot::PopPlotClipRect();
            }
            return;
        }
    }

    // Resting depth in the hovered heatmap row, from the sample drawn there.
    if (!ImGui::GetIO().KeyAlt || !rt_depth_enabled_ || !(native > 0)) return;
    const auto& samples = realtime_samples();
    const RealtimeDepthHistory::Sample* sample = nullptr;
    if (at.x > double(rt_clock_ms_)) {
        // The right margin holds the current book while it is projected.
        if (rt_extend_depth_ && (!rt_history_view_ || realtime_live_edge()) && rt_dom_frame_.fresh() &&
            at.x <= realtime_projection_until(rt_clock_ms_, rt_span_ms_))
            sample = rt_dom_frame_.book.get();
    } else if (!samples.empty()) {
        const auto next = std::upper_bound(samples.begin(), samples.end(), int64_t(at.x),
            [](int64_t t, const RealtimeDepthHistory::SamplePtr& s) { return t < s->timestamp_ms; });
        if (next != samples.begin()) {
            const auto& shown = *std::prev(next);
            const int64_t bin = rt_history_view_ ? std::max<int64_t>(100, rt_loaded_step_) : 100;
            // A new segment after a gap: the gap holds no recorded depth.
            const bool gap = next != samples.end() && (*next)->segment_start && int64_t(at.x) >= shown->timestamp_ms + bin;
            if (!gap && int64_t(at.x) - shown->timestamp_ms <= 15000) sample = shown.get();
        }
    }
    if (!sample) return;
    const int ticks = std::max(1, rt_dom_frame_.bucket_ticks);
    const auto row_of = [&](double p) { return int64_t(std::floor((p / native + 1e-7) / ticks)); };
    const int64_t row = row_of(at.y);
    double bids = 0, asks = 0, value = 0;
    for (const auto& level : sample->levels) {
        if (row_of(level.price) != row || !(level.size > 0)) continue;
        (level.price <= sample->bid ? bids : asks) += level.size;
        value += level.size * level.price;
    }
    if (!(bids + asks > 0)) return;
    format_row_price(native, (double(row) + 0.5) * native * ticks, price, sizeof(price));
    format_base(bids + asks, qty, sizeof(qty));
    format_usd(value, usd, sizeof(usd));
    Theme::begin_tooltip();
    ImGui::TextUnformatted(price);
    ImGui::TextColored(asks == 0 ? Theme::Tokens::UP : bids == 0 ? Theme::Tokens::DOWN : Theme::Tokens::TX1,
        "%s %s resting \xc2\xb7 %s", qty, base, usd);
    Theme::end_tooltip();
}

// A short lane under the RT plot: buys minus sells across the visible time,
// zero at the left edge. Bounded: a column per two pixels, trades in view only.
void ChartWidget::render_realtime_cvd_lane() {
    const float height = realtime_cvd_lane_height();
    if (height <= 0) return;
    ImGui::PushID("RealtimeCVD");
    // The lowest pane owns the time labels.
    const bool labels = !(liquidation_activity_visible() && !rt_liq_collapsed_) && !(activity_visible() && !activity_collapsed_);
    const double from = last_visible_range_.X.Min, to = last_visible_range_.X.Max;
    const auto& trades = realtime_trades();
    // Two pixels per column keeps the step line light at any width.
    const int columns = int(std::clamp(ImGui::GetContentRegionAvail().x * 0.5f, 1.0f, float(RealtimeCvdSeries::kMaxColumns)));
    rt_cvd_.build(trades, from, to, rt_clock_ms_, columns);
    if (ImPlot::BeginPlot("##RealtimeCVD", ImVec2(-1, height),
            ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText | ImPlotFlags_NoInputs)) {
        ImPlot::SetupAxis(ImAxis_X1, nullptr, labels ? ImPlotAxisFlags_None : ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoTickMarks);
        ImPlot::SetupAxis(ImAxis_Y1, nullptr, ImPlotAxisFlags_Opposite | ImPlotAxisFlags_NoTickLabels);
        ImPlot::SetupAxisLimits(ImAxis_X1, from, to, ImGuiCond_Always);
        const double span = std::max(rt_cvd_.high() - rt_cvd_.low(), 1e-9);
        ImPlot::SetupAxisLimits(ImAxis_Y1, rt_cvd_.empty() ? -1.0 : rt_cvd_.low() - span * 0.15,
            rt_cvd_.empty() ? 1.0 : rt_cvd_.high() + span * 0.15, ImGuiCond_Always);
        if (labels) setup_time_axis_ticks(from, to);
        ImPlot::SetupFinish();
        ImDrawList* dl = ImPlot::GetPlotDrawList();
        const ImVec2 pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
        crosshair_state_.last_plot_max = ImVec2(pos.x + size.x, pos.y + size.y);
        const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y + 24));
        ImPlot::PushPlotClipRect();
        const float zero = ImPlot::PlotToPixels(0, 0).y;
        dl->AddLine(ImVec2(pos.x, zero), ImVec2(pos.x + size.x, zero), Theme::u32(Theme::Tokens::BD2));
        const ImU32 up = Theme::u32(Theme::Tokens::UP), down = Theme::u32(Theme::Tokens::DOWN);
        const ImU32 up_fill = Theme::u32(Theme::Tokens::UP, 0.14f), down_fill = Theme::u32(Theme::Tokens::DOWN, 0.14f);
        // A step line: runs of one value merge into one fill and one stroke
        // segment; the stroke splits where the sign (and so the tint) changes.
        auto& points = rt_cvd_points_;
        points.clear();
        if (points.capacity() < size_t(RealtimeCvdSeries::kMaxColumns) * 3) points.reserve(size_t(RealtimeCvdSeries::kMaxColumns) * 3);
        ImVec2 previous = ImPlot::PlotToPixels(from, 0);
        double level = 0;
        points.push_back(previous);
        auto stroke = [&](bool positive) {
            if (points.size() > 1) dl->AddPolyline(points.data(), int(points.size()), positive ? up : down, ImDrawFlags_None, 1.25f);
            points.clear();
        };
        for (int c = 0; c <= rt_cvd_.last_valid(); ++c) {
            const double v = rt_cvd_.column(c).value;
            if (v == level && c < rt_cvd_.last_valid()) continue;
            const ImVec2 point = ImPlot::PlotToPixels(std::min(rt_cvd_.column_end(c), double(rt_clock_ms_)), v);
            if (point.x > previous.x)
                dl->AddRectFilled(ImVec2(previous.x, std::min(previous.y, zero)), ImVec2(point.x, std::max(previous.y, zero)),
                    level >= 0 ? up_fill : down_fill);
            points.push_back(ImVec2(point.x, previous.y));
            if ((level >= 0) != (v >= 0)) {
                points.push_back(ImVec2(point.x, zero));
                stroke(level >= 0);
                points.push_back(ImVec2(point.x, zero));
            }
            points.push_back(point);
            previous = point; level = v;
        }
        stroke(level >= 0);
        char value[32], label[48];
        ImGui::PushFont(Theme::Fonts::mono_sm());
        dl->AddText(ImVec2(pos.x + 6, pos.y + 3), Theme::u32(Theme::Tokens::TX3), "CVD");
        if (!rt_cvd_.empty()) {
            format_base(rt_cvd_.end_value(), value, sizeof(value), true);
            std::snprintf(label, sizeof(label), "%s %s", value, realtime_base_asset());
            const ImVec2 text = ImGui::CalcTextSize(label);
            const float x = std::clamp(previous.x + 6, pos.x + 40, pos.x + size.x - text.x - 4);
            const float y = std::clamp(previous.y - text.y * 0.5f, pos.y + 2, pos.y + size.y - text.y - 2);
            dl->AddRectFilled(ImVec2(x - 3, y), ImVec2(x + text.x + 3, y + text.y), Theme::u32(Theme::Tokens::PANEL, 0.88f), Theme::Radius::R1);
            dl->AddText(ImVec2(x, y), rt_cvd_.end_value() >= 0 ? up : down, label);
        }
        ImGui::PopFont();
        ImPlot::PopPlotClipRect();
        if (hovered) {
            const double t = ImPlot::GetPlotMousePos().x;
            double v = 0;
            if (ImGui::GetIO().KeyAlt && !ImGui::IsAnyMouseDown() && rt_cvd_.value_at(t, v)) {
                format_base(v, value, sizeof(value), true);
                Theme::tooltip("CVD %s %s", value, realtime_base_asset());
            }
            crosshair_state_.is_active = true; crosshair_state_.chart_hovered = false; crosshair_state_.indicator_hovered = false;
            crosshair_state_.plot_pos = ImPlot::GetPlotMousePos();
            crosshair_state_.hovered_plot_min = pos; crosshair_state_.hovered_plot_max = ImVec2(pos.x + size.x, pos.y + size.y);
            crosshair_state_.hovered_y_min = 0; crosshair_state_.hovered_y_max = 0;
        }
        ImPlot::EndPlot();
    }
    ImGui::PopID();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Liquidation cascades (Phase 2 S1). The observed @forceOrder tail feeds the
// incremental detector; finished same-side chains (3+ liquidations within 2s
// gaps) draw as amber time/price bands with a CASCADE pill over the heat and,
// when "Cascade alerts" is on, fire the same toast + browser notification +
// beep plumbing the price alerts use.
// ═══════════════════════════════════════════════════════════════════════════════

// Shared S1/S2 alert path: in-app toast (alert_toast_msg_/alert_toast_until_)
// plus the browser Notification API and a short beep. `freq` separates the
// sources by ear: cascade 660Hz, big liquidation 880Hz, wall pull 520Hz.
static void rt_fire_alert(const char* title, const std::string& msg, const float freq) {
#ifdef __EMSCRIPTEN__
    EM_ASM({
        try {
            var msg = UTF8ToString($0);
            if (window.Notification && Notification.permission === 'granted')
                new Notification(UTF8ToString($1), { body: msg });
            var AC = window.AudioContext || window.webkitAudioContext;
            if (AC) { var c = new AC(); var o = c.createOscillator();
                var g = c.createGain(); o.frequency.value = $2;
                o.connect(g); g.connect(c.destination); g.gain.value = 0.06;
                o.start(); o.stop(c.currentTime + 0.24); }
        } catch (e) {}
    }, msg.c_str(), title, freq);
#else
    (void)title; (void)freq;
#endif
}

static std::string rt_fmt_usd(const double v) {
    char b[24];
    if (v >= 1e9) snprintf(b, sizeof(b), "$%.2fB", v / 1e9);
    else if (v >= 1e6) snprintf(b, sizeof(b), "$%.2fM", v / 1e6);
    else if (v >= 1e3) snprintf(b, sizeof(b), "$%.0fk", v / 1e3);
    else snprintf(b, sizeof(b), "$%.0f", v);
    return b;
}

static std::string rt_upper_symbol(const Terminal::Pair& pair) {
    std::string sym = pair.symbol;
    for (char& c : sym) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    return sym;
}

void ChartWidget::on_liquidation_observed(const Terminal::Liquidation& l) {
    cascade_detector_.observe(l.timestamp_ms, l.price, l.is_buy);
    const double usd = l.qty > 0.0 && l.price > 0.0 ? l.qty * l.price : 0.0;
    if (usd >= liq_alert_usd_) {
        if (liq_alerts_enabled_)
            alert_engine_.observe_liquidation(l.timestamp_ms, l.price, usd, l.is_buy);
        // The events widget collects regardless of the alert toggle.
        char txt[96];
        snprintf(txt, sizeof(txt), "%s %s @ %.0f", rt_fmt_usd(usd).c_str(),
                 l.is_buy ? "SHORTS" : "LONGS", l.price);
        rt_event_feed_.push(l.timestamp_ms, RtEvent::Kind::liq, l.is_buy, usd, txt);
    }
    pump_cascade_alerts();
}

void ChartWidget::pump_cascade_alerts() {
    CascadeChain chain;
    while (cascade_detector_.take_finished(chain)) {
        // The events widget collects finished chains regardless of the toggle.
        char txt[96];
        snprintf(txt, sizeof(txt), "x%d \xc2\xb7 %.1fs", chain.count, chain.span_s());
        rt_event_feed_.push(chain.end_ms, RtEvent::Kind::cascade, chain.buy_side, 0.0, txt);
        if (!cascade_alerts_enabled_ || chain.alerted) continue;
        chain.alerted = true;
        char span[24];
        snprintf(span, sizeof(span), "%.1fs", chain.span_s());
        alert_toast_msg_ = rt_upper_symbol(pair_) + " cascade: " +
                           std::to_string(chain.count) + " liquidations in " + span;
        alert_toast_until_ = ImGui::GetTime() + 6.0;
        rt_fire_alert("EdgeDepth cascade", alert_toast_msg_, 660.0f);
    }
}

// Phase 2 S2: drain the threshold queues. Big liquidation -> 880Hz, wall pull
// -> 520Hz, both through the same toast + notification + beep path as S1.
void ChartWidget::pump_rt_alerts() {
    LiqAlertEvent l;
    while (alert_engine_.take_liq_alert(l)) {
        if (!liq_alerts_enabled_) continue;
        char px[24];
        snprintf(px, sizeof(px), "%.0f", l.price);
        alert_toast_msg_ = rt_upper_symbol(pair_) + " liq " + rt_fmt_usd(l.usd) +
                           (l.buy ? " SHORTS" : " LONGS") + " @ " + px;
        alert_toast_until_ = ImGui::GetTime() + 6.0;
        rt_fire_alert("EdgeDepth liquidation", alert_toast_msg_, 880.0f);
    }
    WallAlertEvent w;
    while (alert_engine_.take_wall_alert(w)) {
        if (!wall_alerts_enabled_) continue;
        char px[24];
        snprintf(px, sizeof(px), "%.0f", w.price);
        alert_toast_msg_ = rt_upper_symbol(pair_) +
                           (w.bid ? " bid wall pulled " : " ask wall pulled ") +
                           rt_fmt_usd(w.vanished * w.price) + " @ " + px;
        alert_toast_until_ = ImGui::GetTime() + 6.0;
        rt_fire_alert("EdgeDepth wall", alert_toast_msg_, 520.0f);
    }
}

// Feed every pull the tracker has classified since the last scan exactly once
// (newest first; the watermark stops the walk). First scan after enable/reset
// fast-forwards to the newest pull so pre-toggle pulls never burst as stale.
// The events widget collects every pull; the toast queue only when its toggle
// is on, so the scan itself no longer gates on wall_alerts_enabled_.
void ChartWidget::scan_new_wall_pulls() {
    if (last_pull_alert_ms_ == 0) {
        rt_walls_.for_each_pull_newest([&](const rt_walls::Pull& p) {
            last_pull_alert_ms_ = p.wall.end_ms;  // arm: skip pre-toggle pulls
            return false;                          // only the newest matters
        });
        return;
    }
    int64_t newest = last_pull_alert_ms_;
    rt_walls_.for_each_pull_newest([&](const rt_walls::Pull& p) {
        if (p.wall.end_ms <= last_pull_alert_ms_) return false;  // seen already
        alert_engine_.observe_wall_pull(p.wall.end_ms, p.wall.price, p.vanished, p.wall.bid);
        char txt[96];
        snprintf(txt, sizeof(txt), "%s pulled %s @ %.0f", p.wall.bid ? "bid" : "ask",
                 rt_fmt_usd(p.vanished * p.wall.price).c_str(), p.wall.price);
        // A bid wall vanishing removes support (bearish); an ask wall going is relief.
        rt_event_feed_.push(p.wall.end_ms, RtEvent::Kind::wall, !p.wall.bid,
                            p.vanished * p.wall.price, txt);
        if (p.wall.end_ms > newest) newest = p.wall.end_ms;
        return true;
    });
    last_pull_alert_ms_ = newest;
}

void ChartWidget::render_realtime_cascades(ImDrawList* dl, const ImPlotRect& limits) {
    cascade_detector_.sweep(int64_t(rt_clock_ms_));
    pump_cascade_alerts();   // chains closed by the quiet-gap sweep alert here
    scan_new_wall_pulls();   // S2: newly classified pulls enter the alert queue
    scan_refill_consumed();  // self-host: consumed refill levels feed + alert
    pump_rt_alerts();        // S2: drain big-liq + wall-pull queues to the toast
    const ImU32 band = Theme::u32(Theme::Tokens::WARN, 0.10f);
    const ImU32 edge = Theme::u32(Theme::Tokens::WARN, 0.55f);
    const ImU32 pill = Theme::u32(Theme::Tokens::WARN, 0.85f);
    const ImU32 text = Theme::u32(Theme::Tokens::BASE);
    const auto draw_one = [&](const CascadeChain& c) {
        const ImVec2 t0 = ImPlot::PlotToPixels(double(c.start_ms), limits.Y.Max);
        const ImVec2 t1 = ImPlot::PlotToPixels(double(c.end_ms), limits.Y.Min);
        dl->AddRectFilled(ImVec2(t0.x, t0.y), ImVec2(t1.x, t1.y), band);
        dl->AddLine(ImVec2(t0.x, t0.y), ImVec2(t0.x, t1.y), edge, 1.0f);
        dl->AddLine(ImVec2(t1.x, t0.y), ImVec2(t1.x, t1.y), edge, 1.0f);
        if (c.hi_price > c.lo_price) {
            const ImVec2 p0 = ImPlot::PlotToPixels(limits.X.Min, c.hi_price);
            const ImVec2 p1 = ImPlot::PlotToPixels(limits.X.Max, c.lo_price);
            dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p1.x, p1.y), band);
        }
        char label[32];
        snprintf(label, sizeof(label), "CASCADE ×%d", c.count);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        const ImVec2 plot_pos = ImPlot::GetPlotPos();
        const ImVec2 base(std::max(t0.x, plot_pos.x) + 4.0f,
                          std::max(t0.y, plot_pos.y) + 4.0f);
        dl->AddRectFilled(ImVec2(base.x, base.y),
                          ImVec2(base.x + ts.x + 10.0f, base.y + ts.y + 6.0f), pill, 3.0f);
        dl->AddText(ImVec2(base.x + 5.0f, base.y + 3.0f), text, label);
    };
    if (const CascadeChain* oc = cascade_detector_.open_chain();
        oc && oc->count >= CascadeDetector::kMinEvents) draw_one(*oc);
    for (const auto& c : cascade_detector_.chains()) draw_one(c);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Phase 2 S3: estimated liquidation clusters (core/liq_levels.h) as horizontal
// bands. Each band sits at the cluster price and spans the view; its thickness
// and tint follow the share of open interest the estimate puts there. Longs
// liquidate below the mark (forced sells, red); shorts above (forced buys,
// green). Clusters outside the price window become left-edge markers (the
// default price fit hugs the mark, and the nearest clusters sit 0.4%+ away).
// The model is fed the live tape tail and recomputes at most every 2s.
// ═══════════════════════════════════════════════════════════════════════════════
void ChartWidget::render_realtime_liq_levels(ImDrawList* dl, const ImPlotRect& limits) {
    if (!rt_liq_levels_enabled_ || !rt_archive_ || !rt_mode_) return;
    const auto& trades = rt_paused_ ? rt_paused_trades_ : candles().realtime_trades().trades();
    const double cap = liq_field::max_leverage(pair_.exchange, pair_.symbol);
    liq_estimator_.update(rt_clock_ms_, liq_mark_price_, liq_oi_contracts_, trades, cap);
    if (liq_estimator_.clusters().empty()) return;
    ProfileScope profile("RT liq levels");
    const ImVec2 pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    if (size.x <= 0 || size.y <= 0) return;
    const float right = pos.x + size.x;
    const float line = ImGui::GetTextLineHeight();
    // Closest to the mark first: the edge slots go to the most relevant
    // clusters when several sit outside the price window.
    std::array<const liq_levels::Cluster*, 8> ordered{};
    size_t n = 0;
    for (const auto& c : liq_estimator_.clusters())
        if (n < ordered.size()) ordered[n++] = &c;
    const double mark = liq_mark_price_;
    std::sort(ordered.begin(), ordered.begin() + std::ptrdiff_t(n),
        [mark](const liq_levels::Cluster* a, const liq_levels::Cluster* b) {
            return std::abs(a->price / mark - 1.0) < std::abs(b->price / mark - 1.0);
        });
    ImGui::PushFont(Theme::Fonts::mono_sm());
    float last_y = -1000.0f;
    int top_used = 0, bottom_used = 0;
    for (size_t i = 0; i < n; ++i) {
        const auto& c = *ordered[i];
        const float strength = float(std::min(1.0, c.share * 4.0));
        const ImVec4& tint = c.longs ? Theme::Tokens::DOWN : Theme::Tokens::UP;
        const float half = 2.5f + 9.0f * strength;
        char usd[24], price[48], label[96];
        format_usd(c.usd, usd, sizeof(usd));
        fmt_.format_price(price, sizeof(price), c.price);
        const float plot_y = ImPlot::PlotToPixels(0, c.price).y;
        const bool band = c.price >= limits.Y.Min && c.price <= limits.Y.Max &&
                          plot_y >= pos.y + 1 && plot_y <= pos.y + size.y - 1;
        float y = plot_y;
        if (!band) {
            // Off-screen: a left-edge marker below the status chip (two per
            // edge), pointing at where the cluster sits relative to the mark.
            const bool above = c.price > limits.Y.Max;
            int& used = above ? top_used : bottom_used;
            if (used >= 2 || !(mark > 0)) continue;
            std::snprintf(label, sizeof(label), "Liq %s %+.2f%%", usd, 100.0 * (c.price / mark - 1.0));
            const ImVec2 ts = ImGui::CalcTextSize(label);
            const float x = pos.x + 10.0f;
            const float chip_h = ctx_.replay_mgr().is_active() ? 62.0f : 34.0f;  // below the status chip
            y = above ? pos.y + chip_h + float(used) * (ts.y + 6.0f)
                      : pos.y + size.y - 8.0f - ts.y - float(used) * (ts.y + 6.0f);
            ++used;
            dl->AddRectFilled(ImVec2(x - 5.0f, y - 2.0f), ImVec2(x + ts.x + 5.0f, y + ts.y + 2.0f),
                Theme::u32(Theme::Tokens::PANEL, 0.85f), Theme::Radius::R1);
            dl->AddText(ImVec2(x, y), Theme::u32(tint, 0.95f), label);
            dl->AddTriangleFilled(ImVec2(x + ts.x + 9.0f, y + ts.y * 0.5f - 3.0f),
                                  ImVec2(x + ts.x + 9.0f, y + ts.y * 0.5f + 3.0f),
                                  ImVec2(x + ts.x + 14.0f, y + ts.y * 0.5f), Theme::u32(tint, 0.9f));
            continue;
        }
        if (std::abs(y - last_y) < line) continue;
        last_y = y;
        dl->AddRectFilled(ImVec2(pos.x, y - half), ImVec2(right, y + half),
            Theme::u32(tint, 0.07f + 0.13f * strength));
        dl->AddLine(ImVec2(pos.x, y), ImVec2(right, y), Theme::u32(tint, 0.28f + 0.30f * strength), 1.0f);
        std::snprintf(label, sizeof(label), "Liq %s \xc2\xb7 %s", usd, price);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        const ImVec2 a(right - ts.x - 12.0f,
            std::clamp(y - ts.y * 0.5f, pos.y + 2.0f, pos.y + size.y - ts.y - 2.0f));
        dl->AddRectFilled(ImVec2(a.x - 5.0f, a.y - 2.0f), ImVec2(a.x + ts.x + 5.0f, a.y + ts.y + 2.0f),
            Theme::u32(Theme::Tokens::PANEL, 0.85f), Theme::Radius::R1);
        dl->AddText(a, Theme::u32(tint), label);
    }
    ImGui::PopFont();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Phase 2 S5: cumulative resting depth - the second heat map. One intensity row
// per price where depth has been resting: brightness follows the time-weighted
// size the level holds (10-minute memory, lazily decayed once a level leaves),
// so persistent shelves and walls stand out from flicker. Gold ramp: distinct
// from the teal main heat, the red/green quotes and the S3 estimate bands.
// ═══════════════════════════════════════════════════════════════════════════════
void ChartWidget::render_realtime_cum_depth(ImDrawList* dl, const ImPlotRect& limits) {
    if (!rt_cum_depth_enabled_ || rt_cum_levels_.empty() || !(tick_size_ > 0) || !rt_mode_) return;
    ProfileScope profile("RT resting depth");
    const ImVec2 pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    if (size.x <= 0 || size.y <= 0) return;
    // The layer normalizes within the view, like the depth heat itself.
    double visible_max = 0;
    for (const auto& entry : rt_cum_levels_) {
        const double price = double(entry.first) * tick_size_;
        if (price < limits.Y.Min || price > limits.Y.Max) continue;
        visible_max = std::max(visible_max,
            double(entry.second.first) * std::exp(-double(rt_clock_ms_ - entry.second.second) / kCumTauMs));
    }
    if (!(visible_max > 0)) return;
    const double mid = rt_book_valid_ && rt_latest_ ? 0.5 * (rt_latest_->bid + rt_latest_->ask) : 0.0;
    const float row_h = std::max(1.0f, size.y / 720.0f);
    int drawn = 0;
    // Hover: map the mouse straight to the nearest tracked price bucket, so
    // the readout works at any zoom (rows are sub-pixel when zoomed out).
    double hover_price = 0, hover_rest = 0;
    bool hover_bid = false;
    if (ImPlot::IsPlotHovered()) {
        const double at = ImPlot::PixelsToPlot(ImGui::GetIO().MousePos).y;
        const auto it = rt_cum_levels_.find(std::llround(at / tick_size_));
        if (it != rt_cum_levels_.end()) {
            const double rested = double(it->second.first) *
                std::exp(-double(rt_clock_ms_ - it->second.second) / kCumTauMs);
            if (rested / visible_max >= 0.03) {
                hover_price = double(it->first) * tick_size_;
                hover_rest = rested;
                hover_bid = mid > 0 && hover_price < mid;
            }
        }
    }
    for (const auto& entry : rt_cum_levels_) {
        if (drawn > 600) break;
        const double price = double(entry.first) * tick_size_;
        if (price < limits.Y.Min || price > limits.Y.Max) continue;
        const double rested = double(entry.second.first) *
            std::exp(-double(rt_clock_ms_ - entry.second.second) / kCumTauMs);
        const float intensity = float(rested / visible_max);
        if (intensity < 0.03f) continue;
        const float y = ImPlot::PlotToPixels(0, price + tick_size_ * 0.5).y;
        dl->AddRectFilled(ImVec2(pos.x, y), ImVec2(pos.x + size.x, y + row_h),
            ImGui::GetColorU32(ImVec4(0.94f, 0.70f, 0.31f, (0.05f + 0.30f * intensity) * rt_cum_opacity_)));
        ++drawn;
    }
    if (!(hover_price > 0)) return;
    ImGui::PushFont(Theme::Fonts::mono_sm());
    char price[48], amount[24];
    fmt_.format_price(price, sizeof(price), hover_price);
    format_base(hover_rest, amount, sizeof(amount));
    ImGui::PopFont();
    Theme::begin_tooltip();
    ImGui::TextUnformatted("Resting depth (cumulative)");
    ImGui::Text("%s · %s %s resting on average over the last minutes",
        price, amount, hover_bid ? "bid" : "ask");
    ImGui::TextUnformatted("Time-weighted from the delivered book samples; flicker fades,\norders that sit keep glowing.");
    Theme::end_tooltip();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Phase 2 S4: the tracked active walls as an always-visible compact table,
// top-right inside the plot. Hand-drawn over the clip exactly like the status
// chip - no layout change, no input grab; the per-wall interactivity stays
// with the edge marks and their hover outlines.
// ═══════════════════════════════════════════════════════════════════════════════
void ChartWidget::render_realtime_walls_table(ImDrawList* dl) {
    if (!rt_walls_table_enabled_ || !rt_walls_enabled_) return;
    const auto limits = ImPlot::GetPlotLimits();
    const ImVec2 pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    if (size.x < 340.0f || size.y < 180.0f) return;
    std::array<const rt_walls::Wall*, 16> rows{};
    size_t count = 0;
    rt_walls_.for_each_live([&](const rt_walls::Wall& w) {
        if (!w.stood(int64_t(rt_clock_ms_)) || w.price < limits.Y.Min || w.price > limits.Y.Max) return;
        if (count < rows.size()) { rows[count++] = &w; return; }
        size_t smallest = 0;
        for (size_t i = 1; i < count; ++i)
            if (rows[i]->size < rows[smallest]->size) smallest = i;
        if (w.size > rows[smallest]->size) rows[smallest] = &w;
    });
    if (!count) return;
    std::sort(rows.begin(), rows.begin() + std::ptrdiff_t(count),
        [](const rt_walls::Wall* a, const rt_walls::Wall* b) { return a->size > b->size; });
    if (count > 8) count = 8;
    const double mid = rt_book_valid_ ? 0.5 * (rt_dom_frame_.bid() + rt_dom_frame_.ask()) : 0.0;

    ImGui::PushFont(Theme::Fonts::mono_sm());
    const float lh = ImGui::GetTextLineHeight();
    const float row_h = lh + 4.0f;
    const float width = 274.0f;
    const float height = 6.0f + lh + 3.0f + lh + 3.0f + count * row_h + 5.0f;
    const ImVec2 a(pos.x + size.x - width - 8.0f, pos.y + 8.0f);
    const ImVec2 b(a.x + width, a.y + height);
    dl->AddRectFilled(a, b, Theme::u32(Theme::Tokens::PANEL, 0.90f), Theme::Radius::R2);
    dl->AddRect(a, b, Theme::u32(Theme::Tokens::BD2), Theme::Radius::R2, 0, 1.0f);
    float y = a.y + 5.0f;
    char title[48];
    std::snprintf(title, sizeof(title), "ACTIVE WALLS \xc2\xb7 %s", realtime_base_asset());
    dl->AddText(ImVec2(a.x + 8.0f, y), Theme::u32(Theme::Tokens::TX2), title);
    y += lh + 3.0f;
    const ImU32 head = Theme::u32(Theme::Tokens::TX3);
    dl->AddText(ImVec2(a.x + 18.0f, y), head, "PRICE");
    dl->AddText(ImVec2(a.x + 86.0f, y), head, "SIZE");
    dl->AddText(ImVec2(a.x + 138.0f, y), head, "\xc3\x97");
    dl->AddText(ImVec2(a.x + 162.0f, y), head, "AGE");
    dl->AddText(ImVec2(a.x + 198.0f, y), head, "DIST");
    dl->AddText(ImVec2(a.x + 240.0f, y), head, "TCH");
    y += lh + 3.0f;
    for (size_t i = 0; i < count; ++i) {
        const auto& wall = *rows[i];
        char price[48], sz[24], mult[16], age[24], dist[24], tch[16];
        fmt_.format_price(price, sizeof(price), wall.price);
        format_base(wall.size, sz, sizeof(sz));
        std::snprintf(mult, sizeof(mult), "%.0f", wall.multiple);
        const int64_t secs = std::max<int64_t>(0, (int64_t(rt_clock_ms_) - wall.start_ms) / 1000);
        if (secs >= 60) std::snprintf(age, sizeof(age), "%lld:%02lld", (long long)(secs / 60), (long long)(secs % 60));
        else std::snprintf(age, sizeof(age), "%llds", (long long)secs);
        if (mid > 0) std::snprintf(dist, sizeof(dist), "%+.2f%%", 100.0 * (wall.price / mid - 1.0));
        else std::snprintf(dist, sizeof(dist), "-");
        std::snprintf(tch, sizeof(tch), "%d", wall.touches);
        const ImU32 ink = Theme::u32(wall.bid ? Theme::Tokens::UP : Theme::Tokens::DOWN);
        dl->AddRectFilled(ImVec2(a.x + 8.0f, y + 3.0f), ImVec2(a.x + 12.0f, y + row_h - 1.0f), ink);
        dl->AddText(ImVec2(a.x + 18.0f, y), ink, price);
        dl->AddText(ImVec2(a.x + 86.0f, y), Theme::u32(Theme::Tokens::TX1), sz);
        dl->AddText(ImVec2(a.x + 138.0f, y), Theme::u32(Theme::Tokens::TX2), mult);
        dl->AddText(ImVec2(a.x + 162.0f, y), Theme::u32(Theme::Tokens::TX2), age);
        dl->AddText(ImVec2(a.x + 198.0f, y),
            Theme::u32(mid > 0 && wall.price < mid ? Theme::Tokens::DOWN : Theme::Tokens::UP), dist);
        dl->AddText(ImVec2(a.x + 240.0f, y), wall.touches > 0 ? Theme::u32(Theme::Tokens::WARN) : head, tch);
        y += row_h;
    }
    ImGui::PopFont();
}

// ═══════════════════════════════════════════════════════════════════════════════
// [SELF-HOST] Whale trail: consecutive large same-side prints linked into a
// visible path over the heat. Built straight from the archived bubble records
// (the same 100ms groups the trade bubbles draw), so there is no extra data
// path - the linking pass runs per frame over the visible window only.
// ═══════════════════════════════════════════════════════════════════════════════
void ChartWidget::render_realtime_whale_trail(ImDrawList* dl, const ImPlotRect& limits) {
    if (!rt_whale_trail_enabled_ || !rt_archive_ || !(tick_size_ > 0) || !rt_mode_) return;
    const double minimum = rt_auto_bubbles_
        ? (rt_bubble_scale_.settled() ? rt_bubble_scale_.minimum() : 0) : rt_min_notional_;
    if (!(minimum > 0)) return;
    std::vector<whale_trail::Node> prints;
    rt_archive_->bubbles().for_each(int64_t(limits.X.Min), int64_t(limits.X.Max), rt_clock_ms_,
        [&](const RealtimeBubbleHistory::Bubble& m) {
            if (m.value() >= minimum)
                prints.push_back({int64_t(m.timestamp_ms), m.price, m.buy_led(), m.value()});
        });
    if (prints.size() < 2) return;
    std::vector<whale_trail::Chain> chains;
    // Price proximity comes from the user's step choice (v2): Tight 20 ticks
    // reads only orders walking one shelf, Wide 80 follows an order climbing
    // several shelves. Best-fit / value gate / span cap live in whale_trail.
    whale_trail::build(prints, rt_whale_window_ms_, double(rt_whale_step_ticks_), tick_size_, chains);
    if (chains.empty()) return;
    for (const auto& chain : chains) {
        const ImVec4& token = chain.buy ? Theme::Tokens::UP : Theme::Tokens::DOWN;
        const ImVec4 halo_col(token.x, token.y, token.z, 0.80f);
        // Line weight carries the trail's quoted total: 1.5 px baseline, up to
        // ~3.4 px once the linked clips sum ~64x over the bubble minimum.
        const float width = std::clamp(
            1.5f + 0.32f * float(std::log2(std::max(1.0, chain.total_value / minimum))),
            1.5f, 3.4f);
        std::vector<ImVec2> pts;
        pts.reserve(chain.nodes.size());
        const whale_trail::Node* peak = &chain.nodes.front();
        for (const auto& n : chain.nodes) {
            pts.push_back(ImPlot::PlotToPixels(double(n.ts_ms), n.price));
            if (n.value > peak->value) peak = &n;
        }
        // Segments brighten toward the present: direction reads even where the
        // path overlaps itself at dense zooms, and older legs recede.
        const int segs = int(pts.size()) - 1;
        for (int i = 0; i < segs; ++i) {
            const float a = segs <= 1 ? 0.55f
                                      : 0.32f + 0.46f * (float(i) / float(segs - 1));
            dl->AddLine(pts[i], pts[i + 1],
                        ImGui::GetColorU32(ImVec4(token.x, token.y, token.z, a)), width);
        }
        // Small anchor dots make single-tap steps readable at dense zooms.
        for (const auto& pt : pts)
            dl->AddCircleFilled(pt, 2.2f, ImGui::GetColorU32(ImVec4(token.x, token.y, token.z, 0.65f)));
        // Arrowhead on the newest end, oriented along the last leg: which way
        // the working order is heading without tracing dot to dot.
        if (segs >= 1) {
            const ImVec2& a = pts[segs - 1];
            const ImVec2& b = pts[segs];
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len > 1.0f) {
                const float ux = dx / len, uy = dy / len;
                const ImVec2 tip(b.x + ux * 6.0f, b.y + uy * 6.0f);
                const ImVec2 w1(b.x - ux * 2.0f - uy * 3.2f, b.y - uy * 2.0f + ux * 3.2f);
                const ImVec2 w2(b.x - ux * 2.0f + uy * 3.2f, b.y - uy * 2.0f - ux * 3.2f);
                const ImVec2 tri[3] = {tip, w1, w2};
                dl->AddConvexPolyFilled(tri, 3,
                    ImGui::GetColorU32(ImVec4(token.x, token.y, token.z, 0.85f)));
            }
        }
        // Halo on the chain's largest clip - the print that made it a whale.
        const float base_r = RealtimeBubble::radius(peak->value, minimum, rt_bubble_cap_);
        const ImVec2 pc = ImPlot::PlotToPixels(double(peak->ts_ms), peak->price);
        dl->AddCircle(pc, base_r + 3.5f, ImGui::GetColorU32(halo_col), 0, 1.5f);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// [SELF-HOST] Cross-venue basis: OKX perp last price vs this chart's venue mid,
// in bps, as a ribbon along the bottom of the real-time plot. Samples once per
// second from the shared ticker store (the watchlist feed already carries every
// venue), so no extra subscription is opened.
// ═══════════════════════════════════════════════════════════════════════════════
static std::string okx_inst_for(const std::string& symbol) {
    // binancef "BTCUSDT" -> okx "btc-usdt-swap" (TickerManager keys are lowercase).
    std::string s;
    s.reserve(symbol.size() + 10);
    for (const char c : symbol) s.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    if (s.find('-') != std::string::npos) return s;              // already an instId
    for (const char* quote : {"usdt", "usdc"}) {
        const size_t qlen = std::strlen(quote);
        if (s.size() > qlen && s.compare(s.size() - qlen, qlen, quote) == 0)
            return s.substr(0, s.size() - qlen) + "-" + quote + "-swap";
    }
    return {};
}

void ChartWidget::render_realtime_basis(ImDrawList* dl, const ImPlotRect& limits) {
    if (!rt_basis_enabled_ || !rt_mode_) return;
    if (rt_basis_inst_.empty()) {
        if (pair_.exchange != "binancef") return;   // basis is defined against the chart venue
        rt_basis_inst_ = okx_inst_for(pair_.symbol);
    }
    if (rt_basis_inst_.empty()) return;
    const TickerEntry* okx = ctx_.ticker_mgr().get("okx", rt_basis_inst_);
    const bool okx_fresh = okx && okx->last_price > 0 &&
        rt_clock_ms_ - okx->event_time_ms < 15000;
    if (okx_fresh && rt_book_valid_ && rt_latest_) {
        const double mid = 0.5 * (rt_latest_->bid + rt_latest_->ask);
        if (mid > 0 && rt_clock_ms_ - rt_basis_last_sample_ms_ >= 1000) {
            rt_basis_history_.emplace_back(rt_clock_ms_,
                (okx->last_price - mid) / mid * 10000.0);
            while (rt_basis_history_.size() > 600) rt_basis_history_.pop_front();
            rt_basis_last_sample_ms_ = rt_clock_ms_;
        }
    }
    if (rt_basis_history_.size() < 2) return;
    const ImVec2 pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    if (size.x <= 0 || size.y <= 0) return;
    // Trim to the visible time window so panning back shows the past ribbon;
    // the 120s tail past the live edge simply sits at the right side.
    while (!rt_basis_history_.empty() &&
           rt_basis_history_.front().first < int64_t(limits.X.Min) - 1000)
        rt_basis_history_.pop_front();
    const float band_h = std::max(26.0f, size.y * 0.14f);
    const float band_top = pos.y + size.y - band_h - 6.0f;
    double max_abs = 2.0;
    for (const auto& s : rt_basis_history_)
        if (s.first >= int64_t(limits.X.Min))
            max_abs = std::max(max_abs, std::fabs(s.second));
    const auto y_for = [&](double bps) {
        return band_top + band_h * 0.5f - float(bps / max_abs) * (band_h * 0.5f - 2.0f);
    };
    const auto x_for = [&](int64_t ts) {
        const double frac = (double(ts) - limits.X.Min) / limits.X.Size();
        return pos.x + float(std::clamp(frac, 0.0, 1.0)) * size.x;
    };
    // Zero baseline.
    dl->AddLine(ImVec2(pos.x, y_for(0)), ImVec2(pos.x + size.x, y_for(0)),
                Theme::u32(Theme::Tokens::TX3), 1.0f);
    // Path + soft area fill, coloured by the newest sample's sign.
    std::vector<ImVec2> pts;
    for (const auto& s : rt_basis_history_)
        if (s.first >= int64_t(limits.X.Min))
            pts.push_back(ImVec2(x_for(s.first), y_for(s.second)));
    if (pts.size() < 2) return;
    const double latest_bps = rt_basis_history_.back().second;
    const ImVec4& token = latest_bps >= 0 ? Theme::Tokens::UP : Theme::Tokens::DOWN;
    const ImVec4 area_col(token.x, token.y, token.z, 0.16f);
    const ImVec4 line_col(token.x, token.y, token.z, 0.85f);
    dl->AddConvexPolyFilled(pts.data(), int(pts.size()), ImGui::GetColorU32(area_col));
    dl->AddPolyline(pts.data(), int(pts.size()), ImGui::GetColorU32(line_col),
                    ImDrawFlags_None, 1.4f);
    // Readout chip at the band's right edge.
    ImGui::PushFont(Theme::Fonts::mono_sm());
    char label[40];
    if (okx_fresh) snprintf(label, sizeof(label), "OKX %+.1f bps", latest_bps);
    else snprintf(label, sizeof(label), "OKX no feed");
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 a(pos.x + size.x - ts.x - 12.0f, band_top + 2.0f);
    dl->AddRectFilled(ImVec2(a.x - 6.0f, a.y - 2.0f), ImVec2(a.x + ts.x + 6.0f, a.y + ts.y + 2.0f),
                      Theme::u32(Theme::Tokens::BASE), 4.0f);
    dl->AddText(ImVec2(a.x, a.y), ImGui::GetColorU32(line_col), label);
    ImGui::PopFont();
}

// ═══════════════════════════════════════════════════════════════════════════════
// [SELF-HOST] Refill-consumed sweep: the refill store closes a level as
// exhausted when the liquidity that kept refilling there is traded through.
// Every such close enters the events feed; when the toggle is on and the
// traded value clears the threshold, the shared toast + Notification + beep
// path fires too. Watermark walk over closed_ms, armed on the first scan so
// pre-toggle closes never burst as stale (same pattern as the wall pulls).
// ═══════════════════════════════════════════════════════════════════════════════
void ChartWidget::scan_refill_consumed() {
    const refill::Market* m = refill::store().find(pair_.exchange, pair_.symbol);
    if (!m || m->levels.empty()) return;
    const bool armed = last_refill_alert_ms_ != 0;
    int64_t newest = last_refill_alert_ms_;
    for (const auto& l : m->levels) {
        if (l.status != refill::End::exhausted) continue;
        const int64_t at = l.closed_ms > 0 ? l.closed_ms : l.updated_ms;
        if (at <= 0) continue;
        if (!armed) {
            if (at > newest) newest = at;  // arm quietly past the backlog
            continue;
        }
        if (at <= last_refill_alert_ms_ || !(l.executed_usd >= refill_alert_usd_)) {
            if (at > newest) newest = at;
            continue;
        }
        if (at > newest) newest = at;
        char price[48], txt[96];
        fmt_.format_price(price, sizeof(price), l.price);
        snprintf(txt, sizeof(txt), "%s consumed %s @ %s",
                 l.side == refill::Side::bid ? "bid" : "ask",
                 rt_fmt_usd(l.executed_usd).c_str(), price);
        // A consumed bid level removes support (bearish); an ask gone is relief.
        rt_event_feed_.push(at, RtEvent::Kind::refill, l.side != refill::Side::bid,
                            l.executed_usd, txt);
        if (refill_alerts_enabled_) {
            alert_toast_msg_ = rt_upper_symbol(pair_) + " refill level consumed " +
                               rt_fmt_usd(l.executed_usd) + " @ " + price;
            alert_toast_until_ = ImGui::GetTime() + 6.0;
            rt_fire_alert("EdgeDepth refill", alert_toast_msg_, 440.0f);
        }
    }
    last_refill_alert_ms_ = newest;
}

// ═══════════════════════════════════════════════════════════════════════════════
// [SELF-HOST] Layer-events widget: the recent layer events (cascades, threshold
// liquidations, pulled walls, consumed refill levels) as a compact corner feed
// on the real-time plot. Passive by design: it collects whatever the alert
// toggles hold, newest first, fading with age. Sits in the top-right corner
// next to the walls table; when that table is on, the feed docks under the
// status chip on the left so the two panels never fight for the corner.
// ═══════════════════════════════════════════════════════════════════════════════
void ChartWidget::render_realtime_event_feed(ImDrawList* dl, const ImPlotRect& limits) {
    (void)limits;
    if (!rt_events_enabled_ || !rt_mode_ || rt_event_feed_.empty()) return;
    const ImVec2 pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    if (size.x < 240.0f || size.y < 180.0f) return;
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const float lh = ImGui::GetTextLineHeight();
    constexpr int kMaxRows = 6;
    char age[kMaxRows][16];
    const char* tags[kMaxRows];
    const char* texts[kMaxRows];
    ImU32 tag_col[kMaxRows], ink_col[kMaxRows];
    int rows = 0;
    float text_w = 0.0f, tag_w = 0.0f;
    rt_event_feed_.for_each([&](const RtEvent& e) {
        if (rows >= kMaxRows) return;
        const int64_t secs = std::max<int64_t>(0, (int64_t(rt_clock_ms_) - e.ts_ms) / 1000);
        if (secs >= 3600) std::snprintf(age[rows], sizeof(age[rows]), "%lldh",
                                        (long long)(secs / 3600));
        else if (secs >= 60) std::snprintf(age[rows], sizeof(age[rows]), "%lldm",
                                           (long long)(secs / 60));
        else std::snprintf(age[rows], sizeof(age[rows]), "%llds", (long long)secs);
        switch (e.kind) {
            case RtEvent::Kind::cascade: tags[rows] = "CASCADE"; break;
            case RtEvent::Kind::wall:    tags[rows] = "WALL";    break;
            case RtEvent::Kind::refill:  tags[rows] = "REFILL";  break;
            default:                     tags[rows] = "LIQ";     break;
        }
        texts[rows] = e.text;
        const ImVec4& side = e.bull ? Theme::Tokens::UP : Theme::Tokens::DOWN;
        tag_col[rows] = Theme::u32(e.kind == RtEvent::Kind::cascade ? Theme::Tokens::WARN : side);
        const float fade = 1.0f - 0.09f * float(rows);
        ink_col[rows] = ImGui::GetColorU32(ImVec4(Theme::Tokens::TX1.x, Theme::Tokens::TX1.y,
                                                  Theme::Tokens::TX1.z, fade));
        text_w = std::max(text_w, ImGui::CalcTextSize(e.text).x);
        tag_w = std::max(tag_w, ImGui::CalcTextSize(tags[rows]).x);
        ++rows;
    });
    if (!rows) { ImGui::PopFont(); return; }
    const float age_w = ImGui::CalcTextSize("00m").x;
    const char* head = i18n::tr("LAYER EVENTS", i18n::fa_events_header);
    const float head_w = ImGui::CalcTextSize(head).x;
    const float width = 8.0f + tag_w + 8.0f + text_w + 8.0f + age_w + 10.0f;
    const float height = 5.0f + lh + 3.0f + float(rows) * (lh + 3.0f) + 4.0f;
    const bool walls_table = rt_walls_table_enabled_ && rt_walls_enabled_;
    const float x = walls_table ? pos.x + 10.0f : pos.x + size.x - width - 8.0f;
    const float y = walls_table ? pos.y + 66.0f : pos.y + 8.0f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height),
                      Theme::u32(Theme::Tokens::PANEL, 0.90f), Theme::Radius::R2);
    dl->AddRect(ImVec2(x, y), ImVec2(x + width, y + height),
                Theme::u32(Theme::Tokens::BD2), Theme::Radius::R2, 0, 1.0f);
    dl->AddText(ImVec2(x + 8.0f, y + 5.0f), Theme::u32(Theme::Tokens::TX2), head);
    dl->AddLine(ImVec2(x + 6.0f, y + 5.0f + lh + 1.0f), ImVec2(x + width - 6.0f, y + 5.0f + lh + 1.0f),
                Theme::u32(Theme::Tokens::BD2), 1.0f);
    float ry = y + 5.0f + lh + 3.0f;
    for (int i = 0; i < rows; ++i) {
        dl->AddText(ImVec2(x + 8.0f, ry), tag_col[i], tags[i]);
        dl->AddText(ImVec2(x + 8.0f + tag_w + 8.0f, ry), ink_col[i], texts[i]);
        dl->AddText(ImVec2(x + width - 8.0f - ImGui::CalcTextSize(age[i]).x, ry),
                    Theme::u32(Theme::Tokens::TX3), age[i]);
        ry += lh + 3.0f;
    }
    ImGui::PopFont();
}

// ═══════════════════════════════════════════════════════════════════════════════
// [SELF-HOST] Slippage cone: walk the live book for a simulated market order of
// the configured size and draw where it would actually fill. The wedge between
// the touch and the volume-weighted fill price is the expected slippage; dashed
// lines mark both fills, the chip carries the numbers in bps. A walk that runs
// out of stored rows reads "+" - the venue's book is deeper than the archive
// keeps at the touch. Buys lift the asks (red side), sells hit the bids (green).
// ═══════════════════════════════════════════════════════════════════════════════
namespace {
struct SlipWalk {
    double vwap = 0.0, filled = 0.0;
    bool complete = false, any = false;
};
SlipWalk slip_walk_side(const std::vector<Terminal::BookEntry>& levels, const size_t begin,
                        const size_t end, const double size) {
    SlipWalk w;
    double notional = 0.0;
    for (size_t i = begin; i < end && w.filled < size && i - begin < 300; ++i) {
        const double q = levels[i].size;
        if (!(q > 0) || !(levels[i].price > 0)) continue;
        const double take = std::min(q, size - w.filled);
        notional += take * levels[i].price;
        w.filled += take;
        w.any = true;
    }
    if (w.filled > 0) w.vwap = notional / w.filled;
    w.complete = w.filled >= size * (1.0 - 1e-9);
    return w;
}
}  // namespace

void ChartWidget::render_realtime_slippage(ImDrawList* dl, const ImPlotRect& limits) {
    if (!rt_slip_enabled_ || !rt_mode_ || !rt_book_valid_ || !rt_latest_) return;
    if (!(rt_slip_size_ > 0)) return;
    const auto& book = *rt_latest_;
    if (book.levels.empty() || !(book.bid > 0) || !(book.ask > book.bid)) return;
    // Archived rows: bids fill the head (best first), asks the tail (best first);
    // the touch prices split them.
    size_t split = 0;
    while (split < book.levels.size() && book.levels[split].price < book.ask) ++split;
    const SlipWalk ask_walk = slip_walk_side(book.levels, split, book.levels.size(), rt_slip_size_);
    const SlipWalk bid_walk = slip_walk_side(book.levels, 0, split, rt_slip_size_);
    if (!ask_walk.any && !bid_walk.any) return;
    const ImVec2 pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    if (size.x <= 0 || size.y <= 0) return;
    const float edge = ImPlot::PlotToPixels(double(rt_clock_ms_), 0).x;
    if (edge < pos.x + 2.0f || edge > pos.x + size.x - 40.0f) return;  // live edge off-view
    const float run = 56.0f;  // cone length in px
    const auto dashed = [&](const float y0, const float y1, const ImU32 col) {
        for (float d = 0.0f; d < run + 20.0f; d += 8.0f)
            dl->AddLine(ImVec2(edge + d, y0), ImVec2(std::min(edge + d + 4.0f, edge + run + 20.0f), y1),
                        col, 1.1f);
    };
    ImGui::PushFont(Theme::Fonts::mono_sm());
    char chip[80];
    if (ask_walk.any && bid_walk.any) {
        const float y_ask_touch = ImPlot::PlotToPixels(0, book.ask).y;
        const float y_bid_touch = ImPlot::PlotToPixels(0, book.bid).y;
        const float y_ask_fill = ImPlot::PlotToPixels(0, ask_walk.vwap).y;
        const float y_bid_fill = ImPlot::PlotToPixels(0, bid_walk.vwap).y;
        const double buy_bps = (ask_walk.vwap - book.ask) / book.ask * 10000.0;
        const double sell_bps = (book.bid - bid_walk.vwap) / book.bid * 10000.0;
        // Wedges: touch at the live edge opening into the fill level.
        dl->AddTriangleFilled(ImVec2(edge, y_ask_touch), ImVec2(edge + run, y_ask_fill),
                              ImVec2(edge + run, y_ask_touch), Theme::u32(Theme::Tokens::DOWN, 0.10f));
        dl->AddTriangleFilled(ImVec2(edge, y_bid_touch), ImVec2(edge + run, y_bid_fill),
                              ImVec2(edge + run, y_bid_touch), Theme::u32(Theme::Tokens::UP, 0.10f));
        dashed(y_ask_fill, y_ask_fill, Theme::u32(Theme::Tokens::DOWN, 0.65f));
        dashed(y_bid_fill, y_bid_fill, Theme::u32(Theme::Tokens::UP, 0.65f));
        const char* plus_buy = ask_walk.complete ? "" : "+";
        const char* plus_sell = bid_walk.complete ? "" : "+";
        snprintf(chip, sizeof(chip), "%g %s \xc2\xb7 slip buy %.1f%s sell %.1f%s bps",
                 rt_slip_size_, realtime_base_asset(), buy_bps, plus_buy, sell_bps, plus_sell);
        const ImVec2 ts = ImGui::CalcTextSize(chip);
        float cy = 0.5f * (y_ask_fill + y_bid_fill) - 0.5f * ts.y;
        cy = std::clamp(cy, pos.y + 4.0f, pos.y + size.y - ts.y - 4.0f);
        float cx = std::clamp(edge + run + 26.0f, pos.x + 4.0f, pos.x + size.x - ts.x - 12.0f);
        dl->AddRectFilled(ImVec2(cx - 6.0f, cy - 2.0f), ImVec2(cx + ts.x + 6.0f, cy + ts.y + 2.0f),
                          Theme::u32(Theme::Tokens::BASE, 0.82f), 4.0f);
        dl->AddText(ImVec2(cx, cy), Theme::u32(Theme::Tokens::TX1), chip);
    } else {
        // Only one side of the book is reachable at this size.
        const bool buy_side = ask_walk.any;
        const double vwap = buy_side ? ask_walk.vwap : bid_walk.vwap;
        const double touch = buy_side ? book.ask : book.bid;
        const float y_touch = ImPlot::PlotToPixels(0, touch).y;
        const float y_fill = ImPlot::PlotToPixels(0, vwap).y;
        const ImVec4& token = buy_side ? Theme::Tokens::DOWN : Theme::Tokens::UP;
        dl->AddTriangleFilled(ImVec2(edge, y_touch), ImVec2(edge + run, y_fill),
                              ImVec2(edge + run, y_touch), Theme::u32(token, 0.10f));
        dashed(y_fill, y_fill, Theme::u32(token, 0.65f));
        const double bps = buy_side ? (ask_walk.vwap - book.ask) / book.ask * 10000.0
                                    : (book.bid - bid_walk.vwap) / book.bid * 10000.0;
        snprintf(chip, sizeof(chip), "%g %s \xc2\xb7 slip %s %.1f%s bps", rt_slip_size_,
                 realtime_base_asset(), buy_side ? "buy" : "sell", bps,
                 (buy_side ? ask_walk.complete : bid_walk.complete) ? "" : "+");
        const ImVec2 ts = ImGui::CalcTextSize(chip);
        float cy = std::clamp(y_fill - 0.5f * ts.y, pos.y + 4.0f, pos.y + size.y - ts.y - 4.0f);
        float cx = std::clamp(edge + run + 26.0f, pos.x + 4.0f, pos.x + size.x - ts.x - 12.0f);
        dl->AddRectFilled(ImVec2(cx - 6.0f, cy - 2.0f), ImVec2(cx + ts.x + 6.0f, cy + ts.y + 2.0f),
                          Theme::u32(Theme::Tokens::BASE, 0.82f), 4.0f);
        dl->AddText(ImVec2(cx, cy), Theme::u32(Theme::Tokens::TX1), chip);
    }
    ImGui::PopFont();
}
