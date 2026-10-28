#!/usr/bin/env python3
"""Phase 7 (fa UX) asset generator.

1. Subsets Vazirmatn (Regular/SemiBold) to the Persian blocks the terminal
   needs and writes them into fonts/ (packaged into index.data, merged as a
   second glyph source in theme.cpp — the established CJK-fallback pattern).
2. Reshapes the logical Persian string table into VISUAL-ORDER presentation
   forms (ImGui renders LTR glyph runs — shaping/RTL must be baked in) and
   emits src/core/i18n_fa.h.

Run from the repo root:  python3 tools/scripts/gen-i18n-fa.py
"""
import io, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# ── 1. fonts ─────────────────────────────────────────────────────────────────
from fontTools.ttLib import TTFont
from fontTools.subset import Subsetter, Options

PERSIAN_RANGES = (
    list(range(0x0600, 0x0700))      # Arabic: letters, ۰-۹, punctuation
    + [0x200C]                       # ZWNJ
    + list(range(0xFB50, 0xFE00))    # Presentation Forms-A (Persian ligatures)
    + list(range(0xFE70, 0xFF00))    # Presentation Forms-B (shaped glyphs)
)

def subset_font(src, dst):
    font = TTFont(src)
    opts = Options()
    opts.layout_features = []          # ImGui needs cmap only
    opts.drop_tables += ["GSUB", "GPOS"]
    ss = Subsetter(opts)
    ss.populate(unicodes=PERSIAN_RANGES)
    ss.subset(font)
    font.save(dst)
    return os.path.getsize(dst)

def _first_font(*candidates):
    """First font source that exists: upstream Vazirmatn in /tmp when fetched
    manually, otherwise the shipped subset (idempotent to re-subset)."""
    for c in candidates:
        if os.path.exists(c):
            return c
    raise SystemExit("no Vazirmatn source found (checked: %s)" % ", ".join(candidates))

os.makedirs(os.path.join(ROOT, "fonts"), exist_ok=True)
# Fresh upstream Vazirmatn lives in /tmp when fetched manually; after a clean
# checkout the shipped subsets are the only source. Re-subsetting a subset
# with the SAME ranges is idempotent (it already keeps exactly those glyphs),
# so falling back to it is lossless and keeps regeneration reproducible.
sz1 = subset_font(_first_font("/tmp/Vazirmatn-Regular.ttf",  os.path.join(ROOT, "fonts/Vazirmatn-Regular-Subset.ttf")),  os.path.join(ROOT, "fonts/Vazirmatn-Regular-Subset.ttf"))
sz2 = subset_font(_first_font("/tmp/Vazirmatn-SemiBold.ttf", os.path.join(ROOT, "fonts/Vazirmatn-SemiBold-Subset.ttf")), os.path.join(ROOT, "fonts/Vazirmatn-SemiBold-Subset.ttf"))
print(f"fonts: Vazirmatn-Regular-Subset.ttf {sz1}B, SemiBold {sz2}B")

# ── 2. translation table ─────────────────────────────────────────────────────
import arabic_reshaper
from bidi.algorithm import get_display

def shape(logical: str) -> str:
    """logical Persian → visual-order presentation forms (ImGui-ready).
    Multi-line strings are shaped line-by-line: python-bidi treats the whole
    input as ONE bidi paragraph, so embedded \n would otherwise let runs
    reorder across line boundaries."""
    return "\n".join(
        get_display(arabic_reshaper.reshape(line), base_dir="R")
        for line in logical.split("\n"))

# key: C identifier fragment → (english, logical persian)
TABLE = [
    # top bar
    ("workspace",      "Workspace",      "محیط کار"),
    ("live",           "Live",           "زنده"),
    ("replay",         "Replay",         "بازپخش"),
    ("market_details", "Market details", "جزئیات بازار"),
    ("details",        "Details",        "جزئیات"),
    ("data",           "Data",           "داده‌ها"),
    ("courses",        "Courses",        "آموزش"),
    ("account",        "Account",        "حساب"),
    ("appearance",     "Appearance",     "ظاهر"),
    ("reset_layout",   "Reset Layout",   "بازنشانی چیدمان"),
    ("market_header",  "Market Header",  "هدر بازار"),
    ("paused",         "Paused",         "مکث"),
    ("ws_disc",        "WS DISCONNECTED", "قطع اتصال WS"),
    ("local_replay",   "LOCAL REPLAY",   "بازپخش محلی"),
    ("connected",      "Connected",      "متصل"),
    # chart toolbar
    ("candles",        "Candles",        "کندل‌ها"),
    ("dom",            "DOM",            "عمق بازار"),
    ("liquidations",   "Liquidations",   "لیکوییدیشن‌ها"),
    ("layers",         "Layers",         "لایه‌ها"),
    ("tools",          "Tools",          "ابزارها"),
    ("add_widget",     "+ Widget",       "+ ویجت"),
    ("rt_heatmap",     "Real-time heatmap", "نقشهٔ حرارتی زنده"),
    ("heatmap",        "Heatmap",        "نقشهٔ حرارتی"),
    # layers menu rows
    ("l_depth",        "Order book depth", "عمق دفتر سفارش"),
    ("l_bubbles",      "Trade bubbles",  "حباب‌های معامله"),
    ("l_trade_line",   "Trade-price line", "خط قیمت معامله"),
    ("l_1s_candles",   "1s candles",     "کندل‌های ۱ثانیه"),
    ("l_heikin",       "Heikin-Ashi",    "هایکین آشی"),
    ("l_est_liq",      "Est. liquidations", "لیکوییدیشن برآوردی"),
    ("l_resting",      "Resting depth heat", "حرارت عمق ماندگار"),
    ("l_reported",     "Reported liquidations", "لیکوییدیشن‌های اعلامی"),
    ("l_absorption",   "Absorption bands", "باندهای جذب"),
    ("l_peak_hold",    "Peak hold",      "نگه‌داشت قله"),
    ("l_vwap",         "Daily VWAP (UTC)", "VWAP روزانه (UTC)"),
    ("l_prev_day",     "Previous day high / low / close", "سقف/کف/بستن روز قبل"),
    ("l_prev_week",    "Previous week high / low / close", "سقف/کف/بستن هفتهٔ قبل"),
    ("l_hollow",       "Hollow candles", "کندل توخالی"),
    ("l_cvd",          "CVD",            "CVD"),
    ("l_hl_positions", "Hyperliquid positions", "پوزیشن‌های هایپرلیکوئید"),
    ("l_liq_map",      "Liquidation map", "نقشهٔ لیکوییدیشن"),
    ("l_scenario",     "Scenario profile", "پروفایل سناریو"),
    ("l_bubbles_large", "Trade bubbles (large prints)", "حباب‌های معامله (فیل‌های بزرگ)"),
    # watchlist
    ("watchlist",      "Watchlist",      "دیده‌بان"),
    ("search_pairs",   "Search pairs",   "جست‌وجوی نماد"),
    ("all_pairs",      "All pairs",      "همهٔ نمادها"),
    ("all_venues",     "All venues",     "همهٔ صرافی‌ها"),
    ("pair",           "Pair",           "نماد"),
    ("volume",         "Volume",         "حجم"),
    ("price",          "Price",          "قیمت"),
    ("h24",            "24h",            "۲۴ساعت"),
    # DOM ladder header
    ("sold",           "Sold",           "فروش"),
    ("bids",           "Bids",           "خریدها"),
    ("asks",           "Asks",           "فروش‌ها"),
    ("bought",         "Bought",         "خرید"),
    # statsbar / header stats
    ("last",           "Last",           "آخرین"),
    ("funding",        "Funding",        "فاندینگ"),
    ("open_interest",  "Open interest",  "اورج باز"),
    ("volume_24h",     "Volume 24h",     "حجم ۲۴ساعت"),
    ("unavailable",    "Unavailable",    "ناموجود"),
    # settings (phase 7 surface)
    ("language",       "LANGUAGE / زبان", "LANGUAGE / زبان"),
    ("english",        "English",       "انگلیسی"),
    ("farsi",          "Farsi",         "فارسی"),
    ("fa_digits",      "Persian digits", "اعداد فارسی"),
    ("theme",          "THEME / پوسته",  "THEME / پوسته"),
    ("dark",           "Dark",           "تاریک"),
    ("light",          "Light",          "روشن"),
    ("save_theme",     "Save theme (JSON)", "ذخیرهٔ پوسته (JSON)"),
    ("load_theme",     "Load theme (JSON)", "بارگذاری پوسته (JSON)"),
    # ── Waves 1-3 (layers management suite + new data layers) ────────────────
    # rows - new layers
    ("l_whale_trail",  "Whale trail",       "رد پای نهنگ"),
    ("l_venue_basis",  "Venue basis (OKX)", "بیسیس صرافی (OKX)"),
    ("l_slippage",     "Slippage cone",     "مخروط لغزش"),
    ("l_events",       "Layer events",      "رویدادهای لایه"),
    # rows - neighbours that shipped English (same menu, same fa pass)
    ("l_walls",        "Walls",             "دیوارها"),
    ("l_pulled_walls", "Pulled walls",      "دیوارهای برداشته‌شده"),
    ("l_walls_table",  "Active walls table", "جدول دیوارهای فعال"),
    ("l_refill",       "Refill levels",     "سطوح ری‌فیل"),
    # sections
    ("sec_ob_signals", "ORDER BOOK SIGNALS", "سیگنال‌های دفتر سفارش"),
    ("sec_cross_venue", "CROSS-VENUE",      "چند-صرافی"),
    ("sec_execution",  "EXECUTION",         "اجرا"),
    ("sec_events",     "EVENTS",            "رویدادها"),
    ("sec_presets",    "LAYER PRESETS",     "پریست لایه‌ها"),
    ("sec_my_sets",    "MY LAYER SETS",     "مجموعه‌های من"),
    # layer sets
    ("save_cur_set",   "Save current as set…", "ذخیرهٔ وضعیت فعلی به‌عنوان مجموعه…"),
    ("set_name",       "Set name",          "نام مجموعه"),
    ("save_set",       "Save set",          "ذخیرهٔ مجموعه"),
    # detail-page fields
    ("opacity",        "Opacity",           "شفافیت"),
    ("depth_settings", "Depth settings…",   "تنظیمات عمق…"),
    ("link_window",    "Link window",       "پنجرهٔ پیوند"),
    ("preset_size",    "Preset size",       "اندازهٔ آماده"),
    ("exact_size",     "Exact size",        "اندازهٔ دقیق"),
    # layer presets
    ("layout",         "Layout",            "چیدمان"),
    ("preset_scalp",   "Scalp",             "اسکالپ"),
    ("preset_swing",   "Swing",             "سوینگ"),
    ("preset_research", "Research",         "تحقیق"),
    # archive retention (Real-time menu)
    ("archive_depth",  "Archive depth",     "عمق آرشیو"),
    ("arch_30m",       "30 min",            "۳۰ دقیقه"),
    ("arch_2h",        "2 h",               "۲ ساعت"),
    ("arch_6h",        "6 h",               "۶ ساعت"),
    ("arch_12h",       "12 h",              "۱۲ ساعت"),
    # whale-trail link window
    ("whale_5s",       "5 s",               "۵ ثانیه"),
    ("whale_8s",       "8 s",               "۸ ثانیه"),
    ("whale_15s",      "15 s",              "۱۵ ثانیه"),
    # alert toggles (Real-time menu, new in wave 3)
    ("refill_consumed", "Refill consumed",  "مصرف ری‌فیل"),
    # events widget header
    ("events_header",  "LAYER EVENTS",      "رویدادهای لایه"),
    # candle-mode Layers menu -> research page (display only; page key stays EN)
    ("research_tools", "Research tools",    "ابزارهای تحقیق"),
    # detail-page back item (first row of every settings page)
    ("back",           "Back",              "بازگشت"),
    # visible notes (menu notes, not hover tooltips)
    ("note_alt",       "Alt+1..0 toggles these layers anywhere:\n1 depth, 2 bubbles, 3 trade line, 4 candles, 5 Heikin-Ashi,\n6 CVD, 7 est. liq, 8 resting heat, 9 refill, 0 walls.",
                                           "Alt+1..0 این لایه‌ها را هرجا عوض می‌کند:\n۱ عمق، ۲ حباب‌ها، ۳ خط معامله، ۴ کندل‌ها، ۵ هایکین آشی،\n۶ CVD، ۷ لیکوییدیشن برآوردی، ۸ حرارت ماندگار، ۹ ری‌فیل، ۰ دیوارها"),
    ("note_okx",       "Cross-venue basis needs an OKX feed; this gateway\nserves Binance futures only.",
                                           "بیسیس چند-صرافی به فید OKX نیاز دارد؛ این گیت‌وی\nفقط فیوچرز بایننس را سرو می‌کند"),
    ("note_slip",      "Fills are volume-weighted over the stored rows at the\ntouch - an estimate over the visible book, not the\nvenue's full depth.",
                                           "فیل‌ها میانگین حجمی روی ردیف‌های ذخیره‌شدهٔ لمس هستند -\nبرآوردی روی کتاب مرئی، نه عمق کامل صرافی"),
]

out = io.StringIO()
out.write("""// Generated by tools/scripts/gen-i18n-fa.py - DO NOT EDIT BY HAND.
// Persian strings are PRE-SHAPED, VISUAL-ORDER (presentation forms B +
// bidi-reversed) so ImGui renders them correctly with plain LTR glyph runs.
// Regenerate after editing the table in the generator script.
#pragma once

#include <cstdint>

namespace i18n {

enum FaKey : uint32_t {
""")
for key, en, fa in TABLE:
    out.write(f"    fa_{key},\n")
out.write("""    fa_COUNT
};

// [fa_COUNT][2]: key → {english, persian visual}
inline const char* const fa_table[fa_COUNT][2] = {
""")
def c_esc(s: str) -> str:
    """C string escaping for a plain UTF-8 literal: backslash/quote plus the
    control chars a table entry can carry (multi-line notes hold real \\n)."""
    return (s.replace("\\", "\\\\").replace('"', '\\"')
             .replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t"))

for key, en, fa in TABLE:
    vis = shape(fa)
    # NOTE: plain literals, NOT u8"" - C++20 char8_t breaks const char* tables.
    # The source is UTF-8 and clang's execution charset is UTF-8, so these
    # bytes land in the binary unchanged.
    out.write(f'    /* fa_{key} */ {{"{c_esc(en)}", "{c_esc(vis)}"}},\n')
out.write("""};

}  // namespace i18n
""")

with open(os.path.join(ROOT, "src/core/i18n_fa.h"), "w", encoding="utf-8") as f:
    f.write(out.getvalue())
print(f"src/core/i18n_fa.h written ({len(TABLE)} strings)")

# coverage check: every shaped codepoint must exist in the subset cmap
from fontTools.ttLib import TTFont
cmap = TTFont(os.path.join(ROOT, "fonts/Vazirmatn-Regular-Subset.ttf")).getBestCmap()
missing = set()
for key, en, fa in TABLE:
    for ch in shape(fa):
        if ord(ch) not in cmap and ch not in " \u200c":
            missing.add(hex(ord(ch)) + ":" + key)
print("missing glyphs:", sorted(missing) if missing else "NONE")
