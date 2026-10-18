// Actual WASM regression, served with serve_threaded.py. Synthetic market data only.
// NODE_PATH=<Playwright modules> node tests/browser/replay_menu_test.cjs <base URL> [evidence dir]
// The live topbar's Replay menu (hub task jump-to-the-move): on the hosted product it leads with
// "Recent moves", which asks the web's Moves panel to open; self-hosted it has no such item.
const assert = require('node:assert/strict');
const {chromium} = require('playwright');
const fs = require('node:fs');
const path = require('node:path');
const {wsServer} = require('playwright-core/lib/utilsBundle');
const server = new wsServer({port:0});
// Minimal protobuf fixture for an actual chart and its context menu.
const cat = (...b) => Buffer.concat(b);
const v = n => { n=BigInt(n); const a=[]; do { a.push(Number(n&127n)|(n>127n?128:0)); n>>=7n; } while(n); return Buffer.from(a); };
const num = (i,n) => cat(v(i*8),v(n));
const bytes = (i,b) => { b=Buffer.from(b); return cat(v(i*8+2),v(b.length),b); };
const dbl = (i,n) => { const b=Buffer.alloc(8); b.writeDoubleLE(n); return cat(v(i*8+1),b); };
const pair = cat(bytes(1,'binancef'),bytes(2,'btcusdt'));
const envelope = (stream,tf,b,t) => cat(bytes(1,pair),num(2,stream),num(3,tf),bytes(4,b),num(5,t));
server.on('connection', ws => {
    ws.on('error', () => {});
    ws.on('message', raw => {
        const q=JSON.parse(raw);
        if(q.method !== 'get_historical_candles') return;
        const tf=Number(q.data?.timeframe || 60), at=Math.floor(Date.now()/(tf*1000))*tf*1000;
        const candles=[];
        for(let i=100;i>=0;i--) {
            const t=at-i*tf*1000, p=100+i*.01;
            candles.push(bytes(2,cat(dbl(1,p),dbl(2,p+.1),dbl(3,p-.1),dbl(4,p+.05),dbl(5,10),num(10,t),num(11,tf),num(12,1))));
        }
        ws.send(envelope(8,tf,cat(num(1,tf),...candles),at));
        const level=(p,q)=>cat(dbl(1,p),dbl(2,q));
        ws.send(envelope(3,0,cat(num(1,at),num(4,1),dbl(5,100.05),num(7,at),bytes(2,level(100.1,5)),bytes(3,level(100,5))),at));
    });
});
(async () => {
    const base = process.argv[2] || 'http://127.0.0.1:8796';
    const evidence = process.argv[3];
    if (evidence) fs.mkdirSync(evidence,{recursive:true});
    const browser = await chromium.launch({headless:true, executablePath:'/usr/bin/google-chrome',
        args:['--no-sandbox','--enable-unsafe-swiftshader']});
    try {
        for (const hosted of [true, false]) {
            const page = await browser.newPage({viewport:{width:1600,height:1000}});
            const errors=[]; page.on('pageerror', e => errors.push(e.message));
            await page.addInitScript(hosted => {
                window.__EDGEDEPTH_TIER__='pro';
                window.__EDGEDEPTH_TEST_UI__=true;
                if (!hosted) window.__EDGEDEPTH_HOSTED__=false;
                window.__movesOpened=0;
                window.addEventListener('edgedepth:moves-open', () => { window.__movesOpened++; });
            }, hosted);
            await page.route('**/*', r => {
                const u=new URL(r.request().url());
                if (u.origin===new URL(base).origin && !u.pathname.startsWith('/api/')) return r.continue();
                return r.fulfill({status:503,body:'unavailable'});
            });
            await page.goto(`${base}/terminal/binancef/btcusdt?ws=ws://127.0.0.1:${server.address().port}`);
            await page.waitForFunction(()=>window.Module?.calledRun && window.__EDGEDEPTH_UI_RECTS__?.['topbar.replay']);
            await page.waitForFunction(() => window.__EDGEDEPTH_UI_RECTS__?.['chart.layers']);
            await page.waitForTimeout(1000);
            const state = () => page.evaluate(() => {
                Module._workspace_flush();
                return JSON.parse(sessionStorage.getItem('edgedepth.workspace.tab.v1'));
            });
            const settings = async () => (await state()).widgets.find(w => w.type === 'chart').settings;
            assert.equal((await settings()).rt_mode, !hosted, 'self-hosted starts in depth; hosted keeps candles');
            assert.equal((await settings()).rt_bubbles, hosted, 'self-hosted bubbles default off');
            assert.equal((await settings()).candle_bubbles, false, 'candle bubbles default off');
            const rects = () => page.evaluate(() => window.__EDGEDEPTH_UI_RECTS__ || {});
            const click = async id => {
                await page.waitForFunction(id => window.__EDGEDEPTH_UI_RECTS__?.[id], id);
                const [x0, y0, x1, y1] = (await rects())[id];
                await page.mouse.move((x0+x1)/2, (y0+y1)/2);
                await page.waitForTimeout(120);
                await page.mouse.click((x0+x1)/2, (y0+y1)/2);
                await page.waitForTimeout(350);
            };
            await click('topbar.replay');
            await page.waitForFunction(()=>window.__EDGEDEPTH_UI_RECTS__?.['replay_menu.library']);
            const r = await rects();
            if (evidence) await page.screenshot({path:path.join(evidence, `replay-menu-${hosted ? 'hosted' : 'self-hosted'}.png`)});
            if (hosted) {
                assert.ok(r['replay_menu.recent_moves'], 'hosted menu offers Recent moves');
                assert.ok(r['replay_menu.recent_moves'][1] < r['replay_menu.library'][1], 'Recent moves comes first');
                await click('replay_menu.recent_moves');
                assert.equal(await page.evaluate(()=>window.__movesOpened), 1, 'Recent moves asks the Moves panel to open');
                assert.ok(!(await rects())['replay_menu.library'], 'the menu closes');
            } else {
                assert.ok(!r['replay_menu.recent_moves'], 'self-hosted has no web Moves panel to open');
                await page.keyboard.press('Escape');
                await click('chart.layers');
                await click('layer.Trade bubbles');
                await page.keyboard.press('Escape');
                assert.equal((await settings()).rt_bubbles, true, 'bubbles remain an opt-in');
                await click('chart.realtime');
                assert.equal((await settings()).rt_mode, false, 'viewer switches to candles');
                await page.reload();
                await page.waitForFunction(() => window.__EDGEDEPTH_UI_RECTS__?.['chart.plot']);
                await page.waitForTimeout(1000);
                assert.equal((await settings()).rt_mode, false, 'saved candle mode survives reload');
                assert.equal((await settings()).rt_bubbles, true, 'opt-in bubbles survive reload');
            }
            await page.waitForFunction(() => window.__EDGEDEPTH_UI_RECTS__?.['chart.plot']);
            const plot=(await rects())['chart.plot'];
            const x=(plot[0]+plot[2])/2, y=(plot[1]+plot[3])/2;
            await page.mouse.move(x,y);
            await page.waitForTimeout(150);
            await page.mouse.click(x,y,{button:'right'});
            await page.waitForTimeout(350);
            if(evidence) await page.screenshot({path:path.join(evidence,`context-${hosted?'hosted':'self-hosted'}.png`)});
            assert.equal(Boolean((await rects())['replay_context.hosted']), hosted, 'hosted replay actions are absent self-hosted');
            await page.keyboard.press('Escape');
            if(!hosted) {
                const before=page.url();
                await page.mouse.move(x,y);
                await page.keyboard.press('r');
                await page.waitForTimeout(350);
                assert.equal(page.url(),before,'R cannot navigate to a hosted-only route');
                await click('topbar.replay');
                await click('replay_menu.library');
                assert.ok((await state()).widgets.some(w => w.type === 'library'), 'Replay Library opens inside the terminal');
            }
            assert.deepEqual(errors, []);
            await page.close();
        }
        console.log('replay_menu_test: PASS');
    } finally {
        await browser.close();
        server.close();
    }
})().catch(e => { console.error(e); process.exit(1); });
