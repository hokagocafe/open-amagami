# Save data

How the game saves and loads, as seen from the decompiled scripts (`scripts-out/image/`), and a proposed format for the port. The memory card side of the engine (the native code behind `MemoryCard`'s primitives, Sony's `libmc`) is a black box and was not read: everything below about the card itself is inferred from what the scripts pass and expect.

**Current state of the port (2026-10-26, confirmed in the code).** The port keeps the Vita's files in the save folder: `vita_system.bin`, `vita_heads.bin` (every slot's header in one record) and `vita_slotNN.body`. The PS2 layout described below (`slotNN.head`, a 256-byte record per slot, `system.bin`) is no longer read or written (D-036); it stays here as the account of how the scripts were first understood.

Confidence levels: **confirmed** (read directly from the script code), **hypothesis** (inferred, not tested), **pending**.

## Overview

The scripts own the whole format. Natives only move opaque byte blobs between a `ByteArray` and the card. Three blobs exist:

| Blob | Built by | Restored by | Buffer size on load | Count |
|---|---|---|---|---|
| Slot header ("shiori head") | `ShioriData#serialize` | `ShioriData#restore` | 256 bytes per slot, read all at once as 5120 | 20 |
| Slot body ("shiori body") | `GameParam.makeShioriBody` | `GameParam.restoreShioriBody` | 28416 bytes | 20 |
| System data | `GameParam.makeSystemData` | `GameParam.restoreSystemData` | 49152 bytes | 1 |

*Shiori* (栞) means bookmark: it is the game's word for a save slot. Confirmed (buffer sizes and calls from `ShioriData.load`, `LoadMenu`, `MainMenu`, `Succession`).

The game asks for 640 KB of free card space (string in `MemoryCard`'s constant pool). Confirmed as a message; how the native code lays the files out on the card: see "The card as the console writes it" at the end of this document (confirmed for the original game, pending for ebKore+; not needed by the port).

## `MemoryCard` (11 primitives)

| # | Method | Script contract |
|---|---|---|
| 0 | `isChanged` | `false` when the card state is stable; the watcher thread only polls `status` then |
| 1 | `status` | Integer state code, see below |
| 2 | `createFileSystemPrim` | Creates the save directory on the card; 0 on success |
| 3 | `format` | Formats the card; 0 on success |
| 4 | `readSystemData(buf)` | Fills `buf` (49152 bytes) with the system blob; 0 on success |
| 5 | `readShioriHeadPrim(buf)` | Fills `buf` (5120 bytes) with the 20 slot headers, 256 bytes each; 0 or an error code (thrown as `MemoryCardError`) |
| 6 | `writeAllData(slot, head, body, system)` | Writes one slot (header and body) and the system blob together; 0 on success |
| 7 | `readShioriBody(slot, buf)` | Fills `buf` (28416 bytes) with that slot's body; 0 on success |
| 8 | `writeSystemData(buf)` | Writes the system blob only; 0 on success |
| 9 | `hasOldData` | `true` if the card holds a save of the original *Amagami* (not ebKore+) |
| 10 | `succeedOldData` | Converts/copies that old save into the ebKore+ save; 0 on success |

Method names and argument shapes: confirmed. That return value 0 means success: confirmed (every caller tests `== 0`). The exact meaning of nonzero codes: pending.

Status codes, from the watcher loop `MemoryCard#run` and the message tables:

| Code | Meaning | Confidence |
|---|---|---|
| 1 | No card in slot 1 | confirmed (message) |
| 2 | Card ready and the ebKore+ save exists (`isReady`) | confirmed |
| 3 | Transient (busy); listeners are not told | hypothesis |
| 4 | Unformatted (`isUnformat`) | confirmed |
| 5 | Not enough free space | hypothesis (mapped to the "not enough space" message) |
| 6 | Card usable but no ebKore+ save yet; `createFileSystem` is called before the first write | hypothesis |

`isOK` is `status` 2 or 6. Confirmed.

## The serializer

`Serializer.serialize(array)` turns a nested array into bytes; `Serializer.restore(bytes)` does the reverse. Confirmed. The format is a sequence of tagged values ended by a zero byte:

| Tag | Value | Payload |
|---|---|---|
| `N` (0x4E) | `nil` | 1 byte, ignored |
| `B` (0x42) | Boolean | 1 byte, 1 = true |
| `I` (0x49) | Integer | 4 bytes, `Integer#toByteArray` |
| `F` (0x46) | Float | 4 bytes, `Float#toByteArray` |
| `S` (0x53) | String | 4-byte little-endian length, then the bytes (Shift-JIS) |
| `A` (0x41) | Nested array | a whole serialized array, with its own terminating 0 |
| 0 | end of array | |

Tags and structure: confirmed. Byte order of `I` and `F`: hypothesis, little-endian like the string length (the payload goes through the `ByteArray` natives `toByteArray` / `asInteger` / `asFloat`). An unknown tag throws `SaveDataFormatErrorException`.

Because restore functions check `length() > n` before reading later fields, older and shorter arrays load fine. That is how the game reads *Amagami* saves and earlier ebKore+ versions. Confirmed.

## What each blob holds

### Slot header (`ShioriData#serialize`)

`[update(4 ints, all 0), surname, given name, date, time, [RenaiMapParam per heroine]]`: what the save/load list shows. A slot is used if `update[0]` is not nil. Confirmed.

The decompiler names `ShioriData`'s class variables wrongly in class methods (it shows `self.time` and `self.mapParam` where the class variables are `shioriList` and `curShioriNum`). The meaning is: 20 `ShioriData` objects and the current slot number.

### Slot body (`GameParam.makeShioriBody`): the playthrough

A 10-element array. Confirmed:

0. Player surname, 1. given name
2. `date` (calendar day), 3. `time` (time slot, 0 to 13), 4. `place`
5. `wallPaper`, 6. `miya` (8 flags)
7. `Heroine.makeShioriBody()`: per-heroine state (`[tempColor heroine numbers, fontColor heroine numbers, per-heroine shiori body, voiceSE]`)
8. `EventNode.makeShioriBody()`: one integer per event node, `index*1024 + openDate*16 + state*4 + (subst+1)`, plus three more `EventNode` class variables
9. `morning`: an already serialized blob kept as is (nil if absent)

So per-playthrough "flags and variables" are not a generic variable table: they are the event graph state (which events are open, done, substituted, on which date) and per-heroine route state. Confirmed for the structure; the meaning of `state` and the heroine sub-fields: pending.

### System data (`GameParam.makeSystemData`): shared across slots

A 10-element array. Confirmed:

0. Current slot number
1. Config (`getConfig`, 22 entries): music, voice, sound and SE volumes; classic music and voice switches; vibration; message speed; auto mode, auto sync, auto wait; skip modes A and B; map grid; departure; `miyaCheck`; `clearBonus`; mahjong speed, cut-in, auto mode, tsumo menu, naki menu
2. `Character.getAlbum()`: gallery unlocks
3. `Heroine.makeSystemData()`: text speed settings and per-heroine data (clear marks, per `getClear`)
4. `EventNode.makeSystemData()`: one integer per node, `index*2048 + block*2 + known`. **This is the read mark** ("already seen" for skip-read and the map), plus two class variables
5. `secret`
6. Player surname, 7. given name (last used)
8. `MahjongBase.makeSystemData()`
9. `OmakeActor.makeSystemData()`: extras

`makeSystemData(old)` (one argument) merges with an existing blob instead of overwriting it, used when *Amagami* data is carried over. Confirmed.

## Flows

All confirmed from the scripts:

- **Boot** (`K2X_Script` init): builds a default system blob and body (`makeSystemData`, `makeShioriBody`) and keeps them in `K2X_Script` class variable 72 as the reset state.
- **Title** (`MainMenu`): if the card is OK, `ShioriData.load` reads the 20 headers, then `readSystemData` and `restoreSystemData`. Shows "carry over" if `hasOldData`.
- **Save** (`SaveMenu`): `ShioriData.set()` fills the header from the current game, then `writeAllData(slot, header, makeShioriBody(), makeSystemData())`. Every slot save also rewrites the system data.
- **Load** (`LoadMenu`): `readShioriBody(slot)` and `readSystemData`, then `throw RestartException(2, [system, body])`; the restart path in `K2X_Script` calls `restoreSystemData` and `restoreShioriBody`, rebuilds the executable node list and continues.
- **System save** (`SystemSave`, `ConfigSave`, after clearing a route or changing settings): `writeSystemData(makeSystemData())` only.
- **Carry over** (`Succession`): `succeedOldData`, reload headers and system data, patch, write system data back.

## Port today

Implemented in `port/src/natives.cpp` (F4-10, 2026-10-15) as the format below. Earlier the port stubbed only primitives 0, 1, 2 and 9 (ready card, no data); with status 2 the title then called 5 and 4, got the receiver back instead of 0, and printed `memcard shiori header error` / `system load failed`.

Argument shapes, checked against the scripts (confirmed):

- Every buffer is a `ByteArray` (an array of byte integers in the VM). `writeAllData` and `writeSystemData` check `isKindOf(ByteArray)` before the primitive and throw `IllegalArgument` otherwise; the natives check again and return nonzero.
- Read buffers are allocated by the caller at a fixed size (`ByteArray.new(49152)`, `ByteArray.new(28416)`, and `readShioriHead(5120)` which allocates and then calls primitive 5). The primitive fills them in place; the scripts then pass the whole buffer, trailing zeros included, to `Serializer.restore`, which stops at the first terminating 0.
- Write buffers come from `Serializer.serialize`, sized exactly (`ByteArray.new(total)`), so the payload stored is exactly the blob. The slot argument of `writeAllData` and `readShioriBody` is `ShioriData.getNum()`, an integer 0 to 19.
- The default system blob at boot is 23890 bytes (printed `makeSystemData size=23890`), well inside the 49152 buffer.

Two engine facts the save path needed, found while testing:

- The image declares `ByteArray : Object`, yet `StringInputStream#initialize` (used by every `Serializer.restore`) rejects anything not `isKindOf(ArrayedCollection)`, and `Serializer.serialize` stores the nested `morning` ByteArray through the same test. So the PS2 engine must treat `ByteArray` as an `ArrayedCollection` (its siblings `Array` and `UShortArray` are declared so, all three with flags 8). The port re-parents `ByteArray` under `ArrayedCollection` at start-up. Hypothesis for how the engine does it; confirmed that the scripts require it.
- `ByteArray#asInteger` and `#asFloat` (used by `Serializer.parseInteger` / `parseFloat`) were missing; added as the little-endian inverses of `Integer#toByteArray` / `Float#toByteArray`. Little-endian matches the PS2 CPU: hypothesis until a PS2 save is decoded.

Tests (2026-10-15, headless `amagami_vm --save-dir`):

- Empty folder: status 6, the title takes the "no ebKore+ save" path (`memcard error` in the log is just the listener's label) and the boot goes on to the prologue as before. Confirmed.
- From the main menu, Config, change one value, leave and accept the save: `ConfigSave` calls `createFileSystem`, `makeSystemData` and `writeSystemData`; `system.bin` appears (23906 bytes: 16-byte header plus the 23890-byte blob) and the watcher prints `laststat=6 stat=2`. Confirmed.
- Next run with that folder: status 2, `readShioriHead` returns 0 with all headers zero (20 empty slots, no error), `readSystemData` returns 0, `restoreSystemData` runs without error and the title continues. Confirmed.
- Slot save and load (`writeAllData`, `readShioriBody`, the restart path): driven by hand in the window on 2026-10-15 (F4-10b); there is no automated test yet (F4-23).

## Port format

Keep the script-level blobs byte for byte, and implement the 11 primitives as file operations. Then no script changes, saves stay compatible with the game's own restore logic, and a PS2 save could later be imported by extracting the same blobs. Built as described here (F4-10).

Location: `%APPDATA%\open-amagami\save\` on Windows (roaming, as Windows games usually do; read from the `APPDATA` variable), `./save` elsewhere. `--save-dir DIR` or the `AMG_SAVE_DIR` variable override it.

```
%APPDATA%\open-amagami\save\
    system.bin          system data blob (serializer bytes, as written by the script)
    slot00.head ... slot19.head   256-byte header blobs
    slot00.body ... slot19.body   body blobs
```

Each file starts with a small header so a bad or foreign file is rejected cleanly:

```
0  4  magic "OASV"
4  2  format version (1)
6  2  kind (0 system, 1 slot header, 2 slot body)
8  4  payload length
12 4  CRC-32 of the payload
16 .. payload (exactly the bytes the script passed in)
```

Primitive mapping:

- `status`: 2 if `system.bin` exists, else 6. Never 1, 4 or 5 (no card can be missing or unformatted). `isChanged` false. `createFileSystemPrim` creates the folder. `format` returns 0.
- `readShioriHeadPrim`: concatenate the 20 header payloads into the 5120-byte buffer, zero-padding each to 256 and leaving missing slots zero (an all-zero header restores to fewer than 6 elements, so `ShioriData` re-initializes it: confirmed from `ShioriData#restore`).
- `readShioriBody`, `readSystemData`: copy the payload into the buffer, zero-filled beyond it; nonzero if the file is missing or the check fails.
- `writeAllData`, `writeSystemData`: write to `name.tmp`, flush, then rename over the old file (`std::filesystem::rename`, which replaces it), so a crash never leaves a half-written save. `writeAllData` writes body, then header, then system data: a header never points at a body that is not on disk. The three files are not one transaction. A header payload over 256 bytes, or any read payload larger than its buffer, fails instead of being cut.
- `hasOldData` false and `succeedOldData` nonzero, until an import of PS2 saves exists.
- Error codes are the port's own: -1 missing file, -2 bad header, CRC or size, -3 write failure, -4 bad argument. The scripts only test `== 0`.

### The card's message box (Vita image, 2026-10-31; confirmed headless on both layers)

On the console, `MemoryCard.displaySysMsg` / `waitSysMsgEnd` (Vita primitives 10 and 11) open the system's own dialog,
which picks its text by the card's status. The port used to end both at once, so Continue with no save slid the Load
icon in and out with no word (`LoadMenu.displayError`; the log said `memcard error`). Now primitive 10 starts a script
thread `PortMessage.show(card, kind)` (`port/script/PortMessage.scfs`) and 11 answers true while it lives, so each
caller's own wait loop waits as on the console; no game method is patched. `PortMessage` shows the game's
`ConfirmDialog` (layer 15, with the OK button, mode 1) with the game's own line, waits for confirm or back
(`ControlPad.waitOX`), plays sound 46 and closes, as the game's own "loaded" messages do. The kind is the calling class,
read from the thread's frames: `LoadMenu` uses `printStatusForLoad`, `SaveMenu` and `SystemSave` `printStatusForSave`,
anything else (`MemoryCardCheck` at boot) `printStatus`. When the chosen line is empty (status 2: the save exists but a
read failed), the line is the table's own "loading failed" (15) for Load, "save failed" (11) for the saves and "system
data loading failed" (19) at boot (`PortMessage.fallback`, native, reads `MemoryCard`'s constant table).

Status 1 to 8 and the lines (index arrays of the Vita `MemoryCard`: load `[0,1,0,0,2,5,5,6,7]`, save
`[0,1,0,4,2,4,0,6,7]`, plain `[0,1,0,0,2,3,5,6,7]`; Japanese / the owner's English patch, which translates the table):

| status | what the port can produce | Continue (load) | save | at boot |
|---|---|---|---|---|
| 1 no card | `AMG_CARD_STATUS=1` only |There is no save data. | same | not called |
| 2 data present, read fails | a damaged `vita_system.bin` |Loading failed. (fallback) | (fallback) | (fallback) |
| 3 data, no room | `AMG_CARD_STATUS=3` | no call (`isExisted`) |Not enough space on memory card. | not called |
| 5 no room | `AMG_CARD_STATUS=5` |This memory card does not contain save data. |Not enough space | not called |
| 6 no save yet | an empty save folder |This memory card does not contain save data. | no call (`isOK`) | not called |
| 7 broken | `AMG_CARD_STATUS=7` |This save data is corrupted. | (SaveMenu and SystemSave overwrite: no call) | same line, before the main menu |

Seen: status 6 and 7 and the boot fallback (a garbage `vita_system.bin` with `amagami_vm`) on the Japanese copy, status
6 on the English copy. With garbage in `vita_heads.bin` too, Continue opens the Load list with every slot empty (status 2,
the headers read as empty), no message: the console would do the same as far as the scripts show. Tests:
`tests/replays/continue_no_save.json`, `continue_broken.json`. The English table has every line, so no port text was
needed (`ui_text.cpp` untouched).

Open points (pending):

- Whether the buffers the scripts allocate can be smaller than what a long playthrough produces (they are fixed: 28416 and 49152). The port writes any size but fails the read if a payload does not fit, rather than truncating.
- The byte order of `Integer#toByteArray` in the port's `ByteArray` natives must match what the original wrote, if PS2 saves are ever imported.
- Whether the PS2 card files contain the blobs raw or with their own wrapping (needs a real save from PCSX2's memory card file).
- Behaviour of nonzero error codes in the game's dialogs.

## The card as the console writes it (2026-10-16, F1-11)

`tools/hokago.py mcard` reads a PS2 card image (`Mcd001.ps2` of PCSX2: 528-byte pages, FAT, 512-byte directory entries) and extracts a save directory. The user's card holds a real save of the **original** *Amagami* (`SLPS-25918`); there is none of ebKore+ yet. What it shows:

- **Directory content (confirmed, real card).** `BISLPS-25918/` holds three files: `icon.sys` (964 bytes), `amagami.ico` (14,380 bytes) and a data file with the directory's own name, `BISLPS-25918` (623,616 bytes).
- **The executable names the same three for ebKore+ (confirmed, strings at 0x28AE80..0x28AF70):** directory `/BISLPS-25987`, data file `BISLPS-25987`, icon `amagami.ico` (the game extracts it from `mc_icon/amagami.ico` in `GRAPH0.ARC`), `icon.sys` with the title in Shift-JIS (エビコレ＋ アマガミ), and the old directory `/BISLPS-25918/*` that `hasOldData` and the carry-over read.
- **The data file holds the script blobs raw, with no wrapping (confirmed on the old game):** it starts with the serializer's own tag bytes (`41 49 04 00 00 00 ...`, an array of integers), and 21 blocks of 29,696 bytes follow one after another: 20 slot bodies (a block with a save is 23 to 24 KB, an empty one is zeros) and a last block of 23,143 bytes that is probably the system data. Total: 21 x 29,696 = 623,616.
- **Coincidence worth checking (hypothesis):** the sizes of ebKore+'s buffers (5,120 of headers + 20 x 28,416 of bodies + 49,152 of system data) add up to 622,592, which is 1,024 less than the old file. The old file has no separate header table that the analysis can see, so the ebKore+ file may be laid out differently from the old one. It cannot be told without a real ebKore+ save.
- **Consequence for the port:** the port keeps its own three files (`system.bin` and the rest, see above), which the scripts cannot tell from the card. Writing a PS2-compatible card file needs the ebKore+ layout, hence a real save: make one in PCSX2 with the ebKore+ disc (any slot, at any save point), and `python tools/hokago.py mcard Mcd001.ps2 BISLPS-25987 out/` gives the file to compare with the port's blobs.
