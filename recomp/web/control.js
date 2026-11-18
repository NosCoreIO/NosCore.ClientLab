// Drives the running client through its own functions instead of the mouse, for automated tests and agents.
// The page writes one JSON request at a time into a shared buffer; the game thread serves it between window messages
// (hle_control.c), reading the client's objects straight from guest memory and calling the client's functions with
// Delphi's register convention. Nothing here builds or sends a packet: every action is a call the client's own UI
// makes, so the client sends whatever it would have sent for that click.
//
// Buffer: Int32 [0] = request sequence, [1] = answered sequence, [2] = request length, [3] = answer length, then the
// UTF-8 JSON bytes from offset 16.
'use strict';

const CONTROL_BYTES = 8 << 20;
self.controlBuf = new SharedArrayBuffer(CONTROL_BYTES);

(() => {
  const head = new Int32Array(self.controlBuf, 0, 4);
  const body = new Uint8Array(self.controlBuf, 16);
  const encoder = new TextEncoder(), utf8 = new TextDecoder();
  const ansi = new TextDecoder('windows-1252');

  // ---- guest memory (identity-mapped into the wasm heap) ----
  let view = null;
  const dv = () => (view && view.buffer === HEAPU8.buffer ? view : (view = new DataView(HEAPU8.buffer)));
  const inRange = (a, n = 4) => a >= 0x10000 && a + n <= HEAPU8.length;
  const u8 = (a) => (inRange(a, 1) ? HEAPU8[a] : 0);
  const s8 = (a) => (u8(a) << 24) >> 24;
  const u16 = (a) => (inRange(a, 2) ? dv().getUint16(a, true) : 0);
  const s16 = (a) => (u16(a) << 16) >> 16;
  const i32 = (a) => (inRange(a) ? dv().getInt32(a, true) : 0);
  const u32 = (a) => (inRange(a) ? dv().getUint32(a, true) : 0);
  const ptr = (a) => (a ? u32(a) : 0);
  const str = (p, max = 64) => {
    if (!inRange(p, 1)) return '';
    let end = p;
    while (end < p + max && end < HEAPU8.length && HEAPU8[end]) end++;
    return ansi.decode(HEAPU8.slice(p, end));
  };
  // Delphi TList: VMT, +4 item array, +8 count.
  const listCount = (l) => (l && u32(l) ? Math.max(0, Math.min(i32(l + 8), 4096)) : 0);
  const listItem = (l, i) => ptr(ptr(l + 4) + i * 4);

  // ---- locating the client's globals and functions by their code ----
  const code = () => {
    const pe = 0x400000 + u32(0x400000 + 0x3c);
    const sections = u16(pe + 6), first = pe + 24 + u16(pe + 20);
    for (let k = 0; k < sections; k++) {
      const s = first + 40 * k;
      if (u32(s + 36) & 0x20000000) return { start: 0x400000 + u32(s + 12), size: u32(s + 8) };
    }
    return { start: 0x401000, size: 0 };
  };
  const parse = (sig) => sig.split(' ').map((b) => (b === '??' ? -1 : parseInt(b, 16)));
  function scan(sig) {
    const p = parse(sig), { start, size } = code(), end = start + size - p.length;
    const bytes = HEAPU8;
    for (let a = bytes.indexOf(p[0], start); a >= 0 && a <= end; a = bytes.indexOf(p[0], a + 1)) {
      let k = 1;
      while (k < p.length && (p[k] < 0 || bytes[a + k] === p[k])) k++;
      if (k === p.length) return a;
    }
    return 0;
  }
  // A global behind `mov eax, [slot]`: the slot holds the address of the Delphi variable that holds the object.
  const global = (sig) => { const site = scan(sig); return site ? ptr(u32(site + 1)) : 0; };
  const callTarget = (sig) => { const site = scan(sig); return site ? (site + 5 + i32(site + 1)) >>> 0 : 0; };

  const SIG = {
    scene: 'A1 ?? ?? ?? ?? 8B 00 8B 70 ?? 8D 86',
    player: 'A1 ?? ?? ?? ?? 8B 00 3B 78 ?? 0F 84',
    inventory: 'A1 ?? ?? ?? ?? 8B 00 8B 80 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 7D ?? ?? 75',
    mates: 'A1 ?? ?? ?? ?? 8B 00 8B 10 FF 52 ?? 6A ?? A1',
    buffs: 'A1 ?? ?? ?? ?? 8B 00 8B 80 ?? ?? ?? ?? 8B 10 FF 52 ?? E8',
    skills: 'A1 ?? ?? ?? ?? 8B 00 8B 80 ?? ?? ?? ?? 8B D3 E8 ?? ?? ?? ?? 8B D8',
    skillUi: 'A1 ?? ?? ?? ?? 8B 00 66 8B 4F',
    mateManager: 'A1 ?? ?? ?? ?? 8B 00 8A 14',
    targetUi: 'A1 ?? ?? ?? ?? 8B 00 80 78 ?? ?? 74 ?? A1 ?? ?? ?? ?? 8B 00 E8 ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? 83 3A ?? 76 ?? ' +
      '8B 15 ?? ?? ?? ?? 8B 12 FF 02 80 3D ?? ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 6A ?? 6A ?? 6A ?? 8B 15 ?? ?? ?? ?? 8B 12 33 C9',
    preWalk: '80 78 ?? ?? 74 ?? 8B 15 ?? ?? ?? ?? 8B 12',
    walk: '55 8B EC 83 C4 ?? 53 56 57 66 89 4D ?? 89 55 ?? 8B F8 33 DB',
    interact: '66 83 B8 ?? ?? ?? ?? ?? 7D ?? 6A',
    armSkill: '53 56 57 55 51 88 0C ?? 8B F2 8B F8',
    pickup: '53 56 57 55 8B F2 8B F8 33 DB 8B C7',
    collect: '53 56 57 55 8B F2 8B E8 33 DB A1',
    rest: '55 8B EC B9 ?? ?? ?? ?? 6A ?? 6A ?? 49 75 ?? 51 53 56 57 33 C0',
    select: '55 8B EC 83 C4 ?? 53 56 88 4D ?? 89 55 ?? 89 45 ?? EB ?? EB ?? 39 19 83 7D',
    // Every yes/no box (qna, dlg and their i18n forms) comes from one pool; this takes the next box and stores its answer callback.
    dialogs: '55 8B EC 53 A1 ?? ?? ?? ?? 8B 00 E8 ?? ?? ?? ?? 8B D8 8B 83 ?? ?? ?? ?? 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 45 08 89 83',
    // A yes/no box button: hide the box, then call its callback with the button's tag (1 yes, 0 no).
    // What a double click on an inventory slot calls: (game, slot record) where the record is item, kind, pocket, slot.
    useItem: '53 56 8B DA 8B F0 83 3B 00 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 31 03',
    // Its callers load the game object behind `mov eax, [slot]` right after `lea edx, [esi+0xb0]`.
    game: '8D 96 B0 00 00 00 A1 ?? ?? ?? ?? 8B 00 E8',
    // The gp handler growing the map's portal array (28-byte records) by one before filling it.
    portals: 'A1 ?? ?? ?? ?? 8B 00 E8 ?? ?? ?? ?? 8B D8 8D 43 01 50 A1 ?? ?? ?? ?? B9 01 00 00 00 8B 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 04 8D 4D D4',
    // The client's string allocator, used for a small scratch record the client's own code can read.
    newString: '85 C0 7E ?? 50 83 C0 0A 83 E0 FE 50 E8',
    dialogButton: '53 56 8B F2 8B D8 8A 15 ?? ?? ?? ?? 8B C3 8B 08 FF 51 ?? 66 83 BB ?? ?? ?? ?? 00 74 ?? 8B 46 ?? 50 8B 4B ?? 8B D3 8B 83 ?? ?? ?? ?? FF 93',
  };
  let api = null;
  function resolve() {
    if (api) return api;
    api = {
      scene: global(SIG.scene), player: global(SIG.player), inventory: global(SIG.inventory), mates: global(SIG.mates),
      buffs: global(SIG.buffs), skills: global(SIG.skills), skillUi: global(SIG.skillUi),
      mateManager: global(SIG.mateManager), targetUi: global(SIG.targetUi),
      preWalk: scan(SIG.preWalk), walk: scan(SIG.walk), interact: scan(SIG.interact), armSkill: scan(SIG.armSkill),
      pickup: scan(SIG.pickup), collect: scan(SIG.collect), rest: scan(SIG.rest), select: scan(SIG.select),
      dialogs: (() => { const site = scan(SIG.dialogs); return site ? ptr(u32(site + 5)) : 0; })(),
      dialogButton: scan(SIG.dialogButton),
      useItem: scan(SIG.useItem),
      game: (() => { const site = scan(SIG.game); return site ? ptr(u32(site + 7)) : 0; })(),
      newString: scan(SIG.newString),
      portals: global(SIG.portals),
    };
    return api;
  }
  const obj = (name) => ptr(resolve()[name]);

  // ---- object layouts ----
  const SCENE = { players: 0x0c, monsters: 0x10, npcs: 0x14, items: 0x18, grid: 0x20, groundX: 0x54, groundY: 0x56 };
  const ENT = { kind: 0x04, id: 0x08, x: 0x0c, y: 0x0e, hp: 0xc8, mp: 0xc9, collect: 0x158, mateKind: 0x160,
    untargetable: 0x194, map: 0x1a4, morph: 0x1b9, vnum: 0x1c2, def: 0x1c4, displayName: 0x1dc, playerName: 0x1f8,
    level: 0x150, heroLevel: 0x151 };
  const DEF = { name: 0x04, race: 0x0c, flags: 0x2f, boss: 0x3a0 };
  const MGR = { action: 0x14, entity: 0x20, id: 0x24, kind: 0x28, sitting: 0xa9 };
  const OBJECT_RACE = 8, UNTARGETABLE = 0x10;
  const TARGET_UI = { entity: 0x34, selected: 0x38 };
  const SKILL = { def: 0x08, cooldown: 0x24, name: 0x14, castId: 0x18, self: 0x1c, range: 0x30, target: 0x38, area: 0x40,
    combo: 0x68, mp: 0x102, list: 0x160 };

  const KINDS = { 1: 'player', 2: 'monster', 3: 'npc' };
  function entityInfo(e, kind = KINDS[u8(e + ENT.kind)] || 'object') {
    const def = ptr(e + ENT.def), race = def ? i32(def + DEF.race) : 0;
    let name = kind === 'player' ? str(ptr(e + ENT.playerName)) : str(ptr(def + DEF.name));
    if (!name) name = str(ptr(e + ENT.displayName));
    return {
      id: i32(e + ENT.id), kind, vnum: s16(e + ENT.vnum), x: u16(e + ENT.x), y: u16(e + ENT.y), hp: s8(e + ENT.hp),
      mp: s8(e + ENT.mp), name, boss: !!(def && u8(def + DEF.boss)),
      targetable: !(race === OBJECT_RACE || (def && u8(def + DEF.flags) === UNTARGETABLE) || u8(e + ENT.untargetable) === 1),
      collectable: kind === 'npc' && race === OBJECT_RACE && u8(e + ENT.kind) === 3 && u8(e + ENT.collect) === 1,
    };
  }

  function entities(list, kind, selfId, keep = () => true) {
    const out = [];
    for (let i = 0, n = listCount(list); i < n; i++) {
      const e = listItem(list, i), id = i32(e + ENT.id);
      if (!e || id <= 0 || (kind === 'player' && id === selfId)) continue;
      const info = entityInfo(e, kind);
      if (keep(info)) out.push(info);
    }
    return out;
  }

  function groundItems(list, keep = () => true) {
    const out = [];
    for (let i = 0, n = listCount(list); i < n; i++) {
      const it = listItem(list, i), id = i32(it + ENT.id);
      if (!it || id <= 0) continue;
      const info = { id, vnum: s16(it + 0xca), x: u16(it + ENT.x), y: u16(it + ENT.y), owner: i32(it + 0xd8),
        name: str(ptr(ptr(it + 0xcc) + 0x38)) };
      if (keep(info)) out.push(info);
    }
    return out;
  }

  function inventoryItems() {
    const inv = obj('inventory'), out = [];
    if (!inv) return out;
    for (const pocket of [0x228, 0x22c, 0x230]) {
      const list = ptr(inv + pocket);
      for (let i = 0, n = listCount(list); i < n; i++) {
        const item = ptr(listItem(list, i)), def = ptr(item + 0x08);
        if (item && def) out.push({ item, def });
      }
    }
    return out;
  }
  function inventory() {
    const inv = obj('inventory'), out = [];
    if (!inv) return out;
    for (const pocket of [0x228, 0x22c, 0x230]) {
      const list = ptr(inv + pocket);
      for (let i = 0, n = listCount(list); i < n; i++) {
        const item = ptr(listItem(list, i)), def = ptr(item + 0x08);
        if (!item || !def) continue;
        out.push({ vnum: i32(def + 0x30), pocket: s8(item + 0x1c), slot: s16(item + 0x1e), amount: s16(item + 0x20),
          name: str(ptr(def + 0x38)) });
      }
    }
    return out;
  }

  function skills() {
    const list = ptr(obj('skills') + SKILL.list), out = [];
    for (let i = 0, n = listCount(list); i < n; i++) {
      const inst = ptr(listItem(list, i)), def = ptr(inst + SKILL.def);
      if (!inst || !def) continue;
      const range = u8(def + SKILL.range);
      out.push({ castId: u16(def + SKILL.castId), vnum: i32(def), name: str(ptr(def + SKILL.name)),
        ready: i32(inst + SKILL.cooldown) === 0, range, area: u8(def + SKILL.area), self: u8(def + SKILL.self) === 2,
        ground: u8(def + SKILL.target) === 3, mp: u16(def + SKILL.mp) });
    }
    return out;
  }

  // Cells between two points as the client counts them for skill range: the larger of the two axis distances.
  const cells = (a, b) => Math.max(Math.abs(a.x - b.x), Math.abs(a.y - b.y));

  // radius limits the lists to that many cells around the character, vnums to those monster/NPC/item vnums, and
  // kinds to some of monsters, npcs, players and items.
  function state({ radius, vnums, kinds } = {}) {
    const mgr = obj('player'), self_ = ptr(mgr + MGR.entity), id = i32(mgr + MGR.id);
    if (!mgr || !self_ || id <= 0) return { inWorld: false };
    const ui = obj('targetUi'), target = u8(ui + TARGET_UI.selected) ? ptr(ui + TARGET_UI.entity) : 0;
    const scene = obj('scene');
    const selected = inRange(target) ? entityInfo(target) : null;
    const me = { x: u16(self_ + ENT.x), y: u16(self_ + ENT.y) };
    const keep = (e) => {
      e.distance = cells(me, e);
      return (radius === undefined || radius === null || e.distance <= radius) && (!vnums?.length || vnums.includes(e.vnum));
    };
    const want = (k) => (!kinds?.length || kinds.includes(k));
    const nearest = (l) => l.sort((a, b) => a.distance - b.distance);
    const list = (k, kind) => (want(k) ? nearest(entities(ptr(scene + SCENE[k]), kind, id, keep)) : undefined);
    return {
      inWorld: true,
      character: { id, name: str(ptr(self_ + ENT.playerName)), map: i32(self_ + ENT.map), x: u16(self_ + ENT.x),
        y: u16(self_ + ENT.y), level: u8(self_ + ENT.level), heroLevel: u8(self_ + ENT.heroLevel), hp: s8(self_ + ENT.hp),
        mp: s8(self_ + ENT.mp), sitting: !!u8(mgr + MGR.sitting), morph: u8(self_ + ENT.morph), action: i32(mgr + MGR.action) },
      target: selected,
      monsters: list('monsters', 'monster'),
      npcs: list('npcs', 'npc'),
      players: list('players', 'player'),
      items: want('items') ? nearest(groundItems(ptr(scene + SCENE.items), keep)) : undefined,
    };
  }

  // The portals of the current map as the client keeps them from its gp packets.
  const PORTAL = { size: 28, x: 0x00, y: 0x02, map: 0x10, type: 0x14, id: 0x18 };
  function portals() {
    const arr = obj('portals'), out = [];
    if (!arr) return out;
    const n = Math.min(i32(arr - 4), 1024);
    for (let i = 0; i < n; i++) {
      const p = arr + i * PORTAL.size;
      out.push({ id: s16(p + PORTAL.id), x: s16(p + PORTAL.x), y: s16(p + PORTAL.y), map: i32(p + PORTAL.map), type: s16(p + PORTAL.type) });
    }
    return out;
  }

  function grid() {
    const g = ptr(obj('scene') + SCENE.grid), w = u16(g), h = u16(g + 2);
    if (!g || !w || !h || w > 1024 || h > 1024) return null;
    // One character per row cell: '.' walkable, '#' blocked.
    const rows = [];
    for (let y = 0; y < h; y++) {
      let row = '';
      for (let x = 0; x < w; x++) row += HEAPU8[g + 4 + x + y * w] ? '#' : '.';
      rows.push(row);
    }
    return { width: w, height: h, rows };
  }

  // ---- actions: the calls the client's own clicks and keys end in ----
  const call = (fn, a = 0, d = 0, c = 0, stack = []) => {
    if (!fn) throw new Error('client function not found in this build');
    if (stack.length > 3) throw new Error('at most three stack arguments are supported');
    return Module._control_call(fn, a, d, c, stack.length, stack[0] ?? 0, stack[1] ?? 0, stack[2] ?? 0) >>> 0;
  };
  const findIn = (list, id) => {
    for (let i = 0, n = listCount(list); i < n; i++) { const e = listItem(list, i); if (e && i32(e + ENT.id) === id) return e; }
    return 0;
  };
  const sceneList = (k) => ptr(obj('scene') + SCENE[k]);
  const entity = (id) => findIn(sceneList('monsters'), id) || findIn(sceneList('npcs'), id) || findIn(sceneList('players'), id);
  const need = (v, what) => { if (!v) throw new Error(what); return v; };
  const player = () => need(obj('player'), 'not in the world');

  function skillDef(castId) {
    const list = ptr(obj('skills') + SKILL.list);
    for (let i = 0, n = listCount(list); i < n; i++) {
      const inst = ptr(listItem(list, i)), def = ptr(inst + SKILL.def);
      if (def && u16(def + SKILL.castId) === castId) return { def, ready: i32(inst + SKILL.cooldown) === 0 };
    }
    return null;
  }

  // ---- yes/no boxes ----
  const DIALOG = { visible: 0x18, code: 0xf0, text: 0xc8, yes: 0x104, no: 0x108 };
  const delphiString = (p) => (p ? str(p, Math.min(Math.max(i32(p - 4), 0), 1024)) : '');
  // A WideString: UTF-16, its byte length in front.
  const wideString = (p) => {
    const n = p ? Math.min(Math.max(i32(p - 4), 0), 2048) & ~1 : 0;
    return n && inRange(p, n) ? new TextDecoder('utf-16le').decode(HEAPU8.slice(p, p + n)) : '';
  };
  // The box's two buttons: neighbouring fields of one class whose tags are 1 (yes) and 0 (no).
  function dialogButtons(box) {
    const tag = u8(need(resolve().dialogButton, 'dialog button code not found in this build') + 31);
    for (let o = 0x20; o < 0x140; o += 4) {
      const a = ptr(box + o), b = ptr(box + o + 4);
      if (a && b && u32(a) && u32(a) === u32(b) && i32(a + tag) === 1 && i32(b + tag) === 0) return { yes: a, no: b };
    }
    return null;
  }
  function dialogs() {
    const pool = obj('dialogs'), out = [];
    if (!pool) return out;
    const boxes = ptr(pool + 0xc), n = u8(pool + 0x13) + 1;
    for (let i = 0; i < n && i < 64; i++) {
      const box = ptr(boxes + i * 4);
      if (!box || !u8(box + DIALOG.visible) || !u32(box + DIALOG.code)) continue;
      out.push({ index: i, text: delphiString(ptr(box + DIALOG.text)),
        yes: wideString(ptr(box + DIALOG.yes)), no: wideString(ptr(box + DIALOG.no)) });
    }
    return out;
  }

  // The target window a click on an entity opens; the client then shows the entity's bars at the top.
  const selectEntity = (e) => call(resolve().select, need(obj('mateManager'), 'no mate manager'), e, 0x78, [1, 1, 0]);
  const standing = () => { if (u8(player() + MGR.sitting)) throw new Error('the character is sitting: call rest to stand up first'); };

  let scratch = 0;
  const RECORD_BYTES = 32;
  const scratchRecord = () => {
    scratch ||= call(need(resolve().newString, 'string allocator not found in this build'), RECORD_BYTES);
    HEAPU8.fill(0, scratch, scratch + RECORD_BYTES);
    return scratch;
  };

  const actions = {
    // What a double click on the item in the inventory does: wear equipment; use potions, scrolls, wings, boxes, quest items.
    use_item({ vnum, pocket, slot }) {
      const found = inventoryItems().find(({ item, def }) => (vnum !== undefined && vnum !== null
        ? i32(def + 0x30) === vnum : s8(item + 0x1c) === pocket && s16(item + 0x1e) === slot));
      const { item, def } = need(found, vnum !== undefined && vnum !== null ? `no item ${vnum} in the inventory` : `no item in pocket ${pocket} slot ${slot}`);
      // u_i names its user from the player's entity reference; outside the character it would name someone else.
      const mgr = player(), me = ptr(mgr + MGR.entity);
      if (u8(mgr + MGR.kind) !== 1 || !me || i32(mgr + MGR.id) !== i32(me + ENT.id))
        throw new Error(`the client would use the item as entity ${i32(mgr + MGR.id)} of kind ${u8(mgr + MGR.kind)}, not the character: not used`);
      const rec = scratchRecord();
      dv().setUint32(rec, item, true);
      // The record's source: 0 wears an equipment item, 1 (the worn-equipment source) would take it off; other pockets use 1.
      HEAPU8[rec + 4] = s8(item + 0x1c) === 0 ? 0 : 1;
      dv().setInt16(rec + 6, s8(item + 0x1c), true);
      dv().setInt16(rec + 8, s16(item + 0x1e), true);
      call(need(resolve().useItem, 'use item code not found in this build'), need(obj('game'), 'not in the world'), rec);
      return { used: { vnum: i32(def + 0x30), name: str(ptr(def + 0x38)), pocket: s8(item + 0x1c), slot: s16(item + 0x1e) } };
    },
    // Press the box's own Yes or No button.
    answer({ index, text, yes }) {
      const pool = need(obj('dialogs'), 'no dialog pool');
      const shown = dialogs();
      if (!shown.length) throw new Error('no dialog is open');
      const has = (d) => [d.text, d.yes, d.no].some((t) => t.toLowerCase().includes(text.toLowerCase()));
      const d = need(text ? shown.find(has) : index === undefined || index === null ? shown[0] : shown.find((x) => x.index === index),
        `no open dialog ${text ? `contains "${text}"` : `has index ${index}`}; open: ${shown.map((x) => `${x.index} "${x.text}"`).join(', ')}`);
      const box = ptr(ptr(pool + 0xc) + d.index * 4);
      const buttons = need(dialogButtons(box), 'dialog buttons not found');
      call(resolve().dialogButton, box, yes ? buttons.yes : buttons.no);
      return { answered: d, choice: yes ? 'yes' : 'no' };
    },
    walk({ x, y }) {
      const mgr = player();
      call(resolve().preWalk, mgr);
      call(resolve().walk, mgr, ((y & 0xffff) << 16) | (x & 0xffff), 0, [1]);
    },
    select({ id }) { selectEntity(need(entity(id), `no entity ${id} on this map`)); },
    // What a click on a monster, NPC or player does: select it, walk up to it, then attack or talk with the armed skill.
    interact({ id }) {
      const e = need(entity(id), `no entity ${id} on this map`);
      standing();
      if (u8(e + ENT.kind) !== 3) selectEntity(e);
      call(resolve().interact, player(), e);
    },
    skill({ castId, target }) {
      const s = need(skillDef(castId), `skill ${castId} not learned`);
      const ui = need(obj('skillUi'), 'no skill bar');
      const e = target ? need(entity(target), `no entity ${target} on this map`) : 0;
      const def = s.def, ground = u8(def + SKILL.target) === 3, range = u8(def + SKILL.range);
      standing();
      if (!s.ready) throw new Error(`skill ${castId} is on cooldown`);
      if (e && range && u8(def + SKILL.self) !== 2) {
        const me = ptr(player() + MGR.entity), d = cells({ x: u16(me + ENT.x), y: u16(me + ENT.y) }, { x: u16(e + ENT.x), y: u16(e + ENT.y) });
        if (d > range) throw new Error(`target ${target} is ${d} cells away and the skill reaches ${range}: walk closer first`);
      }
      if (e && u8(e + ENT.kind) !== 3) selectEntity(e);
      if (ground && e) {
        const scene = obj('scene');
        dv().setUint16(scene + SCENE.groundX, u16(e + ENT.x), true);
        dv().setUint16(scene + SCENE.groundY, u16(e + ENT.y), true);
      }
      call(resolve().armSkill, ui, def, s.ready ? 1 : 0);
      if (e && range !== 0 && u8(def + SKILL.self) !== 2) call(resolve().interact, player(), e);
    },
    pickup({ id }) {
      call(resolve().pickup, player(), need(findIn(sceneList('items'), id), `no item ${id} on the ground`));
    },
    collect({ id }) {
      call(resolve().collect, player(), need(findIn(sceneList('npcs'), id), `no NPC ${id} on this map`));
    },
    rest() { call(resolve().rest, need(obj('skillUi'), 'no skill bar')); },
  };

  // ---- requests ----
  function serve(req) {
    switch (req.op) {
      case 'state': return state(req);
      case 'grid': return grid();
      case 'portals': return portals();
      case 'inventory': return inventory();
      case 'skills': return skills();
      case 'dialogs': return dialogs();
      case 'api': return Object.fromEntries(Object.entries(resolve()).map(([k, v]) => [k, v >>> 0]));
      case 'read': {
        const n = Math.min(req.length >>> 0, 1 << 20);
        if (!inRange(req.address >>> 0, n)) throw new Error('address out of range');
        return { bytes: Array.from(HEAPU8.subarray(req.address >>> 0, (req.address >>> 0) + n)) };
      }
      case 'scan': return { address: scan(req.signature) };
      case 'call': return { eax: call(req.address >>> 0, req.eax >>> 0, req.edx >>> 0, req.ecx >>> 0, req.stack || []) };
      default:
        if (!Object.hasOwn(actions, req.op)) throw new Error(`unknown op ${req.op}`);
        return actions[req.op](req) ?? { ok: true };
    }
  }

  // Called by the game thread (hle_control.c) on every message-loop turn, so the idle path stays a single load.
  // A request being served is not served again from a message pump inside one of its client calls.
  let serving = false;
  self.controlPoll = () => {
    const seq = Atomics.load(head, 0);
    if (serving || seq === Atomics.load(head, 1)) return;
    let answer;
    serving = true;
    try {
      answer = { result: serve(JSON.parse(utf8.decode(body.slice(0, head[2])))) };
    } catch (e) {
      answer = { error: String(e?.message || e) };
    } finally {
      serving = false;
    }
    let bytes = encoder.encode(JSON.stringify(answer));
    if (bytes.length > body.length) bytes = encoder.encode(JSON.stringify({ error: 'answer too large' }));
    body.set(bytes);
    head[3] = bytes.length;
    Atomics.store(head, 1, seq);
    Atomics.notify(head, 1);
  };
})();
