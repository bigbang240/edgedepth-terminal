// Refill levels in Real-time: the subscription, the rails on the chart, the ladder marks, the Layers row and the
// status chip. The model, the access rule and the wire contract are in core/refill_levels.h.
#include "ui/chart_widget.h"
#include "core/i18n.h"
#include "core/education_boot.h"
#include "core/entitlements.h"
#include "core/ui_test_marks.h"
#include "replayer/replay_manager.h"
#include "rendering/realtime_bubble.h"
#include "rendering/theme.h"
#include "types/frame_profiler.h"
#include "stream_handler.h"
#include "ui/upsell_modal.h"
#include <algorithm>
#include <cmath>
#include <emscripten.h>

// The host seeds and refreshes this; re-read on every subscribe and history read, as the other gated streams do.
const char* ChartWidget::refill_token() {
    const char* token = emscripten_run_script_string("window.__EDGEDEPTH_REPLAY_TOKEN__ || ''");
    return token ? token : "";
}

// Local packs carry no replay server, so nothing can be subscribed there.
bool ChartWidget::refill_pack() const {
    return ctx_.replay_mgr().is_pack_mode() || EducationBoot::instance().is_pack();
}

refill::Access ChartWidget::refill_access() const {
    return refill::access(pair_.exchange, pair_.symbol, Entitlements::is_pro(), ctx_.replay_mgr().is_active(),
                          Entitlements::hosted());
}

// The server refused this viewer (pro_required): the locked row, never an error on the chart.
bool ChartWidget::refill_server_locked() const {
    const refill::Market* m = refill::store().find(pair_.exchange, pair_.symbol);
    return m && m->status == refill::Status::unavailable && m->reason == "pro_required";
}

bool ChartWidget::refill_active() const {
    return rt_mode_ && rt_refill_enabled_ && !refill_pack() && refill::granted(refill_access()) &&
        !refill_server_locked();
}

void ChartWidget::open_refill_upsell(const char* cause) {
    auto& modal = ui::UpsellModal::instance();
    modal.open(ui::UpsellModal::Trigger::Layer, nullptr, "refill", cause);
    modal.set_subject(Entitlements::coin_label(pair_.symbol).c_str());
}

void ChartWidget::update_refill_levels() {
    StreamManager& sm = ctx_.stream_mgr();
    const bool replay = ctx_.replay_mgr().is_active();
    // A replay subscribes only once the chart is on the replay context (after replay_joined), never on the live
    // socket while the session is still being created.
    const bool want = is_open && rt_mode_ && rt_refill_enabled_ && tick_size_ > 0 && !refill_pack() &&
        replay == sm.is_replay_mode() && refill::granted(refill_access());
    if (refill_stream_mgr_ && (!want || refill_stream_mgr_ != &sm)) release_refill_levels();
    if (!want) return;
    refill_stream_mgr_ = &sm;
    refill::Subscriptions::Want w;
    w.channel = &sm;
    w.exchange = pair_.exchange;
    w.symbol = pair_.symbol;
    w.tick = tick_size_;
    w.replay = replay;
    w.generation = sm.connection_generation();
    w.connected = sm.connected();
    w.now_ms = emscripten_get_now();
    w.token = refill_token;
    refill_outbox_.clear();
    refill::subscriptions().want(this, w, refill_outbox_);
    for (const auto& m : refill_outbox_)
        if (m.channel == &sm) sm.send_message(m.text);
}

// Leaving Real-time, the layer off, a context swap or the chart closing. The manager the subscription went
// through is still alive at every caller (context swaps release before the old context is retired).
void ChartWidget::release_refill_levels() {
    if (!refill_stream_mgr_) return;
    refill_outbox_.clear();
    refill::subscriptions().release(this, refill_outbox_);
    for (const auto& m : refill_outbox_)
        if (m.channel == refill_stream_mgr_) refill_stream_mgr_->send_message(m.text);
    refill_stream_mgr_ = nullptr;
}

// One absorption bubble per refill level (James 2026-10-14, option A of three mocked): a dark disc behind the trades,
// centred on the level's latest hit, its edge in the colour of the side doing the absorbing (a bid level takes the
// sells: UP; an ask the buys: DOWN). Its size is everything printed into the price since the episode started, on the
// trade bubbles' own curve with room to 40 px, so it keeps swelling while the level holds and hops right with each
// new hit. An ended level keeps its final size, faded. Replaces the 2026-10-13 ring on every absorbed print. Drawn
// before the trade bubbles, so the prints sit inside it.
void ChartWidget::render_refill_lenses(ImDrawList* dl, const ImPlotRect& limits, double minimum) {
    ProfileScope profile("RT refill lenses");
    refill_lens_count_ = 0;
    if (!refill_active() || !rt_archive_) return;
    const refill::Market* m = refill::store().find(pair_.exchange, pair_.symbol);
    if (!m || m->levels.empty()) return;
    const double tick = rt_dom_frame_.native_tick > 0 ? rt_dom_frame_.native_tick : tick_size_;
    // Before automatic sizing settles there is no bubble scale yet; the manual minimum stands in.
    const double reference = minimum > 0 ? minimum : double(rt_min_notional_);
    if (!(tick > 0) || !(reference > 0)) return;
    const int64_t clock = rt_clock_ms_;
    const refill::View view{limits.X.Min, limits.X.Max, limits.Y.Min, limits.Y.Max};
    const size_t n = refill::select(*m, clock, view, refill_order_);
    if (!n) return;
    const RealtimeBubbleHistory& history = rt_archive_->bubbles();
    const int64_t stable = history.stable_through(clock);
    const ImVec2 pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    int marks = 0;
    // Holding levels come first in the order; draw from the back so they end up on top.
    for (size_t k = n; k-- > 0;) {
        const uint16_t index = refill_order_[k];
        const refill::Level& l = m->levels[index];
        refill::Span s;
        if (!refill::span_at(l, *m, clock, s)) continue;
        const bool bid = l.side == refill::Side::bid;
        // A holding level keeps counting up to the clock, between the server's updates.
        const auto a = refill_absorption_.read(history, &*rt_archive_, rt_archive_->generation, stable, l.id, bid,
                                               l.price, tick, s.start, s.open ? clock : s.end, clock);
        // The server's total also covers prints from before this tab was capturing; it counts once the clock has
        // reached the update that carries it.
        const double value = l.updated_ms <= clock ? std::max(a.value, l.executed_usd) : a.value;
        // At the latest hit; else the latest refill the clock has reached; else where it qualified.
        int64_t at = a.last_ms;
        for (uint8_t r = 0; !at && r < l.refill_count; ++r)
            if (l.refill_ms[r] >= s.start && l.refill_ms[r] <= std::min(s.end, clock)) at = std::max(at, l.refill_ms[r]);
        if (!at) at = s.known;
        const ImVec2 c = ImPlot::PlotToPixels(double(at), l.price);
        const float print_r = RealtimeBubble::radius(a.last_value, reference, rt_bubble_cap_);
        const float r = std::max({RealtimeBubble::radius(value, reference, bubble_size::kAbsorptionCap),
                                  print_r + 5.0f, 9.0f});
        if (c.x + r < pos.x || c.x - r > pos.x + size.x || c.y + r < pos.y || c.y - r > pos.y + size.y) continue;
        const float alpha = s.open ? 1.0f : s.end_status == refill::End::pulled ? 0.35f : 0.45f;
        dl->AddCircleFilled(c, r, Theme::u32(Theme::Tokens::BASE, 0.62f * alpha), 0);
        dl->AddCircle(c, r + 1.0f, Theme::u32(Theme::Tokens::BASE, 0.6f * alpha), 0, 1.0f);
        dl->AddCircle(c, r, Theme::u32(bid ? Theme::Tokens::UP : Theme::Tokens::DOWN, alpha), 0, 1.5f);
        if (refill_lens_count_ < int(refill_lenses_.size()))
            refill_lenses_[size_t(refill_lens_count_++)] = {index, c, r};
        if (ui_test::enabled() && marks < 48)
            ui_test::mark_rect("rt.refill.lens." + std::to_string(marks++), ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r));
    }
}

// Refill levels in Real-time, over the trades: a small dot at each refill and a tick at the plot's right edge while
// it holds (no rail: James 2026-10-13, a rail sat on the bid/ask steps and the bubble chain and read as one more line
// in the tape). Neutral near-white with a dark halo; the absorption bubble behind the trades carries the side colour.
// Above bubbles and wall marks, below the live quotes. Called inside the plot clip rect.
void ChartWidget::render_refill_levels(ImDrawList* dl, const ImPlotRect& limits) {
    ProfileScope profile("RT refill marks");
    rt_dom_frame_.refill = nullptr;
    refill_ladder_.count = 0;
    if (!refill_active()) return;
    const refill::Market* m = refill::store().find(pair_.exchange, pair_.symbol);
    if (!m || m->levels.empty()) return;
    // The chart clock (frozen while the display is paused, the replay clock in replay): a level the clock has
    // not reached is not drawn, and one closed later still holds at the clock.
    const int64_t clock = rt_clock_ms_;
    const refill::View view{limits.X.Min, limits.X.Max, limits.Y.Min, limits.Y.Max};
    const size_t n = refill::select(*m, clock, view, refill_order_);
    if (!n) return;

    const ImVec4& ink = Theme::Tokens::TX1;
    int marks = 0;   // browser-test marks published this frame (core/ui_test_marks.h), a few of each
    const ImU32 halo = Theme::u32(Theme::Tokens::BASE, 0.9f);
    const ImVec2 pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    const float right = pos.x + size.x;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool hover_ok = !ImGui::GetIO().KeyAlt && realtime_hover_ready();
    int hovered = -1;
    float hovered_d2 = 1e9f;
    const auto consider = [&](int index, ImVec2 at, float reach) {
        if (!hover_ok) return;
        const float dx = mouse.x - at.x, dy = mouse.y - at.y, d2 = dx * dx + dy * dy;
        if (d2 <= reach * reach && d2 < hovered_d2) { hovered = index; hovered_d2 = d2; }
    };
    // The absorption bubbles drawn behind the trades this frame (render_refill_lenses).
    for (int i = 0; i < refill_lens_count_; ++i) {
        const RefillLens& lens = refill_lenses_[size_t(i)];
        consider(lens.index, lens.center, lens.r + 2.0f);
    }
    // Holding levels come first in the order; draw from the back so they end up on top.
    for (size_t k = n; k-- > 0;) {
        const uint16_t index = refill_order_[k];
        const refill::Level& l = m->levels[index];
        refill::Span s;
        if (!refill::span_at(l, *m, clock, s)) continue;
        const float fade = s.end_status == refill::End::pulled ? 0.5f : 1.0f;
        // Each refill: a small dot on the price, so the rhythm shows even when the prints were too small to
        // draw as bubbles.
        for (uint8_t r = 0; r < l.refill_count; ++r) {
            const int64_t t = l.refill_ms[r];
            if (t < s.start || t > s.end || double(t) < limits.X.Min || double(t) > limits.X.Max) continue;
            const ImVec2 c(std::round(ImPlot::PlotToPixels(double(t), l.price).x) + 0.5f,
                           std::round(ImPlot::PlotToPixels(double(t), l.price).y) + 0.5f);
            const float alpha = (t < s.known ? 0.4f : 1.0f) * fade;
            dl->AddCircleFilled(c, 3.75f, Theme::u32(Theme::Tokens::BASE, 0.85f * alpha), 12);
            dl->AddCircleFilled(c, 2.5f, Theme::u32(ink, alpha), 12);
            consider(index, c, 6.0f);
            if (ui_test::enabled() && marks < 48) ui_test::mark_rect("rt.refill.dot." + std::to_string(marks++),
                                                                    ImVec2(c.x - 2.5f, c.y - 2.5f), ImVec2(c.x + 2.5f, c.y + 2.5f));
        }
        if (!s.open) continue;
        // Holding now: a tick at the right edge on its price (the ladder also marks the row).
        if (limits.X.Max >= double(clock)) {
            const float y = std::round(ImPlot::PlotToPixels(0, l.price).y);
            dl->AddRectFilled(ImVec2(right - 11.0f, y - 2.5f), ImVec2(right, y + 2.5f), halo);
            dl->AddRectFilled(ImVec2(right - 10.0f, y - 1.5f), ImVec2(right, y + 1.5f), Theme::u32(ink));
            consider(index, ImVec2(right - 5.0f, y), 6.0f);
            if (ui_test::enabled()) ui_test::mark_rect("rt.refill.edge." + std::to_string(k), ImVec2(right - 10.0f, y - 1.5f), ImVec2(right, y + 1.5f));
        }
        if (refill_ladder_.count < int(refill_ladder_.marks.size()))
            refill_ladder_.marks[size_t(refill_ladder_.count++)] = {l.price, float(l.shown_ratio), l.side == refill::Side::bid};
    }
    if (refill_ladder_.count) rt_dom_frame_.refill = &refill_ladder_;
    if (hovered < 0) return;
    const refill::Level& l = m->levels[size_t(hovered)];
    char price[32], title[96], values[96];
    fmt_.format_price(price, sizeof(price), l.price);
    refill::hover_title(l, price, title, sizeof(title));
    refill::hover_values(l, values, sizeof(values));
    refill::Span s;
    refill::span_at(l, *m, clock, s);
    Theme::begin_tooltip();
    ImGui::TextUnformatted(title);
    ImGui::TextUnformatted(values);
    if (!s.open && l.status != refill::End::holding && s.end_status == l.status)
        ImGui::TextDisabled("Ended: %s", refill::end_word(l.status));
    Theme::end_tooltip();
}

// A short muted chip under the depth note, only when the server says it cannot serve this market or is still
// warming up. Nothing while observing, nothing for pro_required (that is the locked row).
void ChartWidget::render_refill_status(ImDrawList* dl) {
    if (!rt_mode_ || !rt_refill_enabled_ || refill_pack() || !refill::granted(refill_access())) return;
    const refill::Market* m = refill::store().find(pair_.exchange, pair_.symbol);
    if (!m) return;
    const char* note = m->status == refill::Status::unavailable ? refill::unavailable_note(m->reason)
                     : m->status == refill::Status::warming ? "Waiting for refills" : nullptr;
    if (!note) return;
    const ImVec2 pos = ImPlot::GetPlotPos();
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const float px = ImGui::GetFontSize();
    ImGui::PopFont();
    Theme::push_font(Theme::Fonts::ui(), px);
    const ImVec2 size = ImGui::CalcTextSize(note);
    const ImVec2 a(pos.x + 10, pos.y + (ctx_.replay_mgr().is_active() ? 86.0f : 38.0f));
    const ImVec2 b(std::round(a.x + 9 + 6 + 7 + size.x + 9), a.y + 22);
    dl->AddRectFilled(a, b, Theme::u32(Theme::Tokens::PANEL, 0.92f), Theme::Radius::R2);
    dl->AddRect(a, b, Theme::u32(Theme::Tokens::BD2), Theme::Radius::R2, 0, 1.0f);
    dl->AddCircleFilled(ImVec2(a.x + 12, a.y + 11), 3.0f, Theme::u32(Theme::Tokens::TX2), 12);
    dl->AddText(ImVec2(a.x + 22, std::round(a.y + 11 - size.y * 0.5f)), Theme::u32(Theme::Tokens::TX2), note);
    ImGui::PopFont();
    Theme::chart_chip(a, b);
}

// Layers > REAL-TIME VIEW row. Free on BTC, Pro on every other Binance futures market, any market in a replay.
void ChartWidget::render_refill_layer_row() {
    const refill::Access access = refill_access();
    const bool pack = refill_pack();
    if (pack || access == refill::Access::unsupported) {
        Theme::menu_layer(i18n::tr("Refill levels", i18n::fa_l_refill), false, false, pack ? "not in local replay" : "Binance futures only");
        if (ImGui::IsItemHovered())
            Theme::tooltip(pack ? "Refill levels come from the replay server, not local replays."
                                : "Refill levels are on Binance futures only for now.");
        return;
    }
    if (access == refill::Access::locked || refill_server_locked()) {
        if (Theme::menu_layer(i18n::tr("Refill levels", i18n::fa_l_refill), false, true)) open_refill_upsell("click");
        if (ImGui::IsItemHovered())
            Theme::tooltip("Prices that keep getting hit and keep refilling.\nFree on BTC, Pro on every other market.");
        return;
    }
    const bool free_here = access == refill::Access::free_taste;
    if (Theme::menu_layer(i18n::tr("Refill levels", i18n::fa_l_refill), rt_refill_enabled_, false, free_here ? "free here" : nullptr))
        rt_refill_enabled_ = !rt_refill_enabled_;
    if (ImGui::IsItemHovered())
        Theme::tooltip("Prices that keep getting hit and keep refilling.\nObserved refills, not a hidden order or its size.%s",
                       free_here ? "\nFree on BTC, Pro on every other market." : "");
    const refill::Market* m = refill::store().find(pair_.exchange, pair_.symbol);
    if (rt_refill_enabled_ && m && m->status == refill::Status::unavailable) {
        if (const char* note = refill::unavailable_note(m->reason)) {
            Theme::begin_menu_detail();
            Theme::menu_note(note);
            Theme::end_menu_detail();
        }
    }
}
