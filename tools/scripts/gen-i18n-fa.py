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

os.makedirs(os.path.join(ROOT, "fonts"), exist_ok=True)
sz1 = subset_font("/tmp/Vazirmatn-Regular.ttf",  os.path.join(ROOT, "fonts/Vazirmatn-Regular-Subset.ttf"))
sz2 = subset_font("/tmp/Vazirmatn-SemiBold.ttf", os.path.join(ROOT, "fonts/Vazirmatn-SemiBold-Subset.ttf"))
print(f"fonts: Vazirmatn-Regular-Subset.ttf {sz1}B, SemiBold {sz2}B")

# ── 2. translation table ─────────────────────────────────────────────────────
import arabic_reshaper
from bidi.algorithm import get_display

def shape(logical: str) -> str:
    """logical Persian → visual-order presentation forms (ImGui-ready)."""
    return get_display(arabic_reshaper.reshape(logical), base_dir="R")

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
for key, en, fa in TABLE:
    vis = shape(fa)
    esc = vis.replace("\\", "\\\\").replace('"', '\\"')
    out.write(f'    /* fa_{key} */ {{"{en}", u8"{esc}"}},\n')
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
