# EdgeDepth Terminal

[![build](https://github.com/edgedepthhq/edgedepth-terminal/actions/workflows/build.yml/badge.svg)](https://github.com/edgedepthhq/edgedepth-terminal/actions/workflows/build.yml)
[![License: AGPL-3.0](https://img.shields.io/badge/license-AGPL--3.0-blue.svg)](LICENSE)

**Open-source crypto order flow in your browser.**

See resting orders, watch trades as they happen, and replay recorded markets.
The C++/WebAssembly terminal behind [EdgeDepth](https://edgedepth.com/open-source?utm_source=github&utm_medium=oss&utm_campaign=terminal), ready to run with a local feed or your own data.

[![TUT replay in the OSS terminal: depth heatmap, trade bubbles and attached order-book ladder](assets/oss-realtime-20261001.gif)](https://github.com/edgedepthhq/edgedepth-terminal/raw/refs/heads/master/assets/oss-realtime-20261001.mp4)

*Actual OSS build, captured 1 October 2026. Public TUT recording from 9 August 2026, replayed locally without an account. [Download full-quality MP4](https://github.com/edgedepthhq/edgedepth-terminal/raw/refs/heads/master/assets/oss-realtime-20261001.mp4) · [Full-size screenshot](assets/oss-realtime-20261001.png)*

## Run it locally

Install **Git** and **Docker with Compose** (Docker Desktop on Windows/macOS), then:

```bash
git clone https://github.com/edgedepthhq/edgedepth-terminal.git
cd edgedepth-terminal
docker compose up
```

Open **[localhost:8080](http://localhost:8080)**. This starts the terminal and the
[community gateway](https://github.com/edgedepthhq/edgedepth-gateway) on Binance's public data. No account or API key required.

- **Live:** a fresh workspace opens in **Real-time depth**. Trade bubbles start off; enable them in **Layers** when wanted. Saved workspaces keep your choices.
- **Replay:** choose **Replay → Replay Library** and pick a free recording. It plays locally, without a live feed.

The gateway connects from your machine to Binance's public futures endpoints, and
Binance may be unavailable in some regions. If the live view stays empty, use the Replay
Library (its recordings come from `replays.edgedepth.com`, not Binance) or the no-install
replay below.

Use Chrome or Firefox; Safari is unverified. [Setup, troubleshooting and source builds](docs/SETUP.md).
Prefer no installation? [Play the same TUT recording in your browser](https://app.edgedepth.com/demo/20260809_071325_tutusdt_pump_4h_4h_371pct?utm_source=github&utm_medium=oss&utm_campaign=terminal):
a recorded replay served by EdgeDepth, no account needed.

## What's included

- **Charts:** candles down to 1 second, indicators, drawing tools and VWAP.
- **Order flow:** depth heatmaps, trade bubbles, an order-book ladder (DOM) and trade tape.
- **Volume analysis:** footprints showing buy/sell volume at each price, imbalances and volume profiles.
- **Replay:** pause, scrub and revisit `.edpack` recordings in the same workspace.
- **Your workspace:** dock panels, save layouts and connect your own feed.

| Depth and executed trades | Buy/sell volume inside each minute |
| --- | --- |
| [![OSS real-time replay with attached DOM](assets/oss-realtime-20261001.png)](assets/oss-realtime-20261001.png) | [![OSS footprint replay showing recorded buy and sell volume](assets/oss-footprint-20261001.png)](assets/oss-footprint-20261001.png) |

*Both views use the public TUT pack. Footprints require recorded volume at each price; older context candles may have none.*

## Hosted EdgeDepth

The **terminal is open source; EdgeDepth's hosted data and research services are separate.**
Nothing in this terminal needs a subscription. Self-hosted, you run the feed: depth starts
with what you observe, packs hold only their recorded markets, and server-derived panels
such as VPIN stay empty ([coverage details](docs/DATA_SOURCES.md)). Hosted EdgeDepth adds
managed Binance, Hyperliquid and Bybit feeds, recorded tick history and research tools for
testing an idea against past cases.
[Plans and coverage](https://edgedepth.com/pricing?utm_source=github&utm_medium=oss&utm_campaign=terminal) · [Open-source overview](https://edgedepth.com/open-source?utm_source=github&utm_medium=oss&utm_campaign=terminal)

## Build on it

C++20 · WebAssembly · Dear ImGui / ImPlot · SDL3 / WebGL2.

- [Build and pin versions](docs/SETUP.md) · [Architecture](ARCHITECTURE.md) · [Contributing](CONTRIBUTING.md)
- [Bring your own data](docs/DATA_SOURCES.md#bring-your-own-data) · [CSV/Parquet walkthrough](docs/DATAFRAME_WORKFLOW.md) · [Wire protocol](protos/messages.proto)
- [Real-time depth guide](docs/REALTIME_DEPTH.md) · [Replay pack format](docs/EDPACK.md) · [Replay Library](replay-library/README.md)
- [Design tokens](design/README.md) · [Research MCP](https://github.com/edgedepthhq/edgedepth-research-mcp)

[AGPL-3.0](LICENSE) terminal · [MIT gateway](https://github.com/edgedepthhq/edgedepth-gateway) · [Third-party licenses](THIRD_PARTY_NOTICES.md).
