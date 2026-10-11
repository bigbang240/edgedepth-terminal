// Appearance preferences (core/appearance_preferences.h): v2 is read as saved, a
// browser with only v1 keeps its market colors and accent on the EdgeDepth chrome
// and is flagged for one v2 write, and bad values never apply.
#include "core/appearance_preferences.h"
#include <cstdio>

using workspace::Json;

namespace {

int failures = 0;

void expect_true(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

void expect_int(long long got, long long want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got %lld, want %lld\n", what, got, want);
        ++failures;
    }
}

// This build's highest saved values: High contrast, Neutral accent, Blue / orange, Inferno.
constexpr appearance::Limits kLimits{3, 3, 5, 3};

void test_nothing_saved_keeps_the_defaults() {
    appearance::Preferences p;
    expect_true(appearance::read(Json{}, Json{}, kLimits, p) == appearance::Source::None, "no storage reads nothing");
    expect_int(p.chrome, 0, "default chrome is EdgeDepth");
    expect_int(p.accent, 3, "default accent is Neutral");
    expect_int(p.candles, 0, "default market colors are Teal / rose");
    expect_int(p.liq_map, 0, "default colormap is Ember");
}

void test_v1_migrates_on_the_edgedepth_chrome() {
    // The previous build's save: Blue / white, Mint accent, custom colors kept for Custom.
    const Json v1 = {{"version", 1}, {"accent", 0}, {"candles", 3}, {"up", 0x123456}, {"down", 0xabcdef}};
    appearance::Preferences p;
    expect_true(appearance::read(Json{}, v1, kLimits, p) == appearance::Source::V1, "v1 alone is a migration");
    expect_int(p.chrome, 0, "v1 has no chrome: EdgeDepth");
    expect_int(p.accent, 0, "v1 accent kept");
    expect_int(p.candles, 3, "v1 market colors kept");
    expect_int(p.up, 0x123456, "v1 custom up kept");
    expect_int(p.down, 0xabcdef, "v1 custom down kept");
    expect_int(p.liq_map, 0, "v1 did not save the colormap");

    // The v2 written once from it, which a reload then reads as v2.
    const Json v2 = appearance::to_json(p);
    expect_true(v2 == Json({{"version", 2}, {"chrome", 0}, {"accent", 0}, {"candles", 3}, {"up", 0x123456},
                            {"down", 0xabcdef}, {"liq_map", 0}}), "v2 written from v1");
    appearance::Preferences q;
    expect_true(appearance::read(v2, v1, kLimits, q) == appearance::Source::V2, "v2 wins over v1");
    expect_int(q.candles, 3, "v2 round trip keeps market colors");
}

void test_v2_reads_every_choice() {
    const Json v2 = {{"version", 2}, {"chrome", 2}, {"accent", 1}, {"candles", 5}, {"up", 1}, {"down", 2},
                     {"liq_map", 1}};
    const Json v1 = {{"version", 1}, {"accent", 0}, {"candles", 3}, {"up", 3}, {"down", 4}};
    appearance::Preferences p;
    expect_true(appearance::read(v2, v1, kLimits, p) == appearance::Source::V2, "v2 read");
    expect_int(p.chrome, 2, "Midnight");
    expect_int(p.accent, 1, "Indigo");
    expect_int(p.candles, 5, "Blue / orange");
    expect_int(p.up, 1, "custom up");
    expect_int(p.down, 2, "custom down");
    expect_int(p.liq_map, 1, "Viridis");
}

void test_bad_values_never_apply() {
    // Out of range for this build, wrong types and fractions keep the current value.
    const Json v2 = {{"version", 2}, {"chrome", 9}, {"accent", "1"}, {"candles", 2.5}, {"up", 0x1000000},
                     {"down", -1}, {"liq_map", 4}};
    appearance::Preferences p;
    p.chrome = 1; p.accent = 2; p.candles = 1; p.up = 7; p.down = 8; p.liq_map = 2;
    expect_true(appearance::read(v2, Json{}, kLimits, p) == appearance::Source::V2, "v2 with bad values still read");
    expect_int(p.chrome, 1, "unknown chrome ignored");
    expect_int(p.accent, 2, "string accent ignored");
    expect_int(p.candles, 1, "fractional palette ignored");
    expect_int(p.up, 7, "up beyond 0xffffff ignored");
    expect_int(p.down, 8, "negative down ignored");
    expect_int(p.liq_map, 2, "unknown colormap ignored");

    // A wrong or missing version is not this format: nothing is read.
    appearance::Preferences q;
    expect_true(appearance::read(Json{{"version", 3}, {"chrome", 2}}, Json{{"version", 2}, {"accent", 0}}, kLimits, q) ==
                    appearance::Source::None, "unknown versions are ignored");
    expect_int(q.chrome, 0, "unknown v2 version leaves chrome alone");
    expect_int(q.accent, 3, "v1 slot holding version 2 leaves accent alone");
    expect_true(appearance::read(Json::array({1, 2}), Json("text"), kLimits, q) == appearance::Source::None,
                "non-objects are ignored");
}

}  // namespace

int main() {
    test_nothing_saved_keeps_the_defaults();
    test_v1_migrates_on_the_edgedepth_chrome();
    test_v2_reads_every_choice();
    test_bad_values_never_apply();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("appearance_preferences_test: all assertions passed\n");
    return 0;
}
