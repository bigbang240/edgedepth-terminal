# 🗺️ نقشهٔ راه کامل ارتقا و گسترش EdgeDepth Terminal
**نسخهٔ سلف-هاست | تاریخ: 2026-10-18 | بر پایهٔ بازبینی کامل تک‌تک لایه‌های ریپو**

> این سند حاصل بازبینی خط‌به‌خط ساختار ریپو است: هر لایه بررسی شد (chart، heatmap، liquidations، layers، widgets، draw، DOM، replay، workspace، entitlements، data-flow) و بعد از هر لایه، فاز ارتقا و امکانات پریمیوم پیشنهادی آمده است.
> اولویت‌بندی بر اساس اولویت خود شماست: **هیت‌مپ زنده = مهم‌ترین قابلیت**، چون در آینده این ریپو را در ترمینال اصلی خود ادغام می‌کنید.

---

## 📊 وضعیت فعلی (Phase 0 — انجام شده)

| قابلیت | وضعیت |
|---|---|
| دیپلوی سلف-هاست (WASM بدون pthread + گیت‌وی Go روی :9001) | ✅ |
| هیت‌مپ زندهٔ Real-time (GPU texture، عمق Binance 100ms) | ✅ |
| کندل‌های RT روی بستر هیت‌مپ (trade→bucket aggregation) | ✅ |
| تایم‌فریم RT قابل انتخاب: 1s / 5s / 15s / 30s / 1m / 5m | ✅ |
| زوم TradingView-ساز: درگ لبهٔ محور زمان (کش دادن خط زمان) و درگ محور قیمت | ✅ |
| DOM Ladder متصل به هیت‌مپ (rt_dom_linked: هم‌ترازی bucket، قفل ارتفاع ردیف) | ✅ |
| حذف همهٔ عناصر دمو (EARLY ACCESS، نوار AGPL، لندینگ) | ✅ |
| Watchlist 1219 جفت‌ارز + سوییچ زندهٔ pair | ✅ |
| Entitlements: چون `__EDGEDEPTH_HOSTED__=false` → حالت Pro کامل باز | ✅ |

---

## 🔬 بازبینی لایه‌به‌لایهٔ ریپو (ساختار فعلی)

### 1) لایهٔ رندر چارت — `src/ui/chart_widget*.cpp` (۱۷ فایل)
**موجود:** ۸ نوع چارت (Candles/HeikinAshi/Line/TPO/Renko/Footprint Cluster & Profile/FlowPositioning)، ترتیب رندر دقیق: هیت‌مپ عمق (پس‌زمینه GPU) ← فیلد سناریوی لیکوییدیشن ← ریل‌های HL positions ← VPVR ← **کندل‌ها + خط trade** ← حباب‌های معاملات بزرگ ← مارکرهای لیکوییدیشن ← footprint ← ابزار رسم. سه tier رندر کندل بر اساس سطح زوم. قیدهای زوم (۸ تا ۱۴۴۰ کندل مرئی).

**نقاط قوت:** جداسازی تمیز لایه‌ها، GPU texture برای هیت‌مپ (WebGL2 R32F)، adapt-to-zoom خودکار برای گروه‌بندی ردیف‌ها.
**شکاف:** رنگ هیت‌مپ ثابت است (پالت داخلی)، هیچ export/Snapshot API ندارد، هیت‌مپ فقط per-column "آخرین اسنپ‌شات" است (حالت max/mean ندارد).

### 2) لایهٔ Real-time — `chart_widget_realtime.cpp` (۱۰۵۹ خط) + `core/rt_walls.h` + `core/rt_cvd.h`
**موجود:** تجمیع trade → کندل bucket انتخابی (grid 1/5/15/30/60/300 ثانیه)، خط پله‌ای best bid/ask با هاله، حباب‌های حجمی (log-scale)، auto-price با ضدلرزش (۱-۲-۵-۱۰ decade)، CVD lane (۷۲px)، **دیوارهای سفارش (Walls)** با منطق ۵× میانه / touch-share / خروج ۳× / تعریف pull با ۷۰٪ برداشت، recovery خودکار بعد از قطعی depth.
**شکاف:** ویجت UI برای مدیریت walls (لیست دیوارهای فعال با سن و وضعیت) وجود ندارد؛ هشدار صوتی/دسکتاپ ندارد.

### 3) لایهٔ Liquidations (لیکوییدیشن) — `chart_widget_liquidations.cpp` + `core/liquidation_heatmap_manager.h`
**موجود (۴ مدل داده):**
- **Observed marks**: مارکر لوزی واقعی @forceOrder (Binance/Bybit)، سطح∝USD، مقیاس لگاریتمی
- **Callouts**: برچسب دلاری روی کندل‌های «غیرعادی» (آستانهٔ ×۲ قابل تنظیم)
- **Activity strip**: ساب-چارت قابل جمع‌شدن (long بالا/short پایین محور)
- **Price profile**: توزیع دلاری سمت راست چارت
- **LiqMap V2 rails + HL positions rails** (census واقعی پوزیشن‌های Hyperliquid)
- **فیلتر اهرم**: چیپ‌های 5x/10x/25x/50x/100x
- **فیلد سناریو**: میدان حرارتی «اگر قیمت به X برسد، کجای OI منفجر می‌شود» (LiqFieldRenderer)

**شکاف:** مدل برآورد لیکوییدیشن خودمان (از OI + mark price، مستقل از سرور) پیاده نشده؛ cascade detection (زنجیرهٔ لیکوییدیشن) ندارد؛ هشدار realtime ندارد.

### 4) لایهٔ Layers (لایه‌ها) — `chart_widget_overlays.cpp` (۵۴۱ خط)
**موجود:** منوی Layers با زیرصفحه: Price Levels (VWAP روزانه، H/L/C روز/هفتهٔ قبل، VWAP انکر شده)، Order book depth، Trade bubbles، VPVR، Liquidation map، HL positions، Reported liquidations، Scenario profile، Refill levels/history، Absorption bands، Touch-odds، Compression overlay. هر ردیف: toggle + قفل PRO + ذخیره در workspace. چرخش دو منوی مستقل: Layers و Liquidations.

**شکاف:** ترتیب/مکاش‌پذیری لایه‌ها (z-order UI) ندارند؛ لایهٔ کاربر-سازیشه (import لایهٔ شخصی) ندارد؛ ذخیرهٔ مجموعه-لایه به‌صورت preset نام‌گذاری‌شده ندارد.

### 5) لایهٔ Widgets — `src/ui/widget.h` + `Menu::resolve_widget_add_request`
**موجود:** ۱۲ نوع widget: Chart (چند-نمونه)، Orderbook (۲۵ سطح)، DOM، Trades، Stats، Data&Research، Watchlist، PositionsPanel (Paper trading سه‌تب)، ReplayLibrary، DebugLog، ResearchMomentPanel. سیستم dockspace کامل ImGui (کشیدن/رها کردن، چیدمان‌های پیش‌فرض، reset، حالت companion برای DOM/Trades). روتین به‌روزرسانی هر widget (Turbo/RealTime/Standard/Slow).

**شکاف:** widget سادهٔ Alert/Notification وجود ندارد؛ widget سفارشی کاربر (HTML embed) ندارد؛ ذخیرهٔ چند-چیدمان نام‌گذاری‌شده در نسخهٔ فعلی محدود است.

### 6) لایهٔ Draw (رسم) — `src/ui/drawing/` + `core/drawing_manager.cpp`
**موجود:** ۱۹ ابزار: Trendline/Arrow/Ray/Extended/HLine/HRay/VLine/CrossLine/Rectangle/Brush/Measure/PriceRange/DateRange/Fibonacci (۶ سطح)/Long-Short Position (با R:R)/Text/Parallel Channel/Polyline. لنگر (زمان،قیمت) → پایدار در pan/zoom/tf/replay. ذخیرهٔ localStorage به تفکیک نماد، merge چند-پنجره‌ای، **Undo تا ۶۴ مرحله**، مغناطیس (magnet)، style editor.

**شکاف:** شکل‌ها قابل «نام‌گذاری و ذخیره به‌عنوان الگو» نیستند؛ هشدار متصل به شکل (مثلاً «قیمت به خط رسید») ندارد؛ sync ابری ندارد.

### 7) لایهٔ DOM — `dom_widget.h` + `realtime_dom_ladder.h`
**موجود:** DOM جدا (۶ ستونه با تجمیع محلی معاملات) + **DOM Ladder متصل به RT** (ستون سمت راست چارت تا ۴۵٪ عرض): قفل هم‌ترازی bucket با هیت‌مپ، ردیف‌ها با هیت‌مپ هم‌مرز، ستون‌های bid/ask/buy/sell + نوار عمق، حالت "Last known depth" هنگام قطعی.
**شکاف:** depth-diff animation (نمایش انیمیشنی بزرگ‌شدن/خوردن دیوارها) ندارد؛ heatmap دوم (عمق انباشته در زمان) در خود ladder ندارد.

### 8) لایهٔ Replay/Archive — `replayer/` + `core/realtime_archive.h`
**موجود:** ریپلی شبکه‌ای (State machine کامل: seek/speed/skip)، **.edpack** (فایل آرشیو محلی با اندیس zstd، نیازی به سرور)، ReplayHistoryBuffer برای پرش عقب بدون ریست، ghost candle هنگام scrub، **RealtimeArchive**: ۳۰ دقیقه تاریخچهٔ RT در IndexedDB (worker جاوااسکریپت، gzip) + بازیابی شروع-به-کار.
**شکاف:** ضبط خودکار دائمی همهٔ نمادها (recorder daemon سمت Node) وجود ندارد؛ جست‌وجو در تاریخچهٔ RT (event finder) ندارد.

### 9) لایهٔ Workspace — `core/workspace_manager.cpp`
**موجود:** localStorage ≤4MiB، سند نسخه‌دار (widgets+layout+market)، ۳ preset کارخانه‌ای، نام‌گذاری، import/export، checkpoint برای replay.
**شکاف:** همگام‌سازی ابری ندارد؛ نسخه‌بندی چند-پروفایل (تریدینگ/تحقیق) ندارد.

### 10) لایهٔ Entitlements — `core/entitlements.h`
**موجود:** tier از `__EDGEDEPTH_TIER__` (free/trader/pro/research/admin)؛ سلف-هاست = Pro باز. **در نسخهٔ شما کل این سیستم عملاً غیرفعال است** — می‌توانید بعداً خودتان gate کنید (بخش پریمیوم).
**نکتهٔ مجوز:** کل ریپو **AGPL-3.0** است: تغییرات شما اگر «در شبکه عرضه شود» باید سورس هم باز بماند؛ برای استفادهٔ شخصی کاملاً آزادید.

### 11) لایهٔ Data Flow — `stream_handler.h` + `message_handler.cpp`
**موجود:** WS → zstd → protobuf-lite → ۴۳ نوع استریم (Trades/Candles/Orderbook/Liquidations/Heatmap/…) → DataThread (pthread) → DispatchQueue با بودجهٔ فریم ۳ms → managers. گیت‌وی Go شما (edgedepth-gateway) feed Binance را با depth@100ms و cache REST تأمین می‌کند.
**شکاف:** هیچ خروجی داده (API/WS export) برای ترمینال اصلی شما ندارد — این مهم‌ترین حلقهٔ گمشده برای ادغام است.

---

## 🚀 نقشهٔ راه پیشنهادی (۸ فاز، مرتب‌شده بر ارزش برای شما)

### فاز ۱ — «هیت‌مپ حرفه‌ای» (اولویت مطلق — هستهٔ ادغام آینده)
هدف: هیت‌مپ را به سطح پلتفرم‌های پولی برسانید و **مستقل از UI** قابل استفاده کنید.
- [ ] **پالت رنگی قابل تنظیم** (JSON theme: teal/gold/bluered/…، gamma، شفافیت، نرمال‌سازی per-view/per-column/global)
- [ ] **حالت تجمیع ستون**: آخرین اسنپ‌شات (فعلی) + Max + Mean + Volume-weighted
- [ ] **پیک‌هولد (Peak hold)**: رد تاریک کم‌رنگ برای بیشینهٔ ۱۰/۳۰ دقیقهٔ اخیر — دیوارهای مهم هرگز از چشم نمی‌افتند
- [ ] **Snapshot API**: `window.__ed_snapshot()` → PNG چارت + JSON دادهٔ مرئی (برای اتوماسیون/گزارش)
- [ ] **دیتا-بیرون‌دهی WS**: یک mini-service Node که state هیت‌مپ/کندل/ترید را به‌صورت WS ساده منتشر کند ← **این پل ادغام با ترمینال اصلی شماست**
- [ ] عمق بافر RT قابل تنظیم (۳۰min فعلی → ۲/۶/۱۲ ساعت با مدیر حافظه)

### فاز ۲ — «جنگ افزار لیکوییدیشن»
هدف: دیدن جایی که بازار می‌سوزد، قبل از دیگران.
- [ ] **مدل برآورد لیکوییدیشن خودتان**: از OI + mark price + توزیع اهرم فرضی → محاسبهٔ سطوح انفجار بدون نیاز به سرور EdgeDepth
- [ ] **Cascade detector**: شناسایی زنجیره (۳+ لیکوییدیشن در <۲ ثانیه در یک جهت) + مارکر ویژه + هشدار
- [ ] **Alert engine**: آستانهٔ دلاری لیکوییدیشن، ظاهر/خوردن دیوار، الگوی pull → صدا + Notification API مرورگر
- [ ] جدول «دیوارهای فعال» (widget جدید): سن، حجم اولیه/حاضر، touch count، فاصلهٔ قیمت
- [ ] Heat-map دوم: عمق انباشته در زمان (cumulative resting depth) به‌صورت لایهٔ اختیاری

### فاز ۳ — «چارت تریدینگ-گرید»
- [ ] ذخیره/بارگذاری **چیدمان لایه‌ها به‌صورت preset** («اسکالپ»، «سوئینگ»، «تحقیق»)
- [ ] هشدار متصل به اشکال Draw (عبور قیمت از Trendline/HLine → alert)
- [ ] چند-چارت sync (کراس‌هیر و زوم هماهنگ بین نمادها)
- [ ] نام‌گذاری و ذخیرهٔ الگوهای رسم (template lines: سطوح حمایت شخصی)
- [ ] گزینهٔ Heikin-Ashi روی RT (فعلاً فقط کندل/خط در RT)

### فاز ۴ — «ضبط و درک»
- [ ] **Recorder daemon**: سرویس Node که ۲۴/۷ همهٔ نمادهای watchlist را در .edpack ذخیره کند → آرشیو شخصی بی‌نهایت
- [ ] Event finder: جست‌وجو در آرشیو («همهٔ لحظه‌هایی که cascade>500k$ بود»)
- [ ] گزارش روزانهٔ خودکار (HTML/PNG از snapshot API فاز ۱)

### فاز ۵ — «اجرا و ریسک»
- [ ] توسعهٔ Paper trading فعلی: دستور ساختگی از روی alert، آمار PnL روزانه
- [ ] پل اجرای واقعی (اختیاری، سلف-هاست): webhook به صرافی/ربات شخصی — نسخهٔ شما AGPL است و می‌توانید
- [ ] Position size calculator متصل به Draw (R:R واقعی روی چارت)

### فاز ۶ — «داده و یکپارچه‌سازی»
- [ ] REST/WS سرور تحلیلی (gRPC هم شدنی) روی دادهٔ OrderbookManager — ترمینال اصلی شما به‌عنوان consumer
- [ ] پشتیبانی OKX/DeepCoin در گیت‌وی Go (الگوی Bybit موجود است)
- [ ] ذخیرهٔ تاریخی SQLite/Timescale از جریان نرمال‌شده

### فاز ۷ — «تجربهٔ کاربری فارسی»
- [ ] RTL + ترجمهٔ فارسی UI (theme/menu لایه‌بندی شده، کوتاه‌تر از ۲۰۰ رشته)
- [ ] اعداد فارسی اختیاری در تیک‌ها
- [ ] دارک/لایت + ذخیرهٔ تم JSON

### فاز ۸ — «پایداری و توزیع»
- [ ] CI گیت‌هاب: build WASM + تست‌های CTest (~۸۰ تست موجود) روی هر push
- [ ] نسخه‌بندی SemVer + صفحهٔ CHANGELOG
- [ ] auto-update checker (version.json در githack/pages)
- [ ] بهینه‌سازی: بررسی SIMD، کاهش ۵.۹MB واسم، کش مرورگر هش-دار

---

## 💎 فهرست امکانات «پریمیوم» پیشنهادی (تفکیک‌کنندهٔ رایگان/پول در نسخهٔ خودتان)

اگر روزی خواستید نسخهٔ شما هم سطح‌بندی داشته باشد (یا فقط برای خودتان حس premium بسازید)، این‌ها همان تفکیک‌کننده‌هایی هستند که رقبای پولی می‌فروشند:

| ردیف | امکان | چرا پریمیوم است | فاز |
|---|---|---|---|
| ۱ | هیت‌مپ زندهٔ GPU + کندل هم‌پوشان | هستهٔ اصلی ارزش | ✅دارید |
| ۲ | Peak-hold دیوارها + جدول Walls زنده | سیگنال نهایشی صریح | ۱-۲ |
| ۳ | مدل برآورد لیکوییدیشن اختصاصی (OI-based) | دانش انحصاری مدل | ۲ |
| ۴ | Cascade + Alert engine صوتی/پوش | سرعت واکنش | ۲ |
| ۵ | Recorder ۲۴/۷ شخصی (.edpack بی‌نهایت) | آرشیو اختصاصی | ۴ |
| ۶ | API/WS خروجی برای ربات و ترمینال دوم | بستر اتوماسیون | ۱/۶ |
| ۷ | Workspace ابری + همگام‌سازی چند-دستگاه | چسبندگی محصول | ۶ |
| ۸ | چند-چارت هم‌نفس (sync crosshair) | حرفه‌ای‌ها | ۳ |
| ۹ | Footprint پیشرفته (delta matrix + imbalance سفارشی) | تریدرهای order-flow | ۳ |
| ۱۰ | لایهٔ دلتا-دلتا: Spot↔Perp basis heatmap | دید آربیتراژ | ۶ |

**الگوی gating پیشنهادی برای نسخهٔ شما:** همان `entitlements.h` موجود را نگه دارید؛ فقط سقف tier را خودتان تعریف کنید (مثلاً `local_pro`) و UI قفل‌ها را روی همان slugها فعال کنید — زیرساختش کامل ساخته شده و شما فقط کلیدش را برمی‌دارید.

---

## ⚖️ یادآوری مجوز (مهم برای ادغام آینده)
- ریپو **AGPL-3.0**: استفادهٔ شخصی/توسعه آزاد؛ اگر نسخهٔ تغییریافته را «به دیگران از طریق شبکه سرویس کنید» باید سورس را هم در دسترس بگذارید.
- گیت‌وی Go **MIT**: آزاد کامل، حتی تجاری.
- برای ادغام در ترمینال اصلی خودتان (استفادهٔ شخصی): **هیچ محدودیتی نیست**؛ فقط کپی‌رایت AGPL را در فایل‌های منتقل‌شده حفظ کنید.

---
*ساخته‌شده در بازبینی Task R4 — جزئیات فنی هر لایه در کامنت‌های سورس و `docs/` ریپو موجود است.*
