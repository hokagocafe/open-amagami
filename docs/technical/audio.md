# Audio

Confidence levels: **confirmed**, **hypothesis**, **pending**. See [disc.md](disc.md).

## Overview

| File | Container | Contents | Codec |
|---|---|---|---|
| `SOUND/MUSIC.AFS` | AFS | 106 music tracks (`CLS_M01_Edit.adx`, `CLS_M03.adx`, …) | ADX, encrypted |
| `SOUND/SOUND.AFS` | AFS | 9 ambience loops (`ENV0001.adx` to `ENV0009.adx`) | ADX, encrypted, 24 kHz mono |
| `SOUND/VOICE.AFS` | AFS of AFS | 16 inner archives, one per speaker; 48,252 voice lines | AHX, plus one `.slp` file per line |
| `SOUND/VOICE.IDX` | — | Lookup table from (speaker, line number) to file | — |
| `SOUND/MUSIC.ARC` | `ARC` | Two sound-effect banks (`sys_se0`, `sys_se1`), Sony HD/BD format | Pending |

## AFS (confirmed)

Standard CRI container. All integers little-endian.

| Offset | Type | Field |
|---|---|---|
| `0` | 4 bytes | Magic `AFS\0` |
| `4` | `u32` | File count `n` |
| `8` | (`u32`, `u32`)`[n]` | Offset and size of each file |
| `8 + 8·n` | `u32`, `u32` | Offset and size of the directory |

- File data starts at the next multiple of 2,048 after the table, and every file is padded with zeros to a multiple of 2,048.
- The directory comes last: one 48-byte entry per file, with a 32-byte zero-padded name followed by 16 bytes of metadata (not interpreted; kept verbatim when repacking). Some music names are Shift-JIS.

`tools/hokago.py afs verify` rebuilds each archive from its parsed contents and compares. `SOUND.AFS`, `MUSIC.AFS`, `VOICE.AFS` and the inner voice archive checked all come out byte-identical.

## Voices

### Layout (confirmed)

`VOICE.AFS` contains 16 files, all named `VOICE.AFS`, each an AFS archive for one speaker:

| # | Code | Lines | | # | Code | Lines |
|---|---|---|---|---|---|---|
| 0 | `ETC` | 73 | | 8 | `RIS` | 1,502 |
| 1 | `TUK` | 6,140 | | 9 | `UME` | 2,224 |
| 2 | `RIH` | 5,526 | | 10 | `MAY` | 547 |
| 3 | `CAO` | 6,180 | | 11 | `KAN` | 487 |
| 4 | `SAE` | 5,477 | | 12 | `MAN` | 385 |
| 5 | `AII` | 5,625 | | 13 | `KAO` | 686 |
| 6 | `HAR` | 5,775 | | 14 | `AYU` | 39 |
| 7 | `MIY` | 2,555 | | 15 | `YUK` | 5,031 |

The codes read as character names (hypothesis): `TUK` Tsukasa, `RIH` Rihoko, `CAO` Kaoru, `SAE` Sae, `AII` Ai, `HAR` Haruka, `MIY` Miya, `RIS` Risa, `UME` Umehara.

Inside a speaker's archive each line is a pair of files in this order: `<code><number>.slp` then `<code><number>.ahx`. The number has four digits. `ETC` has only `.ahx` files.

### `VOICE.IDX` (confirmed)

112,600 little-endian `u16` values in two parts:

1. **Group table:** 16 speakers × 100 entries. Entry `speaker·100 + number/100` is the index of the group holding that hundred of line numbers, or `0xFFFF` if the speaker has no lines there.
2. **Groups:** 555 groups of 200 entries. Entry `(number mod 100)·2 + kind` is the file's index inside the speaker's archive, or `0xFFFF`. `kind` is 0 for `.slp` and 1 for `.ahx`.

Groups are numbered in the order their first file appears, going through the speakers in order.

`tools/hokago.py voice_index verify` rebuilds the whole file from nothing but the names inside `VOICE.AFS`, and the result is byte-identical to the original.

### `.slp` files (layout confirmed; engine use hypothesis)

A headerless lip-sync track: one byte per 400 samples of the paired AHX, so **60 bytes per second** at 24 kHz, one per NTSC field.

- **Rate (confirmed).** Over all 47,282 pairs, `len(slp)` is `ceil(samples / 400)` minus 1 (29,757 lines), minus 2 (16,580) or exactly it (616); `samples` is the AHX header's big-endian count at offset 12. Mean 59.6 bytes per second.
- **Byte layout (confirmed by statistics).** Bit 7 is always set (no byte is below `0x80`). Bits 0–1 take only the values 0, 1 and 2, never 3. Bits 2–6 are a loudness 0–31.
- **Bits 0–1 are a mouth level (confirmed by statistics).** Level 0 dominates at loudness 0, level 2 at high loudness, and between consecutive bytes the level only moves by one step: 0↔2 never happens across 9.6 million transitions. That is a quantised, hysteresis-smoothed mouth state (closed, half, open), not a raw amplitude. Tracks start and end at `0x80` (silent, closed).
- **Engine use (hypothesis).** `objects.md` gives the lip-sync mouth frame as `2 − level`; level 2 (open) gives frame 0 (open), level 0 gives frame 2 (closed). The read of the `.slp` byte inside the executable has not been traced; `ActorPicture.lipSync` is primitive 17 and only toggles the feature.

**Port (implemented, see port.md; timing against the emulator pending).** While a character's voice plays, take `i = played_samples // 400` and `level = slp[i] & 3`, and show mouth frame `2 − level` (in phase terms: level 2 → phase 0, level 1 → phase 1, level 0 → phase 2). Past the end, or with `lipSync` off, fall back to the `setMouth` phase. The loudness bits are not needed. `tools/hokago.py slp` decodes a track and implements this mapping.

### AHX (confirmed by decoding; hypotheses marked)

CRI AHX: MPEG-2 audio layer II, mono, 24 kHz, inside an ADX-style header (encoding type `0x10`, total samples big-endian at offset 12, `(c)CRI` copyright). Frames start with the fixed header `FF F5 E0 C0` and have no fixed length: a frame ends where its bits end, padded to a byte. The file ends with `80 01` and an `AHXE(c)CRI` footer.

- **Fixed allocation.** All 41,715 frames sampled use the same bit allocation (subbands 0-3 at 127 levels, 4-7 at 127, 8-10 at 63, 11-23 at 9, 24-27 at 5, 28-29 at 3). Frame size only changes with the number of scale factors.
- **Encryption (flag `0x08` at header byte `0x13`).** The scale factor selection (`scfsi`) of subbands 1 to 8 is XORed with a 16-bit mask chosen by the `scfsi` of subband 0: `0x0000`, `0xD4A5`, `0xF605`, `0x7FE5` (two bits per subband, subband 1 in the top bits). Confirmed: with these masks every frame of 397 checked parses to exactly its length, and every grouped sample code is in range. How the masks derive from the key string is pending; they are kept as constants.
- **Check.** A decoded line has silence at both ends, a speech envelope, no clipping and a low-frequency spectrum. Listening: confirmed by the owner on 2026-10-14 (voices play in the port with lip sync).

The port decodes in `port/src/ahx.cpp`; `amagami_vm --ahx SPEAKER:FILE_INDEX:OUT.wav` writes one file.

### Voice primitives (confirmed from the scripts; hypotheses marked)

**Stalls (2026-10-26).** The window makes the sound one script frame at a time on the game's thread and queues it for the device (so voice and music progress follow the script frames). Two things made a short gap: a Vita music or ambience track is read and decoded whole (372 to 459 ms for the title music) when it starts, on the game's thread, and any slow frame (a picture loading, a font drawn the first time: 25 to 100 ms in the title and menus) left the device's queue empty. Now tracks load on a thread of their own in the window (`AudioEngine::set_async_tracks`; the old track stops at once, the new one starts when ready, `music_busy` is true meanwhile, a newer request or a stop drops an older load; the command line tool loads in line so its renders stay deterministic), and the queue keeps a cushion of 50 ms that grows by 30 ms, up to 150 ms, each time the device runs dry (`win_main.cpp`). Also: the two font sheets are read at start (a 29 ms stall on the first text), and a picture's palette is expanded once, not per texel (a background went from 19 to 4 ms). Still on the game's thread: the character pictures of the title (`ExtraPicture.1`, two of 20 ms in one frame: the CRILAYLA decompression of a 1.5 MB sheet, then the composition), which the cushion covers for the sound but not for the picture. Not measured by ear on a long session yet.

A `Voice` object has a `device`: the actor number, which is also the speaker's archive in `VOICE.AFS` (actors are created in the archive's order, `PLY` being 0 and using `ETC`). Primitives: `setup(n)` 0 prepares line `n`, `playPrim(n, volume)` 1, `stopPrim` 2, `setVolumePrim` 3, `pause(flag)` 4, `length(n)` 5 (used to pace the text; the port returns 1/60 s units, hypothesis), `play2Prim` 6, `stop2` 7 and `setVolume2Prim` 8 on a second channel (backlog, name entry), and the class primitives `stop` 9 and `stop2` 10.

## ADX

### Format (confirmed for the header; hypothesis for exact sample values)

Big-endian header:

| Offset | Size | Field |
|---|---|---|
| `0x00` | 2 | Magic `0x8000` |
| `0x02` | 2 | Offset of the `(c)CRI` marker; audio data starts 4 bytes after it |
| `0x04` | 1 | Encoding type (3) |
| `0x05` | 1 | Block size (18 bytes) |
| `0x06` | 1 | Bits per sample (4) |
| `0x07` | 1 | Channels |
| `0x08` | 4 | Sample rate |
| `0x0C` | 4 | Sample count |
| `0x10` | 2 | High-pass frequency (500), used to derive the predictor coefficients |
| `0x12` | 1 | Version (4) |
| `0x13` | 1 | Flags; `0x08` means the block scales are encrypted |
| `0x24` | 16 | Version 4 loop data: flag, start sample, start byte, end sample |

Audio is a sequence of 18-byte blocks, one per channel in turn. Each block is a 16-bit scale followed by 32 signed 4-bit samples. A sample is `nibble × (scale + 1)` plus a prediction from the two previous samples.

The decoder follows the commonly published description of ADX. It has not been compared against a reference decoder, so individual samples may be off by a rounding step.

### Encryption (confirmed)

Every ADX file on the disc has the encryption flag set. Only the block scales are affected: each one is XORed with the next value of a small pseudo-random sequence.

- The sequence is `x ← (x × multiplier + increment) mod 2^15`, advanced once per block.
- Its three parameters are derived from a **key string** through a table of the 1,024 primes above 2^14. The table is at `0x290890` in the executable and matches the generated list exactly.
- The game's key string is **`mituba`**, giving start 23063, multiplier 20639, increment 23549.

The key was found by trying every string in the executable: `mituba` is the only one for which all decrypted scales fall in the valid 13-bit range. With it, an ambience file decodes with no clipped samples; with a wrong key, more than half the samples clip.

## How the game plays music (confirmed, from the executable and the scripts)

- `Music.play(n)` in a script calls the primitive with `n + 72` unless the "classic" flag is set (`Music.setClassic`); the primitive looks the number up in a table of 145 signed words at `0x28B080` in the executable. The value is the index of the file in `MUSIC.AFS`, or −1 for none. Track 1 is entry 0 (`CLS_M01_Edit.adx`); two sets of 72 tracks share the files with a few differences, which is how "classic" and the new arrangement are told apart.
- The music files are 48 kHz stereo (the ambience files, 24 kHz mono). Most have loop points in the version 4 header (flag, start sample, start byte, end sample), and the player loops between them; jingles have none and `Music.isBusy` turns false when they end.
- `Music.setVolumePrim(v)` takes 0 to 100: the script computes `volume × base × (base + 5) / 50` with `base` from 0 to 5.
- `Sound.playPrim(n)` plays an ambience loop: a table of ten words at `0x28B038` maps `n` (1 to 9) to the file in `SOUND.AFS` (entry `n - 1`, confirmed), and `Sound.playSEPrim(note, volume)` plays an effect from the loaded `sys_se` bank (see below).
- Names the engine builds for sequenced music, `BGM%02dSQ.sq`, `BGM%02dBank.hd` and `BGM%02dBank.bd`, are not used by the tracks above and are probably for the sequencer's own banks (pending).

## Sound effect banks (confirmed structure; hypothesis where marked)

`MUSIC.ARC` holds two banks in Sony's PS2 sound bank format, `sys_se0` and `sys_se1`, each a `.hd` header and a `.bd` body. The engine uses them through Sony's MIDI library: `Sound.playSE(n)` becomes a MIDI note-on for note `n` (`0x90 | note << 8`) with a velocity from the volume, and `Sound.loadSE(bank)` uploads a bank. Bank 0 is loaded at start; only the mahjong game loads bank 1.

**Header (`.hd`).** A chain of chunks, each with an 8-byte magic written backwards (`IECS` is `SCEI`), a size, a highest index and a table of offsets (relative to the chunk) to its records:

| Magic | Reads as | Record |
|---|---|---|
| `IECSsreV` | `Vers` | Version, 16 bytes |
| `IECSdaeH` | `Head` | Total size of the header, size of the body, and the offsets of the other chunks |
| `IECSigaV` | `Vagi` | One per sample: `u32` offset in the body, `u16` sample rate, attribute byte, one reserved byte |
| `IECSlpmS` | `Smpl` | 42 bytes per sample: `u16` index of its `Vagi`, volume, pan, root note (byte 11), ADSR words |
| `IECStesS` | `Sset` | 6 bytes per sample set: velocity range and the `u16` index of its `Smpl` (bytes 4 and 5) |
| `IECSgorP` | `Prog` | Programs: a block with the address of the split blocks (relative to the program), their count and size (`0x14`), volume and pan; then one split per entry: `u16` sample set, lowest note, crossfade, highest note, … |

In both banks every note from 12 up has its own split with one sample set, one sample and one voice, with the root note equal to the note, so a note plays its sample at the recorded rate. The scripts' effect numbers (31, 36, 37, 39, 40, 44, 50, 17, …) are those notes.

**Body (`.bd`).** PlayStation SPU ADPCM: 16-byte blocks, a shift and filter byte, a flag byte (bit 0 ends the sample, bit 1 makes the end loop, bit 2 marks the loop start) and 28 samples of 4 bits. Both banks decode to plausible lengths (a click is about 3,200 samples, the longest effect about 110,000) at 39,000 Hz as stored in `Vagi` (hypothesis: the field is a rate in hertz; the value is odd for a PS2 game).

**Not modelled:** the envelope words (the effects play with a flat envelope and stop at the end of the sample), pan, and the priority and group rules that cut one effect when another starts. Each is a field of the records above and the next step if an effect sounds wrong.

## Tools

```sh
python tools/hokago.py afs list    extracted/SOUND/MUSIC.AFS
python tools/hokago.py afs extract extracted/SOUND/MUSIC.AFS out/music
python tools/hokago.py afs pack    out/music rebuilt.AFS
python tools/hokago.py afs verify  extracted/SOUND/MUSIC.AFS

python tools/hokago.py adx info    out/music/CLS_M03.adx
python tools/hokago.py adx to-wav  out/music/CLS_M03.adx CLS_M03.wav

python tools/hokago.py voice_index list   extracted/SOUND/VOICE.AFS
python tools/hokago.py voice_index verify extracted/SOUND/VOICE.AFS extracted/SOUND/VOICE.IDX
```

The ADX decoder is plain Python and slow: fine for checking a track, not for converting the whole soundtrack.

## No sequencer (confirmed 2026-10-15, F4-8d)

The music is all streamed ADX. `Music` has six primitives (set up, play a track, stop, pause, volume, busy), none of them takes a sequence, and `MUSIC.ARC` holds only the two sound-effect banks (four files, `.hd` and `.bd` each). There are no `.SQ` sequences on the disc, so the `BGM...SQ` sequencer of the plan has nothing to play and is dropped.

## ADX decoder check against FFmpeg (F1-8)

`tools/hokago.py adx_check` removes the scale encryption and compares `tools/hokago.py adx` with FFmpeg sample by sample.
Result (confirmed): 4 music tracks (48 kHz stereo) and the 2 ambience tracks of `SOUND.AFS` (24 kHz mono)
are bit-exact. The predictor coefficients are **rounded** (`round(c*8192)`, `round(-c*c*4096)`), not floored:
at 48 kHz both give the same values, at 24 kHz they differ by one and floor gave errors up to 70.
Fixed in `tools/hokago.py adx` and `port/src/audio.cpp`. FFmpeg's rounding is the reference; that the game's own
decoder rounds the same way is a hypothesis (it matches the encoder-side convention, no hardware check).
