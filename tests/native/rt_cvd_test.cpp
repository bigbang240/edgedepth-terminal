// The real-time CVD lane's series (core/rt_cvd.h).
#include "core/rt_cvd.h"
#include <cmath>
#include <cstdio>
#include <deque>

namespace {
Terminal::Trade trade(int64_t t, double qty, bool buy) {
    Terminal::Trade x{};
    x.timestamp_ms = t; x.price = 100; x.qty = qty; x.is_buy = buy;
    return x;
}
bool near(double a, double b) { return std::abs(a - b) < 1e-9; }
}

int main() {
    int failures = 0;
    auto expect = [&](bool ok, const char* message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    const std::deque<Terminal::Trade> trades{
        trade(500, 9, true),     // before the view: excluded
        trade(1000, 2, true),    // at the left edge: included
        trade(1150, 0.5, false),
        trade(1420, 3, false),
        trade(1420, 1, true),
        trade(1790, 4, false),
        trade(1950, 7, true),    // after the as-of clock: excluded
    };
    RealtimeCvdSeries cvd;
    cvd.build(trades, 1000, 2000, 1900, 10);
    expect(cvd.columns() == 10, "one column per requested slot");
    expect(near(cvd.column(0).value, 2) && cvd.column(0).valid, "starts at zero at the left edge, then buys add");
    expect(near(cvd.column(1).value, 1.5), "sells subtract");
    expect(near(cvd.column(4).value, -0.5), "same-time prints both count");
    expect(near(cvd.column(7).value, -4.5) && cvd.column(8).valid, "the clock's column is drawn");
    expect(!cvd.column(9).valid, "time after the as-of clock is not drawn");
    expect(near(cvd.end_value(), -4.5) && cvd.last_valid() == 8, "end value is the last drawn column");
    expect(near(cvd.low(), -4.5) && near(cvd.high(), 2), "range spans the drawn values");
    double v = 0;
    expect(cvd.value_at(1430, v) && near(v, -0.5), "hover reads the column under the pointer");
    expect(!cvd.value_at(1950, v), "no value past the clock");
    expect(!cvd.value_at(900, v), "no value left of the view");

    // A view entirely before any trade is flat zero, never empty of meaning.
    cvd.build(trades, 0, 400, 1900, 4);
    expect(!cvd.empty() && near(cvd.end_value(), 0) && near(cvd.low(), 0) && near(cvd.high(), 0), "flat at zero with no trades");

    // Bounded: a huge request is clamped.
    cvd.build(trades, 1000, 2000, 2000, 100000);
    expect(cvd.columns() == RealtimeCvdSeries::kMaxColumns, "columns are capped");
    expect(near(cvd.end_value(), 2.5), "every in-view trade counted once");

    // A view starting later restarts from zero there.
    cvd.build(trades, 1400, 2000, 2000, 6);
    expect(near(cvd.column(0).value, -2) && near(cvd.end_value(), 1), "zero again at a new left edge");

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::printf("rt_cvd_test passed\n");
    return failures ? 1 : 0;
}
