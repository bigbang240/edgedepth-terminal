#pragma once
#include "workspace_document.h"
#include <array>
#include <vector>
#include <algorithm>

// Exact 5m observations. Nulls remain NaN and break lines; never forward-fill.
struct MarketDataHistory {
    int64_t from = 0, to = 0, generated = 0;
    std::vector<double> times;
    // Price, OI contracts, global long %, OI/market cap %, top long %, taker buy/sell base volume.
    std::array<std::vector<double>, 7> values;
    std::vector<double> notional;
    bool available() const { return !times.empty(); }
    size_t nearest(double seconds, size_t first = 0) const {
        if (times.empty()) return 0;
        return std::clamp<size_t>(static_cast<size_t>(std::max(0.0, std::round((seconds - times.front()) / 300))), first, times.size()-1);
    }
    static MarketDataHistory parse(std::string_view text, const std::string& venue, const std::string& symbol) {
        MarketDataHistory out;
        const auto j = workspace::parse_bounded(text, 131072);
        if (!j.is_object() || venue != "binancef") return out;
        for (const auto& [key, value] : {std::pair{"venue", venue}, {"symbol", symbol}, {"source", std::string("binance")}, {"status", std::string("ok")}})
            if (!j.contains(key) || j[key] != value) return out;
        auto clock = [&](const char* key) -> int64_t {
            if (!j.contains(key) || !j[key].is_number()) return 0;
            const double v = j[key].get<double>();
            return std::isfinite(v) && v > 0 && v < 9007199254740991.0 && std::floor(v) == v ? static_cast<int64_t>(v) : 0;
        };
        const auto from = clock("from"), to = clock("to"), generated = clock("generated_at"), server = clock("server_now");
        if (clock("v") != 1 || clock("step") != 300000 || !from || to-from != 86400000 ||
            from % 300000 || to % 300000 || generated < to || generated-to >= 300000 || server < generated) return out;
        if (!j.contains("points") || !j["points"].is_array() || j["points"].size() != 289) return out;
        size_t index = 0;
        for (const auto& row : j["points"]) {
            const auto t = from + static_cast<int64_t>(index) * 300000;
            if (!row.is_array() || row.size() != 4 || !row[0].is_number() || row[0].get<double>() != t) return {};
            double nums[3] = {NAN, NAN, NAN};
            for (size_t n=0; n<3; ++n) {
                if (row[n+1].is_null()) continue;
                if (!row[n+1].is_number()) return {};
                nums[n] = row[n+1].get<double>();
                if (!std::isfinite(nums[n]) || nums[n] < 0 || (n == 0 && nums[n] == 0) || (n == 2 && nums[n] > 1)) return {};
            }
            out.times.push_back(static_cast<double>(t)/1000);
            for (size_t n=0; n<3; ++n) out.values[n].push_back(n == 2 ? nums[n]*100 : nums[n]);
            const double usd = nums[0]*nums[1];
            out.notional.push_back(std::isfinite(usd) ? usd : NAN);
            ++index;
        }
        // Additive V1 series keep an older API usable during rolling deploys.
        const char* keys[]={"oi_market_cap","top_long_accounts","taker_buy_volume","taker_sell_volume"};
        if (j.contains("series") && !j["series"].is_object()) return {};
        for (size_t n=0;n<4;++n) {
            auto& values=out.values[n+3]; values.assign(out.times.size(),NAN);
            if (!j.contains("series") || !j["series"].contains(keys[n])) continue;
            const auto& series=j["series"][keys[n]];
            if (!series.is_array() || series.size()!=out.times.size()) return {};
            for (size_t i=0;i<series.size();++i) {
                if (series[i].is_null()) continue;
                if (!series[i].is_number()) return {};
                const double value=series[i].get<double>();
                if (!std::isfinite(value) || value<0 || (n==1 && value>1)) return {};
                values[i]=n<2 ? value*100 : value;
                if (!std::isfinite(values[i])) return {};
            }
        }
        out.from=from; out.to=to; out.generated=generated;
        return out;
    }
};
