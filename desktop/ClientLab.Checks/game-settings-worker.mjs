// Isolated worker/host protocol test: no game, credentials, browser, or HTTP listener.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const worker = fs.readFileSync(new URL('../../recomp/web/worker.js', import.meta.url), 'utf8');
const settingsPath = '/c:/nostale/config.ini';
let saved = new TextEncoder().encode('[EW_Sound_Config]\r\nBGMSoundOff=1\r\nFxSoundOff=0\r\n');
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
    open(method, path, async) { assert.equal(path, '/game-settings/config.ini'); assert.equal(async, false); this.method = method; }
    setRequestHeader() {}
    getResponseHeader(name) { assert.equal(name, 'X-ClientLab-Settings'); return supported ? 'enabled' : null; }
    send(body) {
      if (this.method === 'GET') { this.status = saved ? 200 : 404; this.response = saved?.slice().buffer; }
      else { this.status = failSave ? 500 : 204; if (!failSave) { saved = Uint8Array.from(body); writes++; } }
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
supported = false;
const olderHost = instance(); assert.equal(olderHost.FS.close, olderHost.originalClose, 'An older game host lost its normal file behavior');
console.log('PASS: worker restores original INI bytes, persists write-close and atomic rename, survives restart, excludes other game files, and retries failed saves.');
