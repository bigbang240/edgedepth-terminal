#pragma once
// Real-time heatmap colour reference with optional automatic contrast.
// Set once (the original rule): the first measurement fixes the scale until an
// explicit recalibration. Automatic: every kRemeasureMs the visible columns are
// measured again and the scale eases toward that target with a kTimeConstantMs
// exponential (in log space, so brightening and dimming feel alike). Pure logic;
// ShaderHeatmapRenderer supplies the measurements and owns the GPU side.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rt_contrast {
// How often automatic contrast measures the visible book again (chart clock).
inline constexpr int64_t kRemeasureMs = 2000;
// Exponential time constant of the ease toward the latest measurement.
inline constexpr double kTimeConstantMs = 10000.0;
// The colour reference is this percentile of native-tick quantities (unchanged).
inline constexpr int kPercentile = 90;
// Columns sampled per measurement (unchanged from the set-once rule).
inline constexpr int kSampleColumns = 64;
// The measured reference is drawn as this much more than it is, so a row at the
// reference sits in the cool middle of the ramp and only rows well above it turn
// warm: the book is the quiet context under trades and order book signals.
inline constexpr float kDisplayHeadroom = 1.8f;
}

class RealtimeContrast {
public:
    // `reference` is the renderer's current scale; 0 means not calibrated yet or
    // an explicit recalibration, which always snaps to a fresh measurement.
    // `visible()` measures the newest visible columns; `all()` the original
    // whole-history sample. Both return 0 when nothing can be measured.
    template <class Visible, class All>
    float update(float reference, int64_t clock_ms, bool automatic, Visible&& visible, All&& all) {
        if (!(reference > 0)) {
            float measured = automatic ? visible() : 0.0f;
            if (!(measured > 0)) measured = all();
            target_ = measured > 0 ? measured : 0.0f;
            measured_ms_ = stepped_ms_ = clock_ms;
            return target_;
        }
        if (!automatic) return reference;           // set once: fixed until recalibration
        if (clock_ms < measured_ms_ || clock_ms - measured_ms_ >= rt_contrast::kRemeasureMs) {
            const float measured = visible();
            if (measured > 0) target_ = measured;
            measured_ms_ = clock_ms;
        }
        return step(reference, clock_ms);
    }
    float target() const { return target_; }
    void reset() { target_ = 0; measured_ms_ = stepped_ms_ = 0; }

private:
    float step(float reference, int64_t clock_ms) {
        // A paused or rewound clock never eases (and never jumps).
        if (clock_ms <= stepped_ms_ || !(target_ > 0)) { stepped_ms_ = clock_ms; return reference; }
        const double dt = double(clock_ms - stepped_ms_);
        stepped_ms_ = clock_ms;
        const double a = 1.0 - std::exp(-dt / rt_contrast::kTimeConstantMs);
        const double eased = std::exp(std::log(double(reference)) + (std::log(double(target_)) - std::log(double(reference))) * a);
        return std::isfinite(eased) && eased > 0 ? float(eased) : reference;
    }
    float target_ = 0;
    int64_t measured_ms_ = 0, stepped_ms_ = 0;
};
