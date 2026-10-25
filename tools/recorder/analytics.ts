// ═══════════════════════════════════════════════════════════════════════════════
// Live analytics engine (roadmap Phase 6: "REST/WS analytical server").
//
// Sits on the recorder's normalized gateway feed and keeps, per symbol, the
// same state a consumer terminal needs without re-deriving it from raw wire
// data: a live order book (snapshot + delta semantics, exactly what
// OrderbookManager applies client-side), a rolling trade/flow window
// (CVD, rates, 5m VWAP), rolling liquidation sums and the latest 1m candle.
//
// Book semantics per venue adapter (gateway):
//   binancef  - real deltas: rows with size 0 mean "delete this level".
//   okx       - the adapter emits its whole maintained map on every diff, so
//               rows never carry zeros and vanished levels are simply absent;
//               an incrementally-maintained copy therefore needs periodic
//               pruning around the mid (done here) exactly like the client's
//               prune_orderbook().
//
// No protobuf dependency: callers pass decoded plain objects.
// ═══════════════════════════════════════════════════════════════════════════════

export interface BookRow {
  price: number;
  size: number;
}
export interface TradeTick {
  ts: number;
  price: number;
  qty: number;
  isBuy: boolean;
}
export interface LiqTick {
  ts: number;
  price: number;
  qty: number;
  isBuy: boolean; // forced BUY = shorts liquidated (gateway semantics)
}
export interface CandleTick {
  ts: number;
  o: number;
  h: number;
  l: number;
  c: number;
  v: number;
}

const FLOW_RING_MS = 5 * 60_000; // detailed trade window (CVD-5m, VWAP, rates)
const LIQ_RING_MS = 60 * 60_000; // liquidation window (5m/60m sums)
const TRADE_RING_CAP = 60_000; // hard cap per symbol (≈200/s for 5 minutes)
const LIQ_RING_CAP = 20_000;
// Book levels are kept within this many bps of the mid - far beyond any
// consumer depth window, tight enough to bound memory on full-book venues.
const BOOK_KEEP_BPS = 400;

export interface AnalyticsSnapshot {
  schema: "edgedepth.analytics.v1";
  exchange: string;
  symbol: string;
  ts_ms: number;
  age_ms: { book: number; trade: number; liquidation: number };
  book: {
    best_bid: number;
    best_ask: number;
    mid: number;
    spread: number;
    spread_bps: number;
    depth_bps: number;
    bid_notional: number;
    ask_notional: number;
    imbalance: number; // (bid-ask)/(bid+ask) inside the depth window, [-1..1]
    bid_levels: number;
    ask_levels: number;
    histogram: Array<{ from_bps: number; to_bps: number; bid_usd: number; ask_usd: number }>;
  };
  flow: {
    cvd_qty_session: number; // since recorder start
    cvd_usd_session: number;
    cvd_qty_5m: number;
    cvd_usd_5m: number;
    trade_rate_60s: number; // trades per minute
    notional_rate_60s: number; // USD per minute
    vwap_5m: number;
    buy_ratio_5m: number; // notional share of buys [0..1]
  };
  liquidations: {
    long_usd_5m: number; // forced sells = longs liquidated
    short_usd_5m: number; // forced buys = shorts liquidated
    long_usd_60m: number;
    short_usd_60m: number;
    count_60m: number;
    largest_60m_usd: number;
  };
  candle_1m: CandleTick | null;
}

export class SymbolAnalytics {
  readonly symbol: string;
  exchange = "";
  lastBookMs = 0;
  lastTradeMs = 0;
  lastLiqMs = 0;

  private bids = new Map<number, number>();
  private asks = new Map<number, number>();
  private trades: TradeTick[] = [];
  private liqs: LiqTick[] = [];
  private candle: CandleTick | null = null;
  private cvd = { qty: 0, usd: 0 };
  private lastPruneMs = 0;

  constructor(symbol: string) {
    this.symbol = symbol;
  }

  // ── ingest ──────────────────────────────────────────────────────────────────

  applyBook(u: { snapshot: boolean; asks: BookRow[]; bids: BookRow[]; tsMs: number }) {
    if (u.snapshot) {
      this.bids.clear();
      this.asks.clear();
    }
    for (const r of u.asks) {
      if (r.size > 0) this.asks.set(r.price, r.size);
      else this.asks.delete(r.price);
    }
    for (const r of u.bids) {
      if (r.size > 0) this.bids.set(r.price, r.size);
      else this.bids.delete(r.price);
    }
    this.lastBookMs = u.tsMs || Date.now();
    this.pruneBook();
  }

  applyTrade(t: TradeTick) {
    const usd = t.price * t.qty;
    this.trades.push({ ts: t.ts, price: t.price, qty: t.qty, isBuy: t.isBuy });
    this.cvd.qty += t.isBuy ? t.qty : -t.qty;
    this.cvd.usd += t.isBuy ? usd : -usd;
    this.lastTradeMs = t.ts || Date.now();
    if (this.trades.length > TRADE_RING_CAP) this.trades.splice(0, this.trades.length - TRADE_RING_CAP);
  }

  applyLiq(l: LiqTick) {
    this.liqs.push({ ts: l.ts, price: l.price, qty: l.qty, isBuy: l.isBuy });
    this.lastLiqMs = l.ts || Date.now();
    if (this.liqs.length > LIQ_RING_CAP) this.liqs.splice(0, this.liqs.length - LIQ_RING_CAP);
  }

  applyCandle(c: CandleTick) {
    this.candle = c;
  }

  // Top-of-book row for the history poller.
  bookPollRow(): [number, string, number, number, number, number, number, number] {
    const now = Date.now();
    const depth = this.depthSums(25);
    const bb = this.bestBid(),
      ba = this.bestAsk();
    return [
      now,
      this.symbol,
      bb,
      ba,
      depth.bid,
      depth.ask,
      depth.bidLevels,
      depth.askLevels,
    ];
  }

  // ── projections ─────────────────────────────────────────────────────────────

  bestBid(): number {
    let best = 0;
    for (const p of this.bids.keys()) if (p > best) best = p;
    return best;
  }

  bestAsk(): number {
    let best = Infinity;
    for (const p of this.asks.keys()) if (p < best) best = p;
    return Number.isFinite(best) ? best : 0;
  }

  private depthSums(depthBps: number): {
    bid: number;
    ask: number;
    bidLevels: number;
    askLevels: number;
  } {
    const mid = (this.bestBid() + this.bestAsk()) / 2;
    if (!(mid > 0)) return { bid: 0, ask: 0, bidLevels: 0, askLevels: 0 };
    const band = mid * (depthBps / 10_000);
    let bid = 0,
      ask = 0,
      bidLevels = 0,
      askLevels = 0;
    for (const [p, s] of this.bids)
      if (p >= mid - band) {
        bid += p * s;
        bidLevels++;
      }
    for (const [p, s] of this.asks)
      if (p <= mid + band) {
        ask += p * s;
        askLevels++;
      }
    return { bid, ask, bidLevels, askLevels };
  }

  snapshot(depthBps = 25, buckets = 10): AnalyticsSnapshot {
    const now = Date.now();
    const bb = this.bestBid();
    const ba = this.bestAsk();
    const mid = (bb + ba) / 2;
    const spread = ba > 0 && bb > 0 ? ba - bb : 0;

    // Depth histogram: `buckets` bins of depthBps each on BOTH sides.
    const histogram: AnalyticsSnapshot["book"]["histogram"] = [];
    if (mid > 0 && buckets > 0) {
      const step = depthBps / buckets; // bps per bin
      const binUsd = new Float64Array(buckets * 2); // [0..buckets)=asks, [buckets..2b)=bids
      for (const [p, s] of this.asks) {
        const bps = ((p - mid) / mid) * 10_000;
        if (bps < 0 || bps > depthBps) continue;
        binUsd[Math.min(buckets - 1, Math.floor(bps / step))] += p * s;
      }
      for (const [p, s] of this.bids) {
        const bps = ((mid - p) / mid) * 10_000;
        if (bps < 0 || bps > depthBps) continue;
        binUsd[buckets + Math.min(buckets - 1, Math.floor(bps / step))] += p * s;
      }
      for (let i = 0; i < buckets; i++) {
        histogram.push({
          from_bps: i * step,
          to_bps: (i + 1) * step,
          bid_usd: binUsd[buckets + i],
          ask_usd: binUsd[i],
        });
      }
    }

    // Trade flow over the rolling window.
    while (this.trades.length && now - this.trades[0].ts > FLOW_RING_MS) this.trades.shift();
    let cvdQty5 = 0,
      cvdUsd5 = 0,
      pv5 = 0,
      v5 = 0,
      buy5 = 0,
      tot5 = 0,
      count60 = 0,
      notional60 = 0;
    for (let i = this.trades.length - 1; i >= 0; i--) {
      const t = this.trades[i];
      const age = now - t.ts;
      if (age > FLOW_RING_MS) break;
      const usd = t.price * t.qty;
      cvdQty5 += t.isBuy ? t.qty : -t.qty;
      cvdUsd5 += t.isBuy ? usd : -usd;
      pv5 += t.price * t.qty;
      v5 += t.qty;
      buy5 += t.isBuy ? usd : 0;
      tot5 += usd;
      if (age <= 60_000) {
        count60++;
        notional60 += usd;
      }
    }

    // Liquidation sums.
    while (this.liqs.length && now - this.liqs[0].ts > LIQ_RING_MS) this.liqs.shift();
    let long5 = 0,
      short5 = 0,
      long60 = 0,
      short60 = 0,
      count = 0,
      largest = 0;
    for (const l of this.liqs) {
      const usd = l.price * l.qty;
      const age = now - l.ts;
      if (l.isBuy) short60 += usd;
      else long60 += usd;
      if (age <= 5 * 60_000) {
        if (l.isBuy) short5 += usd;
        else long5 += usd;
      }
      count++;
      if (usd > largest) largest = usd;
    }

    const depth = this.depthSums(depthBps);
    const imb = depth.bid + depth.ask > 0 ? (depth.bid - depth.ask) / (depth.bid + depth.ask) : 0;

    return {
      schema: "edgedepth.analytics.v1",
      exchange: this.exchange,
      symbol: this.symbol,
      ts_ms: now,
      age_ms: {
        book: this.lastBookMs ? now - this.lastBookMs : -1,
        trade: this.lastTradeMs ? now - this.lastTradeMs : -1,
        liquidation: this.lastLiqMs ? now - this.lastLiqMs : -1,
      },
      book: {
        best_bid: bb,
        best_ask: ba,
        mid,
        spread,
        spread_bps: mid > 0 ? (spread / mid) * 10_000 : 0,
        depth_bps: depthBps,
        bid_notional: depth.bid,
        ask_notional: depth.ask,
        imbalance: imb,
        bid_levels: depth.bidLevels,
        ask_levels: depth.askLevels,
        histogram,
      },
      flow: {
        cvd_qty_session: this.cvd.qty,
        cvd_usd_session: this.cvd.usd,
        cvd_qty_5m: cvdQty5,
        cvd_usd_5m: cvdUsd5,
        trade_rate_60s: count60,
        notional_rate_60s: notional60,
        vwap_5m: v5 > 0 ? pv5 / v5 : 0,
        buy_ratio_5m: tot5 > 0 ? buy5 / tot5 : 0.5,
      },
      liquidations: {
        long_usd_5m: long5,
        short_usd_5m: short5,
        long_usd_60m: long60,
        short_usd_60m: short60,
        count_60m: count,
        largest_60m_usd: largest,
      },
      candle_1m: this.candle,
    };
  }

  // ── book maintenance ────────────────────────────────────────────────────────

  private pruneBook() {
    const now = Date.now();
    if (now - this.lastPruneMs < 2_000) return; // amortize
    this.lastPruneMs = now;
    const mid = (this.bestBid() + this.bestAsk()) / 2;
    if (!(mid > 0)) return;
    const band = mid * (BOOK_KEEP_BPS / 10_000);
    for (const p of this.bids.keys()) if (p < mid - band) this.bids.delete(p);
    for (const p of this.asks.keys()) if (p > mid + band) this.asks.delete(p);
  }
}

export class AnalyticsEngine {
  private symbols = new Map<string, SymbolAnalytics>();

  for(symbol: string): SymbolAnalytics {
    let s = this.symbols.get(symbol);
    if (!s) {
      s = new SymbolAnalytics(symbol);
      this.symbols.set(symbol, s);
    }
    return s;
  }

  list(): SymbolAnalytics[] {
    return [...this.symbols.values()];
  }
}
