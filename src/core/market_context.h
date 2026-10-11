#pragma once
// Paid Stats projection. Each reading expires on its source clock, even offline.
#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

struct MarketContext {
    enum Id : size_t { Regime, Readings, Flow, Cascade, OIAhead, Awake, Count };
    struct Item {
        std::string value, detail, help, tone;
        int64_t at = 0, until = 0;
        // Price change, contract OI change, realised volatility (all fractions).
        std::array<double, 3> metrics{NAN, NAN, NAN};
        double oi_mcap_ratio = NAN;
        bool current(int64_t now) const { return at > 0 && at <= now && now < until; }
    };
    std::array<Item, Count> items{};
    std::string status = "unavailable";
    static MarketContext parse(std::string_view text, const std::string& venue, const std::string& symbol) {
        MarketContext out;
        if (text.size() > 16384) return out;
        const auto j = nlohmann::json::parse(text, nullptr, false);
        if (!j.is_object()) return out;
        auto str = [](const nlohmann::json& obj, const char* key, size_t max) -> std::string {
            const auto it = obj.find(key);
            if (it == obj.end() || !it->is_string()) return {};
            const auto& s = it->get_ref<const std::string&>();
            // Reject embedded control bytes: these are display strings, not markup.
            if (s.size() > max || s.find_first_of(std::string("\0\r\n\t", 4)) != std::string::npos) return {};
            return s;
        };
        auto clock = [](const nlohmann::json& obj, const char* key) -> int64_t {
            const auto it = obj.find(key);
            if (it == obj.end() || !it->is_number()) return 0;
            const double n = it->get<double>();
            return std::isfinite(n) && n > 0 && n < 9007199254740991.0 && std::floor(n) == n ? static_cast<int64_t>(n) : 0;
        };
        if (clock(j, "v") != 1 || str(j, "venue", 16) != venue || str(j, "symbol", 40) != symbol) return out;
        const auto state = str(j, "status", 16);
        if (state != "ok" && state != "unavailable" && state != "unsupported") return out;
        const auto it = j.find("items");
        if (it == j.end() || !it->is_array() || it->size() > Count) return out;
        if (state != "ok" && !it->empty()) return out;
        const auto server = clock(j, "server_now");
        if (!server) return out;
        constexpr std::array<const char*, Count> ids{"regime", "readings", "flow", "cascade", "oi_ahead", "awake"};
        constexpr std::array<int64_t, Count> ages{4500000, 600000, 300000, 3600000, 86400000, 4500000};
        std::array<bool, Count> seen{};
        for (const auto& entry : *it) {
            if (!entry.is_object()) return MarketContext{};
            const auto name = str(entry, "id", 16);
            size_t idx = 0;
            while (idx < Count && name != ids[idx]) ++idx;
            if (idx == Count || seen[idx]) return MarketContext{};
            seen[idx] = true;
            Item value{str(entry, "value", 160), str(entry, "detail", 240), str(entry, "help", 640), str(entry, "tone", 8), clock(entry, "at"), clock(entry, "until")};
            if (value.value.empty() || value.help.empty() || value.at > server || value.until <= server ||
                value.until <= value.at || !value.at || value.until - value.at > ages[idx] ||
                (value.tone != "neutral" && value.tone != "up" && value.tone != "down" && value.tone != "warn")) return MarketContext{};
            if (idx == Readings && entry.contains("metrics")) {
                const auto& metrics = entry["metrics"];
                if (!metrics.is_array() || metrics.size() != 3) return MarketContext{};
                for (size_t n = 0; n < 3; ++n) {
                    if (metrics[n].is_null()) continue;
                    if (!metrics[n].is_number()) return MarketContext{};
                    const double number = metrics[n].get<double>();
                    if (!std::isfinite(number) || (n == 2 && number < 0)) return MarketContext{};
                    value.metrics[n] = number;
                }
            }
            if (idx == Readings && entry.contains("oi_mcap_ratio")) {
                const auto& share = entry["oi_mcap_ratio"];
                if (!share.is_number()) return MarketContext{};
                value.oi_mcap_ratio = share.get<double>();
                if (!std::isfinite(value.oi_mcap_ratio) || value.oi_mcap_ratio < 0 || value.oi_mcap_ratio > 10) return MarketContext{};
            }
            out.items[idx] = std::move(value);
        }
        out.status = state;
        return out;
    }
};
