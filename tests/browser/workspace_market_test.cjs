// Actual WASM regression, served with serve_threaded.py. Synthetic market data only.
// NODE_PATH=<Playwright modules> node tests/browser/workspace_market_test.cjs <base URL>
const assert = require('node:assert/strict');
const {chromium} = require('playwright');
const fs = require('node:fs');
const path = require('node:path');
const {wsServer} = require('playwright-core/lib/utilsBundle');
const server = new wsServer({port:0});
server.on('connection', ws => ws.on('error', () => {}));
const pair = (exchange, symbol) => ({exchange, symbol});
const row = (type, market, instance) => ({type,
    title:`${type}###${type}_${market.exchange}_${market.symbol}${instance > 1 ? '_'+instance : ''}`,
    pair:market, ...(instance ? {instance} : {}), settings:{}});
(async () => {
    const base = process.argv[2] || 'http://127.0.0.1:8796';
    const evidence = process.argv[3];
    if (evidence) fs.mkdirSync(evidence,{recursive:true});
    const browser = await chromium.launch({headless:true, executablePath:'/usr/bin/google-chrome',
        args:['--no-sandbox','--enable-unsafe-swiftshader']});
    try {
        for (const primaryClosed of [true, false]) {
            const page = await browser.newPage({viewport:{width:1600,height:1000}});
            const errors=[]; page.on('pageerror', e => errors.push(e.message));
            const source=pair('bybit','rlcusdt');
            const chart=row('chart',source,primaryClosed ? 2 : 1);
            if (!primaryClosed) delete chart.pair;
            const fixture={version:1,layout:'',widgets:[chart,row('data',source),
                row('chart',pair('hl','BTC'),1),row('stats',pair('hl','BTC'))]};
            await page.addInitScript(fixture => {
                window.__EDGEDEPTH_TIER__='admin';
                if (!sessionStorage.getItem('seeded')) {
                    sessionStorage.setItem('edgedepth.workspace.tab.v1',JSON.stringify(fixture));
                    sessionStorage.setItem('seeded','1');
                }
            }, fixture);
            await page.route('**/*', r => {
                const u=new URL(r.request().url());
                if (u.pathname.startsWith('/api/')) return r.fulfill({status:503,body:'unavailable'});
                if (u.origin===new URL(base).origin) return r.continue();
                return r.fulfill({contentType:'application/json',body:JSON.stringify(
                    u.pathname.includes('symbols/metadata') ? {symbols:[
                        ...['bybit','binancef'].flatMap(exchange => ['rlcusdt','ethusdt'].map(symbol =>
                            ({exchange,symbol,tick_size:.001,step_size:1,base_asset:symbol.slice(0,-4).toUpperCase(),quote_asset:'USDT'}))),
                        {exchange:'hl',symbol:'BTC',tick_size:1,step_size:.001,base_asset:'BTC',quote_asset:'USD'}]} : {})});
            });
            const state=()=>page.evaluate(()=>{
                Module._workspace_flush();
                return JSON.parse(sessionStorage.getItem('edgedepth.workspace.tab.v1'));
            });
            const ready=async()=>{
                await page.waitForFunction(()=>window.Module?.calledRun && typeof Module._workspace_flush==='function');
                await page.waitForTimeout(4500);
            };
            for (const market of [pair('binancef','rlcusdt'),pair('binancef','ethusdt'),pair('bybit','rlcusdt')]) {
                await page.goto(`${base}/terminal/${market.exchange}/${market.symbol}?ws=ws://127.0.0.1:${server.address().port}`);
                await ready();
                const saved=await state();
                const primary=saved.widgets.find(w=>w.type==='chart'&&!w.pair&&w.instance===1);
                assert.ok(primary,`URL must own a primary chart, closed=${primaryClosed}: ${JSON.stringify(saved.widgets)}`);
                assert.ok(primary.title.includes(`###chart_${market.exchange}_${market.symbol}`));
                assert.deepEqual(saved.widgets.find(w=>w.type==='data').pair,market,'Data must follow the URL');
                assert.equal(saved.widgets.filter(w=>w.type==='data').length,1);
                assert.deepEqual(saved.widgets.find(w=>w.type==='stats').pair,pair('hl','BTC'),'comparison preserved');
                assert.ok(saved.widgets.some(w=>w.type==='chart'&&w.pair?.exchange==='hl'&&w.pair.symbol==='BTC'));
                await page.reload(); await ready();
                assert.deepEqual((await state()).widgets.map(w=>[w.type,w.pair,w.instance]),saved.widgets.map(w=>[w.type,w.pair,w.instance]));
            }
            // Tab focus also has browser history entries; traverse those first.
            for (let step=0;step<8 && !new URL(page.url()).pathname.endsWith('/ethusdt');step++) {
                await page.goBack(); await ready();
            }
            assert.ok(new URL(page.url()).pathname.endsWith('/ethusdt'));
            assert.deepEqual((await state()).widgets.find(w=>w.type==='data').pair,pair('binancef','ethusdt'));
            await page.goto(`${base}/terminal/binancef/rlcusdt?view=data&range=24h&ws=ws://127.0.0.1:${server.address().port}`);
            await ready();
            const dataState = await state();
            assert.equal(dataState.widgets.filter(w=>w.type==='data').length,1);
            assert.equal(dataState.widgets.find(w=>w.type==='data').settings.hours,24);
            assert.equal(new URL(page.url()).searchParams.get('view'),'data','explicit Data link stays focused');
            if (evidence) {
                await page.screenshot({path:path.join(evidence,`data-primary-closed-${primaryClosed}.png`)});
                fs.writeFileSync(path.join(evidence,`state-${primaryClosed}.json`),JSON.stringify(dataState,null,2));
            }
            assert.deepEqual(errors,[]);
            await page.close();
        }
        console.log('PASS: legacy workspace venue/symbol navigation, closed primary, Data, comparison, reload and Back');
    } finally { await browser.close(); server.close(); }
})().catch(e=>{console.error(e);server.close();process.exitCode=1;});
