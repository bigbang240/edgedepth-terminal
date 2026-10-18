#include "core/refill_history.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace refill::history {
namespace {
using Json = nlohmann::json;

// Typed reads that never throw (WASM builds cannot catch). Level fields are parsed by refill::parse_level.
const Json* member(const Json& j, const char* key) {
    const auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
}
const std::string* text(const Json& j, const char* key) {
    const Json* v = member(j, key);
    return v && v->is_string() ? &v->get_ref<const std::string&>() : nullptr;
}
bool integer(const Json& j, const char* key, int64_t& out) {
    const Json* v = member(j, key);
    if (!v) return false;
    if (v->is_number_unsigned()) {
        const uint64_t u = v->get<uint64_t>();
        if (u > uint64_t(std::numeric_limits<int64_t>::max())) return false;
        out = int64_t(u);
        return true;
    }
    if (!v->is_number_integer()) return false;
    out = v->get<int64_t>();
    return true;
}

uint64_t hash_id(std::string_view s) {
    uint64_t h = 1469598103934665603ULL;  // FNV-1a
    for (const char c : s) { h ^= uint8_t(c); h *= 1099511628211ULL; }
    return h;
}

// Which reads count as answered for a view of this width: complete ones, and truncated ones no wider than kRefine
// views (zooming further into a truncated read asks for the narrower range, which may hold more).
bool counts(const Span& s, int64_t width) {
    return s.truncated == 0 || s.truncated <= kRefine * width;
}

// The end of an answered or pending range holding t, or no value.
bool covering(const Market& m, const std::vector<Pending>& pending, int64_t t, int64_t width, int64_t& from,
              int64_t& to) {
    for (const auto& s : m.covered) {
        if (s.from <= t && t <= s.to && counts(s, width)) { from = s.from; to = s.to; return true; }
    }
    for (const auto& p : pending) {
        if (p.replay == m.replay && p.exchange == m.exchange && p.symbol == m.symbol && p.from <= t && t <= p.to) {
            from = p.from; to = p.to;
            return true;
        }
    }
    return false;
}

// The first to the last point of [left, right] that nothing answered or asked covers; `width` is the view's need,
// which decides whether a truncated read counts.
bool gap(const Market& m, const std::vector<Pending>& pending, int64_t left, int64_t right, int64_t width,
         int64_t& g0, int64_t& g1) {
    int64_t from = 0, to = 0;
    g0 = left;
    while (g0 <= right && covering(m, pending, g0, width, from, to)) g0 = to + 1;
    if (g0 > right) return false;
    g1 = right;
    while (g1 >= g0 && covering(m, pending, g1, width, from, to)) g1 = from - 1;
    return g1 >= g0;
}
}  // namespace

Error error_from(std::string_view code) {
    if (code == "pro_required") return Error::pro_required;
    if (code == "outside_history_window") return Error::outside_history_window;
    if (code == "unsupported_market") return Error::unsupported_market;
    if (code == "invalid_range") return Error::invalid_range;
    if (code == "busy") return Error::busy;
    if (code == "source_unavailable") return Error::source_unavailable;
    if (code == "context_changed") return Error::context_changed;
    return Error::other;
}

// ── Covered ranges ──────────────────────────────────────────────────────────

void paint(std::vector<Span>& spans, Span s) {
    if (s.to < s.from) return;
    // Elementary pieces between every boundary; each takes the best read covering it (complete, else the
    // narrowest truncated one). Called once per reply, over at most kMaxSpans + 1 ranges.
    std::vector<int64_t> cuts;
    cuts.reserve(spans.size() * 2 + 2);
    for (const auto& e : spans) { cuts.push_back(e.from); cuts.push_back(e.to + 1); }
    cuts.push_back(s.from);
    cuts.push_back(s.to + 1);
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    std::vector<Span> out;
    out.reserve(cuts.size());
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        const int64_t a = cuts[i], b = cuts[i + 1] - 1;
        bool any = false;
        int64_t best = 0;
        auto take = [&](const Span& e) {
            if (e.from > a || e.to < b) return;
            if (!any || (e.truncated == 0 ? best != 0 : best != 0 && e.truncated < best)) best = e.truncated;
            any = true;
        };
        for (const auto& e : spans) take(e);
        take(s);
        if (!any) continue;
        if (!out.empty() && out.back().to + 1 == a && out.back().truncated == best) out.back().to = b;
        else out.push_back({a, b, best});
    }
    // Bounded: drop the ranges farthest from the one just answered.
    while (out.size() > kMaxSpans) {
        auto distance = [&](const Span& e) { return e.to < s.from ? s.from - e.to : e.from > s.to ? e.from - s.to : 0; };
        const auto far = std::max_element(out.begin(), out.end(),
            [&](const Span& x, const Span& y) { return distance(x) < distance(y); });
        out.erase(far);
    }
    spans = std::move(out);
}

void uncover(std::vector<Span>& spans, int64_t from, int64_t to) {
    if (to < from) return;
    std::vector<Span> out;
    out.reserve(spans.size() + 1);
    for (const auto& e : spans) {
        if (e.to < from || e.from > to) { out.push_back(e); continue; }
        if (e.from < from) out.push_back({e.from, from - 1, e.truncated});
        if (e.to > to) out.push_back({to + 1, e.to, e.truncated});
    }
    spans = std::move(out);
}

// ── Store ───────────────────────────────────────────────────────────────────

Market* Store::find_mutable(std::string_view exchange, std::string_view symbol, bool replay) {
    for (auto& m : markets_)
        if (m.replay == replay && m.exchange == exchange && m.symbol == symbol) return &m;
    return nullptr;
}

const Market* Store::find(std::string_view exchange, std::string_view symbol, bool replay) const {
    for (const auto& m : markets_)
        if (m.replay == replay && m.exchange == exchange && m.symbol == symbol) return &m;
    return nullptr;
}

Market& Store::open(const void* owner, std::string_view exchange, std::string_view symbol, bool replay) {
    Market* m = find_mutable(exchange, symbol, replay);
    if (!m) {
        markets_.emplace_back();
        m = &markets_.back();
        m->exchange = std::string(exchange);
        m->symbol = std::string(symbol);
        m->replay = replay;
    }
    if (std::find(m->owners.begin(), m->owners.end(), owner) == m->owners.end()) m->owners.push_back(owner);
    return *m;
}

void Store::release(const void* owner) {
    std::erase_if(pending_, [&](const Pending& p) { return p.owner == owner; });
    for (auto& m : markets_) std::erase(m.owners, owner);
    std::erase_if(markets_, [](const Market& m) { return m.owners.empty(); });
}

bool Store::pending(std::string_view id) const {
    for (const auto& p : pending_)
        if (p.id == id) return true;
    return false;
}

bool Store::plan(const void* owner, const Want& w, Request& out) {
    Market* m = find_mutable(w.exchange, w.symbol, w.replay);
    if (!m || w.now < m->retry_at) return false;
    // Refused for this viewer: the chart shows the locked row (or nothing) until the context or socket changes.
    if (m->error == Error::pro_required || m->error == Error::unsupported_market) return false;
    const int64_t span = w.view_to - w.view_from;
    if (span <= 0 || w.clock <= 0) return false;
    const int64_t earliest = m->coverage_known && m->coverage_from_ms > 0 ? std::max(w.floor, m->coverage_from_ms)
                                                                          : w.floor;
    // What the view needs: levels known in it can close after its right edge, so a little past it.
    const int64_t right = std::min(w.view_to + std::min(span / 4, kMaxPadMs), w.clock);
    int64_t left = std::max(w.view_from, earliest);
    left = std::max(left, right - kMaxRangeMs);  // the server's 7 days, kept at the right end of the view
    if (right <= left) return false;
    const bool unrecorded = m->coverage_known && m->coverage_from_ms == 0;
    if (unrecorded && w.now - m->probe_at < kUnrecordedProbeMs) return false;
    const int64_t width = right - left;
    int64_t g0 = 0, g1 = 0;
    if (!gap(*m, pending_, left, right, width, g0, g1)) return false;
    // Only the clock's edge is new: read it at most every kEdgeRefreshMs.
    if (m->edge_at > kNever && g0 > m->edge_to && w.now - m->edge_at < kEdgeRefreshMs) return false;
    // Something in view is missing: read up to half a view more on each side (within the 7 days), so panning a
    // little needs nothing new.
    const int64_t extra = std::min(span / 2, (kMaxRangeMs - width) / 2);
    const int64_t fetch_right = std::min(right + extra, w.clock);
    const int64_t fetch_left = std::max({left - extra, earliest, fetch_right - kMaxRangeMs});
    gap(*m, pending_, fetch_left, fetch_right, width, g0, g1);
    out.id = "rh-" + std::to_string(++serial_);
    out.from = g0;
    out.to = g1;
    Pending p;
    p.id = out.id;
    p.owner = owner;
    p.exchange = m->exchange;
    p.symbol = m->symbol;
    p.replay = m->replay;
    p.from = g0;
    p.to = g1;
    p.sent_at = w.now;
    p.edge = g1 == w.clock;
    pending_.push_back(std::move(p));
    if (unrecorded) m->probe_at = w.now;
    return true;
}

void Store::merge(Market& m, std::vector<Level>& incoming, int64_t from, int64_t to) {
    // Merge by id through a hash of the ids (a reply is up to 2,000 levels into up to 4,000).
    std::unordered_map<uint64_t, uint32_t> index;
    index.reserve(m.levels.size() + incoming.size());
    for (size_t i = 0; i < m.levels.size(); ++i) index.emplace(hash_id(m.levels[i].id), uint32_t(i));
    for (auto& level : incoming) {
        const uint64_t h = hash_id(level.id);
        const auto it = index.find(h);
        size_t at = it != index.end() ? it->second : m.levels.size();
        if (at < m.levels.size() && m.levels[at].id != level.id) {  // a hash collision: look the slow way
            at = m.levels.size();
            for (size_t i = 0; i < m.levels.size(); ++i)
                if (m.levels[i].id == level.id) { at = i; break; }
        }
        if (at < m.levels.size()) { m.levels[at] = std::move(level); continue; }
        if (it == index.end()) index.emplace(h, uint32_t(m.levels.size()));
        m.levels.push_back(std::move(level));
    }
    if (m.levels.size() <= kMaxLevels) return;
    // Over the bound: drop the levels that closed farthest from the range just read (with the live edge in view,
    // the oldest closes go first), the smallest first among equals, and forget that those ranges were read.
    const size_t excess = m.levels.size() - kMaxLevels;
    auto distance = [&](const Level& l) {
        return l.closed_ms < from ? from - l.closed_ms : l.closed_ms > to ? l.closed_ms - to : int64_t(0);
    };
    std::vector<uint32_t> order(m.levels.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = uint32_t(i);
    std::nth_element(order.begin(), order.begin() + std::ptrdiff_t(excess - 1), order.end(), [&](uint32_t a, uint32_t b) {
        const int64_t da = distance(m.levels[a]), db = distance(m.levels[b]);
        return da != db ? da > db : m.levels[a].executed_usd < m.levels[b].executed_usd;
    });
    std::vector<uint8_t> victim(m.levels.size(), 0);
    int64_t older = std::numeric_limits<int64_t>::min(), newer = std::numeric_limits<int64_t>::max();
    for (size_t k = 0; k < excess; ++k) {
        const Level& l = m.levels[order[k]];
        victim[order[k]] = 1;
        if (l.closed_ms < from) older = std::max(older, l.closed_ms);
        else if (l.closed_ms > to) newer = std::min(newer, l.closed_ms);
    }
    size_t keep = 0;
    for (size_t i = 0; i < m.levels.size(); ++i) {
        if (victim[i]) continue;
        if (keep != i) m.levels[keep] = std::move(m.levels[i]);
        ++keep;
    }
    m.levels.resize(keep);
    if (older != std::numeric_limits<int64_t>::min()) uncover(m.covered, std::numeric_limits<int64_t>::min(), older);
    if (newer != std::numeric_limits<int64_t>::max()) uncover(m.covered, newer, std::numeric_limits<int64_t>::max());
}

Receipt Store::receive(const Json& j, double now) {
    if (!j.is_object()) return Receipt::malformed;
    if (const std::string* type = text(j, "type"); type && *type != "refill_history") return Receipt::ignored;
    const std::string* id = text(j, "request_id");
    if (!id) return Receipt::malformed;
    const auto pit = std::find_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.id == *id; });
    if (pit == pending_.end()) return Receipt::ignored;  // released, cleared or timed out meanwhile
    const Pending p = std::move(*pit);
    pending_.erase(pit);
    Market* m = find_mutable(p.exchange, p.symbol, p.replay);
    if (!m) return Receipt::ignored;
    const std::string *exchange = text(j, "exchange"), *symbol = text(j, "symbol");
    if ((exchange && *exchange != p.exchange) || (symbol && *symbol != p.symbol)) {
        m->retry_at = now + kRetryMs;
        return Receipt::malformed;
    }
    // The other context's answer (a replay started or stopped meanwhile): the range is simply asked again.
    if (const std::string* mode = text(j, "mode"); mode && !mode->empty() && (*mode == "replay") != p.replay)
        return Receipt::ignored;
    if (const Json* version = member(j, "version"); version && (!version->is_string() || *version != kVersion)) {
        m->retry_at = now + kRetryMs;
        return Receipt::ignored;
    }
    if (const std::string* error = text(j, "error"); error && !error->empty()) {
        const Error e = error_from(*error);
        switch (e) {
            case Error::busy:
            case Error::context_changed:
                // Transient: ask again after the back-off, keeping whatever the chart already shows.
                m->retry_at = now + kBusyBackoffMs;
                return Receipt::unavailable;
            case Error::outside_history_window:
                // Nothing there for this viewer: never ask for the range again.
                paint(m->covered, {p.from, p.to, 0});
                m->retry_at = now + kBusyBackoffMs;
                break;
            case Error::invalid_range:
                paint(m->covered, {p.from, p.to, 0});
                m->retry_at = now + kInvalidBackoffMs;
                break;
            case Error::pro_required:
            case Error::unsupported_market:
                break;  // plan() stops asking until the context or socket changes
            default:
                m->retry_at = now + kRetryMs;
                break;
        }
        m->error = e;
        ++m->revision;
        return Receipt::unavailable;
    }
    int64_t coverage = 0;
    const bool has_coverage = member(j, "coverage_from_ms") != nullptr;
    if (has_coverage && (!integer(j, "coverage_from_ms", coverage) || coverage < 0)) {
        m->retry_at = now + kRetryMs;
        return Receipt::malformed;
    }
    bool truncated = false;
    if (const Json* t = member(j, "truncated"); t && !t->is_null()) {
        if (!t->is_boolean()) { m->retry_at = now + kRetryMs; return Receipt::malformed; }
        truncated = t->get<bool>();
    }
    const Json* levels = member(j, "levels");
    if (!levels || (!levels->is_array() && !levels->is_null())) {  // Go writes an empty slice as null
        m->retry_at = now + kRetryMs;
        return Receipt::malformed;
    }
    std::vector<Level> incoming;
    if (levels->is_array()) {
        incoming.reserve(std::min(levels->size(), kMaxLevels));
        for (const auto& entry : *levels) {
            if (incoming.size() >= kMaxLevels) { ++dropped_; continue; }
            Level level;
            // Recorded levels are closed; a replay read never reaches past the playhead it was asked at.
            if (!parse_level(entry, level) || level.status == End::holding || level.closed_ms <= 0 ||
                (p.replay && level.closed_ms > p.to)) { ++dropped_; continue; }
            level.current = false;
            incoming.push_back(std::move(level));
        }
    }
    // The answered range: what was asked, narrowed to what the server says it read.
    int64_t from = p.from, to = p.to, echoed = 0;
    if (integer(j, "from_ms", echoed) && echoed > 0) from = std::max(from, echoed);
    if (integer(j, "to_ms", echoed) && echoed > 0) to = std::min(to, echoed);
    merge(*m, incoming, from, to);
    if (to >= from) paint(m->covered, {from, to, truncated ? std::max<int64_t>(1, p.to - p.from) : 0});
    if (has_coverage) {
        m->coverage_from_ms = coverage;
        m->coverage_known = true;
    }
    if (p.edge) {
        m->edge_to = p.to;
        m->edge_at = p.sent_at;
    }
    m->error = Error::none;
    ++m->revision;
    return Receipt::accepted;
}

void Store::expire(double now) {
    for (size_t i = 0; i < pending_.size();) {
        if (now - pending_[i].sent_at <= kTimeoutMs) { ++i; continue; }
        if (Market* m = find_mutable(pending_[i].exchange, pending_[i].symbol, pending_[i].replay))
            m->retry_at = now + kRetryMs;
        pending_.erase(pending_.begin() + std::ptrdiff_t(i));
    }
}

// Empty a market's cache and forget every refusal and back-off; its identity and owners stay registered.
static void reset_cache(Market& m) {
    Market fresh;
    fresh.exchange = std::move(m.exchange);
    fresh.symbol = std::move(m.symbol);
    fresh.replay = m.replay;
    fresh.owners = std::move(m.owners);
    fresh.revision = m.revision + 1;
    m = std::move(fresh);
}

void Store::clear_all() {
    pending_.clear();
    for (auto& m : markets_) reset_cache(m);
}

void Store::clear_replay() {
    std::erase_if(pending_, [](const Pending& p) { return p.replay; });
    for (auto& m : markets_)
        if (m.replay) reset_cache(m);
}

// ── Requester ───────────────────────────────────────────────────────────────

bool Requester::step(Store& s, const Want& w, Request& out) {
    if (registered_ && (w.replay != replay_ || w.exchange != exchange_ || w.symbol != symbol_)) reset(s);
    if (!registered_) {
        exchange_ = std::string(w.exchange);
        symbol_ = std::string(w.symbol);
        replay_ = w.replay;
        registered_ = true;
        key_q_ = 0;
    }
    s.open(this, w.exchange, w.symbol, w.replay);  // again after a clear; cheap when registered
    s.expire(w.now);
    before_floor_ = w.view_from < w.floor;
    // The view must rest: a key that moves only when an edge crosses a 1/64-of-the-view step.
    const int64_t q = std::max<int64_t>(1, (w.view_to - w.view_from) / 64);
    auto bucket = [q](int64_t t) { return t >= 0 ? t / q : -((-t + q - 1) / q); };
    const int64_t kf = bucket(w.view_from), kt = bucket(w.view_to);
    if (q != key_q_ || kf != key_from_ || kt != key_to_) {
        key_q_ = q;
        key_from_ = kf;
        key_to_ = kt;
        changed_at_ = w.now;
        return false;
    }
    if (w.now - changed_at_ < kSettleMs) return false;
    if (in_flight(s)) return false;  // one read per chart
    id_.clear();
    if (!s.plan(this, w, out)) return false;
    id_ = out.id;
    return true;
}

void Requester::reset(Store& s) {
    if (!registered_) return;
    s.release(this);
    registered_ = false;
    before_floor_ = false;
    id_.clear();
    key_q_ = 0;
}

std::string request_message(const Request& r, std::string_view exchange, std::string_view symbol, const char* token) {
    return Json{{"method", "get_refill_history"},
                {"data", {{"request_id", r.id},
                          {"pair", {{"exchange", std::string(exchange)}, {"symbol", std::string(symbol)}}},
                          {"from_ms", r.from},
                          {"to_ms", r.to},
                          {"entitlement_token", token ? token : ""}}}}.dump();
}

// ── Display ─────────────────────────────────────────────────────────────────

size_t select(const Market& m, int64_t clock, const View& v, std::array<uint16_t, kMaxLevels>& idx, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < m.levels.size() && n < kMaxLevels; ++i) {
        const Level& l = m.levels[i];
        if (l.known_ms > clock || l.closed_ms > clock) continue;  // never ahead of the chart's clock
        const int64_t end = std::max(l.closed_ms, l.known_ms + v.min_len);
        if (end < v.t0 || l.known_ms > v.t1 || l.price < v.p0 || l.price > v.p1) continue;
        idx[n++] = uint16_t(i);
    }
    const size_t keep = std::min(n, cap);
    std::partial_sort(idx.begin(), idx.begin() + std::ptrdiff_t(keep), idx.begin() + std::ptrdiff_t(n),
        [&](uint16_t a, uint16_t b) {
            const Level& x = m.levels[a];
            const Level& y = m.levels[b];
            if (x.executed_usd != y.executed_usd) return x.executed_usd > y.executed_usd;
            if (x.known_ms != y.known_ms) return x.known_ms < y.known_ms;
            if (x.price != y.price) return x.price < y.price;
            return x.id < y.id;
        });
    return keep;
}

bool truncated_in(const Market& m, int64_t t0, int64_t t1) {
    for (const auto& s : m.covered)
        if (s.truncated > 0 && s.to >= t0 && s.from <= t1) return true;
    return false;
}

Store& store() {
    static Store s;
    return s;
}

Receipt receive(const Json& j, double now) {
    return store().receive(j, now);
}

void on_transport_changed() {
    store().clear_all();
}

void on_rewind() {
    store().clear_replay();
}

}  // namespace refill::history
