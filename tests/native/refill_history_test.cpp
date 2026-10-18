// Recorded refill levels for candle charts (core/refill_history.h): request planning (debounce, one read per chart,
// gaps, the 7-day clamp, floors, the clock's edge), replies (merge, coverage, truncation, errors and back-off),
// resets, eviction and the drawn selection.
#include "core/refill_history.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

using nlohmann::json;
using namespace refill::history;
using refill::kVersion;
using refill::Level;
using refill::parse_level;
using refill::Receipt;

static constexpr int64_t T = 1790000000000;
static constexpr int64_t H = 3600000;
static constexpr int64_t D = 24 * H;

static json level(const std::string& id, double price, int64_t known, int64_t closed, double usd = 1e6,
                  const char* status = "crossed", const char* side = "bid") {
    return json{{"id", id}, {"side", side}, {"price", price}, {"started_ms", known - 2000}, {"known_ms", known},
                {"updated_ms", closed}, {"closed_ms", closed}, {"status", status},
                {"executed_base", 10.0}, {"executed_usd", usd}, {"max_shown_base", 3.0},
                {"refilled_base", 7.0}, {"refills", 23}, {"shown_ratio", 3.3}, {"refill_ms", nullptr}};
}

static json reply(const std::string& id, json levels = json::array(), int64_t coverage = T - 30 * D,
                  bool truncated = false, const char* error = "", const char* mode = "live") {
    return json{{"type", "refill_history"}, {"request_id", id}, {"exchange", "binancef"}, {"symbol", "btcusdt"},
                {"mode", mode}, {"version", kVersion}, {"from_ms", 0}, {"to_ms", 0},
                {"coverage_from_ms", coverage}, {"levels", std::move(levels)}, {"truncated", truncated},
                {"error", error}};
}

static Want want(int64_t from, int64_t to, double now, int64_t clock = T, bool replay = false,
                 const char* symbol = "btcusdt") {
    Want w;
    w.exchange = "binancef";
    w.symbol = symbol;
    w.replay = replay;
    w.view_from = from;
    w.view_to = to;
    w.floor = T - 72 * H;
    w.clock = clock;
    w.now = now;
    return w;
}

// Runs the requester until it settles and asks (or not). Returns true with `out` when it asked.
static bool settle(Requester& r, Store& s, Want w, Request& out) {
    if (r.step(s, w, out)) return true;
    w.now += kSettleMs;
    return r.step(s, w, out);
}

static void test_debounce_and_request() {
    Store s;
    Requester r;
    Request out;
    // The first frame only starts the settle clock; nothing goes out before the view has rested 500 ms.
    Want w = want(T - 2 * H, T, 1000);
    CHECK(!r.step(s, w, out));
    w.now = 1400;
    CHECK(!r.step(s, w, out));
    // Moving the view restarts the wait.
    w.view_from -= H; w.view_to -= H; w.now = 1450;
    CHECK(!r.step(s, w, out));
    w.now = 1900;
    CHECK(!r.step(s, w, out));
    w.now = 1950;
    CHECK(r.step(s, w, out));
    // The read covers the view, a quarter past its right edge and half a view more each side, capped at the clock.
    CHECK(out.from == T - 4 * H && out.to == T);
    CHECK(s.pending(out.id) && r.in_flight(s));
    const json msg = json::parse(request_message(out, "binancef", "btcusdt", "tok"));
    CHECK(msg["method"] == "get_refill_history");
    CHECK(msg["data"]["request_id"] == out.id && msg["data"]["from_ms"] == out.from && msg["data"]["to_ms"] == out.to);
    CHECK(msg["data"]["pair"]["exchange"] == "binancef" && msg["data"]["pair"]["symbol"] == "btcusdt");
    CHECK(msg["data"]["entitlement_token"] == "tok");
    CHECK(json::parse(request_message(out, "binancef", "btcusdt", nullptr))["data"]["entitlement_token"] == "");
    // One read in flight per chart.
    w.now = 5000;
    Request again;
    CHECK(!r.step(s, w, again));
    // Panning a little stays inside what was read: nothing new is asked.
    s.receive(reply(out.id), 5100);
    w.view_to += 10 * 60000; w.view_from += 10 * 60000; w.now = 5200;
    CHECK(!settle(r, s, w, again));
}

static void test_gaps_and_clamp() {
    Store s;
    Requester r;
    Request out;
    // A week and more in view: the read is clamped to 7 days back from its right end (the clock here).
    Want w = want(T - 20 * D, T - D, 0, T);
    w.floor = 0;  // Pro: no tier floor inside this range
    CHECK(settle(r, s, w, out));
    const int64_t right = std::min(T - D + std::min<int64_t>(19 * D / 4, kMaxPadMs), T);
    CHECK(out.to == right && out.from == right - kMaxRangeMs);
    CHECK(out.to - out.from <= kMaxRangeMs);
    CHECK(s.receive(reply(out.id), 600) == Receipt::accepted);
    // Panning left asks only for the part not yet read.
    w.view_from -= 2 * D; w.view_to -= 2 * D; w.now = 1000;
    CHECK(settle(r, s, w, out));
    const int64_t right2 = w.view_to + std::min<int64_t>(19 * D / 4, kMaxPadMs);
    CHECK(out.to == right - kMaxRangeMs - 1 && out.from == right2 - kMaxRangeMs);
    CHECK(s.receive(reply(out.id), 1600) == Receipt::accepted);
    // Panning back over read ranges asks for nothing.
    w.view_from += D; w.view_to += D; w.now = 2000;
    CHECK(!settle(r, s, w, out));
    const Market* m = s.find("binancef", "btcusdt", false);
    CHECK(m && m->covered.size() == 1);
    CHECK(m->covered[0].from == right2 - kMaxRangeMs && m->covered[0].to == right);

    // A Free viewer never asks before the 3-day window; a view entirely before it asks for nothing.
    Store f;
    Requester rf;
    Want old = want(T - 10 * D, T - 5 * D, 0, T);
    CHECK(!settle(rf, f, old, out));
    CHECK(rf.before_floor());
    Want recent = want(T - 4 * D, T, 1000, T);
    CHECK(settle(rf, f, recent, out));
    CHECK(out.from == recent.floor && out.to == T && rf.before_floor());
    f.receive(reply(out.id, json::array(), T - D), 1100);
    // Known coverage clamps the left side too: nothing closed before the first recorded close.
    Want earlier = want(T - 3 * D, T - 2 * D, 2000, T);
    CHECK(!settle(rf, f, earlier, out));

    // Two charts on one market share the cache and never ask for the same range twice.
    Store sh;
    Requester a, b;
    Request ra, rb;
    Want wa = want(T - 2 * H, T, 0);
    CHECK(settle(a, sh, wa, ra));
    CHECK(!settle(b, sh, wa, rb));  // pending on chart a covers it
    CHECK(sh.market_count() == 1 && sh.find("binancef", "btcusdt", false)->owners.size() == 2);
    // Chart a goes away: its read is forgotten and chart b asks.
    a.reset(sh);
    CHECK(!sh.pending(ra.id) && sh.market_count() == 1);
    wa.now = 2000;
    CHECK(settle(b, sh, wa, rb));
    CHECK(sh.receive(reply(ra.id), 2100) == Receipt::ignored);
    b.reset(sh);
    CHECK(sh.market_count() == 0 && sh.pending_count() == 0);
}

static void test_edge_refresh() {
    Store s;
    Requester r;
    Request out;
    Want w = want(T - H, T, 0, T);
    CHECK(settle(r, s, w, out));
    CHECK(out.to == T);
    s.receive(reply(out.id), 600);
    // The clock moves on; the new tail waits for 30 s after the last edge read was sent.
    w.clock = T + 10000; w.view_to = T + 10000; w.view_from = T - H + 10000;
    w.now = 10000;
    CHECK(!settle(r, s, w, out));
    w.now = 31000;
    CHECK(r.step(s, w, out));
    CHECK(out.from == T + 1 && out.to == T + 10000);
    s.receive(reply(out.id), 31100);
    // A hole earlier in the view is read at once, edge or not.
    Store h;
    Requester rh;
    Want wh = want(T - H, T, 0, T);
    CHECK(settle(rh, h, wh, out));
    h.receive(reply(out.id), 600);
    wh.view_from = T - 3 * H; wh.now = 1000;
    CHECK(settle(rh, h, wh, out));
    CHECK(out.from == T - 9 * H / 2 && out.to == T - 3 * H / 2 - 1);
}

static void test_receive_merge_coverage() {
    Store s;
    Requester r;
    Request out;
    Want w = want(T - 2 * H, T, 0);
    CHECK(settle(r, s, w, out));
    json levels = json::array({level("a", 82462.5, T - H, T - H + 60000, 592000),
                               level("b", 82400.0, T - 50 * 60000, T - 49 * 60000, 1.5e6, "pulled", "ask"),
                               level("bad", -1, T - H, T - H + 1, 1),                     // malformed
                               level("open", 82000, T - H, T - H + 1, 1, "holding")});   // not closed
    // Unknown request ids, other types and other modes are ignored.
    CHECK(s.receive(reply("nope", levels), 600) == Receipt::ignored);
    json other = reply(out.id, levels);
    other["type"] = "refill_levels";
    CHECK(s.receive(other, 600) == Receipt::ignored);
    CHECK(s.pending(out.id));
    CHECK(s.receive(reply(out.id, levels, T - 10 * D, false, "", "replay"), 600) == Receipt::ignored);
    CHECK(!s.pending(out.id));  // that answer is spent; the range is asked again
    w.now = 1000;
    CHECK(settle(r, s, w, out));
    CHECK(s.receive(reply(out.id, levels, T - 10 * D), 1600) == Receipt::accepted);
    const Market* m = s.find("binancef", "btcusdt", false);
    CHECK(m && m->levels.size() == 2 && s.dropped() == 2);
    CHECK(m->coverage_known && m->coverage_from_ms == T - 10 * D && m->error == Error::none);
    CHECK(!truncated_in(*m, T - 2 * H, T));
    // Merge by id: a second read of an overlapping range replaces, never duplicates.
    w.view_from = T - 4 * H; w.now = 2000;
    CHECK(settle(r, s, w, out));
    json more = json::array({level("a", 82462.5, T - H, T - H + 60000, 700000), level("c", 81000, T - 3 * H, T - 3 * H + 1000)});
    more.push_back(more[1]);  // a duplicate inside one reply
    CHECK(s.receive(reply(out.id, more), 2600) == Receipt::accepted);
    CHECK(m->levels.size() == 3);
    for (const auto& l : m->levels)
        if (l.id == "a") CHECK(l.executed_usd == 700000);
    // Go's null slice reads as empty; coverage 0 means never recorded.
    Store n;
    Requester rn;
    CHECK(settle(rn, n, want(T - H, T, 0), out));
    json empty = reply(out.id, nullptr, 0);
    CHECK(n.receive(empty, 600) == Receipt::accepted);
    const Market* nm = n.find("binancef", "btcusdt", false);
    CHECK(nm && nm->levels.empty() && nm->coverage_known && nm->coverage_from_ms == 0);
    // A market never recorded is probed at most once a minute, wherever the view goes.
    CHECK(settle(rn, n, want(T - 5 * H, T - 4 * H, 1000), out));
    n.receive(reply(out.id, nullptr, 0), 1600);
    CHECK(!settle(rn, n, want(T - 9 * H, T - 8 * H, 5000), out));
    CHECK(settle(rn, n, want(T - 9 * H, T - 8 * H, 1000 + kUnrecordedProbeMs), out));
    // Malformed envelopes back off.
    json broken = reply(out.id);
    broken["levels"] = "x";
    CHECK(n.receive(broken, 70000) == Receipt::malformed);
    CHECK(n.find("binancef", "btcusdt", false)->retry_at == 70000 + kRetryMs);
}

static void test_truncated_refine() {
    Store s;
    Requester r;
    Request out;
    Want week = want(T - 6 * D, T, 0, T);
    week.floor = 0;
    CHECK(settle(r, s, week, out));
    const int64_t width = out.to - out.from;
    CHECK(s.receive(reply(out.id, json::array({level("big", 80000, T - 2 * D, T - 2 * D + 1000, 9e6)}), T - 30 * D, true), 600) == Receipt::accepted);
    const Market* m = s.find("binancef", "btcusdt", false);
    CHECK(truncated_in(*m, T - 3 * D, T - D));
    CHECK(m->covered.size() == 1 && m->covered[0].truncated == width);
    // Zoomed in less than 4x: the truncated read stands.
    Want zoom = want(T - 3 * D, T - D, 1000, T);
    zoom.floor = 0;
    CHECK(!settle(r, s, zoom, out));
    // Zoomed in further: the narrower range is asked again and, complete, replaces the truncated part.
    Want close = want(T - 2 * D - H, T - 2 * D + H, 2000, T);
    close.floor = 0;
    CHECK(settle(r, s, close, out));
    CHECK(out.from == close.view_from - H && out.to == close.view_to + H / 2 + H);
    CHECK(s.receive(reply(out.id), 2600) == Receipt::accepted);
    CHECK(!truncated_in(*m, close.view_from, close.view_to));
    CHECK(truncated_in(*m, T - 5 * D, T - 4 * D));
    CHECK(m->covered.size() == 3 && m->covered[1].truncated == 0);
}

static void test_errors() {
    Store s;
    Requester r;
    Request out;
    Want w = want(T - H, T, 0);
    CHECK(settle(r, s, w, out));
    // busy: back off 2 s, then ask the same range again.
    CHECK(s.receive(reply(out.id, json::array(), 0, false, "busy"), 600) == Receipt::unavailable);
    const Market* m = s.find("binancef", "btcusdt", false);
    CHECK(m->error == Error::none && m->retry_at == 600 + kBusyBackoffMs);
    w.now = 2000;
    CHECK(!r.step(s, w, out));
    w.now = 2700;
    CHECK(r.step(s, w, out));
    // context_changed: the same, once the back-off has passed.
    CHECK(s.receive(reply(out.id, json::array(), 0, false, "context_changed"), 2800) == Receipt::unavailable);
    w.now = 4900;
    CHECK(r.step(s, w, out));
    // source_unavailable waits longer and is remembered for the row.
    CHECK(s.receive(reply(out.id, json::array(), 0, false, "source_unavailable"), 5000) == Receipt::unavailable);
    CHECK(m->error == Error::source_unavailable && m->retry_at == 5000 + kRetryMs);
    w.now = 5000 + kRetryMs;
    CHECK(r.step(s, w, out));
    // outside_history_window: a note, and the range is never asked again.
    CHECK(s.receive(reply(out.id, json::array(), 0, false, "outside_history_window"), w.now) == Receipt::unavailable);
    CHECK(m->error == Error::outside_history_window);
    w.now += 10000;
    CHECK(!r.step(s, w, out));
    // pro_required: the locked row; no more reads until the socket changes.
    Store p;
    Requester rp;
    Want eth = want(T - H, T, 0, T, false, "ethusdt");
    CHECK(settle(rp, p, eth, out));
    json refused = reply(out.id, json::array(), 0, false, "pro_required");
    refused["symbol"] = "ethusdt";
    CHECK(p.receive(refused, 600) == Receipt::unavailable);
    CHECK(p.find("binancef", "ethusdt", false)->error == Error::pro_required);
    eth.view_from -= H; eth.now = 100000;
    CHECK(!settle(rp, p, eth, out));
    p.clear_all();
    CHECK(p.find("binancef", "ethusdt", false) && p.find("binancef", "ethusdt", false)->error == Error::none);
    eth.now = 200000;
    CHECK(settle(rp, p, eth, out));
    // A reply for another market under this id is refused, and the market backs off.
    CHECK(p.receive(reply(out.id), 200600) == Receipt::malformed);
    eth.now = 200600 + kRetryMs - 1;
    CHECK(!rp.step(p, eth, out));
    // No reply at all: dropped after the timeout, the market waits, then asks again.
    eth.now = 200600 + kRetryMs;
    CHECK(rp.step(p, eth, out));
    const double sent = eth.now;
    eth.now = sent + kTimeoutMs - 1;
    CHECK(!rp.step(p, eth, out) && p.pending_count() == 1);
    eth.now = sent + kTimeoutMs + 1;
    CHECK(!rp.step(p, eth, out) && p.pending_count() == 0);
    CHECK(p.find("binancef", "ethusdt", false)->retry_at == eth.now + kRetryMs);
    eth.now += kRetryMs;
    CHECK(rp.step(p, eth, out));
}

static void test_resets() {
    Store s;
    Requester live, replay;
    Request lo, ro;
    CHECK(settle(live, s, want(T - H, T, 0), lo));
    s.receive(reply(lo.id, json::array({level("l1", 80000, T - 30 * 60000, T - 29 * 60000)})), 600);
    Want rw = want(T - 5 * H, T - 4 * H, 0, T - 4 * H, true);
    rw.floor = T - 6 * H;
    CHECK(settle(replay, s, rw, ro));
    CHECK(ro.to == T - 4 * H && ro.from == T - 11 * H / 2);  // never past the playhead or before the replay start
    Want early = rw;
    early.view_from = T - 8 * H; early.view_to = T - 7 * H;
    Requester before;
    CHECK(!settle(before, s, early, ro) && before.before_floor());  // all before the replay start: nothing asked
    before.reset(s);
    json rep = reply(ro.id, json::array({level("r1", 80000, T - 4 * H - 120000, T - 4 * H - 60000),
                                         level("late", 80000, T - 4 * H - 60000, T - 4 * H + 1)}),
                     T - 30 * D, false, "", "replay");
    CHECK(s.receive(rep, 600) == Receipt::accepted);
    const Market* rm = s.find("binancef", "btcusdt", true);
    CHECK(rm && rm->levels.size() == 1);  // a close past the playhead it was asked at is dropped
    // A rewind or seek drops replay reads only.
    s.clear_replay();
    rm = s.find("binancef", "btcusdt", true);
    CHECK(rm && rm->levels.empty() && rm->covered.empty() && rm->owners.size() == 1);
    CHECK(s.find("binancef", "btcusdt", false)->levels.size() == 1);
    // The replay chart asks again from scratch.
    rw.now = 2000;
    CHECK(settle(replay, s, rw, ro));
    // A transport change drops everything, including the read in flight.
    s.clear_all();
    CHECK(s.pending_count() == 0 && s.find("binancef", "btcusdt", false)->levels.empty());
    CHECK(!replay.in_flight(s));
    CHECK(s.receive(reply(ro.id, json::array(), 0, false, "", "replay"), 3000) == Receipt::ignored);
    // A chart that changes market (or mode) leaves its old cache.
    Want eth = want(T - H, T, 4000, T, false, "ethusdt");
    Request eo;
    CHECK(settle(live, s, eth, eo));
    CHECK(s.find("binancef", "ethusdt", false) != nullptr);
    CHECK(s.find("binancef", "btcusdt", false) == nullptr);  // nobody else used it
    replay.reset(s);
    live.reset(s);
    CHECK(s.market_count() == 0 && s.pending_count() == 0);
}

static void test_eviction() {
    Store s;
    Requester r;
    Request out;
    // Fill two days of reads, the older one first, then a recent one that takes the store over its bound.
    auto fill = [&](int64_t start, int count, const std::string& tag) {
        json levels = json::array();
        for (int i = 0; i < count; ++i)
            levels.push_back(level(tag + std::to_string(i), 80000 + i, start + i * 1000, start + i * 1000 + 500, 1e5 + i));
        return levels;
    };
    Want older = want(T - 6 * D, T - 5 * D, 0, T);
    older.floor = 0;
    CHECK(settle(r, s, older, out));
    CHECK(s.receive(reply(out.id, fill(T - 6 * D + 1000, 2000, "o")), 600) == Receipt::accepted);
    Want mid = want(T - 4 * D, T - 3 * D, 1000, T);
    mid.floor = 0;
    CHECK(settle(r, s, mid, out));
    CHECK(s.receive(reply(out.id, fill(T - 4 * D + 1000, 1500, "m")), 1600) == Receipt::accepted);
    const Market* m = s.find("binancef", "btcusdt", false);
    CHECK(m->levels.size() == 3500);
    Want recent = want(T - 2 * H, T, 2000, T);
    recent.floor = 0;
    CHECK(settle(r, s, recent, out));
    CHECK(s.receive(reply(out.id, fill(T - 2 * H + 1000, 1000, "n")), 2600) == Receipt::accepted);
    CHECK(m->levels.size() == kMaxLevels);
    // The 500 that went are the oldest closes, farthest from the read just answered.
    int o = 0, mm = 0, n = 0;
    int64_t oldest = T;
    for (const auto& l : m->levels) {
        if (l.id[0] == 'o') { ++o; oldest = std::min(oldest, l.closed_ms); }
        else if (l.id[0] == 'm') ++mm;
        else ++n;
    }
    CHECK(o == 1500 && mm == 1500 && n == 1000);
    CHECK(oldest == T - 6 * D + 1000 + 500 * 1000 + 500);
    // That older range is no longer counted as read, so viewing it asks again.
    for (const auto& span : m->covered) CHECK(span.from > oldest - 1000 || span.to < older.view_from);
    older.now = 3000;
    CHECK(settle(r, s, older, out));
    CHECK(out.from == older.view_from - D / 2);
}

static void test_select() {
    Market m;
    auto add = [&](const char* id, double price, int64_t known, int64_t closed, double usd) {
        Level l;
        CHECK(parse_level(level(id, price, known, closed, usd), l));
        m.levels.push_back(l);
    };
    add("small", 100, T - 10000, T - 9000, 1e5);
    add("big", 101, T - 20000, T - 19000, 5e6);
    add("tie-late", 102, T - 5000, T - 4000, 2e6);
    add("tie-early", 103, T - 8000, T - 7000, 2e6);
    add("future", 104, T + 1000, T + 2000, 9e9);        // after the clock
    add("off-price", 500, T - 8000, T - 7000, 9e9);     // above the view
    add("before", 100, T - 3 * H, T - 3 * H + 10, 9e9); // ends before the view
    std::array<uint16_t, kMaxLevels> idx{};
    View v{T - H, T + H, 50, 200, 60000};
    size_t n = select(m, T, v, idx, 3);
    CHECK(n == 3);
    CHECK(m.levels[idx[0]].id == "big" && m.levels[idx[1]].id == "tie-early" && m.levels[idx[2]].id == "tie-late");
    // Stable: the same answer from a reordered store.
    std::swap(m.levels[2], m.levels[3]);
    n = select(m, T, v, idx, 3);
    CHECK(m.levels[idx[1]].id == "tie-early" && m.levels[idx[2]].id == "tie-late");
    // A short level drawn one candle long reaches into a view that starts just after it closed.
    View after{T - 9000 + 30000, T + H, 50, 200, 60000};
    n = select(m, T, after, idx);
    bool small = false;
    for (size_t k = 0; k < n; ++k) small |= m.levels[idx[k]].id == "small";
    CHECK(small);
    // The cap holds at 24 by default.
    Market many;
    for (int i = 0; i < 100; ++i) {
        Level l;
        CHECK(parse_level(level("x" + std::to_string(i), 100, T - 1000, T - 500, 1000.0 * i), l));
        many.levels.push_back(l);
    }
    n = select(many, T, v, idx);
    CHECK(n == kMaxDrawn && many.levels[idx[0]].executed_usd == 99000.0);
}

static void test_paint() {
    std::vector<Span> s;
    paint(s, {100, 200, 0});
    paint(s, {201, 300, 0});
    CHECK(s.size() == 1 && s[0].from == 100 && s[0].to == 300);
    paint(s, {250, 400, 50});  // truncated never overrides complete
    CHECK(s.size() == 2 && s[0].to == 300 && s[1].from == 301 && s[1].to == 400 && s[1].truncated == 50);
    paint(s, {350, 360, 0});  // complete replaces part of the truncated range
    CHECK(s.size() == 4 && s[2].from == 350 && s[2].to == 360 && s[2].truncated == 0);
    uncover(s, 0, 150);
    CHECK(s[0].from == 151);
    uncover(s, 355, 1000);
    CHECK(s.back().to == 354);
    std::vector<Span> many;
    for (int i = 0; i < 40; ++i) paint(many, {i * 10, i * 10 + 5, 0});
    CHECK(many.size() == kMaxSpans && many.back().from == 390);
}

int main() {
    test_debounce_and_request();
    test_gaps_and_clamp();
    test_edge_refresh();
    test_receive_merge_coverage();
    test_truncated_refine();
    test_errors();
    test_resets();
    test_eviction();
    test_select();
    test_paint();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("refill_history_test: all passed");
    return EXIT_SUCCESS;
}
