#include "rendering/theme.h"
#include "ui/drawing/drawing_icons.h"
#include "imgui_internal.h"   // ImGuiSelectableFlags_SpanAvailWidth
#include <cfloat>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include "core/appearance_preferences.h"
#include "core/heatmap_colormap.h"
#include "core/workspace_settings.h"
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
        // every surface, hairline and text token as 0xRRGGBB; BD3, GRID (BD1's hue) and TX4 keep alphas.
        // To add a preset: append a Chrome value, a row here and its label in kChromeNames. Brand (LOGO, PRO tags),
        // signed market colors, WARN and the heatmap ramps are not chrome and stay fixed.
        struct ChromeRamp {
            uint32_t base, panel, elev, input, hover, active;
            uint32_t bd1, bd2, bd3, tx1, tx2, tx3, tx4;
            float bd3_a, grid_a, tx4_a;
        };
        constexpr ChromeRamp kChrome[] = {
            // EdgeDepth: the v2 token sheet (default; pixels unchanged).
            {0x050505, 0x080808, 0x0d0d0d, 0x0b0b0b, 0x141414, 0x1b1b1b,
             0x1c1c1c, 0x292929, 0x737373, 0xe8e8e8, 0xb0b0b0, 0x858585, 0x737373, 0.55f, 0.55f, 0.72f},
            // Graphite: lifted neutral greys, softer hairlines.
            {0x111113, 0x161618, 0x1b1b1e, 0x19191b, 0x232326, 0x2a2a2e,
             0x252528, 0x313135, 0x7e7e84, 0xe8e8ea, 0xb8b8bc, 0x929297, 0x808085, 0.45f, 0.50f, 0.75f},
            // Midnight: the marketing site's blue-black (--bg-0 #0b0e13, --line-1/2, green-neutral text).
            {0x0b0e13, 0x0e1218, 0x11161d, 0x0f131a, 0x161b24, 0x1c2230,
             0x1e242e, 0x2a3140, 0x66736e, 0xecf0ef, 0xadb9b4, 0x85918c, 0x717d79, 0.60f, 0.55f, 0.75f},
            // High contrast: black base, brighter text ramp and hairlines (WCAG: TX3 >= 9:1 and TX4 >= 6:1 on resting
            // surfaces, BD2 >= 3:1 on panels; EdgeDepth gives 4.9:1 and 2.6:1).
            {0x000000, 0x030303, 0x0b0b0b, 0x0a0a0a, 0x1a1a1a, 0x262626,
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
    // v1 is left in place for an older build. The parsing is in
    // core/appearance_preferences.h, with a native test.
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

        appearance::Preferences current_preferences() {
            appearance::Preferences p;
            p.chrome = int(g_chrome); p.accent = int(g_accent); p.candles = int(g_candles);
            p.up = g_custom_up; p.down = g_custom_down; p.liq_map = int(HeatmapColormap::liq_map());
            return p;
        }
    }

    void load_preferences() {
        appearance::Preferences p = current_preferences();
        const appearance::Limits limits{int(std::size(kChrome)) - 1, int(Accent::Mono),
            int(CandleConvention::BlueOrange), int(HeatmapColormap::LiqMap::Inferno)};
        const auto source = appearance::read(read_storage("edgedepth.appearance.v2"),
                                             read_storage("edgedepth.appearance.v1"), limits, p);
        if (source == appearance::Source::None) return;
        g_custom_up = p.up; g_custom_down = p.down;
        set_chrome(Chrome(p.chrome));
        set_accent(Accent(p.accent));
        set_candle_convention(CandleConvention(p.candles));
        if (p.liq_map != int(HeatmapColormap::liq_map())) HeatmapColormap::set_liq_map(HeatmapColormap::LiqMap(p.liq_map));
        if (source == appearance::Source::V1) save_preferences();
    }

    void save_preferences() {
#ifdef __EMSCRIPTEN__
        const auto text = appearance::to_json(current_preferences()).dump();
        g_preferences_saved = EM_ASM_INT({
            try { localStorage.setItem('edgedepth.appearance.v2', UTF8ToString($0)); return 1; }
            catch (_) { return 0; }
        }, text.c_str());
#endif
    }

    namespace {
        // A full-width combo of labels; returns the picked index.
        int appearance_combo(const char* id, const char* const* labels, int n, int selected) {
            ImGui::SetNextItemWidth(-1);
            int picked = selected;
            if (ImGui::BeginCombo(id, labels[selected])) {
                for (int i = 0; i < n; ++i) {
                    if (ImGui::Selectable(labels[i], i == selected)) picked = i;
                    if (i == selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            return picked;
        }
    }

    void render_appearance_controls() {
        bool changed = false;
        ImGui::TextUnformatted("Interface theme");
        const int c = appearance_combo("##interface_theme", kChromeNames, int(std::size(kChromeNames)), int(chrome()));
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
        const int palette = appearance_combo("##market_palette", kPaletteNames, int(std::size(kPaletteNames)), shown);
        if (palette != shown) {
            set_candle_convention(kPalettes[palette]);
            changed = true;
        }
        ImVec4 up = Tokens::UP, down = Tokens::DOWN;
        const bool up_changed = ImGui::ColorEdit3("Up / buy", &up.x, ImGuiColorEditFlags_NoInputs);
        const bool down_changed = ImGui::ColorEdit3("Down / sell", &down.x, ImGuiColorEditFlags_NoInputs);
        if (up_changed || down_changed) {
            const auto hex = [](ImVec4 col) {
                return (uint32_t(col.x * 255 + 0.5f) << 16) |
                       (uint32_t(col.y * 255 + 0.5f) << 8) | uint32_t(col.z * 255 + 0.5f);
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
        c[ImGuiCol_FrameBg]           = INPUT;
        c[ImGuiCol_FrameBgHovered]    = HOVER;
        c[ImGuiCol_FrameBgActive]     = ACTIVE;

        // ── text ─────────────────────────────────────────────────────────────
        c[ImGuiCol_Text]              = TX1;
        c[ImGuiCol_TextDisabled]      = TX3;

        // ── title / tabs (docked panel chrome stays quiet) ───────────────────
        c[ImGuiCol_TitleBg]           = ELEV;
        c[ImGuiCol_TitleBgActive]     = ELEV;
        c[ImGuiCol_TitleBgCollapsed]  = PANEL;
        c[ImGuiCol_Tab]               = PANEL;
        c[ImGuiCol_TabHovered]        = HOVER;
        c[ImGuiCol_TabActive]         = PANEL;
        c[ImGuiCol_TabUnfocused]      = PANEL;
        c[ImGuiCol_TabUnfocusedActive]= ELEV;

        // ── selection / headers - brand-soft, used sparingly ─────────────────
        c[ImGuiCol_Header]            = BRAND_SOFT;
        c[ImGuiCol_HeaderHovered]     = HOVER;
        c[ImGuiCol_HeaderActive]      = with_a(BRAND, 0.22f);

        // ── buttons ──────────────────────────────────────────────────────────
        c[ImGuiCol_Button]            = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ButtonHovered]     = HOVER;
        c[ImGuiCol_ButtonActive]      = ACTIVE;
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
        s.FrameRounding     = Radius::R2;   // --r2
        s.GrabRounding      = Radius::R1;
        s.PopupRounding     = Radius::R3;   // floating chrome: radius 6
        s.TabRounding       = 0.0f;
        s.WindowBorderSize  = 1.0f;
        s.ChildBorderSize   = 1.0f;
        s.FrameBorderSize   = 0.0f;
        s.PopupBorderSize   = 1.0f;
        s.ScrollbarRounding = 0.0f;
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
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, selected ? Tokens::TX1 : Tokens::TX2);
        const bool clicked = ImGui::Button(label, size);
        if (selected) {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x + 6, b.y - 1),
                ImVec2(b.x - 6, b.y - 1), u32(Tokens::BRAND), 2);
        }
        ImGui::PopStyleColor(2);
        return clicked;
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
            ImGui::PushStyleColor(ImGuiCol_Header, Tokens::ACTIVE);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Tokens::ACTIVE);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, Tokens::HOVER);
            ImGui::BeginDisabled(!enabled);
            const bool clicked = ImGui::Selectable("##menu_row", false,
                flags | ImGuiSelectableFlags_SpanAvailWidth, ImVec2(natural_w, h));
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

    bool begin_menu(const char* id, float min_width, float max_height) {
        // Every value below is latched by BeginPopup, so all of it pops
        // straight away whether or not the popup is open.
        ImGui::SetNextWindowSizeConstraints(ImVec2(min_width, 0.0f), ImVec2(FLT_MAX, max_height));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5.0f, 5.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Radius::R3);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Tokens::PANEL);
        ImGui::PushStyleColor(ImGuiCol_Border, Tokens::BD2);
        const bool open = ImGui::BeginPopup(id);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
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
        dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), u32(Tokens::LOGO, 0.10f));
        dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), u32(Tokens::LOGO, 0.65f), 0.0f, 0, 1.0f);
        ImGui::PushFont(Fonts::label());
        dl->AddText(ImVec2(p.x + 6.0f, p.y + 2.5f), u32(Tokens::LOGO), "PRO");
        ImGui::PopFont();
    }

    bool menu_toggle(const char* label, bool on, bool locked, const char* hint) {
        const float trailing = locked ? pro_tag_size().x : 0.0f;
        ImGui::PushStyleColor(ImGuiCol_Text, locked ? Tokens::TX3 : (on ? Tokens::TX1 : Tokens::TX2));
        const MenuRow r = menu_row(label, locked ? nullptr : hint, true, /*keep_open=*/true, trailing);
        ImGui::PopStyleColor();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (locked) {
            drawing::draw_ui_icon(dl, drawing::UiIcon::Lock, r.icon, 6.0f,
                                  u32(r.hovered ? Tokens::TX2 : Tokens::TX3), 1.3f);
            const ImVec2 tag = pro_tag_size();
            draw_pro_tag(dl, ImVec2(r.max.x - kMenuPadX - tag.x, (r.min.y + r.max.y - tag.y) * 0.5f));
        } else {
            const ImVec2 b0(r.icon.x - 7.0f, r.icon.y - 7.0f), b1(r.icon.x + 7.0f, r.icon.y + 7.0f);
            if (on) {
                dl->AddRectFilled(b0, b1, u32(Tokens::BRAND));
                drawing::draw_ui_icon(dl, drawing::UiIcon::Check, r.icon, 5.0f, u32(Tokens::BRAND_INK), 1.6f);
            } else {
                dl->AddRect(b0, b1, u32(r.hovered ? Tokens::BD3 : Tokens::BD2), 0.0f, 0, 1.0f);
            }
        }
        return r.clicked;
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
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX2);
        const MenuRow r = menu_row(label, nullptr, true, /*keep_open=*/true);
        ImGui::PopStyleColor();
        if (r.clicked) { open = !open; storage->SetBool(id, open); }
        drawing::draw_ui_icon(ImGui::GetWindowDrawList(),
                              open ? drawing::UiIcon::ChevronDown : drawing::UiIcon::ChevronRight,
                              r.icon, 5.0f, r.icon_col, 1.4f);
        if (!open) return false;
        ImGui::Indent(kLabelX);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width());
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        return true;
    }

    void end_menu_group() {
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::PopTextWrapPos();
        ImGui::Unindent(kLabelX);
    }

    void menu_note(const char* text) {
        ImGui::Indent(kMenuPadX);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width());
        ImGui::PushStyleColor(ImGuiCol_Text, Tokens::TX3);
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::Unindent(kMenuPadX);
    }

    void menu_section(const char* label) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushFont(Fonts::label());
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + kMenuPadX, p.y + 8.0f),
                                            u32(Tokens::TX3), label);
        const ImVec2 size(ImGui::CalcTextSize(label).x + kMenuPadX * 2.0f,
                          ImGui::GetFontSize() + 13.0f);
        ImGui::PopFont();
        ImGui::Dummy(size);
    }

    void menu_separator() {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 4.0f, p.y + 4.5f),
                                            ImVec2(p.x + w - 4.0f, p.y + 4.5f),
                                            u32(Tokens::BD1), 1.0f);
        ImGui::Dummy(ImVec2(1.0f, 9.0f));
    }

    void begin_tooltip() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 5.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Radius::R3);
        ImGui::BeginTooltip();
    }

    void end_tooltip() {
        ImGui::EndTooltip();
        ImGui::PopStyleVar(2);
    }

    void tooltip(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        begin_tooltip();
        ImGui::TextV(fmt, args);
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
