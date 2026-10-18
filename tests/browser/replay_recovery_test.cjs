// Actual WASM regression; synthetic data and a local WebSocket only.
// NODE_PATH=<Playwright modules> node tests/browser/replay_recovery_test.cjs <build directory> <evidence directory>
// Exercises a timed-out archive seek, recovery buttons, and a missing session.
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
const start = 1791398700000, end = start + 48 * 3600000,
  target = start + (31 * 60 + 19) * 60000, id = "archive-recovery-fixture";
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
    num(11, 60),
    num(12, 1),
  );
let creates = 0,
  replaySocket,
  requests = [],
  statusTimer;
const server = http.createServer((req, res) => {
  res.setHeader("Cross-Origin-Opener-Policy", "same-origin");
  res.setHeader("Cross-Origin-Embedder-Policy", "require-corp");
  res.setHeader("Cache-Control", "no-store");
  const pathname = new URL(req.url, "http://localhost").pathname;
  if (pathname === "/replay/session/archive") {
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
  sendCandles(ws, at);
  ws.send(
    envelope(
      1,
      0,
      cat(dbl(1, 100.5), dbl(2, 2), num(3, 1), num(4, at), num(5, 99)),
      at,
    ),
  );
}
function sendCandles(ws, at) {
  const candles = cat(
    num(1, 60),
    ...[10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0].map((i) =>
      bytes(2, candle(Math.floor(at / 60000) * 60000 - i * 60000, 100 + i)),
    ),
  );
  ws.send(envelope(8, 60, candles, at));
  ws.send(envelope(2, 60, candle(Math.floor(at / 60000) * 60000), at));
}
let stallSeek = false, rejectSession = false, currentPosition = start;
function play(ws, at) {
  seed(ws, at);
  clearInterval(statusTimer);
  statusTimer = setInterval(() => {
    if (ws.readyState === 1) json(ws, "replay_status", {
      status: "playing", current_time: at,
      extra: { speed: 1, depth_ready: true, depth_timestamp: at },
    });
  }, 150);
}
sockets.on("connection", ws => ws.on("message", raw => {
  const q = JSON.parse(raw);
  requests.push(q);
  if (q.method === "join_replay") {
    replaySocket = ws;
    json(ws, "replay_joined", {
      session_id: id, type: "archive", symbols: ["btcusdt"], exchange: "binancef",
      streams: ["trades", "candles", "orderbook"], start_time: start, end_time: end, speed: 1,
    });
    play(ws, start);
  }
  if (!stallSeek && (q.method === "get_historical_candles" || q.method === "get_candles_before")) seed(ws, currentPosition);
  if (q.method === "control_replay" && q.data.action === "seek") {
    currentPosition = q.data.timestamp;
    clearInterval(statusTimer);
    json(ws, "replay_seeked", { timestamp: q.data.timestamp });
    if (!stallSeek) play(ws, q.data.timestamp);
    else statusTimer = setInterval(() => {
      if (ws.readyState === 1) json(ws, "replay_status", {
        status: "buffering", current_time: q.data.timestamp,
        extra: { speed: 1, depth_ready: false },
      });
    }, 150);
  }
}));
(async () => {
  await new Promise(r => server.listen(0, "127.0.0.1", r));
  const base = `http://127.0.0.1:${server.address().port}`;
  const ws = base.replace("http", "ws") + "/ws";
  const browser = await chromium.launch({ headless: true,
    executablePath: process.env.CHROME_BIN || "/usr/bin/google-chrome",
    args: ["--no-sandbox", "--enable-unsafe-swiftshader"],
  });
  const page = await browser.newPage({ viewport: { width: 1280, height: 800 }, timezoneId: "Pacific/Auckland" });
  const errors = [], consoleMessages = [];
  page.on("pageerror", e => errors.push(String(e)));
  page.on("console", m => consoleMessages.push(m.text()));
  try {
    await page.addInitScript(({start, end, id}) => {
      window.__EDGEDEPTH_TIER__ = "free";
      window.__EDGEDEPTH_REPLAY_TOKEN__ = "fixture-only";
      window.__EDGEDEPTH_USER_EMAIL__ = "fixture@example.invalid";
      window.__EDGEDEPTH_EVENT__ = {eventId:id, symbol:"btcusdt", startMs:start, endMs:end, seekToStart:false};
    }, {start,end,id});
    await page.route("**/*", route => {
      const url = new URL(route.request().url());
      if (url.pathname === "/replay/session/archive") {
        if (rejectSession) return route.fulfill({status:503,contentType:"application/json",body:JSON.stringify({error:"Replay session unavailable"})});
        creates++;
        return route.fulfill({contentType:"application/json",body:JSON.stringify({session_id:id,status:"pending"})});
      }
      if (url.hostname === "127.0.0.1") return route.continue();
      return route.fulfill({contentType:"application/json", body:JSON.stringify(url.pathname.includes("symbols/metadata")
        ? {symbols:[{exchange:"binancef",symbol:"btcusdt",tick_size:0.1,step_size:0.001,base_asset:"BTC",quote_asset:"USDT"}]} : {})});
    });
    await page.routeWebSocket("**", route => {
      if (new URL(route.url()).hostname !== "127.0.0.1") return route.close();
      route.connectToServer();
    });
    await page.goto(`${base}/terminal/binancef/btcusdt?ws=${encodeURIComponent(ws)}&replay_ws=${encodeURIComponent(ws)}`);
    const playing = () => page.waitForFunction(() => window.__EDGEDEPTH_EVENT_STATE__?.playing, {}, {timeout:30000});
    const errored = () => page.waitForFunction(() => window.__EDGEDEPTH_EVENT_STATE__?.errored, {}, {timeout:30000});
    await playing();
    stallSeek = true;
    await page.evaluate(t => window.Module.__event_cmd_seek_ms(Math.floor(t / 4294967296), t >>> 0, 1), target);
    await errored();
    const failed = await page.evaluate(() => window.__EDGEDEPTH_EVENT_STATE__);
    assert.equal(failed.clockMs, target);
    assert.match(failed.error, /did not become ready within 20 seconds/);
    assert.equal(failed.active, true, "failed replay must retain its historical context");
    await page.screenshot({path:path.join(evidence,"unavailable-desktop.png")});
    stallSeek = false;
    // ImGui renders on canvas. These are the two button centres at this viewport.
    await page.mouse.click(520, 448);
    await playing();
    const seeks = () => requests.filter(q => q.method === "control_replay" && q.data.action === "seek");
    assert.equal(seeks().at(-1).data.timestamp, target, "Retry must re-seed the failed position");
    assert.equal(creates, 1, "Retry should reuse the joined archive session");
    clearInterval(statusTimer);
    json(replaySocket, "error", {error:"Replay depth unavailable: synthetic sequence gap"});
    await errored();
    await page.mouse.click(760, 448);
    await playing();
    assert.equal(seeks().at(-1).data.timestamp, start, "Play from start must seek to the archive start");
    await page.screenshot({path:path.join(evidence,"recovered.png")});
    clearInterval(statusTimer);
    json(replaySocket, "error", {error:"Replay depth unavailable: synthetic sequence gap"});
    await errored();
    await page.setViewportSize({width:390,height:700});
    await page.waitForTimeout(300);
    await page.screenshot({path:path.join(evidence,"unavailable-narrow.png")});
    rejectSession = true;
    await page.reload();
    await errored();
    await page.screenshot({path:path.join(evidence,"session-unavailable.png")});
    assert.deepEqual(errors, []);
    assert.ok(!requests.some(q => q.method === "control_replay" && q.data.action === "stop"), "No recovery action may fall back to live");
    fs.writeFileSync(path.join(evidence,"result.json"),JSON.stringify({failed,creates,commands:seeks(),errors},null,2));
    console.log("PASS: archive timeout; same-position retry; restart; narrow layout; missing session; free tier");
  } finally {
    fs.writeFileSync(path.join(evidence,"console.log"),consoleMessages.join("\n"));
    fs.writeFileSync(path.join(evidence,"last-state.json"),JSON.stringify({state:await page.evaluate(()=>window.__EDGEDEPTH_EVENT_STATE__),requests,errors},null,2));
    await page.screenshot({path:path.join(evidence,"last-frame.png")}).catch(()=>{});
    await browser.close();
    clearInterval(statusTimer);
    for (const socket of sockets.clients) socket.terminate();
    sockets.close();
    server.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
