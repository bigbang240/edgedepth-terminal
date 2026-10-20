// ═══════════════════════════════════════════════════════════════════════════════
// local_paper_engine.cpp - client-side paper trading implementation
//
// NOTE (learned the hard way, see phase4-complete worklog): this WASM runs
// with exception catching DISABLED - every nlohmann accessor on stored JSON
// must be find()+type-checked or a malformed blob aborts the terminal.
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/local_paper_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <nlohmann/json.hpp>

#include "core/display_time_zone.h"
#include "core/execution_bridge.h"
#include "core/position_sizing.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// localStorage blob bridges (drawing_manager pattern; caller frees).
EM_JS(char*, edlp_read_blob, (const char* key), {
    try {
        const raw = localStorage.getItem(UTF8ToString(key));
        if (!raw) return 0;
        const len = lengthBytesUTF8(raw) + 1;
        const p = _malloc(len);
        stringToUTF8(raw, p, len);
        return p;
    } catch (e) { return 0; }
});
EM_JS(void, edlp_write_blob, (const char* key, const char* val), {
    try { localStorage.setItem(UTF8ToString(key), UTF8ToString(val)); }
    catch (e) {}
});
#else
static char* edlp_read_blob(const char*) { return nullptr; }
static void  edlp_write_blob(const char*, const char*) {}
#endif

namespace {

std::string make_id(int64_t now_ms) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "lp-%lld-%d",
                  static_cast<long long>(now_ms),
                  static_cast<int>(now_ms % 0xFFFF));
    return buf;
}

}  // namespace

LocalPaperEngine::LocalPaperEngine() {
    load();
}

bool LocalPaperEngine::has_open(const char* exchange, const char* symbol) const {
    for (const auto& p : open_)
        if (p.exchange == exchange && p.symbol == symbol) return true;
    return false;
}

const LocalPaperEngine::Position* LocalPaperEngine::open(
    const char* exchange, const char* symbol, bool is_long, double entry,
    double stop_pct, const char* source, int64_t now_ms) {

    if (!(entry > 0.0) || !exchange || !symbol || !*exchange || !*symbol)
        return nullptr;
    // Auto-open guards: one position per symbol, 60s global cooldown.
    if (has_open(exchange, symbol)) return nullptr;
    if (now_ms - last_auto_ms_ < AUTO_COOLDOWN_MS) return nullptr;
    last_auto_ms_ = now_ms;

    const double spct = std::clamp(stop_pct, 0.05, 20.0);
    const double dir = is_long ? 1.0 : -1.0;
    const double stop = entry * (1.0 - dir * spct / 100.0);
    const double r = std::fabs(entry - stop);
    if (!(r > 0.0)) return nullptr;

    const position_sizing::SizeResult sz =
        position_sizing::compute(entry, stop);
    if (!sz.valid) return nullptr;

    Position p;
    p.id = make_id(now_ms);
    p.exchange = exchange;
    p.symbol = symbol;
    p.long_side = is_long;
    p.source = source ? source : "manual";
    p.entry = entry;
    p.qty = sz.qty;
    p.remaining_qty = sz.qty;
    p.notional = sz.notional;
    p.stop = stop;
    p.tp1 = entry + dir * r;
    p.tp2 = entry + dir * 2.0 * r;
    p.risk_usd = sz.risk_usd;
    p.mark = entry;
    p.opened_ms = now_ms;
    open_.push_back(std::move(p));
    mark_dirty();

    const Position& np = open_.back();
    char pbuf[32];
    std::snprintf(pbuf, sizeof(pbuf), "%.6g", entry);
    std::string msg = std::string("paper ") + (is_long ? "long" : "short") +
                      " opened at " + pbuf;
    exec_bridge::notify("paper_open", np.exchange.c_str(), np.symbol.c_str(),
                        is_long ? "long" : "short", entry, msg.c_str());
    return &open_.back();
}

bool LocalPaperEngine::close(const std::string& id, double price,
                             int64_t now_ms, const char* reason) {
    for (size_t i = 0; i < open_.size(); ++i) {
        if (open_[i].id != id || !open_[i].open()) continue;
        if (!(price > 0.0)) price = open_[i].mark > 0.0 ? open_[i].mark : open_[i].entry;
        finalize(i, price, now_ms, reason ? reason : "manual");
        return true;
    }
    return false;
}

void LocalPaperEngine::flatten_all(
    double (*price_of)(const char*, const char*), int64_t now_ms) {
    for (size_t i = 0; i < open_.size();) {
        Position& p = open_[i];
        double px = price_of && p.mark <= 0.0
            ? price_of(p.exchange.c_str(), p.symbol.c_str()) : p.mark;
        if (!(px > 0.0)) px = p.entry;
        finalize(i, px, now_ms, "manual");
        // finalize erases index i, do not advance
    }
}

void LocalPaperEngine::on_price(const char* exchange, const char* symbol,
                                double price, int64_t now_ms) {
    if (!(price > 0.0)) return;
    bool any = false;
    for (size_t i = 0; i < open_.size();) {
        Position& p = open_[i];
        if (p.exchange != exchange || p.symbol != symbol) { ++i; continue; }
        any = true;
        p.mark = price;
        const double dir = p.dir();
        if (p.entry > 0.0) {
            const double pct = (price - p.entry) / p.entry * 100.0 * dir;
            p.mfe_pct = std::max(p.mfe_pct, pct);
            p.mae_pct = std::min(p.mae_pct, pct);
        }
        const double r = std::fabs(p.entry - p.stop);
        const double half = p.remaining_qty * 0.5;

        // Conservative order: stop first, then the TP ladder, then the trail.
        if ((p.is_long() && price <= p.stop) || (!p.is_long() && price >= p.stop)) {
            finalize(i, p.stop, now_ms, "stop");
            continue;  // index erased
        }
        if (!p.tp1_hit &&
            ((p.is_long() && price >= p.tp1) || (!p.is_long() && price <= p.tp1))) {
            p.realized += (p.tp1 - p.entry) * half * dir;
            p.remaining_qty = half;
            p.tp1_hit = true;
            // Arm the runner's trail at 1R behind the current price.
            const double t = p.is_long() ? price - r : price + r;
            p.trail = t;
        }
        if (p.tp1_hit && !p.tp2_hit &&
            ((p.is_long() && price >= p.tp2) || (!p.is_long() && price <= p.tp2))) {
            p.realized += (p.tp2 - p.entry) * p.remaining_qty * 0.5 * dir;
            p.remaining_qty *= 0.5;
            p.tp2_hit = true;
        }
        if (p.tp1_hit && p.trail > 0.0) {
            if (p.is_long()) {
                p.trail = std::max(p.trail, price - r);
                if (price <= p.trail) { finalize(i, p.trail, now_ms, "trail"); continue; }
            } else {
                p.trail = std::min(p.trail, price + r);
                if (price >= p.trail) { finalize(i, p.trail, now_ms, "trail"); continue; }
            }
        }
        ++i;
    }
    if (any) mark_dirty();
}

void LocalPaperEngine::finalize(size_t index, double exit_price,
                                int64_t now_ms, const char* reason) {
    if (index >= open_.size()) return;
    Position p = open_[index];
    open_.erase(open_.begin() + static_cast<long>(index));

    const double dir = p.dir();
    p.realized += (exit_price - p.entry) * p.remaining_qty * dir;
    p.remaining_qty = 0.0;
    p.closed_ms = now_ms;
    p.exit_reason = reason ? reason : "manual";
    p.mark = exit_price;
    p.r_multiple = p.risk_usd > 0.0 ? p.realized / p.risk_usd : 0.0;

    stats_.realized_total += p.realized;
    stats_.trades++;
    if (p.realized > 0.0) { stats_.wins++; stats_.gross_win += p.realized; }
    else if (p.realized < 0.0) { stats_.losses++; stats_.gross_loss += -p.realized; }

    const double eq = stats_.initial_balance + stats_.realized_total;
    stats_.peak_equity = std::max(stats_.peak_equity, eq);
    if (stats_.peak_equity > 0.0)
        stats_.max_dd_pct = std::max(stats_.max_dd_pct,
            (stats_.peak_equity - eq) / stats_.peak_equity * 100.0);

    closed_.push_front(std::move(p));
    if (closed_.size() > MAX_CLOSED) closed_.pop_back();

    char pbuf[32];
    std::snprintf(pbuf, sizeof(pbuf), "%.6g", exit_price);
    const std::string msg = "paper closed (" + p.exit_reason + ") at " + pbuf;
    exec_bridge::notify("paper_close", p.exchange.c_str(), p.symbol.c_str(),
                        p.is_long() ? "long" : "short", exit_price, msg.c_str());
    mark_dirty();
}

double LocalPaperEngine::open_pnl() const {
    double s = 0.0;
    for (const auto& p : open_) s += p.unrealized_pnl();
    return s;
}

void LocalPaperEngine::set_initial_balance(double v) {
    v = std::clamp(v, 100.0, 100000000.0);
    if (v == stats_.initial_balance) return;
    // Keep peak/dd consistent with the new baseline.
    const double shift = v - stats_.initial_balance;
    stats_.initial_balance = v;
    stats_.peak_equity = std::max(100.0, stats_.peak_equity + shift);
    mark_dirty();
}

std::vector<LocalPaperEngine::DailyRow> LocalPaperEngine::daily_stats() const {
    std::vector<DailyRow> rows;
    for (const auto& p : closed_) {
        char daybuf[16];
        if (!DisplayTimeZone::instance().format(
                p.closed_ms, TimeZoneFormat::DateOnly, daybuf, sizeof(daybuf)))
            std::snprintf(daybuf, sizeof(daybuf), "%lld",
                          static_cast<long long>(p.closed_ms / 86400000));
        auto it = rows.begin();
        for (; it != rows.end(); ++it)
            if (it->day == daybuf) break;
        if (it == rows.end()) {
            rows.push_back({daybuf, 0, 0, 0.0});
            it = rows.end() - 1;
        }
        it->trades++;
        if (p.realized > 0.0) it->wins++;
        it->pnl += p.realized;
    }
    return rows;
}

void LocalPaperEngine::tick(int64_t now_ms) {
    if (!dirty_ || now_ms - last_save_ms_ < 1000) return;
    save();
    last_save_ms_ = now_ms;
    dirty_ = false;
}

void LocalPaperEngine::save() const {
    nlohmann::json j;
    j["v"] = 1;
    j["initial"] = stats_.initial_balance;
    j["realized_total"] = stats_.realized_total;
    j["trades"] = stats_.trades;
    j["wins"] = stats_.wins;
    j["losses"] = stats_.losses;
    j["gross_win"] = stats_.gross_win;
    j["gross_loss"] = stats_.gross_loss;
    j["peak"] = stats_.peak_equity;
    j["maxdd"] = stats_.max_dd_pct;
    j["last_auto_ms"] = last_auto_ms_;

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : open_) {
        nlohmann::json o;
        o["id"] = p.id; o["ex"] = p.exchange; o["sym"] = p.symbol;
        o["long"] = p.is_long(); o["src"] = p.source;
        o["entry"] = p.entry; o["qty"] = p.qty; o["rem"] = p.remaining_qty;
        o["notional"] = p.notional;
        o["stop"] = p.stop; o["tp1"] = p.tp1; o["tp2"] = p.tp2;
        o["trail"] = p.trail; o["risk"] = p.risk_usd; o["mark"] = p.mark;
        o["realized"] = p.realized;
        o["mae"] = p.mae_pct; o["mfe"] = p.mfe_pct;
        o["tp1h"] = p.tp1_hit; o["tp2h"] = p.tp2_hit;
        o["open_ms"] = p.opened_ms;
        arr.push_back(o);
    }
    j["open"] = arr;

    nlohmann::json carr = nlohmann::json::array();
    for (const auto& p : closed_) {
        nlohmann::json o;
        o["id"] = p.id; o["ex"] = p.exchange; o["sym"] = p.symbol;
        o["long"] = p.is_long(); o["src"] = p.source;
        o["entry"] = p.entry; o["qty"] = p.qty; o["rem"] = p.remaining_qty;
        o["notional"] = p.notional;
        o["stop"] = p.stop; o["tp1"] = p.tp1; o["tp2"] = p.tp2;
        o["risk"] = p.risk_usd; o["mark"] = p.mark;
        o["realized"] = p.realized;
        o["mae"] = p.mae_pct; o["mfe"] = p.mfe_pct;
        o["tp1h"] = p.tp1_hit; o["tp2h"] = p.tp2_hit;
        o["open_ms"] = p.opened_ms; o["closed_ms"] = p.closed_ms;
        o["exit"] = p.exit_reason; o["r"] = p.r_multiple;
        carr.push_back(o);
    }
    j["closed"] = carr;

    const std::string blob = j.dump();
    edlp_write_blob(STORAGE_KEY, blob.c_str());
}

void LocalPaperEngine::load() {
    char* raw = edlp_read_blob(STORAGE_KEY);
    if (!raw) return;
    const std::string blob(raw);
    std::free(raw);
    try {
        const nlohmann::json j = nlohmann::json::parse(blob, nullptr, false);
        if (!j.is_object() || j.find("v") == j.end() || !(*j.find("v")).is_number())
            return;

        const auto num = [&j](const char* key, double def) {
            auto it = j.find(key);
            return (it != j.end() && it->is_number()) ? it->get<double>() : def;
        };
        const auto integer = [&j](const char* key, int def) {
            auto it = j.find(key);
            return (it != j.end() && it->is_number()) ? it->get<int>() : def;
        };

        stats_.initial_balance = num("initial", 10000.0);
        stats_.realized_total  = num("realized_total", 0.0);
        stats_.trades = integer("trades", 0);
        stats_.wins   = integer("wins", 0);
        stats_.losses = integer("losses", 0);
        stats_.gross_win  = num("gross_win", 0.0);
        stats_.gross_loss = num("gross_loss", 0.0);
        stats_.peak_equity = num("peak", stats_.initial_balance);
        stats_.max_dd_pct  = num("maxdd", 0.0);
        last_auto_ms_ = static_cast<int64_t>(num("last_auto_ms", 0.0));

        const auto arr = j.find("open");
        if (arr != j.end() && arr->is_array()) {
            for (const auto& o : *arr) {
                if (!o.is_object()) continue;
                Position p;
                auto get_str = [&o](const char* key) {
                    auto it = o.find(key);
                    return (it != o.end() && it->is_string())
                        ? it->get<std::string>() : std::string();
                };
                auto get_num = [&o](const char* key) {
                    auto it = o.find(key);
                    return (it != o.end() && it->is_number()) ? it->get<double>() : 0.0;
                };
                auto get_bool = [&o](const char* key) {
                    auto it = o.find(key);
                    return (it != o.end() && it->is_boolean()) ? it->get<bool>() : false;
                };
                p.id = get_str("id"); p.exchange = get_str("ex"); p.symbol = get_str("sym");
                if (p.exchange.empty() || p.symbol.empty()) continue;
                p.long_side = get_bool("long");
                p.source = get_str("src");
                p.entry = get_num("entry");
                p.qty = get_num("qty");
                p.remaining_qty = get_num("rem");
                p.notional = get_num("notional");
                p.stop = get_num("stop"); p.tp1 = get_num("tp1"); p.tp2 = get_num("tp2");
                p.trail = get_num("trail"); p.risk_usd = get_num("risk");
                p.mark = get_num("mark"); p.realized = get_num("realized");
                p.mae_pct = get_num("mae"); p.mfe_pct = get_num("mfe");
                p.tp1_hit = get_bool("tp1h"); p.tp2_hit = get_bool("tp2h");
                p.opened_ms = static_cast<int64_t>(get_num("open_ms"));
                if (p.entry <= 0.0 || p.qty <= 0.0) continue;
                open_.push_back(std::move(p));
            }
        }
        const auto carr = j.find("closed");
        if (carr != j.end() && carr->is_array()) {
            for (const auto& o : *carr) {
                if (!o.is_object()) continue;
                Position p;
                auto get_str = [&o](const char* key) {
                    auto it = o.find(key);
                    return (it != o.end() && it->is_string())
                        ? it->get<std::string>() : std::string();
                };
                auto get_num = [&o](const char* key) {
                    auto it = o.find(key);
                    return (it != o.end() && it->is_number()) ? it->get<double>() : 0.0;
                };
                auto get_bool = [&o](const char* key) {
                    auto it = o.find(key);
                    return (it != o.end() && it->is_boolean()) ? it->get<bool>() : false;
                };
                p.id = get_str("id"); p.exchange = get_str("ex"); p.symbol = get_str("sym");
                if (p.exchange.empty() || p.symbol.empty()) continue;
                p.long_side = get_bool("long");
                p.source = get_str("src");
                p.entry = get_num("entry");
                p.qty = get_num("qty");
                p.remaining_qty = get_num("rem");
                p.notional = get_num("notional");
                p.stop = get_num("stop"); p.tp1 = get_num("tp1"); p.tp2 = get_num("tp2");
                p.risk_usd = get_num("risk"); p.mark = get_num("mark");
                p.realized = get_num("realized");
                p.mae_pct = get_num("mae"); p.mfe_pct = get_num("mfe");
                p.tp1_hit = get_bool("tp1h"); p.tp2_hit = get_bool("tp2h");
                p.opened_ms = static_cast<int64_t>(get_num("open_ms"));
                p.closed_ms = static_cast<int64_t>(get_num("closed_ms"));
                p.exit_reason = get_str("exit");
                p.r_multiple = get_num("r");
                closed_.push_back(std::move(p));
            }
            while (closed_.size() > MAX_CLOSED) closed_.pop_back();
        }
    } catch (...) {
        // Corrupt blob: start clean rather than aborting the terminal.
        open_.clear();
        closed_.clear();
        stats_ = Stats{};
    }
}
