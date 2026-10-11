#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/market_data_view.h"
#include <cassert>

int main() {
    using namespace market_data_view;
    MarketDataHistory h;
    h.to = 1791238200000LL; h.from = h.to - 86400000;
    for (int64_t t = h.from; t <= h.to; t += 300000) h.times.push_back(double(t) / 1000);
    for (auto& v : h.values) v.assign(h.times.size(), 2);
    h.values[0].assign(h.times.size(), .05); h.values[3].assign(h.times.size(), 125);
    h.notional.assign(h.times.size(), 10);
    const auto first = first_observation(h, 4);
    assert(first == 240);
    Inspection selected;
    selected.hover_chart = 2; selected.hover = 250;
    assert(selected.effective(h, first) == 250);
    selected.point(h, 251, 2); selected.hover = 260; selected.hover_chart = 0;
    assert(selected.effective(h, first) == 251); // A pin always wins.
    selected.point(h, 260, 0); assert(selected.pin == int64_t(h.times[260] * 1000)); // Click moves, never toggles off.
    selected.step(h, first, 1, 0); assert(selected.effective(h, first) == 261);
    selected.unpin(); selected.step(h, first, 1, 0); assert(selected.effective(h, first) == 288);
    selected.step(h, first, 1, 0); assert(selected.effective(h, first) == 288);
    selected.point(h, 240, 1); selected.step(h, first, -1, 1); assert(selected.effective(h, first) == 240);
    auto refreshed = h;
    for (auto& t : refreshed.times) t += 300;
    refreshed.to += 300000;
    selected.point(h, 260, 2);
    assert(!selected.reconcile(refreshed, first)); assert(selected.effective(refreshed, first) == 259);
    selected.point(h, 240, 2);
    assert(selected.reconcile(refreshed, first)); assert(!selected.pin && selected.released);
    selected.point(h, 100, 2); assert(!selected.reconcile(h, 0)); assert(selected.reconcile(h, first));
    selected.point(h, 260, 2); refreshed = h; refreshed.times.erase(refreshed.times.begin() + 260);
    assert(selected.reconcile(refreshed, first)); // Missing exact timestamp releases; nearest isn't silently substituted.
    h.values[0][260] = NAN; h.values[3][261] = NAN;
    selected.point(h, 260, 2); assert(!selected.reconcile(h, first)); // Value gap keeps the observation pinned.
    const auto bounds = fit(h, first, false);
    assert(bounds[2].primary.high > 125 && bounds[2].price.high < 1);
    assert(std::isnan(h.values[0][260]) && h.values[3][260] == 125);
    assert(std::isnan(h.values[3][261]) && h.values[0][261] == .05);
    const auto quote = fit(h, first, true);
    assert(quote[1].primary.high > bounds[1].primary.high);
    assert(quote[2].primary.high == bounds[2].primary.high);
    assert(bounds[5].primary.low == -bounds[5].primary.high);
    h.values[4].assign(h.times.size(), NAN); assert(!fit(h, first, false)[4].available);
    assert(columns(1200, 18.4f) == 3 && columns(1200, 23) == 2 && columns(650, 18.4f) == 1);
    assert(columns(1199, 18.4f) == 2 && columns(759, 18.4f) == 1);
    h.values[3].assign(h.times.size(), NAN);
    assert(fit(h, first, false)[2].available); // Price still renders without a ratio.
    h.values[0].assign(h.times.size(), NAN);
    assert(!fit(h, first, false)[2].available);
    h.values[3][280] = 203.7;
    const auto ratio_only = fit(h, first, false)[2];
    assert(ratio_only.available && ratio_only.primary.low < 203.7 && ratio_only.primary.high > 203.7);
    MarketContext::Item old; old.at = 100; old.until = 1000;
    MarketContext::Item absent;
    assert(context_state(old, absent, 500) == ContextState::Current);
    assert(context_state(old, absent, 1000) == ContextState::Expired);
    assert(context_state(absent, old, 500) == ContextState::Withdrawn);
    assert(context_state(absent, old, 1001) == ContextState::Expired);
    assert(context_state(absent, absent, 500) == ContextState::Missing);
}
