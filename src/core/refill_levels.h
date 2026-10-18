#pragma once
// Refill levels: exact prices where aggressive trading kept meeting resting liquidity that was replenished
// (server rule liquidity_response.refill.v1, the backend's refill levels contract). Observed
// replenishment, not an identified hidden order, its owner or its remaining size.
//
// Main thread only. JSON replies are handled on the main thread (main.cpp on_ws_message), and the chart reads
// the store while rendering, so neither the store nor the subscription counts take a lock.
//
// No Emscripten or ImGui here: the native test (tests/native/refill_levels_test.cpp) compiles this unit as is.
#include <nlohmann/json.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace refill {

inline constexpr const char* kVersion = "liquidity_response.refill.v1";
inline constexpr size_t kMaxLevels = 256;      // merged history kept per market
inline constexpr size_t kMaxRefillTimes = 32;  // the server sends the most recent 32
inline constexpr size_t kMaxDrawn = 32;        // rails drawn per chart
inline constexpr double kResubscribeMs = 2000; // at most one repeat subscribe per market in this window

// ── Access ──────────────────────────────────────────────────────────────────
// Live Binance BTC is free; every other live Binance futures market is Pro; a replay is allowed (the server
// enforces the session's replay scope); other venues are unsupported for now.
enum class Access : uint8_t { free_taste, released, replay, locked, unsupported };

inline bool free_market(std::string_view exchange, std::string_view symbol) {
    return exchange == "binancef" && symbol == "btcusdt";
}
inline Access access(std::string_view exchange, std::string_view symbol, bool pro, bool replay, bool hosted = true) {
    if (exchange != "binancef") return Access::unsupported;
    if (replay) return Access::replay;
    if (pro || !hosted) return Access::released;
    return free_market(exchange, symbol) ? Access::free_taste : Access::locked;
}
inline bool granted(Access a) {
    return a == Access::free_taste || a == Access::released || a == Access::replay;
}

// ── Model ───────────────────────────────────────────────────────────────────
enum class Side : uint8_t { bid, ask };
enum class End : uint8_t { holding, crossed, pulled, exhausted, inactive, interrupted };
enum class Status : uint8_t { none, observing, warming, unavailable };

struct Level {
    std::string id;
    Side side = Side::bid;
    End status = End::holding;
    double price = 0;
    int64_t started_ms = 0, known_ms = 0, updated_ms = 0, closed_ms = 0;
    double executed_base = 0, executed_usd = 0, max_shown_base = 0, refilled_base = 0, shown_ratio = 0;
    int refills = 0;
    std::array<int64_t, kMaxRefillTimes> refill_ms{};
    uint8_t refill_count = 0;
    // In the server's latest complete set. A holding level that drops out of it ends at its last update.
    bool current = true;
};

struct Market {
    std::string exchange, symbol;
    bool replay = false;              // the mode the subscription was made in; other modes are ignored
    Status status = Status::none;     // none = subscribed, nothing received yet
    std::string reason;               // set with unavailable
    int64_t as_of_ms = 0;
    std::vector<Level> levels;        // merged by id, at most kMaxLevels
    uint64_t revision = 0;
};

enum class Receipt : uint8_t { accepted, reset, unavailable, ignored, malformed };

class Store {
public:
    // Subscribing (or re-subscribing) a market starts it empty; only messages in this mode are accepted.
    void expect(std::string_view exchange, std::string_view symbol, bool replay);
    // The last subscriber left: messages for it are ignored again.
    void forget(std::string_view exchange, std::string_view symbol);
    // Transport change, replay rewind or seek: drop every level and wait for the next message.
    void clear_all();
    Receipt receive(const nlohmann::json& j);
    const Market* find(std::string_view exchange, std::string_view symbol) const;
    size_t market_count() const { return markets_.size(); }
    // Levels dropped as malformed since start (diagnostics and tests).
    uint64_t dropped() const { return dropped_; }

private:
    Market* find_mutable(std::string_view exchange, std::string_view symbol);
    void merge(Market& m, Level&& level);
    std::vector<Market> markets_;
    uint64_t dropped_ = 0;
};

// Parses one entry of `levels`; false for anything malformed (the entry is dropped, the message kept).
bool parse_level(const nlohmann::json& j, Level& out);

// ── Subscriptions ───────────────────────────────────────────────────────────
// One server subscription per market and connection, however many charts want it. A channel is the
// StreamManager the subscription was sent through (only compared, never dereferenced here).
struct Message {
    const void* channel = nullptr;
    std::string text;
};

std::string subscribe_message(std::string_view exchange, std::string_view symbol, double tick, const char* token);
std::string unsubscribe_message(std::string_view exchange, std::string_view symbol);

class Subscriptions {
public:
    explicit Subscriptions(Store& store) : store_(store) {}
    struct Want {
        const void* channel = nullptr;
        std::string_view exchange, symbol;
        double tick = 0;
        bool replay = false;
        uint64_t generation = 0;      // the channel's connection generation: a new one needs a new subscribe
        bool connected = false;       // nothing is sent until the channel is open
        double now_ms = 0;            // steady clock, for the repeat-subscribe throttle
        const char* (*token)() = nullptr;
    };
    // Every frame an owner wants refill levels. Messages to send are appended to `out`.
    void want(const void* owner, const Want& w, std::vector<Message>& out);
    // The owner no longer wants them; the last owner of a market sends the unsubscribe.
    void release(const void* owner, std::vector<Message>& out);
    // The socket went away or was replaced: every market subscribes again once its channel is open.
    void transport_changed();
    // The server asked for a new subscribe in this context (unavailable / context_changed).
    void resubscribe(std::string_view exchange, std::string_view symbol);
    size_t count() const { return entries_.size(); }
    size_t owners(std::string_view exchange, std::string_view symbol) const;

private:
    struct Entry {
        const void* channel = nullptr;
        std::string exchange, symbol;
        double tick = 0;
        bool replay = false, sent = false;
        uint64_t generation = 0;
        double sent_at = -1e18;
        std::vector<const void*> owners;
    };
    void drop_owner(const void* owner, std::vector<Message>& out);
    Store& store_;
    std::vector<Entry> entries_;
};

Store& store();
Subscriptions& subscriptions();
// main.cpp's handler for {"type":"refill_levels"}. A context_changed refusal asks for a new subscribe.
Receipt receive(const nlohmann::json& j);
// Reset hooks (main.cpp): the socket changed, or replay rewound/seeked.
void on_transport_changed();
void on_rewind();

// ── Display helpers ─────────────────────────────────────────────────────────
// What one level looks like at the chart clock. A level the clock has not reached yet (known_ms > clock) is not
// shown; one closed after the clock is still holding at the clock.
struct Span {
    int64_t start = 0, known = 0, end = 0;
    End end_status = End::holding;
    bool open = false;               // holding at the clock: the rail runs to the latest observation
};
bool span_at(const Level& l, const Market& m, int64_t clock, Span& out);

struct View { double t0 = 0, t1 = 0, p0 = 0, p1 = 0; };
// Indices of the levels to draw, holding first, then most recent; at most `cap`. No allocation.
size_t select(const Market& m, int64_t clock, const View& v, std::array<uint16_t, kMaxLevels>& idx, size_t cap = kMaxDrawn);

// Rail thickness in three steps against the largest traded value drawn.
float rail_width(double executed_usd, double max_usd);

const char* end_word(End e);
// "$2.4M", "$780k", "$950".
void format_usd(double v, char* out, size_t n);
// "Bid refilled 7× at 101240.5" (price already formatted with the market's PriceFormatter).
void hover_title(const Level& l, const char* price, char* out, size_t n);
// "$2.4M traded vs $780k shown".
void hover_values(const Level& l, char* out, size_t n);
// Short chip copy for a market the server cannot serve (pro_required has none: it is the locked row).
const char* unavailable_note(std::string_view reason);

// Holding levels in the ladder's rows this frame.
struct LadderMark { double price = 0; float ratio = 0; bool bid = true; };
struct LadderMarks {
    std::array<LadderMark, kMaxDrawn> marks{};
    int count = 0;
};

}  // namespace refill
