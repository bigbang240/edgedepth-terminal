#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// entitlements.h - client-side plan/tier gating for replay (UX layer only).
//
// The web host (Next/Payload) knows the signed-in user's tier; it hands it to the
// WASM client via a window global set BEFORE the glue loads (mirrors how lessons
// are passed in via __EDGEDEPTH_LESSON__):
//
//     window.__EDGEDEPTH_TIER__ = "free" | "trader" | "pro" | "research" | "admin";
//
// We read it once at boot. This drives only the UX (locked menu items, the
// scrubber's advertised archive depth, the upgrade nudge). It is NOT a security
// boundary - the Go backend still enforces the real cap on every replay-session
// request (a tampered global must not unlock data). Standalone / dev (no global)
// defaults to Pro so local work is never gated.
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <array>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <chrono>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace Entitlements {

// Research is a SUPERSET of Pro (every Pro entitlement, plus a deeper replay
// reach). Admin is a superset of Research for terminal display gates. Actual
// replay reach still comes from the
// backend-injected lookback below. NOTE: the web deliberately tokenizes research
// subscribers as `pro` (the backend TierConfig knows free/pro/admin only) and
// carries the extra reach as a signed maxLookbackDays claim; do not "fix" that by
// inventing a research TokenTier.
enum class Tier : uint8_t { Free, Trader, Pro, Research, Admin };

// ── Free-tier replay limits (tunable in ONE place - MIRROR the backend's
//    replay_session free_window.go + TierConfigs["free"]) ─────────────────────
// Since 2026-10-04 free replay is one market: Binance Futures BTCUSDT over the
// last 72 hours, ending at the live edge. The archived UTC day on six majors it
// replaced is retired. The window rolls with the wall clock, so it is computed
// here (free_window_range) rather than read from the host.
inline constexpr int64_t FREE_WINDOW_MS       = 72LL * 3600LL * 1000LL;  // mirror FreeReplayWindow
inline constexpr int64_t FREE_WINDOW_INSET_MS = 60LL * 1000LL;           // clock-drift margin at the start
inline constexpr int64_t PRO_REPLAY_LOOKBACK_MS   = 90LL * 24LL * 3600LL * 1000LL;  // 90d (DEFAULT, not a ceiling)

// Pro lookback is a DEFAULT the backend can raise. Accounts whose replay token
// carries a signed maxLookbackDays claim get more (Research and staff track the
// full recorded corpus). Hard-coding 30d here made Free viewers see an obsolete
// Pro offer even after Pro moved to 90 days. Keep the default aligned with Pro;
// deeper signed claims still override it for Research and staff.
//
// The host injects window.__EDGEDEPTH_REPLAY_LOOKBACK_DAYS__ from the SAME number
// it mints into the token, so the client affordance and server enforcement cannot
// disagree, the same way the order-book floor is injected. Absent or invalid
// leaves 90d, so an ordinary Pro user sees their real reach and never fires a
// doomed POST.
inline int64_t& pro_lookback_ms()   { static int64_t v = PRO_REPLAY_LOOKBACK_MS; return v; }
inline int      pro_lookback_days() { return static_cast<int>(pro_lookback_ms() / (24LL * 3600LL * 1000LL)); }

// Free replay speed cap. The server CLAMPS silently to this (never errors, D7); the
// client locks the >1× buttons and funnels a tap into the upsell modal.
inline constexpr double  FREE_MAX_SPEED = 1.0;

// Free replay sessions per UTC day (server enforces via TIER_DAILY → 429; the client
// mirrors it as a "N of 6 today" chip). Design P2.
inline constexpr int     FREE_MAX_SESSIONS_PER_DAY = 6;

// The one market a non-Pro user may REPLAY (live data stays all-symbols for
// everyone): Binance Futures BTCUSDT. MIRROR handler.go FreeReplaySymbols and
// free_window.go FreeReplayExchange.
inline constexpr const char* FREE_REPLAY_EXCHANGE = "binancef";
inline constexpr std::array<const char*, 1> FREE_REPLAY_SYMBOLS = { "btcusdt" };

// Sanity bound on the host-supplied replay reach. MIRRORS the backend SAFETY
// ceiling (handler.go defaultMaxReplayLookbackDays, env REPLAY_MAX_LOOKBACK_DAYS),
// not any tier's product limit. Its only job is to reject an absurd value from a
// malformed host global; the server enforces the real window.
inline constexpr int kMaxLookbackDaysSanity = 730;

// First UTC day the archive holds an ORDER BOOK (2026-05-15T00:00:00Z). This is
// DATA, not a tier: the record's first 304 days (the cryptohftdata backfill,
// 2025-07-15 → 2026-05-14) carry trades, candles, liquidations, OI and funding
// but no depth, and the replay engine cannot open without a book - it primes on
// the orderbook stream and errors after 20s (replay_depth_unavailable). Until
// 2026-09-18 nothing between the date picker and that timeout knew, so a
// Research account could ask for Dec 2025 and watch the spinner, then an error.
//
// The backend now refuses such a window with DATA_WINDOW and publishes the
// MEASURED floor on /replay/availability; the host injects it as
// window.__EDGEDEPTH_BOOK_REPLAY_FROM_MS__ (TerminalEmbed) before the glue
// loads, so this compiled value is only the fallback for standalone / native
// builds; keep it equal to the hosted product's published book-replay floor. It feeds
// replay_start_floor_ms(), so every entry point (date picker, "Replay from
// here", range_replayable) refuses pre-book days with the date instead of
// posting a doomed session.
inline constexpr int64_t BOOK_REPLAY_FROM_MS = 1778803200000LL;  // 2026-05-15 UTC
inline int64_t& book_replay_from_ms() { static int64_t v = BOOK_REPLAY_FROM_MS; return v; }

// Current tier - read once at boot via detect(). Pro by default (dev/standalone).
inline Tier& current() { static Tier t = Tier::Pro; return t; }
inline bool  is_pro()  {
    return current() == Tier::Pro || current() == Tier::Research || current() == Tier::Admin;
}
inline bool  is_research() {
    return current() == Tier::Research || current() == Tier::Admin;
}
inline bool  is_admin() { return current() == Tier::Admin; }

// Whether this build is running behind the hosted product, i.e. whether there
// is an EdgeDepth account and a courses hub on the other side of the chrome.
//
// Defaults TRUE and only ever flips on an EXPLICIT window.__EDGEDEPTH_HOSTED__
// === false. app.edgedepth.com never sets that global, so the hosted deploy is
// byte-identical to before this existed; the bundled docker image sets it in
// docker/entrypoint.sh. Read via detect() at boot.
//
// This exists because the tier default above is Pro, which is right for dev but
// makes a self-hosted stack render a signed-in "PRO . FOUNDER RATE" account the
// user does not have, contradicting the Unlock CTA in the same window. Tier
// answers "what may this user do"; this answers "is there an account at all".
inline bool& hosted() { static bool b = true; return b; }

// Whether a venue can show data in this deployment. Symbol metadata lists every
// venue, but the community gateway bundled with the self-hosted stack serves
// Binance futures only, so Hyperliquid and Bybit would open there as empty panels.
// Off the hosted product they stay unavailable until the connected feed proves
// otherwise by sending that venue's 24h ticker (feed_reports_venue), so a
// gateway that adds one of these venues enables it without a client change.
inline bool venue_needs_hosted_feed(std::string_view exchange) {
    return exchange == "hl" || exchange == "bybit";
}
inline bool venue_available(std::string_view exchange, bool feed_reports_venue) {
    return hosted() || feed_reports_venue || !venue_needs_hosted_feed(exchange);
}

// Signed-in user's email (account menu header). Set by the host via
// window.__EDGEDEPTH_USER_EMAIL__; empty in standalone/dev.
inline std::string& user_email() { static std::string e; return e; }

// Subscription renewal date label for the Pro account menu (e.g. "12 Jul 2026").
// The host sets window.__EDGEDEPTH_RENEWS__ to a short pre-formatted string; empty
// in standalone/dev (the menu then just shows "Active", no date). Display-only.
inline std::string& renews_label() { static std::string s; return s; }

// Forward decl: the window helpers below (replay_start_floor_ms, replay_end_ceil_ms)
// resolve the free window through this single function. Defined lower, once the
// constants are in scope.
inline void free_window_range(int64_t now_ms, int64_t& start_ms, int64_t& end_ms);

inline const char* tier_label() {
    switch (current()) { case Tier::Admin: return "ADMIN";
                         case Tier::Research: return "RESEARCH";
                         case Tier::Pro: return "PRO";
                         case Tier::Trader: return "TRADER";
                         default: return "FREE"; }
}

// Read window.__EDGEDEPTH_TIER__ once. Safe to call when not embedded (leaves Pro).
inline void detect() {
#ifdef __EMSCRIPTEN__
    char buf[16] = {0};
    const int n = EM_ASM_INT({
        try {
            var t = (window.__EDGEDEPTH_TIER__ || "").toString().toLowerCase();
            if (!t) return 0;
            var s = (t === "free") ? "free" : (t === "trader") ? "trader"
                  : (t === "research") ? "research" : (t === "admin") ? "admin" : "pro";
            stringToUTF8(s, $0, 16);
            return s.length;
        } catch (e) { return 0; }
    }, buf);
    if (n > 0) {
        const std::string s(buf);
        current() = (s == "free")     ? Tier::Free
                  : (s == "trader")   ? Tier::Trader
                  : (s == "research") ? Tier::Research
                  : (s == "admin")    ? Tier::Admin
                                      : Tier::Pro;
    }
    // Strict === false, so an absent or malformed global leaves hosted() true
    // and nothing about app.edgedepth.com changes.
    hosted() = EM_ASM_INT({
        try { return window.__EDGEDEPTH_HOSTED__ === false ? 0 : 1; }
        catch (e) { return 1; }
    }) != 0;

    char ebuf[128] = {0};
    EM_ASM({
        try {
            var e = (window.__EDGEDEPTH_USER_EMAIL__ || "").toString();
            stringToUTF8(e.slice(0, 120), $0, 128);
        } catch (err) {}
    }, ebuf);
    user_email() = ebuf;

    // Optional pre-formatted renewal date (Pro account menu). Empty if unset.
    char rbuf[64] = {0};
    EM_ASM({
        try {
            var r = (window.__EDGEDEPTH_RENEWS__ || "").toString();
            stringToUTF8(r.slice(0, 56), $0, 64);
        } catch (err) {}
    }, rbuf);
    renews_label() = rbuf;

    // Backend-authoritative replay reach (window.__EDGEDEPTH_REPLAY_LOOKBACK_DAYS__),
    // set by the host from the SAME number it mints into the replay token.
    // Clamped to [1, kMaxLookbackDaysSanity]; anything absent or absurd leaves
    // the 90d Pro default rather than advertising reach the backend will refuse.
    //
    // The bound used to be 90, mirroring handler.go's old maxReplayLookback.
    // That constant was doing two jobs, safety bound AND the research tier's
    // product limit, and backend 8fd0495 split them: the ceiling is now a
    // 730-day guard against a bad or leaked token, and the web signs a claim
    // that TRACKS THE CORPUS. Mirroring 90 here silently truncated that claim,
    // so the terminal offered 90 days while the backend would have served the
    // whole record. Mirror the SAFETY bound, never the product limit: the
    // server is the enforcer and a claim it rejects costs one refused request,
    // while a claim we truncate is reach the user paid for and never sees.
    {
        const int d = EM_ASM_INT({
            try {
                var v = parseInt(window.__EDGEDEPTH_REPLAY_LOOKBACK_DAYS__, 10);
                return (isFinite(v) && v > 0) ? v : 0;
            } catch (e) { return 0; }
        });
        if (d > 0) {
            const int clamped = d > kMaxLookbackDaysSanity ? kMaxLookbackDaysSanity : d;
            pro_lookback_ms() = static_cast<int64_t>(clamped) * 24LL * 3600LL * 1000LL;
        }
    }

    // Order-book replay floor (window.__EDGEDEPTH_BOOK_REPLAY_FROM_MS__): the
    // backend's measured first book day, injected by the host for every tier.
    // Doubles cross via HEAPF64 (exact for ms timestamps). Absent or absurd leaves the
    // compiled fallback (BOOK_REPLAY_FROM_MS) rather than dropping the floor,
    // because a missing floor means a replay that hangs, not a wider offer.
    {
        double bd = 0.0;
        const int ok = EM_ASM_INT({
            try {
                var v = Number(window.__EDGEDEPTH_BOOK_REPLAY_FROM_MS__);
                if (!(isFinite(v) && v > 0)) return 0;
                HEAPF64[$0 >> 3] = v;
                return 1;
            } catch (err) { return 0; }
        }, &bd);
        if (ok && bd > 1.0e12 && bd < 4.0e12) {   // a plausible epoch-ms (2001..2096)
            book_replay_from_ms() = static_cast<int64_t>(bd);
        }
    }
#endif
}

inline bool is_free() { return current() == Tier::Free; }

// True when the viewer is a signed-in account: Pro/admin, OR a logged-in free user
// whose email the host provided. Drives the AUTH_REQUIRED UX - only a genuinely
// anonymous visitor should be told to "log in" (§8.4); a logged-in user hitting an
// auth error has a token/session problem, not a login problem.
inline bool is_authenticated() { return is_pro() || !user_email().empty(); }

// ── Replay window for the current tier ───────────────────────────────────────
// Earliest replayable START: the LATER of the tier floor and the order-book
// floor. Tier floor: Pro = now−90d by default (deeper on a signed claim); Free =
// the start of the 72h BTCUSDT window. Book floor: book_replay_from_ms(), the
// first day the archive has a book - a Research claim reaches the whole record,
// but the record has no depth before it and replay cannot open without one.
inline int64_t replay_start_floor_ms(int64_t now_ms) {
    int64_t tier_floor;
    if (is_pro()) {
        tier_floor = now_ms - pro_lookback_ms();
    } else {
        int64_t s, e; free_window_range(now_ms, s, e); tier_floor = s;
    }
    return std::max(tier_floor, book_replay_from_ms());
}
// Is this timestamp older than the recorded order book? True means NO tier can
// replay it (a data lock, not a plan lock): the right-click menu and the picker
// word it from book_replay_from_label() rather than offering an upgrade.
inline bool before_book_record(int64_t ts_ms) { return ts_ms < book_replay_from_ms(); }
// "15 May 2026" (UTC) for the book floor. ASCII only (no atlas glyphs).
inline std::string book_replay_from_label() {
    static const char* MON[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
    time_t t = static_cast<time_t>(book_replay_from_ms() / 1000);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[24];
    snprintf(buf, sizeof(buf), "%d %s %d", tmv.tm_mday, MON[tmv.tm_mon], tmv.tm_year + 1900);
    return std::string(buf);
}
// Latest replayable END. Both tiers end at the live edge.
inline int64_t replay_end_ceil_ms(int64_t now_ms) {
    if (is_pro()) return now_ms;
    int64_t s, e; free_window_range(now_ms, s, e); return e;
}
// Back-compat alias for old call sites: the tier's earliest start timestamp.
inline int64_t replay_floor_ms(int64_t now_ms) { return replay_start_floor_ms(now_ms); }

// Is a single timestamp replayable for the current tier? (used by the chart
// right-click "Replay from here" gate - a point must sit inside the window).
inline bool btc_preview_market(const std::string& exchange, const std::string& symbol) {
    return exchange == "binancef" && symbol == "btcusdt";
}
// The free replay market's window: BTCUSDT on Binance Futures, starting inside
// the last 72h and ending by now (mirrors the backend's free-tier check).
inline bool btc_preview_range(const std::string& exchange, const std::string& symbol,
                              int64_t start_ms, int64_t end_ms, int64_t now_ms) {
    return btc_preview_market(exchange, symbol) && start_ms >= now_ms - FREE_WINDOW_MS &&
           start_ms >= book_replay_from_ms() && start_ms < end_ms && end_ms <= now_ms;
}
inline bool time_replayable(int64_t ts_ms, int64_t now_ms,
                            const std::string& exchange = "", const std::string& symbol = "") {
    if (is_pro()) return ts_ms >= replay_start_floor_ms(now_ms) && ts_ms <= now_ms;
    return btc_preview_range(exchange, symbol, ts_ms, ts_ms + 1, now_ms);
}
// Is a whole [start,end] range replayable for the current tier, without naming
// the market? Pro only: a free range is replayable only on the free market,
// which callers check with btc_preview_range (it needs exchange and symbol).
inline bool range_replayable(int64_t start_ms, int64_t end_ms, int64_t now_ms) {
    return is_pro() && start_ms >= replay_start_floor_ms(now_ms) && end_ms <= now_ms;
}

// ── Speed ────────────────────────────────────────────────────────────────────
inline bool   speed_allowed(double s)        { return is_pro() || s <= FREE_MAX_SPEED + 1e-9; }
inline double clamp_speed_for_tier(double s)  { return is_pro() ? s : (s > FREE_MAX_SPEED ? FREE_MAX_SPEED : s); }

// Short human label for the advertised archive depth (scrubber caption).
inline const char* archive_label() {
    if (!is_pro()) return "free replay";
    static char buf[24];
    snprintf(buf, sizeof(buf), "%dd archive", pro_lookback_days());
    return buf;
}

// Wall-clock now (ms).
inline int64_t wall_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// The free replay window at now_ms: the last 72h, its start inset a minute so
// a boundary request survives client/server clock drift, ending at the live edge.
inline void free_window_range(int64_t now_ms, int64_t& start_ms, int64_t& end_ms) {
    start_ms = now_ms - FREE_WINDOW_MS + FREE_WINDOW_INSET_MS;
    end_ms   = now_ms;
}

// Human copy of the free replay offer for the account menu and upsell modal.
inline std::string free_window_label() {
    return "BTCUSDT on Binance Futures, the past 3 days";
}

// May the current tier replay this symbol? (case-insensitive)
inline bool symbol_replay_allowed(const std::string& symbol) {
    if (is_pro()) return true;
    std::string s = symbol;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* a : FREE_REPLAY_SYMBOLS) if (s == a) return true;
    return false;
}

// May the current tier replay this market? Free: only FREE_REPLAY_EXCHANGE's
// BTCUSDT (Bybit or Hyperliquid BTC is not the free market).
inline bool market_replay_allowed(const std::string& exchange, const std::string& symbol) {
    return is_pro() || (exchange == FREE_REPLAY_EXCHANGE && symbol_replay_allowed(symbol));
}

// Send the user to the upgrade page (web host). No-op off-Emscripten.
inline void open_upgrade() {
#ifdef __EMSCRIPTEN__
    EM_ASM({ window.location.href = '/pricing'; });
#endif
}

// Build the pricing-review URL. Keep this pure so the native suite pins the
// billing choice even though the browser navigation only exists in WASM.
inline std::string pricing_href(const char* period) {
    const std::string safe_period =
        period && std::string(period) == "monthly" ? "monthly" : "yearly";
    return "https://edgedepth.com/pricing?billing=" + safe_period;
}

// Straight to card checkout for the interval the upsell shows. The app's
// /pricing gateway resolves the founder plan for that interval and opens
// Stripe (it offers Bitcoin itself when card checkout is off), so the buyer
// skips re-choosing on the plan page. Pure, so the native suite pins it.
inline std::string checkout_href(const char* period) {
    const std::string safe_period =
        period && std::string(period) == "monthly" ? "monthly" : "yearly";
    return "/pricing?plan=" + safe_period + "&rail=card&source=terminal-upsell";
}

// A market's coin for prose and buttons: "ognusdt" -> "OGN", "1000pepeusdt"
// -> "1000PEPE"; a Hyperliquid "BTC" is already a coin. Pure (native-tested).
inline std::string coin_label(const std::string& symbol) {
    std::string s = symbol;
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const char* quote : {"USDT", "USDC", "BUSD", "USD"}) {
        const std::string q(quote);
        if (s.size() > q.size() && s.compare(s.size() - q.size(), q.size(), q) == 0) {
            s.resize(s.size() - q.size());
            break;
        }
    }
    return s;
}

// Opens in a NEW tab so the terminal (live socket, replay session, layout) is
// still there afterwards; a blocked popup falls back to navigating this tab.
// The opener is cut after opening rather than with 'noopener', which makes
// window.open return null and hides a block.
inline void open_in_new_tab(const std::string& href) {
#ifdef __EMSCRIPTEN__
    EM_ASM({
        var url = UTF8ToString($0);
        var w = null;
        try { w = window.open(url, '_blank'); } catch (e) {}
        if (w) { try { w.opener = null; } catch (e) {} }
        else { window.location.href = url; }
    }, href.c_str());
#else
    (void)href;
#endif
}

// Review Free, Pro and Research before the user chooses a payment rail. The
// selected interval arrives preselected, so the extra page adds context, not
// repeated work.
inline void open_pricing(const char* period = "yearly") {
    open_in_new_tab(pricing_href(period));
}

// The upsell's main action: card checkout for the shown interval.
inline void open_checkout(const char* period = "yearly") {
    open_in_new_tab(checkout_href(period));
}

// Send the user to the login page (web host), preserving where they were so the
// login form can bounce them back. Drives the AUTH_REQUIRED/GRANT_REQUIRED prompt.
inline void open_login() {
#ifdef __EMSCRIPTEN__
    EM_ASM({
        try {
            var here = window.location.pathname + window.location.search;
            window.location.href = '/login?redirectTo=' + encodeURIComponent(here);
        } catch (e) { window.location.href = '/login'; }
    });
#endif
}

// ── Sessions-today mirror ("N of 6 today" chip) ──────────────────────────────
// The SERVER is the real enforcer (TIER_DAILY); this is a UX counter in
// localStorage keyed by the UTC date so it resets at 00:00 UTC. NOT a security
// boundary - a tampered client can reset it and the server still 429s.
inline int sessions_today() {
#ifdef __EMSCRIPTEN__
    return EM_ASM_INT({
        try {
            var d = new Date().toISOString().slice(0, 10);
            var raw = localStorage.getItem('edx_replay_day');
            var o = raw ? JSON.parse(raw) : null;
            if (!o || o.d !== d) return 0;
            return o.n | 0;
        } catch (e) { return 0; }
    });
#else
    return 0;
#endif
}
inline void note_session_started() {
#ifdef __EMSCRIPTEN__
    EM_ASM({
        try {
            var d = new Date().toISOString().slice(0, 10);
            var raw = localStorage.getItem('edx_replay_day');
            var o = raw ? JSON.parse(raw) : null;
            var n = (o && o.d === d) ? (o.n | 0) : 0;
            localStorage.setItem('edx_replay_day', JSON.stringify({ d: d, n: n + 1 }));
        } catch (e) {}
    });
#endif
}
inline int sessions_remaining_today() {
    int rem = FREE_MAX_SESSIONS_PER_DAY - sessions_today();
    return rem < 0 ? 0 : rem;
}

}  // namespace Entitlements
