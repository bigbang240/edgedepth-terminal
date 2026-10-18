#pragma once
// Hyperliquid positions as rails (core/hl_rails.h, built on the server; hub task liq-map-v2), drawn in the V2 rails grammar
// (rendering/liqmap2_rails_overlay.h, backend docs/LIQMAP_VISUAL_RULE.md): crisp bars at fixed prices from the snapshot
// that first showed the positions to the candle that reaches them, the snapshot that no longer has them, or the clock;
// coloured by their dollars against the typical rail in view, in eight steps (significance()); candles on top; a profile of the
// standing rails at the right edge with the largest ones' dollars beside it; a compact key with the census coverage.
// Solid bars at the V2 rail's weight (James 2026-10-15: "ACTUAL rails", not the hollow ticks); what sets them apart
// from the V2 rails is the colour, the key's name and the hover's venue and wording. When both draw, one leads
// (liqmap2_rails_overlay::Role, the V2 rails by default); a Hyperliquid follower draws its rails at the same weight but
// fainter, without profile or labels, and the leader's hover wins. A rail is positions that exist, not a prediction that
// price goes there.
#include "core/hl_rails.h"
#include "core/hl_rails_picture.h"
#include "core/ui_test_marks.h"
#include "rendering/liqmap2_rails_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace hl_rails_overlay {
inline constexpr float kHeight = liqmap2_rails_overlay::kWidth;  // a rail's bar, px, as the V2 rail's
static_assert(kHeight == hl_rails::kBarPx, "the picture reads the bar's height");
inline constexpr int kLabels = 4;           // standing rails labelled beside the profile, largest first
inline constexpr float kRowPitch = hl_rails::kRowPitch;    // profile rows
inline constexpr float kMinRunPx = hl_rails::kMinRunPx;    // a stretch narrower than this joins the next
inline constexpr float kMinLinePx = hl_rails::kMinLinePx;  // a whole line shorter than this is a speck

// Hyperliquid colours (James 2026-10-14 chose this "ice" ramp over Hyperliquid's teal straight, which shares the up
// colour's hue and read as up candles, and a desaturated mint): blue-teal for the many, rising to Hyperliquid's pale
// mint and near white for the few. Its body stays clear of the up colour; its top is the brand's mint. The lowest step
// is a clearly visible blue on the near-black canvas (James 2026-10-14: the levels were "quite hard to see").
inline ImU32 ramp(float t, float alpha = 1.0f) {
    static const float s[8][3] = {{40, 108, 164}, {36, 128, 182}, {32, 150, 198}, {42, 172, 208}, {74, 194, 214},
                                  {120, 218, 222}, {172, 240, 230}, {228, 255, 248}};
    t = std::clamp(t, 0.0f, 1.0f)*7;
    const int i = std::min(6, static_cast<int>(t));
    const float f = t - i;
    const auto c = [&](int k) { return static_cast<int>(s[i][k] + f*(s[i + 1][k] - s[i][k])); };
    return IM_COL32(c(0), c(1), c(2), static_cast<int>(255*alpha));
}

inline ImU32 color(int level, float alpha = 1.0f) {
    return ramp(level < 0 ? 0.0f : static_cast<float>(level)/(liqmap2_rails::kLevels - 1), alpha);
}

// A rail's colour step against the typical rail in view (hl_rails::significance).
using hl_rails::significance;

// The bar from x0 to x1 centred on y, the brightest step a pixel heavier so the few big ones lead (as the V2 rails).
// quiet: a rail under the typical one, a pixel lighter and half as bright, so the ladder recedes behind the standouts.
inline void bar(ImDrawList* draw, float x0, float x1, float y, int level, float alpha, bool quiet = false) {
    const float th = level == liqmap2_rails::kLevels - 1 ? kHeight + 1.0f : quiet ? kHeight - 1.0f : kHeight;
    const float top = y - std::floor(th*0.5f);
    draw->AddRectFilled(ImVec2(x0, top), ImVec2(x1, top + th), color(level, quiet ? alpha*0.5f : alpha));
}

using Role = liqmap2_rails_overlay::Role;

// The chart's closed candles, the forming one and the plot's time shift (hl_rails::Candles).
using Candles = hl_rails::Candles;
using hl_rails::clear_start;
using hl_rails::consumed_at;

// The key, drawn after the candles (draw_key) so they never cover it; render fills it. Its hover is written only while
// the pointer is over it (key_lines), from the figures kept here.
struct Key {
    bool valid = false;
    ImVec2 at;
    char basis[64] = "";
    bool warn = false, muted = false;
    bool empty = false;           // nothing to draw: the hover says why (empty_line)
    const char* empty_line = "";
    char coin[32] = "";
    double vmax = 0, typical = 0;
    size_t shown = 0, available = 0;
    bool major_only = true, replay = false, stale = false, mapped = false;
    uint8_t groups = hl_rails::kAllGroups;
    double coverage = -1, located = 0, denominator = 0;
    uint32_t usable = 0, sampled = 0;
};

// The key's hover, a line at a time: what it shows, the census's state and the honest reading.
template <class Emit>
inline void key_lines(const Key& k, Emit&& emit) {
    if (k.empty) {
        emit("Hyperliquid positions by liquidation price, read from our Hyperliquid node.");
        emit(k.empty_line);
        return;
    }
    char max_label[32] = "-", typical_label[32] = "-", line[320], located_line[192];
    if (k.vmax > 0) liqmap2::money(k.vmax, max_label, sizeof(max_label));
    std::snprintf(line, sizeof(line), "Hyperliquid positions by liquidation price (HL %s). Largest in view %s.", k.coin,
                  max_label);
    emit(line);
    if (k.typical > 0) {
        liqmap2::money(k.typical, typical_label, sizeof(typical_label));
        std::snprintf(line, sizeof(line), "Colour: dollars against the typical rail in view (%s); dim below it, brightest "
                      "at 8x or more.", typical_label);
        emit(line);
    }
    emit("Only the significant levels: a level becomes a rail when it is among the few that hold 80% of its side's "
         "dollars.");
    std::snprintf(line, sizeof(line), "%s%zu shown, %zu hidden in this view.", k.major_only ? "Major levels: " : "More detail: ",
                  k.shown, k.available - k.shown);
    emit(line);
    if (k.major_only) emit("Largest fifth per side, up to six each, with nearby rails separated. Size ties prefer longer observed life. A display filter, not a prediction.");
    char located[32], denominator[32];
    liqmap2::money(k.located, located, sizeof(located));
    liqmap2::money(k.denominator, denominator, sizeof(denominator));
    if (k.replay) std::snprintf(located_line, sizeof(located_line), "Replay: the rails as the server recorded them at the time.");
    else if (k.coverage >= 0)
        std::snprintf(located_line, sizeof(located_line), "Located %s of %s open interest (%.1f%%), %u of %u sampled positions.",
                      located, denominator, 100*k.coverage, k.usable, k.sampled);
    else std::snprintf(located_line, sizeof(located_line), "Located %s; this snapshot gives no coverage denominator.", located);
    emit(located_line);
    if (k.groups != hl_rails::kAllGroups) {
        int n = std::snprintf(line, sizeof(line), "Leverage shown:");
        for (int g = 0; g < hl_rails::kGroups; ++g)
            if (k.groups & (1u << g)) n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " %s", hl_rails::kGroupChips[g]);
        std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), ".");
        emit(line);
    }
    if (k.stale) emit("No census snapshot for a while: rails stop at the last one.");
    if (k.mapped) emit("Hyperliquid prices mapped onto this chart by the recent basis.");
    emit("Positions that exist now, read from our Hyperliquid node: not a forecast that price goes there.");
    emit("A rail ends at the candle that reaches it, or when its positions close.");
}

// Called inside the price plot, after the candles.
inline void draw_key(const Key& k) {
    if (!k.valid) return;
    ImPlot::PushPlotClipRect();
    if (liqmap2_rails_overlay::source_chip(ImPlot::GetPlotDrawList(), k.at, "HL", k.basis, k.warn)) {
        Theme::begin_tooltip();
        key_lines(k, [](const char* text) { ImGui::TextUnformatted(text); });
        Theme::end_tooltip();
    }
    ImPlot::PopPlotClipRect();
}

// The plot's projection, as the picture reads it (hl_rails::build): real time t at t - shift.
struct Projection {
    double shift = 0;
    float x(double t) const { return ImPlot::PlotToPixels(t - shift, 0).x; }
    float y(double price) const { return ImPlot::PlotToPixels(0, price).y; }
    double price_at(float y_px) const { return ImPlot::PixelsToPlot(ImVec2(0, y_px)).y; }
};

// What the overlay draws for one chart (hl_rails::Picture), worked out once and kept while the book, candles (the
// forming one included), view, plot, basis, role, profile, leverage, major levels, labels, mode and newest message are
// as they were and the clock has not reached anything that changes it (hl_rails::build's until). A frame then draws
// the kept rectangles and does the hover. One per chart (ChartWidget::hl_rails_cache_), beside its hl_rails::View.
// Rule: a new input to the picture joins Key or until.
struct Cache {
    struct Key {
        const hl_rails::Book* book = nullptr;
        uint64_t book_revision = 0;
        uint64_t candles_revision = 0;
        double shift = 0, x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        float px = 0, py = 0, pw = 0, ph = 0;
        double ratio = 1;
        Role role = Role::alone;
        bool profile = false, major_only = true, value_labels = false, replay = false;
        uint8_t groups = hl_rails::kAllGroups;
        int64_t latest_ms = 0;
        bool operator==(const Key&) const = default;
    };
    bool keyed = false;
    Key key;
    hl_rails::Kept kept;
};

// Called inside the price plot, before the candles; key gets the layer's key for draw_key. cache: the chart's own.
// book: the rails (hl_rails::View over the coin's store, with the viewer's filter); latest: the newest live message's
// census (its coverage and age; null in a replay, which draws the record as it was); candles_revision: changes with the
// candles, the forming one included (liqmap2_rails::ChartCandles::revision); ratio: the chart venue's price over
// Hyperliquid's (1 on a Hyperliquid chart), today's basis applied to every rail; top_y: the key's offset below the plot
// top; profile: the standing rails' profile at the right edge (the chart reserves room for it); groups: the leverage
// groups chosen (hl_rails::Filter).
inline void render(Cache& cache, const hl_rails::Book& book, const hl_rails::Latest* latest, const Candles& candles,
                   uint64_t candles_revision, int64_t asof, double ratio, const char* coin, const char* price_fmt,
                   float top_y, Role role, bool profile, uint8_t groups, bool replay, Key& key, bool major_only = true,
                   bool value_labels = false) {
    using liqmap2_rails::Rail;
    const bool follow = role == Role::follow;
    profile = profile && !follow;
    const auto pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    const auto limits = ImPlot::GetPlotLimits();
    const double shift = candles.shift_ms;
    const double t_lo = limits.X.Min + shift, t_hi = limits.X.Max + shift;  // the view in real time
    const auto x_of = [&](double t) { return ImPlot::PlotToPixels(t - shift, 0).x; };
    const auto y_of = [](double price) { return std::round(ImPlot::PlotToPixels(0, price).y); };
    auto* draw = ImPlot::GetPlotDrawList();
    const auto mouse = ImGui::GetIO().MousePos;
    const bool hovered = ImPlot::IsPlotHovered();
    key.valid = true;
    key.at = ImVec2(pos.x + 10, pos.y + top_y);
    key.muted = follow;
    key.warn = false;
    key.empty = false;
    if (replay) latest = nullptr;  // a replay draws the record; the live census says nothing about then
    const auto state = hl_rails::state(latest, asof);
    ImPlot::PushPlotClipRect();
    if ((!latest && !replay) || book.rails.empty()) {
        const char* why = replay ? "no rails recorded here" : !latest ? "waiting for the rails"
                        : state == hl_rails::State::unavailable ? "no positions located"
                        : state == hl_rails::State::legacy ? "older census only" : "no rails yet";
        std::snprintf(key.basis, sizeof(key.basis), "%s", why);
        key.warn = true;
        key.empty = true;
        key.empty_line = replay ? "No rails were recorded for this window." : !latest ? "No rails message yet."
                                : "No rails with the leverage and Minimum $ chosen.";
        ImPlot::PopPlotClipRect();
        return;
    }
    // The picture at the clock: kept across frames while its inputs hold (Cache).
    Cache::Key k;
    k.book = &book;
    k.book_revision = book.revision;
    k.candles_revision = candles_revision;
    k.shift = shift;
    k.x0 = limits.X.Min; k.x1 = limits.X.Max; k.y0 = limits.Y.Min; k.y1 = limits.Y.Max;
    k.px = pos.x; k.py = pos.y; k.pw = size.x; k.ph = size.y;
    k.ratio = ratio;
    k.role = role;
    k.profile = profile; k.major_only = major_only; k.value_labels = value_labels; k.replay = replay;
    k.groups = groups;
    k.latest_ms = latest ? latest->timestamp_ms : 0;
    const bool same = cache.keyed && cache.key == k;
    cache.keyed = true;
    cache.key = k;
    const hl_rails::Plot view{t_lo, t_hi, limits.Y.Min, limits.Y.Max, pos.y, size.y};
    const auto& pic = hl_rails::keep(cache.kept, same, book, latest, candles, asof, ratio, view, Projection{shift}, profile,
                                     major_only);
    bool stale = false;
    const int64_t clock = hl_rails::picture_clock(latest, asof, stale);
    const int64_t now_edge = hl_rails::now_edge(candles, clock, stale);
    const double typical = pic.typical;

    // The profile's column at the right edge, only in the empty space after now (as the V2 rails'), leaving the
    // dollar labels room between it and the present column.
    const float prof_x1 = pos.x + size.x - 3.0f;
    float prof_w = profile ? std::clamp(size.x*0.09f, 56.0f, 120.0f) : 0.0f;
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const float label_room = ImGui::CalcTextSize("$00.00M").x + 32.0f;
    ImGui::PopFont();
    prof_w = std::min(prof_w, prof_x1 - (x_of(static_cast<double>(now_edge)) + 10.0f + label_room));
    if (prof_w < 32.0f) {
        prof_w = 0.0f;
        profile = false;
    }
    const float prof_x0 = prof_x1 - prof_w;

    // The rails, brightest last. A follower draws fainter beneath the leader.
    const hl_rails::Line* hit = nullptr;
    float hit_d = 1e9f;
    const bool marks = ui_test::enabled();
    for (const auto& l : pic.lines) {
        const float x0 = std::round(x_of(std::max(double(l.t0), t_lo)));
        const float x1 = std::round(x_of(std::min(double(l.t1), t_hi)));
        if (x1 - x0 < 3.0f) continue;
        const float y = y_of(l.price);
        if (l.step->zone_lo > 0 && l.step->zone_hi > l.step->zone_lo) {
            const float ya = y_of(l.step->zone_hi*ratio), yb = y_of(l.step->zone_lo*ratio);
            draw->AddRectFilled(ImVec2(x0, ya), ImVec2(x1, yb), color(l.level, follow ? 0.04f : 0.08f));
        }
        bar(draw, x0, x1, y, l.level, follow ? 0.55f : 1.0f, l.quiet);
        if (marks) ui_test::mark_rect("hl.level." + std::to_string(l.rail->id), ImVec2(x0, y - 3), ImVec2(x1, y + 3));
        const float d = std::abs(mouse.y - y);
        if (hovered && mouse.x >= x0 && mouse.x <= x1 && d <= kHeight*0.5f + 3.0f && d < hit_d) {
            hit_d = d;
            hit = &l;
        }
    }

    // The profile (worked out with the picture): every chosen rail standing at the clock and not consumed by the
    // chart's candles, its dollars in the 5 px row at its price.
    const auto& rows = pic.rows;
    const double row_max = pic.row_max;
    int prof_hit = -1;
    if (profile && prof_w > 0) {
        for (size_t k2 = 0; row_max > 0 && k2 < rows.size(); ++k2) {
            if (!(rows[k2].v > 0)) continue;
            const float len = std::max(2.0f, std::round(prof_w*static_cast<float>(rows[k2].v/row_max)));
            const float y0 = pos.y + k2*kRowPitch;
            draw->AddRectFilled(ImVec2(prof_x1 - len, y0), ImVec2(prof_x1, y0 + kRowPitch - 1.0f),
                                color(liqmap2_rails::level(rows[k2].v, row_max), 0.92f));
            if (hovered && mouse.x >= prof_x0 && mouse.x <= prof_x1 && mouse.y >= y0 && mouse.y < y0 + kRowPitch)
                prof_hit = static_cast<int>(k2);
        }
    }

    // Dollar labels for the largest standing rails, beside the profile, never overlapping, never over the candles.
    if (!follow && value_labels) {
        float used[kLabels];
        int n_used = 0;
        ImGui::PushFont(Theme::Fonts::mono_sm());
        for (uint32_t i : pic.labels) {
            const auto* l = &pic.lines[i];
            if (n_used >= kLabels) break;
            const float y = y_of(l->price);
            if (std::any_of(used, used + n_used, [&](float u) { return std::abs(u - y) < ImGui::GetTextLineHeight() + 4.0f; })) continue;
            char amount[32];
            liqmap2::money(l->v, amount, sizeof(amount));
            const ImVec2 ts = ImGui::CalcTextSize(amount);
            const float right = profile ? prof_x0 - 6.0f : pos.x + size.x - 6.0f;
            const ImVec2 p0(right - ts.x - 14.0f, y - ts.y*0.5f - 2.0f), p1(right, y + ts.y*0.5f + 2.0f);
            if (p0.x < x_of(static_cast<double>(now_edge)) + 6.0f) break;
            draw->AddRectFilled(p0, p1, Theme::u32(Theme::Tokens::PANEL, 0.88f), Theme::Radius::R1);
            // The layer's mark: a small hollow box in the rail's colour (the V2 labels carry a solid bar).
            draw->AddRect(ImVec2(p0.x + 1.0f, p0.y + 3.0f), ImVec2(p0.x + 6.0f, p1.y - 3.0f), color(l->level), 0.0f, 0, 1.5f);
            draw->AddText(ImVec2(p0.x + 10.0f, p0.y + 2.0f), Theme::u32(Theme::Tokens::TX1), amount);
            used[n_used++] = y;
        }
        ImGui::PopFont();
    }

    // The key: coverage of the census beside the scale, its state; what a rail means in its hover (key_lines).
    const hl_rails::Latest none;
    const auto& q = latest ? *latest : none;
    const double coverage = q.coverage_denominator_usd > 0 ? std::min(1.0, q.usable_notional_usd/q.coverage_denominator_usd) : -1;
    const double vmax = pic.vmax;
    char* basis = key.basis;
    const size_t bn = sizeof(key.basis);
    if (replay) std::snprintf(basis, bn, vmax > 0 ? "recorded" : "none in view \xc2\xb7 recorded");
    else if (stale) std::snprintf(basis, bn, "census %.0fs old", (asof - latest->timestamp_ms)/1000.0);
    else if (!(vmax > 0) && coverage >= 0) std::snprintf(basis, bn, "none in view \xc2\xb7 cov %.0f%%", 100*coverage);
    else if (coverage >= 0) std::snprintf(basis, bn, "cov %.0f%%", 100*coverage);
    else std::snprintf(basis, bn, "coverage unknown");
    if (!stale && (replay || (state == hl_rails::State::ready && coverage >= 0)))
        std::snprintf(basis, bn, "%zu/%zu levels%s", pic.selected, pic.available, replay ? " recorded" : "");
    key.warn = stale;
    std::snprintf(key.coin, sizeof(key.coin), "%s", coin);
    key.vmax = vmax;
    key.typical = typical;
    key.shown = pic.selected;
    key.available = pic.available;
    key.major_only = major_only;
    key.replay = replay;
    key.stale = stale;
    key.mapped = ratio != 1;
    key.groups = groups;
    key.coverage = coverage;
    key.located = q.usable_notional_usd;
    key.denominator = q.coverage_denominator_usd;
    key.usable = q.usable_positions;
    key.sampled = q.sampled_positions;
    ImPlot::PopPlotClipRect();

    if (prof_hit >= 0 && !hit) {
        const float y0 = pos.y + prof_hit*kRowPitch;
        char a[48], b[48], v[32], span[104], count[48];
        std::snprintf(a, sizeof(a), price_fmt, ImPlot::PixelsToPlot(ImVec2(0, y0 + kRowPitch)).y);
        std::snprintf(b, sizeof(b), price_fmt, ImPlot::PixelsToPlot(ImVec2(0, y0)).y);
        std::snprintf(span, sizeof(span), "%s to %s", a, b);
        std::snprintf(count, sizeof(count), "%d standing rail%s", rows[prof_hit].rails, rows[prof_hit].rails == 1 ? "" : "s");
        liqmap2::money(rows[prof_hit].v, v, sizeof(v));
        const auto type = liqmap2_rails_overlay::hover::begin(role == Role::lead);
        using liqmap2_rails_overlay::hover::Span;
        const Span where = type.figure(span), standing = type.words(count, Theme::Tokens::TX3);
        const Span usd{Theme::Fonts::heading(), type.hero, v, Theme::Tokens::TX1};
        const Span what = type.words("in Hyperliquid positions", Theme::Tokens::TX2);
        const float w = std::round(std::max({type.min_w, where.w() + 20.0f + standing.w(), usd.w() + 7.0f + what.w()}));
        liqmap2_rails_overlay::hover::line(w, {{0, where}, {w - standing.w(), standing}});
        liqmap2_rails_overlay::hover::gap(9.0f);
        liqmap2_rails_overlay::hover::line(w, {{0, usd}, {usd.w() + 7.0f, what}});
        liqmap2_rails_overlay::hover::end();
    }
    if (!hit) return;
    // The rail at the hovered time, never past the clock: side and price (a span of levels: its range and count) with
    // the venue; the dollars and where they liquidate; their leverage; first seen and as of (or ended); on another
    // venue's chart, the Hyperliquid price.
    const Rail& r = *hit->rail;
    const int64_t at = std::min(clock, static_cast<int64_t>(ImPlot::PixelsToPlot(mouse).x + shift));
    const auto* s = liqmap2_rails::step_at(r, at);
    const size_t si = static_cast<size_t>(s - r.steps.data());
    const auto dit = book.detail.find(r.id);
    const hl_rails::Detail d = dit != book.detail.end() && si < dit->second.size() ? dit->second[si] : hl_rails::Detail{};
    const bool ended = r.end_ms > 0 && r.end_ms <= clock;
    char price[48], own[48], lo[48], hi[48], range[104], usd[32], where[160];
    std::snprintf(price, sizeof(price), price_fmt, hit->price);
    std::snprintf(own, sizeof(own), price_fmt, r.price);
    std::snprintf(lo, sizeof(lo), price_fmt, s->zone_lo*ratio);
    std::snprintf(hi, sizeof(hi), price_fmt, s->zone_hi*ratio);
    std::snprintf(range, sizeof(range), "%s to %s", lo, hi);
    liqmap2::money(s->usd, usd, sizeof(usd));
    std::snprintf(where, sizeof(where), "liquidate if Hyperliquid's mark reaches %s", range);

    char born[40], stamp[40], zone[24] = "UTC", foot[192], mapped[128];
    liqmap2_rails_overlay::hover::when(r.born_ms, clock, born, sizeof(born));
    liqmap2_rails_overlay::hover::when(ended ? r.end_ms : latest ? std::min(clock, latest->timestamp_ms) : clock, clock, stamp,
                                       sizeof(stamp));
    DisplayTimeZone::instance().visible_label(clock, true, zone, sizeof(zone));
    if (std::strcmp(zone, "UTC+00") == 0) std::snprintf(zone, sizeof(zone), "UTC");
    const char* why = r.end == "closed" ? "positions closed" : r.end == "gap" ? "census gap"
                    : r.end == "touched" ? "price reached it" : r.end == "filter" ? "under your filter" : "";
    if (!ended) std::snprintf(foot, sizeof(foot), "seen from %s \xc2\xb7 as of %s %s", born, stamp, zone);
    else if (*why) std::snprintf(foot, sizeof(foot), "seen from %s \xc2\xb7 ended %s %s (%s)", born, stamp, zone, why);
    else std::snprintf(foot, sizeof(foot), "seen from %s \xc2\xb7 ended %s %s", born, stamp, zone);
    std::snprintf(mapped, sizeof(mapped), "%s on Hyperliquid, mapped by the recent basis", own);

    const auto type = liqmap2_rails_overlay::hover::begin(role == Role::lead);
    using liqmap2_rails_overlay::hover::Span;
    const auto& tx2 = Theme::Tokens::TX2;
    const auto& tx3 = Theme::Tokens::TX3;
    const Span side{Theme::Fonts::ui_semibold(), type.small, r.is_long ? "LONG" : "SHORT", r.is_long ? Theme::Tokens::DOWN : Theme::Tokens::UP};
    const Span place = type.figure(price);
    const Span venue = type.words("Hyperliquid", tx3);
    const Span hero{Theme::Fonts::heading(), type.hero, usd, Theme::Tokens::TX1}, held = type.words("in positions", tx2);
    const Span sub = type.words(where, tx2), footer = type.quiet(foot), mapped_line = type.quiet(mapped);
    // Leverage rows: each chosen group's dollars, right-aligned.
    char amounts[hl_rails::kGroups][32];
    float label_w = 0, amount_w = 0;
    int shown = 0;
    for (int g = 0; g < hl_rails::kGroups; ++g) {
        if (!(groups & (1u << g)) || !(d.mix[g] > 0)) continue;
        liqmap2::money(d.mix[g], amounts[g], sizeof(amounts[g]));
        label_w = std::max(label_w, type.words(hl_rails::kGroupLabels[g], tx2).w());
        amount_w = std::max(amount_w, type.figure(amounts[g]).w());
        ++shown;
    }
    const Span unknown = type.words("Leverage not reported", tx3);
    const float rows_w = shown ? label_w + 24.0f + amount_w : unknown.w();
    const float head_w = side.w() + 7.0f + place.w() + 20.0f + venue.w();
    const float w = std::round(std::max({type.min_w, head_w, hero.w() + 7.0f + held.w(), sub.w(), rows_w, footer.w(),
                                         ratio != 1 ? mapped_line.w() : 0.0f}));
    liqmap2_rails_overlay::hover::line(w, {{0, side}, {side.w() + 7.0f, place}, {w - venue.w(), venue}});
    liqmap2_rails_overlay::hover::gap(9.0f);
    liqmap2_rails_overlay::hover::line(w, {{0, hero}, {hero.w() + 7.0f, held}});
    liqmap2_rails_overlay::hover::gap(3.0f);
    liqmap2_rails_overlay::hover::line(w, {{0, sub}});
    liqmap2_rails_overlay::hover::rule(w);
    if (!shown) liqmap2_rails_overlay::hover::line(w, {{0, unknown}});
    for (int g = 0, n = 0; g < hl_rails::kGroups; ++g) {
        if (!(groups & (1u << g)) || !(d.mix[g] > 0)) continue;
        if (n++) liqmap2_rails_overlay::hover::gap(5.0f);
        const Span label = type.words(hl_rails::kGroupLabels[g], tx2), amount = type.figure(amounts[g]);
        liqmap2_rails_overlay::hover::line(w, {{0, label}, {w - amount.w(), amount}});
    }
    liqmap2_rails_overlay::hover::rule(w);
    liqmap2_rails_overlay::hover::line(w, {{0, footer}});
    if (ratio != 1) {
        liqmap2_rails_overlay::hover::gap(3.0f);
        liqmap2_rails_overlay::hover::line(w, {{0, mapped_line}});
    }
    liqmap2_rails_overlay::hover::end();
}
}  // namespace hl_rails_overlay
