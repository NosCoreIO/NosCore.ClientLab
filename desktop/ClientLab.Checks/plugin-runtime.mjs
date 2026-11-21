// Exercise the actual plugin request handler (worker.js) and page relay (index.html) together, without a game.
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';

const between = (text, open, close) => {
  const start = text.indexOf(open), end = text.indexOf(close, start);
  assert.ok(start >= 0 && end > start, `missing ${open}`);
  return text.slice(start + open.length, end);
};
const worker = between(readFileSync(new URL('../../recomp/web/worker.js', import.meta.url), 'utf8'), '// <plugin-runtime>', '// </plugin-runtime>');
const page = between(readFileSync(new URL('../../recomp/web/index.html', import.meta.url), 'utf8'), '// <plugin-relay>', '// </plugin-relay>');

// The game side: guest memory and the exported functions hle_plugin.c provides.
const HEAPU8 = new Uint8Array(0x30000);
const calls = [], packets = [];
let nextString = 0x20000;
const Module = {
  _plugin_call: (...args) => { calls.push(args); return 0xfeedbeef | 0; },
  _plugin_string: (length) => { if (length > 65536) return 0; const text = nextString; nextString += 0x100; return text; },
  _plugin_packet: (kind, text) => { packets.push({kind, text}); return kind === 0 ? 1 : 0; },
};
const game = vm.createContext({params: new URLSearchParams('PORT_PLUGIN=1'), self: {}, HEAPU8, Module, SharedArrayBuffer, Int32Array, Uint8Array,
  Atomics, TextEncoder, TextDecoder, JSON, String, Array, Number});
vm.runInContext(worker, game);
assert.ok(game.self.pluginBuf instanceof SharedArrayBuffer && typeof game.self.pluginPoll === 'function');
const off = vm.createContext({params: new URLSearchParams(''), self: {}});
vm.runInContext(worker, off);
assert.equal(off.self.pluginBuf, undefined, 'the plugin buffer is only allocated when a plugin is selected');

// The page side: the requests the launcher queues, and the answers it gets back.
HEAPU8.set([1, 2, 3, 4], 0x10000);
const requests = [
  {id: 1, op: 'read', address: 0x10000, length: 4},
  {id: 2, op: 'write', address: 0x10002, hex: 'aBff'},
  {id: 3, op: 'call', address: 0x401000, eax: 1, edx: 2, ecx: 3, stack: [4, 5]},
  {id: 4, op: 'packet', kind: 0, raw: 'say é'},
  {id: 5, op: 'packet', kind: 1, raw: 'in 1 2'},
  {id: 6, op: 'read', address: 0x2fffe, length: 4},
  {id: 7, op: 'write', address: 0x10000, hex: 'zz'},
  {id: 8, op: 'call', address: 1, stack: [1, 2, 3, 4]},
  {id: 9, op: 'nope'},
];
const answers = [];
let finished;
const done = new Promise((resolve) => { finished = resolve; });
const fetch = async (path, options) => {
  if (path === '/plugin/next') {
    const request = requests.shift();
    if (!request) { finished(); return new Promise(() => { }); }
    return {status: 200, ok: true, json: async () => request};
  }
  assert.equal(path, '/plugin/answer');
  answers.push(JSON.parse(options.body));
  return {status: 204, ok: true};
};
const relay = vm.createContext({fetch, Atomics, Int32Array, Uint8Array, TextEncoder, TextDecoder, JSON, String, Promise, setTimeout, performance});
vm.runInContext(page, relay);
// The game thread polls between window messages.
const poll = setInterval(() => game.self.pluginPoll(), 1);
relay.pluginRelay(game.self.pluginBuf);
await Promise.race([done, new Promise((_, reject) => setTimeout(() => reject(new Error('the relay stalled')), 10000))]);
clearInterval(poll);

const byId = Object.fromEntries(answers.map((answer) => [answer.id, answer]));
assert.equal(byId[1].result.hex, '01020304');
assert.deepEqual(Array.from(HEAPU8.subarray(0x10002, 0x10004)), [0xab, 0xff]);
assert.deepEqual(calls[0], [0x401000, 1, 2, 3, 2, 4, 5, 0]);
assert.equal(byId[3].result.eax, 0xfeedbeef);
assert.equal(byId[4].result.ok, true);
assert.equal(new TextDecoder().decode(HEAPU8.subarray(0x20000, 0x20000 + 6)), 'say é');
assert.equal(byId[5].result.ok, false);
assert.deepEqual(packets, [{kind: 0, text: 0x20000}, {kind: 1, text: 0x20100}]);
assert.match(byId[6].error, /out of range/);
assert.match(byId[7].error, /not hex/);
assert.match(byId[8].error, /three stack arguments/);
assert.match(byId[9].error, /unknown plugin request/);
assert.equal(calls.length, 1);
console.log('PASS: actual plugin runtime reads and writes bounded guest memory, calls client functions, hands packets over and relays answers and errors.');
process.exit(0);
