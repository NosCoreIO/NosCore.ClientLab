import {logger} from '../../tools/logger.mjs';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {createRequire} from 'node:module';
const source=readFileSync(new URL('../../recomp/web/gl.js',import.meta.url),'utf8');
const require=createRequire(import.meta.url),{chromium}=require(process.env.PLAYWRIGHT_PATH||'C:/Program Files/nodejs/node_modules/@playwright/cli/node_modules/playwright');
const browser=await chromium.launch({channel:'chrome',headless:true});
try{
 const page=await browser.newPage();
 const result=await page.evaluate(async renderer=>{
  const checks=`
   addEventListener('message',e=>{if(e.data.type!=='check')return;try{
    init(2,2);const gl=G.gl;let mipmaps=0;const original=gl.generateMipmap.bind(gl);gl.generateMipmap=(...a)=>{mipmaps++;return original(...a);};
    function check(value,message){if(!value)throw Error(message);}
    const all=new Uint8Array(64);for(let i=0;i<16;i++)all.set([0,0,255,255],4*i);
    texture(101,4,4,all,0);check(mipmaps===0,'Upload built unused mipmaps');
    const states=new Int32Array(41);states[12]=101;states[29]=1;states[30]=2;states[27]=states[28]=2;
    for(let i=0;i<500;i++)textureRect(101,0,0,1,1,new Uint8Array([255,0,0,255]),0);
    G.apply(G.cpu,states,0);check(mipmaps===0,'Non-mipmapped text update built mipmaps');check(gl.getTexParameter(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER)===gl.LINEAR,'MIPFILTER_NONE ignored');
    for(const [min,mip,expected] of [[1,0,gl.NEAREST],[2,0,gl.LINEAR],[1,1,gl.NEAREST_MIPMAP_NEAREST],[2,1,gl.LINEAR_MIPMAP_NEAREST],[1,2,gl.NEAREST_MIPMAP_LINEAR],[2,2,gl.LINEAR_MIPMAP_LINEAR]]){
     states[30]=min;states[31]=mip;G.apply(G.cpu,states,0);check(gl.getTexParameter(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER)===expected,'Wrong min/mip filter');
    }
    check(mipmaps===1,'Unchanged levels rebuilt');textureRect(101,0,0,1,1,new Uint8Array([255,0,0,255]),0);G.apply(G.cpu,states,0);check(mipmaps===2,'Partial update did not invalidate levels');
    states[31]=0;G.apply(G.cpu,states,0);textureRect(101,0,0,1,1,new Uint8Array([255,0,0,255]),0);check(mipmaps===2,'Disabled mipmaps rebuilt');lod(101,2);check(mipmaps===3,'LOD did not build required levels');
    const fb=gl.createFramebuffer();gl.bindFramebuffer(gl.FRAMEBUFFER,fb);gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,G.textures.get(101),2);
    const pixel=new Uint8Array(4);gl.readPixels(0,0,1,1,gl.RGBA,gl.UNSIGNED_BYTE,pixel);check(Math.abs(pixel[0]-16)<=1&&Math.abs(pixel[2]-239)<=1&&pixel[3]===255,'Mip pixels stale after partial update');
    gl.bindFramebuffer(gl.FRAMEBUFFER,null);for(let i=0;i<16;i++)all.set([0,255,0,255],4*i);texture(101,4,4,all,0);G.apply(G.cpu,states,0);check(mipmaps===4,'Full replacement did not rebuild LOD');
    gl.bindFramebuffer(gl.FRAMEBUFFER,fb);gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,G.textures.get(101),2);gl.readPixels(0,0,1,1,gl.RGBA,gl.UNSIGNED_BYTE,pixel);check(pixel.join(',')==='0,255,0,255','Full replacement left stale LOD pixels');
    check(gl.getError()===gl.NO_ERROR,'WebGL validation error');postMessage({type:'result',mipmaps,pixel:Array.from(pixel)});
   }catch(error){postMessage({type:'failure',message:error.message});}});`;
  const worker=new Worker(URL.createObjectURL(new Blob([renderer,checks],{type:'text/javascript'})));
  try{return await new Promise((resolve,reject)=>{const timeout=setTimeout(()=>reject(Error('Texture checks timed out')),20000);worker.onerror=e=>{clearTimeout(timeout);reject(Error(e.message));};worker.onmessage=e=>{if(!['result','failure'].includes(e.data.type))return;clearTimeout(timeout);e.data.type==='result'?resolve(e.data):reject(Error(e.data.message));};worker.postMessage({type:'check'});});}
  finally{worker.terminate();}
 },source);
 assert.equal(result.mipmaps,4);assert.deepEqual(result.pixel,[0,255,0,255]);
 logger.log('PASS: 500 non-mipmapped updates build no mipmaps; six filters match D3D modes; first use, partial/full updates and nonzero LOD preserve real GPU pixels.');
}finally{await browser.close();}
