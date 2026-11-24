// On-screen N64 controller for touch devices (the UI half of the old touch.js): floating stick
// (nipplejs) + buttons -> port_touch_pad. Auto/On/Off from the touch setting.
import nipplejs from 'nipplejs';
import { useEffect, useReducer, useRef } from 'preact/hooks';
import { toggle } from '../menu/control';
import { held, kongSwap, press, release, releaseAll, setStick, stickVec, touchMode, touchStarted } from './pad';
import './touch.css';

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

function padConnected(): boolean {
  try {
    return typeof navigator.getGamepads === 'function' && Array.from(navigator.getGamepads()).some((g) => g && g.connected);
  } catch {
    return false; // insecure origins: getGamepads missing or throwing
  }
}

export function TouchOverlay() {
  const [, tick] = useReducer((n: number) => n + 1, 0);
  const zoneRef = useRef<HTMLDivElement>(null);
  const manager = useRef<ReturnType<typeof nipplejs.create> | null>(null);

  const isStarted = touchStarted.value;
  const mode = touchMode.value;
  const pad = padConnected();
  const coarse = window.matchMedia('(pointer: coarse)').matches;
  const show = isStarted && !pad && (mode === 'on' || (mode === 'auto' && coarse));
  // Off on a touch screen keeps the ☰ button, or the menu (and this setting) would be unreachable without a keyboard
  const menuonly = isStarted && !pad && !show && coarse;

  useEffect(() => {
    if (!show) {
      releaseAll();
      return;
    }
    if (!manager.current) {
      // created on first show so the zone has a real layout box
      const m = nipplejs.create({ zone: zoneRef.current!, mode: 'dynamic', size: STICK_SIZE, color: 'white', restOpacity: 0.5 });
      m.on('move', (evt) => {
        const [x, y] = stickVec(evt.data.vector.x, evt.data.vector.y);
        setStick(x, y);
      });
      m.on('end', () => setStick(0, 0));
      manager.current = m;
    } else {
      manager.current.reposition();
    }
  }, [show]);

  useEffect(() => {
    const onVisibility = () => {
      if (document.hidden) releaseAll();
    };
    window.addEventListener('gamepadconnected', tick);
    window.addEventListener('gamepaddisconnected', tick);
    document.addEventListener('visibilitychange', onVisibility);
    window.addEventListener('blur', releaseAll);
    return () => {
      window.removeEventListener('gamepadconnected', tick);
      window.removeEventListener('gamepaddisconnected', tick);
      document.removeEventListener('visibilitychange', onVisibility);
      window.removeEventListener('blur', releaseAll);
      manager.current?.destroy();
      manager.current = null;
    };
  }, []);

  const bits = held.value;
  let cls = 'dk64t-root';
  if (show) cls += ' on';
  if (menuonly) cls += ' menuonly';
  if (!kongSwap.value) cls += ' noswap';
  return (
    <div class={cls}>
      <div class="dk64t-zone" ref={zoneRef} />
      {BUTTONS.map((def) => {
        const up = (e: PointerEvent) => release(def.bits, e.pointerId);
        return (
          <div
            key={def.bits}
            class={`dk64t-btn dk64t-${def.cls}${(bits & def.bits) !== 0 ? ' down' : ''}`}
            style={def.pos}
            onPointerDown={(e) => {
              e.preventDefault();
              e.currentTarget.setPointerCapture(e.pointerId);
              press(def.bits, e.pointerId);
            }}
            onPointerUp={up}
            onPointerCancel={up}
            onLostPointerCapture={up}
          >
            {def.label}
          </div>
        );
      })}
      {/* web/menu toggle: not a pad button, so it sends no bits */}
      <div
        class="dk64t-btn dk64t-sh dk64t-menu"
        style="right:6vmin;top:3vmin;width:9vmin;height:8vmin"
        onPointerDown={(e) => {
          e.preventDefault();
          toggle();
        }}
      >
        ☰
      </div>
    </div>
  );
}
