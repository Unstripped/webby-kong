# webby-kong

A source port of the Donkey Kong 64 decomp ([Unstripped/decomp_dk64](https://github.com/Unstripped/decomp_dk64),
pinned as the `decomp/` submodule) to WebAssembly: the game's C compiled with Emscripten, libultra replaced by a
small cooperative runtime, and the RSP replaced by an F3DEX2 renderer on WebGL2 and an naudio HLE.

Status: playable in the browser from boot through gameplay. Releases are source-only: you build the game yourself
from your own ROM.

## Legal

- You must own and supply your own ROM: **Donkey Kong 64 (USA)**, SHA-1 `cf806ff2603640a748fca5026ded28802f1f4a50`.
  The browser loader accepts it in z64, v64 or n64 byte order.
- No ROM, game asset or ROM-derived file is in this repository or its releases, and CI rejects any commit that
  adds one (`tools/check_no_roms.py`).
- The built `dk64.wasm` contains data generated from the ROM: treat every build artifact as private and never
  publish it.
- This project is not affiliated with or endorsed by Nintendo or Rare.

## Features

- Widescreen (Hor+) at the browser window's resolution.
- Objects and enemies draw and spawn out to 4× the game's distances (`?drawdist`, 1 = N64 behaviour).
- Keyboard, the first gamepad, and an on-screen touch controller on touch devices.
- EEPROM saves in IndexedDB, with every version also kept by the local save server (SQLite).
- Esc menu with the current level's collectable progress and live settings (draw distance, render scale,
  anti-aliasing, touch controls), remembered in `localStorage`.
- F5 / F9 quicksave and quickload.
- A headless node build with scripted input for regression runs.

## Quick start

Prerequisites: macOS or Linux, git, [mise](https://mise.jdx.dev), Docker, and the ROM.

```sh
git clone https://github.com/mjquinlan2000/webby-kong.git && cd webby-kong
mise install
git submodule update --init
python -m venv tools/.venv
tools/.venv/bin/pip install -r decomp/requirements.txt
tools/.venv/bin/pip install -r tools/requirements.txt --force-reinstall --no-deps  # libclang after the decomp's `clang` package
```

Reference decomp build (Docker; once per ROM or submodule bump):

```sh
cp /path/to/dk64.z64 decomp/donkey-kong.z64
(cd decomp && docker compose build && tools/dk make setup BASEROM=donkey-kong.z64 && tools/dk make)   # ends with OK
```

Build and run:

```sh
tools/prepare.py all            # build/tree = decomp src+include + patches/*.patch; build/gen = generated sources
emcmake cmake -S . -B build/web -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/web          # dk64 (browser) and dk64_headless (node)
tools/serve.py                   # serves build/web on port 8080
```

Open `http://localhost:8080/` and pick the ROM. Details and troubleshooting: [docs/building.md](docs/building.md).

## Playing

| Input | N64 |
| --- | --- |
| Arrow keys | Control stick |
| X / C / Z | A / B / Z |
| A / S | L / R |
| Enter | Start |
| I / K / J / L | C-up / C-down / C-left / C-right |
| T / G / F / H | D-pad up / down / left / right |

The first gamepad works too. On touch devices an on-screen controller (stick on the left half, buttons, no D-pad)
appears once the game runs and hides while a gamepad is connected.

| Key | Action |
| --- | --- |
| Esc (gamepad Back/Select) | Menu with the current level's collectable progress |
| F5 | Quicksave |
| F9 | Quickload |
| F2 | Debug panel ([docs/debugging.md](docs/debugging.md)) |
| F8 | Record a debug mark |

URL parameters (`drawdist`, `scale`, `msaa` and `touch` override the Esc menu's stored setting for that page load):

| Parameter | Effect |
| --- | --- |
| `rom=<url>` | Fetch the ROM from a URL instead of picking it (development: `?rom=/dev-rom.z64` with the ROM copied to `build/web/dev-rom.z64`) |
| `drawdist=1..16` | Draw/spawn distance factor; 1 = N64 distances, default 4 |
| `scale=0..8` | Render scale; 0 (default) = automatic from the canvas height |
| `msaa=0\|1\|2\|4\|8` | Multisampling, default 4 |
| `touch=1` / `touch=0` | Force the touch controller on or off |
| `noresume` | Do not boot into the quicksave resume point after a rebuild |

## Repository layout

- `patches/` — changes to the decomp source, one commit per concern. Edit by committing in `build/tree`,
  then `tools/prepare.py export`.
- `tools/gen_data.py` — the decomp defines its static data in C; this emits what is left as splat asm (data only
  handwritten asm reads, and asm `.bss`) as native C objects at its directive widths, the decomp's ROM-extracted
  initialisers (`data_inc.us.txt`) in N64 byte order, `OVL_FN` thunks, overlay rename headers, `port_abs.h`, overlay
  dispatchers and a layout self-check.
- `tools/` — also `prepare.py` (patched tree), `serve.py` (static server + save API), `check_no_roms.py`
  (ROM guard), `next_version.py` (release version), `wasm_funcs.py`, `link_check.sh`.
- `port/` — runtime: fibers for N64 threads, libultra shims, ROM/asset byte-order swappers, renderer, audio.
- `web/` — page and loader.
- `tests/` — headless input scripts (`*.txt`) and `tests/tools/` unit tests for the tooling.
- `.github/workflows/` — CI and releases.
- `docs/` — building, testing, debugging and architecture guides.
- `decomp/` — the decomp submodule; never edited here.

## Documentation

- [docs/building.md](docs/building.md) — setup, build, serving, decomp bumps, troubleshooting.
- [docs/testing.md](docs/testing.md) — CI checks and headless regression scenarios.
- [docs/debugging.md](docs/debugging.md) — runtime counters, debug panel, quicksave, logpoints.
- [docs/architecture.md](docs/architecture.md) — build pipeline, port runtime and notes on the port.
- [CONTRIBUTING.md](CONTRIBUTING.md) — branches, commit messages, patches, releasing.

## Releases

Every push to `main` publishes a GitHub Release after CI passes. The first is `v0.1.0`; each later one bumps the
patch version, or the minor / major version when a commit message since the last release contains `#minor` /
`#major`. Releases are source-only: no ROM-derived build output is attached.

## License

[CC0 1.0 Universal](LICENSE), the same as the decomp. Exception: `web/nipplejs.js` is nipplejs under the MIT
license (see its header).
