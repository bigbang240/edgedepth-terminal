#pragma once
// Walls and pulled walls in real-time depth, from observations the client
// already has: sampled books (RealtimeDepthHistory) and received trades.
// Pure logic, no ImGui; the chart draws what this tracks
// (src/ui/chart_widget_rt_extras.cpp). Live and replay feed it the same way.
//
// A wall is one exact price, not a display row, so regrouping never moves or
// re-measures it. Every threshold is relative to the market itself (its median
// level in view and the size its touch holds), so a thin perpetual behaves like
// BTC. Size at and next to the best bid and ask follows the price (market makers
// requote as it moves), so walls form only beyond the touch, must outweigh it,
// and must stand a few seconds before they are shown or can be called pulled.
#include "core/realtime_history.h"
#include "types/types.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rt_walls {
// A level is a wall at 5x the median non-empty level on its side within the visible prices...
inline constexpr double kWallMultiple = 5.0;
// ...and at least the size the touch on its side has been holding.
inline constexpr double kTouchShare = 1.0;
// A wall ends below 3x that median; the gap to 5x keeps it from flickering.
inline constexpr double kExitMultiple = 3.0;
// Fewer non-empty levels than this on a side in view: no median, so no walls there.
inline constexpr size_t kMinLevelsForMedian = 3;
// The touch: the first five non-empty levels from the best bid and from the best ask.
inline constexpr size_t kTouchLevels = 5;
// The touch's size is its largest level, eased over this long so one print or quote does not swing it.
inline constexpr double kTouchEaseMs = 5000.0;
// A wall is shown, and can be called pulled, once it has stood this long.
inline constexpr int64_t kMinStandMs = 3000;
// Shown at once: the largest few per side.
inline constexpr size_t kMaxShownPerSide = 3;
// A pull is a drop of at least 70% from the largest size seen in the last second.
inline constexpr double kPullDrop = 0.70;
// The window that drop must happen in.
inline constexpr int64_t kPullWindowMs = 1000;
// "Near price" for a pull, relative to the market: within this many display rows of mid...
inline constexpr int kNearRows = 3;
// ...or within the mid's high-low range over this much recent history, if wider.
inline constexpr int64_t kNearRangeMs = 60000;
// Trades at the price under 25% of the size that vanished: pulled, not traded.
inline constexpr double kPullTradedShare = 0.25;
// Prints can land after the book update that shows the fill: decide this much later.
inline constexpr int64_t kTradeSettleMs = 500;
// Trade clocks can sit a little either side of a 100ms depth sample.
inline constexpr int64_t kTradeSlackMs = 150;
// Live walls tracked at once (the smallest yields to a larger new wall).
inline constexpr size_t kMaxActive = 32;
// Finished walls kept (traded through, faded or out of the observed book).
inline constexpr size_t kMaxEnded = 48;
// Recent pulls kept independently of the display budget (a bounded ring).
inline constexpr size_t kMaxPulls = 256;
// Show retained events for up to the real-time session history window.
inline constexpr int64_t kPullRetentionMs = 30 * 60 * 1000;
// A width-aware budget, with significant and recent pulls rather than only the newest.
inline constexpr size_t kMaxPullsDrawn = 12;
// Observations kept per wall for the one-second drop test (100ms samples).
inline constexpr size_t kWindowSamples = 16;
// Levels per side considered in one observation (512 levels per side at most).
inline constexpr size_t kMaxLevels = 1024;

// The display grid: zero-anchored rows of `ticks` native ticks, lower bound
// inclusive, the same boundaries as the RT heatmap and its ladder.
struct Grid {
    double native_tick = 0;
    int ticks = 1;
    bool valid() const { return native_tick > 0 && std::isfinite(native_tick) && ticks >= 1; }
    double step() const { return native_tick * ticks; }
    int64_t index(double price) const { return int64_t(std::floor((price / native_tick + 1e-7) / ticks)); }
    double low(int64_t row) const { return double(row) * step(); }
    double high(int64_t row) const { return double(row + 1) * step(); }
    bool operator==(const Grid& o) const { return native_tick == o.native_tick && ticks == o.ticks; }
};

struct Wall {
    bool bid = true;
    double price = 0;                // the exact level
    double tick = 0;                 // native tick, for matching levels and trades
    int64_t start_ms = 0, end_ms = 0;  // end_ms 0 while live
    int64_t last_ms = 0;             // last observation that measured it
    double size = 0, peak = 0;       // base quantity: latest and largest
    double multiple = 0, peak_multiple = 0;  // size over its side's median
    bool stood(int64_t now) const { return now - start_ms >= kMinStandMs; }
    bool contains(double p) const { return std::abs(p - price) < tick * 0.5; }
};

struct Pull {
    Wall wall;            // wall.end_ms is the pull time
    double vanished = 0;  // base quantity that left the level
    double traded = 0;    // base quantity traded at the level meanwhile
};

// Fixed-capacity ring, newest last. No allocation after construction.
template <class T, size_t N>
class Ring {
public:
    void push(const T& v) {
        items_[(head_ + count_) % N] = v;
        if (count_ < N) ++count_; else head_ = (head_ + 1) % N;
    }
    size_t size() const { return count_; }
    const T& operator[](size_t i) const { return items_[(head_ + i) % N]; }
    void clear() { head_ = count_ = 0; }
private:
    std::array<T, N> items_{};
    size_t head_ = 0, count_ = 0;
};

class Tracker {
public:
    Tracker() {
        active_.reserve(kMaxActive); pending_.reserve(kMaxActive);
        bids_.reserve(kMaxLevels); asks_.reserve(kMaxLevels);
        sizes_.reserve(kMaxLevels); candidates_.reserve(kMaxLevels * 2);
    }

    void reset() {
        active_.clear(); pending_.clear(); ended_.clear(); pulls_.clear();
        grid_ = {}; median_ = {0, 0}; touch_ = {0, 0}; last_ms_ = 0;
        mids_ = {};
    }

    // One depth observation. `view_low`/`view_high` are the visible prices:
    // the median and new walls come from levels inside them. The grid only
    // sizes "near price" for pulls; walls are exact prices.
    void observe(const RealtimeDepthHistory::Sample& sample, Grid grid, double view_low, double view_high) {
        if (!grid.valid() || sample.timestamp_ms <= 0 || !(sample.ask > sample.bid)) return;
        if (sample.timestamp_ms < last_ms_) reset();   // time went backwards: another traversal
        if (!(grid.native_tick == grid_.native_tick)) for (auto& live : active_) live.wall.tick = grid.native_tick;
        grid_ = grid;
        const int64_t dt = last_ms_ > 0 ? sample.timestamp_ms - last_ms_ : 0;
        last_ms_ = sample.timestamp_ms;
        // Sizes across a depth gap are not comparable: no pull spans a new segment.
        if (sample.segment_start) for (auto& live : active_) live.count = live.next = 0;
        build_levels(sample);
        for (int side = 0; side < 2; ++side) {
            const auto& levels = side ? asks_ : bids_;
            median_[side] = side_median(levels, side == 0, view_low, view_high);
            ease_touch(side, touch_peak(levels, side == 0), sample.segment_start ? 0 : dt);
        }
        observe_mid(sample.timestamp_ms, (sample.bid + sample.ask) * 0.5);
        update_active(sample);
        detect(sample, view_low, view_high);
    }

    // Decide drops once late prints have had time to arrive. `trades` is sorted
    // by time; `trades_through_ms` is the time through which received trades have
    // been delivered (the clock when nothing is queued). Depth is written as it
    // arrives while trades wait for the main thread, so a slow or throttled frame
    // can see a level emptied before the prints that emptied it: nothing is
    // decided until the trades have caught up. A drop older than the retained
    // trades cannot be shown to be untraded, so it ends as a plain wall.
    template <class Trades>
    void settle(int64_t clock_ms, const Trades& trades, int64_t trades_through_ms) {
        for (size_t i = 0; i < pending_.size();) {
            const auto& p = pending_[i];
            if (clock_ms < p.wall.end_ms + kTradeSettleMs || trades_through_ms < p.wall.end_ms + kTradeSlackMs) { ++i; continue; }
            const int64_t from = p.from_ms - kTradeSlackMs;
            const bool covered = trades.empty() || trades.begin()->timestamp_ms <= from;
            const double traded = traded_at(trades, p.wall, from, p.wall.end_ms + kTradeSlackMs);
            if (covered && traded < kPullTradedShare * p.vanished) pulls_.push({p.wall, p.vanished, traded});
            else ended_.push(p.wall);
            pending_[i] = pending_.back(); pending_.pop_back();
        }
    }
    // Received trades were lost before delivery (a full dispatch queue): no drop
    // waiting on them can be shown untraded, so they end as plain walls.
    void trades_lost() {
        for (const auto& p : pending_) ended_.push(p.wall);
        pending_.clear();
    }

    template <class F> void for_each_live(F&& f) const { for (const auto& l : active_) f(l.wall); }
    // Drops waiting on trades count as finished walls until they are decided.
    template <class F> void for_each_finished(F&& f) const {
        for (size_t i = 0; i < ended_.size(); ++i) f(ended_[i]);
        for (const auto& p : pending_) f(p.wall);
    }
    template <class F> void for_each_pull(F&& f) const { for (size_t i = 0; i < pulls_.size(); ++i) f(pulls_[i]); }
    // Newest first; `f` returns false to stop.
    template <class F> void for_each_pull_newest(F&& f) const { for (size_t i = pulls_.size(); i-- > 0;) if (!f(pulls_[i])) break; }

    // The walls to show now: live, stood kMinStandMs, inside the visible prices;
    // the largest kMaxShownPerSide per side, largest first. Returns how many.
    size_t shown(int64_t now, double view_low, double view_high, std::array<Wall, kMaxShownPerSide * 2>& out) const {
        size_t n = 0;
        for (int side = 0; side < 2; ++side) {
            std::array<Wall, kMaxShownPerSide> top{};
            size_t count = 0;
            for (const auto& live : active_) {
                const auto& w = live.wall;
                if (w.bid != (side == 0) || !w.stood(now) || w.price < view_low || w.price > view_high) continue;
                if (count < kMaxShownPerSide) top[count++] = w;
                else if (w.size > top[count - 1].size) top[count - 1] = w;
                else continue;
                for (size_t k = count - 1; k > 0 && top[k].size > top[k - 1].size; --k) std::swap(top[k], top[k - 1]);
            }
            for (size_t k = 0; k < count; ++k) out[n++] = top[k];
        }
        return n;
    }

    size_t live_count() const { return active_.size(); }
    size_t pull_count() const { return pulls_.size(); }
    size_t pending_count() const { return pending_.size(); }
    size_t ended_count() const { return ended_.size(); }
    double median(bool bid) const { return median_[bid ? 0 : 1]; }
    // The size the touch on this side has been holding (its largest level, eased).
    double touch(bool bid) const { return touch_[bid ? 0 : 1]; }
    // How far from mid a drop still counts as "near price" (see kNearRows).
    double near_distance() const { return std::max(kNearRows * grid_.step(), mid_range()); }

private:
    struct Level { double price = 0, size = 0; };
    struct Obs { int64_t ms = 0; double size = 0; };
    struct Live {
        Wall wall;
        std::array<Obs, kWindowSamples> recent{};
        size_t count = 0, next = 0;
        double typical = 0;               // its side's last measured median
        void push(int64_t ms, double size) {
            recent[next] = {ms, size}; next = (next + 1) % kWindowSamples;
            if (count < kWindowSamples) ++count;
        }
        // The largest size in the last second before `now`. A quiet book holds
        // between samples (thin markets can go seconds without one), so the
        // latest earlier observation stands until `now` whatever its age.
        Obs reference(int64_t now) const {
            Obs best{}, latest{};
            for (size_t i = 0; i < count; ++i) {
                if (recent[i].ms >= now) continue;
                if (recent[i].ms > latest.ms) latest = recent[i];
                if (recent[i].ms >= now - kPullWindowMs && recent[i].size > best.size) best = recent[i];
            }
            return latest.size > best.size ? latest : best;
        }
    };
    struct Pending { Wall wall; int64_t from_ms = 0; double vanished = 0; };

    // Both sides ascending by price, one entry per price, empty levels dropped.
    void build_levels(const RealtimeDepthHistory::Sample& sample) {
        bids_.clear(); asks_.clear();
        bid_floor_ = sample.bid; ask_ceiling_ = sample.ask;
        for (const auto& level : sample.levels) {
            if (!(level.size > 0) || !std::isfinite(level.size) || !(level.price > 0)) continue;
            const bool bid = level.price <= sample.bid;
            auto& levels = bid ? bids_ : asks_;
            if (bid) bid_floor_ = std::min(bid_floor_, level.price);
            else ask_ceiling_ = std::max(ask_ceiling_, level.price);
            if (levels.size() < kMaxLevels) levels.push_back({level.price, level.size});
        }
        // Books arrive best first; keep both sides ascending for lookups.
        auto order = [](std::vector<Level>& levels) {
            const auto by_price = [](const Level& a, const Level& b) { return a.price < b.price; };
            if (std::is_sorted(levels.begin(), levels.end(), [](const Level& a, const Level& b) { return a.price > b.price; }))
                std::reverse(levels.begin(), levels.end());
            else if (!std::is_sorted(levels.begin(), levels.end(), by_price)) {
                std::sort(levels.begin(), levels.end(), by_price);
                size_t out = 0;
                for (size_t i = 0; i < levels.size(); ++i) {
                    if (out && levels[out - 1].price == levels[i].price) levels[out - 1].size += levels[i].size;
                    else levels[out++] = levels[i];
                }
                levels.resize(out);
            }
        };
        order(bids_); order(asks_);
    }

    // Index range of the touch within a side's ascending levels: bids the top
    // kTouchLevels, asks the bottom kTouchLevels.
    static bool in_touch(const std::vector<Level>& levels, size_t i, bool bid) {
        return bid ? i + kTouchLevels >= levels.size() : i < kTouchLevels;
    }
    static double touch_peak(const std::vector<Level>& levels, bool bid) {
        double peak = 0;
        for (size_t i = 0; i < levels.size(); ++i) if (in_touch(levels, i, bid)) peak = std::max(peak, levels[i].size);
        return peak;
    }
    void ease_touch(int side, double peak, int64_t dt) {
        double& t = touch_[size_t(side)];
        if (!(t > 0) || dt <= 0) { t = peak; return; }
        t += (peak - t) * (1.0 - std::exp(-double(dt) / kTouchEaseMs));
    }

    // The median level beyond the touch, inside the visible prices.
    double side_median(const std::vector<Level>& levels, bool bid, double view_low, double view_high) {
        sizes_.clear();
        for (size_t i = 0; i < levels.size(); ++i)
            if (!in_touch(levels, i, bid) && levels[i].price >= view_low && levels[i].price <= view_high)
                sizes_.push_back(levels[i].size);
        if (sizes_.size() < kMinLevelsForMedian) return 0;
        const auto middle = sizes_.begin() + std::ptrdiff_t(sizes_.size() / 2);
        std::nth_element(sizes_.begin(), middle, sizes_.end());
        return *middle;
    }

    double lookup(const std::vector<Level>& levels, double price) const {
        const double tolerance = grid_.native_tick * 0.25;
        const auto it = std::lower_bound(levels.begin(), levels.end(), price - tolerance,
            [](const Level& l, double p) { return l.price < p; });
        return it != levels.end() && it->price <= price + tolerance ? it->size : 0.0;
    }

    void finish(size_t i, int64_t at) {
        auto wall = active_[i].wall;
        wall.end_ms = at;
        ended_.push(wall);
        active_[i] = active_.back(); active_.pop_back();
    }

    void update_active(const RealtimeDepthHistory::Sample& sample) {
        const int64_t now = sample.timestamp_ms;
        const double mid = (sample.bid + sample.ask) * 0.5;
        for (size_t i = 0; i < active_.size();) {
            auto& live = active_[i];
            auto& wall = live.wall;
            // A price past the deepest observed level cannot be measured: end it
            // quietly where it was last seen, never as a pull.
            const double half = grid_.native_tick * 0.5;
            const bool observable = wall.bid ? wall.price >= bid_floor_ - half : wall.price <= ask_ceiling_ + half;
            if (!observable) { finish(i, wall.last_ms); continue; }
            const double size = lookup(wall.bid ? bids_ : asks_, wall.price);
            const auto ref = live.reference(now);
            live.push(now, size);
            // Too few levels in view for a median: hold the wall to its last one.
            if (median_[wall.bid ? 0 : 1] > 0) live.typical = median_[wall.bid ? 0 : 1];
            const double typical = live.typical;
            if (ref.size > 0 && size <= (1.0 - kPullDrop) * ref.size) {
                // Only a wall that stood can be called pulled; a quote that came and
                // went with the price is not a wall someone withdrew.
                const bool near = std::abs(wall.price - mid) <= near_distance();
                if (near && wall.stood(now) && pending_.size() < kMaxActive) {
                    Pending p{wall, ref.ms, ref.size - size};
                    p.wall.end_ms = now;
                    pending_.push_back(p);
                    active_[i] = active_.back(); active_.pop_back();
                } else finish(i, now);
                continue;
            }
            if (!(size > 0) || (typical > 0 && size < kExitMultiple * typical)) { finish(i, now); continue; }
            wall.size = size; wall.last_ms = now;
            wall.multiple = typical > 0 ? size / typical : wall.multiple;
            if (size > wall.peak) { wall.peak = size; wall.peak_multiple = wall.multiple; }
            ++i;
        }
    }

    void detect(const RealtimeDepthHistory::Sample& sample, double view_low, double view_high) {
        candidates_.clear();
        for (int side = 0; side < 2; ++side) {
            const double typical = median_[size_t(side)];
            if (!(typical > 0)) continue;
            const double threshold = std::max(kWallMultiple * typical, kTouchShare * touch_[size_t(side)]);
            const auto& levels = side ? asks_ : bids_;
            for (size_t i = 0; i < levels.size(); ++i) {
                const auto& level = levels[i];
                if (level.size < threshold || in_touch(levels, i, side == 0)) continue;
                if (level.price < view_low || level.price > view_high) continue;
                bool tracked = false;
                for (const auto& live : active_)
                    if (live.wall.bid == (side == 0) && live.wall.contains(level.price)) { tracked = true; break; }
                if (!tracked) candidates_.push_back({side == 0, level.price, level.size, typical});
            }
        }
        std::sort(candidates_.begin(), candidates_.end(),
            [](const Candidate& a, const Candidate& b) { return a.size > b.size; });
        for (const auto& c : candidates_) {
            if (active_.size() >= kMaxActive) {
                size_t smallest = 0;
                for (size_t i = 1; i < active_.size(); ++i)
                    if (active_[i].wall.size < active_[smallest].wall.size) smallest = i;
                if (active_[smallest].wall.size >= c.size) break;   // candidates are largest first
                finish(smallest, sample.timestamp_ms);
            }
            Live live;
            auto& w = live.wall;
            w.bid = c.bid; w.price = c.price; w.tick = grid_.native_tick;
            w.start_ms = w.last_ms = sample.timestamp_ms;
            w.size = w.peak = c.size;
            w.multiple = w.peak_multiple = c.size / c.median;
            live.typical = c.median;
            live.push(sample.timestamp_ms, c.size);
            active_.push_back(live);
        }
    }

    template <class Trades>
    static double traded_at(const Trades& trades, const Wall& wall, int64_t from, int64_t to) {
        double traded = 0;
        auto it = std::lower_bound(trades.begin(), trades.end(), from,
            [](const Terminal::Trade& t, int64_t ms) { return t.timestamp_ms < ms; });
        for (; it != trades.end() && it->timestamp_ms <= to; ++it)
            if (wall.contains(it->price)) traded += it->qty;
        return traded;
    }

    // The mid's high and low per second over the last kNearRangeMs (a ring).
    struct MidSecond { int64_t second = -1; double low = 0, high = 0; };
    static constexpr size_t kMidSeconds = size_t(kNearRangeMs / 1000);
    void observe_mid(int64_t ms, double mid) {
        const int64_t second = ms / 1000;
        auto& slot = mids_[size_t(second % int64_t(kMidSeconds))];
        if (slot.second != second) slot = {second, mid, mid};
        else { slot.low = std::min(slot.low, mid); slot.high = std::max(slot.high, mid); }
    }
    double mid_range() const {
        const int64_t now = last_ms_ / 1000;
        double low = 0, high = 0;
        bool any = false;
        for (const auto& slot : mids_) {
            if (slot.second < 0 || slot.second > now || slot.second <= now - int64_t(kMidSeconds)) continue;
            low = any ? std::min(low, slot.low) : slot.low;
            high = any ? std::max(high, slot.high) : slot.high;
            any = true;
        }
        return any ? high - low : 0.0;
    }

    struct Candidate { bool bid = true; double price = 0, size = 0, median = 0; };
    Grid grid_{};
    std::array<double, 2> median_{0, 0}, touch_{0, 0};
    std::array<MidSecond, kMidSeconds> mids_{};
    int64_t last_ms_ = 0;
    double bid_floor_ = 0, ask_ceiling_ = 0;
    std::vector<Live> active_;
    std::vector<Pending> pending_;
    Ring<Wall, kMaxEnded> ended_;
    Ring<Pull, kMaxPulls> pulls_;
    std::vector<Level> bids_, asks_;
    std::vector<double> sizes_;
    std::vector<Candidate> candidates_;
};

// Display selection only. Keep two places for recent, non-overlapping pulls;
// the rest favour size with a gentle five-minute age discount. The source
// event, side, price and time are never merged or moved to make it fit.
struct PullMarker {
    const Pull* pull = nullptr;
    float x = 0, y = 0, diameter = 0, alpha = 0;
    double priority = 0;
};

template <class Project>
size_t select_pulls(const Tracker& tracker, int64_t clock, double from, double to,
                    double low, double high, float width, Project&& project,
                    std::array<PullMarker, kMaxPullsDrawn>& out) {
    if (!(width > 0) || !std::isfinite(width)) return 0;
    std::array<PullMarker, kMaxPulls> candidates{};
    size_t count = 0;
    double largest = 0;
    tracker.for_each_pull([&](const Pull& p) {
        const auto& w = p.wall;
        const double value = p.vanished * w.price;
        if (w.end_ms > clock || clock - w.end_ms > kPullRetentionMs ||
            double(w.end_ms) < from || double(w.end_ms) > to || w.price < low || w.price > high ||
            !(value > 0) || !std::isfinite(value)) return;
        const auto [x, y] = project(p);
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        const double age = double(clock - w.end_ms) / 300000.0;
        candidates[count++] = {&p, x, y, 0, float(0.95 - 0.40 * std::min(1.0, age)), value / (1.0 + age)};
        largest = std::max(largest, value);
    });
    for (size_t i = 0; i < count; ++i) {
        auto& m = candidates[i];
        m.diameter = float(std::clamp(14.0 * std::sqrt(m.pull->vanished * m.pull->wall.price / largest), 7.0, 14.0));
    }
    const size_t budget = size_t(std::clamp(width / 70.0f, 6.0f, float(kMaxPullsDrawn)));
    const auto newer = [](const PullMarker& a, const PullMarker& b) {
        const auto& x = a.pull->wall; const auto& y = b.pull->wall;
        if (x.end_ms != y.end_ms) return x.end_ms > y.end_ms;
        if (x.price != y.price) return x.price < y.price;
        if (x.bid != y.bid) return x.bid;
        return x.start_ms > y.start_ms;
    };
    std::sort(candidates.begin(), candidates.begin() + count, [&](const PullMarker& a, const PullMarker& b) {
        return a.priority != b.priority ? a.priority > b.priority : newer(a, b);
    });
    size_t shown = 0;
    const auto add = [&](const PullMarker& m) {
        for (size_t i = 0; i < shown; ++i) {
            const float gap = (m.diameter + out[i].diameter) * 0.5f + 4.0f;
            if (std::abs(m.x - out[i].x) < gap && std::abs(m.y - out[i].y) < gap) return;
        }
        out[shown++] = m;
    };
    for (size_t i = 0; i < count && shown < budget - 2; ++i) add(candidates[i]);
    std::sort(candidates.begin(), candidates.begin() + count, newer);
    for (size_t i = 0; i < count && shown < budget; ++i) add(candidates[i]);
    return shown;
}
} // namespace rt_walls
