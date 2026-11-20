# Building

The build turns your own Donkey Kong 64 (USA) ROM plus this repository into a browser build (`dk64.js`/`dk64.wasm`
and the page in `web/`) and a node build for headless runs (`dk64_headless.js`). Everything under `build/` contains
ROM-derived data: keep it private.

## Prerequisites

- macOS or Linux with git.
- [mise](https://mise.jdx.dev): `mise install` installs the pinned tools from `mise.toml` — python 3.12.14,
  node 24.22.0, cmake 4.5.0, ninja 1.13.2 and emsdk 6.0.13.
- Docker, for the reference decomp build.
- The ROM: Donkey Kong 64 (USA), SHA-1 `cf806ff2603640a748fca5026ded28802f1f4a50`.

## One-time setup

```sh
mise install
git submodule update --init
python -m venv tools/.venv
tools/.venv/bin/pip install -r decomp/requirements.txt
tools/.venv/bin/pip install -r tools/requirements.txt --force-reinstall --no-deps
```

The order matters: the decomp's requirements install a `clang` package, and the second command then reinstalls
`libclang` from `tools/requirements.txt` over it (without touching other dependencies).

Optional but recommended: refuse ROMs and ROM-derived files at commit time in this clone:

```sh
git config core.hooksPath .githooks
```

## Reference decomp build

The port reads the decomp's split assembly, data and `build/global.elf`, which the decomp's own build produces from
the ROM. Run it in Docker once per ROM, and again after every decomp submodule bump:

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

- `tools/prepare.py all` runs `tree` (copies `decomp/{src,include}` to `build/tree`, tags it `base`, applies
  `patches/*.patch`) and then `gen` (`tools/gen_data.py` writes `build/gen`). `tools/prepare.py --help` lists the
  other subcommands (`rebase`, `export`, `check`).
- CMake links two targets: `dk64` (browser) and `dk64_headless` (node).
- Edits to `web/*.js` and `web/index.html` are copied to `build/web` by their own target, without relinking.
- `-DPORT_SAFE_HEAP=ON` in a separate build directory makes the headless build trap wild memory accesses.

## Serving and saves

```sh
tools/serve.py   # --dir build/web --port 8080 --bind :: --db ~/.local/share/webby-kong/saves.sqlite3
```

Open `http://localhost:8080/` and pick the ROM. For development, copy the ROM to `build/web/dev-rom.z64` and open
`http://localhost:8080/?rom=/dev-rom.z64`.

The game save (EEPROM) lives in IndexedDB; `web/saves.js` also uploads every version to `tools/serve.py`, which keeps
them in `~/.local/share/webby-kong/saves.sqlite3` (change with `--db`) and restores the newest one at page load. The
recovery commands (list, export, make a version current) are in the `tools/serve.py` docstring
(`tools/serve.py --help`). Under a plain static server the page warns and keeps the save in the browser only.

## Bumping the decomp

1. In `decomp/`: check out the new commit, then `tools/dk make setup BASEROM=donkey-kong.z64 && tools/dk make`
   (asm, ELFs and `assets/inc` must match it).
2. `tools/prepare.py rebase <old decomp commit>`; resolve conflicts in `build/tree` (`git rebase --continue`),
   commit fixups there, then `tools/prepare.py export`.
3. `tools/prepare.py gen`; check `build/gen/report.txt` for errors and new `UNRESOLVED` interior labels; fix link
   errors (renamed symbols in patches and `port/src/asm`).
4. Build, run the scenarios in [testing.md](testing.md), then commit the submodule pointer together with the patches.

## Troubleshooting

| Message | Cause and fix |
| --- | --- |
| `prepare: missing decomp/asm, decomp/build/global.elf` | The reference decomp build has not run: see above. |
| `configure with: emcmake cmake -S . -B build/web -G Ninja` | CMake was run without Emscripten: prefix it with `emcmake`. |
| `run tools/prepare.py all (or tree + gen --headers) first` | `build/tree` / `build/gen` are missing: run `tools/prepare.py all`. |
| `link_check: function signature mismatch (fix the prototype by patch)` | A function is declared and defined with different types; fix the prototype in a patch. |
| `Unsupported ROM: need Donkey Kong 64 (USA), SHA-1 …` (in the page) | Wrong ROM, region or revision. |
| `prepare: build/tree has uncommitted changes` | `prepare.py tree`/`all` would discard work in `build/tree`: commit it there and `tools/prepare.py export`, or move it away. |
