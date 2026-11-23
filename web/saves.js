// Save games on the save server (tools/serve.py, tools/saves.py): window.dk64saves.
// - select(): before the module is created, picks this page's save game: ?save=<id>, else the most recently played
//   one (or a new one), written back into the URL. The EEPROM lives at path() (/saves/s<id>.eep in IDBFS), passed to
//   the game as --save-file; scope() keys quicksave.js's per-save storage.
// - boot(FS): before main reads path(), uploads the browser copy (kept as a version), then restores the server's
//   newest version (highest client mtime) into IDBFS if it differs. Never rejects.
// - upload(FS, summary): called from port_save_sync (port/src/save.c) after each EEPROM flush, with the game's
//   summary JSON (or '') for the dashboard.
// - settle() / putResume() / takeResume(): menu.js "Exit to dashboard" and quicksave.js's resume point.
// Without the server (plain http.server answers 501/404) the game runs on /saves/dk64.eep in IDBFS alone and toasts a
// warning; there is no dashboard then.
(function () {
  'use strict';

  const SIZE = 2048;
  const API = 'api/saves';
  const TIMEOUT_MS = 5000;
  const JSON_HEADERS = { 'Content-Type': 'application/json' };

  const state = { server: 'unknown', lastUpload: null };
  let current = null; // the server's save object (tools/saves.py save_json) or null: offline
  let dashboard = false;
  const inflight = new Set();

  class Fatal extends Error {}

  function toast(msg) {
    if (window.dk64dbg) window.dk64dbg.toast(msg);
    console.warn(msg);
  }

  function path() {
    return current ? '/saves/s' + current.id + '.eep' : '/saves/dk64.eep';
  }

  function scope() {
    return current ? '.' + current.id : '';
  }

  function saveUrl(suffix) {
    return API + '/' + current.id + (suffix || '');
  }

  async function call(url, init) {
    const r = await fetch(url, { cache: 'no-store', signal: AbortSignal.timeout(TIMEOUT_MS), ...init });
    if (r.status === 401) {
      location.assign('/auth/?next=' + encodeURIComponent(location.pathname + location.search));
      throw new Fatal('Signing in…');
    }
    return r;
  }

  async function json(r) {
    let data = null;
    try {
      data = await r.json();
    } catch (e) {
      data = null;
    }
    if (!r.ok || data === null || typeof data !== 'object') throw new Error('save server HTTP ' + r.status);
    return data;
  }

  async function select() {
    const id = new URLSearchParams(location.search).get('save');
    try {
      let save;
      if (id !== null) {
        const r = await call(API + '/' + encodeURIComponent(id));
        if (r.status === 404) {
          const body = await r.json().catch(() => null);
          if (body && body.error === 'not_found') {
            throw new Fatal("This save doesn't exist or isn't yours. Pick one on the dashboard.");
          }
        }
        const data = await json(r);
        save = data.save;
        dashboard = !!data.dashboard;
      } else {
        const data = await json(await call(API));
        dashboard = !!data.dashboard;
        save = data.saves[0];
        if (!save) {
          save = (await json(await call(API, { method: 'POST', headers: JSON_HEADERS, body: '{}' }))).save;
        }
        const u = new URL(location.href);
        u.searchParams.set('save', String(save.id));
        history.replaceState(null, '', u.href);
      }
      current = save;
      state.server = 'ok';
      return current;
    } catch (err) {
      if (err instanceof Fatal) throw err;
      current = null;
      dashboard = false;
      state.server = 'unavailable';
      toast('Save server unavailable (' + err.message + '): progress is only in this browser');
      return null;
    }
  }

  function readLocal(FS) {
    try {
      return { bytes: FS.readFile(path()), mtime: Math.round(+FS.stat(path()).mtime) };
    } catch (err) {
      return null;
    }
  }

  async function put(bytes, mtime, keepalive, summary) {
    const headers = { 'X-Save-Mtime': String(mtime) };
    if (summary) headers['X-Save-Summary'] = summary;
    const r = await fetch(saveUrl('/eeprom'), {
      method: 'PUT',
      body: bytes,
      headers,
      keepalive,
      signal: AbortSignal.timeout(TIMEOUT_MS),
    });
    if (!r.ok) throw new Error('save server HTTP ' + r.status);
  }

  function sameBytes(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) {
      if (a[i] !== b[i]) return false;
    }
    return true;
  }

  async function boot(FS) {
    if (!current) return;
    try {
      const local = readLocal(FS);
      if (local) await put(local.bytes, local.mtime, false);
      const r = await fetch(saveUrl('/eeprom'), { cache: 'no-store', signal: AbortSignal.timeout(TIMEOUT_MS) });
      if (r.status === 404) {
        state.server = 'ok';
        return;
      }
      if (!r.ok) throw new Error('save server HTTP ' + r.status);
      const bytes = new Uint8Array(await r.arrayBuffer());
      const mtime = Number(r.headers.get('X-Save-Mtime'));
      if (bytes.length !== SIZE) throw new Error('save server sent ' + bytes.length + ' bytes');
      if (!local || !sameBytes(local.bytes, bytes)) {
        FS.writeFile(path(), bytes);
        FS.utime(path(), mtime, mtime);
        FS.syncfs(false, (err) => err && console.warn('save sync failed', err));
        console.log('PORT: save restored from server (' + r.headers.get('X-Save-Sha1') + ')');
      }
      state.server = 'ok';
    } catch (err) {
      state.server = 'unavailable';
      toast('Save server unavailable (' + err.message + '): progress is only in this browser');
    }
  }

  function upload(FS, summary) {
    if (!current) return Promise.resolve();
    const local = readLocal(FS);
    if (!local) return Promise.resolve();
    const p = put(local.bytes, local.mtime, true, summary).then(
      () => {
        state.lastUpload = { time: Date.now(), ok: true };
      },
      (err) => {
        state.lastUpload = { time: Date.now(), ok: false };
        state.server = 'unavailable';
        toast('Save upload failed (' + err.message + '): progress is only in this browser');
      },
    );
    inflight.add(p);
    p.finally(() => inflight.delete(p));
    return p;
  }

  // true when the server holds this browser's latest save: every upload settled and the last one succeeded
  async function settle() {
    await Promise.allSettled([...inflight]);
    if (current === null) return false;
    return state.lastUpload !== null ? state.lastUpload.ok : state.server === 'ok';
  }

  async function putResume(resume) {
    if (!current) return;
    const r = await fetch(saveUrl('/resume'), {
      method: 'PUT',
      headers: JSON_HEADERS,
      body: JSON.stringify({ resume }),
      signal: AbortSignal.timeout(TIMEOUT_MS),
    });
    if (!r.ok) throw new Error('save server HTTP ' + r.status);
  }

  // the server's resume point from "Exit to dashboard", once: it is cleared on the server as it is used
  function takeResume() {
    const resume = current && current.resume;
    if (!resume) return null;
    current.resume = null;
    putResume(null).catch(() => {});
    return resume;
  }

  function status() {
    return { server: state.server, save: current && current.id, lastUpload: state.lastUpload && { ...state.lastUpload } };
  }

  window.dk64saves = {
    select,
    boot,
    upload,
    settle,
    putResume,
    takeResume,
    path,
    scope,
    current: () => current,
    dashboard: () => !!(current && dashboard),
    status,
  };
})();
