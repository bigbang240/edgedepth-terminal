#pragma once
#include "workspace_settings.h"
#include <string>
#include <string_view>
#include <set>

namespace workspace {
inline constexpr size_t max_bytes = 262144;
inline constexpr size_t max_named = 20;

inline void remap_window_title(std::string& ini, const std::string& old_title,
                              const std::string& new_title) {
    if (old_title.empty()) return;
    const auto old_id = old_title.rfind("###"), new_id = new_title.rfind("###");
    const std::string target = "[Window][" + new_title.substr(
        new_id == std::string::npos ? 0 : new_id + 3) + "]";
    const auto replace_header = [&](const std::string& title) {
        const std::string header = "[Window][" + title + "]";
        if (header == target) return;
        for (size_t pos = 0; (pos = ini.find(header, pos)) != std::string::npos; pos += target.size())
            ini.replace(pos, header.size(), target);
    };
    // Current ImGui writes only the ID after ###. Also accept older exports
    // retaining the marker and verbose exports retaining the visible title.
    replace_header(old_title);
    if (old_id != std::string::npos) {
        replace_header(old_title.substr(old_id));
        replace_header(old_title.substr(old_id + 3));
    }
}

inline bool valid_pair(const Json& pair) {
    if (!pair.is_object()) return false;
    for (const auto* field : {"exchange", "symbol"}) {
        if (!pair.contains(field) || !pair[field].is_string()) return false;
        const auto& value = pair[field].get_ref<const std::string&>();
        if (value.empty() || value.size() > 128 || value.find_first_of("\r\n[]") != std::string::npos) return false;
    }
    return true;
}

// Older documents did not record the page market. The primary chart's stable
// ID still records it; a workspace with only pinned charts uses its first chart.
inline Json document_market(const Json& doc) {
    if (doc.contains("market")) return doc["market"];
    for (const auto& row : doc["widgets"]) {
        if (row["type"] != "chart" || row.contains("pair")) continue;
        const auto title = row["title"].get<std::string>();
        const auto marker = title.rfind("###chart_");
        if (marker == std::string::npos) continue;
        const auto start = marker + 9;
        const auto split = title.find('_', start);
        if (split == std::string::npos) continue;
        Json pair = {{"exchange",title.substr(start, split-start)}, {"symbol",title.substr(split+1)}};
        if (valid_pair(pair)) return pair;
    }
    for (const auto& row : doc["widgets"])
        if (row["type"] == "chart" && row.contains("pair")) return row["pair"];
    return {};
}

// Reuse the layout on the addressed market. Additional views of that market
// follow it too; charts/panels of other markets remain comparisons.
inline Json for_market(Json doc, const Json& market) {
    const auto previous = document_market(doc);
    auto& rows = doc["widgets"];
    // Repair Data left behind by older restores, even if that broken layout
    // has already been saved on another route. Data for a comparison chart stays.
    for (auto& row : rows) {
        if (row["type"] != "data" || !row.contains("pair") || row["pair"] == previous) continue;
        bool comparison = false;
        for (const auto& chart : rows)
            comparison |= chart["type"] == "chart" && chart.contains("pair") && chart["pair"] == row["pair"];
        if (!comparison) row["pair"] = market;
    }
    auto selected = rows.end();
    for (auto it = rows.begin(); it != rows.end(); ++it) {
        if (it->contains("pair") && (*it)["pair"] == previous) (*it)["pair"] = market;
        if ((*it)["type"] == "chart" && !it->contains("pair") && it->value("instance",1) == 1)
            selected = it;
    }
    // Closing the primary must not make the next URL restore only an old coin.
    if (selected == rows.end()) {
        for (auto it = rows.begin(); it != rows.end(); ++it) {
            if ((*it)["type"] != "chart") continue;
            if (selected == rows.end()) selected = it;
            if (!it->contains("pair") || (*it)["pair"] == market) { selected = it; break; }
        }
        if (selected->contains("pair")) {
            const auto promoted_market = (*selected)["pair"];
            for (auto& row : rows)
                if (row.contains("pair") && row["pair"] == promoted_market) row["pair"] = market;
        }
        selected->erase("pair");
        (*selected)["instance"] = 1;
    }
    // Primary first so a comparison of the destination gets a distinct ID.
    Json primary = *selected;
    rows.erase(selected);
    rows.insert(rows.begin(),std::move(primary));
    std::set<std::string> identities;
    for (auto it = rows.begin(); it != rows.end();) {
        const auto type = (*it)["type"].get<std::string>();
        const auto pair = it->contains("pair") ? (*it)["pair"] : market;
        const auto identity = type + ":" + Json::array({pair["exchange"],pair["symbol"]}).dump();
        if (type == "chart") {
            int instance = it->value("instance",1);
            while (!identities.insert(identity + ":" + std::to_string(instance)).second)
                instance = instance % 1000 + 1;
            (*it)["instance"] = instance;
        } else if (!identities.insert(identity).second) {
            // A pinned destination panel and the following panel converge.
            // Keep one subscriber/window, using the first panel's settings.
            auto& ini = doc["layout"].get_ref<std::string&>();
            remap_window_title(ini, (*it)["title"].get<std::string>(), "###workspace_discarded");
            const auto start = ini.find("[Window][workspace_discarded]");
            if (start != std::string::npos) {
                const auto end = ini.find("\n[", start);
                ini.erase(start, end == std::string::npos ? std::string::npos : end-start+1);
            }
            it = rows.erase(it);
            continue;
        }
        ++it;
    }
    doc["market"] = market;
    return doc;
}

inline bool valid_document(const Json& j) {
    if (!j.is_object()) return false;
    auto version = j.find("version"), widgets = j.find("widgets"), ini = j.find("layout");
    if (version == j.end() || !version->is_number_integer() || *version != 1 ||
        widgets == j.end() || !widgets->is_array() || widgets->empty() || widgets->size() > 24 ||
        ini == j.end() || !ini->is_string() || ini->get_ref<const std::string&>().size() > 131072) return false;
    if (j.contains("market") && !valid_pair(j["market"])) return false;
    bool chart = false;
    std::set<std::string> identities;
    for (const auto& w : *widgets) {
        if (!w.is_object()) return false;
        auto t = w.find("type"), s = w.find("settings"), title = w.find("title");
        if (t == w.end() || !t->is_string() || s == w.end() || !s->is_object() ||
            title == w.end() || !title->is_string()) return false;
        const auto& name = t->get_ref<const std::string&>();
        if (name != "chart" && name != "dom" && name != "trades" && name != "depth" &&
            name != "data" && name != "stats" && name != "watchlist" && name != "library" && name != "paper") return false;
        // Primary rows follow the route; explicitly linked market panels and
        // secondary charts keep their own identities across reloads.
        std::string identity = name;
        if (w.contains("pair")) {
            const auto& pair = w["pair"];
            if (name != "chart" && name != "dom" && name != "trades" && name != "depth" && name != "data" && name != "stats") return false;
            if (!valid_pair(pair)) return false;
            identity += ":" + Json::array({pair["exchange"], pair["symbol"]}).dump();
        }
        if (w.contains("route_pair")) {
            const auto& previous = w["route_pair"];
            if ((name != "dom" && name != "trades") || w.contains("pair") || !previous.is_object()) return false;
            for (const auto* field : {"exchange", "symbol"}) {
                if (!previous.contains(field) || !previous[field].is_string()) return false;
                const auto& value = previous[field].get_ref<const std::string&>();
                if (value.empty() || value.size() > 128 || value.find_first_of("\r\n[]") != std::string::npos) return false;
            }
        }
        if (w.contains("instance")) {
            if (name != "chart" || !w["instance"].is_number_integer() || w["instance"] < 1 || w["instance"] > 1000) return false;
            if (w["instance"] != 1) identity += ":" + w["instance"].dump();
        }
        if (!identities.insert(identity).second || title->get_ref<const std::string&>().size() > 256) return false;
        chart |= name == "chart";
        if (name == "chart") {
            auto inds = s->find("indicators");
            if (inds != s->end() && (!inds->is_array() || inds->size() > 16)) return false;
        }
    }
    return chart;
}
inline Json parse_bounded(std::string_view text, size_t limit = max_bytes) {
    if (text.size() > limit) return {};
    // Bound nesting before parsing, including malformed input. Strings can
    // contain braces; escaped quotes must not change the scanner's state.
    int depth = 0; bool quoted = false, escaped = false;
    for (char c : text) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 16) return {}; }
        else if (c == '}' || c == ']') { if (--depth < 0) return {}; }
    }
    return Json::parse(text, nullptr, false);
}
inline Json parse_document(std::string_view text) {
    auto j = parse_bounded(text);
    return valid_document(j) ? j : Json{};
}
}
