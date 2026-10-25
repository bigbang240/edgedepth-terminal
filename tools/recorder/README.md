# Personal recorder daemon (Phase 4)

A 24/7 market recorder for the open-source terminal: it subscribes the gateway's
WebSocket for the configured symbols, captures trades, the order book, reported
liquidations and 1-minute candles, and rolls everything into standard
[`.edpack` replay packs](../../docs/EDPACK.md) — one pack per hour, a manifest
for the built-in Replay Library, served over HTTP with `Range`/ETag so the
terminal streams packs without downloading them whole.

The hosted service sells this back as a subscription; here it is ~300 lines of
bun/TypeScript pointed at your own community gateway.

```
gateway :9001 (WS) ──▶ recorder.ts (:9002, bun) ──▶ data/packs/*.edpack
                                        ├─────────▶ /manifest.json + /packs/…  (Range, ETag, CORS)
                                        ├─────────▶ /events          (event finder feed)
                                        ├─────────▶ /report, /reports (daily HTML reports)
                                        └─────────▶ POST /snapshot   (terminal snapshot intake)
```

## Run

```bash
bun install          # protobufjs only; zstd is Bun-native (Bun.zstdCompressSync)
bun recorder.ts      # or: bun run dev
```

Requires [bun](https://bun.sh) ≥ 1.1 (uses `Bun.zstdCompressSync`,
`Bun.CryptoHasher`, `Bun.serve`, `Bun.file().slice()` for 206 responses).

### Configuration (environment)

| Variable            | Default                      | Meaning                                   |
| ------------------- | ---------------------------- | ----------------------------------------- |
| `RECORDER_PORT`     | `9002`                       | HTTP port (manifest + packs + status)     |
| `RECORDER_GATEWAY`  | `ws://127.0.0.1:9001/ws`     | Community gateway WebSocket               |
| `RECORDER_SYMBOLS`  | `btcusdt`                    | Comma-separated watchlist symbols         |
| `RECORDER_EXCHANGE` | `binancef`                   | Venue key (gateway's Binance futures)     |
| `RECORDER_EPISODE_MS` | `3600000`                  | Roll a new pack every N ms (hourly)       |
| `RECORDER_KEEP`     | `48`                         | Packs kept per symbol (48 h); older pruned|
| `RECORDER_DATA`     | `./data`                     | Pack directory                            |
| `RECORDER_BIGTRADE_USD` | `250000`                 | Single-fill notional indexed as "trade"   |
| `RECORDER_VOL_SPIKE_X` | `5`                        | 1m volume × hourly mean = a "vol" event   |
| `RECORDER_TZ_OFFSET_MIN` | `210`                    | Local-day offset for reports (Asia/Tehran)|
| `RECORDER_HISTORY_DB` | `./data/history/history.db` | SQLite history file (Phase 6)             |
| `RECORDER_HISTORY_DAYS` | `3`                      | History retention (pruned hourly)         |
| `RECORDER_DEPTH_BPS` | `25`                        | Default analytics depth window            |
| `RECORDER_DEPTH_BUCKETS` | `10`                    | Default depth-histogram bins              |
| `RECORDER_ANALYTICS_MS` | `1000`                    | WS analytics push cadence                 |
| `RECORDER_BOOK_POLL_MS` | `5000`                    | Top-of-book history poll cadence          |

Status: `curl localhost:9002/` — gateway connection, per-symbol episode state
(frames, candles seeded, order-book seed), pack count/bytes, event index size,
malformed frames, plus the analytics/history summary (Phase 6).

## Analytical server (roadmap Phase 6 — your main terminal is the consumer)

The recorder keeps the same normalized order-book state the terminal's
OrderbookManager maintains (snapshot + delta semantics, venue-agnostic -
verified on Binance futures and OKX), plus rolling trade flow and liquidation
sums, and serves it over REST and WebSocket:

- `GET /analytics?symbol=&depth_bps=25&buckets=10` — live snapshot, schema
  `edgedepth.analytics.v1`: best bid/ask, mid, spread (abs + bps), bid/ask
  notional inside the depth window, imbalance, per-bin depth histogram,
  CVD (session + 5m), 60s trade/notional rates, 5m VWAP + buy ratio,
  liquidation sums (5m/60m per side), latest 1m candle, feed ages.
- `WS /analytics/ws` — 1s push of the same snapshots for the subscribed
  symbols: `"{"method":"subscribe","data":{"symbols":["btcusdt"]}}"`
  (empty/null = every configured symbol).

```bash
curl 'localhost:9002/analytics?symbol=btcusdt'
# behind the site gateway: /analytics?XTransformPort=9002
```

## Normalized-stream history in SQLite (roadmap Phase 6)

Every trade, every liquidation, completed 1m candles and a 5s top-of-book
poll land in a WAL-mode SQLite DB (`bun:sqlite`, zero new dependencies).
Writes are batched (2 s / 500 rows) inside transactions — the WS ingest path
never blocks. Rows older than `RECORDER_HISTORY_DAYS` (default 3) are pruned
hourly and the WAL is truncated.

- `GET /history/trades?symbol=&from=&to=&limit=`
- `GET /history/liquidations?...`
- `GET /history/candles?...` (ascending)
- `GET /history/book?...` (descending)
- `GET /history` — row counts + time spans (schema `edgedepth.history.v1`)

Timestamps are ms epochs. The raw wire stays in the .edpack packs (Phase 4);
this store is the queryable, normalized view.

## Event finder (search the archive)

While recording, the daemon indexes every meaningful moment in
`data/events/events-YYYYMMDD.jsonl` (append-only, replayed into memory on
boot, capped at 50k):

- **cascade** — 3+ same-side liquidations within 2 s (the terminal's
  `cascade_detector` rules); finalized once the chain goes quiet.
- **liq** — every reported liquidation (`usd = qty × price`, side = which book
  got liquidated: `long` = forced sells, `short` = forced buys).
- **trade** — single fills over `RECORDER_BIGTRADE_USD`.
- **vol** — a final 1m candle trading at `RECORDER_VOL_SPIKE_X` × the previous
  hour's mean volume (15 min cooldown).

```bash
curl 'localhost:9002/events?kind=cascade&min_usd=500000&limit=50'
curl 'localhost:9002/events?kind=liq&symbol=btcusdt&from=1792500000000'
```

Each hit carries `pack` — the containing `.edpack` resolved at query time — so
the terminal's **Replay Library → Event finder** can play the moment: pick a
kind + dollar floor, Search, then Play on a row to open that pack and seek 45 s
before the event (rows whose recording was pruned show `pruned`). The feed URL
is derived from the library manifest's directory + query, so the gateway's
`?XTransformPort=9002` routing suffix rides along untouched.

## Daily report

At every local midnight (`RECORDER_TZ_OFFSET_MIN`, default +210 = Tehran) the
daemon writes the finished day to `data/reports/report-YYYY-MM-DD.html` and
refreshes a live "today" report every 30 minutes.

- `/report` — today, live · `/report?date=2026-10-20` — any recorded day
- `/reports` — JSON list of published reports

The self-contained dark HTML includes OHLC/liquidation KPIs, an SVG 1m price
chart with liquidation bubbles (size ∝ USD) and amber cascade bands, hourly
liquidation bars, event tables, and a **Terminal snapshots** gallery. Every row
links "open in terminal": `/?pack=<pack>&packsym=<sym>&packt=<event − 45s>`,
built at click time from the browser's origin, so the links work whenever the
report is opened through the site gateway (`/report?XTransformPort=9002`).

The snapshot gallery is fed by the landing page's bridge (Phase 1 snapshot
API): every 10 minutes it grabs one `captureStream` frame of the terminal
canvas and POSTs it to `/snapshot?symbol=…`; the newest 12 per day are kept.

## Open the archive in the terminal

The daemon's manifest uses the official replay-library schema
(`edgedepth.replay-library.v1`) with **relative** pack URLs, which the terminal
resolves against the manifest's own URL — the catalog is host-agnostic.

- Behind the site gateway (any host that proxies `?XTransformPort=9002` to the
  recorder), open the terminal as:

  ```
  ?replayLibrary=/manifest.json?XTransformPort=9002
  ```

- Or point the library straight at the daemon: `?replayLibrary=http://127.0.0.1:9002/manifest.json`
  (CORS is open; same-origin via a reverse proxy avoids it entirely).

Packs appear in **Replay → Open Replay Library** with title, duration, size and
SHA-256 (the terminal verifies large fetches against it). Single packs can also
be opened directly: `?pack=<absolute pack URL>&packsym=btcusdt`.

## Pack layout (what the terminal sees)

Exactly [`docs/EDPACK.md`](../../docs/EDPACK.md):

- `EDPK` | version `1` | `u32 LE` header length | protobuf `PackHeader`.
- `ob_seed` — the first `BookUpdate{snapshot:true}` of the episode (verbatim
  gateway bytes), so seeking reconstructs the book from the opening snapshot.
- `candle_seeds` — the episode's completed 1m candles, so the chart has history
  from the first frame of playback.
- Blocks of `[u32 LE len | PackFrame]` records, ~4 MiB uncompressed, each block
  independently zstd-compressed, streams merged by timestamp with the order
  book first on ties.
- `PackFrame.payload` is the gateway's `WSPayload.data` verbatim; `ts_ms` is
  `WSPayload.event_time_ms`.

Verify any pack the way the terminal reads it:

```bash
bun verify.ts data/packs/btcusdt-20261020T1600.edpack
```

## Notes

- The community gateway serves Binance futures public streams only; recording
  starts when the daemon connects (no backfill — the venue's history stays the
  venue's).
- Episodes rotate hourly (or on shutdown — SIGTERM closes the open episode
  cleanly, which the supervisor's restart loop also does after a crash).
- Disk discipline: `RECORDER_KEEP` prunes the oldest packs per symbol after
  each rotation.
- This directory is intentionally dependency-light (`protobufjs` only) so it
  can run next to the gateway on a small box, a NAS or a Raspberry Pi.
