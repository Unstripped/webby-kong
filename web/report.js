// Bug reports and ideas as prefilled GitHub issue forms (.github/ISSUE_TEMPLATE): window.dk64report.
// - collect(): ROM-free game state (EEPROM save, resume point, level progress, version, settings, browser, recent
//   trace and console lines) as a markdown table plus one encoded blob, DK64R1.<base64url(deflate-raw(JSON))>.
//   The RDRAM quicksave is never included: it is ROM-derived memory and issues are public.
// - withMark(): adds the spot the player marked on the screenshot and the renderer's pixel probe there.
// - issueUrl(): the issue form URL, fitted under MAX_URL by dropping trace/log, then probe detail, then text.
// - Crash detection: wasm traps and abort() open the Esc menu's crash view (menu.js openCrash).
// - load(): developer import (F2 → Load report…): writes the reporter's save and boots into their location.
// tools/report.py decodes the same blob outside the browser.
(function () {
  'use strict';

  const REPO = 'https://github.com/Unstripped/webby-kong';
  const MAX_URL = 7500;
  const PREFIX = 'DK64R1.';
  const SAVE_PATH = '/saves/dk64.eep';
  const SETTING_KEYS = ['drawdist', 'scale', 'msaa', 'interp', 'audio3d', 'audiorate'];
  const PROBE_CAPS = [6000, 3000, 1500, 0]; // probe chars per blob variant, largest first
  const LOG_MAX = 60;
  const HIT_RE = /^#\d+ dl=/m;

  let versionInfo; // undefined until fetched, then {version, revision} or null (source build)
  const versionPromise = fetch('version.json', { cache: 'no-store' })
    .then((r) => (r.ok ? r.json() : null))
    .catch(() => null)
    .then((v) => {
      versionInfo = v && typeof v === 'object' ? v : null;
      return versionInfo;
    });

  let gpu = null;
  function gpuName() {
    if (gpu !== null) return gpu;
    gpu = 'unknown';
    try {
      const gl = document.createElement('canvas').getContext('webgl2');
      if (gl) {
        let name = gl.getParameter(gl.RENDERER);
        const dbg = gl.getExtension('WEBGL_debug_renderer_info');
        if (String(name).includes('WebKit WebGL') && dbg) name = gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL);
        gpu = String(name || 'unknown');
        const lose = gl.getExtension('WEBGL_lose_context');
        if (lose) lose.loseContext();
      }
    } catch (err) {
      gpu = 'unknown';
    }
    return gpu;
  }

  // ---- console ring
  const ring = []; // {text, n}
  function log(text) {
    const t = String(text);
    const last = ring[ring.length - 1];
    if (last && last.text === t) {
      last.n++;
      return;
    }
    ring.push({ text: t, n: 1 });
    if (ring.length > LOG_MAX) ring.shift();
  }
  function ringLines(n) {
    return ring.slice(-n).map((e) => (e.n > 1 ? `${e.text} (×${e.n})` : e.text));
  }

  // ---- crash detection
  let crashMsg = null;
  function crashed(msg) {
    if (!window.Module || crashMsg !== null) return;
    crashMsg = String(msg);
    log('crash: ' + crashMsg);
    setTimeout(() => window.dk64menu && window.dk64menu.openCrash(crashMsg), 0);
  }
  function isCrashed() {
    return crashMsg !== null;
  }
  window.addEventListener('error', (e) => {
    log('error: ' + e.message);
    if (e.error instanceof WebAssembly.RuntimeError) crashed(e.error.message);
  });
  window.addEventListener('unhandledrejection', (e) => {
    const r = e.reason;
    log('error: ' + (r && r.message ? r.message : String(r)));
    if (r instanceof WebAssembly.RuntimeError) crashed(r.message);
  });

  // ---- encoding
  function b64url(bytes) {
    let s = '';
    for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  }
  function unb64url(text) {
    const s = atob(text.replace(/-/g, '+').replace(/_/g, '/') + '='.repeat((4 - (text.length % 4)) % 4));
    const out = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
    return out;
  }
  async function encode(obj) {
    const bytes = new TextEncoder().encode(JSON.stringify(obj));
    const stream = new Blob([bytes]).stream().pipeThrough(new CompressionStream('deflate-raw'));
    return PREFIX + b64url(new Uint8Array(await new Response(stream).arrayBuffer()));
  }
  async function decode(text) {
    const m = /DK64R1\.([A-Za-z0-9_-]+)/.exec(String(text));
    if (!m) throw new Error('no DK64R1 report data found');
    const stream = new Blob([unb64url(m[1])]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
    const obj = JSON.parse(await new Response(stream).text());
    if (!obj || obj.v !== 1) throw new Error('unsupported report version');
    return obj;
  }

  function hex(n) {
    return (n >>> 0).toString(16);
  }
  function toHex(bytes) {
    return Array.from(bytes, (x) => x.toString(16).padStart(2, '0')).join('');
  }
  function fromHex(h) {
    const out = new Uint8Array(h.length >> 1);
    for (let i = 0; i < out.length; i++) out[i] = parseInt(h.substr(i * 2, 2), 16);
    return out;
  }

  function versionText() {
    const v = versionInfo;
    if (!v || !v.version) return 'source build';
    return v.revision && v.revision !== 'unknown' ? `${v.version} (${String(v.revision).slice(0, 7)})` : v.version;
  }

  function settingsText(s) {
    const parts = [];
    if (s.drawdist != null) parts.push(`draw distance ${s.drawdist}×`);
    if (s.scale != null) parts.push(`render scale ${s.scale === 0 ? 'auto' : s.scale + '×'}`);
    if (s.msaa != null) parts.push(`MSAA ${s.msaa > 1 ? s.msaa + '×' : 'off'}`);
    if (s.interp != null) parts.push(`${30 << s.interp} fps`);
    if (s.audio3d != null) parts.push(`3D sound ${s.audio3d ? 'on' : 'off'}`);
    if (s.audiorate != null) parts.push(`audio ${s.audiorate / 1000} kHz`);
    if (s.touch != null) parts.push(`touch ${s.touch}`);
    return parts.join(' · ') || '—';
  }

  function inputText() {
    try {
      if (typeof navigator.getGamepads === 'function') {
        const gp = Array.from(navigator.getGamepads()).find((g) => g && g.connected);
        if (gp) return 'gamepad: ' + gp.id;
      }
    } catch (err) {
      // insecure origins: getGamepads missing or throwing
    }
    return navigator.maxTouchPoints > 0 && window.dk64touch ? 'touch' : 'keyboard';
  }

  function hitCount(probe) {
    return probe ? (probe.match(/^#\d+ dl=/gm) || []).length : 0;
  }

  function markRow(mark, hits) {
    const at = `(${mark.fx.toFixed(2)}, ${mark.fy.toFixed(2)})`;
    return ['Marked spot', mark.hasProbe ? `${at} · ${hits} triangles probed` : `${at} · probe unavailable`];
  }

  // ---- collect
  async function collect({ crash = null, level = null, kong = null, progress = null } = {}) {
    const m = window.Module;
    const attempt = (fn, fallback = null) => {
      try {
        return fn();
      } catch (err) {
        return fallback;
      }
    };
    if (!crash) attempt(() => m.ccall('port_save_flush_now', null));
    const save = attempt(() => (m.FS.analyzePath(SAVE_PATH).exists ? toHex(m.FS.readFile(SAVE_PATH)) : null));
    let resumeReason = null;
    const resume = attempt(() => {
      const r = JSON.parse(m.ccall('port_resume_capture', 'string'));
      if (!r.ok) {
        resumeReason = r.reason || 'unavailable';
        return null;
      }
      const { slot, kong: k, map, exit, hasPos, x, y, z, yaw } = r;
      return { slot, kong: k, map, exit, hasPos, x, y, z, yaw };
    });
    if (!resume && resumeReason === null) resumeReason = 'unavailable';
    const prog = progress || attempt(() => JSON.parse(m.ccall('port_level_progress', 'string')));
    const state = attempt(() => window.dk64dbg.state());
    const trace = attempt(
      () => window.dk64dbg.trace(0).events.slice(-30).map((e) => 'vi ' + e.vi + '  ' + e.text),
      [],
    );
    const logLines = ringLines(30);
    const version = await versionPromise;
    const settings = {};
    for (const k of [...SETTING_KEYS, 'touch']) {
      const v = attempt(() => window.dk64settings.get(k));
      if (v !== null && v !== undefined) settings[k] = v;
    }
    const dataSettings = {};
    for (const k of SETTING_KEYS) if (k in settings) dataSettings[k] = settings[k];

    const data = {
      v: 1,
      version: (version && version.version) || null,
      revision: (version && version.revision) || null,
      buildId: attempt(() => m.portBuildId) || null,
      save,
      resume,
      progress: prog,
      settings: dataSettings,
      crash,
      mark: null,
    };

    const map = state && Number.isInteger(state.map) ? state.map : resume ? resume.map : null;
    const where = [];
    if (level) where.push(level);
    if (map !== null) where.push('map 0x' + hex(map));
    if (kong) where.push(kong);
    const p = state && state.player;
    if (p && Number.isFinite(p.x)) where.push(`(${Math.round(p.x)}, ${Math.round(p.y)}, ${Math.round(p.z)})`);

    const table = [
      ['Version', versionText()],
      ['Build', data.buildId ? data.buildId.slice(0, 12) : '—'],
      ['Where', where.join(' · ') || '—'],
      [
        'Game',
        state ? `mode ${state.mode} · overlay ${state.overlay} · frame ${state.frames} · actors ${state.actors}` : '—',
      ],
      ['Save file', save ? 'included' : 'none yet'],
      ['Resume point', resume ? 'included' : `no (${resumeReason})`],
      ['Settings', settingsText(settings)],
      ['Input', inputText()],
      ['Browser', navigator.userAgent],
      ['GPU', gpuName()],
      ['Screen', `${innerWidth}×${innerHeight} @${devicePixelRatio}x`],
      ['Save server', attempt(() => window.dk64saves.status().server, '—')],
    ];
    const err = crash || (state && state.lastError);
    if (err) table.push(['Error', String(err).slice(0, 500)]);

    let blob;
    try {
      blob = await encode(data);
    } catch (e) {
      blob = null;
    }
    return {
      data,
      table,
      trace,
      log: logLines,
      blobs: blob ? [blob] : [],
      place: level || (map !== null ? 'map 0x' + hex(map) : null),
    };
  }

  // keeps the topmost (last-drawn) hits within cap characters
  function trimProbe(text, cap) {
    if (text === null || text === undefined || cap === 0) return null;
    if (text.length <= cap) return text;
    const parts = text.split(/(?=^#\d+ dl=)/m);
    const head = HIT_RE.test(parts[0]) ? '' : parts.shift();
    const hits = parts;
    const kept = [];
    let total = 0;
    for (let i = hits.length - 1; i >= 0; i--) {
      if (total + hits[i].length > cap) break;
      kept.unshift(hits[i]);
      total += hits[i].length;
    }
    if (!kept.length && hits.length) {
      kept.push(hits[hits.length - 1].slice(0, cap));
    }
    const dropped = hits.length - kept.length;
    const prefix = dropped > 0 ? `(${dropped} earlier hits omitted)\n` : head.length + total <= cap ? head : '';
    return prefix + kept.join('');
  }

  async function withMark(report, { fx, fy, probe }) {
    const p = typeof probe === 'string' ? probe : null;
    const mark = { fx, fy, probe: trimProbe(p, PROBE_CAPS[0]) };
    const data = { ...report.data, mark };
    const blobs = [];
    for (const cap of PROBE_CAPS) {
      try {
        blobs.push(await encode({ ...report.data, mark: { fx, fy, probe: trimProbe(p, cap) } }));
      } catch (err) {
        // keep the variants that encoded
      }
    }
    const table = report.table.filter((r) => r[0] !== 'Marked spot');
    const row = markRow({ fx, fy, hasProbe: p !== null }, hitCount(p));
    const errAt = table.findIndex((r) => r[0] === 'Error');
    if (errAt < 0) table.push(row);
    else table.splice(errAt, 0, row);
    return { ...report, data, table, blobs: blobs.length ? blobs : report.blobs };
  }

  // ---- issue URL
  function cell(v) {
    return String(v).replace(/\|/g, '\\|').replace(/\r?\n/g, ' ');
  }
  function fence(lines) {
    return lines.map((l) => String(l).replace(/`{3,}/g, "'''"));
  }
  function details(summary, lines) {
    if (!lines.length) return '';
    return `<details><summary>${summary}</summary>\n\n\`\`\`text\n${fence(lines).join('\n')}\n\`\`\`\n\n</details>`;
  }
  function markdown(report, blob, nTrace, nLog) {
    const parts = [
      ['| | |', '| --- | --- |', ...report.table.map(([k, v]) => `| **${cell(k)}** | ${cell(v)} |`)].join('\n'),
    ];
    parts.push(details('Recent game events', nTrace ? report.trace.slice(-nTrace) : []));
    parts.push(details('Console', nLog ? report.log.slice(-nLog) : []));
    if (blob) {
      parts.push(details('Save, location &amp; probe (developers: F2 → Load report…, or tools/report.py)', [blob]));
    }
    return parts.filter(Boolean).join('\n\n');
  }

  function firstLine(text, n = 60) {
    const l = String(text || '').split('\n')[0].trim();
    return l.length > n ? l.slice(0, n - 1) + '…' : l;
  }

  // text cut in 10% steps (with …) until the URL fits; the empty text as the last resort
  function fitText(text, build) {
    let url = build(text);
    for (let f = 0.9; url.length > MAX_URL && f > 0.05; f -= 0.1) {
      url = build(text.slice(0, Math.floor(text.length * f)) + '…');
    }
    return url.length <= MAX_URL ? url : build('');
  }

  function issueUrl(kind, text, report) {
    text = String(text || '');
    if (kind === 'idea') {
      return fitText(text, (t) =>
        REPO + '/issues/new?' +
        new URLSearchParams({ template: 'idea.yml', title: 'Idea: ' + firstLine(t), idea: t, version: versionText() }),
      );
    }
    const crash = kind === 'crash' ? String((report && report.data.crash) || crashMsg || 'unknown error').slice(0, 500) : null;
    let title;
    if (crash) title = 'Crash: ' + firstLine(crash);
    else title = 'Bug: ' + (firstLine(text) || (report && report.place) || 'report');
    const build = (body, md) => {
      const what = crash ? (body ? body + '\n\n' : '') + 'Crashed with: ' + crash : body;
      const params = { template: 'bug_report.yml', title, what };
      if (md !== null) params.report = md;
      return REPO + '/issues/new?' + new URLSearchParams(params);
    };
    if (!report) return fitText(text, (t) => build(t, null));
    const blobs = report.blobs.length ? report.blobs : [null];
    const tries = [[30, 30], [15, 15], [5, 5], [0, 0]].map(([t, l]) => [blobs[0], t, l]);
    for (const b of blobs.slice(1)) tries.push([b, 0, 0]);
    for (const [b, t, l] of tries) {
      const url = build(text, markdown(report, b, t, l));
      if (url.length <= MAX_URL) return url;
    }
    const md = markdown(report, blobs[blobs.length - 1], 0, 0);
    const url = fitText(text, (t) => build(t, md));
    return url.length <= MAX_URL ? url : fitText(text, (t) => build(t, null));
  }

  // ---- images (clipboard writes take PNG only; dk64dbg.capture() returns JPEG)
  function canvasPng(c) {
    return new Promise((resolve, reject) =>
      c.toBlob((b) => (b ? resolve(b) : reject(new Error('PNG encode failed'))), 'image/png'),
    );
  }
  async function draw(src) {
    const bmp = await createImageBitmap(src);
    const c = document.createElement('canvas');
    c.width = bmp.width;
    c.height = bmp.height;
    const ctx = c.getContext('2d');
    ctx.drawImage(bmp, 0, 0);
    bmp.close();
    return { c, ctx };
  }
  async function toPng(dataUrl) {
    const { c } = await draw(await (await fetch(dataUrl)).blob());
    return canvasPng(c);
  }
  async function markPng(blob, fx, fy) {
    const { c, ctx } = await draw(blob);
    const r = Math.max(14, 0.035 * c.width);
    for (const [color, width] of [['#fff', 7], ['#ff2d2d', 4]]) {
      ctx.beginPath();
      ctx.arc(fx * c.width, fy * c.height, r, 0, Math.PI * 2);
      ctx.strokeStyle = color;
      ctx.lineWidth = width;
      ctx.stroke();
    }
    return canvasPng(c);
  }

  // ---- developer import
  async function load(text) {
    const r = await decode(text);
    const m = window.Module;
    if (!m || !m.FS) throw new Error('start the game first');
    if (
      !confirm(
        "Load this report? It replaces this browser's game save with the reporter's (your current save stays in " +
          'the save server history) and reloads into their location.',
      )
    ) {
      return;
    }
    // flushing leaves the save clean, so the visibilitychange flush (port/src/save.c) cannot overwrite the import
    m.ccall('port_set_paused', null, ['number'], [1]);
    m.ccall('port_save_flush_now', null);
    if (r.save) {
      m.FS.writeFile(SAVE_PATH, fromHex(r.save));
      const t = Date.now();
      m.FS.utime(SAVE_PATH, t, t); // newest mtime: saves.js boot uploads and keeps this copy
      await new Promise((resolve, reject) => m.FS.syncfs(false, (err) => (err ? reject(err) : resolve())));
    } else {
      window.dk64dbg.toast('Report has no save file; loading its location only');
    }
    window.dk64qs.bootInto(r.save ? r.resume : null, r.settings || {});
  }

  window.dk64report = { log, crashed, isCrashed, collect, withMark, issueUrl, toPng, markPng, encode, decode, load };
})();
