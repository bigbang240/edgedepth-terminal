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

### فاز ۱ — «هیت‌مپ حرفه‌ای» (اولویت مطلق — هستهٔ ادغام آینده) — ✅ تحویل‌شده
هدف: هیت‌مپ را به سطح پلتفرم‌های پولی برسانید و **مستقل از UI** قابل استفاده کنید.
- [x] **پالت رنگی قابل تنظیم** (JSON theme: teal/gold/bluered/…، gamma، شفافیت، نرمال‌سازی per-view/per-column/global) — PR#2
- [x] **حالت تجمیع ستون**: آخرین اسنپ‌شات (فعلی) + Max + Mean + Volume-weighted — PR#2
- [x] **پیک‌هولد (Peak hold)**: رد تاریک کم‌رنگ برای بیشینهٔ ۱۰/۳۰ دقیقهٔ اخیر — PR#2
- [x] **Snapshot API**: `window.__ed_state` (mirror دوبار در ثانیه) + ضبط canvas از `captureStream` — PR#2
- [x] **دیتا-بیرون‌دهی WS**: پل JSON-WS روی سیم protobuf گیت‌وی (feat(egress) در edgedepth-gateway master)
- [ ] عمق بافر RT قابل تنظیم (۳۰min فعلی → ۲/۶/۱۲ ساعت با مدیر حافظه) — افق مرورگر تا ۱۲h از `__EDGEDEPTH_RT_ARCHIVE_MS__`

### فاز ۲ — «جنگ افزار لیکوییدیشن»
هدف: دیدن جایی که بازار می‌سوزد، قبل از دیگران.
- [x] **مدل برآورد لیکوییدیشن خودتان**: از OI + mark price + توزیع اهرم فرضی → محاسبهٔ سطوح انفجار بدون نیاز به سرور EdgeDepth — S3 (`core/liq_levels.h`): چگالی ورود از نوار معاملات ۱۵ دقیقه‌ای × ترکیب اهرم (کلاه نماد از liq_field) → خوشه‌های USD-لنگر‌شده به OI واقعی؛ نوارهای افقی قرمز (فورس‌سل لانگ) / سبز (فورس‌بای شورت) با لیبل و tooltip در نمای RT، لایهٔ «Est. liquidations» در منوی Layers، بازمحاسبهٔ throttle ۲s
- [x] **Cascade detector**: شناسایی زنجیره (۳+ لیکوییدیشن در <۲ ثانیه در یک جهت) + مارکر ویژه + هشدار — باند کهربایی زمان/قیمت + pill «CASCADE» روی هیت‌مپ، toast + Notification + بیپ (`core/cascade_detector.h`)
- [x] **Alert engine**: آستانهٔ دلاری لیکوییدیشن (تک‌ریکوییدیشن ≥ آستانه، ددوپ ۲s)، هشدار خوردن دیوار (pull ردیابی‌شدهٔ rt_walls) → toast + Notification + بیپ (کاسکید 660Hz / لیکوییدیشن 880Hz / دیوار 520Hz) — S2 (`core/alert_engine.h`)
- [x] جدول «دیوارهای فعال» (widget جدید): سن، حجم اولیه/حاضر، touch count، فاصلهٔ قیمت — S4 (`render_realtime_walls_table`): پنل همیشه‌نمایان بالا-راست داخل نمودار، sort بر اساس حجم، pip رنگی bid/ask، شمارش تاچ بر پایهٔ ثانیه در `rt_walls::Tracker::count_touches`، toggle «Active walls table» زیر لایهٔ Walls
- [x] Heat-map دوم: عمق انباشته در زمان (cumulative resting depth) به‌صورت لایهٔ اختیاری — S5 (`render_realtime_cum_depth`): میانگین نمایی اندازهٔ هر سطح قیمت با حافظهٔ ~۱۰ دقیقه (کاهیدهٔ تنبل هنگام خروج سطح)، ردیف‌های طلایی روی هیت‌مپ برای قفسه/دیوارهای ماندگار، نرمال‌سازی درون دید، tooltip اندازهٔ استراحت، لایهٔ «Resting depth heat» در منوی Layers، ذخیره در workspace، تجمیع همیشگی (آماده هنگام فعال‌سازی)

### فاز ۳ — «چارت تریدینگ-گرید»
- [x] ذخیره/بارگذاری **چیدمان لایه‌ها به‌صورت preset** («اسکالپ»، «سوئینگ»، «تحقیق») — بخش «LAYER PRESETS» در منوی Layers نمای RT: سه چیدمان یک‌کلیکی (Scalp: کندل توخالی 1s + دیوارها و پول‌ها + CVD + بابل‌ها + هشدارهای سریع / Swing: کندل 1m + برآورد لیکوییدیشن + عمق انباشته، نویز خاموش / Research: همه‌چیز روشن شامل جدول دیوارها و peak-hold) — `apply_layer_preset`، آخرین preset انتخابی در workspace ذخیره می‌شود (`rt_layer_preset`)
- [x] هشدار متصل به اشکال Draw (عبور قیمت از Trendline/HLine → alert) — هر HLine/HRay به‌صورت سطح افقی و هر خط دو-لنگری (Trendline/Ray/ExtendedLine/Arrow) به‌صورت سطح تصویرشده در زمان حال پایش می‌شود؛ عبور در هر دو جهت → toast + Notification + بیپ 740Hz (helper مشترک `alert_notify_beep`)؛ سطح مصرف نمی‌شود (برخلاف price alert یک‌بارمصرف) و تا حذف خط مسلح می‌ماند؛ toggle «Drawing alerts» در گروه Alerts منوی RT، ذخیره در workspace (`drawing_alerts`)؛ در هر دو حالت RT (آخرین قیمت معامله/میانگین quote) و کندلی (last close) کار می‌کند
- [x] چند-چارت sync (کراس‌هیر و زوم هماهنگ بین نمادها) — تاگل «Link charts» در منوی RT (کنار Side panels): همهٔ چارت‌های باز (هر نماد، لایو/ریپلی) یک گروه می‌شوند؛ کراس‌هیر به‌صورت خط‌چین عمودی روی بقیه چارت‌ها آینه می‌شود. دو خانوادهٔ مستقل: چارت‌های RT zoom span مالک را همگام می‌کنند (لبهٔ راست هر چارت از ساعت خودش، view مکث‌شده جابه‌جا نمی‌شود) و چارت‌های کندلی محور کامل را؛ mirror کراس‌هیر بین هر دو خانواده کار می‌کند. فریم-استمپ مالکیت (hover = مالک)، میراث لینک compare هم حفظ شد — ذخیره در workspace (`link_charts`)
- [x] نام‌گذاری و ذخیرهٔ الگوهای رسم (template lines: سطوح حمایت شخصی) — بخش «TEMPLATES» در منوی Draw: نام + دکمهٔ Save = ذخیرهٔ همهٔ رسم‌های نماد فعلی؛ کلیک روی هر الگو = جای‌گذاری روی چارت فعلی (id تازه، زمان‌ها به "الان" re-base می‌شوند، قیمت‌ها مطلق می‌مانند)، راست-کلیک = حذف. فروشگاه سراسری در localStorage (`edgedepth.drawtemplates.v1`، تا ۶۴ الگو) — بین نمادها و نمادهای دیگر مشترک
- [x] گزینهٔ Heikin-Ashi روی RT (فعلاً فقط کندل/خط در RT) — toggle «Heikin-Ashi» زیر «1s candles» در Layers: تبدیل HA روی باکت‌های RT (open = میانهٔ HA قبلی، close = OHLC4 خام؛ باکت درحال‌شکل‌گیری هم همین‌طور) — دقیقاً هم‌فرمول سری تاریخی HA؛ با hollow candles و ذخیرهٔ workspace (`rt_heikin`) سازگار است

### فاز ۴ — «ضبط و درک» — ✅ تحویل‌شده
- [x] **Recorder daemon**: سرویس Node که ۲۴/۷ همهٔ نمادهای watchlist را در .edpack ذخیره کند → آرشیو شخصی بی‌نهایت — `tools/recorder/` (سرویس bun، پورت ۹۰۰۲): اتصال WS به gateway، اشتراک trades/orderbook/liquidations/candles(1m)، بلاک‌بندی zstd (~۴MiB)، هدر PackHeader با ob_seed (اسنپ‌شات اول) و candle_seeds، اپیزود ساعت‌به‌ساعت + manifest.json با سازگاری replay-library (schema v1)؛ HTTP با پشتیبانی Range/ETag/CORS؛ resolve نسبتی pack_url در ویجت کتابخانه (fetch جاوااسکریپت) تا کاتالوگ شخصی با `?replayLibrary=` باز شود
- [x] **Event finder**: جست‌وجو در آرشیو («همهٔ لحظه‌هایی که cascade>500k$ بود») — رکوردر هر لحظهٔ معنادار را زنده ایندکس می‌کند: cascade (زنجیرهٔ ۳+ لیکوییدیشن هم‌جهت با گپ <۲s — همان قواعد cascade_detector ترمینال)، تک-لیکوییدیشن، فیل‌های بزرگ (پیش‌فرض ≥$250K، `RECORDER_BIGTRADE_USD`) و اسپایک حجم ۱m (≥۵× میانگین ساعت، `RECORDER_VOL_SPIKE_X`)؛ ذخیرهٔ append-only در `data/events/events-YYYYMMDD.jsonl` + حلقهٔ حافظه (۵۰هزار رویداد) که از restart جان سالم به در می‌برد؛ endpoint `/events?kind=&min_usd=&symbol=&from=&to=&limit=` با schema `edgedepth.events.v1` و resolve پکِ حاوی هر رویداد در زمان کوئری؛ بخش «EVENT FINDER» در ویجت Replay Library (segmented All/Cascades/Liquidations/Big fills/Volume spikes + کف دلاری + فیلتر نماد) که هر نتیجه را با یک Play به‌صورت «پک + سیک عمدی به ۴۵ ثانیه قبل از لحظه» باز می‌کند (تلاشِ پک هرس‌شده به‌صراحت «pruned» می‌شود)؛ URL فید از دایرکتوری/کوئری manifest URL مشتق می‌شود تا پسوند XTransformPort سلف-هاست دست‌نخورده بماند
- [x] **گزارش روزانهٔ خودکار**: رکوردر هر روز (مرز نیمه‌شب محلی، `RECORDER_TZ_OFFSET_MIN`، پیش‌فرض +۲۱۰ تهران) گزارش HTML خودکفای روز قبل را در `data/reports/` می‌نویسد + نسخهٔ زندهٔ «امروز» هر ۳۰ دقیقه؛ `/report?date=YYYY-MM-DD` تولید لحظه‌ای، `/reports` فهرست JSON؛ شامل: KPI (O/H/L/C/Net، مجموع لیکوییدیشن longs/shorts، تعداد و بزرگ‌ترین cascade، فیل‌های ≥250K، اسپایک‌ها)، نمودار SVG قیمت ۱m با حباب‌های لیکوییدیشن (اندازه ∝ USD، قرمز=فورس‌سل لانگ/سبز=فورس‌بای شورت) و باند کهربایی cascade‌ها، میله‌های دلاری لیکوییدیشن به‌تفکیک ساعت محلی، جدول‌های cascade/بزرگ‌ترین لیکوییدیشن‌ها/فیل‌ها/اسپایک‌ها، هر ردیف با لینک «open in terminal» (پک + packt = لحظه − ۴۵s، ساخته‌شده در زمان کلیک از origin مرورگر تا پشت گیت‌وی هم درست کار کند)؛ گالری «Terminal snapshots» از snapshot API فاز ۱: پل جاوااسکریپت لندینگ (page.tsx) هر ۱۰ دقیقه با captureStream یک فریم از canvas ترمینال به `POST /snapshot` می‌فرستد و رکوردر آخرین ۱۲ تصویر روز را نگه می‌دارد؛ دکمهٔ «Daily report» در بخش Event finder ترمینال گزارش زندهٔ امروز را در تب جدید باز می‌کند

### فاز ۵ — «اجرا و ریسک» — ✅ تحویل‌شده
- [x] **Position size calculator متصل به Draw** — گروه «Execution» در منوی RT + بخش ابزارهای Position در style editor: Account ($) و Risk per trade (%) سراسری (در workspace ذخیره می‌شوند، `poscalc_account`/`poscalc_risk_pct`/`poscalc_show`)؛ چیپ بازهٔ Long/Short Position سطر سوم می‌گیرد: «size <qty> · $<notional> @ risk $<r>» — qty = account×risk% / فاصلهٔ stop با کپ 20x ناتینگال (`core/position_sizing.h`)؛ فیکس موقعیت چیپ در RT: لبهٔ راست پیش‌فرض +۱۰ دقیقه مرکز چیپ را بیرون از پلات می‌برد (RR chip قدیمی هم همین‌طور بود) — حالا مرکز روی بازهٔ مرئی باکس با clamp داخل پلات
- [x] **Paper trading محلی (موتور سمت-کلاینت)** — `core/local_paper_engine.h/.cpp`: استریم STREAM_PAPER_TRADING در سلف-هاست مرده است (گیت‌وی Go آن را سرو نمی‌کند) → موتور کامل محلی: باز شدن خودکار سفارش از روی crossing های alert (قیمتی و خط رسم‌شده — «Paper on alerts» با stop% قابل تنظیم، TP1=1R نصف، TP2=2R ربع، runner با تریل 1R بعد از TP1، MAE/MFE، R-multiple)، mark-to-market از مسیر قیمت خود چارت (تیک RT/مید quote/کلوز کندل)، خروج با stop/trail/manual + دکمهٔ Close هر ردیف و Flatten all؛ ذخیره در localStorage (`edgedepth.localpaper.v1`، debounce ۱s، بازمانده از reload)؛ ویجت Paper Trading: نوار خلاصهٔ LOCAL (Balance/Equity/WR/PF/MaxDD)، جدول positions محلی، Journal محلی، جدول «Daily P&L (local)» با هایلایت امروز در تب By Type
- [x] **پل اجرای واقعی (وب‌هوک، اختیاری، سلف-هاست)** — `core/execution_bridge.h/.cpp`: toggle «Webhook bridge» + InputText URL + «Include paper fills» + خط وضعیت «last: …» در گروه Execution؛ روی هر alert و باز/بسته شدن paper یک POST JSON (kind/exchange/symbol/side/price/msg/ts_ms) با fetch سمت JS (الگوی EM_JS — `-sFETCH` خاموش است) fire-and-forget می‌شود؛ وضعیت آخرین POST در منو نمایش داده می‌شود؛ مقصد باید CORS این origin را بپذیرد (در tooltip مستند شده — تست با رکوردر: dispatch انجام و «Failed to fetch» preflight صادقانه گزارش شد)

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
