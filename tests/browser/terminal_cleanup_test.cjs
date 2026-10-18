// Actual WASM with synthetic candles and depth. No production data or account changes.
// NODE_PATH=<Playwright modules> node tests/browser/terminal_cleanup_test.cjs <build dir> <evidence dir>
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),http=require('node:http');
const {chromium}=require('playwright'),{wsServer,PNG}=require('playwright-core/lib/utilsBundle');
const [root,evidence]=process.argv.slice(2);assert.ok(root&&evidence);fs.mkdirSync(evidence,{recursive:true});
const cat=(...b)=>Buffer.concat(b),v=n=>{n=BigInt(n);const a=[];do{a.push(Number(n&127n)|(n>127n?128:0));n>>=7n;}while(n);return Buffer.from(a);};
const num=(i,n)=>cat(v(i*8),v(n)),bytes=(i,b)=>{b=Buffer.from(b);return cat(v(i*8+2),v(b.length),b)},dbl=(i,n)=>{const b=Buffer.alloc(8);b.writeDoubleLE(n);return cat(v(i*8+1),b)};
const pair={exchange:'binancef',symbol:'btcusdt'},pbpair=cat(bytes(1,pair.exchange),bytes(2,pair.symbol));
const envelope=(stream,tf,b)=>cat(bytes(1,pbpair),num(2,stream),num(3,tf),bytes(4,b),num(5,Date.now()));
const requests=[];
function candles(tf){const end=Math.floor(Date.now()/(tf*1000))*tf*1000,rows=[];for(let i=0;i<240;i++){
 const p=83000+Math.sin(i/11)*180;rows.push(bytes(2,cat(dbl(1,p),dbl(2,p+90),dbl(3,p-60),dbl(4,p+30),dbl(5,10+i%11),dbl(6,8),dbl(7,4),num(10,end-(240-i)*tf*1000),num(11,tf),num(12,1))));}return cat(num(1,tf),...rows);}
function depth(q){const step=q.timeframe*1000,rows=[];for(let t=Math.floor(q.start_time/step)*step;t<=q.end_time;t+=step){
 const levels=[];for(let k=0;k<120;k++)levels.push(dbl(7,82500+k*10),dbl(8,2+k%7),dbl(9,3+k%5));
 rows.push(bytes(1,cat(num(1,Math.min(t+step-60000,q.end_time)),bytes(2,pair.symbol),bytes(3,pair.exchange),bytes(4,q.mode),dbl(5,.1),dbl(6,10),...levels,dbl(10,82500),dbl(11,83700))));}return cat(...rows);}
const server=http.createServer((req,res)=>{res.setHeader('Cross-Origin-Opener-Policy','same-origin');res.setHeader('Cross-Origin-Embedder-Policy','require-corp');res.setHeader('Cache-Control','no-store');
 const pathname=new URL(req.url,'http://local').pathname,file=path.join(root,pathname==='/'||(pathname.startsWith('/terminal/')&&!path.extname(pathname))?'index.html':path.basename(pathname));
 if(!fs.existsSync(file)||fs.statSync(file).isDirectory()){res.setHeader('Content-Type','application/json');res.end('{}');return;}
 res.setHeader('Content-Type',file.endsWith('.html')?'text/html':file.endsWith('.js')?'text/javascript':file.endsWith('.wasm')?'application/wasm':'application/octet-stream');fs.createReadStream(file).pipe(res);});
const sockets=new wsServer({server});
sockets.on('connection',ws=>{ws.on('message',raw=>{const q=JSON.parse(raw);if(q.method==='get_historical_candles')ws.send(envelope(8,q.data.timeframe,candles(q.data.timeframe)));
 if(q.method==='get_historical_heatmap'){requests.push(q.data);ws.send(envelope(13,q.data.timeframe,depth(q.data)));}});});
(async()=>{await new Promise(r=>server.listen(0,'127.0.0.1',r));const base=`http://127.0.0.1:${server.address().port}`;
 const browser=await chromium.launch({headless:true,executablePath:process.env.CHROME_BIN||'/usr/bin/google-chrome',args:['--no-sandbox','--enable-unsafe-swiftshader']});
 try{const page=await browser.newPage({viewport:{width:1800,height:1100}}),errors=[];
 page.on('pageerror',e=>errors.push(e.message));page.on('console',m=>{if(/IM_ASSERT|Assertion failed|User Error/.test(m.text()))errors.push(m.text());});
 await page.addInitScript(()=>{window.__EDGEDEPTH_TIER__='admin';window.__EDGEDEPTH_TEST_UI__=true;
 if(!sessionStorage.getItem('seeded')){sessionStorage.setItem('seeded','1');sessionStorage.setItem('edgedepth.workspace.tab.v1',JSON.stringify({version:1,layout:'',widgets:[{type:'chart',title:'',settings:{timeframe:300,chart_type:0,rt_mode:false,heatmap_enabled:true,chart_layer_defaults:2,liq_dense_field:false,liqmap2_rails:false,liqmap2_profile:false,liq_observed_enabled:false,candle_liq_strip:false,indicators:[{name:'Vol (USDT)',height:160},{name:'CVD',height:160},{name:'RSI',height:160}]}},{type:'watchlist',title:'',settings:{}}]}));}});
 const symbols=Array.from({length:1219},(_,i)=>({...pair,symbol:i?'test'+i+'usdt':'btcusdt',tick_size:.1,step_size:.001,base_asset:i?'TEST'+i:'BTC',quote_asset:'USDT'}));
 await page.route('**/*',r=>new URL(r.request().url()).origin===base?r.continue():r.fulfill({contentType:'application/json',body:JSON.stringify(new URL(r.request().url()).pathname.includes('symbols/metadata')?{symbols}:{})}));
 const ready=async()=>{await page.waitForFunction(()=>typeof window.Module?._workspace_flush==='function');await page.waitForTimeout(2200);};
 const settle=()=>page.waitForTimeout(400),rects=()=>page.evaluate(()=>window.__EDGEDEPTH_UI_RECTS__||{});
 const state=()=>page.evaluate(()=>{Module._workspace_flush();return JSON.parse(sessionStorage.getItem('edgedepth.workspace.tab.v1')).widgets.find(w=>w.type==='chart').settings;});
 const names=async()=>(await state()).indicators.map(i=>i.name);
 const press=async(name)=>{const r=(await rects())[name];assert.ok(r,'missing control '+name);await page.mouse.click((r[0]+r[2])/2,(r[1]+r[3])/2);await settle();};
 const shot=async(name)=>{await page.mouse.move(200,35);await settle();return PNG.sync.read(await page.screenshot({path:path.join(evidence,name+'.png')}));};
 await page.goto(`${base}/terminal/binancef/btcusdt?ws=${base.replace('http:','ws:')}/ws`);await ready();
 assert.deepEqual(await names(),['Vol (USDT)','CVD','RSI']);
 for(const name of await names())assert.ok((await rects())['indicator.close.'+name],'every panel has a close control');
 await shot('three-panels');
 await press('indicator.up.RSI');assert.deepEqual(await names(),['Vol (USDT)','RSI','CVD']);
 const order=await rects();assert.ok(order['indicator.close.RSI'][1]<order['indicator.close.CVD'][1],'middle and bottom visually swapped');
 await shot('bottom-moved-to-middle');await page.reload();await ready();assert.deepEqual(await names(),['Vol (USDT)','RSI','CVD']);
 await press('indicator.down.RSI');assert.deepEqual(await names(),['Vol (USDT)','CVD','RSI']);
 await press('indicator.up.Vol (USDT)');assert.deepEqual(await names(),['Vol (USDT)','CVD','RSI'],'top move up is disabled');
 await press('indicator.close.RSI');assert.deepEqual(await names(),['Vol (USDT)','CVD'],'bottom X removes only that panel');
 await page.reload();await ready();assert.deepEqual(await names(),['Vol (USDT)','CVD']);
 await press('indicator.close.Vol (USDT)');assert.deepEqual(await names(),['CVD'],'top X also removes only that panel');
 await press('indicator.close.CVD');assert.deepEqual(await names(),[]);
 for(const tf of [300,900,14400]){
  requests.length=0;await page.evaluate(tf=>Module.__set_chart_timeframe(tf),tf);await page.waitForTimeout(3000);
  const png=await shot('depth-'+tf);let colored=0;for(let y=240;y<640;y++)for(let x=400;x<1200;x++){const p=(y*png.width+x)*4;const [r,g,b]=png.data.subarray(p,p+3);if(b>r+25&&g>r+20)colored++;}
  assert.ok(colored>10000,'depth renders at '+tf+' seconds: '+colored);
  await page.mouse.move(900,450);await page.mouse.wheel(0,-550);await page.waitForTimeout(2400);await page.mouse.wheel(0,850);await page.waitForTimeout(2400);
  assert.ok(requests.length||tf===300,'timeframe change requests depth');assert.ok(requests.every(r=>r.timeframe===tf),'zoom must not request a different depth cadence');
 }
 await page.setViewportSize({width:1400,height:1000});await shot('narrow-watchlist');
 assert.deepEqual(errors,[]);fs.writeFileSync(path.join(evidence,'result.json'),JSON.stringify({passed:true,errors},null,2));
 console.log('PASS: adjacent panel swaps, disabled edge, reload, individual removal, 5m/15m/4h depth and zoom cadence');
 }finally{await browser.close();for(const ws of sockets.clients)ws.terminate();await new Promise(r=>sockets.close(r));await new Promise(r=>server.close(r));}
})().catch(e=>{console.error(e);for(const ws of sockets.clients)ws.terminate();sockets.close();server.close();process.exitCode=1;});
