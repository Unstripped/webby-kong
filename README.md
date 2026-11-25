# webby-kong

A source port of the Donkey Kong 64 decomp ([Unstripped/decomp_dk64](https://github.com/Unstripped/decomp_dk64),
pinned as the `decomp/` submodule) to WebAssembly: the game's C compiled with Emscripten, libultra replaced by a
small cooperative runtime, and the RSP replaced by an F3DEX2 renderer on WebGL2 and an naudio HLE.

Status: playable in the browser from boot through gameplay. The build needs no ROM: the page loads your ROM at
start. Releases are source plus a Docker image (`ghcr.io/unstripped/webby-kong`) that serves the game.

## Legal

- You must own and supply your own ROM: **Donkey Kong 64 (USA)**, SHA-1 `cf806ff2603640a748fca5026ded28802f1f4a50`.
  The browser loader accepts it in z64, v64 or n64 byte order.
- No ROM, game asset or ROM-derived file is in this repository or its releases, and CI rejects any commit that
  adds one (`tools/check_no_roms.py`).
- The build reads no ROM: `dk64.wasm` carries placeholders for the ROM-extracted data, filled at start from the ROM
  the page loads. The reference decomp build (`decomp/{asm,assets,bin,build}`, only needed to regenerate
  `port/ref` after a decomp bump) is ROM-derived: keep it private.
- This project is not affiliated with or endorsed by Nintendo or Rare.

## Features

- 60, 120 or 240 fps: interpolated in-between frames up to the display's refresh rate; game logic runs at the
  N64's 30 Hz. Esc menu "Frame rate" (or `?interp=`) picks the rate; 30 is the game's own.
- Widescreen (Hor+) at the browser window's resolution.
- Objects and enemies draw and spawn out to 4× the game's distances (`?drawdist`, 1 = N64 behaviour).
- Keyboard, the first gamepad, and an on-screen touch controller on touch devices.
- EEPROM saves in IndexedDB, with every version also kept by the local save server (SQLite). Several save games,
  each with its own EEPROM and history (`/play?save=<id>`).
- Save export / import from the Esc menu: "Export save" downloads the save's 2 KB EEPROM (`.eep`); "Import save…"
  replaces this save game's progress with an `.eep` (Project64 / mupen64plus EEPROM files are the same bytes) or a
  RetroArch mupen64plus `.srm`, keeping the old progress in the save server's history.
- Esc menu with the current level's collectable progress and live settings (draw distance, render scale,
  anti-aliasing, frame rate, touch controls, 3D sound, audio quality), remembered in `localStorage`.
- Bug reports and ideas from the Esc menu: a prefilled GitHub issue with a screenshot (click the broken spot to
  attach the renderer's view of it), your save and exact location. Crashes open the same report.
- F5 / F9 quicksave and quickload.
- Community audio packs: replacement songs and sound effects in a `.zip`, uploaded in the dashboard or imported in
  the Esc menu, turned on per player in priority order ([docs/audio-packs.md](docs/audio-packs.md)). Audio is never
  committed here.
- A headless node build with scripted input for regression runs.

## Quick start

With Docker, mount your ROM (a file, or a directory holding one `*.z64`/`*.n64`/`*.v64`) at `/rom`; the page starts
it without the ROM drop zone, and saves persist in the `/data` volume:

```sh
docker run -p 8080:8080 -v /path/to/dk64.z64:/rom/dk64.z64:ro -v webby-kong-saves:/data ghcr.io/unstripped/webby-kong
```

`:latest` follows `development`; releases are tagged with their version (`:v0.1.0`). Without a ROM mount the page
shows a drop zone: drop the ROM on the page or click it to choose the file.

On Kubernetes, the Helm chart in `charts/webby-kong` runs the same image with a persistent `/data` volume; mount the
ROM from any volume source with `rom.volume` (see the comments in `values.yaml` for ROM, sign-in, SMTP and Ingress):

```sh
helm install webby-kong oci://ghcr.io/unstripped/charts/webby-kong \
  --set rom.volume.persistentVolumeClaim.claimName=dk64-rom
```

## Configuration

The server reads everything from `WEBBY_KONG_*` environment variables, and a command-line flag (`--dir`, `--port`,
`--bind`, `--db`, `--rom`) overrides one. The same `.env` file configures both ways to run it, so a checkout and the
image take the same settings (the image's own layout below is only a default that the file can override):

```sh
mise run serve                        # local: build/web on port 8080, saves in ~/.local/share/webby-kong
cp .env.example .env                  # optional: your settings (gitignored; every variable is in the file)
docker run -p 8080:8080 --env-file .env \
  -v /path/to/dk64.z64:/rom/dk64.z64:ro -v webby-kong-saves:/data ghcr.io/unstripped/webby-kong
```

| Variable | Default | Meaning |
| --- | --- | --- |
| `WEBBY_KONG_DIR` | `build/web` (`/srv/web` in the image) | directory to serve |
| `WEBBY_KONG_BIND` | `::` (`0.0.0.0` in the image) | address to listen on |
| `WEBBY_KONG_PORT` | `8080` | port to listen on |
| `WEBBY_KONG_DB` | `~/.local/share/webby-kong/saves.sqlite3` (`/data/saves.sqlite3` in the image) | SQLite file: saves, and with sign-in on users, passkeys, sessions and invites |
| `WEBBY_KONG_ROM` | none (`/rom` in the image) | ROM file, or a directory holding one `*.z64`/`*.n64`/`*.v64`, served at `/api/rom` |
| `WEBBY_KONG_AUTH` | off | `1` puts the passkey gate in front of every URL (see below) |
| `WEBBY_KONG_EXTERNAL_URL` | `http://localhost:8080` | the URL people actually open: WebAuthn origin, RP ID host and base of invite links |
| `WEBBY_KONG_RP_NAME` | `Webby` | name shown in the passkey prompt |
| `WEBBY_KONG_SESSION_DAYS` | `30` | session cookie lifetime in days |
| `WEBBY_KONG_REGISTRATION_OPEN` | `1` | seed of the owner's registration toggle (the database wins afterwards) |
| `WEBBY_KONG_SMTP_HOST` | none | invite email SMTP host (unset: the dashboard offers the link to copy instead) |
| `WEBBY_KONG_SMTP_PORT` | `587` | SMTP port |
| `WEBBY_KONG_SMTP_USERNAME` | none | SMTP user |
| `WEBBY_KONG_SMTP_PASSWORD` | none | SMTP password |
| `WEBBY_KONG_SMTP_FROM` | `WEBBY_KONG_SMTP_USERNAME` | envelope sender |
| `WEBBY_KONG_SMTP_TLS` | `starttls` | `starttls`, `ssl` or `none` |
| `WEBBY_KONG_SMTP_TIMEOUT` | `10` | SMTP socket timeout in seconds |

The effective configuration is printed at startup (the SMTP password only as set or unset).

## Optional sign-in (passkeys)

`WEBBY_KONG_AUTH=1` puts a passkey gate in front of everything: splash page at `/auth/`, dashboard at `/`, the game at
`/play`. Left unset (the default) every URL behaves exactly as before.

```sh
docker run -p 8080:8080 -e WEBBY_KONG_AUTH=1 -e WEBBY_KONG_EXTERNAL_URL=https://kong.example.com \
  -v /path/to/dk64.z64:/rom/dk64.z64:ro -v webby-kong-saves:/data ghcr.io/unstripped/webby-kong
```

- Sign-in is WebAuthn (passkey) only, no passwords; the one extra dependency (`py_webauthn`) is installed by
  `mise run serve` and shipped in the image, and with the gate off it is never imported. Browsers refuse passkeys on
  plain `http://` LAN addresses, so serve it over TLS or use `http://localhost`.
- Fresh install (no users yet): the first visitor picks a username, registers a passkey and becomes the owner.
- Open registration is on by default; the owner can close it on the dashboard. Invite links (single use, reusable or
  expiring) keep working while registration is closed.
- Users, passkeys, sessions and invites live in the same `/data` SQLite file as the saves, so the existing
  `-v ...:/data` already persists them.
- Saves belong to their user: the dashboard creates, renames and deletes your save games, and lists every player's
  saves with their progress. The Esc menu's "Exit to dashboard" saves in-game and returns there; playing that save
  again resumes on the spot. A save history from before sign-in (or made with the gate off) becomes the owner's.
  Each save tile exports its latest EEPROM, and "Import save…" creates a new save game from an `.eep` / `.srm`.
- Emailed invites use the `WEBBY_KONG_SMTP_*` settings in the table above; without a host the dashboard offers the
  link to copy instead.

From source: macOS or Linux, git and [mise](https://mise.jdx.dev).

```sh
git clone https://github.com/Unstripped/webby-kong.git && cd webby-kong
mise install
mise run serve                   # syncs and builds (no ROM needed), then serves build/web on port 8080
```

`mise run serve` first runs `mise run sync` (`tools/sync.py`): it fast-forwards the branch, checks out the pinned
`decomp/`, sets up `tools/.venv`, regenerates `build/tree` and `build/gen` when their inputs changed and rebuilds
`build/web`. Every step is skipped when already current, so run it whenever you pull or switch branches.

Open `http://localhost:8080/` and pick the ROM, or set `WEBBY_KONG_ROM` in `.env` (or pass
`--rom /path/to/dk64.z64` after `mise run serve --`) to load it automatically. Details, the manual steps, the
reference decomp build (maintainers, after a decomp bump) and troubleshooting: [docs/building.md](docs/building.md).

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

URL parameters (`drawdist`, `scale`, `msaa`, `interp`, `touch`, `audio3d` and `audiorate` override the Esc menu's
stored setting for that page load):

| Parameter | Effect |
| --- | --- |
| `rom=<url>` | Fetch the ROM from a URL instead of picking it (development: `?rom=/dev-rom.z64` with the ROM copied to `build/web/dev-rom.z64`) |
| `drawdist=1..16` | Draw/spawn distance factor; 1 = N64 distances, default 4 |
| `scale=0..8` | Render scale; 0 (default) = automatic from the canvas height |
| `msaa=0\|1\|2\|4\|8` | Multisampling, default 4 |
| `interp=0..3` | Frame rate: 0 = the game's own 30 fps; 1, 2, 3 = up to 60, 120, 240 fps by interpolated in-between frames (limited by the display's refresh rate); default 1 |
| `touch=1` / `touch=0` | Force the touch controller on or off |
| `audio3d=1` / `audio3d=0` | 3D sound for headphones (positional effects get their direction, front and back included); default on |
| `audiorate=22050\|44100` | Sound mix sample rate; 22050 = N64, default 44100; applies at load |
| `noresume` | Do not boot into the quicksave resume point after a rebuild |

## Repository layout

- `patches/` — changes to the decomp source, one commit per concern. Edit by committing in `build/tree`,
  then `tools/prepare.py export`.
- `tools/gen_data.py` — the decomp defines its static data in C; this emits what is left as splat asm (data only
  handwritten asm reads, and asm `.bss`) and the decomp's ROM-extracted initialisers (`data_inc.us.txt`) as
  placeholders plus a table that fills them from the ROM at start, `OVL_FN` thunks, overlay rename headers,
  `port_abs.h`, overlay dispatchers and a layout self-check.
- `port/ref/` — ROM-free manifest of the reference decomp build (ELF symbols, splat's auto symbols, asm label
  sizes and directive widths) that `gen_data.py` reads; regenerated by `tools/prepare.py ref` after a decomp bump.
- `tools/` — also `prepare.py` (patched tree), `serve.py` (static server, save API and mounted ROM),
  `check_no_roms.py` (ROM guard), `next_version.py` (release version), `wasm_funcs.py`, `link_check.py`.
- `port/` — runtime: fibers for N64 threads, libultra shims, ROM/asset byte-order swappers, renderer, audio.
- `web/` — page and loader.
- `tests/` — headless input scripts (`*.txt`) and `tests/tools/` unit tests for the tooling.
- `.github/workflows/` — CI, releases, the Docker image and the Helm chart.
- `Dockerfile` — ROM-free image build: the page, `dk64.wasm` and `tools/serve.py`.
- `charts/webby-kong/` — Helm chart for the image (one replica, `/data` PVC, optional ROM volume and sign-in).
- `docs/` — building, testing, debugging and architecture guides.
- `decomp/` — the decomp submodule; never edited here.

## Documentation

- [docs/building.md](docs/building.md) — setup, build, serving, decomp bumps, troubleshooting.
- [docs/testing.md](docs/testing.md) — CI checks and headless regression scenarios.
- [docs/debugging.md](docs/debugging.md) — runtime counters, debug panel, quicksave, logpoints.
- [docs/architecture.md](docs/architecture.md) — build pipeline, port runtime and notes on the port.
- [docs/audio-packs.md](docs/audio-packs.md) — making and sharing audio packs (replacement songs and sounds).
- [CONTRIBUTING.md](CONTRIBUTING.md) — branches, commit messages, patches, releasing.

## Releases

Every push to `main` publishes a GitHub Release after CI passes. The first is `v0.1.0`; each later one bumps the
patch version, or the minor / major version when a commit message since the last release contains `#minor` /
`#major`. Each release also publishes the multi-arch (amd64/arm64) image `ghcr.io/unstripped/webby-kong:<version>`
and the Helm chart `oci://ghcr.io/unstripped/charts/webby-kong` (chart version = release version without the `v`,
defaulting to that release's image); every push to `development` publishes `:latest`. Neither contains ROM data.

## License

[CC0 1.0 Universal](LICENSE), the same as the decomp. Exceptions: the npm packages bundled into the page (Preact,
@preact/signals and nipplejs, all MIT; see `package.json`); `port/src/audio/hrir_kemar.c` is derived from the KEMAR
HRTF measurements by Bill Gardner and Keith Martin, copyright 1994 MIT Media Laboratory, free to use with this
citation (<https://sound.media.mit.edu/resources/KEMAR.html>).
