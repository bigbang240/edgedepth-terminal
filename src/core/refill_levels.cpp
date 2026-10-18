#include "core/refill_levels.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace refill {
namespace {
using Json = nlohmann::json;

// Typed reads that never throw: WASM builds cannot catch, so every get<> is guarded by its type test.
const Json* member(const Json& j, const char* key) {
    const auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
}
bool text(const Json& j, const char* key, const std::string*& out) {
    const Json* v = member(j, key);
    if (!v || !v->is_string()) return false;
    out = &v->get_ref<const std::string&>();
    return true;
}
bool integer(const Json& v, int64_t& out) {
    if (v.is_number_unsigned()) {
        const uint64_t u = v.get<uint64_t>();
        if (u > uint64_t(std::numeric_limits<int64_t>::max())) return false;
        out = int64_t(u);
        return true;
    }
    if (!v.is_number_integer()) return false;
    out = v.get<int64_t>();
    return true;
}
bool integer(const Json& j, const char* key, int64_t& out) {
    const Json* v = member(j, key);
    return v && integer(*v, out);
}
bool number(const Json& j, const char* key, double& out) {
    const Json* v = member(j, key);
    if (!v || !v->is_number()) return false;
    out = v->get<double>();
    return std::isfinite(out);
}
// An optional non-negative quantity: absent reads as 0, present must be valid.
bool quantity(const Json& j, const char* key, double& out) {
    out = 0;
    if (!member(j, key)) return true;
    return number(j, key, out) && out >= 0;
}
bool end_from(const std::string& s, End& out) {
    if (s == "holding") out = End::holding;
    else if (s == "crossed") out = End::crossed;
    else if (s == "pulled") out = End::pulled;
    else if (s == "exhausted") out = End::exhausted;
    else if (s == "inactive") out = End::inactive;
    else if (s == "interrupted") out = End::interrupted;
    else return false;
    return true;
}
int64_t ended_at(const Level& l) {
    return l.closed_ms > 0 ? l.closed_ms : l.updated_ms;
}
}  // namespace

bool parse_level(const Json& j, Level& out) {
    if (!j.is_object()) return false;
    const std::string *id = nullptr, *side = nullptr, *status = nullptr;
    if (!text(j, "id", id) || id->empty() || id->size() > 64) return false;
    if (!text(j, "side", side) || (*side != "bid" && *side != "ask")) return false;
    if (!text(j, "status", status) || !end_from(*status, out.status)) return false;
    out.side = *side == "bid" ? Side::bid : Side::ask;
    if (!number(j, "price", out.price) || !(out.price > 0)) return false;
    if (!integer(j, "started_ms", out.started_ms) || !integer(j, "known_ms", out.known_ms) ||
        !integer(j, "updated_ms", out.updated_ms) || !integer(j, "closed_ms", out.closed_ms)) return false;
    if (out.started_ms <= 0 || out.known_ms < out.started_ms || out.updated_ms < out.started_ms) return false;
    if (out.closed_ms != 0 && out.closed_ms < out.started_ms) return false;
    if (!number(j, "executed_usd", out.executed_usd) || out.executed_usd < 0) return false;
    if (!quantity(j, "executed_base", out.executed_base) || !quantity(j, "max_shown_base", out.max_shown_base) ||
        !quantity(j, "refilled_base", out.refilled_base) || !quantity(j, "shown_ratio", out.shown_ratio)) return false;
    int64_t refills = 0;
    if (!integer(j, "refills", refills) || refills < 0 || refills > 1000000000) return false;
    out.refills = int(refills);
    out.refill_count = 0;
    if (const Json* times = member(j, "refill_ms"); times && !times->is_null()) {  // Go writes an empty slice as null
        if (!times->is_array()) return false;
        // The server sends the most recent 32; keep the newest 32 of anything longer.
        const size_t skip = times->size() > kMaxRefillTimes ? times->size() - kMaxRefillTimes : 0;
        size_t i = 0;
        for (const auto& t : *times) {
            int64_t at = 0;
            if (!integer(t, at)) return false;
            if (i++ < skip || at <= 0) continue;
            out.refill_ms[out.refill_count++] = at;
        }
    }
    out.id = *id;
    out.current = true;
    return true;
}

Market* Store::find_mutable(std::string_view exchange, std::string_view symbol) {
    for (auto& m : markets_)
        if (m.exchange == exchange && m.symbol == symbol) return &m;
    return nullptr;
}

const Market* Store::find(std::string_view exchange, std::string_view symbol) const {
    for (const auto& m : markets_)
        if (m.exchange == exchange && m.symbol == symbol) return &m;
    return nullptr;
}

void Store::expect(std::string_view exchange, std::string_view symbol, bool replay) {
    Market* m = find_mutable(exchange, symbol);
    if (!m) {
        markets_.emplace_back();
        m = &markets_.back();
        m->exchange = std::string(exchange);
        m->symbol = std::string(symbol);
    }
    m->replay = replay;
    m->status = Status::none;
    m->reason.clear();
    m->as_of_ms = 0;
    m->levels.clear();
    ++m->revision;
}

void Store::forget(std::string_view exchange, std::string_view symbol) {
    std::erase_if(markets_, [&](const Market& m) { return m.exchange == exchange && m.symbol == symbol; });
}

void Store::clear_all() {
    for (auto& m : markets_) {
        m.status = Status::none;
        m.reason.clear();
        m.as_of_ms = 0;
        m.levels.clear();
        ++m.revision;
    }
}

void Store::merge(Market& m, Level&& level) {
    for (auto& existing : m.levels) {
        if (existing.id == level.id) { existing = std::move(level); return; }
    }
    if (m.levels.capacity() < kMaxLevels) m.levels.reserve(kMaxLevels + 1);
    m.levels.push_back(std::move(level));
    if (m.levels.size() <= kMaxLevels) return;
    // Evict the oldest closed (or no longer reported) level first; a holding one only when nothing else is left.
    size_t victim = 0;
    for (size_t i = 1; i < m.levels.size(); ++i) {
        const auto& a = m.levels[i];
        const auto& b = m.levels[victim];
        const bool a_live = a.status == End::holding && a.current;
        const bool b_live = b.status == End::holding && b.current;
        if (a_live != b_live ? !a_live : ended_at(a) < ended_at(b)) victim = i;
    }
    m.levels.erase(m.levels.begin() + std::ptrdiff_t(victim));
}

Receipt Store::receive(const Json& j) {
    if (!j.is_object()) return Receipt::malformed;
    const std::string *type = nullptr, *exchange = nullptr, *symbol = nullptr, *mode = nullptr, *status = nullptr;
    if (text(j, "type", type) && *type != "refill_levels") return Receipt::ignored;
    if (!text(j, "exchange", exchange) || !text(j, "symbol", symbol)) return Receipt::malformed;
    Market* m = find_mutable(*exchange, *symbol);
    if (!m) return Receipt::ignored;  // not subscribed (or already left)
    // The server omits mode when it refuses before choosing one (an unsupported venue); then it applies as is.
    if (text(j, "mode", mode) && !mode->empty()) {
        if (*mode != "live" && *mode != "replay") return Receipt::malformed;
        if ((*mode == "replay") != m->replay) return Receipt::ignored;  // the other context's late message
    } else if (member(j, "mode") && !member(j, "mode")->is_string()) return Receipt::malformed;
    if (const Json* version = member(j, "version"); version && (!version->is_string() || *version != kVersion))
        return Receipt::ignored;
    int64_t as_of = 0;
    if (!text(j, "status", status) || !integer(j, "as_of_ms", as_of) || as_of < 0) return Receipt::malformed;
    const std::string* reason = nullptr;
    text(j, "reason", reason);
    if (*status == "warming" || *status == "unavailable") {
        // Warming means the engine (re)started; unavailable carries no levels. Either way start empty.
        m->levels.clear();
        m->status = *status == "warming" ? Status::warming : Status::unavailable;
        m->reason = reason ? *reason : std::string();
        m->as_of_ms = as_of;
        ++m->revision;
        return m->status == Status::warming ? Receipt::reset : Receipt::unavailable;
    }
    if (*status != "observing") return Receipt::malformed;
    const Json* levels = member(j, "levels");
    if (!levels || (!levels->is_array() && !levels->is_null())) return Receipt::malformed;
    if (m->status == Status::observing && as_of < m->as_of_ms) return Receipt::ignored;  // out of order
    for (auto& l : m->levels) l.current = false;
    size_t seen = 0;
    if (levels->is_array()) for (const auto& entry : *levels) {
        if (++seen > kMaxLevels) break;
        Level level;
        if (parse_level(entry, level)) merge(*m, std::move(level));
        else ++dropped_;
    }
    m->status = Status::observing;
    m->reason.clear();
    m->as_of_ms = as_of;
    ++m->revision;
    return Receipt::accepted;
}

// ── Subscriptions ───────────────────────────────────────────────────────────

std::string subscribe_message(std::string_view exchange, std::string_view symbol, double tick, const char* token) {
    return Json{{"method", "refill_subscribe"},
                {"data", {{"pair", {{"exchange", std::string(exchange)}, {"symbol", std::string(symbol)}}},
                          {"tick_size", tick},
                          {"entitlement_token", token ? token : ""}}}}.dump();
}

std::string unsubscribe_message(std::string_view exchange, std::string_view symbol) {
    return Json{{"method", "refill_unsubscribe"},
                {"data", {{"pair", {{"exchange", std::string(exchange)}, {"symbol", std::string(symbol)}}}}}}.dump();
}

void Subscriptions::drop_owner(const void* owner, std::vector<Message>& out) {
    for (size_t i = 0; i < entries_.size(); ++i) {
        auto& e = entries_[i];
        const auto it = std::find(e.owners.begin(), e.owners.end(), owner);
        if (it == e.owners.end()) continue;
        e.owners.erase(it);
        if (!e.owners.empty()) return;
        if (e.sent) out.push_back({e.channel, unsubscribe_message(e.exchange, e.symbol)});
        const std::string exchange = e.exchange, symbol = e.symbol;
        entries_.erase(entries_.begin() + std::ptrdiff_t(i));
        const bool still = std::any_of(entries_.begin(), entries_.end(), [&](const Entry& other) {
            return other.exchange == exchange && other.symbol == symbol;
        });
        if (!still) store_.forget(exchange, symbol);
        return;
    }
}

void Subscriptions::want(const void* owner, const Want& w, std::vector<Message>& out) {
    Entry* mine = nullptr;
    for (auto& e : entries_) {
        if (std::find(e.owners.begin(), e.owners.end(), owner) == e.owners.end()) continue;
        if (e.channel == w.channel && e.exchange == w.exchange && e.symbol == w.symbol) mine = &e;
        break;
    }
    if (!mine) {
        drop_owner(owner, out);  // held another market or channel before
        for (auto& e : entries_)
            if (e.channel == w.channel && e.exchange == w.exchange && e.symbol == w.symbol) { mine = &e; break; }
        if (!mine) {
            entries_.emplace_back();
            mine = &entries_.back();
            mine->channel = w.channel;
            mine->exchange = std::string(w.exchange);
            mine->symbol = std::string(w.symbol);
            mine->tick = w.tick;
            mine->replay = w.replay;
        }
        mine->owners.push_back(owner);
    }
    Entry& e = *mine;
    const bool stale = !e.sent || e.generation != w.generation || e.replay != w.replay ||
        (w.tick > 0 && w.tick != e.tick);
    if (!stale || !w.connected || w.tick <= 0) return;
    if (w.now_ms - e.sent_at < kResubscribeMs) return;
    e.tick = w.tick;
    e.replay = w.replay;
    e.generation = w.generation;
    e.sent = true;
    e.sent_at = w.now_ms;
    out.push_back({e.channel, subscribe_message(e.exchange, e.symbol, e.tick, w.token ? w.token() : "")});
    store_.expect(e.exchange, e.symbol, e.replay);
}

void Subscriptions::release(const void* owner, std::vector<Message>& out) {
    drop_owner(owner, out);
}

void Subscriptions::transport_changed() {
    for (auto& e : entries_) e.sent = false;
}

void Subscriptions::resubscribe(std::string_view exchange, std::string_view symbol) {
    for (auto& e : entries_)
        if (e.exchange == exchange && e.symbol == symbol) e.sent = false;
}

size_t Subscriptions::owners(std::string_view exchange, std::string_view symbol) const {
    size_t n = 0;
    for (const auto& e : entries_)
        if (e.exchange == exchange && e.symbol == symbol) n += e.owners.size();
    return n;
}

Store& store() {
    static Store s;
    return s;
}

Subscriptions& subscriptions() {
    static Subscriptions s(store());
    return s;
}

Receipt receive(const Json& j) {
    const Receipt r = store().receive(j);
    const std::string *exchange = nullptr, *symbol = nullptr, *reason = nullptr;
    if (r == Receipt::unavailable && text(j, "reason", reason) && *reason == "context_changed" &&
        text(j, "exchange", exchange) && text(j, "symbol", symbol))
        subscriptions().resubscribe(*exchange, *symbol);
    return r;
}

void on_transport_changed() {
    store().clear_all();
    subscriptions().transport_changed();
}

void on_rewind() {
    store().clear_all();
}

// ── Display helpers ─────────────────────────────────────────────────────────

bool span_at(const Level& l, const Market& m, int64_t clock, Span& out) {
    if (l.known_ms > clock) return false;  // not knowable yet at this clock
    out.start = l.started_ms;
    out.known = std::max(l.known_ms, l.started_ms);
    const bool closed = l.status != End::holding;
    if (closed && ended_at(l) <= clock) {
        out.end = ended_at(l);
        out.end_status = l.status;
        out.open = false;
    } else if (l.current && m.status == Status::observing) {
        out.end = std::min(m.as_of_ms, clock);
        out.end_status = End::holding;
        out.open = true;
    } else {
        out.end = std::min(l.updated_ms, clock);
        out.end_status = End::inactive;
        out.open = false;
    }
    if (out.end < out.known) out.end = out.known;
    return true;
}

size_t select(const Market& m, int64_t clock, const View& v, std::array<uint16_t, kMaxLevels>& idx, size_t cap) {
    std::array<int64_t, kMaxLevels> rank{};
    size_t n = 0;
    for (size_t i = 0; i < m.levels.size() && n < kMaxLevels; ++i) {
        const Level& l = m.levels[i];
        Span s;
        if (!span_at(l, m, clock, s)) continue;
        if (double(s.end) < v.t0 || double(s.start) > v.t1 || l.price < v.p0 || l.price > v.p1) continue;
        // Holding first, then the most recent end.
        rank[i] = (s.open ? (int64_t(1) << 62) : 0) + s.end;
        idx[n++] = uint16_t(i);
    }
    const size_t keep = std::min(n, cap);
    std::partial_sort(idx.begin(), idx.begin() + std::ptrdiff_t(keep), idx.begin() + std::ptrdiff_t(n),
        [&](uint16_t a, uint16_t b) { return rank[a] != rank[b] ? rank[a] > rank[b] : a < b; });
    return keep;
}

float rail_width(double executed_usd, double max_usd) {
    if (!(max_usd > 0)) return 2.0f;
    const double r = executed_usd / max_usd;
    return r >= 0.5 ? 4.5f : r >= 0.2 ? 3.0f : 2.0f;
}

const char* end_word(End e) {
    switch (e) {
        case End::holding: return "holding";
        case End::crossed: return "crossed";
        case End::pulled: return "pulled";
        case End::exhausted: return "exhausted";
        case End::inactive: return "inactive";
        case End::interrupted: return "interrupted";
    }
    return "";
}

void format_usd(double v, char* out, size_t n) {
    if (!std::isfinite(v) || v < 0) { std::snprintf(out, n, "-"); return; }
    if (v >= 999.95e6) std::snprintf(out, n, "$%.1fB", v / 1e9);
    else if (v >= 999.5e3) std::snprintf(out, n, "$%.1fM", v / 1e6);
    else if (v >= 99.95e3) std::snprintf(out, n, "$%.0fk", v / 1e3);
    else if (v >= 1e3) std::snprintf(out, n, "$%.1fk", v / 1e3);
    else std::snprintf(out, n, "$%.0f", v);
}

void hover_title(const Level& l, const char* price, char* out, size_t n) {
    std::snprintf(out, n, "%s refilled %d\xc3\x97 at %s", l.side == Side::bid ? "Bid" : "Ask", l.refills, price);
}

void hover_values(const Level& l, char* out, size_t n) {
    char traded[24], shown[24];
    format_usd(l.executed_usd, traded, sizeof(traded));
    format_usd(l.max_shown_base * l.price, shown, sizeof(shown));
    std::snprintf(out, n, "%s traded vs %s shown", traded, shown);
}

const char* unavailable_note(std::string_view reason) {
    if (reason == "pro_required" || reason == "context_changed") return nullptr;
    if (reason == "unsupported_market") return "Refill levels: not on this market";
    if (reason == "invalid_grid") return "Refill levels: price grid unavailable";
    if (reason == "outside_replay_window") return "Refill levels: outside this replay";
    if (reason == "session_limit") return "Refill levels: two markets at a time";
    return "Refill levels unavailable";
}

}  // namespace refill
