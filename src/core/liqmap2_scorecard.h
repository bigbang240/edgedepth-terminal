#pragma once
// Received V2 scorecards (core/liqmap2_store.h Scorecard), one per market: the backend's JSON reply
// type "liqmap2_scorecard" to get_liqmap2_scorecard. Live only; main.cpp routes the reply here.
#include "core/liqmap2_store.h"
#include <nlohmann/json.hpp>
#include <map>
#include <string>
#include <utility>

namespace liqmap2 {

class Scorecards {
public:
    static Scorecards& instance() { static Scorecards s; return s; }
    static void receive(const nlohmann::json& j) { instance().put(j); }
    // Parses one reply; false (and nothing stored) when it is malformed.
    bool put(const nlohmann::json& j) {
        if (!j.is_object() || !j.contains("exchange") || !j["exchange"].is_string() || !j.contains("symbol") ||
            !j["symbol"].is_string() || !j.contains("days") || !j["days"].is_number_integer())
            return false;
        Scorecard sc;
        sc.window_days = j.value("window_days", 0);
        sc.days = j.value("days", 0);
        sc.minutes_scored = j.value("minutes_scored", 0);
        sc.reached_long = j.value("minutes_reached_long", 0);
        sc.reached_short = j.value("minutes_reached_short", 0);
        sc.events_reached = j.value("events_reached", 0);
        sc.events_all = j.value("events_all", 0);
        sc.from = j.value("from", std::string());
        sc.to = j.value("to", std::string());
        sc.source = j.value("realized_source", std::string());
        sc.provisional = j.value("provisional", false);
        sc.pred_long = j.value("predicted_long_usd", 0.0);
        sc.pred_short = j.value("predicted_short_usd", 0.0);
        sc.real_long_reached = j.value("realized_long_reached_usd", 0.0);
        sc.real_short_reached = j.value("realized_short_reached_usd", 0.0);
        sc.real_long_all = j.value("realized_long_all_usd", 0.0);
        sc.real_short_all = j.value("realized_short_all_usd", 0.0);
        if (sc.days < 0 || sc.minutes_scored < 0) return false;
        cards_[{j["exchange"].get<std::string>(), j["symbol"].get<std::string>()}] = std::move(sc);
        return true;
    }
    const Scorecard* get(const std::string& exchange, const std::string& symbol) const {
        const auto it = cards_.find({exchange, symbol});
        return it == cards_.end() ? nullptr : &it->second;
    }
private:
    std::map<std::pair<std::string, std::string>, Scorecard> cards_;
};

}  // namespace liqmap2
