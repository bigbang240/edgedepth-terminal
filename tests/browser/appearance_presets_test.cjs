// Appearance presets on the actual WASM, served with serve_threaded.py. Synthetic market data only.
// NODE_PATH=<Playwright modules> node tests/browser/appearance_presets_test.cjs <base URL> [evidence directory]
// Covers: v1 -> v2 migration keeps market colors and accent; Blue / white paints the calm royal blue; each chrome
// preset recolors the chrome and is saved; Blue / orange market colors; the liquidation colormap is saved; everything
// survives a reload.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {chromium} = require('playwright');
const {wsServer, PNG} = require('playwright-core/lib/utilsBundle');

// The most common colour in a rectangle, as 0xRRGGBB.
function dominant(png, x0, y0, x1, y1) {
    const n = new Map();
    for (let y = y0; y < y1; y++) for (let x = x0; x < x1; x++) {
        const i = (y * png.width + x) * 4, c = (png.data[i] << 16) | (png.data[i + 1] << 8) | png.data[i + 2];
        n.set(c, (n.get(c) || 0) + 1);
    }
    return [...n.entries()].sort((a, b) => b[1] - a[1])[0][0];
}
// Pixels within 1 per channel of a colour.
function count(png, c) {
    let n = 0;
    for (let i = 0; i < png.data.length; i += 4)
        if (Math.abs(png.data[i] - (c >> 16)) <= 1 && Math.abs(png.data[i + 1] - ((c >> 8) & 255)) <= 1 &&
            Math.abs(png.data[i + 2] - (c & 255)) <= 1) n++;
    return n;
}
const near = (a, b) => [16, 8, 0].every(s => Math.abs(((a >> s) & 255) - ((b >> s) & 255)) <= 1);
const hex = c => '#' + c.toString(16).padStart(6, '0');

// Synthetic feed: book, trades, candle, stats and ticker for whatever the terminal subscribes to.
const cat = (...b) => Buffer.concat(b), v = n => { n = BigInt(n); const a = []; do { a.push(Number(n & 127n) | (n > 127n ? 128 : 0)); n >>= 7n; } while (n); return Buffer.from(a); };
const num = (i, n) => cat(v(i * 8), v(n)), bytes = (i, b) => { b = Buffer.from(b); return cat(v(i * 8 + 2), v(b.length), b); };
const dbl = (i, n) => { const b = Buffer.alloc(8); b.writeDoubleLE(n); return cat(v(i * 8 + 1), b); };
const envelope = (pair, stream, tf, b, t) => cat(bytes(1, cat(bytes(1, pair.exchange), bytes(2, pair.symbol))), num(2, stream), num(3, tf), bytes(4, b), num(5, t));
const server = new wsServer({port: 0}), start = Date.now();
server.on('connection', ws => {
    const subs = new Map(); let sequence = 100;
    ws.on('error', () => {});
    ws.on('message', m => { const q = JSON.parse(m); if (q.method === 'subscribe') subs.set(JSON.stringify(q.data), q.data); if (q.method === 'unsubscribe') subs.delete(JSON.stringify(q.data)); });
    const timer = setInterval(() => {
        if (ws.readyState !== 1) return;
        const t = Date.now(), p = .355 + Math.sin((t - start) / 7000) * .0012 + Math.sin((t - start) / 1100) * .0002;
        for (const d of subs.values()) {
            let b = null;
            if (d.stream === 3) { b = cat(num(1, t), num(4, 1), dbl(5, p), num(7, sequence++));
                for (let k = 1; k < 100; k++) b = cat(b, bytes(2, cat(dbl(1, Math.round((p + k * .00001) * 1e5) / 1e5), dbl(2, 100 + (k % 13 === 0 ? 4000 : 30) * k))),
                    bytes(3, cat(dbl(1, Math.round((p - k * .00001) * 1e5) / 1e5), dbl(2, 100 + (k % 17 === 0 ? 3500 : 20) * k)))); }
            if (d.stream === 1) b = cat(dbl(1, p), dbl(2, 30 + (sequence % 17) * 20), num(3, sequence % 2), num(4, t), num(5, sequence++));
            if (d.stream === 2) b = cat(dbl(1, p), dbl(2, p + .0002), dbl(3, p - .0002), dbl(4, p), dbl(5, 200), num(10, Math.floor(t / 60000) * 60000), num(11, 60));
            if (d.stream === 4) b = cat(dbl(1, .7027), dbl(2, -.000132), num(3, t), dbl(8, 110500), dbl(9, 157300), dbl(14, 20250000), num(15, t + 680000));
            if (d.stream === 29) b = cat(bytes(1, cat(bytes(1, 'RLCUSDT'), dbl(2, p), dbl(3, 1.4), dbl(4, 218580000), num(5, t))), num(2, t));
            if (b) ws.send(envelope(d.pair, d.stream, d.timeframe, b, t));
        }
    }, 20);
    ws.on('close', () => clearInterval(timer));
});

// theme.cpp kChrome: the topbar is ELEV in every preset.
const ELEV = {EdgeDepth: 0x0d0d0d, Graphite: 0x1b1b1e, Midnight: 0x11161d, 'High contrast': 0x0b0b0b};
const CHROME = ['EdgeDepth', 'Graphite', 'Midnight', 'High contrast'];

(async () => {
    const base = process.argv[2] || 'http://127.0.0.1:8797';
    const evidence = process.argv[3];
    if (evidence) fs.mkdirSync(evidence, {recursive: true});
    const browser = await chromium.launch({headless: true, executablePath: process.env.CHROME_BIN || '/usr/bin/google-chrome',
        args: ['--no-sandbox', '--enable-unsafe-swiftshader']});
    try {
        const page = await browser.newPage({viewport: {width: 1600, height: 900}});
        const errors = [];
        page.on('pageerror', e => errors.push(e.message));
        page.on('console', m => { if (/IM_ASSERT|User Error|Assertion failed/.test(m.text())) errors.push(m.text()); });
        // A browser that saved appearance with the previous build: Blue / white, Mint accent.
        const v1 = {version: 1, accent: 0, candles: 3, up: 0x2fd6ad, down: 0xee5c78};
        await page.addInitScript(v1 => {
            window.__EDGEDEPTH_TIER__ = 'pro';
            window.__EDGEDEPTH_TEST_UI__ = true;
            if (!sessionStorage.getItem('seeded')) {
                localStorage.removeItem('edgedepth.appearance.v2');
                localStorage.setItem('edgedepth.appearance.v1', JSON.stringify(v1));
                sessionStorage.setItem('seeded', '1');
            }
        }, v1);
        await page.route('**/*', r => {
            const u = new URL(r.request().url());
            if (u.pathname.startsWith('/api/')) return r.fulfill({status: 503, body: 'unavailable'});
            if (u.origin === new URL(base).origin) return r.continue();
            return r.fulfill({contentType: 'application/json', body: JSON.stringify(u.pathname.includes('symbols/metadata')
                ? {symbols: [{exchange: 'binancef', symbol: 'rlcusdt', tick_size: .0001, step_size: 1, base_asset: 'RLC', quote_asset: 'USDT'}]} : {})});
        });
        const settle = () => page.waitForTimeout(400);
        const ready = async () => { await page.waitForFunction(() => window.Module?.calledRun && typeof Module._workspace_flush === 'function'); await page.waitForTimeout(5000); };
        const saved = key => page.evaluate(k => JSON.parse(localStorage.getItem(k) || 'null'), key);
        const rects = () => page.evaluate(() => window.__EDGEDEPTH_UI_RECTS__ || {});
        const press = async name => {
            let r;
            for (let i = 0; i < 10 && !(r = (await rects())[name]); i++) await settle();
            assert.ok(r, 'UI mark missing: ' + name);
            await page.mouse.click((r[0] + r[2]) / 2, (r[1] + r[3]) / 2, {delay: 80});
            await settle();
        };
        const shot = async name => {
            await page.mouse.move(700, 885);  // off the controls, so no hover tooltip in the shot
            await settle();
            const buf = await page.screenshot(evidence ? {path: path.join(evidence, name + '.png')} : {});
            return PNG.sync.read(buf);
        };
        const topbar = png => dominant(png, 0, 1, png.width, 6);
        const choose = async (combo, label) => { await press(combo); await press(combo + '.' + label); };

        await page.goto(`${base}/terminal/binancef/rlcusdt?ws=ws://127.0.0.1:${server.address().port}`);
        await ready();
        // Migration: v2 written once from v1, chrome defaults to EdgeDepth, v1 left for an older build.
        let v2 = await saved('edgedepth.appearance.v2');
        assert.deepEqual({...v2}, {version: 2, chrome: 0, accent: 0, candles: 3, up: v1.up, down: v1.down, liq_map: 0}, 'v1 migrates to v2');
        assert.deepEqual(await saved('edgedepth.appearance.v1'), v1, 'v1 is left in place');
        let png = await shot('edgedepth');
        assert.ok(near(topbar(png), ELEV.EdgeDepth), `EdgeDepth topbar ${hex(topbar(png))}`);
        // The saved Blue / white keeps its index and now paints UP in the royal blue (theme.cpp BlueWhite).
        const blue = count(png, 0x4d7cff);
        assert.ok(blue >= 20, `Blue / white UP pixels: ${blue}`);

        // Each preset from the Appearance panel: saved, applied, and nothing else in the saved object changes.
        await press('topbar.appearance');
        await press('appearance.theme');
        await shot('appearance-theme-menu');
        await page.keyboard.press('Escape');
        await settle();
        for (const name of ['Graphite', 'Midnight', 'High contrast', 'EdgeDepth']) {
            await choose('appearance.theme', name);
            v2 = await saved('edgedepth.appearance.v2');
            assert.equal(v2.chrome, CHROME.indexOf(name), name + ' saved');
            assert.equal(v2.candles, 3, 'chrome leaves market colors alone');
            assert.equal(v2.accent, 0, 'chrome leaves the accent alone');
            await press('topbar.appearance');  // close the panel for a clean shot
            png = await shot(name.toLowerCase().replace(' ', '-'));
            assert.ok(near(topbar(png), ELEV[name]), `${name} topbar ${hex(topbar(png))} != ${hex(ELEV[name])}`);
            await press('topbar.appearance');
        }

        // Colourblind-safer market colors and the liquidation colormap are saved in the same object.
        await choose('appearance.theme', 'Midnight');
        await choose('appearance.market', 'Blue / orange');
        await press('appearance.liq.Viridis');
        v2 = await saved('edgedepth.appearance.v2');
        assert.equal(v2.candles, 5, 'Blue / orange saved');
        assert.equal(v2.liq_map, 1, 'liquidation colormap saved');
        await press('topbar.appearance');
        await shot('midnight-blue-orange');

        // Reload: the saved preset and colormap come back (a later save still carries liq_map 1).
        await page.reload();
        await ready();
        png = await shot('midnight-after-reload');
        assert.ok(near(topbar(png), ELEV.Midnight), `reloaded topbar ${hex(topbar(png))}`);
        await press('topbar.appearance');
        await choose('appearance.theme', 'High contrast');
        v2 = await saved('edgedepth.appearance.v2');
        assert.deepEqual({chrome: v2.chrome, candles: v2.candles, liq_map: v2.liq_map, accent: v2.accent},
            {chrome: 3, candles: 5, liq_map: 1, accent: 0}, 'reload restores every choice');
        await press('topbar.appearance');
        await shot('high-contrast-blue-orange');
        assert.deepEqual(errors, []);
        console.log('appearance presets: ok');
    } finally {
        await browser.close();
        server.close();
    }
})().catch(e => { console.error(e); process.exit(1); });
