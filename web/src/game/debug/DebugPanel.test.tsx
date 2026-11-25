import { act, cleanup, fireEvent, render } from '@testing-library/preact';
import { afterEach, expect, it, vi } from 'vitest';
import { setModule, type Dk64Module } from '../port';
import { overlay } from './api';
import { DebugPanel } from './DebugPanel';

const STATS = '{"map":7,"mode":6,"overlay":0,"vi":120,"frames":60,"actors":3,"player":null}';

function fakeModule() {
  let mask = 3;
  const ccall = vi.fn((name: string, _ret: unknown, _types?: unknown, args?: number[]) => {
    if (name === 'port_debug_stats') return STATS;
    if (name === 'port_debug_trace') return '{"next":0,"events":[]}';
    if (name === 'port_debug_audio') return '{"seq":[42,-1,-1,-1],"sfx":[291]}';
    if (name === 'port_debug_trace_mask') {
      const v = args?.[0] ?? -1;
      if (v !== -1) mask = v;
      return mask;
    }
    return 0;
  });
  setModule({ ccall } as unknown as Dk64Module);
  return ccall;
}

afterEach(() => {
  act(() => {
    overlay(false);
  });
  cleanup();
  setModule(null);
});

it('renders state and toggles a trace chip', () => {
  const ccall = fakeModule();
  const { container, getByText } = render(<DebugPanel />);
  act(() => {
    overlay(true);
  });
  expect(container.textContent).toContain('0x07 (7)');
  expect(container.textContent).toContain('seq 0x2a – – – · sfx 0x123');
  fireEvent.click(getByText('Camera'));
  expect(ccall).toHaveBeenCalledWith('port_debug_trace_mask', 'number', ['number'], [1]);
  for (const b of container.querySelectorAll('button')) expect(b.getAttribute('tabindex')).toBe('-1');
});

it('shows the error without a module', () => {
  const { container } = render(<DebugPanel />);
  act(() => {
    overlay(true);
  });
  expect(container.textContent).toContain('dk64dbg: game not started');
});
