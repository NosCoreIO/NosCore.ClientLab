import {logger} from '../../tools/logger.mjs';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {createRequire} from 'node:module';
const require=createRequire(import.meta.url);
const {chromium}=require(process.env.PLAYWRIGHT_PATH||'C:/Program Files/nodejs/node_modules/@playwright/cli/node_modules/playwright');
const host=readFileSync(new URL('../../recomp/port/hle_dc.c',import.meta.url),'utf8');
const start=host.indexOf('EM_JS(void, js_text_measure,');
const body=host.slice(host.indexOf(', {',start)+3,host.indexOf('\n});',start));
const source='function measureGuest(css,text,n,wide,out){'+body+'}';
const prefixStart=host.indexOf('EM_JS(void, js_text_prefixes,');
const prefixSource='function prefixesGuest(css,text,n,wide,out){'+host.slice(host.indexOf(', {',prefixStart)+3,host.indexOf('\n});',prefixStart))+'}';
const browser=await chromium.launch({channel:'chrome',headless:true});
try{
 const page=await browser.newPage();
 await page.evaluate(code=>{self.ENV={};self.HEAPU8=new Uint8Array(65536);self.HEAPU16=new Uint16Array(HEAPU8.buffer);self.HEAP32=new Int32Array(HEAPU8.buffer);self.UTF8ToString=()=>self.currentFont;(0,eval)(code);},source+prefixSource);
 const result=await page.evaluate(()=>{
  const original=new OffscreenCanvas(4,4).getContext('2d');
  function measure(font,text){self.currentFont=font;for(let i=0;i<text.length;i++)HEAPU16[256+i]=text.charCodeAt(i);measureGuest(0,512,text.length,1,0);return Array.from(HEAP32.subarray(0,3));}
  function same(font,text){const a=measure(font,text);original.font=font;const m=original.measureText(text),b=[Math.round(m.width),Math.ceil(m.fontBoundingBoxAscent??m.actualBoundingBoxAscent??10),Math.ceil(m.fontBoundingBoxDescent??m.actualBoundingBoxDescent??3)];if(a.some((v,i)=>v!==b[i]))throw Error('Changed guest text metrics');}
  for(const font of ['12px Arial','bold 16px Arial','italic 22px serif','invalid font'])for(const text of ['NosVille','Player 123','café','你好','مرحبا','A\u0301','😀','']){same(font,text);same(font,text);}
  // The A calls carry Windows-1252: 0x80 is the euro sign players use for 'e' in names, not an invisible C1 control.
  function measureAnsi(font,bytes){self.currentFont=font;HEAPU8.set(bytes,1024);measureGuest(0,1024,bytes.length,0,0);return Array.from(HEAP32.subarray(0,3));}
  const euro=[0x30,0x4c,0x75,0x63,0x69,0x66,0x80,0x72,0x30];
  for(const font of ['16px Arial','bold 12px Arial']){const a=measureAnsi(font,euro),b=measure(font,'0Lucif€r0');if(a.join()!==b.join())throw Error('ANSI 0x80 is not measured as the euro sign: '+a+' vs '+b);}
  self.currentFont='16px Arial';HEAPU8.set(euro,1024);prefixesGuest(0,1024,euro.length,0,64);original.font=self.currentFont;
  if(HEAP32[16+6]!==Math.round(original.measureText('0Lucif€').width))throw Error('ANSI prefixes do not decode Windows-1252');
  self.currentFont='36px serif';prefixesGuest(0,512,1,1,64);original.font=self.currentFont;same('invalid font','NosVille');
  // Count native crossings, preserving the real Canvas font setter/getter.
  const g=self.__textCtx,descriptor=Object.getOwnPropertyDescriptor(OffscreenCanvasRenderingContext2D.prototype,'font');let writes=0,reads=0;
  Object.defineProperty(g,'font',{configurable:true,get(){reads++;return descriptor.get.call(this);},set(v){writes++;descriptor.set.call(this,v);}});
  for(let i=0;i<100;i++)same('17px Arial','Repeated label');
  if(writes!==1||reads!==1)throw Error('Repeated labels still reparse/read the native font');delete g.font;
  const stats=self.__textMetricsCache;
  if(stats.hits<20)throw Error('Repeated metrics did not use cache');
  for(let i=0;i<5000;i++)measure('12px Arial','distinct '+i);
  if(stats.entries.size!==4096)throw Error('Unbounded text cache');
  measure('12px Arial','x'.repeat(3000));if(stats.entries.size!==4096)throw Error('Long text changed bounded storage');
  const offscreen=OffscreenCanvas;self.OffscreenCanvas=undefined;const fallback=measure('12px Arial','abc');self.OffscreenCanvas=offscreen;if(fallback.join(',')!=='21,13,3')throw Error('Headless text fallback changed');
  const names=Array.from({length:100},(_,i)=>'NosVille player '+i),fonts=['12px Arial','bold 14px Arial','italic 16px serif'];
  function time(enabled){stats.enabled=enabled;let width=0;const start=performance.now();for(let i=0;i<20000;i++)width+=measure(fonts[i%fonts.length],names[i%names.length])[0];return {ms:performance.now()-start,width};}
  const warm=time(true),off=time(false),on=time(true);if(off.width!==on.width||warm.width!==on.width)throw Error('Cached widths changed');
  return {offMs:off.ms,onMs:on.ms,speedup:off.ms/on.ms,hits:stats.hits,misses:stats.misses};
 });
 logger.log('PASS: exact guest text metrics for fonts, scripts, invalid-font fallback; bounded cache and headless fallback.');
 logger.log(JSON.stringify(result));
}finally{await browser.close();}
