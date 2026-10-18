#pragma once
// Recorded refill levels for candle charts: the reply store for {"type":"refill_history"} (server method
// get_refill_history, the backend refill levels contract, "Recorded history") and the per-chart request planner.
// Levels are the live message's RefillLevel (core/refill_levels.h parses them), all closed.
//
// Main thread only, like core/refill_levels.h: replies are handled in main.cpp on_ws_message and charts read the
// store while rendering. No Emscripten or ImGui here: tests/native/refill_history_test.cpp compiles this unit.
#include "core/refill_levels.h"

#include <nlohmann/json.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace refill::history {

inline constexpr size_t kMaxLevels = 4000;                    // merged per market (a reply carries at most 2,000)
inline constexpr size_t kMaxDrawn = 24;                       // levels drawn per chart
inline constexpr size_t kMaxSpans = 32;                       // answered ranges remembered per market
inline constexpr int64_t kMaxRangeMs = 7LL * 24 * 3600 * 1000; // the server's longest read
inline constexpr int64_t kMaxPadMs = 6LL * 3600 * 1000;       // read past the right edge for levels closing later
inline constexpr int64_t kRefine = 4;                         // a truncated read wider than 4 views is asked again
inline constexpr int64_t kLiveMarginMs = 5000;                // live to_ms stays this far behind the client clock
inline constexpr double kSettleMs = 500;                      // the view must rest this long before a read
inline constexpr double kEdgeRefreshMs = 30000;               // the clock's edge is read again at most this often
inline constexpr double kBusyBackoffMs = 2000;                // busy, context_changed, outside_history_window
inline constexpr double kInvalidBackoffMs = 5000;             // invalid_range
inline constexpr double kRetryMs = 15000;                     // no reply, source_unavailable, anything unexpected
inline constexpr double kUnrecordedProbeMs = 60000;           // a market never recorded is asked at most this often
inline constexpr double kTimeoutMs = 15000;                   // a request with no reply by then is dropped
inline constexpr double kNever = -1e18;

enum class Error : uint8_t {
    none, pro_required, outside_history_window, unsupported_market, invalid_range, busy, source_unavailable,
    context_changed, other,
};
Error error_from(std::string_view code);

// A closed_ms range the server answered, inclusive. `truncated` is that read's width when it kept only the
// largest levels (0 = everything in the range is here).
struct Span {
    int64_t from = 0, to = 0;
    int64_t truncated = 0;
};

struct Market {
    std::string exchange, symbol;
    bool replay = false;                 // live and replay reads are kept apart
    std::vector<Level> levels;           // closed levels merged by id, at most kMaxLevels
    std::vector<Span> covered;           // sorted, disjoint, at most kMaxSpans
    int64_t coverage_from_ms = 0;        // earliest recorded close; 0 = never recorded (when coverage_known)
    bool coverage_known = false;
    Error error = Error::none;           // the last refusal that matters to the chart; none after a good reply
    int64_t edge_to = 0;                 // to_ms of the last answered read that reached the clock
    double edge_at = kNever;             // when that read was sent (steady ms)
    double probe_at = kNever;            // last read of a market never recorded
    double retry_at = 0;                 // no read before this steady time
    uint64_t revision = 0;
    std::vector<const void*> owners;     // charts using this cache; the last to leave drops it
};

struct Pending {
    std::string id;
    const void* owner = nullptr;
    std::string exchange, symbol;
    bool replay = false;
    int64_t from = 0, to = 0;
    double sent_at = 0;
    bool edge = false;                   // to_ms is the clock
};

// What a chart wants read, every frame. Times are exchange time (ms); `now` is the steady clock.
struct Want {
    std::string_view exchange, symbol;
    bool replay = false;
    int64_t view_from = 0, view_to = 0;  // the visible time range
    int64_t floor = 0;                   // earliest from_ms this viewer may ask for (tier window, replay start)
    int64_t clock = 0;                   // latest to_ms: the live clock less a margin, or the replay playhead
    double now = 0;
};

struct Request {
    std::string id;
    int64_t from = 0, to = 0;
};

class Store {
public:
    // Registers `owner` on a market's cache, creating it empty.
    Market& open(const void* owner, std::string_view exchange, std::string_view symbol, bool replay);
    // The owner left (layer off, chart closed, context swap, market change): its request is forgotten and a cache
    // nobody uses is dropped.
    void release(const void* owner);
    const Market* find(std::string_view exchange, std::string_view symbol, bool replay) const;
    bool pending(std::string_view id) const;
    // The read this owner should send now, if any: the first to the last uncovered part of the visible range
    // (clamped to the floor, the clock, the recorded coverage and 7 days back from its right end), not already
    // answered or asked by another chart. Registers it as pending. Debouncing is the Requester's.
    bool plan(const void* owner, const Want& w, Request& out);
    Receipt receive(const nlohmann::json& j, double now);
    // Requests with no reply after kTimeoutMs are dropped and their market waits kRetryMs.
    void expire(double now);
    // Transport change: everything is read again.
    void clear_all();
    // Replay rewind or seek: replay reads are dropped (live reads stay).
    void clear_replay();
    size_t market_count() const { return markets_.size(); }
    size_t pending_count() const { return pending_.size(); }
    uint64_t dropped() const { return dropped_; }

private:
    Market* find_mutable(std::string_view exchange, std::string_view symbol, bool replay);
    void merge(Market& m, std::vector<Level>& incoming, int64_t from, int64_t to);
    std::vector<Market> markets_;
    std::vector<Pending> pending_;
    uint64_t serial_ = 0;
    uint64_t dropped_ = 0;
};

// One chart's reads: waits for the view to rest, keeps one read in flight, follows the chart's market and mode.
class Requester {
public:
    Requester() = default;
    Requester(const Requester&) = delete;
    Requester& operator=(const Requester&) = delete;
    // Every frame the layer is wanted. True when `out` should be sent now.
    bool step(Store& s, const Want& w, Request& out);
    // The layer is not wanted here any more.
    void reset(Store& s);
    // The visible range starts before what this viewer may read (Free: the last 3 days).
    bool before_floor() const { return before_floor_; }
    bool in_flight(const Store& s) const { return !id_.empty() && s.pending(id_); }

private:
    bool registered_ = false;
    std::string exchange_, symbol_, id_;
    bool replay_ = false, before_floor_ = false;
    int64_t key_from_ = 0, key_to_ = 0, key_q_ = 0;
    double changed_at_ = 0;
};

std::string request_message(const Request& r, std::string_view exchange, std::string_view symbol, const char* token);

// Covered-range bookkeeping (exposed for tests): mark [from, to] answered, or forget part of it.
void paint(std::vector<Span>& spans, Span s);
void uncover(std::vector<Span>& spans, int64_t from, int64_t to);

// ── Display ─────────────────────────────────────────────────────────────────
// Visible range in exchange time; `min_len` is how long a level is drawn at least (one candle, a few pixels).
struct View {
    int64_t t0 = 0, t1 = 0;
    double p0 = 0, p1 = 0;
    int64_t min_len = 0;
};
// Indices of the levels to draw at the clock (known and closed by it), in view, the largest by traded value first
// (ties: earlier, then lower price, then id, so the choice holds still across frames). No allocation.
size_t select(const Market& m, int64_t clock, const View& v, std::array<uint16_t, kMaxLevels>& idx,
              size_t cap = kMaxDrawn);
// Part of the view was answered by a read that kept only its largest levels.
bool truncated_in(const Market& m, int64_t t0, int64_t t1);

Store& store();
// main.cpp's handler for {"type":"refill_history"}.
Receipt receive(const nlohmann::json& j, double now);
// Reset hooks (main.cpp): the socket changed, or replay rewound.
void on_transport_changed();
void on_rewind();

}  // namespace refill::history
