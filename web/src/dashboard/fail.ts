import { toast } from './toasts';

// The dashboard's one error path: a 401 sends the user to the splash (coming back here after sign-in);
// anything else is an error toast. Network failures from api() carry "Could not reach the server…".
export function fail(e: unknown): void {
  if (e && typeof e === 'object' && 'status' in e && e.status === 401) {
    location.assign('/auth/?next=' + encodeURIComponent(location.pathname + location.search));
    return;
  }
  toast((e instanceof Error && e.message) || 'Something went wrong.', true);
}
