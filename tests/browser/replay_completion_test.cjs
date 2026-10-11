// Actual WASM regression; synthetic data and a local WebSocket only.
// NODE_PATH=<Playwright modules> node tests/browser/replay_completion_test.cjs <build directory> <evidence directory>
// EXPECT_LEGACY=1 checks the old failure against an older asset directory.
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
  id = "funnel-completion-fixture";
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
  requests = [],
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
const sockets = new wsServer({ server });
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
sockets.on("connection", (ws) =>
  ws.on("message", (raw) => {
    const q = JSON.parse(raw);
    requests.push(q);
    if (q.method === "join_replay") {
      replaySocket = ws;
      json(ws, "replay_joined", {
        session_id: id,
        type: "nats",
        symbols: ["btcusdt"],
        exchange: "binancef",
        streams: ["trades", "candles", "orderbook"],
        start_time: start,
        end_time: end,
        speed: 1,
      });
      seed(ws, start);
      let tick = 0;
      clearInterval(statusTimer);
      statusTimer = setInterval(() => {
        if (ws.readyState !== 1) return;
        json(ws, "replay_status", {
          status: "playing",
          current_time: start + Math.min(1000, ++tick * 100),
          progress: 0.1,
          extra: { speed: 1, depth_ready: true, depth_timestamp: start },
        });
      }, 150);
    }
    if (
      q.method === "get_historical_candles" ||
      q.method === "get_candles_before"
    )
      seed(ws, start);
    if (q.method === "control_replay" && q.data.action === "seek") {
      clearInterval(statusTimer);
      // A stale completion in the seek's response window must not win.
      json(ws, "replay_finished", {
        session_id: id,
        symbol: "btcusdt",
        reason: "completed",
      });
      json(ws, "replay_seeked", { timestamp: q.data.timestamp });
      seed(ws, q.data.timestamp);
      statusTimer = setInterval(() => {
        if (ws.readyState === 1)
          json(ws, "replay_status", {
            status: "playing",
            current_time: q.data.timestamp + 100,
            extra: {
              speed: 1,
              depth_ready: true,
              depth_timestamp: q.data.timestamp,
            },
          });
      }, 150);
    }
  }),
);
(async () => {
  await new Promise((r) => server.listen(0, "127.0.0.1", r));
  const port = server.address().port,
    base = `http://127.0.0.1:${port}`,
    ws = `ws://127.0.0.1:${port}/ws`;
  const browser = await chromium.launch({
    headless: true,
    executablePath: process.env.CHROME_BIN || "/usr/bin/google-chrome",
    args: ["--no-sandbox", "--enable-unsafe-swiftshader"],
  });
  try {
    const page = await browser.newPage({
      viewport: { width: 1400, height: 900 },
    });
    const errors = [];
    page.on("pageerror", (e) => {
      errors.push(e.message);
      console.log("PAGE ERROR", e.message);
    });
    page.on("console", (m) => {
      if (
        m.type() === "error" ||
        /IM_ASSERT|User Error|Assertion failed/.test(m.text())
      )
        errors.push(m.text());
    });
    await page.addInitScript(() => {
      window.__EDGEDEPTH_TIER__ = "admin";
      window.__EDGEDEPTH_REPLAY_TOKEN__ = "fixture-only";
      window.__EDGEDEPTH_LESSON__ = {
        mode: "studio",
        symbol: "btcusdt",
        exchange: "binancef",
        chrome: "research",
      };
    });
    await page.route("**/*", (route) =>
      new URL(route.request().url()).hostname === "127.0.0.1"
        ? route.continue()
        : route.fulfill({
            contentType: "application/json",
            body: JSON.stringify(
              new URL(route.request().url()).pathname.includes(
                "symbols/metadata",
              )
                ? {
                    symbols: [
                      {
                        exchange: "binancef",
                        symbol: "btcusdt",
                        tick_size: 0.1,
                        step_size: 0.001,
                        base_asset: "BTC",
                        quote_asset: "USDT",
                      },
                    ],
                  }
                : {},
            ),
          }),
    );
    await page.goto(
      `${base}/terminal/binancef/btcusdt?ws=${encodeURIComponent(ws)}&replay_ws=${encodeURIComponent(ws)}`,
    );
    await page.waitForFunction(
      () =>
        window.Module?.calledRun &&
        typeof window.Module?.__studio_cmd_set_source === "function",
      {},
      { timeout: 60000 },
    );
    await page.evaluate(
      ({ start, end }) => {
        window.__STUDIO_SOURCE__ = JSON.stringify({
          symbol: "btcusdt",
          exchange: "binancef",
          startMs: start,
          endMs: end,
          tf: "5m",
        });
        window.Module.__studio_cmd_set_source();
      },
      { start, end },
    );
    await page
      .waitForFunction(
        () => window.__EDGEDEPTH_STUDIO_STATE__?.playing,
        {},
        { timeout: 30000 },
      )
      .catch(async (e) => {
        console.log(
          "STATE",
          await page.evaluate(() => window.__EDGEDEPTH_STUDIO_STATE__),
        );
        await page.screenshot({ path: path.join(evidence, "failed.png") });
        throw e;
      });
    await page.screenshot({ path: path.join(evidence, "playing.png") });
    clearInterval(statusTimer);
    seed(replaySocket, end);
    json(replaySocket, "replay_status", {
      status: "paused",
      current_time: end,
      progress: 1,
    });
    json(replaySocket, "replay_finished", {
      session_id: id,
      symbol: "btcusdt",
      reason: "completed",
    });
    if (process.env.EXPECT_LEGACY) {
      await page.waitForTimeout(3800);
      const state = await page.evaluate(
        () => window.__EDGEDEPTH_STUDIO_STATE__,
      );
      await page.screenshot({
        path: path.join(evidence, "legacy-finished.png"),
      });
      fs.writeFileSync(
        path.join(evidence, "result.json"),
        JSON.stringify({ state, creates, errors }, null, 2),
      );
      assert.equal(state.active, false);
      assert.equal(state.clockMs, start);
      console.log(
        "Confirmed legacy completion discards recorded state and resets clock",
      );
      return;
    }
    await page.waitForFunction(
      () => window.__EDGEDEPTH_STUDIO_STATE__?.ended === true,
      {},
      { timeout: 5000 },
    );
    await page.waitForTimeout(3800);
    const ended = await page.evaluate(() => window.__EDGEDEPTH_STUDIO_STATE__);
    assert.equal(ended.active, true);
    assert.equal(ended.playing, false);
    assert.equal(ended.paused, true);
    assert.equal(ended.clockMs, end);
    assert.equal(ended.progress, 1);
    await page.screenshot({
      path: path.join(evidence, "completed-retained.png"),
    });
    await page.evaluate(() => window.Module.__studio_cmd_set_paused(0));
    await page.waitForFunction(
      () =>
        window.__EDGEDEPTH_STUDIO_STATE__?.playing &&
        !window.__EDGEDEPTH_STUDIO_STATE__?.ended,
      {},
      { timeout: 10000 },
    );
    assert.equal(
      creates,
      1,
      "Replay again must not create another daily start",
    );
    assert.ok(
      requests.some(
        (q) =>
          q.method === "control_replay" &&
          q.data.action === "seek" &&
          q.data.timestamp === start,
      ),
    );
    assert.deepEqual(errors, []);
    await page.screenshot({ path: path.join(evidence, "replayed.png") });
    fs.writeFileSync(
      path.join(evidence, "result.json"),
      JSON.stringify(
        {
          ended,
          creates,
          commands: requests.filter((q) => q.method === "control_replay"),
          errors,
        },
        null,
        2,
      ),
    );
    console.log(
      "PASS: final frame retained past 3 seconds; exact end clock; replay-again uses same session; stale completion ignored during seek",
    );
  } finally {
    clearInterval(statusTimer);
    await browser.close();
    for (const ws of sockets.clients) ws.terminate();
    sockets.close();
    server.close();
  }
})().catch((e) => {
  console.error(e);
  clearInterval(statusTimer);
  for (const ws of sockets.clients) ws.terminate();
  sockets.close();
  server.close();
  process.exitCode = 1;
});
