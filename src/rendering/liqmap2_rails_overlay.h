#pragma once
// V2 rails (public since 2026-10-11: free on BTC, Pro elsewhere, labelled as a forward test), drawn from
// core/liqmap2_rails.h (hub task liq-map-v2, sprint item 3). Each rail is a crisp
// bar at its fixed price from its birth to the bar that ended it (or the clock), coloured by the liquidations
// expected within the chosen horizon (or, by choice or while any rail in view has no chance yet, by its dollars if
// reached) on a scale set by the rails in view, in eight steps that change only when the rail's numbers step (at
// most hourly). Candles draw on top. The look follows backend docs/LIQMAP_VISUAL_RULE.md and James's MMT references
// (ref-6, ref-7): discrete bars at fixed prices with background between them, ended by the touching candle (also
// applied here to the chart's own candles, the forming one included, so no bar ever crosses a candle), violet for
// the many, through magenta and coral to orange and yellow for the few. A smooth ladder draws as one rail
// (liqmap2_rails::runs). At the right edge, as ref-6, a profile of the live rails by price (both venues summed)
// shows where the fuel stands now whatever the zoom. A compact key (top left) names the colour and its scale.
// Hover: the rail's or the ladder's numbers at the hovered time, within 1 h, 4 h and 24 h.
#include "core/liqmap2_rails.h"
#include "core/liqmap2_store.h"
#include "core/liqmap2_release.h"
#include "rendering/theme.h"
#include "implot.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace liqmap2_rails_overlay {
inline constexpr float kWidth = 4.0f;      // bar height, px: crisp bars as ref-6/7, never hairlines (rule 5)
inline constexpr int kAxisLabels = 4;      // live rails labelled beside the profile, largest first
inline constexpr float kRowPitch = 5.0f;   // profile rows: a 4 px bar and a 1 px gap
inline constexpr float kMinRunPx = 8.0f;   // a line's stretch narrower than this joins the next (one colour, no dashes)
inline constexpr float kMinLinePx = 12.0f; // a whole line shorter than this is a speck, not a rail: the profile has it

// The rails ramp (ref-6/7): violet, magenta, coral, orange, yellow. The lowest step stays a clearly visible violet
// on the near-black canvas, so the many small rails read as rails (rule 4: most bands violet, a few yellow).
inline ImU32 ramp(float t, float alpha = 1.0f) {
    static const float stops[8][3] = {{62, 38, 168}, {88, 34, 190}, {124, 38, 190}, {164, 46, 170},
                                      {204, 64, 134}, {236, 100, 92}, {250, 152, 52}, {253, 214, 66}};
    t = std::clamp(t, 0.0f, 1.0f) * 7;
    const int i = std::min(6, static_cast<int>(t));
    const float f = t - i;
    const auto c = [&](int k) { return static_cast<int>(stops[i][k] + f*(stops[i + 1][k] - stops[i][k])); };
    return IM_COL32(c(0), c(1), c(2), static_cast<int>(255*alpha));
}

inline ImU32 color(int level, float alpha = 1.0f) {
    if (level < 0) return ramp(0.0f, 0.55f*alpha);  // chance unknown (the view then colours by dollars)
    return ramp(static_cast<float>(level)/(liqmap2_rails::kLevels - 1), alpha);
}

// One venue's rails for the chart (one map per coin across venues, James 2026-10-10): its store, its venue, and
// the factor that maps its prices onto the chart's, the chart venue's mark over this venue's mark (1 for the chart's
// own venue). The factor is today's basis applied to every segment in view; a rail's own venue price is in its hover.
struct Source {
    const liqmap2_rails::Store* store = nullptr;
    std::string exchange;
    double ratio = 1;
};

// The chart's candles (open time ms ascending, lows, highs) and the plot's time shift (candles sit centred on their
// open, so a real time t is drawn at t - shift).
struct Candles {
    const std::vector<double>* t = nullptr;
    const std::vector<double>* lo = nullptr;
    const std::vector<double>* hi = nullptr;
    double shift_ms = 0;
};

inline const char* colour_name(liqmap2_rails::Colour c) {
    switch (c) {
        case liqmap2_rails::Colour::expected_1h: return "expected in 1 h";
        case liqmap2_rails::Colour::expected_4h: return "expected in 4 h";
        case liqmap2_rails::Colour::expected: return "expected in 24 h";
        default: return "if reached";
    }
}

// The compact key at the top left (one family with the cascade chip in touch_odds_overlay): a rounded chip with the
// layer's name, its colour ramp, the brightest rail's dollars and what the colour measures; the venues' state and any
// fallback in its hover. Sentence case in Inter, the figure in mono.
inline void key_chip(ImDrawList* draw, ImVec2 at, const char* basis, const char* max_label, bool warn,
                     const std::vector<std::string>& hover_lines) {
    constexpr float h = 22.0f, pad = 9.0f, gap = 8.0f, ramp_w = 48.0f, ramp_h = 6.0f;
    const char* label = "Liq rails";
    // Inter at the mono figure's size, so the words and the number read as one line at any text size.
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const float px = ImGui::GetFontSize();
    ImGui::PopFont();
    Theme::push_font(Theme::Fonts::ui(), px);
    const ImVec2 ls = ImGui::CalcTextSize(label), bs = ImGui::CalcTextSize(basis);
    ImGui::PopFont();
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const ImVec2 ms = max_label ? ImGui::CalcTextSize(max_label) : ImVec2(0, 0);
    ImGui::PopFont();
    float w = pad + ls.x + gap + bs.x + pad;
    if (max_label) w += (warn ? 10.0f : 0.0f) + ramp_w + 6.0f + ms.x + gap;
    const ImVec2 b(std::round(at.x + w), at.y + h);
    draw->AddRectFilled(at, b, Theme::u32(Theme::Tokens::PANEL, 0.92f), Theme::Radius::R2);
    draw->AddRect(at, b, Theme::u32(Theme::Tokens::BD2), Theme::Radius::R2, 0, 1.0f);
    Theme::chart_chip(at, b);
    const float cy = at.y + h*0.5f;
    float x = at.x + pad;
    Theme::push_font(Theme::Fonts::ui(), px);
    draw->AddText(ImVec2(x, std::round(cy - ls.y*0.5f)), Theme::u32(Theme::Tokens::TX2), label);
    ImGui::PopFont();
    x += ls.x + gap;
    if (max_label) {
        if (warn) {
            draw->AddCircleFilled(ImVec2(x + 3.0f, cy), 3.0f, Theme::u32(Theme::Tokens::WARN));
            x += 10.0f;
        }
        const float seg = ramp_w/liqmap2_rails::kLevels;
        for (int k = 0; k < liqmap2_rails::kLevels; ++k) {
            const ImDrawFlags round = k == 0 ? ImDrawFlags_RoundCornersLeft : k == liqmap2_rails::kLevels - 1 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone;
            draw->AddRectFilled(ImVec2(x + k*seg, cy - ramp_h*0.5f), ImVec2(x + (k + 1)*seg, cy + ramp_h*0.5f), color(k), 3.0f, round);
        }
        x += ramp_w + 6.0f;
        ImGui::PushFont(Theme::Fonts::mono_sm());
        draw->AddText(ImVec2(x, std::round(cy - ms.y*0.5f)), Theme::u32(Theme::Tokens::TX1), max_label);
        ImGui::PopFont();
        x += ms.x + gap;
    }
    Theme::push_font(Theme::Fonts::ui(), px);
    draw->AddText(ImVec2(x, std::round(cy - bs.y*0.5f)), Theme::u32(Theme::Tokens::TX3), basis);
    ImGui::PopFont();
    const auto mouse = ImGui::GetIO().MousePos;
    if (!ImPlot::IsPlotHovered() || mouse.x < at.x || mouse.x > b.x || mouse.y < at.y || mouse.y > b.y) return;
    Theme::begin_tooltip();
    for (size_t i = 0; i < hover_lines.size(); ++i) {
        if (i == 0) ImGui::TextUnformatted(hover_lines[i].c_str());
        else ImGui::TextDisabled("%s", hover_lines[i].c_str());
    }
    Theme::end_tooltip();
}

// Called inside the price plot. top_y: pixels below the plot top for the key. profile: draw the live rails' profile
// at the right edge (the chart reserves room for it). short_horizons: show the 1 h and 4 h figures in the hover
// (admins). They are unvalidated for customers until their forward look passes (PREREG_RAILS_HORIZONS_FORWARD.md):
// in development their expected dollars overstate several times on ordinary days (hub RESULT_RAILS_H1B_DEV.md).
template <class FormatTime>
void render(const std::vector<Source>& sources, const Candles& candles, int64_t asof, const char* symbol, const char* price_fmt,
            float top_y, liqmap2_rails::Colour chosen, bool profile, FormatTime format_time, bool short_horizons = true,
            bool replay = false) {
    const auto pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    const auto limits = ImPlot::GetPlotLimits();
    const double shift = candles.shift_ms;
    const double t_lo = limits.X.Min + shift, t_hi = limits.X.Max + shift;  // the view in real time
    const auto x_of = [&](double t) { return ImPlot::PlotToPixels(t - shift, 0).x; };
    auto* draw = ImPlot::GetPlotDrawList();
    const auto mouse = ImGui::GetIO().MousePos;
    const bool hovered = ImPlot::IsPlotHovered();
    const ImVec2 key_at(pos.x + 10, pos.y + top_y);
    ImPlot::PushPlotClipRect();
    // Data in hand: a live message, or stored rails (a replay reads history only) born by the clock.
    const bool any = std::any_of(sources.begin(), sources.end(), [&](const Source& src) {
        if (!src.store) return false;
        if (!replay && src.store->latest().timestamp_ms != 0) return true;
        return std::any_of(src.store->rails().begin(), src.store->rails().end(),
                           [&](const auto& kv) { return kv.second.born_ms <= asof; });
    });
    if (!any) {
        if (replay)
            key_chip(draw, key_at, "none recorded here", nullptr, false,
                     {"No rails stored for this replay window.",
                      "Rails are recorded from 9 Oct 2026 (Bybit) and 10 Oct (Binance)."});
        else
            key_chip(draw, key_at, "waiting for the first message", nullptr, false, {"No rails message yet."});
        ImPlot::PopPlotClipRect();
        return;
    }
    const bool have_candles = candles.t && candles.lo && candles.hi;
    // The chart's present column: the end of the forming candle's slot (on 1 m the clock's minute, on 1 D the day).
    // Live lines run to it, so on 4 h and 1 D they end together just past the forming candle, not at the clock's
    // arbitrary place inside it.
    int64_t now_edge = asof;
    if (have_candles && shift > 0 && !candles.t->empty()) {
        const auto& t = *candles.t;
        auto it = std::upper_bound(t.begin(), t.end(), static_cast<double>(asof));
        if (it != t.begin()) now_edge = std::max(asof, static_cast<int64_t>(*std::prev(it) + 2*shift));
    }
    std::set<std::pair<int, const liqmap2_rails::Rail*>> pushed;  // lines started past a candle's body (gather)
    // Every venue's rails in view, merged into runs (ladders draw as one rail) and mapped to chart prices, on one
    // scale. Rails a little beyond the view's prices join the grouping so a ladder crossing the edge stays one run.
    const auto gather = [&](liqmap2_rails::Colour c, double& vmax, bool& unknown) {
        std::vector<liqmap2_rails::Run> all;
        vmax = 0;
        unknown = false;
        for (size_t i = 0; i < sources.size(); ++i) {
            const auto& src = sources[i];
            if (!src.store || !(src.ratio > 0)) continue;
            double vm = 0;
            const auto segs = liqmap2_rails::segments(src.store->rails(), asof, t_lo, t_hi,
                                                      0.9*limits.Y.Min/src.ratio, 1.1*limits.Y.Max/src.ratio, vm, c);
            for (const auto& sg : segs)
                if (sg.v < 0 && sg.price*src.ratio >= limits.Y.Min && sg.price*src.ratio <= limits.Y.Max) unknown = true;
            for (auto r : liqmap2_rails::runs(segs)) {
                r.price *= src.ratio; r.lo *= src.ratio; r.hi *= src.ratio; r.near *= src.ratio;
                r.source = static_cast<int>(i);
                if (r.price < limits.Y.Min || r.price > limits.Y.Max) continue;
                vmax = std::max(vmax, r.v);
                all.push_back(r);
            }
        }
        // Candles consume: each line ends at the first candle touching its (chart) price after its line began.
        if (have_candles) {
            std::map<std::pair<int, const liqmap2_rails::Rail*>, int64_t> first, ends, starts;
            pushed.clear();
            for (const auto& r : all) {
                auto [it, fresh] = first.try_emplace({r.source, r.rep}, r.t0);
                if (!fresh) it->second = std::min(it->second, r.t0);
            }
            for (const auto& r : all) {
                const auto key = std::make_pair(r.source, r.rep);
                if (ends.contains(key)) continue;
                // The bar's half-height in price at its row, so a candle reaching any part of the row consumes it.
                const float y = ImPlot::PlotToPixels(0, r.price).y;
                const double slack = std::abs(ImPlot::PixelsToPlot(ImVec2(0, y - (kWidth*0.5f + 1.0f))).y - r.price);
                ends[key] = liqmap2_rails::consumed_at(*candles.t, *candles.lo, *candles.hi, r.rep->is_long, r.price,
                                                        first[key], static_cast<int64_t>(t_hi) + 1, slack);
                starts[key] = liqmap2_rails::clear_start(*candles.t, *candles.lo, *candles.hi, r.price, first[key],
                                                         static_cast<int64_t>(2*shift), slack);
                if (starts[key] > first[key]) pushed.insert(key);
            }
            std::vector<liqmap2_rails::Run> kept;
            for (auto r : all) {
                const int64_t end = ends[{r.source, r.rep}];
                r.t0 = std::max(r.t0, starts[{r.source, r.rep}]);
                if (r.t0 >= end || r.t0 >= std::max(r.t1, r.t1 >= asof ? now_edge : r.t1)) continue;
                r.t1 = std::min(r.t1, end);
                // Still standing at the clock: on to the present column.
                if (r.t1 >= asof && end == INT64_MAX && (r.rep->end_ms == 0 || r.rep->end_ms > asof)) r.t1 = std::max(r.t1, now_edge);
                kept.push_back(r);
            }
            all.swap(kept);
        }
        // One colour per stretch: a line's hourly steps sit a few pixels apart on 4 h and 1 D; a stretch narrower
        // than kMinRunPx joins the line's next stretch and takes its figures (the hover still reads each step).
        std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) {
            return a.source != b.source ? a.source < b.source : a.rep != b.rep ? a.rep < b.rep : a.t0 < b.t0;
        });
        std::vector<liqmap2_rails::Run> merged;
        for (size_t i = 0; i < all.size(); ++i) {
            auto r = all[i];
            while (i + 1 < all.size() && all[i + 1].source == r.source && all[i + 1].rep == r.rep && all[i + 1].t0 == r.t1 &&
                   x_of(double(r.t1)) - x_of(double(r.t0)) < kMinRunPx) {
                const int64_t t0 = r.t0;
                r = all[++i];
                r.t0 = t0;
            }
            merged.push_back(r);
        }
        all.swap(merged);
        // Specks: a line that stands only a sliver at this zoom (on 4 h and 1 D, one born inside a candle whose range
        // holds it, so started past that candle's body, or one that ended almost at once) is noise beside the
        // candles; the profile still counts it while it stands. A rail born moments ago, clear of the candles, stays:
        // it is new, not noise.
        {
            std::map<std::pair<int, const liqmap2_rails::Rail*>, std::pair<int64_t, int64_t>> span;
            for (const auto& r : all) {
                auto [it, fresh] = span.try_emplace({r.source, r.rep}, r.t0, r.t1);
                if (!fresh) it->second = {std::min(it->second.first, r.t0), std::max(it->second.second, r.t1)};
            }
            std::erase_if(all, [&](const liqmap2_rails::Run& r) {
                const auto key = std::make_pair(r.source, r.rep);
                const auto& [a, b] = span[key];
                const bool fresh = b >= asof && !pushed.contains(key);
                return !fresh && x_of(std::min(double(b), t_hi)) - x_of(std::max(double(a), t_lo)) < kMinLinePx;
            });
        }
        vmax = 0;
        for (const auto& r : all) vmax = std::max(vmax, r.v);
        std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.v < b.v; });
        return all;
    };
    double vmax = 0;
    bool unknown = false;
    auto rs = gather(chosen, vmax, unknown);
    const auto colour = chosen != liqmap2_rails::Colour::dollars && unknown ? liqmap2_rails::Colour::dollars : chosen;
    if (colour != chosen) rs = gather(colour, vmax, unknown);
    const auto y_of = [](double price) { return std::round(ImPlot::PlotToPixels(0, price).y); };

    // The profile's column at the right edge (ref-6): about a tenth of the plot, never under 56 px or over 120 px,
    // and only in the empty space after now: zoomed or panned so the candles reach the edge, it narrows and then
    // steps aside rather than paint under them.
    const float prof_x1 = pos.x + size.x - 3.0f;
    float prof_w = profile ? std::clamp(size.x*0.09f, 56.0f, 120.0f) : 0.0f;
    prof_w = std::min(prof_w, prof_x1 - (x_of(static_cast<double>(now_edge)) + 10.0f));
    if (prof_w < 32.0f) {
        prof_w = 0.0f;
        profile = false;
    }
    const float prof_x0 = prof_x1 - prof_w;

    // Rails as crisp bars, brightest last; the brightest step a pixel heavier so the few big ones lead.
    const liqmap2_rails::Run* hit = nullptr;
    float hit_d = 1e9f;
    for (const auto& r : rs) {
        const float x0 = std::round(x_of(std::max(double(r.t0), t_lo)));
        const float x1 = std::round(x_of(std::min(double(r.t1), t_hi)));
        if (x1 - x0 < 3.0f) continue;   // a speck reads as noise, not a rail
        const float y = y_of(r.price);
        const int lv = liqmap2_rails::level(r.v, vmax);
        const float th = lv == liqmap2_rails::kLevels - 1 ? kWidth + 1.0f : kWidth;
        const float y0 = y - std::floor(th*0.5f);
        draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + th), color(lv));
        const float d = std::abs(mouse.y - y);
        if (hovered && mouse.x >= x0 && mouse.x <= x1 && d <= th*0.5f + 3.0f && d < hit_d) {
            hit_d = d;
            hit = &r;
        }
    }

    // The profile: every live rail of either venue (as merged nowhere: each rail's own zone), not consumed by the
    // chart's candles, its value spread over the rows its zone covers. Rows 5 px apart keep it discrete at any zoom.
    struct Row { double v = 0; int rails = 0; };
    std::vector<Row> rows;
    double row_max = 0;
    int prof_hit = -1;
    if (profile && prof_w > 0) {
        rows.resize(static_cast<size_t>(std::max(1.0f, std::ceil(size.y/kRowPitch))));
        for (const auto& src : sources) {
            if (!src.store || !(src.ratio > 0)) continue;
            for (const auto& [key, r] : src.store->rails()) {
                if ((r.end_ms > 0 && r.end_ms <= asof) || r.born_ms > asof || r.steps.empty()) continue;
                const auto* st = liqmap2_rails::step_at(r, asof);
                const double v = liqmap2_rails::value(*st, colour);
                const double price = r.price*src.ratio;
                if (!(v > 0) || price < limits.Y.Min || price > limits.Y.Max) continue;
                if (have_candles && liqmap2_rails::consumed_at(*candles.t, *candles.lo, *candles.hi, r.is_long, price,
                                                               r.born_ms, asof + 1) != INT64_MAX) continue;
                const double lo = st->zone_lo > 0 ? st->zone_lo*src.ratio : price, hi = st->zone_hi > 0 ? st->zone_hi*src.ratio : price;
                float ya = ImPlot::PlotToPixels(0, hi).y - pos.y, yb = ImPlot::PlotToPixels(0, lo).y - pos.y;
                if (yb - ya < 1.0f) ya = yb = ImPlot::PlotToPixels(0, price).y - pos.y;
                const int k0 = std::clamp(static_cast<int>(ya/kRowPitch), 0, static_cast<int>(rows.size()) - 1);
                const int k1 = std::clamp(static_cast<int>(yb/kRowPitch), 0, static_cast<int>(rows.size()) - 1);
                for (int k = k0; k <= k1; ++k) {
                    const float r0 = k*kRowPitch, r1 = r0 + kRowPitch;
                    const float share = yb > ya ? (std::min(yb, r1) - std::max(ya, r0))/(yb - ya) : 1.0f;
                    if (share <= 0) continue;
                    rows[k].v += v*share;
                    ++rows[k].rails;
                }
            }
        }
        for (const auto& row : rows) row_max = std::max(row_max, row.v);
        if (row_max > 0) {
            for (size_t k = 0; k < rows.size(); ++k) {
                if (!(rows[k].v > 0)) continue;
                const float len = std::max(2.0f, std::round(prof_w*static_cast<float>(rows[k].v/row_max)));
                const float y0 = pos.y + k*kRowPitch;
                draw->AddRectFilled(ImVec2(prof_x1 - len, y0), ImVec2(prof_x1, y0 + kRowPitch - 1.0f),
                                    color(liqmap2_rails::level(rows[k].v, row_max), 0.92f));
                if (hovered && mouse.x >= prof_x0 && mouse.x <= prof_x1 && mouse.y >= y0 && mouse.y < y0 + kRowPitch)
                    prof_hit = static_cast<int>(k);
            }
        }
    }

    // Dollar labels for the largest live rails (runs) across venues, beside the profile, never overlapping.
    std::vector<const liqmap2_rails::Run*> live;
    for (const auto& r : rs)
        if (r.t1 >= asof && (r.rep->end_ms == 0 || r.rep->end_ms > asof) && r.v > 0) live.push_back(&r);
    std::stable_sort(live.begin(), live.end(), [](const auto* a, const auto* b) { return a->v > b->v; });
    std::vector<float> used;
    ImGui::PushFont(Theme::Fonts::mono_sm());
    for (const auto* r : live) {
        if (static_cast<int>(used.size()) >= kAxisLabels) break;
        const float y = y_of(r->price);
        if (std::any_of(used.begin(), used.end(), [&](float u) { return std::abs(u - y) < ImGui::GetTextLineHeight() + 4.0f; })) continue;
        char amount[32];
        liqmap2::money(r->v, amount, sizeof(amount));
        const ImVec2 ts = ImGui::CalcTextSize(amount);
        const float right = profile ? prof_x0 - 6.0f : pos.x + size.x - 6.0f;
        const ImVec2 p0(right - ts.x - 12.0f, y - ts.y*0.5f - 2.0f), p1(right, y + ts.y*0.5f + 2.0f);
        if (p0.x < x_of(static_cast<double>(now_edge)) + 6.0f) break;  // no room after now: never over the candles
        draw->AddRectFilled(p0, p1, Theme::u32(Theme::Tokens::PANEL, 0.88f), Theme::Radius::R1);
        draw->AddRectFilled(p0, ImVec2(p0.x + 3.0f, p1.y), color(liqmap2_rails::level(r->v, vmax)));
        draw->AddText(ImVec2(p0.x + 8.0f, p0.y + 2.0f), Theme::u32(Theme::Tokens::TX1), amount);
        used.push_back(y);
    }
    ImGui::PopFont();

    // The key: the colour and its scale; the venues' state and any fallback in its hover.
    const auto basis = [](liqmap2_rails::Colour c) {
        switch (c) {
            case liqmap2_rails::Colour::expected_1h: return "expected in 1 h";
            case liqmap2_rails::Colour::expected_4h: return "expected in 4 h";
            case liqmap2_rails::Colour::expected: return "expected in 24 h";
            default: return "if reached";
        }
    };
    const auto horizon = [](liqmap2_rails::Colour c) {
        switch (c) {
            case liqmap2_rails::Colour::expected_1h: return "1 h";
            case liqmap2_rails::Colour::expected_4h: return "4 h";
            default: return "24 h";
        }
    };
    char max_label[32] = "-";
    if (vmax > 0) liqmap2::money(vmax, max_label, sizeof(max_label));
    char basis_text[64];
    if (colour != chosen) std::snprintf(basis_text, sizeof(basis_text), "%s \xc2\xb7 %s pending", basis(colour), horizon(chosen));
    else std::snprintf(basis_text, sizeof(basis_text), "%s", basis(colour));
    std::vector<std::string> lines;
    lines.push_back(std::string("Liquidation rails, coloured by $ ") + colour_name(colour) + ". Brightest " + max_label + ".");
    if (replay) lines.push_back("Replay: the rails as published at the time.");
    if (colour != chosen)
        lines.push_back(std::string("The ") + horizon(chosen) + " figure is not in for every rail in view yet, so the view colours by dollars.");
    bool warn = false;
    for (const auto& src : sources) {
        if (!src.store) continue;
        const auto& latest = src.store->latest();
        const std::string state = replay ? std::string("recorded") : latest.timestamp_ms ? latest.status : std::string("waiting");
        warn = warn || (state != "ready" && state != "recorded");
        lines.push_back(std::string(liqmap2::venue_name(src.exchange)) + ": " + state + (latest.provisional ? " (provisional)" : ""));
    }
    lines.push_back("A rail ends at the candle that reaches it. Not a magnet.");
    lines.push_back(std::string(liqmap2::kForwardTest) + ".");
    key_chip(draw, key_at, basis_text, max_label, warn, lines);
    ImPlot::PopPlotClipRect();

    if (prof_hit >= 0 && !hit) {
        const float y0 = pos.y + prof_hit*kRowPitch;
        char a[48], b[48], v[32];
        std::snprintf(a, sizeof(a), price_fmt, ImPlot::PixelsToPlot(ImVec2(0, y0 + kRowPitch)).y);
        std::snprintf(b, sizeof(b), price_fmt, ImPlot::PixelsToPlot(ImVec2(0, y0)).y);
        liqmap2::money(rows[prof_hit].v, v, sizeof(v));
        Theme::begin_tooltip();
        ImGui::Text("%s %s", v, colour_name(colour));
        ImGui::TextDisabled("%s to %s \xc2\xb7 %d live rail%s, both venues", a, b, rows[prof_hit].rails, rows[prof_hit].rails == 1 ? "" : "s");
        Theme::end_tooltip();
    }
    if (!hit) return;
    // The rail's hover (James 2026-10-11: "clean up the overly verbose hover"): what and where, the dollars, the
    // odds that are in, then one muted line of provenance. Four lines, five on another venue's rail.
    const auto& src = sources[hit->source];
    const auto& r = *hit->rep;
    // Never past the clock: a replay must not show a rail's later step or its end.
    const int64_t at = std::min(asof, static_cast<int64_t>(ImPlot::PixelsToPlot(mouse).x + shift));
    const auto* s = liqmap2_rails::step_at(r, at);
    char price[48], own[48], lo[48], hi[48], born[64], stamp[64], usd[32];
    std::snprintf(price, sizeof(price), price_fmt, hit->price);
    std::snprintf(own, sizeof(own), price_fmt, r.price);
    std::snprintf(lo, sizeof(lo), price_fmt, hit->lo);
    std::snprintf(hi, sizeof(hi), price_fmt, hit->hi);
    format_time(r.born_ms, born, sizeof(born));
    format_time(s->at_ms, stamp, sizeof(stamp));
    const ImVec4 side = r.is_long ? Theme::Tokens::DOWN : Theme::Tokens::UP;
    const bool ladder = hit->members > 1;
    Theme::begin_tooltip();
    if (ladder) ImGui::TextColored(side, "%s ladder \xc2\xb7 %s to %s \xc2\xb7 %d rails", r.is_long ? "Long" : "Short", lo, hi, hit->members);
    else ImGui::TextColored(side, "%s rail \xc2\xb7 %s", r.is_long ? "Long" : "Short", price);
    liqmap2::money(ladder ? hit->usd : s->usd, usd, sizeof(usd));
    if (ladder) ImGui::Text("%s forced out if price runs through it", usd);
    else ImGui::Text("%s forced out if price reaches %s to %s", usd, lo, hi);
    // The odds in hand, one line: customers see 24 h only (1 h and 4 h await their forward look).
    char odds[160] = "";
    size_t n = 0;
    bool scored = false;
    const struct { const char* name; double c, e; } hz[3] = {
        {"1 h", ladder ? -1.0 : s->chance_1h, ladder ? hit->e1 : s->expected_1h},
        {"4 h", ladder ? -1.0 : s->chance_4h, ladder ? hit->e4 : s->expected_4h},
        {"24 h", ladder ? -1.0 : s->chance, ladder ? hit->e24 : s->expected}};
    for (const auto& h : hz) {
        if (!short_horizons && h.name[0] != '2') continue;
        const bool in = ladder ? h.e >= 0 : h.c >= 0;
        char e[32] = "";
        if (in) liqmap2::money(h.e, e, sizeof(e));
        const int w = !in ? std::snprintf(odds + n, sizeof(odds) - n, "%s%s -", n ? "  \xc2\xb7  " : "", h.name)
                    : ladder ? std::snprintf(odds + n, sizeof(odds) - n, "%s%s %s", n ? "  \xc2\xb7  " : "", h.name, e)
                             : std::snprintf(odds + n, sizeof(odds) - n, "%s%s %.0f%%, %s", n ? "  \xc2\xb7  " : "", h.name, 100*h.c, e);
        if (w > 0) n = std::min(sizeof(odds) - 1, n + static_cast<size_t>(w));
        scored = scored || in;
    }
    if (scored) ImGui::Text("Expected  %s", odds);
    else ImGui::TextDisabled("Expected: not scored yet");
    if (src.ratio != 1) ImGui::TextDisabled("At %s on %s, mapped by today's basis", own, liqmap2::venue_name(src.exchange));
    const bool ended = r.end_ms > 0 && r.end_ms <= asof;
    ImGui::TextDisabled("%s%s \xc2\xb7 born %s \xc2\xb7 %s %s", liqmap2::venue_name(src.exchange),
                        src.exchange == "bybit" ? "" : " (provisional)", born, ended ? "ended" : "as of", ended ? r.end.c_str() : stamp);
    Theme::end_tooltip();
}
}  // namespace liqmap2_rails_overlay
