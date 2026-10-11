// Run against a completed release build served by serve_threaded.py.
// NODE_PATH=<Playwright modules> node tests/browser/companion_creation_test.cjs <base URL> <evidence directory>
// This exercises the real + Widget menu, not preassigned ownership in a fixture.
const assert = require('node:assert/strict');
const path = require('node:path');
const fs=require('fs');
const {chromium}=require('playwright');
const {wsServer}=require('playwright-core/lib/utilsBundle');
const cat=(...b)=>Buffer.concat(b), v=n=>{n=BigInt(n);let a=[];do{a.push(Number(n&127n)|(n>127n?128:0));n>>=7n}while(n);return Buffer.from(a)};
const num=(i,n)=>cat(v(i*8),v(n)),bytes=(i,b)=>{b=Buffer.from(b);return cat(v(i*8+2),v(b.length),b)},dbl=(i,n)=>{let b=Buffer.alloc(8);b.writeDoubleLE(n);return cat(v(i*8+1),b)};
const envelope=(pair,stream,tf,b,t)=>cat(bytes(1,cat(bytes(1,pair.exchange),bytes(2,pair.symbol))),num(2,stream),num(3,tf),bytes(4,b),num(5,t));
const server=new wsServer({port:0}); const start=Date.now();
server.on('connection',ws=>{let subs=new Map,sequence=100; ws.on('message',m=>{let q=JSON.parse(m);if(q.method==='subscribe')subs.set(JSON.stringify(q.data),q.data);if(q.method==='unsubscribe')subs.delete(JSON.stringify(q.data))});let timer=setInterval(()=>{if(ws.readyState!==1)return;const t=Date.now(),p=.355+Math.sin((t-start)/7000)*.0012+Math.sin((t-start)/1100)*.0002;for(const d of subs.values()){
 let b=null;
 if(d.stream===3){b=cat(num(1,t),num(4,1),dbl(5,p),num(7,sequence++));for(let k=1;k<100;k++){b=cat(b,bytes(2,cat(dbl(1,Math.round((p+k*.00001)*1e5)/1e5),dbl(2,100+(k%13===0?4000:30)*k))),bytes(3,cat(dbl(1,Math.round((p-k*.00001)*1e5)/1e5),dbl(2,100+(k%17===0?3500:20)*k))))}}
 if(d.stream===1)b=cat(dbl(1,p),dbl(2,30+(sequence%17)*20),num(3,sequence%2),num(4,t),num(5,sequence++));
 if(d.stream===2)b=cat(dbl(1,p),dbl(2,p+.0002),dbl(3,p-.0002),dbl(4,p),dbl(5,200),num(10,Math.floor(t/60000)*60000),num(11,60));
 if(d.stream===29)b=cat(bytes(1,cat(bytes(1,d.pair.symbol),dbl(2,p),dbl(3,1.4),dbl(4,218580000),num(5,t))),num(2,t));
 if(b)ws.send(envelope(d.pair,d.stream,d.timeframe,b,t));
 }},20);ws.on('close',()=>clearInterval(timer))});

(async () => {
    const base = process.argv[2] || 'http://127.0.0.1:8793';
    const evidence = process.argv[3];
    if (evidence) fs.mkdirSync(evidence, {recursive:true});
    const browser = await chromium.launch({headless:true,
        executablePath:process.env.CHROME_BIN || '/usr/bin/google-chrome',
        args:['--no-sandbox','--enable-unsafe-swiftshader']});
    try {
        const page = await browser.newPage({viewport:{width:2200,height:1100}});
        const errors = []; page.on('pageerror', e => errors.push(e.message));
        await page.addInitScript(() => { window.__EDGEDEPTH_TIER__='admin'; });
        await page.route('**/*', r => {
            const u = new URL(r.request().url());
            if (u.origin === new URL(base).origin) return r.continue();
            return r.fulfill({contentType:'application/json',body:JSON.stringify(
                u.pathname.includes('symbols/metadata') ? {symbols:[{exchange:'binancef',symbol:'btcusdt',
                    tick_size:.00001,step_size:1,base_asset:'BTC',quote_asset:'USDT'}]} : {})});
        });
        await page.goto(`${base}/index.html?ws=ws://127.0.0.1:${server.address().port}`);
        await page.waitForFunction(() => typeof window.Module?._workspace_flush === 'function');
        await page.waitForTimeout(8000);
        const state = () => page.evaluate(() => {
            Module._workspace_flush(); return JSON.parse(sessionStorage.getItem('edgedepth.workspace.tab.v1'));
        });
        const settle = () => page.waitForTimeout(700);
        const shot = async name => { if (evidence) {
            await page.mouse.move(200,30); await settle();
            await page.screenshot({path:path.join(evidence,name+'.png')});
        }};
        const add = async y => {
            await page.mouse.click(1025,124); await settle();
            await page.mouse.click(1060,y); await settle();
        };
        const toggle = async () => { await page.mouse.click(648,124); await settle(); };
        // Close the right column's selected tab. The dock node's own close at the tab bar's far right is gone
        // (2026-10-11: one close per panel, the tab's x on hover); a middle click on the tab is its stable hit.
        const closeRightTab = async () => { await page.mouse.click(1800,93,{button:'middle'}); await settle(); };
        const width = d => Number(d.layout.match(/\[Window\]\[chart_binancef_btcusdt\][\s\S]*?Size=(\d+),/)[1]);
        // Remove the boot companions. The bug was in creating replacements.
        await closeRightTab();
        await closeRightTab();
        assert.deepEqual((await state()).widgets.map(w=>w.type),['chart','watchlist']);
        await add(295); // normal DOM, while candles are active
        await add(325); // normal Trades, while candles are active
        await add(295); // focus the existing normal DOM; must not pin it
        const candles = await state();
        for (const type of ['dom','trades']) {
            const rows=candles.widgets.filter(w=>w.type===type);
            assert.equal(rows.length,1,`one ${type}`);
            assert.equal(rows[0].settings.mode_companion,true,`${type} created in candles must follow mode`);
        }
        await shot('created-in-candles');
        await toggle(); const rt = await state();
        assert.equal(rt.widgets.find(w=>w.type==='chart').settings.rt_mode,true);
        assert.ok(width(rt)>width(candles)+400,'both newly created panels must stop occupying the right column');
        await shot('created-panels-hidden-in-rt');
        await page.reload(); await page.waitForTimeout(8000);
        assert.ok(width(await state())>width(candles)+400,'hidden panels remain hidden after reload');
        await toggle(); const restored = await state();
        assert.equal(width(restored),width(candles),'original chart width restored');
        for (const type of ['dom','trades'])
            assert.deepEqual(restored.widgets.find(w=>w.type===type).settings,
                candles.widgets.find(w=>w.type===type).settings,`${type} settings restored`);
        await shot('created-panels-restored');
        await toggle(); await add(295); // explicitly reopen DOM while RT is active
        let d=await state();
        assert.equal(d.widgets.filter(w=>w.type==='dom').length,1,'reopening does not duplicate');
        assert.equal(d.widgets.find(w=>w.type==='dom').settings.mode_companion,false,'explicit RT reopen remains an override');
        await toggle(); await toggle();
        assert.equal((await state()).widgets.find(w=>w.type==='dom').settings.mode_companion,false,'pin survives toggles');
        await shot('explicit-override');
        await closeRightTab();
        await toggle();
        assert.equal((await state()).widgets.filter(w=>w.type==='dom').length,0,'closed DOM is never resurrected');
        await toggle(); await add(295); // create a replacement while already in RT
        const explicit = (await state()).widgets.filter(w=>w.type==='dom');
        assert.equal(explicit.length,1,'explicit RT creation adds exactly one DOM');
        assert.equal(explicit[0].settings.mode_companion,false,'creation in RT requests both views');
        await toggle(); await toggle();
        assert.equal((await state()).widgets.find(w=>w.type==='dom').settings.mode_companion,false);
        assert.deepEqual(errors,[],'no browser errors');
        console.log('PASS real menu creation, focus, auto-hide, reload, restoration, explicit override and close');
    } finally { await browser.close(); server.close(); }
})().catch(e => { console.error(e); server.close(); process.exitCode=1; });
