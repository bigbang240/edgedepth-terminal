// ═══════════════════════════════════════════════════════════════════════════════
// entitlements_test.cpp - native pin on the client-side replay gating.
//
// Entitlements decides what the terminal LETS you click: how far back the
// scrubber reaches, which symbols replay, how fast, and whether a right-click
// offers "Replay from here" or an upgrade nudge. The Go backend enforces the
// real rule on every replay-session request, so nothing here is a security
// boundary. What it is instead is an agreement: the client must gate exactly
// what the server gates, because the two failure modes are both bad.
//
//   Client tighter than server -> the UI refuses clicks the backend would have
//   served. That already happened once: a hard-coded 30d reach made the
//   cryptohftdata backfill (~71 days back) unreachable even for admin, who had
//   a 90d claim minted into their token.
//
//   Client looser than server -> the user gets a doomed POST and an error modal
//   instead of a locked control that explains itself.
//
// Every window function takes now_ms as a PARAMETER rather than reading a clock,
// which is what makes this file deterministic. The tests below drive it from a
// fixed epoch instead of "now".
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/entitlements.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void expect_true(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

void expect_i64(int64_t got, int64_t want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got %lld, want %lld\n", what,
                     static_cast<long long>(got), static_cast<long long>(want));
        ++failures;
    }
}

void expect_eq(const std::string& got, const std::string& want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s\n  got:  '%s'\n  want: '%s'\n", what, got.c_str(),
                     want.c_str());
        ++failures;
    }
}

constexpr int64_t kDay = 86400000LL;
constexpr int64_t kHour = 3600000LL;

// 2026-08-23T13:45:30.123Z. Deliberately mid-day and not on any round boundary.
constexpr int64_t kNow = 1787492730123LL;

// Every test owns its own tier + window state; nothing is inherited from the
// process defaults or from whichever test ran before.
void reset_state() {
    Entitlements::current() = Entitlements::Tier::Pro;
    Entitlements::pro_lookback_ms() = Entitlements::PRO_REPLAY_LOOKBACK_MS;
    Entitlements::user_email().clear();
    Entitlements::hosted() = true;
    // The recorded-book floor is a data limit layered over every tier; tier
    // reach is measured without it and the floor has its own test below.
    Entitlements::book_replay_from_ms() = 0;
}

void test_tier_is_a_superset_chain() {
    using T = Entitlements::Tier;
    reset_state();

    // Research is a superset of Pro; Admin is a superset of Research. Any tier
    // that answers yes to is_research must also answer yes to is_pro, or a
    // research subscriber loses Pro affordances they have paid for.
    const T tiers[] = {T::Free, T::Trader, T::Pro, T::Research, T::Admin};
    for (const T t : tiers) {
        Entitlements::current() = t;
        if (Entitlements::is_admin()) {
            expect_true(Entitlements::is_research(), "admin implies research");
        }
        if (Entitlements::is_research()) {
            expect_true(Entitlements::is_pro(), "research implies pro");
        }
        expect_true(Entitlements::is_free() == (t == T::Free),
                    "is_free is true for exactly one tier");
    }

    Entitlements::current() = T::Free;
    expect_true(!Entitlements::is_pro(), "free is not pro");
    Entitlements::current() = T::Trader;
    expect_true(!Entitlements::is_pro(), "trader is gated like free for replay");
    Entitlements::current() = T::Pro;
    expect_true(Entitlements::is_pro() && !Entitlements::is_research(), "pro stops at pro");
    Entitlements::current() = T::Research;
    expect_true(Entitlements::is_research() && !Entitlements::is_admin(),
                "research stops below admin");
    Entitlements::current() = T::Admin;
    expect_true(Entitlements::is_admin(), "admin is admin");

    // Labels: one per tier, none of them shared.
    Entitlements::current() = T::Free;     expect_eq(Entitlements::tier_label(), "FREE", "free label");
    Entitlements::current() = T::Trader;   expect_eq(Entitlements::tier_label(), "TRADER", "trader label");
    Entitlements::current() = T::Pro;      expect_eq(Entitlements::tier_label(), "PRO", "pro label");
    Entitlements::current() = T::Research; expect_eq(Entitlements::tier_label(), "RESEARCH", "research label");
    Entitlements::current() = T::Admin;    expect_eq(Entitlements::tier_label(), "ADMIN", "admin label");
}

// Free replay (2026-10-04): Binance Futures BTCUSDT over the trailing 72h,
// mirroring the backend's free-tier check. Nothing else is free to replay.
void test_free_window_is_btcusdt_over_the_last_72h() {
    reset_state();
    Entitlements::current() = Entitlements::Tier::Free;

    int64_t start = 0, end = 0;
    Entitlements::free_window_range(kNow, start, end);
    expect_i64(end, kNow, "the free window ends at the live edge");
    expect_i64(start, kNow - Entitlements::FREE_WINDOW_MS + Entitlements::FREE_WINDOW_INSET_MS,
               "the free window starts 72h back, inset for clock drift");
    expect_i64(Entitlements::replay_start_floor_ms(kNow), start, "the floor is the window start");
    expect_i64(Entitlements::replay_end_ceil_ms(kNow), kNow, "the ceiling is the live edge");
    expect_i64(Entitlements::replay_floor_ms(kNow), Entitlements::replay_start_floor_ms(kNow),
               "the legacy floor alias agrees with the floor");

    // A point is replayable only on the free market.
    expect_true(Entitlements::time_replayable(kNow - kHour, kNow, "binancef", "btcusdt"),
                "an hour ago on BTCUSDT is replayable");
    expect_true(Entitlements::time_replayable(kNow - 72 * kHour, kNow, "binancef", "btcusdt"),
                "the 72h boundary is replayable");
    expect_true(!Entitlements::time_replayable(kNow - 72 * kHour - 1, kNow, "binancef", "btcusdt"),
                "older than 72h is not");
    expect_true(!Entitlements::time_replayable(kNow - kHour, kNow, "binancef", "ethusdt"),
                "ETH is not free to replay (the archived majors day is retired)");
    expect_true(!Entitlements::time_replayable(kNow - kHour, kNow, "bybit", "btcusdt"),
                "Bybit BTC is not the free market");
    expect_true(!Entitlements::time_replayable(kNow - 2 * kDay - kHour, kNow),
                "an unnamed market is not replayable on free");

    // range_replayable cannot see the market, so it never admits a free range;
    // callers admit the free market through btc_preview_range.
    expect_true(!Entitlements::range_replayable(kNow - kHour, kNow, kNow),
                "a free range needs the market check");

    expect_true(Entitlements::market_replay_allowed("binancef", "BTCUSDT"), "binancef BTCUSDT is the free market");
    expect_true(!Entitlements::market_replay_allowed("bybit", "btcusdt"), "bybit BTCUSDT is not");
    expect_true(!Entitlements::market_replay_allowed("binancef", "ethusdt"), "ETH is not");
    Entitlements::current() = Entitlements::Tier::Pro;
    expect_true(Entitlements::market_replay_allowed("bybit", "linkusdt"), "pro replays any market");
}

void test_pro_reach_follows_the_backend_injected_lookback() {
    reset_state();
    Entitlements::current() = Entitlements::Tier::Pro;

    // Default Pro reach is 90 days back, up to the live edge.
    expect_i64(Entitlements::replay_start_floor_ms(kNow), kNow - 90 * kDay, "the default Pro floor is 90d");
    expect_i64(Entitlements::replay_end_ceil_ms(kNow), kNow, "pro reaches the live edge");
    expect_true(Entitlements::range_replayable(kNow - 71 * kDay, kNow - 70 * kDay, kNow),
                "a range inside the 90d Pro reach is replayable");
    expect_true(!Entitlements::range_replayable(kNow - 100 * kDay, kNow - 99 * kDay, kNow),
                "a range beyond the 90d Pro reach is refused");
    // Nobody replays the future, whatever their tier.
    expect_true(!Entitlements::range_replayable(kNow - kHour, kNow + kHour, kNow),
                "a range ending in the future is refused");
    expect_true(!Entitlements::time_replayable(kNow + 1, kNow), "a future timestamp is not replayable");

    // Research and staff carry a deeper signed claim that tracks the corpus.
    // Raising the injected lookback must open it immediately without changing
    // the default Pro offer.
    const int64_t deep = kNow - 300 * kDay;
    expect_true(!Entitlements::time_replayable(deep, kNow),
                "a 300-day-old point is out of reach at the 90d Pro default");
    Entitlements::pro_lookback_ms() = 414 * kDay;
    expect_true(Entitlements::time_replayable(deep, kNow),
                "the same point is in reach once the backend supplies the full-record claim");
    expect_i64(Entitlements::replay_start_floor_ms(kNow), kNow - 414 * kDay, "the floor follows the signed lookback");
    expect_true(Entitlements::pro_lookback_days() == 414, "the day count follows the signed lookback");
    expect_eq(Entitlements::archive_label(), "414d archive", "the scrubber caption follows the signed lookback");

    Entitlements::pro_lookback_ms() = Entitlements::PRO_REPLAY_LOOKBACK_MS;
    expect_true(Entitlements::pro_lookback_days() == 90, "the default Pro reach is 90 days");
    expect_eq(Entitlements::archive_label(), "90d archive", "the default caption reads 90d");
    Entitlements::current() = Entitlements::Tier::Free;
    expect_eq(Entitlements::archive_label(), "free replay", "free advertises the free replay, not a depth");

    // Research and admin reach at least as far as pro at the same settings.
    reset_state();
    for (const auto t : {Entitlements::Tier::Pro, Entitlements::Tier::Research,
                         Entitlements::Tier::Admin}) {
        Entitlements::current() = t;
        expect_i64(Entitlements::replay_start_floor_ms(kNow), kNow - 90 * kDay,
                   "every pro-or-better tier shares the pro floor");
        expect_i64(Entitlements::replay_end_ceil_ms(kNow), kNow,
                   "every pro-or-better tier reaches the live edge");
    }
}

// No tier replays a day older than the recorded order book: the engine cannot
// open without a book. The floor clamps the tier's reach, and a point before it
// is a data lock rather than a plan lock.
void test_book_floor_clamps_every_tier() {
    reset_state();
    const int64_t book = kNow - 200 * kDay;
    Entitlements::book_replay_from_ms() = book;
    Entitlements::pro_lookback_ms() = 414 * kDay;
    expect_i64(Entitlements::replay_start_floor_ms(kNow), book, "a deeper lookback stops at the book floor");
    expect_true(!Entitlements::time_replayable(book - kHour, kNow), "a point before the book is not replayable");
    expect_true(Entitlements::before_book_record(book - kHour), "a point before the book is a data lock");
    expect_true(Entitlements::time_replayable(book + kHour, kNow), "a point after the book is replayable");
    Entitlements::pro_lookback_ms() = Entitlements::PRO_REPLAY_LOOKBACK_MS;
    expect_i64(Entitlements::replay_start_floor_ms(kNow), kNow - 90 * kDay, "a shallower tier floor is unchanged");
}

void test_symbol_and_speed_gates() {
    reset_state();
    Entitlements::current() = Entitlements::Tier::Free;

    // The free symbol (BTCUSDT), matched case-insensitively so a link written in
    // any case resolves the same way the route parser resolves it.
    for (const char* sym : Entitlements::FREE_REPLAY_SYMBOLS) {
        expect_true(Entitlements::symbol_replay_allowed(sym), "a listed free symbol is allowed");
    }
    expect_true(Entitlements::symbol_replay_allowed("BTCUSDT"), "uppercase matches the free list");
    expect_true(Entitlements::symbol_replay_allowed("BtcUsdt"), "mixed case matches the free list");
    expect_true(!Entitlements::symbol_replay_allowed("linkusdt"), "an unlisted symbol is refused");
    expect_true(!Entitlements::symbol_replay_allowed("ethusdt"), "a former free major is refused");
    expect_true(!Entitlements::symbol_replay_allowed(""), "an empty symbol is refused");
    expect_true(!Entitlements::symbol_replay_allowed("btc"), "a partial symbol is refused");
    expect_true(!Entitlements::symbol_replay_allowed("btcusdtx"), "a superstring symbol is refused");

    // Free is exactly 1x, including slow motion and malformed requests.
    for (double speed : {0.0, 0.1, 0.2, 0.5, 2.0, 4.0}) {
        expect_true(!Entitlements::speed_allowed(speed), "every non-1x speed is locked on free");
        expect_true(Entitlements::clamp_speed_for_tier(speed) == 1.0, "free always normalizes to 1x");
    }
    expect_true(Entitlements::speed_allowed(1.0), "1x is allowed on free");
    expect_true(Entitlements::speed_allowed(Entitlements::FREE_MAX_SPEED), "the cap itself is allowed");
    expect_true(!Entitlements::speed_allowed(4.0), "4x is locked on free");
    expect_true(Entitlements::clamp_speed_for_tier(8.0) == Entitlements::FREE_MAX_SPEED,
                "a faster request clamps to the cap");
    expect_true(Entitlements::clamp_speed_for_tier(1.0) == 1.0, "a slower request is untouched");
    expect_true(Entitlements::clamp_speed_for_tier(0.5) == 1.0, "slow motion requires Pro");

    // Pro has no symbol or speed gate at all.
    Entitlements::current() = Entitlements::Tier::Pro;
    expect_true(Entitlements::symbol_replay_allowed("linkusdt"), "pro replays any symbol");
    expect_true(Entitlements::speed_allowed(0.1), "pro can use slow motion");
    expect_true(Entitlements::speed_allowed(64.0), "pro replays at any speed");
    expect_true(Entitlements::clamp_speed_for_tier(64.0) == 64.0, "pro speed is never clamped");
}

void test_authentication_is_not_the_same_question_as_tier() {
    reset_state();

    // Only a genuinely anonymous visitor should be told to log in. A logged-in
    // free user hitting an auth error has a token problem, not a login problem,
    // and sending them to the login page is a dead end.
    Entitlements::current() = Entitlements::Tier::Free;
    Entitlements::user_email().clear();
    expect_true(!Entitlements::is_authenticated(), "an anonymous free visitor is not authenticated");

    Entitlements::user_email() = "someone@example.com";
    expect_true(Entitlements::is_authenticated(), "a free user with an email is authenticated");
    expect_true(Entitlements::is_free(), "and is still on the free tier");

    Entitlements::user_email().clear();
    Entitlements::current() = Entitlements::Tier::Pro;
    expect_true(Entitlements::is_authenticated(), "a pro user is authenticated without an email");

    // Sessions-today is a UX mirror of a server rule; off-Emscripten it reports
    // a full allowance rather than a stale or negative one.
    expect_true(Entitlements::sessions_today() == 0, "the native session counter starts at zero");
    expect_true(Entitlements::sessions_remaining_today() == Entitlements::FREE_MAX_SESSIONS_PER_DAY,
                "the native session allowance is full");
    Entitlements::note_session_started();
    expect_true(Entitlements::sessions_remaining_today() >= 0,
                "the remaining count never goes negative");

    // detect() reads browser globals; natively it must leave the state alone
    // rather than resetting the tier a test (or a dev build) has set.
    Entitlements::current() = Entitlements::Tier::Admin;
    Entitlements::detect();
    expect_true(Entitlements::is_admin(), "detect() is inert off-Emscripten");
    reset_state();
}

void test_pricing_href_preserves_billing_choice() {
    expect_eq(Entitlements::pricing_href("yearly"),
              "https://edgedepth.com/pricing?billing=yearly", "annual pricing review");
    expect_eq(Entitlements::pricing_href("monthly"),
              "https://edgedepth.com/pricing?billing=monthly", "monthly pricing review");
    expect_eq(Entitlements::pricing_href("unknown"),
              "https://edgedepth.com/pricing?billing=yearly", "unknown period defaults yearly");
}

void test_checkout_href_goes_straight_to_card_checkout() {
    expect_eq(Entitlements::checkout_href("yearly"),
              "/pricing?plan=yearly&rail=card&source=terminal-upsell", "annual card checkout");
    expect_eq(Entitlements::checkout_href("monthly"),
              "/pricing?plan=monthly&rail=card&source=terminal-upsell", "monthly card checkout");
    expect_eq(Entitlements::checkout_href(nullptr),
              "/pricing?plan=yearly&rail=card&source=terminal-upsell", "unknown period defaults yearly");
}

void test_coin_label_names_the_market() {
    expect_eq(Entitlements::coin_label("ognusdt"), "OGN", "USDT perp");
    expect_eq(Entitlements::coin_label("1000pepeusdt"), "1000PEPE", "multiplier prefix kept");
    expect_eq(Entitlements::coin_label("ethusdc"), "ETH", "USDC perp");
    expect_eq(Entitlements::coin_label("BTC"), "BTC", "Hyperliquid coin unchanged");
    expect_eq(Entitlements::coin_label("usdt"), "USDT", "never strips to nothing");
}

void test_self_hosted_venues_follow_the_feed() {
    reset_state();

    // Hosted: every venue the picker lists is served.
    for (const char* venue : {"binancef", "hl", "bybit"})
        expect_true(Entitlements::venue_available(venue, false), "hosted serves every listed venue");

    // Self-hosted: the bundled community gateway serves Binance only, so the
    // other venues are not offered until the feed sends their 24h ticker.
    Entitlements::hosted() = false;
    expect_true(Entitlements::venue_available("binancef", false), "self-hosted Binance is available");
    expect_true(!Entitlements::venue_available("hl", false), "self-hosted Hyperliquid needs a feed for it");
    expect_true(!Entitlements::venue_available("bybit", false), "self-hosted Bybit needs a feed for it");
    expect_true(Entitlements::venue_available("hl", true), "a feed that reports Hyperliquid enables it");
    expect_true(Entitlements::venue_available("bybit", true), "a feed that reports Bybit enables it");
    reset_state();
}

}  // namespace

int main() {
    test_tier_is_a_superset_chain();
    test_free_window_is_btcusdt_over_the_last_72h();
    test_pro_reach_follows_the_backend_injected_lookback();
    test_book_floor_clamps_every_tier();
    test_symbol_and_speed_gates();
    reset_state();
    Entitlements::current() = Entitlements::Tier::Free;
    expect_true(Entitlements::btc_preview_range("binancef", "btcusdt", kNow-72*kHour, kNow, kNow), "BTC 72h boundary allowed");
    expect_true(!Entitlements::btc_preview_range("binancef", "btcusdt", kNow-72*kHour-1, kNow, kNow), "BTC older denied");
    expect_true(!Entitlements::btc_preview_range("bybit", "btcusdt", kNow-kHour, kNow, kNow), "other venue denied");
    expect_true(!Entitlements::btc_preview_range("binancef", "xrpusdt", kNow-kHour, kNow, kNow), "other coin denied");
    expect_true(!Entitlements::btc_preview_range("binancef", "btcusdt", kNow-kHour, kNow+1, kNow), "future denied");
    expect_true(!Entitlements::speed_allowed(2), "free preview cannot speed up");
    test_authentication_is_not_the_same_question_as_tier();
    test_pricing_href_preserves_billing_choice();
    test_checkout_href_goes_straight_to_card_checkout();
    test_coin_label_names_the_market();
    test_self_hosted_venues_follow_the_feed();

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("entitlements_test: all assertions passed\n");
    return 0;
}
