# Testing

## CI

`.github/workflows/ci.yml` runs on every push to `development`, on every pull request, and before every release from
`main`. It has no ROM, so it cannot build or play the game; it runs the ROM-free checks below. Each can be run
locally from the repository root:

| Job | Proves | Local command |
| --- | --- | --- |
| `rom-guard` | No committed file is a ROM, build output (`.wasm`), starts with an N64 ROM header, or is larger than 2 MiB | `python3 tools/check_no_roms.py` |
| `patches` | `patches/*.patch` apply to the pinned `decomp/` source | `python3 tools/prepare.py check` |
| `checks` | Python and JavaScript parse, shell scripts parse, tooling unit tests pass | see below |

```sh
python3 -m compileall -q tools tests/tools
for f in web/*.js; do node --check "$f"; done
sh -n tools/link_check.sh && sh -n .githooks/pre-commit
python3 -m unittest discover -s tests/tools -v
```

Because CI cannot run the game, the headless scenarios below are a required local step before merging to `main`.

## Headless runner

```sh
node build/web/dk64_headless.js --rom decomp/donkey-kong.z64 --frames 2400 --input tests/title_to_menu.txt
```

Every 60 frames (VI ticks) it prints `frame <N>` and the runtime counters as JSON (VI ticks, frames, gfx and audio
tasks, map, mode, overlay, audio peak, last error, actor count, player count, player 1's position). Flags:

| Flag | Effect |
| --- | --- |
| `--rom FILE` | The ROM (required) |
| `--frames N` | Frames to run (default 600) |
| `--input FILE` | Scripted controller input (format below) |
| `--lax` | Log asset tables without a byte-order swapper instead of aborting |
| `--trace-assets` | Log every asset load with its RDRAM range |
| `--trace MASK` | Print game trace events (`PORT_TRACE`): 1 = map transitions, 2 = camera paths, 4 = actor spawns |
| `--qs-save F --qs-load G` | Snapshot after frame F, restore it after frame G and replay from F |
| `--resume slot,kong,map,exit,hasPos,x,y,z,yaw` | Boot straight into a map at a position |
| `--drawdist K` | Draw/spawn distance factor 1..16 (default 4; 1 = N64) |
| `--audio-rate 22050\|44100` | Synthesizer rate (default 22050 = N64) |
| `--hrtf 0\|1` | 3D sound for positional effects (default 0; on changes only audio, never game state) |
| `--progress` | At the end, print the current level's collectable progress |
| `--net-sim N` | Run in co-op lockstep with N players (1..5): every controller read takes all pads from the netplay ring, fed from the input file two reads ahead; slot s plays kong s |
| `--net-join F` | With `--net-sim`: slots 1..N-1 join at frame F (default 0), which reloads the map with them |
| `--hash-every K` | With `--net-sim`: print `hash <read> <state hash>` every K controller reads (default 30) |

Input files: lines starting with `#` are comments; every other line is `frame <N> press|release <button> [pad <P>]`
or `frame <N> stick <x> <y> [pad <P>]` (x and y clamped to ±80), with buttons
`a b z start l r dup ddown dleft dright cup cdown cleft cright` and pads 0..4 (default 0; pads 1..4 only matter with
`--net-sim`); at most 256 events.

## Regression scenarios

| Scenario | Command arguments (after `--rom decomp/donkey-kong.z64`) | Expected |
| --- | --- | --- |
| Title to main menu | `--frames 2400 --input tests/title_to_menu.txt` | Last line has `"map":80` |
| New game intro | `--frames 21000 --input tests/new_game_intro.txt` | Ends in the Treehouse: `"map":171,"mode":0` |
| Snapshot determinism | the intro run plus `--qs-save 11000 --qs-load 12500 --trace 3` | Trace events after the load match the plain run with `--trace 3` |
| Kong swap barrel | `--frames 1500 --input tests/kong_swap_barrel.txt --resume 0,0,7,0,1,1642,330,1519,0` | No abort; ends on `"map":7` |
| Co-op | `--frames 1800 --net-sim 5 --input tests/coop_japes.txt --resume 0,0,7,0,1,1642,330,1519,0 --trace 1` | No abort; a `coop roster map=0x7 players=5` trace; ends on `"map":7` with `"players":5`; two runs print identical `hash` lines |
| Co-op join | the co-op run plus `--net-join 900` | A map 7 reload (`coop roster ... players=5`) after frame 900; ends with `"players":5` |
| Co-op snapshot | the co-op run plus `--hash-every 5 --qs-save 1000 --qs-load 1300` | Every `hash` line after the load equals the plain run's line for the same read |

The intro's map changes run on wall-clock cues, and patch 0012 keeps checking them after a camera path ends early
(the port runs a steady 30 fps, so frame-counted paths finish sooner than on hardware). The kong swap barrel run
selects a kong in the Japes kong swap barrel twice; each swap reloads the player model.

The co-op runs start lockstep at boot; the first controller reads happen in Japes, so the map reloads once with the
five kongs (DK, Diddy, Lanky, Tiny, Chunky) at DK's position, then pads 1–4 walk them apart. A difference in `hash`
lines between runs means a lockstep desync (rerun with `--hash-every 1` to find the first diverging read).

To reach gameplay without the intro use `--resume`, e.g. `--resume 0,0,0xb0,1,1,941,60,736,3752` (Training Grounds).

The intro and menu runs never move DK, so collision and pickup changes also need a test in the browser.
