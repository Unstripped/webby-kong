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
#dk64menu .hint { font: 600 11px system-ui, sans-serif; color: rgba(255,243,214,.75); }
#dk64menu button { font: 900 15px/1 ${CHUNKY}; letter-spacing: .04em; text-transform: uppercase; cursor: pointer;
  padding: 10px 22px; border-radius: 12px; color: #3a1d00; background: linear-gradient(#ffe36a, #f5b400);
  border: 3px solid #3a1d00; box-shadow: 0 4px 0 #3a1d00; }
#dk64menu button:hover { background: linear-gradient(#fff08f, #ffc21a); }
#dk64menu button:active { transform: translateY(3px); box-shadow: 0 1px 0 #3a1d00; }
@media (max-width: 520px) { #dk64menu .title { font-size: 24px; } }
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
  const actions = el('div', 'actions');
  actions.append(el('span', 'hint', 'Esc · Back to resume'), button('Resume', null, () => close()));
  panel.append(
    ...['tl', 'tr', 'bl', 'br'].map((c) => el('span', 'bolt ' + c)),
    el('div', 'title', 'PAUSED'),
    label,
    cards,
    wide,
    msg,
    actions,
  );
  backdrop.append(panel);
  backdrop.addEventListener('click', (e) => {
    if (e.target === backdrop) close();
  });
  document.body.appendChild(backdrop);

  let isStarted = false;
  let isOpen = false;

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
