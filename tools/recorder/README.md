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
                                        └─────────▶ /manifest.json + /packs/…  (Range, ETag, CORS)
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

Status: `curl localhost:9002/` — gateway connection, per-symbol episode state
(frames, candles seeded, order-book seed), pack count/bytes, malformed frames.

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
