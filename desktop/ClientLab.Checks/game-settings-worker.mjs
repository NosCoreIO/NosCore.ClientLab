// Isolated worker/host protocol test: no game, credentials, browser, or HTTP listener.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const worker = fs.readFileSync(new URL('../../recomp/web/worker.js', import.meta.url), 'utf8');
const settingsPath = '/c:/nostale/config.ini';
const optionsPath = '/c:/nostale/config.dat';
const host = new Map([['/game-settings/config.ini', new TextEncoder().encode('[EW_Sound_Config]\r\nBGMSoundOff=1\r\nFxSoundOff=0\r\n')]]);
let saved = host.get('/game-settings/config.ini');
let writes = 0, failSave = false, supported = true;
const logs = [];
function instance() {
  const files = new Map();
  const full = path => (path.startsWith('/') ? path : '/c:/nostale/' + path).toLowerCase();
  const FS = {
    analyzePath: path => ({ exists: files.has(full(path)) }),
    unlink: path => files.delete(full(path)),
    writeFile: (path, bytes) => files.set(full(path), Uint8Array.from(bytes)),
    readFile: path => { if (!files.has(full(path))) throw Error('ENOENT'); return files.get(full(path)).slice(); },
    getPath: node => node.path,
    lookupPath: path => ({ node: { path: full(path) } }),
    close: () => {},
    rename: (from, to) => { files.set(full(to), files.get(full(from))); files.delete(full(from)); }
  };
  class Xhr {
    open(method, path, async) { assert(['/game-settings/config.ini', '/game-settings/config.dat'].includes(path)); assert.equal(async, false); this.method = method; this.path = path; }
    setRequestHeader() {}
    getResponseHeader(name) { assert.equal(name, 'X-ClientLab-Settings'); return supported ? 'enabled' : null; }
    send(body) {
      const file = host.get(this.path);
      if (this.method === 'GET') { this.status = file ? 200 : 404; this.response = file?.slice().buffer; }
      else {
        this.status = failSave ? 500 : 204;
        if (!failSave) { host.set(this.path, Uint8Array.from(body)); writes++; if (this.path.endsWith('.ini')) saved = host.get(this.path); }
      }
    }
  }
  const context = vm.createContext({ URLSearchParams, SharedArrayBuffer, Int32Array, Uint8Array, XMLHttpRequest: Xhr,
    postMessage: message => { if (message.type === 'log') logs.push(message.text); }, importScripts: () => {} });
  context.self = context; context.location = { search: '' };
  vm.runInContext(worker, context);
  const originalClose = FS.close;
  context.installGameSettings(FS);
  return { FS, files, originalClose };
}
let { FS, files } = instance();
assert.deepEqual([...FS.readFile(settingsPath)], [...saved], 'Saved original-format settings were not restored');
const changed = new TextEncoder().encode('[EW_Sound_Config]\r\nBGMSoundOff=0\r\nFxSoundOff=1\r\n');
FS.writeFile(settingsPath, changed); FS.close({ flags: 1, node: { path: settingsPath } });
assert.equal(writes, 1); assert.deepEqual([...saved], [...changed]);
FS.close({ flags: 0, node: { path: settingsPath } });
FS.close({ flags: 1, node: { path: '/c:/nostale/archive.nos' } });
assert.equal(writes, 1, 'Reading settings or modifying another game file caused a settings write');
FS.close({ flags: 2, node: { path: settingsPath } });
assert.equal(writes, 1, 'Unchanged settings were uploaded again');
const replaced = new TextEncoder().encode('[EW_Multi_Navigator]\r\nFullScreen=0\r\n');
FS.writeFile('config.ini.tmp', replaced); FS.rename('config.ini.tmp', 'Config.ini');
assert.equal(writes, 2); assert.deepEqual([...saved], [...replaced]);
({ FS, files } = instance());
assert.deepEqual([...FS.readFile(settingsPath)], [...replaced], 'Restarting the worker lost saved preferences');
failSave = true; FS.writeFile(settingsPath, changed); FS.close({ flags: 1, node: { path: settingsPath } });
assert.deepEqual([...FS.readFile(settingsPath)], [...changed], 'Persistence failure broke the current game settings');
assert(logs.some(text => text.includes('could not be saved')));
assert.deepEqual([...saved], [...replaced], 'A failed write damaged the last durable settings');
failSave = false; FS.close({ flags: 1, node: { path: settingsPath } });
assert.deepEqual([...saved], [...changed], 'Settings were not retried after a failed save');
({ FS, files } = instance());
assert.equal(FS.analyzePath(optionsPath).exists, false, 'In-game options appeared before the client ever saved them');
const options = Uint8Array.from({ length: 1024 }, (_, index) => index % 251);
FS.writeFile(optionsPath, options); FS.close({ flags: 1, node: { path: optionsPath } });
assert.deepEqual([...host.get('/game-settings/config.dat')], [...options], 'In-game options written at shutdown were not saved');
({ FS, files } = instance());
assert.deepEqual([...FS.readFile(optionsPath)], [...options], 'Restarting the worker lost the in-game options');
assert.deepEqual([...FS.readFile(settingsPath)], [...changed], 'Saving the in-game options changed Config.ini');
supported = false;
const olderHost = instance(); assert.equal(olderHost.FS.close, olderHost.originalClose, 'An older game host lost its normal file behavior');
console.log('PASS: worker restores original Config.ini and Config.dat bytes, persists write-close and atomic rename, survives restart, excludes other game files, and retries failed saves.');
