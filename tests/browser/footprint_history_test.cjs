// Footprint history requests through the production WASM, against a local synthetic feed and a fake
// get_footprint_history reader (backend actor/server_session/footprint_handler.go).
// NODE_PATH=<Playwright modules> node tests/browser/footprint_history_test.cjs <build dir> <evidence dir>
// Scenarios: a failure sentinel (history_error) then a retry after the 3 s backoff, a request that gets no reply
// (asked again after the 20 s timeout plus backoff), and an older backend's bare sentinel (covers, no repeat).
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),http=require('node:http');
const {chromium}=require('playwright');
const {wsServer}=require('playwright-core/lib/utilsBundle');
const [root,evidence]=process.argv.slice(2);fs.mkdirSync(evidence,{recursive:true});
const cat=(...b)=>Buffer.concat(b),v=n=>{n=BigInt(n);const a=[];do{a.push(Number(n&127n)|(n>127n?128:0));n>>=7n;}while(n);return Buffer.from(a);};
const num=(i,n)=>cat(v(i*8),v(n)),bytes=(i,b)=>{b=Buffer.from(b);return cat(v(i*8+2),v(b.length),b)},dbl=(i,n)=>{const b=Buffer.alloc(8);b.writeDoubleLE(n);return cat(v(i*8+1),b)};
const start=Math.floor(Date.now()/60000)*60000;
const envelope=(pair,stream,tf,b,t=start)=>cat(bytes(1,cat(bytes(1,pair.exchange),bytes(2,pair.symbol))),num(2,stream),num(3,tf),bytes(4,b),num(5,t));
const price=i=>100+1.2*Math.sin(i/9);
const candle=i=>{const open=price(i),close=price(i+1),high=Math.max(open,close)+.15,low=Math.min(open,close)-.15;
 return cat(dbl(1,open),dbl(2,high),dbl(3,low),dbl(4,close),dbl(5,2e3),dbl(6,1.1e3),dbl(7,.9e3),num(10,start-(180-i)*60000),num(11,60),num(12,1));};
const candles=()=>cat(num(1,60),...Array.from({length:180},(_,i)=>bytes(2,candle(i))));
// One stored minute per candle in range (TickVolumeUpdate), levels at 0.1 ticks across the candle.
const minute=i=>{const t=start-(180-i)*60000,lo=Math.min(price(i),price(i+1))-.1,levels=[];
 for(let k=0;k<6;k++)levels.push(bytes(1,cat(dbl(1,+(lo+.1*k).toFixed(1)),dbl(2,10+k),dbl(3,12-k),dbl(4,22),num(5,4))));
 return cat(num(2,t),num(3,60000),num(4,t),num(5,t+60000),bytes(6,cat(...levels)),dbl(7,132),dbl(8,75),dbl(9,57),num(10,24),
  dbl(11,lo+.5),dbl(12,lo),dbl(13,lo+.2));};
const batch=(from,to)=>cat(...Array.from({length:180},(_,i)=>i).filter(i=>{const t=start-(180-i)*60000;return t>=from&&t<=to;}).map(i=>bytes(1,minute(i))));
// The completion sentinel: timestamp_ms 0, the requested range echoed, history_error (field 19) on a failure.
const sentinel=(from,to,failure)=>failure===undefined?Buffer.alloc(0):cat(num(4,from),num(5,to),failure?bytes(19,failure):Buffer.alloc(0));
const server=http.createServer((req,res)=>{
 res.setHeader('Cross-Origin-Opener-Policy','same-origin');res.setHeader('Cross-Origin-Embedder-Policy','require-corp');res.setHeader('Cache-Control','no-store');
 const pathname=new URL(req.url,'http://localhost').pathname,file=path.join(root,pathname==='/'||(pathname.startsWith('/terminal/')&&!path.extname(pathname))?'index.html':path.basename(pathname));
 if(!fs.existsSync(file)||fs.statSync(file).isDirectory()){res.setHeader('Content-Type','application/json');res.end('{}');return;}
 res.setHeader('Content-Type',file.endsWith('.html')?'text/html':file.endsWith('.js')?'text/javascript':file.endsWith('.wasm')?'application/wasm':'application/octet-stream');fs.createReadStream(file).pipe(res);
});
// Every footprint history request, in order, and how the fake reader answers: the first one per scenario fails,
// gets no reply or succeeds; later ones succeed with the echoed sentinel (or the bare one, for an older backend).
let requests=[],mode='fail-first';
const sockets=new wsServer({server});
sockets.on('connection',ws=>{const subs=new Map();
 ws.on('message',raw=>{const q=JSON.parse(raw);
  if(q.method==='subscribe')subs.set(JSON.stringify(q.data),q.data);
  if(q.method==='unsubscribe')subs.delete(JSON.stringify(q.data));
  if(q.method==='get_historical_candles'){const pair=q.data.pair||{exchange:'binancef',symbol:'btcusdt'};ws.send(envelope(pair,8,60,candles()));}
  if(q.method==='get_footprint_history'){
   requests.push({at:Date.now(),from:q.data.start_time,to:q.data.end_time});const pair=q.data.pair,from=q.data.start_time,to=q.data.end_time;
   if(requests.length===1&&mode==='fail-first'){
    ws.send(JSON.stringify({error:'footprint history unavailable',method:'get_footprint_history'}));
    ws.send(envelope(pair,17,0,sentinel(from,to,'unavailable')));return;}
   if(requests.length===1&&mode==='silent-first')return;
   const b=batch(from,to);if(b.length)ws.send(envelope(pair,17,60000,b));
   ws.send(envelope(pair,17,0,sentinel(from,to,mode==='legacy'?undefined:'')));
  }
 });
 const timer=setInterval(()=>{if(ws.readyState!==1)return;const t=Date.now();for(const d of subs.values()){
  let b;
  if(d.stream===2)b=candle(180);
  if(b)ws.send(envelope(d.pair,d.stream,d.timeframe||0,b,t));
 }},250);ws.on('close',()=>clearInterval(timer));
});
async function open(browser,base){
 const page=await browser.newPage({viewport:{width:1600,height:900}}),errors=[];
 page.on('pageerror',e=>errors.push(e.message));page.on('console',m=>{if(/IM_ASSERT|Assertion failed|User Error/.test(m.text()))errors.push(m.text())});
 await page.addInitScript(()=>{window.__EDGEDEPTH_TIER__='pro';window.__EDGEDEPTH_REPLAY_TOKEN__='fixture-only';
  sessionStorage.setItem('edgedepth.workspace.tab.v1',JSON.stringify({version:1,layout:'',widgets:[{type:'chart',title:'',settings:{timeframe:60,chart_type:1,rt_mode:false,
   chart_layer_defaults:2,liq_dense_field:false,heatmap_enabled:false,liq_observed_enabled:false,candle_bubbles:false}}]}));});
 await page.route('**/*',r=>new URL(r.request().url()).origin===base?r.continue():r.fulfill({contentType:'application/json',body:JSON.stringify(new URL(r.request().url()).pathname.includes('symbols/metadata')?{symbols:[{exchange:'binancef',symbol:'btcusdt',tick_size:.1,step_size:.001,base_asset:'BTC',quote_asset:'USDT'}]}:{})}));
 await page.goto(`${base}/terminal/binancef/btcusdt?ws=${encodeURIComponent(base.replace('http:','ws:')+'/ws')}`);
 await page.waitForFunction(()=>typeof Module?._workspace_flush==='function',null,{timeout:60000});
 return {page,errors};
}
const waitFor=async(page,n,ms)=>{const until=Date.now()+ms;while(requests.length<n&&Date.now()<until)await page.waitForTimeout(250);};
// After a success, a later request may only widen what is covered (the view moved), never repeat it.
const repeats=(covered,later)=>later.filter(r=>r.from>=covered.from&&r.to<=covered.to);
(async()=>{
 await new Promise(r=>server.listen(0,'127.0.0.1',r));const base=`http://127.0.0.1:${server.address().port}`;
 const browser=await chromium.launch({headless:true,executablePath:'/usr/bin/google-chrome',args:['--no-sandbox','--enable-unsafe-swiftshader']});
 const result={};
 try{
  // 1. The first request fails: no coverage, asked again after the backoff, then covered.
  {
   requests=[];mode='fail-first';
   const {page,errors}=await open(browser,base);
   await waitFor(page,2,15000);
   assert.ok(requests.length>=2,'a failure sentinel leads to another request');
   const gap=requests[1].at-requests[0].at;
   assert.ok(gap>=2800&&gap<8000,`retry after the 3 s backoff (${gap} ms)`);
   // Nothing was covered, so the retry asks for the view as it is now (a minute boundary can shift it).
   assert.ok(requests[1].from<requests[0].to&&requests[1].to>requests[0].from,`the failed range is asked again ${JSON.stringify(requests.slice(0,2))}`);
   await page.waitForTimeout(5000);
   assert.deepEqual(repeats(requests[1],requests.slice(2)),[],'a covered range is not asked again');
   // Zoom into the last minutes so cluster cells draw from the retried reply; inside coverage nothing is asked.
   const before=requests.length;
   for(let k=0;k<12;k++){await page.mouse.move(1400,500);await page.mouse.wheel(0,-240);await page.waitForTimeout(120);}
   await page.waitForTimeout(2500);
   assert.equal(requests.length,before,'zooming inside the covered range asks nothing');
   await page.mouse.move(5,5);await page.waitForTimeout(300);await page.screenshot({path:path.join(evidence,'retried.png')});
   result.failure={gap,requests:requests.length};assert.deepEqual(errors,[]);await page.close();
  }
  // 2. No reply at all: in flight until the 20 s timeout, then the backoff, then asked again.
  {
   requests=[];mode='silent-first';
   const {page,errors}=await open(browser,base);
   await waitFor(page,1,15000);await waitFor(page,2,32000);
   assert.ok(requests.length>=2,'a request with no reply is asked again');
   const gap=requests[1].at-requests[0].at;
   assert.ok(gap>=22500&&gap<30000,`asked again after the 20 s timeout and 3 s backoff (${gap} ms)`);
   await page.waitForTimeout(4000);
   assert.deepEqual(repeats(requests[1],requests.slice(2)),[],'covered once answered');
   result.timeout={gap,requests:requests.length};assert.deepEqual(errors,[]);await page.close();
  }
  // 3. An older backend's bare sentinel (no echo) still completes the request in flight.
  {
   requests=[];mode='legacy';
   const {page,errors}=await open(browser,base);
   await waitFor(page,1,15000);await page.waitForTimeout(6000);
   assert.ok(requests.length>=1);
   assert.deepEqual(repeats(requests[0],requests.slice(1)),[],'a bare sentinel covers the range');
   result.legacy={requests:requests.length};assert.deepEqual(errors,[]);await page.close();
  }
 }finally{
  fs.writeFileSync(path.join(evidence,'result.json'),JSON.stringify(result,null,1));
  await browser.close();server.close();
 }
 console.log(JSON.stringify(result));
})().catch(e=>{console.error(e);process.exit(1);});
