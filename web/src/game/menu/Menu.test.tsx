import { act, cleanup, fireEvent, render } from '@testing-library/preact';
import { afterEach, beforeAll, beforeEach, expect, it, vi } from 'vitest';
import { setModule, type Dk64Module } from '../port';
import { crash } from '../report';
import * as settings from '../settings';
import { close, installMenuControl, isOpen, open, started, toggle } from './control';
import { Menu } from './Menu';

const PROGRESS = JSON.stringify({
  ok: true,
  world: 0,
  kong: 1,
  kongs: [0, 1, 2, 3, 4].map(() => ({ gb: 2, bananas: 40, medal: 0, blueprint: 1 })),
  fairies: 1,
  fairiesTotal: 2,
  crowns: 0,
  crownsTotal: 1,
  keys: 0,
  keysTotal: 0,
});

let ccall: ReturnType<typeof vi.fn>;

beforeAll(() => {
  installMenuControl();
});

beforeEach(() => {
  ccall = vi.fn((name: string) => {
    if (name === 'port_level_progress') return PROGRESS;
    if (name === 'port_resume_capture') return '{"ok":false,"reason":"test"}';
    return 0;
  });
  setModule({ ccall } as unknown as Dk64Module);
  localStorage.clear();
  settings.loadSettings();
  settings.started();
  started();
});

afterEach(() => {
  act(() => close());
  cleanup();
  setModule(null);
});

function pausedCalls(v: number) {
  return ccall.mock.calls.filter((c) => c[0] === 'port_set_paused' && (c[3] as number[])[0] === v);
}

it('toggle() pauses and shows the menu', () => {
  const { container } = render(<Menu />);
  act(() => toggle());
  expect(ccall).toHaveBeenCalledWith('port_set_paused', null, ['number'], [1]);
  expect(container.querySelector('#dk64menu')!.classList.contains('on')).toBe(true);
  expect(container.querySelector('.lbl')!.textContent).toBe('Jungle Japes');
});

it('capture-phase Escape on window closes the menu and unpauses', () => {
  const { container } = render(<Menu />);
  act(() => open());
  expect(isOpen.value).toBe(true);
  ccall.mockClear();
  act(() => {
    window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
  });
  expect(isOpen.value).toBe(false);
  expect(pausedCalls(0)).toHaveLength(1);
  expect(container.querySelector('#dk64menu')!.classList.contains('on')).toBe(false);
});

it('the draw distance slider sets port_set_drawdist', () => {
  const { container } = render(<Menu />);
  act(() => open());
  const input = container.querySelector<HTMLInputElement>('input[type=range]')!;
  act(() => {
    input.value = '8';
    fireEvent.input(input);
  });
  expect(ccall).toHaveBeenCalledWith('port_set_drawdist', 'number', ['number'], [8]);
  expect(settings.get('drawdist')).toBe(8);
});

// last: crash mode cannot be left
it('a crash opens the UH OH! view, which Escape does not close', async () => {
  vi.useFakeTimers();
  try {
    const { container } = render(<Menu />);
    act(() => {
      crash.value = { message: 'boom', stack: [] };
    });
    await act(async () => {
      await vi.runOnlyPendingTimersAsync();
    });
    expect(container.querySelector('.title')!.textContent).toBe('UH OH!');
    expect(container.querySelector('.rcrash')!.textContent).toBe('boom');
    expect(container.querySelector('#dk64menu')!.classList.contains('on')).toBe(true);
    act(() => {
      window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
    });
    expect(isOpen.value).toBe(true);
    expect(container.querySelector('.title')!.textContent).toBe('UH OH!');
    expect(pausedCalls(0)).toHaveLength(0);
  } finally {
    vi.useRealTimers();
  }
});
