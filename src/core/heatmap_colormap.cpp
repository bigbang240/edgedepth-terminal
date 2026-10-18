#include "heatmap_colormap.h"
#include "imgui.h"
#include <cmath>
#include <algorithm>

namespace HeatmapColormap {

namespace {
    // Default = Ember-K (2026-07-02 redesign): a hand ramp with a longer, deeper
    // black→violet toe than Inferno (sub-median fuel melts into the background) and a compressed,
    // rarer orange→yellow ignition top (top ~5% of the ramp). Values are the EXACT stop table from
    // the design system's liq-heatmap-colormap.json (`stops.ember`); the 256-entry `luts.ember` in that
    // file is generated from these stops by the same linear RGB interpolation eval_stops performs.
    // Inferno/Magma/Viridis stay user-switchable live in the Tweaks panel (set_liq_map).
    LiqMap   g_liq_map = LiqMap::Ember;
    // [SELF-HOST P1] depth ramp selection + brightness curve (see header).
    DepthMap g_depth_map = DepthMap::EdgeDepth;
    int      g_depth_gamma_pct = 100;
    uint32_t g_generation = 0;

    struct Stop { float t; uint8_t r, g, b; };

    // Ember-K - hand ramp (15 stops, liq-heatmap-colormap.json stops.ember)
    constexpr Stop EMBER_STOPS[] = {
        {0.000f,   0,   0,   0}, {0.060f,   6,   4,  15}, {0.140f,  14,   9,  34},
        {0.240f,  27,  13,  59}, {0.350f,  45,  17,  84}, {0.460f,  68,  22, 103},
        {0.570f,  95,  28, 110}, {0.670f, 126,  36, 106}, {0.760f, 160,  47,  92},
        {0.840f, 196,  62,  70}, {0.900f, 227,  84,  44}, {0.945f, 246, 114,  20},
        {0.975f, 252, 158,  28}, {0.992f, 253, 201,  62}, {1.000f, 252, 235, 140}};

    // matplotlib viridis - stops match the design system's tokens.json
    constexpr Stop VIRIDIS_STOPS[] = {
        {0.00f,  68,   1,  84}, {0.20f,  65,  68, 135}, {0.40f,  42, 120, 142},
        {0.60f,  34, 168, 132}, {0.80f, 122, 209,  81}, {1.00f, 253, 231,  37}};

    // matplotlib magma
    constexpr Stop MAGMA_STOPS[] = {
        {0.00f,   0,   0,   4}, {0.25f,  81,  18, 124}, {0.50f, 183,  55, 121},
        {0.75f, 252, 137,  97}, {1.00f, 252, 253, 191}};

    template <size_t N>
    void eval_stops(const Stop (&stops)[N], float t,
                    uint8_t& r, uint8_t& g, uint8_t& b) {
        if (t <= stops[0].t) { r = stops[0].r; g = stops[0].g; b = stops[0].b; return; }
        for (size_t i = 1; i < N; ++i) {
            if (t <= stops[i].t) {
                const float s = (t - stops[i - 1].t) / (stops[i].t - stops[i - 1].t);
                r = static_cast<uint8_t>(stops[i - 1].r + s * (stops[i].r - stops[i - 1].r));
                g = static_cast<uint8_t>(stops[i - 1].g + s * (stops[i].g - stops[i - 1].g));
                b = static_cast<uint8_t>(stops[i - 1].b + s * (stops[i].b - stops[i - 1].b));
                return;
            }
        }
        r = stops[N - 1].r; g = stops[N - 1].g; b = stops[N - 1].b;
    }

    // ── [SELF-HOST P1] depth palette stop tables ────────────────────────
    // EdgeDepth keeps the shipped behavior (in apply()), so only the
    // alternates need stop tables. Teal-Gold is a punchier sibling of the
    // shipped ramp with brighter mids; Blue-Red separates resting walls
    // (cool) from eaten-through levels (hot); Mono-Ice is a minimal
    // single-hue ramp for screenshots; Viridis shares the liq table.
    constexpr Stop TEAL_GOLD_STOPS[] = {
        {0.000f,   5,  10,  14}, {0.100f,   8,  32,  40}, {0.250f,  10,  70,  82},
        {0.450f,  22, 140, 150}, {0.620f, 120, 200, 120}, {0.780f, 235, 205,  80},
        {0.900f, 250, 160,  60}, {1.000f, 255, 245, 210}};

    constexpr Stop BLUE_RED_STOPS[] = {
        {0.000f,   5,   8,  16}, {0.200f,  15,  35,  90}, {0.450f,  30,  90, 200},
        {0.620f, 130,  60, 190}, {0.780f, 220,  60,  90}, {0.900f, 250, 140,  40},
        {1.000f, 255, 230, 150}};

    constexpr Stop MONO_ICE_STOPS[] = {
        {0.000f,   6,   9,  13}, {0.300f,  26,  42,  58}, {0.550f,  70, 105, 135},
        {0.780f, 150, 190, 215}, {1.000f, 240, 250, 255}};

    // Route one of the depth ramp types through the viewer's DepthMap pick.
    // EdgeDepth falls through to the shipped hand ramps in apply(); every
    // other pick evaluates its stop table here. RealtimeWarm shares the same
    // ramp by design - with an explicit palette picked, the cool/warm split
    // would fight it.
    bool eval_depth_stops(const Type type, float t, uint8_t& r, uint8_t& g, uint8_t& b) {
        switch (g_depth_map) {
            case DepthMap::TealGold: eval_stops(TEAL_GOLD_STOPS, t, r, g, b); return true;
            case DepthMap::BlueRed:  eval_stops(BLUE_RED_STOPS,  t, r, g, b); return true;
            case DepthMap::MonoIce:  eval_stops(MONO_ICE_STOPS,  t, r, g, b); return true;
            case DepthMap::Viridis:  eval_stops(VIRIDIS_STOPS,   t, r, g, b); return true;
            case DepthMap::Inferno: {
                // matplotlib inferno as stops (close to the liq hand ramp's
                // shape; fine for the depth context).
                static constexpr Stop INFERNO_STOPS[] = {
                    {0.000f,   0,   0,   4}, {0.150f,  30,   9,  68}, {0.300f,  85,  16, 109},
                    {0.450f, 136,  34, 106}, {0.600f, 186,  54,  85}, {0.720f, 227,  89,  51},
                    {0.840f, 249, 140,  10}, {0.930f, 252, 199,  44}, {1.000f, 252, 255, 164}};
                eval_stops(INFERNO_STOPS, t, r, g, b);
                return true;
            }
            case DepthMap::EdgeDepth: break;
        }
        (void)type;
        return false;
    }

    // Brightness curve: pct=100 is neutral; >100 lifts mids, <100 darkens.
    float depth_t(float t) {
        if (g_depth_gamma_pct == 100) return t;
        return std::pow(std::clamp(t, 0.0f, 1.0f), 100.0f / float(g_depth_gamma_pct));
    }
}

LiqMap liq_map() { return g_liq_map; }
void set_liq_map(LiqMap m) {
    if (m == g_liq_map) return;
    g_liq_map = m;
    ++g_generation;
}

// ── [SELF-HOST P1] depth palette accessors ──────────────────────────
DepthMap depth_map() { return g_depth_map; }
void set_depth_map(const DepthMap m) {
    if (m == g_depth_map) return;
    g_depth_map = m;
    ++g_generation;
}
int depth_gamma_pct() { return g_depth_gamma_pct; }
void set_depth_gamma_pct(const int pct) {
    const int clamped = std::clamp(pct, 50, 250);
    if (clamped == g_depth_gamma_pct) return;
    g_depth_gamma_pct = clamped;
    ++g_generation;
}
uint32_t generation() { return g_generation; }

void apply(Type type, float t, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (type == Type::Liquidation || type == Type::LiquidationWarm) {
        if (g_liq_map == LiqMap::Ember)   { eval_stops(EMBER_STOPS, t, r, g, b); return; }
        if (g_liq_map == LiqMap::Viridis) { eval_stops(VIRIDIS_STOPS, t, r, g, b); return; }
        if (g_liq_map == LiqMap::Magma)   { eval_stops(MAGMA_STOPS, t, r, g, b); return; }
        // LiqMap::Inferno - hand-tuned V7 ramp below.
        // V7 Inferno colormap: black → indigo → purple → red → orange → yellow → white
        // Matches matplotlib's inferno perceptual colormap for "heat cloud" look.
        // Single ramp for both long (below mark) and short (above mark) liquidations.
        // Direction conveyed by position relative to mark price, not by color.
        if (t < 0.05f) {
            float s = t / 0.05f;
            r = static_cast<uint8_t>(s * 3);               // (0,0,4) near-black
            g = 0;
            b = static_cast<uint8_t>(s * 4);
        } else if (t < 0.15f) {
            float s = (t - 0.05f) / 0.10f;
            r = static_cast<uint8_t>(3 + s * 27);          // → (30,9,68) deep indigo
            g = static_cast<uint8_t>(s * 9);
            b = static_cast<uint8_t>(4 + s * 64);
        } else if (t < 0.25f) {
            float s = (t - 0.15f) / 0.10f;
            r = static_cast<uint8_t>(30 + s * 43);         // → (73,16,103) indigo-purple
            g = static_cast<uint8_t>(9 + s * 7);
            b = static_cast<uint8_t>(68 + s * 35);
        } else if (t < 0.35f) {
            float s = (t - 0.25f) / 0.10f;
            r = static_cast<uint8_t>(73 + s * 47);         // → (120,28,109) dark magenta
            g = static_cast<uint8_t>(16 + s * 12);
            b = static_cast<uint8_t>(103 + s * 6);
        } else if (t < 0.45f) {
            float s = (t - 0.35f) / 0.10f;
            r = static_cast<uint8_t>(120 + s * 37);        // → (157,42,89) magenta-red
            g = static_cast<uint8_t>(28 + s * 14);
            b = static_cast<uint8_t>(109 - s * 20);
        } else if (t < 0.55f) {
            float s = (t - 0.45f) / 0.10f;
            r = static_cast<uint8_t>(157 + s * 30);        // → (187,55,63) warm red
            g = static_cast<uint8_t>(42 + s * 13);
            b = static_cast<uint8_t>(89 - s * 26);
        } else if (t < 0.65f) {
            float s = (t - 0.55f) / 0.10f;
            r = static_cast<uint8_t>(187 + s * 25);        // → (212,80,35) red-orange
            g = static_cast<uint8_t>(55 + s * 25);
            b = static_cast<uint8_t>(63 - s * 28);
        } else if (t < 0.75f) {
            float s = (t - 0.65f) / 0.10f;
            r = static_cast<uint8_t>(212 + s * 22);        // → (234,117,11) orange
            g = static_cast<uint8_t>(80 + s * 37);
            b = static_cast<uint8_t>(35 - s * 24);
        } else if (t < 0.85f) {
            float s = (t - 0.75f) / 0.10f;
            r = static_cast<uint8_t>(234 + s * 13);        // → (247,161,4) amber-yellow
            g = static_cast<uint8_t>(117 + s * 44);
            b = static_cast<uint8_t>(11 - s * 7);
        } else if (t < 0.95f) {
            float s = (t - 0.85f) / 0.10f;
            r = static_cast<uint8_t>(247 + s * 3);         // → (250,210,52) bright yellow
            g = static_cast<uint8_t>(161 + s * 49);
            b = static_cast<uint8_t>(4 + s * 48);
        } else {
            float s = (t - 0.95f) / 0.05f;
            r = static_cast<uint8_t>(250 + s * 2);         // → (252,255,164) pale yellow-white
            g = static_cast<uint8_t>(210 + s * 45);
            b = static_cast<uint8_t>(52 + s * 112);
        }
        return;
    }

    // ── [SELF-HOST P1] Depth types route through the DepthMap picker ────
    if (type == Type::RealtimeWarm || type == Type::RealtimeOrderbook || type == Type::Orderbook) {
        const float tg = depth_t(t);
        if (eval_depth_stops(type, tg, r, g, b)) return;
        t = tg; // EdgeDepth: shipped ramps below, but still gamma-shaped
    }
    if (type == Type::RealtimeWarm) {
        constexpr Stop stops[] = {
            {0.00f, 8, 13, 18}, {0.15f, 15, 30, 64}, {0.40f, 28, 92, 153},
            {0.65f, 75, 181, 190}, {0.80f, 240, 205, 75},
            {0.94f, 248, 108, 40}, {1.00f, 255, 55, 35}};
        eval_stops(stops, t, r, g, b);
        return;
    }
    if (type == Type::RealtimeOrderbook) {
        // Weak liquidity stays dark; high size becomes cyan, yellow, then white.
        constexpr Stop stops[] = {
            {0.00f, 8, 13, 18}, {0.08f, 12, 27, 36}, {0.25f, 22, 83, 108},
            {0.50f, 48, 182, 201}, {0.75f, 218, 217, 95}, {1.00f, 255, 250, 220}};
        eval_stops(stops, t, r, g, b);
        return;
    }
    // Orderbook colormap
    if (t < 0.01f) {
        r = 15; g = 25; b = 45;
    } else if (t < 0.15f) {
        float s = (t - 0.01f) / 0.14f;
        r = static_cast<uint8_t>(15 + s * 10);
        g = static_cast<uint8_t>(25 + s * 95);
        b = static_cast<uint8_t>(45 + s * 105);
    } else if (t < 0.35f) {
        float s = (t - 0.15f) / 0.20f;
        r = static_cast<uint8_t>(25 + s * 35);
        g = static_cast<uint8_t>(120 + s * 40);
        b = static_cast<uint8_t>(150 + s * 55);
    } else if (t < 0.55f) {
        float s = (t - 0.35f) / 0.20f;
        r = static_cast<uint8_t>(60 + s * 120);
        g = static_cast<uint8_t>(160 - s * 80);
        b = static_cast<uint8_t>(205 + s * 30);
    } else if (t < 0.75f) {
        float s = (t - 0.55f) / 0.20f;
        r = static_cast<uint8_t>(180 + s * 65);
        g = static_cast<uint8_t>(80 + s * 100);
        b = static_cast<uint8_t>(235 - s * 155);
    } else {
        float s = (t - 0.75f) / 0.25f;
        r = static_cast<uint8_t>(245 + s * 10);
        g = static_cast<uint8_t>(180 + s * 75);
        b = static_cast<uint8_t>(80 + s * 175);
    }
}

void build_packed_lut(Type type, float opacity,
                      uint32_t* out_lut256) {
    for (int i = 0; i < 256; i++) {
        float t = static_cast<float>(i) / 255.0f;
        uint8_t r, g, b;
        apply(type, t, r, g, b);

        uint8_t a;
        if (type == Type::Liquidation || type == Type::LiquidationWarm) {
            // V7 Alpha curve for inferno colormap with Gaussian-spread data.
            // Wider spread (±4 rows) means more low-value texels that need
            // to fade smoothly into the background. Steep ramp from noise
            // floor creates "glowing cloud" effect.
            // t < 0.05: invisible (noise floor / Gaussian tails)
            // t 0.05-0.15: faint glow (outer spread halo)
            // t 0.15-0.35: emerging (band edges)
            // t 0.35-0.60: visible (significant clusters)
            // t 0.60-1.0: strong (peaks, near-opaque)
            if (t < 0.05f) {
                a = 0;
            } else if (t < 0.15f) {
                float s = (t - 0.05f) / 0.10f;
                a = static_cast<uint8_t>(s * s * 40.0f);     // Quadratic ramp, max 40
            } else if (t < 0.35f) {
                float s = (t - 0.15f) / 0.20f;
                a = static_cast<uint8_t>(40.0f + s * 80.0f);  // 40→120
            } else if (t < 0.60f) {
                float s = (t - 0.35f) / 0.25f;
                a = static_cast<uint8_t>(120.0f + s * 70.0f); // 120→190
            } else {
                float s = (t - 0.60f) / 0.40f;
                a = static_cast<uint8_t>(
                    std::min(245.0f, 190.0f + s * 55.0f));  // 190→245
            }
            a = static_cast<uint8_t>(a * opacity);
        } else {
            a = 220;
        }
        out_lut256[i] = r | (g << 8) | (b << 16)
                       | (static_cast<uint32_t>(a) << 24);
    }
}

void build_lut(Type type, uint32_t* out_lut256) {
    for (int i = 0; i < 256; i++) {
        const float t = static_cast<float>(i) / 255.0f;
        uint8_t r, g, b;
        apply(type, t, r, g, b);
        out_lut256[i] = IM_COL32(r, g, b, 220);
    }
}

} // namespace HeatmapColormap
