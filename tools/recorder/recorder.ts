// ═══════════════════════════════════════════════════════════════════════════════
// EdgeDepth personal recorder daemon (roadmap Phase 4: "Recorder daemon").
//
// Connects to the community gateway's WebSocket, subscribes the configured
// symbols (trades, order book, liquidations, 1m candles) around the clock and
// rolls the captured stream into standard .edpack replay packs - the personal,
// infinite archive the hosted service sells back to you.
//
//   gateway :9001 (WS)  ──▶  this daemon  ──▶  data/packs/*.edpack  ──▶  HTTP
//   (Binance public)         (bun, :9002)        hourly episodes          Range /
//                                                                        ETag / CORS
//
// Pack layout follows docs/EDPACK.md exactly:
//   "EDPK" | version byte (1) | u32 LE header length | PackHeader (protobuf)
//   then independently zstd-compressed blocks of [u32 LE len | PackFrame] records.
// PackFrame.payload is the gateway's WSPayload.data verbatim; PackFrame.ts_ms is
// WSPayload.event_time_ms. The boot header carries the first order-book
// snapshot (ob_seed) and the episode's final 1m candles (candle_seeds), so the
// terminal can open the pack with a live-looking chart from the first frame.
//
// The same directory serves a replay-library manifest (schema
// edgedepth.replay-library.v1) with RELATIVE pack URLs: the terminal's library
// widget resolves them against the manifest URL, so a self-hosted catalog works
// from any host - open the terminal with
//   ?replayLibrary=/manifest.json?XTransformPort=9002   (behind the gateway)
// or point ?replayLibrary= at this server directly.
//
// Run: bun recorder.ts   (config via env, see README.md; no npm deps beyond
// protobufjs - compression is Bun's native zstd).
// ═══════════════════════════════════════════════════════════════════════════════

import { load as protoLoad } from "protobufjs";
import {
  appendFileSync,
  closeSync,
  existsSync,
  mkdirSync,
  openSync,
  readFileSync,
  readdirSync,
  renameSync,
  rmSync,
  statSync,
  unlinkSync,
  writeFileSync,
  writeSync,
} from "node:fs";
import { basename, join } from "node:path";
import { AnalyticsEngine, type AnalyticsSnapshot } from "./analytics.ts";
import { HistoryStore } from "./history.ts";

// ── Configuration ──────────────────────────────────────────────────────────────

const PORT = Number(process.env.RECORDER_PORT || 9002);
const GATEWAY_WS = process.env.RECORDER_GATEWAY || "ws://127.0.0.1:9001/ws";
const EXCHANGE = process.env.RECORDER_EXCHANGE || "binancef";
const SYMBOLS = (process.env.RECORDER_SYMBOLS || "btcusdt")
  .split(",")
  .map((s) => s.trim().toLowerCase())
  .filter(Boolean);
const DATA_DIR = process.env.RECORDER_DATA || join(import.meta.dir, "data");
const PACKS_DIR = join(DATA_DIR, "packs");
const EVENTS_DIR = join(DATA_DIR, "events");
const CANDLES_DIR = join(DATA_DIR, "candles");
const REPORTS_DIR = join(DATA_DIR, "reports");
const SNAPSHOTS_DIR = join(DATA_DIR, "snapshots");

// Analytical server + SQLite history (roadmap Phase 6).
const HISTORY_DIR = join(DATA_DIR, "history");
const HISTORY_DB = process.env.RECORDER_HISTORY_DB || join(HISTORY_DIR, "history.db");
const HISTORY_DAYS = Number(process.env.RECORDER_HISTORY_DAYS || 3); // retention
const ANALYTICS_DEPTH_BPS = Number(process.env.RECORDER_DEPTH_BPS || 25);
const ANALYTICS_BUCKETS = Number(process.env.RECORDER_DEPTH_BUCKETS || 10);
const ANALYTICS_PUSH_MS = Number(process.env.RECORDER_ANALYTICS_MS || 1000); // WS cadence
const BOOK_POLL_MS = Number(process.env.RECORDER_BOOK_POLL_MS || 5000); // history cadence
const EPISODE_MS = Number(process.env.RECORDER_EPISODE_MS || 3_600_000); // hourly packs
const BLOCK_TARGET = 4 * 1024 * 1024; // uncompressed bytes per zstd block (EDPACK target)
const KEEP_PER_SYMBOL = Number(process.env.RECORDER_KEEP || 48); // 2 days of hourly packs
const CANDLE_TF = 60; // seconds; the gateway aggregates this from the trade feed

// Event finder (roadmap Phase 4): what counts as a searchable moment.
const BIGTRADE_USD = Number(process.env.RECORDER_BIGTRADE_USD || 250_000);
const VOL_SPIKE_X = Number(process.env.RECORDER_VOL_SPIKE_X || 5);
const VOL_SPIKE_COOLDOWN_MS = 15 * 60_000;
const CASCADE_WINDOW_MS = 2_000; // same chain gap the terminal's detector uses
const CASCADE_MIN_EVENTS = 3;
const EVENT_MEM_CAP = 50_000; // ~ several weeks of events for one symbol
const TZ_OFFSET_MIN = Number(process.env.RECORDER_TZ_OFFSET_MIN ?? 210); // Asia/Tehran
const KEEP_EVENT_DAYS = 120;
const KEEP_SNAPSHOTS_PER_DAY = 12;

const STREAM_TRADES = 1;
const STREAM_CANDLES = 2;
const STREAM_ORDERBOOK = 3;
const STREAM_LIQUIDATIONS = 5;
// (stream, timeframe) pairs subscribed for every symbol.
const SUBSCRIPTIONS: Array<[number, number]> = [
  [STREAM_TRADES, 0],
  [STREAM_ORDERBOOK, 0],
  [STREAM_LIQUIDATIONS, 0],
  [STREAM_CANDLES, CANDLE_TF],
];

const log = (msg: string) =>
  console.log(`[${new Date().toISOString()}] ${msg}`);

// protobufjs decodes int64 to Long objects when the `long` lib is present.
function toNum(v: unknown): number {
  if (v == null) return 0;
  if (typeof v === "number") return v;
  if (typeof v === "bigint") return Number(v);
  if (typeof v === "string") return Number(v) || 0;
  const anyV = v as { toNumber?: () => number };
  if (typeof anyV.toNumber === "function") return anyV.toNumber();
  return 0;
}

function ymdLocal(ms: number): string {
  const d = new Date(ms);
  const p = (n: number, w = 2) => String(n).padStart(w, "0");
  return (
    `${d.getUTCFullYear()}${p(d.getUTCMonth() + 1)}${p(d.getUTCDate())}` +
    `T${p(d.getUTCHours())}${p(d.getUTCMinutes())}`
  );
}

// ── Protobuf types (schema mirrors the terminal's protos/messages.proto) ──────

const proto = await protoLoad(join(import.meta.dir, "protos", "messages.proto"));
const WSPayload = proto.lookupType("pb.WSPayload");
const BookUpdate = proto.lookupType("pb.BookUpdate");
const CandlesMsg = proto.lookupType("pb.Candles");
const CandleMsg = proto.lookupType("pb.Candle");
const PackHeader = proto.lookupType("pb.PackHeader");
const PackFrame = proto.lookupType("pb.PackFrame");
const PackCandleSeed = proto.lookupType("pb.PackCandleSeed");
const LiqMsg = proto.lookupType("pb.Liquidation");
const TradePb = proto.lookupType("pb.Trade");

// ── Pack index (manifest source of truth) ─────────────────────────────────────

interface MetaRow {
  file: string;
  exchange: string;
  symbol: string;
  started_at: number; // epoch ms
  ended_at: number; // epoch ms
  frames: number;
  size: number;
  sha256: string;
}

const packIndex = new Map<string, MetaRow>(); // file → meta

// ── Event finder data model (roadmap Phase 4) ─────────────────────────────────

interface MarketEvent {
  ts: number; // epoch ms (chain start for cascades)
  kind: "cascade" | "liq" | "trade" | "vol";
  symbol: string;
  side: "long" | "short" | "buy" | "sell"; // liq/cascade: liquidated side; trade: taker side
  usd: number;
  price: number;
  detail: string;
  end?: number; // cascade end
  count?: number; // cascade liquidation count
}

function utcDayKey(ms: number): string {
  return new Date(ms).toISOString().slice(0, 10);
}

function localDayKey(ms: number, offsetMin = TZ_OFFSET_MIN): string {
  return new Date(ms + offsetMin * 60_000).toISOString().slice(0, 10);
}

function localDayWindow(dateStr: string, offsetMin = TZ_OFFSET_MIN): { from: number; to: number } {
  const [y, m, d] = dateStr.split("-").map(Number);
  const from = Date.UTC(y!, (m ?? 1) - 1, d ?? 1) - offsetMin * 60_000;
  return { from, to: from + 86_400_000 };
}

function mkdirp(dir: string) {
  if (!existsSync(dir)) mkdirSync(dir, { recursive: true });
}

class EventStore {
  events: MarketEvent[] = []; // ascending ts, memory ring capped at EVENT_MEM_CAP
  private loaded = false;

  push(e: MarketEvent) {
    const last = this.events[this.events.length - 1];
    if (last && e.ts < last.ts - 5_000) {
      // Late arrival (rare): keep the ring sorted so queries stay correct.
      let i = this.events.length - 1;
      while (i > 0 && this.events[i - 1]!.ts > e.ts) i--;
      this.events.splice(i, 0, e);
    } else {
      this.events.push(e);
    }
    if (this.events.length > EVENT_MEM_CAP)
      this.events.splice(0, this.events.length - EVENT_MEM_CAP);
    this.persist(e);
  }

  private persist(e: MarketEvent) {
    try {
      mkdirp(EVENTS_DIR);
      appendFileSync(
        join(EVENTS_DIR, `events-${utcDayKey(e.ts)}.jsonl`),
        JSON.stringify(e) + "\n",
      );
    } catch {
      // disk hiccups never block recording
    }
  }

  loadRecent() {
    if (this.loaded) return;
    this.loaded = true;
    mkdirp(EVENTS_DIR);
    const files = readdirSync(EVENTS_DIR)
      .filter((f) => /^events-\d{4}-\d{2}-\d{2}\.jsonl$/.test(f))
      .sort()
      .reverse();
    const lines: MarketEvent[] = [];
    for (const f of files) {
      try {
        for (const line of readFileSync(join(EVENTS_DIR, f), "utf8").split("\n")) {
          if (!line) continue;
          try {
            const e = JSON.parse(line) as MarketEvent;
            if (e && e.ts && e.kind) lines.push(e);
          } catch {
            // skip corrupt line
          }
        }
      } catch {
        // unreadable file
      }
      if (lines.length >= EVENT_MEM_CAP) break;
    }
    lines.sort((a, b) => a.ts - b.ts);
    this.events = lines.slice(-EVENT_MEM_CAP);
    log(`event index: ${this.events.length} event(s) loaded from ${EVENTS_DIR}`);
  }

  pruneOld() {
    try {
      const cutoff = Date.now() - KEEP_EVENT_DAYS * 86_400_000;
      for (const f of readdirSync(EVENTS_DIR)) {
        const m = /^events-(\d{4}-\d{2}-\d{2})\.jsonl$/.exec(f);
        if (m && new Date(`${m[1]}T00:00:00Z`).getTime() < cutoff)
          rmSync(join(EVENTS_DIR, f), { force: true });
      }
    } catch {
      // best effort
    }
  }

  query(opts: {
    kind?: string;
    minUsd?: number;
    symbol?: string;
    from?: number;
    to?: number;
    limit?: number;
  }): MarketEvent[] {
    const kind = opts.kind && opts.kind !== "all" ? opts.kind : null;
    const minUsd = opts.minUsd ?? 0;
    const out: MarketEvent[] = [];
    for (let i = this.events.length - 1; i >= 0; i--) {
      const e = this.events[i]!;
      if (kind && e.kind !== kind) continue;
      if (minUsd > 0 && e.usd < minUsd) continue;
      if (opts.symbol && e.symbol !== opts.symbol) continue;
      if (opts.from != null && e.ts < opts.from) continue;
      if (opts.to != null && e.ts > opts.to) continue;
      out.push(e);
      if (out.length >= (opts.limit ?? 300)) break;
    }
    return out;
  }
}

interface CandleRow {
  t: number;
  o: number;
  h: number;
  l: number;
  c: number;
  v: number;
}

class CandleStore {
  days = new Map<string, CandleRow[]>(); // UTC day → ascending rows
  private dirty = new Set<string>();

  append(sym: string, row: CandleRow) {
    const day = utcDayKey(row.t);
    let rows = this.days.get(day);
    if (!rows) {
      rows = [];
      this.days.set(day, rows);
    }
    // The gateway re-sends the forming candle; keep only the latest per minute.
    // Final candles only reach this store, so plain replacement is correct.
    const last = rows[rows.length - 1];
    if (last && last.t === row.t) rows[rows.length - 1] = row;
    else if (!last || row.t > last.t) rows.push(row);
    this.dirty.add(day);
    void sym;
  }

  flush() {
    if (!this.dirty.size) return;
    mkdirp(CANDLES_DIR);
    for (const day of this.dirty) {
      const rows = this.days.get(day);
      if (!rows) continue;
      try {
        const payload = rows.map((r) => `${r.t},${r.o},${r.h},${r.l},${r.c},${r.v}`).join("\n");
        writeFileSync(join(CANDLES_DIR, `candles-${day}.csv`), payload + "\n");
      } catch {
        // best effort
      }
    }
    this.dirty.clear();
  }

  loadRecent(keepDays = 8) {
    mkdirp(CANDLES_DIR);
    const files = readdirSync(CANDLES_DIR)
      .filter((f) => /^candles-\d{4}-\d{2}-\d{2}\.csv$/.test(f))
      .sort()
      .slice(-keepDays);
    for (const f of files) {
      const day = f.slice(8, 18);
      try {
        const rows: CandleRow[] = [];
        for (const line of readFileSync(join(CANDLES_DIR, f), "utf8").split("\n")) {
          if (!line) continue;
          const [t, o, h, l, c, v] = line.split(",").map(Number);
          if (t && o && h && l && c) rows.push({ t, o, h, l, c, v: v || 0 });
        }
        if (rows.length) this.days.set(day, rows);
      } catch {
        // skip unreadable day
      }
    }
    log(`candle store: ${[...this.days.keys()].length} day file(s) loaded from ${CANDLES_DIR}`);
  }

  window(from: number, to: number): CandleRow[] {
    const out: CandleRow[] = [];
    const first = utcDayKey(from);
    const lastDay = utcDayKey(to - 1);
    for (
      let d = new Date(`${first}T00:00:00Z`);
      d.getTime() < to;
      d = new Date(d.getTime() + 86_400_000)
    ) {
      const rows = this.days.get(d.toISOString().slice(0, 10));
      if (rows) for (const r of rows) if (r.t >= from && r.t < to) out.push(r);
    }
    void lastDay;
    return out;
  }

  pruneOld() {
    for (const [day, rows] of [...this.days]) {
      if (new Date(`${day}T00:00:00Z`).getTime() < Date.now() - 8 * 86_400_000)
        this.days.delete(day);
      else if (rows.length > 1500) this.days.set(day, rows.slice(-1440));
    }
  }
}

const eventStore = new EventStore();
const candleStore = new CandleStore();

// Cascade chains: same-side liquidations with ≤2s gaps, ≥3 events. A chain is
// FINALIZED once 2s pass without a new same-side fill (timer sweep), then it
// becomes one searchable "cascade" event - the same shape the terminal's
// cascade_detector reports live.
interface PendingCascade {
  side: "long" | "short";
  start: number;
  last: number;
  count: number;
  usd: number;
}
const pendingCascades = new Map<string, PendingCascade>();

function feedCascade(sym: string, e: MarketEvent) {
  const side = e.side === "short" ? "short" : "long";
  for (const [key, p] of [...pendingCascades]) {
    if (e.ts - p.last > CASCADE_WINDOW_MS) {
      finalizeCascade(key, p);
      pendingCascades.delete(key);
    }
  }
  const key = `${sym}:${side}`;
  const open = pendingCascades.get(key);
  if (open) {
    open.last = e.ts;
    open.count += 1;
    open.usd += e.usd;
  } else {
    pendingCascades.set(key, { side, start: e.ts, last: e.ts, count: 1, usd: e.usd });
  }
}

function finalizeCascade(key: string, p: PendingCascade) {
  if (p.count < CASCADE_MIN_EVENTS) return;
  const sym = key.slice(0, key.lastIndexOf(":"));
  eventStore.push({
    ts: p.start,
    kind: "cascade",
    symbol: sym,
    side: p.side,
    usd: p.usd,
    price: 0,
    detail: `${p.count} same-side liquidations within ${((p.last - p.start) / 1000).toFixed(1)}s`,
    end: p.last,
    count: p.count,
  });
}

function sweepCascades() {
  const now = Date.now();
  for (const [key, p] of [...pendingCascades]) {
    if (now - p.last > CASCADE_WINDOW_MS) {
      finalizeCascade(key, p);
      pendingCascades.delete(key);
    }
  }
}

// Per-symbol volume-spike tracker: a final 1m candle trading ≥ VOL_SPIKE_X ×
// the mean of the previous hour fires once, then cools down.
const volWindows = new Map<string, number[]>();
const volLastFire = new Map<string, number>();

function feedCandleForEvents(sym: string, c: { volume: number; close: number; timestampMs: number }) {
  if (!(c.volume > 0)) return;
  const win = volWindows.get(sym) ?? [];
  if (win.length >= 60) {
    const mean = win.reduce((a, b) => a + b, 0) / win.length;
    const ratio = mean > 0 ? c.volume / mean : 0;
    const lastFire = volLastFire.get(sym) ?? 0;
    if (ratio >= VOL_SPIKE_X && c.timestampMs - lastFire > VOL_SPIKE_COOLDOWN_MS) {
      volLastFire.set(sym, c.timestampMs);
      eventStore.push({
        ts: c.timestampMs,
        kind: "vol",
        symbol: sym,
        side: "buy",
        usd: c.volume * c.close,
        price: c.close,
        detail: `${ratio.toFixed(1)}x the 1h average volume (${c.volume.toFixed(1)} base units)`,
      });
    }
  }
  win.push(c.volume);
  if (win.length > 60) win.shift();
  volWindows.set(sym, win);
}

function loadIndex() {
  mkdirSync(PACKS_DIR, { recursive: true });
  for (const f of readdirSync(PACKS_DIR)) {
    if (!f.endsWith(".meta.json")) continue;
    try {
      const meta = JSON.parse(readFileSync(join(PACKS_DIR, f), "utf8")) as MetaRow;
      if (meta.file && existsSync(join(PACKS_DIR, meta.file)))
        packIndex.set(meta.file, meta);
    } catch {
      // unreadable sidecar: the pack stays on disk but out of the catalog
    }
  }
  log(`index loaded: ${packIndex.size} pack(s) in ${PACKS_DIR}`);
}

// ── Episode builder: one symbol, one rolling window → one .edpack ─────────────

interface Frame {
  ts: number;
  stream: number;
  tf: number;
  payload: Uint8Array;
}

interface BlockEntry {
  first: number;
  last: number;
  offset: number;
  length: number;
  count: number;
}

class Episode {
  readonly symbol: string;
  readonly startedAt: number;
  frames = 0;      // frames handed to blocks (status uses `captured` live)
  captured = 0;    // frames ingested since episode start
  firstTs = 0;
  lastTs = 0;
  private pending: Frame[] = [];
  private pendingBytes = 0;
  private blocks: BlockEntry[] = [];
  private dataBytes = 0;
  private tmpFd: number | null = null;
  private readonly tmpPath: string;
  obSeed: Uint8Array | null = null;
  obSeedTs = 0;
  candles = new Map<number, unknown>(); // final 1m candles, ts → Candle object

  constructor(symbol: string, startedAt: number) {
    this.symbol = symbol;
    this.startedAt = startedAt;
    this.tmpPath = join(PACKS_DIR, `.tmp-${symbol}-${startedAt}.data`);
  }

  add(f: Frame) {
    if (this.frames === 0) this.firstTs = f.ts;
    this.lastTs = f.ts;
    this.captured++;
    this.pending.push(f);
    this.pendingBytes += f.payload.length + 32;
    this.maybeFlushBlock();
  }

  private maybeFlushBlock() {
    if (this.pendingBytes >= BLOCK_TARGET) this.flushBlock();
  }

  private flushBlock() {
    if (!this.pending.length) return;
    // Merge streams by timestamp, order book first on ties (EDPACK builder
    // convention) - the reader walks blocks strictly in order when seeking.
    this.pending.sort(
      (a, b) =>
        a.ts - b.ts ||
        (a.stream === STREAM_ORDERBOOK ? 0 : 1) -
          (b.stream === STREAM_ORDERBOOK ? 0 : 1),
    );
    const parts: Uint8Array[] = [];
    let total = 0;
    for (const f of this.pending) {
      const enc = PackFrame.encode(
        PackFrame.fromObject({ tsMs: f.ts, stream: f.stream, timeframe: f.tf, payload: f.payload }),
      ).finish();
      const len = Buffer.alloc(4);
      len.writeUInt32LE(enc.length, 0);
      parts.push(len, enc);
      total += 4 + enc.length;
    }
    const comp = Bun.zstdCompressSync(Buffer.concat(parts, total));
    if (this.tmpFd === null) this.tmpFd = openSync(this.tmpPath, "w");
    writeSync(this.tmpFd, comp);
    const first = this.pending[0]!.ts;
    const last = this.pending[this.pending.length - 1]!.ts;
    this.blocks.push({
      first,
      last,
      offset: this.dataBytes,
      length: comp.length,
      count: this.pending.length,
    });
    this.dataBytes += comp.length;
    this.frames += this.pending.length;
    this.pending = [];
    this.pendingBytes = 0;
  }

  async close(): Promise<MetaRow | null> {
    this.flushBlock();
    if (this.tmpFd !== null) {
      closeSync(this.tmpFd);
      this.tmpFd = null;
    }
    if (!this.frames || !this.firstTs) {
      rmSync(this.tmpPath, { force: true });
      log(`[${this.symbol}] episode dropped: no frames captured`);
      return null;
    }

    // Boot seeds: the episode's completed 1m candles (last 720 kept) so the
    // chart has history the moment the pack opens.
    const candleSeeds: unknown[] = [];
    if (this.candles.size) {
      const values = [...this.candles.entries()]
        .sort((a, b) => a[0] - b[0])
        .map((e) => e[1])
        .slice(-720);
      const cbytes = CandlesMsg.encode(
        CandlesMsg.fromObject({ timeframe: CANDLE_TF, values }),
      ).finish();
      candleSeeds.push(
        PackCandleSeed.fromObject({ timeframeSec: CANDLE_TF, candles: cbytes }),
      );
    }

    const fileBase = `${this.symbol}-${ymdLocal(this.startedAt)}`;
    // Window = the true extent of every recorded frame. Block min/max are
    // computed AFTER the per-block timestamp sort, so they are authoritative;
    // firstTs/lastTs are arrival-order values and can carry a wall-clock
    // fallback (a frame with event_time_ms=0 lands at Date.now()), which used
    // to put a dead lead-in at the head of the pack: the replay engine holds
    // its clock at start_ts_ms, finds no frames under it for the whole boot
    // gap, and the buffering gate times out. Blocks only, no arrival times.
    let winFirst = this.blocks.length ? this.blocks[0]!.first : this.firstTs;
    let winLast = this.blocks.length ? this.blocks[0]!.last : this.lastTs;
    for (const b of this.blocks) {
      if (b.first < winFirst) winFirst = b.first;
      if (b.last > winLast) winLast = b.last;
    }
    const header = PackHeader.encode(
      PackHeader.fromObject({
        formatVersion: 1,
        eventId: `${ymdLocal(this.startedAt)}_${this.symbol}_recorder`,
        exchange: EXCHANGE,
        symbol: this.symbol,
        startTsMs: winFirst,
        endTsMs: winLast,
        obSeed: this.obSeed ?? undefined,
        obSeedSrcTsMs: this.obSeed ? this.obSeedTs : 0,
        candleSeeds,
        blocks: this.blocks.map((b) => ({
          firstTsMs: b.first,
          lastTsMs: b.last,
          offset: b.offset,
          length: b.length,
          frameCount: b.count,
        })),
        totalFrames: this.frames,
        description: `Personal recorder: ${EXCHANGE} ${this.symbol}, one-hour live capture via the community gateway (trades, order book, liquidations, 1m candles).`,
        tags: ["recorder", EXCHANGE, "personal"],
      }),
    ).finish();

    const prefix = Buffer.alloc(9);
    prefix.write("EDPK", 0, "ascii");
    prefix.writeUInt8(1, 4);
    prefix.writeUInt32LE(header.length, 5);

    const outPath = join(PACKS_DIR, `${fileBase}.edpack`);
    const incomplete = `${outPath}.incomplete`;
    try {
      writeFileSync(incomplete, prefix);
      appendFileSync(incomplete, Buffer.from(header));
      if (this.dataBytes > 0)
        appendFileSync(incomplete, readFileSync(this.tmpPath));
      // Integrity: sha256 over exactly the bytes the manifest advertises.
      const hasher = new Bun.CryptoHasher("sha256");
      hasher.update(readFileSync(incomplete));
      const sha256 = hasher.digest("hex");
      renameSync(incomplete, outPath);
      const meta: MetaRow = {
        file: `${fileBase}.edpack`,
        exchange: EXCHANGE,
        symbol: this.symbol,
        started_at: winFirst,
        ended_at: winLast,
        frames: this.frames,
        size: statSync(outPath).size,
        sha256,
      };
      writeFileSync(
        join(PACKS_DIR, `${fileBase}.meta.json`),
        JSON.stringify(meta),
      );
      packIndex.set(meta.file, meta);
      log(
        `[${this.symbol}] pack written: ${meta.file} (${(meta.size / 1e6).toFixed(1)} MB, ${meta.frames} frames, ${(this.dataBytes / 1e6).toFixed(1)} MB compressed)`,
      );
      this.prune();
      return meta;
    } catch (e) {
      log(`[${this.symbol}] pack write failed: ${e}`);
      rmSync(incomplete, { force: true });
      return null;
    } finally {
      rmSync(this.tmpPath, { force: true });
    }
  }

  private prune() {
    const mine = [...packIndex.values()]
      .filter((m) => m.symbol === this.symbol)
      .sort((a, b) => a.started_at - b.started_at);
    while (mine.length > KEEP_PER_SYMBOL) {
      const old = mine.shift()!;
      try {
        rmSync(join(PACKS_DIR, old.file), { force: true });
        rmSync(join(PACKS_DIR, old.file.replace(/\.edpack$/, ".meta.json")), {
          force: true,
        });
        packIndex.delete(old.file);
        log(`[${this.symbol}] pruned ${old.file} (keep ${KEEP_PER_SYMBOL}/symbol)`);
      } catch {
        // best effort
      }
    }
  }
}

// ── Sessions: one live episode per symbol + ingest routing ────────────────────

class Session {
  episode: Episode | null = null; // opened lazily on the first real frame
  lastFrameMs = 0;

  constructor(readonly symbol: string) {}

  ingest(stream: number, tf: number, ts: number, payload: Uint8Array) {
    this.lastFrameMs = Date.now();
    // Lazy episode open: the episode's clock starts at the first REAL frame,
    // never at process boot. An episode opened at boot across a subscribe gap
    // used to put a dead lead-in at the head of the pack (a lone forming
    // candle, then nothing for the whole gap), which stalled the terminal's
    // replay buffering gate past its readiness budget.
    if (!this.episode) this.episode = new Episode(this.symbol, ts);
    const ep = this.episode;
    // Boot seeds are captured alongside the raw stream.
    if (stream === STREAM_ORDERBOOK) {
      // Feed the analytics book on EVERY frame (snapshot + delta semantics
      // mirror OrderbookManager) and keep the pack's boot seed as before.
      try {
        const bu = BookUpdate.decode(payload);
        analytics
          .for(this.symbol)
          .applyBook({ snapshot: !!bu.snapshot, asks: bu.asks, bids: bu.bids, tsMs: ts });
        if (!ep.obSeed && bu.snapshot) {
          ep.obSeed = payload; // verbatim marshalled BookUpdate{snapshot:true}
          ep.obSeedTs = ts;
        }
      } catch {
        // not decodable - can't be a seed or analytics input anyway
      }
    } else if (stream === STREAM_CANDLES) {
      // The gateway broadcasts STREAM_CANDLES as a SINGULAR pb.Candle (its
      // hub.go documents why: Candles/Candle field numbers collide on the
      // wire, so a plural parse silently yields zeros). Keep the completed
      // ones; they become the pack's candle_seeds at rotation.
      try {
        const c = CandleMsg.decode(payload);
        if (c.final) {
          ep.candles.set(toNum(c.timestampMs), CandleMsg.toObject(c));
          // Event-finder inputs: the day-candle store (reports) + vol spikes.
          const row: CandleRow = {
            t: toNum(c.timestampMs),
            o: c.open,
            h: c.high,
            l: c.low,
            c: c.close,
            v: c.volume,
          };
          candleStore.append(this.symbol, row);
          analytics.for(this.symbol).applyCandle(row);
          history.queueCandle([
            row.t,
            this.symbol,
            row.o,
            row.h,
            row.l,
            row.c,
            row.v,
          ]);
          feedCandleForEvents(this.symbol, {
            volume: c.volume,
            close: c.close,
            timestampMs: toNum(c.timestampMs),
          });
        }
      } catch {
        // candle decode issues never block recording
      }
    } else if (stream === STREAM_LIQUIDATIONS) {
      // Event finder: every forced fill becomes a searchable "liq" moment and
      // feeds the same-side cascade chains (≤2s gaps, ≥3 events).
      try {
        const liq = LiqMsg.decode(payload);
        const price = liq.price || liq.avg_price;
        const qty = liq.qty || liq.filled_qty;
        if (price > 0 && qty > 0) {
          const usd = price * qty;
          const event: MarketEvent = {
            ts,
            kind: "liq",
            symbol: this.symbol,
            side: liq.is_buy ? "short" : "long", // forced BUY = shorts liquidated
            usd,
            price,
            detail: `${qty.toFixed(4)} @ ${price}`,
          };
          eventStore.push(event);
          feedCascade(this.symbol, event);
          analytics.for(this.symbol).applyLiq({ ts, price, qty, isBuy: liq.is_buy });
          history.queueLiq([ts, this.symbol, event.side, price, qty, usd]);
        }
      } catch {
        // liq decode issues never block recording
      }
    } else if (stream === STREAM_TRADES) {
      // Event finder: outsized single fills (aggTrade notional).
      try {
        const t = TradePb.decode(payload);
        const price = t.price;
        const qty = t.qty;
        const usd = price * qty;
        const tTs = toNum(t.timestampMs) || ts;
        // Analytical flow + full-fidelity normalized history (retention-bound).
        analytics.for(this.symbol).applyTrade({ ts: tTs, price, qty, isBuy: t.is_buy });
        history.queueTrade([tTs, this.symbol, price, qty, t.is_buy ? 1 : 0, usd]);
        if (usd >= BIGTRADE_USD) {
          eventStore.push({
            ts: toNum(t.timestampMs) || ts,
            kind: "trade",
            symbol: this.symbol,
            side: t.is_buy ? "buy" : "sell",
            usd,
            price,
            detail: `${qty.toFixed(3)} @ ${price}`,
          });
        }
      } catch {
        // trade decode issues never block recording
      }
    }
    ep.add({ ts, stream, tf, payload });
  }

  async rotate(): Promise<void> {
    const old = this.episode;
    this.episode = null;
    if (old) await old.close();
  }
}

const sessions = new Map<string, Session>();
for (const s of SYMBOLS) sessions.set(s, new Session(s));

// ── Analytical engine + SQLite history (roadmap Phase 6) ────────────────────

// Live per-symbol analytics on the SAME normalized feed the packs record:
// order book state (snapshot+delta semantics), trade flow (CVD/rates/VWAP),
// liquidation sums, latest 1m candle. Served over REST /analytics and pushed
// on WS /analytics/ws - the consumer terminal never touches raw wire data.
const analytics = new AnalyticsEngine();
for (const s of SYMBOLS) analytics.for(s).exchange = EXCHANGE;

// Normalized-stream history in SQLite (WAL): every trade, every liquidation,
// completed 1m candles and a periodic top-of-book poll, batched so the WS
// ingest path never blocks. REST: /history/{trades,liquidations,candles,book}.
const history = new HistoryStore(HISTORY_DB, HISTORY_DAYS);

// ── Gateway WebSocket client (auto-reconnect, control plane is JSON text) ─────

let ws: WebSocket | null = null;
let backoffMs = 1000;
let wsConnected = false;

function subscribeAll() {
  for (const [sym, sess] of sessions) {
    for (const [stream, tf] of SUBSCRIPTIONS) {
      ws?.send(
        JSON.stringify({
          method: "subscribe",
          data: { pair: { exchange: EXCHANGE, symbol: sym }, stream, timeframe: tf },
        }),
      );
    }
    log(`subscribed ${EXCHANGE}:${sym} (trades/orderbook/liquidations/candles ${CANDLE_TF}s)`);
  }
}

let badFrames = 0;

function handleBinary(data: ArrayBuffer) {
  try {
    const msg = WSPayload.decode(new Uint8Array(data));
    const sym = String((msg.pair as { symbol?: string } | null)?.symbol || "");
    const sess = sessions.get(sym);
    if (!sess) return;
    const ts = toNum(msg.eventTimeMs) || Date.now();
    const payload = msg.data as Uint8Array;
    if (!payload || payload.length === 0) return;
    sess.ingest(msg.stream as number, toNum(msg.timeframe), ts, payload);
  } catch {
    badFrames++;
  }
}

function connect() {
  log(`connecting to gateway ${GATEWAY_WS}`);
  const sock = new WebSocket(GATEWAY_WS);
  sock.binaryType = "arraybuffer";
  sock.onopen = () => {
    wsConnected = true;
    backoffMs = 1000;
    ws = sock;
    subscribeAll();
  };
  sock.onmessage = (ev) => {
    if (typeof ev.data === "string") return; // control plane only
    handleBinary(ev.data as ArrayBuffer);
  };
  sock.onclose = () => {
    wsConnected = false;
    if (ws === sock) ws = null;
    log(`gateway disconnected - retrying in ${backoffMs}ms`);
    setTimeout(connect, backoffMs);
    backoffMs = Math.min(backoffMs * 2, 30_000);
  };
  sock.onerror = () => sock.close();
}
connect();

// ── Rotation loop + graceful shutdown ─────────────────────────────────────────

let rotating = false;
setInterval(async () => {
  if (rotating) return;
  const now = Date.now();
  for (const sess of sessions.values()) {
    if (!sess.episode || now - sess.episode.startedAt < EPISODE_MS) continue;
    rotating = true;
    try {
      await sess.rotate();
    } catch (e) {
      log(`[${sess.symbol}] rotation failed: ${e}`);
    }
    rotating = false;
  }
}, 1000);

// ── Housekeeping: cascade sweep, candle flush, day rollover, event prunes ────

let lastAutoDay = localDayKey(Date.now());
let lastLiveReportAt = 0;
setInterval(() => {
  try {
    sweepCascades();
    const today = localDayKey(Date.now());
    if (today !== lastAutoDay) {
      // Local day rolled over (Asia/Tehran by default): publish yesterday's
      // complete report, then start a fresh live day.
      autoGenerateReport(lastAutoDay);
      lastAutoDay = today;
      eventStore.pruneOld();
    }
    // Refresh the live "today" report every 30 minutes so /reports always has
    // something current even before the rollover.
    if (Date.now() - lastLiveReportAt > 30 * 60_000) {
      lastLiveReportAt = Date.now();
      autoGenerateReport(today);
    }
  } catch {
    // housekeeping never kills the daemon
  }
}, 2000);

setInterval(() => candleStore.flush(), 10_000);
setInterval(() => candleStore.pruneOld(), 3600_000);

// Phase 6 history plumbing: batched SQLite flush, top-of-book polls, prune.
setInterval(() => history.flush(), 2_000);
setInterval(() => {
  for (const s of analytics.list()) history.queueBookPoll(s.bookPollRow());
}, BOOK_POLL_MS);
setInterval(() => history.prune(), 3600_000);

let shuttingDown = false;
async function shutdown() {
  if (shuttingDown) return;
  shuttingDown = true;
  log("shutting down - closing episodes");
  try {
    ws?.close();
  } catch {}
  // Finalize any open cascade chain + persist the day candles before exit.
  for (const [key, p] of [...pendingCascades]) {
    finalizeCascade(key, p);
    pendingCascades.delete(key);
  }
  candleStore.flush();
  history.close();
  for (const sess of sessions.values()) {
    if (sess.episode) await sess.episode.close();
  }
  process.exit(0);
}
process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);

// ── HTTP: status, replay-library manifest, pack files with Range support ──────

function manifestJson(url: URL): Response {
  // Keep the catalog host-agnostic: pack URLs are RELATIVE paths; the
  // terminal's library widget resolves them against the manifest URL. The
  // XTransformPort routing param (when present on this request) rides along so
  // the packs route back to this service behind the site's gateway.
  const xtp = url.searchParams.get("XTransformPort");
  const suffix = xtp ? `?XTransformPort=${encodeURIComponent(xtp)}` : "";
  const packs = [...packIndex.values()]
    .sort((a, b) => b.ended_at - a.ended_at)
    .slice(0, 64) // the library widget caps at 64 entries
    .map((m) => ({
      id: `recorder-${m.symbol}-${ymdLocal(m.started_at)}`,
      event_id: `${ymdLocal(m.started_at)}_${m.symbol}_recorder`,
      title: `${m.symbol.toUpperCase()} recorder episode, ${ymdLocal(m.started_at).replace("T", " ")}`,
      summary: `One hour of live ${m.exchange} ${m.symbol}: order book, trades, liquidations and 1m candles captured by the personal recorder daemon.`,
      exchange: "Binance Futures",
      exchange_id: m.exchange,
      symbol: m.symbol,
      display_symbol: `${m.symbol.toUpperCase()}/USDT`,
      captured_at: new Date(m.started_at).toISOString(),
      window_start: new Date(m.started_at).toISOString(),
      window_end: new Date(m.ended_at).toISOString(),
      duration_seconds: Math.max(1, Math.round((m.ended_at - m.started_at) / 1000)),
      size_bytes: m.size,
      sha256: m.sha256,
      pack_url: `/packs/${m.file}${suffix}`,
      tags: ["recorder", "personal"],
      featured: false,
    }));
  const body = JSON.stringify({
    schema: "edgedepth.replay-library.v1",
    updated_at: new Date().toISOString(),
    packs,
  });
  return new Response(body, {
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
      "access-control-allow-origin": "*",
    },
  });
}

function packFileResponse(req: Request, url: URL): Response {
  const name = basename(url.pathname);
  const path = join(PACKS_DIR, name);
  if (!name.endsWith(".edpack") || !existsSync(path))
    return new Response("not found", { status: 404 });
  const meta = packIndex.get(name);
  const size = statSync(path).size;
  const etag = `"${meta?.sha256 || `${name}-${size}`}"`;
  const base: Record<string, string> = {
    "content-type": "application/octet-stream",
    "accept-ranges": "bytes",
    etag,
    "cache-control": "public, max-age=31536000, immutable",
    "access-control-allow-origin": "*",
    "access-control-allow-headers": "Range",
    "access-control-expose-headers": "Content-Range, Content-Length, ETag, Accept-Ranges",
  };
  if (req.method === "HEAD") return new Response(null, { headers: base });

  const range = req.headers.get("range");
  if (range) {
    const m = /bytes=(\d*)-(\d*)/.exec(range);
    if (m) {
      let start = m[1] ? Number(m[1]) : 0;
      let end = m[2] ? Number(m[2]) : size - 1;
      if (!m[1] && m[2]) {
        start = Math.max(0, size - Number(m[2]));
        end = size - 1;
      }
      start = Math.min(Math.max(0, start), Math.max(0, size - 1));
      end = Math.min(end, size - 1);
      if (start > end || start >= size) {
        return new Response(null, {
          status: 416,
          headers: { ...base, "content-range": `bytes */${size}` },
        });
      }
      return new Response(Bun.file(path).slice(start, end + 1), {
        status: 206,
        headers: {
          ...base,
          "content-range": `bytes ${start}-${end}/${size}`,
          "content-length": String(end - start + 1),
        },
      });
    }
  }
  return new Response(Bun.file(path), {
    headers: { ...base, "content-length": String(size) },
  });
}

// ── Event finder: pack lookup + /events feed ──────────────────────────────────

function packForEvent(sym: string, ts: number): { file: string; start: number; live: boolean } | null {
  let best: MetaRow | null = null;
  for (const m of packIndex.values()) {
    if (m.symbol !== sym) continue;
    if (m.started_at <= ts && ts <= m.ended_at + 60_000) {
      if (!best || m.started_at > best.started_at) best = m;
    }
  }
  if (best) return { file: best.file, start: best.started_at, live: false };
  // The moment may sit inside the still-open episode - it is being recorded,
  // there just is no pack to open yet.
  const sess = sessions.get(sym);
  if (sess && ts >= sess.episode.startedAt) return { file: "", start: sess.episode.startedAt, live: true };
  return null;
}

function eventsResponse(url: URL): Response {
  const q = url.searchParams;
  const events = eventStore.query({
    kind: q.get("kind") || undefined,
    minUsd: Number(q.get("min_usd") || 0),
    symbol: q.get("symbol") || undefined,
    from: q.get("from") ? Number(q.get("from")) : undefined,
    to: q.get("to") ? Number(q.get("to")) : undefined,
    limit: Math.min(Number(q.get("limit") || 300), 1000),
  });
  const body = JSON.stringify({
    schema: "edgedepth.events.v1",
    generated_at: new Date().toISOString(),
    tz_offset_min: TZ_OFFSET_MIN,
    thresholds: { bigtrade_usd: BIGTRADE_USD, vol_spike_x: VOL_SPIKE_X, cascade_min_events: CASCADE_MIN_EVENTS },
    events: events.map((e) => {
      const pack = packForEvent(e.symbol, e.ts);
      return {
        ts_ms: e.ts,
        end_ms: e.end ?? null,
        kind: e.kind,
        symbol: e.symbol,
        side: e.side,
        usd: e.usd,
        price: e.price,
        detail: e.detail,
        count: e.count ?? null,
        pack: pack?.file ? pack.file : null,
        pack_start_ms: pack?.start ?? null,
        live: pack?.live ?? false,
      };
    }),
  });
  return new Response(body, {
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
      "access-control-allow-origin": "*",
    },
  });
}

// ── Daily report (roadmap Phase 4): self-contained HTML with SVG charts ───────

const esc = (s: string) =>
  s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");

const fmtUsd = (v: number): string => {
  const a = Math.abs(v);
  if (a >= 1e9) return `$${(v / 1e9).toFixed(2)}B`;
  if (a >= 1e6) return `$${(v / 1e6).toFixed(2)}M`;
  if (a >= 1e3) return `$${(v / 1e3).toFixed(1)}K`;
  return `$${v.toFixed(0)}`;
};

function hmsLocal(ms: number, offsetMin = TZ_OFFSET_MIN): string {
  const d = new Date(ms + offsetMin * 60_000);
  const p = (n: number) => String(n).padStart(2, "0");
  return `${p(d.getUTCHours())}:${p(d.getUTCMinutes())}:${p(d.getUTCSeconds())}`;
}

const SIDE_COLORS: Record<string, string> = {
  long: "#ef5350", // longs liquidated → forced sells
  short: "#26a69a", // shorts liquidated → forced buys
  buy: "#26a69a",
  sell: "#ef5350",
};

function renderReportHtml(dateStr: string, now = Date.now()): string {
  const { from, to } = localDayWindow(dateStr);
  const blocks: string[] = [];

  for (const sym of SYMBOLS) {
    const candles = candleStore.window(from, to);
    const events = eventStore.query({ symbol: sym, from, to, limit: 1000 });
    const liqs = events.filter((e) => e.kind === "liq");
    const cascades = events.filter((e) => e.kind === "cascade");
    const trades = events.filter((e) => e.kind === "trade");
    const vols = events.filter((e) => e.kind === "vol");
    const liqLong = liqs.filter((e) => e.side === "long").reduce((a, e) => a + e.usd, 0);
    const liqShort = liqs.filter((e) => e.side === "short").reduce((a, e) => a + e.usd, 0);
    const bigCascade = cascades.reduce<MarketEvent | null>((b, e) => (!b || e.usd > b.usd ? e : b), null);
    const bigTrade = trades.reduce<MarketEvent | null>((b, e) => (!b || e.usd > b.usd ? e : b), null);

    const kpi = (label: string, value: string, color = "#e8eaed") =>
      `<div class="kpi"><div class="kpi-l">${esc(label)}</div><div class="kpi-v" style="color:${color}">${esc(value)}</div></div>`;

    let svg = "";
    if (candles.length > 1) {
      const W = 880, H = 230, padL = 8, padR = 62, padT = 12, padB = 20;
      const lo = Math.min(...candles.map((c) => c.l));
      const hi = Math.max(...candles.map((c) => c.h));
      const pad = Math.max((hi - lo) * 0.04, hi * 0.0002);
      const ylo = lo - pad, yhi = hi + pad;
      const x = (t: number) => padL + ((t - from) / (to - from)) * (W - padL - padR);
      const y = (p: number) => padT + (1 - (p - ylo) / (yhi - ylo)) * (H - padT - padB);
      const parts: string[] = [];
      // 4h grid + right-edge price labels
      for (let gt = Math.ceil(from / (4 * 3600_000)) * 4 * 3600_000; gt < to; gt += 4 * 3600_000) {
        parts.push(
          `<line x1="${x(gt).toFixed(1)}" y1="${padT}" x2="${x(gt).toFixed(1)}" y2="${H - padB}" stroke="#21262d" stroke-width="1"/>`,
        );
        parts.push(
          `<text x="${(W - padR + 6).toFixed(1)}" y="${(y(candles.find((c) => c.t >= gt - 60000)?.c ?? hi) + 4).toFixed(1)}" fill="#6e7681" font-size="10">${(Math.round((candles.find((c) => c.t >= gt - 60000)?.c ?? 0) * 10) / 10).toLocaleString("en-US")}</text>`,
        );
      }
      // cascade bands
      for (const c of cascades) {
        const x0 = x(c.ts), x1 = x(c.end ?? c.ts + 2000);
        parts.push(
          `<rect x="${x0.toFixed(1)}" y="${padT}" width="${Math.max(2, x1 - x0).toFixed(1)}" height="${H - padT - padB}" fill="#d29922" opacity="0.13"/>`,
        );
      }
      // price polyline (1m closes)
      const pts = candles
        .filter((_, i) => i % Math.max(1, Math.ceil(candles.length / 800)) === 0 || i === candles.length - 1)
        .map((c) => `${x(c.t).toFixed(1)},${y(c.c).toFixed(1)}`)
        .join(" ");
      parts.push(
        `<polyline points="${pts}" fill="none" stroke="#58a6ff" stroke-width="1.4" stroke-linejoin="round"/>`,
      );
      // liquidation markers
      const maxUsd = liqs.reduce((m, e) => Math.max(m, e.usd), 1);
      for (const e of liqs) {
        const r = 2 + 8 * Math.sqrt(e.usd / maxUsd);
        parts.push(
          `<circle cx="${x(e.ts).toFixed(1)}" cy="${y(e.price).toFixed(1)}" r="${r.toFixed(1)}" fill="${SIDE_COLORS[e.side] ?? "#8b949e"}" opacity="0.55"/>`,
        );
      }
      // volume-spike flags
      for (const v of vols) {
        parts.push(
          `<path d="M ${x(v.ts).toFixed(1)} ${padT + 2} l 5 8 l -10 0 z" fill="#d29922" opacity="0.9"/>`,
        );
      }
      svg = `<svg viewBox="0 0 ${W} ${H}" class="chart">${parts.join("")}</svg>`;
    } else {
      svg = `<div class="chart empty">No 1m candles recorded for this window${wsConnected ? "" : " (gateway offline)"}.</div>`;
    }

    // hourly liquidation USD bars (stacked: shorts up, longs down from a mid axis)
    const HOURS = 24;
    const hourLiq = new Array(HOURS).fill(0).map(() => ({ long: 0, short: 0 }));
    for (const e of liqs) {
      const h = Math.floor((e.ts + TZ_OFFSET_MIN * 60_000) / 3600_000) % 24;
      if (e.side === "long") hourLiq[h]!.long += e.usd;
      else hourLiq[h]!.short += e.usd;
    }
    const hourMax = Math.max(1, ...hourLiq.map((h) => Math.max(h.long, h.short)));
    const HW = 880, HH = 96;
    const bw = HW / HOURS;
    const bars = hourLiq
      .map((h, i) => {
        const mid = HH / 2;
        const up = (h.short / hourMax) * (HH / 2 - 8);
        const dn = (h.long / hourMax) * (HH / 2 - 8);
        return (
          `<rect x="${(i * bw + bw * 0.18).toFixed(1)}" y="${(mid - up).toFixed(1)}" width="${(bw * 0.64).toFixed(1)}" height="${Math.max(0.5, up).toFixed(1)}" fill="#26a69a" opacity="0.8"/>` +
          `<rect x="${(i * bw + bw * 0.18).toFixed(1)}" y="${mid.toFixed(1)}" width="${(bw * 0.64).toFixed(1)}" height="${Math.max(0.5, dn).toFixed(1)}" fill="#ef5350" opacity="0.8"/>` +
          (i % 3 === 0
            ? `<text x="${(i * bw + bw / 2).toFixed(1)}" y="${HH - 2}" fill="#6e7681" font-size="9" text-anchor="middle">${String(i).padStart(2, "0")}</text>`
            : "")
        );
      })
      .join("");

    const link = (e: MarketEvent) => {
      const pack = packForEvent(sym, e.ts);
      if (!pack) return `<span class="dead">recording pruned</span>`;
      const attrs =
        `data-pack="/packs/${esc(pack.file)}${esc(xtpSuffix)}" data-sym="${esc(sym)}" data-t="${e.ts - 45_000}"`;
      return `<a href="#" ${attrs} class="open">open in terminal &#9656;</a>`;
    };

    const rows = (list: MarketEvent[], cols: (e: MarketEvent) => string) =>
      list
        .map(
          (e) =>
            `<tr><td class="t">${hmsLocal(e.ts)}</td><td class="t">${e.end ? hmsLocal(e.end) : "-"}</td>` +
            `<td style="color:${SIDE_COLORS[e.side] ?? "#e8eaed"}">${esc(e.side)}</td>` +
            `<td>${cols(e)}</td><td class="r">${fmtUsd(e.usd)}</td><td>${link(e)}</td></tr>`,
        )
        .join("");

    const cascadeTable = cascades.length
      ? `<table><tr><th>start</th><th>end</th><th>side</th><th>chain</th><th class="r">usd</th><th></th></tr>${rows(
          cascades.sort((a, b) => b.usd - a.usd).slice(0, 12),
          (e) => `${e.count ?? 0} liqs / ${((e.end ?? e.ts) - e.ts) / 1000}s`,
        )}</table>`
      : `<p class="quiet">No cascade chains (≥3 same-side liquidations within 2s) fired this day.</p>`;

    const liqTable = liqs.length
      ? `<table><tr><th>time</th><th>end</th><th>side</th><th>fill</th><th class="r">usd</th><th></th></tr>${rows(
          liqs.sort((a, b) => b.usd - a.usd).slice(0, 12),
          (e) => esc(e.detail),
        )}</table>`
      : `<p class="quiet">No liquidations recorded this day.</p>`;

    const tradeTable = trades.length
      ? `<table><tr><th>time</th><th>end</th><th>taker</th><th>fill</th><th class="r">usd</th><th></th></tr>${rows(
          trades.sort((a, b) => b.usd - a.usd).slice(0, 12),
          (e) => esc(e.detail),
        )}</table>`
      : `<p class="quiet">No single fills ≥ ${fmtUsd(BIGTRADE_USD)} this day.</p>`;

    const volTable = vols.length
      ? `<table><tr><th>time</th><th>end</th><th>side</th><th>minute</th><th class="r">usd</th><th></th></tr>${rows(
          vols.sort((a, b) => b.usd - a.usd).slice(0, 12),
          (e) => esc(e.detail),
        )}</table>`
      : `<p class="quiet">No volume spikes (≥${VOL_SPIKE_X}x the 1h average) this day.</p>`;

    const first = candles[0], last = candles[candles.length - 1];
    const net = first && last ? ((last.c / first.o - 1) * 100).toFixed(2) + "%" : "-";
    blocks.push(`
<section>
  <h2>${esc(sym.toUpperCase())}/USDT <span class="sub">${dateStr} (local UTC+${(TZ_OFFSET_MIN / 60).toFixed(2).replace(/\.00$/, "")})</span></h2>
  <div class="kpis">
    ${kpi("Open", first ? first.o.toLocaleString("en-US") : "-")}
    ${kpi("High", candles.length ? Math.max(...candles.map((c) => c.h)).toLocaleString("en-US") : "-")}
    ${kpi("Low", candles.length ? Math.min(...candles.map((c) => c.l)).toLocaleString("en-US") : "-")}
    ${kpi("Close", last ? last.c.toLocaleString("en-US") : "-")}
    ${kpi("Net", net, Number(net) >= 0 ? "#26a69a" : "#ef5350")}
    ${kpi("Liq · longs", fmtUsd(liqLong), "#ef5350")}
    ${kpi("Liq · shorts", fmtUsd(liqShort), "#26a69a")}
    ${kpi("Cascades", String(cascades.length), cascades.length ? "#d29922" : undefined)}
    ${kpi("Biggest cascade", bigCascade ? fmtUsd(bigCascade.usd) : "-", "#d29922")}
    ${kpi("Fills ≥ 250K", String(trades.length))}
    ${kpi("Biggest fill", bigTrade ? fmtUsd(bigTrade.usd) : "-")}
    ${kpi("Vol spikes", String(vols.length), vols.length ? "#d29922" : undefined)}
  </div>
  ${svg}
  <div class="chart-caption">1m closes · bubbles = liquidations (size ∝ USD) · amber bands = cascades · ▲ = volume spikes · right axis = price</div>
  <svg viewBox="0 0 ${HW} ${HH}" class="chart small">${bars}</svg>
  <div class="chart-caption">Liquidated USD per hour (local) · green = shorts (forced buy) · red = longs (forced sell) · peak hour ${fmtUsd(hourMax)}</div>
  <h3>Cascade chains</h3>${cascadeTable}
  <h3>Largest liquidations</h3>${liqTable}
  <h3>Largest single fills</h3>${tradeTable}
  <h3>Volume spikes</h3>${volTable}
</section>`);
  }

  // Browser snapshots captured by the landing page bridge (Phase 1 snapshot API).
  const snapDir = join(SNAPSHOTS_DIR, dateStr);
  let snaps = "";
  try {
    const files = readdirSync(snapDir)
      .filter((f) => f.endsWith(".jpg"))
      .sort()
      .slice(-KEEP_SNAPSHOTS_PER_DAY);
    if (files.length)
      snaps = `<h3>Terminal snapshots</h3><div class="snaps">${files
        .map((f) => {
          const name = f.replace(/\.jpg$/, "");
          const time = name.slice(0, 5).replace("-", ":");
          return `<figure><img src="/snapshots/${dateStr}/${esc(f)}${esc(xtpSuffix)}" alt="terminal snapshot ${esc(time)}" loading="lazy"/><figcaption>${esc(time)} local</figcaption></figure>`;
        })
        .join("")}</div>`;
  } catch {
    // no snapshots this day
  }

  return `<!doctype html><html lang="en"><head><meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>EdgeDepth daily report · ${esc(dateStr)}</title>
<style>
:root{color-scheme:dark}
body{background:#0d1117;color:#e8eaed;font:14px/1.5 ui-sans-serif,system-ui,"Segoe UI",sans-serif;margin:0;padding:28px 20px 60px}
main{max-width:940px;margin:0 auto}
h1{font-size:22px;margin:0 0 4px}h2{font-size:17px;margin:26px 0 10px}h3{font-size:13px;text-transform:uppercase;letter-spacing:.08em;color:#8b949e;margin:22px 0 8px}
.sub{font-weight:400;color:#8b949e;font-size:13px;margin-left:8px}
.meta{color:#8b949e;font-size:12px;margin-bottom:18px}
.kpis{display:grid;grid-template-columns:repeat(auto-fill,minmax(128px,1fr));gap:8px;margin:12px 0 14px}
.kpi{background:#161b22;border:1px solid #21262d;border-radius:8px;padding:8px 10px}
.kpi-l{font-size:10px;text-transform:uppercase;letter-spacing:.06em;color:#8b949e}
.kpi-v{font-size:14px;font-weight:600;margin-top:2px;font-variant-numeric:tabular-nums}
svg.chart{width:100%;background:#0d1117;border:1px solid #21262d;border-radius:8px;display:block}
svg.chart.small{margin-top:8px}
.chart.empty{padding:40px;text-align:center;color:#6e7681}
.chart-caption{color:#6e7681;font-size:11px;margin:6px 2px 0}
table{width:100%;border-collapse:collapse;font-size:12.5px;font-variant-numeric:tabular-nums}
th{color:#8b949e;text-align:left;font-weight:500;font-size:11px;text-transform:uppercase;letter-spacing:.05em;padding:4px 8px;border-bottom:1px solid #21262d}
td{padding:5px 8px;border-bottom:1px solid #161b22}
td.r,th.r{text-align:right}
td.t{color:#9aa4b2;white-space:nowrap}
.quiet{color:#6e7681;font-size:12.5px}
a.open{color:#58a6ff;text-decoration:none;font-size:12px;white-space:nowrap}
a.open:hover{text-decoration:underline}
.dead{color:#484f58;font-size:12px}
.snaps{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:10px}
.snaps figure{margin:0;background:#161b22;border:1px solid #21262d;border-radius:8px;overflow:hidden}
.snaps img{width:100%;display:block}
.snaps figcaption{font-size:11px;color:#8b949e;padding:5px 8px}
footer{margin-top:34px;color:#484f58;font-size:11.5px}
</style></head><body><main>
<h1>EdgeDepth daily report</h1>
<div class="meta">${esc(dateStr)} (local, UTC+${(TZ_OFFSET_MIN / 60).toFixed(2).replace(/\.00$/, "")}) · generated ${new Date(now).toISOString()} · recorder ${esc(SYMBOLS.join(", "))} · <b>replay links expect this page served via the site gateway</b></div>
${blocks.join("\n")}
${snaps}
<footer>Personal recorder daemon (Phase 4) · events from the 24/7 gateway capture · open the terminal with ?replayLibrary=/manifest.json?XTransformPort=<n> to browse the archive</footer>
<script>
document.addEventListener('click', function(ev){
  var a = ev.target && ev.target.closest ? ev.target.closest('a[data-pack]') : null;
  if(!a) return; ev.preventDefault();
  var pack = location.origin + a.getAttribute('data-pack');
  var url = location.origin + '/?pack=' + encodeURIComponent(pack) +
            '&packsym=' + a.getAttribute('data-sym') + '&packt=' + a.getAttribute('data-t');
  window.open(url, '_blank');
});
</script>
</main></body></html>`;
}

// NOTE: xtpSuffix is bound per-request inside reportResponse (the gateway's
// XTransformPort routing query rides along into every pack deep link).
let xtpSuffix = "";

function reportResponse(url: URL): Response {
  const q = url.searchParams;
  const xtp = q.get("XTransformPort");
  xtpSuffix = xtp ? `?XTransformPort=${encodeURIComponent(xtp)}` : "";
  const today = localDayKey(Date.now());
  const date = /^\d{4}-\d{2}-\d{2}$/.test(q.get("date") || "") ? q.get("date")! : today;
  const html = renderReportHtml(date);
  return new Response(html, {
    headers: {
      "content-type": "text/html; charset=utf-8",
      "cache-control": "no-store",
      "access-control-allow-origin": "*",
    },
  });
}

function autoGenerateReport(dateStr: string): string | null {
  try {
    mkdirp(REPORTS_DIR);
    const out = join(REPORTS_DIR, `report-${dateStr}.html`);
    writeFileSync(out, renderReportHtml(dateStr));
    log(`daily report written: ${out}`);
    return out;
  } catch (e) {
    log(`daily report generation failed: ${e}`);
    return null;
  }
}

// ── Snapshot intake (Phase 1 snapshot API → daily report gallery) ─────────────

async function snapshotResponse(req: Request, url: URL): Promise<Response> {
  const body = new Uint8Array(await req.arrayBuffer());
  if (body.length < 1024) return new Response("snapshot too small", { status: 400 });
  if (body.length > 3_000_000) return new Response("snapshot too large", { status: 413 });
  const now = Date.now();
  const day = localDayKey(now);
  const dir = join(SNAPSHOTS_DIR, day);
  mkdirp(dir);
  const d = new Date(now + TZ_OFFSET_MIN * 60_000);
  const p2 = (n: number) => String(n).padStart(2, "0");
  const sym = (url.searchParams.get("symbol") || "market").replace(/[^a-z0-9]/gi, "").toLowerCase();
  const name = `${p2(d.getUTCHours())}-${p2(d.getUTCMinutes())}-${sym}.jpg`;
  writeFileSync(join(dir, name), body);
  // keep the newest KEEP_SNAPSHOTS_PER_DAY per day
  const files = readdirSync(dir).filter((f) => f.endsWith(".jpg")).sort();
  while (files.length > KEEP_SNAPSHOTS_PER_DAY) {
    const old = files.shift()!;
    try { unlinkSync(join(dir, old)); } catch { /* best effort */ }
  }
  return new Response(null, {
    status: 204,
    headers: { "access-control-allow-origin": "*" },
  });
}

function reportsListResponse(): Response {
  let files: string[] = [];
  try {
    files = readdirSync(REPORTS_DIR).filter((f) => /^report-\d{4}-\d{2}-\d{2}\.html$/.test(f)).sort().reverse();
  } catch {
    // no reports yet
  }
  const body = JSON.stringify({
    schema: "edgedepth.reports.v1",
    tz_offset_min: TZ_OFFSET_MIN,
    today: localDayKey(Date.now()),
    reports: files.map((f) => ({
      date: f.slice(7, 17),
      url: `/report?date=${f.slice(7, 17)}`,
      size: statSync(join(REPORTS_DIR, f)).size,
    })),
    note: "Open through the site gateway (/report?XTransformPort=9002) so the terminal replay links resolve.",
  });
  return new Response(body, {
    headers: {
      "content-type": "application/json; charset=utf-8",
      "access-control-allow-origin": "*",
    },
  });
}

// ── Analytical server (Phase 6): REST /analytics + WS /analytics/ws ──────────

// WS clients; ws.data.symbols === null means "all configured symbols".
const analyticsClients = new Set<{
  data: { symbols: string[] | null };
  send: (s: string) => void;
}>();

let analyticsCacheAt = 0;
const analyticsCache = new Map<string, AnalyticsSnapshot>();

function analyticsSnapshotCached(sym: string): AnalyticsSnapshot {
  const now = Date.now();
  if (now - analyticsCacheAt > 250 || !analyticsCache.has(sym)) {
    analyticsCacheAt = now;
    analyticsCache.set(sym, analytics.for(sym).snapshot(ANALYTICS_DEPTH_BPS, ANALYTICS_BUCKETS));
  }
  return analyticsCache.get(sym)!;
}

// One shared build per tick; each client gets its subscribed slice.
setInterval(() => {
  if (!analyticsClients.size) return;
  const now = Date.now();
  for (const ws of analyticsClients) {
    try {
      const wanted = ws.data.symbols && ws.data.symbols.length ? ws.data.symbols : SYMBOLS;
      const data = wanted.map((s) => analyticsSnapshotCached(s.toLowerCase()));
      ws.send(JSON.stringify({ type: "analytics", schema: "edgedepth.analytics.v1", ts_ms: now, data }));
    } catch {
      analyticsClients.delete(ws);
    }
  }
}, ANALYTICS_PUSH_MS);

function analyticsResponse(url: URL): Response {
  const sym = (url.searchParams.get("symbol") || "").trim().toLowerCase();
  const depthBps = Number(url.searchParams.get("depth_bps")) || ANALYTICS_DEPTH_BPS;
  const buckets = Number(url.searchParams.get("buckets")) || ANALYTICS_BUCKETS;
  const wanted = sym ? [sym] : SYMBOLS;
  const body = JSON.stringify({
    schema: "edgedepth.analytics.v1",
    ts_ms: Date.now(),
    exchange: EXCHANGE,
    depth_bps: depthBps,
    buckets,
    data: wanted.map((s) => analytics.for(s).snapshot(depthBps, Math.min(buckets, 25))),
    note: "Live analytical view of the normalized feed. WS stream: /analytics/ws (subscribe {\"method\":\"subscribe\",\"data\":{\"symbols\":[\"btcusdt\"]}} or null for all).",
  });
  return new Response(body, {
    headers: {
      "content-type": "application/json; charset=utf-8",
      "access-control-allow-origin": "*",
    },
  });
}

// ── History queries (Phase 6): REST /history/... over the SQLite store ────────

function historyParams(url: URL): { symbol?: string; from?: number; to?: number; limit?: number } {
  const num = (k: string) => {
    const v = Number(url.searchParams.get(k));
    return Number.isFinite(v) && v > 0 ? v : undefined;
  };
  const symbol = (url.searchParams.get("symbol") || "").trim().toLowerCase() || undefined;
  return { symbol, from: num("from"), to: num("to"), limit: num("limit") };
}

function historyJson(kind: string, rows: unknown[], note?: string): Response {
  const body = JSON.stringify({
    schema: "edgedepth.history.v1",
    kind,
    count: rows.length,
    ...(note ? { note } : {}),
    data: rows,
  });
  return new Response(body, {
    headers: {
      "content-type": "application/json; charset=utf-8",
      "access-control-allow-origin": "*",
    },
  });
}

Bun.serve({
  port: PORT,
  async fetch(req, server) {
    const url = new URL(req.url);
    try {
      if (req.method === "OPTIONS") {
        return new Response(null, {
          status: 204,
          headers: {
            "access-control-allow-origin": "*",
            "access-control-allow-methods": "POST, GET, OPTIONS",
            "access-control-allow-headers": "Content-Type",
          },
        });
      }
      if (url.pathname === "/manifest.json") return manifestJson(url);
      if (url.pathname === "/events") return eventsResponse(url);
      if (url.pathname === "/analytics") return analyticsResponse(url);
      if (url.pathname === "/analytics/ws") {
        const upgraded = server.upgrade(req, { data: { symbols: null as string[] | null } });
        if (upgraded) return undefined; // handled by the websocket handler
        return new Response("websocket upgrade failed", { status: 400 });
      }
      if (url.pathname === "/history")
        return historyJson("info", [history.stats()], "Query /history/trades|liquidations|candles|book?symbol=&from=&to=&limit= (ms epochs).");
      if (url.pathname === "/history/trades") return historyJson("trades", history.trades(historyParams(url)));
      if (url.pathname === "/history/liquidations") return historyJson("liquidations", history.liquidations(historyParams(url)));
      if (url.pathname === "/history/candles") return historyJson("candles_1m", history.candles(historyParams(url)));
      if (url.pathname === "/history/book") return historyJson("book_polls", history.bookPolls(historyParams(url)));
      if (url.pathname === "/report") return reportResponse(url);
      if (url.pathname === "/reports") return reportsListResponse();
      if (url.pathname === "/snapshot" && req.method === "POST") return snapshotResponse(req, url);
      if (url.pathname.startsWith("/snapshots/")) {
        // Daily-report gallery images: /snapshots/<local-day>/<HH-MM>-<sym>.jpg
        const rel = url.pathname.slice("/snapshots/".length);
        const safe = rel
          .split("/")
          .map((p) => basename(p))
          .join("/");
        const path = join(SNAPSHOTS_DIR, safe);
        if (!safe || !existsSync(path)) return new Response("not found", { status: 404 });
        return new Response(Bun.file(path), {
          headers: {
            "content-type": "image/jpeg",
            "cache-control": "public, max-age=86400",
            "access-control-allow-origin": "*",
          },
        });
      }
      if (url.pathname.startsWith("/packs/")) return packFileResponse(req, url);
      if (url.pathname === "/") {
        const totalBytes = [...packIndex.values()].reduce((a, m) => a + m.size, 0);
        const body = JSON.stringify(
          {
            service: "edgedepth-recorder",
            gateway: { url: GATEWAY_WS, connected: wsConnected },
            symbols: SYMBOLS.map((s) => ({
              symbol: s,
              episode_started_at: sessions.get(s)!.episode
                ? new Date(sessions.get(s)!.episode!.startedAt).toISOString()
                : null,
              frames: sessions.get(s)!.episode ? sessions.get(s)!.episode!.captured : 0,
              candles_seeded: sessions.get(s)!.episode ? sessions.get(s)!.episode!.candles.size : 0,
              has_ob_seed: sessions.get(s)!.episode ? sessions.get(s)!.episode!.obSeed !== null : false,
              last_frame_ms: sessions.get(s)!.lastFrameMs || null,
            })),
            packs: { count: packIndex.size, bytes: totalBytes, dir: PACKS_DIR },
            events: {
              indexed: eventStore.events.length,
              today: eventStore.query({ from: localDayWindow(localDayKey(Date.now())).from }).length,
              bigtrade_usd: BIGTRADE_USD,
              vol_spike_x: VOL_SPIKE_X,
              tz_offset_min: TZ_OFFSET_MIN,
            },
            daily_reports: { dir: REPORTS_DIR, endpoint: "/report?date=YYYY-MM-DD", list: "/reports" },
            snapshots: { dir: SNAPSHOTS_DIR, endpoint: "POST /snapshot" },
            analytics: {
              endpoint: "/analytics",
              ws: "/analytics/ws",
              depth_bps: ANALYTICS_DEPTH_BPS,
              buckets: ANALYTICS_BUCKETS,
              push_ms: ANALYTICS_PUSH_MS,
            },
            history: {
              db: HISTORY_DB,
              keep_days: HISTORY_DAYS,
              endpoints: ["/history/trades", "/history/liquidations", "/history/candles", "/history/book"],
              stats: history.stats(),
            },
            bad_frames: badFrames,
          },
          null,
          2,
        );
        return new Response(body, {
          headers: { "content-type": "application/json; charset=utf-8" },
        });
      }
      return new Response("not found", { status: 404 });
    } catch (e) {
      log(`http error: ${e}`);
      return new Response("internal error", { status: 500 });
    }
  },
  websocket: {
    open(ws) {
      analyticsClients.add(ws as never);
    },
    // Control plane: {"method":"subscribe","data":{"symbols":["btcusdt"]}}
    // (null / empty = every configured symbol).
    message(ws, message) {
      try {
        const m = JSON.parse(String(message));
        if (m?.method === "subscribe") {
          const raw = Array.isArray(m.data?.symbols) ? m.data.symbols : null;
          (ws.data as { symbols: string[] | null }).symbols = raw
            ? raw.map((s: unknown) => String(s).toLowerCase()).filter((s: string) => !!s)
            : null;
        }
      } catch {
        // ignore malformed control frames
      }
    },
    close(ws) {
      analyticsClients.delete(ws as never);
    },
  },
});

loadIndex();
eventStore.loadRecent();
candleStore.loadRecent();
// On boot, publish yesterday's report if the rollover never ran (restarts).
{
  const yesterdayMs = Date.now() - 86_400_000;
  const ypath = join(REPORTS_DIR, `report-${localDayKey(yesterdayMs)}.html`);
  if (!existsSync(ypath)) autoGenerateReport(localDayKey(yesterdayMs));
}
log(
  `edgedepth-recorder on :${PORT} - symbols=[${SYMBOLS.join(", ")}] episode=${EPISODE_MS / 60000}min ` +
    `blocks=${BLOCK_TARGET / 1024 / 1024}MiB keep=${KEEP_PER_SYMBOL}/symbol data=${DATA_DIR}`,
);
