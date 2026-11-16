import {logger} from '../../tools/logger.mjs';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {createRequire} from 'node:module';
const require=createRequire(import.meta.url);
const {chromium}=require(process.env.PLAYWRIGHT_PATH||'C:/Program Files/nodejs/node_modules/@playwright/cli/node_modules/playwright');
const html=readFileSync(new URL('../ClientLab.Launcher/Assets/packet-logger.html',import.meta.url),'utf8');
const browser=await chromium.launch({channel:'chrome',headless:true});
try{
 const page=await browser.newPage();let update=false;
 const entry=Id=>({Id,Timestamp:'2026-11-17T00:00:00.000Z',Connection:'World',Direction:'Receive',Raw:'packet '+Id});
 await page.route('**/*',route=>route.request().url().includes('/packets?')
  ?route.fulfill({json:{file:'synthetic.jsonl',cursor:update?10001:10000,entries:route.request().url().endsWith('since=0')?Array.from({length:10000},(_,i)=>entry(i+1)):update&&route.request().url().endsWith('since=10000')?[entry(10001)]:[]}})
  :route.fulfill({contentType:'text/html',body:html}));
 await page.goto('http://127.0.0.1:8458/packet-log');
 await page.waitForFunction(()=>document.getElementById('packets').children.length===10000);
 await page.evaluate(()=>{window.packetChanges=[];window.stablePacket=document.querySelector('[data-id="5000"]');new MutationObserver(records=>window.packetChanges.push(...records.map(r=>({type:r.type,attribute:r.attributeName})))).observe(document.getElementById('packets'),{subtree:true,attributes:true,childList:true});});
 update=true;
 await page.waitForFunction(()=>!!document.querySelector('[data-id="10001"]'));
 const result=await page.evaluate(()=>({rows:document.getElementById('packets').children.length,first:document.getElementById('packets').firstChild.dataset.id,stable:window.stablePacket===document.querySelector('[data-id="5000"]'),changes:window.packetChanges}));
 assert.equal(result.rows,10000);assert.equal(result.first,'2');assert.equal(result.stable,true);
 assert.ok(result.changes.length<=4,'A single packet rewrote the full buffered view');
 assert.equal(result.changes.filter(c=>c.type==='attributes').length,0);
 logger.log('PASS: a full 10000-packet view handles one arrival with '+result.changes.length+' DOM changes, preserves existing nodes, and bounds retention.');
}finally{await browser.close();}
