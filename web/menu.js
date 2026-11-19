// Esc menu: freezes the game (port_set_paused) and shows the current level's collectables (port_level_progress).
// Opened by Esc, gamepad Back/Select (Standard mapping button 8) or the touch overlay's ☰ button.
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
  const KONGS = ['DK', 'Diddy', 'Lanky', 'Tiny', 'Chunky'];
  const BLUEPRINT = ['–', 'Found', 'Turned in'];
  // field of each kong entry, header, cell text, complete
  const COLUMNS = [
    { key: 'gb', label: 'Golden Bananas', text: (v) => `${v}/5`, done: (v) => v === 5 },
    { key: 'bananas', label: 'Bananas', text: (v) => `${v}/100`, done: (v) => v === 100 },
    { key: 'medal', label: 'Medal', text: (v) => (v ? '✓' : '–'), done: (v) => v === 1 },
    { key: 'blueprint', label: 'Blueprint', text: (v) => BLUEPRINT[v] || String(v), done: (v) => v === 2 },
  ];

  const style = document.createElement('style');
  style.textContent = `
#dk64menu {
  position: fixed; inset: 0; z-index: 10; background: rgba(0,0,0,.55); display: none;
  align-items: center; justify-content: center; pointer-events: auto;
}
#dk64menu.on { display: flex; }
#dk64menu .panel {
  width: min(560px, 92vw); box-sizing: border-box; padding: 14px 16px 16px; border-radius: 10px;
  border: 1px solid rgba(255,255,255,.09); background: rgba(16,18,24,.84);
  backdrop-filter: blur(8px); -webkit-backdrop-filter: blur(8px); box-shadow: 0 8px 28px rgba(0,0,0,.45);
  color: #d7dce5; font: 13px/1.4 system-ui, -apple-system, sans-serif; user-select: none;
}
#dk64menu .hd { display: flex; align-items: baseline; gap: 8px; margin-bottom: 12px; }
#dk64menu .title { font-weight: 600; font-size: 16px; letter-spacing: .02em; color: #f2f4f8; }
#dk64menu .hint { margin-left: auto; color: #7d8594; font-size: 11px; }
#dk64menu .lbl { color: #7d8594; font-size: 11px; text-transform: uppercase; letter-spacing: .06em; margin-bottom: 6px; }
#dk64menu table { width: 100%; border-collapse: collapse; }
#dk64menu th { color: #7d8594; font-weight: 500; font-size: 11px; text-align: left; padding: 4px 8px;
  border-bottom: 1px solid rgba(255,255,255,.07); }
#dk64menu td { padding: 5px 8px; font-family: ui-monospace, monospace; color: #e8ecf2; }
#dk64menu td:first-child { font-family: system-ui, -apple-system, sans-serif; font-weight: 500; }
#dk64menu tr.cur td { background: rgba(90,160,255,.18); }
#dk64menu td.done { color: #6fe09a; }
#dk64menu .wide { margin-top: 10px; color: #c9cfda; }
#dk64menu .msg { color: #c9cfda; margin: 4px 0; }
#dk64menu .msg span { color: #7d8594; }
#dk64menu .actions { display: flex; justify-content: flex-end; margin-top: 14px; }
#dk64menu button {
  font: 500 13px/1 system-ui, -apple-system, sans-serif; cursor: pointer; padding: 8px 16px; border-radius: 7px;
  background: rgba(90,160,255,.25); border: 1px solid rgba(110,170,255,.6); color: #e4efff;
}
#dk64menu button:hover { background: rgba(90,160,255,.38); }
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

  const backdrop = el('div');
  backdrop.id = 'dk64menu';
  const panel = el('div', 'panel');
  const hd = el('div', 'hd');
  hd.append(el('span', 'title', 'Paused'), el('span', 'hint', 'Esc · Back to resume'));
  const label = el('div', 'lbl');
  const table = el('table');
  const wide = el('div', 'wide');
  const msg = el('p', 'msg');
  const actions = el('div', 'actions');
  actions.append(button('Resume', null, () => close()));
  panel.append(hd, label, table, wide, msg, actions);
  backdrop.append(panel);
  backdrop.addEventListener('click', (e) => {
    if (e.target === backdrop) close();
  });
  document.body.appendChild(backdrop);

  let isStarted = false;
  let isOpen = false;

  function showMessage(reason) {
    label.textContent = 'Progress';
    table.style.display = 'none';
    wide.style.display = 'none';
    msg.style.display = '';
    msg.replaceChildren("Progress appears while you're playing a level. ", el('span', null, `(${reason})`));
  }

  function render(data) {
    if (!data.ok) {
      showMessage(data.reason);
      return;
    }
    label.textContent = `Progress — ${LEVELS[data.world] || 'level ' + data.world}`;
    const cols = COLUMNS.filter((c) => data.kongs.some((k) => k[c.key] !== null));
    const head = el('tr');
    head.append(el('th', null, 'Kong'), ...cols.map((c) => el('th', null, c.label)));
    const rows = data.kongs.map((k, i) => {
      const tr = el('tr', i === data.kong ? 'cur' : null);
      tr.append(el('td', null, KONGS[i]));
      for (const c of cols) {
        const v = k[c.key];
        tr.append(v === null ? el('td', null, '') : el('td', c.done(v) ? 'done' : null, c.text(v)));
      }
      return tr;
    });
    table.replaceChildren(head, ...rows);
    const items = [];
    if (data.fairiesTotal) items.push(`Fairies ${data.fairies}/${data.fairiesTotal}`);
    if (data.crownsTotal) {
      items.push(`${data.crownsTotal === 1 ? 'Battle crown' : 'Battle crowns'} ${data.crowns}/${data.crownsTotal}`);
    }
    if (data.keysTotal) items.push(`Boss key ${data.keys}/${data.keysTotal}`);
    wide.textContent = items.join(' · ');
    table.style.display = '';
    wide.style.display = items.length ? '' : 'none';
    msg.style.display = 'none';
  }

  function open() {
    if (!isStarted || isOpen) return;
    const m = window.Module;
    m.ccall('port_set_paused', null, ['number'], [1]);
    if (window.dk64touch) window.dk64touch.releaseAll();
    try {
      // one snapshot per open: the game is frozen until close()
      render(JSON.parse(m.ccall('port_level_progress', 'string')));
    } catch (err) {
      showMessage(err.message);
    }
    backdrop.classList.add('on');
    isOpen = true;
  }

  function close() {
    if (!isOpen) return;
    backdrop.classList.remove('on');
    window.Module.ccall('port_set_paused', null, ['number'], [0]);
    isOpen = false;
    document.getElementById('canvas').focus();
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
      if (!isStarted) return;
      if (e.key === 'Escape') {
        e.preventDefault();
        e.stopPropagation();
        if (!e.repeat) toggle();
        return;
      }
      if (isOpen) e.stopPropagation(); // no preventDefault: browser shortcuts still work
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
  };
})();
