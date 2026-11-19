# webby-kong

A source port of the Donkey Kong 64 decomp ([Unstripped/decomp_dk64](https://github.com/Unstripped/decomp_dk64),
pinned as the `decomp/` submodule) to WebAssembly: the game's C compiled with Emscripten, libultra replaced by a
small cooperative runtime, and the RSP replaced by an F3DEX2 renderer on WebGL2 and an naudio HLE.

You must supply your own ROM: **Donkey Kong 64 (USA)**, SHA-1 `cf806ff2603640a748fca5026ded28802f1f4a50`.
The built `.wasm` contains data generated from that ROM, so treat every build artifact as private.

## Setup

```sh
mise install
git submodule update --init
python -m venv tools/.venv
tools/.venv/bin/pip install -r decomp/requirements.txt
tools/.venv/bin/pip install -r tools/requirements.txt --force-reinstall --no-deps  # libclang after the decomp's `clang` package
```

Reference decomp build (Docker; once per ROM or submodule bump; after a bump carry the patches over with
`tools/prepare.py rebase <old decomp commit>`, resolve in `build/tree`, then `tools/prepare.py export`):

```sh
cp /path/to/dk64.z64 decomp/donkey-kong.z64
cd decomp && docker compose build && tools/dk make setup BASEROM=donkey-kong.z64 && tools/dk make   # ends with OK
```

## Build

```sh
tools/prepare.py all            # build/tree = decomp src+include + patches/*.patch; build/gen = generated sources
emcmake cmake -S . -B build/web -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/web          # dk64 (browser) and dk64_headless (node)
```

Run in a browser: `python3 -m http.server -d build/web 8080`, open `http://localhost:8080/`, pick the ROM
(development: copy the ROM to `build/web/dev-rom.z64` and open `http://localhost:8080/?rom=/dev-rom.z64`).
`Module.ccall('port_debug_stats', 'string')` in the console returns the runtime counters (VI ticks, frames,
RSP tasks, map, overlay, audio peak, actor count, player position). `web/debug.js` adds `dk64dbg` (state, trace log,
frame grabs, marks): F2 toggles a debug panel (state, trace category toggles that persist across reloads, Mark /
Export buttons, quicksave), F8 records a mark. F5 quicksaves and F9 quickloads (`web/quicksave.js`, one slot in
IndexedDB): an exact snapshot of RDRAM plus the port/game `.data`/`.bss` (`port/src/qs.c`) that restores only into the
identical `dk64.wasm`, plus a resume point (save slot, kong, map, position) taken in adventure mode. After a reload the
snapshot is restored if the wasm is unchanged; after a rebuild the game boots straight into the resume point once the
EEPROM is read. `?noresume` or the panel's Auto-resume chip turns that off. The `dk64-live-debug` skill
(`.omp/skills/dk64-live-debug/SKILL.md`) documents attaching to a running tab, trace categories and wasm logpoints
(`tools/wasm_funcs.py`).

Headless: `node build/web/dk64_headless.js --rom decomp/donkey-kong.z64 --frames 2400 --input tests/title_to_menu.txt`
prints those counters every 60 VI ticks and reaches the main menu (map 0x50) by the end. `--lax` logs asset tables
without a byte-order swapper instead of aborting; `--trace-assets` logs every asset load with its RDRAM range;
`--trace <mask>` prints game trace events (`PORT_TRACE`; 1 = map transitions, 2 = camera paths, 4 = actor spawns).
`--qs-save F --qs-load G` snapshots after frame F, restores it after frame G and replays from F (the run must match
one without them); `--resume slot,kong,map,exit,hasPos,x,y,z,yaw` boots into a resume point (`--resume
0,0,0xb0,1,1,941,60,736,3752`: Training Grounds).
`tests/new_game_intro.txt` with `--frames 21000` starts a new game and must end in the Treehouse (map 171, mode 0):
the intro story's map changes run on wall-clock cues, and patch 0012 keeps checking them after a camera path ends
early (the port runs a steady 30 fps, so frame-counted paths finish sooner than on hardware).
Configure with `-DPORT_SAFE_HEAP=ON` (separate build dir) to trap wild accesses.

Controls: arrows = stick, X = A, C = B, Z = Z, A/S = L/R, Enter = Start, I/K/J/L = C-buttons, T/G/F/H = D-pad;
the first gamepad works too. Saves (EEPROM) persist in IndexedDB.

## Layout

- `patches/` — changes to the decomp source, one commit per concern. Edit by committing in `build/tree`,
  then `tools/prepare.py export`.
- `tools/gen_data.py` — the decomp defines its static data in C; this emits what is left as splat asm (data only
  handwritten asm reads, and asm `.bss`) as native C objects at its directive widths, the decomp's ROM-extracted
  initialisers (`data_inc.us.txt`) in N64 byte order, `OVL_FN` thunks, overlay rename headers, `port_abs.h`, overlay
  dispatchers and a layout self-check.
- `port/` — runtime: fibers for N64 threads, libultra shims, ROM/asset byte-order swappers, renderer, audio.
- `web/` — page and loader.

## Notes on the port

- Memory model: wasm addresses 0–8 MB are N64 RDRAM 1:1; every game-visible static and stack lives below
  16 MB, so F3DEX2 segment math works on host pointers.
- `-sGLOBAL_BASE=8MB`: emcc rejects a hex value for this setting.
- wasm clang ignores `#pragma clang section`, so overlay data is bracketed by link-order marker objects
  (`build/web/ovl_markers/`) instead; loading an overlay restores its pristine data and zeroes its bss.
- libultra's VI mode tables are compiled from the decomp (`vitbl.c`, `vimodes/`); `port/src/boot.c` replaces
  `src/boot/main.c` and defines the globals of it that game code reads.
- Interior aliases of C objects that the decomp links by absolute address are defined with `.set` next to
  their parent (little-endian offsets); byte reads of other-width data (N64 code addresses inside function
  tables, bytes of s16 tables) use `port/shadow_data.txt` snapshots of the N64 bytes.
- Overlay functions stored in global data are `OVL_FN(type, addr)` in the decomp: the raw VRAM there, a thunk to
  the loaded overlay's function here (`build/gen/ovl_entry.c`).
- Textures in `data_inc.us.txt` blobs stay in N64 byte order (the RDP reads them so); C data is native, so a reader
  of another width than the definition needs a patch (or a fix of the decomp's type: they must match).
- Pacing: the scheduler freezes the RDP after a framebuffer swap and thaws it on the retrace that shows it; the
  port holds the next gfx task while frozen, which is what limits the game to its 30 Hz frame rate.
- IDO returned whatever was left in `$v0` from functions that fall off the end; those sites now return that
  value explicitly and game TUs build with `-Werror=return-type`.
- Framebuffer readbacks write only the RDRAM range the game asked for: a colour image's height is not in the
  display list (4:3 is assumed), and RDRAM past the real image belongs to the heap.
- Widescreen (Hor+): the canvas fills the browser window at device-pixel resolution. VI-sized colour images
  are FBOs widened to the window aspect (uncapped: on very wide windows some backdrop meshes, e.g. the main
  menu's, end before the screen edge), with the N64 image centred at `GfxFb.ox`; 3D clip x is divided by
  `gGfxTargetWide`, so the original frustum covers the centre and the sides show more scene. 2D draws keep
  their 4:3 place; scissors and fill rects spanning the width (overscan margins up to width/16 included)
  reach the edges, and the side areas are blanked once per gfx task. RDRAM uploads and readbacks use only the
  centred region. `portGfxWideHalfYaw` (patch 0011) widens the map-culling frustum and `portGfxWideClipX`
  (patch 0013) the screen bounds objects are culled against. Map chunks seen through portals (doorways) are
  still found in 4:3 screen space, so a room visible only through a doorway at the side edge may not draw.
