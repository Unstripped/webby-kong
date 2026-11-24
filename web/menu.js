// Esc menu: freezes the game (port_set_paused) and shows the current level's collectables (port_level_progress).
// Opened by Esc, gamepad Back/Select (Standard mapping button 8) or the touch overlay's ☰ button.
// Report a bug / Suggest an idea switch the panel to a report view that opens a prefilled GitHub issue form
// (web/report.js): a bug report shows a screenshot of the frozen frame, where a click marks the broken spot and
// probes the renderer there. A crash (wasm trap or abort, detected by report.js) opens the same view as "UH OH!",
// which cannot be closed.
// Exit to dashboard (sign-in gate on, web/saves.js dashboard()): records where the player stands
// (port_resume_capture), runs the pause menu's in-game save (port_save_request; the game runs for the ~1 s the
// EEPROM thread needs, port_save_busy), waits for the upload, stores the resume point on the server and goes to /.
// When something fails it says so and offers "Exit anyway".
(function () {
  'use strict';

  const LEVELS = [
    'Jungle Japes',
    'Angry Aztec',
    'Frantic Factory',
    'Gloomy Galleon',
    'Fungi Forest',
    'Crystal Caves',
    'Creepy Castle',
    'DK Isles',
    'Hideout Helm',
  ];
  const KONGS = [
    { id: 'dk', name: 'DK', color: '#f5c518' },
    { id: 'diddy', name: 'Diddy', color: '#e53935' },
    { id: 'lanky', name: 'Lanky', color: '#1e88e5' },
    { id: 'tiny', name: 'Tiny', color: '#8e44ad' },
    { id: 'chunky', name: 'Chunky', color: '#43a047' },
  ];
  const BLUEPRINT = ['–', 'Found', 'Turned in'];
  // field of each kong entry, label, icon (web/menu/<name>.png), value text, complete
  const COLUMNS = [
    { key: 'gb', label: 'Golden Bananas', icon: () => 'gb', text: (v) => `${v}/5`, done: (v) => v === 5 },
    {
      key: 'bananas',
      label: 'Bananas',
      icon: (kong) => `banana-${kong.id}`,
      text: (v) => `${v}/100`,
      done: (v) => v === 100,
    },
    { key: 'medal', label: 'Medal', icon: () => 'medal', text: (v) => (v ? '✓' : '–'), done: (v) => v === 1 },
    {
      key: 'blueprint',
      label: 'Blueprint',
      icon: () => 'blueprint',
      text: (v) => BLUEPRINT[v] || String(v),
      done: (v) => v === 2,
    },
  ];
  // level-wide collectables: count field, total field (0 = none in this level)
  const WIDE = [
    { key: 'fairies', total: 'fairiesTotal', icon: 'fairy', label: () => 'Fairies' },
    { key: 'crowns', total: 'crownsTotal', icon: 'crown', label: (t) => (t === 1 ? 'Battle crown' : 'Battle crowns') },
    { key: 'keys', total: 'keysTotal', icon: 'key', label: () => 'Boss key' },
  ];
  const CHUNKY = '"Arial Black", "Arial Bold", Gadget, system-ui, sans-serif';

  const style = document.createElement('style');
  style.textContent = `
#dk64menu { position: fixed; inset: 0; z-index: 10; display: none; align-items: center; justify-content: center;
  pointer-events: auto; background: radial-gradient(ellipse at center, rgba(18,46,12,.5), rgba(0,0,0,.8)); }
#dk64menu.on { display: flex; }
#dk64menu .panel { position: relative; width: min(680px, 94vw); max-height: 92vh; overflow: auto; box-sizing: border-box;
  padding: 20px 22px 16px; border-radius: 14px; background: #7a3f1a url('menu/wood.jpg') center / cover;
  border: 4px solid #3a1d00;
  box-shadow: 0 0 0 4px #c9a25a, 0 0 0 7px #3a1d00, inset 0 0 40px rgba(0,0,0,.55), 0 14px 40px rgba(0,0,0,.7);
  color: #fff3d6; font: 700 13px/1.3 ${CHUNKY}; user-select: none; }
#dk64menu .panel::before { content: ''; position: absolute; inset: 7px; border: 2px dashed rgba(240,214,150,.55);
  border-radius: 9px; pointer-events: none; }
#dk64menu .bolt { position: absolute; width: 12px; height: 12px; border-radius: 50%;
  background: radial-gradient(circle at 35% 35%, #f0f0f0, #9a9a9a 55%, #4a4a4a); box-shadow: 0 1px 2px rgba(0,0,0,.7); }
#dk64menu .tl { top: 12px; left: 12px; } #dk64menu .tr { top: 12px; right: 12px; }
#dk64menu .bl { bottom: 12px; left: 12px; } #dk64menu .br { bottom: 12px; right: 12px; }
#dk64menu .title { text-align: center; margin: 0 0 10px; font: 900 32px/1 ${CHUNKY}; letter-spacing: .06em; color: #ffd21f;
  -webkit-text-stroke: 2px #3a1d00; paint-order: stroke fill; text-shadow: 0 4px 0 #3a1d00, 0 6px 10px rgba(0,0,0,.5); }
#dk64menu .lbl { display: table; margin: 0 auto 16px; padding: 5px 18px; font: 900 15px/1.2 ${CHUNKY}; color: #3a1d00;
  text-transform: uppercase; letter-spacing: .05em; background: linear-gradient(#f1c27d, #d39a4f);
  border: 3px solid #3a1d00; border-radius: 6px; box-shadow: 0 3px 0 #3a1d00; }
#dk64menu .cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(112px, 1fr)); gap: 8px; }
#dk64menu .card { background: rgba(36,18,4,.62); border: 3px solid var(--k); border-radius: 10px; padding-bottom: 8px;
  overflow: hidden; box-shadow: 0 3px 0 #2a1405; }
#dk64menu .name { background: var(--k); text-align: center; padding: 5px 4px; margin-bottom: 8px; font: 900 15px/1.2 ${CHUNKY};
  color: #fff; -webkit-text-stroke: 1px #2a1405; paint-order: stroke fill; text-shadow: 0 2px 0 #2a1405; }
#dk64menu .row { display: flex; align-items: center; gap: 6px; padding: 2px 8px; white-space: nowrap; }
#dk64menu .row.none { visibility: hidden; }
#dk64menu .row img { width: 22px; height: 22px; flex: none; filter: drop-shadow(0 1px 0 rgba(0,0,0,.7)); }
#dk64menu .val { font: 900 12px/1 ${CHUNKY}; color: #fff3d6; text-shadow: 0 1px 0 #000, 0 0 3px rgba(0,0,0,.6); }
#dk64menu .done .val { color: #b6ff6a; }
#dk64menu .card.cur { border-color: #fff3a0; box-shadow: 0 0 0 2px var(--k), 0 0 18px 5px var(--k), 0 3px 0 #2a1405;
  animation: dk64bob 1.4s ease-in-out infinite; }
@keyframes dk64bob { 0%, 100% { transform: translateY(-5px); } 50% { transform: translateY(-1px); } }
@media (prefers-reduced-motion: reduce) { #dk64menu .card.cur { animation: none; transform: translateY(-4px); } }
#dk64menu .wide { display: flex; justify-content: center; flex-wrap: wrap; gap: 8px 22px; margin-top: 14px;
  padding: 6px 12px; background: rgba(36,18,4,.55); border-radius: 10px; }
#dk64menu .item { display: flex; align-items: center; gap: 6px; }
#dk64menu .item img { width: 28px; height: 28px; filter: drop-shadow(0 1px 0 rgba(0,0,0,.7)); }
#dk64menu .item .val { font-size: 14px; }
#dk64menu .msg { text-align: center; margin: 10px 0; font: 900 15px/1.4 ${CHUNKY}; color: #ffd21f;
  -webkit-text-stroke: 1px #3a1d00; paint-order: stroke fill; text-shadow: 0 2px 0 #3a1d00; }
#dk64menu .msg span { display: block; font: 600 11px/1.4 system-ui, sans-serif; color: rgba(255,243,214,.7);
  -webkit-text-stroke: 0; text-shadow: none; }
#dk64menu .actions { display: flex; align-items: center; justify-content: space-between; margin-top: 16px; }
#dk64menu .actions.main { flex-direction: column-reverse; align-items: stretch; justify-content: flex-start; gap: 8px; }
#dk64menu .hint { font: 600 11px system-ui, sans-serif; color: rgba(255,243,214,.75); text-align: center; }
#dk64menu button { font: 900 15px/1 ${CHUNKY}; letter-spacing: .04em; text-transform: uppercase; cursor: pointer;
  padding: 10px 22px; border-radius: 12px; color: #3a1d00; background: linear-gradient(#ffe36a, #f5b400);
  border: 3px solid #3a1d00; box-shadow: 0 4px 0 #3a1d00; }
#dk64menu button:hover { background: linear-gradient(#fff08f, #ffc21a); }
#dk64menu button:active { transform: translateY(3px); box-shadow: 0 1px 0 #3a1d00; }
#dk64menu .lbl.sec { margin-top: 18px; }
#dk64menu .settings { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
#dk64menu .set { background: rgba(36,18,4,.62); border: 3px solid #c9a25a; border-radius: 10px; padding: 8px 10px 9px;
  box-shadow: 0 3px 0 #2a1405; }
#dk64menu .set .head { display: flex; align-items: baseline; justify-content: space-between; gap: 8px; margin-bottom: 7px; }
#dk64menu .set .sname { font: 900 13px/1.1 ${CHUNKY}; color: #ffd21f; -webkit-text-stroke: 1px #3a1d00; paint-order: stroke fill;
  text-shadow: 0 2px 0 #3a1d00; text-transform: uppercase; letter-spacing: .04em; }
#dk64menu .set .now { font: 900 13px/1 ${CHUNKY}; color: #b6ff6a; text-shadow: 0 1px 0 #000; }
#dk64menu .set .note { font: 600 10.5px/1.3 system-ui, sans-serif; color: rgba(255,243,214,.7); margin-top: 6px; }
#dk64menu .slider { position: relative; }
#dk64menu .ticks { position: absolute; left: 9px; right: 9px; top: 19px; display: flex; justify-content: space-between;
  pointer-events: none; }
#dk64menu .ticks i { width: 2px; height: 4px; background: rgba(255,243,214,.4); }
#dk64menu .ends { display: flex; justify-content: space-between; font: 600 10px system-ui, sans-serif; color: rgba(255,243,214,.6); margin-top: 6px; }
#dk64menu .foot { font: 600 10.5px system-ui, sans-serif; color: rgba(255,243,214,.6); text-align: center; margin-top: 8px; }
#dk64menu input[type=range] { -webkit-appearance: none; appearance: none; display: block; width: 100%; height: 22px; margin: 0;
  background: transparent; cursor: pointer; }
#dk64menu input[type=range]::-webkit-slider-runnable-track { height: 8px; border-radius: 4px; border: 2px solid #3a1d00;
  background: linear-gradient(#ffe36a, #f5b400) 0 0 / var(--p) 100% no-repeat, rgba(0,0,0,.45); }
#dk64menu input[type=range]::-moz-range-track { height: 8px; border-radius: 4px; border: 2px solid #3a1d00;
  background: linear-gradient(#ffe36a, #f5b400) 0 0 / var(--p) 100% no-repeat, rgba(0,0,0,.45); }
#dk64menu input[type=range]::-webkit-slider-thumb { -webkit-appearance: none; margin-top: -9px; width: 22px; height: 22px;
  border-radius: 50%; border: 3px solid #3a1d00; box-sizing: border-box;
  background: radial-gradient(circle at 35% 30%, #fff6b0, #f5b400 60%, #b07800); box-shadow: 0 2px 0 #3a1d00; }
#dk64menu input[type=range]::-moz-range-thumb { width: 22px; height: 22px; border-radius: 50%; border: 3px solid #3a1d00;
  box-sizing: border-box; background: radial-gradient(circle at 35% 30%, #fff6b0, #f5b400 60%, #b07800);
  box-shadow: 0 2px 0 #3a1d00; }
#dk64menu .seg { display: flex; gap: 4px; }
#dk64menu .seg button { flex: 1; padding: 6px 0; font: 900 12px/1 ${CHUNKY}; letter-spacing: 0; text-transform: none;
  color: #fff3d6; background: rgba(0,0,0,.35); border: 2px solid #3a1d00; border-radius: 7px; box-shadow: none;
  text-shadow: 0 1px 0 #000; }
#dk64menu .seg button:hover:not(:disabled):not(.sel) { background: rgba(0,0,0,.5); }
#dk64menu .seg button.sel { color: #3a1d00; text-shadow: none; background: linear-gradient(#ffe36a, #f5b400);
  box-shadow: 0 2px 0 #3a1d00; }
#dk64menu .seg button:active { transform: none; }
#dk64menu .seg button:disabled { opacity: .35; cursor: default; }
#dk64menu .btns { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
#dk64menu .btns button { width: 100%; }
#dk64menu button.alt { padding: 10px 14px; font-size: 13px; background: linear-gradient(#f1c27d, #d39a4f); }
#dk64menu button.alt:hover { background: linear-gradient(#f8d59c, #e0ad66); }
#dk64menu button:disabled { opacity: .6; cursor: default; }
#dk64menu button:disabled:active { transform: none; box-shadow: 0 4px 0 #3a1d00; }
#dk64menu .rintro { margin: 0 0 10px; font: 600 13px/1.45 system-ui, sans-serif; color: #fff3d6; }
#dk64menu .rtext { display: block; width: 100%; box-sizing: border-box; resize: vertical; padding: 8px 10px;
  font: 500 14px/1.4 system-ui, sans-serif; color: #fff3d6; background: rgba(20,10,2,.72); border: 3px solid #c9a25a;
  border-radius: 10px; outline: none; user-select: text; -webkit-user-select: text; }
#dk64menu .rtext:focus { border-color: #ffd21f; }
#dk64menu .rtext::placeholder { color: rgba(255,243,214,.5); }
#dk64menu .rshot { margin-top: 12px; text-align: center; }
#dk64menu .rshotwrap { position: relative; display: inline-block; line-height: 0; }
#dk64menu .rimg { max-width: 100%; max-height: 220px; cursor: crosshair; border: 3px solid #3a1d00; border-radius: 6px;
  box-shadow: 0 3px 0 #2a1405; }
#dk64menu .rpin { position: absolute; width: 22px; height: 22px; margin: -11px 0 0 -11px; box-sizing: border-box;
  border: 3px solid #ff2d2d; border-radius: 50%; box-shadow: 0 0 0 2px #fff; pointer-events: none; display: none; }
#dk64menu .rhint { margin: 6px 0 2px; font: 900 13px/1.3 ${CHUNKY}; color: #ffd21f; text-shadow: 0 1px 0 #3a1d00; }
#dk64menu .rstatus { display: block; font: 600 11px/1.4 system-ui, sans-serif; color: rgba(255,243,214,.8); }
#dk64menu .rshot button { margin-top: 6px; }
#dk64menu .rcrash { margin: 10px 0 0; padding: 8px 10px; max-height: 90px; overflow: auto; white-space: pre-wrap;
  word-break: break-word; font: 500 11px/1.4 ui-monospace, monospace; color: #ffb4a8; background: rgba(20,10,2,.72);
  border-radius: 8px; user-select: text; -webkit-user-select: text; }
#dk64menu .rlist { margin: 12px 0 0; padding-left: 20px; font: 600 12px/1.5 system-ui, sans-serif; color: rgba(255,243,214,.85); }
#dk64menu .rnote { margin: 10px 0 0; font: 600 11px/1.4 system-ui, sans-serif; color: rgba(255,243,214,.7); }
#dk64menu .rdone { margin: 10px 0 0; text-align: center; font: 600 12px/1.4 system-ui, sans-serif; color: #b6ff6a; }
#dk64menu .rdone a { color: #ffd21f; margin-left: 6px; }
@media (max-width: 520px) {
  #dk64menu .title { font-size: 24px; }
  #dk64menu .settings { grid-template-columns: 1fr; }
  #dk64menu .actions { flex-wrap: wrap; gap: 8px; }
}
`;
  document.head.appendChild(style);

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

  function img(name, cls) {
    const i = el('img', cls);
    i.src = 'menu/' + name + '.png';
    i.alt = '';
    i.draggable = false;
    return i;
  }

  // preload so the first open doesn't pop in
  for (const n of ['gb', 'medal', 'blueprint', 'fairy', 'crown', 'key', ...KONGS.map((k) => 'banana-' + k.id)]) {
    new Image().src = 'menu/' + n + '.png';
  }
  new Image().src = 'menu/wood.jpg';

  const backdrop = el('div');
  backdrop.id = 'dk64menu';
  const panel = el('div', 'panel');
  const label = el('div', 'lbl');
  const cards = el('div', 'cards');
  const wide = el('div', 'wide');
  const msg = el('p', 'msg');
  const setLabel = el('div', 'lbl sec', 'Settings');
  const settingsEl = el('div', 'settings');
  const foot = el('div', 'foot', 'Changes apply immediately and are remembered on this device');
  const actions = el('div', 'actions main');
  const btns = el('div', 'btns');
  const exitBtn = button('Exit to dashboard', 'alt', () => exitToDashboard());
  const resumeBtn = button('Resume', null, () => close());
  btns.append(
    button('Suggest an idea', 'alt', () => showReport('idea')),
    button('Report a bug', 'alt', () => showReport('bug')),
    exitBtn,
    resumeBtn,
  );
  actions.append(el('span', 'hint', 'Esc · Back to resume'), btns);
  const exitNote = el('p', 'msg');
  exitNote.style.display = 'none';
  let exiting = false;
  let exitForce = false;
  const titleEl = el('div', 'title', 'PAUSED');
  const mainView = el('div');
  mainView.append(label, cards, wide, msg, setLabel, settingsEl, foot, actions, exitNote);

  // report view (bug, idea, crash): see showReport()
  const reportView = el('div');
  reportView.style.display = 'none';
  const rintro = el('p', 'rintro');
  const rtext = el('textarea', 'rtext');
  rtext.maxLength = 1000;
  rtext.rows = 4;
  const rshot = el('div', 'rshot');
  const rshotwrap = el('div', 'rshotwrap');
  const rimg = el('img', 'rimg');
  rimg.alt = 'Screenshot of the game';
  rimg.draggable = false;
  const rpin = el('span', 'rpin');
  rshotwrap.append(rimg, rpin);
  const rhint = el('p', 'rhint');
  const rstatus = el('span', 'rstatus');
  const rsave = button('Save screenshot', 'alt', () => saveShot());
  rshot.append(rshotwrap, rhint, rstatus, rsave);
  const rcrash = el('pre', 'rcrash');
  const rlist = el('ul', 'rlist');
  const rnote = el(
    'p',
    'rnote',
    'Opens GitHub in a new tab. Sign in (free account), check it, and press Submit. Issues are public.',
  );
  const ractions = el('div', 'actions');
  const rleft = el('span');
  const rback = button('Back', 'alt', () => showMain());
  const rreload = button('Reload game', 'alt', () => location.reload());
  rleft.append(rback, rreload);
  const go = button('Open GitHub', null, () => openGithub());
  ractions.append(rleft, go);
  const rdone = el('p', 'rdone', 'GitHub opened in a new tab.');
  const rdoneLink = el('a', null, 'Open it again');
  rdoneLink.target = '_blank';
  rdoneLink.rel = 'noopener';
  rdone.append(rdoneLink);
  reportView.append(rintro, rtext, rshot, rcrash, rlist, rnote, ractions, rdone);

  panel.append(...['tl', 'tr', 'bl', 'br'].map((c) => el('span', 'bolt ' + c)), titleEl, mainView, reportView);
  backdrop.append(panel);
  // close only when the press also started on the backdrop: the click that follows the ☰ tap that opened the
  // menu, or a slider drag released outside the panel, lands on the backdrop too
  let downOnBackdrop = false;
  backdrop.addEventListener('pointerdown', (e) => {
    downOnBackdrop = e.target === backdrop;
  });
  backdrop.addEventListener('click', (e) => {
    if (e.target === backdrop && downOnBackdrop) close();
    downOnBackdrop = false;
  });
  document.body.appendChild(backdrop);

  // one box per setting: name and current value, the control, a note; refresh() re-reads web/settings.js
  function settingBox(name, control, extra) {
    const box = el('div', 'set');
    const head = el('div', 'head');
    const now = el('span', 'now');
    const note = el('div', 'note');
    head.append(el('span', 'sname', name), now);
    box.append(head, control, ...extra, note);
    return { box, now, note };
  }

  function rangeSetting(key, name, min, max, ends, fmt, noteText) {
    const input = el('input');
    input.type = 'range';
    input.min = String(min);
    input.max = String(max);
    input.step = '1';
    input.tabIndex = -1;
    const slider = el('div', 'slider');
    const ticks = el('div', 'ticks');
    for (let v = min; v <= max; v++) ticks.append(el('i'));
    slider.append(input, ticks);
    const endsEl = el('div', 'ends');
    endsEl.append(el('span', null, ends[0]), el('span', null, ends[1]));
    const { box, now, note } = settingBox(name, slider, [endsEl]);
    note.textContent = noteText;
    function refresh() {
      const v = window.dk64settings.get(key);
      input.value = String(v);
      input.style.setProperty('--p', ((v - min) / (max - min)) * 100 + '%');
      now.textContent = fmt(v);
    }
    input.addEventListener('input', () => {
      window.dk64settings.set(key, Number(input.value));
      refresh();
    });
    return { box, refresh };
  }

  function segSetting(key, name, options, noteFn) {
    const seg = el('div', 'seg');
    const buttons = options.map(([value, text]) =>
      button(text, null, () => {
        window.dk64settings.set(key, value);
        refresh();
      }),
    );
    seg.append(...buttons);
    const { box, now, note } = settingBox(name, seg, []);
    function refresh() {
      const cur = window.dk64settings.get(key);
      const sel = key === 'msaa' && cur === 1 ? 0 : cur;
      const max = key === 'msaa' ? window.dk64settings.maxSamples() : 0;
      options.forEach(([value, text], i) => {
        buttons[i].classList.toggle('sel', value === sel);
        if (key === 'msaa') buttons[i].disabled = value > max;
        if (value === sel) now.textContent = text;
      });
      note.textContent = noteFn();
    }
    return { box, refresh };
  }

  const SETTINGS = [
    rangeSetting('drawdist', 'Draw distance', 1, 16, ['1× N64', '16×'], (v) => `${v}×`,
      'Objects & enemies spawn further out'),
    rangeSetting('scale', 'Render scale', 0, 8, ['Auto', '8×'], (v) => (v === 0 ? 'Auto' : `${v}×`),
      'Auto follows the window height'),
    segSetting('msaa', 'Anti-aliasing', [[0, 'Off'], [2, '2×'], [4, '4×'], [8, '8×']], () => {
      const m = window.dk64settings.maxSamples();
      return m >= 2 ? `MSAA; your GPU supports up to ${m}×` : 'MSAA is not supported by this GPU';
    }),
    segSetting('interp', 'Frame rate', [[0, '30 (N64)'], [1, '60'], [2, '120'], [3, '240']],
      () => 'Game logic stays at 30; higher rates add interpolated frames, up to the display\'s refresh rate'),
    segSetting('touch', 'Touch controls', [['auto', 'Auto'], ['on', 'On'], ['off', 'Off']],
      () => 'Auto: shown on touch screens without a gamepad'),
    segSetting('kongswap', 'Kong swap', [[0, 'Off'], [1, 'On']],
      () => 'D-pad ◀ ▶ (keys F / H) switches to an unlocked kong where a swap barrel could be used'),
    segSetting('audio3d', '3D sound', [[0, 'Off'], [1, 'Headphones']],
      () => 'Positional effects come from their direction, front and back included'),
    segSetting('audiorate', 'Audio quality', [[22050, '22 kHz (N64)'], [44100, '44 kHz']], () => {
      const running = window.dk64settings.audioRate();
      return running && running !== window.dk64settings.get('audiorate')
        ? `Applies when the game is reloaded (running at ${running / 1000} kHz)`
        : 'Sample rate of the sound mix';
    }),
  ];
  settingsEl.append(...SETTINGS.map((s) => s.box));

  let isStarted = false;
  let isOpen = false;
  let lastProgress = null; // port_level_progress at the last open()
  let crashMode = false;
  let crashMsg = null;

  function showMessage(reason) {
    label.textContent = 'Progress';
    cards.style.display = 'none';
    wide.style.display = 'none';
    msg.style.display = '';
    msg.replaceChildren("Progress appears while you're playing a level. ", el('span', null, `(${reason})`));
  }

  function render(data) {
    if (!data.ok) {
      showMessage(data.reason);
      return;
    }
    label.textContent = LEVELS[data.world] || 'Level ' + data.world;
    const cols = COLUMNS.filter((c) => data.kongs.some((k) => k[c.key] !== null));
    cards.replaceChildren(
      ...data.kongs.map((k, i) => {
        const kong = KONGS[i];
        const card = el('div', i === data.kong ? 'card cur' : 'card');
        card.style.setProperty('--k', kong.color);
        card.append(el('div', 'name', kong.name));
        for (const c of cols) {
          const v = k[c.key];
          // empty rows keep the rows of every card aligned
          if (v === null) {
            const row = el('div', 'row none');
            row.append(img(c.icon(kong)), el('span', 'val'));
            card.append(row);
            continue;
          }
          const row = el('div', c.done(v) ? 'row done' : 'row');
          row.title = c.label;
          row.setAttribute('aria-label', `${c.label}: ${c.text(v)}`);
          row.append(img(c.icon(kong)), el('span', 'val', c.text(v)));
          card.append(row);
        }
        return card;
      }),
    );
    const items = WIDE.filter((w) => data[w.total]).map((w) => {
      const item = el('span', data[w.key] === data[w.total] ? 'item done' : 'item');
      item.title = w.label(data[w.total]);
      item.append(img(w.icon), el('span', 'val', `${data[w.key]}/${data[w.total]}`));
      return item;
    });
    wide.replaceChildren(...items);
    cards.style.display = '';
    wide.style.display = items.length ? '' : 'none';
    msg.style.display = 'none';
  }

  // ---- report view
  const KIND_TEXT = {
    bug: {
      title: 'REPORT A BUG',
      intro:
        'Something look wrong, act weird or get stuck? Tell us what happened. The game adds a screenshot of this ' +
        'moment, your save and your location.',
      placeholder: 'What happened? What did you expect?',
    },
    idea: { title: 'SUGGEST AN IDEA', intro: 'What would make the game better?', placeholder: 'Your idea…' },
    crash: {
      title: 'UH OH!',
      intro: 'The game crashed. Sending a report gets it fixed faster.',
      placeholder: 'What were you doing when it crashed?',
    },
  };
  const LIST = [
    'Your save file and exactly where you are, so we can load your game right there',
    'Game version, settings, browser and graphics card',
    'Recent game events and errors',
  ];
  const coarse = () => matchMedia('(pointer: coarse)').matches;

  let reportKind = null;
  let report = null; // resolved dk64report report, or null
  let reportPromise = null;
  let shotBlob = null; // unmarked screenshot PNG
  let curShot = null; // the PNG Save screenshot downloads
  let gen = 0; // bumped per view change: late results of an earlier view are dropped
  let pending = 0; // report preparations in flight
  let markSeq = 0;
  let imgUrl = null;

  function setGo(ready) {
    go.disabled = !ready;
    go.textContent = ready ? 'Open GitHub' : 'Preparing…';
  }
  function begin() {
    pending++;
    setGo(false);
  }
  function end(g) {
    if (g !== gen) return;
    pending = Math.max(0, pending - 1);
    if (!pending) setGo(true);
  }

  function setImg(blob) {
    if (imgUrl) URL.revokeObjectURL(imgUrl);
    imgUrl = URL.createObjectURL(blob);
    rimg.src = imgUrl;
  }

  function setList(marked) {
    const items = marked ? [...LIST, 'What the renderer drew at the spot you marked'] : LIST;
    rlist.replaceChildren(...items.map((t) => el('li', null, t)));
  }

  // the game draws one or two frames (keys stay swallowed by the open menu): capture and probe need presented frames
  function runFrames(promiseFn) {
    window.Module.ccall('port_set_paused', null, ['number'], [0]);
    return promiseFn().finally(() => {
      if (isOpen && !crashMode) window.Module.ccall('port_set_paused', null, ['number'], [1]);
    });
  }

  // must run inside the click: clipboard writes need the user gesture
  function copyShot(pngPromise) {
    const g = gen;
    const fallback = () =>
      pngPromise.then(
        () => {
          if (g !== gen) return;
          rstatus.textContent = 'Screenshot ready: save it and drag it into the issue';
          rsave.style.display = '';
        },
        () => {},
      );
    if (!navigator.clipboard || !window.ClipboardItem) {
      fallback();
      return;
    }
    let write;
    try {
      write = navigator.clipboard.write([new ClipboardItem({ 'image/png': pngPromise })]);
    } catch (err) {
      fallback();
      return;
    }
    write.then(() => {
      if (g !== gen) return;
      rstatus.textContent = 'Screenshot copied: paste it into the issue (Ctrl+V / ⌘V)';
    }, fallback);
  }

  function saveShot() {
    if (!curShot) return;
    const d = new Date();
    const p = (n) => String(n).padStart(2, '0');
    const a = document.createElement('a');
    a.href = URL.createObjectURL(curShot);
    a.download = `dk64-screenshot-${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}` +
      `${p(d.getMinutes())}${p(d.getSeconds())}.png`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
  }

  function showReport(kind) {
    const g = ++gen;
    pending = 0;
    reportKind = kind;
    const t = KIND_TEXT[kind];
    titleEl.textContent = t.title;
    rintro.textContent = t.intro;
    rtext.placeholder = t.placeholder;
    rshot.style.display = kind === 'bug' ? '' : 'none';
    rcrash.style.display = kind === 'crash' ? '' : 'none';
    rlist.style.display = kind === 'idea' ? 'none' : '';
    rback.style.display = kind === 'crash' ? 'none' : '';
    rreload.style.display = kind === 'crash' ? '' : 'none';
    rcrash.textContent = kind === 'crash' ? String(crashMsg).slice(0, 300) : '';
    setList(false);
    mainView.style.display = 'none';
    reportView.style.display = '';
    rdone.style.display = 'none';
    rpin.style.display = 'none';
    if (!coarse()) rtext.focus();
    report = null;
    reportPromise = null;
    shotBlob = curShot = null;
    if (kind === 'idea') {
      setGo(true);
      return;
    }
    begin();
    const p = lastProgress && lastProgress.ok ? lastProgress : null;
    reportPromise = window.dk64report
      .collect({
        crash: kind === 'crash' ? crashMsg : null,
        level: p ? LEVELS[p.world] || null : null,
        kong: p && KONGS[p.kong] ? KONGS[p.kong].name : null,
        progress: lastProgress,
      })
      .catch(() => null);
    reportPromise.then((r) => {
      if (g !== gen) return;
      report = r;
      end(g);
    });
    if (kind !== 'bug') return;
    rimg.style.display = '';
    rimg.removeAttribute('src');
    rhint.style.display = '';
    rhint.textContent = `Something look wrong? ${coarse() ? 'Tap' : 'Click'} the spot on the screenshot.`;
    rstatus.textContent = '';
    rsave.style.display = 'none';
    const png = runFrames(() => window.dk64dbg.capture()).then(window.dk64report.toPng);
    copyShot(png);
    png.then(
      (b) => {
        if (g !== gen) return;
        shotBlob = curShot = b;
        setImg(b);
      },
      () => {
        if (g !== gen) return;
        rstatus.textContent = "No screenshot (the game didn't draw a frame)";
        rimg.style.display = 'none';
        rhint.style.display = 'none';
        rsave.style.display = 'none';
      },
    );
  }

  rimg.addEventListener('click', (e) => {
    if (reportKind !== 'bug' || !shotBlob || !rimg.clientWidth) return;
    const g = gen;
    const seq = ++markSeq;
    const fx = Math.min(1, Math.max(0, e.offsetX / rimg.clientWidth));
    const fy = Math.min(1, Math.max(0, e.offsetY / rimg.clientHeight));
    rpin.style.left = fx * 100 + '%';
    rpin.style.top = fy * 100 + '%';
    rpin.style.display = 'block';
    setList(true);
    // always marked from the clean screenshot, so a second click moves the mark
    const marked = window.dk64report.markPng(shotBlob, fx, fy);
    copyShot(marked);
    marked.then(
      (b) => {
        if (g !== gen || seq !== markSeq) return;
        curShot = b;
        setImg(b);
      },
      () => {},
    );
    begin();
    const probe = runFrames(() => window.dk64dbg.probe(fx, fy)).catch(() => null);
    Promise.all([reportPromise, probe])
      .then(([r, pr]) => r && window.dk64report.withMark(r, { fx, fy, probe: pr }))
      .then(
        (r) => {
          if (r && g === gen && seq === markSeq) report = r;
        },
        () => {},
      )
      .finally(() => end(g));
  });

  function openGithub() {
    const url = window.dk64report.issueUrl(reportKind, rtext.value.trim(), report);
    const w = window.open(url, '_blank');
    if (w) w.opener = null;
    rdoneLink.href = url;
    rdone.style.display = '';
    rtext.value = '';
  }

  function showMain() {
    gen++;
    pending = 0;
    reportView.style.display = 'none';
    mainView.style.display = '';
    titleEl.textContent = 'PAUSED';
    if (document.activeElement === rtext) rtext.blur();
  }

  // report.js: the game trapped or aborted. Not closable; needs no started game or pause.
  function openCrash(msg) {
    crashMode = true;
    crashMsg = msg;
    lastProgress = null;
    if (window.dk64touch) window.dk64touch.releaseAll();
    backdrop.classList.add('on');
    isOpen = true;
    showReport('crash');
  }

  function open() {
    if (!isStarted || isOpen || crashMode) return;
    const m = window.Module;
    m.ccall('port_set_paused', null, ['number'], [1]);
    if (window.dk64touch) window.dk64touch.releaseAll();
    lastProgress = null;
    try {
      // one snapshot per open: the game is frozen until close()
      lastProgress = JSON.parse(m.ccall('port_level_progress', 'string'));
      render(lastProgress);
    } catch (err) {
      showMessage(err.message);
    }
    for (const s of SETTINGS) s.refresh();
    const shown = !!(window.dk64saves && window.dk64saves.dashboard());
    exitBtn.style.display = shown ? '' : 'none';
    resumeBtn.style.gridColumn = shown ? '' : '1 / -1'; // gate off: Resume alone spans the bottom row
    exitForce = false;
    exitBtn.textContent = 'Exit to dashboard';
    exitBtn.disabled = false;
    exitNote.style.display = 'none';
    backdrop.classList.add('on');
    isOpen = true;
  }

  function close() {
    if (!isOpen || crashMode || exiting) return;
    showMain();
    backdrop.classList.remove('on');
    window.Module.ccall('port_set_paused', null, ['number'], [0]);
    isOpen = false;
    document.getElementById('canvas').focus();
  }

  function sleep(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
  }

  // the game saves itself (pause menu Save) while unpaused behind the open menu; resolves to a problem or null
  async function inGameSave(m) {
    m.ccall('port_save_request', null, ['number'], [1]);
    m.ccall('port_set_paused', null, ['number'], [0]);
    const deadline = performance.now() + 8000;
    let busy = true;
    while (busy && performance.now() < deadline) {
      await sleep(50);
      busy = m.ccall('port_save_busy', 'number') !== 0;
    }
    m.ccall('port_set_paused', null, ['number'], [1]);
    if (!busy) return null;
    m.ccall('port_save_request', null, ['number'], [0]);
    return "The game couldn't save right now. Try again in a moment, or exit with your last save.";
  }

  async function exitToDashboard() {
    if (exiting) return;
    exiting = true;
    exitBtn.disabled = true;
    exitNote.textContent = 'Saving…';
    exitNote.style.display = '';
    const m = window.Module;
    let problem = null;
    let resume = { ok: false };
    try {
      resume = JSON.parse(m.ccall('port_resume_capture', 'string')); // where the player stands, while frozen
      if (resume.ok && !exitForce) problem = await inGameSave(m);
    } catch (err) {
      problem = "The game couldn't save (" + err.message + ').';
    }
    if (!problem) {
      try {
        m.ccall('port_save_flush_now', null);
        const ok = await window.dk64saves.settle();
        await window.dk64saves.putResume(resume.ok ? resume : null);
        if (!ok) {
          problem =
            "The save server didn't take the upload: your progress stays in this browser and uploads the next time you play this save.";
        }
      } catch (err) {
        problem = "Couldn't reach the save server (" + err.message + ').';
      }
    }
    if (problem && !exitForce) {
      exitNote.textContent = problem;
      exitBtn.textContent = 'Exit anyway';
      exitBtn.disabled = false;
      exitForce = true;
      exiting = false;
      return;
    }
    location.assign('/');
  }

  function toggle() {
    if (isOpen) close();
    else open();
  }

  // capture phase: runs before SDL's keydown listener. Key-ups are left alone so SDL sees every release and no
  // key held at open stays stuck; swallowing key-downs keeps keys pressed in the menu from reaching the game.
  window.addEventListener(
    'keydown',
    (e) => {
      if (isOpen) e.stopPropagation(); // no preventDefault: browser shortcuts and typing in the report still work
      if (e.key !== 'Escape' || !isStarted || crashMode) return;
      e.preventDefault();
      e.stopPropagation();
      if (!e.repeat) toggle();
    },
    true,
  );
  // SDL's window keypress listener calls preventDefault, which would keep characters out of the report textarea
  window.addEventListener(
    'keypress',
    (e) => {
      if (isOpen) e.stopPropagation();
    },
    true,
  );

  // gamepad Back/Select (Standard Gamepad button 8, unmapped by port/src/input.c): toggle on press
  let prevBack = false;
  function pollPad() {
    let gp = null;
    try {
      if (typeof navigator.getGamepads === 'function') {
        gp = Array.from(navigator.getGamepads()).find((g) => g && g.connected) || null;
      }
    } catch (e) {
      gp = null; // insecure origins: getGamepads missing or throwing
    }
    const back = !!(gp && gp.buttons[8] && gp.buttons[8].pressed);
    if (back && !prevBack) toggle();
    prevBack = back;
    requestAnimationFrame(pollPad);
  }

  function started() {
    if (isStarted) return;
    isStarted = true;
    requestAnimationFrame(pollPad);
  }

  window.dk64menu = {
    started,
    toggle,
    open,
    close,
    isOpen: () => isOpen,
    openCrash,
  };
})();
