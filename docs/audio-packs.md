# Audio packs

An audio pack replaces some of the game's songs and sound effects with your own audio. Anything a pack does not
replace plays the original audio from the ROM. Audio is never committed to this repository: packs are shared through
a server's dashboard or passed around as files.

Only upload or share audio you have the right to share.

## Using packs

- **Turn packs on.** In the game, press Esc and scroll to **Audio packs**. Every pack on the server and every pack
  imported on this device is listed. **On**/**Off** turns a pack on or off for you only.
- **Priority.** When two enabled packs replace the same song or sound, the higher one wins. **▲**/**▼** move an
  enabled pack up or down. A newly enabled pack goes to the top.
- **When changes apply.** On the next page load, like the audio quality setting.
- **Import a pack** on this device only: Esc → Audio packs → **Import pack…** (a `.zip`). **Remove** deletes it from
  this device.
- **Share a pack** with everyone on a server: sign in to the dashboard and use **Upload pack…** in the Audio packs
  panel. The uploader and the server owner can delete it. Without the sign-in gate there is no dashboard; upload
  with `curl -X POST --data-binary @pack.zip -H 'Content-Type: application/zip' http://<server>/api/packs`.

Quicksaves remember the pack set they were taken with: a quicksave taken with other packs (or none) is not restored;
the game resumes at its resume point instead.

## Finding song and sound ids

Press F2 in the game. The **audio** row shows:

- `seq`: the song (sequence) each of the 4 sequence players is playing, or `–` when stopped. The title music, a
  level's music and its ambience are on different players.
- `sfx`: the last sounds played, newest first.

Play the scene whose audio you want to replace and note the ids. Write them in the manifest in hex (`0x2A`) or
decimal (`42`).

## Pack format

A pack is a `.zip` holding a `manifest.json` plus audio files.

### Zip rules

- Entries are stored or deflated (the default of every zip tool); no zip64, no encryption.
- At most 2000 entries and at most 512 MiB uncompressed in total. An upload is at most 256 MiB.
- Entry names must not start with `/`, contain `\` or contain a `..` segment.
- `manifest.json` sits at the root of the zip, is UTF-8 JSON and is at most 64 KiB.

### Manifest (format 1)

Only the keys below are read; any other key is ignored.

| Key | Rule | Error |
| --- | --- | --- |
| `format` | must be `1` | `format must be 1` |
| `name` | string, 1–60 characters after trimming | `name must be 1-60 characters` |
| `author`, `version`, `description` | optional strings of at most 60, 20 and 500 characters; default `""` | `<field> must be a string of at most N characters` |
| `songs` | optional object: song id → entry | `songs key "<k>" is not a song id (0-175 or 0x0-0xAF)` |
| `sfx` | optional object: sound id → entry | `sfx key "<k>" is not a sound id (1-8191 or 0x1-0x1FFF)` |

- Song ids are 0..175 (`0x0`..`0xAF`), sound ids 1..8191 (`0x1`..`0x1FFF`), written in decimal or `0x` hex
  (case-insensitive).
- Two keys naming the same id fail with `<songs|sfx> id 0x<HEX> is listed twice`.
- A manifest with no songs and no sfx fails with `the pack replaces nothing (no songs or sfx)`.

An entry is an object:

| Field | Rule | Default |
| --- | --- | --- |
| `file` | the exact path of an entry in the zip, ending in `.ogg`, `.oga`, `.opus`, `.mp3`, `.wav` or `.flac` (any case) | required |
| `loop` | boolean | see below |
| `loopStart` | number of seconds ≥ 0 | `0` |
| `loopEnd` | number of seconds greater than `loopStart` | end of the file |
| `gain` | number, 0 < gain ≤ 4 | `1` |

Errors: `<songs|sfx> 0x<HEX>: file "<f>" is not in the pack`,
`<songs|sfx> 0x<HEX>: file must be .ogg, .oga, .opus, .mp3, .wav or .flac`, and for a wrong type or range
`<songs|sfx> 0x<HEX>: <field> is invalid`.

The server checks an upload with these rules, and the game checks an imported pack with the same rules and messages.
Every format your browser can decode works; Ogg Vorbis and MP3 play everywhere.

### Example

```json
{
  "format": 1,
  "name": "Jungle Remix",
  "author": "kong fan",
  "version": "1.0",
  "description": "An orchestral title theme and a softer banana pickup.",
  "songs": {
    "0x2A": { "file": "music/title.ogg", "loopStart": 4.25, "loopEnd": 92.5 },
    "17": { "file": "music/jingle.ogg", "loop": false, "gain": 0.8 }
  },
  "sfx": {
    "0x123": { "file": "sfx/banana.wav" }
  }
}
```

## Songs

- The file plays while the game's sequence with that id plays on any sequence player, and restarts whenever the game
  (re)starts that sequence.
- It follows the game's music and sound effect volume settings and fades. The game's per-song volume is not applied,
  so balance the file itself.
- The original sequence keeps running silently, so game timing (cutscenes, rhythm sections) is unchanged.
- `loop` defaults to `true`: the region [`loopStart`, `loopEnd`) repeats after the file plays up to `loopEnd`. With
  `loop: false` the file plays once and then stays silent.
- Limits: the per-kong instrument layering of some level themes and the underwater filter do not apply to replaced
  songs.

## Sound effects

- The sample replaces that sound everywhere the game uses it, including MIDI songs that use the same sample (ids
  0x800 and up are also the songs' instrument samples).
- The game still plays it through its own synthesizer: pitch, pan, volume, echo and 3D sound work as for the original.
- The original envelope is kept, so the sound lasts as long as the original. Longer audio is cut off; shorter audio
  ends in silence.
- `loop` absent: the sound loops the whole sample (or [`loopStart`, `loopEnd`)) exactly when the original loops, as
  many times as the original. `true` loops for as long as the game plays the sound; `false` never loops.
- Loops shorter than 256 samples at the game's playback rate loop the whole sample instead.

## Loudness

Normalise songs to about -14 LUFS integrated, roughly the level of the original soundtrack at the default volume
setting, then fine-tune with `gain`. Keep sound effects peak-normalised to about -1 dBFS and adjust with `gain`.
