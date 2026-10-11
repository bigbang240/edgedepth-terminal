#pragma once
// Historical, single-venue observations. Never splice a live price into an old ratio.
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct StatsRatioHistory {
    std::vector<double> times, prices, oi_share;
    int64_t from = 0, to = 0, step = 0;
    bool binance = false;
    bool available() const { return !times.empty(); }
    static StatsRatioHistory parse(std::string_view text, const std::string& venue, const std::string& symbol) {
        StatsRatioHistory out;
        if (venue != "binancef" || text.size() > 262144) return out;
        const auto j = nlohmann::json::parse(text, nullptr, false);
        if (!j.is_object()) return out;
        auto matches = [&](const char* key, const std::string& value) {
            const auto it = j.find(key); return it != j.end() && it->is_string() && *it == value;
        };
        auto clock = [&](const char* key) -> int64_t {
            const auto it = j.find(key);
            if (it == j.end() || !it->is_number()) return 0;
            const double x = it->get<double>();
            return std::isfinite(x) && x > 0 && x < 9007199254740991.0 && std::floor(x) == x ? static_cast<int64_t>(x) : 0;
        };
        if (clock("v") != 1 || !matches("venue", venue) || !matches("symbol", symbol) || !matches("status", "ok")) return out;
        out.binance = matches("source", "binance");
        const auto from = clock("from"), to = clock("to"), step = clock("step"), generated = clock("generated_at"), server = clock("server_now");
        if (!from || to < from || to - from > 7 * 86400000LL || (step != 300000 && step != 3600000) ||
            from % step || to % step || generated < to || server < generated) return out;
        const auto points = j.find("points");
        if (points == j.end() || !points->is_array() || points->empty() || points->size() > 2017 ||
            points->size() != static_cast<size_t>((to - from) / step + 1)) return out;
        size_t i = 0;
        for (const auto& p : *points) {
            if (!p.is_array() || p.size() != 3 || !p[0].is_number() || p[0].get<double>() != from + i * step) return {};
            double values[2] = {NAN, NAN};
            for (size_t n = 0; n < 2; ++n) {
                if (p[n+1].is_null()) continue;
                if (!p[n+1].is_number()) return {};
                values[n] = p[n+1].get<double>();
                if (!std::isfinite(values[n]) || values[n] <= 0 ||
                    (n == 1 && ((!out.binance && values[n] > 10) || !std::isfinite(1.0 / values[n])))) return {};
            }
            out.times.push_back(static_cast<double>(from + i * step) / 1000.0);
            out.prices.push_back(values[0]); out.oi_share.push_back(values[1]); ++i;
        }
        out.from = from; out.to = to; out.step = step;
        return out;
    }
};
