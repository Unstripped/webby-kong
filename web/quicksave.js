// Quicksave / quickload (F5 / F9, wired in debug.js): window.dk64qs.
// - Snapshot: RDRAM + the port/game .data/.bss (port/src/qs.c), exact, valid only for the identical dk64.wasm
//   (Module.portBuildId = SHA-1 of the wasm main.js instantiated). Kept in IndexedDB, so it survives a reload.
// - Resume point: save slot, kong, map, exit, position. After a reload of any build the game boots straight
//   into it once the EEPROM is read (src/global/450.c), skipping logos, title and file select.
(function () {
  'use strict';

  const DB_NAME = 'dk64dbg';
  const STORE = 'snapshots';
  const KEY = 'quick';
  const RESUME_KEY = 'dk64dbg.resume';
  const AUTO_KEY = 'dk64dbg.autoResume';
  const RESUME_ONCE = 'dk64dbg.resumeOnce';
  const SKIP_SNAPSHOT = 'dk64dbg.skipSnapshot';

  let snap = null; // {buildId, time, map, bytes: Uint8Array}

  function mod() {
    const m = window.Module;
    if (!m || !m.ccall) {
      throw new Error('dk64qs: game not started');
    }
    return m;
  }

  function toast(msg) {
    if (window.dk64dbg) window.dk64dbg.toast(msg);
  }

  function hex(n) {
    return (n >>> 0).toString(16);
  }

  function idbOpen() {
    return new Promise((resolve, reject) => {
      const req = indexedDB.open(DB_NAME, 1);
      req.onupgradeneeded = () => req.result.createObjectStore(STORE);
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
  }

  async function idbGet() {
    const db = await idbOpen();
    try {
      return await new Promise((resolve, reject) => {
        const req = db.transaction(STORE, 'readonly').objectStore(STORE).get(KEY);
        req.onsuccess = () => resolve(req.result || null);
        req.onerror = () => reject(req.error);
      });
    } finally {
      db.close();
    }
  }

  async function idbPut(rec) {
    const db = await idbOpen();
    try {
      await new Promise((resolve, reject) => {
        const tx = db.transaction(STORE, 'readwrite');
        tx.objectStore(STORE).put(rec, KEY);
        tx.oncomplete = () => resolve();
        tx.onerror = () => reject(tx.error);
        tx.onabort = () => reject(tx.error);
      });
    } finally {
      db.close();
    }
  }

  // [[start, length] x3]: RDRAM, .data, .bss (port_qs_table)
  function ranges(m) {
    const t = m.ccall('port_qs_table', 'number') >>> 2;
    return [0, 2, 4].map((k) => [m.HEAPU32[t + k], m.HEAPU32[t + k + 1]]);
  }

  function currentMap(m) {
    return JSON.parse(m.ccall('port_debug_stats', 'string')).map;
  }

  function readResume() {
    try {
      const s = localStorage.getItem(RESUME_KEY);
      return s ? JSON.parse(s) : null;
    } catch (e) {
      return null;
    }
  }

  function autoResume(on) {
    if (on === undefined) {
      try {
        return localStorage.getItem(AUTO_KEY) !== '0';
      } catch (e) {
        return true;
      }
    }
    localStorage.setItem(AUTO_KEY, on ? '1' : '0');
    return !!on;
  }

  async function save() {
    const m = mod();
    const res = JSON.parse(m.ccall('port_resume_capture', 'string'));
    if (!m.ccall('port_qs_save_begin', 'number')) {
      throw new Error('not at a frame boundary');
    }
    let bytes;
    try {
      const r = ranges(m);
      bytes = new Uint8Array(r.reduce((a, [, n]) => a + n, 0));
      let o = 0;
      for (const [s, n] of r) {
        bytes.set(m.HEAPU8.subarray(s, s + n), o);
        o += n;
      }
    } finally {
      m.ccall('port_qs_save_end', null);
    }
    snap = { buildId: m.portBuildId, time: new Date().toISOString(), map: currentMap(m), bytes };
    if (res.ok) {
      localStorage.setItem(RESUME_KEY, JSON.stringify({ ...res, time: snap.time }));
    }
    const noResume = res.ok ? '' : ` · no resume point: ${res.reason}`;
    try {
      await idbPut(snap);
    } catch (err) {
      toast(`Quicksaved (not persisted: ${err && err.message})${noResume}`);
      return;
    }
    toast(`Quicksaved · map 0x${hex(snap.map)}${noResume}`);
  }

  function restore(rec) {
    const m = mod();
    if (rec.bytes.length !== m.ccall('port_qs_size', 'number') >>> 0) {
      throw new Error('snapshot size mismatch');
    }
    const stash = m.ccall('port_qs_load_begin', 'number');
    if (!stash) {
      throw new Error('not at a frame boundary');
    }
    let o = 0;
    for (const [s, n] of ranges(m)) {
      m.HEAPU8.set(rec.bytes.subarray(o, o + n), s);
      o += n;
    }
    if (!m.ccall('port_qs_load_end', 'number', ['number'], [stash])) {
      // memory is already overwritten: the snapshot's threads cannot resume in this page
      sessionStorage.setItem(SKIP_SNAPSHOT, '1');
      sessionStorage.setItem(RESUME_ONCE, '1');
      toast('Quickload failed; reloading');
      setTimeout(() => location.reload(), 800);
      return false;
    }
    return true;
  }

  async function load() {
    const m = mod();
    if (!snap) snap = await idbGet().catch(() => null);
    if (snap && snap.buildId === m.portBuildId) {
      if (restore(snap)) toast(`Quickloaded · map 0x${hex(snap.map)}`);
    } else if (readResume()) {
      sessionStorage.setItem(RESUME_ONCE, '1');
      toast('Snapshot is from another build; reloading to resume');
      setTimeout(() => location.reload(), 600);
    } else {
      toast('Nothing to load');
    }
  }

  // report.js load(): reload into a resume point (or none) with settings URL params for that load only. Skips the
  // same-build snapshot so it cannot override the imported state.
  function bootInto(resume, params) {
    if (resume) {
      localStorage.setItem(RESUME_KEY, JSON.stringify({ ok: true, ...resume, time: new Date().toISOString() }));
    } else {
      localStorage.removeItem(RESUME_KEY);
    }
    sessionStorage.setItem(SKIP_SNAPSHOT, '1');
    sessionStorage.setItem(RESUME_ONCE, '1');
    const u = new URL(location.href);
    u.searchParams.delete('noresume');
    for (const [k, v] of Object.entries(params || {})) u.searchParams.set(k, String(v));
    location.replace(u.href);
  }

  // main.js, right after callMain
  async function started() {
    const once = sessionStorage.getItem(RESUME_ONCE) === '1';
    const skipSnapshot = sessionStorage.getItem(SKIP_SNAPSHOT) === '1';
    sessionStorage.removeItem(RESUME_ONCE);
    sessionStorage.removeItem(SKIP_SNAPSHOT);
    const m = mod();
    const rec = skipSnapshot ? null : await idbGet().catch(() => null);
    if (new URLSearchParams(location.search).has('noresume') || !(once || autoResume())) {
      snap = rec;
      return;
    }
    if (rec && rec.buildId === m.portBuildId) {
      snap = rec;
      if (restore(rec)) toast(`Resumed snapshot · map 0x${hex(rec.map)}`);
      return;
    }
    snap = rec;
    const r = readResume();
    if (r) {
      m.ccall('port_resume_request', null, Array(9).fill('number'),
        [r.slot, r.kong, r.map, r.exit, r.hasPos, r.x, r.y, r.z, r.yaw]);
      toast(`Resuming at map 0x${hex(r.map)} after boot`);
    }
  }

  function status() {
    const id = window.Module && window.Module.portBuildId;
    return {
      snapshot: snap ? { time: snap.time, map: snap.map, sameBuild: snap.buildId === id } : null,
      resume: readResume(),
      auto: autoResume(),
    };
  }

  window.dk64qs = { save, load, bootInto, started, status, autoResume };
})();
