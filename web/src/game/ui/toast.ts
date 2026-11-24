import { signal } from '@preact/signals';

// The game page's toast (#dk64dbg-toast, rendered by <Toast/>): the text stays while it fades out.
export const toastText = signal('');
export const toastShown = signal(false);

let timer: ReturnType<typeof setTimeout> | undefined;

export function toast(msg: string): void {
  toastText.value = msg;
  toastShown.value = true;
  clearTimeout(timer);
  timer = setTimeout(() => (toastShown.value = false), 1800);
}
