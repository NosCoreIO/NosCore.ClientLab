// MCP server (stdio) that plays the browser client through the client's own functions (control.js): walk to a cell,
// select, attack or talk, cast a skill, pick up, collect, rest, and read the character, the map and what is on it.
// Screenshots, clicks and keys are there for the windows and dialogues that have no call yet.
//
// usage: node mcp.mjs [--url http://127.0.0.1:8090/] [--params PORT_SCREEN=1280x720] [--headed]
//        node mcp.mjs --attach http://127.0.0.1:9222   (drive a running desktop app started with CLIENTLAB_DEVTOOLS_PORT=9222)
//        --log actions.jsonl   appends every action (walk, answer_dialog, ...) with its arguments and result
// Register it with an MCP client, for example: claude mcp add clientlab -- node /path/to/recomp/web/mcp.mjs
import { createRequire } from 'node:module';
import { appendFileSync } from 'node:fs';
import { createInterface } from 'node:readline';

const require = createRequire(import.meta.url);
const windows = process.platform === 'win32';
const PLAYWRIGHT = process.env.PLAYWRIGHT_PATH
  || (windows ? 'C:/Program Files/nodejs/node_modules/@playwright/cli/node_modules/playwright' : 'playwright');
const { chromium } = require(PLAYWRIGHT);
const { ws: WebSocket } = require(require.resolve('playwright-core/lib/utilsBundle', { paths: [require.resolve(PLAYWRIGHT)] }));

const arg = (name, fallback) => {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : fallback;
};
const defaults = { url: arg('--url', 'http://127.0.0.1:8090/'), params: arg('--params', ''), headed: process.argv.includes('--headed'),
  attach: arg('--attach', process.env.CLIENTLAB_ATTACH || ''), log: arg('--log', process.env.CLIENTLAB_ACTION_LOG || '') };
// Reads (state, screenshots, memory) are left out of the action log.
const QUIET = new Set(['client_state', 'skills', 'inventory', 'map_grid', 'dialogs', 'screenshot', 'client_log', 'read_memory',
  'find_signature', 'client_api']);
function logAction(name, args, content, error) {
  if (!defaults.log || QUIET.has(name)) return;
  let result = content?.type === 'text' ? content.text : content?.type;
  try { result = JSON.parse(result); } catch { }
  try {
    appendFileSync(defaults.log, `${JSON.stringify({ time: new Date().toISOString(), tool: name, args, ...(error ? { error } : { result }) })}\n`);
  } catch (e) {
    process.stderr.write(`action log ${defaults.log}: ${e.message}\n`);
  }
}
if (defaults.log) {
  try { appendFileSync(defaults.log, ''); } catch (e) {
    process.stderr.write(`cannot write the action log ${defaults.log}: ${e.message}\n`);
    process.exit(1);
  }
}

// The argument checks the tool schemas here need: required keys and the four scalar/array types.
const TYPES = { integer: Number.isInteger, boolean: (v) => typeof v === 'boolean', string: (v) => typeof v === 'string', array: Array.isArray };
function invalidArgs(schema, args) {
  if (args === null || typeof args !== 'object' || Array.isArray(args)) return 'arguments must be an object';
  for (const key of schema.required || []) if (args[key] === undefined) return `${key} is required`;
  for (const [key, value] of Object.entries(args)) {
    const spec = schema.properties?.[key];
    if (!spec) return `unknown argument ${key}`;
    if (value === undefined || value === null) continue;
    if (TYPES[spec.type] && !TYPES[spec.type](value)) return `${key} must be ${spec.type === 'integer' ? 'an' : 'a'} ${spec.type}`;
    if (spec.enum && !spec.enum.includes(value)) return `${key} must be one of ${spec.enum.join(', ')}`;
    if (spec.type === 'array' && spec.items?.type && !value.every(TYPES[spec.items.type])) return `${key} items must be ${spec.items.type}s`;
  }
  return null;
}

// Screen positions in the 1680x1050 canvas space the page is scaled to; the login screens move at 1280x720.
const LAYOUT = {
  default: { server: [788, 422], channel: [1068, 422], slot: [1170, 70], start: [840, 965] },
  '1280x720': { server: [770, 376], channel: [1090, 376], slot: [1270, 75], start: [838, 985] },
};
const CANVAS = { width: 1680, height: 1050 };

let browser = null, page = null, attached = false, layout = LAYOUT.default;
// Browser-side errors (the renderer worker, the page) that never reach the client log.
const browserErrors = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const watch = (p) => {
  const note = (text) => { browserErrors.push(text); if (browserErrors.length > 200) browserErrors.shift(); };
  p.on('pageerror', (e) => note(`pageerror: ${e.message}`));
  p.on('console', (m) => { if (m.type() === 'error' || m.type() === 'warning') note(`${m.type()}: ${m.text()}`); });
  p.on('worker', (w) => w.on('console', (m) => { if (m.type() === 'error') note(`worker: ${m.text()}`); }));
};

// Attaching leaves the app's own window and session alone: close() only disconnects.
async function attach(endpoint) {
  await close();
  try {
    browser = await chromium.connectOverCDP(endpoint);
    for (const p of browser.contexts().flatMap((c) => c.pages())) {
      if (await p.evaluate(() => !!window.clientControl).catch(() => false)) { page = p; break; }
    }
  } catch {
    // CEF in the desktop app refuses Playwright's connectOverCDP (Browser.setDownloadBehavior is not supported).
    browser = null;
    page = await cdpGamePage(endpoint);
  }
  if (!page) { await close(); throw new Error(`no game page at ${endpoint}`); }
  attached = true;
  layout = LAYOUT[/PORT_SCREEN=(\d+x\d+)/.exec(page.url())?.[1]] || LAYOUT.default;
  watch(page);
  return `attached to ${page.url()}`;
}

async function open({ url = defaults.url, params = defaults.params, headed = defaults.headed, attach: endpoint = defaults.attach } = {}) {
  if (endpoint) return attach(endpoint);
  await close();
  browser = await chromium.launch({
    headless: !headed,
    channel: process.env.DRIVE_CHANNEL || (windows ? 'chrome' : undefined),
    args: [...(windows ? ['--use-angle=d3d11'] : []), '--enable-gpu', '--ignore-gpu-blocklist', '--enable-unsafe-swiftshader',
      ...(process.env.CHROME_ARGS ? process.env.CHROME_ARGS.split(' ') : [])],
  });
  page = await browser.newPage({ viewport: { width: 1800, height: 1200 } });
  watch(page);
  layout = LAYOUT[/PORT_SCREEN=(\d+x\d+)/.exec(params)?.[1]] || LAYOUT.default;
  // Message boxes answer themselves with their default button so nothing waits on a human.
  await page.goto(`${url}?autodismiss=1${params ? `&${params}` : ''}`);
  return 'client opened';
}

async function close() {
  // Closing a CDP connection only disconnects; the attached app keeps running.
  if (browser) await browser.close().catch(() => { });
  page?.disconnect?.();
  browser = page = null;
  attached = false;
}

// Raw CDP on the game page, with the page calls the tools use: evaluate, focus, url, on, screenshot, mouse and keyboard.
const NAMED_KEYS = { Escape: [27, 'Escape'], Enter: [13, 'Enter', '\r'], Tab: [9, 'Tab'], Backspace: [8, 'Backspace'],
  Delete: [46, 'Delete'], Space: [32, 'Space', ' '], ' ': [32, 'Space', ' '], ArrowLeft: [37, 'ArrowLeft'], ArrowUp: [38, 'ArrowUp'],
  ArrowRight: [39, 'ArrowRight'], ArrowDown: [40, 'ArrowDown'], Shift: [16, 'ShiftLeft'], Control: [17, 'ControlLeft'], Alt: [18, 'AltLeft'] };
function keyInfo(key) {
  const named = NAMED_KEYS[key];
  if (named) return { key: named[2] ?? key, code: named[1], vk: named[0], text: named[2] };
  const f = /^F(\d{1,2})$/.exec(key);
  if (f) return { key, code: key, vk: 111 + Number(f[1]) };
  if (key.length !== 1) throw new Error(`unknown key ${key}`);
  const upper = key.toUpperCase();
  const code = /\d/.test(key) ? `Digit${key}` : /[A-Z]/.test(upper) ? `Key${upper}` : '';
  return { key, code, vk: /[A-Z0-9]/.test(upper) ? upper.charCodeAt(0) : key.charCodeAt(0), text: key };
}

async function cdpGamePage(endpoint) {
  const targets = await (await fetch(new URL('/json/list', endpoint))).json();
  for (const t of targets.filter((t) => t.type === 'page' && t.webSocketDebuggerUrl)) {
    const p = await cdpPage(t).catch(() => null);
    if (!p) continue;
    if (await p.evaluate(() => !!window.clientControl).catch(() => false)) return p;
    p.disconnect();
  }
  return null;
}

async function cdpPage(target) {
  const ws = new WebSocket(target.webSocketDebuggerUrl, { perMessageDeflate: false, maxPayload: 256 * 1024 * 1024 });
  await new Promise((resolve, reject) => { ws.once('open', resolve); ws.once('error', reject); });
  let id = 0;
  const pending = new Map(), handlers = { console: [], pageerror: [] };
  ws.on('message', (data) => {
    const m = JSON.parse(data.toString());
    if (m.id && pending.has(m.id)) {
      const { resolve, reject } = pending.get(m.id);
      pending.delete(m.id);
      if (m.error) reject(new Error(m.error.message)); else resolve(m.result);
    } else if (m.method === 'Runtime.exceptionThrown') {
      const d = m.params.exceptionDetails;
      for (const h of handlers.pageerror) h({ message: d.exception?.description || d.text });
    } else if (m.method === 'Runtime.consoleAPICalled') {
      const type = m.params.type, text = m.params.args.map((a) => a.value ?? a.description ?? '').join(' ');
      for (const h of handlers.console) h({ type: () => type, text: () => text });
    }
  });
  ws.on('close', () => { for (const { reject } of pending.values()) reject(new Error('CDP connection closed')); pending.clear(); });
  const send = (method, params = {}) => new Promise((resolve, reject) => {
    if (ws.readyState !== ws.OPEN) return reject(new Error('CDP connection closed'));
    pending.set(++id, { resolve, reject });
    ws.send(JSON.stringify({ id, method, params }));
  });
  let mouse = { x: 0, y: 0 };
  const mouseEvent = (type, button = 'none', clickCount = 0) =>
    send('Input.dispatchMouseEvent', { type, x: mouse.x, y: mouse.y, button, buttons: type === 'mousePressed' ? (button === 'right' ? 2 : 1) : 0, clickCount });
  const keyEvent = (type, key) => {
    const k = keyInfo(key);
    return send('Input.dispatchKeyEvent', { type: type === 'keyDown' && !k.text ? 'rawKeyDown' : type, key: k.key, code: k.code,
      windowsVirtualKeyCode: k.vk, nativeVirtualKeyCode: k.vk, ...(type === 'keyDown' && k.text ? { text: k.text, unmodifiedText: k.text } : {}) });
  };
  const p = {
    url: () => target.url,
    disconnect: () => ws.close(),
    on: (event, handler) => handlers[event]?.push(handler),
    evaluate: async (fn, arg) => {
      const expression = typeof fn === 'function' ? `(${fn})(${arg === undefined ? '' : JSON.stringify(arg)})` : fn;
      const r = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true, userGesture: true });
      if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
      return r.result.value;
    },
    focus: (selector) => p.evaluate((s) => document.querySelector(s)?.focus(), selector),
    screenshot: async ({ clip }) => Buffer.from((await send('Page.captureScreenshot', { format: 'png', clip: { ...clip, scale: 1 } })).data, 'base64'),
    mouse: {
      move: async (x, y, { steps = 1 } = {}) => {
        const from = mouse;
        for (let i = 1; i <= steps; i++) {
          mouse = { x: from.x + ((x - from.x) * i) / steps, y: from.y + ((y - from.y) * i) / steps };
          await mouseEvent('mouseMoved');
        }
      },
      down: ({ button = 'left' } = {}) => mouseEvent('mousePressed', button, 1),
      up: ({ button = 'left' } = {}) => mouseEvent('mouseReleased', button, 1),
    },
    keyboard: {
      down: (key) => keyEvent('keyDown', key),
      up: (key) => keyEvent('keyUp', key),
      type: async (text, { delay = 0 } = {}) => {
        for (const ch of text) { await keyEvent('keyDown', ch); await keyEvent('keyUp', ch); if (delay) await sleep(delay); }
      },
    },
  };
  await send('Runtime.enable');
  return p;
}

const ensurePage = async () => { if (!page) await open(); return page; };
const control = async (req) => (await ensurePage()).evaluate((r) => window.clientControl.request(r), req);
const logText = () => page.evaluate(() => (window.allLog || []).join('\n'));

async function waitLog(re, seconds) {
  for (let i = 0; i < seconds; i++) {
    const t = await logText();
    if (re.test(t)) return true;
    if (/FATAL/.test(t)) throw new Error(t.split('\n').find((l) => /FATAL/.test(l)));
    await sleep(1000);
  }
  return false;
}

async function waitState(test, seconds) {
  for (const end = Date.now() + seconds * 1000; Date.now() < end; await sleep(500)) {
    const s = await control({ op: 'state' }).catch(() => null);
    if (s && test(s)) return s;
  }
  return null;
}

// Our own browser gets the canvas at 1680x1050; an attached app keeps its window size and coordinates are scaled to it.
async function canvasOrigin() {
  return page.evaluate(({ width, height, resize }) => {
    const c = document.getElementById('screen');
    if (resize) {
      c.style.maxWidth = 'none';
      c.style.width = `${width}px`;
      c.style.height = `${height}px`;
    }
    const b = c.getBoundingClientRect();
    return { x: b.left, y: b.top, sx: b.width / width, sy: b.height / height, width: b.width, height: b.height };
  }, { ...CANVAS, resize: !attached });
}

// Held for about a frame each way: the client samples input once per frame.
async function click([x, y], { button = 'left', double = false } = {}) {
  await ensurePage();
  const o = await canvasOrigin();
  await page.mouse.move(o.x + x * o.sx, o.y + y * o.sy, { steps: 5 });
  await sleep(300);
  for (let k = 0; k < (double ? 2 : 1); k++) {
    await page.mouse.down({ button });
    await sleep(double ? 25 : 150);
    await page.mouse.up({ button });
    if (double) await sleep(25);
  }
}

async function screenshot() {
  await ensurePage();
  const o = await canvasOrigin();
  const png = await page.screenshot({ clip: { x: o.x, y: o.y, width: o.width, height: o.height } });
  return { type: 'image', data: png.toString('base64'), mimeType: 'image/png' };
}

async function enterWorld({ slot } = {}) {
  await ensurePage();
  if (!await waitLog(/closesocket 00000100/, 240)) throw new Error('login did not complete');
  await sleep(4000);
  await click(layout.server);
  await sleep(2000);
  await click(layout.channel);
  if (!await waitLog(/connect 127\.0\.0\.1:1337/, 60)) throw new Error('no world connection');
  await sleep(3000);
  for (let attempt = 0; attempt < 2; attempt++) {
    await click(slot || layout.slot);
    await sleep(2000);
    await click(layout.start);
    const s = await waitState((st) => st.inWorld, 30);
    if (s) return s.character;
  }
  throw new Error('did not enter the world');
}

async function walk({ x, y, wait = true, timeout = 30 }) {
  await control({ op: 'walk', x, y });
  if (!wait) return 'walking';
  let last = null, still = 0;
  for (const end = Date.now() + timeout * 1000; Date.now() < end; await sleep(500)) {
    const c = (await control({ op: 'state' })).character;
    if (!c) break;
    if (c.x === x && c.y === y) return c;
    // Stopped short: the path is blocked or the walk was refused.
    still = last && last.x === c.x && last.y === c.y ? still + 1 : 0;
    if (still >= 6) return { ...c, stoppedShort: true };
    last = c;
  }
  return { ...(await control({ op: 'state' })).character, timedOut: true };
}

const json = (v) => ({ type: 'text', text: typeof v === 'string' ? v : JSON.stringify(v, null, 1) });
const id = { type: 'integer', description: 'Entity id, from client_state' };
const obj = (properties = {}, required = []) => ({ type: 'object', properties, required });

const TOOLS = [
  { name: 'client_open', description: 'Open the browser client on the login screen (closes a running one). Other tools open it on demand.',
    inputSchema: obj({ url: { type: 'string' }, params: { type: 'string', description: 'Extra page query, e.g. PORT_SCREEN=1280x720' }, headed: { type: 'boolean' },
      attach: { type: 'string', description: 'DevTools endpoint of a running desktop app (CLIENTLAB_DEVTOOLS_PORT), e.g. http://127.0.0.1:9222' } }),
    run: (a) => open(a).then(json) },
  { name: 'client_close', description: 'Close the browser client, or disconnect from an attached desktop app without closing it.', inputSchema: obj(), run: () => close().then(() => json('closed')) },
  { name: 'enter_world', description: 'Pick the first server and channel and enter the world with a character slot. Returns the character.',
    inputSchema: obj({ slot: { type: 'array', items: { type: 'integer' }, description: 'Character slot position [x,y] on the 1680x1050 screen' } }),
    run: (a) => enterWorld(a).then(json) },
  { name: 'client_state', description: 'Character (id, name, map, cell, level, HP/MP, sitting), current target, and the monsters, NPCs, players and ground items the client knows on this map.',
    inputSchema: obj(), run: () => control({ op: 'state' }).then(json) },
  { name: 'skills', description: 'Learned skills with cast id, range, area, MP cost and whether each is off cooldown.', inputSchema: obj(), run: () => control({ op: 'skills' }).then(json) },
  { name: 'inventory', description: 'Inventory items (vnum, pocket, slot, amount, name).', inputSchema: obj(), run: () => control({ op: 'inventory' }).then(json) },
  { name: 'map_grid', description: 'The walkability grid of the current map: one row string per y, "." walkable, "#" blocked.', inputSchema: obj(), run: () => control({ op: 'grid' }).then(json) },
  { name: 'walk', description: 'Walk to a cell like a floor click. Waits for arrival unless wait is false.',
    inputSchema: obj({ x: { type: 'integer' }, y: { type: 'integer' }, wait: { type: 'boolean' }, timeout: { type: 'number', description: 'Seconds' } }, ['x', 'y']),
    run: (a) => walk(a).then(json) },
  { name: 'select', description: 'Select a monster, NPC or player as the target, like clicking it once.', inputSchema: obj({ id }, ['id']), run: (a) => control({ op: 'select', id: a.id }).then(json) },
  { name: 'interact', description: 'Act on an entity like clicking it: walk up to it, then attack a monster with the armed skill or talk to an NPC.',
    inputSchema: obj({ id }, ['id']), run: (a) => control({ op: 'interact', id: a.id }).then(json) },
  { name: 'use_skill', description: 'Arm a skill by cast id (0 is the basic attack) and use it on a target, like the skill bar.',
    inputSchema: obj({ cast_id: { type: 'integer' }, target: id }, ['cast_id']), run: (a) => control({ op: 'skill', castId: a.cast_id, target: a.target ?? 0 }).then(json) },
  { name: 'pick_up', description: 'Pick up a ground item by id.', inputSchema: obj({ id }, ['id']), run: (a) => control({ op: 'pickup', id: a.id }).then(json) },
  { name: 'collect', description: 'Collect from a collectable map object (an NPC entry).', inputSchema: obj({ id }, ['id']), run: (a) => control({ op: 'collect', id: a.id }).then(json) },
  { name: 'use_item', description: 'Use an inventory item the way a double click does (potion, scroll, wing, box, quest item), by vnum or by pocket and slot.',
    inputSchema: obj({ vnum: { type: 'integer' }, pocket: { type: 'integer' }, slot: { type: 'integer' } }),
    run: (a) => control({ op: 'use_item', vnum: a.vnum, pocket: a.pocket, slot: a.slot }).then(json) },
  { name: 'dialogs', description: 'Open yes/no boxes (questions, confirmations, revive, teleport offers): their text and what Yes and No would answer.',
    inputSchema: obj(), run: () => control({ op: 'dialogs' }).then(json) },
  { name: 'answer_dialog', description: 'Press Yes or No on an open yes/no box (the first one unless index is given).',
    inputSchema: obj({ yes: { type: 'boolean' }, index: { type: 'integer' } }, ['yes']),
    run: (a) => control({ op: 'answer', yes: a.yes, index: a.index }).then(json) },
  { name: 'rest', description: 'Sit down or stand up.', inputSchema: obj(), run: () => control({ op: 'rest' }).then(json) },
  { name: 'screenshot', description: 'The game screen (1680x1050).', inputSchema: obj(), run: () => screenshot() },
  { name: 'click', description: 'Click the game screen at [x,y] in 1680x1050 coordinates, for windows and dialogues.',
    inputSchema: obj({ x: { type: 'integer' }, y: { type: 'integer' }, button: { type: 'string', enum: ['left', 'right'] }, double: { type: 'boolean' } }, ['x', 'y']),
    run: (a) => click([a.x, a.y], a).then(() => json('clicked')) },
  { name: 'press_key', description: 'Press a key (Playwright key name, e.g. i, Escape, Enter, F1).',
    inputSchema: obj({ key: { type: 'string' } }, ['key']),
    run: async (a) => { await ensurePage(); await page.focus('#screen'); await page.keyboard.down(a.key); await sleep(120); await page.keyboard.up(a.key); return json('pressed'); } },
  { name: 'type_text', description: 'Type text into the focused game field (open the chat with Enter first).',
    inputSchema: obj({ text: { type: 'string' } }, ['text']),
    run: async (a) => { await ensurePage(); await page.focus('#screen'); await page.keyboard.type(a.text, { delay: 100 }); return json('typed'); } },
  { name: 'client_log', description: 'The last lines of the client log, then recent browser errors.', inputSchema: obj({ lines: { type: 'integer' } }),
    run: async (a) => {
      await ensurePage();
      const lines = (await logText()).split('\n').slice(-(a.lines || 50));
      return json([...lines, ...browserErrors.slice(-20).map((e) => `browser ${e}`)].join('\n'));
    } },
  { name: 'read_memory', description: 'Read client memory (for finding new structures).',
    inputSchema: obj({ address: { type: 'integer' }, length: { type: 'integer' } }, ['address', 'length']),
    run: async (a) => json(Buffer.from((await control({ op: 'read', address: a.address, length: a.length })).bytes).toString('hex')) },
  { name: 'find_signature', description: 'Find a byte pattern ("55 8B EC ?? ..") in the client code; returns its address or 0.',
    inputSchema: obj({ signature: { type: 'string' } }, ['signature']), run: (a) => control({ op: 'scan', signature: a.signature }).then(json) },
  { name: 'call_function', description: 'Call a client function with Delphi\'s register convention (EAX, EDX, ECX, then up to three stack arguments) on the game thread.',
    inputSchema: obj({ address: { type: 'integer' }, eax: { type: 'integer' }, edx: { type: 'integer' }, ecx: { type: 'integer' }, stack: { type: 'array', items: { type: 'integer' } } }, ['address']),
    run: (a) => control({ op: 'call', address: a.address, eax: a.eax ?? 0, edx: a.edx ?? 0, ecx: a.ecx ?? 0, stack: a.stack ?? [] }).then(json) },
  { name: 'client_api', description: 'Addresses the client functions and globals resolved to in this build (0 = not found).', inputSchema: obj(), run: () => control({ op: 'api' }).then(json) },
];

// ---- JSON-RPC over stdio, one message per line ----
const send = (m) => process.stdout.write(`${JSON.stringify({ jsonrpc: '2.0', ...m })}\n`);
async function handle(m) {
  if (m === null || typeof m !== 'object' || Array.isArray(m)) return send({ id: null, error: { code: -32600, message: 'invalid request' } });
  if (m.id === undefined) return;
  try {
    switch (m.method) {
      case 'initialize':
        return send({ id: m.id, result: { protocolVersion: m.params?.protocolVersion || '2025-06-18', capabilities: { tools: {} },
          serverInfo: { name: 'clientlab', version: '1.0.0' } } });
      case 'ping': return send({ id: m.id, result: {} });
      case 'tools/list':
        return send({ id: m.id, result: { tools: TOOLS.map(({ name, description, inputSchema }) => ({ name, description, inputSchema })) } });
      case 'tools/call': {
        const tool = TOOLS.find((t) => t.name === m.params?.name);
        if (!tool) return send({ id: m.id, error: { code: -32602, message: `unknown tool ${m.params?.name}` } });
        const args = m.params.arguments || {};
        const invalid = invalidArgs(tool.inputSchema, args);
        if (invalid) return send({ id: m.id, error: { code: -32602, message: `${tool.name}: ${invalid}` } });
        try {
          const content = await tool.run(args);
          logAction(tool.name, args, content);
          return send({ id: m.id, result: { content: [content] } });
        } catch (e) {
          logAction(tool.name, args, null, String(e?.message || e));
          return send({ id: m.id, result: { content: [json(String(e?.message || e))], isError: true } });
        }
      }
      default: return send({ id: m.id, error: { code: -32601, message: `unknown method ${m.method}` } });
    }
  } catch (e) {
    send({ id: m.id, error: { code: -32603, message: String(e?.message || e) } });
  }
}

// Tool calls run one after another: they share one page and one game thread.
let queue = Promise.resolve();
createInterface({ input: process.stdin }).on('line', (line) => {
  if (!line.trim()) return;
  let m;
  try { m = JSON.parse(line); } catch { return send({ id: null, error: { code: -32700, message: 'parse error' } }); }
  queue = queue.then(() => handle(m)).catch(() => { });
}).on('close', () => queue.then(close).then(() => process.exit(0)));
process.on('SIGTERM', () => close().then(() => process.exit(0)));
