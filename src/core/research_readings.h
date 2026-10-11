#pragma once
#include "workspace_document.h"
#include <vector>
#include <set>
#include <cmath>

// Registry values stay separate from formatted text for future chart consumers.
struct ResearchReadings {
    struct Item {
        std::string id, name, group, unit, description, display, availability, label, search;
        double value = NAN;
        int tone = 0; // Shared web formatter: -1 down, 0 neutral, 1 up.
        bool present() const { return availability == "present"; }
    };
    int64_t observed = 0, calculated = 0, valid_until = 0;
    std::string revision, version;
    std::vector<Item> items;
    struct Group { std::string name; std::vector<size_t> items; };
    std::vector<Group> groups;
    bool available() const { return !items.empty(); }
    bool stale(int64_t now) const { return now >= valid_until || now < calculated; }
    static ResearchReadings parse(std::string_view text, const std::string& venue, const std::string& symbol) {
        ResearchReadings out;
        const auto j = workspace::parse_bounded(text, 262144);
        if (!j.is_object() || venue != "binancef" || !j.contains("v") || j["v"] != 1) return out;
        for (const auto& [key, value] : {std::pair{"venue", venue}, {"symbol", symbol},
            {"feature_version", std::string("feature_defs.v1")}, {"feature_edition", std::string("original_live.v1")}})
            if (!j.contains(key) || j[key] != value) return out;
        auto clock = [&](const char* key) -> int64_t {
            if (!j.contains(key) || !j[key].is_number()) return 0;
            const double value = j[key].get<double>();
            return std::isfinite(value) && value > 0 && value < 9007199254740991.0 && std::floor(value) == value ? static_cast<int64_t>(value) : 0;
        };
        out.observed = clock("observed_at"); out.calculated = clock("calculated_at"); out.valid_until = clock("valid_until");
        if (!out.observed || out.observed % 60000 || out.calculated < out.observed ||
            clock("server_now") < out.calculated || out.valid_until != out.observed + 180000) return {};
        auto string = [](const auto& obj, const char* key, size_t max, std::string& dest) {
            if (!obj.contains(key) || !obj[key].is_string()) return false;
            dest = obj[key].template get<std::string>();
            return !dest.empty() && dest.size() <= max && dest.find('\0') == std::string::npos;
        };
        if (!string(j, "dataset_revision", 64, out.revision) || out.revision.size() != 64 ||
            out.revision.find_first_not_of("0123456789abcdef") != std::string::npos) return {};
        out.version = "feature_defs.v1";
        if (!j.contains("features") || !j["features"].is_array() || j["features"].empty() || j["features"].size() > 256) return {};
        std::set<std::string> seen;
        for (const auto& f : j["features"]) {
            Item item;
            if (!f.is_object() || !string(f, "id", 128, item.id) || !item.id.starts_with("feature.") || !seen.insert(item.id).second ||
                !string(f, "name", 256, item.name) || !string(f, "group", 100, item.group) ||
                !string(f, "unit", 100, item.unit) || !string(f, "description", 4096, item.description) ||
                !string(f, "availability", 32, item.availability) || !f.contains("source") || f["source"] != "original_live" ||
                !f.contains("value") || !f.contains("dtype") || !f.contains("display")) return {};
            if (f["dtype"] != "number" && f["dtype"] != "enum") return {};
            if (item.present()) {
                if (!string(f, "display", 256, item.display)) return {};
                if (f["dtype"] == "number") {
                    if (!f["value"].is_number()) return {};
                    item.value = f["value"].get<double>();
                    if (!std::isfinite(item.value)) return {};
                } else if (!string(f, "value", 100, item.label)) return {};
            } else if ((item.availability != "history_only" && item.availability != "unavailable") ||
                       !f["value"].is_null() || !f["display"].is_null()) return {};
            if (f.contains("tone") && !f["tone"].is_null()) {
                if (!item.present() || !f["tone"].is_string() || (f["tone"] != "up" && f["tone"] != "down")) return {};
                item.tone = f["tone"] == "up" ? 1 : -1;
            }
            size_t group = 0;
            while (group < out.groups.size() && out.groups[group].name != item.group) ++group;
            if (group == out.groups.size()) out.groups.push_back({item.group, {}});
            out.groups[group].items.push_back(out.items.size());
            item.search = item.name + " " + item.group + " " + item.id;
            out.items.push_back(std::move(item));
        }
        return out;
    }
};
