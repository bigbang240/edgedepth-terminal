// Refill levels in Real-time through the production WASM renderer, against a local synthetic feed and a fake
// refill engine (the backend refill_levels wire shape).
// NODE_PATH=<Playwright modules> node tests/browser/refill_levels_test.cjs <build dir> <evidence dir>
// Scenarios: a free viewer on Binance BTC (subscribe, warming chip, marks, hover, ladder), a free viewer on
// another market (locked: no subscribe), and the layer switched off (no subscribe).
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),http=require('node:http');
const {chromium}=require('playwright');
const {wsServer}=require('playwright-core/lib/utilsBundle');
const [root,evidence]=process.argv.slice(2);fs.mkdirSync(evidence,{recursive:true});
const cat=(...b)=>Buffer.concat(b),v=n=>{n=BigInt(n);const a=[];do{a.push(Number(n&127n)|(n>127n?128:0));n>>=7n;}while(n);return Buffer.from(a);};
const num=(i,n)=>cat(v(i*8),v(n)),bytes=(i,b)=>{b=Buffer.from(b);return cat(v(i*8+2),v(b.length),b)},dbl=(i,n)=>{const b=Buffer.alloc(8);b.writeDoubleLE(n);return cat(v(i*8+1),b)};
const envelope=(pair,stream,tf,b,t)=>cat(bytes(1,cat(bytes(1,pair.exchange),bytes(2,pair.symbol))),num(2,stream),num(3,tf),bytes(4,b),num(5,t));
const level=(p,q)=>cat(dbl(1,p),dbl(2,q));
const depth=t=>{const a=[1,t,7+80*2,0,100,100.1,80];for(let k=0;k<40;k++)a.push(100-k*.1,20+k%9*30,100.1+k*.1,30+k%7*40);return a;};
const server=http.createServer((req,res)=>{
 res.setHeader('Cross-Origin-Opener-Policy','same-origin');res.setHeader('Cross-Origin-Embedder-Policy','require-corp');res.setHeader('Cache-Control','no-store');
 const pathname=new URL(req.url,'http://localhost').pathname,file=path.join(root,pathname==='/'||(pathname.startsWith('/terminal/')&&!path.extname(pathname))?'index.html':path.basename(pathname));
 if(!fs.existsSync(file)||fs.statSync(file).isDirectory()){res.setHeader('Content-Type','application/json');res.end('{}');return;}
 res.setHeader('Content-Type',file.endsWith('.html')?'text/html':file.endsWith('.js')?'text/javascript':file.endsWith('.wasm')?'application/wasm':'application/octet-stream');fs.createReadStream(file).pipe(res);
});
// Every refill request the terminal sent, in order.
const requests=[];
// The fake engine's levels, fixed at subscribe time so ids and times are stable across messages.
const engineLevels=t0=>{
 const L=(id,side,price,started,known,status,closed,usd,shown,ratio,refills)=>({id,side,price,started_ms:t0-started,known_ms:t0-known,
  updated_ms:closed?t0-closed:t0,closed_ms:closed?t0-closed:0,status,executed_base:usd/price,executed_usd:usd,max_shown_base:shown,
  refilled_base:usd/price*.8,refills,shown_ratio:ratio,refill_ms:Array.from({length:refills},(_,i)=>t0-started+Math.round((started-(closed||1))*(i+1)/(refills+1)))});
 return [L('h-bid','bid',99.5,40000,30000,'holding',0,2440000,7.7,3.1,7),L('h-ask','ask',100.4,15000,10000,'holding',0,1200000,4.9,2.4,5),
  L('c-ask','ask',100.6,50000,45000,'crossed',20000,400000,2.0,2.0,3),L('p-bid','bid',99.2,35000,28000,'pulled',12000,150000,0.7,2.1,3),
  L('e-ask','ask',100.9,25000,18000,'exhausted',8000,900000,3.0,3.0,4)];
};
const sockets=new wsServer({server});
sockets.on('connection',ws=>{const subs=new Map(),timers=[];let sequence=100,bookSequence=100,seeded=false;
 ws.on('message',raw=>{const q=JSON.parse(raw);
  if(q.method==='subscribe')subs.set(JSON.stringify(q.data),q.data);
  if(q.method==='unsubscribe')subs.delete(JSON.stringify(q.data));
  if(q.method==='refill_subscribe'||q.method==='refill_unsubscribe'){requests.push({at:Date.now(),...q});}
  if(q.method==='refill_subscribe'){
   const pair=q.data.pair,base={type:'refill_levels',exchange:pair.exchange,symbol:pair.symbol,mode:'live',version:'liquidity_response.refill.v1',reason:''};
   ws.send(JSON.stringify({...base,status:'warming',as_of_ms:Date.now(),levels:[]}));
   const t0=Date.now()+2500,levels=engineLevels(t0);
   timers.push(setTimeout(()=>timers.push(setInterval(()=>{if(ws.readyState!==1)return;const now=Date.now();
    for(const l of levels)if(l.status==='holding')l.updated_ms=now-300;
    ws.send(JSON.stringify({...base,status:'observing',as_of_ms:now-300,levels}));},500)),2500));
  }
  if(q.method==='refill_unsubscribe')for(const t of timers){clearTimeout(t);clearInterval(t);}
  if(q.method==='get_rt_history'){
   const values=[];for(let t=q.data.end_ms-90000;t<q.data.end_ms;t+=500)values.push(...depth(t));
   const packed=Buffer.alloc(values.length*8);values.forEach((n,i)=>packed.writeDoubleLE(n,i*8));
   ws.send(envelope(q.data.pair||{exchange:'binancef',symbol:'btcusdt'},36,0,cat(bytes(1,q.data.request_id),num(2,q.data.end_ms),bytes(3,packed)),Date.now()));
  }
 });
 const timer=setInterval(()=>{if(ws.readyState!==1)return;const t=Date.now();for(const d of subs.values()){
  let b;
  if(d.stream===3){b=cat(num(1,t),num(4,seeded?0:1),dbl(5,100.05),num(6,bookSequence+1),num(7,bookSequence+1),num(8,bookSequence));bookSequence++;seeded=true;for(let k=0;k<40;k++)b=cat(b,bytes(3,level(100-k*.1,20+k%9*30)),bytes(2,level(100.1+k*.1,30+k%7*40)));}
  if(d.stream===1)b=cat(dbl(1,100.05),dbl(2,2),num(3,sequence%2),num(4,t),num(5,sequence++));
  if(d.stream===2)b=cat(dbl(1,100),dbl(2,101),dbl(3,99),dbl(4,100.05),dbl(5,20),num(10,Math.floor(t/60000)*60000),num(11,60));
  if(b)ws.send(envelope(d.pair,d.stream,d.timeframe||0,b,t));
 }},100);ws.on('close',()=>{clearInterval(timer);for(const t of timers){clearTimeout(t);clearInterval(t);}});
});
// The refill marks the chart and its ladder drew this frame, by name (core/ui_test_marks.h): absorption bubbles,
// dots at refills, edge ticks for holding levels, and the ladder's row bars.
const refillMarks=async page=>{const rects=await page.evaluate(()=>window.__EDGEDEPTH_UI_RECTS__||{});
 const of=prefix=>Object.keys(rects).filter(k=>k.startsWith(prefix)).map(k=>rects[k]);
 return {lenses:of('rt.refill.lens.'),dots:of('rt.refill.dot.'),edges:of('rt.refill.edge.'),ladder:of('rt.ladder.refill.')};};
// Hover, then press and release on later frames: a popup row needs the hover before the press.
const slowClick=async(page,x,y)=>{await page.mouse.move(x,y);await page.waitForTimeout(200);await page.mouse.down();await page.waitForTimeout(100);await page.mouse.up();await page.waitForTimeout(300);};
async function open(browser,base,{tier,symbol,settings}){
 const page=await browser.newPage({viewport:{width:1600,height:900}}),errors=[];
 page.on('pageerror',e=>errors.push(e.message));page.on('console',m=>{if(/IM_ASSERT|Assertion failed|User Error/.test(m.text()))errors.push(m.text())});
 await page.addInitScript(([t,s])=>{window.__EDGEDEPTH_TIER__=t;window.__EDGEDEPTH_REPLAY_TOKEN__='fixture-only';window.__EDGEDEPTH_TEST_UI__=true;
  window.__usage=[];window.addEventListener('edgedepth:usage',e=>{try{window.__usage.push(JSON.parse(e.detail))}catch(_){window.__usage.push(e.detail)}});
  sessionStorage.setItem('edgedepth.workspace.tab.v1',JSON.stringify({version:1,layout:'',widgets:[{type:'chart',title:'',settings:{timeframe:60,chart_type:0,rt_mode:true,rt_span_ms:90000,rt_depth:true,rt_bubbles:false,rt_trade_line:false,rt_dom:true,...s}}]}));
 },[tier,settings||{}]);
 await page.route('**/*',r=>new URL(r.request().url()).origin===base?r.continue():r.fulfill({contentType:'application/json',body:JSON.stringify(new URL(r.request().url()).pathname.includes('symbols/metadata')?{symbols:[{exchange:'binancef',symbol,tick_size:.1,step_size:.001,base_asset:symbol.replace('usdt','').toUpperCase(),quote_asset:'USDT'}]}:{})}));
 await page.goto(`${base}/terminal/binancef/${symbol}?ws=${encodeURIComponent(base.replace('http:','ws:')+'/ws')}&realtime=1`);
 await page.waitForFunction(()=>window.Module?.rtArchive?.diagnostics().sessions.some(s=>s.bytes>0),null,{timeout:60000});
 return {page,errors};
}
(async()=>{
 await new Promise(r=>server.listen(0,'127.0.0.1',r));const base=`http://127.0.0.1:${server.address().port}`;
 const browser=await chromium.launch({headless:true,executablePath:'/usr/bin/google-chrome',args:['--no-sandbox','--enable-unsafe-swiftshader']});
 const result={};
 try{
  // 1. Free viewer on Binance BTC: free here.
  {
   const {page,errors}=await open(browser,base,{tier:'free',symbol:'btcusdt'});
   await page.waitForTimeout(800);
   const subscribe=requests.find(r=>r.method==='refill_subscribe'&&r.data.pair.symbol==='btcusdt');
   assert.ok(subscribe,'refill_subscribe sent for BTC');
   assert.deepEqual(subscribe.data.pair,{exchange:'binancef',symbol:'btcusdt'});
   assert.equal(subscribe.data.tick_size,0.1,'native tick');
   assert.equal(subscribe.data.entitlement_token,'fixture-only');
   assert.equal(subscribe.type,undefined,'requests use method, not type');
   assert.equal(requests.filter(r=>r.method==='refill_subscribe').length,1,'one subscribe');
   await page.mouse.move(5,5);await page.screenshot({path:path.join(evidence,'warming.png')});
   if(process.env.LAYERS_X){await page.mouse.click(+process.env.LAYERS_X,+process.env.LAYERS_Y);await page.waitForTimeout(300);
    await page.screenshot({path:path.join(evidence,'layers-free-btc.png')});await page.keyboard.press('Escape');await page.mouse.move(5,5);}
   await page.waitForTimeout(4000);
   await page.screenshot({path:path.join(evidence,'marks.png')});
   const marks=await refillMarks(page);
   result.btc={lenses:marks.lenses.length,dots:marks.dots.length,edges:marks.edges.length,ladder:marks.ladder.length};
   // The feed prints away from the level prices, so each holding level's absorption bubble sits at its latest refill,
   // sized by the server's traded total.
   assert.ok(marks.lenses.length>=2,'both holding levels draw an absorption bubble');
   assert.ok(marks.edges.length>=2,'both holding levels tick the right edge');
   assert.ok(marks.dots.length>=2,'refills marked as dots');
   assert.ok(marks.ladder.length>=1,'ladder marks drawn');
   // Hover a holding level's edge tick.
   const tick=marks.edges[0];await page.mouse.move((tick[0]+tick[2])/2,(tick[1]+tick[3])/2);await page.waitForTimeout(400);
   await page.screenshot({path:path.join(evidence,'hover.png')});
   if(process.env.LAYERS_X&&process.env.REFILL_ROW_Y){
    // Layers > Refill levels off: the last owner unsubscribes and the rails go.
    await page.mouse.move(5,5);await page.mouse.click(+process.env.LAYERS_X,+process.env.LAYERS_Y);await page.waitForTimeout(300);
    await slowClick(page,+process.env.LAYERS_X+60,+process.env.REFILL_ROW_Y);await page.keyboard.press('Escape');await page.mouse.move(5,5);await page.waitForTimeout(600);
    const unsub=requests.find(r=>r.method==='refill_unsubscribe'&&r.data.pair.symbol==='btcusdt');
    assert.ok(unsub,'turning the layer off unsubscribes');assert.deepEqual(unsub.data,{pair:{exchange:'binancef',symbol:'btcusdt'}});
    await page.screenshot({path:path.join(evidence,'layer-off.png')});
    const off=await refillMarks(page);
    result.btc.marksAfterOff=off.lenses.length+off.dots.length+off.edges.length+off.ladder.length;
    assert.equal(result.btc.marksAfterOff,0,'no refill marks on the chart or ladder with the layer off');
   }
   result.btc.errors=errors;assert.deepEqual(errors,[]);
   await page.close();
  }
  // 2. Free viewer on another market: locked, nothing subscribed (the Real-time preview still runs).
  {
   const before=requests.length;
   const {page,errors}=await open(browser,base,{tier:'free',symbol:'ethusdt'});
   await page.waitForTimeout(3000);
   assert.equal(requests.slice(before).filter(r=>r.method==='refill_subscribe').length,0,'locked market sends no subscribe');
   // The preview countdown widens the Real-time pill, so Layers sits further right here.
   const lx=+(process.env.LOCKED_LAYERS_X||process.env.LAYERS_X||0);
   if(lx){await page.mouse.click(lx,+process.env.LAYERS_Y);await page.waitForTimeout(300);}
   await page.screenshot({path:path.join(evidence,'locked.png')});
   if(lx&&process.env.LOCKED_ROW_Y){await slowClick(page,lx+60,+process.env.LOCKED_ROW_Y);await page.waitForTimeout(300);
    await page.screenshot({path:path.join(evidence,'upsell.png')});
    const usage=await page.evaluate(()=>window.__usage);result.lockedUsage=usage.filter(u=>u&&u.props&&u.props.layer==='refill').map(u=>u.event);
    assert.deepEqual(result.lockedUsage,['locked_action','upsell_impression'],'locked row opens the refill upsell');}
   result.locked={errors};assert.deepEqual(errors,[]);await page.close();
  }
  // 3. Pro viewer with the layer saved off: nothing subscribed.
  {
   const before=requests.length;
   const {page,errors}=await open(browser,base,{tier:'pro',symbol:'ethusdt',settings:{rt_refill:false}});
   await page.waitForTimeout(2500);
   assert.equal(requests.slice(before).filter(r=>r.method==='refill_subscribe').length,0,'layer off sends no subscribe');
   const saved=await page.evaluate(()=>{Module._workspace_flush();return JSON.parse(sessionStorage.getItem('edgedepth.workspace.tab.v1')).widgets.find(w=>w.type==='chart').settings.rt_refill});
   assert.equal(saved,false,'rt_refill persists');
   result.off={errors};assert.deepEqual(errors,[]);await page.close();
  }
  fs.writeFileSync(path.join(evidence,'result.json'),JSON.stringify({result,requests},null,2));
  console.log(JSON.stringify(result));console.log('PASS: refill levels subscribe, draw, hover and gate');
 }finally{await browser.close();for(const ws of sockets.clients)ws.terminate();await new Promise(r=>sockets.close(r));await new Promise(r=>server.close(r));}
})().catch(e=>{console.error(e);process.exitCode=1});
