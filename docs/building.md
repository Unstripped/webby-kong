# Building

The build turns this repository into a browser build (`dk64.js`/`dk64.wasm` and the page in `web/`) and a node
build for headless runs (`dk64_headless.js`). It needs no ROM: the data the decomp extracts from the ROM is compiled
as placeholders and filled at start from the ROM the page (or `--rom` of the headless build) loads, using the
committed manifest in `port/ref/`. Only regenerating that manifest after a decomp bump needs the ROM.

## Prerequisites

- macOS or Linux with git.
- [mise](https://mise.jdx.dev): `mise install` installs the pinned tools from `mise.toml` — python 3.12.14,
  node 24.22.0, cmake 4.5.0, ninja 1.13.2 and emsdk 6.0.13.
- The ROM, to play: Donkey Kong 64 (USA), SHA-1 `cf806ff2603640a748fca5026ded28802f1f4a50`.
- Docker, only for the reference decomp build (maintainers, after a decomp bump) or the image build.

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

## Reference decomp build (after a decomp bump)

`tools/gen_data.py` reads `port/ref/`: the reference decomp build's ELF symbols, splat's auto symbols, and the
asm data labels with their sizes and directive widths. `port/ref/decomp_fingerprint.txt` records the decomp
inputs it was made from; `gen` fails when `decomp/` no longer matches. Regenerating it needs the decomp's own
build from the ROM, in Docker:

```sh
cp /path/to/dk64.z64 decomp/donkey-kong.z64
cd decomp && docker compose build && tools/dk make setup BASEROM=donkey-kong.z64 && tools/dk make   # ends with OK
cd .. && tools/prepare.py ref   # rewrites port/ref; checks every filled byte run against the ROM's segments
```

`decomp/{asm,assets,bin,build}` are ROM-derived: keep them private. `port/ref/` holds names, addresses and sizes
only, and passes `tools/check_no_roms.py`.

## Build

```sh
tools/prepare.py all            # build/tree = decomp src+include + patches/*.patch; build/gen = generated sources
emcmake cmake -S . -B build/web -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/web          # dk64 (browser) and dk64_headless (node)
```

- `tools/prepare.py all` runs `tree` (copies `decomp/{src,include}` to `build/tree`, tags it `base`, applies
  `patches/*.patch`) and then `gen` (`tools/gen_data.py` writes `build/gen` from `decomp/` and `port/ref/`).
  `tools/prepare.py --help` lists the other subcommands (`rebase`, `export`, `ref`, `check`).
- At start, `portRomFill` (`port/src/romfill.c`) inflates the global, arcade and jetpac segments from the ROM and
  copies the pieces listed in `build/gen/rom_fill.c` into their placeholders (`PORT: rom fill ok <n> pieces`).
- CMake links two targets: `dk64` (browser) and `dk64_headless` (node).
- Edits to `web/*.js` and `web/index.html` are copied to `build/web` by their own target, without relinking.
- `-DPORT_SAFE_HEAP=ON` in a separate build directory makes the headless build trap wild memory accesses.

## Serving and saves

```sh
tools/serve.py   # --dir build/web --port 8080 --bind :: --db ~/.local/share/webby-kong/saves.sqlite3
```

Open `http://localhost:8080/` and pick the ROM. With `--rom /path/to/dk64.z64` (or a directory holding one
`*.z64`/`*.n64`/`*.v64`) the server serves it at `/api/rom` and the page starts it without the picker. For a ROM at
another URL, open `http://localhost:8080/?rom=<url>`.

The game save (EEPROM) lives in IndexedDB; `web/saves.js` also uploads every version to `tools/serve.py`, which keeps
them in `~/.local/share/webby-kong/saves.sqlite3` (change with `--db`) and restores the newest one at page load. The
recovery commands (list, export, make a version current) are in the `tools/serve.py` docstring
(`tools/serve.py --help`). Under a plain static server the page warns and keeps the save in the browser only.

## Docker image

```sh
docker build -t webby-kong .
docker run -p 8080:8080 -v /path/to/dk64.z64:/rom/dk64.z64:ro -v webby-kong-saves:/data webby-kong
```

The build stage (emsdk, on the build platform: the wasm is architecture-independent) runs `tools/prepare.py all`
and builds `dk64`; `.dockerignore` keeps ROMs and the reference decomp build out of the context. The runtime stage
is `python:3.12-slim` running `tools/serve.py` as uid 10001 with `--rom /rom` and saves in the `/data` volume (with a
host bind mount instead of a volume, add `--user $(id -u)`). `.github/workflows/docker.yml` publishes
`ghcr.io/mjquinlan2000/webby-kong` for linux/amd64 and linux/arm64: `:latest` from `development`, `:<version>` from
each release.

## Bumping the decomp

1. In `decomp/`: check out the new commit, then `tools/dk make setup BASEROM=donkey-kong.z64 && tools/dk make`
   (asm, ELFs and `assets/inc` must match it), then `tools/prepare.py ref` from the repository root.
2. `tools/prepare.py rebase <old decomp commit>`; resolve conflicts in `build/tree` (`git rebase --continue`),
   commit fixups there, then `tools/prepare.py export`.
3. `tools/prepare.py gen`; check `build/gen/report.txt` for errors and new `UNRESOLVED` interior labels; fix link
   errors (renamed symbols in patches and `port/src/asm`).
4. Build, run the scenarios in [testing.md](testing.md), then commit the submodule pointer together with the patches
   and `port/ref/`.

## Troubleshooting

| Message | Cause and fix |
| --- | --- |
| `port/ref was generated from a different decomp (decomp/ changed)` | `decomp/` changed since `port/ref/` was made: run the reference decomp build and `tools/prepare.py ref` (see above). |
| `prepare: missing decomp/asm, …` (from `prepare.py ref`) | The reference decomp build has not run: see above. |
| `PORT: rom fill seg <n> inflated 0x… bytes, expected 0x…` | `port/ref/` does not match the ROM's segment layout: regenerate it with `tools/prepare.py ref`. |
| `configure with: emcmake cmake -S . -B build/web -G Ninja` | CMake was run without Emscripten: prefix it with `emcmake`. |
| `run tools/prepare.py all (or tree + gen --headers) first` | `build/tree` / `build/gen` are missing: run `tools/prepare.py all`. |
| `Cannot find source file: …/build/gen/<file>.c` (CMake) | `build/gen` predates a change to `tools/gen_data.py` that added the file: run `tools/prepare.py gen`. |
| `link_check: function signature mismatch (fix the prototype by patch)` | A function is declared and defined with different types; fix the prototype in a patch. |
| `Unsupported ROM: need Donkey Kong 64 (USA), SHA-1 …` (in the page) | Wrong ROM, region or revision. |
| `prepare: build/tree has uncommitted changes` | `prepare.py tree`/`all` would discard work in `build/tree`: commit it there and `tools/prepare.py export`, or move it away. |
