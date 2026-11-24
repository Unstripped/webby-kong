import { toastShown, toastText } from './toast';

export function GameToast() {
  return (
    <div id="dk64dbg-toast" class={toastShown.value ? 'show' : undefined}>
      {toastText.value}
    </div>
  );
}
