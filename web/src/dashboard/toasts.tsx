import { signal } from '@preact/signals';

export interface ToastItem {
  id: number;
  msg: string;
  err: boolean;
}

const toasts = signal<ToastItem[]>([]);
let seq = 0;

// Shows msg in #toasts for 3.5 s, or 6 s for errors. There is no limit on how many stack.
export function toast(msg: string, isErr = false): void {
  const t: ToastItem = { id: ++seq, msg, err: isErr };
  toasts.value = [...toasts.value, t];
  setTimeout(() => {
    toasts.value = toasts.value.filter((x) => x !== t);
  }, isErr ? 6000 : 3500);
}

export function Toasts() {
  return (
    <div id="toasts" class="toasts" role="status" aria-live="polite">
      {toasts.value.map((t) => (
        <div key={t.id} class={'toast' + (t.err ? ' err' : '')}>
          {t.msg}
        </div>
      ))}
    </div>
  );
}
