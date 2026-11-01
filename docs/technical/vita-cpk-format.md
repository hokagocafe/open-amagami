# PS Vita release: package and container formats

Confidence levels: **confirmed**, **hypothesis**, **pending**. See [disc.md](disc.md).

Covers `EbiKore+ Amagami (JPN)` (title `PCSG00291`, 2014), the source for the port's new reference (D-020). Everything here is about the Vita package's own containers; the `SCF` language and bytecode are unchanged from [scf-format.md](scf-format.md) and [scf-bytecode.md](scf-bytecode.md) — see "Scripts are unchanged" below.

## Decrypting the package (confirmed)

Every game data file under `app/PCSG00291/` (`eboot.bin`, `AMAGAMI.IMG`, `SCENARIO.CPK`, `graph/GRAPH.CPK`, `sound/*.CPK`, `sound/AMAPSP.ACF`) is wrapped in the Vita's per-title `sce_pfs` filesystem encryption; only metadata (`param.sfo`, `sce_pfs/pflist`) is in the clear. "NoNpDrm" (the distribution's own label) only strips `eboot.bin`'s licence check — it does not touch this layer.

Decrypted with `psvpfstools` (`motoharu-gosuto/psvpfstools`, no licence file in that repository, so it is not vendored here — build and run it outside the repo) given the title's own `zRIF`. It needed three small local patches to build with current GCC/CMake/Boost (missing `<cstring>` includes, a `LIBTOMCRYPT`/`LibTomCrypt` case mismatch in its `find_package` call, and a dependency-ordering bug in its bundled `FindLibTomCrypt.cmake` that left `LIBTOMCRYPT_LIBRARIES` empty) — none of that is kept, it is a one-time local build.

`eboot.bin` still starts with Sony's `SCE\0` SELF header after this — a separate unwrap step (not PFS) stands between it and the plain ARM ELF, done with `self2elf.py` from `rreha/psvdec` (MIT licence, not vendored; needs `pycryptodome`, its `Crypto` import had to be repointed at Debian's `Cryptodome` package name). That tool wants a RIF license file, not a `zRIF` string — and `sce_sys/package/work.bin` (512 bytes, present in the dump already) turned out to be exactly that, so no extra key material was needed beyond what shipped in the package. Result: a valid `ELF32`/`ARM` binary (`readelf`-clean, 5 program headers, no section headers — expected for anything extracted from a SELF).

## `AMAGAMI.IMG`: same format as the PS2 (confirmed)

Byte-identical container to the PS2's system image ([scf-format.md](scf-format.md)): same LZSS scheme (`tools/hokago.py lzss unpack` decompresses it with no changes), same table layout (total size, class count, name offsets, class offsets, `SCF` classes back to back). `tools/hokago.py scf verify-image` round-trips it exactly.

Only difference found: **260 classes** against the PS2's 258.

## `CPK`: a new container, not `AFS` (confirmed)

`SCENARIO.CPK`, `graph/GRAPH.CPK` and `sound/{MUSIC,SOUND,VOICE}.CPK` use CRI's `CPK` container — a different, later CRI format than the PS2's `AFS` ([audio.md](audio.md)). Public, documented format, same standing as `AFS` under CLAUDE.md's CRI black-box rule (the container layout is read, CRI's own code is not touched). `tools/hokago.py cpk` (new) reads it:

```sh
python tools/hokago.py cpk list    SCENARIO.CPK
python tools/hokago.py cpk extract SCENARIO.CPK out_dir
```

Structure: a `CPK ` magic, then one or more CRI "`@UTF`" binary tables — the main header table (one row, with fields such as `TocOffset`/`TocSize`, `ContentOffset`/`ContentSize`, `Files`), and the file table itself (`TocOffset`, one row per file: `FileName`, `FileOffset`, `FileSize`, `ExtractSize`). `@UTF` packets are masked with a simple reversible XOR keystream (seed `0x655F`, multiplier `0x4115` per byte) — not real cryptography, the same obfuscation used across CRI middleware. A file's absolute offset is `min(TocOffset, ContentOffset) + FileOffset`, confirmed by trial against `SCENARIO.CPK` (the two offsets can differ, and only one of them is the right base).

Per-file compression is CRI's own `CRILAYLA` codec (`tools/hokago.py cpk`'s `decompress_crilayla`) when `FileSize != ExtractSize`: a back-to-front LZ77 variant with an 8-bit magic/size header, a bit-packed stream read from the end of the block backward, and a 0x100-byte literal tail copied straight to the output's start. Confirmed by decompressing `AbstractKawa.scf` and getting back a byte-exact `SCF` header.

## Scripts are unchanged (confirmed)

`SCENARIO.CPK` holds **2,486** `.scf` files — the PS2's `SCENARIO.ARC` holds 2,485 ([scf-format.md](scf-format.md)). Same names seen so far: the mahjong classes (`AbstractKawa`, `AbstractNaki`, `AbstractTaku`, `AbstractTehai`, `AbstractYama`, `AgariBox`, `BitCalc`), all 35 `c##_cutin` cut-in windows. The one extra file is `Yama_Debug.scf`; not yet checked whether it is reachable from anything.

`AbstractKawa.scf`, decompressed and parsed with the unmodified `tools/hokago.py scf`, is byte-for-byte the documented `SCF` format: magic, flags, version, instance variables (including Japanese names like `nnn立直Pos`), methods and bytecode, all readable.

**This resolves the open half of D-020/P-9's "same game and scripts are expected":** confirmed, at least for this one class and the file inventory. Full method-by-method comparison against the PS2 classes is not done.

### The system image's engine classes gained methods, not just content (confirmed)

Loading the whole Vita class table (260 image classes via the unmodified `read_image`, all 2,486 scenario classes via `tools/hokago.py cpk extract` fed into `port/src/vm.cpp`'s `AMG_OVERLAY`) into the existing C++ VM works with zero new loader code and **zero parse errors across every class** — strong, broad confirmation that the scenario script format (not just one class) is unchanged.

Execution diverges early, at instruction 2214 of boot (`Application.run` → `K2X_Script.setup` → `.setScreen` → `.setScrOrgY`), with "missing native: K2X_Script primitive 7". This is not a loader bug or a scenario-script difference: `K2X_Script` in the **Vita system image** has 14 class methods against the PS2's 9 — five new ones: `setScreen`, `setScrOrgY`, `setScrScl` (screen setup, presumably for the different native resolution), `unlockTrophy` (Vita trophies), `getClock`. None of these exist in the PS2 `K2X_Script`, confirmed by diffing `cls["class_methods"]` of both images with `tools/hokago.py scf`'s `load_image`/`read_image`. The scenario content is unchanged; the **engine base classes gained Vita-specific capabilities**, each presumably backed by a new native primitive the port's `natives.cpp` has no reason to implement (it only ever targeted the PS2's set).

**Done 2026-10-18, revised 2026-10-19:** primitives 5 to 8 are `getClock`, `setScrScl`, `setScrOrgY` and `unlockTrophy`; `setScreen` has no primitive of its own.

- `setScrScl` and `setScrOrgY` set the port's view; see "Screen and view" below.
- `getClock` answers `[month, day, hour, minute]` of the PC's clock. `ShioriData.serialize` stores it as the save's time stamp, which the load list shows as MM/DD hh:mm. This is the PS2's order; there, the engine stamped it into the header itself.
- `unlockTrophy` is a no-op: trophies have no PC equivalent.

The Vita-only primitive changes of other classes are listed in "Scripts: what the Vita system image changed".

## Audio: a new container layout and two new codecs (confirmed)

Not investigated when D-020 was written; done 2026-10-18. The PS2 side is `AFS` containers of `ADX` (music/ambience) and `AHX` (voice), confirmed in [audio.md](audio.md). None of that carries over:

| File | Container | Codec |
|---|---|---|
| `sound/MUSIC.CPK` (106 tracks), `sound/SOUND.CPK` (ambience) | `CPK`, **ITOC mode** | `RIFF`/`WAVE`, **ATRAC9** |
| `sound/VOICE.CPK` | `CPK` of 16 inner `CPK`s (one per speaker — the same shape as the PS2's "`AFS` of `AFS`") | `HCA` |
| `sound/AMAPSP.ACF`, `sound/SYSSE0.ACB`, `sound/SYSSE1.ACB` | `ACB`/`ACF` (CRI Atom cue/bank metadata, an unmasked `@UTF` table) with an embedded `AWB` | `VAG` (see "Sound effects" below) |

**`CPK` has a second table layout** (`CpkMode=0` in the header row, against the `SCENARIO.CPK`/`GRAPH.CPK` "`TocOffset`" layout): instead of a `TocOffset` to one table with a `FileName` per row, there's an `ItocOffset` to a `CpkItocInfo` table whose `DataL`/`DataH` columns are themselves nested `@UTF` blobs (`CpkItocL`/`CpkItocH`) listing `ID`/`FileSize`/`ExtractSize` only — no filenames, files identified by sequence number. **`tools/hokago.py cpk`'s `read_index` now reads both layouts** (`read_itoc_entries`, added 2026-10-18): files are stored back to back from `ContentOffset` in `ID` order, each padded up to `Align` (2,048 here) — confirmed against `MUSIC.CPK` (106 files) and a `VOICE.CPK` member (73 files) by checking that every computed offset lands exactly on a real file header (`RIFF`/`HCA\0`), and with a full `extract` round trip (`file` identifies the output as `RIFF (little-endian) data, WAVE audio, stereo 48000 Hz`). Entries get a numbered placeholder name (`00000.bin`, …) since the format carries no filenames.

**ATRAC9** is Sony's own Vita codec; confirmed by the exact match of the `WAVE_FORMAT_EXTENSIBLE` subformat GUID in each track's `fmt ` chunk (`47E142D2-36BA-4D8D-88FC-61654F8C836C` = the public `KSDATAFORMAT_SUBTYPE_ATRAC9`).

**HCA** is a CRI codec, not Sony's — publicly documented and widely tooled (`vgmstream` and others decode it). **Checked 2026-10-18: this game's voice files are not enciphered at all** — the header's `ciph` chunk is type `0` ("not enciphered"), read directly off a voice file (`HCA\0`, version `0x0102`, one channel, 48 kHz, `ath` type `0`, `ciph` type `0`, confirmed by locating each chunk tag in the first 96 header bytes). No key to find, unlike the PS2's `AHX` (`scfsi`, [audio.md](audio.md)).

**Neither needs a bit-exact decoder written for this project: `ffmpeg` (already in this environment) decodes both natively** (`atrac9` and `hca` both listed in `ffmpeg -decoders`), confirmed 2026-10-18 by converting real files — one music track (1:21, stereo, 48 kHz) and one voice line (2.35 s, mono, 48 kHz) both came out as clean PCM `WAVE`. Batch-checked at full scale with no failures: all 106 `MUSIC.CPK` tracks, all 9 `SOUND.CPK` loops, all 73 lines of one `VOICE.CPK` speaker. `tools/hokago.py vita_audio` (new) wraps this — the one tool in this project that isn't standard-library only, since it shells out to `ffmpeg`:

```sh
python tools/hokago.py vita_audio MUSIC.CPK out_dir
```

This settles P-9/D-020's audio question in practice: converting is solved.

### Wired into the port: music and ambience (confirmed, F4-40)

**Decided:** pre-convert to PCM at extraction time (`tools/hokago.py vita_audio`), not link `libavcodec` or bundle an `ffmpeg` binary into the shipped port — keeps the port itself dependency-light, same as everywhere else in this project. `AdxStream` (`port/src/audio.cpp`/`.hpp`) gained a `load_wav`/`pcm_only` mode that reads a plain `WAVE` file straight into the buffer `next()` already streams from, so the resampling and mixing code needed no changes — it was already generic over sample rate. `AudioEngine::music_play`/`ambience_play` use this when `root/extracted/sound_wav/` (the tool's output) exists. Verified: real, non-silent audio renders for both a music track and an ambience loop, with sensible start/end behaviour.

Looping: solved. The source `RIFF`s (both ATRAC9 music/ambience) carry a standard `smpl` chunk with sample-accurate loop points, which `ffmpeg` drops when writing its output `.wav`; `tools/hokago.py vita_audio` now copies it over, and `AdxStream::load_wav`/`next()` read and honour it. Verified by watching the output level across several seconds, not just "does audio still come out" — an early version looked like it looped but had actually frozen on a single sample (the loop reset moved the read position back without resetting the play-position counter that decides *when* to loop, so the loop-check re-fired every single sample).

**Track-number mapping: found and confirmed correct — it was not the identity guess.** Traced the single reference to the literal path `"app0:./sound/MUSIC.CPK"` up to its caller, which reads a table at `0x810a818c` indexed by the raw track number, `-1` for none — same 145-entry length as the PS2's own `track_table_` (`SLPS_259.87` offset `0x28B080`), same shape, just a different address because it's the same source recompiled. Track 0, for instance, maps to file **66**, not file 0 — confirming the naive "track number = file index" assumption from F4-40's first pass was genuinely wrong, not just unverified. `AudioEngine`'s constructor now reads this table from the decrypted `eboot.bin` directly, the same way the PS2 side reads its own from `SLPS_259.87`.

### Voices: `VOICE.IDX` and the lip-sync tracks are the PS2's own (confirmed, F4-41)

`sound/VOICE.IDX` exists in the Vita package (missed in the first pass over `sound/`'s file listing) and is **byte-for-byte identical** to the PS2's own `VOICE.IDX` — `cmp` says so outright. This is the lookup from `(speaker, line number)` to an index into that speaker's own archive (`docs/technical/audio.md`), and it needed no changes at all: Vita's per-speaker archives (`Voice00.cpk` … `Voice15.cpk`, each an `ITOC`-mode `CPK` of sequential-ID entries) are indexed the same way, entry for entry.

**Lip sync (`.slp`) is there, and byte-identical to the PS2's.** Each per-speaker archive interleaves, like the PS2's inner `AFS`es, a line's lip-sync track with its audio, at the two indices `VOICE.IDX` gives for `kind` 0 and 1. Checked over all of `Voice01.cpk`: its 12,280 entries are exactly the 12,280 indices `VOICE.IDX` names for speaker 1, every `kind` 1 entry is `HCA`, every `kind` 0 entry is not, and all 6,140 of those lip-sync tracks are byte-for-byte the PS2's own at the same index. Speaker 0 is the exception that misled a first pass: its 73 lines simply have no `.slp` (every `kind` 0 index is `0xFFFF`), and that was over-generalized into "Vita has no lip sync" — wrong, corrected the same day.

The tracks keep one byte per 1/60 s, so the port reads them at 800 samples per byte against the Vita's 48 kHz `HCA` instead of the PS2's 400 per byte at 24 kHz (`AudioEngine::mouth_level`). `tools/hokago.py vita_audio` keeps these entries as raw `.bin` beside the converted `.wav`s (`ffmpeg` can't, and needn't, decode them) instead of counting them as failed conversions, which is what a first full run did — about half of every per-speaker archive looked "failed" for exactly this reason.

**Wired into the port**: `AudioEngine::voice_pcm`'s Vita branch reuses `VOICE.IDX` (`voice_idx_`, loaded the same way for both platforms now) to compute the per-speaker file index, then loads the matching pre-converted `.wav` (same `AdxStream::load_wav` as music/ambience). Two bugs this surfaced and fixed: `voice_play`/`voice_length` had `24000` (the PS2's fixed `AHX` rate) hardcoded in two places — Vita's `HCA` voices are 48,000 Hz, confirmed on real files, so these now branch on platform. Verified: a real line (speaker 0, line 1) plays as non-silent audio, with `voice_length` matching the duration already measured for that exact file earlier in this session (2.33 s).

**Superseded 2026-10-20 (D-021).** All 16 speakers were first converted to WAV, with no failed conversions, but that took 14 GB. The port now decodes HCA itself (`port/src/hca.cpp`, vendored clHCA) from the original files. `tools/hokago.py cpk extract-nested VOICE.CPK extracted/sound/voice` writes them as `speaker_NN/NNNNN.bin`, HCA lines and `.slp` tracks by index, 2.5 GB in all. Checked against ffmpeg on four lines from four speakers: equal to ±1 per sample, apart from the encoder delay of 128 samples (which clHCA drops) and the polarity (opposite in the two decoders, inaudible).

### Sound effects: standard CRI `ACB`/`AWB`, and the samples are `VAG` — the PS2's own codec (confirmed)

**Correction 2026-10-20: the samples are HE-VAG, not the PS2's VAG.** The header version is `0x20001`, filter indices above 4 occur, and the flags byte's high nibble is used. HE-VAG has the same 16-byte blocks of 28 samples, but a 7-bit filter index (byte 0's high nibble plus byte 1's) into 128 four-tap filters, and four samples of history. `tools/hokago.py acb` now decodes it (`decode_hevag`, filter table from vgmstream's `psv_decoder.c`, ISC). The PS2 decoder had clipped the effects: 1,117 saturated samples in cue 12, none with HE-VAG. This is what the menu's broken sound was.

`AMAPSP.ACF`/`SYSSE0.ACB`/`SYSSE1.ACB` turned out to be CRI's well-documented, widely-tooled **Atom** sound format, not anything custom — `AMAPSP.ACF`'s own `VersionString` field says so outright (`"ACF Format/PSP2 Ver.0.85.00"`). `tools/hokago.py cpk`'s `parse_utf` (the same `@UTF` reader everything else uses) reads the `.ACB`'s top-level table directly: standard field names (`CueTable`, `CueNameTable`, `WaveformTable`, `SynthTable`, …) and an embedded `AwbFile` blob — CRI's **AWB** wave bank, magic `AFS2`, a simple sequential-ID container (file count, alignment, then a `u16` id table and a `u32` offset table, both confirmed by reading `SYSSE0.ACB`'s real header: 49 files, 32-byte alignment, offsets landing exactly where the header's own field sizes predict).

**Each waveform inside the `AWB` is `VAG`** — Sony's PlayStation ADPCM, the exact codec the PS2 side's sound effects already use (`decode_vag`, `port/src/audio.cpp`). Confirmed completely end to end on the first sample of `SYSSE0.ACB`: `VAGp` magic 6 bytes in (not at byte 0 — a small AWB-side pad), a standard 48-byte `VAG` header (`data_size`, `sample_rate` fields both present and sane: 39,680 bytes / 48,000 Hz), a real asset name (`M001_Enc_48000_5`), `header_size + data_size` landing exactly on the sample's own end (no slack, no guessing), and decoding that data through the PS2's own `decode_vag` logic (cross-checked in Python) gives plausible, non-trivial PCM (average amplitude ~6,800, full-scale peaks).

### `@UTF` column flags are a bitfield (confirmed)

The flag byte in front of each column is not three fixed storage kinds (`0x10`/`0x30`/`0x50`). It is a bitfield over the low-nibble type:

- `0x10`: the column has a name.
- `0x20`: a default value is stored inline in the schema.
- `0x40`: there is a value per row.

A column that has a default stores nothing per row, even when `0x40` is set as well (`0x70`, seen in ACB tables).

This is the only reading under which every table seen satisfies two checks: the schema ends exactly at `rows_offset`, and the per-row fields add up to `row_length`. The checks cover all CPK TOC/ITOC tables and every nested ACB/ACF table. The earlier three-kind reading misaligned `CueTable` and crashed on `SynthTable`.

`tools/hokago.py cpk` and `port/src/cpk.cpp` both implement the bitfield reading and refuse a schema that does not end at the rows. The C++ and Python `GRAPH.CPK` indexes are identical.

### Note → sample: a cue id is the PS2 note (confirmed, F4-42)

The chain from cue to sample:

1. `CueTable` row: `CueId`, `ReferenceType` (2 = synth), `ReferenceIndex`.
2. `SynthTable` row: `ReferenceItems` is a list of (u16 type, u16 index) pairs; type 1 points at a waveform, type 2 at another synth.
3. `WaveformTable` row: its index is the sample's AWB index.

In `SYSSE0.ACB`, cue ids run from 12 to 60. These are exactly the notes the PS2's `sys_se0` program splits cover, and cue *n*'s waveform is the same sample as PS2 note *n*'s vag. All 49 decoded durations match the PS2's within 10 ms. `WaveformTable.NumSamples` is about 2.18 times smaller than the decoded length for every sample (not explained; not used).

`tools/hokago.py acb extract` writes one `cue_NNN.wav` per cue, with a `smpl` loop chunk when the VAG loops. `SeBank::load_vita` turns that folder into one-note splits, so `se_play(note)` is unchanged.

`SYSSE1.ACB` (bank 1, used only by mahjong) gives 43 cues. It has not been checked against the PS2, because the port's PS2 `SeBank::load` crashes on `sys_se1` (pending, PS2 side).

## `GRAPH.CPK`: one picture format, palette + 8-bit indices (confirmed)

**Correction (2026-10-19) to F4-33/F4-39.** Those entries read the pictures as headerless `RGB888` at width 320 and `RGBA8888` at width 256. Both readings were wrong, and only looked clean by accident: three palette indices read as one RGB pixel give a grey-ish but recognisable picture, because neighbouring pixels have similar indices.

Every one of the 2,608 files follows the same layout, with no header:

1. A palette of 256 colours, 1,024 bytes, alpha 0–255.
2. One 8-bit index per pixel, row by row.

The width is fixed per directory, and the height follows from the size. Every directory's sizes fit exactly (`1024 + width × height`):

| Path | Size | Palette order |
|---|---|---|
| `bg/*.gxt`, `epi/*.gxt` | 960×720 | BGRA |
| `body/*.gxt` (characters, and the `x?_??` extras) | 1024×1536 | BGRA |
| `face/*.tm2`, `eface/*.tm2` | 1024×1024 | RGBA |
| `font/AMV_font18_21*.tm2` | 4-bit, see "Font" | RGBA |
| `system/sysgraph.tm2` | a container, see below | RGBA |
| `face/*.zb`, `eface/*.zb` | layout records, see "Characters" | — |

How each piece was confirmed:

- **Palette order:** the `.gxt` files use BGR and the `.tm2` ones RGB. Confirmed by rendering a background (a school with a dirt field and a blue sky) and a body (skin tones); the other order turns both blue. Confirmed by rendering a face sheet too.
- **Extensions:** neither extension is Sony's `GXT` or a real `TIM2`.
- **Alpha:** the port maps alpha to the PS2's 0–128 scale.

`tools/hokago.py gxt to-png` converts any of these files; `tools/hokago.py gxt sysgraph` converts every interface picture.

### `system/sysgraph.tm2`: the interface pictures (confirmed, F4-43)

This file is the counterpart of the PS2's `GRAPH0.ARC`. It decompresses to 6,293,504 bytes and is laid out as follows:

- **Offsets:** it opens with 762 increasing u32 offsets from the file's start. Entry 0 is empty.
- **Entries:** each entry has the same layout as every `GRAPH.CPK` picture: a palette, then 8-bit indices.
- **Sizes:** the width, height and row pitch of each entry are in a table in the eboot, at virtual address `0x810ADF44` (file offset `0xAE024`). Each record is 16 bytes: `(flag, width, height, pitch)`.

Found by width search, then by the table: the 761 widths found by smoothness match the eboot's table.

**Texture id → picture (confirmed from the code).** A script's `Sprite.new(texture, variant)` works as follows:

1. `0x810DDF94` is an array of 337 pointers, one per texture id.
2. Each pointer leads to a list of pointers, one per variant.
3. Each of those points to an 8-byte record (entry, flag) in the table at `0x810AC158`.

The reading code is at `0x8107FAEA` (`rec = textures[tex][variant]`, `entry = rec[0]`). The size lookup is at `0x8101382A`. The flag's meaning is pending.

**Vita ids are not the PS2's (confirmed).** The system image that creates interface sprites is the Vita's own, so the ids it passes are the Vita's. Ids up to about 60 coincide with the PS2's. After that they drift: −3 for the map, −6 around the clock, other shifts further on.

The port's own code also names some ids, taken from the PS2 engine. `Graphics::engine_tex` translates them, each read from the Vita engine's code:

| What | PS2 id | Vita id | Where it was read |
|---|---|---|---|
| Node icons | `kind + 0x52/0x59` | `kind + 0x4F/0x56` | `0x81092A40` |
| Node marks | `0x61`/`0x62` | `0x5E`/`0x5F` | `0x81092A40` |
| Node extras | `0xAA + low`, `0xAE`, `0xB1 + extra` | `0xA4 + low`, `0xA8`, `0xAB + extra` | `0x81092D0A`, `0x81092D80` |
| Clock (faces and hands) | `0xAF`–`0xB1` | 169–171 | faces confirmed by picture |
| Light ball | 209 | 202 | `0x8100BA6A` |
| Map | 201 | 194 | `0x8100CA2E`, under `NodeMapNaviBase` |
| Balloon | 240/241 | 215 | `0x8100B2C4` |
| Cut-in frame and texture | 320/319 | 321/322 | `0x8100AE44` (`movw #0x141`, `#0x142`), confirmed from code |

**PS2 constant tables.** The PS2 tables the port reads (clock and map icons) were found in the eboot. The clock tables are byte-identical; the map-icon tables hold Vita ids. `Graphics::exe_word` maps each PS2 address to its Vita counterpart.

**The map's dots (hypothesis).** The Vita map is texture 194, 260×260. Its `setMark` (`0x81095138`) writes 4×4 dots of palette index 247–255 at the following position:

- x = 4·(n mod 65)
- y = 4·(n / 65) − 4, two pixels higher in odd columns

These are relative to a base pointer from the texture object. That base was put at (2, 10) to match the picture's own grid; the pointer itself was not traced.

**Balloon (hypothesis).** On Vita, `Balloon.initialize(mark, layer)` has no colour category. Its pictures are texture 215: variants 0 and 1 are white bubbles, variant 2 is the "?".

### Screen and view (confirmed, except where marked)

The Vita's system scripts place things in a 480×272 space, and its pictures have twice that resolution. Evidence:

- `TextWindow` is `(-220, 45, 440×86)` with an 18×21 font.
- Its window picture is 880×172, and the glyph cells are 36×42.

The port does the following:

- **Window:** it draws at 960×544.
- **Positions:** they are scaled ×2·`setScrScl`.
- **Pictures:** they are drawn at `setScrScl` pixels per texel. Plain rectangles and the full-screen texture 0 are in units, so they are scaled ×2.
- **Origin:** `setScrOrgY` is stored but not applied (pending). With either sign, the title and the main menu ended up 32 px off the screen (a band at the top or the bottom); without it they line up exactly. A turned picture scales along its own axes, not the screen's (hypothesis, from the title's sky: an event picture turned 90° that only covers the screen that way).

`K2X_Script.setScreen` switches between `setScrScl(1)` with `setScrOrgY(16)`, and `0.775` with origin 0 (the whole 960×720 picture fits).

**Characters keep the PS2's coordinates (hypothesis).** `Actor`/`KaiwaMain` are byte-identical to the PS2's, positions and zooms included. The port draws a character with its position and size × 272/448, which keeps the PS2's framing. Checked by comparing the same frame on both platforms (`n1dea_ec01A`, frame 900): same pose, same framing. The engine's own constant was not found.

### Characters (confirmed)

- **Body:** a body is one whole picture, `body/<stem>x`, with no layout of its own.
- **Stem:** the eboot's two-level table (`0x810AC0B2`, `0x810CA9CC`) gives stems like `b1_Aa`. The capital letter is the pose and the small one the cloth, as in the PS2's stem plus cloth letter. The index is `row·264 + pose·12 + cloth`. Confirmed by the names and by the uniform in the frame comparison.
- **Face layout:** `face/<stem>z.zb` uses the PS2's 32-byte records (dx, dy, sx, sy, w, h as floats, then a group as u32). The groups are:
  - 1 and 2: the two eyes
  - 3: the mouth (28 of each)
  - 4: the sweat drop
  - 5: the blush

  The PS2 keeps groups 4 and 5 in the body's layout. The layout is in texels; the scripts' offsets are doubled.
- **Extras:** `body/x{l,s}_{A..}{a..}x`, the same names as the PS2's `stand/`.

### Font (confirmed)

`font/AMV_font18_21.tm2` and its `im` variant are laid out as follows:

- **Palettes:** four 16-colour palettes (white, blue, pink, dark grey; the same roles as the PS2's) in the first 1,024 bytes.
- **Pixels:** 4-bit, 1,024 wide (512 bytes per row), 6,600 rows.

Glyph *n* is the 36×42 cell at column n mod 28 and row n / 28. The eboot's lookup (`0x8101389E`) computes *n* from Shift-JIS exactly as the PS2 does, with 28 per row (the divisor `0x92492493`) and a cell height of 42.

### Scripts: what the Vita system image changed (confirmed)

The scenario scripts are the PS2's. The system image (`AMAGAMI.IMG`, 260 classes) is the Vita's own, and the port treats it as such. The changes were found by matching method names between the two images:

- **Primitive numbers that moved:**
  - `ControlPad`, class side: Vita 2 to 6 = PS2 5, 6, 3, 2, 4.
  - `ExtraPicture`: `spin` is new at 13, so `skip` and `pause` move to 14 and 15.
- **Classes that only exist on Vita:**
  - `GalleryPicture`: `ExtraPicture`'s methods with the same numbers.
  - `TouchPanel`: the Windows port maps mouse taps and drags to touch input; pinch and rotation remain unsupported (see [port.md](port.md)).
  - `Gallery`.
- **`MemoryCard`:** new numbers and signatures, listed in `natives.cpp`.
  - `writeShioriData(head, body, slot)` and `writeSystemData(buf, flag)` write separately.
  - `readShioriBody(buf, slot, checksum)`.
  - `checkData` returns the card itself.
  - The console's message box (`displaySysMsg`, `waitSysMsgEnd`) ends at once.
- **Save headers:** one `Serializer` record for all 20 slots, `[slot 0 … slot 19, checksums]`, against the PS2's 256-byte record per slot. The `Serializer` format is the same on both: tagged values (`N` nil + 1 byte, `B` + 1, `I` and `F` + 4, `S` + 4-byte length + bytes, `A` a nested record) up to a 0 byte. A PS2 header converts by nesting it as an `A`. The port writes the Vita's own `vita_*` files and imports the PS2's when those are missing (F4-48).
- **Texture rectangles:** `setTexRect` takes units, and a Vita picture has two texels per unit (F4-49).
- **Extra pictures (hypothesis):** drawn at (480/640, 360/448) of their zoom (F4-50).
- **`Balloon.initialize`:** takes 2 arguments, not 3.
- **Buttons (corrected 2026-10-22):** the system image reads SceCtrl's layout; the scenario scripts (mahjong's table, the events) still test the PS2 mask. Confirmed in Vita3K on the mahjong table: the d-pad left and right move the cursor and triangle opens the menu, up and square do nothing, which only fits `& 32768` and `& 16` as the PS2 values. The port gives a scenario script the PS2 mask and the image classes SceCtrl's, chosen by the calling class (hypothesis: how the eboot decides is not known). The text that follows describes the SceCtrl side: the scripts read SceCtrl's layout (circle `0x2000`, up `0x10`, …), which is the PS2 engine's mask with its two bytes swapped. `VM::to_script_buttons` swaps them. The PS2's L1/R1 also set the triggers (`0x100`, `0x200`).

No other primitive has a different argument count; every primitive wrapper of both images was compared.

`SCENARIO.CPK` is read directly by the loader (`vm.cpp`). No `AMG_OVERLAY` is needed any more.
