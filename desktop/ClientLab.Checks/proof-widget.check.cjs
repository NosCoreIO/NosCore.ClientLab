const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../ClientLab.Launcher/Assets/gameforge-verification.js'), 'utf8');
function setup(mode) {
  const listeners = new Set(), frame = { contentWindow: {} }, status = { hidden: false };
  let removed = false, loadFails = false, scripts = 0;
  const host = { querySelector: () => frame, remove() { removed = true; } };
  const window = { addEventListener: (_, fn) => listeners.add(fn), removeEventListener: (_, fn) => listeners.delete(fn) };
  const dispatch = (origin, sender, type) => { for (const fn of [...listeners]) fn({ origin, source: sender, data: { type } }); };
  const widget = (_, options) => new Promise((resolve, reject) => {
    assert.equal(options.challengeId, 'test-id');
    queueMicrotask(() => {
      const origin = mode === 'foreign-origin' ? 'https://untrusted.invalid' : 'https://pow-captcha.gameforge.com';
      const sender = mode === 'foreign-frame' ? {} : frame.contentWindow;
      const type = mode === 'failed' ? 'pow-captcha-failed' : mode === 'cancelled' ? 'pow-captcha-canceled' : 'pow-captcha-solved';
      dispatch(origin, sender, type);
      if (mode === 'cancelled') { const error = new Error('cancelled'); error.name = 'CaptchaCanceledError'; reject(error); }
      else resolve();
    });
  });
  window['gf-pow-captcha'] = widget;
  const document = { querySelector: () => status, body: { appendChild() {} },
    createElement: type => type === 'div' ? host : { remove() {} },
    head: { appendChild(script) { scripts++; queueMicrotask(() => { if (loadFails) script.onerror(); else { window['gf-pow-captcha'] = widget; script.onload(); } }); } } };
  const context = { window, document, setTimeout, clearTimeout };
  vm.runInNewContext(source, context);
  return { run: () => context.ClientLabGameforgeVerification.run('test-id'),
    cleanup: () => { assert.equal(listeners.size, 0); assert.equal(removed, true); assert.equal(status.hidden, false); },
    setLoadFailure: value => { delete window['gf-pow-captcha']; loadFails = value; }, scripts: () => scripts };
}
(async () => {
  const success = setup('success'); await success.run(); success.cleanup();
  for (const mode of ['failed', 'foreign-origin', 'foreign-frame']) {
    const test = setup(mode); await assert.rejects(test.run(), /verification failed/); test.cleanup();
  }
  const cancelled = setup('cancelled'); await assert.rejects(cancelled.run(), error => error.name === 'AbortError'); cancelled.cleanup();
  const retry = setup('success'); retry.setLoadFailure(true); await assert.rejects(retry.run(), /Could not load/);
  retry.setLoadFailure(false); await retry.run(); assert.equal(retry.scripts(), 2); retry.cleanup();
  console.log('PASS: official PoW adapter accepts only its own iframe success, rejects failure/cancellation/spoofed messages, cleans up, and retries a failed script load.');
})().catch(error => { console.error(error); process.exitCode = 1; });
