// V2 scorecard display (core/liqmap2_store.h scorecard_line/detail, core/liqmap2_scorecard.h receive).
#include "core/liqmap2_scorecard.h"

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

static bool has(const char* s, const char* part) { return std::strstr(s, part) != nullptr; }

int main() {
    char line[300];
    liqmap2::Frame f;
    f.provisional = true;
    // Why a market has no scorecard.
    liqmap2::scorecard_line("hl", nullptr, nullptr, line, sizeof(line));
    CHECK(has(line, "Hyperliquid not scored yet"));
    liqmap2::scorecard_line("bybit", &f, nullptr, line, sizeof(line));
    CHECK(has(line, "held out of the Bybit test"));
    liqmap2::scorecard_line("binancef", &f, nullptr, line, sizeof(line));
    CHECK(has(line, "loading"));  // Binance is provisional but scored

    // A reply: parse, then the line and the detail.
    const auto j = nlohmann::json::parse(R"({"type":"liqmap2_scorecard","exchange":"binancef","symbol":"btcusdt",
        "window_days":28,"days":3,"from":"2026-10-18","to":"2026-10-20","realized_source":"lower_bound","provisional":true,
        "minutes_scored":4000,"minutes_reached_long":120,"minutes_reached_short":90,"predicted_long_usd":4e6,
        "predicted_short_usd":6e6,"realized_long_reached_usd":2e6,"realized_short_reached_usd":3e6,
        "realized_long_all_usd":5e6,"realized_short_all_usd":5e6,"events_reached":40,"events_all":100})");
    auto& cards = liqmap2::Scorecards::instance();
    CHECK(cards.put(j));
    const auto* sc = cards.get("binancef", "btcusdt");
    CHECK(sc && sc->days == 3 && sc->reached_long == 120 && sc->pred_short == 6e6);
    liqmap2::scorecard_line("binancef", &f, sc, line, sizeof(line));
    CHECK(has(line, "3 days") && has(line, "$10.00M where price went") && has(line, "$5.00M reported there") &&
          has(line, "(0.50x)") && has(line, "lower bound"));
    char detail[4][200];
    const int n = liqmap2::scorecard_detail(*sc, detail);
    CHECK(n == 4 && has(detail[0], "2026-10-18 to 2026-10-20") && has(detail[2], "50% came where the map had levels") &&
          has(detail[3], "lower bound"));

    // Nothing scored yet, and malformed replies.
    liqmap2::Scorecard empty;
    empty.days = 0;
    liqmap2::scorecard_line("bybit", nullptr, &empty, line, sizeof(line));
    CHECK(has(line, "no scored day yet"));
    CHECK(liqmap2::scorecard_detail(empty, detail) == 0);
    CHECK(!cards.put(nlohmann::json::parse(R"({"exchange":"bybit","symbol":"ethusdt"})")));
    CHECK(!cards.put(nlohmann::json::parse(R"({"exchange":"bybit","symbol":"ethusdt","days":-1})")));
    CHECK(cards.get("bybit", "ethusdt") == nullptr);

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("liqmap2_scorecard_test: all passed");
    return EXIT_SUCCESS;
}
