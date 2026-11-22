// Exercise the actual embedded capture JS and game-page transport, without a game account.
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const source = readFileSync(new URL('../../recomp/port/rt_port.c',import.meta.url),'utf8');
const capture = source.slice(source.indexOf('    EM_ASM({') + '    EM_ASM({'.length, source.indexOf('    }, packet, kind);'));
const memory = new ArrayBuffer(1024), bytes = new Uint8Array(memory), words = new Uint32Array(memory);
// Client packets are Windows-1252: 0x80 is the euro sign players use for 'e' in names.
const events = [], raw = 'say 0Lucif€r0';
bytes.set([...new TextEncoder().encode('say 0Lucif'), 0x80, ...new TextEncoder().encode('r0')],64); words[15]=raw.length;
const context = vm.createContext({ENV:{PORT_PACKET_LOG:'1'}, self:{}, HEAPU8:bytes, HEAPU32:words, TextDecoder, Date, postMessage:event=>events.push(event)});
vm.runInContext('function capture($0,$1){'+capture+'}',context);
context.capture(64,0);
assert.equal(events[0].raw,raw); assert.equal(events[0].kind,0);
context.ENV.PORT_PACKET_LOG='0'; context.capture(64,0);
assert.equal(events.length,1);
context.ENV.PORT_PACKET_LOG='1'; words[15]=65537; context.capture(64,0); context.capture(0,0);
assert.equal(events.length,1);
const html=readFileSync(new URL('../../recomp/web/index.html',import.meta.url),'utf8');
const start=html.indexOf('  function handleWorkerMessage('), end=html.indexOf('  function startGame(',start);
assert.ok(start>=0&&end>start);
const requests=[];
const page=vm.createContext({packetSends:Promise.resolve(), JSON, console, fetch:async(path,options)=>{requests.push({path,options});return {ok:true};}});
vm.runInContext(html.slice(start,end),page);
page.handleWorkerMessage({data:events[0]});
await page.packetSends;
assert.equal(requests[0].path,'/packets');
assert.equal(JSON.parse(requests[0].options.body).raw,raw);
console.log('PASS: actual runtime capture checks enabled state and memory bounds, preserves cleartext and posts the event to the game host.');
