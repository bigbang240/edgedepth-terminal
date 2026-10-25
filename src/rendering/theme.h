#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// theme.h - edgedepth design system: tokens + ImGui/ImPlot style + fonts
//
// Source of truth: the edgedepth design system - tokens.json (v0.1.0) and
// edgedepth.css (:root block). Dark-only.
// Near-black surfaces, teal-up / magenta-rose-down market data, a neutral
// chrome accent with the mint brand identity, amber for replay/events/POC.
//
// Semantic colors (UP/DOWN/BRAND/…) are runtime-mutable so the Tweaks panel
// can switch candle conventions and accents without a rebuild. Surfaces,
// borders and text ramp are runtime-mutable too: a chrome preset (set_chrome)
// swaps them as one ramp. Read tokens at draw time; never copy one into a
// static or constexpr.
//
// Legacy aliases (Theme::Colors::BUY_GREEN etc.) are kept so existing widgets
// compile unchanged; new/reworked code should use Theme::Tokens:: directly.
// ═══════════════════════════════════════════════════════════════════════════════
#include <cfloat>
#include <cstdint>

#include "imgui.h"

namespace drawing { enum class UiIcon : uint8_t; }  // ui/drawing/drawing_icons.h

namespace Theme {

    // hex 0xRRGGBB → ImVec4
    constexpr ImVec4 from_hex(uint32_t rgb, float a = 1.0f) {
        return ImVec4(((rgb >> 16) & 0xFF) / 255.0f,
                      ((rgb >> 8) & 0xFF) / 255.0f,
                      (rgb & 0xFF) / 255.0f, a);
    }

    // ImVec4 → ImU32 with optional alpha multiplier
    inline ImU32 u32(const ImVec4& c, float alpha_mul = 1.0f) {
        return IM_COL32(static_cast<int>(c.x * 255.0f),
                        static_cast<int>(c.y * 255.0f),
                        static_cast<int>(c.z * 255.0f),
                        static_cast<int>(c.w * alpha_mul * 255.0f));
    }

    // ── Runtime tweak enums (wired to the Tweaks panel in a later phase) ─────
    enum class Accent : uint8_t { Teal, Indigo, Amber, Mono };
    // Saved as ints in edgedepth.appearance.v2: append new values, never reorder.
    enum class CandleConvention : uint8_t { TealMag, Classic, Muted, BlueWhite, Custom, BlueOrange };
    // Dark chrome presets (surfaces, controls, hairlines, text). Market colors
    // and the accent stay separate choices. Values live in theme.cpp kChrome.
    // Daylight (Phase 7) is the first LIGHT preset.
    enum class Chrome : uint8_t { EdgeDepth, Graphite, Midnight, HighContrast, Daylight };

    namespace Tokens {
        // ── Surfaces - theme-tokens.json bg ramp (2026-07-02 redesign) ───────
        // bg-0 app background / chart canvas · bg-1 bars + panel chrome ·
        // Near-black planes; controls have no idle tile. Hover and focus
        // reveal the hit area, while a rule and text identify selection.
        // Surfaces, controls, hairlines and text are runtime-mutable: these
        // initial values are the EdgeDepth chrome preset (set_chrome).
        inline ImVec4 BASE   = from_hex(0x050505);  // bg-0
        inline ImVec4 PANEL  = from_hex(0x080808);  // bg-1
        inline ImVec4 ELEV   = from_hex(0x0d0d0d);  // bg-2
        inline ImVec4 INPUT  = from_hex(0x0b0b0b);  // editable fields
        inline ImVec4 HOVER  = from_hex(0x141414);  // bg-2
        inline ImVec4 ACTIVE = from_hex(0x1b1b1b);  // bg-2 (accent marks "on")

        // ── Controls (design v3) - soft filled buttons, segmented tracks, cards ─
        inline ImVec4 CONTROL        = from_hex(0x171717);  // soft button rest
        inline ImVec4 CONTROL_HOVER  = from_hex(0x202020);
        inline ImVec4 CONTROL_ACTIVE = from_hex(0x2a2a2a);
        inline ImVec4 TRACK          = from_hex(0x101010);  // segmented control track
        inline ImVec4 CARD           = from_hex(0x0c0c0c);  // grouped content surface
        inline ImVec4 MENU           = from_hex(0x0e0e0e);  // floating menus (begin_menu)

        // ── Hairlines - line-1 borders/dividers, line-2 control borders ──────
        inline ImVec4 BD1  = from_hex(0x1c1c1c);          // line-1 - the only separation
        inline ImVec4 BD2  = from_hex(0x292929);          // line-2 - control borders
        inline ImVec4 BD3  = from_hex(0x737373, 0.55f);   // hover borders (text-3 hue)
        inline ImVec4 GRID = from_hex(0x1c1c1c, 0.55f);   // chart gridlines (line-1)

        // ── Text ramp - text-1/2/3 (+ dimmed text-3 for axis/disabled) ───────
        inline ImVec4 TX1 = from_hex(0xe8e8e8);  // primary numerals, titles
        inline ImVec4 TX2 = from_hex(0xb0b0b0);  // labels, secondary data
        inline ImVec4 TX3 = from_hex(0x858585);  // captions, units, group labels
        inline ImVec4 TX4 = from_hex(0x737373, 0.72f);  // axis ticks, disabled

        // ── Semantic - runtime-mutable (candle convention / accent tweaks) ──
        inline ImVec4 UP         = from_hex(0x2fd6ad);         // up / bid / positive
        inline ImVec4 DOWN       = from_hex(0xee5c78);         // down / ask / negative
        inline ImVec4 UP_SOFT    = from_hex(0x2fd6ad, 0.13f);
        inline ImVec4 DOWN_SOFT  = from_hex(0xee5c78, 0.13f);
        inline ImVec4 UP_LINE    = from_hex(0x2fd6ad, 0.50f);
        inline ImVec4 DOWN_LINE  = from_hex(0xee5c78, 0.50f);
        inline ImVec4 BRAND      = from_hex(0xe7e9ed);         // accent - the only on-state hue
        inline ImVec4 BRAND_SOFT = from_hex(0xe7e9ed, 0.14f);
        inline ImVec4 BRAND_LINE = from_hex(0xe7e9ed, 0.55f);
        inline ImVec4 BRAND_TX   = from_hex(0xffffff);         // accent textTint (on-state text)
        inline constexpr ImVec4 WARN = from_hex(0xf0b350);     // funding, big prints, alerts
        inline constexpr ImVec4 ACTIVITY_TRADES = from_hex(0x87b9e5);
        inline constexpr ImVec4 ACTIVITY_ACTIVE = from_hex(0x4ddbac, 0.11f);
        inline constexpr ImVec4 WARN_SOFT = from_hex(0xf0b350, 0.12f);  // replay banner, high-regime chip fill
        // dark ink for text on solid BRAND fills (play orb, price chip, pills)
        inline constexpr ImVec4 BRAND_INK = from_hex(0x111111);
        // Brand identity, FIXED: the EdgeDepth D mark and the EARLY ACCESS lockup
        // keep the marketing mint (tokens.css --accent / --accent-text, both
        // #4ddbac since 2026-09-18) whatever accent the chrome runs, so the
        // terminal header matches the web header.
        // Identity only - never an on-state hue (that stays BRAND*). Upgrade
        // surfaces (PRO tags, the upsell CTA) wear it as brand, not as state.
        inline constexpr ImVec4 LOGO    = from_hex(0x4ddbac);
        inline constexpr ImVec4 LOGO_HOVER = from_hex(0x78e7c4);  // marketing mint hover tint
        inline constexpr ImVec4 LOGO_TX = from_hex(0x4ddbac);
        // Data inspection has a fixed mint cursor, distinct from mutable chrome.
        inline constexpr ImVec4 INSPECT = LOGO;
        // resting-book blue (DOM v2): pending limit-order depth, side-agnostic. The
        // one neutral data hue - teal/rose stays reserved for EXECUTED flow. (Confirm
        // this hue or remap to a brand blue before shipping - dom.SPEC.md §8.)
        inline constexpr ImVec4 REST      = from_hex(0x4d8cd6);
        inline constexpr ImVec4 REST_SOFT = from_hex(0x4d8cd6, 0.30f);

        // ── Heatmap ramps (fixed LUT stops; full LUTs built in heatmap code) ─
        // viridis - liquidation field legend / colormap stops
        inline constexpr ImVec4 VIRIDIS[6] = {
            from_hex(0x440154), from_hex(0x414487), from_hex(0x2a788e),
            from_hex(0x22a884), from_hex(0x7ad151), from_hex(0xfde725)};
        // ocean - DOM depth cells (small=deep blue → large=green/yellow)
        inline constexpr ImVec4 OCEAN[6] = {
            from_hex(0x06101d), from_hex(0x0e2f5e), from_hex(0x1c6a8e),
            from_hex(0x26a59a), from_hex(0x7fd17a), from_hex(0xeaf06a)};
        // iceberg marker accent (DOM row tick)
        inline constexpr ImVec4 ICEBERG_VIOLET = from_hex(0xb07cff);
    }

    // ── Radii ────────────────────────────────────────────────────────────────
    // Design v3 (James 2026-10-10: "I really hate those rectangle/square
    // buttons"; supersedes the 2026-09-13 all-square rule): docked panels stay
    // SQUARE with 1px line-1 borders; controls, cards and floating chrome are
    // rounded.
    namespace Radius {
        inline constexpr float R1 = 4.0f;   // chips, badges, small controls
        inline constexpr float R2 = 6.0f;   // buttons, inputs, segmented controls
        inline constexpr float R3 = 10.0f;  // cards, popovers, menus, dialogs
    }

    // ── Layout metrics (px, logical) - theme-tokens.json `density` ──────────
    namespace Layout {
        inline constexpr float TOPBAR_H        = 44.0f;
        inline float topbar_h() { return ImGui::GetIO().DisplaySize.x < 1150 ? 82.0f : TOPBAR_H; }
        inline constexpr float STATSBAR_H      = 36.0f;   // compact market header; closeable
        inline constexpr float PILLSROW_H      = 44.0f;   // v2 chart toolbar band (was 36)
        inline constexpr float STATUSBAR_H     = 30.0f;   // owns telemetry (WS · FPS · CLOCK)
        inline constexpr float TRANSPORT_H     = 66.0f;
        // The watchlist stacks price below the symbol on compact rails.
        // Reserve the order-book width from the remaining center/right space.
        inline constexpr float WATCHLIST_W     = 288.0f;
        inline constexpr float RIGHTCOL_W      = 440.0f;
        inline constexpr float INDI_PANE_H     = 172.0f;
        inline constexpr float PANEL_HEADER_H  = 26.0f;
        inline constexpr float WATCHLIST_ROW_H = 24.0f;   // default-density watchlist row
        inline constexpr float DOMTAPE_ROW_H   = 18.0f;   // default-density DOM/tape row
        inline constexpr float PANEL_PAD       = 8.0f;
        inline constexpr float PRICE_GUTTER_R  = 66.0f;
        inline constexpr float TIME_AXIS_B     = 22.0f;
    }

    // ── Density (Tweaks-driven; 3-10, default 8) ─────────────────────────────
    int  density();
    void set_density(int d);                 // clamps to [3,10], re-derives row heights
    float row_h();                           // watchlist rows - d8 = WATCHLIST_ROW_H (24)
    float row_h_dense();                     // DOM/tape rows - d8 = DOMTAPE_ROW_H (18)

    // ── Runtime tweaks ───────────────────────────────────────────────────────
    Accent accent();
    CandleConvention candles();
    Chrome chrome();
    void load_preferences();
    void save_preferences();                         // after a change outside the controls below (liq colormap)
    void render_appearance_controls();
    void set_accent(Accent a);                       // mutates BRAND* tokens
    void set_candle_convention(CandleConvention c);  // mutates UP*/DOWN* tokens
    void set_chrome(Chrome c);                       // mutates surface/control/hairline/text tokens

    // ── Fonts ────────────────────────────────────────────────────────────────
    // Inter = chrome/labels/headings. Roboto Mono = ALL numerics
    // (naturally fixed-advance → tabular alignment for free).
    namespace Fonts {
        ImFont* ui_sm();        // Inter Regular 15, secondary UI text
        ImFont* ui();           // Inter Regular 13 - default chrome text
        ImFont* ui_semibold();  // Inter SemiBold 13 - emphasis, symbol names
        ImFont* heading();      // Inter SemiBold 17 - panel/modal headings
        ImFont* label();        // Inter SemiBold 9.5 - uppercase micro-labels
        ImFont* mono_xs();      // Roboto Mono SemiBold ~9.5 - EARLY ACCESS pill / micro badges
        ImFont* mono_sm();      // Roboto Mono Regular 10.5 - dense ladders/tape
        ImFont* mono();         // Roboto Mono Regular 12 - default numerics
        ImFont* mono_md();      // Roboto Mono Medium 14 - mark price, replay clock
        ImFont* mono_lg();      // Roboto Mono SemiBold 20 - large displays
    }

    // ── Style application ────────────────────────────────────────────────────
    void apply_dark_theme();      // full ImGui + ImPlot mapping from tokens
    void apply_trading_colors();  // table borders/headers (hairline style)
    bool load_fonts();            // builds the 8-face atlas (DPI-aware)

    // Shared transient surfaces and selection controls. Popup padding is latched
    // by BeginPopup, so callers keep the normal ImGui::EndPopup pairing.
    bool begin_popup(const char* id, ImGuiWindowFlags flags = 0);
    // A toggle chip: rounded, filled while selected (was an underlined text button).
    bool choice_button(const char* label, bool selected, ImVec2 size = ImVec2(0, 0));
    void section_label(const char* label);

    // ── Controls (design v3) ─────────────────────────────────────────────────
    // A font at an exact logical size (ImGui 1.92 rasterizes it crisply).
    void push_font(ImFont* font, float logical_px);
    // Rounded buttons. Soft: the everyday action (filled). Ghost: quiet until
    // hovered. Primary: the one main action on a surface. Upgrade: mint, upsell
    // CTAs only. size 0 = fit the label at the current frame height; an icon
    // leads the label, or trails it with trailing_icon.
    enum class ButtonKind : uint8_t { Soft, Ghost, Primary, Upgrade };
    bool button(const char* label, ButtonKind kind = ButtonKind::Soft, ImVec2 size = ImVec2(0, 0),
                drawing::UiIcon icon = drawing::UiIcon(255), bool trailing_icon = false);
    // A round, icon-only ghost button; active keeps it filled (a toggle that is on).
    bool icon_button(const char* id, drawing::UiIcon icon, const char* tooltip = nullptr,
                     float size = 26.0f, bool active = false);
    // One of n options on a rounded track, the selected one raised. Returns the
    // clicked index, else selected. height 0 = the frame height.
    int segmented(const char* id, const char* const* labels, int n, int selected, float height = 0.0f);
    // Widths, for right-aligning controls before drawing them.
    float button_width(const char* label, float height = 0.0f, bool with_icon = false);
    float segmented_width(const char* const* labels, int n, float height = 0.0f);
    // An inline rounded chip (status, qualifier); advances the cursor like text.
    void chip(const char* text, const ImVec4& fg, const ImVec4& bg);
    // A signed 24h change as a slim pill (watchlist, symbol picker; James 2026-10-11: the mono pills read "thick"
    // and wide for the number): Inter at px, 7 px of padding, px * 1.5 tall, a faint signed fill. Right-aligned at
    // right_x, centred on cy; at least as wide as "+00.00%" so a column of pills lines up. Returns its width.
    float change_pill(ImDrawList* dl, float right_x, float cy, double pct, float px);
    float change_pill_width(float px);   // the column's width (that of "+00.00%")
    // The OCEAN ramp at t in [0, 1] (DOM depth: small = deep blue, large = green/yellow).
    ImVec4 ocean(float t);
    // A DOM depth cell in [min, max]: a bar from the price side (from_right for bids, whose price column is on their
    // right) that always holds its whole number and runs on by size (frac^0.6 of the cell), brighter on the OCEAN
    // ramp as it grows, kept short of the palest end so the number stays readable. The number sits at text_y.
    void depth_bar(ImDrawList* dl, ImVec2 min, ImVec2 max, const char* text, float frac, bool from_right, float text_y);
    // A small circled "i" whose hover shows text; replaces "?" buttons.
    void info_hint(const char* text);
    // A hairline across width at the cursor, with vertical room around it.
    void hairline(float width, float space = 10.0f);

    // Floating action menus (chart right-click, + widget, chart tools, Layers,
    // Chart view, timeframes): one look everywhere. begin_menu pushes the
    // chrome and a minimum width and lets the popup auto-size, so a long label
    // or hint widens the menu instead of running past its edge. Rows are 30px:
    // a 16px icon slot, the label in the current ImGuiCol_Text and a
    // right-hand mono hint (a price, a time, a market). Rows close the popup on
    // click like ImGui::MenuItem unless keep_open; a disabled row still answers
    // IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) for its tooltip.
    // Callers keep the normal ImGui::EndPopup pairing.
    bool begin_menu(const char* id, float min_width = 232.0f, float max_height = FLT_MAX);
    struct MenuRow {
        bool   clicked = false, right_clicked = false, hovered = false;
        ImVec2 min, max;       // the full row
        ImVec2 icon;           // centre of the icon slot
        ImU32  icon_col = 0;   // label colour, muted until hovered
    };
    // The primitive under the rows below, for a caller that draws its own
    // glyph in the icon slot. trailing_w reserves room at the right for a
    // control the caller submits after the row (it may overlap the row).
    MenuRow menu_row(const char* label, const char* hint = nullptr, bool enabled = true,
                     bool keep_open = false, float trailing_w = 0.0f);
    bool menu_item(drawing::UiIcon icon, const char* label,
                   const char* hint = nullptr, bool enabled = true, bool keep_open = false);
    // Checklist row for multi-select menus: a rounded check box in the icon
    // slot, stays open on click. Locked = a lock and a PRO tag; the caller
    // routes the click to the upsell. trailing_w reserves room at the right
    // for a control the caller submits over the row afterwards (as menu_row).
    // Inside a row's settings (begin_menu_detail / an open group) it draws as
    // a setting instead: a shorter row with a switch at the right.
    bool menu_toggle(const char* label, bool on, bool locked = false, const char* hint = nullptr,
                     float trailing_w = 0.0f);
    // A layer or indicator row (Layers, Indicators): menu_toggle with the tag
    // ("admin", "pilot", "not on HL") drawn as a quiet chip at the right.
    bool menu_layer(const char* label, bool on, bool locked = false, const char* tag = nullptr, bool* settings = nullptr);
    // An action the viewer cannot take yet, under its own name: lock, label,
    // and the reason on a second line. pro adds the mint PRO tag.
    bool menu_item_locked(const char* label, const char* reason, bool pro, bool enabled = true);
    // Collapsible settings inside a menu. true = open: the content that
    // follows is indented to the label column and wraps at a fixed width;
    // close it with end_menu_group. Open state lives in ImGui storage. Inside
    // a row's settings it is a quiet "Options >" disclosure.
    bool begin_menu_group(const char* label);
    void end_menu_group();
    // A layer's settings while it is on, always open: under its row, the
    // labels on the row's label column, joined to the row's check box by a
    // guide line. Close with end_menu_detail. Keep it to the controls a viewer
    // reaches for; longer settings go in a collapsed begin_menu_group inside
    // it. menu_toggle, menu_item and menu_note draw as settings in here.
    void begin_menu_detail();
    void end_menu_detail();
    // One of n choices as a segmented control across the content width (the
    // selected one raised). Returns the clicked index, or selected when
    // nothing was clicked.
    int menu_choice(const char* id, const char* const* labels, int n, int selected);
    // A setting with a few values: its label, a compact segmented control at the right.
    int menu_choice_row(const char* label, const char* id, const char* const* labels, int n, int selected);
    // A setting with a number: its label, a compact rounded field at the right. Returns true when edited.
    bool menu_field(const char* label, float* value, const char* format = "%.0f", float step = 0.0f,
                    float field_w = 96.0f);
    void menu_note(const char* text);        // muted wrapped explanation
    void menu_section(const char* label);
    void menu_separator();
    // The upgrade tag (mint outline, "PRO"): menus, timeframe groups, locks.
    ImVec2 pro_tag_size();
    void draw_pro_tag(ImDrawList* dl, ImVec2 top_left);
    // The quiet tag (micro caps on a soft fill, "ADMIN", "PROVISIONAL"): layer rows and hovers. draw_tag upper-cases
    // and puts its right edge at right_mid; tag_size measures the text as given. A hover's lighter surface takes a
    // lighter fill (CONTROL_HOVER) so the chip still reads.
    ImVec2 tag_size(const char* tag);
    void draw_tag(ImDrawList* dl, ImVec2 right_mid, const char* tag, const ImVec4& fill = Tokens::CONTROL);

    // ── Chart chips and loading ─────────────────────────────────────────────
    // Chips drawn over a chart (OHLC readout, status, layer keys) register their rectangle for the frame; the chart's
    // crosshair passes under them instead of running through their text. A chart resets the list as it starts.
    void chart_chips_reset();
    void chart_chip(ImVec2 a, ImVec2 b);
    bool chart_chip_covers(ImVec2 p, float margin = 2.0f);
    // A soft drop shadow outside a floating surface (menus, undocked panels), drawn into dl with a full-screen clip.
    void float_shadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding);
    // The one loading mark (replay scrim, real-time chart, empty chart): a mint arc turning over a faint ring.
    void spinner(ImDrawList* dl, ImVec2 center, float radius, float thickness = 2.5f);

    // ── Tooltips ─────────────────────────────────────────────────────────────
    // The global WindowPadding is (0,0) (panels manage their own gutters) and
    // ImGui tooltip windows inherit it, so raw ImGui::SetTooltip/BeginTooltip
    // renders text flush against the border (looks clipped). All tooltips
    // route through these, which push floating-chrome padding + radius first.
    void begin_tooltip(bool replace_previous = false);
    void end_tooltip();
    void tooltip(const char* fmt, ...) IM_FMTARGS(1);  // SetTooltip replacement

    // ── Legacy compatibility layer - migrate widgets off these per-phase ─────
    namespace Colors {
        inline const ImVec4& BUY_GREEN       = Tokens::UP;
        inline const ImVec4& SELL_RED        = Tokens::DOWN;
        inline const ImVec4& BUY_GREEN_DIM   = Tokens::UP_LINE;
        inline const ImVec4& SELL_RED_DIM    = Tokens::DOWN_LINE;
        inline constexpr ImVec4 PRICE_YELLOW = Tokens::WARN;
        inline const ImVec4& NEUTRAL_GRAY    = Tokens::TX3;
        inline const ImVec4& BACKGROUND_DARK = Tokens::BASE;
        inline const ImVec4& PANEL_BG        = Tokens::PANEL;
        inline const ImVec4& BORDER          = Tokens::BD2;
        inline const ImVec4& TEXT_PRIMARY    = Tokens::TX1;
        inline const ImVec4& TEXT_SECONDARY  = Tokens::TX2;
    }

    namespace FontSizes {  // legacy names, new scale
        inline constexpr float SMALL  = 13.0f;
        inline constexpr float NORMAL = 16.0f;
        inline constexpr float LARGE  = 20.0f;
        inline constexpr float XLARGE = 23.0f;
    }
    namespace Spacing {  // legacy names, new metrics
        inline constexpr float PADDING         = 8.0f;
        inline constexpr float ITEM_SPACING    = 6.0f;
        inline constexpr float WINDOW_ROUNDING = 0.0f;  // panels are square
        inline constexpr float FRAME_ROUNDING  = Radius::R2;
    }

    // legacy font accessors → new atlas
    ImFont* get_regular_font();  // = Fonts::ui()
    ImFont* get_bold_font();     // = Fonts::ui_semibold()
    ImFont* get_large_font();    // = Fonts::mono_md()

    // legacy semantic helpers (read mutable tokens → tweak-aware)
    ImVec4 get_buy_color(float alpha = 1.0f);
    ImVec4 get_sell_color(float alpha = 1.0f);
    ImU32 get_buy_color_u32(uint8_t alpha = 255);
    ImU32 get_sell_color_u32(uint8_t alpha = 255);
}
