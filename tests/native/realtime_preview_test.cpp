// Free real-time preview budget (core/realtime_preview.h): 3 visible minutes per
// UTC day, shared across markets, never billing a backgrounded tab's gap.
#include "core/realtime_preview.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int failures = 0;

void expect_eq(int64_t got, int64_t want, const char* what) {
    if (got != want) {
        std::printf("FAIL %s: got %lld want %lld\n", what, static_cast<long long>(got), static_cast<long long>(want));
        ++failures;
    }
}

void expect_str(const std::string& got, const std::string& want, const char* what) {
    if (got != want) {
        std::printf("FAIL %s: got '%s' want '%s'\n", what, got.c_str(), want.c_str());
        ++failures;
    }
}

void test_a_new_day_has_the_full_budget() {
    using namespace rt_preview;
    expect_eq(remaining_ms(Budget{}, "2026-10-11"), kDailyMs, "nothing stored");
    expect_eq(remaining_ms(Budget{"2026-10-10", kDailyMs}, "2026-10-11"), kDailyMs, "yesterday spent, today full");
    expect_eq(remaining_ms(Budget{"2026-10-11", 60'000}, "2026-10-11"), 120'000, "a minute used");
    expect_eq(remaining_ms(Budget{"2026-10-11", 999'999}, "2026-10-11"), 0, "never negative");
}

void test_spending_bills_visible_frames_only() {
    using namespace rt_preview;
    Budget b = spend(Budget{}, "2026-10-11", 0.016);
    expect_eq(b.used_ms, 16, "one frame");
    // A tab returning from the background reports one long frame: cap it.
    b = spend(b, "2026-10-11", 600.0);
    expect_eq(b.used_ms, 16 + static_cast<int64_t>(kMaxFrameSeconds * 1000), "backgrounded gap is not billed");
    b = spend(Budget{"2026-10-11", kDailyMs - 5}, "2026-10-11", 0.1);
    expect_eq(b.used_ms, kDailyMs, "stops at the budget");
    b = spend(Budget{"2026-10-10", kDailyMs}, "2026-10-11", 0.05);
    expect_str(b.day, "2026-10-11", "a new day starts a new record");
    expect_eq(b.used_ms, 50, "and bills from zero");
}

void test_day_and_label() {
    using namespace rt_preview;
    expect_str(utc_day(1791633600000LL), "2026-10-10", "noon UTC on the 10th");
    expect_str(utc_day(1791676799000LL), "2026-10-10", "last second of the UTC day");
    expect_str(utc_day(1791676800000LL), "2026-10-11", "midnight UTC rolls the day");
    expect_str(format_left(kDailyMs), "3:00", "full");
    expect_str(format_left(161'000), "2:41", "minutes and seconds");
    expect_str(format_left(400), "0:01", "rounds up, never shows 0:00 while time remains");
    expect_str(format_left(0), "0:00", "spent");
}

}  // namespace

int main() {
    test_a_new_day_has_the_full_budget();
    test_spending_bills_visible_frames_only();
    test_day_and_label();
    if (failures) return EXIT_FAILURE;
    std::printf("realtime_preview_test: ok\n");
    return EXIT_SUCCESS;
}
