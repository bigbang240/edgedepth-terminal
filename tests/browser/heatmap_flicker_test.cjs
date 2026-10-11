// Actual WASM + IndexedDB regression using a local synthetic live feed.
// NODE_PATH=<Playwright/pngjs modules> node tests/browser/heatmap_flicker_test.cjs <build dir> <evidence dir>
// EXPECT_LEGACY=1 verifies that the previous assets reproduce the blank frame.
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), http = require('node:http');
const {chromium} = require('playwright');
const {wsServer} = require('playwright-core/lib/utilsBundle');
const {PNG} = require('pngjs');
const [root, evidence] = process.argv.slice(2);
assert.ok(root && evidence, 'Provide build and evidence directories');
fs.mkdirSync(evidence, {recursive:true});
const cat = (...b) => Buffer.concat(b);
const v = n => { n=BigInt(n); const a=[]; do { a.push(Number(n&127n)|(n>127n?128:0)); n>>=7n; } while(n); return Buffer.from(a); };
const num=(i,n)=>cat(v(i*8),v(n)), bytes=(i,b)=>{b=Buffer.from(b);return cat(v(i*8+2),v(b.length),b)};
const dbl=(i,n)=>{const b=Buffer.alloc(8);b.writeDoubleLE(n);return cat(v(i*8+1),b)};
const pair={exchange:'binancef',symbol:'btcusdt'};
const envelope=(stream,tf,b,t)=>cat(bytes(1,cat(bytes(1,pair.exchange),bytes(2,pair.symbol))),num(2,stream),num(3,tf),bytes(4,b),num(5,t));
const level=(p,q)=>cat(dbl(1,p),dbl(2,q));
const depth=t=>{const a=[1,t,7+80*2,0,100,100.1,80];for(let k=0;k<40;k++)a.push(100-k*.1,20+k%9*30,100.1+k*.1,30+k%7*40);return a;};
function history(ws,q) {
    const values=[];
    const recovery=q.request_id.includes(':recovery:');
    for(let t=q.end_ms-(recovery?1800:60000);t<q.end_ms;t+=500)values.push(...depth(t));
    const packed=Buffer.alloc(values.length*8); values.forEach((n,i)=>packed.writeDoubleLE(n,i*8));
    ws.send(envelope(36,0,cat(bytes(1,q.request_id),num(2,q.end_ms),bytes(3,packed)),Date.now()));
}
const server=http.createServer((req,res)=>{
    res.setHeader('Cross-Origin-Opener-Policy','same-origin');res.setHeader('Cross-Origin-Embedder-Policy','require-corp');res.setHeader('Cache-Control','no-store');
    const pathname=new URL(req.url,'http://localhost').pathname;
    const file=path.join(root,pathname==='/'||(pathname.startsWith('/terminal/')&&!path.extname(pathname))?'index.html':path.basename(pathname));
    if(!fs.existsSync(file)||fs.statSync(file).isDirectory()){res.setHeader('Content-Type','application/json');res.end('{}');return;}
    res.setHeader('Content-Type',file.endsWith('.html')?'text/html':file.endsWith('.js')?'text/javascript':file.endsWith('.wasm')?'application/wasm':'application/octet-stream');
    fs.createReadStream(file).pipe(res);
});
const sockets=new wsServer({server});
let suppressDepth=false, recoveries=[], startupCount=0;
sockets.on('connection',ws=>{
    const subs=new Map();let sequence=100, bookSequence=100, seeded=false;
    ws.on('message',raw=>{const q=JSON.parse(raw);
        if(q.method==='subscribe')subs.set(JSON.stringify(q.data),q.data);
        if(q.method==='unsubscribe')subs.delete(JSON.stringify(q.data));
        if(q.method==='get_rt_history') {
            if(q.data.request_id.includes(':recovery:'))recoveries.push({ws,data:q.data});
            else {history(ws,q.data);startupCount++;}
        }
    });
    const timer=setInterval(()=>{if(ws.readyState!==1)return;const t=Date.now();
        for(const d of subs.values()) {
            let b;
            if(d.stream===3&&!suppressDepth) {
                b=cat(num(1,t),num(4,seeded?0:1),dbl(5,100.05),num(6,bookSequence+1),num(7,bookSequence+1),num(8,bookSequence));
                bookSequence++;seeded=true;
                for(let k=0;k<40;k++)b=cat(b,bytes(3,level(100-k*.1,20+k%9*30)),bytes(2,level(100.1+k*.1,30+k%7*40)));
            }
            if(d.stream===1)b=cat(dbl(1,100.05),dbl(2,2),num(3,sequence%2),num(4,t),num(5,sequence++));
            if(d.stream===2)b=cat(dbl(1,100),dbl(2,101),dbl(3,99),dbl(4,100.05),dbl(5,20),num(10,Math.floor(t/60000)*60000),num(11,60));
            if(b)ws.send(envelope(d.stream,d.timeframe||0,b,t));
        }
    },100);
    ws.on('close',()=>clearInterval(timer));
});
(async()=>{
    await new Promise(r=>server.listen(0,'127.0.0.1',r));
    const base=`http://127.0.0.1:${server.address().port}`;
    const browser=await chromium.launch({headless:true,executablePath:process.env.CHROME_BIN||'/usr/bin/google-chrome',args:['--no-sandbox','--enable-unsafe-swiftshader']});
    try {
        const page=await browser.newPage({viewport:{width:1400,height:900}});
        const errors=[];page.on('pageerror',e=>errors.push(e.message));
        page.on('console',m=>{if(/IM_ASSERT|Assertion failed|User Error/.test(m.text()))errors.push(m.text());});
        await page.addInitScript(()=>{window.__EDGEDEPTH_TIER__='admin';window.__EDGEDEPTH_REPLAY_TOKEN__='fixture-only';});
        await page.route('**/*',r=>new URL(r.request().url()).origin===base?r.continue():r.fulfill({contentType:'application/json',body:JSON.stringify(new URL(r.request().url()).pathname.includes('symbols/metadata')?{symbols:[{...pair,tick_size:.1,step_size:.001,base_asset:'BTC',quote_asset:'USDT'}]}:{})}));
        await page.goto(`${base}/terminal/binancef/btcusdt?ws=${encodeURIComponent(base.replace('http:','ws:')+'/ws')}&realtime=1`);
        await page.waitForFunction(()=>window.Module?.rtArchive?.diagnostics().sessions.some(s=>s.bytes>0),null,{timeout:60000});
        await page.waitForTimeout(3000);
        assert.ok(startupCount>0,'fixture exercises the real startup response');
        await page.evaluate(()=>{
            const archive=Module.rtArchive, query=archive.query.bind(archive), append=archive.append.bind(archive);
            window.blockHistory=false;window.recoveryWrites=0;window.acceptedQueries=0;
            archive.query=(...args)=>{if(window.blockHistory)return 0;const accepted=query(...args);if(accepted)window.acceptedQueries++;return accepted;};
            archive.append=(...args)=>{const accepted=append(...args);if(accepted===1&&args[3]===2)window.recoveryWrites++;return accepted;};
        });
        const colored=png=>{let count=0;for(let y=230;y<650;y++)for(let x=100;x<700;x++){
            const i=(y*png.width+x)*4;const [r,g,b]=png.data.subarray(i,i+3);
            if(Math.max(r,g,b)-Math.min(r,g,b)>25&&Math.max(r,g,b)>65)count++;
        }return count;};
        const shot=async name=>colored(PNG.sync.read(await page.screenshot({path:path.join(evidence,name+'.png')})));
        const baseline=await shot('before');assert.ok(baseline>20000,`historical heatmap is populated: ${baseline}`);
        const results=[];
        for(let i=0;i<3;i++) {
            recoveries=[];suppressDepth=true;await page.waitForTimeout(1800);suppressDepth=false;
            for(let wait=0;recoveries.length===0&&wait<50;wait++)await page.waitForTimeout(100);
            assert.ok(recoveries.length,'source gap requests actual recovery');
            await page.evaluate(()=>{window.blockHistory=true;});
            const priorWrites=await page.evaluate(()=>window.recoveryWrites);
            const recovery=recoveries.shift();history(recovery.ws,recovery.data);
            await page.waitForFunction(n=>window.recoveryWrites>n,priorWrites);
            await page.waitForTimeout(200);
            const pending=await shot(`recovery-${i+1}-pending`);
            await page.waitForTimeout(700);
            const delayed=await shot(`recovery-${i+1}-delayed`);
            const queries=await page.evaluate(()=>window.acceptedQueries);
            await page.evaluate(()=>{window.blockHistory=false;});
            await page.waitForFunction(n=>window.acceptedQueries>n,queries);
            await page.waitForTimeout(800);
            const replaced=await shot(`recovery-${i+1}-replaced`);
            const result={pending,delayed,replaced};results.push(result);
            console.log(JSON.stringify(result));
            if(process.env.EXPECT_LEGACY)assert.ok(Math.min(pending,delayed)<baseline*.15,'old build reproduces blank history');
            else assert.ok(Math.min(pending,delayed)>baseline*.75,'displayed history survives recovery while its replacement is delayed');
            assert.ok(replaced>baseline*.75,'complete replacement restores/retains visible history');
        }
        assert.deepEqual(errors,[]);
        fs.writeFileSync(path.join(evidence,'result.json'),JSON.stringify({legacy:!!process.env.EXPECT_LEGACY,baseline,results,errors},null,2));
        console.log('PASS: three real source recoveries, delayed replacements, rendered heatmap continuity');
    } finally {await browser.close();for(const ws of sockets.clients)ws.terminate();await new Promise(r=>sockets.close(r));await new Promise(r=>server.close(r));}
})().catch(e=>{console.error(e);process.exitCode=1;});
