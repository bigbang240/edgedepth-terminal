#include "ui/chart_widget.h"
#include "core/display_time_zone.h"
#include "types/frame_profiler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>
#include <string>
#include <vector>

// Time Axis

// "Nice" tick intervals shared by the candle-TF chooser and the RT span cap.
static constexpr int64_t kNiceTickIntervals[] = {
    1, 2, 5, 10, 15, 20, 30, 60, 120, 180, 300, 600, 900, 1200, 1800, 2700,
    3600, 5400, 7200, 10800, 14400, 18000, 21600,
    28800, 36000, 43200, 64800, 86400, 172800,
    259200, 432000, 604800, 1209600, 2592000, 5184000, 7776000
};

void ChartWidget::setup_time_axis_ticks(double visible_x_min, double visible_x_max) const {
    generate_time_ticks(visible_x_min, visible_x_max);
}

void ChartWidget::generate_time_ticks(double visible_x_min, double visible_x_max) const {
    ProfileScope _ps("TimeTicks");
    // Cached tick set (FPS item, 2026-07-05). The label SET only changes when
    // the first visible tick / interval / count changes - under live-follow at
    // 160fps that's once per interval crossing, not per frame. Cache key holds
    // every format-affecting input (the audit's "zoom-dependent format"
    // concern: interval + count capture zoom; chart_type + tf capture mode).
    // Storage is static and only mutated on a key change, then ptrs are built
    // in a SECOND pass (emplace-as-you-go dangled SSO c_str()s on realloc).
    // ImPlot::SetupAxisTicks copies labels into the frame's Ticker each call,
    // so it still runs every frame - browser-Intl formatting is skipped.
    static std::vector<double> tick_positions;
    static std::vector<std::string> tick_labels_storage;
    static std::vector<const char*> tick_labels_ptrs;
    static int64_t ck_first = -1, ck_interval = -1, ck_tf = -1;
    static uint64_t ck_timezone_generation = 0;
    static int     ck_count = -1, ck_chart_type = -1;

    // Setup is not locked yet: GetPlotSize would consume input and prohibit
    // SetupAxisTicks. Reserve a conservative price gutter from the current
    // content width, so resizing does not reuse the previous frame's width.
    const float axis_width = std::max(1.0f, ImGui::GetContentRegionAvail().x -
        ImGui::CalcTextSize("00000000.00000000").x - 20.0f);
    const auto submit = [&]() {
        if (!tick_positions.empty()) {
            // Custom ImPlot labels are not collision-culled. Check actual
            // text widths, including date changes and the active font scale.
            static std::vector<double> positions;
            static std::vector<const char*> labels;
            positions.clear();
            labels.clear();
            float last_right = -10000.0f;
            for (size_t i = 0; i < tick_positions.size(); ++i) {
                const float x = static_cast<float>((tick_positions[i] - visible_x_min) /
                    std::max(1.0, visible_x_max - visible_x_min)) * axis_width;
                const float half_width = ImGui::CalcTextSize(tick_labels_ptrs[i]).x * 0.5f;
                if (x - half_width < last_right + 16.0f) continue;
                positions.push_back(tick_positions[i]);
                labels.push_back(tick_labels_ptrs[i]);
                last_right = x + half_width;
            }
            ImPlot::SetupAxisTicks(ImAxis_X1,
                positions.data(),
                static_cast<int>(positions.size()),
                labels.data(),
                false);
        }
    };

    // ── Resolve (first_tick, interval, count) arithmetically - no tm calls ──
    int64_t interval_s, first_tick_s;
    int64_t tf_sec = 0;
    if (chart_type_ == ChartType::TPO) {
        // TPO mode: daily ticks only, date labels (no intraday times)
        interval_s = 86400;
        const auto start_s = static_cast<int64_t>(visible_x_min / 1000.0);
        const int64_t aligned = (start_s / interval_s) * interval_s;
        first_tick_s = aligned;
        while (first_tick_s * 1000.0 < visible_x_min) first_tick_s += interval_s;
    } else {
        tf_sec = candles().timeframe_seconds();
        const double visible_span = visible_x_max - visible_x_min;
        const double visible_candles = visible_span / (static_cast<double>(tf_sec) * 1000.0);
        interval_s = calculate_tick_interval(visible_candles);
        // RT: the x axis is wall-clock ms, so a candle-TF-derived interval can
        // exceed the whole live window at narrow widths — zero aligned ticks
        // then let ImPlot fall back to its raw scientific labels ("e+12").
        // Cap the interval at the span so at least one boundary always lands
        // inside the window (any window of length >= interval has one).
        if (rt_mode_) {
            const double span_s = std::max(1.0, (visible_x_max - visible_x_min) / 1000.0);
            if (static_cast<double>(interval_s) > span_s) {
                interval_s = 1;
                for (const int64_t it : kNiceTickIntervals) {
                    if (static_cast<double>(it) > span_s) break;
                    interval_s = it;
                }
            }
        }
        const auto start_s = static_cast<int64_t>(visible_x_min / 1000.0);
        first_tick_s = align_to_interval(start_s, interval_s);
        while (first_tick_s * 1000.0 < visible_x_min) first_tick_s += interval_s;
    }
    const auto end_s = static_cast<int64_t>(visible_x_max / 1000.0);
    int count = 0;
    if (first_tick_s <= end_s) {
        count = static_cast<int>((end_s - first_tick_s) / interval_s) + 1;
        // Honor the exact original bound (tick_ms <= visible_x_max)
        while (count > 0 &&
               (first_tick_s + static_cast<int64_t>(count - 1) * interval_s) * 1000.0 > visible_x_max) {
            count--;
        }
    }

    // ── Cache hit → resubmit stored arrays, skip regeneration ──
    if (first_tick_s == ck_first && interval_s == ck_interval && count == ck_count &&
        static_cast<int>(chart_type_) == ck_chart_type && tf_sec == ck_tf &&
        ck_timezone_generation == DisplayTimeZone::instance().generation() &&
        static_cast<int>(tick_positions.size()) == count) {
        submit();
        return;
    }
    ck_first = first_tick_s; ck_interval = interval_s; ck_count = count;
    ck_chart_type = static_cast<int>(chart_type_); ck_tf = tf_sec;
    ck_timezone_generation = DisplayTimeZone::instance().generation();

    tick_positions.clear();
    tick_labels_storage.clear();
    tick_labels_ptrs.clear();
    if (count <= 0) return;
    tick_positions.reserve(static_cast<size_t>(count));
    tick_labels_storage.reserve(static_cast<size_t>(count));
    tick_labels_ptrs.reserve(static_cast<size_t>(count));

    if (chart_type_ == ChartType::TPO) {
        for (int i = 0; i < count; i++) {
            const int64_t tick_s = first_tick_s + static_cast<int64_t>(i) * interval_s;
            tick_positions.push_back(static_cast<double>(tick_s) * 1000.0);
            char label[32] = "--";
            DisplayTimeZone::instance().format(tick_s * 1000,
                TimeZoneFormat::WeekdayMonthDay, label, sizeof(label));
            tick_labels_storage.emplace_back(label);
        }
    } else {
        char prev_day[16]{};
        for (int i = 0; i < count; i++) {
            const int64_t tick_s = first_tick_s + static_cast<int64_t>(i) * interval_s;
            tick_positions.push_back(static_cast<double>(tick_s) * 1000.0);

            char label[32] = "--";
            char day[16]{};
            DisplayTimeZone::instance().format(tick_s * 1000, TimeZoneFormat::DateOnly,
                                                day, sizeof(day));

            const bool day_changed = prev_day[0] && std::strcmp(day, prev_day) != 0;

            if (day_changed || !prev_day[0]) {
                DisplayTimeZone::instance().format(tick_s * 1000, TimeZoneFormat::MonthDay,
                                                    label, sizeof(label));
            } else if (interval_s < 60) {
                DisplayTimeZone::instance().format(tick_s * 1000, TimeZoneFormat::TimeSeconds,
                                                    label, sizeof(label));
            } else {
                DisplayTimeZone::instance().format(tick_s * 1000, TimeZoneFormat::TimeMinutes,
                                                    label, sizeof(label));
            }

            std::snprintf(prev_day, sizeof(prev_day), "%s", day);
            tick_labels_storage.emplace_back(label);
        }
    }
    // Second pass: storage is final → c_str() pointers are stable.
    for (const auto& s : tick_labels_storage) tick_labels_ptrs.push_back(s.c_str());

    submit();
}

int64_t ChartWidget::calculate_tick_interval(double visible_candles) const {
    const int64_t tf_sec = candles().timeframe_seconds();
    const double visible_span_seconds = visible_candles * static_cast<double>(tf_sec);
    // static constexpr - the old std::vector heap-allocated EVERY FRAME.
    static constexpr int64_t nice_intervals[] = {
        1, 2, 5, 10, 15, 20, 30, 60, 120, 180, 300, 600, 900, 1200, 1800, 2700,
        3600, 5400, 7200, 10800, 14400, 18000, 21600,
        28800, 36000, 43200, 64800, 86400, 172800,
        259200, 432000, 604800, 1209600, 2592000, 5184000, 7776000
    };
    static_assert(std::size(nice_intervals) == std::size(kNiceTickIntervals),
                  "keep the two interval tables in sync");
    const float axis_width = std::max(1.0f, ImGui::GetContentRegionAvail().x -
        ImGui::CalcTextSize("00000000.00000000").x - 20.0f);
    const float label_pitch = ImGui::CalcTextSize("23:59:59").x + 16.0f;
    const double min_interval = visible_span_seconds * label_pitch / axis_width;
    for (const int64_t interval : nice_intervals)
        if (static_cast<double>(interval) >= min_interval) return interval;
    return nice_intervals[std::size(nice_intervals) - 1];
}

int64_t ChartWidget::align_to_interval(int64_t timestamp_s, int64_t interval_seconds) const {
    const auto start_time_t = static_cast<time_t>(timestamp_s);
    // Label tick positions remain UTC-aligned so a display preference never moves
    // a candle or marker. Only their rendered wall-clock labels are converted.
    tm* start_tm = gmtime(&start_time_t);

    if (interval_seconds == 30) {
        start_tm->tm_sec = (start_tm->tm_sec / 30) * 30;
        return timegm(start_tm);
    } else if (interval_seconds == 60) {
        start_tm->tm_sec = 0;
        return timegm(start_tm);
    } else if (interval_seconds <= 1200 && interval_seconds % 60 == 0) {
        // 2m, 3m, 5m, 10m, 15m, 20m
        const int mins = static_cast<int>(interval_seconds / 60);
        start_tm->tm_min = (start_tm->tm_min / mins) * mins;
        start_tm->tm_sec = 0;
        return timegm(start_tm);
    } else if (interval_seconds == 1800) {
        start_tm->tm_min = (start_tm->tm_min / 30) * 30;
        start_tm->tm_sec = 0;
        return timegm(start_tm);
    } else if (interval_seconds == 2700) {
        start_tm->tm_min = (start_tm->tm_min / 45) * 45;
        start_tm->tm_sec = 0;
        return timegm(start_tm);
    } else if (interval_seconds == 3600) {
        start_tm->tm_min = 0;
        start_tm->tm_sec = 0;
        return timegm(start_tm);
    } else if (interval_seconds == 5400) {
        const int total_minutes = start_tm->tm_hour * 60 + start_tm->tm_min;
        const int rounded_minutes = (total_minutes / 90) * 90;
        start_tm->tm_hour = rounded_minutes / 60;
        start_tm->tm_min = rounded_minutes % 60;
        start_tm->tm_sec = 0;
        return timegm(start_tm);
    } else if (interval_seconds <= 43200 && interval_seconds % 3600 == 0) {
        // 2h, 3h, 4h, 5h, 6h, 8h, 10h, 12h
        const int hours = static_cast<int>(interval_seconds / 3600);
        start_tm->tm_hour = (start_tm->tm_hour / hours) * hours;
        start_tm->tm_min = 0;
        start_tm->tm_sec = 0;
        return timegm(start_tm);
    } else if (interval_seconds == 86400) {
        start_tm->tm_hour = 0;
        start_tm->tm_min = 0;
        start_tm->tm_sec = 0;
        return timegm(start_tm);
    } else if (interval_seconds % 86400 == 0) {
        const int day_interval = static_cast<int>(interval_seconds / 86400);
        start_tm->tm_mday = ((start_tm->tm_mday - 1) / day_interval) * day_interval + 1;
        start_tm->tm_hour = 0;
        start_tm->tm_min = 0;
        start_tm->tm_sec = 0;
        return timegm(start_tm);
    } else {
        return (timestamp_s / interval_seconds) * interval_seconds;
    }
}
