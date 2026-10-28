// Drives the browser client through login into the world, clicks the ground, and saves screenshots and the log.
// The browser is always closed at the end: the client reserves 1 GB+ and stacked runs exhaust RAM.
//
// usage: node drive.mjs [--url http://127.0.0.1:8077/] [--trace] [--out dir] [--headed]
import { createRequire } from 'node:module';
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const require = createRequire(import.meta.url);
const playwrightPath = process.env.PLAYWRIGHT_PATH
  || 'C:/Program Files/nodejs/node_modules/@playwright/cli/node_modules/playwright';
const { chromium } = require(playwrightPath);

const arg = (name, fallback) => {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : fallback;
};
const flag = (name) => process.argv.includes(name);

const out = arg('--out', 'C:/tools/re/work/drive');
const url = arg('--url', 'http://127.0.0.1:8077/') + (flag('--trace') ? '?trace=1' : '');
mkdirSync(out, { recursive: true });

// Screen positions on the 1680x1050 client.
const SERVER_ROW = [788, 422];
const CHANNEL_ROW = [1068, 422];
const CHARACTER_SLOT = [1170, 70];
const START_BUTTON = [840, 965];
const GROUND = [1000, 520];

const browser = await chromium.launch({
  headless: !flag('--headed'),
  channel: process.env.DRIVE_CHANNEL || 'chrome',
  args: ['--use-angle=d3d11', '--enable-gpu', '--ignore-gpu-blocklist', '--enable-unsafe-swiftshader'],
});
let page;
try {
  scenario: {
  page = await browser.newPage({ viewport: { width: 1800, height: 1200 } });
  page.on('pageerror', (e) => console.log('pageerror', e.message));
  await page.goto(url);
  const logText = () => page.evaluate(() => (window.allLog || []).join('\n'));
  const waitFor = async (re, seconds) => {
    for (let i = 0; i < seconds; i++) {
      const t = await logText();
      if (re.test(t)) return true;
      if (/FATAL/.test(t)) return false;
      await page.waitForTimeout(1000);
    }
    return false;
  };
  const canvasAt = async () => page.evaluate(() => {
    const c = document.getElementById('screen');
    c.style.maxWidth = 'none';
    c.style.width = '1680px';
    c.style.height = '1050px';
    const b = c.getBoundingClientRect();
    return { x: b.left, y: b.top };
  });
  const shot = async (name) => {
    const r = await canvasAt();
    await page.screenshot({ path: join(out, name), clip: { x: r.x, y: r.y, width: 1680, height: 1050 } });
  };
  const click = async ([x, y], button = 'left') => {
    const r = await canvasAt();
    await page.mouse.move(r.x + x, r.y + y, { steps: 5 });
    await page.waitForTimeout(300);
    await page.mouse.down({ button });
    await page.waitForTimeout(150);
    await page.mouse.up({ button });
  };
  const step = (name) => console.log(`[${new Date().toISOString().slice(11, 19)}] ${name}`);

  step('waiting for login');
  if (!await waitFor(/closesocket 00000100/, 240)) throw new Error('login did not complete');
  await page.waitForTimeout(4000);
  await shot('1-servers.png');

  step('server and channel');
  await click(SERVER_ROW);
  await page.waitForTimeout(2000);
  await click(CHANNEL_ROW);
  if (!await waitFor(/connect 127\.0\.0\.1:1337/, 60)) throw new Error('no world connection');
  await page.waitForTimeout(3000);
  await shot('2-characters.png');

  if (flag('--charselect-input')) {
    step('input on the character screen');
    const beforeInput = (await logText()).length;
    await click(GROUND);
    await page.waitForTimeout(1500);
    await page.focus('#screen');
    await page.keyboard.press('Enter');
    await page.keyboard.type('hello', { delay: 100 });
    await page.keyboard.press('Enter');
    await page.waitForTimeout(5000);
    await shot('2b-after-input.png');
    const lines = (await logText()).slice(beforeInput).split('\n');
    console.log(lines.filter((l) => /FATAL|exited|RaiseException|Delphi|fault/.test(l)).slice(0, 10).join('\n') || 'no errors');
    break scenario;
  }

  step('enter world');
  await click(CHARACTER_SLOT);
  await page.waitForTimeout(2000);
  const beforeStart = (await logText()).length;
  // The map arrives as several large packets; a START pressed too early is ignored, so press it again once.
  const entered = async (seconds) => {
    for (let i = 0; i < seconds; i++) {
      if (/recv 00000100: \d{4,} bytes/.test((await logText()).slice(beforeStart))) return true;
      await page.waitForTimeout(1000);
    }
    return false;
  };
  await click(START_BUTTON);
  if (!await entered(15)) {
    step('START ignored, pressing again');
    await click(CHARACTER_SLOT);
    await page.waitForTimeout(2000);
    await click(START_BUTTON);
    if (!await entered(20)) throw new Error('did not enter the world');
  }
  await page.waitForTimeout(10000);
  await shot('3-world.png');

  step('click ground');
  const before = (await logText()).length;
  await click(GROUND);
  await page.waitForTimeout(4000);
  await shot('4-after-click.png');
  const after = (await logText()).slice(before);
  writeFileSync(join(out, 'after-click.log'), after);
  console.log(after.split('\n').filter((l) => /^(fn |send |input |FATAL)/.test(l)).slice(0, 60).join('\n'));

  step('chat');
  const beforeChat = (await logText()).length;
  await page.focus('#screen');
  await page.keyboard.press('Enter');
  await page.waitForTimeout(800);
  for (const ch of 'hello from the browser') { await page.keyboard.type(ch); await page.waitForTimeout(120); }
  await page.waitForTimeout(500);
  await shot('5-typing.png');
  await page.keyboard.press('Enter');
  await page.waitForTimeout(3000);
  await shot('6-after-chat.png');
  console.log((await logText()).slice(beforeChat).split('\n').filter((l) => /^(send |FATAL)/.test(l)).join('\n'));
  console.log('status:', await page.evaluate(() => document.getElementById('status').textContent));
  console.log('sounds:', JSON.stringify(await page.evaluate(() => window.soundStats)));

  step('game windows');
  const beforeWindows = (await logText()).length;
  // The chat line keeps focus after a message is sent; Escape hands the keyboard back to the game.
  await page.keyboard.press('Escape');
  await page.waitForTimeout(500);
  for (const key of ['i', 'c', 'k', 'p', 'l', 'f', 'm']) {
    await page.keyboard.press(key);
    await page.waitForTimeout(1500);
    await shot(`7-window-${key}.png`);
    await page.keyboard.press(key);
    await page.waitForTimeout(500);
  }
  const windowErrors = (await logText()).slice(beforeWindows).split('\n')
    .filter((l) => /FATAL|exited|Delphi|fault|STUB/.test(l));
  console.log(windowErrors.slice(0, 12).join('\n') || 'windows: no errors');

  // The test characters are game masters, so a map change is a chat command away.
  for (const map of arg('--maps', '2').split(',')) {
    step(`teleport to map ${map}`);
    const beforeTeleport = (await logText()).length;
    await page.keyboard.press('Enter');
    await page.waitForTimeout(500);
    await page.keyboard.type(`$Teleport ${map}`, { delay: 60 });
    await page.keyboard.press('Enter');
    await page.waitForTimeout(500);
    await page.keyboard.press('Escape');
    await page.waitForTimeout(14000);
    await shot(`8-map-${map}.png`);
    if (flag('--dump-frame')) { await page.keyboard.press('F9'); await page.waitForTimeout(2000); }
    const teleportLog = (await logText()).slice(beforeTeleport).split('\n');
    console.log(teleportLog.filter((l) => /FATAL|exited|Delphi|fault|STUB/.test(l)).slice(0, 12).join('\n') || `map ${map}: no errors`);
    console.log('status:', await page.evaluate(() => document.getElementById('status').textContent));
    if (teleportLog.some((l) => /FATAL|exited/.test(l))) break;
  }
  }
} catch (e) {
  console.log('error:', e.message);
} finally {
  if (page) writeFileSync(join(out, 'full.log'), await page.evaluate(() => (window.allLog || []).join('\n')).catch(() => ''));
  await browser.close();
}
