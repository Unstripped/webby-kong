import { cleanup, fireEvent, render } from '@testing-library/preact';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { setModule, type Dk64Module } from '../port';
import { loadSettings, set } from '../settings';
import { started } from './pad';
import { TouchOverlay } from './TouchOverlay';

vi.mock('../menu/control', () => ({ toggle: vi.fn() }));
vi.mock('nipplejs', () => ({
  default: { create: () => ({ on: vi.fn(), reposition: vi.fn(), destroy: vi.fn() }) },
}));

let ccall: ReturnType<typeof vi.fn>;
beforeEach(() => {
  HTMLElement.prototype.setPointerCapture = vi.fn();
  ccall = vi.fn();
  setModule({ ccall } as unknown as Dk64Module);
  loadSettings();
  set('touch', 'on');
  started();
});
afterEach(() => {
  cleanup();
  setModule(null);
});

it('sends the A button bits on press and release', () => {
  const { getByText } = render(<TouchOverlay />);
  const a = getByText('A');
  fireEvent.pointerDown(a, { pointerId: 1 });
  expect(ccall).toHaveBeenLastCalledWith('port_touch_pad', null, ['number', 'number', 'number'], [0x8000, 0, 0]);
  fireEvent.pointerUp(a, { pointerId: 1 });
  expect(ccall).toHaveBeenLastCalledWith('port_touch_pad', null, ['number', 'number', 'number'], [0, 0, 0]);
});
