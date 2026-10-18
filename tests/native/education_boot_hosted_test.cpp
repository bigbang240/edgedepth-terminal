// ═══════════════════════════════════════════════════════════════════════════════
// education_boot_hosted_test.cpp - native pin on the hosted research replay
// switch (2026-10-13).
//
// The live terminal can lift the research replay chrome around its running
// canvas instead of navigating to /terminal?replay=. While it does, the canvas
// must answer every per-frame question exactly as a research viewer boot does
// (mode Studio, chrome "research", embedded), and every question that was
// decided ONCE at boot (which socket, the live auto-subscribes, the usage
// chrome) must keep its boot answer, because the canvas returns to live.
//
//   1. A live boot can host (begin refuses unless boot_mode() is None, so a
//      lesson, event, pack or Course Studio mount never switches; the native
//      stub always boots live, so that refusal is not reachable here).
//   2. begin while hosted re-targets the symbol and venue; end restores the
//      boot mode exactly, so a second hosted replay starts from a clean state.
//   3. boot_mode() and boot_embedded() never move.
// ═══════════════════════════════════════════════════════════════════════════════

#include "core/education_boot.cpp"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void expect_true(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

void expect_eq(const std::string& got, const std::string& want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got '%s' want '%s'\n", what, got.c_str(), want.c_str());
        ++failures;
    }
}

}  // namespace

int main() {
    auto& boot = EducationBoot::instance();
    boot.detect();  // native stub: a live boot

    expect_true(boot.mode() == EducationBoot::Mode::None, "live boot starts in mode None");
    expect_true(boot.boot_mode() == EducationBoot::Mode::None, "boot mode is None");
    expect_true(!boot.is_embedded(), "live boot is not embedded");
    expect_true(!boot.hosted_research(), "not hosted before begin");

    // 1. begin from a live boot: every per-frame predicate reads research viewer.
    expect_true(boot.begin_hosted_research("btcusdt", "binancef"), "live boot can host");
    expect_true(boot.hosted_research(), "hosted after begin");
    expect_true(boot.is_studio(), "hosted reads as studio");
    expect_true(boot.is_research_replay(), "hosted reads as the research viewer");
    expect_true(boot.is_embedded(), "hosted reads as embedded (no native shell or bar)");
    expect_true(!boot.is_lesson() && !boot.is_event() && !boot.is_pack(), "hosted is not lesson/event/pack");
    expect_eq(boot.studio_symbol(), "btcusdt", "hosted symbol");
    expect_eq(boot.studio_exchange(), "binancef", "hosted venue");

    // 3. boot decisions keep their boot answer while hosted.
    expect_true(boot.boot_mode() == EducationBoot::Mode::None, "boot mode unchanged while hosted");
    expect_true(!boot.boot_embedded(), "boot_embedded unchanged while hosted");

    // 2. a re-anchor to another market while hosted re-targets.
    expect_true(boot.begin_hosted_research("SOL", "hl"), "begin while hosted re-targets");
    expect_eq(boot.studio_symbol(), "SOL", "re-targeted symbol keeps the venue's case");
    expect_eq(boot.studio_exchange(), "hl", "re-targeted venue");

    boot.end_hosted_research();
    expect_true(boot.mode() == EducationBoot::Mode::None, "end restores mode None");
    expect_true(!boot.hosted_research(), "not hosted after end");
    expect_true(!boot.is_embedded(), "end restores the full shell");
    expect_true(!boot.is_research_replay(), "end leaves no research chrome");
    expect_eq(boot.studio_symbol(), "", "end clears the hosted symbol");
    expect_eq(boot.studio_exchange(), "", "end clears the hosted venue");

    // end when not hosted is a no-op.
    boot.end_hosted_research();
    expect_true(boot.mode() == EducationBoot::Mode::None, "second end is a no-op");

    // A second hosted replay in the same page starts from the same clean state.
    expect_true(boot.begin_hosted_research("ethusdt", "bybit"), "second begin");
    expect_true(boot.is_research_replay(), "second hosted replay reads research");
    boot.end_hosted_research();

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("education_boot_hosted_test: ok\n");
    return 0;
}
