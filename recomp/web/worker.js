// Runs the recompiled client. The game folder is mounted at /C:/Nostale and read on demand from the dev server's
// /game/ (mountGameFile), so nothing of the client is bundled with the page.
const params = new URLSearchParams(self.location.search);

// Input ring shared with the page: [0] = events written, [1] = events read, then 5 ints per event (type, argument, x, y, page time in ms). The worker never
// yields, so postMessage could not deliver input; the host drains this from PeekMessage.
self.inputRing = new Int32Array(new SharedArrayBuffer((2 + 5 * 1024) * 4));
// audioDone[sample] = generation of the last sound on that sample handle that finished playing (hle_audio.c).
self.audioDone = new Int32Array(new SharedArrayBuffer(128 * 4));
// Text pasted in the page: [0] = paste sequence, [1] = length, then UTF-16 code units (hle_menu.c).
self.pasteBuf = new Int32Array(new SharedArrayBuffer(8 + 2 * 65536));
// The page counts the frames it has shown here (one per display refresh at most); Present stays two frames ahead.
self.vsync = new Int32Array(new SharedArrayBuffer(4));
postMessage({ type: 'input', ring: self.inputRing, audioDone: self.audioDone, paste: self.pasteBuf, vsync: self.vsync });

// The page's renderer worker (gl.js) does the WebGL calls; its port arrives right after start, and the game waits
// for it because this worker stops receiving messages once the game runs.
let renderer = null;
self.onmessage = (e) => {
  if (e.data?.type !== 'renderer') return;
  self.glPort = e.data.port;
  const start = renderer;
  renderer = true;
  if (typeof start === 'function') start();
};
const whenRenderer = (start) => { if (renderer === true) start(); else renderer = start; };

// Game files are fetched on demand in 256 KB chunks with synchronous range requests (the worker never yields, so it
// cannot wait on anything asynchronous), and each chunk is kept once read. The client reads its archives at scattered
// offsets, a sprite here and a texture there, so chunks are small; reads copy whole slices. One request covers every
// missing chunk a read needs, and a file being read front to back (a map, the music) is fetched two chunks ahead, so a
// request's fixed cost is paid once rather than per chunk. Every range stays in the browser's cache across reloads.
const CHUNK = 256 * 1024, AHEAD = 2;

function fetchRange(url, from, to) {
  const xhr = new XMLHttpRequest();
  xhr.open('GET', url, false);
  xhr.setRequestHeader('Range', `bytes=${from}-${to}`);
  xhr.responseType = 'arraybuffer';
  xhr.send();
  if (xhr.status !== 206 && xhr.status !== 200) throw new Error(`${url}: HTTP ${xhr.status}`);
  return new Uint8Array(xhr.response);
}

// Loads the missing chunks among first..last, each contiguous run of them in one request; when the read continues
// where the last one ended, the request also takes the chunks ahead.
function loadChunks(game, first, last, sequential) {
  let missing = false;
  for (let n = first; n <= last && !missing; n++) missing = !game.chunks.has(n);
  if (!missing) return;
  if (sequential) last = Math.min(last + AHEAD, Math.ceil(game.size / CHUNK) - 1);
  for (let n = first; n <= last;) {
    if (game.chunks.has(n)) { n++; continue; }
    let end = n;
    while (end < last && !game.chunks.has(end + 1)) end++;
    const bytes = fetchRange(game.url, n * CHUNK, Math.min((end + 1) * CHUNK, game.size) - 1);
    for (let k = n; k <= end; k++) game.chunks.set(k, bytes.subarray((k - n) * CHUNK, (k - n + 1) * CHUNK));
    n = end + 1;
  }
}

function gameRead(game, buffer, offset, length, position) {
  const size = Math.max(0, Math.min(game.size - position, length));
  if (!size) return 0;
  loadChunks(game, Math.floor(position / CHUNK), Math.floor((position + size - 1) / CHUNK), position === game.end);
  game.end = position + size;
  for (let done = 0; done < size;) {
    const at = position + done, chunk = game.chunks.get(Math.floor(at / CHUNK)), from = at % CHUNK;
    const take = Math.min(chunk.length - from, size - done);
    buffer.set(chunk.subarray(from, from + take), offset + done);
    done += take;
  }
  return size;
}

// A game file node is an ordinary in-memory file whose contents arrive on demand. Writing or resizing one loads it
// whole first and turns it into a plain in-memory file, so the client can still rewrite its own files.
function mountGameFile(FS, dir, name, url, size) {
  const node = FS.createFile(dir, name, { isDevice: false }, true, true);
  const file = node.stream_ops, fileNode = node.node_ops;
  node.game = { url, size, chunks: new Map(), end: -1 };
  node.usedBytes = size;
  const materialise = (n) => {
    if (!n.game) return;
    const data = new Uint8Array(n.game.size);
    gameRead(n.game, data, 0, n.game.size, 0);
    n.game = null;
    n.contents = data;
    n.usedBytes = data.length;
    n.stream_ops = file;
  };
  const plain = (op) => (stream, ...args) => {
    materialise(stream.node);
    stream.stream_ops = file;
    return file[op](stream, ...args);
  };
  node.stream_ops = {
    ...file,
    read: (stream, buffer, offset, length, position) =>
      stream.node.game ? gameRead(stream.node.game, buffer, offset, length, position) : file.read(stream, buffer, offset, length, position),
    write: plain('write'),
    mmap: plain('mmap'),
  };
  node.node_ops = { ...fileNode, setattr: (n, attr) => { materialise(n); return fileNode.setattr(n, attr); } };
}

// The client reads Config.ini at start and writes its in-game options to Config.dat when it shuts down. The C# host
// keeps both files outside patched game versions, so desktop and browser launches restore exactly the same bytes.
const SETTINGS_FILES = ['config.ini', 'config.dat'];

function installGameSettings(FS) {
  const report = text => postMessage({ type: 'log', text });
  const settingsFile = name => {
    if (typeof name !== 'string') return null;
    const path = name.replace(/\\/g, '/').toLowerCase();
    return SETTINGS_FILES.find(file => path === `/c:/nostale/${file}`) ?? null;
  };
  const lastSaved = new Map();
  for (const file of SETTINGS_FILES) {
    const path = `/c:/nostale/${file}`;
    try {
      const request = new XMLHttpRequest();
      request.open('GET', `/game-settings/${file}`, false);
      request.responseType = 'arraybuffer';
      request.send();
      // Older standalone game hosts still work without the persistence extension.
      if (request.getResponseHeader('X-ClientLab-Settings') !== 'enabled') return;
      if (request.status === 200) {
        if (FS.analyzePath(path).exists) FS.unlink(path);
        FS.writeFile(path, new Uint8Array(request.response));
      } else if (request.status !== 404) {
        report('Game settings could not be restored; using the available client defaults.');
      }
    } catch (error) {
      report('Game settings could not be restored; using the available client defaults.');
      return;
    }
  }
  const save = file => {
    try {
      const bytes = FS.readFile(`/c:/nostale/${file}`), previous = lastSaved.get(file);
      if (previous && bytes.length === previous.length && bytes.every((value, index) => value === previous[index])) return;
      const request = new XMLHttpRequest();
      request.open('POST', `/game-settings/${file}`, false);
      request.setRequestHeader('Content-Type', 'application/octet-stream');
      request.send(bytes);
      if (request.status !== 204) throw new Error('Settings save failed');
      lastSaved.set(file, bytes.slice());
    } catch (error) { report('Game settings could not be saved. Check Activity and the data folder permissions.'); }
  };
  const close = FS.close;
  FS.close = function(stream) {
    let file = null;
    try { if ((stream.flags & 3) !== 0) file = settingsFile(FS.getPath(stream.node)); } catch (error) { }
    const result = close.call(FS, stream);
    if (file) save(file);
    return result;
  };
  const rename = FS.rename;
  FS.rename = function(from, to) {
    const result = rename.call(FS, from, to);
    let file = settingsFile(to);
    try { file ??= settingsFile(FS.getPath(FS.lookupPath(to).node)); } catch (error) { }
    if (file) save(file);
    return result;
  };
}

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
    mountGameFile(FS, dir, name, `/game/${encodeURI(f.path)}?v=${f.mtime}`, f.size);
  }
  installGameSettings(FS);
  postMessage({ type: 'log', text: `mounted ${files.length} game files` });
}

self.Module = {
  noInitialRun: true,
  locateFile: (path) => `/build/${path}`,
  print: (text) => postMessage({ type: 'log', text }),
  printErr: (text) => postMessage({ type: 'log', text }),
  onExit: (code) => postMessage({ type: 'exited', code }),
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
    const installationId = params.get('NC_INSTALLATION_ID');
    if (installationId) Module.ENV.NC_INSTALLATION_ID = installationId;
    if (params.get('trace')) Module.ENV.PORT_TRACE_CLICK = '1';
    // Debug switches read by the host (PORT_TRACE, PORT_PROFILE_API, ...) pass straight through.
    for (const [k, v] of params) if (k.startsWith('PORT_')) Module.ENV[k] = v;
  }],
  onRuntimeInitialized() {
    mountGame(Module.FS);
    whenRenderer(() => {
      try {
        Promise.resolve(Module.callMain([`C:/Nostale/${params.get('exe') || 'NosCore.exe'}`, 'gf', params.get('region') || '0']))
          .catch((e) => postMessage({ type: 'log', text: `exited: ${e}` }));
      } catch (e) {
        postMessage({ type: 'log', text: `exited: ${e}` });
      }
    });
  },
};

importScripts('/build/nostale.js');
