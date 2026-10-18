// Refill levels in a research replay: the subscription goes out on the replay socket only once the replay
// has joined, in replay mode, and replay-mode levels draw at the replay clock. Synthetic data, local sockets.
// NODE_PATH=<Playwright modules> node tests/browser/refill_levels_replay_test.cjs <build directory> <evidence directory>
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const http = require("node:http");
const { chromium } = require("playwright");
const { wsServer } = require("playwright-core/lib/utilsBundle");
const root = process.argv[2],
  evidence = process.argv[3];
if (!root || !evidence)
  throw new Error(
    "Usage: node browser-probe.cjs <WASM build dir> <evidence dir>",
  );
fs.mkdirSync(evidence, { recursive: true });
const start = 1791239280000,
  end = start + 5000,
  id = "refill-replay-fixture";
const cat = (...b) => Buffer.concat(b),
  v = (n) => {
    n = BigInt(n);
    const a = [];
    do {
      a.push(Number(n & 127n) | (n > 127n ? 128 : 0));
      n >>= 7n;
    } while (n);
    return Buffer.from(a);
  };
const num = (i, n) => cat(v(i * 8), v(n)),
  bytes = (i, b) => {
    b = Buffer.from(b);
    return cat(v(i * 8 + 2), v(b.length), b);
  },
  dbl = (i, n) => {
    const b = Buffer.alloc(8);
    b.writeDoubleLE(n);
    return cat(v(i * 8 + 1), b);
  };
const pair = cat(bytes(1, "binancef"), bytes(2, "btcusdt"));
const envelope = (stream, tf, b, t) =>
  cat(bytes(1, pair), num(2, stream), num(3, tf), bytes(4, b), num(5, t));
const candle = (t, p = 100) =>
  cat(
    dbl(1, p),
    dbl(2, p + 1),
    dbl(3, p - 1),
    dbl(4, p + 0.5),
    dbl(5, 10),
    num(10, t),
    num(11, 300),
    num(12, 1),
  );
let creates = 0,
  replaySocket,
  joinedAt = 0,
  requests = [],
  refillTimer,
  statusTimer;
const server = http.createServer((req, res) => {
  res.setHeader("Cross-Origin-Opener-Policy", "same-origin");
  res.setHeader("Cross-Origin-Embedder-Policy", "require-corp");
  res.setHeader("Cache-Control", "no-store");
  const pathname = new URL(req.url, "http://localhost").pathname;
  if (pathname === "/replay/session") {
    creates++;
    res.setHeader("Content-Type", "application/json");
    return res.end(JSON.stringify({ session_id: id, status: "pending" }));
  }
  let file = path.join(root, path.basename(pathname));
  if (
    pathname === "/" ||
    (pathname.startsWith("/terminal/") && !path.extname(pathname))
  )
    file = path.join(root, "index.html");
  if (file.endsWith("index.html") && !fs.existsSync(file)) {
    res.setHeader("Content-Type", "text/html");
    return res.end(
      '<html><body style="margin:0;background:#0b0e13"><canvas id="canvas" style="width:100vw;height:100vh;display:block"></canvas><script>var Module={canvas:document.getElementById("canvas"),print:console.log,printErr:console.error};Module.canvas.width=innerWidth;Module.canvas.height=innerHeight;</script><script src="/index.js"></script></body></html>',
    );
  }
  if (!fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    res.setHeader("Content-Type", "application/json");
    return res.end("{}");
  }
  res.setHeader(
    "Content-Type",
    file.endsWith(".html")
      ? "text/html"
      : file.endsWith(".js")
        ? "text/javascript"
        : file.endsWith(".wasm")
          ? "application/wasm"
          : "application/octet-stream",
  );
  fs.createReadStream(file).pipe(res);
});
const sockets = new wsServer({ server, verifyClient: ({ req }) => req.url !== "/live-offline" });
function json(ws, type, data) {
  ws.send(JSON.stringify({ type, data }));
}
function seed(ws, at) {
  const level = (price, qty) => cat(dbl(1, price), dbl(2, qty));
  ws.send(
    envelope(
      3,
      0,
      cat(
        num(1, at),
        num(4, 1),
        dbl(5, 100.5),
        num(7, at),
        bytes(2, level(101, 3)),
        bytes(2, level(102, 8)),
        bytes(3, level(100, 4)),
        bytes(3, level(99, 7)),
      ),
      at,
    ),
  );
  const candles = cat(
    num(1, 300),
    ...[10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0].map((i) =>
      bytes(2, candle(Math.floor(at / 300000) * 300000 - i * 300000, 100 + i)),
    ),
  );
  ws.send(envelope(8, 300, candles, at));
  ws.send(envelope(2, 300, candle(Math.floor(at / 300000) * 300000), at));
  ws.send(
    envelope(
      1,
      0,
      cat(dbl(1, 100.5), dbl(2, 2), num(3, 1), num(4, at), num(5, 99)),
      at,
    ),
  );
}

const level = (id, side, price, started, known, status, closed, usd, shown, ratio, refills) => ({
  id, side, price, started_ms: start + started, known_ms: start + known, updated_ms: start + (closed || 700),
  closed_ms: closed ? start + closed : 0, status, executed_base: usd / price, executed_usd: usd, max_shown_base: shown,
  refilled_base: (usd / price) * 0.8, refills, shown_ratio: ratio,
  refill_ms: Array.from({ length: refills }, (_, i) => start + started + Math.round(((closed || 700) - started) * (i + 1) / (refills + 1))),
});
sockets.on("connection", (ws) =>
  ws.on("message", (raw) => {
    const q = JSON.parse(raw);
    requests.push({ ...q, replaySocket: ws === replaySocket, at: Date.now() });
    if (q.method === "join_replay") {
      replaySocket = ws;
      joinedAt = Date.now();
      json(ws, "replay_joined", {
        session_id: id, type: "nats", symbols: ["btcusdt"], exchange: "binancef",
        streams: ["trades", "candles", "orderbook"], start_time: start, end_time: end, speed: 1,
      });
      seed(ws, start);
      let tick = 0;
      clearInterval(statusTimer);
      statusTimer = setInterval(() => {
        if (ws.readyState !== 1) return;
        json(ws, "replay_status", {
          status: "playing", current_time: start + Math.min(1000, ++tick * 100), progress: 0.1,
          extra: { speed: 1, depth_ready: true, depth_timestamp: start },
        });
      }, 150);
    }
    if (q.method === "get_historical_candles" || q.method === "get_candles_before") seed(ws, start);
    if (q.method === "refill_subscribe") {
      const base = { type: "refill_levels", exchange: "binancef", symbol: "btcusdt", mode: "replay",
        version: "liquidity_response.refill.v1", reason: "" };
      ws.send(JSON.stringify({ ...base, status: "warming", as_of_ms: start, levels: [] }));
      // A late live-mode message must be ignored by a replay subscription.
      ws.send(JSON.stringify({ ...base, mode: "live", status: "observing", as_of_ms: start + 900,
        levels: [level("live-only", "bid", 99.0, -20000, -10000, "holding", 0, 5e6, 9, 3, 6)] }));
      const levels = [level("r-bid", "bid", 100.0, -20000, -10000, "holding", 0, 2.44e6, 7.7, 1.6, 7),
        level("r-ask", "ask", 101.0, -15000, -8000, "crossed", 500, 6e5, 3.0, 1.3, 4)];
      clearInterval(refillTimer);
      refillTimer = setInterval(() => {
        if (ws.readyState === 1) ws.send(JSON.stringify({ ...base, status: "observing", as_of_ms: start + 800, levels }));
      }, 500);
    }
  }),
);
(async () => {
  await new Promise((r) => server.listen(0, "127.0.0.1", r));
  const port = server.address().port, base = `http://127.0.0.1:${port}`, ws = `ws://127.0.0.1:${port}/ws`;
  const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME_BIN || "/usr/bin/google-chrome",
    args: ["--no-sandbox", "--enable-unsafe-swiftshader"] });
  try {
    const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
    const errors = [];
    page.on("pageerror", (e) => errors.push(e.message));
    page.on("console", (m) => { if (/IM_ASSERT|User Error|Assertion failed/.test(m.text())) errors.push(m.text()); });
    await page.addInitScript(() => {
      window.__EDGEDEPTH_TIER__ = "free";
      window.__EDGEDEPTH_TEST_UI__ = true;
      window.__EDGEDEPTH_REPLAY_TOKEN__ = "fixture-only";
      window.__EDGEDEPTH_USER_EMAIL__ = "fixture@example.invalid";
      window.__EDGEDEPTH_LESSON__ = { mode: "studio", symbol: "btcusdt", exchange: "binancef", chrome: "research" };
    });
    await page.route("**/*", (route) => new URL(route.request().url()).hostname === "127.0.0.1" ? route.continue()
      : route.fulfill({ contentType: "application/json", body: JSON.stringify(new URL(route.request().url()).pathname.includes("symbols/metadata")
        ? { symbols: [{ exchange: "binancef", symbol: "btcusdt", tick_size: 0.1, step_size: 0.001, base_asset: "BTC", quote_asset: "USDT" }] } : {}) }));
    await page.goto(`${base}/terminal/binancef/btcusdt?ws=${encodeURIComponent(ws)}&replay_ws=${encodeURIComponent(ws)}&realtime=1`);
    await page.waitForFunction(() => window.Module?.calledRun && typeof window.Module?.__studio_cmd_set_source === "function", {}, { timeout: 60000 });
    await page.evaluate(({ start, end }) => {
      window.__STUDIO_SOURCE__ = JSON.stringify({ symbol: "btcusdt", exchange: "binancef", startMs: start, endMs: end, tf: "5m" });
      window.Module.__studio_cmd_set_source();
    }, { start, end });
    await page.waitForFunction(() => window.__EDGEDEPTH_STUDIO_STATE__?.playing, {}, { timeout: 30000 });
    await page.waitForTimeout(2500);
    const subs = requests.filter((q) => q.method === "refill_subscribe");
    const result = { joinedAt, subs: subs.map((q) => ({ replaySocket: q.replaySocket, at: q.at, data: q.data })), errors };
    assert.equal(subs.length, 1, "one refill_subscribe for the replay market");
    assert.ok(subs[0].replaySocket, "sent on the replay socket");
    assert.ok(subs[0].at >= joinedAt, "sent after the replay joined");
    assert.deepEqual(subs[0].data.pair, { exchange: "binancef", symbol: "btcusdt" });
    assert.equal(subs[0].data.tick_size, 0.1);
    await page.screenshot({ path: path.join(evidence, "replay-marks.png") });
    const rects = await page.evaluate(() => window.__EDGEDEPTH_UI_RECTS__ || {});
    result.refillMarks = Object.keys(rects).filter((k) => k.startsWith("rt.refill."));
    // The replay bid holds at the replay clock; the live-only level never draws.
    assert.ok(result.refillMarks.some((k) => k.startsWith("rt.refill.edge.")), "the holding replay level ticks the right edge");
    fs.writeFileSync(path.join(evidence, "result.json"), JSON.stringify(result, null, 2));
    console.log(JSON.stringify(result));
    assert.deepEqual(errors, []);
    console.log("PASS: refill subscription follows the replay context");
  } finally {
    clearInterval(statusTimer); clearInterval(refillTimer);
    await browser.close();
    for (const s of sockets.clients) s.terminate();
    sockets.close(); server.close();
  }
})().catch((e) => { console.error(e); clearInterval(statusTimer); clearInterval(refillTimer);
  for (const s of sockets.clients) s.terminate(); sockets.close(); server.close(); process.exitCode = 1; });
