// Local synthetic ORCA feed through the production WASM renderer.
// NODE_PATH=<Playwright modules> node tests/browser/liquidation_diamonds_test.cjs <build dir> <evidence dir>
// EXPECT_LEGACY=1 reproduces the older $8k/$2382 size inversion.
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),http=require('node:http');
const {chromium}=require('playwright');
const {wsServer,PNG}=require('playwright-core/lib/utilsBundle');
const [root,evidence]=process.argv.slice(2);fs.mkdirSync(evidence,{recursive:true});
const cat=(...b)=>Buffer.concat(b),v=n=>{n=BigInt(n);const a=[];do{a.push(Number(n&127n)|(n>127n?128:0));n>>=7n;}while(n);return Buffer.from(a);};
const num=(i,n)=>cat(v(i*8),v(n)),bytes=(i,b)=>{b=Buffer.from(b);return cat(v(i*8+2),v(b.length),b)},dbl=(i,n)=>{const b=Buffer.alloc(8);b.writeDoubleLE(n);return cat(v(i*8+1),b)};
const pair={exchange:'binancef',symbol:'orcausdt'},start=Date.now();
const envelope=(stream,tf,b,t)=>cat(bytes(1,cat(bytes(1,pair.exchange),bytes(2,pair.symbol))),num(2,stream),num(3,tf),bytes(4,b),num(5,t));
const level=(p,q)=>cat(dbl(1,p),dbl(2,q));
const depth=t=>{const a=[1,t,7+80*2,0,100,100.1,80];for(let k=0;k<40;k++)a.push(100-k*.1,20+k%9*30,100.1+k*.1,30+k%7*40);return a;};
const reports=[...Array.from({length:19},(_,i)=>({amount:300,t:start-140000+i*1000,p:100.05})),
 {amount:8000,t:start-23000,p:102},{amount:300,t:start-18500,p:102},{amount:2382.37,t:start-14000,p:102},{amount:5000,t:start-9500,p:102},
 {amount:4000,t:start-5000,p:102},{amount:4262.55,t:start-5000,p:102}];
const server=http.createServer((req,res)=>{
 res.setHeader('Cross-Origin-Opener-Policy','same-origin');res.setHeader('Cross-Origin-Embedder-Policy','require-corp');res.setHeader('Cache-Control','no-store');
 const pathname=new URL(req.url,'http://localhost').pathname,file=path.join(root,pathname==='/'||(pathname.startsWith('/terminal/')&&!path.extname(pathname))?'index.html':path.basename(pathname));
 if(!fs.existsSync(file)||fs.statSync(file).isDirectory()){res.setHeader('Content-Type','application/json');res.end('{}');return;}
 res.setHeader('Content-Type',file.endsWith('.html')?'text/html':file.endsWith('.js')?'text/javascript':file.endsWith('.wasm')?'application/wasm':'application/octet-stream');fs.createReadStream(file).pipe(res);
});
const sockets=new wsServer({server});let sent=0;
sockets.on('connection',ws=>{const subs=new Map();let sequence=100,bookSequence=100,seeded=false,liqSeeded=false;
 ws.on('message',raw=>{const q=JSON.parse(raw);
  if(q.method==='subscribe')subs.set(JSON.stringify(q.data),q.data);
  if(q.method==='unsubscribe')subs.delete(JSON.stringify(q.data));
  if(q.method==='get_rt_history'){
   const values=[];for(let t=q.data.end_ms-90000;t<q.data.end_ms;t+=500)values.push(...depth(t));
   const packed=Buffer.alloc(values.length*8);values.forEach((n,i)=>packed.writeDoubleLE(n,i*8));
   ws.send(envelope(36,0,cat(bytes(1,q.data.request_id),num(2,q.data.end_ms),bytes(3,packed)),Date.now()));
  }
 });
 const timer=setInterval(()=>{if(ws.readyState!==1)return;const t=Date.now();for(const d of subs.values()){
  let b;
  if(d.stream===3){b=cat(num(1,t),num(4,seeded?0:1),dbl(5,100.05),num(6,bookSequence+1),num(7,bookSequence+1),num(8,bookSequence));bookSequence++;seeded=true;for(let k=0;k<40;k++)b=cat(b,bytes(3,level(100-k*.1,20+k%9*30)),bytes(2,level(100.1+k*.1,30+k%7*40)));}
  if(d.stream===1)b=cat(dbl(1,100.05),dbl(2,2),num(3,sequence%2),num(4,t),num(5,sequence++));
  if(d.stream===2)b=cat(dbl(1,100),dbl(2,101),dbl(3,99),dbl(4,100.05),dbl(5,20),num(10,Math.floor(t/60000)*60000),num(11,60));
  if(d.stream===5&&!liqSeeded){liqSeeded=true;for(const r of reports){ws.send(envelope(5,0,cat(num(1,r.t),dbl(2,r.p),dbl(3,r.p),dbl(4,r.amount/r.p),num(5,1)),r.t));sent++;}}
  if(b)ws.send(envelope(d.stream,d.timeframe||0,b,t));
 }},100);ws.on('close',()=>clearInterval(timer));
});
(async()=>{
 await new Promise(r=>server.listen(0,'127.0.0.1',r));const base=`http://127.0.0.1:${server.address().port}`;
 const browser=await chromium.launch({headless:true,executablePath:'/usr/bin/google-chrome',args:['--no-sandbox','--enable-unsafe-swiftshader']});
 try{
  const page=await browser.newPage({viewport:{width:1600,height:900}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));page.on('console',m=>{if(/IM_ASSERT|Assertion failed|User Error/.test(m.text()))errors.push(m.text())});
  await page.addInitScript(()=>{window.__EDGEDEPTH_TIER__='admin';window.__EDGEDEPTH_REPLAY_TOKEN__='fixture-only';
   sessionStorage.setItem('edgedepth.workspace.tab.v1',JSON.stringify({version:1,layout:'',widgets:[{type:'chart',title:'',settings:{timeframe:60,chart_type:0,rt_mode:true,rt_span_ms:90000,rt_depth:true,rt_bubbles:false,rt_trade_line:false,rt_dom:false,liq_observed_enabled:true,liq_markers:true,liq_obs_auto_size:true,rt_liq_strip:true,rt_liq_collapsed:false}}]}));
  });
  await page.route('**/*',r=>new URL(r.request().url()).origin===base?r.continue():r.fulfill({contentType:'application/json',body:JSON.stringify(new URL(r.request().url()).pathname.includes('symbols/metadata')?{symbols:[{...pair,tick_size:.1,step_size:.001,base_asset:'ORCA',quote_asset:'USDT'}]}:{})}));
  await page.goto(`${base}/terminal/binancef/orcausdt?ws=${encodeURIComponent(base.replace('http:','ws:')+'/ws')}&realtime=1`);
  await page.waitForFunction(()=>window.Module?.rtArchive?.diagnostics().sessions.some(s=>s.bytes>0),null,{timeout:60000});
  await page.waitForTimeout(3000);assert.equal(sent,reports.length);
  // Pan off the live edge so tooltip inspection uses stationary diamonds.
  await page.mouse.move(800,250);await page.mouse.down();await page.mouse.move(850,250,{steps:8});await page.mouse.up();
  await page.waitForTimeout(200);await page.mouse.move(5,5);
  const png=PNG.sync.read(await page.screenshot({path:path.join(evidence,'diamonds.png')}));
  const state=await page.evaluate(()=>{Module._workspace_flush();return JSON.parse(sessionStorage.getItem('edgedepth.workspace.tab.v1'))});
  fs.writeFileSync(path.join(evidence,'workspace.json'),JSON.stringify(state,null,2));
  // Connected green components above the quote line isolate the five diamonds.
  const seen=new Uint8Array(png.width*png.height),components=[];
  const green=(x,y)=>{const i=(y*png.width+x)*4,[r,g,b]=png.data.subarray(i,i+3);return g>120&&g>r*1.5&&b>80&&g>b*1.03;};
  for(let y=170;y<700;y++)for(let x=100;x<1500;x++){
   if(seen[y*png.width+x]||!green(x,y))continue;
   const stack=[[x,y]],pts=[];seen[y*png.width+x]=1;
   while(stack.length){const [a,b]=stack.pop();pts.push([a,b]);for(const [dx,dy]of[[1,0],[-1,0],[0,1],[0,-1],[1,1],[-1,-1],[1,-1],[-1,1]]){
    const c=a+dx,d=b+dy,i=d*png.width+c;if(c<100||c>=1500||d<170||d>=700||seen[i]||!green(c,d))continue;seen[i]=1;stack.push([c,d]);
   }}
   const xs=pts.map(p=>p[0]),ys=pts.map(p=>p[1]),box={x:Math.min(...xs),y:Math.min(...ys),w:Math.max(...xs)-Math.min(...xs)+1,h:Math.max(...ys)-Math.min(...ys)+1,pixels:pts.length};
   if(box.w>=7&&box.h>=7&&box.w<55&&Math.abs(box.w-box.h)<=3)components.push(box);
  }
  components.sort((a,b)=>a.x-b.x);console.log(JSON.stringify({components,errors,sent}));
  fs.writeFileSync(path.join(evidence,'result.json'),JSON.stringify({legacy:!!process.env.EXPECT_LEGACY,components,errors,sent},null,2));
  if(components.length===5){
   const [eight,small,two,five,group]=components;
   if(process.env.EXPECT_LEGACY)assert.ok(eight.w<two.w,'legacy reproduces inverted $8k/$2382 sizes');
   else{assert.ok(eight.w>two.w,'$8k must be larger than $2382');assert.ok(five.w>small.w*2,'$5k must clearly exceed $300');assert.ok(group.w>=eight.w,'$8262 group must use its combined total');}
   await page.mouse.move(group.x+group.w/2,group.y+group.h/2);await page.waitForTimeout(450);await page.screenshot({path:path.join(evidence,'group-tooltip.png')});
   await page.keyboard.down('Alt');await page.waitForTimeout(250);await page.screenshot({path:path.join(evidence,'reference-tooltip.png')});await page.keyboard.up('Alt');
  }else throw new Error(`Expected five diamond components, got ${components.length}`);
  if(process.env.CHECK_RT_MENU){
   const settings=()=>page.evaluate(()=>{Module._workspace_flush();return JSON.parse(sessionStorage.getItem('edgedepth.workspace.tab.v1')).widgets.find(w=>w.type==='chart').settings});
   const click=async(x,y)=>{await page.mouse.click(x,y);await page.mouse.move(5,5);await page.waitForTimeout(200);};
   await click(403,125);await page.screenshot({path:path.join(evidence,'menu-wide.png')});
   await click(400,297);assert.equal((await settings()).rt_dom,true,'depth ladder control');
   await click(400,297);assert.equal((await settings()).rt_dom,false);
   await click(400,255);assert.ok((await settings()).rt_span_ms>1800000,'whole-session navigation');
   await click(403,125);await click(400,165); // Pause before returning live.
   await click(400,225);assert.ok(Math.abs((await settings()).rt_span_ms-30000/.88)<1,'return-live navigation includes right margin');
   await click(403,125);await page.screenshot({path:path.join(evidence,'menu-returned-live.png')});
   await click(400,370);await page.screenshot({path:path.join(evidence,'session-history.png')});
   await page.keyboard.press('Escape');await click(1540,125);
   await page.screenshot({path:path.join(evidence,'settings-wide.png')});
   await page.keyboard.press('Escape');await click(592,125);
   await page.screenshot({path:path.join(evidence,'layers-wide.png')});
   await click(650,556);let s=await settings();
   assert.equal(s.rt_liq_observed_enabled,false);assert.equal(s.liq_observed_enabled,true,'RT switch changed Chart reports');
   await click(650,556);await click(650,394);s=await settings();
   assert.equal(s.rt_bubbles,true);assert.equal(s.candle_bubbles,false,'RT switch changed Chart bubbles');
   await click(650,394);await page.keyboard.press('Escape');
   await page.setViewportSize({width:700,height:800});await page.waitForTimeout(400);
   await click(194,163);await page.screenshot({path:path.join(evidence,'menu-narrow.png')});
   fs.writeFileSync(path.join(evidence,'menu-settings.json'),JSON.stringify(await settings(),null,2));
   console.log('PASS: RT menu navigation, panel visibility and independent layer switches');
  }
  assert.deepEqual(errors,[]);console.log(process.env.EXPECT_LEGACY?'PASS: legacy ORCA size inversion reproduced':'PASS: rendered ORCA diamond size ordering and grouped total');
 }finally{await browser.close();for(const ws of sockets.clients)ws.terminate();await new Promise(r=>sockets.close(r));await new Promise(r=>server.close(r));}
})().catch(e=>{console.error(e);process.exitCode=1});
