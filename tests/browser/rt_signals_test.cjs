// Real-time order book signals through the production WASM renderer, against a synthetic market that behaves like
// the real ones: on BTC most resting size sits at and next to the touch and moves with the price, a few genuine
// walls rest away from it (one is pulled as price approaches, one stands), trades hit the touch and sweep it on
// moves; a thin market has sparse ticks and occasional prints. A fake refill engine publishes levels on the path.
// NODE_PATH=<Playwright modules> node tests/browser/rt_signals_test.cjs <build dir> <evidence dir> [scenario...]
// Scenarios: btc-busy, btc-quiet, btc-spoof, thin, pull-history (append :contrast-off for auto contrast off, :candles for the candle
// chart with the Real-time pill at rest). SHOT_ONLY=1 takes
// screenshots without the assertions (for a build from before these marks).
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),http=require('node:http');
const {chromium}=require('playwright');
const {wsServer}=require('playwright-core/lib/utilsBundle');
const [root,evidence,...wanted]=process.argv.slice(2);fs.mkdirSync(evidence,{recursive:true});
const shotOnly=!!process.env.SHOT_ONLY,liveSeconds=process.env.LIVE_SECONDS ? +process.env.LIVE_SECONDS : null;
const cat=(...b)=>Buffer.concat(b),v=n=>{n=BigInt(n);const a=[];do{a.push(Number(n&127n)|(n>127n?128:0));n>>=7n;}while(n);return Buffer.from(a);};
const num=(i,n)=>cat(v(i*8),v(n)),bytes=(i,b)=>{b=Buffer.from(b);return cat(v(i*8+2),v(b.length),b)},dbl=(i,n)=>{const b=Buffer.alloc(8);b.writeDoubleLE(n);return cat(v(i*8+1),b)};
const envelope=(pair,stream,tf,b,t)=>cat(bytes(1,cat(bytes(1,pair.exchange),bytes(2,pair.symbol))),num(2,stream),num(3,tf),bytes(4,b),num(5,t));
const level=(p,q)=>cat(dbl(1,p),dbl(2,q));

// Deterministic randomness: the same market for every build compared.
const rng=seed=>{let s=seed>>>0;return()=>{s=(s+0x6D2B79F5)>>>0;let t=s;t=Math.imul(t^t>>>15,t|1);t^=t+Math.imul(t^t>>>7,t|61);return((t^t>>>14)>>>0)/4294967296;};};
const hash=(n,salt)=>{let x=Math.imul((n|0)^salt,2654435761)>>>0;x^=x>>>13;x=Math.imul(x,1274126177)>>>0;return((x^x>>>16)>>>0)/4294967296;};

const SCENARIOS={
 // Busy BTC: a staircase of moves, ~$40 over the view, heavy prints, size concentrated at the touch.
 'btc-busy':{symbol:'btcusdt',tick:0.1,p0:82955,levels:320,spread:1,trades:9,move:0.24,jump:2,regime:6000,sweep:[0.006,30,110],
  mm:[[0,6,14],[1,2,5],[2,1.5,4],[3,1.2,3.5],[4,1,3],[8,0.4,1.4],[20,0.15,0.7],[60,0.05,0.35]],
  rest:{p:0.06,lo:0.6,hi:3.2,round:100,roundLo:2,roundHi:6},minQty:0.001,trade:{median:0.03,tail:0.04,tailLo:0.8,tailHi:5},
  // Genuine walls: [side, offset in ticks from p0, size, from s, until s, pull when price within ticks]
  walls:[['bid',-150,48,-200,1e9,0],['ask',95,36,-40,1e9,25],['ask',230,40,-200,1e9,0],['bid',-260,30,-200,1e9,0]]},
 // Quiet BTC: a narrow range, few prints, the same book shape.
 'btc-quiet':{symbol:'btcusdt',tick:0.1,p0:82955,levels:320,spread:1,trades:0.8,move:0.02,jump:1,regime:20000,sweep:[0,0,0],
  mm:[[0,6,14],[1,2,5],[2,1.5,4],[3,1.2,3.5],[4,1,3],[8,0.4,1.4],[20,0.15,0.7],[60,0.05,0.35]],
  rest:{p:0.06,lo:0.6,hi:3.2,round:100,roundLo:2,roundHi:6},minQty:0.001,trade:{median:0.02,tail:0.02,tailLo:0.5,tailHi:2},
  walls:[['bid',-60,40,-200,1e9,0],['ask',80,32,-200,1e9,0]]},
 // A spoof: price climbs steadily toward a 45 BTC ask that is cancelled once price is within $0.80, while a bid
 // wall below stands. Exactly one pull, and it is the cancelled ask.
 'btc-spoof':{symbol:'btcusdt',tick:0.1,p0:82955,levels:320,spread:1,trades:2,move:0.02,jump:1,regime:1e9,trend:0.03,sweep:[0,0,0],
  mm:[[0,6,14],[1,2,5],[2,1.5,4],[3,1.2,3.5],[4,1,3],[8,0.4,1.4],[20,0.15,0.7],[60,0.05,0.35]],
  rest:{p:0.06,lo:0.6,hi:3.2,round:100,roundLo:2,roundHi:6},minQty:0.001,trade:{median:0.02,tail:0.02,tailLo:0.5,tailHi:2},
  // Timed for the screenshot 150 s after the page connects: the bid appears once price climbs past it and stands;
  // the ask is cancelled when price is 8 ticks away, inside the last minute of the view.
  walls:[['bid',25,40,-200,1e9,0],['ask',60,45,-200,1e9,8]]},
 // A thin perpetual: 0.00001 tick, sparse ticks, a wide spread, prints now and then.
 'thin':{symbol:'wusdt',tick:0.00001,p0:0.29685,levels:600,spread:4,trades:0.35,move:0.03,jump:3,regime:12000,sparse:0.35,sweep:[0.003,20,70],
  mm:[[0,3000,12000],[4,1500,6000],[20,600,3000],[80,200,1500]],
  rest:{p:0.05,lo:4000,hi:20000,round:100,roundLo:8000,roundHi:30000},minQty:1,trade:{median:400,tail:0.05,tailLo:3000,tailHi:20000},
  walls:[['bid',-120,90000,-200,1e9,0],['ask',140,70000,-60,1e9,40]]},
};

// Busy exact-price bursts reproduce grouped totals dwarfing the old single-fill reference.
SCENARIOS['btc-burst']={...SCENARIOS['btc-busy'],trades:64,move:0.06};

// Many genuine sustained walls, withdrawn at the same two prices. The first
// is largest; later small pulls must not erase it from a readable history.
SCENARIOS['pull-history']={...SCENARIOS['btc-quiet'],move:0,trades:1,
 walls:Array.from({length:24},(_,i)=>[i%2?'bid':'ask',i%2?-8:8,i===0?120:i%3===0?60:16,100+i*4,103.4+i*4,0])};

// One simulated market per connection, from the same seed, so every build sees the same sequence.
function market(cfg,t0){
 const r=rng(20261013),tick=cfg.tick,round=x=>Math.round(x/tick)*tick,dec=Math.max(0,-Math.floor(Math.log10(tick)+1e-9))+1;
 const fix=x=>+x.toFixed(dec);
 let mid=Math.round(cfg.p0/tick),drift=0,regimeEnd=0,tradeId=1000,step=0,sweepLeft=0,sweepStep=0,trend=0,last=null;
 const trades=[],samples=[];
 const lognormal=(m,s)=>m*Math.exp(s*Math.sqrt(-2*Math.log(r()+1e-12))*Math.cos(2*Math.PI*r()));
 const mmSize=(d,jitter)=>{let lo=0,hi=0;for(const [from,a,b] of cfg.mm)if(d>=from){lo=a;hi=b;}return lo+(hi-lo)*(0.5+0.5*Math.sin(jitter))*0.8+(hi-lo)*0.2*r();};
 const build=(t)=>{
  const bid=new Map(),ask=new Map(),best={bid:mid,ask:mid+cfg.spread},elapsed=(t-t0)/1000;
  for(const side of ['bid','ask']){const m=side==='bid'?bid:ask;
   for(let d=0;d<cfg.levels;d++){
    const ticks=side==='bid'?best.bid-d:best.ask+d;
    if(cfg.sparse&&d>2&&hash(ticks,7)>cfg.sparse)continue;   // a thin book's empty prices stay empty
    let q=mmSize(d,ticks*0.7+step*0.05+(side==='bid'?0:1.3));
    const h=hash(ticks,side==='bid'?11:23);
    if(h<cfg.rest.p)q+=cfg.rest.lo+(cfg.rest.hi-cfg.rest.lo)*hash(ticks,5);
    if(ticks%cfg.rest.round===0)q+=cfg.rest.roundLo+(cfg.rest.roundHi-cfg.rest.roundLo)*hash(ticks,9);
    for(const [ws,off,size,from,until,pullWithin] of cfg.walls){
     if(ws!==side||Math.round(cfg.p0/tick)+off!==ticks||elapsed<from||elapsed>until)continue;
     const distance=Math.abs(ticks-(side==='bid'?best.bid:best.ask));
     if(pullWithin&&distance<=pullWithin)continue;   // spoof: gone as price approaches
     q+=size*(0.97+0.06*hash(step,3));
    }
    q=Math.max(cfg.minQty,+q.toPrecision(4));m.set(ticks,q);
   }}
  return {bid,ask,best};
 };
 const advance=t=>{
  step++;
  if(t>=regimeEnd){drift=(r()-0.5)*2*cfg.move*(r()<0.6?1:2.2);regimeEnd=t+cfg.regime*(0.4+r());}
  const before=mid;let moved=0;
  if(cfg.trend){trend+=cfg.trend;if(trend>=1){trend-=1;mid+=1;moved=1;}}
  if(!sweepLeft&&r()<cfg.sweep[0]){const total=cfg.sweep[1]+Math.floor(r()*(cfg.sweep[2]-cfg.sweep[1]));const steps=4+Math.floor(r()*8);
   sweepLeft=(r()<0.5?1:-1)*total;sweepStep=Math.max(1,Math.ceil(total/steps));}
  if(sweepLeft){moved=Math.sign(sweepLeft)*Math.min(Math.abs(sweepLeft),sweepStep);sweepLeft-=moved;mid+=moved;}
  else if(r()<cfg.move+Math.abs(drift)){moved=(r()<0.5+drift*3?1:-1)*(1+Math.floor(r()*cfg.jump));mid+=moved;}
  const next=build(t);
  // A move consumes the levels it passes: each prints its whole size, as a real sweep does.
  if(moved&&last){const side=moved>0?'ask':'bid',from=moved>0?last.best.ask:last.best.bid,to=moved>0?next.best.ask:next.best.bid;
   for(let k=from;moved>0?k<to:k>to;k+=moved>0?1:-1){const q=last[side].get(k);
    if(q>0)trades.push({t:t-95,price:fix(k*tick),qty:q,buy:moved>0,id:tradeId++});}}
  last=next;
  // Prints: at the touch, and through the swept levels on a move.
  const n=Math.floor(-Math.log(1-r())*cfg.trades*(moved?2.5:1));
  const offsets=Array.from({length:n},()=>Math.floor(r()*90)).sort((a,b)=>b-a);
  for(let k=0;k<n;k++){
   const buy=moved?moved>0:r()<0.5,px=buy?(moved>0?before+cfg.spread+Math.floor(r()*moved):mid+cfg.spread):(moved<0?before-Math.floor(r()*-moved):mid);
   let q=lognormal(cfg.trade.median,1.1);if(r()<cfg.trade.tail)q=cfg.trade.tailLo+(cfg.trade.tailHi-cfg.trade.tailLo)*r();
   trades.push({t:t-offsets[k],price:fix(px*tick),qty:+q.toPrecision(4),buy,id:tradeId++});
  }
  samples.push({t,bid:next.best.bid,ask:next.best.ask,book:next});
  if(samples.length>2000)samples.shift();
  return next;
 };
 return {cfg,tick,fix,advance,trades,samples,get mid(){return mid}};
}

// A fake refill engine: the prices the simulated touch held longest while it kept trading (at least 2.5 s), known
// 1.5 s after they first held, closed once price has been away for 3 s (crossed, or now and then pulled).
function refillLevels(sim,now){
 const seen=new Map();
 for(const s of sim.samples)for(const [side,p] of [['bid',s.bid],['ask',s.ask]]){
  const k=side+p,e=seen.get(k)||{side,price:p,first:s.t,last:s.t,n:0};e.last=s.t;e.n++;seen.set(k,e);}
 return [...seen.values()].filter(e=>e.n>=25).sort((a,b)=>b.n-a.n).slice(0,4).map(e=>{
  const h=hash(e.price,e.side==='bid'?4:6),price=sim.fix(e.price*sim.tick),closed=now-e.last>3000?e.last+500:0;
  const usd=price*(sim.cfg.symbol==='btcusdt'?12+h*30:50000+h*90000),refills=4+Math.floor(h*8);
  return {id:`${e.side}${e.price}`,side:e.side,price,started_ms:e.first,known_ms:e.first+1500,updated_ms:closed||now,closed_ms:closed,
   status:closed?(h<0.25?'pulled':'crossed'):'holding',executed_base:usd/price,executed_usd:usd,max_shown_base:usd/price/2.6,
   refilled_base:usd/price*0.7,refills,shown_ratio:2.6,refill_ms:Array.from({length:refills},(_,k)=>e.first+Math.round(((closed||now)-e.first)*(k+1)/(refills+1)))};
 });
}

const server=http.createServer((req,res)=>{
 res.setHeader('Cross-Origin-Opener-Policy','same-origin');res.setHeader('Cross-Origin-Embedder-Policy','require-corp');res.setHeader('Cache-Control','no-store');
 const pathname=new URL(req.url,'http://localhost').pathname,file=path.join(root,pathname==='/'||(pathname.startsWith('/terminal/')&&!path.extname(pathname))?'index.html':path.basename(pathname));
 if(!fs.existsSync(file)||fs.statSync(file).isDirectory()){res.setHeader('Content-Type','application/json');res.end('{}');return;}
 res.setHeader('Content-Type',file.endsWith('.html')?'text/html':file.endsWith('.js')?'text/javascript':file.endsWith('.wasm')?'application/wasm':'application/octet-stream');fs.createReadStream(file).pipe(res);
});
let current=null;   // the scenario the next connection plays
const sockets=new wsServer({server});
sockets.on('connection',ws=>{
 const cfg=SCENARIOS[current],pair={exchange:'binancef',symbol:cfg.symbol},subs=new Map();
 // 100 s of history before the page connects, then live at 100 ms.
 const start=Date.now()-100000,sim=market(cfg,start);
 let simT0=start;for(;simT0+100<=Date.now();simT0+=100)sim.advance(simT0+100);
 let prev={bid:new Map(),ask:new Map()},u=500,seeded=false,sentTrades=sim.trades.length,refillTimer=null;
 const sendBook=(t,next)=>{
  let b=cat(num(1,t),num(4,seeded?0:1),dbl(5,sim.fix(next.best.bid*sim.tick)),num(6,u+1),num(7,u+1),num(8,u));u++;
  for(const side of ['bid','ask']){const now=next[side],was=seeded?prev[side]:new Map(),field=side==='bid'?3:2;
   for(const [k,q] of now)if(was.get(k)!==q)b=cat(b,bytes(field,level(sim.fix(k*sim.tick),q)));
   for(const k of was.keys())if(!now.has(k))b=cat(b,bytes(field,level(sim.fix(k*sim.tick),0)));}
  seeded=true;prev={bid:next.bid,ask:next.ask};
  for(const d of subs.values())if(d.stream===3)ws.send(envelope(pair,3,d.timeframe||0,b,t));
 };
 ws.on('message',raw=>{const q=JSON.parse(raw);
  if(q.method==='subscribe'){subs.set(JSON.stringify(q.data),q.data);if(q.data.stream===3){seeded=false;}}
  if(q.method==='unsubscribe')subs.delete(JSON.stringify(q.data));
  if(q.method==='refill_subscribe'){
   const base={type:'refill_levels',exchange:pair.exchange,symbol:pair.symbol,mode:'live',version:'liquidity_response.refill.v1',reason:''};
   const send=()=>{if(ws.readyState===1)ws.send(JSON.stringify({...base,status:'observing',as_of_ms:Date.now()-300,levels:refillLevels(sim,Date.now())}));};
   send();refillTimer=setInterval(send,500);
  }
  if(q.method==='refill_unsubscribe'&&refillTimer){clearInterval(refillTimer);refillTimer=null;}
  if(q.method==='get_rt_history'){
   const end=q.data.end_ms,values=[];
   for(const s of sim.samples){if(s.t<end-90000||s.t>=end||s.t%500>=100)continue;
    const lv=[];for(const side of ['bid','ask']){let n=0;for(const [k,qq] of s.book[side]){if(n++>=250)break;lv.push(sim.fix(k*sim.tick),qq);}}
    values.push(1,s.t,7+lv.length,0,sim.fix(s.bid*sim.tick),sim.fix(s.ask*sim.tick),lv.length/2,...lv);}
   const packed=Buffer.alloc(values.length*8);values.forEach((n,i)=>packed.writeDoubleLE(n,i*8));
   let trades=Buffer.alloc(0);
   for(const tr of sim.trades)if(tr.t>=end-90000&&tr.t<end)trades=cat(trades,bytes(4,cat(dbl(1,tr.price),dbl(2,tr.qty),num(3,tr.buy?1:0),num(4,tr.t),num(5,tr.id))));
   ws.send(envelope(pair,36,0,cat(bytes(1,q.data.request_id),num(2,end),bytes(3,packed),trades),Date.now()));
  }
 });
 // The market steps every 100 ms of wall time, catching up after a late timer, so every build sees the same state
 // at the same moment after it connects.
 let simT=simT0;
 const timer=setInterval(()=>{if(ws.readyState!==1)return;let t=simT,next=null;
  while(simT+100<=Date.now()){simT+=100;t=simT;next=sim.advance(t);}
  if(!next)return;
  if([...subs.values()].some(d=>d.stream===3))sendBook(t,next);
  for(;sentTrades<sim.trades.length;sentTrades++){const tr=sim.trades[sentTrades];
   for(const d of subs.values())if(d.stream===1)ws.send(envelope(pair,1,0,cat(dbl(1,tr.price),dbl(2,tr.qty),num(3,tr.buy?1:0),num(4,tr.t),num(5,tr.id)),t));}
  for(const d of subs.values())if(d.stream===2){const p=sim.fix(sim.mid*sim.tick);
   ws.send(envelope(pair,2,d.timeframe||0,cat(dbl(1,p),dbl(2,p),dbl(3,p),dbl(4,p),dbl(5,20),num(10,Math.floor(t/60000)*60000),num(11,60)),t));}
 },100);
 ws.on('close',()=>{clearInterval(timer);if(refillTimer)clearInterval(refillTimer);});
});

async function open(browser,base,name,settings){
 const cfg=SCENARIOS[name];current=name;
 const page=await browser.newPage({viewport:{width:1920,height:1080}}),errors=[];
 page.on('pageerror',e=>errors.push(e.message));page.on('console',m=>{if(/IM_ASSERT|Assertion failed|User Error/.test(m.text()))errors.push(m.text())});
 await page.addInitScript(([s])=>{window.__EDGEDEPTH_TIER__='admin';window.__EDGEDEPTH_REPLAY_TOKEN__='fixture-only';window.__EDGEDEPTH_TEST_UI__=true;
  sessionStorage.setItem('edgedepth.workspace.tab.v1',JSON.stringify({version:1,layout:'',widgets:[{type:'chart',title:'',settings:{timeframe:60,chart_type:0,rt_mode:true,rt_span_ms:90000,rt_dom:true,...s}}]}));
 },[settings||{}]);
 await page.route('**/*',r=>new URL(r.request().url()).origin===base?r.continue():r.fulfill({contentType:'application/json',body:JSON.stringify(new URL(r.request().url()).pathname.includes('symbols/metadata')?{symbols:[{exchange:'binancef',symbol:cfg.symbol,tick_size:cfg.tick,step_size:cfg.minQty,base_asset:cfg.symbol.replace('usdt','').toUpperCase(),quote_asset:'USDT'}]}:{})}));
 await page.goto(`${base}/terminal/binancef/${cfg.symbol}?ws=${encodeURIComponent(base.replace('http:','ws:')+'/ws')}&realtime=1`);
 if(settings&&settings.rt_mode===false)await page.waitForTimeout(12000);   // a candle chart records no real-time archive
 else await page.waitForFunction(()=>window.Module?.rtArchive?.diagnostics().sessions.some(s=>s.bytes>0),null,{timeout:60000});
 return {page,errors};
}

(async()=>{
 await new Promise(r=>server.listen(0,'127.0.0.1',r));const base=`http://127.0.0.1:${server.address().port}`;
 const browser=await chromium.launch({headless:true,executablePath:'/usr/bin/google-chrome',args:['--no-sandbox','--enable-unsafe-swiftshader']});
 const result={};
 try{
  for(const name of (wanted.length?wanted:Object.keys(SCENARIOS))){
   const [scenario,variant]=name.split(':');
   const {page,errors}=await open(browser,base,scenario,variant==='contrast-off'?{rt_auto_contrast:false}:variant==='candles'?{rt_mode:false}:{});
   await page.mouse.move(760,560);await page.waitForTimeout(300);
   for(let k=0;k<+(process.env.ZOOM_OUT||4);k++){await page.mouse.wheel(0,120);await page.waitForTimeout(150);}
   await page.mouse.move(5,5);
   await page.waitForTimeout((liveSeconds ?? (scenario==='btc-spoof'?150:70))*1000);
   await page.screenshot({path:path.join(evidence,`${name.replace(':','-')}.png`)});
   const marks=await page.evaluate(()=>window.__EDGEDEPTH_UI_RECTS__||{});
   const walls=Object.keys(marks).filter(k=>k.startsWith('rt.wall.')),pulls=Object.keys(marks).filter(k=>k.startsWith('rt.pull.'));
   const refill=Object.keys(marks).filter(k=>k.startsWith('rt.refill.'));
   const diameters=Object.keys(marks).filter(k=>k.startsWith('rt.bubble.')).map(k=>marks[k][2]-marks[k][0]).sort((a,b)=>a-b);
   result[name]={refillMarks:refill.length,walls:walls.length,pulls:pulls.length,
    bidWalls:walls.filter(k=>k.includes('.bid.')).length,askWalls:walls.filter(k=>k.includes('.ask.')).length,
    bubbleCount:diameters.length,bubbleP90:diameters[Math.floor((diameters.length-1)*0.9)]||0,bubbleMax:diameters.at(-1)||0,errors};
   if(!shotOnly&&variant!=='candles'){
    if(scenario==='btc-burst'){
     assert.ok(diameters.length>=20,'busy BTC still shows significant executed groups');
     assert.ok(result[name].bubbleP90<24,'ordinary BTC groups must not fill the chart with oversized bubbles');
    }
    if(scenario==='thin')assert.ok(diameters.length>0,'thin altcoin bubbles remain visible');
    assert.ok(result[name].bidWalls<=3&&result[name].askWalls<=3,`${name}: at most three walls a side`);
    assert.ok(pulls.length<=12,`${name}: bounded pull display`);
    for(const key of pulls){const r=marks[key];assert.ok(r[2]-r[0]>=7&&r[2]-r[0]<=14,`${name}: pull size stays readable and capped`);}
    for(let i=0;i<pulls.length;i++)for(let j=i+1;j<pulls.length;j++){const a=marks[pulls[i]],b=marks[pulls[j]];
     assert.ok(a[2]+3.8<=b[0]||b[2]+3.8<=a[0]||a[3]+3.8<=b[1]||b[3]+3.8<=a[1],`${name}: differently sized pull squares never overlap`);}
    if(scenario==='btc-busy'||scenario==='btc-quiet')assert.ok(refill.length>0,`${name}: refill levels marked on the trades`);
    if(scenario==='pull-history'){
     assert.ok(pulls.length>6, 'pull history keeps more than the old six-event display');
     const sizes=pulls.map(k=>marks[k][2]-marks[k][0]);
     assert.ok(Math.max(...sizes)-Math.min(...sizes)>3, 'larger pulls are visibly more prominent');
    }
    if(scenario==='btc-spoof'){
     assert.equal(pulls.length,1,'btc-spoof: the cancelled ask is the one pull');
     assert.ok(result[name].bidWalls>=1,'btc-spoof: the standing bid wall is marked');
    }
    if(scenario==='btc-spoof'||scenario==='pull-history'){
     // Freeze through the user-facing control before inspecting a moving glyph.
     const named=async key=>{const r=await page.evaluate(k=>window.__EDGEDEPTH_UI_RECTS__?.[k],key);
      assert.ok(r,`${name}: ${key} is available`);await page.mouse.click((r[0]+r[2])/2,(r[1]+r[3])/2);await page.waitForTimeout(250);};
     await named('chart.settings');await named('rt.pause');await page.keyboard.press('Escape');await page.waitForTimeout(300);
     const frozen=await page.evaluate(()=>window.__EDGEDEPTH_UI_RECTS__||{});
     const p=frozen[pulls[0]];assert.ok(p,'pulled wall remains available while paused');await page.mouse.move((p[0]+p[2])/2,(p[1]+p[3])/2);await page.waitForTimeout(400);
     await page.screenshot({path:path.join(evidence,`${name}-pull-hover.png`)});
     // Inspect the visible edge mark; the heatmap row itself no longer captures hover.
     if(scenario==='btc-spoof'){
      const w=frozen[walls.find(k=>k.includes('.bid.'))];await page.mouse.move((w[0]+w[2])/2,(w[1]+w[3])/2);await page.waitForTimeout(400);
      await page.screenshot({path:path.join(evidence,`${name}-wall-hover.png`)});
     }
    }
   }
   if(!shotOnly&&scenario==='btc-burst'){
    const named=async key=>{await page.waitForFunction(k=>window.__EDGEDEPTH_UI_RECTS__?.[k],key);
     const r=await page.evaluate(k=>window.__EDGEDEPTH_UI_RECTS__[k],key);
     await page.mouse.move((r[0]+r[2])/2,(r[1]+r[3])/2);await page.waitForTimeout(100);
     await page.mouse.click((r[0]+r[2])/2,(r[1]+r[3])/2);await page.mouse.move(5,5);await page.waitForTimeout(200);};
    const settings=()=>page.evaluate(()=>{Module._workspace_flush();return JSON.parse(sessionStorage.getItem('edgedepth.workspace.tab.v1')).widgets.find(w=>w.type==='chart').settings;});
    await named('chart.layers');await named('layer.Trade bubbles.settings');
    assert.equal((await settings()).rt_auto_bubbles,true,'market calibration defaults on');
    await named('rt.bubbles.auto');assert.equal((await settings()).rt_auto_bubbles,false,'manual choice is saved');
    assert.ok(!(await page.evaluate(()=>window.__EDGEDEPTH_UI_RECTS__))['rt.bubbles.recalibrate'],'manual mode hides auto recalibration');
    await page.screenshot({path:path.join(evidence,'bubble-settings-manual.png')});
    await named('rt.bubbles.auto');assert.equal((await settings()).rt_auto_bubbles,true,'auto choice is restored');
    await named('rt.bubbles.recalibrate');
    assert.equal((await settings()).rt_bubble_cap,24,'recalibration preserves the larger size cap');
    await page.screenshot({path:path.join(evidence,'bubble-settings-auto.png')});
    await page.keyboard.press('Escape');
   }
   if(!shotOnly)assert.deepEqual(errors,[],`${name}: no page errors`);
   await page.close();
  }
  fs.writeFileSync(path.join(evidence,'result.json'),JSON.stringify(result,null,2));
  console.log(JSON.stringify(result));
 }finally{await browser.close();for(const ws of sockets.clients)ws.terminate();await new Promise(r=>sockets.close(r));await new Promise(r=>server.close(r));}
})().catch(e=>{console.error(e);process.exitCode=1});
