# 📔 CHANGELOG — EdgeDepth Terminal (fork شخصی bigbang240)

قالب: [Keep a Changelog](https://keepachangelog.com) · نسخه‌بندی: [SemVer](https://semver.org)
هر فازِ `docs/UPGRADE-ROADMAP.md` یک ریزنسخهٔ minor است. تاریخ‌ها از merge های گیت‌هاب.

## [Unreleased] — فاز ۸ «پایداری و توزیع» (در جریان)
### Added
- فعال‌سازی GitHub Actions روی fork (build.yml و release.yml از upstream، اجرا روی هر push به master)
- `workflow_dispatch` برای اجرای دستی CI
- `VERSION` + این CHANGELOG + هم‌ترازسازی نسخهٔ پروژهٔ CMake با SemVer
### Planned
- `version.json` + اعلان به‌روزرسانی خودکار مرورگر
- بهینه‌سازی WASM (بررسی SIMD، کاهش حجم، کش مرورگر هش‌دار)

## [0.7.0] — 2026-10-25 — فاز ۷ «تجربهٔ کاربری فارسی»
### Added
- لایهٔ ترجمهٔ فارسی: جدول ~۶۸ رشته‌ای پیش‌شکل‌شدهٔ بصری (`src/core/i18n_fa.h` تولیدی با `tools/scripts/gen-i18n-fa.py` + arabic_reshaper/bidi)، موتور `tr()`، نگاشت اعداد فارسی ۰-۹، ماندگاری در `localStorage (edgedepth.i18n.v1)` و بارگذاری در boot
- فونت Vazirmatn (زیرمجموعهٔ Regular/SemiBold ~۲۶KB) به‌عنوان منبع گلیف دوم روی همهٔ صورت‌ها (الگوی fallback سابقاً CJK)
- پنل Appearance: سکشن LANGUAGE (سوییچ English/فارسی + چک‌باکس اعداد فارسی برای آمار هدر) و THEME JSON (ذخیرهٔ snapshot پوسته در `edgedepth.theme.v1` + بارگذاری و اعمال مجدد)
- کروم `Daylight` — نخستین پیش‌تنیم روشن (سطوح کاغذی، جوهر تیره)؛ رمپ رنگی هیت‌مپ ثابت می‌ماند
- ترجمه در کروم مرئی: نوار بالا، نوار وضعیت، تولبار چارت، ۲۰ ردیف Layers، واچ‌لیست، هدر نردبان DOM، آمار هدر
### Fixed
- لیترال‌های `u8""` → UTF-8 ساده (char8_t در C++20 جدول `const char*` را می‌شکست)
- شکافت ویرگول پیش‌پردازنده در EM_ASM — ترجیحات i18n به یک int بسته‌بندی شد

## [0.6.0] — 2026-10-21 — فاز ۶ «داده و یکپارچه‌سازی» (مرحلهٔ ۱: OKX)
### Added (gateway)
- آداپتور OKX v5 برای قراردادهای دائمی SWAP (`internal/okx`): کانال‌های books/trades/mark-price/funding-rate/open-interest روی `/ws/v5/public` + liquidation-orders؛ تبدیل قرارداد→کوین با ctVal؛ تیکر سراسری REST-poll هر ۳۰s؛ کندل تاریخی REST با 1s و صفحه‌بندی history-candles؛ ثبت با `-okx-base` (~۱۰۴۴ نماد SWAP)
- سمت ترمینال: venue «okx» در url_router + واچ‌لیست + اشتراک تیکر؛ universe خود-سرو؛ **tick ضمنی از کتاب زنده**
### Fixed (gateway)
- نظم prime اشتراک نسبت به broadcast با قفل نوشتن hub؛ diff های OKX با FirstUpdateId=seq (معادل U-range بایننس)
### Known issue
- زنجیرهٔ depth روی OKX گاهی «Recovering depth» می‌زند (خودترمیم ~۵s؛ ردیابی در worklog)
- باقی‌ماندهٔ فاز ۶: REST/WS سرور تحلیلی روی OrderBookManager + ذخیرهٔ تاریخی SQLite

## [0.5.0] — 2026-10-21 — فاز ۵ «اجرا و ریسک»
### Added
- ماشین‌حساب Position متصل به Draw (Account/Risk% سراسری، چیپ بازهٔ Long/Short با qty/notational و کپ ۲۰x)
- موتور paper trading محلی سمت-کلاینت (`core/local_paper_engine`): باز شدن خودکار از crossing هشدارها، TP1/TP2/trailing، MAE/MFE، R-multiple، ذخیرهٔ `edgedepth.localpaper.v1`، ویجت با Balance/Equity/WR/PF/MaxDD و Daily P&L
- پل اجرای واقعی اختیاری: webhook bridge با POST JSON روی هر alert/fill (الگوی EM_JS، fire-and-forget)

## [0.4.0] — 2026-10-21 — فاز ۴ «ضبط و درک»
### Added
- Recorder daemon (`tools/recorder/`، سرویس bun :9002): ضبط ۲۴/۷ واچ‌لیست به .edpack (بلاک zstd ~۴MiB، هدر PackHeader با ob_seed، اپیزود ساعت‌به‌hour + manifest)، HTTP با Range/ETag/CORS
- Event finder: ایندکس زندهٔ cascade/لیکوییدیشن/فیل بزرگ/اسپایک حجم + بخش EVENT FINDER در Replay Library با پخش عمیق («۴۵ ثانیه قبل از لحظه»)
- گزارش روزانهٔ خودکار HTML (`data/reports/` + نسخهٔ زندهٔ امروز): KPI، نمودار SVG قیمت با حباب‌های لیکوییدیشن، جدول‌ها با لینک «open in terminal»، گالری Terminal snapshots (captureStream هر ۱۰ دقیقه)

## [0.3.0] — 2026-10-18 — فاز ۳ «چارت تریدینگ-گرید» (PR #1)
### Added
- Preset های چیدمان لایه (Scalp/Swing/Research) + هشدار متصل به اشکال Draw + چند-چارت sync (کراس‌هیر/زوم) + template lines با ذخیرهٔ localStorage + Heikin-Ashi روی RT
- سند نقشهٔ راه ۸ فازی + راهنمای Git/GitHub (`docs/`)

## [0.2.0] — 2026-10-21 — فاز ۲ «جنگ‌افزار لیکوییدیشن» (PR #3)
### Added
- مدل برآورد لیکوییدیشن خود-میزبان از OI + mark + توزیع اهرم (`core/liq_levels.h`) با نوارهای فورس‌سل/فورس‌بای
- Cascade detector (۳+ لیکوییدیشن <۲s) با pill/toast/Notification/بیپ + Alert engine (آستانهٔ دلاری، هشدار خوردن دیوار، بیپ‌های ۶۶۰/۸۸۰/۵۲۰Hz)
- جدول «دیوارهای فعال» + هیت‌مپ دوم «Resting depth heat» با میانگین نمایی ~۱۰ دقیقه

## [0.1.0] — 2026-10-18 — فاز ۱ «هیت‌مپ حرفه‌ای» (PR #2)
### Added
- پالت JSON قابل تنظیم (teal/gold/bluered، gamma، شفافیت، نرمال‌سازی per-view/per-column/global)
- تجمیع ستون (آخرین/Max/Mean/Volume-weighted) + Peak-hold ۱۰/۳۰ دقیقه
- Snapshot API `window.__ed_state` + ضبط canvas از captureStream
- پل دیتا-بیرون‌دهی JSON-WS روی سیم protobuf گیت‌وی (سمت gateway)

## [مبنا] — 2026-08-09 — انتشار عمومی اولیه upstream
- ترمینال C++20/WASM (SDL3 + ImGui/ImPlot) + گیت‌وی Go بایننس، بستهٔ docker compose
