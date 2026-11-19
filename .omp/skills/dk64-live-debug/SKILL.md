---
name: dk64-live-debug
description: Attach to the user's running Donkey Kong 64 web port tab (omp browser relay) to watch game state, read marks, trace events and frame grabs while they play, toggle trace categories, add temporary trace points, and set no-rebuild wasm logpoints. Use when the user is playing and reporting issues live.
---

# DK64 live debugging

All commands run from the repo root. The page exposes `window.dk64dbg` (`web/debug.js`); the C side lives in
`port/src/debug.c` (state JSON, trace ring) and `port/src/gfx/gfx_gl.c` (frame grab). Trace points in game code
come from `patches/0014-port-debug-trace-hooks.patch`.

## 1. Start
- If `lsof -iTCP:8080 -sTCP:LISTEN` prints nothing, start the bash service `python3 -m http.server -d build/web 8080`
  with name `dk64-web` (ready: port 8080).
- Tell the user to open `http://localhost:8080/?rom=/dev-rom.z64` in Chrome. **F2** toggles the debug panel
  (state, trace category toggles, recent trace events, Mark / Export / Clear buttons); **F8** drops a mark
  (state + last 50 trace events + frame grab). Ask them to mark the moment something looks wrong and describe it.
  For a rendering glitch, ask them to **Alt+click** the broken spot: that drops a mark whose `probe` field lists the
  RDP state of every triangle drawn through that pixel on the next frame (display-list path, other modes, combiner,
  G_TEXTURE tile/levels/scale, the 8 tile descriptors with decoded sizes, the last 8 texture loads, and each vertex's
  object position, st, screen position and shade). Works from an exported marks file too, so no tab access is needed.
- **F5** quicksaves, **F9** quickloads (`web/quicksave.js`). A reload of the same `dk64.wasm` restores the snapshot;
  after a rebuild the page boots straight into the resume point (map + position, adventure mode only). So after a
  rebuild the user only reloads (⌘⇧R) instead of replaying to the bug. `dk64qs.status()` shows what is stored.
- If the relay can't see their tab, ask them to press **Export** in the panel: it downloads
  `~/Downloads/dk64-marks-<YYYYMMDD-HHMMSS>.json` (`{when, state, mask, marks, trace}`; images are JPEG data URLs).

## 2. Attach
Never navigate, reload, resize, or send keys to the user's tab. Never pass `url` to this `open`.
```js
const t = (await browser.targets({ app: 'relay' })).find(x => x.url.includes('localhost:8080'));
if (!t) throw new Error('game tab not open');            // ask the user to open the game URL
const tab = await browser.open({ name: 'dk64-live', app: 'relay', target: t.target });
```
Release with `browser.close({ name: 'dk64-live' })`; for an adopted tab this only detaches.

## 3. Look
- State: `await tab.evaluate(() => dk64dbg.state())` →
  `{vi, frames, gfxTasks, audTasks, map, mode, overlay, audioPeak, viFb, viBlack, lastError, actors, player}`;
  `player` is `{x, y, z, yaw, chunk, type}` or `null`. `frames` advancing between two reads = game running.
- Current frame: `await tab.screenshot()`.
- Marks without images: `await tab.evaluate(() => dk64dbg.marks().map(m => ({ ...m, image: !!m.image })))`.
  Each mark: `{id, time, note, state, trace, image}`; the page keeps the last 20, ids restart on reload.
- A mark's image (JPEG data URL, max 960 wide):
  ```js
  const img = await tab.evaluate(id => dk64dbg.marks().find(m => m.id === id).image, id);
  await Bun.write(`/tmp/dk64-mark-${id}.jpg`, Buffer.from(img.split(',')[1], 'base64'));
  ```
  then `read` `/tmp/dk64-mark-<id>.jpg` to view it.
- Fresh grab without a mark: `await tab.evaluate(() => dk64dbg.capture())` (same data URL format).
- Pixel probe without a mark: `await tab.evaluate(() => dk64dbg.probe(fx, fy))`, `fx`/`fy` as fractions of the canvas
  (top-left origin). Rejects when the point is off the game image.
- `tab.console()` shows browser-side warnings (e.g. `GL_INVALID_*`) logged since attach.

## 4. Trace
- Read incrementally: `const r = await tab.evaluate(s => dk64dbg.trace(s), since)` →
  `{events: [{seq, vi, cat, text}], next, dropped}`; carry `r.next` forward as the next `since`.
  `dropped: true` means events between `since` and the oldest kept one fell out of the 256-entry ring.
- Categories: MAP=1 (`mapRequestTransition*`), CAM=2 (`camStartPath`/`camEndPath`), ACTOR=4 (`actorSpawn`/`actorDespawn`,
  noisy), DEBUG=8 (temporary trace points from section 6). Default mask 3. `dk64dbg.traceMask()` reads it; setting it
  (API or the panel's chips) is stored in localStorage and re-applied before boot on the next load, so restore the
  user's previous mask when done.
- Map ids appear as hex in trace text, decimal in `state().map`. Names: `decomp/tools/emu/maps.tsv` (labels come from
  emulator screenshots; unreliable) and the randomizer list
  `https://raw.githubusercontent.com/2dos/DK64-Randomizer/dev/randomizer/Enums/Maps.jsonc` (e.g. 171 Treehouse,
  176 Training Grounds, 76 DK Rap).

## 5. Logpoints (no rebuild, no pause)
- Offsets, from the same build the tab loaded:
  `python3 tools/wasm_funcs.py build/web/dk64.wasm '^camUpdate$'` → `<name>\t<funcIndex>\t<offset>`.
- Only use functions called at most a few hundred times per second (per-frame functions are fine; `camUpdate` gave
  ~20 hits/s with the game still at ~24 fps). Keep windows short.
- Wasm locals show up as `$var0..`; for parameters these are the C arguments in order (pointers are wasm addresses:
  RDRAM is mapped 1:1 at 0, so `0x80xxxxxx` game pointers appear as `xxxxxx`).
- Tell the user before starting: "watching <fn> for N s, do the thing now". Then:
  ```js
  const logpoints = async ({ page }, { funcs, seconds = 60, maxHits = 2000 }) => {
    seconds = Math.min(seconds, 280);
    const cdp = await page.target().createCDPSession();
    const scripts = [], hits = [], ids = [];
    let truncated = false;
    const start = Date.now();
    const removeAll = async () => {
      for (const id of ids.splice(0)) await cdp.send('Debugger.removeBreakpoint', { breakpointId: id }).catch(() => {});
    };
    cdp.on('Debugger.scriptParsed', e => { if (e.scriptLanguage === 'WebAssembly') scripts.push(e); });
    cdp.on('Runtime.consoleAPICalled', e => {
      const a0 = e.args[0] && e.args[0].value;
      if (typeof a0 !== 'string' || !a0.startsWith('LP ') || truncated) return;
      hits.push({ t: Math.round(e.timestamp - start), args: e.args.map(a => (a.value !== undefined ? a.value : a.description)) });
      if (hits.length > maxHits) { truncated = true; removeAll(); }
    });
    // cleanup always runs: a failed setBreakpoint must not leave earlier logpoints active in the user's game
    try {
      await cdp.send('Runtime.enable');
      await cdp.send('Debugger.enable');
      const t0 = Date.now();
      while (!scripts.length && Date.now() - t0 < 2000) await new Promise(r => setTimeout(r, 50));
      if (!scripts.length) throw new Error('no dk64.wasm script');
      const script = scripts.find(s => s.url.endsWith('dk64.wasm')) || scripts.reduce((a, b) => (b.length > a.length ? b : a));
      for (const { name, offset } of funcs) {
        const r = await cdp.send('Debugger.setBreakpoint', {
          location: { scriptId: script.scriptId, lineNumber: 0, columnNumber: offset },
          // wasm values arrive wrapped as {type, value}; the trailing `false` keeps execution from pausing
          condition: 'console.log("LP ' + name + '", typeof $var0 !== "undefined" ? $var0.value : "", typeof $var1 !== "undefined" ? $var1.value : ""), false',
        });
        ids.push(r.breakpointId);
      }
      await new Promise(r => setTimeout(r, seconds * 1000));
      return { hits, truncated };
    } finally {
      await removeAll();
      await cdp.send('Debugger.disable').catch(() => {});
      await cdp.detach().catch(() => {});
    }
  };
  const seconds = 10;
  const offset = 636846; // third column of tools/wasm_funcs.py for the current build; changes on every rebuild
  const r = await tab.run(logpoints, { args: [{ funcs: [{ name: 'camUpdate', offset }], seconds, maxHits: 2000 }], timeout: seconds + 20 });
  ```
  `hits[i].t` is ms since start (page timestamp); `truncated` means `maxHits` was exceeded and breakpoints were removed early.
  For more arguments extend the condition with `$var2`, `$var3`, ... the same way.

## 6. Temporary trace points
- Add `PORT_TRACE(PORT_TRACE_MAP|CAM|ACTOR, "fmt", ...)` in `build/tree/src/...` (no include needed; `port.h` is
  force-included), rebuild with `cmake --build build/web`, and ask the user before they reload: a reload loses progress
  since the last save.
- Revert with `git -C build/tree checkout -- <file>` once done. Promote a point to a patch only if it stays useful
  (commit in `build/tree`, then `python3 tools/prepare.py export`).

## 7. Headless repro
`node build/web/dk64_headless.js --rom decomp/donkey-kong.z64 --frames N --input tests/<file>.txt --trace 3`
prints `TRACE vi=<vi> <text>` lines as events happen, plus the `frame N {state json}` line every 60 VIs. No audio output.
Input files: `tests/title_to_menu.txt`, `tests/new_game_intro.txt`.

## 8. Testing etiquette
Any tab the agent opens itself must suspend audio. Install the shim before loading the game:
```js
const tab = await browser.open({ name: 'dk64-verify', app: 'relay', url: 'about:blank' });
await tab.run(async ({ page }) => {
  await page.evaluateOnNewDocument(() => {
    for (const k of ['AudioContext', 'webkitAudioContext']) { const O = window[k]; if (!O) continue; window[k] = class extends O { constructor(o) { super(o); this.suspend(); } resume() { return Promise.resolve(); } }; }
  });
  await page.goto('http://localhost:8080/?rom=/dev-rom.z64&noresume');
}, { timeout: 60 });
```
Check `Module.SDL2.audioContext.state === 'suspended'`, and close the tab with `browser.close({ name })` when done.
Agent tabs share the user's origin storage: `?noresume` keeps them from restoring the user's quicksave, and F5 /
`dk64qs.save()` in an agent tab overwrites the user's only slot (IndexedDB `dk64dbg/snapshots/quick`, localStorage
`dk64dbg.resume`). Don't quicksave there unless the user asked.
