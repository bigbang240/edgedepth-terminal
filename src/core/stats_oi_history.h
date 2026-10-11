#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

// Contract quantity only: USD mark-price movement must not become OI growth.
// One latest observation per minute bounds memory; keep an extra anchor beyond 4h.
class StatsOIHistory {
    struct Sample { int64_t ms; double qty; };
    std::deque<Sample> samples_;
public:
    bool add(int64_t ms, double qty, int64_t now) {
        if (ms <= 0 || ms > now || now - ms > 4*3600000LL + 300000 || !std::isfinite(qty) || qty <= 0) return false;
        auto it = std::lower_bound(samples_.begin(), samples_.end(), ms / 60000,
            [](const Sample& s, int64_t bucket) { return s.ms / 60000 < bucket; });
        if (it != samples_.end() && it->ms / 60000 == ms / 60000) {
            if (ms < it->ms) return false;
            *it = {ms, qty};
        } else samples_.insert(it, {ms, qty});
        const auto cutoff = samples_.back().ms - 4*3600000LL - 120000;
        while (samples_.size() > 1 && samples_[1].ms < cutoff) samples_.pop_front();
        while (samples_.size() > 248) samples_.pop_front();
        return true;
    }
    // Only complete historical minutes. Their close can be up to a minute old;
    // never let the current partial DB bucket masquerade as a live observation.
    bool seed(int64_t bucket_ms, double close, int64_t now) {
        if (bucket_ms <= 0 || bucket_ms > now - 60000 || bucket_ms % 60000) return false;
        return add(bucket_ms + 59999, close, now);
    }
    bool change(int64_t window_ms, int64_t now, double& fraction, bool velocity = false) const {
        if (samples_.size() < 2 || window_ms <= 0) return false;
        const auto& latest = samples_.back();
        if (latest.ms > now || now - latest.ms > 120000) return false;
        const int64_t target = latest.ms - window_ms;
        const Sample* ref = nullptr;
        for (const auto& s : samples_) { if (s.ms <= target) ref = &s; else break; }
        if (!ref || target - ref->ms > 90000) return false; // do not bridge a missing anchor
        fraction = latest.qty / ref->qty - 1;
        if (velocity) fraction *= 60000.0 / (latest.ms - ref->ms);
        return true;
    }
};
