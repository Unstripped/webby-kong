# Debugging

## Runtime counters

In the browser console, `Module.ccall('port_debug_stats', 'string')` returns the runtime counters as JSON (VI ticks,
frames, RSP tasks, map, overlay, audio peak, actor count, player position). The headless build prints the same
JSON every 60 frames ([testing.md](testing.md)).

## Debug panel and `dk64dbg`

`web/debug.js` adds a `dk64dbg` object to the page (state, trace log, frame grabs, marks):

- F2 toggles the debug panel: state, trace category toggles (they persist across reloads), Mark and Export buttons,
  quicksave controls.
- F8 records a mark in the trace log.
- Alt+click on the canvas probes a pixel.

## Quicksave and resume

F5 quicksaves and F9 quickloads (`web/quicksave.js`, one slot in IndexedDB). A quicksave is an exact snapshot of
RDRAM plus the port and game `.data`/`.bss` (`port/src/qs.c`) and restores only into the identical `dk64.wasm`
(it is bound to the wasm's SHA-1). It also records a resume point (save slot, kong, map, position) taken in
adventure mode.

After a reload the snapshot is restored if the wasm is unchanged; after a rebuild the game boots straight into the
resume point once the EEPROM is read. `?noresume` or the panel's Auto-resume chip turns that off.

## Logpoints and live debugging

`tools/wasm_funcs.py build/web/dk64.wasm <regex>` prints the function index and breakpoint offset of every matching
wasm function, for DevTools logpoints. The `dk64-live-debug` skill
(`.omp/skills/dk64-live-debug/SKILL.md`) documents attaching to a running tab, trace categories and wasm logpoints.

## Memory errors

Configure a separate build directory with `-DPORT_SAFE_HEAP=ON` to make the headless build trap wild accesses:

```sh
emcmake cmake -S . -B build/safe -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPORT_SAFE_HEAP=ON
cmake --build build/safe --target dk64_headless
```
