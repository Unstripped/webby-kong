// Online co-op over the tools/serve.py relay (GET /api/net): window.dk64net.
// Every browser runs the same simulation in input lockstep (port/src/net.c): the game consumes one bundle of every
// player's pad per controller read and stalls until it has it; the local pad is sampled `delay` reads ahead. The
// host owns the session: it collects inputs (its own through onLocalInput, joiners' as INPUT), emits bundles in read
// order, ships the quicksave snapshot to joiners (join, and resync after a state-hash mismatch) and decides the
// roster (which slot plays which kong).
// - Host: Esc menu "Host co-op", or ?net=host. ?netdelay=2..10 sets the input delay in reads (default 3).
// - Join: ?net=join on the host's server (tools/serve.py also serves the host's ROM at /api/rom).
// Wire format, every message binary [u8 peer][u8 type][payload], little-endian:
//   0 CTRL    UTF-8 JSON: welcome {slot, buildId, delay, drawdist}, snapshot {read, size}, ready, reject {reason},
//             freeze {reason}, thaw, resync; from the relay (peer 0): peer-join / peer-leave {id}, end {reason}
//   1 INPUT   joiner -> host: u32 read, u32 pad
//   2 BUNDLE  host -> all: u32 read, u8 mask, u8 joinSlot, u8 joinKong, 5 x u32 pad (0xFF = no join)
//   3 HASH    joiner -> host: u32 read, u32 hash
//   4 SNAP    host -> one: u32 offset, u32 total, gzip bytes (512 KiB chunks)
// Pads are packed (button << 16) | ((u8)stick_x << 8) | (u8)stick_y. The relay sets byte 0 of joiner messages to
// the sender's peer id; byte 0 of host messages picks the joiner (0 = every joiner).
(function () {
  'use strict';

  const T_CTRL = 0;
  const T_INPUT = 1;
  const T_BUNDLE = 2;
  const T_HASH = 3;
  const T_SNAP = 4;
  const SLOTS = 5;
  const NONE = 0xff;
  const BUNDLE_LEN = 29;
  const SNAP_CHUNK = 512 * 1024;
  const HISTORY = 512; // bundles kept for joiners that restore an older read
  const HASH_EVERY = 30; // port/src/net.c reports a state hash every 30 reads
  const HASH_KEEP = 256;
  const INPUT_TIMEOUT_MS = 10000;
  const READY_TIMEOUT_MS = 60000;
  const STALL_MS = 500;
  const KONGS = ['DK', 'Diddy', 'Lanky', 'Tiny', 'Chunky'];
  const CLOSE_REASONS = {
    1009: 'message too big',
    4001: 'another host is already connected',
    4002: 'no host is running a session',
    4003: 'the session is full',
  };

  const params = new URLSearchParams(location.search);
  let role = params.get('net') === 'join' ? 'join' : null; // 'host' while hosting; a joiner page stays 'join'
  let isStarted = false;
  let ws = null;
  let begun = false; // between port_net_begin and port_net_end
  let delay = 3;
  let localSlot = 0;
  let muted = false;
  let frozen = null; // reason while a join or resync holds the session
  let ended = null; // why a joiner's session ended

  function mod() {
    return window.Module;
  }

  function toast(msg) {
    if (window.dk64dbg) window.dk64dbg.toast(msg);
    console.log('co-op: ' + msg);
  }

  function changed() {
    updateStatus();
    window.dispatchEvent(new Event('dk64net'));
  }

  // ---- wire

  function socketUrl(r) {
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    return `${proto}://${location.host}${location.pathname.replace(/[^/]*$/, '')}api/net?role=${r}`;
  }

  function openSocket(r) {
    const s = new WebSocket(socketUrl(r));
    s.binaryType = 'arraybuffer';
    return s;
  }

  function closeReason(ev) {
    return CLOSE_REASONS[ev.code] || ev.reason || 'connection to the server lost';
  }

  function view(b) {
    return new DataView(b.buffer, b.byteOffset, b.byteLength);
  }

  function message(peer, type, size) {
    const b = new Uint8Array(2 + size);
    b[0] = peer;
    b[1] = type;
    return b;
  }

  function send(b) {
    if (ws && ws.readyState === WebSocket.OPEN) ws.send(b);
  }

  function sendCtrl(peer, obj) {
    const json = new TextEncoder().encode(JSON.stringify(obj));
    const b = message(peer, T_CTRL, json.length);
    b.set(json, 2);
    send(b);
  }

  function sendPair(type, a, c) {
    const b = message(0, type, 8);
    const v = view(b);
    v.setUint32(2, a, true);
    v.setUint32(6, c, true);
    send(b);
  }

  function bundle(read, mask, joinSlot, joinKong, pads) {
    const b = message(0, T_BUNDLE, BUNDLE_LEN - 2);
    const v = view(b);
    v.setUint32(2, read, true);
    b[6] = mask;
    b[7] = joinSlot;
    b[8] = joinKong;
    for (let i = 0; i < SLOTS; i++) v.setUint32(9 + 4 * i, pads[i], true);
    return b;
  }

  function pushBundle(b) {
    const v = view(b);
    mod()._port_net_push(v.getUint32(2, true), b[6], b[7], b[8], v.getUint32(9, true), v.getUint32(13, true),
      v.getUint32(17, true), v.getUint32(21, true), v.getUint32(25, true));
  }

  function parseCtrl(b) {
    try {
      return JSON.parse(new TextDecoder().decode(b.subarray(2)));
    } catch (err) {
      return null;
    }
  }

  async function pipe(bytes, stream) {
    return new Uint8Array(await new Response(new Blob([bytes]).stream().pipeThrough(stream)).arrayBuffer());
  }

  function begin(r) {
    const m = mod();
    m._port_net_begin(r, localSlot, delay);
    m._port_net_set_muted(muted ? 1 : 0); // port_net_begin clears it
    begun = true;
  }

  function end() {
    if (!begun) return;
    begun = false;
    mod()._port_net_end();
  }

  function netRead() {
    return mod()._port_net_read() >>> 0;
  }

  // ---- host

  let E = 0; // next read to emit
  let curMask = 1; // mask of the last emitted bundle
  let slots = []; // slot -> null or {peer, firstInput, hashFloor}; slot 0 is the host
  let peerSlot = new Map(); // joiner peer id -> slot
  let pending = []; // roster changes, one per bundle: {slot, kong} adds a slot, {slot} removes it
  let unconsumed = []; // emitted joins this sim has not read yet: {read, kong}
  let inputs = new Map(); // read -> per-slot packed pads
  let history = new Map(); // read -> bundle message
  let ownHashes = new Map(); // read -> this sim's hash
  let peerHashes = new Map(); // read -> [{slot, hash}] that arrived before this sim got there
  let jobs = []; // queued joins / resyncs: {kind: 'join' | 'resync', peer, tries}
  let job = null; // the join or resync in progress
  let blocked = null; // {slot, since}: emission waits for this slot's input
  let emitQueued = false;
  let hostTimer = 0;

  function resetHost() {
    E = 0;
    curMask = 1;
    slots = [];
    peerSlot = new Map();
    pending = [];
    unconsumed = [];
    inputs = new Map();
    history = new Map();
    ownHashes = new Map();
    peerHashes = new Map();
    jobs = [];
    job = null;
    blocked = null;
  }

  function host() {
    if (!isStarted || role) return;
    const d = Number(params.get('netdelay'));
    delay = Number.isInteger(d) && d >= 2 && d <= 10 ? d : 3;
    localSlot = 0;
    role = 'host';
    resetHost();
    ws = openSocket('host');
    ws.onopen = () => {
      begin(1);
      E = netRead();
      curMask = 1;
      // reads E .. E+delay-1 have no host input yet: they go out with zero pads
      slots = [{ peer: 0, firstInput: E + delay, hashFloor: E }];
      hostTimer = setInterval(hostTick, 1000);
      scheduleEmit();
      changed();
    };
    ws.onmessage = (ev) => onHostMessage(new Uint8Array(ev.data));
    ws.onclose = (ev) => stopHosting(closeReason(ev));
    changed();
  }

  // the Esc menu's "Stop hosting", or the socket closed; ending a session with joiners reloads to one player
  function stopHosting(reason) {
    if (role !== 'host') return;
    const s = ws;
    ws = null;
    if (s) {
      s.onclose = null;
      s.close();
    }
    end();
    clearInterval(hostTimer);
    if (job) clearTimeout(job.timer);
    resetHost();
    role = null;
    frozen = null;
    if (reason) toast(`Co-op ended: ${reason}`);
    changed();
  }

  function onHostMessage(b) {
    if (b.length < 2) return;
    const peer = b[0];
    const type = b[1];
    if (type === T_CTRL) {
      const msg = parseCtrl(b);
      if (!msg) return;
      if (peer === 0) {
        if (msg.t === 'peer-join') {
          jobs.push({ kind: 'join', peer: msg.id, tries: 0 });
          startJob();
        } else if (msg.t === 'peer-leave') {
          peerLeft(msg.id);
        }
      } else if (msg.t === 'ready' && job && job.peer === peer && job.sent) {
        finishJob();
      }
      return;
    }
    const slot = peerSlot.get(peer);
    if (peer === 0 || slot === undefined || b.length !== 10) return;
    const v = view(b);
    if (type === T_INPUT) {
      setInput(v.getUint32(2, true), slot, v.getUint32(6, true));
      scheduleEmit();
    } else if (type === T_HASH) {
      peerHash(slot, v.getUint32(2, true), v.getUint32(6, true));
    }
  }

  function setInput(read, slot, pad) {
    if (read < E || read >= E + 1024) return;
    let row = inputs.get(read);
    if (!row) {
      row = new Array(SLOTS);
      inputs.set(read, row);
    }
    row[slot] = pad;
  }

  // wasm calls back into JS during the sim step: emitting (port_net_push) waits for the step to finish
  function scheduleEmit() {
    if (emitQueued) return;
    emitQueued = true;
    queueMicrotask(emit);
  }

  function emit() {
    emitQueued = false;
    if (role !== 'host' || !begun || frozen) return;
    for (;;) {
      const change = pending[0];
      let mask = curMask;
      if (change) mask = change.kong === undefined ? mask & ~(1 << change.slot) : mask | (1 << change.slot);
      const row = inputs.get(E);
      const pads = [0, 0, 0, 0, 0];
      let wait = -1;
      for (let s = 0; s < SLOTS && wait < 0; s++) {
        const sl = slots[s];
        if (!sl || !(mask & (1 << s))) continue; // a slot that left pads 0 until its removal goes out
        if (row && row[s] !== undefined) pads[s] = row[s];
        else if (E >= sl.firstInput) wait = s;
      }
      if (wait >= 0) {
        if (!blocked || blocked.slot !== wait) blocked = { slot: wait, since: performance.now() };
        return;
      }
      blocked = null;
      const join = !!change && change.kong !== undefined;
      const b = bundle(E, mask, join ? change.slot : NONE, join ? change.kong : NONE, pads);
      if (change) {
        pending.shift();
        if (join) unconsumed.push({ read: E, kong: change.kong });
      }
      curMask = mask;
      pushBundle(b);
      send(b);
      history.set(E, b);
      history.delete(E - HISTORY);
      inputs.delete(E);
      E++;
    }
  }

  function hostTick() {
    if (frozen || !blocked || blocked.slot === 0) return;
    if (performance.now() - blocked.since > INPUT_TIMEOUT_MS) {
      const s = blocked.slot;
      blocked = null;
      dropSlot(s, 'No input for 10 s');
    }
  }

  function dropSlot(slot, reason) {
    const sl = slots[slot];
    if (!sl || slot === 0) return;
    slots[slot] = null;
    peerSlot.delete(sl.peer);
    pending.push({ slot });
    jobs = jobs.filter((j) => j.peer !== sl.peer);
    for (const [read, list] of peerHashes) peerHashes.set(read, list.filter((p) => p.slot !== slot));
    if (reason) sendCtrl(sl.peer, { t: 'reject', reason });
    scheduleEmit();
    changed();
  }

  function peerLeft(id) {
    jobs = jobs.filter((j) => j.peer !== id);
    if (job && job.peer === id) cancelJob(null);
    const slot = peerSlot.get(id);
    if (slot !== undefined) dropSlot(slot, null);
  }

  function freeSlot() {
    for (let s = 1; s < SLOTS; s++) {
      if (!slots[s] && !(curMask & (1 << s)) && !pending.some((c) => c.slot === s)) return s;
    }
    return -1;
  }

  // unlocked kongs nobody plays, minus joins this sim has not applied yet
  function freeKongs() {
    let k = mod()._port_coop_free_kongs() & 0x1f;
    const r = netRead();
    unconsumed = unconsumed.filter((j) => j.read >= r);
    for (const j of unconsumed) k &= ~(1 << j.kong);
    for (const c of pending) if (c.kong !== undefined) k &= ~(1 << c.kong);
    return k;
  }

  function startJob() {
    if (job || frozen || !begun || !jobs.length) return;
    const next = jobs.shift();
    let slot;
    if (next.kind === 'join') {
      slot = freeSlot();
      const reason = !window.dk64qs ? 'Host cannot send the game'
        : slot < 0 ? 'Session full' : !freeKongs() ? 'No free kong' : null;
      if (reason) {
        sendCtrl(next.peer, { t: 'reject', reason });
        startJob();
        return;
      }
    } else {
      slot = peerSlot.get(next.peer);
      if (slot === undefined) {
        startJob();
        return;
      }
    }
    let bytes;
    try {
      bytes = window.dk64qs.captureBytes();
    } catch (err) {
      // not at a frame boundary: try again shortly
      if (++next.tries < 20) {
        jobs.unshift(next);
        setTimeout(startJob, 100);
      } else if (next.kind === 'join') {
        sendCtrl(next.peer, { t: 'reject', reason: 'Host could not capture the game' });
      } else {
        dropSlot(slot, 'Resync failed');
      }
      return;
    }
    const j = { ...next, slot, rs: netRead(), sent: false, timer: 0 };
    j.timer = setTimeout(() => {
      if (job === j) cancelJob('Timed out');
    }, READY_TIMEOUT_MS);
    job = j;
    frozen = next.kind === 'join' ? 'Player joining' : `Resyncing P${slot + 1}`;
    sendCtrl(0, { t: 'freeze', reason: frozen });
    if (next.kind === 'join') {
      sendCtrl(j.peer, {
        t: 'welcome', slot, buildId: mod().portBuildId, delay, drawdist: mod()._port_get_drawdist(),
      });
    } else {
      sendCtrl(j.peer, { t: 'resync' });
    }
    changed();
    shipSnapshot(j, bytes);
  }

  async function shipSnapshot(j, bytes) {
    let gz;
    try {
      gz = await pipe(bytes, new CompressionStream('gzip'));
    } catch (err) {
      if (job === j) cancelJob('Host could not compress the game');
      return;
    }
    if (job !== j) return;
    sendCtrl(j.peer, { t: 'snapshot', read: j.rs, size: bytes.length });
    for (let o = 0; o < gz.length; o += SNAP_CHUNK) {
      const part = gz.subarray(o, o + SNAP_CHUNK);
      const b = message(j.peer, T_SNAP, 8 + part.length);
      const v = view(b);
      v.setUint32(2, o, true);
      v.setUint32(6, gz.length, true);
      b.set(part, 10);
      send(b);
    }
    // emitted after the snapshot's read: the joiner's sim needs them before the session thaws
    for (let r = j.rs; r < E; r++) {
      const h = history.get(r);
      if (!h) continue;
      const c = h.slice();
      c[0] = j.peer;
      send(c);
    }
    j.sent = true;
  }

  function finishJob() {
    const j = job;
    clearTimeout(j.timer);
    job = null;
    if (j.kind === 'join') {
      const k = freeKongs();
      if (k) {
        // the joiner's first input is for the read its restored sim consumes first, plus the delay
        slots[j.slot] = { peer: j.peer, firstInput: j.rs + delay, hashFloor: j.rs };
        peerSlot.set(j.peer, j.slot);
        pending.push({ slot: j.slot, kong: 31 - Math.clz32(k & -k) });
      } else {
        sendCtrl(j.peer, { t: 'reject', reason: 'No free kong' });
      }
    } else if (slots[j.slot]) {
      slots[j.slot].firstInput = j.rs + delay;
      slots[j.slot].hashFloor = j.rs;
      for (const [read, list] of peerHashes) peerHashes.set(read, list.filter((p) => p.slot !== j.slot));
    }
    thaw();
  }

  function cancelJob(reason) {
    const j = job;
    if (!j) return;
    clearTimeout(j.timer);
    job = null;
    if (reason) {
      if (j.kind === 'join') sendCtrl(j.peer, { t: 'reject', reason });
      else dropSlot(j.slot, reason);
    }
    thaw();
  }

  function thaw() {
    frozen = null;
    blocked = null; // the input timeout counts from now, not from before the freeze
    sendCtrl(0, { t: 'thaw' });
    scheduleEmit();
    changed();
    setTimeout(startJob, 0); // let the session move before the next queued join freezes it again
  }

  function ownHash(read, hash) {
    ownHashes.set(read, hash);
    ownHashes.delete(read - HASH_EVERY * HASH_KEEP);
    const list = peerHashes.get(read);
    if (list) {
      peerHashes.delete(read);
      for (const p of list) compareHash(p.slot, read, p.hash);
    }
  }

  function peerHash(slot, read, hash) {
    if (read < slots[slot].hashFloor) return;
    if (ownHashes.has(read)) {
      compareHash(slot, read, hash);
      return;
    }
    // the joiner got there first
    if (!peerHashes.has(read)) peerHashes.set(read, []);
    peerHashes.get(read).push({ slot, hash });
    if (peerHashes.size > HASH_KEEP) peerHashes.delete(peerHashes.keys().next().value);
  }

  function compareHash(slot, read, hash) {
    const sl = slots[slot];
    if (!sl || read < sl.hashFloor || ownHashes.get(read) === hash) return;
    console.warn('co-op desync at read', read, 'slot', slot);
    if ((job && job.peer === sl.peer) || jobs.some((j) => j.peer === sl.peer)) return;
    jobs.push({ kind: 'resync', peer: sl.peer, tries: 0 });
    startJob();
  }

  // ---- joiner

  let jstate = 'idle'; // connecting, welcome (waiting for the snapshot), loading, running, resync, ended
  let snap = null; // {read, size, gz, got} of the snapshot being received
  let queue = []; // bundles received before the snapshot is in place

  function join() {
    jstate = 'connecting';
    ws = openSocket('join');
    ws.onmessage = (ev) => onJoinMessage(new Uint8Array(ev.data));
    ws.onclose = (ev) => endJoin(closeReason(ev));
    changed();
  }

  function endJoin(reason) {
    if (ended) return;
    ended = reason;
    jstate = 'ended';
    frozen = null;
    snap = null;
    queue = [];
    const s = ws;
    ws = null;
    if (s) {
      s.onclose = null;
      s.close();
    }
    end();
    mod()._port_set_paused(1); // after port_net_end: the game holds on the host's state
    changed();
  }

  function onJoinMessage(b) {
    if (b.length < 2 || ended) return;
    if (b[1] === T_CTRL) {
      const msg = parseCtrl(b);
      if (msg) onJoinCtrl(msg);
    } else if (b[1] === T_BUNDLE && b.length === BUNDLE_LEN) {
      if (jstate === 'running') pushBundle(b);
      else queue.push(b);
    } else if (b[1] === T_SNAP && b.length > 10 && snap) {
      const v = view(b);
      const off = v.getUint32(2, true);
      const total = v.getUint32(6, true);
      if (!snap.gz) snap.gz = new Uint8Array(total);
      snap.gz.set(b.subarray(10), off);
      snap.got += b.length - 10;
      if (snap.got >= total) applySnapshot(snap);
      else updateStatus();
    }
  }

  function onJoinCtrl(msg) {
    switch (msg.t) {
      case 'welcome':
        if (msg.buildId !== mod().portBuildId) {
          endJoin('Host runs a different build; reload');
          return;
        }
        localSlot = msg.slot;
        delay = msg.delay;
        mod()._port_set_drawdist(msg.drawdist); // the session's value, not stored in this browser's settings
        jstate = 'welcome';
        changed();
        break;
      case 'resync':
        jstate = 'resync';
        snap = null;
        queue = [];
        changed();
        break;
      case 'snapshot':
        if (jstate !== 'welcome' && jstate !== 'resync') return;
        snap = { read: msg.read >>> 0, size: msg.size, gz: null, got: 0 };
        queue = []; // the host resends every bundle from the snapshot's read on
        break;
      case 'freeze':
        frozen = msg.reason || 'Paused';
        changed();
        break;
      case 'thaw':
        frozen = null;
        changed();
        break;
      case 'reject':
      case 'end':
        endJoin(msg.reason || 'Session ended');
        break;
    }
  }

  async function applySnapshot(s) {
    jstate = 'loading';
    changed();
    let bytes;
    try {
      bytes = await pipe(s.gz, new DecompressionStream('gzip'));
    } catch (err) {
      endJoin('The game snapshot was damaged');
      return;
    }
    if (snap !== s || ended) return;
    snap = null;
    if (bytes.length !== s.size || bytes.length !== mod()._port_qs_size() >>> 0) {
      endJoin('The game snapshot does not fit this build');
      return;
    }
    begin(2);
    for (let tries = 0; ; tries++) {
      try {
        // false: the load failed after memory was overwritten and the page reloads (and joins again)
        if (!window.dk64qs.restoreBytes(bytes)) return;
        break;
      } catch (err) {
        if (tries >= 20 || !/frame boundary/.test(err.message)) {
          endJoin('Could not load the game: ' + err.message);
          return;
        }
        await new Promise((r) => setTimeout(r, 50));
        if (ended) return;
      }
    }
    for (const q of queue) pushBundle(q);
    queue = [];
    jstate = 'running';
    sendCtrl(0, { t: 'ready' });
    changed();
  }

  // ---- callbacks from port/src/net.c (during the sim step: no wasm calls here)

  function onLocalInput(read, packed) {
    if (role === 'host' && begun) {
      setInput(read, 0, packed);
      scheduleEmit();
    } else if (role === 'join' && begun) {
      sendPair(T_INPUT, read, packed);
    }
  }

  function onHash(read, hash) {
    if (role === 'host' && begun) queueMicrotask(() => ownHash(read, hash));
    else if (role === 'join' && jstate === 'running') sendPair(T_HASH, read, hash);
  }

  // ---- status line

  const statusEl = document.createElement('div');
  statusEl.id = 'netstatus';
  statusEl.style.cssText = 'position:fixed;top:8px;right:8px;z-index:5;display:none;padding:4px 9px;border-radius:6px;'
    + 'background:rgba(0,0,0,.6);color:#fff3d6;font:600 12px system-ui,sans-serif;pointer-events:none;';
  document.body.appendChild(statusEl);
  let lastRead = -1;
  let lastMove = 0;

  function playerCount(m) {
    let n = 0;
    for (let s = 0; s < SLOTS; s++) if (m._port_coop_slot_kong(s) !== NONE) n++;
    return n;
  }

  function statusText() {
    if (ended) return `Session ended: ${ended}`;
    if (!role || !isStarted) return null;
    if (!begun) {
      if (role === 'host' || jstate === 'connecting') return 'Co-op · connecting…';
      if (snap && snap.gz) return `Co-op · receiving game ${Math.floor((snap.got / snap.gz.length) * 100)}%`;
      return jstate === 'loading' ? 'Co-op · loading game…' : 'Co-op · waiting for the host…';
    }
    if (frozen) return `Co-op · ${frozen}…`;
    if (jstate === 'resync' || jstate === 'loading') return 'Co-op · resyncing…';
    const m = mod();
    const r = netRead();
    const now = performance.now();
    if (r !== lastRead) {
      lastRead = r;
      lastMove = now;
    }
    if (now - lastMove > STALL_MS) {
      return role === 'host' && blocked && blocked.slot > 0 ? `Waiting for P${blocked.slot + 1}…` : 'Waiting for network…';
    }
    const n = playerCount(m);
    const kong = KONGS[m._port_coop_slot_kong(localSlot)] || '—';
    return `Co-op · You: ${kong} (P${m._port_net_local_view() + 1}) · ${n} player${n === 1 ? '' : 's'} · delay ${delay}`;
  }

  function updateStatus() {
    const text = statusText();
    statusEl.style.display = text ? '' : 'none';
    if (text && statusEl.textContent !== text) statusEl.textContent = text;
  }
  setInterval(updateStatus, 250);

  // ---- API

  // main.js, after callMain (and after quicksave.js resumed a snapshot)
  function started() {
    if (isStarted) return;
    isStarted = true;
    if (role === 'join') join();
    else if (params.get('net') === 'host') host();
  }

  function active() {
    return role !== null && !ended;
  }

  function setMuted(on) {
    muted = !!on;
    if (begun) mod()._port_net_set_muted(muted ? 1 : 0);
  }

  function leave() {
    const u = new URL(location.href);
    u.searchParams.delete('net');
    location.href = u.toString();
  }

  function status() {
    const roster = [];
    if (begun) {
      const m = mod();
      for (let s = 0; s < SLOTS; s++) {
        const k = m._port_coop_slot_kong(s);
        if (k !== NONE) roster.push({ slot: s, kong: KONGS[k] || '?', you: s === localSlot });
      }
    }
    return {
      role,
      active: active(),
      connected: begun,
      slot: localSlot,
      delay,
      frozen,
      ended,
      joinUrl: `${location.origin}${location.pathname}?net=join`,
      roster,
    };
  }

  window.dk64net = {
    started,
    host,
    stop: () => stopHosting(null),
    leave,
    active,
    role: () => role,
    setMuted,
    onLocalInput,
    onHash,
    status,
  };
})();
