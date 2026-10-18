// Production WASM with synthetic candles/V2 frames: default layers, saved overrides and visual QA.
// NODE_PATH=<Playwright modules> node tests/browser/liqmap2_presentation_test.cjs <build dir> <evidence dir>
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),http=require('node:http');
const {chromium}=require('playwright');const {wsServer,PNG}=require('playwright-core/lib/utilsBundle');
const [root,evidence]=process.argv.slice(2);fs.mkdirSync(evidence,{recursive:true});
const cat=(...b)=>Buffer.concat(b),v=n=>{n=BigInt(n);const a=[];do{a.push(Number(n&127n)|(n>127n?128:0));n>>=7n;}while(n);return Buffer.from(a);};
const num=(i,n)=>cat(v(i*8),v(n)),bytes=(i,b)=>{b=Buffer.from(b);return cat(v(i*8+2),v(b.length),b)},dbl=(i,n)=>{const b=Buffer.alloc(8);b.writeDoubleLE(n);return cat(v(i*8+1),b)};
const start=Math.floor(Date.now()/60000)*60000,pair={exchange:'binancef',symbol:'wusdt'};
let lowDollar=false,railsUnknown=false,railsSubs=0,railsHistory=0,railsVenues=new Set();
const pbpair=cat(bytes(1,pair.exchange),bytes(2,pair.symbol));
const envelope=(stream,tf,b,t=start)=>cat(bytes(1,pbpair),num(2,stream),num(3,tf),bytes(4,b),num(5,t));
const price=i=>lowDollar?.0173+.00012*Math.sin(i/7)-.00009*Math.cos(i/12):.014+.000018*i+.00016*Math.sin(i/7)-.0002*Math.cos(i/12);
const candle=i=>{const open=price(i),close=price(i+1),high=Math.max(open,close)+.000035,low=Math.min(open,close)-.000025;return cat(dbl(1,open),dbl(2,high),dbl(3,low),dbl(4,close),dbl(5,1e7),dbl(6,6e6),dbl(7,4e6),num(10,start-(180-i)*60000),num(11,60),num(12,1))};
function frame(i,expected=false){const ts=start-(180-i)*60000,mark=price(i+1),step=Math.log(1.0025),center=Math.floor(Math.log(mark)/step);let p=[],l=[],s=[],tl=0,tsd=0;
 for(let k=center-80;k<center+73;k++){const px=Math.exp((k+.5)*step),d=Math.abs(px/mark-1),usd=lowDollar?(432.5*(.4+.6*Math.min(1,d/.02))+15800*Math.exp(-Math.pow((d-.18)/.006,2))):4e4*(1-Math.exp(-d*40))*Math.exp(-d*25)*(1+.05*Math.sin(k));p.push(dbl(5,px));l.push(dbl(6,px<mark?usd:0));s.push(dbl(7,px>mark?usd:0));if(px<mark)tl+=usd;else tsd+=usd;}
 let b=cat(bytes(1,pbpair),num(2,ts),dbl(3,mark),dbl(4,.25),...p,...l,...s,dbl(8,tl),dbl(9,tsd),bytes(10,'ready'),num(11,start-30*864e5),num(12,ts),bytes(13,'mid'),dbl(14,1),bytes(15,'synthetic-v2-provisional'),num(16,1));
 if(expected){for(let k=1;k<=40;k++)b=cat(b,dbl(19,3500*Math.exp(-k/10)),dbl(20,5000*Math.exp(-k/12)));b=cat(b,bytes(21,'active'),bytes(24,'synthetic-v22'));}return b;}
// Rails preview (stream 40): 12 rails a side within 4% of the mark, dollars over two orders of magnitude; with
// railsUnknown no rail has a chance yet (expected -1), as on most live markets on 2026-10-10: it painted every
// rail the dimmest step.
// One map per coin across venues: the chart's venue and its sibling (bybit) each send rails; the sibling's mark sits 0.1%
// above (a basis the chart maps out) and its rails sit at other prices. Each rail carries 1 h and 4 h figures (11 to 14).
// Eight separate rails a side (zones 0.4% apart) plus, beyond them, a ladder of five whose zones touch, which must draw
// as one rail (core/liqmap2_rails.h runs).
// Born three minutes ago, so the candles since have not reached them (candles consume rails), except one long rail
// at the price the last minutes traded through, which the chart must cut at the touching candle.
const railMsg=ex=>{const sib=ex!==pair.exchange,mark=price(181)*(sib?1.001:1),born=start-3*60000;const rs=[];
 const add=(id,long,d,usd,k)=>{const px=mark*(long?1-d:1+d),known=!railsUnknown,chance=known?Math.max(.05,.6-.05*k):-1,c1=known?chance*.2:-1,c4=known?chance*.5:-1;
  rs.push(bytes(14,cat(num(1,id),num(2,long?1:0),dbl(3,px),num(4,born),dbl(5,px*.999),dbl(6,px*1.001),dbl(7,usd),dbl(8,chance),dbl(9,known?usd*chance:-1),num(10,born),dbl(11,c1),dbl(12,known?usd*c1:-1),dbl(13,c4),dbl(14,known?usd*c4:-1))));};
 for(const long of [true,false]){for(let k=1;k<=8;k++)add(k*2+(long?0:1)+(sib?1000:0),long,.012+.006*(k-1)+(sib?.003:0),800*Math.pow(1.8,k),k);
  for(let j=0;j<5;j++)add(100+j*2+(long?0:1)+(sib?1000:0),long,.07+.002*j+(sib?.001:0),400,9);}
 if(!sib)add(999,true,-(price(179)/mark-1)+.0001,5000,1);
 // Born two hours ago above the price then; the rising candles later reach it, so the chart must end it there.
 if(!sib){const px=price(60)*1.02,b2=start-120*60000;rs.push(bytes(14,cat(num(1,998),num(2,0),dbl(3,px),num(4,b2),dbl(5,px*.999),dbl(6,px*1.001),dbl(7,20000),dbl(8,-1),dbl(9,-1),num(10,b2))));}
 return cat(bytes(1,cat(bytes(1,ex),bytes(2,pair.symbol))),num(2,Date.now()),dbl(3,mark),bytes(4,'ready'),bytes(5,'synthetic-rails'),bytes(6,'v1'),bytes(7,'mid'),num(8,sib?1:0),num(9,railsUnknown?0:1),...rs);};
const candles=()=>cat(num(1,60),...Array.from({length:180},(_,i)=>bytes(2,candle(i))));
const frames=()=>cat(bytes(1,pbpair),num(2,start-180*60000),num(3,start),...Array.from({length:181},(_,i)=>bytes(4,frame(i))));
const palette=png=>{let violet=0,orange=0,yellow=0;for(let y=200;y<png.height-100;y++)for(let x=100;x<png.width-130;x++){const i=(y*png.width+x)*4,[r,g,b]=png.data.subarray(i,i+3);if(r>30&&b>30&&g<r*.55&&g<b*.55)violet++;if(r>110&&g>40&&g<r*.75&&b<70)orange++;if(r>130&&g>110&&b<g*.85)yellow++;}return {violet,orange,yellow};};
const server=http.createServer((req,res)=>{res.setHeader('Cross-Origin-Opener-Policy','same-origin');res.setHeader('Cross-Origin-Embedder-Policy','require-corp');res.setHeader('Cache-Control','no-store');const pathname=new URL(req.url,'http://localhost').pathname,file=path.join(root,pathname==='/'||(pathname.startsWith('/terminal/')&&!path.extname(pathname))?'index.html':path.basename(pathname));if(!fs.existsSync(file)||fs.statSync(file).isDirectory()){res.setHeader('Content-Type','application/json');res.end('{}');return;}res.setHeader('Content-Type',file.endsWith('.html')?'text/html':file.endsWith('.js')?'text/javascript':file.endsWith('.wasm')?'application/wasm':'application/octet-stream');fs.createReadStream(file).pipe(res);});
const sockets=new wsServer({server});let receivedFrames=0,liveMapSubscriptions=0;let useExpected=false;
sockets.on('connection',ws=>{const subs=new Map();let sequence=100,seeded=false;
 ws.on('message',raw=>{const q=JSON.parse(raw);if(q.method==='subscribe'){subs.set(JSON.stringify(q.data),q.data);if(q.data.stream===38)liveMapSubscriptions++;if(q.data.stream===40){railsSubs++;railsVenues.add((q.data.pair||pair).exchange);}}if(q.method==='get_historical_liqmap2_rails')railsHistory++;if(q.method==='unsubscribe')subs.delete(JSON.stringify(q.data));
 if(q.method==='get_liqmap2_scorecard')ws.send(JSON.stringify({type:'liqmap2_scorecard',...pair,window_days:28,days:3,minutes_scored:4000,realized_source:'lower_bound',predicted_long_usd:140000,predicted_short_usd:59100,realized_long_reached_usd:20000,realized_short_reached_usd:9400}));
 if(q.method==='get_historical_candles')ws.send(envelope(8,60,candles()));
 if(q.method==='get_historical_liqmap2'){ws.send(envelope(39,0,frames()));receivedFrames++;}
 });
 const timer=setInterval(()=>{if(ws.readyState!==1)return;for(const d of subs.values()){let b;const t=Date.now();
 if(d.stream===1)b=cat(dbl(1,price(181)),dbl(2,100),num(3,sequence%2),num(4,t),num(5,sequence++));
 if(d.stream===2)b=candle(180);
 if(d.stream===38)b=frame(180,useExpected);
 if(d.stream===40){const ex=(d.pair||pair).exchange;ws.send(cat(bytes(1,cat(bytes(1,ex),bytes(2,pair.symbol))),num(2,40),num(3,0),bytes(4,railMsg(ex)),num(5,t)));continue;}
 if(d.stream===5&&!seeded){seeded=true;for(let i=20;i<180;i+=11){const ts=start-(180-i)*60000;ws.send(envelope(5,0,cat(num(1,ts),dbl(2,price(i)),dbl(3,price(i)),dbl(4,(1000+i*20)/price(i)),num(5,i%2)),ts));}}
 if(b)ws.send(envelope(d.stream,d.timeframe||0,b,t));}},500);ws.on('close',()=>clearInterval(timer));
});
(async()=>{await new Promise(r=>server.listen(0,'127.0.0.1',r));const base=`http://127.0.0.1:${server.address().port}`;
 const browser=await chromium.launch({headless:true,executablePath:'/usr/bin/google-chrome',args:['--no-sandbox','--enable-unsafe-swiftshader']});
 try{const results=[];
 for(const name of ['low-dollar','defaults','legacy','legacy-rt','saved','saved-off','free','legacy-free','released-pro','released-research','defaults-rt','legacy-rt-on','saved-split','saved-split-rt']){lowDollar=false;railsUnknown=name==='low-dollar';  // the banned band heatmap's narrow low-dollar path no longer draws for admins
 railsSubs=0;railsHistory=0;railsVenues=new Set();const beforeFrames=receivedFrames,beforeSubs=liveMapSubscriptions;const page=await browser.newPage({viewport:{width:1600,height:900}}),errors=[];
 page.on('pageerror',e=>errors.push(e.message));page.on('console',m=>{if(/IM_ASSERT|Assertion failed|User Error/.test(m.text()))errors.push(m.text());});
 await page.addInitScript(({name})=>{window.__EDGEDEPTH_TIER__=name.endsWith('free')?'free':name==='released-pro'?'pro':name==='released-research'?'research':'admin';window.__EDGEDEPTH_REPLAY_TOKEN__='fixture-only';const settings={timeframe:60,chart_type:0,rt_mode:['legacy-rt','legacy-rt-on','defaults-rt','saved-split-rt'].includes(name),liq_dense_field:true};
 if(name.startsWith('legacy'))Object.assign(settings,{rt_bubbles:true,candle_bubbles:true,liq_observed_enabled:false,rt_liq_strip:false,candle_liq_strip:false});
 if(name==='saved'||name==='saved-off')Object.assign(settings,{chart_layer_defaults:1,rt_bubbles:name==='saved',candle_bubbles:name==='saved',liq_observed_enabled:false,liqmap2_rails:true,liqmap2_profile:true,liqmap2_history:false});
 if(name==='legacy-rt-on')Object.assign(settings,{rt_bubbles:false,liq_observed_enabled:true,rt_liq_strip:true});
 if(name.startsWith('saved-split'))Object.assign(settings,{chart_layer_defaults:2,rt_bubbles:false,candle_bubbles:true,liq_observed_enabled:false,rt_liq_observed_enabled:true,rt_liq_strip:true,candle_liq_strip:false,liqmap2_rails:true,liqmap2_profile:true,liqmap2_history:false});if(!sessionStorage.getItem('edgedepth.workspace.tab.v1'))sessionStorage.setItem('edgedepth.workspace.tab.v1',JSON.stringify({version:1,layout:'',widgets:[{type:'chart',title:'',settings}]}));},{name});
 await page.route('**/*',r=>new URL(r.request().url()).origin===base?r.continue():r.fulfill({contentType:'application/json',body:JSON.stringify(new URL(r.request().url()).pathname.includes('symbols/metadata')?{symbols:[{...pair,tick_size:.000001,step_size:1,base_asset:'W',quote_asset:'USDT'}]}:{})}));
 await page.goto(`${base}/terminal/binancef/wusdt?ws=${encodeURIComponent(base.replace('http:','ws:')+'/ws')}`);
 await page.waitForFunction(()=>typeof Module?._workspace_flush==='function',null,{timeout:60000});await page.waitForTimeout(3000);
 const state=await page.evaluate(()=>{Module._workspace_flush();return JSON.parse(sessionStorage.getItem('edgedepth.workspace.tab.v1'));});
 const s=state.widgets.find(w=>w.type==='chart').settings;
 const saved=name.startsWith('saved');if(!process.env.EXPECT_LEGACY)assert.equal(s.chart_layer_defaults,3);
 // The rejected heatmap's own keys (liqmap2_rails/profile/history) are gone with its draw path (2026-10-11).
 assert.equal(s.liqmap2_rails,undefined);assert.equal(s.liqmap2_history,undefined);
 const rtBubbles=!(name==='saved-off'||name==='legacy-rt-on'||name.startsWith('saved-split'));
 const rtReports=name==='legacy-rt-on'||name.startsWith('saved-split');
 assert.equal(s.rt_bubbles,rtBubbles,'candle defaults changed RT bubbles');assert.equal(s.candle_bubbles,name==='saved'||name.startsWith('saved-split'));
 assert.equal(s.liq_observed_enabled,!saved&&!name.endsWith('free'));assert.equal(s.rt_liq_observed_enabled,rtReports,'candle reports leaked into RT');
 assert.equal(s.rt_liq_strip,rtReports,'candle defaults changed RT subplot');
 if(name.startsWith('legacy')){
 assert.equal(s.rt_liq_strip,rtReports);assert.equal(s.candle_liq_strip,true);
 // Reload the migrated document: the version and resulting choices must persist.
 await page.reload();await page.waitForFunction(()=>typeof Module?._workspace_flush==='function',null,{timeout:60000});await page.waitForTimeout(1000);
 const again=await page.evaluate(()=>{Module._workspace_flush();return JSON.parse(sessionStorage.getItem('edgedepth.workspace.tab.v1')).widgets.find(w=>w.type==='chart').settings});
 assert.equal(again.chart_layer_defaults,3);assert.equal(again.rt_bubbles,rtBubbles);assert.equal(again.rt_liq_observed_enabled,rtReports);assert.equal(again.rt_liq_strip,rtReports);assert.equal(again.candle_bubbles,false);assert.equal(again.liq_observed_enabled,!name.endsWith('free'));
 }
 if(name==='defaults'||name==='saved-split'){
 // The same widget must retain different report switches when changing modes.
 await page.mouse.click(320,125);await page.waitForTimeout(500);
 const switched=await page.evaluate(()=>{Module._workspace_flush();return JSON.parse(sessionStorage.getItem('edgedepth.workspace.tab.v1')).widgets.find(w=>w.type==='chart').settings});
 assert.equal(switched.rt_mode,true,'mode switch did not enter RT');assert.equal(switched.liq_observed_enabled,s.liq_observed_enabled);assert.equal(switched.rt_liq_observed_enabled,rtReports);assert.equal(switched.rt_bubbles,rtBubbles);
 await page.screenshot({path:path.join(evidence,name+'-switched-rt.png')});
 await page.mouse.click(320,125);await page.waitForTimeout(400);
 const returned=await page.evaluate(()=>{Module._workspace_flush();return JSON.parse(sessionStorage.getItem('edgedepth.workspace.tab.v1')).widgets.find(w=>w.type==='chart').settings});
 assert.equal(returned.rt_mode,false);assert.equal(returned.liq_observed_enabled,s.liq_observed_enabled);assert.equal(returned.rt_liq_observed_enabled,rtReports);
 }
 if(name==='low-dollar'){
 // Admins see the rails only (the heatmap cloud and step lines are banned, backend docs/LIQMAP_VISUAL_RULE.md).
 // No rail has a chance yet: the view must colour by dollars, not paint every rail the dimmest step.
 await page.mouse.move(5,5);await page.waitForTimeout(800);const png=PNG.sync.read(await page.screenshot({path:path.join(evidence,'rails-unknown-chance.png')}));
 const pal=palette(png);fs.writeFileSync(path.join(evidence,'rails-unknown-measurement.json'),JSON.stringify(pal));
 assert.ok(railsSubs>0&&railsHistory>0,'admin rails stream not requested');
 assert.ok(pal.violet>50&&pal.orange>50&&pal.yellow>20,`Rails with unknown chances not multi-coloured: ${JSON.stringify(pal)}`);
 await page.mouse.move(850,550);await page.waitForTimeout(300);await page.screenshot({path:path.join(evidence,'rails-hover.png')});
 results.push({name,palette:pal,settings:s,errors});
 }else if(name==='defaults'){assert.equal(s.rt_liq_strip,false);assert.equal(s.candle_liq_strip,true);assert.equal(s.liq_markers,false);
 await page.mouse.move(5,5);await page.waitForTimeout(800);const png=PNG.sync.read(await page.screenshot({path:path.join(evidence,'rails-preview.png')}));
 const pal=palette(png);assert.ok(railsVenues.has('binancef')&&railsVenues.has('bybit')&&railsHistory>=2,`one map per coin: rails of both venues not requested (${[...railsVenues]}, ${railsHistory})`);
 assert.ok(pal.violet>50&&pal.orange>50&&pal.yellow>20,`Missing rails palette colours: ${JSON.stringify(pal)}`);
 await page.setViewportSize({width:700,height:800});await page.waitForTimeout(400);await page.screenshot({path:path.join(evidence,'rails-narrow.png')});
 results.push({name,palette:pal,settings:s,errors});
 }else {if(name==='saved')await page.screenshot({path:path.join(evidence,'optional-rails-profile.png')});results.push({name,settings:s,errors});}
 // Released 2026-10-11: free off BTC is a locked row (no rails, no frames); Pro and Research get the rails of both
 // venues; customers never subscribe to the frames (only admins' touch-odds fuel reads them).
 if(name.endsWith('free')||name.startsWith('released')){assert.equal(receivedFrames,beforeFrames,'customer requested frame history');assert.equal(liveMapSubscriptions,beforeSubs,'customer subscribed to frames');}
 if(name.endsWith('free'))assert.equal(railsSubs,0,'locked market subscribed to rails');
 if(name.startsWith('released')){await page.waitForTimeout(800);assert.ok(railsVenues.has('binancef')&&railsVenues.has('bybit')&&railsHistory>=2,`released rails not requested on both venues (${[...railsVenues]}, ${railsHistory})`);
  await page.mouse.move(5,5);await page.waitForTimeout(600);const png=PNG.sync.read(await page.screenshot({path:path.join(evidence,name+'-rails.png')}));const pal=palette(png);
  assert.ok(pal.violet>50&&pal.orange>50,`released rails not drawn: ${JSON.stringify(pal)}`);}
 assert.deepEqual(errors,[]);await page.close();}
 fs.writeFileSync(path.join(evidence,'browser-checks.json'),JSON.stringify(results,null,2));if(!process.env.EXPECT_LEGACY)console.log('PASS: rails for admins and released Pro/Research (multi-colour, both venues, unknown chances fall back to dollars), locked free market without subscriptions, candle-only defaults/migration, independent RT choices and mode switches, wide/narrow rendering');
 }finally{await browser.close();sockets.close();server.close();}
})().catch(e=>{console.error(e);process.exit(1)});
