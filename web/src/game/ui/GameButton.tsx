import type { ButtonHTMLAttributes } from 'preact';

// Buttons never take focus: keys keep going to the game (Enter on a focused button would re-click it).
export function GameButton(props: Omit<ButtonHTMLAttributes<HTMLButtonElement>, 'type' | 'tabIndex' | 'onMouseDown'>) {
  return <button {...props} type="button" tabIndex={-1} onMouseDown={(e) => e.preventDefault()} />;
}
