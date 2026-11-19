// EEPROM save backup to the local save server (tools/serve.py): window.dk64saves.
// - boot(FS): before main reads /saves/dk64.eep, uploads the browser copy (kept as a version), then restores the
//   server's newest save (highest client mtime) into IDBFS if it differs. Never rejects.
// - upload(FS): called from port_save_sync (port/src/save.c) after each EEPROM flush.
// Without the server (plain http.server answers 501) the game runs on the IDBFS copy alone and toasts a warning.
(function () {
  'use strict';

  const PATH = '/saves/dk64.eep';
  const SIZE = 2048;
  const URL = 'api/eeprom';
  const TIMEOUT_MS = 5000;

  const state = { server: 'unknown', lastUpload: null };

  function toast(msg) {
    if (window.dk64dbg) window.dk64dbg.toast(msg);
    console.warn(msg);
  }

  function readLocal(FS) {
    try {
      return { bytes: FS.readFile(PATH), mtime: Math.round(+FS.stat(PATH).mtime) };
    } catch (err) {
      return null;
    }
  }

  async function put(bytes, mtime, keepalive) {
    const r = await fetch(URL, {
      method: 'PUT',
      body: bytes,
      headers: { 'X-Save-Mtime': String(mtime) },
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
    try {
      const local = readLocal(FS);
      if (local) await put(local.bytes, local.mtime, false);
      const r = await fetch(URL, { cache: 'no-store', signal: AbortSignal.timeout(TIMEOUT_MS) });
      if (r.status === 404) {
        state.server = 'ok';
        return;
      }
      if (!r.ok) throw new Error('save server HTTP ' + r.status);
      const bytes = new Uint8Array(await r.arrayBuffer());
      const mtime = Number(r.headers.get('X-Save-Mtime'));
      if (bytes.length !== SIZE) throw new Error('save server sent ' + bytes.length + ' bytes');
      if (!local || !sameBytes(local.bytes, bytes)) {
        FS.writeFile(PATH, bytes);
        FS.utime(PATH, mtime, mtime);
        FS.syncfs(false, (err) => err && console.warn('save sync failed', err));
        console.log('PORT: save restored from server (' + r.headers.get('X-Save-Sha1') + ')');
      }
      state.server = 'ok';
    } catch (err) {
      state.server = 'unavailable';
      toast('Save server unavailable (' + err.message + '): progress is only in this browser');
    }
  }

  function upload(FS) {
    const local = readLocal(FS);
    if (!local) return;
    put(local.bytes, local.mtime, true).then(
      () => {
        state.lastUpload = { time: Date.now(), ok: true };
      },
      (err) => {
        state.lastUpload = { time: Date.now(), ok: false };
        state.server = 'unavailable';
        toast('Save upload failed (' + err.message + '): progress is only in this browser');
      },
    );
  }

  function status() {
    return { server: state.server, lastUpload: state.lastUpload && { ...state.lastUpload } };
  }

  window.dk64saves = { boot, upload, status };
})();
