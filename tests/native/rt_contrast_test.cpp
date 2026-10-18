// Real-time heatmap colour reference (core/rt_contrast.h): set once versus
// automatic contrast, recalibration snaps, bounded measurement.
#include "core/rt_contrast.h"
#include <cmath>
#include <cstdio>

int main() {
    int failures = 0;
    auto expect = [&](bool ok, const char* message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    float visible_value = 40, all_value = 10;
    int visible_calls = 0, all_calls = 0;
    auto visible = [&] { ++visible_calls; return visible_value; };
    auto all = [&] { ++all_calls; return all_value; };

    {   // Off: the original set-once rule. First measurement from the whole history, then fixed.
        RealtimeContrast c;
        float ref = c.update(0, 1000, false, visible, all);
        expect(ref == 10 && all_calls == 1 && visible_calls == 0, "off calibrates once from the whole history");
        for (int64_t t = 1100; t <= 60000; t += 100) ref = c.update(ref, t, false, visible, all);
        expect(ref == 10 && all_calls == 1 && visible_calls == 0, "off never measures again or moves");
        ref = c.update(0, 61000, false, visible, all);
        expect(ref == 10 && all_calls == 2, "Recalibrate colors measures again");
    }
    visible_calls = all_calls = 0;
    {   // On: the first scale and a recalibration snap to the visible book.
        RealtimeContrast c;
        float ref = c.update(0, 1000, true, visible, all);
        expect(ref == 40 && visible_calls == 1 && all_calls == 0, "auto snaps to the visible measurement");
        visible_value = 0;   // nothing visible to measure (a trades-only stretch)
        expect(c.update(0, 2000, true, visible, all) == 10, "with nothing visible a snap falls back to the whole history");
        visible_value = 40;
    }
    visible_calls = all_calls = 0;
    {   // On: eases toward a new level without overshoot, measuring every 2 s only.
        RealtimeContrast c;
        visible_value = 10;
        float ref = c.update(0, 0, true, visible, all);
        visible_value = 40;   // price moved into a thicker book
        float previous = ref;
        bool monotonic = true, bounded = true;
        float at_10s = 0;
        for (int64_t t = 16; t <= 60000; t += 16) {   // ~60 fps
            ref = c.update(ref, t, true, visible, all);
            monotonic &= ref >= previous;
            bounded &= ref <= 40.0001f;
            previous = ref;
            if (t == 10000) at_10s = ref;
        }
        expect(monotonic && bounded, "the scale moves one way and never overshoots");
        // Log-space ease with a 10 s constant: about 63% of the way (in log) after 10 s,
        // measured late by up to the 2 s remeasure interval.
        expect(at_10s > 18 && at_10s < 30, "about two thirds of the way after ten seconds");
        expect(std::abs(ref - 40) < 0.5f, "converges within a minute");
        expect(visible_calls <= 31 && visible_calls >= 29, "measures every two seconds, not every frame");
        expect(all_calls == 0, "never the whole-history sample while easing");
    }
    {   // A paused or rewound clock does not move the scale.
        RealtimeContrast c;
        visible_value = 10;
        float ref = c.update(0, 5000, true, visible, all);
        visible_value = 100;
        ref = c.update(ref, 7000, true, visible, all);   // measures the new target
        const float held = c.update(ref, 7000, true, visible, all);
        expect(held == ref, "a paused clock holds the scale");
        const float rewound = c.update(ref, 6000, true, visible, all);
        expect(rewound == ref, "a clock that went back never jumps the scale");
        expect(c.update(ref, 6100, true, visible, all) > ref, "easing resumes as the clock moves on");
    }
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::printf("rt_contrast_test passed\n");
    return failures ? 1 : 0;
}
