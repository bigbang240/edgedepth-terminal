#include "core/i18n.h"
#include "ui/chart_widget.h"
#include "ui/compression_overlay.h"
#include "core/liq_field_venue_caps.h"
#include "core/liq_field_tiers.h"
#include "core/entitlements.h"
#include "core/education_boot.h"
#include "core/ticker_manager.h"
#include "core/ui_test_marks.h"
#include "core/liqmap2_scorecard.h"
#include "rendering/observed_liquidations.h"
#include "rendering/theme.h"
#include "replayer/replay_manager.h"
#include "ui/open_url.h"
#include "ui/upsell_modal.h"
#include "ui/touch_odds_overlay.h"
#include "ui/drawing/drawing_icons.h"
#include <algorithm>
#include <cmath>

// Sources and chart aids have separate menus. A settings button opens one detail
// page, so switching a source on never grows a list of unrelated controls.
void ChartWidget::render_overlay_menu(bool liquidations, ImVec2 anchor) {
    const char* id = liquidations ? "liquidations_popup" : "layers_popup";
    auto& detail = liquidations ? liquidation_menu_detail_ : layers_menu_detail_;
    ImGui::SetNextWindowPos(ImVec2(anchor.x, anchor.y + 4.0f));
    if (!Theme::begin_menu(id, 300.0f, ImGui::GetMainViewport()->WorkSize.y - 32.0f)) return;
    using drawing::UiIcon;
    const bool pro = Entitlements::is_pro();
    const bool absorption_supported = liquidity_response::supported(pair_.exchange, pair_.symbol);
    if (!detail.empty()) {
        if (Theme::menu_item(UiIcon::ChevronLeft, "Back", nullptr, true, true)) detail.clear();
        ui_test::mark("overlay.back");
        Theme::menu_separator();
    }
    // Snapshot this frame's page: clicks navigate on the next frame, not halfway through the list.
    const std::string page = detail;
    const auto wants = [&](const char* label) { return page.empty() || page == label; };
    auto layer_section = [&](const char* label) { if (page.empty()) Theme::menu_section(label); };
    auto layer_row = [&](const char* label, bool on, bool locked, const char* tag = nullptr) {
        const bool has_settings = page.empty() && !locked &&
            (std::string_view(label) == i18n::tr("Liquidation map", i18n::fa_l_liq_map) || std::string_view(label) == i18n::tr("Hyperliquid positions", i18n::fa_l_hl_positions) ||
             std::string_view(label) == i18n::tr("Reported liquidations", i18n::fa_l_reported) || std::string_view(label) == i18n::tr("Trade bubbles (large prints)", i18n::fa_l_bubbles_large) ||
             std::string_view(label) == i18n::tr("Trade bubbles", i18n::fa_l_bubbles) || std::string_view(label) == i18n::tr("Order book depth", i18n::fa_l_depth) || std::string_view(label) == "Volume profile (VPVR)" ||
             std::string_view(label) == i18n::tr("Absorption bands", i18n::fa_l_absorption) ||
             std::string_view(label) == i18n::tr("Resting depth heat", i18n::fa_l_resting) ||
             std::string_view(label) == "Whale trail" || std::string_view(label) == "Slippage cone");
        bool open = false;
        const bool clicked = Theme::menu_layer(label, on, locked, tag, has_settings ? &open : nullptr);
        if (open) detail = label;
        return clicked;
    };
    const auto options = [](const char*, auto&& body) { Theme::begin_menu_detail(); body(); Theme::end_menu_detail(); };
    const auto absorption_row = [&] {
        if (!absorption_supported || !wants(i18n::tr("Absorption bands", i18n::fa_l_absorption))) return;
        const bool locked = absorption_locked();
        if (layer_row(i18n::tr("Absorption bands", i18n::fa_l_absorption), liquidity_response_enabled_ && !locked, locked, locked ? nullptr : "pilot")) {
            if (locked) open_absorption_upsell();
            else { liquidity_response_enabled_ = !liquidity_response_enabled_; liquidity_response_history_.reset(); }
        }
        if (ImGui::IsItemHovered()) Theme::tooltip("Bids and asks that keep refilling. Binance BTC and SOL pilot.");
        if (page == i18n::tr("Absorption bands", i18n::fa_l_absorption) && !locked) options("", [&] { render_liquidity_response_settings(); });
    };
    if (liquidations) {
        if (rt_mode_) {

            if (layer_row(i18n::tr("Reported liquidations", i18n::fa_l_reported), reported_liquidations_enabled(), false)) {
                reported_liquidations_enabled() = !reported_liquidations_enabled();
                if (reported_liquidations_enabled()) { rt_liq_strip_=true; rt_liq_markers_=true; }
            }
            if (ImGui::IsItemHovered())
                Theme::tooltip("Liquidations the exchange published, as they happen.\n%s.",
                               observed_liquidations::feed_line(pair_.exchange));
            if (page == i18n::tr("Reported liquidations", i18n::fa_l_reported)) {
                Theme::begin_menu_detail();
                if (Theme::menu_toggle("Diamonds on the tape", rt_liq_markers_)) rt_liq_markers_ = !rt_liq_markers_;
                if (ImGui::IsItemHovered())
                    Theme::tooltip(observed_liquidations::bankruptcy_prices(pair_.exchange)
                        ? "Each report at its time, on the print the forced order made (Bybit reports\nbankruptcy prices). Sized against this market's last hour."
                        : "Each report at its time and fill price, sized against this market's last hour.");
                if (Theme::menu_toggle("Activity subplot", rt_liq_strip_)) rt_liq_strip_ = !rt_liq_strip_;
                Theme::end_menu_detail();
            }
        } else {
            layer_section("POSITION LEVELS");
            if (wants(i18n::tr("Hyperliquid positions", i18n::fa_l_hl_positions))) {
        // Hyperliquid positions = observed position levels from the HL census (Pro, P2e) - ground truth,
        // never folded into the modelled map above. Keyed by underlying, so it's offered on any venue's
        // chart, tagged "not on HL" (inert) when the underlying has no HL market. The ticker24h hl feed is
        // the market registry - before it loads, "unknown" stays offered and data arrival decides (the
        // layer's own NO DATA badge covers the miss).
        {
            // Free on BTC on Binance and Hyperliquid charts, Pro elsewhere (hl_rails_allowed).
            const bool hl_missing = hl_coin_missing();
            const bool hl_ok = hl_rails_allowed();
            const bool census_on = hl_ok && liq_census_enabled_ && !hl_missing;
            const char* hl_tag = hl_missing ? "not on HL" : hl_rails_free() && !pro ? "free here" : nullptr;
            if (layer_row(i18n::tr("Hyperliquid positions", i18n::fa_l_hl_positions), census_on, !hl_ok, hl_tag)) {
                if (!hl_ok) ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Layer,
                                                            nullptr, "hl_liq_levels");
                else if (!hl_missing) toggle_liq_census();
            }
            if (!hl_missing && ImGui::IsItemHovered())
                Theme::tooltip("Hyperliquid positions by the price where they liquidate, drawn as rails: only the\n"
                               "significant levels, the few that hold most of the dollars.\n"
                               "Positions that exist now, read from our Hyperliquid node: not a forecast.\n"
                               "Free on BTC, Pro on every coin.");
            if (hl_ok && !hl_missing && page == i18n::tr("Hyperliquid positions", i18n::fa_l_hl_positions)) {
                // Under the row while it is on: the real leverage shown (the census's own figures, not the scenario
                // map's assumed tiers), which layer leads when the V2 rails draw too, and the profile. Its own ID
                // scope: the V2 row's settings carry a "Profile at right edge" too.
                ImGui::PushID("hl_rails");
                Theme::begin_menu_detail();
                {
                    const float gap = 6.0f, band_w = ImGui::GetContentRegionAvail().x - 10.0f;
                    const float chip_w = std::max(44.0f, (band_w - gap*(hl_rails::kGroups - 1))/hl_rails::kGroups);
                    const float chip_h = 24.0f;
                    const ImVec2 base = ImGui::GetCursorScreenPos();
                    ImGui::Dummy(ImVec2(band_w, chip_h));   // reserve the chip band so the popup grows its bounds
                    for (int g = 0; g < hl_rails::kGroups; ++g) {
                        ImGui::PushID(700 + g);
                        ImGui::SetCursorScreenPos(ImVec2(base.x + g*(chip_w + gap), base.y));
                        if (Theme::choice_button(hl_rails::kGroupChips[g], hl_groups_ & (1u << g), ImVec2(chip_w, chip_h))) {
                            const uint8_t next = hl_groups_ ^ static_cast<uint8_t>(1u << g);
                            if (next) hl_groups_ = next;  // one group at least
                        }
                        if (ImGui::IsItemHovered())
                            Theme::tooltip("%s: the positions' own leverage on Hyperliquid.", hl_rails::kGroupLabels[g]);
                        ImGui::PopID();
                    }
                }
                if (liqmap2_rails_drawn()) {
                    static const char* const kLead[] = {"V2 rails", "Hyperliquid"};
                    hl_over_v2_ = Theme::menu_choice_row("Leads", "hl_lead", kLead, 2, hl_over_v2_ ? 1 : 0) == 1;
                    if (ImGui::IsItemHovered())
                        Theme::tooltip("Both rails draw. The leader keeps its profile, labels and hover; the other draws fainter\n"
                                       "beneath it. V2: modelled from this venue's open interest. Hyperliquid: positions that exist.");
                }
                if (Theme::menu_toggle("Value labels", rail_value_labels_)) rail_value_labels_ = !rail_value_labels_;
                if (Theme::menu_toggle("Profile at right edge", hl_profile_)) hl_profile_ = !hl_profile_;
                ui_test::mark("overlay.hl.profile");
                {
                if (Theme::menu_field("Minimum $", &liq_census_min_usd_)) {
                    if (!std::isfinite(liq_census_min_usd_)) liq_census_min_usd_ = 1000;
                    liq_census_min_usd_ = std::clamp(liq_census_min_usd_, 0.0f, 1e12f);
                }
                if (ImGui::IsItemHovered())
                    Theme::tooltip("Hide rails whose positions, in the leverage chosen, hold less than this.");
                Theme::menu_note("History: the last seven days, loaded as the view reaches back.");
                }
                Theme::end_menu_detail();
                ImGui::PopID();
            }
        }
            }
            if (wants(i18n::tr("Liquidation map", i18n::fa_l_liq_map))) {
        // One liquidation map row, one switch (liq_dense_field_; a saved V1 "on" reads as map on). Live, the
        // map is V2's milestone rails wherever the viewer has V2 (admin preview, free BTC, Pro) and a locked
        // PRO row into the upsell where V2 is Pro; the scenario map (V1) no longer draws on live charts.
        // Replay, packs and lessons carry no V2 frames (stream 38 is live only; lesson grants refuse its
        // history), so there the row is the scenario map with its settings, as before. The daily record
        // (public for every Binance and Bybit market) is one click away, locked or not.
        // The rails are here: live, or a box replay the viewer has them for (a locked replay keeps the scenario row).
        const bool live = live_chart() || (rails_replay() && liqmap2_allowed());
        const auto v2_access = liqmap2_access();
        const bool v2_on = liqmap2_shown(), scenario_on = scenario_map_shown();
        char record_url[160];
        const bool has_record = live && liqmap2::scorecard_url(pair_.exchange, pair_.symbol, record_url, sizeof(record_url));
        const auto record_item = [&] {
            if (!has_record) return;
            if (Theme::menu_item(UiIcon::Bars, "Daily record", "edgedepth.com")) ui::open_url(record_url);
            if (ImGui::IsItemHovered()) Theme::tooltip("The map's public scorecard for this market, day by day.");
        };
        if (live && v2_access == liqmap2::Access::locked) {
            if (layer_row(i18n::tr("Liquidation map", i18n::fa_l_liq_map), false, true))
                ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Layer, nullptr, "liqmap2");
            if (ImGui::IsItemHovered())
                Theme::tooltip("Liquidation rails: where positions would be forced out, in $.\nFree on BTC, Pro on every market.\n%s.",
                               liqmap2::kForwardTest);
            if (page.empty()) record_item();  // The public record remains available on locked markets.
        } else if (live && v2_access != liqmap2::Access::hidden) {
            const char* tag = v2_access == liqmap2::Access::preview ? "admin"
                            : v2_access == liqmap2::Access::free_taste ? "free here" : nullptr;
            if (layer_row(i18n::tr("Liquidation map", i18n::fa_l_liq_map), v2_on, false, tag)) liq_dense_field_ = !liq_dense_field_;
            if (ImGui::IsItemHovered())
                Theme::tooltip("Liquidation rails: where positions would be forced out, in $, ended by the candle that\n"
                               "reaches them. %s%s.", v2_access == liqmap2::Access::free_taste ? "Free on BTC, Pro on every market.\n" : "",
                               liqmap2::kForwardTest);
        } else if (!live) {
            if (layer_row(i18n::tr("Liquidation map", i18n::fa_l_liq_map), scenario_on, false, "scenario")) {
                liq_dense_field_ = !liq_dense_field_;
                liq_shelf_cache_ts_ = -1;
            }
            if (ImGui::IsItemHovered()) Theme::tooltip("Scenario map with assumed leverage.\nThe dollar map (V2) is live only.");
            // Right-click still opens the scenario map's style panel.
            if (scenario_on && ImGui::IsItemClicked(ImGuiMouseButton_Right)) open_liq_settings_ = true;
        }
        if (page == i18n::tr("Liquidation map", i18n::fa_l_liq_map) && live && v2_access != liqmap2::Access::locked && v2_access != liqmap2::Access::hidden) {
            // The rails' colour and profile sit under the row while it is on: this coin's Bybit and Binance rails
            // together (render_liqmap2), coloured by the liquidations expected within a horizon (the visual rule's
            // horizons; 4 h by default, as 1 h understates in development) or by the dollars.
            Theme::begin_menu_detail();
            if (v2_access == liqmap2::Access::preview) {
                static const char* const kColour[] = {"1h", "4h", "24h", "Dollars"};
                liqmap2_rails_colour_ = Theme::menu_choice_row("Colour", "liqmap2_colour", kColour, 4, std::clamp(liqmap2_rails_colour_, 0, 3));
                if (ImGui::IsItemHovered())
                    Theme::tooltip("Rail colour: liquidations expected within 1 h, 4 h or 24 h, or the dollars forced out\n"
                                   "if price reaches the rail. A horizon falls back to dollars while any rail in view has\n"
                                   "no figure for it yet. Bybit and Binance rails are both drawn.\n"
                                   "1 h and 4 h are admin only until their forward look passes.");
            } else {
                // Customers: 24 h or dollars. The 1 h / 4 h figures stay unvalidated until their forward look.
                static const char* const kColour[] = {"24h", "Dollars"};
                const int sel = Theme::menu_choice_row("Colour", "liqmap2_colour", kColour, 2, rails_colour() == 3 ? 1 : 0);
                liqmap2_rails_colour_ = 2 + sel;
                if (ImGui::IsItemHovered())
                    Theme::tooltip("Rail colour: liquidations expected within 24 h, or the dollars forced out if price\n"
                                   "reaches the rail. Bybit and Binance rails are both drawn.");
            }
            if (Theme::menu_toggle("Value labels", rail_value_labels_)) rail_value_labels_ = !rail_value_labels_;
            if (Theme::menu_toggle("Profile at right edge", liqmap2_rails_profile_)) liqmap2_rails_profile_ = !liqmap2_rails_profile_;
            if (ImGui::IsItemHovered()) Theme::tooltip("The live rails' value by price at the right edge, both venues summed.");
            Theme::menu_note(liqmap2::kForwardTest);
            record_item();
            Theme::end_menu_detail();
        } else if (!live && page == i18n::tr("Liquidation map", i18n::fa_l_liq_map)) {
            options("Options##scenario", [&] {
                // Scenario profile = price-marginal of the scenario map (free), not observed liquidations.
                if (layer_row(i18n::tr("Scenario profile", i18n::fa_l_scenario), liq_profile_enabled_, false))
                    liq_profile_enabled_ = !liq_profile_enabled_;
                // Assumed leverage tiers (Pro): the leverage mask the scenario map is built from.
                {
                    const ImVec2 hp = ImGui::GetCursorScreenPos();
                    Theme::menu_section("ASSUMED LEVERAGE");
                    if (!pro) {
                        ImGui::PushFont(Theme::Fonts::label());
                        const float lw = ImGui::CalcTextSize("ASSUMED LEVERAGE").x;
                        ImGui::PopFont();
                        Theme::draw_pro_tag(ImGui::GetWindowDrawList(), ImVec2(hp.x + 10.0f + lw + 8.0f, hp.y + 5.0f));
                    }
                }
                const double cap = liq_field::max_leverage(pair_.exchange, pair_.symbol);
                {
                    struct Lev { const char* label; bool* flag; };
                    Lev levs[4] = { {"25\xc3\x97", &liq_lev_25x_}, {"50\xc3\x97", &liq_lev_50x_},
                                    {"75\xc3\x97", &liq_lev_75x_}, {"100\xc3\x97", &liq_lev_100x_} };
                    const float gap = 6.0f;
                    const float band_w = ImGui::GetContentRegionAvail().x - 10.0f;
                    const float chip_w = std::max(52.0f, (band_w - gap * 3.0f) / 4.0f);
                    const float chip_h = 24.0f;
                    const ImVec2 base = ImGui::GetCursorScreenPos();
                    ImGui::Dummy(ImVec2(band_w, chip_h));   // reserve the chip band so the popup auto-grows its bounds
                    bool changed = false, lock_hit = false;
                    for (int i = 0; i < 4; ++i) {
                        const float x = base.x + i * (chip_w + gap);
                        ImGui::PushID(600 + i);
                        ImGui::SetCursorScreenPos(ImVec2(x, base.y));
                        const bool clicked = Theme::choice_button(levs[i].label,
                            pro && *levs[i].flag, ImVec2(chip_w, chip_h));
                        if (ImGui::IsItemHovered()) {
                            if (!pro) Theme::tooltip("Pro leverage filter");
                            else if (cap > 0) Theme::tooltip("Assumed leverage, not account leverage.\nVenue cap here: %gx.", cap);
                            else Theme::tooltip("Assumed leverage, not account leverage.");
                        }
                        ImGui::PopID();
                        if (clicked) { if (pro) { *levs[i].flag = !*levs[i].flag; changed = true; } else lock_hit = true; }
                    }
                    if (lock_hit) ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Layer,
                                                                  nullptr, "liq_tier_chips");
                    if (changed) {
                        uint8_t mask = 0;
                        if (liq_lev_5x_)   mask |= 0x01;
                        if (liq_lev_10x_)  mask |= 0x02;
                        if (liq_lev_25x_)  mask |= 0x04;
                        if (liq_lev_50x_)  mask |= 0x08;
                        if (liq_lev_75x_)  mask |= 0x10;
                        if (liq_lev_100x_) mask |= 0x20;
                        ctx_.liq_heatmap_mgr().set_leverage_mask(mask);
                    }
                }
                if (cap > 0) {
                    const uint8_t lmask = ctx_.liq_heatmap_mgr().get_leverage_mask();
                    if (liq_field::select_tiers(lmask, cap).enabled == 0 && liq_field::display_tiers(lmask, cap).enabled != 0)
                        ImGui::TextDisabled("Showing every tier up to the %gx cap.", cap);
                    else if (liq_field::display_tiers(lmask, cap).enabled == 0 && lmask != 0)
                        ImGui::TextDisabled("No tier fits this market's %gx cap.", cap);
                }
                if (Theme::menu_item(UiIcon::Gear, "Map style\xe2\x80\xa6")) {
                    open_liq_settings_ = true;
                    ImGui::CloseCurrentPopup();
                }
            });
        }
            }
            if (wants(i18n::tr("Reported liquidations", i18n::fa_l_reported))) {
                layer_section("REPORTED EVENTS");
        // Observed = real @forceOrder prints via the liquidations stream (Pro).
        if (layer_row(i18n::tr("Reported liquidations", i18n::fa_l_reported), pro && reported_liquidations_enabled(), !pro)) {
            // Switching the source on brings its callouts with it, as the
            // real-time row does for its diamonds (James, 2026-10-17).
            if (pro) { reported_liquidations_enabled() = !reported_liquidations_enabled();
                if (reported_liquidations_enabled()) { candle_liq_strip_=true; liq_markers_=true; }
            }
            else ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Layer,
                                                 nullptr, "liq_observed");
        }
        if (pro && ImGui::IsItemHovered())
            Theme::tooltip("Liquidations the exchange published, totalled per candle.\n%s.", observed_liquidations::feed_line(pair_.exchange));
        if (pro && page == i18n::tr("Reported liquidations", i18n::fa_l_reported)) {
            Theme::begin_menu_detail();
            render_liquidation_settings();
            Theme::end_menu_detail();
        }
            }
            if (page.empty()) {
                Theme::menu_separator();
                static const char* const modes[] = {"Major levels", "More detail"};
                hl_major_levels_ = Theme::menu_choice("hl_detail", modes, 2, hl_major_levels_ ? 0 : 1) == 0;
                if (ImGui::IsItemHovered()) Theme::tooltip("Hyperliquid display only. Major levels selects larger concentrations\nand separates nearby rails. More detail shows every eligible rail. Capture is unchanged.");
            }
        }
    } else {
        if (rt_mode_ && page != "Research tools") {
            // Real-time mode has its own layer set: the candle-chart rows below
            // draw nothing here, and i18n::tr("Order book depth", i18n::fa_l_depth) must mean the sampled
            // depth cells, not the candle heatmap. Overlay switches live here;
            // the Real-time menu owns navigation and panel visibility.
            layer_section("REAL-TIME VIEW");
            if (wants(i18n::tr("Order book depth", i18n::fa_l_depth))) {
                const bool c = layer_row(i18n::tr("Order book depth", i18n::fa_l_depth), rt_depth_enabled_, false, "Alt+1");
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    open_heatmap_settings_ = true;
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered())
                    Theme::tooltip("Sampled book behind the trades. Off leaves trades, quotes and liquidations on a clear background.");
                if (c) rt_depth_enabled_ = !rt_depth_enabled_;
            }
            if (page == i18n::tr("Order book depth", i18n::fa_l_depth)) {
                Theme::begin_menu_detail();
                if (Theme::menu_item(UiIcon::Gear, "Depth settings\xe2\x80\xa6")) {
                    open_heatmap_settings_ = true;
                    ImGui::CloseCurrentPopup();
                }
                if (Theme::menu_field("Opacity", &rt_depth_opacity_, "%.2f", 0.05f))
                    rt_depth_opacity_ = std::clamp(rt_depth_opacity_, 0.05f, 1.0f);
                if (ImGui::IsItemHovered())
                    Theme::tooltip("Fade the depth cells without turning the layer off -\nkeep trades and quotes readable on a busy book.");
                Theme::end_menu_detail();
            }
            if (wants(i18n::tr("Trade bubbles", i18n::fa_l_bubbles)) && layer_row(i18n::tr("Trade bubbles", i18n::fa_l_bubbles), rt_bubbles_, false, "Alt+2")) rt_bubbles_ = !rt_bubbles_;
            if (page == i18n::tr("Trade bubbles", i18n::fa_l_bubbles)) {
                // Per-market reference, with a separate upper size cap for exceptional groups.
                static const char* const kCaps[] = {"Compact", "Standard", "Large"};
                static constexpr float kCapPx[] = {bubble_size::kCompact, bubble_size::kStandard, bubble_size::kLarge};
                Theme::begin_menu_detail();
                if (Theme::menu_toggle("Auto market size", rt_auto_bubbles_)) {
                    rt_auto_bubbles_ = !rt_auto_bubbles_;
                    rt_bubble_scale_ = {};
                }
                ui_test::mark("rt.bubbles.auto");
                if (ImGui::IsItemHovered())
                    Theme::tooltip("Sizes and filters bubbles against this market's last minute of grouped trades.\nThe reference eases over about 20 seconds, so ordinary activity stays small and\nstandouts stand out. Recalibrate snaps it to now.");
                if (!rt_auto_bubbles_) {
                    if (Theme::menu_field("Minimum value ($)", &rt_min_notional_))
                        if (!std::isfinite(rt_min_notional_) || rt_min_notional_ < 1) rt_min_notional_ = 1;
                } else {
                    if (Theme::menu_item(UiIcon::Refresh, "Recalibrate sizes", nullptr, true, true)) rt_bubble_scale_ = {};
                    ui_test::mark("rt.bubbles.recalibrate");
                }
                const int cap = rt_bubble_cap_ == bubble_size::kCompact ? 0 : rt_bubble_cap_ == bubble_size::kLarge ? 2 : 1;
                const int next = Theme::menu_choice_row("Max size", "rt_bubble_cap", kCaps, 3, cap);
                if (next != cap) rt_bubble_cap_ = kCapPx[next];
                if (ImGui::IsItemHovered())
                    Theme::tooltip("How big the largest prints grow. Compact stops at 12 px, Standard at 24, Large at 32.");
                if (Theme::menu_field("Opacity", &rt_bubble_alpha_, "%.2f", 0.05f))
                    rt_bubble_alpha_ = std::clamp(rt_bubble_alpha_, 0.05f, 1.0f);
                if (ImGui::IsItemHovered())
                    Theme::tooltip("Fade every trade bubble (real-time and candle views) -\nkeep them as a background accent under the tape.");
                Theme::end_menu_detail();
            }
            if (page == i18n::tr("Resting depth heat", i18n::fa_l_resting)) {
                Theme::begin_menu_detail();
                if (Theme::menu_field("Opacity", &rt_cum_opacity_, "%.2f", 0.05f))
                    rt_cum_opacity_ = std::clamp(rt_cum_opacity_, 0.05f, 1.0f);
                if (ImGui::IsItemHovered())
                    Theme::tooltip("Fade the gold resting-depth rows when they compete with\nthe depth cells underneath.");
                Theme::end_menu_detail();
            }
            if (page == "Whale trail") {
                Theme::begin_menu_detail();
                static const char* const kWindows[] = {"5 s", "8 s", "15 s"};
                static constexpr int kWindowMs[] = {5000, 8000, 15000};
                int wi = rt_whale_window_ms_ <= 5000 ? 0 : rt_whale_window_ms_ <= 8000 ? 1 : 2;
                const int next_w = Theme::menu_choice_row("Link window", "rt_whale_window", kWindows, 3, wi);
                if (next_w != wi) rt_whale_window_ms_ = kWindowMs[next_w];
                if (ImGui::IsItemHovered())
                    Theme::tooltip("How far apart two large prints may be (time and 40 ticks of\nprice) and still count as one working order.");
                Theme::end_menu_detail();
            }
            if (page.empty()) {
            if (layer_row(i18n::tr("Trade-price line", i18n::fa_l_trade_line), rt_trade_line_, false, "Alt+3")) rt_trade_line_ = !rt_trade_line_;
            if (layer_row(i18n::tr("1s candles", i18n::fa_l_1s_candles), rt_candles_, false, "Alt+4")) {
                rt_candles_ = !rt_candles_;
                chart_type_ = rt_candles_ ? ChartType::Candles : ChartType::Line;
            }
            if (rt_candles_) {
                if (layer_row(i18n::tr("Heikin-Ashi", i18n::fa_l_heikin), rt_heikin_, false, "Alt+5")) rt_heikin_ = !rt_heikin_;
                if (ImGui::IsItemHovered())
                    Theme::tooltip("Smoothed real-time candles: the open is the midpoint of the previous\nHA candle, the close is the raw OHLC4. Trend structure without wick noise.");
            }
            render_realtime_cvd_layer();
            if (layer_row(i18n::tr("Est. liquidations", i18n::fa_l_est_liq), rt_liq_levels_enabled_, false, "Alt+7")) rt_liq_levels_enabled_ = !rt_liq_levels_enabled_;
            if (ImGui::IsItemHovered())
                Theme::tooltip("Estimated liquidation clusters: open interest split over leverage tiers\nusing the recent tape. Red bands are forced sells (longs), green forced buys (shorts).\nAn estimate, not real orders.");
            if (layer_row(i18n::tr("Resting depth heat", i18n::fa_l_resting), rt_cum_depth_enabled_, false, "Alt+8")) rt_cum_depth_enabled_ = !rt_cum_depth_enabled_;
            if (ImGui::IsItemHovered())
                Theme::tooltip("A second heat map: time-weighted resting depth per price (10-minute\nmemory). Gold rows are prices where orders have been sitting - shelves and\nwalls stand out from flicker. Hover a row for its size.");
            layer_section("ORDER BOOK SIGNALS");
            render_refill_layer_row();
            render_realtime_wall_layer();
            // [SELF-HOST] Whale trail: consecutive large same-side prints linked
            // into a path over the heat. Reads straight from the bubble archive.
            if (wants("Whale trail") && layer_row("Whale trail", rt_whale_trail_enabled_, false))
                rt_whale_trail_enabled_ = !rt_whale_trail_enabled_;
            if (ImGui::IsItemHovered())
                Theme::tooltip("Consecutive large same-side clips linked into a trail: a working\n"
                               "order shows up as a path, scattered small flow stays unlinked.\n"
                               "Reads the same grouped prints the trade bubbles draw.");
            layer_section("CROSS-VENUE");
            if (ctx_.ticker_mgr().has_venue("okx")) {
                if (wants("Venue basis (OKX)") && layer_row("Venue basis (OKX)", rt_basis_enabled_, false, "okx"))
                    rt_basis_enabled_ = !rt_basis_enabled_;
                if (ImGui::IsItemHovered())
                    Theme::tooltip("OKX perp last price vs this chart's mid, in bps, as a ribbon\n"
                                   "along the bottom. Premium above zero: OKX leads. Divergence\n"
                                   "spikes often mark where pressure started first.");
            } else if (wants("Venue basis (OKX)")) {
                Theme::menu_note("Cross-venue basis needs an OKX feed; this gateway\nserves Binance futures only.");
            }
            layer_section("EXECUTION");
            if (wants("Slippage cone") && layer_row("Slippage cone", rt_slip_enabled_, false))
                rt_slip_enabled_ = !rt_slip_enabled_;
            if (ImGui::IsItemHovered())
                Theme::tooltip("Walk the live book for a simulated market order and draw\n"
                               "where it would actually fill: the wedge between the touch\n"
                               "and the dashed line is the expected slippage, bps on the chip.");
            layer_section("EVENTS");
            if (wants("Layer events") && layer_row("Layer events", rt_events_enabled_, false))
                rt_events_enabled_ = !rt_events_enabled_;
            if (ImGui::IsItemHovered())
                Theme::tooltip("Corner feed of the recent layer events: cascades, threshold\n"
                               "liquidations, pulled walls, consumed refill levels. Collects\n"
                               "regardless of the alert toggles; the Real-time menu arms alerts.");
            if (page.empty())
                Theme::menu_note("Alt+1..0 toggles these layers anywhere:\n1 depth, 2 bubbles, 3 trade line, 4 candles, 5 Heikin-Ashi,\n6 CVD, 7 est. liq, 8 resting heat, 9 refill, 0 walls.");
            layer_section("LAYER PRESETS");
            {
                static const char* kPresets[] = {"Scalp", "Swing", "Research"};
                const int next = Theme::menu_choice_row("Layout", "rt_layer_preset", kPresets, 3,
                    rt_layer_preset_ >= 0 && rt_layer_preset_ <= 2 ? rt_layer_preset_ : -1);
                if (next >= 0 && next != rt_layer_preset_) {
                    rt_layer_preset_ = next;
                    apply_layer_preset(next);
                }
                if (ImGui::IsItemHovered())
                    Theme::tooltip("One-click layer layouts for the real-time view:\n"
                        "- Scalp: 1s hollow candles, walls + pulls, CVD, bubbles, fast alerts\n"
                        "- Swing: 1m candles, estimated liquidations + resting depth, noise off\n"
                        "- Research: everything on (walls table, peak-hold, trade line)\n"
                        "Applies instantly; keep tuning afterwards, the workspace remembers it.");
            }
            // [SELF-HOST] User-saved layer sets: click applies, right-click deletes.
            // A set snapshots every RT layer switch + the candle bucket size, so a
            // trader can keep a scalping view and a liquidation view side by side.
            {
                if (!layer_sets_.empty()) {
                    Theme::menu_separator();
                    Theme::menu_section("MY LAYER SETS");
                    std::vector<std::string> erase_names;
                    for (auto it = layer_sets_.begin(); it != layer_sets_.end(); ++it) {
                        const std::string name = it.key();
                        ImGui::PushID(name.c_str());
                        if (Theme::menu_item(UiIcon::Candles, name.c_str(), nullptr, true, true)) {
                            apply_layer_flags(it.value());
                            rt_layer_preset_ = -1;   // a custom set supersedes the fixed layouts
                        }
                        if (ImGui::IsItemHovered())
                            Theme::tooltip("Click: apply this set. Right-click: delete it.\nThe workspace remembers applied sets across reloads.");
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                            erase_names.push_back(name);
                        ImGui::PopID();
                    }
                    for (const auto& name : erase_names) layer_sets_.erase(name);
                }
                if (Theme::menu_item(UiIcon::Plus, "Save current as set\xe2\x80\xa6", nullptr, true, true))
                    layer_set_input_open_ = !layer_set_input_open_;
                if (ImGui::IsItemHovered())
                    Theme::tooltip("Name and save the layers exactly as they are now,\nthen bring them back with one click from this menu.");
                if (layer_set_input_open_) {
                    ImGui::SetNextItemWidth(158);
                    const bool enter = ImGui::InputTextWithHint("##layerset_name", "Set name",
                        layer_set_name_buf_, sizeof(layer_set_name_buf_));
                    const std::string candidate = layer_set_name_buf_;
                    const bool valid = !candidate.empty() && candidate.size() <= 24;
                    if ((valid && (enter || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) ||
                        (valid && Theme::menu_item(UiIcon::Check, "Save set", nullptr, true, true))) {
                        layer_sets_[candidate] = capture_layer_flags();
                        rt_layer_preset_ = -1;
                        layer_set_input_open_ = false;
                        layer_set_name_buf_[0] = '\0';
                    }
                }
            }
            }
            // [SELF-HOST] Slippage-cone detail page: renders outside the
            // page.empty() block (detail pages never render there - same
            // structure as the resting-heat and whale-trail pages).
            if (page == "Slippage cone") {
                Theme::begin_menu_detail();
                static const char* const kSizes[] = {"0.5", "1", "5", "10", "25"};
                static constexpr double kSizeVals[] = {0.5, 1.0, 5.0, 10.0, 25.0};
                int si = 2;
                for (int i = 0; i < 5; ++i)
                    if (std::abs(rt_slip_size_ - kSizeVals[i]) < 1e-9) si = i;
                const int next_s = Theme::menu_choice_row("Preset size", "rt_slip_preset", kSizes, 5, si);
                if (next_s != si) rt_slip_size_ = kSizeVals[next_s];
                if (ImGui::IsItemHovered())
                    Theme::tooltip("Quick sizes for the simulated market order.");
                float custom = float(rt_slip_size_);
                if (Theme::menu_field("Exact size", &custom, "%.1f", 1.0f))
                    rt_slip_size_ = std::clamp(double(custom), 0.1, 10000.0);
                if (ImGui::IsItemHovered())
                    Theme::tooltip("Market order size to simulate, in base units. A plus sign\n"
                                   "on the bps means the archived book ends before the size fills.");
                Theme::menu_note("Fills are volume-weighted over the stored rows at the\n"
                                 "touch - an estimate over the visible book, not the\n"
                                 "venue's full depth.");
                Theme::end_menu_detail();
            }
        } else if (!rt_mode_) {
            if (page == "Price levels") {
        layer_section("PRICE LEVELS");
        if (layer_row(i18n::tr("Daily VWAP (UTC)", i18n::fa_l_vwap), session_vwap_, false)) {
            session_vwap_ = !session_vwap_; reference_update_time_ = -1;
        }
        if (ImGui::IsItemHovered())
            Theme::tooltip("Current UTC day only, starting at midnight UTC.\nUses completed candle HLC3 and base volume.\nRight-click an earlier candle to anchor a longer VWAP.");
        if (layer_row(i18n::tr("Previous day high / low / close", i18n::fa_l_prev_day), previous_day_, false)) {
            previous_day_ = !previous_day_; reference_update_time_ = -1;
        }
        if (ImGui::IsItemHovered()) Theme::tooltip("Days start at midnight UTC.");
        if (layer_row(i18n::tr("Previous week high / low / close", i18n::fa_l_prev_week), previous_week_, false)) {
            previous_week_ = !previous_week_; reference_update_time_ = -1;
        }
        if (ImGui::IsItemHovered()) Theme::tooltip("Weeks start Monday, midnight UTC.");
        {
            const bool missing = (session_vwap_ && !session_vwap_data_.complete) ||
                                 (vwap_anchor_ms_ && !anchored_vwap_data_.complete) ||
                                 (previous_day_ && !previous_day_data_.complete) ||
                                 (previous_week_ && !previous_week_data_.complete);
            const bool time_views = ct_allows_time_overlays(chart_type_);
            if (vwap_anchor_ms_ || missing || !time_views) {
                Theme::begin_menu_detail();
                if (vwap_anchor_ms_ && Theme::menu_item(UiIcon::Close, "Clear anchored VWAP", nullptr, true, true)) {
                    vwap_anchor_ms_ = 0; anchored_vwap_data_.clear();
                }
                if (missing && candles().is_loading()) {
                    Theme::menu_note("Loading earlier candles\xe2\x80\xa6");
                } else if (missing) {
                    // Bounded, explicit read, ending at the observed replay clock.
                    if (Theme::menu_item(UiIcon::Refresh, "Load earlier candles", "history missing", !candles().is_loading(), true))
                        candles().request_historical(20160, reference_asof_);
                    if (ImGui::IsItemHovered())
                        Theme::tooltip("Levels stop where bars are missing.\nCoarse candles may not align to the period.");
                }
                if (!time_views) Theme::menu_note("Time-based chart views only.");
                Theme::end_menu_detail();
            }
        }
            } else {
                if (page.empty()) {
                    if (Theme::menu_item(UiIcon::ChevronRight, "Price levels", nullptr, true, true)) detail = "Price levels";
                    ui_test::mark("overlay.price");
                    Theme::menu_separator();
                }
                if (wants(i18n::tr("Order book depth", i18n::fa_l_depth))) {
        // OB Depth = orderbook depth heatmap (free). Right-click opens its settings.
        {
            const bool c = layer_row(i18n::tr("Order book depth", i18n::fa_l_depth), heatmap_enabled_, false);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                open_heatmap_settings_ = true;
                ImGui::CloseCurrentPopup();
            }
            if (c) heatmap_enabled_ = !heatmap_enabled_;
        }
        if (page == i18n::tr("Order book depth", i18n::fa_l_depth)) {
            Theme::begin_menu_detail();
            if (Theme::menu_item(UiIcon::Gear, "Depth settings\xe2\x80\xa6")) {
                open_heatmap_settings_ = true;
                ImGui::CloseCurrentPopup();
            }
            Theme::end_menu_detail();
        }
                }
                if (wants(i18n::tr("Trade bubbles (large prints)", i18n::fa_l_bubbles_large))) {
        // Trade bubbles on candles (free): large prints as bubbles. Real-time mode
        // (the pill beside the timeframes) draws EVERY trade; this is the subset
        // that clears the market-size floor, on every plan.
        if (layer_row(i18n::tr("Trade bubbles (large prints)", i18n::fa_l_bubbles_large), candle_bubbles_, false))
            candle_bubbles_ = !candle_bubbles_;
        if (ImGui::IsItemHovered())
            Theme::tooltip("Large trades drawn as bubbles on the candles: colored by taker buy/sell using your selected palette, sized on a compressed value scale.\n"
                           "Real-time mode (beside the timeframes) shows every trade with the order book behind it.");
        if (page == i18n::tr("Trade bubbles (large prints)", i18n::fa_l_bubbles_large)) options("Options##bubbles", [&] {
                if (Theme::menu_field(candle_bubble_min_ > 0 ? "Minimum value" : "Minimum value (0 = auto)", &candle_bubble_min_))
                    if (!std::isfinite(candle_bubble_min_) || candle_bubble_min_ < 0) candle_bubble_min_ = 0;
                if (ImGui::IsItemHovered()) {
                    const double floor = candle_bubble_floor();
                    if (floor > 0) Theme::tooltip("Quote value a trade needs to draw. 0 = auto: a fixed reference at 8x typical recorded trade value (currently %.4g).", floor);
                    else           Theme::tooltip("Quote value a trade needs to draw. 0 = auto: 8x the market's typical trade size over the last minute (still calibrating).");
                }
                if (Theme::menu_item(UiIcon::Refresh, "Recalibrate bubble sizes", nullptr, true, true)) {
                    candle_bubble_size_reference_ = 0; candle_bubble_auto_floor_ = 0; candle_bubble_scale_ = {}; candle_bubble_scale_since_ms_ = 0;
                    candle_bubble_history_.reset();
                }
                if (ctx_.replay_mgr().is_active() || EducationBoot::instance().is_pack())
                    Theme::menu_note("Replay bubbles show traversed trades.");
                else if (pair_.exchange != "binancef")
                    Theme::menu_note("Recent history: Binance futures only.");
                else if (!Entitlements::is_pro())
                    Theme::menu_note("Recent trade history requires Pro.");
                else {
                    if (Theme::menu_toggle("Recent trade history (6 h)", candle_bubble_history_enabled_)) {
                        candle_bubble_history_enabled_ = !candle_bubble_history_enabled_;
                        if (!candle_bubble_history_enabled_) candle_bubble_history_.reset();
                    }
                    if (ImGui::IsItemHovered())
                        Theme::tooltip("Strongest non-overlapping prints; zoom in for more.\nMinimum value filters records without resizing them.");
                    if (!candle_bubble_history_.error.empty() && Theme::menu_item(UiIcon::Refresh, "Retry history", nullptr, true, true))
                        candle_bubble_history_.reset();
                }
        });
                }
                if (wants("Volume profile (VPVR)")) {
        // VPVR = volume profile visible range (free). Right-click opens its settings.
        {
            const bool c = layer_row("Volume profile (VPVR)", vpvr_enabled_, false);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) open_vpvr_settings_ = true;
            if (c) {
                vpvr_enabled_ = !vpvr_enabled_;
                ctx_.vpvr_mgr().set_enabled(vpvr_enabled_);
                if (vpvr_enabled_) { vpvr_last_request_start_ = 0; vpvr_last_request_end_ = 0; }
            }
        }
        if (page == "Volume profile (VPVR)") {
            Theme::begin_menu_detail();
            if (Theme::menu_item(UiIcon::Gear, "Profile settings\xe2\x80\xa6")) {
                open_vpvr_settings_ = true;
                ImGui::CloseCurrentPopup();
            }
            Theme::end_menu_detail();
        }
                }
                if (page.empty()) render_refill_history_layer_row();
                absorption_row();
            }
        }
        const bool research = Entitlements::is_admin() || (!rt_mode_ && pair_.exchange == "binancef");
        if (research && page.empty()) {
            Theme::menu_separator();
            if (Theme::menu_item(UiIcon::ChevronRight, "Research tools", nullptr, true, true)) detail = "Research tools";
            ui_test::mark("overlay.research");
        }
        if (research && page == "Research tools") {
            if (!rt_mode_ && touch_odds_overlay::available(pair_))
                touch_odds_overlay::menu(pair_, touch_odds_enabled_, observed_rails_allowed(), touch_rails_off());
#ifdef EDGEDEPTH_EXPOSURE_V2_DEV
        // Exposure V2 = the server's recorded per-minute scenario bands (pilot,
        // Pro): where positions opened since capture began would sit under a
        // fixed buffer. Not the candle field above and never summed with it.
        if (layer_row("Exposure V2 bands (pilot)", pro && exposure_enabled_, !pro)) {
            if (!pro) ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Layer,
                                                     nullptr, "exposure_v2");
            else { exposure_enabled_ = !exposure_enabled_; exposure_history_.reset(); exposure_summary_.reset(); }
        }
        if (Theme::begin_menu_group("Exposure V2 settings")) {
            render_exposure_v2_settings();
            Theme::end_menu_group();
        }
#endif
            render_activity_settings();
            if (!rt_mode_ && pair_.exchange == "binancef") compression::menu(pair_, ctx_.replay_mgr().is_active());
        }
        if (page.empty() && Entitlements::hosted()) {
            Theme::menu_separator();
            render_bookmark_action();
        }
    }
    ImGui::EndPopup();
}
