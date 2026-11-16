import {logger} from '../../tools/logger.mjs';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import {createRequire} from 'node:module';
const host=readFileSync(new URL('../../recomp/port/render_gl.inc',import.meta.url),'utf8');
const start=host.indexOf('EM_JS(void, js_gl_submit,');
const body=host.slice(host.indexOf(', {',start)+3,host.indexOf('\n});',start));
const renderer=readFileSync(new URL('../../recomp/web/gl.js',import.meta.url),'utf8');
const globals={ArrayBuffer,SharedArrayBuffer,Uint8Array,Int32Array,Float32Array,Float64Array,Atomics,performance};
const messages=[],logs=[],received=[];
const sender=vm.createContext({...globals,self:{glCommandPoolSupported:true,glPort:{postMessage:m=>messages.push(m)}},ENV:{},HEAPU8:new Uint8Array(9*1024*1024)});
vm.runInContext('function submit(q,len){'+body+'}',sender);
const receiver=vm.createContext({...globals,postMessage:m=>logs.push(m),onmessage:null});
vm.runInContext(renderer,receiver);
receiver.inspect=(buffer,offset=0,length=buffer.byteLength-offset)=>received.push(Array.from(new Uint8Array(buffer,offset,length)));
vm.runInContext('run=inspect;',receiver);
const port={};receiver.onmessage({data:{type:'port',port}});
for(let i=1;i<=4;i++){sender.HEAPU8.fill(i,0,16);sender.submit(0,16);}
assert.equal(messages[0].type,'command-pool');
assert.deepEqual(messages.slice(1,4).map(m=>m.slot),[0,1,2]);
assert(messages[4] instanceof ArrayBuffer,'A busy pool must fall back without waiting');
sender.HEAPU8.fill(99);
for(const data of messages)port.onmessage({data});
assert.deepEqual(received,[1,2,3,4].map(n=>Array(16).fill(n)));
assert.deepEqual(Array.from(sender.self.glCommandPool.states),[0,0,0]);
assert.equal(logs.length,0);
messages.length=0;sender.HEAPU8.fill(7,0,16);sender.submit(0,16);
vm.runInContext('run=()=>{throw Error("test failure")};',receiver);
port.onmessage({data:messages[0]});
assert.equal(sender.self.glCommandPool.states[messages[0].slot],0,'Release ownership even when decoding throws');
assert.equal(logs.length,1);
for(const mode of ['disabled','unsupported','oversized']){
 messages.length=0;sender.ENV.PORT_GL_COMMAND_POOL=mode==='disabled'?'0':'';sender.self.glCommandPoolSupported=mode!=='unsupported';
 sender.submit(0,mode==='oversized'?9*1024*1024:16);
 assert(messages[0] instanceof ArrayBuffer,mode+' must retain the transfer path');
}
logger.log('PASS: actual command sender/receiver preserve order, owned bytes, nonzero offsets, busy/disabled/oversized fallback and exception release.');

// Exercise the real decoder and WebGL uploads in an isolated browser worker.
const require=createRequire(import.meta.url);
const {chromium}=require(process.env.PLAYWRIGHT_PATH||'C:/Program Files/nodejs/node_modules/@playwright/cli/node_modules/playwright');
const browser=await chromium.launch({channel:'chrome',headless:true,args:['--enable-gpu','--ignore-gpu-blocklist']});
try{
 const page=await browser.newPage();
 await page.route('http://localhost/**',route=>route.fulfill({contentType:'text/html',headers:{'Cross-Origin-Opener-Policy':'same-origin','Cross-Origin-Embedder-Policy':'require-corp'},body:'<!doctype html><title>Command pool check</title>'}));
 await page.goto('http://localhost/');
 const pixels=await page.evaluate(async source=>{
  const worker=new Worker(URL.createObjectURL(new Blob([source],{type:'text/javascript'})));
  const channel=new MessageChannel(),states=new Int32Array(new SharedArrayBuffer(12)),slotBytes=256,bytes=new SharedArrayBuffer(3*slotBytes);
  const results=[];
  let finish,fail;
  const done=new Promise((resolve,reject)=>{finish=resolve;fail=reject;});
  const timeout=setTimeout(()=>fail(Error('Renderer did not produce frames')),20000);
  worker.onerror=e=>fail(Error(e.message));
  worker.onmessage=e=>{
   if(e.data.type==='log'&&e.data.text.includes('FATAL'))fail(Error(e.data.text));
   if(e.data.type!=='bitmap')return;
   const c=new OffscreenCanvas(2,2),ctx=c.getContext('2d');ctx.drawImage(e.data.bmp,0,0);e.data.bmp.close();results.push(Array.from(ctx.getImageData(0,0,1,1).data));
   if(results.length===4)finish();
  };
  worker.postMessage({type:'port',port:channel.port1},[channel.port1]);
  channel.port2.postMessage({type:'command-pool',states:states.buffer,bytes,slotBytes});
  for(let slot=0;slot<3;slot++){
   // Upload a real shared RGBA payload with opcode 7 before presenting.
   const words=new Int32Array(bytes,slot*slotBytes,16);
   words.set([...(slot===0?[1,2,2]:[]),7,0,0,1,1,0xff000000|(255<<(slot*8)),9]);
   const count=(slot===0?3:0)+7;Atomics.store(states,slot,2);channel.port2.postMessage({type:'commands',slot,bytes:count*4});
  }
  const legacy=new Int32Array([6,1,0xffffff,0x3f800000,0,0,2,2,9]);channel.port2.postMessage(legacy.buffer,[legacy.buffer]);
  try{await done;await new Promise(r=>setTimeout(r,30));if(states.some(n=>n!==0))throw Error('Unreleased pool slot');return results;}
  finally{clearTimeout(timeout);worker.terminate();channel.port2.close();}
 },renderer);
 assert.deepEqual(pixels,[[255,0,0,255],[0,255,0,255],[0,0,255,255],[255,255,255,255]]);
 logger.log('PASS: real worker/WebGL renderer presents exact pixels from all shared slot offsets, shared texture uploads, and legacy transfer.');
}finally{await browser.close();}
