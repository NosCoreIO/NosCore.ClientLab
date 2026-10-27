// Runs the recompiled client. The game folder is mounted lazily at /C:/Nostale from the dev server's /game/ (one sync
// range request per touched chunk), so nothing of the client is bundled with the page.
const params = new URLSearchParams(self.location.search);

// Input ring shared with the page: [0] = events written, [1] = events read, then 4 ints per event. The worker never
// yields, so postMessage could not deliver input; the host drains this from PeekMessage.
self.inputRing = new Int32Array(new SharedArrayBuffer((2 + 4 * 1024) * 4));
postMessage({ type: 'input', ring: self.inputRing });

function mountGame(FS) {
  const xhr = new XMLHttpRequest();
  xhr.open('GET', '/game-index.json', false);
  xhr.send();
  const files = JSON.parse(xhr.responseText);
  // Lower-cased to match the host's case-insensitive path mapping (port_host_path).
  const root = '/c:/nostale';
  for (const f of files) {
    const lower = f.path.toLowerCase();
    const slash = lower.lastIndexOf('/');
    const dir = slash < 0 ? root : `${root}/${lower.slice(0, slash)}`;
    const name = slash < 0 ? lower : lower.slice(slash + 1);
    FS.mkdirTree(dir);
    FS.createLazyFile(dir, name, `/game/${encodeURI(f.path)}`, true, true);
  }
  postMessage({ type: 'log', text: `mounted ${files.length} game files` });
}

self.Module = {
  noInitialRun: true,
  locateFile: (path) => `/build/${path}`,
  print: (text) => postMessage({ type: 'log', text }),
  printErr: (text) => postMessage({ type: 'log', text }),
  preRun: [() => {
    let code = params.get('auth');
    if (!code) {
      const xhr = new XMLHttpRequest();
      xhr.open('GET', '/auth-code', false);
      xhr.send();
      if (xhr.status === 200) code = xhr.responseText;
      postMessage({ type: 'log', text: code ? 'auth code from dev server' : `no auth code (${xhr.status} ${xhr.responseText})` });
    }
    if (code) Module.ENV._NC_AUTH_CODE = code;
    if (params.get('trace')) Module.ENV.PORT_TRACE_CLICK = '1';
  }],
  onRuntimeInitialized() {
    mountGame(Module.FS);
    try {
      Module.callMain([`C:/Nostale/${params.get('exe') || 'NosCore.exe'}`, 'gf', params.get('region') || '0']);
    } catch (e) {
      postMessage({ type: 'log', text: `exited: ${e}` });
    }
  },
};

importScripts('/build/nostale.js');
