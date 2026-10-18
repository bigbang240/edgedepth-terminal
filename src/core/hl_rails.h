#pragma once
// Hyperliquid positions drawn as rails (hub task liq-map-v2; James 2026-10-14: "rails like the current V2 rails, but in
// Hyperliquid colours"; 2026-10-15: "ACTUAL rails", "significant levels, not just ALL levels ... the 80-20"). The
// server builds them once from our node's census (backend cmd/hlrails-publisher, internal/hlrails, docs/HL_RAILS.md)
// and serves them: live on stream 42 (HLRails: every standing rail each census minute, the ends and resumes since the
// last message) and stored on stream 43 (HLRailsHistory, up to seven days a read). One rail per 0.25% census bin, at
// the bin's price for life, born only among the bins holding most of their side's dollars, ended once: by the
// Hyperliquid bar that reaches it ("touched"), when its positions leave ("closed"), or at a break in the census
// ("gap"). A rail ended "closed" can resume. The terminal keeps them (Store) and draws them in the V2 grammar
// (rendering/hl_rails_overlay.h) through View, which applies the viewer's real-leverage choice and Minimum $. A rail is
// positions that exist, read from our Hyperliquid node, not a prediction that price goes there.
#include "core/liqmap2_rails.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hl_rails {
// Real leverage, as the census buckets it (backend scripts/hl-node-census/node_census.py lev_tier_index: est_5x up to
// 7.5x, est_10x to 17.5x, est_25x to 37.5x, est_50x and above). Hyperliquid's largest is 40x, so the top three merge.
inline constexpr int kGroups = 4;
inline constexpr uint8_t kAllGroups = (1u << kGroups) - 1;
inline constexpr const char* kGroupLabels[kGroups] = {"Up to 7x", "8x to 17x", "18x to 37x", "38x and up"};
inline constexpr const char* kGroupChips[kGroups] = {"1-7x", "8-17x", "18-37x", "38x+"};  // the atlas: Latin-1 only
using Mix = std::array<double, kGroups>;

inline constexpr size_t kMaxRails = 8000;                 // per coin; the oldest ended rails go first
inline constexpr int64_t kKeepEndedMs = 7*24*3600000LL;   // the history the server serves
inline constexpr int64_t kHistoryMs = 24*3600000LL;       // one history request: a day
inline constexpr int kHistoryDays = 7;                    // asked for a day at a time while the view reaches back
inline constexpr size_t kMaxMarkets = 4;
inline constexpr int64_t kFreshMs = 180000;               // no message for longer: the census is stale (the server's gap)
inline constexpr int64_t kColumnMs = 60000;               // one census minute
inline constexpr double kKeep = 0.5;                      // a stretch drawn at the Minimum $ runs on down to half of it

// Presentation only. Candidates are built from visible, as-of-clipped stretches,
// never from future steps. The largest fifth per side (at most six) gets priority;
// equal dollars prefer the longer observed life. Nearby prices remain distinct:
// suppress overlap rather than moving a price or inventing an aggregate.
struct DisplayCandidate {
    int64_t id = 0;
    bool is_long = false;
    double usd = 0;
    int64_t observed_ms = 0;
    float y = 0;
};
inline std::set<int64_t> major_levels(std::vector<DisplayCandidate> candidates, float spacing = 14.0f) {
    std::erase_if(candidates, [](const auto& c) { return !std::isfinite(c.usd) || c.usd <= 0 || !std::isfinite(c.y); });
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.usd != b.usd) return a.usd > b.usd;
        if (a.observed_ms != b.observed_ms) return a.observed_ms > b.observed_ms;
        return a.id < b.id;
    });
    size_t counts[2]{};
    for (const auto& c : candidates) ++counts[c.is_long];
    const size_t budget[2] = {std::min<size_t>(6, (counts[0] + 4)/5), std::min<size_t>(6, (counts[1] + 4)/5)};
    size_t used[2]{};
    std::vector<float> ys;
    std::set<int64_t> selected;
    for (const auto& c : candidates) {
        if (used[c.is_long] >= budget[c.is_long]) continue;
        if (std::any_of(ys.begin(), ys.end(), [&](float y) { return std::abs(y - c.y) < spacing; })) continue;
        selected.insert(c.id);
        ys.push_back(c.y);
        ++used[c.is_long];
    }
    return selected;
}

// A rail's numbers from at_ms: the dollars of positions liquidating in its bin, by real leverage.
struct Step {
    int64_t at_ms = 0;
    double usd = 0, zone_lo = 0, zone_hi = 0;
    Mix lev{};
    int64_t positions = 0;
};

struct Rail {
    int64_t id = 0;
    bool is_long = false;
    double price = 0;         // Hyperliquid's price, fixed for life
    int64_t born_ms = 0;
    int64_t end_ms = 0;       // 0 while standing
    std::string end;          // touched | closed | gap; empty: ended while this terminal was away
    std::vector<Step> steps;  // ascending
};

// The census behind the newest live message, as the frame said it (never recomputed here: the census's own fixes move
// it).
struct Latest {
    int64_t timestamp_ms = 0;
    double mark = 0;
    std::string config_id;
    uint32_t census_version = 0;
    double usable_notional_usd = 0, coverage_denominator_usd = 0;
    uint32_t usable_positions = 0, sampled_positions = 0;
};

inline void add_step(Rail& r, const Step& s) {
    auto it = std::lower_bound(r.steps.begin(), r.steps.end(), s.at_ms, [](const Step& a, int64_t v) { return a.at_ms < v; });
    if (it != r.steps.end() && it->at_ms == s.at_ms) *it = s;
    else r.steps.insert(it, s);
}

// One coin's rails: the live messages and the stored records, merged by config and rail id.
class Store {
public:
    using Key = std::pair<std::string, int64_t>;  // config id, rail id

    // A live message: every standing rail at its current step, the ends since the previous message and the resumes. A
    // rail the store holds as standing that the message neither carries nor ends ended while this terminal was away
    // (a missed message): it ends at the message, reason unknown.
    void apply_live(const Latest& latest, const std::vector<Rail>& standing, const std::vector<Rail>& ended,
                    const std::vector<int64_t>& resumed) {
        if (latest.timestamp_ms < latest_.timestamp_ms) return;  // a late duplicate of an older message
        latest_ = latest;
        if (from_ms_ == 0 || latest.timestamp_ms < from_ms_) from_ms_ = latest.timestamp_ms;
        const std::string& cfg = latest.config_id;
        std::set<int64_t> carried;
        for (const auto& r : standing) {
            Rail& x = merge(cfg, r);
            if (x.end_ms > 0 && x.end_ms <= latest.timestamp_ms) {  // resumed, or an end this message overrides
                x.end_ms = 0;
                x.end.clear();
            }
            carried.insert(r.id);
        }
        for (const auto& e : ended) {
            auto it = rails_.find({cfg, e.id});
            if (it == rails_.end()) continue;
            it->second.end_ms = e.end_ms;
            it->second.end = e.end;
        }
        for (int64_t id : resumed) {
            auto it = rails_.find({cfg, id});
            if (it != rails_.end() && carried.contains(id)) {
                it->second.end_ms = 0;
                it->second.end.clear();
            }
        }
        for (auto& [key, r] : rails_)
            if (key.first == cfg && r.end_ms == 0 && r.born_ms <= latest.timestamp_ms && !carried.contains(key.second)) {
                r.end_ms = std::max(latest.timestamp_ms, r.born_ms + 1);
                r.end.clear();
            }
        bound();
        revision_ = ++revisions_;
    }

    // A stored record (history): the rail, every stored step, its end as stored. covered_from records a read's start,
    // so the store knows how far back it holds the record.
    // A stored end always holds (the server stores an end before it publishes one; a later resume comes live). A record
    // saying standing yields to the live messages once there are any: every message carries all standing rails, so a
    // reply read just before a message that ended the rail must not stand it up again.
    void apply_record(const std::string& config_id, const Rail& r) {
        Rail& x = merge(config_id, r);
        if (r.end_ms > 0) {
            x.end_ms = r.end_ms;
            x.end = r.end;
        } else if (latest_.timestamp_ms == 0) {
            x.end_ms = 0;
            x.end.clear();
        }
        bound();
        revision_ = ++revisions_;
    }
    void covered_from(int64_t start_ms) {
        if (start_ms > 0 && (from_ms_ == 0 || start_ms < from_ms_)) from_ms_ = start_ms;
    }

    const std::map<Key, Rail>& rails() const { return rails_; }
    const Latest& latest() const { return latest_; }
    // The earliest time the store holds the record from (a history read's start or the first live message); 0: none.
    int64_t from_ms() const { return from_ms_; }
    // Changes with every change to the store and is never reused by another store, so a store and its revision name
    // one state.
    uint64_t revision() const { return revision_; }

private:
    Rail& merge(const std::string& config_id, const Rail& r) {
        auto [it, fresh] = rails_.try_emplace({config_id, r.id});
        Rail& x = it->second;
        if (fresh) {
            x.id = r.id;
            x.is_long = r.is_long;
            x.price = r.price;
            x.born_ms = r.born_ms;
        }
        for (const auto& s : r.steps) add_step(x, s);
        return x;
    }
    // Drop rails ended a week before the newest message, then the oldest ended ones beyond the cap.
    void bound() {
        const int64_t cut = latest_.timestamp_ms - kKeepEndedMs;
        for (auto it = rails_.begin(); it != rails_.end();)
            it = it->second.end_ms > 0 && it->second.end_ms < cut ? rails_.erase(it) : std::next(it);
        while (rails_.size() > kMaxRails) {
            auto victim = rails_.end();
            for (auto it = rails_.begin(); it != rails_.end(); ++it)
                if (it->second.end_ms > 0 && (victim == rails_.end() || it->second.end_ms < victim->second.end_ms)) victim = it;
            if (victim == rails_.end()) break;  // every rail is standing: keep them
            rails_.erase(victim);
        }
    }
    std::map<Key, Rail> rails_;
    Latest latest_;
    int64_t from_ms_ = 0;
    uint64_t revision_ = 0;
    inline static uint64_t revisions_ = 0;  // main thread only, as every store
};

// The viewer's choice: the real leverage shown and the dollars a rail needs there to draw.
struct Filter {
    uint8_t groups = kAllGroups;
    double min_usd = 1000;
    bool operator==(const Filter&) const = default;
};

// A step's dollars within the chosen leverage, and those dollars by group. A step without leverage figures counts
// only while every group is chosen.
inline double chosen_usd(const Step& s, uint8_t groups, Mix& mix) {
    mix = {};
    double total = 0, usd = 0;
    for (double g : s.lev) total += std::max(0.0, g);
    if (!(total > 0)) return groups == kAllGroups ? s.usd : 0;
    for (int g = 0; g < kGroups; ++g) {
        if (!(groups & (1u << g))) continue;
        mix[g] = std::max(0.0, s.lev[g]);
        usd += mix[g];
    }
    return usd;
}

// What a drawn step holds beyond liqmap2_rails::Step: the leverage mix of its dollars and the positions behind them.
struct Detail {
    Mix mix{};
    int64_t positions = 0;
};

using Key = std::pair<std::string, int64_t>;  // {"hl", drawn id}, the key liqmap2_rails::segments reads

// What the overlay draws: liqmap2_rails rails (the V2 grammar), each a stretch of a server rail whose dollars in the
// chosen leverage reach the Minimum $ (and run on down to kKeep of it), with each step's detail. A stretch the filter
// cut ends with reason "filter"; one that runs to the server rail's end takes its end.
struct Book {
    std::map<Key, liqmap2_rails::Rail> rails;
    std::map<int64_t, std::vector<Detail>> detail;  // by drawn id, one per step
    uint64_t revision = 0;                          // changes with the rails
};

class View {
public:
    const Book& sync(const Store& store, const Filter& f) {
        if (built_ && store.revision() == store_revision_ && f == filter_) return book_;
        built_ = true;
        store_revision_ = store.revision();
        filter_ = f;
        const uint64_t revision = book_.revision + 1;
        book_ = Book{};
        book_.revision = revision;
        int64_t next = 1;
        for (const auto& [key, r] : store.rails()) {
            liqmap2_rails::Rail cur;
            std::vector<Detail> det;
            bool open = false;
            const auto close = [&](int64_t at, const std::string& why) {
                cur.end_ms = at;
                cur.end = why;
                book_.detail[cur.id] = std::move(det);
                book_.rails.emplace(Key{"hl", cur.id}, std::move(cur));
                cur = liqmap2_rails::Rail{};
                det.clear();
                open = false;
            };
            for (size_t i = 0; i < r.steps.size(); ++i) {
                const Step& s = r.steps[i];
                Mix mix;
                const double usd = chosen_usd(s, f.groups, mix);
                const bool in = open ? usd >= kKeep*f.min_usd && usd > 0 : usd >= f.min_usd && usd > 0;
                const int64_t at = i == 0 ? r.born_ms : std::max(r.born_ms, s.at_ms);
                if (!in) {
                    if (open) close(at, "filter");
                    continue;
                }
                if (!open) {
                    open = true;
                    cur.id = next++;
                    cur.is_long = r.is_long;
                    cur.price = r.price;
                    cur.born_ms = at;
                }
                cur.steps.push_back({at, usd, -1, -1, s.zone_lo, s.zone_hi, -1, -1, -1, -1});
                det.push_back({mix, s.positions});
            }
            if (open) close(r.end_ms, r.end_ms > 0 ? r.end : std::string());
        }
        // A stretch still standing is stored with end_ms 0 (close() was given the server rail's 0).
        return book_;
    }
    const Book& book() const { return book_; }

private:
    Book book_;
    Filter filter_;
    uint64_t store_revision_ = 0;
    bool built_ = false;
};

// The census state at the clock, from the newest live message (replay has none: the record is what was).
enum class State { missing, stale, legacy, unavailable, ready };
inline State state(const Latest* l, int64_t asof) {
    if (!l || l->timestamp_ms <= 0) return State::missing;
    if (asof - l->timestamp_ms > kFreshMs) return State::stale;
    if (l->census_version != 2) return State::legacy;
    if (!(l->usable_notional_usd > 0)) return State::unavailable;
    return State::ready;
}

// The chart venue's price over Hyperliquid's, which maps the rails onto another venue's chart. The chart once divided
// its live price by the newest message's mark every frame; the mark holds for a census minute while the live price
// moves with every trade, so every rail moved with price (James 2026-10-15: "moving up and down slightly"). Now one
// sample per message (the chart's price as the message arrives over its mark), averaged: a running mean over the
// first kBasisSamples, then an exponential mean of that length. A sample more than kBasisJump away starts over (another
// market, a bad mark): the basis between two perpetuals of one coin is a few hundredths of a percent.
inline constexpr int kBasisSamples = 15;
inline constexpr double kBasisJump = 0.01;
class Basis {
public:
    // ts: the message's clock (a sample per message); sample: the chart's price over the message's mark.
    void update(int64_t ts, double sample) {
        if (!(sample > 0) || ts == ts_) return;
        ts_ = ts;
        if (n_ == 0 || std::abs(sample/ratio_ - 1.0) > kBasisJump) {
            ratio_ = sample;
            n_ = 1;
            return;
        }
        n_ = std::min(n_ + 1, kBasisSamples);
        ratio_ += (sample - ratio_)/n_;
    }
    double ratio() const { return n_ ? ratio_ : 1.0; }  // 1 before any sample: Hyperliquid's own prices
    void reset() { *this = Basis{}; }

private:
    int64_t ts_ = 0;
    double ratio_ = 1.0;
    int n_ = 0;
};
}  // namespace hl_rails
