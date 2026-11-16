// Exercises the real browser launcher UI without accounts, a game or a network listener.
import { createRequire } from 'node:module';
import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
const require = createRequire(import.meta.url);
const { chromium } = require(process.env.PLAYWRIGHT_PATH || 'C:/Program Files/nodejs/node_modules/@playwright/cli/node_modules/playwright');
const html = readFileSync(new URL('../ClientLab.Launcher/Assets/browser-launcher.html', import.meta.url), 'utf8');
const state = { settings: {provider:'noscore'}, signIn:{stage:'NeedCredentials'}, busy:false, running:false, logs:[], message:'Ready' };
let options = {Music:false, SoundEffects:true, SpatialSound:false, FullScreen:true};
let saved;
const browser = await chromium.launch({channel:'chrome', headless:true});
try {
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.route('**/*', async route => {
    const request = route.request(), path = new URL(request.url()).pathname;
    if(path === '/') return route.fulfill({contentType:'text/html', body:html});
    let result = {};
    if(path === '/api/state') result = state;
    else if(path === '/api/news') result = [];
    else if(path === '/api/credentials') result = {Email:'', Password:''};
    else if(path === '/api/game-options') {
      if(request.method() === 'POST') { saved = request.postDataJSON(); options = saved; }
      result = options;
    } else if(path === '/api/settings') Object.assign(state.settings, request.postDataJSON());
    else return route.fulfill({contentType:'image/svg+xml', body:'<svg xmlns="http://www.w3.org/2000/svg"/>'});
    return route.fulfill({json:result});
  });
  await page.goto('http://127.0.0.1:8456/#test-token');
  await page.locator('[data-page=settings]').click();
  await page.waitForFunction(() => document.getElementById('game-fullscreen').checked);
  assert.equal(await page.locator('#game-music').isChecked(), false);
  assert.equal(await page.locator('#game-effects').isChecked(), true);
  assert.equal(await page.locator('#game-spatial').isChecked(), false);
  await page.locator('#game-music').check();
  await page.locator('#game-effects').uncheck();
  await page.locator('#game-spatial').check();
  await page.locator('#game-fullscreen').uncheck();
  await page.waitForFunction(() => document.getElementById('game-preferences-status').textContent.startsWith('Saved.'));
  // No Save settings click is required for game preferences.
  await page.waitForFunction(() => !document.getElementById('game-fullscreen').checked);
  await page.waitForTimeout(150);
  assert.deepEqual(saved, {Music:true, SoundEffects:false, SpatialSound:true, FullScreen:false});
  // The periodic state refresh must not discard unsaved edits.
  await page.waitForTimeout(650);
  assert.equal(await page.locator('#game-music').isChecked(), true);
  await page.locator('#packet-logger').check();
  state.busy = true;
  await page.waitForTimeout(600);
  assert.equal(await page.getByRole('button', {name:'Save settings', exact:true}).isDisabled(), false);
  await page.getByRole('button', {name:'Save settings', exact:true}).click();
  await page.waitForFunction(() => document.getElementById('settings-status').textContent.startsWith('Settings saved.'));
  assert.deepEqual(saved, {Music:true, SoundEffects:false, SpatialSound:true, FullScreen:false});
  assert.equal(state.settings.packet_logger, 'true');
  await page.reload();
  await page.locator('[data-page=settings]').click();
  await page.waitForFunction(() => document.getElementById('game-music').checked);
  assert.equal(await page.locator('#game-effects').isChecked(), false);
  assert.equal(await page.locator('#game-spatial').isChecked(), true);
  assert.equal(await page.locator('#game-fullscreen').isChecked(), false);
  state.running = true;
  await page.waitForFunction(() => document.getElementById('game-preferences').disabled);
  assert.equal(await page.getByRole('button', {name:'Save settings', exact:true}).isDisabled(), false);
  await page.getByRole('button', {name:'Save settings', exact:true}).click();
  await page.waitForFunction(() => document.getElementById('settings-status').textContent.startsWith('Settings saved.'));
  assert.deepEqual(errors, []);
  const packetPage = await browser.newPage();
  await packetPage.addInitScript(() => Object.defineProperty(navigator,'clipboard',{value:{writeText:async text=>{window.copiedPacketText=text;}}}));
  const packetErrors=[];
  packetPage.on('pageerror',error=>packetErrors.push(error.message));
  let extraTraffic=false;
  const packetHtml = readFileSync(new URL('../ClientLab.Launcher/Assets/packet-logger.html', import.meta.url), 'utf8');
  await packetPage.route('**/*', route => route.request().url().includes('/packets?')
    ? route.fulfill({json:{file:'packets-20261116T000000.000Z.jsonl', cursor:extraTraffic?3:2, entries:route.request().url().endsWith('since=0') ? [
      {Id:1,Timestamp:'2026-11-16T00:00:00.000Z',Connection:'World',Direction:'Send',Raw:'walk 10 20'},
      {Id:2,Timestamp:'2026-11-16T00:00:01.000Z',Connection:'World',Direction:'Receive',Raw:'say hello'}] : extraTraffic&&route.request().url().endsWith('since=2') ? [{Id:3,Timestamp:'2026-11-16T00:00:02.000Z',Connection:'World',Direction:'Receive',Raw:'say more'}] : []}})
    : route.fulfill({contentType:'text/html',body:packetHtml}));
  await packetPage.goto('http://127.0.0.1:8457/packet-log');
  await packetPage.waitForFunction(() => document.getElementById('packets').textContent.includes('walk 10 20'));
  const firstPacket=packetPage.locator('.packet').first();
  await firstPacket.click({button:'right'});
  assert.equal(await packetPage.getByRole('menu',{name:'Packet actions'}).isVisible(),true);
  await packetPage.getByRole('menuitem',{name:'Copy raw text',exact:true}).click();
  assert.equal(await packetPage.evaluate(()=>window.copiedPacketText),'walk 10 20');
  await firstPacket.click({button:'right'});
  await firstPacket.evaluate(node=>{node.selectionProbe=true;});
  extraTraffic=true;
  await packetPage.waitForFunction(()=>document.getElementById('packets').textContent.includes('say more'));
  assert.equal(await firstPacket.evaluate(node=>node.selectionProbe),true);
  assert.equal(await firstPacket.getAttribute('aria-selected'),'true');
  assert.equal(await packetPage.getByRole('menu',{name:'Packet actions'}).isVisible(),true);
  await packetPage.getByRole('menuitem',{name:'Copy packet',exact:true}).click();
  assert.equal(await packetPage.evaluate(()=>window.copiedPacketText),'[00:00:00.000] [World] [Client] walk 10 20');
  await firstPacket.click({button:'right'});
  if(process.env.CLIENTLAB_UI_SCREENSHOT)await packetPage.screenshot({path:process.env.CLIENTLAB_UI_SCREENSHOT});
  await packetPage.keyboard.press('Escape');
  assert.equal(await packetPage.getByRole('menu',{name:'Packet actions'}).isVisible(),false);
  await packetPage.locator('#send').uncheck();
  assert.equal((await packetPage.locator('#packets').textContent()).includes('walk'), false);
  assert.equal((await packetPage.locator('#packets').textContent()).includes('say hello'), true);
  await packetPage.getByRole('button',{name:'Clear view'}).click();
  assert.equal(await packetPage.locator('#packets').textContent(), '');
  assert.deepEqual(packetErrors,[]);
  console.log('PASS: browser settings restore, edit, save, survive polling/reload and lock while the game is running.');
  console.log('PASS: packet logger option persists, packet window renders timestamped events and filters/clears its view.');
} finally { await browser.close(); }
