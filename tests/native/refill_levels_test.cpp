// Refill levels (core/refill_levels.h): parse, merge by id, warming and rewind resets, bounded history,
// subscription counts and the access rule (BTC free live, Pro elsewhere, replay allowed, other venues not).
#include "core/refill_levels.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

using nlohmann::json;
using namespace refill;

static constexpr int64_t T = 1790000000000;

static json level(const std::string& id, const char* side, double price, int64_t started, int64_t known,
                  const char* status = "holding", int64_t closed = 0) {
    return json{{"id", id}, {"side", side}, {"price", price}, {"started_ms", started}, {"known_ms", known},
                {"updated_ms", closed ? closed : known + 1000}, {"closed_ms", closed}, {"status", status},
                {"executed_base", 24.1}, {"executed_usd", 2440000.0}, {"max_shown_base", 7.7},
                {"refilled_base", 19.3}, {"refills", 7}, {"shown_ratio", 3.1},
                {"refill_ms", json::array({started + 500, started + 900, known})}};
}

static json message(const char* status, int64_t as_of, json levels = json::array(), const char* mode = "live",
                    const char* symbol = "btcusdt", const char* reason = "") {
    return json{{"type", "refill_levels"}, {"exchange", "binancef"}, {"symbol", symbol}, {"mode", mode},
                {"version", kVersion}, {"status", status}, {"reason", reason}, {"as_of_ms", as_of},
                {"levels", std::move(levels)}};
}

static void test_access() {
    // Live: BTC is free for everyone, the rest of Binance futures is Pro.
    CHECK(access("binancef", "btcusdt", false, false) == Access::free_taste);
    CHECK(access("binancef", "btcusdt", true, false) == Access::released);
    CHECK(access("binancef", "ethusdt", false, false) == Access::locked);
    CHECK(access("binancef", "ethusdt", true, false) == Access::released);
    // Replay: allowed on any Binance futures market; the server enforces the replay scope.
    CHECK(access("binancef", "ethusdt", false, true) == Access::replay);
    CHECK(access("binancef", "btcusdt", false, true) == Access::replay);
    // Other venues are unsupported in live and replay alike.
    CHECK(access("bybit", "btcusdt", true, false) == Access::unsupported);
    CHECK(access("hl", "BTC", false, true) == Access::unsupported);
    // Standalone (not hosted) builds are not gated by tier.
    CHECK(access("binancef", "ethusdt", false, false, false) == Access::released);
    CHECK(granted(Access::free_taste) && granted(Access::released) && granted(Access::replay));
    CHECK(!granted(Access::locked) && !granted(Access::unsupported));
}

static void test_parse_and_merge() {
    Store s;
    // Unsubscribed markets are ignored.
    CHECK(s.receive(message("observing", T, json::array({level("a", "bid", 100.5, T - 9000, T - 5000)}))) ==
          Receipt::ignored);
    s.expect("binancef", "btcusdt", false);
    const Market* m = s.find("binancef", "btcusdt");
    CHECK(m && m->status == Status::none && m->levels.empty());

    CHECK(s.receive(message("observing", T, json::array({level("a", "bid", 100.5, T - 9000, T - 5000),
                                                         level("b", "ask", 101.0, T - 8000, T - 4000)}))) ==
          Receipt::accepted);
    CHECK(m->status == Status::observing && m->as_of_ms == T && m->levels.size() == 2);
    CHECK(m->levels[0].id == "a" && m->levels[0].side == Side::bid && m->levels[0].price == 100.5);
    CHECK(m->levels[0].refills == 7 && m->levels[0].refill_count == 3 && m->levels[0].shown_ratio == 3.1);
    CHECK(m->levels[1].side == Side::ask);

    // Merge by id: "a" closes, "b" drops out of the complete set, "c" is new.
    CHECK(s.receive(message("observing", T + 500, json::array({level("a", "bid", 100.5, T - 9000, T - 5000,
                                                                     "crossed", T + 200),
                                                               level("c", "bid", 99.5, T - 3000, T)}))) ==
          Receipt::accepted);
    CHECK(m->levels.size() == 3);
    CHECK(m->levels[0].status == End::crossed && m->levels[0].closed_ms == T + 200 && m->levels[0].current);
    CHECK(!m->levels[1].current);  // "b" kept as history, ended at its last update
    CHECK(m->levels[2].id == "c");

    // An older message is out of order and ignored.
    CHECK(s.receive(message("observing", T + 100)) == Receipt::ignored);
    CHECK(m->levels.size() == 3);
    // The other context's message is ignored.
    CHECK(s.receive(message("observing", T + 900, json::array(), "replay")) == Receipt::ignored);
    // A different version is ignored rather than misread.
    json other = message("observing", T + 900);
    other["version"] = "liquidity_response.refill.v2";
    CHECK(s.receive(other) == Receipt::ignored);
    CHECK(m->levels.size() == 3);
}

static void test_malformed() {
    Store s;
    s.expect("binancef", "btcusdt", false);
    const Market* m = s.find("binancef", "btcusdt");
    json bad_price = level("p", "bid", -1.0, T - 9000, T - 5000);
    json nan_usd = level("u", "bid", 100.0, T - 9000, T - 5000);
    nan_usd["executed_usd"] = "lots";
    json closed_early = level("c", "ask", 100.0, T - 9000, T - 5000, "crossed", T - 10000);
    json known_early = level("k", "ask", 100.0, T - 9000, T - 9500);
    json bad_side = level("s", "middle", 100.0, T - 9000, T - 5000);
    json bad_status = level("t", "bid", 100.0, T - 9000, T - 5000, "gone");
    json bad_refills = level("r", "bid", 100.0, T - 9000, T - 5000);
    bad_refills["refill_ms"] = json::array({T, "soon"});
    json fractional = level("f", "bid", 100.0, T - 9000, T - 5000);
    fractional["refills"] = 2.5;
    json zero_start = level("z", "bid", 100.0, 0, 0);
    json good = level("g", "bid", 100.0, T - 9000, T - 5000);
    CHECK(s.receive(message("observing", T, json::array({bad_price, nan_usd, closed_early, known_early, bad_side,
                                                         bad_status, bad_refills, fractional, zero_start, good,
                                                         json("not an object")}))) == Receipt::accepted);
    CHECK(m->levels.size() == 1 && m->levels[0].id == "g");
    CHECK(s.dropped() == 10);
    // A malformed envelope changes nothing.
    json no_levels = message("observing", T + 1000);
    no_levels.erase("levels");
    CHECK(s.receive(no_levels) == Receipt::malformed);
    json no_clock = message("observing", T + 1000);
    no_clock["as_of_ms"] = "now";
    CHECK(s.receive(no_clock) == Receipt::malformed);
    CHECK(s.receive(json::array()) == Receipt::malformed);
    CHECK(m->levels.size() == 1 && m->as_of_ms == T);
    // Go writes empty slices as null: an empty refill_ms and an empty set are valid.
    json null_times = level("n", "bid", 100.0, T - 9000, T - 5000);
    null_times["refill_ms"] = nullptr;
    Level none;
    CHECK(parse_level(null_times, none) && none.refill_count == 0);
    json null_levels = message("observing", T + 1000);
    null_levels["levels"] = nullptr;
    CHECK(s.receive(null_levels) == Receipt::accepted && m->as_of_ms == T + 1000);
    CHECK(m->levels.size() == 1 && !m->levels[0].current);
    // A refusal before the server picked a mode (no mode field) still applies.
    json no_mode = message("unavailable", T + 1100, json::array(), "live", "btcusdt", "unsupported_market");
    no_mode.erase("mode");
    CHECK(s.receive(no_mode) == Receipt::unavailable && m->reason == "unsupported_market");
    json bad_mode = message("observing", T + 1200);
    bad_mode["mode"] = "paper";
    CHECK(s.receive(bad_mode) == Receipt::malformed);
    // Refill times past 32 keep the newest 32.
    json many = level("m", "bid", 100.0, T - 90000, T - 5000);
    json times = json::array();
    for (int i = 0; i < 40; ++i) times.push_back(T - 90000 + i);
    many["refill_ms"] = times;
    Level parsed;
    CHECK(parse_level(many, parsed) && parsed.refill_count == 32 && parsed.refill_ms[0] == T - 90000 + 8);
}

static void test_resets() {
    Store s;
    s.expect("binancef", "btcusdt", false);
    const Market* m = s.find("binancef", "btcusdt");
    s.receive(message("observing", T, json::array({level("a", "bid", 100.5, T - 9000, T - 5000)})));
    CHECK(m->levels.size() == 1);
    // Warming is a reset for that market.
    CHECK(s.receive(message("warming", T + 100)) == Receipt::reset);
    CHECK(m->status == Status::warming && m->levels.empty());
    s.receive(message("observing", T + 200, json::array({level("a", "bid", 100.5, T - 9000, T - 5000)})));
    CHECK(m->levels.size() == 1);
    // Unavailable empties it and keeps the reason.
    CHECK(s.receive(message("unavailable", T + 300, json::array(), "live", "btcusdt", "pro_required")) ==
          Receipt::unavailable);
    CHECK(m->status == Status::unavailable && m->reason == "pro_required" && m->levels.empty());
    // A rewind or transport change clears every market but keeps the subscription's expectation.
    s.receive(message("observing", T + 400, json::array({level("a", "bid", 100.5, T - 9000, T - 5000)})));
    s.clear_all();
    CHECK(m->status == Status::none && m->levels.empty() && m->as_of_ms == 0);
    CHECK(s.receive(message("observing", T + 100, json::array({level("a", "bid", 100.5, T - 9000, T - 5000)}))) ==
          Receipt::accepted);
    // Re-subscribing in replay switches the accepted mode.
    s.expect("binancef", "btcusdt", true);
    CHECK(m->levels.empty());
    CHECK(s.receive(message("observing", T + 500, json::array({level("a", "bid", 100.5, T - 9000, T - 5000)}))) ==
          Receipt::ignored);
    CHECK(s.receive(message("observing", T + 500, json::array({level("a", "bid", 100.5, T - 9000, T - 5000)}),
                            "replay")) == Receipt::accepted);
    s.forget("binancef", "btcusdt");
    CHECK(s.find("binancef", "btcusdt") == nullptr);
}

static void test_eviction() {
    Store s;
    s.expect("binancef", "btcusdt", false);
    const Market* m = s.find("binancef", "btcusdt");
    // 200 closed levels (oldest first), then 100 holding ones, in batches like the server's.
    int64_t clock = T;
    for (int i = 0; i < 200; i += 50) {
        json batch = json::array();
        for (int k = i; k < i + 50; ++k)
            batch.push_back(level("closed-" + std::to_string(k), "bid", 100.0 + k, T + k * 10, T + k * 10 + 5,
                                  "inactive", T + k * 10 + 9));
        s.receive(message("observing", ++clock + 100000, batch));
    }
    CHECK(m->levels.size() == 200);
    for (int i = 0; i < 100; i += 50) {
        json batch = json::array();
        for (int k = i; k < i + 50; ++k)
            batch.push_back(level("hold-" + std::to_string(k), "ask", 200.0 + k, T + 5000 + k, T + 6000 + k));
        s.receive(message("observing", ++clock + 100000, batch));
    }
    CHECK(m->levels.size() == kMaxLevels);
    // The 44 oldest closed levels went first; every holding level remains.
    bool oldest_gone = true, holding_kept = true;
    int holding = 0;
    for (const auto& l : m->levels) {
        if (l.id == "closed-0" || l.id == "closed-43") oldest_gone = false;
        if (l.status == End::holding) ++holding;
    }
    for (int k = 0; k < 100; ++k) {
        bool found = false;
        for (const auto& l : m->levels) found = found || l.id == "hold-" + std::to_string(k);
        holding_kept = holding_kept && found;
    }
    CHECK(oldest_gone && holding_kept && holding == 100);
    bool kept_44 = false;
    for (const auto& l : m->levels) kept_44 = kept_44 || l.id == "closed-44";
    CHECK(kept_44);
}

static void test_display() {
    Store s;
    s.expect("binancef", "btcusdt", false);
    const Market* m = s.find("binancef", "btcusdt");
    s.receive(message("observing", T, json::array({
        level("old", "bid", 100.0, T - 60000, T - 50000, "pulled", T - 40000),
        level("hold", "ask", 101.0, T - 9000, T - 5000),
        level("later", "bid", 99.0, T - 2000, T - 1000, "crossed", T - 500),
        level("future", "bid", 98.0, T - 2000, T + 5000)})));
    Span sp;
    // As of T: the holding rail runs to the latest observation; the crossed one ends at its close.
    CHECK(span_at(m->levels[1], *m, T, sp) && sp.open && sp.end == T && sp.known == T - 5000);
    CHECK(span_at(m->levels[2], *m, T, sp) && !sp.open && sp.end == T - 500 && sp.end_status == End::crossed);
    // Not knowable yet at the clock: not shown.
    CHECK(!span_at(m->levels[3], *m, T, sp));
    // A replay clock before the close shows it still holding at the clock.
    CHECK(span_at(m->levels[2], *m, T - 800, sp) && sp.open && sp.end == T - 800);
    std::array<uint16_t, kMaxLevels> idx{};
    const View all{double(T - 120000), double(T + 1000), 0, 1000};
    CHECK(select(*m, T, all, idx) == 3);
    CHECK(idx[0] == 1 && idx[1] == 2 && idx[2] == 0);  // holding first, then most recent
    CHECK(select(*m, T, all, idx, 2) == 2 && idx[0] == 1 && idx[1] == 2);
    const View narrow{double(T - 3000), double(T + 1000), 98.5, 99.5};
    CHECK(select(*m, T, narrow, idx) == 1 && idx[0] == 2);
    CHECK(rail_width(1e6, 1e6) == 4.5f && rail_width(3e5, 1e6) == 3.0f && rail_width(1e5, 1e6) == 2.0f);

    char out[96];
    hover_title(m->levels[1], "101.0", out, sizeof(out));
    CHECK(std::strcmp(out, "Ask refilled 7\xc3\x97 at 101.0") == 0);
    Level l;
    l.executed_usd = 2440000;
    l.max_shown_base = 7.7;
    l.price = 101300;
    hover_values(l, out, sizeof(out));
    CHECK(std::strcmp(out, "$2.4M traded vs $780k shown") == 0);
    format_usd(12500, out, sizeof(out));
    CHECK(std::strcmp(out, "$12.5k") == 0);
    format_usd(999700, out, sizeof(out));
    CHECK(std::strcmp(out, "$1.0M") == 0);
    format_usd(950, out, sizeof(out));
    CHECK(std::strcmp(out, "$950") == 0);
    CHECK(unavailable_note("pro_required") == nullptr && unavailable_note("context_changed") == nullptr);
    CHECK(unavailable_note("session_limit") != nullptr);
}

static const char* test_token() { return "signed"; }

static void test_subscriptions() {
    Store s;
    Subscriptions subs(s);
    std::vector<Message> out;
    int live = 0, replay = 0, chart_a = 0, chart_b = 0;
    Subscriptions::Want w;
    w.channel = &live; w.exchange = "binancef"; w.symbol = "ethusdt"; w.tick = 0.01;
    w.generation = 1; w.connected = false; w.now_ms = 0; w.token = test_token;
    // Nothing is sent until the channel is open.
    subs.want(&chart_a, w, out);
    CHECK(out.empty() && subs.count() == 1);
    w.connected = true;
    subs.want(&chart_a, w, out);
    CHECK(out.size() == 1 && out[0].channel == &live);
    {
        const json sent = json::parse(out[0].text);
        CHECK(sent["method"] == "refill_subscribe" && sent["data"]["pair"]["symbol"] == "ethusdt" &&
              sent["data"]["tick_size"] == 0.01 && sent["data"]["entitlement_token"] == "signed");
    }
    CHECK(s.find("binancef", "ethusdt") != nullptr);
    // A second chart on the same market shares the subscription.
    out.clear();
    subs.want(&chart_b, w, out);
    subs.want(&chart_a, w, out);
    CHECK(out.empty() && subs.count() == 1 && subs.owners("binancef", "ethusdt") == 2);
    // A reconnect (new generation) subscribes again; so does a transport change, once the throttle allows.
    w.generation = 2; w.now_ms = 5000;
    subs.want(&chart_a, w, out);
    CHECK(out.size() == 1);
    out.clear();
    subs.transport_changed();
    w.now_ms = 6000;
    subs.want(&chart_a, w, out);
    CHECK(out.empty());  // throttled: within two seconds of the last subscribe
    w.now_ms = 7100;
    subs.want(&chart_a, w, out);
    CHECK(out.size() == 1);
    out.clear();
    // context_changed asks for a new subscribe.
    subs.resubscribe("binancef", "ethusdt");
    w.now_ms = 9200;
    subs.want(&chart_b, w, out);
    CHECK(out.size() == 1);
    out.clear();
    // The first chart leaves: no unsubscribe while the second still wants it.
    subs.release(&chart_a, out);
    CHECK(out.empty() && subs.owners("binancef", "ethusdt") == 1);
    // The second moves to the replay context: the live subscription ends, the replay one starts.
    Subscriptions::Want r = w;
    r.channel = &replay; r.replay = true; r.generation = 1;
    subs.want(&chart_b, r, out);
    CHECK(out.size() == 2 && out[0].channel == &live && out[1].channel == &replay);
    CHECK(json::parse(out[0].text)["method"] == "refill_unsubscribe");
    CHECK(json::parse(out[1].text)["method"] == "refill_subscribe");
    CHECK(s.find("binancef", "ethusdt") && s.find("binancef", "ethusdt")->replay);
    out.clear();
    subs.release(&chart_b, out);
    CHECK(out.size() == 1 && out[0].channel == &replay && subs.count() == 0);
    CHECK(s.find("binancef", "ethusdt") == nullptr);
    // Releasing an owner that holds nothing is harmless.
    out.clear();
    subs.release(&chart_b, out);
    CHECK(out.empty());
    // An owner that never got a subscribe out sends no unsubscribe either.
    Subscriptions::Want closed = w;
    closed.connected = false;
    subs.want(&chart_a, closed, out);
    subs.release(&chart_a, out);
    CHECK(out.empty() && subs.count() == 0);
}

int main() {
    test_access();
    test_parse_and_merge();
    test_malformed();
    test_resets();
    test_eviction();
    test_display();
    test_subscriptions();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("refill_levels_test: all passed");
    return EXIT_SUCCESS;
}
