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
const EPISODE_MS = Number(process.env.RECORDER_EPISODE_MS || 3_600_000); // hourly packs
const BLOCK_TARGET = 4 * 1024 * 1024; // uncompressed bytes per zstd block (EDPACK target)
const KEEP_PER_SYMBOL = Number(process.env.RECORDER_KEEP || 48); // 2 days of hourly packs
const CANDLE_TF = 60; // seconds; the gateway aggregates this from the trade feed

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
    // Window = the true extent of every recorded frame (blocks carry their own
    // exact min/max after the timestamp sort; arrival-time first/last can be
    // inside that range when the gateway delivers buffered events).
    let winFirst = this.firstTs;
    let winLast = this.lastTs;
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
  episode: Episode;
  lastFrameMs = 0;

  constructor(readonly symbol: string) {
    this.episode = new Episode(symbol, Date.now());
  }

  ingest(stream: number, tf: number, ts: number, payload: Uint8Array) {
    this.lastFrameMs = Date.now();
    const ep = this.episode;
    // Boot seeds are captured alongside the raw stream.
    if (stream === STREAM_ORDERBOOK && ep.obSeed === null) {
      try {
        const bu = BookUpdate.decode(payload);
        if (bu.snapshot) {
          ep.obSeed = payload; // verbatim marshalled BookUpdate{snapshot:true}
          ep.obSeedTs = ts;
        }
      } catch {
        // not decodable - can't be a seed anyway
      }
    } else if (stream === STREAM_CANDLES) {
      // The gateway broadcasts STREAM_CANDLES as a SINGULAR pb.Candle (its
      // hub.go documents why: Candles/Candle field numbers collide on the
      // wire, so a plural parse silently yields zeros). Keep the completed
      // ones; they become the pack's candle_seeds at rotation.
      try {
        const c = CandleMsg.decode(payload);
        if (c.final) ep.candles.set(toNum(c.timestampMs), CandleMsg.toObject(c));
      } catch {
        // candle decode issues never block recording
      }
    }
    ep.add({ ts, stream, tf, payload });
  }

  async rotate(): Promise<void> {
    const old = this.episode;
    this.episode = new Episode(this.symbol, Date.now());
    await old.close();
  }
}

const sessions = new Map<string, Session>();
for (const s of SYMBOLS) sessions.set(s, new Session(s));

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
    if (now - sess.episode.startedAt < EPISODE_MS) continue;
    rotating = true;
    try {
      await sess.rotate();
    } catch (e) {
      log(`[${sess.symbol}] rotation failed: ${e}`);
    }
    rotating = false;
  }
}, 1000);

let shuttingDown = false;
async function shutdown() {
  if (shuttingDown) return;
  shuttingDown = true;
  log("shutting down - closing episodes");
  try {
    ws?.close();
  } catch {}
  for (const sess of sessions.values()) await sess.episode.close();
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

Bun.serve({
  port: PORT,
  async fetch(req) {
    const url = new URL(req.url);
    try {
      if (url.pathname === "/manifest.json") return manifestJson(url);
      if (url.pathname.startsWith("/packs/")) return packFileResponse(req, url);
      if (url.pathname === "/") {
        const totalBytes = [...packIndex.values()].reduce((a, m) => a + m.size, 0);
        const body = JSON.stringify(
          {
            service: "edgedepth-recorder",
            gateway: { url: GATEWAY_WS, connected: wsConnected },
            symbols: SYMBOLS.map((s) => ({
              symbol: s,
              episode_started_at: new Date(sessions.get(s)!.episode.startedAt).toISOString(),
              frames: sessions.get(s)!.episode.captured,
              candles_seeded: sessions.get(s)!.episode.candles.size,
              has_ob_seed: sessions.get(s)!.episode.obSeed !== null,
              last_frame_ms: sessions.get(s)!.lastFrameMs || null,
            })),
            packs: { count: packIndex.size, bytes: totalBytes, dir: PACKS_DIR },
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
});

loadIndex();
log(
  `edgedepth-recorder on :${PORT} - symbols=[${SYMBOLS.join(", ")}] episode=${EPISODE_MS / 60000}min ` +
    `blocks=${BLOCK_TARGET / 1024 / 1024}MiB keep=${KEEP_PER_SYMBOL}/symbol data=${DATA_DIR}`,
);
