// Real WASM: live -> replay -> another window -> same window -> live -> replay.
// Local synthetic socket only; run like replay_create_join_test.cjs.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const http = require("node:http");
const { chromium } = require("playwright");
const { wsServer } = require("playwright-core/lib/utilsBundle");
const root = process.argv[2],
  evidence = process.argv[3];
if (!root || !evidence) throw new Error("Usage: node replay_create_join_test.cjs <WASM build dir> <evidence dir>");
fs.mkdirSync(evidence, { recursive: true });
const start = 1791239280000,
  end = start + 60000,
  anchor = start + 20000,
  id = "create-join-fixture";
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

async function run() {
  const log = { posts: [], joins: [], errors: [] };
  const timers = new Set();
  const bySocket = new WeakMap();
  const server = http.createServer((req, res) => {
    res.setHeader("Cross-Origin-Opener-Policy", "same-origin");
    res.setHeader("Cross-Origin-Embedder-Policy", "require-corp");
    res.setHeader("Cache-Control", "no-store");
    const pathname = new URL(req.url, "http://localhost").pathname;
    if (pathname === "/replay/session") {
      let body = "";
      req.on("data", (c) => (body += c));
      req.on("end", () => {
        log.posts.push({ body: JSON.parse(body), auth: req.headers.authorization });
        res.setHeader("Content-Type", "application/json");
        res.statusCode = 201;
        res.end(JSON.stringify({ session_id: id, status: "pending" }));
      });
      return;
    }
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
  const joined = (ws, create) => {
    clearInterval(bySocket.get(ws));
    const anchor = create.anchor_timestamp;
    const start = create.start_timestamp, end = create.end_timestamp;
    json(ws, "replay_joined", { session_id: id, type: "nats", symbols: ["btcusdt"], exchange: "binancef",
      streams: ["trades", "candles", "orderbook"], start_time: start, end_time: end, speed: 1 });
    seed(ws, anchor);
    let tick = 0;
    const timer = setInterval(() => {
      if (ws.readyState === 1)
        json(ws, "replay_status", { status: "playing", current_time: anchor + Math.min(1000, ++tick * 100), progress: 0.3,
          extra: { speed: 1, depth_ready: true, depth_timestamp: anchor } });
    }, 150);
    timers.add(timer);
    bySocket.set(ws, timer);
    ws.on("close", () => { clearInterval(timer); timers.delete(timer); });
  };
  sockets.on("connection", (ws) =>
    ws.on("message", (raw) => {
      const q = JSON.parse(raw);
      if (q.method === "get_historical_candles") {
        // The first page ends where playback opens; later pages are the
        // chart's own scroll-back loads, which go further back.
        if (q.data.end_time === anchor) log.candleRequests = (log.candleRequests || 0) + 1;
        // The join primes each replay. Never inject the first replay's book
        // while answering scroll-back requests from a later replay.
        if (!log.joins.length) seed(ws, anchor);
      }
      if (q.method !== "join_replay") return;
      log.joins.push(q.data);
      assert.ok(q.data.create, "each switch uses one-message create-and-join");
      joined(ws, q.data.create);
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
      if (m.type() === "error" || /IM_ASSERT|User Error|Assertion failed/.test(m.text())) log.errors.push(m.text());
    });
    await page.addInitScript(() => {
      window.__EDGEDEPTH_TIER__ = "admin";
      window.__EDGEDEPTH_REPLAY_TOKEN__ = "fixture-token";
      window.__EDGEDEPTH_USER_EMAIL__ = "fixture@example.invalid";

      window.__usage = [];
      window.addEventListener("edgedepth:usage", (e) => window.__usage.push(e.detail));
    });
    await page.route("**/*", (route) =>
      new URL(route.request().url()).hostname === "127.0.0.1" ? route.continue()
        : route.fulfill({ contentType: "application/json", body: JSON.stringify(
          new URL(route.request().url()).pathname.includes("symbols/metadata")
            ? { symbols: [{ exchange: "binancef", symbol: "btcusdt", tick_size: 0.1, step_size: 0.001, base_asset: "BTC", quote_asset: "USDT" }] }
            : {}) }));
    let documents = 0;
    page.on("request", (r) => { if (r.isNavigationRequest() && r.frame() === page.mainFrame()) documents++; });
    await page.goto(`${base}/terminal/binancef/btcusdt?ws=${encodeURIComponent(ws)}&replay_ws=${encodeURIComponent(ws)}`);
    await page.waitForFunction(() => window.Module?.calledRun && typeof window.Module?.__hosted_replay_begin === "function", {}, { timeout: 60000 });
    await page.evaluate(() => { window.__originalModule = window.Module; window.__originalCanvas = document.getElementById("canvas"); });
    for (const offset of [0, 120000, 120000]) {
      const count = log.joins.length;
      await page.evaluate(({ start, end, anchor, offset }) => {
        delete window.__EDGEDEPTH_STUDIO_STATE__;
        window.__EDGEDEPTH_HOSTED_REPLAY__ = JSON.stringify({ symbol: "btcusdt", exchange: "binancef", realtime: false });
        if (window.Module.__hosted_replay_begin() !== 1) throw new Error("begin refused");
        window.__STUDIO_SOURCE__ = JSON.stringify({ symbol: "btcusdt", startMs: start + offset, endMs: end + offset, anchorMs: anchor + offset, tf: "5m" });
        window.Module.__studio_cmd_set_source();
      }, { start, end, anchor, offset });
      await page.waitForFunction(({ start, offset }) => window.__EDGEDEPTH_STUDIO_STATE__?.playing &&
        window.__EDGEDEPTH_STUDIO_STATE__?.startMs === start + offset, { start, offset }, { timeout: 30000 });
      assert.equal(log.joins.length, count + 1, "including an identical window: one new session per selection");
      assert.equal(log.joins.at(-1).create.anchor_timestamp, anchor + offset);
      assert.equal(await page.evaluate(() => window.Module === window.__originalModule && document.getElementById("canvas") === window.__originalCanvas), true);
    }
    await page.evaluate(() => window.Module.__hosted_replay_end());
    await page.waitForTimeout(600);
    // Re-entry after live restores the source and creates a fourth session.
    await page.evaluate(({ start, end, anchor }) => {
      delete window.__EDGEDEPTH_STUDIO_STATE__;
      window.__EDGEDEPTH_HOSTED_REPLAY__ = JSON.stringify({ symbol: "btcusdt", exchange: "binancef" });
      window.Module.__hosted_replay_begin();
      window.__STUDIO_SOURCE__ = JSON.stringify({ symbol: "btcusdt", startMs: start, endMs: end, anchorMs: anchor });
      window.Module.__studio_cmd_set_source();
    }, { start, end, anchor });
    await page.waitForFunction(() => window.__EDGEDEPTH_STUDIO_STATE__?.playing, {}, { timeout: 30000 });
    assert.equal(log.joins.length, 4);
    assert.equal(log.posts.length, 0);
    assert.equal(documents, 1, "all switches stay in the original document");
    const usage = await page.evaluate(() => window.__usage);
    assert.equal(usage.filter(e => e.event === "session_start").length, 1);
    assert.equal(usage.filter(e => e.event === "replay_start").length, 4);
    log.usage = usage;
    assert.deepEqual(log.errors, []);
    await page.screenshot({ path: path.join(evidence, "in-place.png") });
    return log;
  } finally {
    for (const timer of timers) clearInterval(timer);
    await browser.close();
    for (const c of sockets.clients) c.terminate();
    sockets.close();
    server.close();
  }
}

run().then(result => {
  fs.writeFileSync(path.join(evidence, "result.json"), JSON.stringify(result, null, 2));
  console.log("PASS in-place: four sessions, one document/module/canvas, no POST");
}).catch(e => { console.error(e); process.exitCode = 1; });
