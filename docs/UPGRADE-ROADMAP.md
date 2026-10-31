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
**موجود:** منوی Layers با زیرصفحه: Price Levels (VWAP روزانه، H/L/C روز/هفتهٔ قبل، VWAP انکر شده)، Order book depth، Trade bubbles، VPVR، Liquidation map، HL positions، Reported liquidations، Scenario profile، Refill levels/history، Absorption bands، Touch-odds، Compression overlay. هر ردیف: toggle + قفل PRO + ذخیره در workspace. چرخش دو منوی مستقل: Layers و Liquidations. بعد از فاز ۹: لایه‌ست نام‌دار کاربر، شفافیت per-layer، کلیدهای میان‌بر Alt و لایه‌های Whale trail / Venue basis / Slippage cone / Layer events هم اضافه شدند.

**شکاف:** ترتیب/مکاش‌پذیری لایه‌ها (z-order UI) ندارند؛ لایهٔ کاربر-سازیشه (import لایهٔ شخصی) ندارد.

### 5) لایهٔ Widgets — `src/ui/widget.h` + `Menu::resolve_widget_add_request`
**موجود:** ۱۲ نوع widget: Chart (چند-نمونه)، Orderbook (۲۵ سطح)، DOM، Trades، Stats، Data&Research، Watchlist، PositionsPanel (Paper trading سه‌تب)، ReplayLibrary، DebugLog، ResearchMomentPanel. سیستم dockspace کامل ImGui (کشیدن/رها کردن، چیدمان‌های پیش‌فرض، reset، حالت companion برای DOM/Trades). روتین به‌روزرسانی هر widget (Turbo/RealTime/Standard/Slow).

**شکاف:** widget سادهٔ Alert/Notification وجود ندارد؛ widget سفارشی کاربر (HTML embed) ندارد؛ ذخیرهٔ چند-چیدمان نام‌گذاری‌شده در نسخهٔ فعلی محدود است.

### 6) لایهٔ Draw (رسم) — `src/ui/drawing/` + `core/drawing_manager.cpp`
**موجود:** ۱۹ ابزار: Trendline/Arrow/Ray/Extended/HLine/HRay/VLine/CrossLine/Rectangle/Brush/Measure/PriceRange/DateRange/Fibonacci (۶ سطح)/Long-Short Position (با R:R)/Text/Parallel Channel/Polyline. لنگر (زمان،قیمت) → پایدار در pan/zoom/tf/replay. ذخیرهٔ localStorage به تفکیک نماد، merge چند-پنجره‌ای، **Undo تا ۶۴ مرحله**، مغناطیس (magnet)، style editor.

**شکاف:** شکل‌ها قابل «نام‌گذاری و ذخیره به‌عنوان الگو» نیستند؛ هشدار متصل به شکل (مثلاً «قیمت به خط رسید») ندارد؛ sync ابری ندارد.

### 7) لایهٔ DOM — `dom_widget.h` + `realtime_dom_ladder.h`
**موجود:** DOM جدا (۶ ستونه با تجمیع محلی معاملات) + **DOM Ladder متصل به RT** (ستون سمت راست چارت تا ۴۵٪ عرض): قفل هم‌ترازی bucket با هیت‌مپ، ردیف‌ها با هیت‌مپ هم‌مرز، ستون‌های bid/ask/buy/sell + نوار عمق، حالت "Last known depth" هنگام قطعی.
**شکاف:** depth-diff animation (نمایش انیمیشنی بزرگ‌شدن/خوردن دیوارها) ندارد؛ heatmap دوم (عمق انباشته در زمان) در خود ladder ندارد. ← «depth-diff animation» در فاز ۱۱ بسته شد.

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
- [x] **عمق بافر RT قابل تنظیم** (۳۰min فعلی → ۲/۶/۱۲ ساعت با مدیر حافظه) — انتخابگر «Archive depth» در منوی Real-time، پایدار در workspace؛ سقف زوم و بودجهٔ کارگر IndexedDB از `RealtimeArchive::target_ms` پیروی می‌کنند و افق مرورگر تا ۱۲h از `__EDGEDEPTH_RT_ARCHIVE_MS__` قابل بالا بردن است

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
- [x] **REST/WS سرور تحلیلی** روی دادهٔ نرمال‌شدهٔ OrderbookManager — ترمینال اصلی شما به‌عنوان consumer: در رکوردر (`tools/recorder/analytics.ts`)، کتاب سفارش زنده با همان نتیک snapshot+delta کلاینت (best bid/ask، mid، اسپرد bps، عدم‌توازن، ناتینگال بای/سل در پنجرهٔ عمق قابل‌تنظیم ±۲۵bps، هیستوگرام عمق تا ۲۵ باکت) + جریان معاملات (CVD سشن/۵m، نرخ معامله و ناتینگال ۶۰s، ‏VWAP ۵m، نسبت خرید) + جمع لیکوییدیشن ۵m/۶۰m تفکیک سمت + آخرین کندل ۱m؛ ‏REST ‏`GET /analytics` (schema `edgedepth.analytics.v1`) و پوش WS روی `/analytics/ws` هر ۱s (سابسکرایب JSON: `{"method":"subscribe","data":{"symbols":["btcusdt"]}}`؛ null = همه)؛ venue-agnostic آزمون‌شده روی binancef و okx
- [x] **پشتیبانی OKX در گیت‌وی Go** (`internal/okx` — الگوی adapter.go بایننس): کانال‌های books/trades/mark-price/funding-rate/open-interest روی `/ws/v5/public` + liquidation-orders (نیازمند `instType`، همان ریشهٔ public — بدون `/business` درست می‌شود)؛ تبدیل واحد قرارداد→کوین با ctVal در نقطهٔ ورود؛ نرمال‌سازی case-insensitive نماد (deep link حروف کوچک هم روت می‌شود)؛ تیکر سراسری REST-poll هر ۳۰s (OKX پوش همه‌بازار ندارد)؛ کندل تاریخی REST با پشتیبانی 1s و ادامهٔ صفحه‌بندی از history-candles؛ ثبت در `hub.New(log, binance.New(log), okx.New(log))`. سمت ترمینال: venue id «okx» در url_router + watchlist + subscribe تیکر؛ جهانی‌سازی universe خود-سرو (نمادهای venue هایی که API متادیتا ندارد از تیکر خودش ساخته می‌شود) + **tick ضمنی از کتاب زنده** (کمینهٔ گپ اسک‌های snapshot → seed_tick؛ قاعدهٔ «پاسخ API همیشه برنده» حفظ شده). hardening گیت‌وی حین bring-up: ‏prime اشتراک حالا زیر قفل نوشتن hub ساخته/ارسال می‌شود (نظم prime نسبت به broadcast تضمین می‌شود) و diff های OKX ‏FirstUpdateId=seq می‌فرستند (معادل U-range بایننس؛ فقط event دقیقاً بعدی از bridge می‌گذرد). **مشکل شناخته‌شده — بسته شد**: زنجیرهٔ depth ترمینال روی OKX گاهی «Recovering depth» می‌زد (هر ~۵s خودترمیم می‌شد؛ نقشهٔ گیت‌وی و استریم خام OKX هر دو بی‌نقص اثبات شده بودند). ریشه در سمت کلاینت بود: `check_delta` پل نمایش/تاریخچه **قبل از** verdict گیت سخت‌گیرانهٔ continuity صدا زده می‌شد و هر duplicate/out-of-order که گیت عمداً silent-drop می‌کند، پل را «Depth sequence gap» می‌زد؛ حالا صدا زدن **بعد از** verdict است — رویدادهای drop شده پل را نمی‌شکنند و گپ واقعی مثل قبل invalidate می‌کند (`orderbook_manager.cpp`). تماشای ۳ دقیقه‌ای پیوستهٔ OKX BTC-USDT-SWAP بدون هیچ فلاپ + صفر resubscribe گیت‌وی. یافتهٔ جانبی هم‌زمان: تیکر سراسری OKX در گیت‌وی روی عدد خالی `""` قراردادهای pre-market (مثل MP-USDT-SWAP) کل فریم را رد می‌کرد و یونیورس OKX watchlist را صفر می‌کرد — با `flexNum` tolerant در `internal/okx/rest.go` بسته شد (۵۲۴ نماد پایدار)
- [x] **ذخیرهٔ تاریخی SQLite از جریان نرمال‌شده** — در رکوردر (`tools/recorder/history.ts`، ‏`bun:sqlite` بدون وابستگی جدید، WAL): جدول‌های `trades` (هر معامله)، `liquidations`، `candles_1m` (تکمیل‌شده‌ها) و `book_polls` (نمونهٔ top-of-book هر ۵s با ناتینگال ±۲۵bps و شمار سطح‌ها)؛ درج دسته‌ای در تراکنش (هر ۲s یا ۵۰۰ ردیف) کاملاً خارج از مسیر داغ WS؛ نگه‌داری پیش‌فرض ۳ روز (`RECORDER_HISTORY_DAYS`، prune ساعتی + `wal_checkpoint(TRUNCATE)`)؛ ‏REST ‏`/history/{trades,liquidations,candles,book}?symbol=&from=&to=&limit=` (schema `edgedepth.history.v1`) + آمار در `/history` و خلاصه در `/`

### فاز ۷ — «تجربهٔ کاربری فارسی» — ✅ تحویل‌شده (20251020e/f)
- [x] RTL + ترجمهٔ فارسی UI — لایهٔ i18n (`src/core/i18n_fa.h` تولیدشده با gen-i18n-fa.py): ~۶۸ رشتهٔ پیش‌شکل‌شده در presentation forms (رندر درست بدون HarfBuzz)، فونت Vazirmatn subset روی همهٔ face ها، پنل Appearance با سوییچ English/فارسی؛ ترجمهٔ chrome/toolbar/Layers/watchlist/DOM/status
- [x] اعداد فارسی اختیاری در تیک‌ها — checkbox «اعداد فارسی» (فعلاً نوار هدر/آمار؛ محور قیمت لاتین می‌ماند — hook در `i18n::digits` برای ادامه)
- [x] دارک/لایت + ذخیرهٔ تم JSON — پریست Daylight (روشن) + ذخیره/بارگذاری snapshot پوسته در `edgedepth.theme.v1` با کپی/پیست JSON

### فاز ۸ — «پایداری و توزیع» — ✅ تحویل‌شده
- [x] CI گیت‌هاب: build WASM + تست‌های CTest روی هر push (Actions فعال‌سازی شد، تست‌ها با g++-14 پین شدند، هر دو ریپو سبز)
- [x] نسخه‌بندی SemVer (0.7.0) + صفحهٔ CHANGELOG در هر دو ریپو + `VERSION`
- [x] auto-update checker — `version.json` در deploy سلف-هاست + بنر کهربایی «New build» با reload با کلیک (بررسی هنگام بارگذاری + هر ۵ دقیقه)
- [x] بهینه‌سازی: SIMD از پیش فعال (`-msimd128`)، بیلد Release بدون debug info با `-O3 -flto`، کش مرورگر هش‌دار با `?v=<tag>` — یافته‌ها در CHANGELOG ثبت شد

### فاز ۹ — «سوئیت مدیریت لایه‌ها» (Wave 1-3) — ✅ تحویل‌شده (20261026b + 20261028b)
بر پایهٔ پیشنهادهای بخش Layers، در سه موج با بازبینی مرورگر و push جداگانه:
- [x] **Wave 1 — مدیریت**: مجموعه‌لایهٔ نام‌دار کاربر («MY LAYER SETS»: کلیک = اعمال، راست‌کلیک = حذف، ≤۱۲ مجموعه، snapshot کامل سوییچ‌های RT + باکت کندل، ماندگار در workspace)؛ شفافیت per-layer (عمق RT، حباب‌ها، حرارت ماندگار — اسلایدر در صفحهٔ تنظیم هر لایه)؛ کلیدهای میان‌بر Alt+1..0 با گارد WantTextInput/popup و چیپ «Alt+n» روی همهٔ ردیف‌ها؛ انتخابگر عمق آرشیو (فاز ۱ را هم بست)
- [x] **Wave 2 — لایه‌های دادهٔ جدید**: Whale trail (زنجیرهٔ حریصانهٔ clip های بزرگ هم‌سمت روی آرشیو حباب‌ها: پنجرهٔ ۵/۸/۱۵ ثانیه، نزدیکی قیمت ۴۰تیکی، هاله روی peak clip — `core/whale_trail.h`؛ اکتشافی — فیدهای عمومی id سفارش ندارند)؛ Venue basis (OKX) (آخرین پرپ OKX در برابر mid چارت به bps، روبان ۱۲۰ ثانیه‌ای در کف پلات، نمونه‌برداری ۱/s از TickerManager مشترک — بدون اشتراک جدید؛ gated روی has_venue("okx"))
- [x] **Wave 3 — یکپارچه‌سازی**: ویجت Layer events (رینگ ۲۴تایی: cascade / لیکوییدیشن بالای آستانه / wall pull / مصرف refill؛ رنگ تگ بر اساس سمت)؛ هشدار «Refill consumed» (سطوح exhausted با آبستراکت اجراشده ≥ آستانهٔ دلاری قابل‌ویرایش → toast + Notification + بیپ ۴۴۰Hz)؛ Slippage cone (پیمایش کتاب زنده برای سفارش بازار شبیه‌سازی‌شده: خطوط VWAP خط‌چین + چیپ «N BTC - slip buy X sell Y bps»؛ پریست ۰.۵/۱/۵/۱۰/۲۵ + اندازهٔ دقیق)
- [x] **پاس فارسی Wave ها**: همهٔ ردیف‌ها/سکشن‌ها/فیلدها/نوت‌های جدید (رد پای نهنگ، بیسیس صرافی، مخروط لغزش، رویدادهای لایه، دیوارها، سطوح ری‌فیل، مجموعه‌های من، شفافیت، عمق آرشیو و…) در جدول i18n (`gen-i18n-fa.py` — شکل‌دهی خط‌به‌خط برای رشته‌های چندخطی؛ تولتیپ‌های بلند مطابق الگوی فاز ۷ انگلیسی می‌مانند)

### فاز ۱۰ — «هوش رد پای نهنگ + تکمیل فارسی» — ✅ تحویل‌شده (20261028e)
ادامهٔ فاز ۹: الگوریتم Whale Trail سخت‌گیرانه‌تر شد و آخرین ردیف‌های انگلیسی منوها هم فارسی شدند:
- [x] **Whale Trail v2** (`core/whale_trail.h`): اتصال **best-fit** به‌جای first-fit — امتیاز ادامه = `۰٫۷۵×(گام قیمت/سقف) + ۰٫۲۵×(فاصلهٔ زمانی/پنجره)`؛ با دو نهنگِ هم‌زمان، هر پرینت به زنجیره‌ای می‌رود که واقعاً نزدیک‌ترین ادامهٔ اوست. **گیت ارزش**: کلیپ فقط با ≥۱۵٪ قلهٔ زنجیره به آن می‌چسبد (ترافیک متوسط وسط دو نهنگ دیگر زنجیره‌ها را نمی‌دزدد). **سقف پهنای زنجیره** ۹۰ ثانیه (ردپای زامبی که با پرینت‌های تصادفی دقایق زنده می‌ماند حذف می‌شود). آمار `total_value`/`span_ms` برای رندر
- [x] **رندر v2**: ضخامت خط با log(ارزش کل) زنجیره (۱٫۵ تا ۳٫۴px)، آلفای سگمنت‌ها به سمت اکنون روشن‌تر (جهت حتی در هم‌پوشانی خوانده می‌شود)، سرچرخانِ فلش روی آخرین لبه، هالهٔ قله مثل قبل
- [x] **گام قیمت قابل‌انتخاب**: ردیف «Price step» در تنظیمات رد پای نهنگ — باریک ۲۰ / معمولی ۴۰ / باز ۸۰ تیک (قبلاً ۴۰ هاردکد بود)؛ ذخیره در workspace (`rt_whale_step_ticks`) و اعمال زندهٔ زنجیره‌ها
- [x] **تکمیل کامل پاس فارسی** (۱۹۵ رشته از ۱۰۹): کل منوی Real-time (توقف نمایش، دنبال کردن قیمت، بازگشت به زنده، کل نشست، پنجرهٔ حرارت، نردبان عمق، پنل‌های کناری، همگام‌سازی چارت‌ها، اندازهٔ کندل، هشدارها — کاسکید/لیکوییدیشن‌های بزرگ/برداشت دیوارها/آستانه‌ها، اجرای معامله — پوزیشن کاغذی/حساب/ریسک/پل وب‌هوک/فیل‌های کاغذی، تاریخچهٔ نشست)، ردیف‌های تنظیمات جامانده (لوزی‌ها، زیرنمودار فعالیت، برچسب ارزش، پروفایل در لبهٔ راست، رنگ/پیشرو/دلاری، بیشینهٔ اندازه + فشرده/استاندارد/بزرگ، اندازهٔ خودکار بازار، کال‌اوت‌ها، نوار فعالیت/خطوط/نشانگرهای دیده‌بان، ریل‌های شانس لمس + سوخت، بازبینی فشردگی ۱/۴ساعته، گزینه‌ها)، سکشن‌های حالت کندل (سطوح قیمت/سطوح پوزیشن/رویدادهای اعلامی/نمای زنده)، آیتم‌های ابزاری (گزارش روزانه، سبک نقشه، بازتنظیم اندازه‌ها، حذف VWAP لنگرشده، بارگذاری کندل‌های قدیمی‌تر، تنظیمات پروفایل، تلاش دوباره برای تاریخچه) و نوت‌های مرئی منو؛ تولتیپ‌های hover مطابق پیشینهٔ فاز ۷ انگلیسی می‌مانند

### فاز ۱۱ — «فوترپرینت پیشرفته + نردبان عمق زنده» — ✅ تحویل‌شده (20261028f)
دو شکاف باقی‌ماندهٔ نقشهٔ راه: ردیف ۹ لیست پریمیوم (فوترپرینت پیشرفته) و شکاف بخش ۷ (depth-diff animation):
- [x] **حالت Matrix (ماتریس دلتا)** (`footprint_manager.h` + `chart_widget_footprint.cpp`): حالت پنجم فوترپرینت — هر ردیف چهار ستون عددی «سل‌ها | خریدها | دلتا | حجم» با پالت تیرهٔ هم‌خانوادهٔ Cluster (سل قرمزِ کم‌رنگ / خرید سبزِ کم‌رنگ / دلتا علامت‌دار / حجم خنثی)؛ در عرض کم <۹۲px ستون حجم حذف و دلتا پهن می‌شود؛ ذخیره در workspace (mode تا ۴ خوانده می‌شود)
- [x] **واگرایی دلتا** (`show_delta_divergence`): مارکر مثلثی جذب/absorption — سقف بالاتر با دلتای ضعیف‌تر از کندل قبل = ▼ قرمز بالای سقف؛ کف پایین‌تر با دلتای قوی‌تر = ▲ سبز زیر کف؛ فقط کندل‌های بسته و آشتی‌یافته (بدون observed/provisional) و با اختلاف ≥۲۵٪ بزرگ‌ترین دلتای دو کندل (ضد نویز)؛ toggle در تب Display + ذخیره در workspace
- [x] **Depth-diff animation روی نردبان عمق** (`realtime_dom_ladder.{h,cpp}`): هر ردیف نسبت به نمونهٔ قبلی کتاب مقایسه می‌شود — پرش ≥۲٪ مقیاس عمق مرئی **و** ≥۳۰٪ سطح = فلاش سبز؛ خوردنِ ≥۵۰٪ سطح = فلاش قرمز، اگر سطح یک دیوار (≥ مرجع p90) بوده کندتر محو می‌شود؛ عدد تغییر امضادار (+۲٫۴ / ‎-۱٫۳K) در لبهٔ بیرونی سلول عمق تا پایان فلاش؛ decay بر پایهٔ DeltaTime (۰٫۵۵s عادی / ۱٫۴s دیوار)، پاک‌سازی دوره‌ای باکت‌های خارج دید، reset هنگام تغییر bucket؛ toggle «Depth changes» زیر «Depth ladder» در منوی RT (فارسی: تغییرات عمق)، ذخیره در workspace (`rt_dom_diff`)
- [x] پاس i18n: کلید «depth_changes» به جدول فارسی اضافه شد (۱۹۶ رشته)

### فاز ۱۲ — «بیسیس اسپات-پرپ» — ✅ تحویل‌شده (20261029a)
آخرین ردیف باز جدول پریمیوم (ردیف ۱۰) بسته شد — با یک venue فقط-تیکری در گیت‌وی و یک نوار حرارتی در ترمینال:
- [x] **آداپتور اسپات گیت‌وی** (`internal/binance` + `cmd/edgedepth-gateway`): venue «binancespot» فقط-تیکری — همان !ticker@arr اسپات روی stream.binance.com:9443 (یک پوش در ثانیه) + fallback ‏REST سبک /api/v3/ticker/price (وزن ۴ — روی IPهای بن‌شده هم پاسخ می‌دهد، برخلاف /ticker/24hr وزن ۸۰) و وایت‌لیست exchangeInfo اسپات؛ پیاده‌سازی با اتصال هاست صریح به همان ترانسپورت فیوچرز (NewStreamAt/getJSONAt/tickers24hAt/exchangeSymbolsAt — رفتار فیوچرز دست‌نخورده)؛ **گیت REST به‌ازای هر هاست** (sync.Map) — بن ۴۱۸ یکی (مثلاً exchangeInfo اسپات هنگام boot) دیگر (اسنپ‌شات عمق فیوچرز) را گرسنگی نمی‌دهد؛ ‏NewFeed عمداً idle (ترمینال هرگز اسپات را چارت نمی‌کند)؛ فلگ‌های -binance-spot-rest/-binance-spot-ws
- [x] **لایهٔ بیسیس اسپات-پرپ** (terminal): نوار حرارتی در پایین چارت زنده — last اسپات بایننس vs mid پرپِ همین چارت به bps؛ نمونه‌برداری ۱/s از TickerManager مشترک (بدون سابسکرایب اضافه)، حلقهٔ ۱۸۰۰ نمونه (۳۰ دقیقه)، ستون‌های ~۶px با آلفای تطبیقی (مقیاس کلامپ ۲–۳۰ bps)؛ پرمیوم پرپ سبز / دیسکانت قرمز + خط پایه صفر + چیپ «S/P ±x.x bps»؛ روی ریبون OKX پشته می‌شود (هر دو هرگز هم‌پوشان نمی‌شوند)؛ نگاشت نماد: binancef/bybit همان، okx از BTC-USDT-SWAP→BTCUSDT، ‏hl بدون قرینه رد می‌شود؛ سابسکرایب ticker24h.binancespot.global در boot
- [x] **ماندگاری + i18n**: کلید rt_basis_spot در workspace (نبود در ذخیره‌های قدیمی = خاموش)؛ ردیف «Spot-Perp basis / بیسیس اسپات-پرپ» + نوت نبود فید در جدول i18n (۱۹۸ رشته)

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
| ۹ | Footprint پیشرفته (delta matrix + imbalance سفارشی) | تریدرهای order-flow | ✅ فاز ۱۱ |
| ۱۰ | لایهٔ دلتا-دلتا: Spot↔Perp basis heatmap | دید آربیتراژ | ✅ فاز ۱۲ |

**الگوی gating پیشنهادی برای نسخهٔ شما:** همان `entitlements.h` موجود را نگه دارید؛ فقط سقف tier را خودتان تعریف کنید (مثلاً `local_pro`) و UI قفل‌ها را روی همان slugها فعال کنید — زیرساختش کامل ساخته شده و شما فقط کلیدش را برمی‌دارید.

---

## ⚖️ یادآوری مجوز (مهم برای ادغام آینده)
- ریپو **AGPL-3.0**: استفادهٔ شخصی/توسعه آزاد؛ اگر نسخهٔ تغییریافته را «به دیگران از طریق شبکه سرویس کنید» باید سورس را هم در دسترس بگذارید.
- گیت‌وی Go **MIT**: آزاد کامل، حتی تجاری.
- برای ادغام در ترمینال اصلی خودتان (استفادهٔ شخصی): **هیچ محدودیتی نیست**؛ فقط کپی‌رایت AGPL را در فایل‌های منتقل‌شده حفظ کنید.

---
*ساخته‌شده در بازبینی Task R4 — جزئیات فنی هر لایه در کامنت‌های سورس و `docs/` ریپو موجود است.*
