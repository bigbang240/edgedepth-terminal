#pragma once
// Free real-time preview (2026-10-11). A free account may run real-time mode on a Pro market for
// kDailyMs of VISIBLE time per UTC day, shared across every market, so the upsell comes after the
// viewer has seen live depth on the coin they care about rather than before. Binance BTCUSDT stays
// free and unlimited (Entitlements::btc_preview_market) and never draws on this budget.
//
// Why: real-time on alts is the strongest buying signal in the funnel (115 of 222 Pro locks, every
// checkout start), and the only free alternative the upsell offered, BTC, was never taken.
//
// No backend change: live order books stream for every market to everyone; real-time is a view of
// them. The budget lives in the browser (localStorage), so a cleared profile resets it. That is a
// conversion device, not an entitlement, and it is priced accordingly.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <cstdlib>
#endif

namespace rt_preview {

inline constexpr int64_t kDailyMs = 3LL * 60LL * 1000LL;
// A frame that took longer than this is a tab coming back from the background (rAF stops while
// hidden); it must not bill the hidden time.
inline constexpr double kMaxFrameSeconds = 0.25;

// UTC calendar day of an epoch-ms instant, "YYYY-MM-DD".
inline std::string utc_day(int64_t epoch_ms) {
    const std::time_t t = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[48];   // sized for any int, so -Wformat-truncation has nothing to say
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

// The stored spend. A record from another UTC day is spent nothing today.
struct Budget {
    std::string day;
    int64_t used_ms = 0;
};

inline int64_t remaining_ms(const Budget& b, const std::string& today) {
    if (b.day != today) return kDailyMs;
    return std::clamp<int64_t>(kDailyMs - b.used_ms, 0, kDailyMs);
}

// Add one frame of visible preview time. Returns the budget to store.
inline Budget spend(Budget b, const std::string& today, double frame_seconds) {
    if (b.day != today) b = Budget{today, 0};
    const double s = std::clamp(frame_seconds, 0.0, kMaxFrameSeconds);
    b.used_ms = std::min<int64_t>(kDailyMs, b.used_ms + static_cast<int64_t>(s * 1000.0));
    return b;
}

// "2:41"
inline std::string format_left(int64_t ms) {
    const int64_t s = (std::max<int64_t>(0, ms) + 999) / 1000;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%lld:%02lld", static_cast<long long>(s / 60), static_cast<long long>(s % 60));
    return buf;
}

// ── Process-wide budget (one per browser profile) ──────────────────────────────
inline int64_t now_ms() {
    return static_cast<int64_t>(std::time(nullptr)) * 1000;
}

class Store {
public:
    static Store& instance() { static Store s; return s; }

    int64_t remaining() { load(); return remaining_ms(budget_, utc_day(now_ms())); }
    bool available() { return remaining() > 0; }

    // Bill one frame. Several previewing charts in one frame bill it once.
    void bill_frame(int frame, double frame_seconds) {
        if (frame == last_frame_) return;
        last_frame_ = frame;
        load();
        budget_ = spend(budget_, utc_day(now_ms()), frame_seconds);
        // Persist about once a second and on the last millisecond, not every frame.
        if (budget_.used_ms - saved_used_ms_ >= 1000 || budget_.used_ms >= kDailyMs || budget_.day != saved_day_) save();
    }

private:
    Budget budget_;
    bool loaded_ = false;
    int last_frame_ = -1;
    std::string saved_day_;
    int64_t saved_used_ms_ = 0;

    void load() {
        if (loaded_) return;
        loaded_ = true;
#ifdef __EMSCRIPTEN__
        // No top-level commas or empty '' in the JS: EM_ASM is a C macro and
        // would split its argument there.
        char* raw = static_cast<char*>(EM_ASM_PTR({
            try {
                var v = window.localStorage.getItem("edgedepth.rt_preview.v1") || "";
                var n = lengthBytesUTF8(v) + 1;
                var p = _malloc(n);
                stringToUTF8(v, p, n);
                return p;
            } catch (e) { return 0; }
        }));
        if (raw) {
            // "YYYY-MM-DD:<used_ms>"; anything else reads as nothing spent.
            const std::string v(raw);
            std::free(raw);
            const auto colon = v.find(':');
            if (colon == 10) {
                budget_.day = v.substr(0, colon);
                budget_.used_ms = std::clamp<int64_t>(std::atoll(v.c_str() + colon + 1), 0, kDailyMs);
            }
        }
#endif
        saved_day_ = budget_.day;
        saved_used_ms_ = budget_.used_ms;
    }

    void save() {
        saved_day_ = budget_.day;
        saved_used_ms_ = budget_.used_ms;
#ifdef __EMSCRIPTEN__
        const std::string v = budget_.day + ":" + std::to_string(budget_.used_ms);
        EM_ASM({
            try { window.localStorage.setItem("edgedepth.rt_preview.v1", UTF8ToString($0)); } catch (e) {}
        }, v.c_str());
#endif
    }
};

}  // namespace rt_preview
