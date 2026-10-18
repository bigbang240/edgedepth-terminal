# 📘 راهنمای کامل Git & GitHub — الگوی قابل استفاده برای هر پروژه
**راهنمای حرفه‌ای push/pull/merge برای ریپوهای شما (terminal، gateway، و هر پروژهٔ آینده)**

> این سند یک الگوی همیشگی است: هر پروژهٔ جدیدی ساختید، همین مراحل را تکرار کنید.
> وضعیت فعلی: `/home/z/edgedepth-terminal` و `/home/z/edgedepth-gateway` هر دو `origin`شان به ریپوی رسمی `edgedepthhq` اشاره می‌کند که **اجازهٔ push ندارید** — پس اول باید ریپوی خودتان را بسازید.

---

## گام ۰ — آماده‌سازی یک‌باره (فقط اولین بار)

```bash
# هویت git (همان‌طور که در commitها دیده می‌شوید)
git config --global user.name  "Your Name"
git config --global user.email "you@example.com"

# کلید SSH برای push بدون رمز
ssh-keygen -t ed25519 -C "you@example.com"
cat ~/.ssh/id_ed25519.pub
# ↑ محتوایش را در GitHub → Settings → SSH and GPG keys → New SSH key بچسبانید
ssh -T git@github.com        # تست: باید بگوید "Hi USERNAME!"
```

---

## گام ۱ — ساخت ریپوی خودتان (سه راه)

**راه A — Fork (پیشنهادی برای این ریپو):**
1. در گیت‌هاب به `github.com/edgedepthhq/edgedepth-terminal` بروید → دکمهٔ **Fork** → Create fork.
2. مزیت: تاریخچهٔ کامل + دکمهٔ «Sync fork» برای گرفتن آپدیت‌های رسمی + مجوز AGPL خودبه‌خود حفظ می‌شود.

**راه B — ریپوی خالی (پیشنهادی برای پروژه‌های شخصی آینده):**
1. GitHub → **New repository** → نام بدهید (بدون README چون محلی داریم) → Create.
2. با گام ۲ وصلش کنید.

**راه C — GitHub CLI (سریع‌ترین):**
```bash
gh auth login
gh repo fork edgedepthhq/edgedepth-terminal --clone=false
gh repo create my-new-project --private --source=. --push   # برای پروژهٔ جدید
```

---

## گام ۲ — وصل کردن remoteها (قلب الگو)

به‌جای تغییر `origin`، الگوی استاندارد حرفه‌ای: **origin = ریپوی خودتان، upstream = ریپوی رسمی**

```bash
cd /home/z/edgedepth-terminal

# origin فعلی (رسمی) را به upstream تغییر نام بده
git remote rename origin upstream

# ریپوی خودتان را به‌عنوان origin اضافه کن (SSH بهتر از https است)
git remote add origin git@github.com:USERNAME/edgedepth-terminal.git

git remote -v
# origin    git@github.com:USERNAME/edgedepth-terminal.git (fetch/push)   ← مال شما
# upstream  https://github.com/edgedepthhq/edgedepth-terminal.git         ← رسمی
```

> همین ۲ خط برای `/home/z/edgedepth-gateway` و هر پروژهٔ دیگر تکرار می‌شود.

---

## گام ۳ — اولین push (تغییرات فعلی شما)

تغییرات شما الان `modified`اند: `chart_widget.cpp/h`، `chart_widget_realtime.cpp`، `app_shell.cpp`، `workspace_manager.cpp`، `CMakeLists.txt` + دو سند جدید `docs/`.

```bash
# ۱) شاخهٔ کاری بسازید (هیچ‌وقت مستقیم روی master کار نکنید)
git checkout -b feat/rt-timeframes-axis-zoom

# ۲) بررسی دقیق قبل از add
git diff --stat
git add src/ CMakeLists.txt docs/

# ۳) commit با پیام معنادار (قالب Conventional Commits)
git commit -m "feat(rt): selectable 1s-5m buckets + TradingView axis-drag scaling

- RT candle grid: 1s/5s/15s/30s/1m/5m via toolbar + set_rt_candle_seconds()
- Time-axis gutter drag: stretch/shrink visible span (idempotent factor)
- Price-axis gutter drag: vertical scale, detaches rt_auto_price_
- docs: UPGRADE-ROADMAP.md (layer review + 8 phases) + GITHUB-WORKFLOW.md"

# ۴) push اول با -u تا آپ‌استریم ست شود
git push -u origin feat/rt-timeframes-axis-zoom

# ۵) بعد از تست، ادغام در master خودتان
git checkout master
git merge --no-ff feat/rt-timeframes-axis-zoom
git push origin master
```

---

## گام ۴ — چرخهٔ روزانهٔ کار (push/pull حرفه‌ای)

```bash
git switch -c feat/<نام-قابلیت>     # شاخهٔ جدید برای هر قابلیت
# ... کدنویسی ...
git add -p                          # بررسی تکه‌تکه‌ای (hunk به hunk)
git commit -m "feat(scope): خلاصه"
git push -u origin feat/<نام-قابلیت>

git switch master && git pull --ff-only origin master   # همگام با خودتان
git merge --no-ff feat/<نام-قابلیت> && git push origin master
git branch -d feat/<نام-قابلیت>                          # پاک‌سازی
```

**قالب پیام commit (Conventional Commits):**
| پیشوند | کاربرد | مثال |
|---|---|---|
| `feat:` | قابلیت جدید | `feat(heatmap): peak-hold layer` |
| `fix:` | رفع باگ | `fix(rt): stale depth recovery loop` |
| `perf:` | بهینه‌سازی | `perf(chart): skip empty buckets` |
| `docs:` | مستندات | `docs: roadmap phases` |
| `refactor:` | تغییر ساختار بدون تغییر رفتار | `refactor: extract palette loader` |
| `chore:` | ابزار/بیلد | `chore: bump cache v to 20251018b` |

---

## گام ۵ — گرفتن آپدیت‌های رسمی (Sync with upstream)

```bash
git fetch upstream                          # آخرین state رسمی را بیاور (کدت را دست نمی‌زند)
git switch master
git merge --no-ff upstream/master           # یا: git rebase upstream/master
# اگر تعارض (conflict) شد:
git status                                  # فایل‌های <<<<<<< دار
# فایل را باز کن، بین <<<<<<< ======= >>>>>>> را دستی درست کن، بعد:
git add <فایل> && git commit                # برای merge
# یا برای rebase: git add <فایل> && git rebase --continue
git push origin master
```

> 💡 قاعدهٔ طلایی برای این ریپو: **تغییرات خودتان را در فایل‌های جدا یا با کامنت `// [SELF-HOST]` مشخص کنید** تا هنگام sync با نسخهٔ رسمی، تعارض کمتر شود.

---

## گام ۶ — الگوی همیشگی برای «هر پروژهٔ جدید»

هر پروژه‌ای ساختید، این ۷ قدم را انجام بدهید:

```bash
# 1) پوشهٔ پروژه + git init
cd ~/my-new-project && git init -b main

# 2) .gitignore مناسب (الگو پایین)
# 3) README.md با سه بخش: چی هست / چطور اجرا کنم / چطور توسعه بدم
# 4) اولین commit
git add . && git commit -m "chore: initial scaffold"

# 5) ریپوی گیت‌هاب + push (یک خط!)
gh repo create my-new-project --private --source=. --push

# 6) محافظت از main: GitHub → Settings → Branches → Add rule:
#    ✅ Require pull request  ✅ Include administrators (اختیاری برای تیم)

# 7) برچسب نسخه
git tag -a v0.1.0 -m "first working build" && git push origin v0.1.0
```

**`.gitignore` همگانی (کپی در هر پروژه):**
```gitignore
# artifacts
build/
dist/
*.wasm
!public/index.wasm        # اگر مثل این پروژه خروجی را دیپلوی می‌کنید
node_modules/
__pycache__/
*.pyc
# secrets — هرگز به گیت‌هاب نروند!
.env
.env.*
*.pem
*.key
# editors/OS
.vscode/
.idea/
.DS_Store
# این پروژه خاص
*.log
.next/
db/*.sqlite*
```

**قانون طلایی secrets:** کلید/توکن هرگز داخل کد. به‌جایش `.env.example` (بدون مقدار واقعی) را commit کنید و `.env` را ignore کنید.

---

## گام ۷ — ابزارهای حرفه‌ای که ارزش‌شان را ثابت می‌کنند

| ابزار | دستور | فایده |
|---|---|---|
| تاریخچهٔ تمیز | `git log --oneline --graph --all` | دید کامل شاخه‌ها |
| برگرداندن امن | `git revert <hash>` (نه reset روی مشترک) | لغو یک commit بدون خراب‌کردن تاریخچه |
| ذخیرهٔ موقت | `git stash` / `git stash pop` | وسط کار ناچار به سوییچ شدید |
| جست‌وجو در تاریخچه | `git log -S "set_rt_candle_seconds"` | کدام commit این تابع را آورد؟ |
| بی‌تفاوت‌سازی فایل محلی | `git restore <file>` | دور ریختن تغییرات اجرایی |
| مشاهدهٔ تغییرات قبل از pull | `git log HEAD..upstream/master --oneline` | چی می‌خواهد بیاید؟ |
| GitHub Actions | فایل `.github/workflows/build.yml` | CI خودکار: هر push → build + test |

**نمونهٔ CI مینیمال برای این ریپو (`.github/workflows/build.yml`):**
```yaml
name: build
on: [push, pull_request]
jobs:
  native-tests:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: configure & test
        run: |
          cmake -B build -DEDGEDEPTH_PTHREADS=OFF -DCMAKE_BUILD_TYPE=Release
          cmake --build build -j2
          ctest --test-dir build --output-on-failure
```

---

## چک‌لیست سریع همین امروز (terminal + gateway)

```bash
# ۱) fork هر دو ریپو در گیت‌هاب (دکمهٔ Fork)
# ۲) remoteها:
cd /home/z/edgedepth-terminal
git remote rename origin upstream
git remote add origin git@github.com:USERNAME/edgedepth-terminal.git

cd /home/z/edgedepth-gateway
git remote rename origin upstream
git remote add origin git@github.com:USERNAME/edgedepth-gateway.git

# ۳) push اولیه:
cd /home/z/edgedepth-terminal
git switch -c feat/rt-timeframes-axis-zoom
git add -A && git commit -m "feat(rt): 1s-5m buckets + axis-drag zoom + roadmap docs"
git push -u origin feat/rt-timeframes-axis-zoom
# در گیت‌هاب: Compare & pull request → Merge (یا مستقیم merge محلی مثل گام ۳)

cd /home/z/edgedepth-gateway
git switch -c feat/depth-cache-tuning
git add -A && git commit -m "chore: local depth20 fallback tuning"
git push -u origin feat/depth-cache-tuning
```

---
*این راهنما عمداً عام است: برای «هر» پروژه کار می‌کند. برای پروژهٔ خاص فقط نام‌ها وremoteها را عوض کنید.*
