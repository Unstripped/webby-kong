import { signal } from '@preact/signals';
import { useEffect, useRef } from 'preact/hooks';

type DialogReq =
  | { id: number; kind: 'confirm'; text: string; label: string; done: (v: boolean) => void }
  | { id: number; kind: 'prompt'; text: string; value: string; label: string; done: (v: string | null) => void };

// Open dialogs, oldest first. Like the original's body-appended modals, several may stack.
const open = signal<DialogReq[]>([]);
let seq = 0;

function show<T>(make: (id: number, done: (v: T) => void) => DialogReq): Promise<T> {
  return new Promise((resolve) => {
    const id = ++seq;
    let settled = false;
    open.value = [
      ...open.value,
      make(id, (v) => {
        if (settled) return;
        settled = true;
        open.value = open.value.filter((d) => d.id !== id);
        resolve(v);
      }),
    ];
  });
}

// Resolves true for the danger button, false for Cancel, a backdrop click or Escape.
export function confirmDialog(text: string, label?: string): Promise<boolean> {
  return show<boolean>((id, done) => ({ id, kind: 'confirm', text, label: label || 'Confirm', done }));
}

// text prompt modelled on confirmDialog: resolves the trimmed text, or null when cancelled
export function promptDialog(text: string, value?: string, label?: string): Promise<string | null> {
  return show<string | null>((id, done) => ({ id, kind: 'prompt', text, value: value || '', label: label || 'OK', done }));
}

function Dialog({ d }: { d: DialogReq }) {
  const cancelRef = useRef<HTMLButtonElement>(null);
  const inputRef = useRef<HTMLInputElement>(null);
  const cancel = () => (d.kind === 'confirm' ? d.done(false) : d.done(null));

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') cancel();
    };
    document.addEventListener('keydown', onKey);
    if (d.kind === 'confirm') cancelRef.current!.focus();
    else {
      inputRef.current!.focus();
      inputRef.current!.select();
    }
    return () => document.removeEventListener('keydown', onKey);
  }, []);

  return (
    <div
      class="modal-bg"
      role="dialog"
      aria-modal="true"
      onClick={(e) => {
        if (e.target === e.currentTarget) cancel();
      }}
    >
      {d.kind === 'confirm' ? (
        <div class="modal">
          <p>{d.text}</p>
          <div class="row">
            <button ref={cancelRef} class="btn ghost" type="button" onClick={cancel}>
              Cancel
            </button>
            <button class="btn danger" type="button" onClick={() => d.done(true)}>
              {d.label}
            </button>
          </div>
        </div>
      ) : (
        <div class="modal">
          <p>{d.text}</p>
          <input
            ref={inputRef}
            type="text"
            maxLength={40}
            aria-label={d.text}
            defaultValue={d.value}
            onKeyDown={(e) => {
              if (e.key === 'Enter') d.done(e.currentTarget.value.trim());
            }}
          />
          <div class="row" style="margin-top:12px">
            <button class="btn ghost" type="button" onClick={cancel}>
              Cancel
            </button>
            <button class="btn" type="button" onClick={() => d.done(inputRef.current!.value.trim())}>
              {d.label}
            </button>
          </div>
        </div>
      )}
    </div>
  );
}

export function DialogHost() {
  return (
    <>
      {open.value.map((d) => (
        <Dialog key={d.id} d={d} />
      ))}
    </>
  );
}
