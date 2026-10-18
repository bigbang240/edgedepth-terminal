#include "workspace_manager.h"
#include "imgui_internal.h"
#include "workspace_document.h"
#include "core/symbol_metadata.h"
#include "core/education_boot.h"
#include "core/recorder_glue.h"
#include "replayer/replay_manager.h"
#include "rendering/layout.h"
#include "rendering/theme.h"
#include "ui/chart_widget.h"
#include "ui/dom_widget.h"
#include "ui/trades_widget.h"
#include "ui/orderbook_widget.h"
#include "ui/stats_widget.h"
#include "ui/data_widget.h"
#include "ui/watchlist_widget.h"
#include "ui/replay_library_widget.h"
#include "ui/positions_panel.h"
#include <cstring>
#include <cstdlib>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(char*, workspace_read, (), {
    try { const value = localStorage.getItem('edgedepth.workspaces.v1') || '';
          if (value.length > 4194304) return 0;
          return stringToNewUTF8(value); } catch (_) { return 0; }
});
EM_JS(int, workspace_write, (const char* text), {
    try { localStorage.setItem('edgedepth.workspaces.v1', UTF8ToString(text)); return 1; }
    catch (_) { return 0; }
});
// This tab's own layout. sessionStorage is per tab, survives a reload and a
// detach into another window, and is copied by "duplicate tab", so two
// terminal windows keep their own layouts while the shared library's
// "current" stays the default a brand-new window opens with.
EM_JS(char*, workspace_read_tab, (), {
    try { const value = sessionStorage.getItem('edgedepth.workspace.tab.v1') || '';
          if (value.length > 4194304) return 0;
          return stringToNewUTF8(value); } catch (_) { return 0; }
});
EM_JS(void, workspace_write_tab, (const char* text), {
    try { sessionStorage.setItem('edgedepth.workspace.tab.v1', UTF8ToString(text)); } catch (_) {}
});
EM_JS(void, workspace_export, (const char* text), {
    const url = URL.createObjectURL(new Blob([UTF8ToString(text)], {type:'application/json'}));
    const a = document.createElement('a'); a.href = url; a.download = 'edgedepth-workspace.json';
    a.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
});
EM_JS(void, workspace_import, (), {
    const input = document.createElement('input'); input.type = 'file'; input.accept = '.json';
    input.onchange = async () => {
        const file = input.files && input.files[0]; if (!file) return;
        let text = '';
        try { if (file.size <= 262144) text = await file.text(); } catch (_) {}
        const p = stringToNewUTF8(text); _workspace_import_text(p); _free(p);
    };
    input.click();
});
#endif
namespace workspace {
namespace {
Json library = {{"version",1},{"named",Json::object()}};
Json pending, current;
std::vector<std::unique_ptr<Widget>>* panels = nullptr;
const AppContext* context = nullptr;
Terminal::Pair active_pair;
bool ready = false, enabled_now = false, was_enabled = false;
bool save_requested = false, export_requested = false;
bool focus_restored_market = false, focus_restored_data = false;
int capture_after_frame = 0;
double last_capture = -1;
double last_library_write = -1;  // shared library autosave, spaced out to limit cross-window churn
char name[49] = {};
std::string notice;
std::string last_written;
bool library_changed_elsewhere = false;  // another window wrote the shared library
const char* key(WidgetType type) {
    switch(type) {
    case WidgetType::Chart:return "chart"; case WidgetType::DOM:return "dom";
    case WidgetType::Trades:return "trades"; case WidgetType::Orderbook:return "depth";
    case WidgetType::Data:return "data"; case WidgetType::Stats:return "stats"; case WidgetType::Watchlist:return "watchlist";
    case WidgetType::ReplayLibrary:return "library"; case WidgetType::PaperTrading:return "paper";
    default:return nullptr;
    }
}
Json capture() {
    Json doc = {{"version",1},{"widgets",Json::array()},{"layout",ImGui::SaveIniSettingsToMemory()},
                {"market",{{"exchange",active_pair.exchange},{"symbol",active_pair.symbol}}}};

    for (const auto& w : *panels) {
        if (!w || !w->is_open || w->is_replay_widget) continue;
        const auto* type = key(w->type()); if (!type) continue;
        Json row = {{"type",type},{"title",w->title()},{"settings",w->save_settings()}};
        const Terminal::Pair* pair = nullptr;
        if (w->type() == WidgetType::Chart) {
            const auto& chart = *static_cast<const ChartWidget*>(w.get());
            row["instance"] = chart.chart_instance();
            if (chart.is_secondary()) pair = &chart.pair();
        } else if (w->type() == WidgetType::DOM && !w->follows_route) pair = &static_cast<const DOMWidget*>(w.get())->pair();
        else if (w->type() == WidgetType::Trades && !w->follows_route) pair = &static_cast<const TradesWidget*>(w.get())->pair();
        else if (w->type() == WidgetType::Orderbook) pair = &static_cast<const OrderbookWidget*>(w.get())->pair();
        else if (w->type() == WidgetType::Stats) pair = &static_cast<const StatsWidget*>(w.get())->pair();
        else if (w->type() == WidgetType::Data) pair = &static_cast<const DataWidget*>(w.get())->pair();
        if (pair) row["pair"] = {{"exchange",pair->exchange},{"symbol",pair->symbol}};
        if (w->follows_route && (w->type() == WidgetType::DOM || w->type() == WidgetType::Trades)) {
            const auto& linked = w->type() == WidgetType::DOM ? static_cast<const DOMWidget*>(w.get())->pair()
                                                            : static_cast<const TradesWidget*>(w.get())->pair();
            row["route_pair"] = {{"exchange",linked.exchange},{"symbol",linked.symbol}};
        }
        doc["widgets"].push_back(std::move(row));
    }
    return valid_document(doc) ? doc : Json{};
}
// Named workspaces are shared between windows, so take them from storage
// rather than from this window's memory before every write that follows a
// write elsewhere; otherwise an autosave here would drop a copy saved there.
void load_named(const Json& stored) {
    library["named"] = Json::object();
    if (!stored.is_object() || !stored.contains("named") || !stored["named"].is_object() ||
        stored["named"].size() > max_named) return;
    for (const auto& item : stored["named"].items())
        if (item.key().size() <= 48 && valid_document(item.value())) library["named"][item.key()] = item.value();
}
#ifdef __EMSCRIPTEN__
void resync_named_if_changed() {
    if (!library_changed_elsewhere) return;
    library_changed_elsewhere = false;
    char* raw = workspace_read();
    if (raw && *raw) {
        auto stored = parse_bounded(raw, 4194304);
        if (stored.is_object() && stored.contains("version") && stored["version"] == 1) load_named(stored);
    }
    std::free(raw);
}
#endif
bool persist() {
    const std::string bytes = library.dump();
    if (bytes.size() > 4194304) {
        notice = "Workspace library is full. Delete a saved copy or export this setup."; return false;
    }
    if (bytes == last_written) return true;
#ifdef __EMSCRIPTEN__
    if (!workspace_write(bytes.c_str())) {
        notice = "Browser storage unavailable or full. Export your workspace to keep it."; return false;
    }
#endif
    last_written = bytes;
    return true;
}
void apply(const Json& saved) {
    if (!valid_document(saved)) return;
    const Json market = {{"exchange",active_pair.exchange},{"symbol",active_pair.symbol}};
    const bool market_changed = document_market(saved) != market;
    const auto doc = for_market(saved, market);
    focus_restored_data = false;
#ifdef __EMSCRIPTEN__
    focus_restored_data = EM_ASM_INT({return new URLSearchParams(location.search).get('view') === 'data';});
#endif
    focus_restored_market = market_changed || focus_restored_data;
    // Retire subscribers before recreating the same keys. The context is still
    // live and stable here, and no widget iteration is in progress.
    panels->clear();
    std::string ini = doc["layout"].get<std::string>();
    std::vector<Widget*> legacy_companions;
    // Stage every old ID before assigning new market IDs. Otherwise switching
    // A to B can overwrite the saved dock of a custom B chart already present.
    size_t row_index = 0;
    for (const auto& row : doc["widgets"])
        remap_window_title(ini, row["title"].get<std::string>(),
                           "###workspace_restore_" + std::to_string(row_index++));
    row_index = 0;
    for (const auto& row : doc["widgets"]) {
        const std::string staged_title = "###workspace_restore_" + std::to_string(row_index++);
        const auto type = row["type"].get<std::string>();
        Terminal::Pair pair = active_pair;
        if (row.contains("pair")) pair = {row["pair"]["exchange"].get<std::string>(), row["pair"]["symbol"].get<std::string>()};
        const auto fmt = SymbolRegistry::instance().get_formatter(pair.exchange, pair.symbol);
        const auto tick_size = SymbolRegistry::instance().tick_or_zero(pair.exchange, pair.symbol);
        int instance = row.value("instance", 1);
        std::unique_ptr<Widget> w;
        if (type == "chart") w = std::make_unique<ChartWidget>(pair,*context,tick_size,instance);
        else if (type == "dom") w = std::make_unique<DOMWidget>(pair,*context,tick_size,20);
        else if (type == "trades") w = std::make_unique<TradesWidget>(pair,*context,fmt);
        else if (type == "depth") w = std::make_unique<OrderbookWidget>(pair,*context,fmt);
        else if (type == "data") w = std::make_unique<DataWidget>(pair,*context);
        else if (type == "stats") w = std::make_unique<StatsWidget>(pair,*context,fmt);
        else if (type == "watchlist") w = std::make_unique<WatchlistWidget>(pair,*context);
        else if (type == "library") w = std::make_unique<ReplayLibraryWidget>(*context);
        else if (type == "paper") w = std::make_unique<PositionsPanel>(*context);
        if (!w) continue;
        w->follows_route = !row.contains("pair");
        w->load_settings(row["settings"]);
        if ((type == "dom" || type == "trades") && !row.contains("pair") &&
            !row["settings"].contains("mode_companion")) {
            w->mode_companion = ini.empty(); // A preset uses the default dock tree.
            legacy_companions.push_back(w.get());
        }
        remap_window_title(ini, staged_title, w->title());
        panels->push_back(std::move(w));
    }
    ImGui::ClearIniSettings();
    if (!ini.empty()) {
        ImGui::LoadIniSettingsFromMemory(ini.c_str(),ini.size());
        // Older workspaces have no ownership flag. Recognize only the default
        // chart-left / DOM-over-tape-right split; retain arbitrary custom layouts.
        const auto node_for = [](const Widget& widget) -> ImGuiDockNode* {
            const auto* settings = ImGui::FindWindowSettingsByID(ImHashStr(widget.title()));
            return settings ? ImGui::DockBuilderGetNode(settings->DockId) : nullptr;
        };
        ImGuiDockNode* chart_node = nullptr;
        for (const auto& w : *panels)
            if (w->type() == WidgetType::Chart && !static_cast<ChartWidget*>(w.get())->is_secondary())
                chart_node = node_for(*w);
        for (auto* w : legacy_companions) {
            const auto* node = node_for(*w);
            const auto* column = node ? node->ParentNode : nullptr;
            const auto* split = column ? column->ParentNode : nullptr;
            const int slot = w->type() == WidgetType::DOM ? 0 : 1;
            w->mode_companion = chart_node && column && split &&
                column->SplitAxis == ImGuiAxis_Y && column->ChildNodes[slot] == node &&
                split->SplitAxis == ImGuiAxis_X && split->ChildNodes[0] == chart_node &&
                split->ChildNodes[1] == column;
        }
        // Floating, tabbed and temporarily hidden windows are valid layouts.
        // ImGui retains their DockId and split SizeRef while not submitted.
        LayoutManager::restore_layout_for(active_pair.exchange,active_pair.symbol);

    }
    else LayoutManager::reset_layout_for(active_pair.exchange,active_pair.symbol);
}
Json preset(int index) {
    Json settings = {{"chart_type",index == 0 ? 1 : 0},{"timeframe",60},
        {"rt_mode",index == 1},{"liq_dense_field",index == 3},{"liq_profile_enabled",false},
        {"heatmap_enabled",index == 1},{"indicators",Json::array()}};
    if (index != 1) {
        settings["indicators"].push_back({{"name","Vol (USDT)"}});
        settings["indicators"].push_back({{"name","CVD"}});
    }
    if (index == 0) settings["footprint"] = {{"comparison",1},{"stacked_levels",3}};
    Json d = {{"version",1},{"layout",""},{"widgets",Json::array()}};
    for (const auto* type : {"chart","dom","trades","watchlist"})
        d["widgets"].push_back({{"type",type},{"title",""},
            {"settings",std::string(type)=="chart" ? settings :
                (std::string(type)=="dom" || std::string(type)=="trades") ?
                    Json{{"mode_companion",true}} : Json::object()}});
    if (index == 2) d["widgets"].push_back({{"type","library"},{"title",""},{"settings",Json::object()}});
    return d;
}
}
bool enabled() { return enabled_now; }
void checkpoint_live() {
    if (!ready || !enabled_now || !panels) return;
    auto next = capture();
    if (!next.is_null() && next.dump().size() <= max_bytes) current = std::move(next);
}
void flush() {
    if (!ready || !enabled_now || !panels || !context || context->replay_mgr().is_active() ||
        ImGui::GetFrameCount() <= capture_after_frame) return;
    auto next = capture();
    if (next.is_null() || next.dump().size() > max_bytes) return;
    current = std::move(next); library["current"] = current; persist();
#ifdef __EMSCRIPTEN__
    workspace_write_tab(current.dump().c_str());
#endif
}
void tick(std::vector<std::unique_ptr<Widget>>& widgets, const AppContext& ctx,
          const Terminal::Pair& pair, bool enabled) {
    panels = &widgets; context = &ctx; active_pair = pair;
    enabled_now = enabled;
#ifdef __EMSCRIPTEN__
    // Before this frame's save, delete or autosave, not inside persist():
    // a resync there would discard the copy being saved right now.
    resync_named_if_changed();
#endif
    if (!enabled) { was_enabled = false; return; }
    if (!ready) {
        ready = true;
#ifdef __EMSCRIPTEN__
        char* raw = workspace_read();
        if (raw && *raw) {
            auto stored = parse_bounded(raw,4194304); std::free(raw);
            if (stored.is_object() && stored.contains("version") && stored["version"] == 1 &&
                stored.contains("named") && stored["named"].is_object() && stored["named"].size() <= max_named) {
                load_named(stored);
                if (stored.contains("current") && valid_document(stored["current"])) {
                    pending = stored["current"];
                    if (!stored.contains("layout_revision") || stored["layout_revision"] != 2) {
                        bool rt = false;
                        for (const auto& row : pending["widgets"])
                            if (row["type"] == "chart")
                                rt = row["settings"].value("rt_mode", Json(false)) == true;
                        pending = preset(rt ? 1 : 3);
                        notice = "Default layout repaired. Your named workspaces are still available.";
                    }
                }
            } else if (!stored.is_null()) notice = "Saved workspace could not be read. Defaults are available.";
        } else std::free(raw);
        // This tab's own last layout wins over the shared default, so a
        // second window keeps its layout across a reload while the first
        // window keeps autosaving its own. Only this build writes the tab
        // document, so it never needs the layout-revision repair above.
        if (char* tab = workspace_read_tab()) {
            if (*tab) {
                auto doc = parse_document(tab);
                if (!doc.is_null()) pending = std::move(doc);
            }
            std::free(tab);
        }
        // A first session, or one whose saved layout could not be used, starts
        // from the same preset Reset Layout produces, so the Candles default
        // (volume and CVD subplots) applies without a saved workspace.
        if (pending.is_null()) reset_default();
        EM_ASM({
            window.addEventListener('pagehide', () => _workspace_flush());
            document.addEventListener('visibilitychange', () => { if (document.hidden) _workspace_flush(); });
            window.addEventListener('storage', (event) => {
                if (event.key === 'edgedepth.workspaces.v1') _workspace_storage_changed();
            });
        });
#endif
    } else if (!was_enabled && !current.is_null()) pending = current;
    was_enabled = true;
    if (!pending.is_null()) {
        apply(pending); pending = Json{}; last_capture = -1;
        library["layout_revision"] = 2;
        // DockBuilder and new windows need a completed frame before capture.
        capture_after_frame = ImGui::GetFrameCount() + 1;
        return;
    }
    if (focus_restored_market) {
        // Newly appearing comparison windows may take focus on their first
        // Begin. Wait one frame so the requested market wins after all exist.
        focus_restored_market = false;
        for (const auto& w : widgets) {
            if (focus_restored_data && w->type() == WidgetType::Data) {
                const auto& target = static_cast<DataWidget*>(w.get())->pair();
                if (target.exchange != pair.exchange || target.symbol != pair.symbol) continue;
            } else if (!focus_restored_data && w->type() == WidgetType::Chart) {
                if (static_cast<ChartWidget*>(w.get())->is_secondary()) continue;
            } else continue;
            ImGui::SetWindowFocus(w->title());
            break;
        }
    }
    if (ImGui::GetFrameCount() <= capture_after_frame) return;
    library["layout_revision"] = 2;
    if (save_requested || export_requested || ImGui::GetTime() - last_capture >= 1.0) {
        auto next = capture(); last_capture = ImGui::GetTime();
        if (next.is_null() || next.dump().size() > max_bytes) {
            save_requested = export_requested = false; return;
        }
        current = std::move(next); library["current"] = current;
        // The tab document carries this window's layout every second; the
        // shared library's "current" is only the default for a new window,
        // and every write to it wakes every other terminal window (storage
        // event, named resync), so space those out. Explicit saves, pagehide
        // and hidden-tab flushes still write immediately.
        if (save_requested || last_library_write < 0 || ImGui::GetTime() - last_library_write >= 10.0) {
            persist(); last_library_write = ImGui::GetTime();
        }
#ifdef __EMSCRIPTEN__
        workspace_write_tab(current.dump().c_str());
#endif
        if (save_requested && !current.is_null()) {
            library["named"][name] = current;
            if (persist()) notice = "Workspace saved in this browser.";
        }
#ifdef __EMSCRIPTEN__
        if (export_requested && !current.is_null()) workspace_export(current.dump(2).c_str());
#endif
        save_requested = export_requested = false;
    }
}
void reset_default() {
    if (!enabled_now || !panels) { LayoutManager::reset_layout(); return; }
    bool rt = false;
    for (const auto& w : *panels)
        if (w && w->type() == WidgetType::Chart) {
            rt = static_cast<const ChartWidget*>(w.get())->rt_mode(); break;
        }
    pending = preset(rt ? 1 : 3);
}
void menu() {
    ImGui::TextDisabled("Workspaces apply to the current market");
    ImGui::TextDisabled("Changes are saved in this browser");
    ImGui::BeginDisabled(!enabled_now);
    if (ImGui::BeginMenu("Presets")) {
        const char* labels[] = {"Order Flow", "Liquidity", "Replay Review", "Candles"};
        for (int i=0;i<4;++i) if (ImGui::MenuItem(labels[i])) pending = preset(i);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Saved workspaces")) {
        if (library["named"].empty()) ImGui::TextDisabled("No saved workspaces yet");
        std::string remove;
        for (const auto& item : library["named"].items()) {
            ImGui::PushID(item.key().c_str());
            if (ImGui::BeginMenu(item.key().c_str())) {
                if (ImGui::MenuItem("Load")) pending = item.value();
                if (ImGui::MenuItem("Delete saved copy")) remove = item.key();
                ImGui::EndMenu();
            }
            ImGui::PopID();
        }
        if (!remove.empty()) { library["named"].erase(remove); persist(); }
        ImGui::EndMenu();
    }
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##workspace_name","Workspace name",name,sizeof(name));
    const bool exists = library["named"].contains(name);
    ImGui::BeginDisabled(!name[0] || (!exists && library["named"].size() >= max_named));
    if (ImGui::Button(exists ? "Update saved workspace" : "Save workspace")) save_requested = true;
    ImGui::EndDisabled();
    if (ImGui::MenuItem("Export current workspace")) export_requested = true;
#ifdef __EMSCRIPTEN__
    if (ImGui::MenuItem("Import workspace...")) workspace_import();
#endif
    ImGui::EndDisabled();
    if (!enabled_now) ImGui::TextDisabled("Return to live to manage workspaces");
    if (!notice.empty()) { ImGui::Separator(); ImGui::TextWrapped("%s",notice.c_str()); }
}
void import_text(const char* text) {
    auto doc = parse_document(text ? text : "");
    if (doc.is_null()) { notice = "Invalid workspace. Use a version 1 export under 256 KiB."; return; }
    if (!enabled_now) { notice = "Return to live before importing a workspace."; return; }
    pending = std::move(doc); notice = "Imported workspace. Save a name to keep another copy.";
}
}
#ifdef __EMSCRIPTEN__
extern "C" {
EMSCRIPTEN_KEEPALIVE void workspace_flush() { workspace::flush(); }
EMSCRIPTEN_KEEPALIVE void workspace_import_text(const char* text) { workspace::import_text(text); }
EMSCRIPTEN_KEEPALIVE void workspace_storage_changed() { workspace::library_changed_elsewhere = true; }
}
#endif
