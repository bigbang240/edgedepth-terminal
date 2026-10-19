// Real-time extras: the hover readout, walls and pulled walls, and the CVD
// lane. The logic lives in core/rt_walls.h and core/rt_cvd.h; this file reads
// the chart's real-time state and draws. Automatic heatmap contrast lives in
// the renderer (core/rt_contrast.h); the chart only passes its switch.
#include "ui/chart_widget.h"
#include "core/candle_manager.h"
#include "core/display_time_zone.h"
#include "core/entitlements.h"
#include "core/symbol_metadata.h"
#include "core/ui_test_marks.h"
#include "rendering/realtime_bubble.h"
#include "rendering/theme.h"
#include "replayer/replay_manager.h"
#include "imgui_internal.h"
#include "types/frame_profiler.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

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
}

// Layers rows: Walls (with Pulled walls under it) in ORDER BOOK SIGNALS, CVD in REAL-TIME VIEW. Free wherever
// real-time itself is available.
void ChartWidget::render_realtime_wall_layer() {
    if (Theme::menu_layer("Walls", rt_walls_enabled_)) rt_walls_enabled_ = !rt_walls_enabled_;
    if (ImGui::IsItemHovered())
        Theme::tooltip("The largest resting orders away from the touch: at least 5x the usual level\nand larger than what the touch holds, standing 3 s or more. Marked at the right edge.");
    if (!rt_walls_enabled_) return;
    Theme::begin_menu_detail();
    if (Theme::menu_toggle("Pulled walls", rt_pulls_enabled_)) rt_pulls_enabled_ = !rt_pulls_enabled_;
    if (ImGui::IsItemHovered())
        Theme::tooltip("A wall that stood, then mostly vanished within a second near price\nwithout trading there. Larger squares mean more removed depth.\nSignificant and recent pulls stay visible, up to 30 minutes or 256 retained events.");
    Theme::end_menu_detail();
}

void ChartWidget::render_realtime_cvd_layer() {
    if (Theme::menu_layer("CVD", rt_cvd_enabled_)) rt_cvd_enabled_ = !rt_cvd_enabled_;
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
    if (liq_alerts_enabled_) {
        const double usd = l.qty > 0.0 && l.price > 0.0 ? l.qty * l.price : 0.0;
        if (usd >= liq_alert_usd_)
            alert_engine_.observe_liquidation(l.timestamp_ms, l.price, usd, l.is_buy);
    }
    pump_cascade_alerts();
}

void ChartWidget::pump_cascade_alerts() {
    CascadeChain chain;
    while (cascade_detector_.take_finished(chain)) {
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
void ChartWidget::scan_new_wall_pulls() {
    if (!wall_alerts_enabled_) return;
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
        if (p.wall.end_ms > newest) newest = p.wall.end_ms;
        return true;
    });
    last_pull_alert_ms_ = newest;
}

void ChartWidget::render_realtime_cascades(ImDrawList* dl, const ImPlotRect& limits) {
    cascade_detector_.sweep(int64_t(rt_clock_ms_));
    pump_cascade_alerts();  // chains closed by the quiet-gap sweep alert here
    scan_new_wall_pulls();  // S2: newly classified pulls enter the alert queue
    pump_rt_alerts();       // S2: drain big-liq + wall-pull queues to the toast
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
