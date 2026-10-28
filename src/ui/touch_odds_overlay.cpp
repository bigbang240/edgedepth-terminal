#include "ui/touch_odds_overlay.h"
#include "core/display_time_zone.h"
#include "core/i18n.h"
#include "core/research_url.h"
#include "core/touch_odds.h"
#include "core/liq_rails.h"
#include "core/heatmap_colormap.h"
#include "rendering/theme.h"
#include "ui/drawing/drawing_icons.h"
#include "ui/upsell_modal.h"
#include "implot.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace touch_odds_overlay {
namespace {
touch_odds::Snapshot snapshot;
bool have = false;
bool hosted = false;  // the web host mounts the bridge for admins only (server-side role check)
bool zone_hovered = false;  // last frame's zone-label hover: the cursor readout yields to its tooltip
bool rails_on = true;       // Liquidation rails layer, toggled in menu()
bool observed_on = false;   // rails fuel: observed Hyperliquid positions instead of the scenario map (admin)
constexpr int64_t kRailsHorizonMs = 24LL*3600*1000;
constexpr int kHistoryHorizon = 1;  // historical rails weigh fuel by the 4h chance

// Recorded history from the bridge, one point per candle. Absent, sealed and unreleased candles simply have
// no point; `sealed` counts the candles the forward tests still keep closed.
struct History {
    std::string key, venue, symbol, status;
    int64_t from = 0, to = 0, every = 0;
    std::vector<liq_rails::Point> points;
    std::vector<std::array<double, 3>> rv;
    touch_odds::LongRange model;  // horizons and distance range; mark and rv are set per point
    int sealed = 0;
};
History hist;
int hist_rev = -1;
std::string hist_asked;
int history_frame = -1, history_sealed_frame = -1;  // what the rails caption should mention this frame
struct HistoryCache { int rev = -1; uint64_t source = 0; long long binq = 0; std::vector<liq_rails::Cell> cells; double wmax = 0; } hcache;

bool parse_history(const nlohmann::json& j, History& h) {
    using namespace touch_odds::detail;
    if (!j.is_object() || !j.contains("key") || !j["key"].is_string() || !j.contains("venue") || !j["venue"].is_string()
        || !j.contains("symbol") || !j["symbol"].is_string() || !j.contains("status") || !j["status"].is_string()
        || !integer(j, "every", h.every) || !integer(j, "from", h.from) || !integer(j, "to", h.to) || h.every <= 0) return false;
    h.key = j["key"].get<std::string>(); h.venue = j["venue"].get<std::string>(); h.symbol = j["symbol"].get<std::string>();
    h.status = j["status"].get<std::string>();
    if (j.contains("sealed") && j["sealed"].is_number_integer()) h.sealed = j["sealed"].get<int>();
    if (h.status != "ok" || !j.contains("points") || !j["points"].is_array() || j["points"].empty()) return true;
    int64_t prev = 0;
    for (const auto& p : j["points"]) {
        liq_rails::Point pt; std::array<double, 3> rv{};
        int64_t asof = 0;
        if (!p.is_object() || !integer(p, "t", pt.t) || !integer(p, "asof", asof) || !number(p, "mark", pt.mark) || !(pt.mark > 0)
            || pt.t <= prev || pt.t % h.every || asof > pt.t || !p.contains("rv") || !p["rv"].is_array() || p["rv"].size() != 3) return false;
        for (int k = 0; k < 3; ++k) { if (!p["rv"][k].is_number() || !(p["rv"][k].get<double>() > 0)) return false; rv[k] = p["rv"][k].get<double>(); }
        h.points.push_back(pt); h.rv.push_back(rv); prev = pt.t;
    }
    if (!j.contains("model") || !j["model"].is_object()) return false;
    const auto& m = j["model"];
    nlohmann::json lr = {{"asof", h.points[0].t - 1}, {"hide_at", h.points[0].t}, {"mark", h.points[0].mark}, {"rv", h.rv[0]},
                         {"distance_range", m.value("distance_range", nlohmann::json())}, {"horizons", m.value("horizons", nlohmann::json())}};
    return touch_odds::parse_longrange(lr, h.model);
}
void read_history() {
#ifdef __EMSCRIPTEN__
    static int frame = -1;
    if (frame == ImGui::GetFrameCount()) return;
    frame = ImGui::GetFrameCount();
    const int rev = emscripten_run_script_int("window.__EDGEDEPTH_REACH_HISTORY_REV__|0");
    if (rev == hist_rev) return;
    hist_rev = rev;
    const char* raw = emscripten_run_script_string("JSON.stringify(window.__EDGEDEPTH_REACH_HISTORY__ || null)");
    const auto j = nlohmann::json::parse(raw ? raw : "null", nullptr, false);
    History h;
    hist = parse_history(j, h) ? std::move(h) : History{};
#endif
}
void ask_history(const std::string& venue, const std::string& symbol, int64_t from, int64_t to, int64_t every) {
    const std::string key = venue + "/" + symbol + "/" + std::to_string(from) + "/" + std::to_string(to) + "/" + std::to_string(every);
    if (key == hist_asked) return;
    hist_asked = key;
#ifdef __EMSCRIPTEN__
    const nlohmann::json q = {{"venue", venue}, {"symbol", symbol}, {"from", from}, {"to", to}, {"every", every}};
    emscripten_run_script(("window.__EDGEDEPTH_REACH_HISTORY_REQ__=" + q.dump() + ";").c_str());
#endif
}
void read() {
#ifdef __EMSCRIPTEN__
    static int frame = -1, rev = -1;
    if (frame == ImGui::GetFrameCount()) return;
    frame = ImGui::GetFrameCount();
    // Main-thread bridge shared by all charts; the host bumps the revision on every publish.
    const int next = emscripten_run_script_int("window.__EDGEDEPTH_REACH_REV__|0");
    if (next == rev) return;
    rev = next;
    hosted = rev > 0;
    const char* raw = emscripten_run_script_string("JSON.stringify(window.__EDGEDEPTH_REACH__ || null)");
    const auto j = nlohmann::json::parse(raw ? raw : "null", nullptr, false);
    have = touch_odds::parse(j, snapshot);
#endif
}
// Venue and symbol both: Bybit BTCUSDT must never draw Binance BTCUSDT's odds.
bool matches(const Terminal::Pair& pair) {
    return have && hosted && pair.exchange == snapshot.venue
        && research_url::normalize_symbol(pair.symbol) == snapshot.symbol;
}
// How a tooltip names the record behind the odds: the admin preview, or the record the release cites.
const char* record_note(const char* preview) {
    static char out[160];
    if (!snapshot.released()) return preview;
    std::snprintf(out, sizeof(out), "Frozen model. %s.", snapshot.release_label.c_str());
    return out;
}
void hhmm(int64_t ms, char* out, size_t size) {
    if (!DisplayTimeZone::instance().format(ms, TimeZoneFormat::TimeMinutes, out, size)) std::snprintf(out, size, "--:--");
}
// The P9 cascade chip (one family with the rails key in liqmap2_rails_overlay): a rounded chip in Inter, a status
// dot, the reading, and an info mark when something limits it. The reasons (bands untested here, long-range odds
// pending) moved off the chart into the hover (James 2026-10-11: the line beside the pill needed a repolish).
void cascade_chip(const ImVec2& plot, int64_t clock) {
    using namespace touch_odds;
    if (snapshot.access != "admin_preview" && snapshot.tier == Tier::None) return;
    const bool tier = snapshot.tier_at(clock);
    const char* band = snapshot.status == "no_current_frame" ? nullptr
        : snapshot.status == "forecast_unavailable" && snapshot.history ? "no released recorded forecast at this time"
        : missing_reason(snapshot.status);
    const char* lr = longrange_reason(snapshot.longrange_status);
    const char* value = !tier ? "Unavailable" : snapshot.tier == Tier::Top1 ? "Riskiest 1% of hours"
                      : snapshot.tier == Tier::Top5 ? "Riskiest 5% of hours" : "Not in the riskiest 5%";
    const ImVec4 tone = tier && snapshot.tier == Tier::Top1 ? Theme::Tokens::WARN
                      : tier && snapshot.tier == Tier::Top5 ? Theme::Tokens::TX1 : Theme::Tokens::TX3;
    const char* label = "Downside cascade 1 h";
    auto* dl = ImPlot::GetPlotDrawList();
    constexpr float h = 22.0f, pad = 9.0f, gap = 7.0f, dot = 3.0f;
    const bool info = band || lr || !tier;
    // Inter at the mono figures' size, as the rails key.
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const float px = ImGui::GetFontSize();
    ImGui::PopFont();
    Theme::push_font(Theme::Fonts::ui(), px);
    const ImVec2 ls = ImGui::CalcTextSize(label), vs = ImGui::CalcTextSize(value);
    const float w = pad + dot*2.0f + gap + ls.x + gap + vs.x + (info ? std::round(px*0.84f) : 0.0f) + pad;
    const ImVec2 a(plot.x + 10.0f, plot.y + 58.0f), b(std::round(a.x + w), a.y + h);  // below the status chip
    const ImVec4 fill = tier && snapshot.tier == Tier::Top1 ? ImVec4(Theme::Tokens::WARN.x, Theme::Tokens::WARN.y, Theme::Tokens::WARN.z, 0.10f)
                                                             : ImVec4(Theme::Tokens::PANEL.x, Theme::Tokens::PANEL.y, Theme::Tokens::PANEL.z, 0.92f);
    dl->AddRectFilled(a, b, Theme::u32(fill), Theme::Radius::R2);
    dl->AddRect(a, b, Theme::u32(tier && snapshot.tier == Tier::Top1 ? ImVec4(tone.x, tone.y, tone.z, 0.45f) : Theme::Tokens::BD2),
                Theme::Radius::R2, 0, 1.0f);
    Theme::chart_chip(a, b);
    const float cy = a.y + h*0.5f;
    float x = a.x + pad + dot;
    if (tier) dl->AddCircleFilled(ImVec2(x, cy), dot, Theme::u32(tone), 12);
    else dl->AddCircle(ImVec2(x, cy), dot - 0.5f, Theme::u32(Theme::Tokens::TX3), 12, 1.2f);
    x += dot + gap;
    dl->AddText(ImVec2(x, std::round(cy - ls.y*0.5f)), Theme::u32(Theme::Tokens::TX2), label);
    x += ls.x + gap;
    dl->AddText(ImVec2(x, std::round(cy - vs.y*0.5f)), Theme::u32(tier ? tone : Theme::Tokens::TX3), value);
    x += vs.x + gap;
    ImGui::PopFont();
    if (info) drawing::draw_ui_icon(dl, drawing::UiIcon::Info, ImVec2(x + std::round(px*0.42f), cy), std::round(px*0.42f), Theme::u32(Theme::Tokens::TX3), 1.2f);
    const auto mouse = ImGui::GetMousePos();
    if (!ImPlot::IsPlotHovered() || mouse.x < a.x || mouse.x > b.x || mouse.y < a.y || mouse.y > b.y) return;
    Theme::begin_tooltip();
    if (tier) {
        char from[16], to[16];
        hhmm(snapshot.hour_start, from, sizeof(from)); hhmm(snapshot.hour_end, to, sizeof(to));
        ImGui::Text("P9 cascade tier, %s to %s", from, to);
        ImGui::TextUnformatted("A rank, not a probability: this market's forecast chance of a downside cascade in the hour "
                               "(price down 5%+ in 15 min while open interest falls 3%+) against P9's frozen cutoffs "
                               "for the riskiest 1% and 5% of market-hours. It does not say price will fall.");
    } else if (snapshot.venue != "binancef") {
        ImGui::TextUnformatted("No P9 tier: the cascade model is fitted on Binance Futures markets only.");
    } else {
        ImGui::TextUnformatted("No current P9 tier: outside the fitted market set, or this hour is not published yet.");
    }
    if (band || lr) {
        ImGui::Separator();
        if (band) ImGui::TextDisabled("Touch odds: %s", band);
        if (lr) ImGui::TextDisabled("%s", lr);
    }
    ImGui::TextDisabled("Research preview; forward test first look 1 Dec 2026.");
    Theme::end_tooltip();
}
}  // namespace

// Every venue shows the admin row: an unsupported venue draws its reason rather than nothing.
bool chip_visible(const Terminal::Pair& pair, bool replay) { return snapshot.history == replay && matches(pair); }

bool available(const Terminal::Pair&) {
    read();
    return hosted;
}

int64_t window_end(const Terminal::Pair& pair, int64_t clock) {
    read();
    if (rails_active(pair, clock, false)) return snapshot.longrange.asof + kRailsHorizonMs;
    if (rails_on && !snapshot.history && matches(pair) && snapshot.access == "locked") return clock + kRailsHorizonMs;
    return matches(pair) && snapshot.bands_at(clock) ? snapshot.expires_at : 0;
}

bool rails_active(const Terminal::Pair& pair, int64_t clock, bool replay) {
    read();
    return rails_on && snapshot.history == replay && matches(pair) && snapshot.longrange.at(clock);
}

bool owns_forward_area(const Terminal::Pair& pair, int64_t clock, bool replay) {
    return rails_active(pair, clock, replay) || (rails_on && !replay && !snapshot.history && matches(pair) && snapshot.access == "locked");
}

void step_aside() { zone_hovered = false; }

bool observed_fuel_selected() { return observed_on; }

void menu(const Terminal::Pair& pair, bool& enabled, bool observed_allowed, const char* rails_off) {
    if (!available(pair)) return;
    // An admin preview carries the "admin" tag; its sub-rows sit under it while it is on. "Touch-odds rails"
    // is this layer's own fuel-times-chance rails, not the V2 map's rails.
    const bool preview = snapshot.access == "admin_preview";
    if (Theme::menu_layer("Touch odds (20 min)", enabled, false, preview ? "admin" : nullptr)) enabled = !enabled;
    if (ImGui::IsItemHovered())
        Theme::tooltip("Calibrated touch chances: 20 min bands; 1h, 4h, 24h at the cursor.\n"
                       "Bands overlap: never add them.");
    if (!enabled) return;
    Theme::begin_menu_detail();
    // Where the rails cannot draw (the V2 rails hold the forward area on every live chart that has V2, James
    // 2026-10-15: the switches "did nothing"), one line says why instead of switches that change nothing.
    if (rails_off) {
        Theme::menu_note(rails_off);
    } else {
        if (Theme::menu_toggle(i18n::tr("Touch-odds rails", i18n::fa_touch_rails), rails_on)) rails_on = !rails_on;
        if (ImGui::IsItemHovered())
            Theme::tooltip("Fuel x chance of touch by 1h, 4h and 24h, in place of the bands. Not a magnet.\n"
                           "Fuel: the scenario map, or observed positions on Hyperliquid charts.");
        if (rails_on && observed_allowed && Theme::menu_toggle(i18n::tr("Fuel: observed HL positions", i18n::fa_fuel_observed), observed_on))
            observed_on = !observed_on;
    }
    Theme::end_menu_detail();
}

// Long-range readout beside the cursor: the frozen 1h/4h/24h touch odds for the hovered level,
// measured from the same recorded mark in live and replay (0.5% to 10% away only).
void crosshair(const ImVec2& pos, const ImVec2& size, int64_t clock) {
    const auto& lr = snapshot.longrange;
    const double reference = lr.mark;
    if (!lr.at(clock) || !(reference > 0) || !ImPlot::IsPlotHovered() || zone_hovered) return;
    const double level = ImPlot::GetPlotMousePos().y;
    double p[3];
    for (int k = 0; k < 3; ++k) if ((p[k] = touch_odds::probability(lr, lr.horizons[k], level)) < 0) return;
    auto pct = [](double v, char* out, size_t n) {
        if (v < 0.01) std::snprintf(out, n, "<1%%"); else if (v > 0.99) std::snprintf(out, n, ">99%%"); else std::snprintf(out, n, "%.0f%%", 100.0*v);
    };
    char a[8], b[8], c[8], text[128], at[16];
    hhmm(lr.asof, at, sizeof(at));
    pct(p[0], a, sizeof(a)); pct(p[1], b, sizeof(b)); pct(p[2], c, sizeof(c));
    std::snprintf(text, sizeof(text), "%sTOUCH%s from %s  %+.2f%%  1h %s  4h %s  24h %s", snapshot.history ? "RECORDED " : "",
                  snapshot.price_kind == "trade" ? " (trade price)" : "", at, 100.0*(level/reference - 1.0), a, b, c);
    auto* dl = ImPlot::GetPlotDrawList();
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const ImVec2 tsz = ImGui::CalcTextSize(text), mouse = ImGui::GetMousePos();
    float x = mouse.x + 16.0f;
    if (x + tsz.x + 12.0f > pos.x + size.x) x = mouse.x - 16.0f - tsz.x - 12.0f;
    x = std::clamp(x, pos.x + 2.0f, std::max(pos.x + 2.0f, pos.x + size.x - tsz.x - 14.0f));
    const float y = std::clamp(mouse.y - tsz.y*0.5f - 3.0f, pos.y + 2.0f, pos.y + size.y - tsz.y - 8.0f);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + tsz.x + 12.0f, y + tsz.y + 6.0f), Theme::u32(Theme::Tokens::PANEL, 0.94f));
    dl->AddRect(ImVec2(x, y), ImVec2(x + tsz.x + 12.0f, y + tsz.y + 6.0f), Theme::u32(Theme::Tokens::REST, 0.55f), 0.0f, 0, 1.0f);
    dl->AddText(ImVec2(x + 6.0f, y + 3.0f), Theme::u32(Theme::Tokens::TX1), text);
    ImGui::PopFont();
}

void zone_labels(const Terminal::Pair& pair, int64_t clock, const char* price_fmt,
                 std::vector<touch_zones::Fuel> fuel, bool replay) {
    read();
    if (snapshot.history != replay) return;
    zone_hovered = false;
    const auto& lr = snapshot.longrange;
    // The zone test (zones did not change these odds) was run on Binance 5m only.
    if (!matches(pair) || !lr.at(clock) || snapshot.venue != "binancef") return;
    const auto zones = touch_zones::strongest(std::move(fuel), lr.mark, 3);
    if (zones.empty()) return;
    const auto pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    auto* dl = ImPlot::GetPlotDrawList();
    const auto mouse = ImGui::GetMousePos();
    const bool hovered = ImPlot::IsPlotHovered();
    auto pct = [](double v, char* out, size_t n) {
        if (v < 0.01) std::snprintf(out, n, "<1%%"); else if (v > 0.99) std::snprintf(out, n, ">99%%"); else std::snprintf(out, n, "%.0f%%", 100.0*v);
    };
    float used[8]; int n_used = 0;
    const touch_zones::Zone* hover = nullptr; double hover_p[3]{};
    ImPlot::PushPlotClipRect();
    ImGui::PushFont(Theme::Fonts::mono_sm());
    for (const auto& z : zones) {
        double p[3];
        bool ok = true;
        for (int k = 0; k < 3; ++k) ok = ok && (p[k] = touch_odds::probability(lr, lr.horizons[k], z.price)) >= 0;
        if (!ok) continue;
        const float y = ImPlot::PlotToPixels(0.0, z.price).y;
        if (y < pos.y + 4 || y > pos.y + size.y - 4) continue;
        char a[8], b[8], c[8], text[64];
        pct(p[2], a, sizeof(a)); pct(p[1], b, sizeof(b)); pct(p[0], c, sizeof(c));
        std::snprintf(text, sizeof(text), "TOUCH 1h %s  4h %s  24h %s", c, b, a);  // the cursor readout's order
        const ImVec2 tsz = ImGui::CalcTextSize(text);
        bool overlaps = false;
        for (int k = 0; k < n_used; ++k) overlaps = overlaps || std::abs(used[k] - y) < tsz.y + 6.0f;
        if (overlaps || n_used >= 8) continue;
        const float x1 = pos.x + size.x - 8.0f, x0 = x1 - tsz.x - 12.0f, y0 = y - tsz.y*0.5f - 3.0f, y1 = y + tsz.y*0.5f + 3.0f;
        dl->AddLine(ImVec2(x0 - 10.0f, y), ImVec2(x0, y), Theme::u32(Theme::Tokens::REST, 0.70f), 1.0f);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), Theme::u32(Theme::Tokens::PANEL, 0.94f));
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), Theme::u32(Theme::Tokens::REST, 0.70f), 0.0f, 0, 1.0f);
        dl->AddText(ImVec2(x0 + 6.0f, y - tsz.y*0.5f), Theme::u32(Theme::Tokens::TX1), text);
        used[n_used++] = y;
        if (hovered && mouse.x >= x0 && mouse.x <= x1 && mouse.y >= y0 && mouse.y <= y1) { hover = &z; std::copy(p, p + 3, hover_p); }
    }
    ImGui::PopFont();
    ImPlot::PopPlotClipRect();
    if (!hover) return;
    zone_hovered = true;
    char price[48], mark[48], at[48], a[8], b[8], c[8];
    std::snprintf(mark, sizeof(mark), price_fmt, lr.mark);
    if (!DisplayTimeZone::instance().format(lr.asof, TimeZoneFormat::DateTimeMinutes, at, sizeof(at)))
        std::snprintf(at, sizeof(at), "--");
    std::snprintf(price, sizeof(price), price_fmt, hover->price);
    pct(hover_p[0], c, sizeof(c)); pct(hover_p[1], b, sizeof(b)); pct(hover_p[2], a, sizeof(a));
    Theme::begin_tooltip();
    ImGui::Text("Scenario zone at %s (%+.2f%% from forecast mark %s)", price, 100.0*(hover->price/lr.mark - 1.0), mark);
    ImGui::Text("Forecast from %s. Horizons start at that time.", at);
    ImGui::Text("Chance price touches it: 1h %s, 4h %s, 24h %s.", c, b, a);
    ImGui::TextUnformatted("The zone is estimated liquidation fuel from candles, not observed positions. In our");
    ImGui::TextUnformatted("registered test the zones did not change these odds; they come from volatility alone.");
    ImGui::TextUnformatted("Reaching a zone also did not make a further 1% run more likely in a second test.");
    ImGui::TextUnformatted(record_note("Frozen model; live-window test pending. Admin preview."));
    Theme::end_tooltip();
}

// Liquidation rails (see core/liq_rails.h): the forward area splits into three columns, "by 1h",
// "by 4h" and "by 24h" from the forecast. A rail's colour and weight in each column is its fuel times
// that horizon's touch chance, relative to the brightest rail on screen, so the picture fans out from
// near levels (lit early) to far ones (lit only by 24h). The strongest rails per side get odds labels.
void rails(const Terminal::Pair& pair, int64_t clock, const char* price_fmt,
           const liq_rails::FuelSource& src, bool replay) {
    zone_hovered = false;
    if (!rails_active(pair, clock, replay)) return;
    const auto& lr = snapshot.longrange;
    const bool observed = src.kind == liq_rails::FuelKind::Observed;
    const bool modelled = src.kind == liq_rails::FuelKind::Modelled;
    double dropped = 0;
    const auto fuel = liq_rails::standing_at(src, lr.mark, &dropped);
    const auto pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    const auto lim = ImPlot::GetPlotLimits();
    const double start = std::max<double>(clock, lr.asof);
    if (start < lim.X.Min || start >= lim.X.Max) return;  // panned away from now
    const float x0 = ImPlot::PlotToPixels(start, 0.0).x, x3 = pos.x + size.x - 4.0f;
    if (x3 - x0 < 120.0f) return;  // no readable forward area
    // Rails about 9 px apart at the current zoom.
    const double px_per_log = std::abs(ImPlot::PlotToPixels(0.0, lr.mark).y - ImPlot::PlotToPixels(0.0, lr.mark*1.01).y)/std::log(1.01);
    if (!(px_per_log > 0)) return;
    const auto all = liq_rails::build(fuel, lr.mark, lr.dmin, lr.dmax, std::max(0.0005, 9.0/px_per_log),
                                      [&](double level, int h) { return touch_odds::probability(lr, lr.horizons[h], level); });
    double wmax = 0;
    for (const auto& r : all) for (double w : r.w) wmax = std::max(wmax, w);
    if (!(wmax > 0)) return;

    auto* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    const float col_w = (x3 - x0)/3.0f, top = pos.y + 26.0f, bottom = pos.y + size.y;
    const char* heads[3] = {"BY 1H", "BY 4H", "BY 24H"};
    ImGui::PushFont(Theme::Fonts::mono_sm());
    dl->AddLine(ImVec2(x0, pos.y), ImVec2(x0, bottom), Theme::u32(Theme::Tokens::REST, 0.30f), 1.0f);
    for (int h = 0; h < 3; ++h) {
        const float a = x0 + col_w*h;
        if (h > 0) dl->AddLine(ImVec2(a, top), ImVec2(a, bottom), Theme::u32(Theme::Tokens::REST, 0.14f), 1.0f);
        const ImVec2 tsz = ImGui::CalcTextSize(heads[h]);
        dl->AddText(ImVec2(a + (col_w - tsz.x)*0.5f, pos.y + 8.0f), Theme::u32(Theme::Tokens::TX3), heads[h]);
    }
    // Each rail brightens along time: from nothing at now to its 1h weight at the end of the first
    // column, then on to its 4h and 24h weights, so the picture fans out as reach grows.
    auto shade = [&](double w, float glow) -> std::pair<ImU32, ImU32> {
        const float t = static_cast<float>(std::pow(std::clamp(w/wmax, 0.0, 1.0), 0.35));
        uint8_t cr, cg, cb;
        HeatmapColormap::apply(HeatmapColormap::Type::Liquidation, 0.42f + 0.58f*t, cr, cg, cb);
        const float a = t < 0.08f ? 0.0f : 0.30f + 0.70f*t;
        return {IM_COL32(cr, cg, cb, static_cast<int>(255.0f*a)), IM_COL32(cr, cg, cb, static_cast<int>(255.0f*a*glow))};
    };
    for (const auto& r : all) {
        const float y = ImPlot::PlotToPixels(0.0, r.price).y;
        if (y < top || y > bottom || std::pow(r.w[2]/wmax, 0.35) < 0.08) continue;
        const float half = r.w[1]/wmax >= 0.25 ? 2.25f : 1.25f;
        for (int h = 0; h < 3; ++h) {
            const auto [c0, g0] = shade(h == 0 ? 0.0 : r.w[h - 1], 0.20f);
            const auto [c1, g1] = shade(r.w[h], 0.20f);
            const float a = x0 + col_w*h, b = x0 + col_w*(h + 1);
            dl->AddRectFilledMultiColor(ImVec2(a, y - half - 4.0f), ImVec2(b, y + half + 4.0f), g0, g1, g1, g0);
            dl->AddRectFilledMultiColor(ImVec2(a, y - half), ImVec2(b, y + half), c0, c1, c1, c0);
        }
    }

    // Odds labels on the strongest rails at 4h, two per side, never overlapping.
    auto pct = [](double v, char* out, size_t n) {
        if (v < 0.01) std::snprintf(out, n, "<1%%"); else if (v > 0.99) std::snprintf(out, n, ">99%%"); else std::snprintf(out, n, "%.0f%%", 100.0*v);
    };
    const auto mouse = ImGui::GetMousePos();
    const bool hovered = ImPlot::IsPlotHovered();
    const liq_rails::Rail* hover = nullptr;
    std::vector<liq_rails::Rail> labelled;
    for (int side : {1, -1}) for (const auto& r : liq_rails::strongest(all, side, 1, 2, 0.01)) labelled.push_back(r);
    float used[4]; int n_used = 0;
    for (const auto& r : labelled) {
        const float y = ImPlot::PlotToPixels(0.0, r.price).y;
        if (y < top + 8.0f || y > bottom - 8.0f) continue;
        char a[8], text[48];
        pct(r.p[1], a, sizeof(a));
        std::snprintf(text, sizeof(text), "%s 4h %s", r.side > 0 ? "SHORTS" : "LONGS", a);
        const ImVec2 tsz = ImGui::CalcTextSize(text);
        bool overlaps = false;
        for (int k = 0; k < n_used; ++k) overlaps = overlaps || std::abs(used[k] - y) < tsz.y + 8.0f;
        if (overlaps) continue;
        const float bx1 = x3 - 2.0f, bx0 = std::max(x0 + 2.0f, bx1 - tsz.x - 24.0f), by0 = y - tsz.y*0.5f - 3.0f, by1 = y + tsz.y*0.5f + 3.0f;
        dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), Theme::u32(Theme::Tokens::PANEL, 0.94f), Theme::Radius::R2);
        dl->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1), Theme::u32(Theme::Tokens::REST, 0.70f), Theme::Radius::R2, 0, 1.0f);
        dl->AddRectFilled(ImVec2(bx0 + 6.0f, y - 3.0f), ImVec2(bx0 + 12.0f, y + 3.0f), shade(r.w[1], 0.0f).first, 1.0f);
        dl->AddText(ImVec2(bx0 + 17.0f, y - tsz.y*0.5f), Theme::u32(Theme::Tokens::TX1), text);
        used[n_used++] = y;
        if (hovered && mouse.x >= bx0 && mouse.x <= bx1 && mouse.y >= by0 && mouse.y <= by1) hover = &r;
    }

    // Say what the picture is, naming the fuel, above the scenario caption (bottom left).
    char caption[280], extra[120];
    const int frame = ImGui::GetFrameCount();
    std::snprintf(extra, sizeof(extra), "%s%s",
                  history_frame == frame ? " \xc2\xb7 history: recorded 4h chance" : history_sealed_frame == frame ? " \xc2\xb7 history sealed until the forward look" : "",
                  snapshot.access == "free_taste" ? " \xc2\xb7 free here, Pro on every market" : "");
    liq_rails::caption(src, clock, extra, caption, sizeof(caption));
    const ImVec2 cs = ImGui::CalcTextSize(caption), at(pos.x + 10.0f, pos.y + size.y - 2.0f*(cs.y + 8.0f) - 2.0f);
    dl->AddRectFilled(ImVec2(at.x - 4.0f, at.y - 2.0f), ImVec2(at.x + cs.x + 4.0f, at.y + cs.y + 2.0f), Theme::u32(Theme::Tokens::PANEL, 0.80f));
    dl->AddText(at, Theme::u32(Theme::Tokens::TX3), caption);
    ImGui::PopFont();
    ImPlot::PopPlotClipRect();
    if (!hover) return;
    zone_hovered = true;
    char price[48], mark[48], when[48], a[8], b[8], c[8];
    std::snprintf(price, sizeof(price), price_fmt, hover->price);
    std::snprintf(mark, sizeof(mark), price_fmt, lr.mark);
    if (!DisplayTimeZone::instance().format(lr.asof, TimeZoneFormat::DateTimeMinutes, when, sizeof(when)))
        std::snprintf(when, sizeof(when), "--");
    pct(hover->p[0], a, sizeof(a)); pct(hover->p[1], b, sizeof(b)); pct(hover->p[2], c, sizeof(c));
    Theme::begin_tooltip();
    ImGui::Text("%s liquidation rail at %s (%+.2f%% from forecast mark %s)", hover->side > 0 ? "Short" : "Long", price,
                100.0*(hover->price/lr.mark - 1.0), mark);
    ImGui::Text("Chance price touches it: 1h %s, 4h %s, 24h %s, from %s.", a, b, c, when);
    auto money = [](double v, char* out, size_t n) {
        if (v >= 1e9) std::snprintf(out, n, "$%.2fB", v/1e9); else if (v >= 1e6) std::snprintf(out, n, "$%.2fM", v/1e6);
        else if (v >= 1e3) std::snprintf(out, n, "$%.0fK", v/1e3); else std::snprintf(out, n, "$%.0f", v);
    };
    if (modelled) {
        char usd[24], off[24];
        money(hover->fuel, usd, sizeof(usd));
        ImGui::Text("V2 map (%s): about %s of estimated %s liquidations in this rail.", src.claim ? src.claim : "untested",
                    usd, hover->side > 0 ? "short" : "long");
        ImGui::TextUnformatted("Brightness is that chance times the V2 map's estimated dollars, relative to the brightest");
        ImGui::TextUnformatted("rail on screen. The V2 map estimates positions from this venue's open interest; it is not");
        ImGui::TextUnformatted("observed positions. Not a magnet: the odds come from volatility, not this fuel.");
        if (dropped > 0) {
            money(dropped, off, sizeof(off));
            ImGui::Text("%s of the frame's levels sit behind price since it was published and are left out.", off);
        }
    } else if (observed) {
        char usd[24], off[24];
        money(hover->fuel, usd, sizeof(usd));
        ImGui::Text("Observed on Hyperliquid: about %s of sampled %s positions liquidate in this rail.", usd, hover->side > 0 ? "short" : "long");
        ImGui::TextUnformatted("Brightness is that chance times the observed notional, relative to the brightest rail on");
        ImGui::TextUnformatted("screen. These are real liquidation prices, but only the positions the census has located,");
        ImGui::TextUnformatted("so the full exposure is larger. Not a magnet: the odds come from volatility, not these positions.");
        if (dropped > 0) {
            money(dropped, off, sizeof(off));
            ImGui::Text("%s of sampled positions sit on the wrong side of price (stale) and are left out.", off);
        }
    } else {
        ImGui::TextUnformatted("Brightness is that chance times the scenario map's estimated fuel here, relative to the");
        ImGui::TextUnformatted("brightest rail on screen. The fuel assumes leverage from candles; it is not observed");
        ImGui::TextUnformatted("positions or dollars, and in our registered Binance test it did not change the odds.");
        ImGui::TextUnformatted("So a bright rail is a lot of estimated fuel within likely reach, not a magnet.");
        if (src.fallback) ImGui::Text("The chosen fuel is not usable now (%s); the scenario map stands in.", src.fallback);
    }
    ImGui::TextUnformatted(record_note("Frozen touch model; live-window test pending. Admin preview."));
    Theme::end_tooltip();
}

// Locked (a released venue, a free viewer, not a free market): where the rails would be, a quiet panel over a
// few ghost rails says what Pro adds and where it is free, with one button into the shared upsell. No forecast
// reaches this viewer, so nothing here is real data.
void locked_panel(const ImVec2& pos, const ImVec2& size) {
    const auto lim = ImPlot::GetPlotLimits();
    const double now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (now < lim.X.Min || now >= lim.X.Max) return;
    const float x0 = ImPlot::PlotToPixels(now, 0.0).x, x3 = pos.x + size.x - 4.0f;
    if (x3 - x0 < 150.0f) return;
    auto* dl = ImPlot::GetPlotDrawList();
    const float h = size.y;
    for (int k = 0; k < 9; ++k) {  // ghost rails: fixed shapes, no data
        const float y = pos.y + h*(0.14f + 0.085f*k), len = (x3 - x0)*(0.35f + 0.07f*((k*5) % 9));
        dl->AddRectFilled(ImVec2(x0, y - 1.0f), ImVec2(x0 + len, y + 1.0f), Theme::u32(Theme::Tokens::REST, 0.10f + 0.03f*(k % 3)));
    }
    std::string free_at;
    for (const auto& f : snapshot.free_symbols) { std::string u = f; for (char& c : u) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32); free_at += (free_at.empty() ? "" : ", ") + u; }
    char line2[96];
    std::snprintf(line2, sizeof(line2), free_at.empty() ? "Pro: every market" : "Free on %s, Pro everywhere", free_at.c_str());
    const char* title = "LIQUIDATION RAILS";
    const char* line1 = "Touch odds x liquidation fuel";
    const char* cta = "Unlock with Pro";
    ImGui::PushFont(Theme::Fonts::mono_sm());
    const ImVec2 t1 = ImGui::CalcTextSize(title), t2 = ImGui::CalcTextSize(line1), t3 = ImGui::CalcTextSize(line2), tc = ImGui::CalcTextSize(cta);
    // Sized to its words and anchored to the right edge: it may reach left over the chart rather than clip.
    const float w = std::max({t1.x, t2.x, t3.x, tc.x + 20.0f}) + 28.0f;
    const float ph = t1.y + t2.y + t3.y + tc.y + 50.0f, cy = pos.y + h*0.5f;
    const ImVec2 a(x3 - 6.0f - w, cy - ph*0.5f), b(x3 - 6.0f, cy + ph*0.5f);
    dl->AddRectFilled(a, b, Theme::u32(Theme::Tokens::PANEL, 0.96f), Theme::Radius::R2);
    dl->AddRect(a, b, Theme::u32(Theme::Tokens::REST, 0.70f), Theme::Radius::R2, 0, 1.0f);
    float y = a.y + 12.0f;
    dl->AddText(ImVec2(a.x + 14.0f, y), Theme::u32(Theme::Tokens::TX1), title); y += t1.y + 6.0f;
    dl->AddText(ImVec2(a.x + 14.0f, y), Theme::u32(Theme::Tokens::TX2), line1); y += t2.y + 4.0f;
    dl->AddText(ImVec2(a.x + 14.0f, y), Theme::u32(Theme::Tokens::TX3), line2); y += t3.y + 10.0f;
    const ImVec2 ba(a.x + 14.0f, y), bb(a.x + 14.0f + tc.x + 20.0f, y + tc.y + 8.0f);
    const auto mouse = ImGui::GetMousePos();
    const bool over = ImPlot::IsPlotHovered() && mouse.x >= ba.x && mouse.x <= bb.x && mouse.y >= ba.y && mouse.y <= bb.y;
    dl->AddRectFilled(ba, bb, Theme::u32(Theme::Tokens::BRAND, over ? 1.0f : 0.88f), Theme::Radius::R2);
    dl->AddText(ImVec2(ba.x + 10.0f, ba.y + 4.0f), Theme::u32(Theme::Tokens::BRAND_INK), cta);
    ImGui::PopFont();
    if (over) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            ui::UpsellModal::instance().open(ui::UpsellModal::Trigger::Layer,
                free_at.empty() ? "Liquidation rails and touch odds on every market are part of Pro."
                                : "Liquidation rails and touch odds on every market are part of Pro. Try them free on the showcase market.",
                "liq_rails");
    }
}

bool rails_history_ready(const Terminal::Pair& pair, bool replay) {
    read(); read_history();
    return rails_on && !replay && hosted && !hist.points.empty() && hist.venue == pair.exchange
        && hist.symbol == research_url::normalize_symbol(pair.symbol);
}

void rails_history(const Terminal::Pair& pair, uint64_t source_key, double bw,
                   const std::function<std::vector<liq_rails::Run>()>& runs_of, int64_t tf_ms, bool replay) {
    read(); read_history();
    // Saved forecasts are Binance's; only a viewer the live odds already reach asks for them.
    if (!rails_on || replay || !hosted || pair.exchange != "binancef" || !matches(pair) || snapshot.access == "locked"
        || snapshot.access == "not_released") return;
    const auto lim = ImPlot::GetPlotLimits();
    const auto pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t step = 300000, every = std::max<int64_t>(step, tf_ms/step*step);
    const int64_t to = std::min<int64_t>(static_cast<int64_t>(lim.X.Max)/every + 1, now/every + 1)*every;
    int64_t from = static_cast<int64_t>(std::max(0.0, lim.X.Min))/every*every;
    from = from/(50*every)*(50*every);  // whole chunks, so panning does not ask again for every pixel
    if ((to - from)/every > 600) from = to - 600*every;
    if (from <= 0 || to <= from) return;
    ask_history(pair.exchange, research_url::normalize_symbol(pair.symbol), from, to, every);
    if (hist.venue != pair.exchange || hist.symbol != research_url::normalize_symbol(pair.symbol) || hist.every != every) return;
    if (hist.points.empty()) { if (hist.sealed > 0) history_sealed_frame = ImGui::GetFrameCount(); return; }
    // Rows about 9 px apart at this zoom, as the forward rails.
    const double ref = hist.points.back().mark;
    const double px_per_log = std::abs(ImPlot::PlotToPixels(0.0, ref).y - ImPlot::PlotToPixels(0.0, ref*1.01).y)/std::log(1.01);
    if (!(px_per_log > 0) || !(bw > 0)) return;
    const double bin = std::max(0.0005, 9.0/px_per_log);
    const long long binq = std::llround(std::log(bin)*10.0);
    if (hcache.rev != hist_rev || hcache.source != source_key || hcache.binq != binq) {
        const std::vector<liq_rails::Run> runs = runs_of();
        auto& lr = hist.model;
        const double fixed_bin = std::exp(static_cast<double>(binq)/10.0);
        hcache.cells = liq_rails::history_cells(runs, bw, hist.points, lr.dmin, lr.dmax, fixed_bin, [&](size_t k, double level) {
            lr.mark = hist.points[k].mark;
            for (int i = 0; i < 3; ++i) lr.rv[i] = hist.rv[k][i];
            return touch_odds::probability(lr, lr.horizons[kHistoryHorizon], level);
        });
        std::sort(hcache.cells.begin(), hcache.cells.end(), [](const liq_rails::Cell& a, const liq_rails::Cell& b) { return a.row != b.row ? a.row < b.row : a.t < b.t; });
        hcache.wmax = 0;
        for (const auto& c : hcache.cells) hcache.wmax = std::max(hcache.wmax, c.w);
        hcache.rev = hist_rev; hcache.source = source_key; hcache.binq = binq;
    }
    if (!(hcache.wmax > 0)) return;
    history_frame = ImGui::GetFrameCount();
    auto* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    const double fixed_bin = std::exp(static_cast<double>(binq)/10.0);
    // One rect per row run of candles with the same shade: streaks where fuel stood within reach.
    size_t i = 0;
    while (i < hcache.cells.size()) {
        const auto& c = hcache.cells[i];
        const float tn = static_cast<float>(std::pow(c.w/hcache.wmax, 0.35));
        const int level = static_cast<int>(std::lround(tn*24.0f));
        size_t j = i + 1;
        while (j < hcache.cells.size() && hcache.cells[j].row == c.row && hcache.cells[j].t == hcache.cells[j - 1].t + every
               && static_cast<int>(std::lround(std::pow(hcache.cells[j].w/hcache.wmax, 0.35)*24.0)) == level) ++j;
        if (tn >= 0.16f) {  // history is denser than the forward fan: only rails worth reading
            uint8_t cr, cg, cb;
            HeatmapColormap::apply(HeatmapColormap::Type::Liquidation, 0.42f + 0.58f*(level/24.0f), cr, cg, cb);
            const float x0 = ImPlot::PlotToPixels(static_cast<double>(c.t), 0.0).x;
            const float x1 = ImPlot::PlotToPixels(static_cast<double>(hcache.cells[j - 1].t + every), 0.0).x;
            const float y = ImPlot::PlotToPixels(0.0, std::exp((static_cast<double>(c.row) + 0.5)*fixed_bin)).y;
            const float half = level >= 12 ? 1.75f : 1.0f, a = 0.85f*(0.30f + 0.70f*level/24.0f);
            if (x1 > pos.x && x0 < pos.x + size.x && y > pos.y && y < pos.y + size.y) {
                dl->AddRectFilled(ImVec2(x0, y - half - 3.0f), ImVec2(x1, y + half + 3.0f), IM_COL32(cr, cg, cb, static_cast<int>(255.0f*a*0.18f)));
                dl->AddRectFilled(ImVec2(x0, y - half), ImVec2(x1, y + half), IM_COL32(cr, cg, cb, static_cast<int>(255.0f*a)));
            }
        }
        i = j;
    }
    ImPlot::PopPlotClipRect();
}

void render(const Terminal::Pair& pair, int64_t clock, const char* price_fmt, bool replay, bool rails_drawn) {
    read();
    if (snapshot.history != replay) return;
    if (!matches(pair)) return;
    const auto pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
    if (snapshot.access == "locked" || snapshot.access == "not_released") {
        if (snapshot.access == "locked" && rails_on && !replay) { ImPlot::PushPlotClipRect(); locked_panel(pos, size); ImPlot::PopPlotClipRect(); }
        return;
    }
    ImPlot::PushPlotClipRect();
    cascade_chip(pos, clock);
    // The bands give the forward area only to rails that draw: a rails switch left on under the V2 rails used to
    // hide them and draw nothing in their place.
    if (!snapshot.bands_at(clock) || rails_drawn) { crosshair(pos, size, clock); ImPlot::PopPlotClipRect(); return; }
    const auto lim = ImPlot::GetPlotLimits();
    const double start = std::max<double>(clock, snapshot.asof);
    if (start < lim.X.Min || start >= lim.X.Max) { ImPlot::PopPlotClipRect(); return; }  // panned away from now
    const float right_edge = pos.x + size.x - 4.0f;
    const float x0 = ImPlot::PlotToPixels(start, 0.0).x;
    // True to time where the margin allows; never narrower than a readable column.
    const float x1 = std::min(right_edge, std::max(ImPlot::PlotToPixels(double(snapshot.expires_at), 0.0).x, x0 + 72.0f));
    if (x1 - x0 < 24.0f) { ImPlot::PopPlotClipRect(); return; }
    // Neutral veils (James 2026-10-15: the blue outlined boxes read as clutter): each band a faint fill that deepens
    // with its chance, so nested bands shade towards price on their own, and one hairline on its far edge (the edge
    // away from price) with its chance beside it. The hovered band lifts.
    auto* dl = ImPlot::GetPlotDrawList();
    const ImVec4& hue = Theme::Tokens::TX1;
    const auto mouse = ImGui::GetMousePos();
    const bool hovered = ImPlot::IsPlotHovered();
    int hover = -1;
    for (int i = 0; i < snapshot.band_count && hovered; ++i) {
        const auto& b = snapshot.bands[i];
        const float top = ImPlot::PlotToPixels(0.0, b.high).y, bottom = ImPlot::PlotToPixels(0.0, b.low).y;
        // The innermost band under the pointer: the one with the highest chance.
        if (mouse.x >= x0 && mouse.x <= x1 && mouse.y >= top - 1.0f && mouse.y <= bottom + 1.0f &&
            (hover < 0 || b.p > snapshot.bands[hover].p)) hover = i;
    }
    float label_y[6]; int labels = 0;
    ImGui::PushFont(Theme::Fonts::mono_sm());
    for (int i = 0; i < snapshot.band_count; ++i) {
        const auto& b = snapshot.bands[i];
        float top = ImPlot::PlotToPixels(0.0, b.high).y, bottom = ImPlot::PlotToPixels(0.0, b.low).y;
        if (bottom < pos.y || top > pos.y + size.y) continue;
        if (bottom - top < 2.0f) { const float mid = (top + bottom) * 0.5f; top = mid - 1.0f; bottom = mid + 1.0f; }
        const bool hot = i == hover;
        dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, bottom), Theme::u32(hue, float(0.025 + 0.07 * b.p) * (hot ? 1.8f : 1.0f)),
                          Theme::Radius::R1);
        // Above price (side > 0) the far edge is the band's top; below, its bottom.
        const float edge = std::round(b.side > 0 ? top : bottom) + 0.5f;
        dl->AddLine(ImVec2(x0 + 2.0f, edge), ImVec2(x1 - 2.0f, edge), Theme::u32(hot ? Theme::Tokens::TX2 : Theme::Tokens::TX3, hot ? 0.9f : 0.45f), 1.0f);
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%.0f%%", 100.0 * b.p);
        const ImVec2 tsz = ImGui::CalcTextSize(pct);
        // Inside the band beside its far edge, so the figure reads as that edge's chance.
        const float y = b.side > 0 ? edge + 3.0f + tsz.y * 0.5f : edge - 3.0f - tsz.y * 0.5f;
        bool overlaps = false;
        for (int k = 0; k < labels; ++k) overlaps = overlaps || std::abs(label_y[k] - y) < tsz.y + 2.0f;
        if (overlaps || x1 - x0 < tsz.x + 12.0f || bottom - top < tsz.y + 4.0f) continue;
        dl->AddText(ImVec2(x0 + 6.0f, std::round(y - tsz.y * 0.5f)), Theme::u32(hot ? Theme::Tokens::TX1 : Theme::Tokens::TX2), pct);
        label_y[labels++] = y;
    }
    ImGui::PopFont();
    if (hover < 0) crosshair(pos, size, clock);
    ImPlot::PopPlotClipRect();
    if (hover < 0) return;
    const auto& b = snapshot.bands[hover];
    char at[16], low[48], high[48], mark[48];
    hhmm(snapshot.asof, at, sizeof(at));
    std::snprintf(low, sizeof(low), price_fmt, b.low); std::snprintf(high, sizeof(high), price_fmt, b.high); std::snprintf(mark, sizeof(mark), price_fmt, snapshot.mark);
    Theme::begin_tooltip();
    ImGui::Text("%.1f%% chance of touching %s to %s", 100.0 * b.p, low, high);
    ImGui::Text("within 20 minutes of %s (mark %s, %+.2f%% to the near edge).", at, mark, 100.0 * ((b.side > 0 ? b.low : b.high) / snapshot.mark - 1.0));
    ImGui::TextUnformatted("Mark price on 5-minute bars, not exact entry or fills. Bands overlap: never add them.");
    ImGui::TextUnformatted(record_note("Frozen model; historical tests passed, live-window test scored 12 Oct 2026. Admin preview."));
    Theme::end_tooltip();
}
}  // namespace touch_odds_overlay
