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
#include "core/display_time_zone.h"
#include "core/liqmap2_rails.h"
#include "core/liqmap2_store.h"
#include "core/liqmap2_release.h"
#include "rendering/theme.h"
#include "implot.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace liqmap2_rails_overlay {
inline constexpr float kWidth = 4.0f;      // bar height, px: crisp bars as ref-6/7, never hairlines (rule 5)
inline constexpr int kAxisLabels = 4;      // live rails labelled beside the profile, largest first
inline constexpr float kRowPitch = 5.0f;   // profile rows: a 4 px bar and a 1 px gap
inline constexpr float kMinRunPx = 8.0f;   // a line's stretch narrower than this joins the next (one colour, no dashes)
inline constexpr float kMinLinePx = 12.0f; // a whole line shorter than this is a speck, not a rail: the profile has it
inline constexpr double kPeakGapPx = 12.0; // a ladder's lines stand at least this far apart (liqmap2_rails::space_hills)

// The price kPeakGapPx spans at the plot's zoom, on a ladder of steps 2^(1/4) apart (so 11 to 13.5 px): a zoom, or
// the price axis refitting the candles as the view pans, that moves less than a step keeps the merged runs (the costly
// part), and the lines in view do not regroup at every wheel notch.
inline double peak_gap(double y_range, float plot_h) {
    const double g = kPeakGapPx*y_range/std::max(1.0f, plot_h);
    return g > 0 && std::isfinite(g) ? std::exp2(std::round(std::log2(g)*4)/4) : 0;
}

// The span the runs are merged over, for a view [lo, hi]: g is a quarter of the view's width on the 2^(1/4) ladder;
// the span starts one to two g before lo, on a grid of g, and runs a fixed 7 g, so it ends past hi by at least 0.6 g.
// It moves only when lo crosses a grid line, so a pan redoes the merge (the costly part) about once per quarter view,
// over less than two views, not every frame (James 2026-10-18: 2.7 ms a frame panning AGT on 1 h with the price axis
// fixed, 8.1 ms with it refitting the candles, the merge most of it).
struct Window {
    double lo = 0, hi = 0;
    bool operator==(const Window&) const = default;
};
inline Window pan_window(double lo, double hi) {
    const double w = hi - lo;
    if (!(w > 0) || !std::isfinite(w) || !std::isfinite(lo)) return {lo, hi};
    const double g = std::exp2(std::round(std::log2(w)*4)/4)/4;
    const double start = (std::floor(lo/g) - 1)*g;
    return {start, start + 7*g};
}

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

// When the V2 rails and the Hyperliquid positions (rendering/hl_rails_overlay.h) both draw, one leads: it draws on top
// with its profile and labels, and its hover wins. The other follows: thin and dim beneath, only its rails standing at
// the clock and only over the leader's window (its history would fill the chart), still with a hover.
enum class Role { alone, lead, follow };

// One venue's rails for the chart (one map per coin across venues, James 2026-10-10): its store, its venue, and
// the factor that maps its prices onto the chart's, the chart venue's mark over this venue's mark (1 for the chart's
// own venue). The factor is today's basis applied to every segment in view; a rail's own venue price is in its hover.
struct Source {
    const liqmap2_rails::Store* store = nullptr;
    std::string exchange;
    double ratio = 1;
};

// The chart's candles (open time ms ascending, lows, highs), the forming one last (liqmap2_rails::ChartCandles), and
// the plot's time shift (candles sit centred on their open, so a real time t is drawn at t - shift).
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
// Draws the chip; true while the pointer is over it, when the caller shows its hover (so a frame writes the hover's
// lines only then).
inline bool source_chip(ImDrawList* draw, ImVec2 at, const char* label, const char* status, bool warn) {
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const float px = ImGui::GetFontSize();
    ImGui::PopFont();
    Theme::push_font(Theme::Fonts::ui(), px);
    char text[160];
    if (status && *status) std::snprintf(text, sizeof(text), "%s  %s", label, status);
    else std::snprintf(text, sizeof(text), "%s", label);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 end(at.x + ts.x + 16, at.y + ts.y + 8);
    draw->AddRectFilled(at, end, Theme::u32(Theme::Tokens::PANEL, 0.82f), Theme::Radius::R1);
    draw->AddText(ImVec2(at.x + 8, at.y + 4), Theme::u32(warn ? Theme::Tokens::WARN : Theme::Tokens::TX2), text);
    ImGui::PopFont();
    Theme::chart_chip(at, end);
    const auto mouse = ImGui::GetIO().MousePos;
    return ImPlot::IsPlotHovered() && !(mouse.x < at.x || mouse.x > end.x || mouse.y < at.y || mouse.y > end.y);
}
inline bool key_chip(ImDrawList* draw, ImVec2 at, const char* basis, const char* max_label, bool warn) {
    const char* status = warn ? "check data" : !max_label ? (std::string_view(basis).starts_with("none") ? "no record" : "waiting") : "estimated";
    return source_chip(draw, at, "Map", status, warn || !max_label);
}

// The rail and profile hovers (James 2026-10-13: "less verbose, more UI/UX hierarchy"): a header (what and where, the
// venue at the right), the dollars large with what they mean beside them, then aligned label / value rows and a small
// muted footer between hairlines. Each line is laid out in a box of known width, so values align and the hover keeps
// its size from rail to rail. Sizes follow the tooltip's own text size, as every other hover reads.
namespace hover {
// Text in one face and size.
struct Span {
    ImFont* font = nullptr;
    float px = 0;
    const char* text = "";
    ImVec4 col;
    float w() const { return text && *text ? font->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x : 0.0f; }
    float ascent() const {
        const ImFontBaked* b = font->GetFontBaked(px);
        return b->Ascent*px/b->Size;
    }
};

// The type scale: body (labels, figures, the sub line) at the tooltip's text size; small (the side word, the footer);
// hero (the dollars); the least width.
struct Type {
    float body, small, hero, min_w;
    Span words(const char* t, const ImVec4& c) const { return {Theme::Fonts::ui(), body, t, c}; }
    Span quiet(const char* t) const { return {Theme::Fonts::ui(), small, t, Theme::Tokens::TX3}; }
    // Roboto Mono's digits stand about a tenth shorter than Inter's at one size: lifted to read as one line with them.
    Span figure(const char* t) const { return {Theme::Fonts::mono(), std::round(body*1.1f), t, Theme::Tokens::TX1}; }
};

// Spacing is explicit: begin pushes no item spacing, gap adds it. replace: this hover wins over one opened earlier in
// the frame (the leading layer over a follower beneath it).
inline Type begin(bool replace = false) {
    Theme::begin_tooltip(replace);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 0.0f));
    const float b = ImGui::GetFontSize();
    return {b, std::round(b*0.85f), std::round(b*1.4f), std::round(b*16.0f)};
}
inline void end() {
    ImGui::PopStyleVar();
    Theme::end_tooltip();
}
inline void gap(float px) { ImGui::Dummy(ImVec2(0.0f, px)); }

// One line: each span at its x within the box (w wide), all on one baseline. Returns the line's top left.
struct At { float x; Span s; };
inline ImVec2 line(float w, std::initializer_list<At> spans) {
    const ImVec2 o = ImGui::GetCursorScreenPos();
    float base = 0.0f, h = 0.0f;
    for (const auto& a : spans) {
        base = std::max(base, a.s.ascent());
        h = std::max(h, a.s.px);
    }
    auto* dl = ImGui::GetWindowDrawList();
    for (const auto& a : spans)
        if (a.s.text && *a.s.text)
            dl->AddText(a.s.font, a.s.px, ImVec2(std::round(o.x + a.x), std::round(o.y + base - a.s.ascent())),
                        Theme::u32(a.s.col), a.s.text);
    ImGui::Dummy(ImVec2(w, h));
    return o;
}

// A hairline across the box, with room around it.
inline void rule(float w) {
    gap(8.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 0.5f), ImVec2(p.x + w, p.y + 0.5f), Theme::u32(Theme::Tokens::BD2));
    gap(9.0f);
}

// A time in the display zone, dated only when its day is not the clock's.
inline void when(int64_t t, int64_t clock, char* out, size_t n) {
    const auto& tz = DisplayTimeZone::instance();
    char day[16] = "", today[16] = "";
    tz.format(t, TimeZoneFormat::MonthDay, day, sizeof(day));
    tz.format(clock, TimeZoneFormat::MonthDay, today, sizeof(today));
    if (!tz.format(t, std::strcmp(day, today) ? TimeZoneFormat::MonthDayTime : TimeZoneFormat::TimeMinutes, out, n))
        std::snprintf(out, n, "?");
}
}  // namespace hover

// What the overlay draws for one chart, worked out once and kept while nothing it depends on changes (James
// 2026-10-14: 20 FPS zoomed out). Merging days of hourly steps on two venues into runs took tens of milliseconds, and
// it ran every frame. A frame reuses the picture while the stores, candles, view, plot, colour and profile are as
// they were and the clock has not reached anything that changes it: a rail's birth, step or end, the view's left
// edge, a candle's open, the present column, a line's start past a candle body, or the moment a short pushed line
// grows long enough to draw. Until then the runs standing at the clock follow it, as a fresh pass would draw them.
struct Cache {
    struct Venue {
        const liqmap2_rails::Store* store = nullptr;
        uint64_t revision = 0;
        double ratio = 1;
        bool operator==(const Venue&) const = default;
    };
    struct Key {
        std::vector<Venue> venues;
        const std::vector<double>* t = nullptr;
        const std::vector<double>* lo = nullptr;
        const std::vector<double>* hi = nullptr;
        size_t candles = 0;
        uint64_t candles_revision = 0;
        double shift = 0, x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        float px = 0, py = 0, pw = 0, ph = 0;
        liqmap2_rails::Colour chosen = liqmap2_rails::Colour::expected;
        bool profile = false, replay = false;
        bool operator==(const Key&) const = default;
    };
    struct Row { double v = 0; int rails = 0; int level = -1; };
    bool valid = false;
    Key key;
    Key next;                             // this frame's key, kept for its capacity (a frame allocates nothing)
    int64_t asof = 0, until = 0;          // the picture holds for a clock in [asof, until), or at asof alone
    bool any = false;                     // data in hand: a live message, or stored rails born by the clock
    liqmap2_rails::Colour colour = liqmap2_rails::Colour::expected;
    double vmax = 0;
    std::vector<liqmap2_rails::Run> rails;  // dimmest first
    std::vector<int> levels;                // each rail's colour step
    std::vector<size_t> at_clock;           // rails that end at the clock, and so follow it
    std::vector<size_t> live;               // rails standing at the clock, most dollars first (the labels)
    std::vector<std::pair<int, const liqmap2_rails::Rail*>> standing;  // the live rails by venue and rail (a follower)
    std::vector<Row> rows;                  // the profile
    double row_max = 0;
    // Under the picture, each venue's segments over the pan window and its merged runs (the costly part), kept while
    // the store, the window and the line rule hold and the clock has not reached a rail's start, step or known end
    // (until): a replay's view follows the playhead every frame, a pan moves the view every frame, and the clock moves
    // every frame, yet the rails and their steps change only every few minutes of data. Segments open at the clock end
    // at it; so do their runs. The runs are redone when the colour or the spacing changes.
    struct Runs {
        const liqmap2_rails::Store* store = nullptr;
        uint64_t revision = 0;
        Window t, p;
        bool from_birth = false;
        int64_t asof = 0, until = 0;
        std::vector<liqmap2_rails::Segment> segments;
        bool merged = false;
        liqmap2_rails::Colour colour = liqmap2_rails::Colour::expected;
        double min_gap = 0;
        std::vector<liqmap2_rails::Run> runs;
    };
    std::vector<Runs> venues;
};

// One venue's segments over the window [t] x [p] (venue prices) at asof, from its last ones while they hold; the open
// ones (and runs) follow the clock.
inline const std::vector<liqmap2_rails::Segment>& venue_segments(Cache::Runs& kept, const liqmap2_rails::Store* store,
                                                                 int64_t asof, int64_t until, Window t, Window p,
                                                                 bool from_birth) {
    const bool same = kept.store == store && store && kept.revision == store->revision() && kept.t == t && kept.p == p &&
                      kept.from_birth == from_birth && asof >= kept.asof && asof < kept.until;
    if (same) {
        if (asof != kept.asof) {
            for (auto& sg : kept.segments)
                if (sg.t1 == kept.asof) sg.t1 = asof;
            for (auto& r : kept.runs)
                if (r.t1 == kept.asof) r.t1 = asof;
        }
    } else {
        double vm = 0;
        kept.segments = store ? liqmap2_rails::segments(store->rails(), asof, t.lo, t.hi, p.lo, p.hi, vm,
                                                        liqmap2_rails::Colour::dollars, from_birth)
                              : std::vector<liqmap2_rails::Segment>{};
        kept.store = store;
        kept.revision = store ? store->revision() : 0;
        kept.t = t;
        kept.p = p;
        kept.from_birth = from_birth;
        kept.until = until;
        kept.merged = false;
    }
    kept.asof = asof;
    return kept.segments;
}

// That venue's runs by colour, merged with the spacing min_gap, from its last ones while the segments, colour and
// spacing hold.
inline const std::vector<liqmap2_rails::Run>& venue_runs(Cache::Runs& kept, liqmap2_rails::Colour colour, double min_gap) {
    if (!kept.merged || kept.colour != colour || kept.min_gap != min_gap) {
        for (auto& sg : kept.segments) sg.v = liqmap2_rails::value(*sg.step, colour);
        kept.runs = liqmap2_rails::runs(kept.segments, min_gap);
        kept.merged = true;
        kept.colour = colour;
        kept.min_gap = min_gap;
    }
    return kept.runs;
}

// Works out the picture at the clock asof, inside the price plot, and how long it holds (cache.until).
inline void build(Cache& cache, const std::vector<Source>& sources, const Candles& candles, int64_t asof,
                  int64_t now_edge, liqmap2_rails::Colour chosen, bool profile, bool replay) {
    using liqmap2_rails::Run;
    const auto pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    const auto limits = ImPlot::GetPlotLimits();
    const double shift = candles.shift_ms;
    const double t_lo = limits.X.Min + shift, t_hi = limits.X.Max + shift;
    const auto x_of = [&](double t) { return ImPlot::PlotToPixels(t - shift, 0).x; };
    const bool have_candles = candles.t && candles.lo && candles.hi;
    cache.rails.clear();
    cache.levels.clear();
    cache.at_clock.clear();
    cache.live.clear();
    cache.rows.clear();
    cache.standing.clear();
    cache.vmax = cache.row_max = 0;
    cache.colour = chosen;
    // The next moment the picture can change: anything at or after the clock that the passes below compare it with.
    int64_t until = INT64_MAX;
    const auto ahead = [&](int64_t t) { if (t >= asof) until = std::min(until, t); };
    for (const auto& src : sources) {
        if (!src.store) continue;
        for (const auto& [id, r] : src.store->rails()) {
            ahead(liqmap2_rails::start_ms(r));
            if (r.end_ms > 0) ahead(liqmap2_rails::end_known_ms(r));
            for (const auto& s : r.steps) ahead(s.at_ms);
        }
    }
    if (std::isfinite(t_lo) && t_lo >= static_cast<double>(asof) && t_lo < 9.2e18)
        ahead(static_cast<int64_t>(std::ceil(t_lo)));
    if (have_candles) {
        const auto& t = *candles.t;
        auto it = std::lower_bound(t.begin(), t.end(), static_cast<double>(asof));
        if (it != t.end()) ahead(static_cast<int64_t>(std::ceil(*it)));
    }
    if (now_edge > asof) ahead(now_edge);
    // Data in hand: a live message, or stored rails (a replay reads history only) published by the clock.
    cache.any = std::any_of(sources.begin(), sources.end(), [&](const Source& src) {
        if (!src.store) return false;
        if (!replay && src.store->latest().timestamp_ms != 0) return true;
        return std::any_of(src.store->rails().begin(), src.store->rails().end(),
                           [&](const auto& kv) { return liqmap2_rails::start_ms(kv.second) <= asof; });
    });
    cache.until = until;
    if (!cache.any) return;

    // Every venue's rails in view, merged into runs (ladders draw as one rail, its lines at least kPeakGapPx apart) and
    // mapped to chart prices, on one scale. They are merged over the pan window (pan_window: a sixth of a view or more
    // past each price edge, a quarter or more of history before the left), so a ladder crossing the edge stays one run
    // and a pan reuses the merge; only what reaches the view is kept. Live, a line starts at the rail's birth; a replay draws it as published then (liqmap2_rails::line_from).
    // The colour first: the viewer's choice, unless a rail in view has no figure for it yet (then dollars).
    const bool from_birth = !replay;
    const double gap = peak_gap(limits.Y.Max - limits.Y.Min, size.y);
    const auto t_win = pan_window(t_lo, t_hi);
    const auto p_win = pan_window(limits.Y.Min, limits.Y.Max);
    cache.venues.resize(sources.size());
    bool unknown = false;
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto& src = sources[i];
        if (!src.store || !(src.ratio > 0)) {
            cache.venues[i] = {};
            continue;
        }
        const auto& segs = venue_segments(cache.venues[i], src.store, asof, until, t_win,
                                          {p_win.lo/src.ratio, p_win.hi/src.ratio}, from_birth);
        if (chosen != liqmap2_rails::Colour::dollars)
            for (const auto& sg : segs)
                if (liqmap2_rails::value(*sg.step, chosen) < 0 && sg.price*src.ratio >= limits.Y.Min &&
                    sg.price*src.ratio <= limits.Y.Max && sg.t1 >= t_lo && sg.t0 <= t_hi) {
                    unknown = true;
                    break;
                }
    }
    if (unknown) cache.colour = liqmap2_rails::Colour::dollars;
    std::vector<Run> all;
    for (size_t i = 0; i < sources.size(); ++i) {
        if (cache.venues[i].segments.empty()) continue;
        const double ratio = sources[i].ratio;
        for (auto r : venue_runs(cache.venues[i], cache.colour, gap/ratio)) {
            r.price *= ratio; r.lo *= ratio; r.hi *= ratio; r.near *= ratio;
            r.source = static_cast<int>(i);
            if (r.price < limits.Y.Min || r.price > limits.Y.Max || r.t1 < t_lo || r.t0 > t_hi) continue;
            all.push_back(r);
        }
    }
    // Each line (a venue's representative rail) in time order, its stretches together.
    const auto same_line = [](const Run& a, const Run& b) { return a.source == b.source && a.rep == b.rep; };
    std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) {
        return a.source != b.source ? a.source < b.source : a.rep != b.rep ? a.rep < b.rep : a.t0 < b.t0;
    });
    std::vector<char> pushed;  // per stretch: its line started past a candle's body
    pushed.reserve(all.size());
    // Candles consume: each line ends at the first candle touching its (chart) price after its line began.
    if (have_candles) {
        std::vector<Run> kept;
        kept.reserve(all.size());
        for (size_t g0 = 0; g0 < all.size();) {
            size_t g1 = g0 + 1;
            while (g1 < all.size() && same_line(all[g1], all[g0])) ++g1;
            const Run& head = all[g0];
            // The bar's half-height in price at its row, so a candle reaching any part of the row consumes it.
            const float y = ImPlot::PlotToPixels(0, head.price).y;
            const double slack = std::abs(ImPlot::PixelsToPlot(ImVec2(0, y - (kWidth*0.5f + 1.0f))).y - head.price);
            // Live, a line drawn from its birth reaches back from its publication only past the last candle touching it.
            const int64_t first = from_birth ? liqmap2_rails::clear_back(*candles.t, *candles.lo, *candles.hi,
                                                                         head.rep->is_long, head.price, head.t0,
                                                                         liqmap2_rails::start_ms(*head.rep),
                                                                         static_cast<int64_t>(2*shift), slack)
                                             : head.t0;
            const int64_t end = liqmap2_rails::consumed_at(*candles.t, *candles.lo, *candles.hi, head.rep->is_long,
                                                           head.price, first, static_cast<int64_t>(t_hi) + 1, slack);
            const int64_t start = liqmap2_rails::clear_start(*candles.t, *candles.lo, *candles.hi, head.price, first,
                                                             static_cast<int64_t>(2*shift), slack);
            ahead(start);  // a line born inside the latest candle starts past its body: drawn once the clock is there
            for (size_t i = g0; i < g1; ++i) {
                Run r = all[i];
                r.t0 = std::max(r.t0, start);
                if (r.t0 >= end || r.t0 >= std::max(r.t1, r.t1 >= asof ? now_edge : r.t1)) continue;
                r.t1 = std::min(r.t1, end);
                // Still standing at the clock: on to the present column.
                if (r.t1 >= asof && end == INT64_MAX && liqmap2_rails::end_at(*r.rep, asof) == 0) r.t1 = std::max(r.t1, now_edge);
                kept.push_back(r);
                pushed.push_back(start > first);
            }
            g0 = g1;
        }
        all.swap(kept);
    } else {
        pushed.assign(all.size(), 0);
    }
    // One colour per stretch: a line's hourly steps sit a few pixels apart on 4 h and 1 D; a stretch narrower
    // than kMinRunPx joins the line's next stretch and takes its figures (the hover still reads each step).
    std::vector<Run> merged;
    std::vector<char> merged_pushed;
    merged.reserve(all.size());
    for (size_t i = 0; i < all.size(); ++i) {
        auto r = all[i];
        while (i + 1 < all.size() && same_line(all[i + 1], r) && all[i + 1].t0 == r.t1 &&
               x_of(double(r.t1)) - x_of(double(r.t0)) < kMinRunPx) {
            const int64_t t0 = r.t0;
            r = all[++i];
            r.t0 = t0;
        }
        merged.push_back(r);
        merged_pushed.push_back(pushed[i]);
    }
    // Specks: a line that stands only a sliver at this zoom (on 4 h and 1 D, one born inside a candle whose range
    // holds it, so started past that candle's body, or one that ended almost at once) is noise beside the
    // candles; the profile still counts it while it stands. A rail born moments ago, clear of the candles, stays:
    // it is new, not noise.
    std::vector<Run>& out = cache.rails;
    out.reserve(merged.size());
    for (size_t g0 = 0; g0 < merged.size();) {
        size_t g1 = g0 + 1;
        int64_t a = merged[g0].t0, b = merged[g0].t1;
        for (; g1 < merged.size() && same_line(merged[g1], merged[g0]); ++g1) {
            a = std::min(a, merged[g1].t0);
            b = std::max(b, merged[g1].t1);
        }
        const bool fresh = b >= asof && !merged_pushed[g0];
        const auto width = [&](double until_t) {
            return x_of(std::min(until_t, t_hi)) - x_of(std::max(double(a), t_lo));
        };
        if (fresh || width(double(b)) >= kMinLinePx) {
            out.insert(out.end(), merged.begin() + static_cast<ptrdiff_t>(g0),
                       merged.begin() + static_cast<ptrdiff_t>(g1));
        } else if (b == asof && width(t_hi) >= kMinLinePx) {
            // A pushed line standing at the clock grows with it: the picture holds until it is long enough to draw.
            int64_t lo = asof, hi = static_cast<int64_t>(std::ceil(t_hi));
            while (hi - lo > 1) {
                const int64_t mid = lo + (hi - lo)/2;
                (width(double(mid)) >= kMinLinePx ? hi : lo) = mid;
            }
            ahead(hi);
        }
        g0 = g1;
    }
    for (const auto& r : out) cache.vmax = std::max(cache.vmax, r.v);
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.v < b.v; });
    cache.levels.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        cache.levels.push_back(liqmap2_rails::level(out[i].v, cache.vmax));
        if (out[i].t1 == asof) cache.at_clock.push_back(i);
        if (out[i].t1 >= asof && liqmap2_rails::end_at(*out[i].rep, asof) == 0 && out[i].v > 0)
            cache.live.push_back(i);
    }
    std::stable_sort(cache.live.begin(), cache.live.end(), [&](size_t a, size_t b) { return out[a].v > out[b].v; });
    // A follower draws only these (render).
    cache.standing.clear();
    for (size_t i : cache.live) cache.standing.push_back({out[i].source, out[i].rep});
    std::sort(cache.standing.begin(), cache.standing.end());

    // The profile: every live rail of either venue (as merged nowhere: each rail's own zone), not consumed by the
    // chart's candles, its value spread over the rows its zone covers. Rows 5 px apart keep it discrete at any zoom.
    if (profile) {
        auto& rows = cache.rows;
        rows.resize(static_cast<size_t>(std::max(1.0f, std::ceil(size.y/kRowPitch))));
        for (const auto& src : sources) {
            if (!src.store || !(src.ratio > 0)) continue;
            for (const auto& [id, r] : src.store->rails()) {
                if (!liqmap2_rails::standing(r, asof)) continue;
                const auto* st = liqmap2_rails::step_at(r, asof);
                const double v = liqmap2_rails::value(*st, cache.colour);
                const double price = r.price*src.ratio;
                if (!(v > 0) || price < limits.Y.Min || price > limits.Y.Max) continue;
                if (have_candles) {
                    int64_t from = liqmap2_rails::line_from(r, asof, from_birth);
                    if (from_birth)
                        from = liqmap2_rails::clear_back(*candles.t, *candles.lo, *candles.hi, r.is_long, price, from,
                                                         liqmap2_rails::start_ms(r), static_cast<int64_t>(2*shift));
                    if (liqmap2_rails::consumed_at(*candles.t, *candles.lo, *candles.hi, r.is_long, price, from, asof + 1) !=
                        INT64_MAX)
                        continue;
                }
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
        for (const auto& row : rows) cache.row_max = std::max(cache.row_max, row.v);
        for (auto& row : rows) row.level = liqmap2_rails::level(row.v, cache.row_max);
    }
    cache.until = until;
}

// Called inside the price plot. top_y: pixels below the plot top for the key. profile: draw the live rails' profile
// at the right edge (the chart reserves room for it). short_horizons: show the 1 h and 4 h figures in the hover
// (admins). They are unvalidated for customers until their forward look passes (PREREG_RAILS_HORIZONS_FORWARD.md):
// in development their expected dollars overstate several times on ordinary days (hub RESULT_RAILS_H1B_DEV.md).
// cache: the chart's own (one per chart, as each draws its own view). candles_revision: changes whenever the candles
// do, the forming candle's range included (ChartCandles::revision). role: with the Hyperliquid positions also drawn
// (Role above); a follower has no profile (so the cache keys on it) and draws from follow_from, the leader's first
// data.
inline void render(Cache& cache, const std::vector<Source>& sources, const Candles& candles, uint64_t candles_revision,
                   int64_t asof, [[maybe_unused]] const char* symbol, const char* price_fmt, float top_y,
                   liqmap2_rails::Colour chosen, bool profile, bool short_horizons = true, bool replay = false,
                   Role role = Role::alone, int64_t follow_from = 0, bool value_labels = false) {
    const bool follow = role == Role::follow;
    profile = profile && !follow;
    const auto pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    const auto limits = ImPlot::GetPlotLimits();
    const double shift = candles.shift_ms;
    const double t_lo = limits.X.Min + shift, t_hi = limits.X.Max + shift;  // the view in real time
    const auto x_of = [&](double t) { return ImPlot::PlotToPixels(t - shift, 0).x; };
    auto* draw = ImPlot::GetPlotDrawList();
    const auto mouse = ImGui::GetIO().MousePos;
    const bool hovered = ImPlot::IsPlotHovered();
    const ImVec2 key_at(pos.x + 10, pos.y + top_y);
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
    Cache::Key& key = cache.next;
    key.venues.clear();
    for (const auto& src : sources)
        key.venues.push_back({src.store, src.store ? src.store->revision() : 0, src.ratio});
    key.t = key.lo = key.hi = nullptr;
    key.candles = 0;
    key.candles_revision = 0;
    if (have_candles) {
        key.t = candles.t; key.lo = candles.lo; key.hi = candles.hi;
        key.candles = std::min({candles.t->size(), candles.lo->size(), candles.hi->size()});
        key.candles_revision = candles_revision;
    }
    key.shift = shift;
    key.x0 = limits.X.Min; key.x1 = limits.X.Max; key.y0 = limits.Y.Min; key.y1 = limits.Y.Max;
    key.px = pos.x; key.py = pos.y; key.pw = size.x; key.ph = size.y;
    key.chosen = chosen; key.profile = profile; key.replay = replay;
    if (!cache.valid || !(cache.key == key) || !(asof == cache.asof || (asof > cache.asof && asof < cache.until))) {
        cache.valid = true;
        std::swap(cache.key, cache.next);
        cache.asof = asof;
        build(cache, sources, candles, asof, now_edge, chosen, profile, replay);
    }
    for (size_t i : cache.at_clock) cache.rails[i].t1 = asof;
    ImPlot::PushPlotClipRect();
    if (!cache.any) {
        if (key_chip(draw, key_at, replay ? "none recorded here" : "waiting for the first message", nullptr, false)) {
            Theme::begin_tooltip();
            if (replay) {
                ImGui::TextUnformatted("No rails stored for this replay window.");
                ImGui::TextUnformatted("Rails are recorded from 9 Oct 2026 (Bybit) and 10 Oct (Binance).");
            } else {
                ImGui::TextUnformatted("No rails message yet.");
            }
            Theme::end_tooltip();
        }
        ImPlot::PopPlotClipRect();
        return;
    }
    const auto& rs = cache.rails;
    const double vmax = cache.vmax;
    const auto colour = cache.colour;
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

    // Rails as crisp bars, brightest last; the brightest step a pixel heavier so the few big ones lead. A follower draws
    // only the rails standing at the clock, from the leader's first data.
    const auto& standing = cache.standing;
    const liqmap2_rails::Run* hit = nullptr;
    float hit_d = 1e9f;
    for (size_t i = 0; i < rs.size(); ++i) {
        const auto& r = rs[i];
        if (follow && (r.t1 < follow_from || !std::binary_search(standing.begin(), standing.end(), std::make_pair(r.source, r.rep))))
            continue;
        const float x0 = std::round(x_of(std::max({double(r.t0), t_lo, double(follow ? follow_from : 0)})));
        const float x1 = std::round(x_of(std::min(double(r.t1), t_hi)));
        if (x1 - x0 < 3.0f) continue;   // a speck reads as noise, not a rail
        const float y = y_of(r.price);
        const int lv = cache.levels[i];
        const float th = follow ? 2.0f : lv == liqmap2_rails::kLevels - 1 ? kWidth + 1.0f : kWidth;
        const float y0 = y - std::floor(th*0.5f);
        draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + th), color(lv, follow ? 0.4f : 1.0f));
        const float d = std::abs(mouse.y - y);
        if (hovered && mouse.x >= x0 && mouse.x <= x1 && d <= th*0.5f + 3.0f && d < hit_d) {
            hit_d = d;
            hit = &r;
        }
    }

    // The profile (worked out with the rails, in the cache).
    const auto& rows = cache.rows;
    int prof_hit = -1;
    if (profile && prof_w > 0 && cache.row_max > 0) {
        for (size_t k = 0; k < rows.size(); ++k) {
            if (!(rows[k].v > 0)) continue;
            const float len = std::max(2.0f, std::round(prof_w*static_cast<float>(rows[k].v/cache.row_max)));
            const float y0 = pos.y + k*kRowPitch;
            draw->AddRectFilled(ImVec2(prof_x1 - len, y0), ImVec2(prof_x1, y0 + kRowPitch - 1.0f), color(rows[k].level, 0.92f));
            if (hovered && mouse.x >= prof_x0 && mouse.x <= prof_x1 && mouse.y >= y0 && mouse.y < y0 + kRowPitch)
                prof_hit = static_cast<int>(k);
        }
    }

    // Dollar labels for the largest live rails (runs) across venues, beside the profile, never overlapping.
    float used[kAxisLabels];
    int n_used = 0;
    ImGui::PushFont(Theme::Fonts::mono_sm());
    for (size_t i : cache.live) {
        const auto* r = &rs[i];
        if (!value_labels || follow || n_used >= kAxisLabels) break;
        const float y = y_of(r->price);
        if (std::any_of(used, used + n_used, [&](float u) { return std::abs(u - y) < ImGui::GetTextLineHeight() + 4.0f; })) continue;
        char amount[32];
        liqmap2::money(r->v, amount, sizeof(amount));
        const ImVec2 ts = ImGui::CalcTextSize(amount);
        const float right = profile ? prof_x0 - 6.0f : pos.x + size.x - 6.0f;
        const ImVec2 p0(right - ts.x - 12.0f, y - ts.y*0.5f - 2.0f), p1(right, y + ts.y*0.5f + 2.0f);
        if (p0.x < x_of(static_cast<double>(now_edge)) + 6.0f) break;  // no room after now: never over the candles
        draw->AddRectFilled(p0, p1, Theme::u32(Theme::Tokens::PANEL, 0.88f), Theme::Radius::R1);
        draw->AddRectFilled(p0, ImVec2(p0.x + 3.0f, p1.y), color(cache.levels[i]));
        draw->AddText(ImVec2(p0.x + 8.0f, p0.y + 2.0f), Theme::u32(Theme::Tokens::TX1), amount);
        used[n_used++] = y;
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
    // Each venue's state: "recorded" in a replay, else its newest message's status ("waiting" before one).
    const auto venue_state = [&](const liqmap2_rails::Latest& latest) {
        return replay ? "recorded" : latest.timestamp_ms ? latest.status.c_str() : "waiting";
    };
    bool warn = false;
    for (const auto& src : sources) {
        if (!src.store) continue;
        const char* state = venue_state(src.store->latest());
        warn = warn || (std::strcmp(state, "ready") != 0 && std::strcmp(state, "recorded") != 0);
    }
    // The hover's lines are written only while the pointer is over the key.
    if (key_chip(draw, key_at, basis_text, max_label, warn)) {
        std::vector<std::string> lines;
        lines.push_back(std::string("Liquidation rails, coloured by $ ") + colour_name(colour) + ". Brightest " + max_label + ".");
        if (replay) lines.push_back("Replay: the rails as published at the time.");
        if (colour != chosen)
            lines.push_back(std::string("The ") + horizon(chosen) + " figure is not in for every rail in view yet, so the view colours by dollars.");
        for (const auto& src : sources) {
            if (!src.store) continue;
            const auto& latest = src.store->latest();
            lines.push_back(std::string(liqmap2::venue_name(src.exchange)) + ": " + venue_state(latest) +
                            (latest.provisional ? " (provisional)" : ""));
        }
        lines.push_back("A rail ends at the candle that reaches it. Not a magnet.");
        lines.push_back(std::string(liqmap2::kForwardTest) + ".");
        Theme::begin_tooltip();
        for (const auto& line : lines) ImGui::TextUnformatted(line.c_str());
        Theme::end_tooltip();
    }
    ImPlot::PopPlotClipRect();

    if (prof_hit >= 0 && !hit) {
        // The profile row: its price span and how many live rails it sums (header), its dollars (hero).
        const float y0 = pos.y + prof_hit*kRowPitch;
        char a[48], b[48], v[32], span[104], count[48];
        std::snprintf(a, sizeof(a), price_fmt, ImPlot::PixelsToPlot(ImVec2(0, y0 + kRowPitch)).y);
        std::snprintf(b, sizeof(b), price_fmt, ImPlot::PixelsToPlot(ImVec2(0, y0)).y);
        std::snprintf(span, sizeof(span), "%s to %s", a, b);
        std::snprintf(count, sizeof(count), "%d live rail%s, both venues", rows[prof_hit].rails, rows[prof_hit].rails == 1 ? "" : "s");
        liqmap2::money(rows[prof_hit].v, v, sizeof(v));
        const auto type = hover::begin(role == Role::lead);
        const hover::Span where = type.figure(span), live = type.words(count, Theme::Tokens::TX3);
        const hover::Span usd{Theme::Fonts::heading(), type.hero, v, Theme::Tokens::TX1};
        const hover::Span what = type.words(colour_name(colour), Theme::Tokens::TX2);
        const float w = std::round(std::max({type.min_w, where.w() + 20.0f + live.w(), usd.w() + 7.0f + what.w()}));
        hover::line(w, {{0, where}, {w - live.w(), live}});
        hover::gap(9.0f);
        hover::line(w, {{0, usd}, {usd.w() + 7.0f, what}});
        hover::end();
    }
    if (!hit) return;
    // The rail's or the ladder's hover at the hovered time, never past the clock (a replay must not show a rail's
    // later step or its end): side and price (a ladder: its span and count) with the venue; the dollars forced out if
    // price reaches the zone; the odds in hand (customers 24 h only, 1 h and 4 h await their forward look; a ladder its
    // expected dollars only); born and as of (or ended), and for the other venue's rail its own price.
    const auto& src = sources[hit->source];
    const auto& r = *hit->rep;
    const int64_t at = std::min(asof, static_cast<int64_t>(ImPlot::PixelsToPlot(mouse).x + shift));
    const auto* s = liqmap2_rails::step_at(r, at);
    const bool ladder = hit->members > 1;
    const bool ended = liqmap2_rails::end_at(r, asof) > 0;
    const bool provisional = src.exchange != "bybit";
    char price[48], own[48], lo[48], hi[48], span[104], count[24], usd[32], where[128];
    std::snprintf(price, sizeof(price), price_fmt, hit->price);
    std::snprintf(own, sizeof(own), price_fmt, r.price);
    std::snprintf(lo, sizeof(lo), price_fmt, hit->lo);
    std::snprintf(hi, sizeof(hi), price_fmt, hit->hi);
    std::snprintf(span, sizeof(span), "%s to %s", lo, hi);
    std::snprintf(count, sizeof(count), "%d rails", hit->members);
    liqmap2::money(ladder ? hit->usd : s->usd, usd, sizeof(usd));
    if (ladder) std::snprintf(where, sizeof(where), "if price runs through it");
    else std::snprintf(where, sizeof(where), "if price reaches %s", span);

    // The odds rows: "24h chance 12%  expected $370"; a ladder "24h expected $370"; "pending" where a figure is not in.
    struct Odds { char label[24], chance[16], expected[24]; bool chance_in, expected_in; };
    Odds odds[3];
    int n = 0;
    bool scored = false;
    const struct { const char* name; double c, e; } hz[3] = {
        {"1h", ladder ? -1.0 : s->chance_1h, ladder ? hit->e1 : s->expected_1h},
        {"4h", ladder ? -1.0 : s->chance_4h, ladder ? hit->e4 : s->expected_4h},
        {"24h", ladder ? -1.0 : s->chance, ladder ? hit->e24 : s->expected}};
    for (const auto& h : hz) {
        if (!short_horizons && h.name[0] != '2') continue;
        Odds& o = odds[n++];
        std::snprintf(o.label, sizeof(o.label), "%s %s", h.name, ladder ? "expected" : "chance");
        o.chance_in = h.c >= 0;
        o.expected_in = h.e >= 0;
        std::snprintf(o.chance, sizeof(o.chance), "%.0f%%", 100*h.c);
        if (o.expected_in) liqmap2::money(h.e, o.expected, sizeof(o.expected));
        scored = scored || (ladder ? o.expected_in : o.chance_in);
    }

    // The footer: born and as of (or ended) in the display zone, dated only off the clock's day; for the other venue's
    // rail, its own price mapped onto this chart.
    char born[40], stamp[40], zone[24] = "UTC", foot[192], mapped[128];
    hover::when(r.born_ms, asof, born, sizeof(born));
    hover::when(ended ? r.end_ms : s->at_ms, asof, stamp, sizeof(stamp));
    DisplayTimeZone::instance().visible_label(asof, true, zone, sizeof(zone));
    if (std::strcmp(zone, "UTC+00") == 0) std::snprintf(zone, sizeof(zone), "UTC");
    if (!ended) std::snprintf(foot, sizeof(foot), "born %s \xc2\xb7 as of %s %s", born, stamp, zone);
    else if (r.end.empty()) std::snprintf(foot, sizeof(foot), "born %s \xc2\xb7 ended %s %s", born, stamp, zone);
    else std::snprintf(foot, sizeof(foot), "born %s \xc2\xb7 ended %s %s (%s)", born, stamp, zone, r.end.c_str());
    std::snprintf(mapped, sizeof(mapped), "%s on %s, mapped by today's basis", own, liqmap2::venue_name(src.exchange));

    const auto type = hover::begin(role == Role::lead);
    const auto& tx2 = Theme::Tokens::TX2;
    const auto& tx3 = Theme::Tokens::TX3;
    using hover::Span;
    const Span side{Theme::Fonts::ui_semibold(), type.small, r.is_long ? "LONG" : "SHORT", r.is_long ? Theme::Tokens::DOWN : Theme::Tokens::UP};
    const Span place = type.figure(ladder ? span : price), members = type.words(ladder ? count : "", tx3);
    const Span venue = type.words(liqmap2::venue_name(src.exchange), tx3);
    const float tag_w = provisional ? Theme::tag_size("PROVISIONAL").x + 8.0f : 0.0f;
    const Span hero{Theme::Fonts::heading(), type.hero, usd, Theme::Tokens::TX1}, forced = type.words("forced out", tx2);
    const Span sub = type.words(where, tx2), footer = type.quiet(foot), mapped_line = type.quiet(mapped);
    const Span unscored = type.words(short_horizons ? "Odds not scored yet" : "24h odds not scored yet", tx3);
    const Span pending = type.words("pending", tx3), expected = type.words("expected", tx2);
    // Columns: label | chance (right-aligned) ... "expected" | dollars (right-aligned at the box's edge).
    float label_w = 0, chance_w = 0, dollars_w = 0;
    for (int i = 0; i < n; ++i) {
        label_w = std::max(label_w, type.words(odds[i].label, tx2).w());
        chance_w = std::max(chance_w, odds[i].chance_in ? type.figure(odds[i].chance).w() : pending.w());
        dollars_w = std::max(dollars_w, odds[i].expected_in ? type.figure(odds[i].expected).w() : pending.w());
    }
    const float rows_w = !scored ? unscored.w()
                       : ladder ? label_w + 20.0f + dollars_w
                                : label_w + 14.0f + chance_w + 24.0f + expected.w() + 8.0f + dollars_w;
    const float head_w = side.w() + 7.0f + place.w() + (ladder ? 9.0f + members.w() : 0.0f) + 20.0f + venue.w() + tag_w;
    const float w = std::round(std::max({type.min_w, head_w, hero.w() + 7.0f + forced.w(), sub.w(), rows_w, footer.w(),
                                         src.ratio != 1 ? mapped_line.w() : 0.0f}));

    const ImVec2 head = hover::line(w, {{0, side}, {side.w() + 7.0f, place}, {side.w() + 7.0f + place.w() + 9.0f, members},
                                        {w - tag_w - venue.w(), venue}});
    if (provisional)
        Theme::draw_tag(ImGui::GetWindowDrawList(), ImVec2(head.x + w, head.y + type.body*0.5f), "PROVISIONAL",
                        Theme::Tokens::CONTROL_HOVER);
    hover::gap(9.0f);
    hover::line(w, {{0, hero}, {hero.w() + 7.0f, forced}});
    hover::gap(3.0f);
    hover::line(w, {{0, sub}});
    hover::rule(w);
    if (!scored) hover::line(w, {{0, unscored}});
    for (int i = 0; scored && i < n; ++i) {
        if (i) hover::gap(5.0f);
        const Odds& o = odds[i];
        const Span label = type.words(o.label, tx2);
        const Span dollars = o.expected_in ? type.figure(o.expected) : pending;
        if (ladder) {
            hover::line(w, {{0, label}, {w - dollars.w(), dollars}});
            continue;
        }
        const Span chance = o.chance_in ? type.figure(o.chance) : pending;
        hover::line(w, {{0, label}, {label_w + 14.0f + chance_w - chance.w(), chance},
                        {w - dollars_w - 8.0f - expected.w(), expected}, {w - dollars.w(), dollars}});
    }
    hover::rule(w);
    hover::line(w, {{0, footer}});
    if (src.ratio != 1) {
        hover::gap(3.0f);
        hover::line(w, {{0, mapped_line}});
    }
    hover::end();
}
}  // namespace liqmap2_rails_overlay
