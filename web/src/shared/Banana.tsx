import { useEffect, useRef } from 'preact/hooks';
import { mountBanana } from './bananaCanvas';

// The animated banana's container (#banana). The page's CSS sizes it: full-viewport backdrop on the splash,
// the 56×56 header slot on the dashboard.
export function Banana() {
  const ref = useRef<HTMLDivElement>(null);
  useEffect(() => mountBanana(ref.current!, { src: '/auth/banana.png' }).stop, []);
  return <div id="banana" aria-hidden="true" ref={ref} />;
}
