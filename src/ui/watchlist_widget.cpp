// ═══════════════════════════════════════════════════════════════════════════════
// watchlist_widget.cpp - watchlist rail (design v3, 2026-10-11; see the header).
// Colours resolve to Theme::Tokens, numerals to PriceFormatter, rows are
// virtualised with ImGuiListClipper; no per-row heap allocations in the hot path.
// ═══════════════════════════════════════════════════════════════════════════════
#include "ui/watchlist_widget.h"
#include "rendering/theme.h"
#include "core/ticker_manager.h"
#include "core/logo_manager.h"
#include "ui/drawing/drawing_icons.h"
#include "core/url_router.h"
#include "imgui.h"
#include "imgui_internal.h"          // ImGuiDockNodeFlags_NoTabBar (window class)
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace {

struct WatchlistVenue {
    const char* id;
    const char* label;
    const char* compact_label;
};

constexpr WatchlistVenue kWatchlistVenues[] = {
    {"binancef", "Binance",     "Binance"},
    {"hl",       "Hyperliquid", "Hyperliquid"},
    {"bybit",    "Bybit",       "Bybit"},
};

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// case-insensitive substring (ASCII symbols / categories only)
bool contains_ci(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return true;
    const size_t nlen = strlen(needle);
    if (nlen > hay.size()) return false;
    for (size_t i = 0; i + nlen <= hay.size(); ++i) {
        size_t j = 0;
        while (j < nlen &&
               std::tolower(static_cast<unsigned char>(hay[i + j])) ==
               std::tolower(static_cast<unsigned char>(needle[j]))) j++;
        if (j == nlen) return true;
    }
    return false;
}

// 5-point star, drawn (fonts carry no reliable star glyph). Filled = triangle
// fan from centre (a 5-point star is star-convex from its centroid).
void draw_star(ImDrawList* dl, ImVec2 ctr, float r, bool filled, ImU32 col) {
    ImVec2 p[10];
    const float ri = r * 0.42f;
    for (int i = 0; i < 10; ++i) {
        const float a = (-90.0f + i * 36.0f) * 3.14159265f / 180.0f;
        const float rr = (i & 1) ? ri : r;
        p[i] = ImVec2(ctr.x + std::cos(a) * rr, ctr.y + std::sin(a) * rr);
    }
    if (filled) {
        for (int i = 0; i < 10; ++i)
            dl->AddTriangleFilled(ctr, p[i], p[(i + 1) % 10], col);
    } else {
        dl->AddPolyline(p, 10, col, ImDrawFlags_Closed, 1.0f);
    }
}

// compact 24h quote-volume label: "$1.2B" / "$340M" / "$12M" / "$120K"
void fmt_volume(char* buf, size_t n, double v) {
    if (v >= 1e9)      snprintf(buf, n, "$%.2fB", v / 1e9);
    else if (v >= 1e6) snprintf(buf, n, "$%.0fM", v / 1e6);
    else if (v >= 1e3) snprintf(buf, n, "$%.0fK", v / 1e3);
    else               snprintf(buf, n, "$%.0f", v);
}

} // namespace

WatchlistWidget::WatchlistWidget(const Terminal::Pair& active_pair, const AppContext& ctx)
    : active_pair_(active_pair)
    , ctx_(ctx)
{}

void WatchlistWidget::update() {}

// ── Per-category pair counts (built once with the category list) ─────────────
void WatchlistWidget::build_category_counts() {
    auto& reg = SymbolRegistry::instance();
    category_counts_.assign(categories_.size(), 0);
    exchange_counts_.fill(0);
    all_count_ = 0;
    scoped_count_ = 0;
    for (const auto& [key, meta] : reg.all()) {
        if (!meta.is_active) continue;
        ++all_count_;
        for (size_t i = 0; i < exchange_counts_.size(); ++i) {
            if (meta.exchange == kWatchlistVenues[i].id) {
                ++exchange_counts_[i];
                break;
            }
        }
        if (!selected_exchange_.empty() && meta.exchange != selected_exchange_) continue;
        ++scoped_count_;
        for (const auto& c : meta.categories) {
            for (size_t i = 0; i < categories_.size(); ++i) {
                if (categories_[i] == c) { ++category_counts_[i]; break; }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Visible-list cache - filter (search + category) then sort.
// ═══════════════════════════════════════════════════════════════════════════════

void WatchlistWidget::rebuild_visible() {
    auto& reg = SymbolRegistry::instance();
    auto& tickers = TickerManager::instance();

    visible_.clear();
    const char* needle = search_buf_;
    const std::string* cat = (selected_category_ >= 0 &&
                              selected_category_ < static_cast<int>(categories_.size()))
                             ? &categories_[selected_category_] : nullptr;

    for (const auto& [key, meta] : reg.all()) {
        if (!meta.is_active) continue;
        if (!selected_exchange_.empty() && meta.exchange != selected_exchange_) continue;
        if (cat) {
            bool has = false;
            for (const auto& c : meta.categories) if (c == *cat) { has = true; break; }
            if (!has) continue;
        }
        if (needle[0] && !contains_ci(meta.symbol, needle) &&
            !contains_ci(meta.base_asset, needle)) continue;
        visible_.push_back(&meta);
    }

    auto& tm = tickers;
    const bool desc = sort_desc_;
    auto sort_by = [&](auto valfn) {
        std::sort(visible_.begin(), visible_.end(),
            [&](const SymbolMetadata* a, const SymbolMetadata* b) {
                const double va = valfn(a), vb = valfn(b);
                return desc ? (va > vb) : (va < vb);
            });
    };
    switch (sort_) {
        case Sort::Symbol:
            std::sort(visible_.begin(), visible_.end(),
                [desc](const SymbolMetadata* a, const SymbolMetadata* b) {
                    return desc ? (a->symbol > b->symbol) : (a->symbol < b->symbol);
                });
            break;
        case Sort::Change:
            sort_by([&tm](const SymbolMetadata* m){ const auto* t=tm.get(m->exchange, m->symbol); return t?t->change_pct_24h:-1e9; });
            break;
        case Sort::Volume:
            sort_by([&tm](const SymbolMetadata* m){ const auto* t=tm.get(m->exchange, m->symbol); return t?t->volume_quote:0.0; });
            break;
        case Sort::Price:
            sort_by([&tm](const SymbolMetadata* m){ const auto* t=tm.get(m->exchange, m->symbol); return t?t->last_price:0.0; });
            break;
    }

    strncpy(prev_search_, search_buf_, sizeof(prev_search_));
    prev_category_ = selected_category_;
    prev_exchange_ = selected_exchange_;
    prev_sort_ = sort_;
    prev_sort_desc_ = sort_desc_;
    prev_ticker_ms_ = tickers.last_update_ms();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Render - header stack (title · filter · category · sort) then the scroll list
// ═══════════════════════════════════════════════════════════════════════════════

void WatchlistWidget::render() {
    if (!is_open) return;

    // One clean panel: suppress the single-tab dock tab bar so our custom title
    // bar (live dot · WATCHLIST · count · close) reads as the panel header.
    ImGuiWindowClass wc;
    wc.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&wc);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    place_new_window();
    const bool visible = ImGui::Begin("Watchlist", &is_open,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    was_visible_last_frame_ = visible;
    if (!visible) { ImGui::End(); return; }

    auto& reg = SymbolRegistry::instance();
    if (!reg.is_loaded()) {
        ImGui::TextColored(Theme::Tokens::TX2, "  Loading symbols\xE2\x80\xA6");
        ImGui::End();
        return;
    }
    if (!categories_loaded_) {
        categories_ = reg.unique_categories();
        build_category_counts();
        categories_loaded_ = true;
    }

    render_title_bar();
    render_filter_bar();
    render_category_selector();
    render_sort_header();

    // Rebuild on any input change immediately; live ticker drift re-sorts at most
    // every 5s (re-sorting ~680 symbols is the costly part, not the filter).
    const int64_t now = now_ms();
    const bool inputs_changed =
        strcmp(prev_search_, search_buf_) != 0 ||
        prev_category_ != selected_category_ ||
        prev_exchange_ != selected_exchange_ ||
        prev_sort_ != sort_ || prev_sort_desc_ != sort_desc_;
    const bool ticker_drift =
        (sort_ != Sort::Symbol) && (now - last_resort_ms_ > 5000) &&
        (prev_ticker_ms_ != TickerManager::instance().last_update_ms());
    if (inputs_changed || ticker_drift || visible_.empty()) {
        rebuild_visible();
        last_resort_ms_ = now;
    }

    render_rows();
    ImGui::End();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Design v3 (James 2026-10-11: "watchlist redesigned / repolished"). Title with a
// count chip and round icon controls; a rounded search field; category and venue as
// soft dropdowns; a quiet sort header; two-line rows (logo with a venue badge, pair,
// volume; price over a coloured change pill), rounded hover and selection, the star
// shown on hover or when set. The 60 s sparkline is gone: it read as a trend but
// covered one minute of ticker prices. Sizes scale with the UI text size (WK).
// ═══════════════════════════════════════════════════════════════════════════════

namespace {
float WK = 1.0f;
// The row geometry, shared by the sort header and the rows so their columns agree.
struct RowGrid {
    float pad, star_x, logo_x, logo, sym_x, pill_w, pill_right, price_right, h;
    bool stacked, compact;
};
RowGrid row_grid(float w, bool compact) {
    RowGrid g{};
    g.compact = compact;
    g.stacked = !compact && w < 420 * WK;
    g.pad = 8 * WK;
    g.h = compact ? 38 * WK : 58 * WK;
    g.star_x = g.pad + 11 * WK;
    g.logo = compact ? 22 * WK : 30 * WK;
    g.logo_x = g.pad + 22 * WK;
    g.sym_x = g.logo_x + g.logo + 10 * WK;
    g.pill_w = Theme::change_pill_width((compact ? 13.0f : 13.5f) * WK);
    g.pill_right = w - g.pad - 10 * WK;
    g.price_right = g.stacked ? g.pill_right : g.pill_right - g.pill_w - 16 * WK;
    return g;
}
// A soft dropdown button: rounded fill, chevron at the right. Returns clicked; content is drawn by the caller.
bool dropdown_button(const char* id, ImVec2 p, ImVec2 size, bool open) {
    ImGui::SetCursorScreenPos(p);
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hot = ImGui::IsItemHovered() || open;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), Theme::u32(hot ? Theme::Tokens::CONTROL_HOVER : Theme::Tokens::CONTROL), Theme::Radius::R2);
    drawing::draw_ui_icon(dl, open ? drawing::UiIcon::ChevronUp : drawing::UiIcon::ChevronDown,
                          ImVec2(p.x + size.x - 14 * WK, p.y + size.y * 0.5f), 4.5f * WK, Theme::u32(Theme::Tokens::TX2), 1.4f);
    return clicked;
}
}  // namespace

void WatchlistWidget::render_title_bar() {
    WK = ImGui::GetFontSize() / 18.4f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    // The header reads as a dock tab, one family with every other panel's (theme.cpp tab colours): the tab strip, a
    // tab on the panel's surface with a leading glyph, the name and the count, the accent line while the panel has
    // focus; its controls sit at the strip's right. Same height and type as the tabs beside it, so the panel tops line
    // up (2026-10-11; it was a 46 px semibold title row).
    const float h = ImGui::GetFrameHeight();
    const float pad = ImGui::GetStyle().FramePadding.x;
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), Theme::u32(Theme::Tokens::BASE));
    char cnt[24]; snprintf(cnt, sizeof(cnt), "%d", all_count_);
    const char* label = "Watchlist";
    const float glyph = std::min(16.0f, h - 4.0f);
    const float lw = ImGui::CalcTextSize(label).x, cw = ImGui::CalcTextSize(cnt).x;
    const float tab_w = std::round(pad + glyph + 8.0f + lw + 8.0f + cw + pad + 2.0f);
    dl->AddRectFilled(p0, ImVec2(p0.x + tab_w, p0.y + h), Theme::u32(Theme::Tokens::PANEL));
    if (focused) dl->AddRectFilled(p0, ImVec2(p0.x + tab_w, p0.y + 2.0f), Theme::u32(Theme::Tokens::BRAND));
    const float cy = p0.y + h * 0.5f, ty = std::round(cy - ImGui::GetFontSize() * 0.5f);
    drawing::draw_ui_icon(dl, drawing::UiIcon::Star, ImVec2(p0.x + pad + glyph * 0.5f, cy), glyph * 0.36f,
                          Theme::u32(Theme::Tokens::TX2), 1.3f);
    float x = p0.x + pad + glyph + 8.0f;
    dl->AddText(ImVec2(x, ty), Theme::u32(Theme::Tokens::TX1), label);
    x += lw + 8.0f;
    dl->AddText(ImVec2(x, ty), Theme::u32(Theme::Tokens::TX3), cnt);
    const float ib = std::round(h - 4.0f);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + w - 4.0f - ib, p0.y + (h - ib) * 0.5f));
    if (Theme::icon_button("##wl_close", drawing::UiIcon::Close, "Close watchlist", ib)) is_open = false;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + w - 4.0f - ib * 2 - 2.0f, p0.y + (h - ib) * 0.5f));
    if (Theme::icon_button("##wl_density", drawing::UiIcon::Rows, compact_ ? "Comfortable rows" : "Compact rows", ib, compact_))
        compact_ = !compact_;
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(w, h + 8.0f * WK));
}

// ── Search: a rounded field with a magnifier in its left padding ────────────

void WatchlistWidget::render_filter_bar() {
    const float pad = 12 * WK;
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    const float ww = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorScreenPos(ImVec2(bp.x + pad, bp.y));
    Theme::push_font(Theme::Fonts::ui(), 15 * WK);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(32 * WK, 9 * WK));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::Radius::R2);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, Theme::Tokens::TX3);
    ImGui::SetNextItemWidth(ww - pad * 2);
    ImGui::InputTextWithHint("##wl_search", "Search pairs", search_buf_, sizeof(search_buf_));
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    drawing::draw_ui_icon(ImGui::GetWindowDrawList(), drawing::UiIcon::Search, ImVec2(a.x + 16 * WK, (a.y + b.y) * 0.5f),
                          6 * WK, Theme::u32(ImGui::IsItemActive() ? Theme::Tokens::TX1 : Theme::Tokens::TX3), 1.4f);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(bp.x, b.y));
    ImGui::Dummy(ImVec2(ww, 8 * WK));
}

// ── Category and venue: soft dropdowns with popovers ────────────────────────

void WatchlistWidget::render_category_selector() {
    const float pad = 12 * WK;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    const float ww = ImGui::GetContentRegionAvail().x;
    const float h = 36 * WK, gap = 8 * WK;
    const float w = ww - pad * 2;
    const float venue_w = std::clamp(w * 0.44f, 128 * WK, 180 * WK);
    const float category_w = w - gap - venue_w;
    const ImVec2 p0(bp.x + pad, bp.y);
    const ImVec2 venue_p(p0.x + category_w + gap, p0.y);

    // Category: grid glyph, name, count.
    if (dropdown_button("##wl_cat", p0, ImVec2(category_w, h), ImGui::IsPopupOpen("##wl_cat_pop"))) ImGui::OpenPopup("##wl_cat_pop");
    const bool category_valid = selected_category_ >= 0 && selected_category_ < static_cast<int>(categories_.size());
    const char* name = category_valid ? categories_[selected_category_].c_str() : "All pairs";
    const int ct = category_valid && selected_category_ < static_cast<int>(category_counts_.size())
                   ? category_counts_[selected_category_] : scoped_count_;
    drawing::draw_ui_icon(dl, drawing::UiIcon::Grid, ImVec2(p0.x + 16 * WK, p0.y + h * 0.5f), 5.5f * WK, Theme::u32(Theme::Tokens::TX2), 1.4f);
    char cbuf[16]; snprintf(cbuf, sizeof(cbuf), "%d", ct);
    Theme::push_font(Theme::Fonts::ui(), 13 * WK);
    const float count_w = ImGui::CalcTextSize(cbuf).x;
    ImGui::PopFont();
    Theme::push_font(Theme::Fonts::ui_semibold(), 15 * WK);
    // The count is also in the header/popover. Preserve the category label first.
    const bool show_count = category_w >= 30 * WK + ImGui::CalcTextSize(name).x + 6 * WK + count_w + 28 * WK;
    const float label_end = p0.x + category_w - 28 * WK - (show_count ? count_w + 6 * WK : 0);
    if (show_count) {
        Theme::push_font(Theme::Fonts::ui(), 13 * WK);
        dl->AddText(ImVec2(p0.x + category_w - 28 * WK - count_w, p0.y + (h - ImGui::GetFontSize()) * 0.5f),
                    Theme::u32(Theme::Tokens::TX3), cbuf);
        ImGui::PopFont();
    }
    dl->PushClipRect(ImVec2(p0.x + 30 * WK, p0.y), ImVec2(std::max(p0.x + 31 * WK, label_end), p0.y + h), true);
    dl->AddText(ImVec2(p0.x + 30 * WK, p0.y + (h - ImGui::GetFontSize()) * 0.5f), Theme::u32(Theme::Tokens::TX1), name);
    dl->PopClipRect();
    ImGui::PopFont();

    // Venue: its logo (or a globe for all venues) and name.
    if (dropdown_button("##wl_venue", venue_p, ImVec2(venue_w, h), ImGui::IsPopupOpen("##wl_venue_pop"))) ImGui::OpenPopup("##wl_venue_pop");
    const WatchlistVenue* selected_venue = nullptr;
    for (const auto& venue : kWatchlistVenues)
        if (selected_exchange_ == venue.id) { selected_venue = &venue; break; }
    const float logo = 16 * WK;
    if (selected_venue) LogoManager::instance().draw_exchange(dl, selected_venue->id, ImVec2(venue_p.x + 10 * WK, venue_p.y + (h - logo) * 0.5f), logo);
    else drawing::draw_ui_icon(dl, drawing::UiIcon::Globe, ImVec2(venue_p.x + 10 * WK + logo * 0.5f, venue_p.y + h * 0.5f), 6 * WK, Theme::u32(Theme::Tokens::TX2), 1.3f);
    Theme::push_font(Theme::Fonts::ui(), 14.5f * WK);
    const float vtx = venue_p.x + 10 * WK + logo + 8 * WK;
    dl->PushClipRect(ImVec2(vtx, venue_p.y), ImVec2(venue_p.x + venue_w - 24 * WK, venue_p.y + h), true);
    dl->AddText(ImVec2(vtx, venue_p.y + (h - ImGui::GetFontSize()) * 0.5f), Theme::u32(Theme::Tokens::TX1),
                selected_venue ? selected_venue->compact_label : "All venues");
    dl->PopClipRect();
    ImGui::PopFont();

    // Popovers share the floating chrome: elevated, rounded, hairlined.
    auto begin_pop = [&](const char* id, ImVec2 pos, float width, ImVec2 padding) {
        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Tokens::ELEV);
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Tokens::BD2);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Theme::Radius::R3);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        const bool open = Theme::begin_popup(id);
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        return open;
    };
    // One selectable row: rounded hover, filled when active; label, optional logo, count.
    auto option = [&](const char* id_label, const char* label, int count, bool active, float rw, const char* venue_id) {
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        const float rh = 32 * WK;
        const bool clicked = ImGui::InvisibleButton(id_label, ImVec2(rw, rh));
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        if (active || hovered)
            pdl->AddRectFilled(rp, ImVec2(rp.x + rw, rp.y + rh), Theme::u32(active ? Theme::Tokens::CONTROL_ACTIVE : Theme::Tokens::CONTROL), Theme::Radius::R2);
        float tx = rp.x + 10 * WK;
        if (venue_id && venue_id[0]) {
            const float lg = 15 * WK;
            LogoManager::instance().draw_exchange(pdl, venue_id, ImVec2(tx, rp.y + (rh - lg) * 0.5f), lg);
            tx += lg + 8 * WK;
        }
        Theme::push_font(Theme::Fonts::ui(), 14 * WK);
        char nb[16]; snprintf(nb, sizeof(nb), "%d", count);
        const float nbw = ImGui::CalcTextSize(nb).x;
        pdl->PushClipRect(ImVec2(tx, rp.y), ImVec2(rp.x + rw - nbw - 16 * WK, rp.y + rh), true);
        pdl->AddText(ImVec2(tx, rp.y + (rh - ImGui::GetFontSize()) * 0.5f), Theme::u32(active || hovered ? Theme::Tokens::TX1 : Theme::Tokens::TX2), label);
        pdl->PopClipRect();
        pdl->AddText(ImVec2(rp.x + rw - 10 * WK - nbw, rp.y + (rh - ImGui::GetFontSize()) * 0.5f), Theme::u32(Theme::Tokens::TX3), nb);
        ImGui::PopFont();
        return clicked;
    };

    // Categories: a search field over a two-column grid.
    if (begin_pop("##wl_cat_pop", ImVec2(bp.x + pad, p0.y + h + 6 * WK),
                  std::min(std::max(w, 420 * WK), ImGui::GetMainViewport()->Size.x - 20.0f), ImVec2(10 * WK, 10 * WK))) {
        Theme::push_font(Theme::Fonts::ui(), 14 * WK);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(30 * WK, 7 * WK));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::Radius::R2);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, Theme::Tokens::TX3);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##wl_cat_search", "Search categories", cat_search_, sizeof(cat_search_));
        const ImVec2 sa = ImGui::GetItemRectMin(), sb = ImGui::GetItemRectMax();
        drawing::draw_ui_icon(ImGui::GetWindowDrawList(), drawing::UiIcon::Search, ImVec2(sa.x + 15 * WK, (sa.y + sb.y) * 0.5f), 5.5f * WK, Theme::u32(Theme::Tokens::TX3), 1.3f);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 6 * WK));
        // Fit the grid to the full list (not the filtered one, so typing does not resize it),
        // scrolling only past 320 px.
        const int grid_rows = (static_cast<int>(categories_.size()) + 2) / 2;
        const float grid_full = grid_rows * 34 * WK, grid_h = std::min(grid_full, 320 * WK);
        const bool grid_scrolls = grid_full > grid_h + 0.5f;
        ImGui::BeginChild("##wl_cat_grid", ImVec2(0, grid_h));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 2.0f * WK));
        const float chip_gap = 6 * WK;
        const float cw = (ImGui::GetContentRegionAvail().x - (grid_scrolls ? ImGui::GetStyle().ScrollbarSize : 0.0f) - chip_gap) * 0.5f;
        int col = 0;
        auto chip = [&](const char* label, int count, bool active, int catidx) {
            if (col == 1) ImGui::SameLine(0.0f, chip_gap);
            ImGui::PushID(catidx + 2);
            if (option("##c", label, count, active, cw, nullptr)) { selected_category_ = catidx; ImGui::CloseCurrentPopup(); }
            ImGui::PopID();
            col ^= 1;
        };
        if (!cat_search_[0] || contains_ci(std::string("All pairs"), cat_search_))
            chip("All pairs", scoped_count_, selected_category_ < 0, -1);
        for (int i = 0; i < static_cast<int>(categories_.size()); ++i) {
            if (cat_search_[0] && !contains_ci(categories_[i], cat_search_)) continue;
            chip(categories_[i].c_str(), category_counts_[i], selected_category_ == i, i);
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::EndPopup();
    }

    // Venues: All, then each venue with its logo and count.
    const float venue_popup_w = std::max(240 * WK, venue_w);
    if (begin_pop("##wl_venue_pop", ImVec2(bp.x + ww - pad - venue_popup_w, p0.y + h + 6 * WK), venue_popup_w, ImVec2(6 * WK, 6 * WK))) {
        const float rw = ImGui::GetContentRegionAvail().x;
        auto pick = [&](const char* id) {
            selected_exchange_ = id;
            selected_category_ = -1;
            cat_search_[0] = '\0';
            build_category_counts();
            ImGui::CloseCurrentPopup();
        };
        if (option("##venue_all", "All venues", all_count_, selected_exchange_.empty(), rw, nullptr)) pick("");
        for (size_t i = 0; i < exchange_counts_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (option("##venue", kWatchlistVenues[i].label, exchange_counts_[i], selected_exchange_ == kWatchlistVenues[i].id, rw, kWatchlistVenues[i].id))
                pick(kWatchlistVenues[i].id);
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }

    ImGui::SetCursorScreenPos(bp);
    ImGui::Dummy(ImVec2(ww, h + 8 * WK));
}

// ── Sort header: quiet labels on the row grid; the active one bright with its direction ──

void WatchlistWidget::render_sort_header() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float hh = 28 * WK;
    const float cy = bp.y + hh * 0.5f;
    const RowGrid g = row_grid(w, compact_);
    Theme::push_font(Theme::Fonts::ui(), 13 * WK);
    const float fs = ImGui::GetFontSize();
    auto cell = [&](const char* label, Sort col, float x, bool right_align) -> float {
        const bool active = sort_ == col;
        const float lw = ImGui::CalcTextSize(label).x;
        const float chev = active ? 12 * WK : 0.0f;
        const float cw = lw + chev;
        const float x0 = right_align ? x - cw : x;
        ImGui::SetCursorScreenPos(ImVec2(x0 - 4 * WK, bp.y));
        ImGui::PushID(static_cast<int>(col) + 40);
        const bool clk = ImGui::InvisibleButton("##sc", ImVec2(cw + 8 * WK, hh));
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        dl->AddText(ImVec2(x0, cy - fs * 0.5f), Theme::u32(active ? Theme::Tokens::TX1 : hov ? Theme::Tokens::TX2 : Theme::Tokens::TX3), label);
        if (active)
            drawing::draw_ui_icon(dl, sort_desc_ ? drawing::UiIcon::ChevronDown : drawing::UiIcon::ChevronUp,
                                  ImVec2(x0 + lw + 7 * WK, cy + 0.5f), 3.5f * WK, Theme::u32(Theme::Tokens::TX1), 1.3f);
        if (clk) {
            if (sort_ == col) sort_desc_ = !sort_desc_;
            else { sort_ = col; sort_desc_ = (col != Sort::Symbol); }
        }
        return cw;
    };
    const float pair_w = cell("Pair", Sort::Symbol, bp.x + g.sym_x, false);
    cell("Volume", Sort::Volume, bp.x + g.sym_x + pair_w + 16 * WK, false);
    const float chg_w = cell("24h", Sort::Change, bp.x + g.pill_right, true);
    cell("Price", Sort::Price, g.stacked ? bp.x + g.pill_right - chg_w - 18 * WK : bp.x + g.price_right, true);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(bp);
    ImGui::Dummy(ImVec2(w, hh));
    dl->AddLine(ImVec2(bp.x + 12 * WK, bp.y + hh - 1.0f), ImVec2(bp.x + w - 12 * WK, bp.y + hh - 1.0f), Theme::u32(Theme::Tokens::BD1));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Rows: [star] logo (venue badge) pair / volume ... price / change pill. Compact
// rows are one line: pair, price, pill.
// ═══════════════════════════════════════════════════════════════════════════════

void WatchlistWidget::render_rows() {
    auto& tickers = TickerManager::instance();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 4.0f * WK));
    ImGui::BeginChild("##wl_list", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const RowGrid g = row_grid(w, compact_);
    const float pitch = g.h + 2 * WK;
    ImFont* sym_font = Theme::Fonts::ui_semibold();
    ImFont* ui_font = Theme::Fonts::ui();
    ImFont* mono_font = Theme::Fonts::mono();
    const float sym_px = (g.compact ? 15.5f : 16.5f) * WK, sub_px = 13.5f * WK, price_px = (g.compact ? 15 : 16) * WK, pill_px = 13.5f * WK;

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible_.size()), pitch);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto* meta = visible_[i];
            const auto* t = tickers.get(meta->exchange, meta->symbol);
            const bool selected = meta->exchange == active_pair_.exchange && meta->symbol == active_pair_.symbol;
            const bool fav = favorites_.find(meta->pair_key) != favorites_.end();
            ImGui::PushID(i);
            const ImVec2 row_min = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, pitch));
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 a(row_min.x + g.pad, row_min.y + 1 * WK), b(row_min.x + w - g.pad, row_min.y + pitch - 1 * WK);
            const float mid = (a.y + b.y) * 0.5f;
            if (selected) {
                dl->AddRectFilled(a, b, Theme::u32(Theme::Tokens::CONTROL_HOVER), Theme::Radius::R2);
                dl->AddRectFilled(ImVec2(a.x, mid - 9 * WK), ImVec2(a.x + 3 * WK, mid + 9 * WK), Theme::u32(Theme::Tokens::BRAND), 1.5f * WK);
            } else if (hovered) {
                dl->AddRectFilled(a, b, Theme::u32(Theme::Tokens::CONTROL), Theme::Radius::R2);
            }
            const float mx = ImGui::GetIO().MousePos.x;
            const bool on_star = mx <= row_min.x + g.logo_x - 2 * WK;
            if (clicked) {
                if (on_star) {
                    auto it = favorites_.find(meta->pair_key);
                    if (it == favorites_.end()) favorites_.insert(meta->pair_key);
                    else favorites_.erase(it);
                } else if (!selected) {
#ifdef __EMSCRIPTEN__
                    // url_navigate carries ?ws= (and any other user params) across the reload.
                    url_navigate(build_terminal_path(meta->exchange, meta->symbol));
#endif
                }
            }
            // Star: shown when set (amber) or while the row is hovered.
            if (fav || hovered)
                draw_star(dl, ImVec2(row_min.x + g.star_x, mid), 5.5f * WK, fav,
                          Theme::u32(fav ? Theme::Tokens::WARN : (on_star && hovered ? Theme::Tokens::TX1 : Theme::Tokens::TX3)));
            // Logo, with the venue's badge when venues are mixed.
            LogoManager::instance().draw_coin(dl, meta->base_asset, ImVec2(row_min.x + g.logo_x, mid - g.logo * 0.5f), g.logo);
            if (selected_exchange_.empty()) {
                if (ImTextureID ex = LogoManager::instance().exchange(meta->exchange)) {
                    const float bw = std::round(g.logo * 0.46f);
                    const ImVec2 bc(row_min.x + g.logo_x + g.logo - bw * 0.35f, mid + g.logo * 0.5f - bw * 0.35f);
                    dl->AddCircleFilled(bc, bw * 0.62f, Theme::u32(selected ? Theme::Tokens::CONTROL_HOVER : hovered ? Theme::Tokens::CONTROL : Theme::Tokens::PANEL));
                    dl->AddImage(ex, ImVec2(bc.x - bw * 0.5f, bc.y - bw * 0.5f), ImVec2(bc.x + bw * 0.5f, bc.y + bw * 0.5f));
                }
            }
            // Price and change, measured first so the pair can ellipsize into what is left.
            char pbuf[32] = "", cbuf[16] = "";
            if (t && t->last_price > 0.0) meta->fmt.format_price(pbuf, sizeof(pbuf), t->last_price);
            if (t) snprintf(cbuf, sizeof(cbuf), "%+.2f%%", t->change_pct_24h);
            const float price_w = pbuf[0] ? mono_font->CalcTextSizeA(price_px, FLT_MAX, 0, pbuf).x : 0.0f;
            const float right_block = g.stacked ? std::max(price_w, g.pill_w) : (g.pill_right - g.price_right) + price_w;
            const float sym_right = row_min.x + g.pill_right - right_block - 12 * WK;
            const bool two_line = !g.compact;
            const float line1 = two_line ? mid - 11 * WK : mid;   // centre of the first line
            const float line2 = mid + 12 * WK;
            // Pair: base bright, /quote dim; ellipsized on a character boundary (CJK names are 3 bytes a glyph).
            {
                const char* base = meta->base_asset.empty() ? meta->symbol.c_str() : meta->base_asset.c_str();
                char quote[16] = {};
                if (!meta->base_asset.empty()) snprintf(quote, sizeof(quote), "/%s", meta->quote_asset.c_str());
                const float sx = row_min.x + g.sym_x, avail = sym_right - sx;
                const char* ell = "\xE2\x80\xA6";
                const float base_w = sym_font->CalcTextSizeA(sym_px, FLT_MAX, 0, base).x;
                const float by = line1 - sym_px * 0.58f;
                if (base_w <= avail) {
                    dl->AddText(sym_font, sym_px, ImVec2(sx, by), Theme::u32(Theme::Tokens::TX1), base);
                    if (quote[0]) {
                        const float qw = ui_font->CalcTextSizeA(sym_px, FLT_MAX, 0, quote).x;
                        if (base_w + qw <= avail) dl->AddText(ui_font, sym_px, ImVec2(sx + base_w + 1.0f, by), Theme::u32(Theme::Tokens::TX3), quote);
                    }
                } else {
                    const float ell_w = sym_font->CalcTextSizeA(sym_px, FLT_MAX, 0, ell).x;
                    int k = static_cast<int>(strlen(base));
                    while (k > 0 && sym_font->CalcTextSizeA(sym_px, FLT_MAX, 0, base, base + k).x + ell_w > avail) {
                        --k;
                        while (k > 0 && (static_cast<unsigned char>(base[k]) & 0xC0) == 0x80) --k;
                    }
                    dl->AddText(sym_font, sym_px, ImVec2(sx, by), Theme::u32(Theme::Tokens::TX1), base, base + k);
                    dl->AddText(sym_font, sym_px, ImVec2(sx + sym_font->CalcTextSizeA(sym_px, FLT_MAX, 0, base, base + k).x, by), Theme::u32(Theme::Tokens::TX1), ell);
                }
                if (two_line && t && t->volume_quote > 0.0) {
                    char vbuf[24], line[32];
                    fmt_volume(vbuf, sizeof(vbuf), t->volume_quote);
                    snprintf(line, sizeof(line), "Vol %s", vbuf);
                    dl->AddText(ui_font, sub_px, ImVec2(sx, line2 - sub_px * 0.58f), Theme::u32(Theme::Tokens::TX3), line);
                }
            }
            // Price: stacked on the first line at the right, else its own column.
            if (pbuf[0]) {
                const float py = (g.stacked ? line1 : mid) - price_px * 0.58f;
                const float px = row_min.x + (g.stacked ? g.pill_right : g.price_right) - price_w;
                dl->AddText(mono_font, price_px, ImVec2(px, py), Theme::u32(Theme::Tokens::TX1), pbuf);
            }
            // Change pill: under the price when stacked, else beside it.
            if (cbuf[0]) {
                Theme::change_pill(dl, row_min.x + g.pill_right, g.stacked ? line2 : mid, t->change_pct_24h,
                                   g.compact ? 13.0f * WK : pill_px);
            }
            if (hovered && !on_star && mx <= row_min.x + g.sym_x)
                Theme::tooltip("%s / %s", meta->symbol.c_str(), widget_venue_label(meta->exchange));
            ImGui::PopID();
        }
    }
    clipper.End();
    ImGui::PopStyleVar();   // ItemSpacing
    ImGui::EndChild();
    ImGui::PopStyleVar();   // WindowPadding
}
