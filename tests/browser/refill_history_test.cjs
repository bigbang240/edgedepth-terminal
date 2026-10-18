// Refill levels on candle charts (recorded history) through the production WASM renderer, against a local synthetic
// feed and a fake get_refill_history reader (the backend refill levels contract, "Recorded history").
// NODE_PATH=<Playwright modules> node tests/browser/refill_history_test.cjs <build dir> <evidence dir>
// Scenarios: a free viewer on Binance BTC (one read with the token, inside 7 days and not past now, at most 24
// levels drawn, hover; a busy refusal backs off 2 s), a truncated read (note), a free viewer on another market
// (locked: no read), and the layer saved off (no read, the setting persists).
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),http=require('node:http');
const {chromium}=require('playwright');
const {wsServer,PNG}=require('playwright-core/lib/utilsBundle');
const [root,evidence]=process.argv.slice(2);fs.mkdirSync(evidence,{recursive:true});
const cat=(...b)=>Buffer.concat(b),v=n=>{n=BigInt(n);const a=[];do{a.push(Number(n&127n)|(n>127n?128:0));n>>=7n;}while(n);return Buffer.from(a);};
const num=(i,n)=>cat(v(i*8),v(n)),bytes=(i,b)=>{b=Buffer.from(b);return cat(v(i*8+2),v(b.length),b)},dbl=(i,n)=>{const b=Buffer.alloc(8);b.writeDoubleLE(n);return cat(v(i*8+1),b)};
const start=Math.floor(Date.now()/60000)*60000;
const envelope=(pair,stream,tf,b,t=start)=>cat(bytes(1,cat(bytes(1,pair.exchange),bytes(2,pair.symbol))),num(2,stream),num(3,tf),bytes(4,b),num(5,t));
// Candles swinging between about 98.6 and 101.4, so the price axis holds every level below.
const price=i=>100+1.2*Math.sin(i/9);
const candle=i=>{const open=price(i),close=price(i+1),high=Math.max(open,close)+.15,low=Math.min(open,close)-.15;
 return cat(dbl(1,open),dbl(2,high),dbl(3,low),dbl(4,close),dbl(5,2e3),dbl(6,1.1e3),dbl(7,.9e3),num(10,start-(180-i)*60000),num(11,60),num(12,1));};
const candles=()=>cat(num(1,60),...Array.from({length:180},(_,i)=>bytes(2,candle(i))));
// 40 closed levels over the last hour at distinct prices; level k traded $100k x (k + 1), so the largest 24 are k >= 16.
const recorded=Array.from({length:40},(_,k)=>{const known=start-60*60000+k*80000,closed=known+120000,usd=1e5*(k+1),p=99+.05*k;
 return {id:`rec-${k}`,side:k%2?'ask':'bid',price:+p.toFixed(2),started_ms:known-4000,known_ms:known,updated_ms:closed,closed_ms:closed,
  status:k%5===0?'pulled':'crossed',executed_base:usd/p,executed_usd:usd,max_shown_base:usd/p/3,refilled_base:usd/p*.8,refills:5+k,shown_ratio:3,refill_ms:null};});
const server=http.createServer((req,res)=>{
 res.setHeader('Cross-Origin-Opener-Policy','same-origin');res.setHeader('Cross-Origin-Embedder-Policy','require-corp');res.setHeader('Cache-Control','no-store');
 const pathname=new URL(req.url,'http://localhost').pathname,file=path.join(root,pathname==='/'||(pathname.startsWith('/terminal/')&&!path.extname(pathname))?'index.html':path.basename(pathname));
 if(!fs.existsSync(file)||fs.statSync(file).isDirectory()){res.setHeader('Content-Type','application/json');res.end('{}');return;}
 res.setHeader('Content-Type',file.endsWith('.html')?'text/html':file.endsWith('.js')?'text/javascript':file.endsWith('.wasm')?'application/wasm':'application/octet-stream');fs.createReadStream(file).pipe(res);
});
// Every history read the terminal sent, in order, and how the fake reader answers the next ones.
const requests=[];let mode={busyFirst:false,truncated:false};
const sockets=new wsServer({server});
sockets.on('connection',ws=>{const subs=new Map();let sequence=100;
 ws.on('message',raw=>{const q=JSON.parse(raw);
  if(q.method==='subscribe')subs.set(JSON.stringify(q.data),q.data);
  if(q.method==='unsubscribe')subs.delete(JSON.stringify(q.data));
  if(q.method==='get_historical_candles'){const pair=q.data.pair||{exchange:'binancef',symbol:'btcusdt'};ws.send(envelope(pair,8,60,candles()));}
  if(q.method==='get_refill_history'){
   requests.push({at:Date.now(),...q});const d=q.data,pair=d.pair;
   const base={type:'refill_history',request_id:d.request_id,exchange:pair.exchange,symbol:pair.symbol,mode:'live',
    version:'liquidity_response.refill.v1',from_ms:d.from_ms,to_ms:d.to_ms,coverage_from_ms:start-2*864e5};
   if(mode.busyFirst&&requests.filter(r=>r.data.pair.symbol===pair.symbol).length===1){ws.send(JSON.stringify({...base,levels:[],truncated:false,error:'busy'}));return;}
   const levels=recorded.filter(l=>l.closed_ms>=d.from_ms&&l.closed_ms<=d.to_ms);
   ws.send(JSON.stringify({...base,levels:mode.truncated?levels.slice(-30):levels,truncated:mode.truncated,error:''}));
  }
 });
 const timer=setInterval(()=>{if(ws.readyState!==1)return;const t=Date.now();for(const d of subs.values()){
  let b;
  if(d.stream===1)b=cat(dbl(1,price(181)),dbl(2,2),num(3,sequence%2),num(4,t),num(5,sequence++));
  if(d.stream===2)b=candle(180);
  if(b)ws.send(envelope(d.pair,d.stream,d.timeframe||0,b,t));
 }},250);ws.on('close',()=>clearInterval(timer));
});
// The levels the chart drew this frame, by name (core/ui_test_marks.h): one rect per level, the pointer beside the
// candle where it qualified.
const levelMarks=async page=>{const rects=await page.evaluate(()=>window.__EDGEDEPTH_UI_RECTS__||{});
 return Object.keys(rects).filter(k=>k.startsWith('chart.refill.')).map(k=>rects[k]);};
async function open(browser,base,{tier,symbol,settings}){
 const page=await browser.newPage({viewport:{width:1600,height:900}}),errors=[];
 page.on('pageerror',e=>errors.push(e.message));page.on('console',m=>{if(/IM_ASSERT|Assertion failed|User Error/.test(m.text()))errors.push(m.text())});
 await page.addInitScript(([t,s])=>{window.__EDGEDEPTH_TIER__=t;window.__EDGEDEPTH_REPLAY_TOKEN__='fixture-only';window.__EDGEDEPTH_TEST_UI__=true;
  window.__usage=[];window.addEventListener('edgedepth:usage',e=>{try{window.__usage.push(JSON.parse(e.detail))}catch(_){window.__usage.push(e.detail)}});
  sessionStorage.setItem('edgedepth.workspace.tab.v1',JSON.stringify({version:1,layout:'',widgets:[{type:'chart',title:'',settings:{timeframe:60,chart_type:0,rt_mode:false,
   chart_layer_defaults:3,candle_refill:true,liq_dense_field:false,heatmap_enabled:false,liq_observed_enabled:false,candle_bubbles:false,...s}}]}));
 },[tier,settings||{}]);
 await page.route('**/*',r=>new URL(r.request().url()).origin===base?r.continue():r.fulfill({contentType:'application/json',body:JSON.stringify(new URL(r.request().url()).pathname.includes('symbols/metadata')?{symbols:[{exchange:'binancef',symbol,tick_size:.1,step_size:.001,base_asset:symbol.replace('usdt','').toUpperCase(),quote_asset:'USDT'}]}:{})}));
 await page.goto(`${base}/terminal/binancef/${symbol}?ws=${encodeURIComponent(base.replace('http:','ws:')+'/ws')}`);
 await page.waitForFunction(()=>typeof Module?._workspace_flush==='function',null,{timeout:60000});
 return {page,errors};
}
(async()=>{
 await new Promise(r=>server.listen(0,'127.0.0.1',r));const base=`http://127.0.0.1:${server.address().port}`;
 const browser=await chromium.launch({headless:true,executablePath:'/usr/bin/google-chrome',args:['--no-sandbox','--enable-unsafe-swiftshader']});
 const result={};
 try{
  // 1. Free viewer on Binance BTC, layer on by default: the first read is refused busy, the next waits 2 s.
  {
   mode={busyFirst:true,truncated:false};
   const {page,errors}=await open(browser,base,{tier:'free',symbol:'btcusdt'});
   await page.waitForTimeout(6000);
   const reads=requests.filter(r=>r.data.pair.symbol==='btcusdt');
   assert.ok(reads.length>=2,'a read, then another after the busy refusal');
   const first=reads[0].data,now=Date.now();
   assert.deepEqual(first.pair,{exchange:'binancef',symbol:'btcusdt'});
   assert.equal(first.entitlement_token,'fixture-only');
   assert.equal(reads[0].type,undefined,'requests use method, not type');
   assert.equal(typeof first.request_id,'string');
   assert.ok(first.to_ms<=now&&first.to_ms>now-60000,'to_ms at the clock, not in the future');
   assert.ok(first.from_ms<first.to_ms&&first.to_ms-first.from_ms<=7*864e5,'at most 7 days');
   assert.ok(first.from_ms>=now-72*3600e3,'never before the free 3-day window');
   assert.ok(reads[1].at-reads[0].at>=1900,'busy backs off 2 s');
   assert.ok(Math.abs(reads[1].data.from_ms-first.from_ms)<5*60000,'about the same range again');
   result.btc={reads:reads.map(r=>({from:r.data.from_ms,to:r.data.to_ms,at:r.at-reads[0].at}))};
   await page.mouse.move(5,5);await page.waitForTimeout(500);
   await page.screenshot({path:path.join(evidence,'levels.png')});
   const found=await levelMarks(page);
   result.btc.levels=found.length;
   assert.ok(found.length>=12,'levels drawn');
   assert.ok(found.length<=24,'at most 24 levels drawn');
   // Hover the widest level, on its pointer.
   const widest=found.sort((a,b)=>(b[2]-b[0])-(a[2]-a[0]))[0];
   await page.mouse.move(Math.round((widest[0]+widest[2])/2),Math.round((widest[1]+widest[3])/2));await page.waitForTimeout(500);
   await page.screenshot({path:path.join(evidence,'hover.png')});
   if(process.env.LAYERS_X){await page.mouse.move(5,5);await page.mouse.click(+process.env.LAYERS_X,+process.env.LAYERS_Y);await page.waitForTimeout(400);
    await page.screenshot({path:path.join(evidence,'layers-free-btc.png')});
    if(process.env.REFILL_ROW_Y){await page.mouse.move(+process.env.LAYERS_X+60,+process.env.REFILL_ROW_Y);await page.waitForTimeout(900);
     await page.screenshot({path:path.join(evidence,'row-tooltip.png')});}
    await page.keyboard.press('Escape');}
   // Panning a little reads nothing new.
   const before=requests.length;await page.mouse.move(700,400);await page.mouse.down();await page.mouse.move(740,400,{steps:6});await page.mouse.up();
   await page.waitForTimeout(1500);result.btc.readsAfterSmallPan=requests.length-before;
   assert.equal(requests.length-before,0,'a small pan stays inside what was read');
   result.btc.errors=errors;assert.deepEqual(errors,[]);
   await page.close();
  }
  // 2. A read that kept only its largest levels: the note shows.
  {
   mode={busyFirst:false,truncated:true};
   const {page,errors}=await open(browser,base,{tier:'pro',symbol:'btcusdt'});
   await page.waitForTimeout(4000);await page.mouse.move(5,5);
   await page.screenshot({path:path.join(evidence,'truncated.png')});
   result.truncated={errors};assert.deepEqual(errors,[]);await page.close();
  }
  // 3. Free viewer on another market: locked, nothing read.
  {
   mode={busyFirst:false,truncated:false};
   const before=requests.length;
   const {page,errors}=await open(browser,base,{tier:'free',symbol:'ethusdt'});
   await page.waitForTimeout(3500);
   assert.equal(requests.length-before,0,'locked market sends no read');
   // The Real-time preview countdown widens its pill, so Layers sits further right here.
   const lx=+(process.env.LOCKED_LAYERS_X||process.env.LAYERS_X||0);
   if(lx){await page.mouse.click(lx,+process.env.LAYERS_Y);await page.waitForTimeout(400);await page.screenshot({path:path.join(evidence,'locked.png')});
    if(process.env.LOCKED_ROW_Y){await page.mouse.move(lx+60,+process.env.LOCKED_ROW_Y);await page.waitForTimeout(200);await page.mouse.down();await page.waitForTimeout(100);await page.mouse.up();await page.waitForTimeout(400);
     await page.screenshot({path:path.join(evidence,'upsell.png')});
     const usage=await page.evaluate(()=>window.__usage);result.lockedUsage=usage.filter(u=>u&&u.props&&u.props.layer==='refill').map(u=>u.event);
     assert.deepEqual(result.lockedUsage,['locked_action','upsell_impression'],'locked row opens the refill upsell');}}
   result.locked={errors};assert.deepEqual(errors,[]);await page.close();
  }
  // 4. Pro viewer with the layer saved off: nothing read, and the setting persists.
  {
   const before=requests.length;
   const {page,errors}=await open(browser,base,{tier:'pro',symbol:'ethusdt',settings:{candle_refill:false}});
   await page.waitForTimeout(3000);
   assert.equal(requests.length-before,0,'layer off sends no read');
   const saved=await page.evaluate(()=>{Module._workspace_flush();return JSON.parse(sessionStorage.getItem('edgedepth.workspace.tab.v1')).widgets.find(w=>w.type==='chart').settings.candle_refill});
   assert.equal(saved,false,'candle_refill persists');
   result.off={errors};assert.deepEqual(errors,[]);await page.close();
  }
  // 5. The quiet-default migration switches existing candle refill markers off once.
  {
   const before=requests.length;
   const {page,errors}=await open(browser,base,{tier:'pro',symbol:'ethusdt',settings:{chart_layer_defaults:2}});
   await page.waitForTimeout(3500);
   assert.equal(requests.length,before,'quiet migration does not request refills');
   const saved=await page.evaluate(()=>{Module._workspace_flush();return JSON.parse(sessionStorage.getItem('edgedepth.workspace.tab.v1')).widgets.find(w=>w.type==='chart').settings.candle_refill});
   assert.equal(saved,false,'old workspace migrates to quiet defaults');
   result.proEth={errors};assert.deepEqual(errors,[]);await page.close();
  }
  fs.writeFileSync(path.join(evidence,'result.json'),JSON.stringify({result,requests},null,2));
  console.log(JSON.stringify(result));console.log('PASS: refill history reads, draws, backs off and gates');
 }finally{await browser.close();for(const ws of sockets.clients)ws.terminate();await new Promise(r=>sockets.close(r));await new Promise(r=>server.close(r));}
})().catch(e=>{console.error(e);process.exitCode=1});
