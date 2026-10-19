#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
// liq_levels.h - the venue's own liquidation estimation (Phase 2 S3 of
// docs/UPGRADE-ROADMAP.md, "جنگ افزار لیکوییدیشن"): where resting leverage
// would be forced out, computed on the client from what the terminal already
// observes - no EdgeDepth server involved.
//
// The model, in three steps:
//   1. Opening density. The recent tape says where positions (would have)
//      opened: taker buys at a price are longs opening (or shorts closing),
//      taker sells the mirror. Per-price USD mass over the last
//      kTapeWindowMs is the entry distribution.
//   2. Leverage mix. Each entry level's mass spreads over the leverage tiers
//      the venue allows for the symbol (liq_field caps) with fixed retail-
//      weighted probabilities. Tier-L longs opened at P liquidate near
//      P*(1 - 1/L); shorts near P*(1 + 1/L). Maintenance margin and fees are
//      folded into the smoothing, not modelled separately.
//   3. Anchoring. The densities integrate to the side split of the tape, so
//      they are rescaled to real open interest (Stat.open_interest_usd, the
//      field carries contract quantity, times the mark) split by the tape's
//      taker buy share. The result is a USD-notional liquidation density on
//      each side of the mark; its largest local maxima are the clusters the
//      chart draws.
//
// It is an estimate and the layer labels it as one. The recompute is
// throttled to kRecomputeMs; the tape scan is bounded in trades and age, so
// a frame cost is a few thousand multiplications at most.
//
// Self-host note (EDGEDEPTH_PTHREADS=OFF): main-thread only, fed from the
// chart's real-time state exactly like cascade_detector_. Rewinds reset it.
// ═══════════════════════════════════════════════════════════════════════════════
#include "core/liq_field_tiers.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace liq_levels {

// The tape window behind the mark that seeds the opening density.
inline constexpr int64_t kTapeWindowMs = 15 * 60 * 1000;
// Bounded scan from the newest trade backwards (a runaway tape cannot stall a frame).
inline constexpr size_t kMaxTapeTrades = 24000;
// Recompute throttle: the map is a slow structure, not a per-tick one.
inline constexpr int64_t kRecomputeMs = 2000;
// Price bins across the +/- range around the mark.
inline constexpr int kBins = 240;
// Half-range of the map around the mark: 5x liqs sit at 20% from entry.
inline constexpr double kRange = 0.22;
// Clusters per side, and how much of that side's OI a cluster must carry.
inline constexpr int kMaxClustersPerSide = 3;
inline constexpr double kMinClusterShare = 0.05;
// Two clusters closer than this fraction of the mark are one cluster.
inline constexpr double kMinClusterGap = 0.004;
// Fixed retail-weighted leverage mix, aligned with liq_field::kLeverages.
inline constexpr std::array<double, 6> kTierWeights{0.10, 0.20, 0.35, 0.20, 0.08, 0.07};

struct Cluster {
    double price = 0;    // estimated liquidation price (map bin centre)
    double usd = 0;      // estimated notional forced out around it
    double share = 0;    // that usd over the side's anchored OI
    bool longs = true;   // longs liquidating (forced sells) vs shorts (forced buys)
    int tier = 0;        // the leverage contributing the most there
};

class Estimator {
public:
    // `oi_contracts` is Stat.open_interest_usd (contract quantity despite the
    // field name). `trades` is the real-time tape, oldest first, newest last;
    // only [now - kTapeWindowMs, now] is read, from the tail. `cap` is the
    // symbol's max leverage (liq_field::max_leverage); 0 keeps every tier.
    template <class Trades>
    void update(int64_t now_ms, double mark, double oi_contracts,
                const Trades& trades, double cap) {
        if (now_ms - last_update_ms_ < kRecomputeMs) return;
        last_update_ms_ = now_ms;
        clusters_.clear();
        if (!(mark > 0) || !(oi_contracts > 0) || !std::isfinite(mark)) return;

        // ── 1. Tape mass per entry bin ─────────────────────────────────────
        buy_.assign(size_t(kBins), 0.0);
        sell_.assign(size_t(kBins), 0.0);
        double total_buy = 0, total_sell = 0;
        size_t scanned = 0;
        const int64_t oldest = now_ms - kTapeWindowMs;
        for (auto it = trades.rbegin(); it != trades.rend() && scanned < kMaxTapeTrades; ++it, ++scanned) {
            if (it->timestamp_ms < oldest) break;
            const double notional = it->qty * it->price;
            if (!(notional > 0) || !std::isfinite(notional)) continue;
            const int bin = entry_bin(it->price, mark);
            if (bin < 0) continue;   // outside the map's range
            // is_buyer_maker == false is a taker buy on Binance; the terminal
            // stores the aggressor side directly where available.
            (it->is_buy ? buy_ : sell_)[size_t(bin)] += notional;
            if (it->is_buy) total_buy += notional; else total_sell += notional;
        }
        const double flow = total_buy + total_sell;
        if (!(flow > 0)) return;   // no opening density to speak of yet

        // ── 2. Spread every entry bin over the allowed leverage tiers ─────
        long_.assign(size_t(kBins), 0.0);
        short_.assign(size_t(kBins), 0.0);
        tier_long_.assign(size_t(kBins) * liq_field::kLeverages.size(), 0.0);
        tier_short_.assign(size_t(kBins) * liq_field::kLeverages.size(), 0.0);
        double weight_sum = 0;
        bool allowed[size_t(liq_field::kLeverages.size())] = {};
        for (size_t i = 0; i < liq_field::kLeverages.size(); ++i) {
            allowed[i] = cap <= 0 || liq_field::kLeverages[i] <= cap;
            if (allowed[i]) weight_sum += kTierWeights[i];
        }
        if (!(weight_sum > 0)) return;
        for (size_t b = 0; b < size_t(kBins); ++b) {
            const double entry = bin_price(int(b), mark);
            for (size_t i = 0; i < liq_field::kLeverages.size(); ++i) {
                if (!allowed[i]) continue;
                const double share = kTierWeights[i] / weight_sum;
                const double inv = 1.0 / liq_field::kLeverages[i];
                const int lb = level_bin(entry * (1.0 - inv), mark);
                const int sb = level_bin(entry * (1.0 + inv), mark);
                if (lb >= 0) {
                    long_[size_t(lb)] += buy_[b] * share;
                    tier_long_[size_t(lb) * liq_field::kLeverages.size() + i] += buy_[b] * share;
                }
                if (sb >= 0) {
                    short_[size_t(sb)] += sell_[b] * share;
                    tier_short_[size_t(sb) * liq_field::kLeverages.size() + i] += sell_[b] * share;
                }
            }
        }

        // ── 3. Anchor to open interest and pick the clusters ──────────────
        const double oi_usd = oi_contracts * mark;
        const double long_share = std::clamp(total_buy / flow, 0.15, 0.85);
        pick(mark, long_, tier_long_, oi_usd * long_share, total_buy, true);
        pick(mark, short_, tier_short_, oi_usd * (1.0 - long_share), total_sell, false);
    }

    const std::vector<Cluster>& clusters() const { return clusters_; }
    int64_t last_update_ms() const { return last_update_ms_; }
    void reset() { clusters_.clear(); last_update_ms_ = 0; }

private:
    static int entry_bin(double price, double mark) {
        const double rel = (price / mark - 1.0 + kRange) / (2.0 * kRange);
        const int bin = int(std::floor(rel * double(kBins)));
        return rel >= 0.0 && rel < 1.0 && bin >= 0 && bin < kBins ? bin : -1;
    }
    static int level_bin(double price, double mark) { return entry_bin(price, mark); }
    static double bin_price(int bin, double mark) {
        return mark * (1.0 - kRange + (double(bin) + 0.5) / double(kBins) * 2.0 * kRange);
    }

    // Smooth a 5-bin triangular window over the density, then keep the
    // strongest separated local maxima as clusters. `anchor` is the side's
    // OI notional; `flow_side` the raw tape mass that seeded the density.
    void pick(double mark, const std::vector<double>& density,
              const std::vector<double>& tiers, double anchor, double flow_side, bool longs) {
        if (!(anchor > 0) || !(flow_side > 0)) return;
        const double scale = anchor / flow_side;
        smooth_.assign(size_t(kBins), 0.0);
        for (int b = 0; b < kBins; ++b) {
            double acc = 0, weight = 0;
            for (int k = -2; k <= 2; ++k) {
                const int j = b + k;
                if (j < 0 || j >= kBins) continue;
                const double w = 3.0 - double(std::abs(k));
                acc += density[size_t(j)] * w; weight += w;
            }
            smooth_[size_t(b)] = weight > 0 ? acc / weight * scale : 0.0;
        }
        struct Peak { int bin = 0; double usd = 0; };
        std::vector<Peak> peaks;
        for (int b = 1; b + 1 < kBins; ++b)
            if (smooth_[size_t(b)] > smooth_[size_t(b - 1)] && smooth_[size_t(b)] >= smooth_[size_t(b + 1)] &&
                smooth_[size_t(b)] >= kMinClusterShare * anchor)
                peaks.push_back({b, smooth_[size_t(b)]});
        std::sort(peaks.begin(), peaks.end(), [](const Peak& a, const Peak& c) { return a.usd > c.usd; });
        int taken = 0;
        for (const auto& p : peaks) {
            if (taken >= kMaxClustersPerSide) break;
            const double price = bin_price(p.bin, mark);
            bool near = false;
            for (const auto& c : clusters_)
                if (c.longs == longs && std::abs(c.price - price) < kMinClusterGap * mark) { near = true; break; }
            if (near) continue;
            size_t best_tier = 0;
            double best_tier_usd = 0;
            for (size_t i = 0; i < liq_field::kLeverages.size(); ++i) {
                const double t = tiers[size_t(p.bin) * liq_field::kLeverages.size() + i] * scale;
                if (t > best_tier_usd) { best_tier_usd = t; best_tier = i; }
            }
            Cluster c;
            c.price = price;
            c.usd = p.usd;
            c.share = p.usd / anchor;
            c.longs = longs;
            c.tier = int(liq_field::kLeverages[best_tier]);
            clusters_.push_back(c);
            ++taken;
        }
    }

    std::vector<double> buy_, sell_, long_, short_, smooth_;
    std::vector<double> tier_long_, tier_short_;
    std::vector<Cluster> clusters_;
    int64_t last_update_ms_ = 0;
};

} // namespace liq_levels
