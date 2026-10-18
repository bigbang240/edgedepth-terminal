#pragma once
// Cumulative traded base volume (buys minus sells) across a visible time range,
// zero at its left edge. Pure logic for the real-time CVD lane: one bounded
// column per pixel, rebuilt from the sorted trade records the chart displays.
#include "types/types.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

class RealtimeCvdSeries {
public:
    // Columns per series; one per plot pixel at most.
    static constexpr int kMaxColumns = 4096;
    struct Column { double value = 0; bool valid = false; };

    // Trades before `from` are excluded (CVD starts at 0 at the left edge), as
    // are trades after `clock` (the display's as-of time). `trades` is sorted by time.
    template <class Trades>
    void build(const Trades& trades, double from, double to, int64_t clock, int columns) {
        count_ = std::clamp(columns, 1, kMaxColumns);
        from_ = from; to_ = std::max(to, from + 1.0);
        width_ = (to_ - from_) / count_;
        low_ = high_ = end_value_ = 0; last_valid_ = -1;
        const double end = std::min(to_, double(clock));
        auto it = std::lower_bound(trades.begin(), trades.end(), from,
            [](const Terminal::Trade& t, double ms) { return double(t.timestamp_ms) < ms; });
        double cumulative = 0;
        for (int c = 0; c < count_; ++c) {
            const double start = from_ + c * width_;
            const double limit = std::min(from_ + (c + 1) * width_, end);
            for (; it != trades.end() && double(it->timestamp_ms) <= limit; ++it)
                if (it->qty > 0 && std::isfinite(it->qty)) cumulative += it->is_buy ? it->qty : -it->qty;
            const bool valid = start < end;
            columns_[size_t(c)] = {cumulative, valid};
            if (!valid) continue;
            low_ = std::min(low_, cumulative); high_ = std::max(high_, cumulative);
            end_value_ = cumulative; last_valid_ = c;
        }
    }

    int columns() const { return count_; }
    const Column& column(int c) const { return columns_[size_t(std::clamp(c, 0, count_ - 1))]; }
    // Right edge of column c in chart time.
    double column_end(int c) const { return from_ + (c + 1) * width_; }
    // The cumulative value at time t (its column's), or false outside the valid series.
    bool value_at(double t, double& value) const {
        if (last_valid_ < 0 || t < from_) return false;
        const int c = std::min(count_ - 1, int((t - from_) / width_));
        if (!columns_[size_t(c)].valid) return false;
        value = columns_[size_t(c)].value;
        return true;
    }
    bool empty() const { return last_valid_ < 0; }
    int last_valid() const { return last_valid_; }
    double end_value() const { return end_value_; }
    double low() const { return low_; }    // never above zero: the series starts there
    double high() const { return high_; }  // never below zero

private:
    std::array<Column, kMaxColumns> columns_{};
    int count_ = 0, last_valid_ = -1;
    double from_ = 0, to_ = 1, width_ = 1;
    double low_ = 0, high_ = 0, end_value_ = 0;
};
