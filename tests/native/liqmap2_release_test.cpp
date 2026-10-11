// V2 map access (core/liqmap2_release.h): admins preview; once public, Pro everywhere, BTC free on every
// venue, a locked row elsewhere.
#include "core/liqmap2_release.h"

#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

int main() {
    using liqmap2::Access;
    // Released 2026-10-11 (the rails, as a forward test): Pro everywhere, BTC free on every venue, locked elsewhere.
    CHECK(liqmap2::kPublic);
    CHECK(liqmap2::access("binancef", "wusdt", false, true) == Access::released);
    CHECK(liqmap2::access("binancef", "btcusdt", false, false) == Access::free_taste);
    CHECK(liqmap2::access("binancef", "wusdt", false, false) == Access::locked);
    CHECK(liqmap2::access("bybit", "btcusdt", false, true) == Access::released);
    // Before release: admins only.
    CHECK(liqmap2::access("bybit", "ethusdt", true, false, false) == Access::preview);
    CHECK(liqmap2::access("bybit", "btcusdt", false, true, false) == Access::hidden);
    CHECK(liqmap2::access("hl", "BTC", false, false, false) == Access::hidden);
    // Public: Pro on every market, everyone on BTC, locked elsewhere.
    CHECK(liqmap2::access("bybit", "ethusdt", false, true, true) == Access::released);
    CHECK(liqmap2::access("binancef", "btcusdt", false, false, true) == Access::free_taste);
    CHECK(liqmap2::access("bybit", "btcusdt", false, false, true) == Access::free_taste);
    CHECK(liqmap2::access("hl", "BTC", false, false, true) == Access::free_taste);
    CHECK(liqmap2::access("hl", "btc", false, false, true) == Access::locked);  // hub naming is exact
    CHECK(liqmap2::access("bybit", "ethusdt", false, false, true) == Access::locked);
    CHECK(liqmap2::access("hl", "ETH", true, false, true) == Access::preview);
    // The shipped default follows kPublic.
    CHECK(liqmap2::access("bybit", "btcusdt", false, false) == (liqmap2::kPublic ? Access::free_taste : Access::hidden));

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("liqmap2_release_test: all passed");
    return EXIT_SUCCESS;
}
