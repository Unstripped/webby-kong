import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { setModule, type Dk64Module } from './port';
import * as settings from './settings';

describe('settings', () => {
  let ccall: ReturnType<typeof vi.fn>;

  beforeEach(() => {
    localStorage.setItem('dk64.settings', JSON.stringify({ drawdist: 8, msaa: 3 }));
    history.replaceState(null, '', '/?drawdist=2&touch=1');
    ccall = vi.fn(() => 0);
    setModule({ ccall } as unknown as Dk64Module);
    settings.loadSettings();
  });

  afterEach(() => {
    setModule(null);
    localStorage.clear();
    history.replaceState(null, '', '/');
  });

  it('layers defaults, valid stored values and URL overrides', () => {
    expect(settings.get('drawdist')).toBe(2);
    expect(settings.get('msaa')).toBe(4); // stored 3 is invalid: the default
    expect(settings.get('touch')).toBe('on');
    expect(settings.args()).toEqual([
      '--drawdist', '2', '--scale', '0', '--msaa', '4', '--interp', '1', '--hrtf', '1', '--audio-rate', '44100',
      '--kong-swap', '1', '--disco-chunky', '0', '--stick-camera', '1', '--camera-speed', '5', '--camera-invert', '0',
      '--per-pixel', '0',
    ]);
  });

  it('rejects invalid values and persists only the changed key', () => {
    expect(settings.set('drawdist', 99)).toBe(2);
    expect(JSON.parse(localStorage.getItem('dk64.settings')!)).toEqual({ drawdist: 8, msaa: 3 });
    settings.set('scale', 3);
    expect(JSON.parse(localStorage.getItem('dk64.settings')!)).toEqual({ drawdist: 8, msaa: 3, scale: 3 });
  });

  it('applies to the game only once started', () => {
    settings.set('msaa', 2);
    expect(ccall).not.toHaveBeenCalled();
    settings.started();
    settings.set('msaa', 8);
    expect(ccall).toHaveBeenCalledWith('port_set_msaa', 'number', ['number'], [8]);
  });
});
