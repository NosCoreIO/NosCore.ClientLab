import {logger} from '../../tools/logger.mjs';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const html=readFileSync(new URL('../../recomp/web/index.html',import.meta.url),'utf8');
const source=html.slice(html.indexOf('  const requestedFps ='),html.indexOf('  function handleWorkerMessage('));
function run(refresh,producer,target=60,seconds=2,jitter=0){
 const context=vm.createContext({URLSearchParams,location:{search:'?fps='+target},window:{},performance:{now:()=>context.now},requestAnimationFrame:()=>{},Int32Array,SharedArrayBuffer,Atomics,canvas:{width:1920,height:1080},bitmaps:{transferFromImageBitmap:()=>{}},frames:0,last:0,now:0,publishStats:()=>{},status:{},addEventListener:()=>{},ResizeObserver:class{observe(){}},HTMLIFrameElement:{prototype:{credentialless:true}}});
 vm.runInContext(source+';shownCount=new Int32Array(new SharedArrayBuffer(4));globalThis.tick=()=>{if(queued.length<2&&now>=nextProduced){queued.push({width:1920,height:1080});produced++;nextProduced+=1000/producer;}showFrame(now);};',context);
 context.producer=producer;context.nextProduced=0;context.produced=0;
 let seed=7;const wobble=()=>{seed=(seed*1103515245+12345)%2147483648;return (seed/2147483648*2-1)*jitter;};
 for(let n=0;n<refresh*seconds;n++){context.now=n*1000/refresh+wobble();context.tick();}
 return context;
}
for(const refresh of [60,120,144]){
 const context=run(refresh,refresh);
 assert.ok(Math.abs(context.window.frameStats.shown-120)<=1,'60 FPS presentation failed on '+refresh+' Hz');
}
// Refreshes up to 3 ms early or late (a USB dock's display): each one still shows a frame.
const uneven=run(60,60,60,2,3);
assert.ok(uneven.window.frameStats.shown>=118,'An unevenly timed 60 Hz display dropped to '+uneven.window.frameStats.shown+' frames in 2 s');
const slow=run(120,30);
assert.ok(Math.abs(slow.window.frameStats.shown-60)<=1,'Slow producer was counted as 60 FPS');
const uncapped=run(120,120,0);
assert.equal(uncapped.window.frameStats.shown,uncapped.produced);
uncapped.window.setTargetFps(60);
assert.throws(()=>uncapped.window.setTargetFps(NaN));
logger.log('PASS: actual presentation code targets 60 FPS at 60/120/144 Hz and on an unevenly timed 60 Hz display, reports a slow producer honestly, and supports uncapped measurement.');
