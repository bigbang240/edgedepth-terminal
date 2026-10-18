// Actual Release-WASM Library -> different market -> add charts -> exit regression.
// node tests/browser/replay_library_widget_binding_test.cjs <base URL> <TUT pack> <evidence dir>
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {chromium} = require('playwright');
const [base, packPath, evidence] = process.argv.slice(2);
if (!base || !packPath || !evidence) throw Error('Expected base URL, TUT pack path, evidence directory');
const pack = fs.readFileSync(packPath);
fs.mkdirSync(evidence, {recursive:true});
const {wsServer} = require('playwright-core/lib/utilsBundle');
const server = new wsServer({port:0});
// Minimal protobuf fixture for an actual chart and its context menu.
const cat = (...b) => Buffer.concat(b);
const v = n => { n=BigInt(n); const a=[]; do { a.push(Number(n&127n)|(n>127n?128:0)); n>>=7n; } while(n); return Buffer.from(a); };
const num = (i,n) => cat(v(i*8),v(n));
const bytes = (i,b) => { b=Buffer.from(b); return cat(v(i*8+2),v(b.length),b); };
const dbl = (i,n) => { const b=Buffer.alloc(8); b.writeDoubleLE(n); return cat(v(i*8+1),b); };
const pair = cat(bytes(1,'binancef'),bytes(2,'gtcusdt'));
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
  const browser = await chromium.launch({headless:true, executablePath:'/usr/bin/google-chrome',
    args:['--no-sandbox','--enable-unsafe-swiftshader']});
  try {
    const page = await browser.newPage({viewport:{width:1600,height:1000}});
    const errors=[];
    page.on('pageerror', e => errors.push(e.message));
    await page.addInitScript(() => {
      window.__EDGEDEPTH_TIER__='pro';
      window.__EDGEDEPTH_TEST_UI__=true;
    });
    await page.route('**/*', route => {
      const req=route.request(), u=new URL(req.url());
      if(u.pathname==='/binding-manifest.json') return route.fulfill({json:{
        schema:'edgedepth.replay-library.v1', packs:[{id:'binding-tut',title:'TUT binding fixture',
          symbol:'tutusdt',exchange:'Binance Futures',exchange_id:'binancef',display_symbol:'TUT/USDT',
          pack_url:`${base}/binding.edpack`,duration_seconds:120,featured:true}]}});
      if(u.pathname==='/binding.edpack') {
        const range=req.headers().range?.match(/bytes=(\d+)-(\d*)/);
        const from=range ? Number(range[1]) : 0;
        const to=range && range[2] ? Math.min(Number(range[2]),pack.length-1) : pack.length-1;
        return route.fulfill({status:range?206:200,contentType:'application/octet-stream',
          headers:{'Accept-Ranges':'bytes',...(range?{'Content-Range':`bytes ${from}-${to}/${pack.length}`}:{})},
          body:pack.subarray(from,to+1)});
      }
      if(u.origin===new URL(base).origin && !u.pathname.startsWith('/api/')) return route.continue();
      return route.fulfill({status:503,body:'unavailable'});
    });
    const rects=()=>page.evaluate(()=>window.__EDGEDEPTH_UI_RECTS__||{});
    const wait= async id=> {
      try { await page.waitForFunction(id=>window.__EDGEDEPTH_UI_RECTS__?.[id],id,{timeout:60000}); }
      catch(e) { console.error('Missing',id,await rects(),errors); await page.screenshot({path:path.join(evidence,'failed.png')}); throw e; }
    };
    const click=async id=>{
      await wait(id);
      const [x0,y0,x1,y1]=(await rects())[id];
      await page.mouse.click((x0+x1)/2,(y0+y1)/2);
      await page.waitForTimeout(400);
    };
    await page.goto(`${base}/terminal/binancef/gtcusdt?ws=ws://127.0.0.1:${server.address().port}&replayLibrary=${encodeURIComponent(base+'/binding-manifest.json')}`);
    await wait('chart.market.binancef.gtcusdt.1');
    const liveUrl=page.url();
    await click('topbar.replay');
    await click('replay_menu.library');
    await click('library.play.binding-tut');
    await wait('replay.scrubber');
    await wait('chart.market.binancef.tutusdt.1');
    assert.equal(page.url(),liveUrl,'live route remains the return destination');
    assert.ok(!(await rects())['replay.preview_hint'],'packs must not advertise unavailable ghost candles');
    await click('chart.add_widget');
    if ((await rects())['chart.tools.add_widget']) await click('chart.tools.add_widget');
    await click('widget_add.chart');
    await wait('chart.market.binancef.tutusdt.2');
    await click('chart.add_widget');
    if ((await rects())['chart.tools.add_widget']) await click('chart.tools.add_widget');
    await click('widget_add.chart');
    await wait('chart.market.binancef.tutusdt.3');
    assert.ok(!Object.keys(await rects()).some(k=>k.startsWith('chart.market.binancef.gtcusdt.')),
      'no chart added against the live page market during the pack');
    await page.screenshot({path:path.join(evidence,'pack-three-tut-charts.png')});
    await click('replay.exit');
    await wait('chart.market.binancef.gtcusdt.1');
    assert.ok(!Object.keys(await rects()).some(k=>k.startsWith('chart.market.binancef.tutusdt.')),
      'replay-owned charts close on exit');
    assert.equal(page.url(),liveUrl);
    await click('chart.add_widget');
    if ((await rects())['chart.tools.add_widget']) await click('chart.tools.add_widget');
    await click('widget_add.chart');
    await wait('chart.market.binancef.gtcusdt.2');
    assert.deepEqual(errors,[]);
    await page.screenshot({path:path.join(evidence,'restored-gtc.png')});
    console.log('PASS: Library TUT over GTC, two added TUT charts, exit and new GTC chart; pack hint absent');
  } finally { await browser.close(); server.close(); }
})().catch(e=>{console.error(e);process.exitCode=1;});
