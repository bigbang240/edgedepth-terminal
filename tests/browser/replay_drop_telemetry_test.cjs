// Actual WASM regression; synthetic data and a local WebSocket only.
// NODE_PATH=<Playwright modules> node tests/browser/replay_drop_telemetry_test.cjs <build directory> <evidence directory>
//
// A replay whose socket drops records why (hub task replay-socket-drops): the
// replay_reconnect "lost" event carries the browser's close code, wasClean and
// reason, how long the socket was open, the last frame's age and the tab's
// visibility, then the replay resumes. Two drops on one page:
//   unclean  the box's TCP connection is destroyed: 1006, wasClean false
//   clean    the box closes with 4000 "fixture drop": wasClean true, reason kept
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const http = require("node:http");
const { chromium } = require("playwright");
const { wsServer } = require("playwright-core/lib/utilsBundle");
const root = process.argv[2],
  evidence = process.argv[3];
if (!root || !evidence) throw new Error("Usage: node replay_drop_telemetry_test.cjs <WASM build dir> <evidence dir>");
fs.mkdirSync(evidence, { recursive: true });
const start = 1791239280000,
  end = start + 60000,
  anchor = start + 20000,
  id = "drop-telemetry-fixture";
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
const envelope = (stream, tf, b, t) => cat(bytes(1, pair), num(2, stream), num(3, tf), bytes(4, b), num(5, t));
const candle = (t, p = 100) =>
  cat(dbl(1, p), dbl(2, p + 1), dbl(3, p - 1), dbl(4, p + 0.5), dbl(5, 10), num(10, t), num(11, 300), num(12, 1));
function json(ws, type, data) {
  ws.send(JSON.stringify(data === undefined ? type : { type, data }));
}
function seed(ws, at) {
  const level = (price, qty) => cat(dbl(1, price), dbl(2, qty));
  ws.send(envelope(3, 0, cat(num(1, at), num(4, 1), dbl(5, 100.5), num(7, at),
    bytes(2, level(101, 3)), bytes(2, level(102, 8)), bytes(3, level(100, 4)), bytes(3, level(99, 7))), at));
  const candles = cat(num(1, 300), ...[10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0].map((i) =>
    bytes(2, candle(Math.floor(at / 300000) * 300000 - i * 300000, 100 + i))));
  ws.send(envelope(8, 300, candles, at));
  ws.send(envelope(2, 300, candle(Math.floor(at / 300000) * 300000), at));
  ws.send(envelope(1, 0, cat(dbl(1, 100.5), dbl(2, 2), num(3, 1), num(4, at), num(5, 99)), at));
}

(async () => {
  const log = { joins: 0, errors: [] };
  let statusTimer, replaySocket;
  const server = http.createServer((req, res) => {
    res.setHeader("Cross-Origin-Opener-Policy", "same-origin");
    res.setHeader("Cross-Origin-Embedder-Policy", "require-corp");
    res.setHeader("Cache-Control", "no-store");
    const pathname = new URL(req.url, "http://localhost").pathname;
    let file = path.join(root, path.basename(pathname));
    if (pathname === "/" || (pathname.startsWith("/terminal/") && !path.extname(pathname))) file = path.join(root, "index.html");
    if (!fs.existsSync(file) || fs.statSync(file).isDirectory()) {
      res.setHeader("Content-Type", "application/json");
      return res.end("{}");
    }
    res.setHeader("Content-Type", file.endsWith(".html") ? "text/html" : file.endsWith(".js") ? "text/javascript"
      : file.endsWith(".wasm") ? "application/wasm" : "application/octet-stream");
    fs.createReadStream(file).pipe(res);
  });
  const sockets = new wsServer({ server });
  sockets.on("connection", (ws) =>
    ws.on("message", (raw) => {
      const q = JSON.parse(raw);
      if (q.method === "get_historical_candles") seed(ws, anchor);
      if (q.method !== "join_replay" || !q.data.create) return;
      log.joins++;
      replaySocket = ws;
      clearInterval(statusTimer);
      json(ws, "replay_joined", { session_id: id, type: "nats", symbols: ["btcusdt"], exchange: "binancef",
        streams: ["trades", "candles", "orderbook"], start_time: start, end_time: end, speed: 1 });
      seed(ws, anchor);
      let tick = 0;
      statusTimer = setInterval(() => {
        if (ws.readyState === 1)
          json(ws, "replay_status", { status: "playing", current_time: anchor + Math.min(1000, ++tick * 100), progress: 0.3,
            extra: { speed: 1, depth_ready: true, depth_timestamp: anchor } });
      }, 150);
    }),
  );
  await new Promise((r) => server.listen(0, "127.0.0.1", r));
  const base = `http://127.0.0.1:${server.address().port}`,
    ws = base.replace("http", "ws") + "/ws";
  const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME_BIN || "/usr/bin/google-chrome",
    args: ["--no-sandbox", "--enable-unsafe-swiftshader"] });
  try {
    const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
    page.on("pageerror", (e) => log.errors.push(e.message));
    page.on("console", (m) => {
      if (m.text() === "Replay lane socket connected") return;
      // The browser logs every dropped WebSocket; the drops here are deliberate.
      if (/WebSocket connection to .* failed/.test(m.text())) return;
      if (m.type() === "error" || /IM_ASSERT|User Error|Assertion failed/.test(m.text())) log.errors.push(m.text());
    });
    await page.addInitScript(() => {
      window.__EDGEDEPTH_TIER__ = "admin";
      window.__EDGEDEPTH_REPLAY_TOKEN__ = "fixture-token";
      window.__EDGEDEPTH_USER_EMAIL__ = "fixture@example.invalid";
      window.__EDGEDEPTH_LESSON__ = { mode: "studio", symbol: "btcusdt", exchange: "binancef", chrome: "research" };
      window.__usage = [];
      window.addEventListener("edgedepth:usage", (e) => window.__usage.push(e.detail));
    });
    await page.route("**/*", (route) =>
      new URL(route.request().url()).hostname === "127.0.0.1" ? route.continue()
        : route.fulfill({ contentType: "application/json", body: JSON.stringify(
          new URL(route.request().url()).pathname.includes("symbols/metadata")
            ? { symbols: [{ exchange: "binancef", symbol: "btcusdt", tick_size: 0.1, step_size: 0.001, base_asset: "BTC", quote_asset: "USDT" }] }
            : {}) }));
    await page.goto(`${base}/terminal/binancef/btcusdt?ws=${encodeURIComponent(ws)}&replay_ws=${encodeURIComponent(ws)}`);
    await page.waitForFunction(() => window.Module?.calledRun && typeof window.Module?.__studio_cmd_set_source === "function", {}, { timeout: 60000 });
    await page.evaluate(({ start, end, anchor }) => {
      window.__STUDIO_SOURCE__ = JSON.stringify({ symbol: "btcusdt", exchange: "binancef", startMs: start, endMs: end, anchorMs: anchor, tf: "5m" });
      window.Module.__studio_cmd_set_source();
    }, { start, end, anchor });
    await page.waitForFunction(() => window.__EDGEDEPTH_STUDIO_STATE__?.playing, {}, { timeout: 30000 });
    const reconnects = () => page.evaluate(() => window.__usage.filter((e) => e.event === "replay_reconnect").map((e) => e.props));
    const waitPhase = (phase, n) => page.waitForFunction(({ phase, n }) =>
      window.__usage.filter((e) => e.event === "replay_reconnect" && e.props.phase === phase).length >= n, { phase, n }, { timeout: 30000 });

    await page.waitForTimeout(1500);   // the socket has been open a while and frames have arrived
    replaySocket.terminate();
    await waitPhase("lost", 1);
    await waitPhase("resumed", 1);
    const unclean = (await reconnects()).find((p) => p.phase === "lost");
    assert.equal(unclean.measurement_version, 2);
    assert.equal(unclean.close_code, 1006, "a destroyed connection reads 1006");
    assert.equal(unclean.close_clean, false);
    assert.equal(unclean.close_reason, "");
    assert.ok(unclean.socket_open_s >= 1, `socket_open_s ${unclean.socket_open_s}`);
    assert.ok(unclean.last_frame_age_ms >= 0 && unclean.last_frame_age_ms < 5000, `last_frame_age_ms ${unclean.last_frame_age_ms}`);
    assert.equal(unclean.hidden, false);
    assert.equal(log.joins, 2, "the replay resumed with a fresh create-and-join");

    await page.waitForTimeout(1500);
    replaySocket.close(4000, "fixture drop");
    await waitPhase("lost", 2);
    await waitPhase("resumed", 2);
    const clean = (await reconnects()).filter((p) => p.phase === "lost")[1];
    assert.equal(clean.close_code, 4000);
    assert.equal(clean.close_clean, true);
    assert.equal(clean.close_reason, "fixture drop");
    for (const p of (await reconnects()).filter((p) => p.phase !== "lost"))
      assert.equal(p.close_code, undefined, `close details ride the lost event only (${p.phase})`);
    assert.deepEqual(log.errors, []);
    await page.screenshot({ path: path.join(evidence, "resumed.png") });
    fs.writeFileSync(path.join(evidence, "result.json"), JSON.stringify({ reconnects: await reconnects(), joins: log.joins }, null, 2));
    console.log("PASS: unclean drop 1006 and clean close 4000 recorded on the lost event; replay resumed both times");
  } finally {
    clearInterval(statusTimer);
    await browser.close();
    for (const c of sockets.clients) c.terminate();
    sockets.close();
    server.close();
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
