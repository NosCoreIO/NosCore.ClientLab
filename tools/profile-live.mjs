import {logger} from './logger.mjs';
// Read-only CDP profiling of an existing ClientLab session. Never closes the game.
import {mkdirSync,writeFileSync,readFileSync} from 'node:fs';
import {join} from 'node:path';
const arg=(name,fallback)=>{const i=process.argv.indexOf(name);return i<0?fallback:process.argv[i+1];};
const origin='http://127.0.0.1:'+arg('--port','9237'),seconds=Number(arg('--seconds','12')),out=arg('--out',join(process.cwd(),'perf-results'));
const version=await(await fetch(origin+'/json/version',{signal:AbortSignal.timeout(5000)})).json();
const ws=new WebSocket(version.webSocketDebuggerUrl),waiting=new Map(),attached=new Map();let id=0;
await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
ws.onmessage=event=>{const reply=JSON.parse(event.data);if(reply.method==='Target.attachedToTarget')attached.set(reply.params.sessionId,reply.params.targetInfo);const pending=waiting.get(reply.id);if(pending){waiting.delete(reply.id);clearTimeout(pending.timer);reply.error?pending.reject(Error(JSON.stringify(reply.error))):pending.resolve(reply.result);}};
const send=(method,params={},sessionId)=>new Promise((resolve,reject)=>{const key=++id,timer=setTimeout(()=>{waiting.delete(key);reject(Error('CDP timed out: '+method));},method==='Profiler.enable'?60000:15000);waiting.set(key,{resolve,reject,timer});ws.send(JSON.stringify({id:key,method,params,...(sessionId?{sessionId}:{})}));});
const attach=async targetId=>(await send('Target.attachToTarget',{targetId,flatten:true})).sessionId;
const evaluate=async(sessionId,expression)=>(await send('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true},sessionId)).result.value;
const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));
mkdirSync(out,{recursive:true});
let pageSession,toggled=false,restoreFps=null;const profilers=[];
try{
 const targets=(await send('Target.getTargets')).targetInfos;
 const game=targets.find(t=>t.type==='page'&&t.title==='NosTale');if(!game)throw Error('No running NosTale page.');
 pageSession=await attach(game.targetId);
 if(process.argv.includes('--uncapped')){
  restoreFps=await evaluate(pageSession,'window.getTargetFps?.() ?? null');
  if(restoreFps!==null){await evaluate(pageSession,'window.setTargetFps(0);true');await pause(500);}
 }
 const symbolText=arg('--symbols','')?readFileSync(arg('--symbols',''),'utf8'):await evaluate(pageSession,"fetch('/build/nostale.js.symbols').then(r=>r.ok?r.text():'').catch(()=>'')");
 const symbols=new Map(typeof symbolText==='string'?symbolText.trim().split(/\r?\n/).map(line=>{const i=line.indexOf(':');return[line.slice(0,i),line.slice(i+1)];}):[]);
 // Avoid attaching the never-yielding game worker during ordinary measurements.
 // CEF 120 can stall its inspector when that worker's session is detached.
 const requestedCpu=arg('--cpu','page.js');
 if(!process.argv.includes('--frames-only')&&requestedCpu!=='page.js'){
  if(requestedCpu==='worker.js'||/Chrome\/120\./.test(version.Browser||''))throw Error('Worker inspection is disabled for this CEF runtime; use --frames-only or page profiling.');
  await send('Target.setAutoAttach',{autoAttach:true,waitForDebuggerOnStart:false,flatten:true},pageSession);
  await pause(300);
 }
 const read=()=>evaluate(pageSession,`({frames:{...window.frameStats},game:window.gameStats,targetFps:window.getTargetFps?.() ?? null,logs:(window.allLog||[]).filter(line=>/^(profile:|frames:|draw paths|api )/.test(line)).slice(-24)})`);
 await evaluate(pageSession,`(()=>{const state=window.__liveFrameMeasure={active:true,shown:window.frameStats.shown,last:0,intervals:[]};function frame(time){if(!state.active)return;if(window.frameStats.shown!==state.shown){if(state.last)state.intervals.push(time-state.last);state.last=time;state.shown=window.frameStats.shown;}requestAnimationFrame(frame);}requestAnimationFrame(frame);return true;})()`);
 const before=await read(),start=performance.now();await pause(seconds*1000);const baseline=await read(),elapsed=(performance.now()-start)/1000;
 const intervals=await evaluate(pageSession,'window.__liveFrameMeasure.active=false;window.__liveFrameMeasure.intervals');intervals.sort((a,b)=>a-b);
 const percentile=p=>intervals[Math.min(intervals.length-1,Math.floor(intervals.length*p))];
 const summary={elapsed,displayedFps:(baseline.frames.shown-before.frames.shown)/elapsed,refreshFps:(baseline.frames.refreshes-before.frames.refreshes)/elapsed,repeated:baseline.frames.repeated-before.frames.repeated,overwritten:baseline.frames.overwritten-before.frames.overwritten,displayIntervalsMs:{median:percentile(.5),p95:percentile(.95),p99:percentile(.99),max:intervals.at(-1)},baseline};
 logger.log('Baseline:',JSON.stringify(summary));
 writeFileSync(join(out,'summary.json'),JSON.stringify(summary,null,2));
 if(!process.argv.includes('--frames-only')){
 if(process.argv.includes('--detail')){await evaluate(pageSession,'window.toggleProfile();true');toggled=true;}
 // The game worker never returns to its event loop. Enabling its profiler
 // after startup can stall CEF's inspector; renderer/page profiling remains
 // available without attaching a sampler to that worker.
 const candidates=[{session:pageSession,name:'page.js'},...[...attached].filter(([,t])=>t.type==='worker').map(([session,target])=>({session,name:new URL(target.url).pathname.split('/').pop()}))].filter(p=>p.name===requestedCpu);
 summary.profilerErrors=[];
 for(const profiler of candidates){
  logger.log('Starting CPU sampler:',profiler.name);
  try{const {session}=profiler;await send('Profiler.enable',{},session);profiler.enabled=true;profilers.push(profiler);await send('Profiler.setSamplingInterval',{interval:1000},session);await send('Profiler.start',{},session);profiler.started=true;}
  catch(error){summary.profilerErrors.push({name:profiler.name,error:error.message});logger.log('Sampler unavailable:',profiler.name,error.message);}
 }
 await pause(seconds*1000);
 const detailed=await read();summary.detailed=detailed;
 for(const profiler of profilers){
  if(!profiler.started)continue;
  const {profile}=await send('Profiler.stop',{},profiler.session);profiler.stopped=true;
  for(const node of profile.nodes){try{node.callFrame.url=node.callFrame.url.split('?')[0];const index=/wasm-function\[(\d+)\]/.exec(node.callFrame.functionName);if(index&&symbols.has(index[1]))node.callFrame.functionName=symbols.get(index[1]);}catch{}}
  writeFileSync(join(out,profiler.name+'.cpuprofile'),JSON.stringify(profile));
  const totals=new Map();for(const node of profile.nodes)totals.set(node.callFrame.functionName,(totals.get(node.callFrame.functionName)||0)+(node.hitCount||0));
  const hits=[...totals].map(([fn,hits])=>({fn,hits})).sort((a,b)=>b.hits-a.hits).slice(0,20);
  summary[profiler.name]=hits;logger.log(profiler.name,JSON.stringify(hits));
 }
 logger.log('Detailed:',JSON.stringify(detailed));
 }
 writeFileSync(join(out,'summary.json'),JSON.stringify(summary,null,2));
}finally{
 if(pageSession)try{await evaluate(pageSession,'if(window.__liveFrameMeasure)window.__liveFrameMeasure.active=false;true');}catch{}
 for(const profiler of profilers){if(profiler.started&&!profiler.stopped)try{await send('Profiler.stop',{},profiler.session);}catch{}if(profiler.enabled)try{await send('Profiler.disable',{},profiler.session);}catch{}}
 if(toggled)try{await evaluate(pageSession,'window.toggleProfile();true');}catch{}
 if(restoreFps!==null)try{await evaluate(pageSession,'window.setTargetFps('+restoreFps+');true');}catch{}
 ws.close();
}
