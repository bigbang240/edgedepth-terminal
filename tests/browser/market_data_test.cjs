// Run a completed native WASM build served with COOP/COEP headers.
// NODE_PATH=<Playwright and pngjs modules> node tests/browser/market_data_test.cjs <base URL> <evidence directory>
// All market and research values below are synthetic fixtures, never live claims.
const assert = require('node:assert/strict');
const path = require('node:path');
const fs=require('fs');
const {chromium}=require('playwright');
const {PNG}=require('pngjs');
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
 if(d.stream===4)b=cat(dbl(1,.7027),dbl(2,-.000132),num(3,t),dbl(8,110500),dbl(9,157300),dbl(14,20250000),num(15,t+680000));
 if(d.stream===16)b=cat(dbl(3,.43),dbl(7,110500),dbl(8,157300),num(17,t));
 if(d.stream===29)b=cat(bytes(1,cat(bytes(1,'RLCUSDT'),dbl(2,p),dbl(3,1.4),dbl(4,218580000),num(5,t))),num(2,t));
 if(b)ws.send(envelope(d.pair,d.stream,d.timeframe,b,t));
 }},20);ws.on('close',()=>clearInterval(timer))});

(async () => {
 const base=process.argv[2] || 'http://127.0.0.1:8795';
 const evidence=process.argv[3]; if(!evidence)throw new Error('Provide an evidence directory'); fs.mkdirSync(evidence,{recursive:true});
 const browser=await chromium.launch({headless:true,executablePath:process.env.CHROME_BIN || '/usr/bin/google-chrome',args:['--no-sandbox','--enable-unsafe-swiftshader']});
 try {
 const page=await browser.newPage({viewport:{width:1600,height:900}}); const errors=[],reads=[];
 page.on('pageerror',e=>errors.push(e.message)); page.on('console',m=>{if(m.type()==='error'||/IM_ASSERT|User Error|Assertion failed/.test(m.text()))errors.push(m.text())});
 let advance=0, contextMode='current', historyShift=0, ratioMissing=false;
 await page.addInitScript(()=>{window.__EDGEDEPTH_TIER__='pro';window.__EDGEDEPTH_TEST_UI__=true;window.testOpened=[];window.open=url=>{window.testOpened.push(url);return null};window.testOffset=0;Date.now=()=>new Date().getTime()+window.testOffset});
 let rejectHistory=false, lockedHistory=false, legacyHistory=false;
 await page.route('**/*',r=>{
  const u=new URL(r.request().url()),now=Date.now()+advance,to=Math.floor(now/300000)*300000+historyShift,from=to-86400000;
  if(u.pathname==='/api/terminal/market-data'){
   reads.push(u.search);if(lockedHistory)return r.fulfill({status:403,body:'{}'});if(rejectHistory)return r.fulfill({status:503,body:'{}'});
   return r.fulfill({contentType:'application/json',body:JSON.stringify({v:1,venue:'binancef',symbol:u.searchParams.get('symbol'),source:'binance',status:'ok',from,to,step:300000,generated_at:now,server_now:now,series:legacyHistory?undefined:{
    oi_market_cap:Array.from({length:289},(_,i)=>ratioMissing||i===268?null:.18+i*.0002+Math.sin(i*.18)*.009),
    top_long_accounts:Array.from({length:289},(_,i)=>.54-i*.0001+Math.sin(i*.17)*.025),
    taker_buy_volume:Array.from({length:289},(_,i)=>i===271?null:70000+Math.abs(Math.sin(i*.41))*220000),
    taker_sell_volume:Array.from({length:289},(_,i)=>i===271?null:60000+Math.abs(Math.cos(i*.31))*260000)
   },points:Array.from({length:289},(_,i)=>[from+i*300000,i===271?null:.65+i*.0003+Math.sin(i*.25)*.008,i===268?null:20000000+i*19000,.48-i*.0002+Math.sin(i*.13)*.015])})});
  }
  if(u.pathname==='/api/terminal/research') {
   const observed=Math.floor((now-60000)/60000)*60000;
   const rows=[
    ['feature.ret_15m','Return (15m)','Price and volatility','percent',-.027,'-2.70%','down'],
    ['feature.ret_1h','Return (1h)','Price and volatility','percent',.0753,'+7.53%','up'],
    ['feature.rv_1h','Realized vol (1h)','Price and volatility','percent',.0219,'2.19%',null],
    ['feature.range_1h','Range (1h)','Price and volatility','percent',.19,'19.00%',null],
    ['feature.taker_buy_ratio_15m','Taker buy share (15m)','Trade flow','fraction',.47,'47.0%','down'],
    ['feature.vpin','Toxic flow (VPIN)','Trade flow','fraction',.26,'0.26',null],
    ['feature.vpin_regime','VPIN regime','Trade flow','label','normal','normal',null],
    ['feature.volume_rank','Volume rank (own history)','Trade flow','percentile',.97,'97th pctl',null],
    ['feature.book_imbalance','Book imbalance (top levels)','Order book','fraction',.54,'77% BID','up'],
    ['feature.spread_bps','Spread (bps)','Order book','bps',2.03,'2.03 bps',null],
    ['feature.spread_normalized','Spread (normalized)','Order book','fraction',0,'0.00',null],
    ['feature.book_imbalance_pctrank','Book imbalance percentile','Order book','percentile',.07,'7th pctl',null],
    ['feature.oi_contract_change_1h','Open interest change (1h)','Funding and positioning','percent',.078,'+7.8%','up'],
    ['feature.funding_rate','Funding rate (latest)','Funding and positioning','percent',-.00013,'-0.013%','down'],
    ['feature.global_long_ratio','Long account share','Funding and positioning','fraction',.6496,'64.96%',null],
    ['feature.oi_mcap_ratio','OI / market cap','Funding and positioning','percent',.5684,'56.84%',null],
    ['feature.candle_body_share_15m','Body share (15m)','Candle formations','percent',-.294,'-29.4%','down'],
    ['feature.candle_compression_1h','Compression (1h)','Candle formations','ratio',1.81,'1.81x',null],
    ['feature.candle_engulfing_15m','Engulfing (15m)','Candle formations','label','none','none',null],
    ['feature.liq_notional_usd_1h','Liquidated notional (1h)','Liquidations','USD',97000,'$97K',null],
    ['feature.liq_long_notional_usd_1h','Longs liquidated (1h)','Liquidations','USD',25000,'$25K',null],
    ['feature.liq_short_notional_usd_1h','Shorts liquidated (1h)','Liquidations','USD',72000,'$72K',null],
    ['feature.coil_state_1h','Coiling line state (1h)','Coiling line','label',null,null,null],
    ['feature.illiquidity','Amihud illiquidity (1h)','Trade flow','per $1M',null,null,null],
   ];
   return r.fulfill({contentType:'application/json',body:JSON.stringify({v:1,venue:'binancef',symbol:u.searchParams.get('symbol'),feature_version:'feature_defs.v1',feature_edition:'original_live.v1',dataset_revision:'a'.repeat(64),observed_at:observed,calculated_at:observed+65000,valid_until:observed+180000,server_now:Math.max(now,observed+65000),features:rows.map(([id,name,group,unit,value,display,tone])=>({id,name,group,unit,value,display,tone,dtype:typeof value==='string'||id.includes('coil_')?'enum':'number',source:'original_live',description:id==='feature.ret_15m'?'Price return across the trailing 15 minutes ending at the recorded minute. A past move, not a forecast.':'Recorded '+name.toLowerCase()+'. The stated window belongs to this measurement.',availability:value!==null?'present':id.includes('coil_')?'history_only':'unavailable'}))})});
  }
  if(u.pathname==='/api/terminal/market-context')return r.fulfill({contentType:'application/json',body:JSON.stringify({v:1,venue:'binancef',symbol:u.searchParams.get('symbol'),server_now:now,status:'ok',items:contextMode==='missing'?[]:[
   {id:'readings',value:'Price +3.6% / OI -5.8% / RV 0.7%',help:'Recorded one-hour price, open contracts and realised volatility.',tone:'neutral',at:now-60000,until:now+480000,metrics:[.036,-.058,.007]},
   {id:'regime',value:'Quiet / Flat',detail:'Four-hour background from the latest hourly publication.',help:'Descriptive context, not a directional forecast.',tone:'neutral',at:now-900000,until:now+1800000},
   {id:'flow',value:'No active 15m detector match',detail:'Requires a 5% price move, 3% OI drop and liquidation confirmation in the same 15m.',help:'Liquidations can occur without a match.',tone:'neutral',at:now-60000,until:now+180000},
   {id:'cascade',value:'Top 5%',detail:'Research preview / relative tier, not a probability.',help:'Keep this source clock separate from the price chart cursor.',tone:'warn',at:now-600000,until:now+1200000},
   {id:'awake',value:'Volume awake / 3.8x',detail:'24h turnover relative to the usual day.',help:'Registered direction test failed. Unusual activity does not establish direction.',tone:'neutral',at:now-300000,until:now+900000} ]})});
  if(u.origin===new URL(base).origin)return r.continue();
  return r.fulfill({contentType:'application/json',body:JSON.stringify(u.pathname.includes('symbols/metadata')?{symbols:[{exchange:'binancef',symbol:'rlcusdt',tick_size:.0001,step_size:1,base_asset:'RLC',quote_asset:'USDT'}]}:{})});
 });
 const route=`${base}/terminal/binancef/rlcusdt?ws=ws://127.0.0.1:${server.address().port}&realtime=1&view=data&range=4h`;
 const settle=()=>page.waitForTimeout(350);
 const state=()=>page.evaluate(()=>{Module._workspace_flush();return JSON.parse(sessionStorage.getItem('edgedepth.workspace.tab.v1'))});
 const shot=async name=>{await settle();const buf=await page.screenshot({path:path.join(evidence,name+'.png')});return PNG.sync.read(buf)};
 // Controls by name (core/ui_test_marks.h): their rects as the terminal last drew them.
 const rects=()=>page.evaluate(()=>window.__EDGEDEPTH_UI_RECTS__||{});
 const region=async name=>{const r=(await rects())[name];assert.ok(r,'UI mark missing: '+name);return r.map(Math.round)};
 const centre=async name=>{const r=await region(name);return [(r[0]+r[2])/2,(r[1]+r[3])/2]};
 const press=async name=>{const [x,y]=await centre(name);await click(x,y)};
 const hover=async name=>{const [x,y]=await centre(name);await page.mouse.move(x,y);await settle()};
 const reveal=async name=>{for(let i=0;i<8;i++){const r=(await rects())[name];if(r&&r[1]>90&&r[3]<860)return;await page.mouse.move(800,600);await page.mouse.wheel(0,r?Math.max(-1200,Math.min(1200,r[1]-450)):600);await settle()}assert.fail('Could not scroll to '+name)};
 // Rows to scan for pin cursors: the first chart's plot (its marked rect), set before use.
 let cursorRows=[560,780];
 const cursors=png=>{
  const found=[];
  for(let x=0;x<png.width;x++){let count=0;for(let y=cursorRows[0];y<cursorRows[1];y++){
   const i=(y*png.width+x)*4,[r,g,b]=png.data.subarray(i,i+3);
   if(r>=30&&r<=90&&g>=100&&g<=240&&g>r*2&&b>=g*.65&&b<=g*.9)count++;
  }if(count>30 && (!found.length || x>found.at(-1)+4))found.push(x)}
  return found;
 };
 const fresh=async()=>{await page.reload();await page.waitForFunction(()=>typeof window.Module?._workspace_flush==='function');await page.waitForTimeout(4500)};
 const click=async(x,y)=>{await page.mouse.click(x,y,{delay:100});await settle()};
 const top=async()=>{await page.mouse.move(1200,600);await page.mouse.wheel(0,-6000);await settle()};
 const refresh=async()=>{await click(140,93);assert.equal(await page.evaluate(()=>Object.keys(Module.terminalRequests||{}).length),0,'hidden Data cancels requests');await click(337,93)};
 await page.goto(route.replace('&view=data&range=4h',''));
 await page.waitForFunction(()=>typeof window.Module?._workspace_flush==='function');await page.waitForTimeout(6500);
 assert.equal((await state()).widgets.filter(w=>w.type==='data').length,0);
 await shot('visible-data-entry');await click(1428,60);
 assert.equal(new URL(page.url()).searchParams.get('view'),'data','Strip button directly opens Data');
 assert.equal((await state()).widgets.filter(w=>w.type==='data').length,1);
 await shot('docked-with-watchlist');await click(274,94);await page.mouse.move(1000,90);
 const overview=await shot('overview-1600');
 const signedPixels=(png,x1,x2,y1,y2)=>{
  let up=0,down=0;
  for(let y=y1;y<y2;y++)for(let x=x1;x<x2;x++){
   const i=(y*png.width+x)*4,[r,g,b]=png.data.subarray(i,i+3);
   if(g>r*1.6&&g>b*1.1&&g>120)up++;
   if(r>g*1.6&&r>b*1.1&&r>120)down++;
  }return {up,down};
 };
 const p0=await region('data.reading_part.0'),p1=await region('data.reading_part.1'),p2=await region('data.reading_part.2');
 const summaryColors=signedPixels(overview,p0[0],p1[2],p0[1],p0[3]+3);
 assert.ok(summaryColors.up>15&&summaryColors.down>15,'Price/OI numeric signs render in semantic colors');
 const rvColors=signedPixels(overview,p2[0],p2[2],p2[1],p2[3]+3);
 assert.equal(rvColors.up+rvColors.down,0,'RV level stays neutral');
 assert.ok(reads.length>0);
 const dock=(doc,id)=>doc.layout.split('[Window]['+id+']')[1]?.split('[Window]')[0]?.match(/DockId=(0x[0-9A-F]+)/)?.[1];
 let s=await state();assert.ok(dock(s,'Data binancef rlcusdt'));assert.equal(dock(s,'Data binancef rlcusdt'),dock(s,'chart_binancef_rlcusdt'));
 {const r=await region('data.chart.0.plot');cursorRows=[Math.max(0,r[1]),Math.min(899,r[3])];}
 let ratio=await centre('data.chart.2.plot');await page.mouse.move(...ratio);await shot('ratio-hover');await click(...ratio);await page.mouse.move(1000,90);
 const pinned=cursors(await shot('ratio-pinned'));assert.ok(pinned.length>=3,'Pin draws linked cursors');
 await page.mouse.move(300,650);assert.deepEqual(cursors(await shot('pin-wins-over-hover')),pinned);
 await page.keyboard.press('ArrowLeft');const stepped=cursors(await shot('pin-stepped'));assert.notDeepEqual(stepped,pinned,'Focused arrow moves pin');
 await click(1530,60);await shot('market-details');await page.keyboard.press('Escape');assert.deepEqual(cursors(await shot('popup-escape')),stepped,'Popup dismissal preserves pin');
 await reveal('data.actions.investigate');await shot('pinned-research-actions');
 await press('data.actions.investigate');await press('data.actions.leadup');
 const chartOpened=await page.evaluate(()=>window.testOpened);assert.equal(chartOpened.length,2,'Both pinned-minute research buttons open');
 const chartInvestigation=new URL(chartOpened[0]),chartLeadup=new URL(chartOpened[1]);
 assert.equal(chartLeadup.searchParams.get('mfields'),'feature.oi_mcap_ratio');
 assert.equal(chartInvestigation.searchParams.get('moment'),'rlcusdt,'+chartLeadup.searchParams.get('at'));
 const pinnedAt=Date.parse(chartLeadup.searchParams.get('at'));assert.equal(pinnedAt%300000,0);assert.ok(pinnedAt<Date.now()-3600000,'Handoff keeps historical pin, not latest research minute');
 await top();ratio=await centre('data.chart.2.plot');await click(...ratio);await page.mouse.move(1000,90);await page.keyboard.press('Escape');assert.equal(cursors(await shot('unpin')).length,0);
 await press('data.chart.2.expand');await page.mouse.move(1000,90);await shot('ratio-expanded');await press('data.chart.2.expand');await top();
 ratio=await centre('data.chart.2.plot');await click(...ratio);await page.mouse.move(1000,90);assert.ok(cursors(await shot('ratio-collapsed')).length>=3,'Collapse restores the linked grid');await page.keyboard.press('Escape');
 const [ix,iy]=await centre('data.chart.2.info');await page.mouse.move(ix,iy);const help=await shot('ratio-explanation-tooltip');await page.mouse.move(1000,90);const noHelp=await shot('tooltip-dismissed');
 let tooltipPixels=0;for(let y=Math.round(iy);y<Math.min(help.height,Math.round(iy)+330);y++)for(let x=Math.max(0,Math.round(ix)-560);x<help.width;x++){const i=(y*help.width+x)*4;if(Math.abs(help.data[i]-noHelp.data[i])+Math.abs(help.data[i+1]-noHelp.data[i+1])+Math.abs(help.data[i+2]-noHelp.data[i+2])>30)tooltipPixels++;}
 assert.ok(tooltipPixels>1000,'Hover shows a readable explanation; leaving dismisses it');
 await press('range.24h');assert.equal((await state()).widgets.find(w=>w.type==='data').settings.hours,24);assert.equal(new URL(page.url()).searchParams.get('range'),'24h');
 await press('units.Quote value');assert.equal((await state()).widgets.find(w=>w.type==='data').settings.oi_usd,true);
 await refresh();await page.goBack();await settle();assert.equal(new URL(page.url()).searchParams.get('view'),null);
 await page.goForward();await settle();assert.equal(new URL(page.url()).searchParams.get('view'),'data');
 await fresh();s=await state();assert.equal(s.widgets.find(w=>w.type==='data').settings.hours,24);assert.equal(s.widgets.find(w=>w.type==='data').settings.oi_usd,true);assert.equal(s.widgets.filter(w=>w.type==='data').length,1);
 await shot('restored-data');await press('range.4h');await press('units.Contracts');
 await press('data.source_clocks');await shot('source-clocks');await press('data.source_clocks');
 await press('data_view.Readings');assert.equal((await state()).widgets.find(w=>w.type==='data').settings.readings_view,true);await page.mouse.move(1000,90);await shot('readings-1600');
 await press('data.reading.feature.ret_15m');await page.mouse.move(1000,90);await shot('reading-detail-1600');
 await press('data.actions.investigate');await press('data.actions.leadup');
 const opened=await page.evaluate(()=>window.testOpened);assert.equal(opened.length,2);
 const investigation=new URL(opened[0]),leadup=new URL(opened[1]);
 assert.equal(investigation.pathname,'/research/workbench');assert.equal(leadup.pathname,'/research/symbols/rlcusdt');
 assert.equal(investigation.searchParams.get('mfields'),'feature.ret_15m');assert.equal(leadup.searchParams.get('mfields'),'feature.ret_15m');
 assert.equal(investigation.searchParams.get('moment'),'rlcusdt,'+leadup.searchParams.get('at'));assert.equal(leadup.searchParams.get('leadup'),'30');
 assert.ok(!investigation.searchParams.has('run')&&!leadup.searchParams.has('run'));
 await press('data.reading.close');await press('data.readings.missing');await page.mouse.move(1000,90);await shot('readings-missing');
 await press('data.readings.search');await page.keyboard.type('coil');await shot('readings-search-missing');await page.keyboard.press('Control+a');await page.keyboard.press('Backspace');await press('data.readings.missing');
 await fresh();assert.equal((await state()).widgets.find(w=>w.type==='data').settings.readings_view,true);await shot('restored-readings');await press('data_view.Overview');
 await reveal('data.saved_research');await shot('current-research');await press('data.saved_research');assert.equal(await page.evaluate(()=>window.testOpened.at(-1)),'https://edgedepth.com/research/workbench?tab=saved');await top();
 await press('data.context.1.details');await shot('research-inspector');await top();
 await page.setViewportSize({width:1200,height:900});await page.waitForTimeout(700);await top();await shot('overview-1200');await press('data_view.Readings');await shot('readings-1200');await press('data_view.Overview');
 await page.setViewportSize({width:650,height:900});await page.waitForTimeout(700);await page.mouse.move(500,600);await page.mouse.wheel(0,-6000);await shot('overview-650');
 await page.mouse.wheel(0,600);await shot('charts-650');await page.mouse.wheel(0,-6000);await settle();await press('data_view.Readings');await shot('readings-650');await press('data_view.Overview');
 await page.setViewportSize({width:352,height:900});await page.waitForTimeout(700);await page.mouse.move(250,600);await page.mouse.wheel(0,-6000);await shot('overview-352');await press('data_view.Readings');await shot('readings-352');
 await page.setViewportSize({width:3406,height:1325});await page.waitForTimeout(700);await top();await press('data_view.Overview');await page.mouse.move(700,90);await shot('overview-ultrawide');await press('data_view.Readings');assert.equal((await state()).widgets.find(w=>w.type==='data').settings.readings_view,true);await page.mouse.move(700,90);await shot('readings-ultrawide');
 await page.setViewportSize({width:1600,height:900});await page.waitForTimeout(700);await fresh();await press('data_view.Overview');
 contextMode='missing';await refresh();await shot('withdrawn-research');
 advance=7200000;await page.evaluate(()=>window.testOffset=7200000);await page.waitForTimeout(600);await shot('expired-research');
 advance=0;await page.evaluate(()=>window.testOffset=0);contextMode='current';await refresh();
 legacyHistory=true;await refresh();await shot('optional-series-missing');legacyHistory=false;ratioMissing=true;await refresh();await shot('ratio-missing-price-retained');ratioMissing=false;
 rejectHistory=true;await refresh();await shot('history-error');rejectHistory=false;await refresh();
 lockedHistory=true;await refresh();await shot('access-denied');lockedHistory=false;await refresh();
 const cdp=await page.context().newCDPSession(page);await cdp.send('Emulation.setTouchEmulationEnabled',{enabled:true});
 await top();const [tx,ty]=await centre('data.chart.2.plot');await cdp.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[{x:tx,y:ty}]});await cdp.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});await page.mouse.move(1000,90);assert.ok(cursors(await shot('touch-pin')).length>=3,'Touch tap pins');
 await cdp.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[{x:500,y:650}]});
 for(let y=620;y>=350;y-=30)await cdp.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[{x:500,y}]});
 await cdp.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});await shot('touch-scroll');
 await page.addInitScript(()=>{window.__EDGEDEPTH_TIER__='free'});const freeReads=reads.length;await fresh();await shot('free-plan');assert.equal(reads.length,freeReads);
 await page.addInitScript(()=>{window.__EDGEDEPTH_TIER__='pro'});
 const beforeUnsupported=reads.length;await page.goto(route.replace('/binancef/','/bybit/'));await page.waitForFunction(()=>typeof window.Module?._workspace_flush==='function');await page.waitForTimeout(4500);await shot('unsupported-venue');assert.ok(reads.slice(beforeUnsupported).every(q=>new URLSearchParams(q).get('venue')!=='bybit'),'Unsupported venue makes no history request for that venue');
 assert.deepEqual(errors,[
  'Failed to load resource: the server responded with a status of 503 (Service Unavailable)',
  'Failed to load resource: the server responded with a status of 403 (Forbidden)'
 ],'Only injected HTTP failures may reach the console');
 fs.writeFileSync(path.join(evidence,'workspace.json'),JSON.stringify(s,null,2));
 fs.writeFileSync(path.join(evidence,'handoffs.json'),JSON.stringify({chart:chartOpened,reading:opened},null,2));
 console.log('PASS: direct entry, dock identity, linked pin/hover/keys/popup, range, units, navigation, restoration, grouped readings, exact-minute handoffs, source states, optional series, errors, access and touch. Synthetic fixtures.');
 }finally{await browser.close();server.close()}
})().catch(e=>{console.error(e);server.close();process.exitCode=1});
