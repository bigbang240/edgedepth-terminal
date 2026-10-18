// Hyperliquid rails from the server (core/hl_rails.h): live messages and stored records merge into one rail each, a
// standing rail missing from a message ended while the terminal was away, a stored end holds while a stale "standing"
// record does not, and the view draws each rail's stretches whose dollars in the chosen leverage reach the Minimum $.
// The rules that make the rails (one per census bin, the 80-20 birth, holds, steps, touches) are the server's, tested
// in backend internal/hlrails. The overlay's picture (core/hl_rails_picture.h), kept across frames, equals the plain
// per-frame pass it replaced at every clock, view and mode below.
#include "core/hl_rails.h"
#include "core/hl_rails_picture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

using namespace hl_rails;

static constexpr int64_t t0 = 1'800'000'000'000;
static int64_t at(int m) { return t0 + int64_t(m)*60'000; }

// A long rail at a price, born at minute b, with one step at minute s: usd split over the leverage groups.
static Rail rail(int64_t id, int b, int s, double usd, Mix lev = {}) {
    Rail r;
    r.id = id;
    r.is_long = true;
    r.price = 100.0 - static_cast<double>(id);
    r.born_ms = at(b);
    if (lev == Mix{}) lev = {0, usd, 0, 0};
    r.steps.push_back({at(s), usd, r.price*0.9988, r.price*1.0012, lev, 7});
    return r;
}

static Latest latest(int m) {
    Latest l;
    l.timestamp_ms = at(m);
    l.mark = 110;
    l.config_id = "hlrails.v1";
    l.census_version = 2;
    l.usable_notional_usd = 5e6;
    l.coverage_denominator_usd = 1e7;
    return l;
}

static const Rail* find(const Store& s, int64_t id) {
    const auto it = s.rails().find({"hlrails.v1", id});
    return it == s.rails().end() ? nullptr : &it->second;
}

static Rail ended(int64_t id, int m, const char* why) {
    Rail e;
    e.id = id;
    e.end_ms = at(m);
    e.end = why;
    return e;
}


// ---- The kept picture against the plain per-frame pass (rendering/hl_rails_overlay.h before 2026-10-18) ----

// A plot: the view in chart time (real time less shift) mapped linearly onto pixels, y downwards.
struct LinearProj {
    double x0 = 0, x1 = 1, y0 = 0, y1 = 1, shift = 0;
    float px = 0, py = 0, pw = 1, ph = 1;
    float x(double t) const { return static_cast<float>(px + (t - shift - x0)*pw/(x1 - x0)); }
    float y(double price) const { return static_cast<float>(py + ph - (price - y0)*ph/(y1 - y0)); }
    double price_at(float yp) const { return y0 + (py + ph - yp)*(y1 - y0)/ph; }
};

struct Plain {
    std::vector<Line> lines;  // level, quiet set from the plain typical
    std::vector<size_t> labels;
    std::vector<Row> rows;
    double row_max = 0, typical = 0, vmax = 0;
    size_t selected = 0, available = 0;
};

// The overlay's pass as it ran every frame until 2026-10-18 (segments(), the sorts, the sets, the profile's scans),
// with ImPlot's projection swapped for proj.
static Plain plain_picture(const Book& book, const Latest* latest, const Candles& candles, int64_t asof, double ratio,
                           const Plot& view, const LinearProj& proj, bool profile, bool major_only) {
    using liqmap2_rails::Rail;
    Plain out;
    const auto st = state(latest, asof);
    const bool stale = st == State::stale;
    const int64_t clock = stale ? std::min(asof, latest->timestamp_ms + kColumnMs) : asof;
    const bool have_candles = candles.t && candles.lo && candles.hi;
    const double shift = candles.shift_ms;
    int64_t now_edge = clock;
    if (!stale && shift > 0) {
        if (candles.forming) now_edge = std::max(clock, static_cast<int64_t>(candles.forming_t + 2*shift));
        else if (have_candles && !candles.t->empty()) {
            const auto& t = *candles.t;
            auto it = std::upper_bound(t.begin(), t.end(), static_cast<double>(clock));
            if (it != t.begin()) now_edge = std::max(clock, static_cast<int64_t>(*std::prev(it) + 2*shift));
        }
    }
    const double t_lo = view.t_lo, t_hi = view.t_hi;
    const auto x_of = [&](double t) { return proj.x(t); };
    const auto y_of = [&](double price) { return std::round(proj.y(price)); };
    double vm = 0;
    auto segs = liqmap2_rails::segments(book.rails, clock, t_lo, t_hi, 0.9*view.y_min/ratio, 1.1*view.y_max/ratio, vm,
                                        liqmap2_rails::Colour::dollars);
    std::stable_sort(segs.begin(), segs.end(), [](const auto& a, const auto& b) {
        return a.rail != b.rail ? a.rail->id < b.rail->id : a.t0 < b.t0;
    });
    std::vector<Line> lines;
    std::set<const Rail*> pushed, consumed;
    for (size_t i = 0; i < segs.size();) {
        size_t j = i + 1;
        while (j < segs.size() && segs[j].rail == segs[i].rail) ++j;
        const Rail& r = *segs[i].rail;
        const double price = r.price*ratio;
        if (price < view.y_min || price > view.y_max) { i = j; continue; }
        int64_t end = INT64_MAX, start = segs[i].t0;
        if (have_candles || candles.forming) {
            const float y = proj.y(price);
            const double slack = std::abs(proj.price_at(y - (4.0f*0.5f + 1.0f)) - price);
            end = consumed_at(candles, r.is_long, price, segs[i].t0, static_cast<int64_t>(t_hi) + 1, slack);
            start = clear_start(candles, price, segs[i].t0, static_cast<int64_t>(2*shift), slack);
            if (start > segs[i].t0) pushed.insert(&r);
            if (end != INT64_MAX) consumed.insert(&r);
        }
        std::vector<Line> mine;
        for (size_t k = i; k < j; ++k) {
            Line l{std::max(segs[k].t0, start), segs[k].t1, price, segs[k].v, &r, segs[k].step, 0, false, false};
            if (l.t0 >= end || l.t0 >= std::max(l.t1, l.t1 >= clock ? now_edge : l.t1)) continue;
            l.t1 = std::min(l.t1, end);
            if (l.t1 >= clock && end == INT64_MAX && (r.end_ms == 0 || r.end_ms > clock)) l.t1 = std::max(l.t1, now_edge);
            if (!mine.empty() && mine.back().t1 == l.t0 && x_of(double(mine.back().t1)) - x_of(double(mine.back().t0)) < 8.0f) {
                l.t0 = mine.back().t0;
                mine.pop_back();
            }
            mine.push_back(l);
        }
        if (!mine.empty()) {
            const double a = std::max(double(mine.front().t0), t_lo), b = std::min(double(mine.back().t1), t_hi);
            const bool fresh = mine.back().t1 >= clock && !pushed.contains(&r);
            if (fresh || x_of(b) - x_of(a) >= 12.0f) lines.insert(lines.end(), mine.begin(), mine.end());
        }
        i = j;
    }
    std::erase_if(lines, [&](const Line& l) {
        return l.price < view.y_min || l.price > view.y_max ||
               x_of(std::min(double(l.t1), t_hi)) - x_of(std::max(double(l.t0), t_lo)) < 3.0f;
    });
    std::vector<DisplayCandidate> candidates;
    for (size_t i = 0; i < lines.size();) {
        const auto& first = lines[i];
        double peak = 0;
        int64_t end = first.t0;
        size_t j = i;
        for (; j < lines.size() && lines[j].rail == first.rail; ++j) {
            peak = std::max(peak, lines[j].v);
            end = std::max(end, lines[j].t1);
        }
        candidates.push_back({first.rail->id, first.rail->is_long, peak,
            std::max<int64_t>(0, std::min(clock, end) - first.rail->born_ms), y_of(first.price)});
        i = j;
    }
    std::set<int64_t> selected;
    if (major_only) selected = major_levels(candidates);
    else for (const auto& c : candidates) selected.insert(c.id);
    out.available = candidates.size();
    out.selected = selected.size();
    for (const auto& l : lines) out.vmax = std::max(out.vmax, l.v);
    {
        std::vector<double> peaks;
        for (size_t i = 0; i < lines.size();) {
            double peak = 0;
            size_t j = i;
            for (; j < lines.size() && lines[j].rail == lines[i].rail; ++j) peak = std::max(peak, lines[j].v);
            if (peak > 0) peaks.push_back(peak);
            i = j;
        }
        if (!peaks.empty()) {
            const auto mid = peaks.begin() + static_cast<std::ptrdiff_t>((peaks.size() - 1)/2);
            std::nth_element(peaks.begin(), mid, peaks.end());
            out.typical = *mid;
        }
    }
    std::erase_if(lines, [&](const Line& l) { return !selected.contains(l.rail->id); });
    std::stable_sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.v < b.v; });
    for (auto& l : lines) {
        l.level = significance(l.v, out.typical);
        l.quiet = l.v < out.typical;
    }
    std::vector<size_t> live;
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& l = lines[i];
        if (l.t1 >= clock && !stale && (l.rail->end_ms == 0 || l.rail->end_ms > clock) && l.v > 0) live.push_back(i);
    }
    std::stable_sort(live.begin(), live.end(), [&](size_t a, size_t b) { return lines[a].v > lines[b].v; });
    out.labels = live;
    if (profile) {
        out.rows.resize(static_cast<size_t>(std::max(1.0f, std::ceil(view.height/5.0f))));
        for (const auto& [key, r] : book.rails) {
            if (!selected.contains(r.id)) continue;
            if ((r.end_ms > 0 && r.end_ms <= clock) || r.born_ms > clock || r.steps.empty() || stale) continue;
            const auto* step = liqmap2_rails::step_at(r, clock);
            const double price = r.price*ratio;
            if (!(step->usd > 0) || price < view.y_min || price > view.y_max) continue;
            if (consumed.contains(&r) || consumed_at(candles, r.is_long, price, r.born_ms, clock + 1, 0) != INT64_MAX) continue;
            const int k = std::clamp(static_cast<int>((proj.y(price) - view.top)/5.0f), 0, static_cast<int>(out.rows.size()) - 1);
            out.rows[k].v += step->usd;
            ++out.rows[k].rails;
        }
        for (const auto& row : out.rows) out.row_max = std::max(out.row_max, row.v);
    }
    out.lines = std::move(lines);
    return out;
}

static bool same_picture(const Picture& p, const Plain& q) {
    if (p.lines.size() != q.lines.size() || p.labels.size() != q.labels.size() || p.rows.size() != q.rows.size()) return false;
    if (p.typical != q.typical || p.vmax != q.vmax || p.selected != q.selected || p.available != q.available ||
        p.row_max != q.row_max)
        return false;
    for (size_t i = 0; i < p.lines.size(); ++i) {
        const auto& a = p.lines[i];
        const auto& b = q.lines[i];
        if (a.t0 != b.t0 || a.t1 != b.t1 || a.price != b.price || a.v != b.v || a.rail != b.rail || a.step != b.step ||
            a.level != b.level || a.quiet != b.quiet)
            return false;
    }
    for (size_t i = 0; i < p.labels.size(); ++i)
        if (p.labels[i] != q.labels[i]) return false;
    for (size_t i = 0; i < p.rows.size(); ++i)
        if (p.rows[i].v != q.rows[i].v || p.rows[i].rails != q.rows[i].rails) return false;
    return true;
}

static uint64_t rng_state = 88172645463325252ULL;
static uint64_t rnd() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}
static int rnd_in(int lo, int hi) { return lo + static_cast<int>(rnd()%static_cast<uint64_t>(hi - lo + 1)); }

// Server rails around 100: longs below, shorts above, born over ten hours, a step every half hour to an hour and a
// half from a few dollar figures (ties on purpose), some ended (touched, closed, gap), some dipping under the filter.
static Store random_store(int n) {
    static const double kUsd[] = {0.5e6, 1e6, 2e6, 2e6, 3e6, 5e6, 5e6, 8e6, 13e6};
    Store store;
    for (int i = 0; i < n; ++i) {
        Rail r;
        r.id = i + 1;
        r.price = 84.0 + 0.5*rnd_in(0, 64);
        r.is_long = r.price < 100.0;
        const int born = rnd_in(-60, 700);
        r.born_ms = at(born);
        int m = born + rnd_in(0, 20);
        while (m < 900) {
            const double usd = kUsd[rnd()%9];
            r.steps.push_back({at(m), usd, r.price*0.9988, r.price*1.0012, {}, 3});
            m += rnd_in(30, 90);
        }
        const int fate = rnd_in(0, 9);
        if (fate < 4) {
            r.end_ms = at(born + rnd_in(5, 400)) + rnd_in(0, 59)*1000;
            r.end = fate == 0 ? "touched" : fate == 1 ? "closed" : fate == 2 ? "gap" : "";
            while (r.steps.size() > 1 && r.steps.back().at_ms > r.end_ms) r.steps.pop_back();
        }
        store.apply_record("hlrails.v1", r);
    }
    return store;
}

// Every clock the kept picture is asked at gets the plain pass's answer; returns how many times it was rebuilt.
struct PictureRun {
    int checked = 0, rebuilt = 0, mismatched = 0;
};
static void run_clocks(PictureRun& run, Kept& kept, const Book& book, const Latest* latest, const Candles& candles,
                       const std::vector<int64_t>& clocks, double ratio, const Plot& view, const LinearProj& proj, bool profile,
                       bool major_only, bool live_messages) {
    int64_t keyed_ms = -1;
    uint64_t keyed_book = 0;
    for (const int64_t asof : clocks) {
        Latest msg;
        const Latest* l = latest;
        if (live_messages) {  // a live message every minute (the overlay keys on its time)
            msg = *latest;
            msg.timestamp_ms = asof - (asof - t0)%60'000;
            l = &msg;
        }
        const int64_t ms = l ? l->timestamp_ms : 0;
        const bool same = ms == keyed_ms && book.revision == keyed_book;
        keyed_ms = ms;
        keyed_book = book.revision;
        const int64_t built = kept.valid ? kept.asof : -1;
        const auto& pic = keep(kept, same, book, l, candles, asof, ratio, view, proj, profile, major_only);
        if (!kept.valid || kept.asof != built || !same) ++run.rebuilt;
        const auto plain = plain_picture(book, l, candles, asof, ratio, view, proj, profile, major_only);
        ++run.checked;
        if (!same_picture(pic, plain)) {
            if (run.mismatched++ < 5)
                std::fprintf(stderr, "picture differs at clock %+.3f min (lines %zu vs %zu, selected %zu vs %zu)\n",
                             double(asof - t0)/60000.0, pic.lines.size(), plain.lines.size(), pic.selected, plain.selected);
        }
    }
}

int main() {
    // Live: rails carried each minute keep one record, a step adds once, an end sticks, a resume lifts it.
    {
        Store s;
        s.apply_live(latest(3), {rail(1, 0, 2, 5000), rail(2, 1, 3, 9000)}, {}, {});
        s.apply_live(latest(4), {rail(1, 0, 2, 5000), rail(2, 1, 3, 9000)}, {}, {});
        CHECK(s.rails().size() == 2 && find(s, 1)->steps.size() == 1 && find(s, 1)->end_ms == 0);
        s.apply_live(latest(6), {rail(1, 0, 6, 8000)}, {ended(2, 5, "touched")}, {});
        CHECK(find(s, 1)->steps.size() == 2 && find(s, 1)->steps.back().usd == 8000);
        CHECK(find(s, 2)->end_ms == at(5) && find(s, 2)->end == "touched");
        s.apply_live(latest(7), {}, {ended(1, 7, "closed")}, {});
        CHECK(find(s, 1)->end == "closed");
        s.apply_live(latest(9), {rail(1, 0, 9, 8000)}, {}, {1});
        CHECK(find(s, 1)->end_ms == 0 && find(s, 1)->end.empty());
        // An older message arriving late changes nothing.
        const uint64_t rev = s.revision();
        s.apply_live(latest(8), {}, {ended(1, 8, "gap")}, {});
        CHECK(s.revision() == rev && find(s, 1)->end_ms == 0);
        CHECK(s.from_ms() == at(3) && s.latest().timestamp_ms == at(9));
    }

    // A standing rail a message neither carries nor ends ended while the terminal was away: it ends at the message.
    {
        Store s;
        s.apply_live(latest(3), {rail(1, 0, 2, 5000), rail(2, 1, 3, 9000)}, {}, {});
        s.apply_live(latest(10), {rail(1, 0, 2, 5000)}, {}, {});
        CHECK(find(s, 2)->end_ms == at(10) && find(s, 2)->end.empty());
        // A rail born after the message is not ended by it.
        s.apply_record("hlrails.v1", rail(3, 11, 11, 5000));
        s.apply_live(latest(10), {rail(1, 0, 2, 5000)}, {}, {});
        CHECK(find(s, 3)->end_ms == 0);
    }

    // History: a stored record brings every step and its end; a stored end holds; a record saying standing yields to
    // the live messages once there are any (a reply read just before a message that ended the rail).
    {
        Store s;
        Rail r = rail(4, 0, 1, 5000);
        r.steps.push_back({at(30), 12000, r.price*0.9988, r.price*1.0012, {0, 12000, 0, 0}, 9});
        r.end_ms = at(40);
        r.end = "touched";
        s.covered_from(at(-1440));
        s.apply_record("hlrails.v1", r);
        CHECK(find(s, 4)->steps.size() == 2 && find(s, 4)->end == "touched" && s.from_ms() == at(-1440));
        Rail standing = rail(5, 0, 1, 5000);
        s.apply_record("hlrails.v1", standing);  // no live message yet: standing
        CHECK(find(s, 5)->end_ms == 0);
        s.apply_live(latest(50), {}, {ended(5, 45, "closed")}, {});
        CHECK(find(s, 5)->end == "closed");
        s.apply_record("hlrails.v1", standing);  // a stale reply
        CHECK(find(s, 5)->end == "closed" && find(s, 5)->end_ms == at(45));
        // Another config's rail with the same id stays apart.
        s.apply_record("hlrails.v2", rail(5, 0, 1, 7000));
        CHECK(s.rails().size() == 3);
    }

    // Bounded: rails ended a week before the newest message go.
    {
        Store s;
        Rail old = rail(6, 0, 0, 5000);
        old.end_ms = at(10);
        old.end = "closed";
        s.apply_record("hlrails.v1", old);
        s.apply_live(latest(10 + 8*24*60), {}, {}, {});
        CHECK(find(s, 6) == nullptr);
    }

    // The view: the chosen leverage's dollars set each step; a rail draws only from a step reaching the Minimum $,
    // runs on down to half of it, and a stretch the filter cuts ends with "filter".
    {
        Store s;
        Rail r = rail(7, 0, 2, 10000, {1000, 3000, 4000, 2000});
        r.steps.push_back({at(10), 2400, r.price*0.9988, r.price*1.0012, {200, 600, 1000, 600}, 3});   // 18-37x: 1000
        r.steps.push_back({at(20), 900, r.price*0.9988, r.price*1.0012, {100, 200, 300, 300}, 2});     // 18-37x: 300
        r.steps.push_back({at(30), 20000, r.price*0.9988, r.price*1.0012, {0, 0, 20000, 0}, 4});
        r.end_ms = at(50);
        r.end = "touched";
        s.apply_record("hlrails.v1", r);
        View v;
        const Book& all = v.sync(s, {kAllGroups, 1000});
        CHECK(all.rails.size() == 1);
        const auto& a = all.rails.begin()->second;
        CHECK(a.born_ms == at(0) && a.end_ms == at(50) && a.end == "touched" && a.steps.size() == 4);
        CHECK(a.steps[0].usd == 10000 && a.steps[2].usd == 900);  // 900 >= half of 1000: runs on
        CHECK(all.detail.at(a.id)[0].mix[2] == 4000 && all.detail.at(a.id)[0].positions == 7);
        // Only 18-37x, at $2,000: 4,000 draws, 1,000 runs on (half is 1,000), 300 cuts, 20,000 draws again.
        const Book& mid = v.sync(s, {static_cast<uint8_t>(1u << 2), 2000});
        CHECK(mid.rails.size() == 2);
        const auto& first = mid.rails.begin()->second;
        const auto& second = std::next(mid.rails.begin())->second;
        CHECK(first.born_ms == at(0) && first.end_ms == at(20) && first.end == "filter" && first.steps.size() == 2);
        CHECK(second.born_ms == at(30) && second.end_ms == at(50) && second.end == "touched" && second.steps[0].usd == 20000);
        // At $50,000 nothing draws.
        CHECK(v.sync(s, {kAllGroups, 50000}).rails.empty());
        // Unchanged store and filter: the same book (no rebuild).
        const uint64_t rev = v.sync(s, {kAllGroups, 50000}).revision;
        CHECK(v.sync(s, {kAllGroups, 50000}).revision == rev);
        // A step without leverage figures counts only while every group is chosen.
        Store t;
        t.apply_record("hlrails.v1", rail(8, 0, 0, 5000, {0, 0, 0, 0}));
        Rail z = rail(8, 0, 0, 5000);
        z.steps[0].lev = {};
        Store u;
        u.apply_record("hlrails.v1", z);
        View w;
        CHECK(w.sync(u, {kAllGroups, 1000}).rails.size() == 1);
        CHECK(w.sync(u, {1, 1000}).rails.empty());
    }

    // The census state from the newest message.
    {
        Latest l = latest(0);
        CHECK(state(nullptr, at(0)) == State::missing);
        CHECK(state(&l, at(1)) == State::ready);
        CHECK(state(&l, at(4)) == State::stale);
        l.census_version = 1;
        CHECK(state(&l, at(1)) == State::legacy);
        l.census_version = 2;
        l.usable_notional_usd = 0;
        CHECK(state(&l, at(1)) == State::unavailable);
    }
    // The basis: one sample a message (frames between messages do not move it), averaged; a jump starts over.
    {
        Basis b;
        CHECK(b.ratio() == 1.0);
        b.update(at(0), 0.0);  // no price yet
        CHECK(b.ratio() == 1.0);
        b.update(at(0), 1.0004);
        CHECK(std::abs(b.ratio() - 1.0004) < 1e-12);
        b.update(at(0), 1.0030);  // the same message, the live price moved: held
        CHECK(std::abs(b.ratio() - 1.0004) < 1e-12);
        b.update(at(1), 1.0000);  // the running mean of two
        CHECK(std::abs(b.ratio() - 1.0002) < 1e-12);
        for (int m = 2; m < 200; ++m) b.update(at(m), m % 2 ? 1.0006 : 0.9998);  // alternating noise
        CHECK(std::abs(b.ratio() - 1.0002) < 0.0001);
        const double before = b.ratio();
        b.update(at(200), 1.0030);  // one noisy minute moves it a fifteenth of the way at most
        CHECK(b.ratio() - before <= (1.0030 - before)/kBasisSamples + 1e-12);
        b.update(at(201), 1.05);  // another market or a bad mark: starts over
        CHECK(std::abs(b.ratio() - 1.05) < 1e-12);
        b.reset();
        CHECK(b.ratio() == 1.0);
    }

    // Display selection is bounded per side and invariant to input order.
    // A larger nearby rail wins without changing the smaller rail's price/data.
    {
        std::vector<DisplayCandidate> c;
        for (int i = 0; i < 100; ++i) c.push_back({i + 1, i >= 50, double(i + 1), 60000, float(i*20)});
        auto selected = major_levels(c);
        CHECK(selected.size() == 12);
        CHECK(selected.contains(100) && selected.contains(50));
        CHECK(!selected.contains(1));
        std::reverse(c.begin(), c.end());
        CHECK(major_levels(c) == selected);
        CHECK(c.size() == 100);  // source inventory was not filtered in place
        c = {{1, true, 100, 60000, 10}, {2, true, 100, 120000, 10},
             {3, false, 80, 60000, 12}, {4, false, 70, 60000, 100},
             {5, false, 0, 60000, 200}};
        selected = major_levels(c);
        CHECK(selected.size() == 2);
        CHECK(selected.contains(2) && selected.contains(4));
        CHECK(major_levels({}).empty());
    }


    // The overlay's picture, kept across frames (core/hl_rails_picture.h), equals the plain per-frame pass at every
    // clock: live with a message a minute (forming candle, then past its slot so standing lines follow the clock),
    // live going stale, replay forwards and back; four views (whole record, zoomed, the left edge ahead of the clock,
    // a narrow plot full of specks); major levels or every rail; profile on or off; Hyperliquid's own chart and a
    // basis. Ties in dollars are common here, so the major levels' longer-life tie-break turns as standing rails age.
    {
        Store store = random_store(140);
        hl_rails::View view_of_store;  // the overlay's View (core/hl_rails.h), Minimum $ 1.5M: some stretches cut
        const Book* book = &view_of_store.sync(store, {kAllGroups, 1.5e6});
        CHECK(book->rails.size() > 100);
        // 1 h candles, opens 00:00 to 09:00, the forming one at 10:00; a walk between 95 and 105 that eats some rails.
        std::vector<double> ct, clo, chi;
        double mid = 100;
        for (int m = 0; m <= 540; m += 60) {
            mid = std::clamp(mid + 0.5*rnd_in(-4, 4), 96.0, 104.0);
            ct.push_back(double(at(m)));
            clo.push_back(mid - 0.25*rnd_in(1, 6));
            chi.push_back(mid + 0.25*rnd_in(1, 6));
        }
        Candles forming{&ct, &clo, &chi, 1'800'000.0, true, double(at(600)), 98.2, 101.7};
        Candles closed_only{&ct, &clo, &chi, 1'800'000.0};
        Candles none;
        struct PlotSpec { double t_lo, t_hi, y0, y1; float px, py, pw, ph; };
        const PlotSpec plots[] = {{double(at(-30)), double(at(760)), 84, 116, 0, 0, 1400, 800},
                              {double(at(400)), double(at(700)), 94, 106, 12.5f, 37.25f, 900, 600},
                              {double(at(650)), double(at(900)), 80, 120, 0, 10, 600, 500},
                              {double(at(-200)), double(at(2000)), 86, 114, 3, 3, 300, 420}};
        std::vector<int64_t> live_clocks, replay_clocks;
        for (int64_t c = at(610); c <= at(700); c += 13'000) live_clocks.push_back(c);
        for (int64_t c = at(-40); c <= at(900); c += 61'000) replay_clocks.push_back(c);
        // Every moment the picture can turn, and either side of it.
        for (const auto& [k, r] : book->rails) {
            std::vector<int64_t> ts{r.born_ms, r.end_ms, liqmap2_rails::end_known_ms(r)};
            for (const auto& st : r.steps) ts.push_back(st.at_ms);
            for (int64_t t : ts)
                for (int64_t d : {-1, 0, 1}) {
                    if (t + d >= at(610) && t + d <= at(700)) live_clocks.push_back(t + d);
                    if (t + d >= at(-40) && t + d <= at(900)) replay_clocks.push_back(t + d);
                }
        }
        for (int64_t t : {at(600), at(660), at(650), at(700)})
            for (int64_t d : {-1, 0, 1}) live_clocks.push_back(t + d);
        for (auto* cs : {&live_clocks, &replay_clocks}) {
            std::sort(cs->begin(), cs->end());
            cs->erase(std::unique(cs->begin(), cs->end()), cs->end());
        }
        Latest fresh = latest(610);
        Latest going_stale = latest(640);  // stale from 643 on: every rail stops at 641
        PictureRun run;
        int combos = 0;
        for (const auto& pl : plots)
            for (int mode = 0; mode < 4; ++mode)
                for (int opts = 0; opts < 4; ++opts) {
                    const bool profile = opts & 1, major_only = !(opts & 2);
                    const double ratio = mode == 1 ? 1.0004 : 1.0;
                    const Candles& candles = mode == 3 ? closed_only : mode == 2 && opts == 3 ? none : forming;
                    LinearProj proj{pl.t_lo - candles.shift_ms, pl.t_hi - candles.shift_ms, pl.y0, pl.y1, candles.shift_ms,
                                    pl.px, pl.py, pl.pw, pl.ph};
                    const Plot view{pl.t_lo, pl.t_hi, pl.y0, pl.y1, pl.py, pl.ph};
                    Kept kept;
                    if (mode == 0 || mode == 1)
                        run_clocks(run, kept, *book, &fresh, candles, live_clocks, ratio, view, proj, profile, major_only, true);
                    else if (mode == 2)
                        run_clocks(run, kept, *book, &going_stale, candles, live_clocks, ratio, view, proj, profile, major_only,
                                   false);
                    else {
                        run_clocks(run, kept, *book, nullptr, candles, replay_clocks, ratio, view, proj, profile, major_only,
                                   false);
                        std::vector<int64_t> back(replay_clocks.rbegin(), replay_clocks.rend());  // a seek back rebuilds
                        run_clocks(run, kept, *book, nullptr, candles, back, ratio, view, proj, profile, major_only, false);
                    }
                    ++combos;
                }
        CHECK(run.mismatched == 0);
        CHECK(run.checked > 50'000);
        // Frames between those moments reuse the picture: twenty seconds of frames at 60 Hz, one message.
        {
            Kept steady;
            PictureRun frames;
            std::vector<int64_t> clocks;
            for (int64_t c = at(615) + 2'000; c < at(615) + 22'000; c += 16) clocks.push_back(c);
            const Plot view{double(at(-30)), double(at(760)), 84, 116, 0, 800};
            const LinearProj proj{double(at(-30)) - 1'800'000.0, double(at(760)) - 1'800'000.0, 84, 116, 1'800'000.0, 0, 0,
                                  1400, 800};
            Latest msg = latest(615);
            run_clocks(frames, steady, *book, &msg, forming, clocks, 1.0, view, proj, true, true, false);
            CHECK(frames.mismatched == 0);
            CHECK(frames.checked == 1250);
            CHECK(frames.rebuilt <= 3);
        }
        std::printf("hl_rails_test: picture equal over %d clocks in %d runs, rebuilt %d times\n", run.checked, combos,
                    run.rebuilt);

        // A message adds a step mid-run: the book changes (its revision), the picture follows.
        PictureRun again;
        Kept kept;
        const Plot view{double(at(-30)), double(at(760)), 84, 116, 0, 800};
        const LinearProj proj{double(at(-30)) - 1'800'000.0, double(at(760)) - 1'800'000.0, 84, 116, 1'800'000.0, 0, 0, 1400, 800};
        std::vector<int64_t> first(live_clocks.begin(), live_clocks.begin() + live_clocks.size()/2);
        std::vector<int64_t> second(live_clocks.begin() + live_clocks.size()/2, live_clocks.end());
        run_clocks(again, kept, *book, &fresh, forming, first, 1.0, view, proj, true, true, true);
        Rail grown = store.rails().begin()->second;
        grown.steps.push_back({second.front() - 1, 40e6, grown.price*0.9988, grown.price*1.0012, {}, 9});
        grown.end_ms = 0;
        grown.end.clear();
        store.apply_record("hlrails.v1", grown);
        book = &view_of_store.sync(store, {kAllGroups, 1.5e6});
        run_clocks(again, kept, *book, &fresh, forming, second, 1.0, view, proj, true, true, true);
        CHECK(again.mismatched == 0);
    }

    // Equal dollars, the longer seen first: a standing rail overtakes an ended one when its life passes the other's.
    {
        Store store;
        Rail a;  // seen from 0 to 300
        a.id = 1; a.is_long = true; a.price = 99; a.born_ms = at(0); a.end_ms = at(300); a.end = "closed";
        a.steps.push_back({at(0), 5e6, 98.9, 99.1, {}, 1});
        Rail b = a;  // standing from 200: seen as long as a at 500
        b.id = 2; b.price = 97; b.born_ms = at(200); b.end_ms = 0; b.end.clear();
        b.steps[0] = {at(200), 5e6, 96.9, 97.1, {}, 1};
        store.apply_record("hlrails.v1", a);
        store.apply_record("hlrails.v1", b);
        hl_rails::View v;
        const Book& book = v.sync(store, {kAllGroups, 1000});
        std::vector<double> ct{double(at(420))}, clo{101}, chi{102};
        const Candles candles{&ct, &clo, &chi, 1'800'000.0, true, double(at(480)), 101, 102};
        const Plot view{double(at(-30)), double(at(560)), 90, 110, 0, 600};
        const LinearProj proj{double(at(-30)) - 1'800'000.0, double(at(560)) - 1'800'000.0, 90, 110, 1'800'000.0, 0, 0, 1000, 600};
        std::vector<int64_t> clocks;
        for (int64_t c = at(481); c <= at(560); c += 7'000) clocks.push_back(c);  // past the forming slot at 540 too
        for (int64_t d : {-1, 0, 1, 2}) clocks.push_back(at(500) + d);
        std::sort(clocks.begin(), clocks.end());
        Kept kept;
        PictureRun run;
        run_clocks(run, kept, book, nullptr, candles, clocks, 1.0, view, proj, false, true, false);
        CHECK(run.mismatched == 0);
        const auto& before = keep(kept, true, book, nullptr, candles, at(500) - 1, 1.0, view, proj, false, true);
        CHECK(before.lines.size() == 1 && before.lines[0].rail->born_ms == at(0));
        const auto& after = keep(kept, true, book, nullptr, candles, at(500) + 1, 1.0, view, proj, false, true);
        CHECK(after.lines.size() == 1 && after.lines[0].rail->born_ms == at(200));
    }

    // major_levels as flags equals major_levels.
    {
        for (int round = 0; round < 200; ++round) {
            std::vector<DisplayCandidate> c;
            const int n = rnd_in(0, 40);
            for (int i = 0; i < n; ++i)
                c.push_back({i + 1, rnd_in(0, 1) == 1, double(rnd_in(0, 6))*1e6, int64_t(rnd_in(0, 5))*60000, float(rnd_in(0, 400))});
            std::vector<char> chosen;
            std::vector<uint32_t> order;
            std::vector<float> ys;
            const size_t n_chosen = mark_major_levels(c, chosen, order, ys);
            const auto set = major_levels(c);
            CHECK(n_chosen == set.size());
            for (size_t i = 0; i < c.size(); ++i) CHECK(bool(chosen[i]) == set.contains(c[i].id));
        }
    }

    if (failures) {
        std::fprintf(stderr, "hl_rails_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("hl_rails_test: ok\n");
    return 0;
}
