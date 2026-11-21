// Live-debug layer: window.dk64dbg API, F2 debug panel (state, trace toggles, quicksave, mark/export), F8 marks,
// F5/F9 quicksave/quickload (web/quicksave.js).
// Used by the dk64-live-debug skill.
(function () {
  'use strict';

  const MAX_MARKS = 20;
  const MASK_KEY = 'dk64dbg.traceMask';
  // bits match PORT_TRACE_* in port/include/port/port.h; 8 is for temporary trace points
  const CATEGORIES = [
    { bit: 1, label: 'Map' },
    { bit: 2, label: 'Camera' },
    { bit: 4, label: 'Actors' },
    { bit: 8, label: 'Debug' },
  ];
  const marksList = [];
  let nextMarkId = 1;

  function mod() {
    const m = window.Module;
    if (!m || !m.ccall) {
      throw new Error('dk64dbg: game not started');
    }
    return m;
  }

  function state() {
    return JSON.parse(mod().ccall('port_debug_stats', 'string'));
  }

  function trace(since = 0) {
    return JSON.parse(mod().ccall('port_debug_trace', 'string', ['number'], [since >>> 0]));
  }

  // setting a mask also remembers it, so it applies from boot on the next load
  function traceMask(mask) {
    const cur = mod().ccall('port_debug_trace_mask', 'number', ['number'], [mask === undefined ? -1 : mask | 0]) >>> 0;
    if (mask !== undefined) {
      try {
        localStorage.setItem(MASK_KEY, String(cur));
      } catch (e) {
        // storage unavailable: the mask still applies to this session
      }
    }
    return cur;
  }

  function capture() {
    const m = mod();
    const seq0 = m.ccall('port_debug_capture_seq', 'number');
    m.ccall('port_debug_capture_request', null);
    const start = performance.now();
    return new Promise((resolve, reject) => {
      function poll() {
        if (m.ccall('port_debug_capture_seq', 'number') === seq0) {
          if (performance.now() - start > 1000) {
            reject(new Error('dk64dbg: capture timed out'));
          } else {
            requestAnimationFrame(poll);
          }
          return;
        }
        const w = m.ccall('port_debug_capture_width', 'number');
        const h = m.ccall('port_debug_capture_height', 'number');
        const p = m.ccall('port_debug_capture_pixels', 'number') >>> 0;
        const src = m.HEAPU8.subarray(p, p + w * h * 4);
        const img = new ImageData(w, h);
        const row = w * 4;
        for (let y = 0; y < h; y++) {
          // GL rows are bottom-up
          img.data.set(src.subarray((h - 1 - y) * row, (h - y) * row), y * row);
        }
        const full = document.createElement('canvas');
        full.width = w;
        full.height = h;
        full.getContext('2d').putImageData(img, 0, 0);
        const out = document.createElement('canvas');
        out.width = Math.min(w, 960);
        out.height = Math.round((h * out.width) / w);
        out.getContext('2d').drawImage(full, 0, 0, out.width, out.height);
        resolve(out.toDataURL('image/jpeg', 0.8));
      }
      requestAnimationFrame(poll);
    });
  }

  // RDP state of every triangle drawn through a point of the canvas (fractions of its size, top-left origin), recorded
  // during the next frame: display-list path, modes, combiner, tiles, recent texture loads, vertices
  function probe(fx, fy) {
    const m = mod();
    if (!m.ccall('port_debug_probe', 'number', ['number', 'number'], [fx, fy])) {
      return Promise.reject(new Error('dk64dbg: point is off the game image'));
    }
    const start = performance.now();
    return new Promise((resolve, reject) => {
      function poll() {
        const r = m.ccall('port_debug_probe_result', 'string');
        if (r) {
          resolve(r);
        } else if (performance.now() - start > 2000) {
          reject(new Error('dk64dbg: probe timed out'));
        } else {
          requestAnimationFrame(poll);
        }
      }
      requestAnimationFrame(poll);
    });
  }

  // extra: fields merged into the mark (Alt+click adds `probe`)
  async function mark(note = '', extra = {}) {
    const cur = trace(0);
    const m = {
      id: nextMarkId++,
      time: new Date().toISOString(),
      note,
      state: state(),
      trace: trace(Math.max(0, cur.next - 50)).events,
      image: null,
      ...extra,
    };
    try {
      m.image = await capture();
    } catch (e) {
      m.image = null;
    }
    marksList.push(m);
    if (marksList.length > MAX_MARKS) {
      marksList.shift();
    }
    toast(`Mark #${m.id} saved`);
    refreshIfVisible();
    return m;
  }

  // Alt+click on the game: a mark carrying the probe of the clicked point
  async function probeMark(fx, fy) {
    const p = await probe(fx, fy);
    return mark(`probe ${fx.toFixed(3)},${fy.toFixed(3)}`, { probe: p });
  }

  // downloads every mark plus the full trace ring as dk64-marks-<local time>.json; returns the file name
  function exportMarks() {
    const now = new Date();
    const pad = (n) => String(n).padStart(2, '0');
    const name = `dk64-marks-${now.getFullYear()}${pad(now.getMonth() + 1)}${pad(now.getDate())}-${pad(now.getHours())}${pad(now.getMinutes())}${pad(now.getSeconds())}.json`;
    const data = { when: now.toISOString(), state: null, mask: null, marks: marksList, trace: null };
    try {
      data.state = state();
      data.mask = traceMask();
      data.trace = trace(0);
    } catch (e) {
      // game not started: export the marks alone
    }
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([JSON.stringify(data)], { type: 'application/json' }));
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
    toast(`Exported ${marksList.length} mark${marksList.length === 1 ? '' : 's'} → ${name}`);
    return name;
  }

  function clearMarks() {
    marksList.length = 0;
    refreshIfVisible();
  }

  // called by main.js after window.Module is set and before callMain, so the remembered mask covers boot
  function moduleReady() {
    let saved = null;
    try {
      saved = localStorage.getItem(MASK_KEY);
    } catch (e) {
      return;
    }
    if (saved !== null) {
      traceMask(Number(saved) >>> 0);
    }
  }

  // ---- panel / toast
  const style = document.createElement('style');
  style.textContent = `
#dk64dbg-overlay {
  position: fixed; top: 40px; left: 10px; z-index: 2; width: 420px; box-sizing: border-box;
  padding: 10px 12px 12px; border-radius: 10px; border: 1px solid rgba(255,255,255,.09);
  background: rgba(16,18,24,.84); backdrop-filter: blur(8px); -webkit-backdrop-filter: blur(8px);
  box-shadow: 0 8px 28px rgba(0,0,0,.45); color: #d7dce5;
  font: 12px/1.4 system-ui, -apple-system, sans-serif; user-select: none; display: none;
}
#dk64dbg-overlay .hd { display: flex; align-items: center; gap: 8px; margin-bottom: 8px; }
#dk64dbg-overlay .title { font-weight: 600; letter-spacing: .02em; color: #f2f4f8; }
#dk64dbg-overlay .hint { color: #7d8594; font-size: 11px; }
#dk64dbg-overlay .fps { margin-left: auto; padding: 1px 8px; border-radius: 999px; font: 600 11px/18px ui-monospace, monospace; }
#dk64dbg-overlay .ok { background: rgba(80,200,120,.16); color: #6fe09a; }
#dk64dbg-overlay .warn { background: rgba(240,180,60,.16); color: #f2c35b; }
#dk64dbg-overlay .bad { background: rgba(240,90,90,.18); color: #ff8080; }
#dk64dbg-overlay .grid { display: grid; grid-template-columns: 58px 1fr; gap: 2px 10px; }
#dk64dbg-overlay .k { color: #7d8594; }
#dk64dbg-overlay .v { font-family: ui-monospace, monospace; color: #e8ecf2; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
#dk64dbg-overlay .err { color: #ff8080; white-space: normal; }
#dk64dbg-overlay .sec { margin-top: 10px; padding-top: 8px; border-top: 1px solid rgba(255,255,255,.07); }
#dk64dbg-overlay .lbl { color: #7d8594; font-size: 11px; text-transform: uppercase; letter-spacing: .06em; margin-bottom: 6px; display: flex; }
#dk64dbg-overlay .lbl span:last-child { margin-left: auto; text-transform: none; letter-spacing: 0; font-family: ui-monospace, monospace; }
#dk64dbg-overlay .chips, #dk64dbg-overlay .actions { display: flex; flex-wrap: wrap; gap: 6px; }
#dk64dbg-overlay button {
  font: 500 12px/1 system-ui, -apple-system, sans-serif; color: #c9cfda; cursor: pointer;
  padding: 6px 10px; border-radius: 7px; border: 1px solid rgba(255,255,255,.12); background: rgba(255,255,255,.05);
}
#dk64dbg-overlay button:hover { background: rgba(255,255,255,.11); color: #fff; }
#dk64dbg-overlay button.chip { border-radius: 999px; padding: 4px 10px; }
#dk64dbg-overlay button.chip.on { background: rgba(90,160,255,.22); border-color: rgba(110,170,255,.6); color: #cfe2ff; }
#dk64dbg-overlay button.primary { background: rgba(90,160,255,.25); border-color: rgba(110,170,255,.6); color: #e4efff; }
#dk64dbg-overlay button.primary:hover { background: rgba(90,160,255,.38); }
#dk64dbg-overlay .log { margin-top: 8px; font: 11px/1.45 ui-monospace, monospace; color: #b8c0cc; }
#dk64dbg-overlay .log div { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
#dk64dbg-overlay .log .vi { color: #66707f; margin-right: 6px; }
#dk64dbg-overlay .log .empty { color: #66707f; }
#dk64dbg-toast {
  position: fixed; bottom: 16px; left: 16px; z-index: 3; padding: 8px 14px; border-radius: 8px;
  background: rgba(16,18,24,.9); border: 1px solid rgba(255,255,255,.1); color: #e8ecf2;
  box-shadow: 0 6px 20px rgba(0,0,0,.4); font: 500 12px/1.3 system-ui, -apple-system, sans-serif;
  pointer-events: none; opacity: 0; transform: translateY(6px); transition: opacity .15s, transform .15s;
}
#dk64dbg-toast.show { opacity: 1; transform: none; }
`;
  document.head.append(style);

  function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) {
      e.className = cls;
    }
    if (text !== undefined) {
      e.textContent = text;
    }
    return e;
  }

  // buttons never take focus: keys keep going to the game (Enter on a focused button would re-click it)
  function button(label, cls, onClick) {
    const b = el('button', cls, label);
    b.type = 'button';
    b.tabIndex = -1;
    b.addEventListener('mousedown', (e) => e.preventDefault());
    b.addEventListener('click', onClick);
    return b;
  }

  const panel = el('div');
  panel.id = 'dk64dbg-overlay';

  const hd = el('div', 'hd');
  const fpsEl = el('span', 'fps ok', '-- fps');
  hd.append(el('span', 'title', 'DK64 debug'), el('span', 'hint', 'F2 hide · F8 mark · Alt+click probe · F5/F9 quicksave'), fpsEl);

  const grid = el('div', 'grid');
  const rows = {};
  for (const [key, label] of [
    ['map', 'map'],
    ['mode', 'mode'],
    ['time', 'time'],
    ['pos', 'player'],
    ['facing', 'facing'],
    ['actors', 'actors'],
  ]) {
    rows[key] = el('div', 'v', '-');
    grid.append(el('div', 'k', label), rows[key]);
  }
  const errK = el('div', 'k', 'error');
  const errV = el('div', 'v err');
  grid.append(errK, errV);

  const traceSec = el('div', 'sec');
  const maskEl = el('span', null, '');
  const traceLbl = el('div', 'lbl');
  traceLbl.append(el('span', null, 'Trace'), maskEl);
  const chips = el('div', 'chips');
  const chipEls = CATEGORIES.map((c) => {
    const b = button(c.label, 'chip', () => {
      try {
        traceMask(traceMask() ^ c.bit);
      } catch (e) {
        toast(e.message);
      }
      refreshIfVisible();
    });
    chips.append(b);
    return { bit: c.bit, el: b };
  });
  const logEl = el('div', 'log');
  traceSec.append(traceLbl, chips, logEl);

  const qsSec = el('div', 'sec');
  const qsStatus = el('span', null, '—');
  const qsLbl = el('div', 'lbl');
  qsLbl.append(el('span', null, 'Quicksave'), qsStatus);
  const qsAuto = button('Auto-resume', 'chip', () => {
    window.dk64qs.autoResume(!window.dk64qs.autoResume());
    refreshIfVisible();
  });
  const qsActions = el('div', 'actions');
  qsActions.append(
    button('⤒ Save (F5)', 'primary', () => quicksave()),
    button('⤓ Load (F9)', null, () => quickload()),
    qsAuto,
  );
  qsSec.append(qsLbl, qsActions);
  if (!window.dk64qs) {
    qsSec.style.display = 'none';
  }

  const marksSec = el('div', 'sec');
  const marksCount = el('span', null, '0');
  const marksLbl = el('div', 'lbl');
  marksLbl.append(el('span', null, 'Marks'), marksCount);
  const actions = el('div', 'actions');
  actions.append(
    button('● Mark', 'primary', () => mark().catch((err) => toast('Mark failed: ' + err.message))),
    button('⤓ Export', null, () => exportMarks()),
    button('Clear', null, () => clearMarks()),
  );
  marksSec.append(marksLbl, actions);

  panel.append(hd, grid, traceSec, qsSec, marksSec);

  const toastEl = el('div');
  toastEl.id = 'dk64dbg-toast';
  document.body.append(panel, toastEl);

  let toastTimer = 0;
  function toast(msg) {
    toastEl.textContent = msg;
    toastEl.classList.add('show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => toastEl.classList.remove('show'), 1800);
  }

  // a co-op session must not load or reuse snapshots outside web/net.js
  function coopBlocks() {
    if (!window.dk64net || !window.dk64net.active()) return false;
    toast('Disabled in co-op');
    return true;
  }

  function quicksave() {
    if (!window.dk64qs) {
      toast('quicksave.js not loaded');
      return;
    }
    if (coopBlocks()) return;
    window.dk64qs.save().catch((err) => toast('Quicksave failed: ' + err.message)).finally(refreshIfVisible);
  }

  function quickload() {
    if (!window.dk64qs) {
      toast('quicksave.js not loaded');
      return;
    }
    if (coopBlocks()) return;
    window.dk64qs.load().catch((err) => toast('Quickload failed: ' + err.message)).finally(refreshIfVisible);
  }

  let refreshTimer = 0;
  let last = null; // {frames, t} of the previous refresh, for fps

  function hex(n, width) {
    return n.toString(16).padStart(width, '0');
  }

  function refresh() {
    marksCount.textContent = String(marksList.length);
    refreshQs();
    let s, tr, mask;
    try {
      s = state();
      tr = trace(0);
      mask = traceMask();
    } catch (e) {
      rows.map.textContent = e.message;
      return;
    }
    const now = performance.now();
    if (last && now - last.t >= 1000) {
      const fps = ((s.frames - last.frames) * 1000) / (now - last.t);
      fpsEl.textContent = `${fps.toFixed(1)} fps`;
      fpsEl.className = 'fps ' + (fps >= 27 ? 'ok' : fps >= 20 ? 'warn' : 'bad');
      last = { frames: s.frames, t: now };
    } else if (!last) {
      last = { frames: s.frames, t: now };
    }
    const p = s.player;
    rows.map.textContent = `0x${hex(s.map, 2)} (${s.map})`;
    rows.mode.textContent = `${s.mode} · overlay ${s.overlay}`;
    rows.time.textContent = `vi ${s.vi} · frame ${s.frames}`;
    rows.pos.textContent = p ? `${p.x.toFixed(1)}, ${p.y.toFixed(1)}, ${p.z.toFixed(1)}` : '—';
    rows.facing.textContent = p ? `yaw ${p.yaw} · chunk ${p.chunk}` : '—';
    rows.actors.textContent = String(s.actors);
    errK.style.display = errV.style.display = s.lastError ? '' : 'none';
    errV.textContent = s.lastError;

    maskEl.textContent = `0x${hex(mask, 1)}`;
    for (const c of chipEls) {
      c.el.classList.toggle('on', (mask & c.bit) !== 0);
    }
    logEl.replaceChildren();
    const recent = tr.events.slice(-5).reverse();
    if (!recent.length) {
      logEl.append(el('div', 'empty', 'no events yet'));
    }
    for (const ev of recent) {
      const line = el('div');
      line.title = ev.text;
      line.append(el('span', 'vi', String(ev.vi)), document.createTextNode(ev.text));
      logEl.append(line);
    }
  }

  function refreshQs() {
    if (!window.dk64qs) {
      return;
    }
    const st = window.dk64qs.status();
    const parts = [];
    if (st.snapshot) {
      const t = new Date(st.snapshot.time).toTimeString().slice(0, 8);
      parts.push(`snap ${t} 0x${hex(st.snapshot.map, 2)}${st.snapshot.sameBuild ? '' : ' (other build)'}`);
    }
    if (st.resume) {
      parts.push(`resume 0x${hex(st.resume.map, 2)}`);
    }
    qsStatus.textContent = parts.length ? parts.join(' · ') : '—';
    qsAuto.classList.toggle('on', st.auto);
  }

  function refreshIfVisible() {
    if (panel.style.display === 'block') {
      refresh();
    }
  }

  function overlay(on) {
    const show = on === undefined ? panel.style.display !== 'block' : !!on;
    panel.style.display = show ? 'block' : 'none';
    clearInterval(refreshTimer);
    refreshTimer = 0;
    last = null;
    if (show) {
      refresh();
      refreshTimer = setInterval(refresh, 250);
    }
    return show;
  }

  // capture phase: runs before SDL's keydown listener
  window.addEventListener(
    'keydown',
    (e) => {
      if (e.key !== 'F2' && e.key !== 'F8' && e.key !== 'F5' && e.key !== 'F9') {
        return;
      }
      e.preventDefault();
      if (e.repeat) {
        return;
      }
      if (e.key === 'F2') {
        overlay();
      } else if (e.key === 'F5') {
        quicksave();
      } else if (e.key === 'F9') {
        quickload();
      } else {
        mark().catch((err) => toast('Mark failed: ' + err.message));
      }
    },
    true,
  );

  // Alt+click on the game canvas: probe mark of that point (capture phase, so SDL never sees the click)
  window.addEventListener(
    'mousedown',
    (e) => {
      const c = document.getElementById('canvas');
      if (!e.altKey || e.button !== 0 || e.target !== c) {
        return;
      }
      e.preventDefault();
      e.stopPropagation();
      const r = c.getBoundingClientRect();
      probeMark((e.clientX - r.left) / r.width, (e.clientY - r.top) / r.height).catch((err) =>
        toast('Probe failed: ' + err.message),
      );
    },
    true,
  );

  window.dk64dbg = {
    state,
    trace,
    traceMask,
    capture,
    mark,
    probe,
    marks: () => marksList,
    clearMarks,
    exportMarks,
    moduleReady,
    overlay,
    toast,
  };
})();
