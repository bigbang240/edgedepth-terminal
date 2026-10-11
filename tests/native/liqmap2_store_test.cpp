// V2 modelled liquidation map store (core/liqmap2_store.h): validation, bounded history, freshness and
// the claims-ladder wording per venue.
#include "core/liqmap2_store.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static liqmap2::Frame frame(int64_t ts, double mark = 100.0) {
    liqmap2::Frame f;
    f.timestamp_ms = ts;
    f.mark_price = mark;
    f.band_width_pct = 0.25;
    f.status = "ready";
    f.model_class = "majors";
    f.config_id = "v2-test";
    f.levels = {{mark*0.97, 3e6, 0}, {mark*0.99, 1e6, 0}, {mark*1.01, 0, 2e6}, {mark*1.04, 0, 5e5}};
    f.total_long_usd = 4e6;
    f.total_short_usd = 2.5e6;
    return f;
}

static void validation() {
    using liqmap2::valid;
    CHECK(valid(frame(60000)));
    auto f = frame(60000);
    f.levels[2].long_usd = 1;  // a long above the mark
    f.total_long_usd += 1;
    CHECK(!valid(f));
    f = frame(60000);
    f.levels[0].short_usd = 1;  // a short below the mark
    f.total_short_usd += 1;
    CHECK(!valid(f));
    f = frame(60000);
    std::swap(f.levels[0], f.levels[1]);
    CHECK(!valid(f));
    f = frame(60000);
    f.total_long_usd *= 1.01;
    CHECK(!valid(f));
    f = frame(60000);
    f.total_long_usd *= 1 + 1e-8;  // float32 rounding of history levels is within tolerance
    CHECK(valid(f));
    f = frame(60000);
    f.status = "sampled";
    CHECK(!valid(f));
    f = frame(60000);
    f.levels.assign(liqmap2::kMaxLevels + 1, {90.0, 0, 0});
    for (size_t i = 0; i < f.levels.size(); ++i) f.levels[i].price = 50.0 + 0.1*static_cast<double>(i);
    f.total_long_usd = f.total_short_usd = 0;
    CHECK(!valid(f));
    f = frame(60000, 0.0);
    CHECK(!valid(f));  // levels without a mark
    f.levels.clear();
    f.total_long_usd = f.total_short_usd = 0;
    f.status = "stale";
    CHECK(valid(f));
    f = frame(60000);
    f.levels[1].long_usd = -1;
    CHECK(!valid(f));
}

static void history() {
    liqmap2::History h;
    for (int64_t i = 0; i < static_cast<int64_t>(liqmap2::kMaxFrames) + 10; ++i) CHECK(h.insert(frame((i + 1)*60000)));
    CHECK(h.frames().size() == liqmap2::kMaxFrames);
    CHECK(h.frames().begin()->first == 11*60000);
    CHECK(h.at(10*60000) == nullptr);
    CHECK(h.at(100*60000 + 30000)->timestamp_ms == 100*60000);
    auto bad = frame(500*60000);
    bad.status = "unknown";
    CHECK(!h.insert(bad));
    CHECK(h.at(500*60000)->timestamp_ms != 500*60000);

    liqmap2::History g;
    CHECK(g.insert(frame(60000)));
    CHECK(g.insert(frame(5*60000)));  // a four-minute gap
    CHECK(g.display_end(60000, 10*60000) == 2*60000);       // never painted through the gap
    CHECK(g.display_end(5*60000, 5*60000 + 30000) == 5*60000 + 30000);  // newest held to the clock
    CHECK(g.display_end(5*60000, 20*60000) == 6*60000);     // but never past its own minute
    CHECK(g.display_end(5*60000, 4*60000) == 5*60000);      // a future frame paints nothing
}

static void states() {
    using liqmap2::State;
    auto f = frame(60000);
    CHECK(liqmap2::state(nullptr, 60000) == State::missing);
    CHECK(liqmap2::state(&f, 30000) == State::missing);
    CHECK(liqmap2::state(&f, 60000 + liqmap2::kFreshMs) == State::ready);
    CHECK(liqmap2::state(&f, 60000 + liqmap2::kFreshMs + 1) == State::stale);
    f.status = "warming";
    CHECK(liqmap2::state(&f, 70000) == State::warming);
    f.status = "stale";
    CHECK(liqmap2::state(&f, 70000) == State::stale);
}

static bool contains(const char* s, const char* part) { return s && std::strstr(s, part) != nullptr; }

static void wording() {
    using liqmap2::Claim;
    char cap[256];
    auto f = frame(60000);
    CHECK(liqmap2::claim("hl", f) == Claim::tested);
    CHECK(liqmap2::claim("bybit", f) == Claim::tested);
    liqmap2::caption("bybit", f, cap, sizeof(cap));
    CHECK(contains(cap, "tested") && contains(cap, "not a magnet") && contains(cap, "estimated liquidations in $"));
    CHECK(contains(liqmap2::record("bybit", f), "6.2%"));
    CHECK(contains(liqmap2::dollars("bybit", f), "not at 1h"));
    CHECK(contains(liqmap2::dollars("hl", f), "not calibrated"));

    f.model_class = "small";
    CHECK(liqmap2::claim("hl", f) == Claim::untested_small);
    CHECK(liqmap2::claim("bybit", f) == Claim::tested);  // Bybit small coins passed Test 2
    liqmap2::caption("hl", f, cap, sizeof(cap));
    CHECK(contains(cap, "small coins untested") && !contains(cap, ", tested"));
    CHECK(contains(liqmap2::record("hl", f), "small-coin split did not"));
    CHECK(contains(liqmap2::dollars("hl", f), "untested"));

    f = frame(60000);
    f.provisional = true;
    CHECK(liqmap2::claim("bybit", f) == Claim::provisional_bybit);
    CHECK(liqmap2::claim("binancef", f) == Claim::provisional_venue);
    liqmap2::caption("bybit", f, cap, sizeof(cap));
    CHECK(contains(cap, "not covered by the Bybit test"));
    liqmap2::caption("binancef", f, cap, sizeof(cap));
    CHECK(contains(cap, "untested on this venue") && contains(cap, "not a magnet"));
    CHECK(liqmap2::record("bybit", f) == nullptr && liqmap2::record("binancef", f) == nullptr);
    CHECK(contains(liqmap2::dollars("bybit", f), "untested"));
    CHECK(std::strcmp(liqmap2::claim_short("bybit", f), "not covered by the Bybit test") == 0);
    CHECK(std::strcmp(liqmap2::claim_short("binancef", f), "untested on this venue") == 0);
    f.provisional = false;
    CHECK(std::strcmp(liqmap2::claim_short("hl", f), "tested") == 0);
}

// V2.2 fields (hub v21/PREREG_V2_2A): forty bands per side or none, finite and non-negative, a known regime; band
// geometry from the mark; the panel line and the record per venue.
static void v22() {
    using liqmap2::valid;
    auto f = frame(60000);
    CHECK(!liqmap2::has_v22(f));
    f.expected_long.assign(liqmap2::kV22Bands, 1000.0);
    f.expected_short.assign(liqmap2::kV22Bands, 500.0);
    f.regime = "extreme";
    f.hot = true;
    f.day_move = std::log(1.34);
    CHECK(valid(f) && liqmap2::has_v22(f));
    auto g = f;
    g.expected_short.pop_back();
    CHECK(!valid(g));
    g = f;
    g.expected_long[3] = -1;
    CHECK(!valid(g));
    g = f;
    g.regime = "wild";
    CHECK(!valid(g));
    double lo = 0, hi = 0;
    liqmap2::v22_band(true, 3, 100, lo, hi);
    CHECK(std::abs(lo - 98.5) < 1e-9 && std::abs(hi - 99.0) < 1e-9);
    liqmap2::v22_band(false, 1, 100, lo, hi);
    CHECK(std::abs(lo - 100.0) < 1e-9 && std::abs(hi - 100.5) < 1e-9);
    char line[240];
    liqmap2::v22_line("bybit", f, line, sizeof(line));
    CHECK(std::strstr(line, "extreme volatility") && std::strstr(line, "HOT +34%") && std::strstr(line, "longs $6.0K") &&
          std::strstr(line, "shorts $3.0K") && !std::strstr(line, "untested"));
    liqmap2::v22_line("binancef", f, line, sizeof(line));
    CHECK(std::strstr(line, "untested on Binance"));
    liqmap2::v22_line("bybit", frame(60000), line, sizeof(line));
    CHECK(line[0] == 0);
    CHECK(std::strstr(liqmap2::v22_record("bybit"), "2.0 points better than V2") && std::strstr(liqmap2::v22_record("binancef"), "untested") &&
          liqmap2::v22_record("hl") == nullptr);
    for (const char* text : {liqmap2::v22_record("bybit"), liqmap2::v22_record("binancef")})
        CHECK(!std::strstr(text, "magnet") || std::strstr(text, "not a magnet"));
}

// The daily record link: Binance and Bybit markets (locked or not), never Hyperliquid.
static void scorecard_links() {
    char url[160];
    CHECK(liqmap2::scorecard_url("binancef", "btcusdt", url, sizeof(url)));
    CHECK(std::strcmp(url, "https://edgedepth.com/research/liquidation-map/scorecard/binance/btcusdt") == 0);
    CHECK(liqmap2::scorecard_url("bybit", "ETHUSDT", url, sizeof(url)));
    CHECK(std::strcmp(url, "https://edgedepth.com/research/liquidation-map/scorecard/bybit/ethusdt") == 0);
    CHECK(!liqmap2::scorecard_url("hl", "BTC", url, sizeof(url)) && url[0] == 0);
    CHECK(!liqmap2::scorecard_url("okx", "btcusdt", url, sizeof(url)));
    CHECK(!liqmap2::scorecard_url("bybit", "", url, sizeof(url)));
}

int main() {
    scorecard_links();
    v22();
    validation();
    history();
    states();
    wording();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("liqmap2_store_test: all passed");
    return EXIT_SUCCESS;
}
