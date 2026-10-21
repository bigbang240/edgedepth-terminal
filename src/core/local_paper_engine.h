#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// local_paper_engine.h - Client-side paper trading for the self-hosted terminal
//
// The upstream PaperTradingManager is fed by STREAM_PAPER_TRADING from the
// hosted backend, which the Go gateway does not implement - the panel is dead
// in self-host mode. This engine provides the same workflow entirely client
// side (Phase 5):
//   - orders opened automatically from price/drawing alert crossings
//     (risk-based quantity from position_sizing, stop% configurable)
//   - TP1 (50%) / TP2 (50% of the remainder) ladder + trailing runner armed
//     after TP1 at 1R distance
//   - mark-to-market + MAE/MFE fed from the chart's own price path
//     (last RT trade / quote mid / candle close - the same source alerts use)
//   - daily P&L stats + win-rate/profit-factor in a compact account struct
//   - localStorage persistence (1s debounce), drawings blob pattern
//
// Not per-symbol: one engine, keyed by exchange+symbol. Auto-opens are
// guarded by a 60s global cooldown and an "already open on this symbol" rule
// so re-arming drawing levels cannot stack positions.
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

class LocalPaperEngine {
public:
    static constexpr size_t  MAX_CLOSED = 200;
    static constexpr int64_t AUTO_COOLDOWN_MS = 60000;
    static constexpr const char* STORAGE_KEY = "edgedepth.localpaper.v1";

    struct Position {
        std::string id;
        std::string exchange;
        std::string symbol;
        bool        long_side = true;
        std::string source;        // "alert" | "drawing" | "manual"
        double entry = 0.0;
        double qty = 0.0;          // original quantity (base units)
        double remaining_qty = 0.0;
        double notional = 0.0;     // entry * qty at open
        double stop = 0.0;
        double tp1 = 0.0;
        double tp2 = 0.0;
        double trail = 0.0;        // armed once tp1_hit
        double risk_usd = 0.0;     // actual $ risk at entry (R denominator)
        double mark = 0.0;
        double realized = 0.0;     // banked by partial TPs (+ final fill on close)
        double mae_pct = 0.0;
        double mfe_pct = 0.0;
        bool   tp1_hit = false;
        bool   tp2_hit = false;
        int64_t opened_ms = 0;
        int64_t closed_ms = 0;     // 0 = open
        std::string exit_reason;   // "" open | stop | trail | tp2 | manual
        double r_multiple = 0.0;   // filled on close

        [[nodiscard]] bool   open() const { return closed_ms == 0; }
        [[nodiscard]] bool   is_long() const { return long_side; }
        [[nodiscard]] double dir() const { return long_side ? 1.0 : -1.0; }
        [[nodiscard]] double unrealized_pnl() const {
            return (mark - entry) * remaining_qty * dir();
        }
        [[nodiscard]] double total_pnl() const { return realized + unrealized_pnl(); }
        [[nodiscard]] int64_t duration_s() const {
            return (closed_ms ? closed_ms : 0) / 1000 - opened_ms / 1000;
        }
    };

    struct Stats {
        double initial_balance = 10000.0;
        double realized_total  = 0.0;   // sum of closed-trade totals
        int    trades = 0;
        int    wins   = 0;
        int    losses = 0;
        double gross_win  = 0.0;
        double gross_loss = 0.0;
        double peak_equity = 10000.0;
        double max_dd_pct  = 0.0;

        [[nodiscard]] double win_rate() const {
            return trades > 0 ? 100.0 * wins / trades : 0.0;
        }
        [[nodiscard]] double profit_factor() const {
            if (gross_loss > 0.0) return gross_win / gross_loss;
            return gross_win > 0.0 ? 99.9 : 0.0;
        }
    };

    struct DailyRow {
        std::string day;    // YYYY-MM-DD in the display time zone
        int    trades = 0;
        int    wins   = 0;
        double pnl    = 0.0;
    };

    LocalPaperEngine();

    // ── Opening ────────────────────────────────────────────────────────────
    // Alert/drawing-triggered market entry. Quantity comes from
    // position_sizing (account * risk% / stop distance, 20x notional cap).
    // Returns nullptr when rejected (bad inputs, cooldown, duplicate).
    const Position* open(const char* exchange, const char* symbol, bool is_long,
                         double entry, double stop_pct, const char* source,
                         int64_t now_ms);

    bool has_open(const char* exchange, const char* symbol) const;

    // ── Manual management ──────────────────────────────────────────────────
    bool close(const std::string& id, double price, int64_t now_ms,
               const char* reason);
    void flatten_all(double (*price_of)(const char* exch, const char* sym),
                     int64_t now_ms);

    // ── Market feed (called per frame from each chart with its own price) ──
    void on_price(const char* exchange, const char* symbol, double price,
                  int64_t now_ms);

    void set_initial_balance(double v);

    // Persistence debounce (call once per frame with a wall-clock ms stamp).
    void tick(int64_t now_ms);

    // ── Accessors ──────────────────────────────────────────────────────────
    [[nodiscard]] const std::deque<Position>& positions() const { return open_; }
    [[nodiscard]] const std::deque<Position>& closed() const { return closed_; }
    [[nodiscard]] const Stats& stats() const { return stats_; }
    [[nodiscard]] double open_pnl() const;
    [[nodiscard]] double equity() const {
        return stats_.initial_balance + stats_.realized_total + open_pnl();
    }
    [[nodiscard]] bool empty() const { return open_.empty() && closed_.empty(); }

    // Closed trades grouped by calendar day (display time zone), oldest last.
    [[nodiscard]] std::vector<DailyRow> daily_stats() const;

private:
    void finalize(size_t index, double exit_price, int64_t now_ms,
                  const char* reason);
    void mark_dirty() { dirty_ = true; }
    void save() const;
    void load();

    std::deque<Position> open_;
    std::deque<Position> closed_;   // newest first
    Stats stats_{};
    bool    dirty_       = false;
    int64_t last_save_ms_ = 0;
    int64_t last_auto_ms_ = 0;
};
