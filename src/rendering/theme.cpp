#include "rendering/theme.h"
#include "ui/drawing/drawing_icons.h"
#include "core/ui_test_marks.h"
#include "imgui_internal.h"   // ImGuiSelectableFlags_SpanAvailWidth
#include <cctype>
#include <cfloat>
#include <cmath>
#include <string>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include "core/workspace_settings.h"
#include "core/heatmap_colormap.h"
#include <implot.h>
#ifdef __EMSCRIPTEN__
#include <emscripten/em_asm.h>
#endif

// ═══════════════════════════════════════════════════════════════════════════════
// theme.cpp - edgedepth design system implementation
// Style mapping per the design system's ImGui notes §1, tokens per tokens.json.
// ═══════════════════════════════════════════════════════════════════════════════

namespace Theme {
    namespace {
        // font atlas
        ImFont* f_ui          = nullptr;
        ImFont* f_ui_semibold = nullptr;
        ImFont* f_ui_sm = nullptr;
        ImFont* f_heading     = nullptr;
        ImFont* f_label       = nullptr;
        ImFont* f_mono_xs     = nullptr;
        ImFont* f_mono_sm     = nullptr;
        ImFont* f_mono        = nullptr;
        ImFont* f_mono_md     = nullptr;
        ImFont* f_mono_lg     = nullptr;

        // tweak state
        Accent           g_accent  = Accent::Mono;
        CandleConvention g_candles = CandleConvention::TealMag;
        Chrome           g_chrome  = Chrome::EdgeDepth;
        int              g_density = 8;
        uint32_t g_custom_up = 0x2fd6ad, g_custom_down = 0xee5c78;
        bool g_preferences_saved = true;

        ImVec4 with_a(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }
    }

    int density() { return g_density; }
    void set_density(int d) { g_density = std::clamp(d, 3, 10); }
    // Anchored to the theme-tokens density values at the default dial (d8):
    // watchlist rows 24px, DOM/tape rows 18px; the dial trades ±1.1/±0.9 px per step.
    float row_h()       { return Layout::WATCHLIST_ROW_H - (g_density - 8) * 1.1f; }
    float row_h_dense() { return Layout::DOMTAPE_ROW_H   - (g_density - 8) * 0.9f; }

    Accent accent() { return g_accent; }
    CandleConvention candles() { return g_candles; }
    Chrome chrome() { return g_chrome; }

    namespace {
        // Dark chrome presets (James 2026-10-11). One row per Chrome value, in enum order:
        // every surface, control, hairline and text token as 0xRRGGBB; BD3, GRID (BD1's hue) and TX4 keep alphas.
        // To add a preset: append a Chrome value, a row here and its label in kChromeNames. Brand (LOGO, PRO tags),
        // signed market colors, WARN and the heatmap ramps are not chrome and stay fixed.
        struct ChromeRamp {
            uint32_t base, panel, elev, input, hover, active;
            uint32_t control, control_hover, control_active, track, card, menu;
            uint32_t bd1, bd2, bd3, tx1, tx2, tx3, tx4;
            float bd3_a, grid_a, tx4_a;
        };
        constexpr ChromeRamp kChrome[] = {
            // EdgeDepth: the v2 token sheet (default; pixels unchanged).
            {0x050505, 0x080808, 0x0d0d0d, 0x0b0b0b, 0x141414, 0x1b1b1b,
             0x171717, 0x202020, 0x2a2a2a, 0x101010, 0x0c0c0c, 0x0e0e0e,
             0x1c1c1c, 0x292929, 0x737373, 0xe8e8e8, 0xb0b0b0, 0x858585, 0x737373, 0.55f, 0.55f, 0.72f},
            // Graphite: lifted neutral greys, softer hairlines.
            {0x111113, 0x161618, 0x1b1b1e, 0x19191b, 0x232326, 0x2a2a2e,
             0x26262a, 0x2f2f33, 0x39393e, 0x1d1d20, 0x1a1a1c, 0x1d1d20,
             0x252528, 0x313135, 0x7e7e84, 0xe8e8ea, 0xb8b8bc, 0x929297, 0x808085, 0.45f, 0.50f, 0.75f},
            // Midnight: the marketing site's blue-black (--bg-0 #0b0e13, --line-1/2, green-neutral text).
            {0x0b0e13, 0x0e1218, 0x11161d, 0x0f131a, 0x161b24, 0x1c2230,
             0x1a2029, 0x212835, 0x2a3140, 0x131820, 0x10141b, 0x131820,
             0x1e242e, 0x2a3140, 0x66736e, 0xecf0ef, 0xadb9b4, 0x85918c, 0x717d79, 0.60f, 0.55f, 0.75f},
            // High contrast: black base, brighter text ramp and hairlines (WCAG: TX3 >= 9:1 and TX4 >= 6:1 on resting
            // surfaces, BD2 >= 3:1 on panels; EdgeDepth gives 4.9:1 and 2.6:1).
            {0x000000, 0x030303, 0x0b0b0b, 0x0a0a0a, 0x1a1a1a, 0x262626,
             0x1c1c1c, 0x2a2a2a, 0x383838, 0x101010, 0x080808, 0x0d0d0d,
             0x3d3d3d, 0x5f5f5f, 0xa6a6a6, 0xffffff, 0xdedede, 0xbdbdbd, 0xa8a8a8, 0.70f, 0.70f, 0.90f},
        };
        constexpr const char* kChromeNames[] = {"EdgeDepth", "Graphite", "Midnight", "High contrast"};
        static_assert(std::size(kChrome) == std::size(kChromeNames) &&
                      std::size(kChrome) == size_t(Chrome::HighContrast) + 1, "one row and label per Chrome value");
    }

    void set_chrome(Chrome c) {
        const ChromeRamp& r = kChrome[std::min<size_t>(size_t(c), std::size(kChrome) - 1)];
        g_chrome = Chrome(&r - kChrome);
        using namespace Tokens;
        BASE = from_hex(r.base);   PANEL = from_hex(r.panel); ELEV = from_hex(r.elev);
        INPUT = from_hex(r.input); HOVER = from_hex(r.hover); ACTIVE = from_hex(r.active);
        CONTROL = from_hex(r.control); CONTROL_HOVER = from_hex(r.control_hover);
        CONTROL_ACTIVE = from_hex(r.control_active); TRACK = from_hex(r.track);
        CARD = from_hex(r.card); MENU = from_hex(r.menu);
        BD1 = from_hex(r.bd1); BD2 = from_hex(r.bd2); BD3 = from_hex(r.bd3, r.bd3_a); GRID = from_hex(r.bd1, r.grid_a);
        TX1 = from_hex(r.tx1); TX2 = from_hex(r.tx2); TX3 = from_hex(r.tx3); TX4 = from_hex(r.tx4, r.tx4_a);
        apply_dark_theme();  // re-map style colors from the new ramp
    }

    void set_accent(Accent a) {
        g_accent = a;
        uint32_t hex, tx; float soft, line;
        switch (a) {
            case Accent::Indigo: hex = 0x6d8bff; tx = 0x8ba3ff; soft = 0.15f; line = 0.55f; break;
            case Accent::Amber:  hex = 0xf0b350; tx = 0xf6c76e; soft = 0.15f; line = 0.55f; break;
            case Accent::Mono:   hex = 0xe7e9ed; tx = 0xffffff; soft = 0.13f; line = 0.45f; break;
            case Accent::Teal:   // brand mint: accent #4ddbac, hover tint #78e7c4 (was cyan)
            default:             hex = 0x4ddbac; tx = 0x78e7c4; soft = 0.14f; line = 0.55f; break;
        }
        Tokens::BRAND      = from_hex(hex);
        Tokens::BRAND_SOFT = from_hex(hex, soft);
        Tokens::BRAND_LINE = from_hex(hex, line);
        Tokens::BRAND_TX   = from_hex(tx);
        apply_dark_theme();  // re-map style colors that reference BRAND
    }

    void set_candle_convention(CandleConvention c) {
        g_candles = c;
        uint32_t up, down; float line = 0.50f;
        switch (c) {
            // Calm royal blue / white (the TradingView look; James 2026-10-11): #2962ff lifted from 58% to 65%
            // lightness, hue and saturation kept, so UP text holds 4.5:1 on every chrome preset's resting surfaces.
            case CandleConvention::BlueWhite: up = 0x4d7cff; down = 0xf2f2f2; break;
            case CandleConvention::Custom: up = g_custom_up; down = g_custom_down; break;
            case CandleConvention::Classic: up = 0x26d07a; down = 0xff4d5e; break;
            case CandleConvention::Muted:   up = 0x41a583; down = 0xcf6173; line = 0.45f; break;
            // Colourblind-safer: blue/orange stays apart for red-green colour vision deficiency.
            case CandleConvention::BlueOrange: up = 0x56b4e9; down = 0xf28c28; break;
            case CandleConvention::TealMag: // design tokens: up #2fd6ad / down #ee5c78
            default:                        up = 0x2fd6ad; down = 0xee5c78; break;
        }
        Tokens::UP        = from_hex(up);
        Tokens::DOWN      = from_hex(down);
        Tokens::UP_SOFT   = from_hex(up, 0.13f);
        Tokens::DOWN_SOFT = from_hex(down, 0.13f);
        Tokens::UP_LINE   = from_hex(up, line);
        Tokens::DOWN_LINE = from_hex(down, line);
    }


    // Appearance is a browser preference, shared by live charts and replay.
    // Keep it independent of live-only workspace capture. v2 adds the chrome
    // preset and the liquidation colormap; a browser with only v1 keeps its
    // market colors and accent (chrome EdgeDepth) and is written as v2 once.
    // v1 is left in place for an older build.
    namespace {
        workspace::Json read_storage(const char* key) {
#ifdef __EMSCRIPTEN__
            char* text = reinterpret_cast<char*>(EM_ASM_PTR({
                try {
                    const value = localStorage.getItem(UTF8ToString($0));
                    return value && value.length < 4096 ? stringToNewUTF8(value) : 0;
                } catch (_) { return 0; }
            }, key));
            if (!text) return {};
            auto j = workspace::Json::parse(text, nullptr, false);
            free(text);
            return j.is_object() ? j : workspace::Json{};
#else
            (void)key;
            return {};
#endif
        }
    }

    void load_preferences() {
        workspace::Json j = read_storage("edgedepth.appearance.v2");
        int version = 0;
        workspace::read(j, "version", version, 2, 2);
        if (version != 2) {
            j = read_storage("edgedepth.appearance.v1");
            workspace::read(j, "version", version, 1, 1);
            if (version != 1) return;
        }
        int liq = int(HeatmapColormap::liq_map());
        workspace::read(j, "chrome", g_chrome, 0, std::size(kChrome) - 1);
        workspace::read(j, "accent", g_accent, 0, 3);
        workspace::read(j, "candles", g_candles, 0, int(CandleConvention::BlueOrange));
        workspace::read(j, "up", g_custom_up, 0, 0xffffff);
        workspace::read(j, "down", g_custom_down, 0, 0xffffff);
        workspace::read(j, "liq_map", liq, 0, int(HeatmapColormap::LiqMap::Inferno));
        set_chrome(g_chrome);
        set_accent(g_accent);
        set_candle_convention(g_candles);
        if (liq != int(HeatmapColormap::liq_map())) HeatmapColormap::set_liq_map(HeatmapColormap::LiqMap(liq));
        if (version == 1) save_preferences();
    }

    void save_preferences() {
#ifdef __EMSCRIPTEN__
        const auto text = workspace::Json({{"version",2}, {"chrome",int(chrome())}, {"accent",int(accent())},
            {"candles",int(candles())}, {"up",g_custom_up}, {"down",g_custom_down},
            {"liq_map",int(HeatmapColormap::liq_map())}}).dump();
        g_preferences_saved = EM_ASM_INT({
            try { localStorage.setItem('edgedepth.appearance.v2', UTF8ToString($0)); return 1; }
            catch (_) { return 0; }
        }, text.c_str());
#endif
    }

    namespace {
        // A full-width combo of labels; returns the picked index. Browser tests click "<name>" and its rows
        // "<name>.<label>" (ui_test marks).
        int appearance_combo(const char* name, const char* const* labels, int n, int selected) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const ImVec2 q(p.x + ImGui::GetContentRegionAvail().x, p.y + ImGui::GetFrameHeight());
            ImGui::SetNextItemWidth(-1);
            const std::string id = std::string("##") + name;
            int picked = selected;
            const bool open = ImGui::BeginCombo(id.c_str(), labels[selected]);
            if (ui_test::enabled()) ui_test::mark_rect(name, p, q);
            if (open) {
                for (int i = 0; i < n; ++i) {
                    if (ImGui::Selectable(labels[i], i == selected)) picked = i;
                    if (i == selected) ImGui::SetItemDefaultFocus();
                    if (ui_test::enabled()) ui_test::mark(std::string(name) + "." + labels[i]);
                }
                ImGui::EndCombo();
            }
            return picked;
        }
    }

    void render_appearance_controls() {
        bool changed = false;
        ImGui::TextUnformatted("Interface theme");
        const int c = appearance_combo("appearance.theme", kChromeNames, int(std::size(kChromeNames)), int(chrome()));
        if (c != int(chrome())) { set_chrome(Chrome(c)); changed = true; }
        ImGui::Spacing();
        // Display order keeps Custom last; the enum order is the saved one.
        static constexpr CandleConvention kPalettes[] = {CandleConvention::TealMag, CandleConvention::Classic,
            CandleConvention::Muted, CandleConvention::BlueWhite, CandleConvention::BlueOrange, CandleConvention::Custom};
        static constexpr const char* kPaletteNames[] = {"Teal / rose", "Green / red", "Muted", "Blue / white",
            "Blue / orange", "Custom"};
        int shown = 0;
        while (kPalettes[shown] != candles() && shown + 1 < int(std::size(kPalettes))) ++shown;
        ImGui::TextUnformatted("Market colors");
        const int palette = appearance_combo("appearance.market", kPaletteNames, int(std::size(kPaletteNames)), shown);
        if (palette != shown) {
            set_candle_convention(kPalettes[palette]);
            changed = true;
        }
        ImVec4 up = Tokens::UP, down = Tokens::DOWN;
        const bool up_changed = ImGui::ColorEdit3("Up / buy", &up.x, ImGuiColorEditFlags_NoInputs);
        const bool down_changed = ImGui::ColorEdit3("Down / sell", &down.x, ImGuiColorEditFlags_NoInputs);
        if (up_changed || down_changed) {
            const auto hex = [](ImVec4 c) {
                return (uint32_t(c.x * 255 + 0.5f) << 16) |
                       (uint32_t(c.y * 255 + 0.5f) << 8) | uint32_t(c.z * 255 + 0.5f);
            };
            g_custom_up = hex(up); g_custom_down = hex(down);
            set_candle_convention(CandleConvention::Custom);
            changed = true;
        }
        ImGui::TextWrapped("Applies to candles, trade bubbles and signed market data throughout the terminal.");
        ImGui::Spacing();
        ImGui::TextUnformatted("Interface accent");
        int a = int(accent());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##interface_accent", &a, "Mint\0Indigo\0Amber\0Neutral\0")) {
            set_accent(Accent(a)); changed = true;
        }
        if (ImGui::Button("Reset appearance")) {
            set_chrome(Chrome::EdgeDepth);
            set_candle_convention(CandleConvention::TealMag);
            set_accent(Accent::Mono); changed = true;
        }
        if (changed) save_preferences();
        if (!g_preferences_saved)
            ImGui::TextWrapped("Browser storage is unavailable. These colors apply until you reload.");
    }

    namespace {
        void apply_implot_style() {
            using namespace Tokens;
            ImPlotStyle& s = ImPlot::GetStyle();
            // chart sits directly on the app base - charcoal ground shows
            // through the liq field's transparent low-intensity cells
            s.Colors[ImPlotCol_PlotBg]        = BASE;
            s.Colors[ImPlotCol_FrameBg]       = BASE;
            s.Colors[ImPlotCol_PlotBorder]    = ImVec4(0, 0, 0, 0);
            s.PlotBorderSize = 0.0f;
            // gridlines - barely-there hairlines
            s.Colors[ImPlotCol_AxisGrid]      = GRID;
            s.Colors[ImPlotCol_AxisBg]        = BASE;
            s.Colors[ImPlotCol_AxisBgHovered] = BASE;
            s.Colors[ImPlotCol_AxisBgActive]  = BASE;
            s.Colors[ImPlotCol_AxisText]      = TX3;
            // crosshair - dashed look approximated by low-alpha solid
            s.Colors[ImPlotCol_Crosshairs]    = from_hex(0x96a8b8, 0.35f);
            s.Colors[ImPlotCol_Selection]     = BRAND;
            s.Colors[ImPlotCol_LegendBg]      = with_a(PANEL, 0.98f);
            s.Colors[ImPlotCol_LegendBorder]  = BD2;
            // no tick marks - labels only, like the reference
            s.MajorTickLen  = ImVec2(0, 0);
            s.MinorTickLen  = ImVec2(0, 0);
            s.MajorTickSize = ImVec2(0, 0);
            s.MinorTickSize = ImVec2(0, 0);
            s.MajorGridSize = ImVec2(1.0f, 1.0f);
            s.MinorGridSize = ImVec2(0.5f, 0.5f);
            s.MinorAlpha    = 0.25f;
            s.PlotPadding   = ImVec2(0, 0);
            s.LabelPadding  = ImVec2(4, 2);
            s.LegendPadding = ImVec2(10, 10);
        }
    }

    void apply_dark_theme() {
        using namespace Tokens;
        ImGuiStyle& s = ImGui::GetStyle();
        ImVec4* c = s.Colors;

        // ── windows / surfaces ───────────────────────────────────────────────
        c[ImGuiCol_WindowBg]          = PANEL;
        c[ImGuiCol_ChildBg]           = ImVec4(0, 0, 0, 0);  // children inherit
        c[ImGuiCol_PopupBg]           = ELEV;
        c[ImGuiCol_MenuBarBg]         = ELEV;
        c[ImGuiCol_Border]            = BD2;
        c[ImGuiCol_BorderShadow]      = ImVec4(0, 0, 0, 0);

        // ── frames (inputs, idle controls) ───────────────────────────────────
        // Fields and checkboxes share the soft control fill (design v3), so they read on the near-black panels.
        c[ImGuiCol_FrameBg]           = CONTROL;
        c[ImGuiCol_FrameBgHovered]    = CONTROL_HOVER;
        c[ImGuiCol_FrameBgActive]     = CONTROL_ACTIVE;

        // ── text ─────────────────────────────────────────────────────────────
        c[ImGuiCol_Text]              = TX1;
        c[ImGuiCol_TextDisabled]      = TX3;

        // ── title / tabs (docked panel chrome stays quiet) ───────────────────
        // James 2026-10-11: "remove the blue bit from above a widget heading, or improve it so it's cleaner to see
        // which tab we are on". The tab strip sits a step below the panels; a panel's selected tab takes the panel's
        // own surface, so it reads as the front of its panel, and other tabs sit dimmed on the strip (main.cpp's tab
        // pass). Only the focused panel's selected tab carries a line; other panels carry none. James 2026-10-15: the
        // white line read as a hard rule above the widget ("maybe a thin strip of grey"), so it is a 1 px hairline a
        // step brighter than the panel borders (BD3, the hover-border grey of the chrome preset), whatever the accent.
        c[ImGuiCol_TitleBg]           = BASE;
        c[ImGuiCol_TitleBgActive]     = BASE;
        c[ImGuiCol_TitleBgCollapsed]  = PANEL;
        c[ImGuiCol_Tab]               = BASE;
        c[ImGuiCol_TabHovered]        = HOVER;
        c[ImGuiCol_TabActive]         = PANEL;   // TabSelected
        c[ImGuiCol_TabUnfocused]      = BASE;    // TabDimmed
        c[ImGuiCol_TabUnfocusedActive]= PANEL;   // TabDimmedSelected
        c[ImGuiCol_TabSelectedOverline]       = BD3;
        c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0, 0, 0, 0);

        // ── selection / headers - brand-soft, used sparingly ─────────────────
        c[ImGuiCol_Header]            = BRAND_SOFT;
        c[ImGuiCol_HeaderHovered]     = HOVER;
        c[ImGuiCol_HeaderActive]      = with_a(BRAND, 0.22f);

        // ── buttons - soft filled and rounded by default (design v3) ─────────
        c[ImGuiCol_Button]            = CONTROL;
        c[ImGuiCol_ButtonHovered]     = CONTROL_HOVER;
        c[ImGuiCol_ButtonActive]      = CONTROL_ACTIVE;
        c[ImGuiCol_CheckMark]         = BRAND;
        c[ImGuiCol_SliderGrab]        = BRAND;
        c[ImGuiCol_SliderGrabActive]  = BRAND;

        // ── scrollbars - thin, hairline thumbs ───────────────────────────────
        c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab]        = BD2;
        c[ImGuiCol_ScrollbarGrabHovered] = BD3;
        c[ImGuiCol_ScrollbarGrabActive]  = BD3;

        // ── separators / resize ──────────────────────────────────────────────
        c[ImGuiCol_Separator]         = BD1;
        c[ImGuiCol_SeparatorHovered]  = BD3;
        c[ImGuiCol_SeparatorActive]   = BRAND_LINE;
        c[ImGuiCol_ResizeGrip]        = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ResizeGripHovered] = BD3;
        c[ImGuiCol_ResizeGripActive]  = BRAND_LINE;

        // ── tables - hairlines, no zebra ─────────────────────────────────────
        c[ImGuiCol_TableHeaderBg]     = ELEV;
        c[ImGuiCol_TableBorderStrong] = BD2;
        c[ImGuiCol_TableBorderLight]  = BD1;
        c[ImGuiCol_TableRowBg]        = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TableRowBgAlt]     = ImVec4(0, 0, 0, 0);

        // ── plots / docking / nav ────────────────────────────────────────────
        c[ImGuiCol_PlotLines]         = BRAND;
        c[ImGuiCol_PlotHistogram]     = BRAND;
        c[ImGuiCol_DockingPreview]    = with_a(BRAND, 0.35f);
        c[ImGuiCol_DockingEmptyBg]    = BASE;
        c[ImGuiCol_NavHighlight]      = BRAND_LINE;
        c[ImGuiCol_ModalWindowDimBg]  = from_hex(0x030508, 0.62f);

        // ── metrics (IMGUI-NOTES §1) ─────────────────────────────────────────
        s.WindowRounding    = 0.0f;   // docked panels are square (chrome rules)
        s.ChildRounding     = 0.0f;
        s.FrameRounding     = Radius::R2;   // buttons, inputs, checkboxes
        s.GrabRounding      = Radius::R1;
        s.PopupRounding     = Radius::R3;   // floating chrome
        s.TabRounding       = 0.0f;         // dock tabs stay square with their panels
        s.TabBarOverlineSize = 1.0f;        // the focused panel's hairline (see the tab colours above)
        // One close per panel: the tab's own x, shown on hover (the dock node's second x at the far right of every
        // tab bar is gone, as the window-menu arrow went on 2026-10-11).
        s.TabCloseButtonMinWidthSelected   = 0.0f;
        s.TabCloseButtonMinWidthUnselected = 0.0f;
        s.DockingNodeHasCloseButton = false;
        // No window menu (the down arrow at the left of every dock tab bar; James 2026-10-11: "it doesn't even
        // work"). Panels are closed with their tab's x and docked by dragging; widgets have no collapse.
        s.WindowMenuButtonPosition = ImGuiDir_None;
        s.WindowBorderSize  = 1.0f;
        s.ChildBorderSize   = 1.0f;
        s.FrameBorderSize   = 0.0f;
        s.PopupBorderSize   = 1.0f;
        s.ScrollbarRounding = 4.0f;
        s.ScrollbarSize     = 9.0f;
        s.CellPadding       = ImVec2(6, 2);
        s.ItemSpacing       = ImVec2(8, 6);
        s.ItemInnerSpacing  = ImVec2(6, 4);
        s.FramePadding      = ImVec2(9, 4);
        s.WindowPadding     = ImVec2(0, 0);   // panels manage their own gutters
        s.GrabMinSize       = 16.0f;
        s.DisabledAlpha     = 0.65f;

        apply_implot_style();
    }

    void apply_trading_colors() {
        // retained for call-site compatibility - table styling now lives in
        // apply_dark_theme(); nothing extra to override here.
    }

    bool begin_popup(const char* id, ImGuiWindowFlags flags) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
        const bool open = ImGui::BeginPopup(id, flags);
        ImGui::PopStyleVar();
        return open;
    }

    bool choice_button(const char* label, bool selected, ImVec2 size) {
        ImGui::PushStyleColor(ImGuiCol_Button, selected ? Tokens::CONTROL_ACTIVE : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? Tokens::CONTROL_ACTIVE : Tokens::CONTROL);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Tokens::CONTROL_ACTIVE);
        ImGui::PushStyleColor(ImGuiCol_Text, selected ? Tokens::TX1 : Tokens::TX2);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::R2);
        const bool clicked = ImGui::Button(label, size);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        return clicked;
    }

    void push_font(ImFont* font, float logical_px) {
        const float scale = ImGui::GetIO().FontGlobalScale;
        ImGui::PushFont(font, logical_px / (scale > 0.0f ? scale : 1.0f));
    }

    namespace {
        // Colours honour BeginDisabled (GetColorU32 applies style alpha).
        ImU32 col(const ImVec4& c, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); }
        bool has_icon(drawing::UiIcon icon) { return static_cast<uint8_t>(icon) != 255 && icon != drawing::UiIcon::None; }
    }

    bool button(const char* label, ButtonKind kind, ImVec2 size, drawing::UiIcon icon, bool trailing_icon) {
        const char* end = ImGui::FindRenderedTextEnd(label);
        const ImVec2 ts = ImGui::CalcTextSize(label, end);
        const float h = size.y > 0.0f ? size.y : ImGui::GetFrameHeight();
        const bool iconed = has_icon(icon);
        const float glyph = std::round(h * 0.20f), icon_w = iconed ? glyph * 2.0f : 0.0f;
        const float gap = iconed && ts.x > 0.0f ? 7.0f : 0.0f;
        const float pad = std::round(h * 0.46f);
        const float content = ts.x + icon_w + gap;
        const float w = size.x > 0.0f ? size.x : pad * 2.0f + content;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        ImVec4 bg(0, 0, 0, 0), fg = Tokens::TX1;
        switch (kind) {
            case ButtonKind::Soft:
                bg = held ? Tokens::CONTROL_ACTIVE : hovered ? Tokens::CONTROL_HOVER : Tokens::CONTROL; break;
            case ButtonKind::Ghost:
                bg = held ? Tokens::CONTROL_HOVER : hovered ? Tokens::CONTROL : ImVec4(0, 0, 0, 0);
                fg = hovered ? Tokens::TX1 : Tokens::TX2; break;
            case ButtonKind::Primary:
                bg = held ? Tokens::TX2 : hovered ? from_hex(0xffffff) : Tokens::TX1; fg = Tokens::BRAND_INK; break;
            case ButtonKind::Upgrade:
                bg = hovered || held ? Tokens::LOGO_HOVER : Tokens::LOGO; fg = Tokens::BRAND_INK; break;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 q(p.x + w, p.y + h);
        if (bg.w > 0.0f) dl->AddRectFilled(p, q, col(bg), std::min(Radius::R2, h * 0.5f));
        if (ImGui::IsItemFocused() && GImGui->NavCursorVisible)
            dl->AddRect(ImVec2(p.x - 2, p.y - 2), ImVec2(q.x + 2, q.y + 2), col(Tokens::BRAND_LINE), Radius::R2 + 2.0f, 0, 1.5f);
        float x = p.x + (w - content) * 0.5f;
        const float cy = p.y + h * 0.5f;
        if (iconed && !trailing_icon) { drawing::draw_ui_icon(dl, icon, ImVec2(x + glyph, cy), glyph, col(fg), 1.5f); x += icon_w + gap; }
        dl->AddText(ImVec2(std::round(x), std::round(cy - ts.y * 0.5f)), col(fg), label, end);
        if (iconed && trailing_icon) drawing::draw_ui_icon(dl, icon, ImVec2(x + ts.x + gap + glyph, cy), glyph, col(fg), 1.5f);
        return clicked;
    }

    float button_width(const char* label, float height, bool with_icon) {
        const float h = height > 0.0f ? height : ImGui::GetFrameHeight();
        const float tsx = ImGui::CalcTextSize(label, nullptr, true).x;
        const float icon_w = with_icon ? std::round(h * 0.20f) * 2.0f + (tsx > 0.0f ? 7.0f : 0.0f) : 0.0f;
        return std::round(h * 0.46f) * 2.0f + tsx + icon_w;
    }

    float segmented_width(const char* const* labels, int n, float height) {
        const float h = height > 0.0f ? height : ImGui::GetFrameHeight();
        float total = 6.0f;
        for (int i = 0; i < std::min(n, 16); ++i) total += ImGui::CalcTextSize(labels[i], nullptr, true).x + std::round(h * 0.5f) * 2.0f;
        return total;
    }

    bool icon_button(const char* id, drawing::UiIcon icon, const char* tooltip, float size, bool active) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
        const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
        if (held || hovered || active)
            dl->AddCircleFilled(c, size * 0.5f, col(held ? Tokens::CONTROL_ACTIVE : active ? Tokens::CONTROL_HOVER : Tokens::CONTROL), 24);
        drawing::draw_ui_icon(dl, icon, c, std::round(size * 0.24f), col(hovered || active ? Tokens::TX1 : Tokens::TX3), 1.4f);
        if (tooltip && hovered) Theme::tooltip("%s", tooltip);
        return clicked;
    }

    int segmented(const char* id, const char* const* labels, int n, int selected, float height) {
        ImGui::PushID(id);
        const float h = height > 0.0f ? height : ImGui::GetFrameHeight();
        constexpr float inset = 3.0f;
        const float pad = std::round(h * 0.5f);
        float total = inset * 2.0f;
        float widths[16];
        n = std::min(n, 16);
        for (int i = 0; i < n; ++i) { widths[i] = ImGui::CalcTextSize(labels[i], nullptr, true).x + pad * 2.0f; total += widths[i]; }
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + total, p.y + h), col(Tokens::TRACK), Radius::R2 + 2.0f);
        dl->AddRect(p, ImVec2(p.x + total, p.y + h), col(Tokens::BD1), Radius::R2 + 2.0f);
        int clicked = selected;
        float x = p.x + inset;
        for (int i = 0; i < n; ++i) {
            ImGui::SetCursorScreenPos(ImVec2(x, p.y));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##seg", ImVec2(widths[i], h))) clicked = i;
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (ui_test::enabled()) ui_test::mark(std::string(id) + "." + std::string(labels[i], ImGui::FindRenderedTextEnd(labels[i])));
            if (i == selected)
                dl->AddRectFilled(ImVec2(x, p.y + inset), ImVec2(x + widths[i], p.y + h - inset), col(Tokens::CONTROL_ACTIVE), Radius::R2);
            else if (hovered)
                dl->AddRectFilled(ImVec2(x, p.y + inset), ImVec2(x + widths[i], p.y + h - inset), col(Tokens::CONTROL), Radius::R2);
            const char* end = ImGui::FindRenderedTextEnd(labels[i]);
            const ImVec2 ts = ImGui::CalcTextSize(labels[i], end);
            dl->AddText(ImVec2(std::round(x + (widths[i] - ts.x) * 0.5f), std::round(p.y + (h - ts.y) * 0.5f)),
                        col(i == selected ? Tokens::TX1 : hovered ? Tokens::TX2 : Tokens::TX3), labels[i], end);
            x += widths[i];
        }
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy(ImVec2(total, h));
        ImGui::PopID();
        return clicked;
    }

    void chip(const char* text, const ImVec4& fg, const ImVec4& bg) {
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 p = ImGui::GetCursorScreenPos(), size(ts.x + 14.0f, ts.y + 4.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), col(bg), size.y * 0.5f);
        dl->AddText(ImVec2(p.x + 7.0f, p.y + 2.0f), col(fg), text);
        ImGui::Dummy(size);
    }

    float change_pill_width(float px) {
        return Fonts::ui()->CalcTextSizeA(px, FLT_MAX, 0, "+00.00%").x + px;
    }

    float change_pill(ImDrawList* dl, float right_x, float cy, double pct, float px) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%+.2f%%", pct);
        ImFont* f = Fonts::ui();
        const float tw = f->CalcTextSizeA(px, FLT_MAX, 0, buf).x;
        const float w = std::max(change_pill_width(px), tw + px), h = std::round(px * 1.5f);
        const ImVec4& fg = pct > 0 ? Tokens::UP : pct < 0 ? Tokens::DOWN : Tokens::TX2;
        const ImVec4 bg = pct > 0 ? ImVec4(Tokens::UP.x, Tokens::UP.y, Tokens::UP.z, 0.10f)
                        : pct < 0 ? ImVec4(Tokens::DOWN.x, Tokens::DOWN.y, Tokens::DOWN.z, 0.10f) : Tokens::CONTROL;
        const float y0 = std::round(cy - h * 0.5f);
        dl->AddRectFilled(ImVec2(right_x - w, y0), ImVec2(right_x, y0 + h), col(bg), Radius::R1);
        dl->AddText(f, px, ImVec2(std::round(right_x - px * 0.5f - tw), std::round(cy - px * 0.6f)), col(fg), buf);
        return w;
    }

    ImVec4 ocean(float t) {
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        const float f = t * 5.0f;
        const int i = std::min(4, static_cast<int>(f));
        const float fr = f - static_cast<float>(i);
        const ImVec4& a = Tokens::OCEAN[i];
        const ImVec4& b = Tokens::OCEAN[i + 1];
        return ImVec4(a.x + (b.x - a.x) * fr, a.y + (b.y - a.y) * fr, a.z + (b.z - a.z) * fr, 1.0f);
    }

    void depth_bar(ImDrawList* dl, ImVec2 min, ImVec2 max, const char* text, float frac, bool from_right, float text_y) {
        const float cell_w = max.x - min.x;
        const float tw = ImGui::CalcTextSize(text).x;
        const float inset = 5.0f;
        const float bar_w = std::min(cell_w, std::max(tw + inset * 2.0f, std::round(std::pow(frac, 0.6f) * cell_w)));
        const ImVec4 c = ocean(0.34f + 0.52f * frac);
        const ImVec2 a = from_right ? ImVec2(max.x - bar_w, min.y) : min;
        const ImVec2 b = from_right ? max : ImVec2(min.x + bar_w, max.y);
        dl->AddRectFilled(a, b, col(ImVec4(c.x, c.y, c.z, 0.34f + 0.36f * frac)), 3.0f);
        const float x = from_right ? max.x - inset - tw : min.x + inset;
        dl->AddText(ImVec2(std::round(x), text_y), col(frac >= 0.3f ? Tokens::TX1 : Tokens::TX2), text);
    }

    void info_hint(const char* text) {
        const float s = std::round(ImGui::GetFontSize() * 0.80f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float y = p.y + (ImGui::GetTextLineHeight() - s) * 0.5f;
        ImGui::Dummy(ImVec2(s + 2.0f, ImGui::GetTextLineHeight()));
        const bool hovered = ImGui::IsItemHovered();
        drawing::draw_ui_icon(ImGui::GetWindowDrawList(), drawing::UiIcon::Info, ImVec2(p.x + s * 0.5f + 1.0f, y + s * 0.5f),
                              s * 0.5f, col(hovered ? Tokens::TX1 : Tokens::TX3), 1.2f);
        if (hovered) {
            begin_tooltip();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 300.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            end_tooltip();
        }
    }

    void hairline(float width, float space) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, std::round(p.y + space * 0.5f) + 0.5f),
                                            ImVec2(p.x + width, std::round(p.y + space * 0.5f) + 0.5f), col(Tokens::BD1), 1.0f);
        ImGui::Dummy(ImVec2(width, space));
    }

    void section_label(const char* label) {
        ImGui::Spacing();
        ImGui::PushFont(Fonts::label());
        ImGui::TextColored(Tokens::TX3, "%s", label);
        ImGui::PopFont();
        ImGui::Spacing();
    }

    namespace {
        constexpr float kMenuRowH  = 30.0f;
        constexpr float kMenuPadX  = 10.0f;  // row inset for icon and hint
        constexpr float kMenuIconW = 16.0f;
        constexpr float kMenuGap   = 10.0f;  // icon to label
        constexpr float kHintGap   = 24.0f;  // label to hint, minimum
        constexpr float kLabelX    = kMenuPadX + kMenuIconW + kMenuGap;  // label column
        constexpr float kGroupWrap = 300.0f;  // text width inside an open group

        // The hover fill, hit box and natural width every row shares. The
        // natural width goes to ItemSize (so an auto-sizing popup fits its
        // widest row); the hit and hover box spans the whole menu.
        bool row_selectable(const char* label, float natural_w, float h, bool enabled,
                            ImGuiSelectableFlags flags) {
            ImGui::PushID(label);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
            // Rounded hover (Selectable fills square): drawn here, Selectable's own fill off.
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
            ImGui::BeginDisabled(!enabled);
            const bool clicked = ImGui::Selectable("##menu_row", false,
                flags | ImGuiSelectableFlags_SpanAvailWidth, ImVec2(natural_w, h));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                    u32(ImGui::IsItemActive() ? Tokens::HOVER : Tokens::ACTIVE), Radius::R2);
            ImGui::EndDisabled();
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();
            ImGui::PopID();
            return clicked;
        }

        // Wrapped text in a menu keeps the row inset on its right as well as
        // its left, and never pushes the menu wider than kGroupWrap needs.
        float wrap_width() {
            return std::min(kGroupWrap, std::max(160.0f, ImGui::GetContentRegionAvail().x - kMenuPadX));
        }

        float text_w(ImFont* font, const char* text) {
            if (!text || !*text) return 0.0f;
            ImGui::PushFont(font);
            const float w = ImGui::CalcTextSize(text, nullptr, true).x;
            ImGui::PopFont();
            return w;
        }
    }

    void float_shadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding) {
        dl->PushClipRectFullScreen();
        // Rings outside the border only (from 2 px out), deeper below than above: light from above.
        for (int i = 2; i <= 14; ++i) {
            const float k = static_cast<float>(i), f = 1.0f - k / 15.0f;
            dl->AddRect(ImVec2(a.x - k, a.y - k * 0.55f), ImVec2(b.x + k, b.y + k * 1.35f),
                        IM_COL32(0, 0, 0, static_cast<int>(255.0f * 0.22f * f * f)), rounding + k, 0, 1.6f);
        }
        dl->PopClipRect();
    }

    namespace {
        // Menus (James 2026-10-11: "think hierarchy and design"). Three levels, each with its own grammar:
        //   section  - a micro caps label (LIQUIDATIONS, PRICE LEVELS);
        //   row      - a layer or indicator: rounded check box, the name, a tag chip ("admin", "pilot");
        //   setting  - under its row while it is on: indented to the row's label column on a guide line from the
        //              row's check box, shorter rows, switches instead of check boxes, segmented choices, compact
        //              fields, muted notes.
        // The surface sits a step above the canvas with a soft shadow, so a menu reads as lifted off the chart.
        int g_menu_depth = 0;                 // > 0 inside a row's settings (detail) or an open group
        // Each open detail: its guide line (x, top), or, for the outermost one, the inset card drawn behind it once its
        // height is known (James 2026-10-15: "make it look more polished"; a layer's settings read as one block under
        // their row instead of loose rows on a guide line). The card is drawn into channel 0 of a split draw list after
        // the settings (channel 1), so it sits under them; one splitter, so only the outermost detail has a card.
        struct Detail { ImVec2 guide; float card_x0 = 0, card_x1 = 0; bool card = false; };
        ImVector<Detail> g_details;
        ImDrawListSplitter g_detail_splitter;
        bool g_card_open = false;
        constexpr float kCardPadY = 4.0f;     // inside the card, above the first setting and below the last
        constexpr float kSubRowH = 26.0f;
        constexpr float kSwitchW = 26.0f, kSwitchH = 14.0f;
        constexpr float kDetailIndent = kLabelX - 8.0f;  // a setting's label lines up with its row's label
        constexpr float kGroupIndent = 12.0f;            // an open group's settings, a step in from its disclosure
        const ImVec4& kMenuSurface = Tokens::MENU;  // per chrome preset

        void menu_shadow() {
            ImGuiWindow* w = ImGui::GetCurrentWindow();
            float_shadow(w->DrawList, w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y), Radius::R3);
        }

        void draw_switch(ImDrawList* dl, ImVec2 p, bool on, bool hovered) {
            const ImVec2 q(p.x + kSwitchW, p.y + kSwitchH);
            const float r = kSwitchH * 0.5f;
            dl->AddRectFilled(p, q, u32(on ? Tokens::BRAND : hovered ? Tokens::BD3 : Tokens::CONTROL_ACTIVE), r);
            const float kx = on ? q.x - r : p.x + r;
            dl->AddCircleFilled(ImVec2(kx, p.y + r), r - 2.5f, u32(on ? Tokens::BRAND_INK : Tokens::TX2), 16);
        }

        // The check box of a top-level row: rounded, filled with a check when on.
        void draw_check(ImDrawList* dl, ImVec2 c, bool on, bool hovered) {
            const ImVec2 b0(c.x - 7.0f, c.y - 7.0f), b1(c.x + 7.0f, c.y + 7.0f);
            if (on) {
                dl->AddRectFilled(b0, b1, u32(Tokens::BRAND), Radius::R1);
                drawing::draw_ui_icon(dl, drawing::UiIcon::Check, c, 5.0f, u32(Tokens::BRAND_INK), 1.6f);
            } else {
                dl->AddRectFilled(b0, b1, u32(Tokens::TRACK), Radius::R1);
                dl->AddRect(b0, b1, u32(hovered ? Tokens::BD3 : Tokens::BD2), Radius::R1, 0, 1.0f);
            }
        }
    }  // namespace

    ImVec2 tag_size(const char* tag) {
        ImGui::PushFont(Fonts::label());
        const ImVec2 t = ImGui::CalcTextSize(tag);
        ImGui::PopFont();
        return ImVec2(std::round(t.x + 10.0f), std::round(t.y + 5.0f));
    }

    // Who a row is for or why it is idle, as a quiet chip at the row's right end ("ADMIN", "PILOT"); also a hover's
    // qualifier (the rails hover's "PROVISIONAL").
    void draw_tag(ImDrawList* dl, ImVec2 right_mid, const char* tag, const ImVec4& fill) {
        char caps[32];
        size_t i = 0;
        for (; tag[i] && i + 1 < sizeof(caps); ++i) caps[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(tag[i])));
        caps[i] = 0;
        const ImVec2 sz = tag_size(caps);
        const ImVec2 p(right_mid.x - sz.x, std::round(right_mid.y - sz.y * 0.5f));
        dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), u32(fill), Radius::R1);
        ImGui::PushFont(Fonts::label());
        dl->AddText(ImVec2(p.x + 5.0f, p.y + 2.5f), u32(Tokens::TX3), caps);
        ImGui::PopFont();
    }

    namespace {
        // A setting under a row: label, optional mono hint, a switch at the right.
        bool sub_switch(const char* label, bool on, const char* hint) {
            const float label_w = text_w(Fonts::ui(), label);
            const float hint_w = text_w(Fonts::mono_sm(), hint);
            const float natural = 8.0f + label_w + (hint_w > 0.0f ? 12.0f + hint_w : 0.0f) + 16.0f + kSwitchW + kMenuPadX;
            const bool clicked = row_selectable(label, natural, kSubRowH, true, ImGuiSelectableFlags_NoAutoClosePopups);
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const float cy = std::round((a.y + b.y) * 0.5f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImGui::PushFont(Fonts::ui());
            // Settings read a step quieter than their row; the switch carries the state.
            dl->AddText(ImVec2(a.x + 8.0f, cy - ImGui::GetFontSize() * 0.5f), u32(hovered ? Tokens::TX1 : Tokens::TX2),
                        label, ImGui::FindRenderedTextEnd(label));
            ImGui::PopFont();
            const ImVec2 s(b.x - kMenuPadX - kSwitchW, cy - kSwitchH * 0.5f);
            if (hint_w > 0.0f) {
                ImGui::PushFont(Fonts::mono_sm());
                dl->AddText(ImVec2(s.x - 10.0f - hint_w, cy - ImGui::GetFontSize() * 0.5f), u32(Tokens::TX3), hint);
                ImGui::PopFont();
            }
            draw_switch(dl, s, on, hovered);
            return clicked;
        }

        bool toggle_row(const char* label, bool on, bool locked, const char* hint, float trailing_w, bool tag) {
            if (g_menu_depth > 0 && !locked && trailing_w <= 0.0f) return sub_switch(label, on, hint);
            const float tag_w = tag && hint && !locked ? tag_size(hint).x : 0.0f;
            const float trailing = locked ? pro_tag_size().x : std::max(trailing_w, tag_w);
            ImGui::PushStyleColor(ImGuiCol_Text, locked ? Tokens::TX3 : (on ? Tokens::TX1 : Tokens::TX2));
            const MenuRow r = menu_row(label, locked || tag ? nullptr : hint, true, /*keep_open=*/true, trailing);
            ImGui::PopStyleColor();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (locked) {
                drawing::draw_ui_icon(dl, drawing::UiIcon::Lock, r.icon, 6.0f,
                                      u32(r.hovered ? Tokens::TX2 : Tokens::TX3), 1.3f);
                const ImVec2 t = pro_tag_size();
                draw_pro_tag(dl, ImVec2(r.max.x - kMenuPadX - t.x, (r.min.y + r.max.y - t.y) * 0.5f));
            } else {
                draw_check(dl, r.icon, on, r.hovered);
                if (tag_w > 0.0f) draw_tag(dl, ImVec2(r.max.x - kMenuPadX, (r.min.y + r.max.y) * 0.5f), hint);
            }
            return r.clicked;
        }
    }

    bool begin_menu(const char* id, float min_width, float max_height) {
        // Every value below is latched by BeginPopup, so all of it pops
        // straight away whether or not the popup is open.
        ImGui::SetNextWindowSizeConstraints(ImVec2(min_width, 0.0f), ImVec2(FLT_MAX, max_height));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Radius::R3);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, kMenuSurface);
        ImGui::PushStyleColor(ImGuiCol_Border, Tokens::BD2);
        const bool open = ImGui::BeginPopup(id);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
        if (open) {
            menu_shadow();
            g_menu_depth = 0;
            g_details.resize(0);
            // A detail left open by a menu that returned early: forget its split (that draw list is gone) so the next
            // card can split again.
            if (g_card_open) { g_detail_splitter.Clear(); g_card_open = false; }
        }
        return open;
    }

    MenuRow menu_row(const char* label, const char* hint, bool enabled, bool keep_open,
                     float trailing_w) {
        const float label_w = text_w(Fonts::ui(), label);
        const float hint_w = text_w(Fonts::mono_sm(), hint);
        const float trail = trailing_w > 0.0f ? trailing_w + 8.0f : 0.0f;
        const float natural = kLabelX + label_w + (hint_w > 0.0f ? kHintGap + hint_w : 0.0f) +
                              trail + kMenuPadX;
        const ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);

        MenuRow r;
        ImGuiSelectableFlags flags = 0;
        if (keep_open) flags |= ImGuiSelectableFlags_NoAutoClosePopups;
        if (trailing_w > 0.0f) flags |= ImGuiSelectableFlags_AllowOverlap;
        r.clicked = row_selectable(label, natural, kMenuRowH, enabled, flags);
        r.hovered = enabled && ImGui::IsItemHovered();
        r.right_clicked = r.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        r.min = ImGui::GetItemRectMin();
        r.max = ImGui::GetItemRectMax();
        const float cy = (r.min.y + r.max.y) * 0.5f;
        r.icon = ImVec2(r.min.x + kMenuPadX + kMenuIconW * 0.5f, cy);
        // The icon follows the label's colour (a warning row stays amber),
        // muted until the row is hovered.
        r.icon_col = !enabled ? u32(Tokens::TX4) : u32(text, r.hovered ? 1.0f : 0.6f);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::PushFont(Fonts::ui());
        const float lx = r.min.x + kLabelX;
        dl->AddText(ImVec2(lx, cy - ImGui::GetFontSize() * 0.5f), u32(enabled ? text : Tokens::TX4),
                    label, ImGui::FindRenderedTextEnd(label));
        ImGui::PopFont();
        if (hint_w > 0.0f) {
            ImGui::PushFont(Fonts::mono_sm());
            const float hx = std::max(lx + label_w + kHintGap, r.max.x - kMenuPadX - trail - hint_w);
            dl->AddText(ImVec2(hx, cy - ImGui::GetFontSize() * 0.5f),
                        u32(enabled ? Tokens::TX3 : Tokens::TX4), hint);
            ImGui::PopFont();
        }
        return r;
    }

    bool menu_item(drawing::UiIcon icon, const char* label, const char* hint, bool enabled,
                   bool keep_open) {
        if (g_menu_depth > 0) {
            // A setting's action (Depth settings..., Daily record): a short row, the icon after the label column.
            const float label_w = text_w(Fonts::ui(), label);
            const float hint_w = text_w(Fonts::mono_sm(), hint);
            const float natural = 8.0f + 18.0f + label_w + (hint_w > 0.0f ? kHintGap + hint_w : 0.0f) + kMenuPadX;
            const bool clicked = row_selectable(label, natural, kSubRowH, enabled,
                                                keep_open ? ImGuiSelectableFlags_NoAutoClosePopups : 0);
            const bool hovered = enabled && ImGui::IsItemHovered();
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const float cy = std::round((a.y + b.y) * 0.5f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 c = u32(!enabled ? Tokens::TX4 : hovered ? Tokens::TX1 : Tokens::TX2);
            drawing::draw_ui_icon(dl, icon, ImVec2(a.x + 8.0f + 6.0f, cy), 5.5f, c, 1.3f);
            ImGui::PushFont(Fonts::ui());
            dl->AddText(ImVec2(a.x + 8.0f + 18.0f, cy - ImGui::GetFontSize() * 0.5f), c, label, ImGui::FindRenderedTextEnd(label));
            ImGui::PopFont();
            if (hint_w > 0.0f) {
                ImGui::PushFont(Fonts::mono_sm());
                dl->AddText(ImVec2(b.x - kMenuPadX - hint_w, cy - ImGui::GetFontSize() * 0.5f), u32(Tokens::TX3), hint);
                ImGui::PopFont();
            }
            return clicked;
        }
        const MenuRow r = menu_row(label, hint, enabled, keep_open);
        drawing::draw_ui_icon(ImGui::GetWindowDrawList(), icon, r.icon, 6.5f, r.icon_col, 1.4f);
        return r.clicked;
    }

    ImVec2 pro_tag_size() {
        ImGui::PushFont(Fonts::label());
        const ImVec2 t = ImGui::CalcTextSize("PRO");
        ImGui::PopFont();
        return ImVec2(t.x + 12.0f, t.y + 5.0f);
    }

    void draw_pro_tag(ImDrawList* dl, ImVec2 p) {
        const ImVec2 sz = pro_tag_size();
        dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), u32(Tokens::LOGO, 0.12f), Radius::R1);
        dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), u32(Tokens::LOGO, 0.55f), Radius::R1, 0, 1.0f);
        ImGui::PushFont(Fonts::label());
        dl->AddText(ImVec2(p.x + 6.0f, p.y + 2.5f), u32(Tokens::LOGO), "PRO");
        ImGui::PopFont();
    }

    bool menu_toggle(const char* label, bool on, bool locked, const char* hint, float trailing_w) {
        return toggle_row(label, on, locked, hint, trailing_w, false);
    }

    bool menu_layer(const char* label, bool on, bool locked, const char* tag, bool* settings) {
        if (!settings || locked) {
            const bool clicked = toggle_row(label, on, locked, tag, 0.0f, true);
            ui_test::mark(std::string("layer.") + label);
            return clicked;
        }
        *settings = false;
        const float tag_w = tag ? tag_size(tag).x + 8.0f : 0.0f;
        ImGui::PushStyleColor(ImGuiCol_Text, on ? Tokens::TX1 : Tokens::TX2);
        const MenuRow r = menu_row(label, nullptr, true, true, 30.0f + tag_w);
        ImGui::PopStyleColor();
        ui_test::mark(std::string("layer.") + label);
        auto* dl = ImGui::GetWindowDrawList();
        draw_check(dl, r.icon, on, r.hovered);
        if (tag) draw_tag(dl, ImVec2(r.max.x - kMenuPadX - 30.0f, r.icon.y), tag);
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const ImVec2 at(r.max.x - kMenuPadX - 26.0f, r.min.y + 2.0f);
        ImGui::PushID(label);
        ImGui::SetCursorScreenPos(at);
        const bool clicked = ImGui::InvisibleButton("settings", ImVec2(26.0f, 26.0f));
        ui_test::mark(std::string("layer.") + label + ".settings");
        const bool hot = ImGui::IsItemHovered();
        if (hot) dl->AddRectFilled(at, ImVec2(at.x + 26, at.y + 26), u32(Tokens::HOVER), Radius::R1);
        drawing::draw_ui_icon(dl, drawing::UiIcon::ChevronRight, ImVec2(at.x + 13, at.y + 13), 5,
                             u32(hot ? Tokens::TX1 : Tokens::TX3), 1.3f);
        if (hot) tooltip("%s settings", label);
        ImGui::PopID();
        ImGui::SetCursorScreenPos(cursor);
        *settings = clicked || r.right_clicked;
        return r.clicked && !hot && !*settings;
    }

    bool menu_item_locked(const char* label, const char* reason, bool pro, bool enabled) {
        constexpr float kH = 46.0f;
        const float label_w = text_w(Fonts::ui(), label);
        const float reason_w = text_w(Fonts::label(), reason);
        const float tag_w = pro ? pro_tag_size().x + kHintGap : 0.0f;
        const float natural = kLabelX + std::max(label_w + tag_w, reason_w) + kMenuPadX;
        const bool clicked = row_selectable(label, natural, kH, enabled, 0);
        const bool hot = enabled && ImGui::IsItemHovered();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::PushFont(Fonts::ui());
        const float fs = ImGui::GetFontSize();
        const float y1 = a.y + 7.0f;
        // The lock sits on the action's own line: this is the row a Pro viewer
        // clicks, shown with why it is closed rather than as a separate offer.
        drawing::draw_ui_icon(dl, drawing::UiIcon::Lock, ImVec2(a.x + kMenuPadX + kMenuIconW * 0.5f, y1 + fs * 0.5f),
                              6.0f, u32(pro ? Tokens::LOGO : Tokens::TX3, hot || !pro ? 1.0f : 0.85f), 1.3f);
        dl->AddText(ImVec2(a.x + kLabelX, y1), u32(enabled ? Tokens::TX1 : Tokens::TX3), label);
        ImGui::PopFont();
        if (reason && *reason) {
            ImGui::PushFont(Fonts::label());
            dl->AddText(ImVec2(a.x + kLabelX, y1 + fs + 4.0f), u32(Tokens::TX3), reason);
            ImGui::PopFont();
        }
        if (pro) {
            const ImVec2 tag = pro_tag_size();
            draw_pro_tag(dl, ImVec2(b.x - kMenuPadX - tag.x, y1 + (fs - tag.y) * 0.5f));
        }
        return clicked;
    }

    bool begin_menu_group(const char* label) {
        ImGuiStorage* storage = ImGui::GetStateStorage();
        const ImGuiID id = ImGui::GetID(label);
        bool open = storage->GetBool(id, false);
        if (g_menu_depth > 0) {
            // Inside a row's settings: a quiet disclosure ("Options" and a chevron), its content at the same indent.
            const char* end = ImGui::FindRenderedTextEnd(label);
            const float label_w = text_w(Fonts::ui(), label);
            const bool clicked = row_selectable(label, 8.0f + label_w + 22.0f + kMenuPadX, kSubRowH, true,
                                                ImGuiSelectableFlags_NoAutoClosePopups);
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const float cy = std::round((a.y + b.y) * 0.5f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 c = u32(hovered || open ? Tokens::TX2 : Tokens::TX3);
            ImGui::PushFont(Fonts::ui());
            dl->AddText(ImVec2(a.x + 8.0f, cy - ImGui::GetFontSize() * 0.5f), c, label, end);
            ImGui::PopFont();
            drawing::draw_ui_icon(dl, open ? drawing::UiIcon::ChevronDown : drawing::UiIcon::ChevronRight,
                                  ImVec2(a.x + 8.0f + label_w + 10.0f, cy), 4.0f, c, 1.4f);
            (void)b;
            if (clicked) { open = !open; storage->SetBool(id, open); }
            if (!open) return false;
            ++g_menu_depth;
            ImGui::Indent(kGroupIndent);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width() - kGroupIndent);
            return true;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
        const MenuRow r = menu_row(label, nullptr, true, /*keep_open=*/true);
        ImGui::PopStyleColor();
        if (r.clicked) { open = !open; storage->SetBool(id, open); }
        drawing::draw_ui_icon(ImGui::GetWindowDrawList(),
                              open ? drawing::UiIcon::ChevronDown : drawing::UiIcon::ChevronRight,
                              r.icon, 5.0f, r.icon_col, 1.4f);
        if (!open) return false;
        ++g_menu_depth;
        ImGui::Indent(kDetailIndent);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width());
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        return true;
    }

    void end_menu_group() {
        g_menu_depth = std::max(0, g_menu_depth - 1);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::PopTextWrapPos();
        ImGui::Unindent(g_menu_depth == 0 ? kDetailIndent : kGroupIndent);
    }

    void begin_menu_detail() {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        Detail d;
        d.guide = ImVec2(std::round(p.x + kMenuPadX + kMenuIconW * 0.5f) + 0.5f, p.y);
        if (!g_card_open) {
            // The card spans the settings' rows (their hover fills it edge to edge), from a step right of the row's
            // check box to the menu's right edge; the settings' labels keep the row label's column inside it.
            ImDrawList* dl = ImGui::GetWindowDrawList();
            d.card = true;
            d.card_x0 = std::round(p.x + kDetailIndent);
            d.card_x1 = std::round(p.x + ImGui::GetContentRegionAvail().x);
            g_detail_splitter.Split(dl, 2);
            g_detail_splitter.SetCurrentChannel(dl, 1);
            g_card_open = true;
        }
        g_details.push_back(d);
        ++g_menu_depth;
        ImGui::Indent(kDetailIndent);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width());
        if (d.card) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + kCardPadY);
    }

    void end_menu_detail() {
        const bool card = !g_details.empty() && g_details.back().card;
        // The last setting's bottom, whatever spacing it left after itself.
        const float last = ImGui::GetItemRectMax().y;
        if (!card) ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::PopTextWrapPos();
        ImGui::Unindent(kDetailIndent);
        g_menu_depth = std::max(0, g_menu_depth - 1);
        if (g_details.empty()) return;
        const Detail d = g_details.back();
        g_details.pop_back();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (d.card) {
            // Behind the settings: a fill a step above the menu with a hairline edge, rounded as the rows' hover. The
            // next row starts one item spacing under it.
            const float bottom = std::max(last, d.guide.y + 4.0f) + kCardPadY;
            g_detail_splitter.SetCurrentChannel(dl, 0);
            const ImVec2 a(d.card_x0, d.guide.y), b(d.card_x1, bottom);
            dl->AddRectFilled(a, b, u32(ImLerp(Tokens::MENU, Tokens::HOVER, 0.42f)), Radius::R2);
            dl->AddRect(a, b, u32(Tokens::BD1), Radius::R2, 0, 1.0f);
            g_detail_splitter.Merge(dl);
            g_card_open = false;
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, bottom));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            return;
        }
        // A detail inside a card: the guide, from just under the row's check box to the last setting.
        const float bottom = ImGui::GetCursorScreenPos().y - 8.0f;
        if (bottom > d.guide.y + 4.0f)
            dl->AddLine(ImVec2(d.guide.x, d.guide.y - 6.0f), ImVec2(d.guide.x, bottom), u32(Tokens::BD2), 1.0f);
    }

    int menu_choice(const char* id, const char* const* labels, int n, int selected) {
        // A segmented control across the content width: a rounded track, the selected option raised.
        ImGui::PushID(id);
        const float lead = g_menu_depth > 0 ? 8.0f : kMenuPadX;
        const ImVec2 c = ImGui::GetCursorScreenPos();
        const ImVec2 p(c.x + lead, c.y + 2.0f);
        const float w = std::max(56.0f * n, ImGui::GetContentRegionAvail().x - lead - kMenuPadX);
        constexpr float h = 26.0f, inset = 3.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), u32(Tokens::TRACK), Radius::R2 + 1.0f);
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), u32(Tokens::BD1), Radius::R2 + 1.0f);
        const float seg = (w - inset * 2.0f) / float(std::max(1, n));
        int out = selected;
        for (int i = 0; i < n; ++i) {
            const float x = p.x + inset + seg * float(i);
            ImGui::SetCursorScreenPos(ImVec2(x, p.y));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##choice", ImVec2(seg, h))) out = i;
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (ui_test::enabled()) ui_test::mark(std::string(id) + "." + std::string(labels[i], ImGui::FindRenderedTextEnd(labels[i])));
            if (i == selected)
                dl->AddRectFilled(ImVec2(x, p.y + inset), ImVec2(x + seg, p.y + h - inset), u32(Tokens::CONTROL_ACTIVE), Radius::R2);
            else if (hovered)
                dl->AddRectFilled(ImVec2(x, p.y + inset), ImVec2(x + seg, p.y + h - inset), u32(Tokens::CONTROL), Radius::R2);
            const char* end = ImGui::FindRenderedTextEnd(labels[i]);
            const ImVec2 ts = ImGui::CalcTextSize(labels[i], end);
            dl->AddText(ImVec2(std::round(x + (seg - ts.x) * 0.5f), std::round(p.y + (h - ts.y) * 0.5f)),
                        u32(i == selected ? Tokens::TX1 : hovered ? Tokens::TX2 : Tokens::TX3), labels[i], end);
        }
        ImGui::SetCursorScreenPos(c);
        ImGui::Dummy(ImVec2(lead + w, h + 6.0f));
        ImGui::PopID();
        return out;
    }

    int menu_choice_row(const char* label, const char* id, const char* const* labels, int n, int selected) {
        // A setting with a few values: the label, a compact segmented control at the right.
        ImGui::PushID(id);
        const ImVec2 c = ImGui::GetCursorScreenPos();
        constexpr float h = 22.0f, inset = 2.0f;
        ImGui::PushFont(Fonts::ui());
        const float label_w = ImGui::CalcTextSize(label, nullptr, true).x;
        float widths[8], total = inset * 2.0f;
        n = std::min(n, 8);
        for (int i = 0; i < n; ++i) { widths[i] = ImGui::CalcTextSize(labels[i], nullptr, true).x + 16.0f; total += widths[i]; }
        const float avail = ImGui::GetContentRegionAvail().x;
        const float right = c.x + std::max(avail - kMenuPadX, 8.0f + label_w + 16.0f + total);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cy = std::round(c.y + kSubRowH * 0.5f);
        dl->AddText(ImVec2(c.x + 8.0f, cy - ImGui::GetFontSize() * 0.5f), u32(Tokens::TX2), label, ImGui::FindRenderedTextEnd(label));
        const ImVec2 p(right - total, cy - h * 0.5f);
        dl->AddRectFilled(p, ImVec2(p.x + total, p.y + h), u32(Tokens::TRACK), Radius::R2);
        dl->AddRect(p, ImVec2(p.x + total, p.y + h), u32(Tokens::BD1), Radius::R2);
        int out = selected;
        float x = p.x + inset;
        for (int i = 0; i < n; ++i) {
            ImGui::SetCursorScreenPos(ImVec2(x, p.y));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##seg", ImVec2(widths[i], h))) out = i;
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (ui_test::enabled()) ui_test::mark(std::string(id) + "." + std::string(labels[i], ImGui::FindRenderedTextEnd(labels[i])));
            if (i == selected)
                dl->AddRectFilled(ImVec2(x, p.y + inset), ImVec2(x + widths[i], p.y + h - inset), u32(Tokens::CONTROL_ACTIVE), Radius::R1);
            else if (hovered)
                dl->AddRectFilled(ImVec2(x, p.y + inset), ImVec2(x + widths[i], p.y + h - inset), u32(Tokens::CONTROL), Radius::R1);
            const char* end = ImGui::FindRenderedTextEnd(labels[i]);
            const ImVec2 ts = ImGui::CalcTextSize(labels[i], end);
            dl->AddText(ImVec2(std::round(x + (widths[i] - ts.x) * 0.5f), std::round(cy - ts.y * 0.5f)),
                        u32(i == selected ? Tokens::TX1 : hovered ? Tokens::TX2 : Tokens::TX3), labels[i], end);
            x += widths[i];
        }
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(c);
        ImGui::Dummy(ImVec2(right - c.x, kSubRowH));
        ImGui::PopID();
        return out;
    }

    bool menu_field(const char* label, float* value, const char* format, float step, float field_w) {
        // A setting with a number: the label, a compact rounded field at the right (Enter or click away commits).
        ImGui::PushID(label);
        const ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::PushFont(Fonts::ui());
        const float label_w = ImGui::CalcTextSize(label, nullptr, true).x;
        const float avail = ImGui::GetContentRegionAvail().x;
        const float right = c.x + std::max(avail - kMenuPadX, 8.0f + label_w + 16.0f + field_w);
        const float cy = std::round(c.y + kSubRowH * 0.5f);
        ImGui::GetWindowDrawList()->AddText(ImVec2(c.x + 8.0f, cy - ImGui::GetFontSize() * 0.5f), u32(Tokens::TX2),
                                            label, ImGui::FindRenderedTextEnd(label));
        ImGui::PopFont();
        ImGui::PushFont(Fonts::mono());
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 3.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Radius::R2);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Tokens::TRACK);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Tokens::CONTROL);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Tokens::CONTROL);
        ImGui::PushStyleColor(ImGuiCol_Border, Tokens::BD1);
        const float fh = ImGui::GetFrameHeight();
        ImGui::SetCursorScreenPos(ImVec2(right - field_w, cy - fh * 0.5f));
        ImGui::SetNextItemWidth(field_w);
        const bool changed = ImGui::InputFloat("##field", value, step, 0.0f, format,
                                               ImGuiInputTextFlags_CharsScientific | ImGuiInputTextFlags_AutoSelectAll);
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(3);
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(c);
        ImGui::Dummy(ImVec2(right - c.x, kSubRowH));
        ImGui::PopID();
        return changed;
    }

    void menu_note(const char* text) {
        const float lead = g_menu_depth > 0 ? 8.0f : kMenuPadX;
        ImGui::Indent(lead);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width() - lead);
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX3);
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::TextWrapped("%s", text);
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::Unindent(lead);
    }

    void menu_section(const char* label) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushFont(Fonts::label());
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + kMenuPadX, p.y + 9.0f),
                                            u32(Tokens::TX3), label);
        const ImVec2 size(ImGui::CalcTextSize(label).x + kMenuPadX * 2.0f,
                          ImGui::GetFontSize() + 15.0f);
        ImGui::PopFont();
        ImGui::Dummy(size);
    }

    void menu_separator() {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 4.0f, p.y + 5.5f),
                                            ImVec2(p.x + w - 4.0f, p.y + 5.5f),
                                            u32(Tokens::BD1), 1.0f);
        ImGui::Dummy(ImVec2(1.0f, 11.0f));
    }

    namespace { ImVector<ImVec4> g_chart_chips; }
    void chart_chips_reset() { g_chart_chips.resize(0); }
    void chart_chip(ImVec2 a, ImVec2 b) { g_chart_chips.push_back(ImVec4(a.x, a.y, b.x, b.y)); }
    bool chart_chip_covers(ImVec2 p, float margin) {
        for (const ImVec4& r : g_chart_chips)
            if (p.x >= r.x - margin && p.x <= r.z + margin && p.y >= r.y - margin && p.y <= r.w + margin) return true;
        return false;
    }

    void spinner(ImDrawList* dl, ImVec2 c, float r, float thickness) {
        const float t = static_cast<float>(ImGui::GetTime()) * 3.2f;
        dl->AddCircle(c, r, u32(Tokens::LOGO, 0.16f), 48, thickness);
        dl->PathClear();
        dl->PathArcTo(c, r, t, t + 4.2f, 48);
        dl->PathStroke(u32(Tokens::LOGO), 0, thickness);
    }

    // Tooltips (design v3, 2026-10-11): a raised surface (#141414, clear of the near-black canvas) with a hairline
    // border, roomy padding, a little line spacing, TX1 body and TX3 for disabled (secondary) lines, wrapped at a
    // readable width.
    void begin_tooltip(bool replace_previous) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 9.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 4.0f));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Tokens::HOVER);
        ImGui::PushStyleColor(ImGuiCol_Border, Tokens::BD2);
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX1);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, Tokens::TX3);
        ImGui::BeginTooltipEx(replace_previous ? ImGuiTooltipFlags_OverridePrevious : ImGuiTooltipFlags_None, ImGuiWindowFlags_None);
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
    }

    void end_tooltip() {
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(4);
    }

    void tooltip(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        begin_tooltip();
        ImGui::TextWrappedV(fmt, args);
        end_tooltip();
        va_end(args);
    }

    bool load_fonts() {
        ImGuiIO& io = ImGui::GetIO();

        float dpi_scale = 1.0f;
#ifdef __EMSCRIPTEN__
        dpi_scale = EM_ASM_DOUBLE({ return window.devicePixelRatio || 1.0; });
#endif
        // Rasterize at a minimum of 2x regardless of devicePixelRatio -
        // on 1x displays this supersamples the atlas for crisper glyphs.
        const float raster_scale = dpi_scale < 2.0f ? 2.0f : dpi_scale;

        ImFontConfig cfg;
        cfg.OversampleH = 3;
        cfg.OversampleV = 2;
        // Preserve fractional glyph advances in the supersampled UI atlas.
        cfg.PixelSnapH  = false;

        // Default latin range + Δ (U+0394, DOM delta column) + … (U+2026)
        static const ImWchar glyph_ranges[] = {
            0x0020, 0x00FF,  // Basic Latin + Latin Supplement
            0x0394, 0x0394,  // Greek capital delta
            0x2026, 0x2026,  // horizontal ellipsis
            0,
        };

        io.Fonts->Clear();

        // ui_scale - single proportional knob for the whole terminal's logical
        // type size. This is the dial to tune readability; it is ORTHOGONAL to
        // raster_scale (which only governs glyph crispness via supersampling).
        // 1.0 = the original base sizes; 1.15 = ~15% larger across every face,
        // keeping the type hierarchy intact. Applied to logical_px BEFORE the
        // raster multiply, so atlas crispness is unaffected.
        constexpr float ui_scale = 1.15f;

        // CJK fallback. Binance lists meme perps whose ticker is Chinese
        // (today 龙虾USDT and 牛来USDT). Neither Hanken Grotesk nor JetBrains
        // Mono carries a CJK glyph, so those names drew as ImGui's "?" fallback
        // wherever a symbol is written: watchlist rows, the symbol picker, the
        // pair pill. Merged as a SECOND SOURCE on every face rather than kept
        // as a separate font, so callers that already picked a face get the
        // glyph without having to know they needed one. The atlas is dynamic
        // (the GL backend sets ImGuiBackendFlags_RendererHasTextures), so an
        // unused source costs nothing until a glyph from it is drawn.
        //
        // The file is subset to the codepoints the live universe actually
        // lists - 4 of them, 2 KB - because this is preloaded into index.data
        // on every cold boot and a general Chinese font is ~780 KB even cut
        // down to GB2312 level 1. scripts/gen-cjk-subset.sh regenerates it; a
        // ticker listed after that run reverts to "?" until it is rerun.
        static const ImWchar cjk_ranges[] = {
            0x2E80, 0x9FFF,  // CJK radicals through unified ideographs
            0,
        };
        ImFontConfig cjk_cfg = cfg;
        cjk_cfg.MergeMode = true;

        auto add = [&](const char* path, float logical_px) -> ImFont* {
            const float px = logical_px * ui_scale * raster_scale;
            ImFont* f = io.Fonts->AddFontFromFileTTF(path, px, &cfg, glyph_ranges);
            if (f) {
                io.Fonts->AddFontFromFileTTF("/fonts/NotoSansCJK-Subset.ttf", px,
                                             &cjk_cfg, cjk_ranges);
            }
            return f;
        };

        // UI face - Inter (chrome, labels, headings)
        f_ui          = add("/fonts/Inter-400.ttf",  16.0f);
        f_ui_semibold = add("/fonts/Inter-600.ttf", 16.0f);
        f_ui_sm       = add("/fonts/Inter-400.ttf", 15.0f);
        f_heading     = add("/fonts/Inter-600.ttf", 18.0f);
        f_label       = add("/fonts/Inter-600.ttf", 11.5f);

        // Numeric face - Roboto Mono (every table/ladder/clock/price).
        // Base logical sizes; ui_scale (above) applies the global multiplier so
        // the dense DOM ladder / tape / depth widgets scale with everything else.
        // ImGui tables auto-size rows to the font, so row heights track this.
        f_mono_xs     = add("/fonts/RobotoMono-500.ttf",  9.5f);
        f_mono_sm     = add("/fonts/RobotoMono-400.ttf",  15.0f);
        f_mono        = add("/fonts/RobotoMono-400.ttf",  16.0f);
        f_mono_md     = add("/fonts/RobotoMono-500.ttf",   17.0f);
        f_mono_lg     = add("/fonts/RobotoMono-500.ttf", 23.0f);

        if (!f_ui) f_ui = io.Fonts->AddFontDefault();
        if (!f_ui_semibold) f_ui_semibold = f_ui;
        if (!f_ui_sm)       f_ui_sm       = f_ui;
        if (!f_heading)     f_heading     = f_ui_semibold;
        if (!f_label)       f_label       = f_ui_semibold;
        if (!f_mono)        f_mono        = f_ui;
        if (!f_mono_xs)     f_mono_xs     = f_mono;
        if (!f_mono_sm)     f_mono_sm     = f_mono;
        if (!f_mono_md)     f_mono_md     = f_mono;
        if (!f_mono_lg)     f_mono_lg     = f_mono_md;

        io.FontDefault = f_ui;

        // Fonts rasterize at logical_px * raster_scale for crispness; ImGui
        // works in logical pixels, so scale back down by the same factor.
        io.FontGlobalScale = 1.0f / raster_scale;

        return f_ui != nullptr;
    }

    namespace Fonts {
        ImFont* ui_sm()       { return f_ui_sm; }
        ImFont* ui()          { return f_ui; }
        ImFont* ui_semibold() { return f_ui_semibold; }
        ImFont* heading()     { return f_heading; }
        ImFont* label()       { return f_label; }
        ImFont* mono_xs()     { return f_mono_xs; }
        ImFont* mono_sm()     { return f_mono_sm; }
        ImFont* mono()        { return f_mono; }
        ImFont* mono_md()     { return f_mono_md; }
        ImFont* mono_lg()     { return f_mono_lg; }
    }

    ImFont* get_regular_font() { return f_ui; }
    ImFont* get_bold_font()    { return f_ui_semibold; }
    ImFont* get_large_font()   { return f_mono_md; }

    ImVec4 get_buy_color(float alpha) {
        return ImVec4(Tokens::UP.x, Tokens::UP.y, Tokens::UP.z, alpha);
    }

    ImVec4 get_sell_color(float alpha) {
        return ImVec4(Tokens::DOWN.x, Tokens::DOWN.y, Tokens::DOWN.z, alpha);
    }

    ImU32 get_buy_color_u32(uint8_t alpha) {
        const ImVec4& c = Tokens::UP;
        return IM_COL32(static_cast<int>(c.x * 255.0f),
                        static_cast<int>(c.y * 255.0f),
                        static_cast<int>(c.z * 255.0f), alpha);
    }

    ImU32 get_sell_color_u32(uint8_t alpha) {
        const ImVec4& c = Tokens::DOWN;
        return IM_COL32(static_cast<int>(c.x * 255.0f),
                        static_cast<int>(c.y * 255.0f),
                        static_cast<int>(c.z * 255.0f), alpha);
    }
}
