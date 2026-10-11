#include "rendering/menu.h"
#include "rendering/layout.h"
#include "rendering/theme.h"
#include "ui/widget.h"
#include "ui/trades_widget.h"
#include "ui/orderbook_widget.h"
#include "ui/dom_widget.h"
#include "ui/stats_widget.h"
#include "ui/data_widget.h"
#include "imgui_internal.h"
#include "ui/debug_widget.h"
#include "ui/positions_panel.h"
#include "ui/replay_library_widget.h"
#include "ui/watchlist_widget.h"
#include "stream_handler.h"
#include "types/types.h"
#include "imgui.h"
#include "ui/chart_widget.h"
#include "core/app_context.h"
#include "core/symbol_metadata.h"
#include "core/ticker_manager.h"
#include "core/logo_manager.h"
#include "ui/drawing/drawing_icons.h"
#include "core/scanner_manager.h"
#include "core/stream_presence.h"
#include "core/url_router.h"
#include "core/entitlements.h"
#include "replayer/replay_manager.h"
#include "ui/upsell_modal.h"
#include <cctype>
#include <cfloat>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <unordered_set>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace Menu {

namespace {
    PriceFormatter fmt_for(const SymbolMetadata& meta) { return meta.fmt; }
    double tick_for(const SymbolMetadata& meta) { return meta.tick_size; }
    Terminal::Pair pair_for(const SymbolMetadata& meta) {
        return Terminal::Pair{meta.exchange, meta.symbol};
    }

    bool matches_search(const std::string& haystack, const char* needle) {
        if (needle[0] == '\0') return true;
        std::string lower_hay, lower_needle;
        for (char c : haystack) lower_hay += static_cast<char>(tolower(static_cast<unsigned char>(c)));
        for (const char* c = needle; *c; ++c)
            lower_needle += static_cast<char>(tolower(static_cast<unsigned char>(*c)));
        return lower_hay.find(lower_needle) != std::string::npos;
    }

    bool matches_category(const SymbolMetadata& meta, int cat_idx, const std::vector<std::string>& cats) {
        if (cat_idx < 0) return true;
        if (cat_idx >= static_cast<int>(cats.size())) return true;
        const auto& target = cats[cat_idx];
        for (const auto& c : meta.categories) {
            if (c == target) return true;
        }
        return false;
    }

    void format_volume(char* buf, size_t buf_size, double vol) {
        if (vol >= 1e9)      snprintf(buf, buf_size, "%.2fB", vol / 1e9);
        else if (vol >= 1e6) snprintf(buf, buf_size, "%.2fM", vol / 1e6);
        else if (vol >= 1e3) snprintf(buf, buf_size, "%.1fK", vol / 1e3);
        else                 snprintf(buf, buf_size, "%.0f", vol);
    }

    // ── Recents / favorites persistence (comma-joined symbol lists in localStorage) ──
    constexpr const char* LS_RECENTS = "edgedepth.picker.recents";
    constexpr const char* LS_FAVS    = "edgedepth.picker.favorites";

    std::string ls_get(const char* key) {
#ifdef __EMSCRIPTEN__
        char buf[2048];
        buf[0] = '\0';
        EM_ASM({
            var v = localStorage.getItem(UTF8ToString($0)) || "";
            stringToUTF8(v, $1, $2);
        }, key, buf, (int)sizeof(buf));
        return std::string(buf);
#else
        (void)key;
        return std::string();
#endif
    }
    void ls_set(const char* key, const std::string& val) {
#ifdef __EMSCRIPTEN__
        EM_ASM({ localStorage.setItem(UTF8ToString($0), UTF8ToString($1)); }, key, val.c_str());
#else
        (void)key; (void)val;
#endif
    }
    std::vector<std::string> split_csv(const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }
    std::string join_csv(const std::vector<std::string>& v) {
        std::string out;
        for (size_t i = 0; i < v.size(); ++i) { if (i) out += ','; out += v[i]; }
        return out;
    }
    void persist_recents() { ls_set(LS_RECENTS, join_csv(g_symbol_picker.recents)); }
    void persist_favorites() {
        std::vector<std::string> v(g_symbol_picker.favorites.begin(), g_symbol_picker.favorites.end());
        ls_set(LS_FAVS, join_csv(v));
    }
    void load_persistence() {
        g_symbol_picker.recents = split_csv(ls_get(LS_RECENTS));
        if (g_symbol_picker.recents.size() > 8) g_symbol_picker.recents.resize(8);
        auto favs = split_csv(ls_get(LS_FAVS));
        g_symbol_picker.favorites.clear();
        g_symbol_picker.favorites.insert(favs.begin(), favs.end());
    }

}

// ═══════════════════════════════════════════════════════════════════════════════
// Symbol Picker Popup
// ═══════════════════════════════════════════════════════════════════════════════

void render_symbol_picker_popup(
    std::vector<std::unique_ptr<Widget>>& widgets, const AppContext& ctx)
{
    // ── Subscribe/unsubscribe to ticker24h on picker open/close ──────
    static bool was_open = false;
    static bool ticker_subscribed = false;
    static bool scanner_subscribed = false;
    if (g_symbol_picker.open && !was_open) {
        // Picker just opened - subscribe to the scanner for every venue (the feeds
        // are venue-parameterized: scanner.{binancef,hl,bybit}.global; ScannerManager
        // keys by exchange). ticker24h is always-on, owned by AppShell - don't manage it here.
        if (!scanner_subscribed) {
            for (const char* ex : {"binancef", "hl", "bybit"}) {
                StreamKey scanner_key{
                    Terminal::Pair{ex, "global"},
                    Terminal::Stream::Scanner,
                    0
                };
                ctx.stream_mgr().send_subscribe(scanner_key);
            }
            scanner_subscribed = true;
        }
        g_symbol_picker.cached_categories = SymbolRegistry::instance().unique_categories();
        g_symbol_picker.selected_category = -1;
        g_symbol_picker.invalidate_cache();

        if (!g_symbol_picker.persistence_loaded) {
            load_persistence();
            g_symbol_picker.persistence_loaded = true;
        }
        g_symbol_picker.keyboard_sel = -1;
    }
    // Per-category active-symbol counts on the selected venue, parallel to cached_categories
    // (the list shows one venue, so a count across venues would not match it). Recounted on
    // open and when the venue changes.
    {
        static std::string counted_for;
        static bool counted_open = false;
        if (g_symbol_picker.open && (!counted_open || counted_for != g_symbol_picker.selected_exchange)) {
            auto& cc = g_symbol_picker.cached_category_counts;
            cc.assign(g_symbol_picker.cached_categories.size(), 0);
            for (const auto& [k, m] : SymbolRegistry::instance().all()) {
                if (!m.is_active || m.exchange != g_symbol_picker.selected_exchange) continue;
                for (const auto& c : m.categories) {
                    for (size_t ci = 0; ci < g_symbol_picker.cached_categories.size(); ++ci) {
                        if (g_symbol_picker.cached_categories[ci] == c) { cc[ci]++; break; }
                    }
                }
            }
            counted_for = g_symbol_picker.selected_exchange;
        }
        counted_open = g_symbol_picker.open;
    }
    if (!g_symbol_picker.open && was_open) {
        // Picker just closed - unsubscribe from scanner only
        if (scanner_subscribed) {
            for (const char* ex : {"binancef", "hl", "bybit"}) {
                StreamKey scanner_key{
                    Terminal::Pair{ex, "global"},
                    Terminal::Stream::Scanner,
                    0
                };
                ctx.stream_mgr().send_unsubscribe(scanner_key);
            }
            scanner_subscribed = false;
        }
    }
    was_open = g_symbol_picker.open;

    if (!g_symbol_picker.open) return;
    auto& reg = SymbolRegistry::instance();
    if (!reg.is_loaded()) return;

    // ── Design v3 (James 2026-10-11: "symbol picker redesigned / repolished") ──
    // A rounded card: title, venue segmented control with logos, Esc keycap and close;
    // a rounded search field with a Ctrl K keycap; recent pills with logos; category
    // filter pills; a quiet sortable header; rows with logos, a change pill, volume and
    // a type chip (Score only when some row has one); keycap hints in the footer.
    using namespace Theme;
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    const ImVec2 viewport_size = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSize(ImVec2(std::min(1040.0f, viewport_size.x - 32.0f),
                                  std::min(700.0f, viewport_size.y - 32.0f)), ImGuiCond_Appearing);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Radius::R3 + 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Tokens::ELEV);
    ImGui::PushStyleColor(ImGuiCol_Border, Tokens::BD2);

    ImGui::OpenPopup("Find a symbol");
    if (!ImGui::BeginPopupModal("Find a symbol", &g_symbol_picker.open,
                                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        g_symbol_picker.open = false;
        g_symbol_picker.compare_mode = false;
        ImGui::CloseCurrentPopup();
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 w0 = ImGui::GetWindowPos();
    const float  W  = ImGui::GetWindowWidth();
    const float  K  = ImGui::GetFontSize() / 18.4f;   // the viewer's text size
    const float  PADX = 22.0f * K;
    ImFont* ui_f = Fonts::ui();
    ImFont* semi_f = Fonts::ui_semibold();
    ImFont* mono_f = Fonts::mono();
    auto text_w = [](ImFont* f, float px, const char* t) { return f->CalcTextSizeA(px, FLT_MAX, 0, t).x; };
    auto put = [&](ImDrawList* d, ImFont* f, float px, ImVec2 at, const ImVec4& c, const char* t, const char* e = nullptr) {
        d->AddText(f, px, at, u32(c), t, e);
    };
    // A keycap: rounded, hairlined, quiet text. Returns its width.
    // An empty key draws up/down chevrons (the UI font has no arrow glyphs).
    auto keycap_w = [&](const char* k) { return k[0] ? text_w(ui_f, 12.0f * K, k) + 12.0f * K : 30.0f * K; };
    auto keycap = [&](ImDrawList* d, ImVec2 p, const char* k) {
        const float px = 12.0f * K, h = 20.0f * K;
        const float w = keycap_w(k);
        d->AddRectFilled(p, ImVec2(p.x + w, p.y + h), u32(Tokens::CONTROL), Radius::R1);
        d->AddRect(p, ImVec2(p.x + w, p.y + h), u32(Tokens::BD2), Radius::R1);
        if (k[0]) put(d, ui_f, px, ImVec2(p.x + 6.0f * K, p.y + (h - px * 1.17f) * 0.5f), Tokens::TX2, k);
        else {
            drawing::draw_ui_icon(d, drawing::UiIcon::ChevronUp, ImVec2(p.x + w * 0.33f, p.y + h * 0.5f), 3.2f * K, u32(Tokens::TX2), 1.3f);
            drawing::draw_ui_icon(d, drawing::UiIcon::ChevronDown, ImVec2(p.x + w * 0.67f, p.y + h * 0.5f), 3.2f * K, u32(Tokens::TX2), 1.3f);
        }
        return w;
    };

    // Open a symbol: record it as recent, then add the pending widget (or, in
    // replace-mode, reload the terminal at that symbol). Shared by the row click,
    // the recent chips, and keyboard Enter.
    auto open_symbol = [&](const SymbolMetadata& m) {
        auto pair = pair_for(m); auto fmt = fmt_for(m); auto tick = tick_for(m);
        g_symbol_picker.record_recent(m.symbol);
        persist_recents();
        if (g_symbol_picker.compare_mode) {
            // Two markets on one clock: navigate to the focused replay of the
            // anchored chart with this row as its compare market. Same venue
            // only (a session is single-venue); the picker was pinned to it.
            g_symbol_picker.compare_mode = false;
            if (m.exchange != g_symbol_picker.compare_exchange) {
                ui::UpsellModal::instance().toast("Compare replays play one venue at a time. Pick a market on the same exchange.");
            } else if (m.symbol != g_symbol_picker.compare_symbol) {
                ctx.replay_mgr().open_focused_replay_at(
                    g_symbol_picker.compare_exchange, g_symbol_picker.compare_symbol,
                    g_symbol_picker.compare_anchor_ms, {m.symbol});
            }
            g_symbol_picker.open = false;
            g_symbol_picker.replace_mode = false;
            ImGui::CloseCurrentPopup();
            return;
        }
        if (g_symbol_picker.replace_mode) {
            // url_navigate carries ?ws= (and any other user params) across the
            // reload; a bare location.href would drop a self-hosted feed here.
            url_navigate(build_terminal_path(m.exchange, m.symbol));
        } else {
            g_widget_add_request = {g_symbol_picker.pending, pair, fmt, tick, true};
        }
        g_symbol_picker.open = false;
        g_symbol_picker.replace_mode = false;
        ImGui::CloseCurrentPopup();
    };

    // ── header: title, venue segmented control (logos), Esc keycap, close ─
    const float head_h = 64.0f * K;
    {
        const float title_px = 19.0f * K;
        put(dl, semi_f, title_px, ImVec2(w0.x + PADX, w0.y + (head_h - title_px * 1.17f) * 0.5f), Tokens::TX1, "Search markets");
        const float tw = text_w(semi_f, title_px, "Search markets");
        struct Venue { const char* id; const char* label; };
        const Venue venues[3] = { {"binancef", "Binance"}, {"hl", "Hyperliquid"}, {"bybit", "Bybit"} };
        const float seg_h = 34.0f * K, logo = 16.0f * K, inset = 3.0f * K, label_px = 14.0f * K;
        float widths[3], total = inset * 2;
        for (int i = 0; i < 3; ++i) { widths[i] = 14.0f * K + logo + 8.0f * K + text_w(ui_f, label_px, venues[i].label) + 14.0f * K; total += widths[i]; }
        const ImVec2 sp(w0.x + PADX + tw + 24.0f * K, w0.y + (head_h - seg_h) * 0.5f);
        dl->AddRectFilled(sp, ImVec2(sp.x + total, sp.y + seg_h), u32(Tokens::TRACK), Radius::R2 + 2.0f);
        dl->AddRect(sp, ImVec2(sp.x + total, sp.y + seg_h), u32(Tokens::BD1), Radius::R2 + 2.0f);
        float x = sp.x + inset;
        for (int i = 0; i < 3; ++i) {
            const bool active = g_symbol_picker.selected_exchange == venues[i].id;
            // A self-hosted feed without this venue would open empty panels;
            // keep the segment visible but disabled so the boundary is explicit.
            const bool usable = active || Entitlements::venue_available(
                venues[i].id, TickerManager::instance().has_venue(venues[i].id));
            ImGui::SetCursorScreenPos(ImVec2(x, sp.y));
            ImGui::PushID(900 + i);
            const bool clk = ImGui::InvisibleButton("##venue", ImVec2(widths[i], seg_h));
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            if (active || (hov && usable))
                dl->AddRectFilled(ImVec2(x, sp.y + inset), ImVec2(x + widths[i], sp.y + seg_h - inset),
                                  u32(active ? Tokens::CONTROL_ACTIVE : Tokens::CONTROL), Radius::R2);
            if (hov && !usable)
                Theme::tooltip("Hosted only: this feed does not serve %s.\nThe bundled community gateway covers Binance.",
                               venues[i].label);
            const ImVec2 lp(x + 14.0f * K, sp.y + (seg_h - logo) * 0.5f);
            if (usable) LogoManager::instance().draw_exchange(dl, venues[i].id, lp, logo);
            else if (ImTextureID tex = LogoManager::instance().exchange(venues[i].id))
                dl->AddImage(tex, lp, ImVec2(lp.x + logo, lp.y + logo), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 90));
            put(dl, ui_f, label_px, ImVec2(x + 14.0f * K + logo + 8.0f * K, sp.y + (seg_h - label_px * 1.17f) * 0.5f),
                active ? Tokens::TX1 : !usable ? Tokens::TX4 : hov ? Tokens::TX2 : Tokens::TX3, venues[i].label);
            if (clk && !active && usable) {
                g_symbol_picker.selected_exchange = venues[i].id;
                g_symbol_picker.selected_category = -1; // categories are per-venue
                g_symbol_picker.invalidate_cache();
            }
            x += widths[i];
        }
        const float ib = 30.0f * K;
        ImGui::SetCursorScreenPos(ImVec2(w0.x + W - PADX - ib + 6.0f * K, w0.y + (head_h - ib) * 0.5f));
        if (Theme::icon_button("##pick_close", drawing::UiIcon::Close, nullptr, ib)) { g_symbol_picker.open = false; ImGui::CloseCurrentPopup(); }
        const float ew = text_w(ui_f, 12.0f * K, "Esc") + 12.0f * K;
        keycap(dl, ImVec2(w0.x + W - PADX - ib - 8.0f * K - ew, w0.y + (head_h - 20.0f * K) * 0.5f), "Esc");
    }

    // ── search: a rounded field, magnifier inside, Ctrl K keycap ─────────
    const float srch_y = w0.y + head_h;
    const float srch_h = 48.0f * K;
    {
        const float fx = w0.x + PADX, fw = W - PADX * 2, fh = 44.0f * K;
        Theme::push_font(ui_f, 15.5f * K);
        ImGui::SetCursorScreenPos(ImVec2(fx, srch_y));
        ImGui::SetNextItemWidth(fw);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Tokens::CONTROL);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Tokens::CONTROL_HOVER);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Tokens::CONTROL_HOVER);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, Tokens::TX3);
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(42.0f * K, (fh - ImGui::GetFontSize()) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::R2 + 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        // The search keeps the keyboard: refocus it once a chip or row click ends, and own
        // Up/Down so ImGui's keyboard nav does not move focus off it (the list handles them).
        if (ImGui::IsWindowAppearing() ||
            (!ImGui::IsAnyItemActive() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)))
            ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##sym_search", "Search by symbol or category",
            g_symbol_picker.search_buf, sizeof(g_symbol_picker.search_buf));
        const bool focused = ImGui::IsItemActive();
        if (focused) {
            ImGui::SetItemKeyOwner(ImGuiKey_UpArrow);
            ImGui::SetItemKeyOwner(ImGuiKey_DownArrow);
        }
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(5);
        ImGui::PopFont();
        dl->AddRect(a, b, u32(focused ? Tokens::BD3 : Tokens::BD1), Radius::R2 + 2.0f);
        drawing::draw_ui_icon(dl, drawing::UiIcon::Search, ImVec2(a.x + 21.0f * K, (a.y + b.y) * 0.5f), 7.0f * K, u32(Tokens::TX2), 1.5f);
        const float kw = text_w(ui_f, 12.0f * K, "Ctrl K") + 12.0f * K;
        keycap(dl, ImVec2(b.x - 12.0f * K - kw, (a.y + b.y) * 0.5f - 10.0f * K), "Ctrl K");
    }

    // ── recent: pills with logos (persisted) ──────────────────────────────
    const float rec_y = srch_y + srch_h + 6.0f * K;
    float rec_h = 0.0f;
    const float label_col = 104.0f * K;   // section labels' column
    if (!g_symbol_picker.recents.empty()) {
        rec_h = 44.0f * K;
        const float lpx = 13.0f * K;
        put(dl, ui_f, lpx, ImVec2(w0.x + PADX, rec_y + (rec_h - lpx * 1.17f) * 0.5f), Tokens::TX3, "Recent");
        const float rch = 32.0f * K, rcy = rec_y + (rec_h - rch) * 0.5f, rxmax = w0.x + W - PADX;
        float rx = w0.x + PADX + label_col;
        int shown = 0;
        for (const auto& sym : g_symbol_picker.recents) {
            if (shown >= 8) break;
            // Recents are bare symbols (no venue stored); they are binancef-historical.
            const auto* m = SymbolRegistry::instance().get("binancef", sym);
            std::string up = m ? (m->base_asset.empty() ? sym : m->base_asset) : sym;
            std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c) { return std::toupper(c); });
            const auto* tk = TickerManager::instance().get("binancef", sym);
            char pctbuf[16] = "";
            if (tk) snprintf(pctbuf, sizeof(pctbuf), "%+.2f%%", tk->change_pct_24h);
            const float spx = 14.5f * K, ppx = 13.0f * K, lg = 20.0f * K;
            const float symw = text_w(semi_f, spx, up.c_str());
            const float pctw = tk ? text_w(ui_f, ppx, pctbuf) + 8.0f * K : 0.0f;
            const float cw = 6.0f * K + lg + 8.0f * K + symw + pctw + 14.0f * K;
            if (rx + cw > rxmax) break;
            ImGui::SetCursorScreenPos(ImVec2(rx, rcy));
            ImGui::PushID(3000 + shown);
            const bool clk = ImGui::InvisibleButton("##rec", ImVec2(cw, rch));
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            dl->AddRectFilled(ImVec2(rx, rcy), ImVec2(rx + cw, rcy + rch), u32(hov ? Tokens::CONTROL_HOVER : Tokens::CONTROL), rch * 0.5f);
            LogoManager::instance().draw_coin(dl, m ? m->base_asset : up, ImVec2(rx + 6.0f * K, rcy + (rch - lg) * 0.5f), lg);
            put(dl, semi_f, spx, ImVec2(rx + 6.0f * K + lg + 8.0f * K, rcy + (rch - spx * 1.17f) * 0.5f), Tokens::TX1, up.c_str());
            if (tk) put(dl, ui_f, ppx, ImVec2(rx + 6.0f * K + lg + 8.0f * K + symw + 8.0f * K, rcy + (rch - ppx * 1.17f) * 0.5f),
                        tk->change_pct_24h >= 0.0 ? Tokens::UP : Tokens::DOWN, pctbuf);
            if (clk && m) open_symbol(*m);
            rx += cw + 8.0f * K;
            shown++;
        }
    }

    // ── categories: filter pills (selected = solid), wrapping, with counts ─
    const float cat_y = rec_y + rec_h;
    const float chh = 30.0f * K, chip_gap = 8.0f * K, row_gap = 8.0f * K, cat_top = 6.0f * K, cat_bot = 14.0f * K;
    float cat_h = 0.0f;
    {
        const float lpx = 13.0f * K, cpx = 14.0f * K;
        put(dl, ui_f, lpx, ImVec2(w0.x + PADX, cat_y + cat_top + (chh - lpx * 1.17f) * 0.5f), Tokens::TX3, "Categories");
        const auto& cats = g_symbol_picker.cached_categories;
        const auto& counts = g_symbol_picker.cached_category_counts;
        const float x0 = w0.x + PADX + label_col, xmax = w0.x + W - PADX;
        float chx = x0, chy = cat_y + cat_top;
        auto cat_chip = [&](const char* label, int count, bool selected, int idx) -> bool {
            char nbuf[16] = "";
            float nw = 0.0f;
            if (count >= 0) { snprintf(nbuf, sizeof(nbuf), "%d", count); nw = text_w(ui_f, cpx, nbuf) + 7.0f * K; }
            const float lw = text_w(ui_f, cpx, label);
            const float cw2 = lw + nw + 26.0f * K;
            if (chx + cw2 > xmax) { chx = x0; chy += chh + row_gap; }   // wrap to next line
            ImGui::SetCursorScreenPos(ImVec2(chx, chy));
            ImGui::PushID(4000 + idx);
            const bool clk = ImGui::InvisibleButton("##cat", ImVec2(cw2, chh));
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            dl->AddRectFilled(ImVec2(chx, chy), ImVec2(chx + cw2, chy + chh),
                              u32(selected ? Tokens::TX1 : hov ? Tokens::CONTROL_HOVER : Tokens::CONTROL), chh * 0.5f);
            const float ty = chy + (chh - cpx * 1.17f) * 0.5f;
            put(dl, ui_f, cpx, ImVec2(chx + 13.0f * K, ty), selected ? Tokens::BRAND_INK : hov ? Tokens::TX1 : Tokens::TX2, label);
            if (count >= 0)
                put(dl, ui_f, cpx, ImVec2(chx + 13.0f * K + lw + 7.0f * K, ty), selected ? ImVec4(0.25f, 0.25f, 0.25f, 1.0f) : Tokens::TX3, nbuf);
            chx += cw2 + chip_gap;
            return clk;
        };
        if (cat_chip("All", g_symbol_picker.exchange_total,
                     g_symbol_picker.selected_category < 0, -1))
            g_symbol_picker.selected_category = -1;
        for (int i = 0; i < static_cast<int>(cats.size()); ++i) {
            const bool sel = (g_symbol_picker.selected_category == i);
            const int cnt = i < static_cast<int>(counts.size()) ? counts[i] : -1;
            if (cnt == 0 && !sel) continue;   // nothing in it on this venue
            if (cat_chip(cats[i].c_str(), cnt, sel, i))
                g_symbol_picker.selected_category = sel ? -1 : i;
        }
        cat_h = (chy + chh + cat_bot) - cat_y;
    }

    // ── Build filtered + sorted list (cached - only rebuild when inputs change) ──
    auto& ticker_mgr = TickerManager::instance();
    auto& scanner_mgr = ScannerManager::instance();
    const auto& cats = g_symbol_picker.cached_categories;
    bool filter_dead = ticker_mgr.count() > 400;

    // Detect if any input has changed since last frame
    bool search_changed = (strcmp(g_symbol_picker.search_buf, g_symbol_picker.prev_search) != 0);
    bool cat_changed = (g_symbol_picker.selected_category != g_symbol_picker.prev_category);
    bool exch_changed = (g_symbol_picker.selected_exchange != g_symbol_picker.prev_exchange);
    bool sort_changed = (g_symbol_picker.sort_mode != g_symbol_picker.prev_sort_mode ||
                         g_symbol_picker.sort_ascending != g_symbol_picker.prev_sort_asc);

    // Live ticker/scanner drift only forces a rebuild every 2s - re-sorting
    // Hundreds of symbols with per-element manager lookups several times a second was
    // the picker's real cost (immediate-mode redraw is already clipped/cheap).
    const int64_t now_ms_pick = static_cast<int64_t>(ImGui::GetTime() * 1000.0);
    bool data_drift = (now_ms_pick - g_symbol_picker.last_data_rebuild_ms > 2000) &&
                      (ticker_mgr.last_update_ms() != g_symbol_picker.prev_ticker_update_ms ||
                       scanner_mgr.last_update_ms() != g_symbol_picker.prev_scanner_update_ms);

    if (g_symbol_picker.cache_dirty || search_changed || cat_changed || exch_changed || sort_changed || data_drift) {
        auto& visible = g_symbol_picker.cached_visible;
        visible.clear();
        visible.reserve(reg.size());
        int ex_total = 0;
        for (const auto& [key, meta] : reg.all()) {
            if (!meta.is_active) continue;
            // Venue filter: the picker shows one exchange at a time (toggle).
            if (meta.exchange != g_symbol_picker.selected_exchange) continue;
            ex_total++;
            // filter_dead drops symbols with no live ticker. Applied to binancef only:
            // HL now HAS a ticker feed (ticker24h.hl.global), but a brief HL feed gap
            // shouldn't wipe every HL row from the picker, so keep HL rows unconditional.
            if (filter_dead && meta.exchange == "binancef" && !ticker_mgr.get(meta.exchange, meta.symbol)) continue;
            if (!matches_search(meta.base_asset, g_symbol_picker.search_buf) &&
                !matches_search(meta.symbol, g_symbol_picker.search_buf) &&
                std::none_of(meta.categories.begin(), meta.categories.end(),
                             [](const std::string& c) { return matches_search(c, g_symbol_picker.search_buf); }))
                continue;
            if (!matches_category(meta, g_symbol_picker.selected_category, cats))
                continue;
            visible.push_back(&meta);
        }
        g_symbol_picker.exchange_total = ex_total;

        bool sort_asc = g_symbol_picker.sort_ascending;
        switch (g_symbol_picker.sort_mode) {
        case SymbolPickerState::SortMode::Symbol:
            std::sort(visible.begin(), visible.end(), [sort_asc](auto* a, auto* b) {
                return sort_asc ? a->base_asset < b->base_asset : a->base_asset > b->base_asset;
            });
            break;
        case SymbolPickerState::SortMode::Score:
            std::sort(visible.begin(), visible.end(), [&, sort_asc](auto* a, auto* b) {
                double sa = 0, sb = 0;
                if (auto* s = scanner_mgr.get(a->exchange, a->symbol)) sa = s->edgedepth_score;
                if (auto* s = scanner_mgr.get(b->exchange, b->symbol)) sb = s->edgedepth_score;
                return sort_asc ? sa < sb : sa > sb;
            });
            break;
        case SymbolPickerState::SortMode::VPIN:
            std::sort(visible.begin(), visible.end(), [&, sort_asc](auto* a, auto* b) {
                double va = 0, vb = 0;
                if (auto* s = scanner_mgr.get(a->exchange, a->symbol)) va = s->vpin;
                if (auto* s = scanner_mgr.get(b->exchange, b->symbol)) vb = s->vpin;
                return sort_asc ? va < vb : va > vb;
            });
            break;
        case SymbolPickerState::SortMode::Price:
            std::sort(visible.begin(), visible.end(), [&, sort_asc](auto* a, auto* b) {
                double pa = 0, pb = 0;
                if (auto* t = ticker_mgr.get(a->exchange, a->symbol)) pa = t->last_price;
                if (auto* t = ticker_mgr.get(b->exchange, b->symbol)) pb = t->last_price;
                return sort_asc ? pa < pb : pa > pb;
            });
            break;
        case SymbolPickerState::SortMode::Change24h:
            std::sort(visible.begin(), visible.end(), [&, sort_asc](auto* a, auto* b) {
                double ca = -9999, cb = -9999;
                if (auto* t = ticker_mgr.get(a->exchange, a->symbol)) ca = t->change_pct_24h;
                if (auto* t = ticker_mgr.get(b->exchange, b->symbol)) cb = t->change_pct_24h;
                return sort_asc ? ca < cb : ca > cb;
            });
            break;
        case SymbolPickerState::SortMode::Volume:
            std::sort(visible.begin(), visible.end(), [&, sort_asc](auto* a, auto* b) {
                double va = 0, vb = 0;
                if (auto* t = ticker_mgr.get(a->exchange, a->symbol)) va = t->volume_quote;
                if (auto* t = ticker_mgr.get(b->exchange, b->symbol)) vb = t->volume_quote;
                return sort_asc ? va < vb : va > vb;
            });
            break;
        }

        // Save state for next-frame comparison
        strncpy(g_symbol_picker.prev_search, g_symbol_picker.search_buf, sizeof(g_symbol_picker.prev_search));
        g_symbol_picker.prev_category = g_symbol_picker.selected_category;
        g_symbol_picker.prev_exchange = g_symbol_picker.selected_exchange;
        g_symbol_picker.prev_sort_mode = g_symbol_picker.sort_mode;
        g_symbol_picker.prev_sort_asc = g_symbol_picker.sort_ascending;
        g_symbol_picker.prev_ticker_update_ms = ticker_mgr.last_update_ms();
        g_symbol_picker.prev_scanner_update_ms = scanner_mgr.last_update_ms();
        g_symbol_picker.last_data_rebuild_ms = now_ms_pick;
        g_symbol_picker.cache_dirty = false;
    }

    const auto& visible = g_symbol_picker.cached_visible;
    // Score only when the scanner has scored a row in this list (an all-dash column is noise).
    bool any_score = false;
    for (const auto* m : visible)
        if (const auto* sc = scanner_mgr.get(m->exchange, m->symbol); sc && sc->edgedepth_score > 0.0) { any_score = true; break; }

    // ── column header: quiet, sortable, sentence case ─────────────────────
    const float hdr_h = 34.0f * K;
    const float hdr_y = cat_y + cat_h;
    const float col_star_x  = w0.x + PADX + 6.0f * K;
    const float col_logo_x  = w0.x + PADX + 26.0f * K;
    const float col_sym_x   = col_logo_x + 26.0f * K + 12.0f * K;
    const float col_type_r  = w0.x + W - PADX - 6.0f * K;
    const float col_score_r = any_score ? w0.x + W * 0.78f : col_type_r;
    const float col_vol_r   = any_score ? w0.x + W * 0.68f : w0.x + W * 0.74f;
    const float col_chg_r   = any_score ? w0.x + W * 0.56f : w0.x + W * 0.60f;
    const float col_last_r  = any_score ? w0.x + W * 0.44f : w0.x + W * 0.47f;
    {
        dl->AddLine(ImVec2(w0.x + PADX, hdr_y + hdr_h - 1.0f), ImVec2(w0.x + W - PADX, hdr_y + hdr_h - 1.0f), u32(Tokens::BD1));
        const float hpx = 12.5f * K;
        auto& smode = g_symbol_picker.sort_mode;
        auto& asc = g_symbol_picker.sort_ascending;
        auto hdr_cell = [&](const char* label, float anchor_x, bool right_align, SymbolPickerState::SortMode mode) {
            const bool active = (smode == mode);
            const float lw = text_w(ui_f, hpx, label), chev = active ? 13.0f * K : 0.0f;
            const float tx2 = right_align ? anchor_x - lw - chev : anchor_x;
            const float ty = hdr_y + (hdr_h - hpx * 1.17f) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(tx2 - 6.0f * K, hdr_y));
            ImGui::PushID(label);
            const bool clk = ImGui::InvisibleButton("##hdr", ImVec2(lw + chev + 12.0f * K, hdr_h));
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            if (clk) {
                if (active) asc = !asc;
                else { smode = mode; asc = (mode == SymbolPickerState::SortMode::Symbol); }
            }
            put(dl, ui_f, hpx, ImVec2(tx2, ty), active ? Tokens::TX1 : hov ? Tokens::TX2 : Tokens::TX3, label);
            if (active)
                drawing::draw_ui_icon(dl, asc ? drawing::UiIcon::ChevronUp : drawing::UiIcon::ChevronDown,
                                      ImVec2(tx2 + lw + 7.5f * K, hdr_y + hdr_h * 0.5f + 0.5f), 3.5f * K, u32(Tokens::TX1), 1.3f);
        };
        hdr_cell("Market",     col_sym_x,  false, SymbolPickerState::SortMode::Symbol);
        hdr_cell("Price",      col_last_r, true,  SymbolPickerState::SortMode::Price);
        hdr_cell("24h",        col_chg_r,  true,  SymbolPickerState::SortMode::Change24h);
        hdr_cell("Volume",     col_vol_r,  true,  SymbolPickerState::SortMode::Volume);
        if (any_score) hdr_cell("Score", col_score_r, true, SymbolPickerState::SortMode::Score);
        const float tw2 = text_w(ui_f, hpx, "Type");
        put(dl, ui_f, hpx, ImVec2(col_type_r - tw2, hdr_y + (hdr_h - hpx * 1.17f) * 0.5f), Tokens::TX3, "Type");
    }
    ImGui::SetCursorScreenPos(ImVec2(w0.x, hdr_y + hdr_h));

    // ── keyboard navigation (arrows move highlight, Enter opens) ─────
    const int vis_n = static_cast<int>(visible.size());
    if (g_symbol_picker.keyboard_sel >= vis_n) g_symbol_picker.keyboard_sel = vis_n - 1;
    if (vis_n > 0) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
            g_symbol_picker.keyboard_sel = (g_symbol_picker.keyboard_sel < 0)
                ? 0 : std::min(g_symbol_picker.keyboard_sel + 1, vis_n - 1);
            g_symbol_picker.scroll_to_sel = true;
        } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
            g_symbol_picker.keyboard_sel = (g_symbol_picker.keyboard_sel <= 0)
                ? 0 : g_symbol_picker.keyboard_sel - 1;
            g_symbol_picker.scroll_to_sel = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && g_symbol_picker.keyboard_sel >= 0) {
            open_symbol(*visible[g_symbol_picker.keyboard_sel]);
        }
    }

    // ── rows (clipped): star, logo, market, price, change pill, volume, score, type chip ─
    const float row_h = 46.0f * K, foot_h = 48.0f * K;
    ImGui::BeginChild("SymbolList", ImVec2(0, -foot_h), false);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImDrawList* rdl = ImGui::GetWindowDrawList();

    if (g_symbol_picker.scroll_to_sel && g_symbol_picker.keyboard_sel >= 0) {
        const float top = g_symbol_picker.keyboard_sel * row_h;
        const float bot = top + row_h;
        const float sy = ImGui::GetScrollY();
        const float ch = ImGui::GetWindowHeight();
        if (top < sy) ImGui::SetScrollY(top);
        else if (bot > sy + ch) ImGui::SetScrollY(bot - ch);
        g_symbol_picker.scroll_to_sel = false;
    }

    const float sym_px = 15.5f * K, num_px = 15.0f * K, small_px = 13.5f * K;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()), row_h);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto* meta = visible[i];
            ImGui::PushID(i);
            const auto* ticker = ticker_mgr.get(meta->exchange, meta->symbol);
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            const bool ksel = (i == g_symbol_picker.keyboard_sel);
            const bool row_clk = ImGui::InvisibleButton("##row", ImVec2(W, row_h));
            const bool row_hov = ImGui::IsItemHovered();
            const ImVec2 ra(rp.x + PADX - 8.0f * K, rp.y + 1.0f), rb(rp.x + W - PADX + 8.0f * K, rp.y + row_h - 1.0f);
            if (ksel || row_hov) rdl->AddRectFilled(ra, rb, u32(ksel ? Tokens::CONTROL_HOVER : Tokens::CONTROL), Radius::R2);
            const float cy = rp.y + row_h * 0.5f;

            // Favourite star, with its own click zone at the left of the row.
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const bool on_star = mp.x >= col_star_x - 8.0f * K && mp.x <= col_star_x + 14.0f * K;
            const bool fav = g_symbol_picker.is_favorite(meta->symbol);
            {
                ImVec2 pt[10];
                const float r = 6.0f * K, ri = r * 0.42f;
                const ImVec2 c(col_star_x + 4.0f * K, cy);
                for (int k = 0; k < 10; ++k) {
                    const float ang = (-90.0f + k * 36.0f) * 3.14159265f / 180.0f;
                    const float rr = (k & 1) ? ri : r;
                    pt[k] = ImVec2(c.x + std::cos(ang) * rr, c.y + std::sin(ang) * rr);
                }
                if (fav) for (int k = 0; k < 10; ++k) rdl->AddTriangleFilled(c, pt[k], pt[(k + 1) % 10], u32(Tokens::WARN));
                else rdl->AddPolyline(pt, 10, u32(row_hov && on_star ? Tokens::TX1 : row_hov ? Tokens::TX3 : Tokens::TX4), ImDrawFlags_Closed, 1.1f);
            }
            if (row_clk) {
                if (on_star) { g_symbol_picker.toggle_favorite(meta->symbol); persist_favorites(); }
                else { open_symbol(*meta); }
            }

            // Logo and market: base bright, /quote dim. Non-ASCII bases fall back to the raw symbol.
            LogoManager::instance().draw_coin(rdl, meta->base_asset, ImVec2(col_logo_x, cy - 13.0f * K), 26.0f * K);
            {
                std::string display = meta->display_name();
                bool has_nonascii = false;
                for (char c : meta->base_asset)
                    if (static_cast<unsigned char>(c) > 127) { has_nonascii = true; break; }
                if (has_nonascii) {
                    std::string sym_upper = meta->symbol;
                    std::transform(sym_upper.begin(), sym_upper.end(), sym_upper.begin(), [](unsigned char c) { return std::toupper(c); });
                    if (sym_upper.size() > 4 && sym_upper.substr(sym_upper.size() - 4) == "USDT") sym_upper = sym_upper.substr(0, sym_upper.size() - 4);
                    display = sym_upper + "/USDT";
                }
                const float ty = cy - sym_px * 0.58f;
                const auto slash = display.find('/');
                if (slash != std::string::npos) {
                    put(rdl, semi_f, sym_px, ImVec2(col_sym_x, ty), Tokens::TX1, display.c_str(), display.c_str() + slash);
                    const float bw = semi_f->CalcTextSizeA(sym_px, FLT_MAX, 0, display.c_str(), display.c_str() + slash).x;
                    put(rdl, ui_f, sym_px, ImVec2(col_sym_x + bw + 1.0f, ty), Tokens::TX3, display.c_str() + slash);
                } else put(rdl, semi_f, sym_px, ImVec2(col_sym_x, ty), Tokens::TX1, display.c_str());
            }
            auto num_cell = [&](float right_x, const char* s, const ImVec4& col, float px) {
                put(rdl, mono_f, px, ImVec2(right_x - text_w(mono_f, px, s), cy - px * 0.58f), col, s);
            };
            if (ticker && ticker->last_price > 0.0) {
                char pbuf[32];
                meta->fmt.format_price(pbuf, sizeof(pbuf), ticker->last_price);
                num_cell(col_last_r, pbuf, Tokens::TX1, num_px);
            } else num_cell(col_last_r, "-", Tokens::TX4, num_px);
            // 24h: a coloured pill, as the watchlist.
            if (ticker) Theme::change_pill(rdl, col_chg_r, cy, ticker->change_pct_24h, small_px);
            else num_cell(col_chg_r, "-", Tokens::TX4, num_px);
            if (ticker && ticker->volume_quote > 0.0) {
                char vbuf[32];
                format_volume(vbuf, sizeof(vbuf), ticker->volume_quote);
                num_cell(col_vol_r, vbuf, Tokens::TX2, 14.5f * K);
            } else num_cell(col_vol_r, "-", Tokens::TX4, 14.5f * K);
            if (any_score) {
                const auto* scan = scanner_mgr.get(meta->exchange, meta->symbol);
                if (scan && scan->edgedepth_score > 0.0) {
                    char sbuf[16];
                    snprintf(sbuf, sizeof(sbuf), "%.0f", scan->edgedepth_score);
                    num_cell(col_score_r, sbuf, scan->edgedepth_score >= 70.0 ? Tokens::BRAND_TX : scan->edgedepth_score >= 40.0 ? Tokens::TX1 : Tokens::TX2, num_px);
                } else num_cell(col_score_r, "-", Tokens::TX4, num_px);
            }
            // Type: a quiet chip (DATED for a dated contract, else the first category or PERP).
            {
                const char* tp = !meta->perpetual ? "Dated" : meta->categories.empty() ? "Perp" : meta->categories[0].c_str();
                const float tpx = 12.5f * K, tw2 = text_w(ui_f, tpx, tp), ch = 22.0f * K;
                const ImVec2 ca(col_type_r - tw2 - 16.0f * K, cy - ch * 0.5f);
                rdl->AddRectFilled(ca, ImVec2(col_type_r, cy + ch * 0.5f), u32(row_hov || ksel ? Tokens::CONTROL_ACTIVE : Tokens::CONTROL), ch * 0.5f);
                put(rdl, ui_f, tpx, ImVec2(ca.x + 8.0f * K, cy - tpx * 0.58f), Tokens::TX2, tp);
                if (ksel) keycap(rdl, ImVec2(ca.x - 12.0f * K - (text_w(ui_f, 12.0f * K, "Enter") + 12.0f * K), cy - 10.0f * K), "Enter");
            }
            ImGui::PopID();
        }
    }
    clipper.End();
    if (visible.empty()) {
        const char* none = "No markets match";
        const float px = 15.0f * K;
        put(rdl, ui_f, px, ImVec2(w0.x + (W - text_w(ui_f, px, none)) * 0.5f, ImGui::GetCursorScreenPos().y + 40.0f * K), Tokens::TX3, none);
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();

    // ── footer: count on the left, keycap hints on the right ──────────────
    {
        const float fy = ImGui::GetCursorScreenPos().y;
        dl->AddLine(ImVec2(w0.x, fy), ImVec2(w0.x + W, fy), u32(Tokens::BD1));
        const float px = 13.5f * K, ty = fy + (foot_h - px * 1.17f) * 0.5f;
        const char* venue = g_symbol_picker.selected_exchange == "hl" ? "Hyperliquid" : g_symbol_picker.selected_exchange == "bybit" ? "Bybit" : "Binance";
        char summary[96];
        snprintf(summary, sizeof(summary), "%zu of %d markets on %s", visible.size(), g_symbol_picker.exchange_total, venue);
        put(dl, ui_f, px, ImVec2(w0.x + PADX, ty), Tokens::TX2, summary);
        struct Hint { const char* key; const char* label; };
        const Hint hints[] = {{"", "Navigate"}, {"Enter", "Open"}, {"Esc", "Close"}};
        float total = 0.0f;
        for (const auto& h : hints) total += keycap_w(h.key) + 6.0f * K + text_w(ui_f, px, h.label) + 18.0f * K;
        float hx = w0.x + W - PADX - total + 18.0f * K;
        if (hx > w0.x + PADX + text_w(ui_f, px, summary) + 24.0f * K) {
            for (const auto& h : hints) {
                hx += keycap(dl, ImVec2(hx, fy + (foot_h - 20.0f * K) * 0.5f), h.key) + 6.0f * K;
                put(dl, ui_f, px, ImVec2(hx, ty), Tokens::TX3, h.label);
                hx += text_w(ui_f, px, h.label) + 18.0f * K;
            }
        }
        ImGui::Dummy(ImVec2(W, foot_h - 1.0f));
    }
    ImGui::EndPopup();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

// The identity a NEW chart of `pair` takes: 1 (the primary, "###chart_<ex>_<sym>")
// when no chart draws the pair, else one past the highest instance already
// open. Max, not count: closing chart 2 while chart 3 stays open must not hand
// the next chart 3's window. Widgets closed this frame still count, so a
// close-and-reopen within one frame cannot collide either.
int next_chart_instance(const std::vector<std::unique_ptr<Widget>>& widgets,
                        const Terminal::Pair& pair)
{
    int next = 1;
    for (const auto& widget : widgets) {
        if (!widget || widget->type() != WidgetType::Chart) continue;
        const auto* chart = static_cast<const ChartWidget*>(widget.get());
        if (chart->pair().exchange != pair.exchange || chart->pair().symbol != pair.symbol) continue;
        next = std::max(next, chart->chart_instance() + 1);
    }
    return next;
}

// Drain one add-widget request per frame (chart-toolbar "+ widget" in the
// embedded chromes). Mirrors the picker's open_symbol switch but uses the
// already-resolved pair/fmt/tick the chart handed us, so it needs no registry
// lookup and works with no WebSocket (a /demo pack has none).
void sync_market_view(const Terminal::Pair& pair, bool data, int hours) {
#ifdef __EMSCRIPTEN__
    // Only a focus/range transition touches history, never every render frame.
    static std::string venue, symbol;
    static int previous = -1;
    const int next = data ? hours : 0;
    if (!market_view_ready || market_view_navigation_pending) return;
    if (previous == next && venue == pair.exchange && symbol == pair.symbol) return;
    previous=next; venue=pair.exchange; symbol=pair.symbol;
    const auto route=parse_route(url_get_current_path(),url_get_current_search());
    if (route.exchange != venue || route.symbol != symbol) return;
    EM_ASM({
        const u=new URL(location.href);
        if ($0) {u.searchParams.set('view','data');u.searchParams.set('range',$0===24?'24h':'4h');}
        else {u.searchParams.delete('view');u.searchParams.delete('range');}
        if (u.href!==location.href) history.pushState({},"",u);
    },next);
#else
    (void)pair; (void)data; (void)hours;
#endif
}

void request_market_data(const Terminal::Pair& pair, int hours) {
    g_widget_add_request = {SymbolPickerState::PendingWidget::Data,pair,
        SymbolRegistry::instance().get_formatter(pair.exchange,pair.symbol),0,true,hours};
}

void resolve_widget_add_request(
    std::vector<std::unique_ptr<Widget>>& widgets, const AppContext& ctx)
{
    auto& req = g_widget_add_request;
    if (!req.pending) return;
    req.pending = false;
    using PW = SymbolPickerState::PendingWidget;
    // Charts are exempt from the reopen/focus rule below: a second chart of a
    // pair that already has one is a SECONDARY chart with its own identity and
    // its own CandleManager (see ChartWidget), so a real-time depth view and a
    // candle chart of one market can sit side by side. In a replay the
    // secondary's manager lives in the replay context (seeks reach it), so
    // the chart belongs to the replay and closes with it.
    if (req.type == PW::Charts) {
        auto chart = std::make_unique<ChartWidget>(
            req.pair, ctx, req.tick_size, next_chart_instance(widgets, req.pair));
        chart->is_replay_widget = ctx.replay_mgr().is_active();
        chart->open_centered = true;
        widgets.push_back(std::move(chart));
        return;
    }
    // Market widgets have stable window IDs. Reopen/focus that window instead
    // of constructing a second widget with the same ImGui identity.
    for (auto& widget : widgets) {
        if (widget->suspended_for_replay) continue;
        const Terminal::Pair* pair=nullptr;
        if(req.type==PW::Charts && widget->type()==WidgetType::Chart) pair=&static_cast<ChartWidget*>(widget.get())->pair();
        if(req.type==PW::DOM && widget->type()==WidgetType::DOM) pair=&static_cast<DOMWidget*>(widget.get())->pair();
        if(req.type==PW::Orderbook && widget->type()==WidgetType::Orderbook) pair=&static_cast<OrderbookWidget*>(widget.get())->pair();
        if(req.type==PW::Trades && widget->type()==WidgetType::Trades) pair=&static_cast<TradesWidget*>(widget.get())->pair();
        if(req.type==PW::Stats && widget->type()==WidgetType::Stats) pair=&static_cast<StatsWidget*>(widget.get())->pair();
        if(req.type==PW::Data && widget->type()==WidgetType::Data) pair=&static_cast<DataWidget*>(widget.get())->pair();
        if(req.type==PW::Debug && widget->type()==WidgetType::DebugLog) pair=&static_cast<DebugWidget*>(widget.get())->pair();
        if(!pair || pair->exchange!=req.pair.exchange || pair->symbol!=req.pair.symbol)continue;
        if (req.type==PW::Data) {
            static_cast<DataWidget*>(widget.get())->set_hours(req.data_hours);
            // A restored tab has no ImGui window yet on the first frame.
            // Focus on render so an explicit ?view=data beats chart restore.
            widget->open_centered = true;
        }
        widget->is_open=true;
        // Focusing a normal panel in candles must not silently pin it.
        // Explicitly reopening an automatically hidden panel is an override.
        if (widget->hidden_by_chart) widget->mode_companion = false;
        widget->hidden_by_chart=false;
        ImGui::SetWindowFocus(widget->title());
        return;
    }
    const size_t before = widgets.size();
    switch (req.type) {
        case PW::Orderbook:
            widgets.push_back(std::make_unique<OrderbookWidget>(req.pair, ctx, req.fmt, 25)); break;
        case PW::DOM:
            widgets.push_back(std::make_unique<DOMWidget>(req.pair, ctx, req.tick_size, 25)); break;
        case PW::Trades:
            widgets.push_back(std::make_unique<TradesWidget>(req.pair, ctx, req.fmt)); break;
        case PW::Charts:
            widgets.push_back(std::make_unique<ChartWidget>(req.pair, ctx, req.tick_size)); break;
        case PW::Data: {
            auto data=std::make_unique<DataWidget>(req.pair,ctx);
            data->set_hours(req.data_hours);
            for (const auto& widget : widgets) {
                if (!widget->is_open || widget->type()!=WidgetType::Chart) continue;
                const auto& chart=*static_cast<const ChartWidget*>(widget.get());
                if (chart.pair().exchange==req.pair.exchange && chart.pair().symbol==req.pair.symbol) {
                    data->initial_dock_target=chart.title();
                    break;
                }
            }
            widgets.push_back(std::move(data)); break;
        }
        case PW::Stats:
            widgets.push_back(std::make_unique<StatsWidget>(req.pair, ctx, req.fmt)); break;
        case PW::Debug:
            widgets.push_back(std::make_unique<DebugWidget>(req.pair, ctx)); break;
        case PW::PaperTrading:
            widgets.push_back(std::make_unique<PositionsPanel>(ctx)); break;
        case PW::Watchlist: {
            auto existing = std::find_if(widgets.begin(), widgets.end(), [](const auto& w) {
                return w && w->type() == WidgetType::Watchlist;
            });
            if (existing != widgets.end()) {
                (*existing)->is_open = true;
                ImGui::SetWindowFocus((*existing)->title());
            } else widgets.push_back(std::make_unique<WatchlistWidget>(req.pair, ctx));
            break;
        }
        case PW::ReplayLibrary: {
            const bool already_open = std::any_of(
                widgets.begin(), widgets.end(), [](const auto& widget) {
                    return widget && widget->type() == WidgetType::ReplayLibrary;
                });
            if (!already_open) {
                widgets.push_back(std::make_unique<ReplayLibraryWidget>(ctx));
            } else {
                ImGui::SetWindowFocus("Replay Library###replay_library");
            }
            break;
        }
        default: break;
    }
    if (widgets.size() > before) {
        widgets.back()->open_centered = true;
        widgets.back()->is_replay_widget = ctx.replay_mgr().is_active();
        if (req.type == PW::DOM || req.type == PW::Trades) {
            // Creating a panel while already in RT explicitly asks for both
            // views. Creating it in candles keeps the constructor's auto-hide.
            for (const auto& widget : widgets) {
                if (!widget->is_open || widget->suspended_for_replay || widget->type() != WidgetType::Chart) continue;
                const auto& chart = *static_cast<const ChartWidget*>(widget.get());
                if (chart.pair().exchange == req.pair.exchange && chart.pair().symbol == req.pair.symbol && chart.rt_mode()) {
                    widgets.back()->mode_companion = false;
                    break;
                }
            }
        }
    }
}

} // namespace Menu
