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
  attach: arg('--attach', process.env.CLIENTLAB_ATTACH || ''), log: arg('--log', process.env.CLIENTLAB_ACTION_LOG || ''),
  slot: (arg('--slot', process.env.CLIENTLAB_SLOT || '') || null)?.split(',').map(Number) };
// Reads (state, screenshots, memory) are left out of the action log.
const QUIET = new Set(['client_state', 'skills', 'inventory', 'map_grid', 'dialogs', 'screenshot', 'client_log', 'read_memory',
  'find_signature', 'client_api', 'packets', 'wait_for', 'portals']);
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
const TYPES = { integer: Number.isInteger, number: Number.isFinite, boolean: (v) => typeof v === 'boolean', string: (v) => typeof v === 'string',
  array: Array.isArray };
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
  // The packet log feeds packets and wait_for.
  const query = [params, /PORT_PACKET_LOG=/.test(params) ? '' : 'PORT_PACKET_LOG=1'].filter(Boolean).join('&');
  await page.goto(`${url}?autodismiss=1&${query}`);
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

async function waitLog(re, seconds, after = 0) {
  for (let i = 0; i < seconds; i++) {
    const t = await logText();
    if (re.test(t.slice(after))) return true;
    if (/FATAL/.test(t)) throw new Error(t.split('\n').find((l) => /FATAL/.test(l)));
    await sleep(1000);
  }
  return false;
}

// Packets the client sent or received after seq, newest last, filtered by header, direction and text.
async function packetsSince(seq, { header, direction, contains } = {}) {
  const log = await page.evaluate(() => window.packetLog ?? null);
  if (!log) throw new Error('this client keeps no packet log: open it with PORT_PACKET_LOG=1 (the launcher\'s packet logger setting)');
  return { seq: log.seq, list: log.list.filter((p) => p.seq > seq && (!direction || p.direction === direction)
    && (!header || p.raw.split(' ')[0] === header || p.raw.split(' ')[1] === header) && (!contains || p.raw.includes(contains))) };
}
const packetSeq = async () => (page ? (await page.evaluate(() => window.packetLog?.seq ?? 0).catch(() => 0)) : 0);
// Where wait_for starts: the packet count just before the last action, so a reply that came before the call is not missed.
let actionSeq = 0;

async function waitFor({ header, direction, contains, dialog, timeout = 30 }) {
  await ensurePage();
  if (!header && !contains && !dialog) throw new Error('give a packet header, a text it contains or a dialog text');
  for (const end = Date.now() + timeout * 1000; ; await sleep(250)) {
    if (header || contains) {
      const { list } = await packetsSince(actionSeq, { header, direction, contains });
      if (list.length) return list[0];
    }
    if (dialog) {
      const box = (await control({ op: 'dialogs' })).find((d) => [d.text, d.yes, d.no].some((t) => t.toLowerCase().includes(dialog.toLowerCase())));
      if (box) return box;
    }
    if (Date.now() >= end) throw new Error(`nothing matched within ${timeout} s`);
    await guardTick();
  }
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
    const b = c.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
    return { x: b.left, y: b.top, sx: b.width / width, sy: b.height / height, width: b.width, height: b.height, dpr,
      shot: { width: Math.round(b.width * dpr), height: Math.round(b.height * dpr) } };
  }, { ...CANVAS, resize: !attached });
}

// Held for about a frame each way: the client samples input once per frame. [x,y] are screenshot pixels; the login
// layout positions pass layout: true and are in the 1680x1050 space.
async function click([x, y], { button = 'left', double = false, layout: scaled = false } = {}) {
  await ensurePage();
  const o = await canvasOrigin();
  const [px, py] = scaled ? [x * o.sx, y * o.sy] : [x / o.dpr, y / o.dpr];
  await page.mouse.move(o.x + px, o.y + py, { steps: 5 });
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
  return [{ type: 'image', data: png.toString('base64'), mimeType: 'image/png' },
    json(`${o.shot.width}x${o.shot.height}: click and enter_world's slot take these pixel coordinates`)];
}

async function enterWorld({ slot = defaults.slot } = {}) {
  await ensurePage();
  if (!await waitLog(/closesocket /, 240)) throw new Error('login did not complete');
  await sleep(4000);
  await click(layout.server, { layout: true });
  await sleep(2000);
  const before = (await logText()).length;
  await click(layout.channel, { layout: true });
  // The world server's address depends on the server list, so any new connection counts.
  if (!await waitLog(/connect \S+ -> ok/, 60, before)) throw new Error('no world connection');
  await sleep(3000);
  for (let attempt = 0; attempt < 2; attempt++) {
    await (slot ? click(slot) : click(layout.slot, { layout: true }));
    await sleep(2000);
    await click(layout.start, { layout: true });
    const s = await waitState((st) => st.inWorld && st.character.hp > 0, 30);
    if (s) return s.character;
  }
  throw new Error('did not enter the world: if the slot was empty, pass slot as the character\'s [x,y] on a character-select screenshot (or start the MCP with --slot x,y)');
}

// The guard eats or drinks below an HP or MP percentage and accepts chosen yes/no boxes. It runs before every tool,
// inside walk and wait_for loops, and once a second while the MCP is idle.
const guard = { hpBelow: null, hpItem: null, mpBelow: null, mpItem: null, foodBelow: null, foodItem: null, safeRadius: 6, accept: [],
  cooldown: 4, events: [], used: {} };
let guarding = false;
async function guardTick() {
  if (!page || guarding || !(guard.hpItem || guard.mpItem || guard.foodItem || guard.accept.length)) return;
  guarding = true;
  try {
    const note = (event) => {
      guard.events.push({ time: new Date().toISOString(), ...event });
      guard.events.splice(0, Math.max(0, guard.events.length - 50));
      logAction('guard', {}, json(event));
    };
    const s = await control({ op: 'state', kinds: ['none'] });
    // A dead character (0 %) waits for the revive box; eating then only wastes the item.
    if (s.inWorld && s.character.hp > 0) {
      for (const [stat, below, vnum] of [['hp', guard.hpBelow, guard.hpItem], ['mp', guard.mpBelow, guard.mpItem]]) {
        if (!vnum || below === null || s.character[stat] > below || Date.now() - (guard.used[stat] || 0) < guard.cooldown * 1000) continue;
        guard.used[stat] = Date.now();
        const used = await control({ op: 'use_item', vnum }).then(() => true, (e) => { note({ stat, vnum, error: String(e?.message || e) }); return false; });
        if (used) note({ stat, percent: s.character[stat], used: vnum });
      }
      // Food sits the character down, so it waits until no monster is within safeRadius.
      const food = guard.foodItem, hp = s.character.hp;
      if (food && guard.foodBelow !== null && hp <= guard.foodBelow && !s.character.sitting
        && Date.now() - (guard.used.food || 0) >= guard.cooldown * 1000) {
        const near = (await worldState({ kinds: ['monsters'], radius: guard.safeRadius })).monsters ?? [];
        if (!near.length) {
          guard.used.food = Date.now();
          const used = await control({ op: 'use_item', vnum: food }).then(() => true, (e) => { note({ stat: 'food', vnum: food, error: String(e?.message || e) }); return false; });
          if (used) note({ stat: 'food', percent: hp, used: food });
        }
      }
    }
    if (guard.accept.length) {
      for (const d of await control({ op: 'dialogs' })) {
        const rule = guard.accept.find((t) => [d.text, d.yes, d.no].some((x) => x.toLowerCase().includes(t.toLowerCase())));
        if (!rule) continue;
        await control({ op: 'answer', index: d.index, yes: true });
        note({ accepted: d.text, rule });
      }
    }
  } catch { } finally { guarding = false; }
}
function setGuard(a) {
  if (a.off) Object.assign(guard, { hpBelow: null, hpItem: null, mpBelow: null, mpItem: null, foodBelow: null, foodItem: null, accept: [] });
  for (const [arg, key] of [['hp_below', 'hpBelow'], ['hp_item', 'hpItem'], ['mp_below', 'mpBelow'], ['mp_item', 'mpItem'], ['food_below', 'foodBelow'],
    ['food_item', 'foodItem'], ['safe_radius', 'safeRadius'], ['accept', 'accept'], ['cooldown', 'cooldown']]) if (a[arg] !== undefined && a[arg] !== null) guard[key] = a[arg];
  const { events, used, ...config } = guard;
  return { ...config, events: events.slice(-10) };
}

// The client walks up to the entity before it acts, so this returns once the character has stood still for a second.
async function interact({ id, wait = true, timeout = 20 }) {
  await control({ op: 'interact', id });
  if (!wait) return 'walking';
  let last = null, still = 0;
  for (const end = Date.now() + timeout * 1000; Date.now() < end; await sleep(250)) {
    await guardTick();
    const c = (await control({ op: 'state', kinds: ['none'] })).character;
    if (!c) break;
    still = last && last.x === c.x && last.y === c.y ? still + 1 : 0;
    if (still >= 4) return c;
    last = c;
  }
  return { ...(await control({ op: 'state', kinds: ['none'] })).character, timedOut: true };
}

// Portals of the current map: the gp packets since the last at (map change) in the packet log.
async function portals() {
  return control({ op: 'portals' });
}

// Sitting and standing take a server round trip; the character's flag flips when it lands.
async function rest({ timeout = 5 } = {}) {
  const before = (await control({ op: 'state', kinds: ['none'] })).character?.sitting;
  await control({ op: 'rest' });
  for (const end = Date.now() + timeout * 1000; Date.now() < end; await sleep(200)) {
    const c = (await control({ op: 'state', kinds: ['none'] })).character;
    if (c && c.sitting !== before) return c;
  }
  throw new Error(`still ${before ? 'sitting' : 'standing'} after ${timeout} s`);
}

// A skill still cooling down (often from the auto-attack interact starts) is waited for, up to wait seconds.
async function useSkill({ cast_id: castId, target = 0, wait = 5 }) {
  for (const end = Date.now() + wait * 1000; ; await sleep(200)) {
    const skill = (await control({ op: 'skills' })).find((k) => k.castId === castId);
    if (!skill || skill.ready || Date.now() >= end) break;
    await guardTick();
  }
  return control({ op: 'skill', castId, target });
}

// The pickup walks to the item first; done once the item has left the ground.
async function pickUp({ id, timeout = 15 }) {
  await control({ op: 'pickup', id });
  for (const end = Date.now() + timeout * 1000; Date.now() < end; await sleep(250)) {
    await guardTick();
    const s = await control({ op: 'state', kinds: ['items'] });
    if (!s.inWorld || !s.items) throw new Error(`left the world while picking up item ${id}`);
    if (!s.items.some((i) => i.id === id)) return { pickedUp: id, character: s.character };
  }
  throw new Error(`item ${id} is still on the ground after ${timeout} s`);
}

// Monsters the server has reported dead (packet log) are left out, though the client still draws them.
async function worldState(filters) {
  const s = await control({ op: 'state', ...filters });
  const dead = await page.evaluate(() => window.deadEntities ?? {});
  const alive = (e) => !(e.kind === 'monster' && dead[`3:${e.id}`]);
  if (s.monsters) s.monsters = s.monsters.filter(alive);
  if (s.target && !alive(s.target)) s.target = { ...s.target, dead: true };
  return s;
}

async function walk({ x, y, wait = true, timeout = 30, through_portal: throughPortal = false }) {
  await ensurePage();
  // Stepping on (or next to) a portal cell sends the character through it.
  const known = throughPortal ? [] : await portals();
  const near = known.find((p) => Math.max(Math.abs(p.x - x), Math.abs(p.y - y)) <= 1);
  if (near) throw new Error(`[${x},${y}] is on or next to portal ${near.id} at [${near.x},${near.y}] to map ${near.map}: pick another cell, or pass through_portal to take it`);
  if (!wait) {
    // Without waiting there is a single leg, so with portals around it is only sent when it cannot pass one.
    if (known.length) {
      const from = (await control({ op: 'state', kinds: ['none'] })).character;
      const grid = await control({ op: 'grid' });
      const avoid = portalAvoid(known, from);
      const path = gridPath(grid, from, { x, y }, avoid);
      if (!path) throw new Error(`no walkable path from [${from.x},${from.y}] to [${x},${y}] that keeps off the portals`);
      if (path.length < 2 && known.some((p) => p.x === from.x && p.y === from.y)) throw new Error('one step off the portal the character stands on can take it back: walk at least two cells');
      if (!legSafe(grid, from, { x, y }, avoid, path.length)) throw new Error(`the way to [${x},${y}] may pass a portal: walk with wait to follow a route around it`);
    }
    await control({ op: 'walk', x, y });
    return 'walking';
  }
  const end = Date.now() + timeout * 1000;
  // With portals around, the route is planned around them up front; otherwise the client's own walk goes first, and
  // since it gives up on long detours, a stalled walk is retried along waypoints from the map grid.
  let c = known.length ? null : await walkLeg(x, y, end);
  if (c?.changedMap) return c.character;
  if (c?.arrived) return throughPortal ? afterPortal(c.character, end) : c.character;
  if (c?.timedOut) throw new Error(`timed out at [${c.character.x},${c.character.y}] on the way to [${x},${y}]`);
  const from = c?.character ?? (await control({ op: 'state', kinds: ['none'] })).character;
  const grid = await control({ op: 'grid' });
  if (grid && grid.rows[y]?.[x] !== '.') throw new Error(`[${x},${y}] is not walkable`);
  const avoid = portalAvoid(known, from);
  const path = gridPath(grid, from, { x, y }, avoid);
  if (!path) throw new Error(`no walkable path from [${from.x},${from.y}] to [${x},${y}]${known.length ? ' that keeps off the portals' : ''}`);
  if (!path.length) return from;
  // The client picks its own route between waypoints, so near portals a leg is shortened until none of its routes
  // can pass one; a single step is always safe.
  // A walk that ends one step off the portal the character stands on can take it back, so that first leg is longer.
  const onPortal = known.some((p) => p.x === from.x && p.y === from.y);
  for (let at = from, i = Math.min(WAYPOINT, path.length) - 1; ; i = Math.min(i + WAYPOINT, path.length - 1)) {
    const first = path.indexOf(at) + 1 + (onPortal && at === from && path.length > 1 ? 1 : 0);
    while (known.length && i > first && !legSafe(grid, at, path[i], avoid, i - path.indexOf(at))) i--;
    const p = path[i];
    at = p;
    c = await walkLeg(p.x, p.y, end);
    if (c.changedMap) return c.character;
    if (!c.arrived) throw new Error(`stopped at [${c.character.x},${c.character.y}] on the way to [${x},${y}] (waypoint [${p.x},${p.y}]${c.timedOut ? ', timed out' : ''})`);
    if (i === path.length - 1) return throughPortal ? afterPortal(c.character, end) : c.character;
  }
}
const WAYPOINT = 8;

// The server moves the character a moment after it steps on a portal; until then the client still shows the old map.
async function afterPortal(arrived, end) {
  for (const stop = Math.min(end, Date.now() + 10000); Date.now() < stop; await sleep(250)) {
    const c = (await control({ op: 'state', kinds: ['none'] })).character;
    if (c && c.map !== arrived.map) return settled(c);
  }
  return arrived;
}

// After a map change, the character's cell once it has stopped moving on the new map.
async function settled(c) {
  for (let i = 0; i < 20; i++) {
    await sleep(250);
    const n = (await control({ op: 'state', kinds: ['none'] })).character;
    if (n && n.map === c.map && n.x === c.x && n.y === c.y) return { ...n, changedMap: true };
    if (n) c = n;
  }
  return { ...c, changedMap: true };
}

// Leaving an arrival portal is allowed: around the portal the character stands next to, only its own cell is avoided.
function portalAvoid(known, from) {
  const start = new Set(known.filter((p) => cells(p, from) <= 1));
  return (cx, cy) => known.some((p) => cells(p, { x: cx, y: cy }) <= (start.has(p) ? 0 : 1));
}

const cells = (a, b) => Math.max(Math.abs(a.x - b.x), Math.abs(a.y - b.y));

// Every shortest eight-direction route from a to b stays on cells c with |a,c| + |c,b| = |a,b|. A leg is safe when it
// takes steps = |a,b| cells, so the client's shortest route stays in that area, and no walkable cell there is near a
// portal.
function legSafe(grid, a, b, avoid, steps) {
  const d = cells(a, b);
  if (!grid || steps !== d) return false;
  for (let cy = Math.min(a.y, b.y) - d; cy <= Math.max(a.y, b.y) + d; cy++)
    for (let cx = Math.min(a.x, b.x) - d; cx <= Math.max(a.x, b.x) + d; cx++) {
      const c = { x: cx, y: cy };
      if ((cx === a.x && cy === a.y) || cells(a, c) + cells(c, b) !== d) continue;
      if (grid.rows[cy]?.[cx] === '.' && avoid(cx, cy)) return false;
    }
  return true;
}

async function walkLeg(x, y, end) {
  if (Date.now() >= end) return { arrived: false, timedOut: true, character: (await control({ op: 'state', kinds: ['none'] })).character };
  const map = (await control({ op: 'state', kinds: ['none'] })).character?.map;
  await control({ op: 'walk', x, y });
  let last = null, still = 0, c = null, away = 0;
  for (; Date.now() < end; await sleep(500)) {
    await guardTick();
    c = (await control({ op: 'state', kinds: ['none'] })).character;
    // A portal on the way loads another map; the character is missing from the state while it loads.
    if (!c) { if (++away > 20) throw new Error('not in the world'); continue; }
    if (c.map !== map) return { arrived: false, changedMap: true, character: await settled(c) };
    if (c.x === x && c.y === y) return { arrived: true, character: c };
    still = last && last.x === c.x && last.y === c.y ? still + 1 : 0;
    if (still >= 6) return { arrived: false, character: c };
    last = c;
  }
  return { arrived: false, timedOut: true, character: c ?? (await control({ op: 'state', kinds: ['none'] })).character };
}

// Breadth-first search over the walkability grid, eight directions, no corner cutting.
function gridPath(grid, from, to, avoid = () => false) {
  if (!grid) return null;
  const { width: w, height: h, rows } = grid;
  const open = (x, y) => x >= 0 && y >= 0 && x < w && y < h && rows[y][x] === '.' && !avoid(x, y);
  if (!open(to.x, to.y)) return null;
  const prev = new Int32Array(w * h).fill(-1), start = from.y * w + from.x, goal = to.y * w + to.x;
  prev[start] = start;
  for (let queue = [start], head = 0; head < queue.length;) {
    const cur = queue[head++];
    if (cur === goal) break;
    const cx = cur % w, cy = (cur / w) | 0;
    for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1], [1, 1], [1, -1], [-1, 1], [-1, -1]]) {
      const nx = cx + dx, ny = cy + dy, n = ny * w + nx;
      if (!open(nx, ny) || prev[n] >= 0 || (dx && dy && (!open(cx + dx, cy) || !open(cx, cy + dy)))) continue;
      prev[n] = cur;
      queue.push(n);
    }
  }
  if (prev[goal] < 0) return null;
  const path = [];
  for (let n = goal; n !== start; n = prev[n]) path.unshift({ x: n % w, y: (n / w) | 0 });
  return path;
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
    inputSchema: obj({ slot: { type: 'array', items: { type: 'integer' }, description: 'The character\'s [x,y] on a character-select screenshot; defaults to --slot, else the first NosCore slot' } }),
    run: (a) => enterWorld(a).then(json) },
  { name: 'client_state', description: 'Character (id, name, map, cell, level, HP/MP, sitting), current target, and the monsters, NPCs, players and ground items the client knows on this map.',
    inputSchema: obj({ radius: { type: 'integer', description: 'Only entities within this many cells of the character' },
      vnums: { type: 'array', items: { type: 'integer' }, description: 'Only these monster, NPC or item vnums' },
      kinds: { type: 'array', items: { type: 'string' }, description: 'Only these lists: monsters, npcs, players, items' } }),
    run: (a) => worldState({ radius: a.radius, vnums: a.vnums, kinds: a.kinds }).then(json) },
  { name: 'skills', description: 'Learned skills with cast id, range, area, MP cost and whether each is off cooldown.', inputSchema: obj(), run: () => control({ op: 'skills' }).then(json) },
  { name: 'inventory', description: 'Inventory items (vnum, pocket, slot, amount, name).', inputSchema: obj(), run: () => control({ op: 'inventory' }).then(json) },
  { name: 'map_grid', description: 'The walkability grid of the current map: one row string per y, "." walkable, "#" blocked.', inputSchema: obj(), run: () => control({ op: 'grid' }).then(json) },
  { name: 'walk', description: 'Walk to a cell like a floor click. Waits for arrival unless wait is false.',
    inputSchema: obj({ x: { type: 'integer' }, y: { type: 'integer' }, wait: { type: 'boolean' }, timeout: { type: 'number', description: 'Seconds' },
      through_portal: { type: 'boolean', description: 'Allow a cell on or next to a portal (refused otherwise)' } }, ['x', 'y']),
    run: (a) => walk(a).then(json) },
  { name: 'select', description: 'Select a monster, NPC or player as the target, like clicking it once.', inputSchema: obj({ id }, ['id']), run: (a) => control({ op: 'select', id: a.id }).then(json) },
  { name: 'interact', description: 'Act on an entity like clicking it: walk up to it, then attack a monster with the armed skill or talk to an NPC. Waits until the character has stopped walking unless wait is false.',
    inputSchema: obj({ id, wait: { type: 'boolean' }, timeout: { type: 'number', description: 'Seconds' } }, ['id']), run: (a) => interact(a).then(json) },
  { name: 'use_skill', description: 'Arm a skill by cast id (0 is the basic attack) and use it on a target, like the skill bar.',
    inputSchema: obj({ cast_id: { type: 'integer' }, target: id, wait: { type: 'number', description: 'Seconds to wait for the skill to come off cooldown, default 5' } }, ['cast_id']),
    run: (a) => useSkill(a).then(json) },
  { name: 'pick_up', description: 'Pick up a ground item by id: walks to it and waits until it has left the ground.',
    inputSchema: obj({ id, timeout: { type: 'number', description: 'Seconds' } }, ['id']), run: (a) => pickUp(a).then(json) },
  { name: 'portals', description: 'Portals of the current map as the client keeps them: id, cell, destination map, type.', inputSchema: obj(),
    run: async () => { await ensurePage(); return json(await portals()); } },
  { name: 'collect', description: 'Collect from a collectable map object (an NPC entry).', inputSchema: obj({ id }, ['id']), run: (a) => control({ op: 'collect', id: a.id }).then(json) },
  { name: 'use_item', description: 'Use an inventory item the way a double click does (wear equipment; potion, scroll, wing, box, quest item), by vnum or by pocket and slot.',
    inputSchema: obj({ vnum: { type: 'integer' }, pocket: { type: 'integer' }, slot: { type: 'integer' } }),
    run: (a) => control({ op: 'use_item', vnum: a.vnum, pocket: a.pocket, slot: a.slot }).then(json) },
  { name: 'dialogs', description: 'Open yes/no boxes (questions, confirmations, revive, teleport offers): their text and what Yes and No would answer.',
    inputSchema: obj(), run: () => control({ op: 'dialogs' }).then(json) },
  { name: 'answer_dialog', description: 'Press Yes or No on an open yes/no box: the one whose text or answers contain text, else the one at index, else the first.',
    inputSchema: obj({ yes: { type: 'boolean' }, text: { type: 'string', description: 'Part of the box text or of its answers, e.g. "Continue here" or "#revival"' },
      index: { type: 'integer' } }, ['yes']),
    run: (a) => control({ op: 'answer', yes: a.yes, text: a.text, index: a.index }).then(json) },
  { name: 'guard', description: 'Keep the character alive between calls: use hp_item (a potion) when HP is at or below hp_below percent, the same for MP, eat food_item out of combat, and press Yes on any box whose text or answers contain one of accept (e.g. "Teleport", "#revival^0"). Checked before every tool, during walk and wait_for, and every second when idle. No arguments returns the settings and the last things it did; off clears it.',
    inputSchema: obj({ hp_below: { type: 'integer' }, hp_item: { type: 'integer' }, mp_below: { type: 'integer' }, mp_item: { type: 'integer' },
      accept: { type: 'array', items: { type: 'string' } },
      food_item: { type: 'integer', description: 'Food to eat at or below food_below percent HP, only with no monster within safe_radius cells (it sits the character down)' },
      food_below: { type: 'integer' }, safe_radius: { type: 'integer', description: 'Default 6' }, cooldown: { type: 'number', description: 'Seconds between two uses, default 4' },
      off: { type: 'boolean' } }),
    run: async (a) => json(setGuard(a)) },
  { name: 'wait_for', description: 'Wait for a packet (by header, direction and/or text) sent or received since the last action, or for a yes/no box whose text contains dialog. Returns the packet or the box.',
    inputSchema: obj({ header: { type: 'string', description: 'Packet header, e.g. at, c_info, sayi' }, direction: { type: 'string', enum: ['send', 'recv'] },
      contains: { type: 'string' }, dialog: { type: 'string' }, timeout: { type: 'number', description: 'Seconds, default 30' } }),
    run: (a) => waitFor(a).then(json) },
  { name: 'packets', description: 'The last packets the client sent and received (newest last), optionally filtered by header, direction and text.',
    inputSchema: obj({ header: { type: 'string' }, direction: { type: 'string', enum: ['send', 'recv'] }, contains: { type: 'string' },
      last: { type: 'integer', description: 'How many, default 30' } }),
    run: async (a) => { await ensurePage(); return json((await packetsSince(0, a)).list.slice(-(a.last || 30)).map((p) => `${p.direction} ${p.raw}`)); } },
  { name: 'rest', description: 'Sit down or stand up; returns once the server has confirmed it.', inputSchema: obj({ timeout: { type: 'number', description: 'Seconds' } }),
    run: (a) => rest(a).then(json) },
  { name: 'screenshot', description: 'The game screen, with its size in pixels; click takes the same pixel coordinates.', inputSchema: obj(), run: () => screenshot() },
  { name: 'click', description: 'Click the game screen at [x,y] in screenshot pixels, for windows and dialogues.',
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
          if (tool.name !== 'guard' && tool.name !== 'client_close' && tool.name !== 'client_open') await guardTick();
          if (!QUIET.has(tool.name)) actionSeq = await packetSeq();
          const content = await tool.run(args);
          logAction(tool.name, args, content);
          return send({ id: m.id, result: { content: [content].flat() } });
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
let queue = Promise.resolve(), pending = 0, idleTick = false;
setInterval(() => {
  if (pending || idleTick) return;
  idleTick = true;
  queue = queue.then(guardTick).catch(() => { }).finally(() => { idleTick = false; });
}, 1000).unref();
createInterface({ input: process.stdin }).on('line', (line) => {
  if (!line.trim()) return;
  let m;
  try { m = JSON.parse(line); } catch { return send({ id: null, error: { code: -32700, message: 'parse error' } }); }
  pending++;
  queue = queue.then(() => handle(m)).catch(() => { }).finally(() => pending--);
}).on('close', () => queue.then(close).then(() => process.exit(0)));
process.on('SIGTERM', () => close().then(() => process.exit(0)));
