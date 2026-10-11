#pragma once
// Appearance preferences: the chrome preset, interface accent, market colors and
// liquidation colormap a browser chose, saved in localStorage as
// edgedepth.appearance.v2. Pure (JSON in, values out) so the v1 -> v2 migration
// has a native test; Theme::load_preferences reads the storage and applies the
// result, Theme::save_preferences writes to_json.
//
// v1 {version, accent, candles, up, down} came before chrome presets and did not
// save the colormap. A browser with only v1 keeps those choices on the EdgeDepth
// chrome and is written as v2 once; v1 is left in place for an older build.
// Every enum is saved as an int, so its values are append-only.
#include "core/workspace_settings.h"
#include <cstdint>

namespace appearance {

struct Preferences {
    int chrome = 0;     // Theme::Chrome
    int accent = 3;     // Theme::Accent (Mono)
    int candles = 0;    // Theme::CandleConvention
    uint32_t up = 0x2fd6ad, down = 0xee5c78;  // Custom market colors, 0xRRGGBB
    int liq_map = 0;    // HeatmapColormap::LiqMap
};

// The highest value each saved enum may take in this build.
struct Limits {
    int chrome, accent, candles, liq_map;
};

enum class Source { None, V1, V2 };

// Reads v2 when it is an object with version 2, else v1 with version 1 (chrome and
// colormap keep their current values), else nothing. Wrong types and out-of-range
// values keep the current ones. Source::V1 means the caller should save v2 once.
inline Source read(const workspace::Json& v2, const workspace::Json& v1, const Limits& limits, Preferences& p) {
    const workspace::Json* j = &v2;
    int version = 0;
    if (v2.is_object()) workspace::read(v2, "version", version, 2, 2);
    if (version != 2) {
        j = &v1;
        if (v1.is_object()) workspace::read(v1, "version", version, 1, 1);
        if (version != 1) return Source::None;
    }
    if (version == 2) workspace::read(*j, "chrome", p.chrome, 0, limits.chrome);
    workspace::read(*j, "accent", p.accent, 0, limits.accent);
    workspace::read(*j, "candles", p.candles, 0, limits.candles);
    workspace::read(*j, "up", p.up, 0, 0xffffff);
    workspace::read(*j, "down", p.down, 0, 0xffffff);
    if (version == 2) workspace::read(*j, "liq_map", p.liq_map, 0, limits.liq_map);
    return version == 2 ? Source::V2 : Source::V1;
}

inline workspace::Json to_json(const Preferences& p) {
    return workspace::Json({{"version", 2}, {"chrome", p.chrome}, {"accent", p.accent}, {"candles", p.candles},
                            {"up", p.up}, {"down", p.down}, {"liq_map", p.liq_map}});
}

}  // namespace appearance
