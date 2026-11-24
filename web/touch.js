// On-screen N64 controller for touch devices: floating stick (nipplejs) + buttons -> port_touch_pad. Auto/On/Off from web/settings.js (Esc menu or ?touch=1/0).
(function () {
  'use strict';

  // bits = N64 button masks (decomp/include/PR/os_cont.h CONT_*); pos = fixed-position CSS
  const BUTTONS = [
    { label: 'A', bits: 0x8000, cls: 'a', pos: 'right:6vmin;bottom:8vmin;width:16vmin;height:16vmin' },
    { label: 'B', bits: 0x4000, cls: 'b', pos: 'right:26vmin;bottom:16vmin;width:13vmin;height:13vmin' },
    { label: 'C▲', bits: 0x0008, cls: 'c', pos: 'right:20vmin;bottom:52vmin;width:9vmin;height:9vmin' },
    { label: 'C▼', bits: 0x0004, cls: 'c', pos: 'right:20vmin;bottom:34vmin;width:9vmin;height:9vmin' },
    { label: 'C◀', bits: 0x0002, cls: 'c', pos: 'right:29vmin;bottom:43vmin;width:9vmin;height:9vmin' },
    { label: 'C▶', bits: 0x0001, cls: 'c', pos: 'right:11vmin;bottom:43vmin;width:9vmin;height:9vmin' },
    { label: 'R', bits: 0x0010, cls: 'sh', pos: 'right:6vmin;top:14vmin;width:18vmin;height:9vmin' },
    { label: 'L', bits: 0x0020, cls: 'sh', pos: 'left:6vmin;top:14vmin;width:18vmin;height:9vmin' },
    { label: 'Z', bits: 0x2000, cls: 'sh', pos: 'left:6vmin;bottom:58vmin;width:14vmin;height:9vmin' },
    { label: 'START', bits: 0x1000, cls: 'st', pos: 'left:50%;bottom:4vmin;width:14vmin;height:7vmin;transform:translateX(-50%)' },
    // D-pad Right: next unlocked kong (patches/0027-port-dpad-kong-swap.patch); hidden while the Kong swap setting is Off
    { label: '⇄', bits: 0x0100, cls: 'swap', pos: 'left:6vmin;top:3vmin;width:14vmin;height:8vmin' },
  ];
  const STICK_SIZE = 120; // nipplejs outer circle, px
  const STICK_DEADZONE = 0.1; // radial, of full deflection

  const style = document.createElement('style');
  style.textContent = `
    .dk64t-root { position: fixed; inset: 0; z-index: 2; pointer-events: none; display: none;
      user-select: none; -webkit-user-select: none; -webkit-touch-callout: none; }
    .dk64t-root.on { display: block; }
    .dk64t-root.menuonly { display: block; }
    .dk64t-root.menuonly:not(.on) > :not(.dk64t-menu) { display: none; }
    .dk64t-zone { position: fixed; left: 0; top: 0; width: 50vw; height: 100vh; pointer-events: auto; touch-action: none; z-index: 2; }
    .dk64t-btn { position: fixed; z-index: 3; pointer-events: auto; touch-action: none; display: flex; align-items: center;
      justify-content: center; border-radius: 50%; border: 2px solid rgba(255,255,255,.45); background: rgba(255,255,255,.14);
      color: rgba(255,255,255,.85); font: 600 3.2vmin/1 system-ui, sans-serif; box-sizing: border-box; }
    .dk64t-sh, .dk64t-st, .dk64t-swap { border-radius: 2vmin; }
    .dk64t-root.noswap .dk64t-swap { display: none; }
    .dk64t-a { background: rgba(60,110,255,.35); }
    .dk64t-b { background: rgba(40,180,80,.35); }
    .dk64t-c { background: rgba(240,200,40,.30); }
    .dk64t-btn.down { background: rgba(255,255,255,.45); }
  `;
  document.head.appendChild(style);

  const root = document.createElement('div');
  root.className = 'dk64t-root';
  const zone = document.createElement('div');
  zone.className = 'dk64t-zone';
  root.appendChild(zone);

  let held = 0, sx = 0, sy = 0, sent = '';
  let manager = null;

  function send() {
    const key = held + ',' + sx + ',' + sy;
    if (key === sent) return;
    const m = window.Module;
    if (!m || !m.ccall) return; // game not started; resend once it is
    m.ccall('port_touch_pad', null, ['number', 'number', 'number'], [held, sx, sy]);
    sent = key;
  }

  const buttons = BUTTONS.map((def) => {
    const el = document.createElement('div');
    el.className = 'dk64t-btn dk64t-' + def.cls;
    el.style.cssText = def.pos;
    el.textContent = def.label;
    el._bits = def.bits;
    el._ptrs = new Set();
    el.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      el.setPointerCapture(e.pointerId);
      el._ptrs.add(e.pointerId);
      recompute();
    });
    const up = (e) => {
      el._ptrs.delete(e.pointerId);
      recompute();
    };
    el.addEventListener('pointerup', up);
    el.addEventListener('pointercancel', up);
    el.addEventListener('lostpointercapture', up);
    root.appendChild(el);
    return el;
  });

  function recompute() {
    held = 0;
    for (const el of buttons) {
      const down = el._ptrs.size > 0;
      if (down) held |= el._bits;
      el.classList.toggle('down', down);
    }
    send();
  }

  // nipplejs vector (unit radius, y up) -> N64 stick units; circle of radius 80 past a radial deadzone
  function stickVec(vx, vy) {
    const m = Math.min(Math.hypot(vx, vy), 1);
    if (m < STICK_DEADZONE) return [0, 0];
    const s = (m - STICK_DEADZONE) / (1 - STICK_DEADZONE) / m;
    return [Math.round(vx * s * 80), Math.round(vy * s * 80)];
  }

  function releaseAll() {
    for (const el of buttons) {
      el._ptrs.clear();
      el.classList.remove('down');
    }
    held = 0;
    sx = 0;
    sy = 0;
    send();
  }

  // web/menu.js toggle; not a pad button, so it sends no bits
  const menuBtn = document.createElement('div');
  menuBtn.className = 'dk64t-btn dk64t-sh dk64t-menu';
  menuBtn.style.cssText = 'right:6vmin;top:3vmin;width:9vmin;height:8vmin';
  menuBtn.textContent = '☰';
  menuBtn.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    if (window.dk64menu) window.dk64menu.toggle();
  });
  root.appendChild(menuBtn);

  document.body.appendChild(root);

  let mode = window.dk64settings.get('touch');
  let isStarted = false;

  function padConnected() {
    try {
      return typeof navigator.getGamepads === 'function' && Array.from(navigator.getGamepads()).some((g) => g && g.connected);
    } catch (e) {
      return false; // insecure origins: getGamepads missing or throwing
    }
  }

  function update() {
    const pad = padConnected();
    const coarse = window.matchMedia('(pointer: coarse)').matches;
    const show = isStarted && !pad && (mode === 'on' || (mode === 'auto' && coarse));
    // Off on a touch screen keeps the ☰ button, or the menu (and this setting) would be unreachable without a keyboard
    root.classList.toggle('menuonly', isStarted && !pad && !show && coarse);
    if (show === root.classList.contains('on')) return;
    if (show) {
      root.classList.add('on');
      if (!manager) {
        // created on first show so the zone has a real layout box
        manager = nipplejs.create({ zone, mode: 'dynamic', size: STICK_SIZE, color: 'white', restOpacity: 0.5 });
        manager.on('move', (evt) => {
          [sx, sy] = stickVec(evt.data.vector.x, evt.data.vector.y);
          send();
        });
        manager.on('end', () => {
          sx = 0;
          sy = 0;
          send();
        });
      } else {
        manager.reposition();
      }
    } else {
      root.classList.remove('on');
      releaseAll();
    }
  }

  function started() {
    isStarted = true;
    update();
  }

  function setMode(m) {
    mode = m;
    update();
  }

  function setKongSwap(v) {
    root.classList.toggle('noswap', !v);
    if (!v) releaseAll();
  }
  setKongSwap(window.dk64settings.get('kongswap'));

  window.addEventListener('gamepadconnected', update);
  window.addEventListener('gamepaddisconnected', update);
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) releaseAll();
  });
  window.addEventListener('blur', releaseAll);

  window.dk64touch = { started, releaseAll, setMode, setKongSwap };
})();
