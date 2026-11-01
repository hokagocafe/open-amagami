# The port's script machine

`port/` holds the Windows port. Its first piece is the **script virtual machine**, a C++17 rendition of the reference interpreter `tools/hokago.py scfvm`. It reads the game's scripts straight from the extracted disc, runs `SystemStartup.run()` and prints what the scripts print.

Status:

- **Script machine:** confirmed equivalent to the reference interpreter over 40 million instructions of the real boot (logos, memory card check, title, menu, name entry, action map), with every printed line identical, and the same uncaught script error at the same instruction when graphics are off.
- **Screen objects:** `gfx.cpp` is a port of the reference model. With `--shot`, the frames at the title opening, title, menu, name entry and action map are identical **pixel for pixel** to the reference model's, and so are the object lists written beside them.
- No sound, no window yet: the renderer writes PNG files. The engine primitives that are not screen objects are the same headless stubs the reference uses.

## Playing it on Windows

`amagami.exe` is a Win32 front end (`port/src/win_main.cpp`) with no dependency beyond the system libraries: a window that shows the frame, the keyboard as the controller, and 60 Hz pacing. The script machine runs on a worker thread; the window thread only paints.

```
amagami.exe --root <folder that has extracted/> [--scale N]
```

**PS Vita data (D-020).** The same binaries run the Vita release when `extracted/` holds that release's files. The port switches on `extracted/graph/GRAPH.CPK`. It needs:

- `AMAGAMI.IMG`, `SCENARIO.CPK` and `graph/GRAPH.CPK`;
- `eboot_decrypted.elf`;
- `sound/VOICE.IDX`;
- the audio converted by `tools/hokago.py vita_audio` (music, ambience) and `tools/hokago.py acb extract` (`sound_wav/se0`, `se1`) under `sound_wav/`;
- the voices under `sound/VOICE.CPK` plus `sound/VOICE.IDX`, or extracted HCA files at `sound/voice/speaker_XX/*.bin` / `extracted/sound/voice/speaker_XX/*.bin` plus `sound/VOICE.IDX`; the port decodes HCA itself. An optional `sound/VOICE_P.CPK` plus `sound/VOICE_P.IDX` overlays mapped voice and lip-sync entries before the base archive. `AMG_VITA_VOICE_DIR` points elsewhere for base voices.
- music and ambience as `sound/MUSIC.CPK` / `sound/SOUND.CPK` or prepared WAVs under `extracted/sound_wav/music/` and `extracted/sound_wav/ambience/`;
- sound effects as `sound/SYSSE0.ACB` / `sound/SYSSE1.ACB` or prepared cue WAVs under `extracted/sound_wav/se0/` and `extracted/sound_wav/se1/`.

On Vita data the port changes its behaviour as follows:

- **Screen:** it is 960×544.
- **Buttons:** they are given to the scripts in the Vita's layout.
- **Primitives:** the Vita system image's own primitives are used.

The launcher recognizes either packed or prepared sound sources. When importing, it recursively copies `sound/`, `extracted/sound/` and `extracted/sound_wav/`, preserving their relative paths; the runtime selects extracted voices from either supported voice directory and applies mapped `VOICE_P` entries ahead of base voices. On the owner's Vita3K English export, all 558 non-empty audio/lip-sync references in `VOICE_P.IDX` were confirmed to point to entries in the corresponding speaker archives. For Vita3K folders, the port reads static engine tables from `app/PCSG00291/eboot.bin`; the optional patch eboot changes those table contents and is not a compatible source for the port's address maps. The English export completed the `n1dea_ev01A` smoke scene and produced a non-silent WAV; this does not replace a full owner-run playback test. The formats and the mapping are in [vita-cpk-format.md](vita-cpk-format.md).

**The port is Vita-only (D-036, 2026-10-26, plan F5-24).** The PS2 data layout and its readers are gone from `port/`: the sound back end (AFS archives, ADX, the PS2 sound banks, AHX voices; `--adx`, `--se` and `--ahx` of the command line tool with them), the scenario archive reader, the drawing module's PS2 archives, TIM2 pictures and texture records, and every `vita ? a : b` choice in it. Each stage was checked against the Vita result: the WAV of a scene byte for byte, 15 screenshots of 9 scenes by hash, and a sweep of the scenes against the earlier results. What is left is the natives' PS2 primitive numbering with its Vita remap and the PS2-address keys of the engine tables (they are the Vita's tables found through the PS2's addresses, F4-56); the PS2 run of the port (`--root` on a disc's `extracted/`) no longer works, and `tools/sweep.py`'s old PS2 results are history. Everything removed is in git history.

Scene sweep on Vita data: `python3 tools/sweep.py --exe <build>/amagami_vm --root <vita-root> --out build/sweep-vita` (`--out` keeps it apart from the PS2 results); `--report --out build/sweep-vita` reads it. Result on 2026-10-20 (confirmed, rerun after the merge): DONE 2,110, DONE-STATE 5, ERROR 5, the PS2's result.


| Key | Button | Key | Button |
|---|---|---|---|
| arrows | d-pad | Enter | start |
| Z | circle (confirm) | Backspace | select |
| X | cross (cancel) | Q / W | L1 / R1 |
| A | square | E / R | L2 / R2 |
| S | triangle | Esc | quit |

**F8** opens the port's settings over the game (the launcher's Settings window without the game-folder buttons and the game's language): the game waits while it is open (`g_paused`; the sound device is not counted as running dry), and the rows apply at once. Alt+Enter or F11 toggles borderless full screen on the window's monitor (the cursor is hidden there). In both modes the frame keeps the aspect ratio of `SCREEN_W` x `SCREEN_H` and is letterboxed with black bars; it is drawn pixel-sharp when the picture is a whole multiple of the screen size and smoothed otherwise.

An XInput pad (Xbox-style; `xinput1_4.dll`, falling back to `xinput9_1_0.dll` and `xinput1_3.dll`, loaded at run time so the build needs no SDK library) works together with the keyboard; every connected pad is merged into the same controller mask, read once per game frame:

| Pad | Button | Pad | Button |
|---|---|---|---|
| D-pad, left stick | d-pad | Start | start |
| A | circle (confirm) | Back | select |
| B | cross (cancel) | LB / RB | L1 / R1 |
| X | square | LT / RT | L2 / R2 |
| Y | triangle | | |

The stick counts past about half its travel. Status: builds with MSVC x86 without warnings in `win_main.cpp`; pending a test with a real pad and a full-screen session on screen.

### Reading the game's own folder (F6-1, started 2026-10-22)

`--root` can be the Vita game folder as a dump has it (`AMAGAMI.IMG`, `SCENARIO.CPK`, `graph/GRAPH.CPK`, `sound/`, `eboot.bin`), a Vita3K export folder containing `app/PCSG00291/` and optionally `patch/PCSG00291/`, or the prepared `extracted/` tree with `extracted/eboot_decrypted.elf` (`port/src/datapath.hpp`: `data_file` overlays the Vita3K patch over its app tree; `vita_exe` reads the prepared ELF or unwraps `eboot.bin` in memory). The launcher accepts either executable layout too; its “Game data” check used to require `eboot.bin` even though import and execution accepted the prepared ELF (fixed 2026-10-25). The executable is read in any decrypted form (2026-10-26, checked with synthetic variants of the owner's file): the SELF `eboot.bin` of a decrypted dump, the same SELF with its segments zlib-compressed (flag 2; `inflate.hpp`), or a plain ELF under any of the names `eboot.bin`, `eboot.elf`, `eboot_decrypted.elf` (at the root or in `extracted/`). The version check hashes the unwrapped ELF as well as the file, so one build matches one table entry in either form (`af3a79...` is the ELF of the table's `eboot.bin`); a build not in `known_versions.txt` gives the non-blocking warning, except the English patch's own `eboot.bin` (736 bytes longer, its tables moved by different amounts, so no fixed offset recovers them; D-034), which is listed as "not usable" and fails the launcher's Executable check with the instruction to use the original game's (checked 2026-10-26 with a synthetic folder). The folder the player picks may be above the game's own (`find_game_root`: the first folder up to four levels down holding `AMAGAMI.IMG` and `SCENARIO.CPK`, so a decryption tool's title folder or a Vita3K `ux0` works). The static tables are found through the ELF program headers (`vita_va_to_off`), not through the segment layout of one build. The eboot must have plain segments: the unwrap is a copy of the ELF header, the program headers and the segments to their file offsets, and it gives the same bytes as the ELF the tools made (checked 2026-10-22); a retail eboot (encrypted segments) is refused with a message, the port does not decrypt. Confirmed with the owner's decrypted package: the window runs at 60 fps and the scene sweep command reaches DONE on a test scene. **Voices (done 2026-10-22):** the port reads `sound/VOICE.CPK` itself (`cpk_read_entries` handles the ITOC layout; the outer archive holds one inner archive per speaker) and `sound/VOICE.IDX` from the same folder (the Vita package has it); an event scene rendered to WAV with the voices read this way is byte-identical to the one with the extracted `sound/voice` folder (`n1dea_ev01A`, 4000 ticks, music off). **Music and ambience (done 2026-10-22, D-024):** `AudioEngine::vita_track` reads the converted `sound_wav` file if it exists, else the ATRAC9 RIFF from `sound/MUSIC.CPK` / `sound/SOUND.CPK` and decodes it with the vendored LibAtrac9 (`port/src/at9.cpp`; the decoder's output is ffmpeg's negated, so it is negated back). An event scene rendered to WAV from a folder that holds only the raw CPKs differs from the render with the prepared files by at most 1 level over 567,420 samples (`n1dea_ev01A`, 4000 ticks). **Sound effects (done 2026-10-22):** `SeBank::load_acb` reads `sound/SYSSE0.ACB` / `SYSSE1.ACB` itself (the @UTF reader of `cpk.cpp` now also serves ACB tables and their blob columns; AFS2 wave bank; VAG and HE-VAG decoded as `tools/hokago.py acb` does, with the filter table in `hevag_coefs.inc` generated from it): bank 0 gives the same 49 samples, and a scene rendered to WAV from a folder with only the raw sound files differs from the render with the prepared files by at most 1 level. **Checked on Windows (2026-10-22):** `amagami.exe --root` on the owner's decrypted package folder runs at 60 fps with the effects loaded and the music playing from the CPK. **Found along the way:** the English patch's image adds script-level touch methods (`TouchPanel.getScreenDrag`, `checkTapP`, used by the text window and the backlog) that call the same primitives; `checkDrag` now answers a float id and `checkDragEnd` false for an id that never was a drag, or the patch's code subtracted from an unset variable.

### The program's icon

**Icons (2026-10-25, owner's decision):** the program file carries our mascot (`port/art/amagami.ico`, from `port/art/icon.svg`, the site's logo), which the public builds ship. The launcher, the game window and the shortcuts show the game's own home screen icon instead, read at run time from the player's copy (`sce_sys/icon0.png`, the Vita's snowflake; the import copies it) and written as `game-icon.ico` beside the imported copy (`shortcut.cpp`, a 32-bit bitmap .ico); a copy without it keeps the mascot. Nothing of the game ships with the program (D-012). The earlier `assets/icon/game-icon.jpg` is no longer used.

### Importing the player's copy (v0.1)

`amagami.exe` with no `--root` runs the folder remembered in `%APPDATA%\open-amagami\config.txt` (`root=`, and `source=` with where the copy was imported from, `lang=` for the language layer). With nothing remembered it asks for the player's PS Vita copy, checks it (`amg::check_game_folder`) and offers to import it into `%LOCALAPPDATA%\open-amagami\game` (`amg::import_game`: the data files, `eboot.bin` and `sound/` copied as they are, since the port decodes the console's formats itself; a file already there with the same size is kept, so re-importing is quick) or to run it in place. `--import FOLDER` imports from a given folder, `--reimport` asks for another one. `--root FOLDER` still runs a folder as it is (development).

### Language layer (F5-6)

`amagami.exe --lang CODE` (or `AMG_LANG=CODE`) makes the port look in `<root>/lang/CODE/extracted/` before `<root>/extracted/` for `AMAGAMI.IMG`, `SCENARIO.CPK` and `graph/GRAPH.CPK`; whatever the layer does not hold (sound, voices, the eboot) comes from the base data. The layer for English is the owner's copy of the fan patch's three files (`~/amg-vita-en/extracted` in WSL has them), copied into the Vita data folder; none of it is in the repository. Confirmed 2026-10-22 on the name entry screen. Saves are shared between languages.

### Mouse touch input (F5-8)

On Vita data, the left mouse button supplies touch input to `TouchPanel`. The cursor is mapped from the fitted, letterboxed game viewport to the scripts' centred coordinates; a press must begin inside the picture. A short release is a tap, and moving more than six script units while held starts a drag. Losing window focus or mouse capture cancels the gesture. The gallery interaction still needs a live verification. Pinch and rotation primitives remain unsupported; no script use has been confirmed, and native touch handling in the Vita executable is still an open question.

Confirmed on 2026-10-14 by playing the real boot from the window: logos, title, menu, name entry, the action map and the first line of the prologue, at 58 to 60 frames per second. The first ten seconds are spent building the event map before the first frame.

The window draws smoothly (bilinear texture sampling, snow that glides rather than hops, and a 1 ms timer plus a short spin for frame pacing). The title bar shows the frame rate and the worst frame time of the last second. The comparison with the reference model turns smoothing off, because the reference samples with the nearest texel.

## Sound

`audio.cpp` reads the music and ambience from the Vita's `MUSIC.CPK` and `SOUND.CPK` (ATRAC9, decoded whole when a track starts), the voices from `VOICE.CPK` (HCA), and the effects from the sound banks (`SYSSE*.ACB`), and mixes them in software at 44.1 kHz stereo (linear resampling from the files' 48 kHz; the PS2's AFS/ADX/AHX back end was removed, D-036). The window plays the mix through WinMM `waveOut`; the command line machine can write it to a file with `--wav`.

- The decoder is **bit-exact** against `tools/hokago.py adx` on a whole music file (7.8 million samples, no difference), including the encrypted scales.
- Primitives done: `Music` (`playPrim`, `stopPrim`, `pause`, `setVolumePrim`, `isBusy`) and `Sound` (ambience loops, `loadSE`, `playSEPrim`). The effect banks are decoded once when loaded (PlayStation ADPCM) and mixed as short voices; checked with `--wav` that the menu sounds appear at the frames where the scripts play them. `amagami_vm --se BANK[:NOTE:OUT.wav]` lists a bank and writes one note. Not yet: the effects' envelopes and the sequencer.
- Lip sync (implemented; matches the emulator: pending). `Voice.play` on channel 0 also loads the line's `.slp` track (`VOICE.IDX` kind 0). `AudioEngine::mouth_level(speaker)` returns `slp[position / 400] & 3` from the mixer's play position (24 kHz samples), or -1 when nothing plays or the track has ended. `Graphics::mouth_level` is a `std::function` hook that `main.cpp` (with `--wav`) and `win_main.cpp` (unless `AMG_NO_AUDIO`) point at the engine; without it the mouth follows `setMouth` as before, so headless runs are unchanged. Each frame an `ActorPicture` with `lipSync` on asks for the level of its character number and, when it is 0 or above, draws mouth frame `2 - level` instead of its phase. The speaker is the picture's character number: `Actor` creates its picture with `cnum` and its `Voice` with `anum`, and the two are equal because `K2X_Script` creates the actors in `cnum` order 0 to 34 (hypothesis that holds for every actor the game creates there).
- Checked with `--wav`: the opening and menu music appear in the mix with sensible loudness; whether it sounds right to a listener is not something this environment can judge.

### Measuring performance

The title bar shows the game's frame rate (script frames per second), the rate at which pictures reach the screen, the average time spent drawing and the worst time between two game frames in the last second. Three environment variables help to find a slowdown: `AMG_PROFILE=1` writes `build/profile.csv` (frame, interval, draw and copy times, one row per frame), `AMG_PROFILE_PRIMS=1` writes `build/slowprim.log` with every primitive that takes more than 3 ms, and `AMG_NO_AUDIO` / `AMG_NO_SMOOTH` switch the sound and the smooth drawing off.

**Finding a stall (2026-10-26).** With `AMG_PROFILE=1` the window also writes to stderr, and with `AMG_PROFILE_PRIMS=1` that goes to `build/slowprim.log` (cwd = the program's folder): `SLOWFRAME` (a frame over 30 ms and what the machine's own part of it took), `SLOWPRIM` and `SLOWNATIVE` (a primitive or a native call over 3 ms; `sleep` is left out, it is the thread switch), `SLOWCLASS` (a script class defined, with every method decoded), `SLOWPICTURE` (a picture the game had to wait for, and whether it had been preloaded) and `UNDERRUN` (the sound device ran dry: samples got, cushion, frame). `AMG_BANDS=N` (1 to 8, default 4) sets the number of threads that draw one frame. `python tools/drive_window.py ROOT OUT` plays the title and the Settings with these on (`AMG_SOUND=1` keeps the sound; the default is silent) and `python tools/shot_hashes.py FILE` records the hashes of captures of a few scenes, to show that a change did not alter what is drawn. Measured on the Japanese dump: the extra pictures were preloaded under the PS2's names, so each one was decoded and pasted into its canvas on the game's thread (two frames lost at the title when the girls appear); they are now preloaded and composed on a worker, and the title shows no slow frame. One stretch of script is long, 5.7 million instructions in about 0.5 s right after the splash (the system data being built, mostly `Serializer.serialize` and `Vector`), at 80 ns an instruction; the sound is now made during it (`VM::on_busy`, every 65,536 instructions, paid back by skipping frames later), which removed the 21 underruns it caused. Backgrounds and event pictures the game had to wait for (over 2 ms, not preloaded by a script) are written to `warm.txt` beside `config.ini` as they happen, at most 12 names, and the next start decodes them one after the other on a worker while the splash is up (`Graphics::warm`; three title backgrounds measured on a Japanese copy are always in the list). The main menu's background had cost 4 to 41 ms on the game's thread; with the list it no longer shows in the log. `AMG_NO_WARM=1` skips the warm-up. The splash no longer comes before the machine starts: the game's first 31 frames run behind it, unpaced, with no drawing and no sound queued (`HIDDEN_TICKS` in `win_main.cpp`), and the splash's four seconds are then waited out; so the half second of script is spent while the splash is up, and the game's own first fade starts right after it (checked with `SHOTS_AT=1,2.5,3.7,4.3,5 python tools/drive_window.py ...`, which captures the boot). The 31 is where that stretch ends on a Japanese copy; on other data it only means the freeze is not hidden. A sample of where the 0.5 s goes (a thread reading the machine's instruction pointer against the linker map): a third of the time is in the system's heap functions and the rest is spread over the interpreter loop, `Value` copies, hash lookups and vector construction, with no single hot spot. The first line of each speaker read that speaker's table out of `VOICE.CPK` on the game's thread (68 ms at the first line of a new game); the tables are now read for all 16 speakers at the start on a helper (`AudioEngine::warm_voices`), and that line costs 5 ms. Later in a play session the voices cost 40 to 60 ms a time: a line being prepared on the helper held `voice_mutex_` for its whole decoding, and the line the game asked for at that moment waited for it. The decoding now runs outside the lock (`voice_inflight_` makes a second asker wait for the same line, not for the lock), and the same session shows voice calls of 4 to 7 ms. A cache of method lookups and a pool for argument lists were tried and gave 4 % on a heavy scene, so they were not kept.

Findings of 2026-10-14 on the development laptop (4 cores, Ryzen 5 3450U, with a browser and an editor using about half of it):

- Drawing a frame takes 3 to 15 ms, close to the 16.7 ms budget when several large pictures cross-fade.
- **The game and the drawing are separate.** The machine runs a tick, hands the drawing thread a snapshot of the screen (`Graphics::scene`) and waits for the next 60 Hz beat; the drawing thread renders the newest snapshot (in four horizontal bands on four threads) and shows it. If drawing falls behind, snapshots are replaced, so a slow frame costs a frame on screen and never slows the game. The game runs at 60.0 fps with the worst gap between ticks at 17 to 20 ms; the picture reaches 58 to 60 per second.
- Windows slows minimized or background processes (EcoQoS) and ignores their timer request; the front end opts out of both (`SetProcessInformation`, `SetThreadInformation`), raises its priority a notch, and skips drawing while minimized. Minimized, the game runs at exactly 60.
- Pictures named by a script's `load` are decoded on another thread ahead of use.
- OS threads were the cause of random 50 to 140 ms stalls: handing the baton between fifty of them hit the system scheduler. Fibers removed that.

## Building on Windows

With the Visual Studio C++ tools (Community 2026 is on the development machine), for 32-bit:

```bat
call "...\VC\Auxiliary\Buildcvarsall.bat" x86
cmake -S port -B build/win-x86 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/win-x86
```

This gives `amagami.exe` (the window) and `amagami_vm.exe` (the command line machine, with `--shot`). Both produce the same frames and printed lines as the WSL build, and the 32-bit Windows build runs the 40 million instruction test in about 26 seconds (the Linux build takes 55 because its thread hand-over is slower).

## Layout

| File | Contents |
|---|---|
| `port/src/vm.hpp` | Values, classes, threads, the machine |
| `port/src/vm.cpp` | Class table, instruction decoder, interpreter loop, thread scheduler |
| `port/src/loader.cpp` | LZSS system image, ARC archives, SCF classes |
| `port/src/natives.cpp` | Built-in classes (Object, Class, String, Array, Integer, Float, Thread, …) and the headless primitive stubs |
| `port/src/gfx.hpp`, `gfx.cpp` | Screen objects (sprites, backgrounds, text, characters, whole pictures, event pictures, approximate snow), texture loading from the archives, the software renderer, PNG output |
| `port/src/main.cpp` | Command line driver |
| `port/CMakeLists.txt` | Build; no dependencies beyond the C++ standard library and threads |

## Building and running

```sh
# in WSL (the only toolchain on the development machine so far)
cmake -S port -B <build> -DCMAKE_BUILD_TYPE=Release && cmake --build <build> -j8
<build>/amagami_vm --root . --steps 12000000 [--press TICK:MASK[:FRAMES]]... [--log FILE] [--shot TICK:FILE.png]...
```

`--root` is the repository with `extracted/` in it. The output has the reference interpreter's format: a stop line with the instruction and frame counts, an error if any, the last 20 printed lines, and the stack of every thread.

## Checking against the reference

```sh
MSYS_NO_PATHCONV=1 python tools/compare_vm.py --exe <build>/amagami_vm --wsl -- --steps 12000000
```

It runs both machines with the same arguments and compares the stop line, the error and every printed line byte for byte. (`MSYS_NO_PATHCONV` is needed only under Git Bash, which otherwise rewrites the `/home/…` path.)

To compare pictures, give both machines the same `--shot` list and compare the files:

```sh
python tools/hokago.py compare_shots build/cmp_py_2800.png build/cmp_cpp_2800.png
```

`--shot` turns the screen-object model on (as in the reference); without it the engine primitives are stubs.

Since F4-10 the memory card is real (see [save.md](save.md)) and the reference still stubs it as a ready card with no data, so the memory card lines differ by design: with an empty save folder this machine prints `stat=6` and `memcard error` (the title's "no ebKore+ save" path) where the reference prints `stat=2` and a failed header load. Compare against the reference with a save folder holding a `system.bin`, or read past those lines. Confirmed (2026-10-15 run).

## How it works

- **Values** are nil, true, false, 64-bit integers, doubles, strings (Shift-JIS bytes), symbols, arrays, objects and classes. Heap values are reference counted.
- **Methods** are decoded once into `(opcode, a, b)` triples with jump targets as instruction indices, as in the reference. Names are interned and method tables are keyed by `(name, argument count)`.
- **Threads.** Scripts block in `sleep` at any call depth (inside `initialize` called from native code, for instance), which a single-threaded loop could not resume, so every script thread needs its own C stack. The reference gives each one an OS thread and passes a baton. On Windows this machine uses **fibers** instead: all script threads are fibers of one OS thread and a hand-over is a `SwitchToFiber`, with no trip through the system scheduler. One runs at a time, round robin in creation order, and the frame clock advances only when every thread is asleep. Other platforms keep the OS-thread version (one condition variable per thread). The fiber build is 3.3 times faster on the 40-million-instruction test (8 s against 26 s) and its output is identical.
- **Errors.** `throw` is a C++ exception inside the loop; `try` handlers live on an explicit stack exactly as in the reference, so a throw unwinds through native calls and script frames alike.

## Save files

`MemoryCard`'s 11 primitives read and write files, as [save.md](save.md) describes. The folder is, in order of precedence: `--save-dir DIR` (both `amagami_vm` and `amagami.exe`), the `AMG_SAVE_DIR` environment variable, `%APPDATA%\open-amagami\save` on Windows, `./save` elsewhere. Headless test runs should pass `--save-dir` to a scratch folder so they neither read nor overwrite real saves.

## Known differences from the engine (shared with the reference)

| Point | State |
|---|---|
| Integers are 64-bit. The engine tags them in a word (`(n << 1) | 1`), so they are 31-bit and wrap | Scanned 2026-10-27: no script relies on it (two exceptions: the mahjong generator's stream, bit 30 of the flag masks); technical/testing.md |
| Floats are doubles; the engine uses 32-bit floats | Scanned 2026-10-27: no divergence found at the `toInteger()` sites or the float comparisons; run evidence pending; technical/testing.md |
| `Number.rnd` is a fixed linear congruential generator | The engine's generator (`0x168F40`) is not modelled |
| Values are reference counted; reference cycles leak | Fine for a boot; to be replaced before long sessions |
| The renderer is a CPU loop that writes PNG files (stored deflate blocks, no compression) | Placeholder: the window and the frame pacing come with the SDL front end (F4-5) |
| `String.hashCode` is FNV-1a here and salted in the reference | Not observable so far |

### Differences seen against the emulator (2026-10-15, resolved 2026-10-15)

Found by comparing frames of the prologue day's action map on PCSX2 and on `amagami_vm`. **Confirmed fixed** (same frame, same picture, same counters):

| Point | Emulator | Our engine before | Cause |
|---|---|---|---|
| Location tile at the top | home picture with the label for "home" | a tile reading "1 year" | `Sprite.setTex(texture, variant)` (primitive 4) was a stub, so `NodeMapArea` kept the variant it was created with. Now it re-resolves the picture like the constructor |
| Counters at the bottom left | 32 / 31 | 00 / 00 | the same primitive (the digits are swapped with `setTex`) |
| Slow sky pan behind dialogue | at a different phase for the same line of text | (timing differs; not a bug by itself) | |

The emulator also shows only the cursor's cell with a hexagon (grid option) and the node's symbol; that matches the engine's node icons.

### Frame matching against the emulator (2026-10-15)

**Confirmed.** The sky pan of the prologue is the same in `amagami_vm.exe` and in PCSX2: frames without text match to a mean difference under 1 of 255 (for example emulator frame at 4:15 of play against tick 5345). The pan depends only on ticks since the scene started, so a PCSX2 capture can be paired with the engine tick that reproduces it.

Method: capture PCSX2 once per second, then run `amagami_vm --shot` every few ticks over the same stretch and take the tick with the smallest difference; refine tick by tick, and move the `--press` times so that the text typing lines up with the pan (the pan does not wait for the player, the text does). A pair is a few ticks apart at best, because the window capture is not tick-exact.

## Performance

For the first 12 million instructions the machine takes about 8 seconds against 30 for the Python interpreter. Most of the time is the cost of switching between the fifty script threads at every frame boundary; running the threads as fibers (or as coroutines on one OS thread, which needs the interpreter loop to be resumable) is the planned fix.

## Testing aids

- Hold **Tab** (window focused) to run the game unpaced: a turbo for getting through scenes.
- `--skip-prologue` (or `AMG_SKIP_PROLOGUE=1`): the prologue scene `n0dea_ev01A` runs in the game's own skip mode (`GameParam.isSkip` answers true while it runs). It must run, not be skipped: it opens the heroines' nodes for day 1. Choices still wait for the player: a direction first (the cursor appears), then circle.
- `AMG_RENDERBENCH=1` (window or `--shot`, also in `--scene` runs): one line per drawn frame, `RENDERBENCH items=N ms=T`, the time of `render_scene` (view transform and drawing, not the upload to the window). **Measured 2026-10-20** (confirmed; Ryzen 5 3450U, 4 bands, Vita data, 960x544, 43 frames from 5 event scenes sampled every 150 ticks): median 6.3 ms, 90th percentile 8.9 ms, worst 13.1 ms, about 44 items per frame (up to 82); none over the 16.7 ms of a 60 Hz frame. At the native size the CPU rasterizer holds. Cost per pixel is the whole story: 1920x1088 is 4x the pixels and 3840x2160 is 16x (hypothesis, not measured: the render scale of F5-3 does not exist yet), which puts them at roughly 25 ms and 100 ms, over budget. That is the case for the GPU back end (D-022, F5-7).
- `AMG_VIEWSTAT=1` (window or `--shot`): prints, every 600 frames and for the first five, how the drawn items of a Vita run split by horizontal position (full width, touching the left or right edge, centred, plain rectangles). For judging what a wider screen has to anchor (F5-4). Headless `--scene` runs do not render, so it needs a window session or `--shot` ticks.
- `AMG_INPUT_LOG=1` (window): one line `input TICK MASK` per change of the buttons, to replay a session headless with `--press TICK:MASK:FRAMES`.
- **F10** writes `build/threads.txt`: the tick, the last 40 lines the scripts printed and the call stack of every script thread (for finding where a hang is).
- `AMG_VOICE_LOG=1` writes every voice setup, play, stop, pause and end, and every inline text command run, to standard error (`Start-Process ... -RedirectStandardError build/voice_log.txt`).

## Loading a saved game

Loading throws `RestartException(2, data)`; `GameMain.scriptMain` catches it and calls `K2X_Script.reset(data)`, which kills the threads, runs `resetPrim` (primitive 0: the engine drops every screen object) and `setup(data)` to rebuild. In the port `resetPrim` clears the object table; before that it was a stub and the previous scene's fade layers stayed on top of the rebuilt one, leaving the screen black (confirmed: slot save then load now shows the map). Slot save and load round trip works in the window.

## Determinism (confirmed 2026-10-15)

The same `--press` list now gives the same run on the Linux and the Windows builds (same tick after the same instruction count). `Object.hashCode` answers a creation serial instead of the object's address. The engine uses `(address >> 4) & 0x7FFFFFF` (`0x119F60`), which cannot be reproduced; no script reached it in the runs checked. `String.hashCode` is the engine's (`0x11D250`): `h = h + (h ^ c)` over the 16-bit characters, 30 bits; `HashMap` walks its table in hash order, so this sets the order of every table keyed by strings.

Pitfall found on the way: a run that showed day 1's heroines came from a binary with a different `--skip-prologue`; skipping the prologue scene outright leaves day 1 empty.

## Comparison of the prologue's last scene with PCSX2 (2026-10-15)

Method: a PCSX2 state taken just before Miya appears is replayed with `pcsx2_drive.ps1 -LoadSlot` (a press every few seconds, a capture per second); the engine runs the same stretch from `--press` lists and takes a shot every 50 to 100 ticks. For each emulator frame, the engine shot with the smallest mean absolute difference (80 by 56, 0 to 255) is taken.

| Part | Mean difference |
|---|---|
| Starry sky of the dream (dark frames) | 1.5 to 2.5 |
| Miya with text (face, curtain, window) | 5 to 7 |
| Frames across a scene change (fades out of phase) | 14 to 19 |

**Confirmed:** background, character position and size, text window, speaker colours and the transition to the classroom are the same. **Not covered:** mouth and blink timing, which needs a tick-exact pairing. The window capture of PCSX2 is not tick-exact.

## Test harness: one scene at a time (2026-10-15, D-016)

```sh
amagami_vm --root . --save-dir /tmp/x --scene n0dea_ev01A [--choice K] [--steps N]
```

Runs one event scene headless and prints one line, `SCENE <name> DONE|ERROR|HANG ticks=<n> steps=<n> [message]`:

- **Fast boot.** The title screens, the main menu and the prologue's map answer at once (`TitleLogo.scriptMain`, `MainMenu.scriptMain` and `NodeMapMain.selectProlog` are replaced), so the first event the game starts is the one asked for, a few seconds after the start instead of 15 to 20 (`AMG_SCENE_BOOT=1` keeps the real boot).
- **Skip mode.** While the scene runs `GameParam.isSkip` answers true: the text goes by at once but everything the scene does still happens.
- **Automatic input.** From tick 60, in cycles: `K + 1` presses of down (the first one only shows the cursor of a choice) and then circle. `--choice K` therefore picks the K-th answer of every choice and moves the text on.
- **Result.** DONE: `execEvent` returned (the machine stops). ERROR: an uncaught exception in the main thread, with its message and the stack of the throw. HANG: the instruction budget ran out (a scene that waits for the player, or one that is stuck). An exception that ends a secondary thread is not an error (the thread dies, the scene goes on) and is listed as `terr=` in `SCENEINFO`. The step budget is 60 million; the finals (`_xd`, `_xp`, `_xs`) need about 100 million (the staff roll lasts 23,000 ticks), so `tools/sweep.py` gives them 150 million. `AMG_SCENE_STATE=fan` presets the heroines' levels for scenes that need a game in progress.

`tools/sweep.py` runs all scenes (the event nodes of `NodeTable` that have a script: 2,136) in parallel and keeps `build/sweep/results.tsv`; a stopped sweep resumes where it was. It prints the counts and the errors grouped by message. The first six nodes (`n0fre___*`) are the developers' test nodes: interactive viewers wait for the player for ever (HANG is expected), one is the mahjong table and one a debug tool.

### Reading a sweep without reading scenes (2026-10-15)

`amagami_vm --scene` also prints `SCENEINFO <name> at=<Class.method> stubs=<Class.n,...>`: where an error was thrown and which still-stubbed primitives the run reached. `python3 tools/sweep.py --report` turns `build/sweep/results.tsv` into a few lines: counts by status, the errors grouped by message and place, the scenes that ended asking the game to restart (a result, not a failure: the harness has no save slot for the restart handler), and the stubbed primitives ranked by the scenes that reach them, which is the order in which to implement them. `tools/hokago.py imgdiff A.png B.png` compares two screenshots with numbers (mean difference and a grid), so a picture is looked at only when the numbers say so.

### Scenes in one process: faster, but not equivalent (2026-10-15)

`amagami_vm --scene-list FILE` runs the scenes of a list one after another in one machine, skipping the boot (most of the 11 million instructions of a small scene). A scene that fails or hangs ends the process and `tools/sweep.py --batch N` starts a new one with the rest. **Measured: it changes the result.** State carries over (read marks, passed nodes, what `findEvent` finds), and later scenes of a batch finish in 0 to 15 ticks where they take hundreds on their own. For the D-016 criterion (every scene runs to its end) each scene is run in its own process, which is the default (`--batch 1`); a batch is a quick smoke test, not a result.

### Scripts that are not nodes: chains (2026-10-15)

2,136 of the 2,485 scenario scripts are nodes of the event graph (`NodeTable`) and run alone with `--scene`. The other 349 are mahjong classes, 35 cut-ins and about 270 events that another event chains to. Running one of those directly (the harness tried it) finds its class but fails on objects that the previous scene of the chain leaves (`fadeIn` or `getFavor` of `nil`): the dependency is real, so they are covered the way the game reaches them. `--chain` lets the scene's `execEventChain` go on until it ends, and `SCENEINFO` lists the events it went through (`chain=a>b>c`); `sweep.py --chain` counts the scripts reached. Errors deep in a chain can be missing state of the isolated start rather than port faults.

### Why scenes fail alone (2026-10-16, first sweep: 37 ERROR, 32 HANG of 2,136)

Read with the stack of the throw (`vm.cpp` now keeps the first trace through a `rethrow`; before, the sweep saw only `GameMain.scriptMain`):

- **State of a game in progress (about 24 ERROR, confirmed by reading the scripts).** The mid-chapter scenes (`*_me*`, `*_st*`, `*_ep01*`) call `Heroine.getList(3)` (the heroines who like the player) and `getList(4)` (friends), `getXmas()`, `isOpenNode(...)` and `K2X_Script_.cvar12`, which a new game leaves empty or `nil` (`Heroine.level` is `nil` until the heroine is met). They cannot pass alone; they are not port faults. `AMG_SCENE_STATE=fan` sets heroine 1 to "likes" (level 2, star 2), 2 and 3 to friends and the rest to acquaintances before the scene: it clears the 5 scenes that only need that (the `level_nil` family), the others also need opened nodes and earlier choices, which only a chain or a recorded session gives. Hypothesis: the chain sweep resolves most of them.
- **`Music.fade(0, 0)` (1 ERROR, `n2dea_ev06A`).** `Music.run` divides by `sync`, which is 0. The shipped game does this in a normal scene, so a thread that dies from an uncaught error must not stop the machine (hypothesis; the port used to stop). Now only the main thread is fatal; the others are counted and listed as `terr=` in `SCENEINFO`.
- **`StageSet :/1` (5 ERROR, `*skx_ep00A`) and `Debug_Num` (`n0fre___04A`, `n2fre___19A`).** `StageSet` has no `:` method; those scripts look like leftovers (the developers' nodes are `n0fre___*`). Not investigated further.
- **HANG in `Ending.scriptMain` (about 24).** The staff roll is long: 12,500 ticks (3.5 minutes of game time) in 60 million steps, not finished. Whether it ends within a larger budget is being measured.

## Reference screens in PCSX2 without playing: the teleport disc (2026-10-16, F4-22)

`tools/teleport.py` makes a working copy of the disc (`init`, 20 s, 4 GB, in `build/`, ignored) and `go WORKING_ISO SCENE` replaces the class of the prologue's first event (`n0dea_ev01A` in `SCENARIO.ARC`, 38,458 bytes) with the class of any other scene (renamed, same 11 characters, index size updated); 2,479 of the 2,485 classes fit. **Confirmed** with `n2ski_ev01A`: PCSX2 loaded from that disc, with a save state taken at the title screen (`build/title_screen.p2s`, slot 2: the drive script loads it, so the 2 minutes of boot and logos are skipped), goes Start, New game, the name entry and the prologue map, and then shows that scene's first lines instead of the dream (the line matches `n2ski_ev01A`, not `n0dea_ev01A`).

Recipe: `python tools/teleport.py go build/teleport.iso SCENE`, then `tools/hokago-tools/emulators/pcsx2_drive.ps1 -Iso build/teleport.iso -LoadStateAt 20 -LoadSlot 2 -PressAfterLoad 3 -PressKeys "VFFFF..."` (`-PressKeys` is new: the keys pressed in turn, V Start, F Circle, D Down; the last repeats). About 80 seconds from launch to the scene. The state must be taken before the class loads: a state in the middle of the first event (slot 6) keeps the old class in memory. Limits: the scene runs with a new game's state, like `amagami_vm --scene`, so the mid-chapter scenes that need a game in progress cannot be reached this way; the name entry ends with the default characters.

## Reference set: the chosen scenes (2026-10-16, F4-22)

`python tools/pick_refs.py` chooses the scenes: for each category (kind of scene, heroine code, chapter, and each primitive that draws something) the shortest scene that has it, and it fills up to 20 with the chapters and codes least covered. It leaves out `me` and `st` scenes (they need a game in progress), the developers' `n0fre___` nodes, scenes under 60 ticks and the six classes too large for the teleport disc. From the isolated sweep (1,722 candidates) the set is: `n5sox_ep01A` (ActorPicture.25), `n3skx_xs01B` (snow and `StructPicture`: a staff roll of 19,526 ticks, so the screen is taken at a chosen second), `n6skx_ep02A` (`EventPicture.3`), `n1ako_se01C` (`EventPicture.2`), `n6sir_ec03E` (balloons and `LightBall`), and one scene each of the kinds `xd`, `lu`, `xp`, `sf`, `ev`, `sb`, `gd`, `kd` and of the codes and chapters not yet covered. The list is regenerated with the same command whenever a sweep changes.

## Design: which script methods ran (2026-10-16, F4-19, criterion 3 of D-016)

Nothing is built yet. Plan, in the order that costs least:

1. **Count in the VM.** One flag on each `Method` set when a frame for it is pushed (the interpreter already builds `frames`), no counters, so the cost is one store per call. `AMG_COVERAGE=FILE` writes `Class.method/args` of every flagged method when the process ends (`vm.run()` returns or the scene finishes).
2. **Merge outside the VM.** `tools/sweep.py --coverage` gives each process its own file and a script (`tools/coverage.py`) merges them and compares with the list of all methods of `AMAGAMI.IMG` (5,871) and `SCENARIO.ARC` (13,077) from the decompiled sources. Output: the percentage, per class group (image, scenario, mini-games, album and extras), and the unreached methods in a file.
3. **What the criterion can reach.** The scenes run the scenario methods of their own class and the image methods they call; the menus, the map, the mahjong tables, the album and the extras need the recorded replays (F4-23). So the 95 % is measured over the union of both, and the rest (debug and developer code: `Debug_*`, the `n0fre___` nodes, unused macro methods) is listed, not counted against.
4. **Expected size of the first result.** Scenario classes are about two thirds of all methods, and every one of them is exercised by its own scene (2,084 of 2,136 run to their end alone), so the first number should be high on that side and low on the image side; this is a guess to be replaced by the measurement.

## Fast sweeps and selective reruns (2026-10-16)

Measured on the same scene (`n2ski_ev01A`): 15 to 25 s of wall clock in the first sweeps, 3 s now; a list of 12 small scenes 7 s; the whole set of 2,136 in about 14 minutes with 6 workers (it was 2.5 hours). Three changes:

- **Coroutines on Linux.** The script threads were one OS thread each, handing a baton through condition variables: the kernel time was three times the interpreter time. `vm.cpp` now gives Linux the same fiber calls as Windows, on top of `ucontext` (`makecontext`/`swapcontext`), so every script thread is a coroutine of one OS thread.
- **The disc files on the Linux file system.** Reading `extracted/` through `/mnt/c` (the Windows disk seen from WSL) cost most of the remaining kernel time. `~/amg-root/extracted` is a copy (3.2 GB, made once with `cp -r`); `sweep.py` uses it when it exists (`--root`). `GLIBC_TUNABLES=glibc.malloc.hugetlb=1` cuts the page faults to a third.
- **One boot, one fork per scene.** `amagami_vm --scene-list FILE --fork` boots once and, at the first event, forks a copy of the process for each scene: every scene starts from exactly the state of an isolated run (same ticks, confirmed on 41 scenes) and the boot is not repeated; `--scene-timeout S` kills a copy that runs too long (reported TIMEOUT). Picture preloading on helper threads is off in this mode (a forked copy keeps only one thread). `sweep.py` uses it by default off Windows, with lists of up to 25 scenes per process.

What each scene executed is now recorded: `AMG_COVERAGE=DIR` (or `sweep.py --coverage`) writes `DIR/<scene>.cov` with the script methods run (`m Class#method/args`), the native methods (`n selector/args`) and the primitives (`p Class.N`). `python3 tools/coverage.py` gives the share of all script methods executed (criterion 3 of D-016) and `--touching X` lists the scenes that used X, so a change to a native is checked by running only those scenes (`sweep.py --again $(tools/coverage.py --touching copyFrom)`). Full sweeps are for milestones.

`sweep.py` also reruns the scenes that failed with the state of a new game once more with a game in progress (`AMG_SCENE_STATE=fan`: heroine levels and the Christmas pair); those that pass are counted as `DONE-STATE`. The report leaves out the scripts that are not meant to end on their own: the developers' `n?fre___??A` nodes, the pose viewer `*_me19A` (it shows a name, a pose and X/Y and waits for keys: confirmed by reading it) and `*skx_ep00A` (sends `:` to a `StageSet`, a leftover).

The bug the chain sweep found: `Array.copyFrom` grew the receiver to the source's size. The game uses `Array.new(count).copyFrom(vector.image())` to trim a vector's array (room for eight) to its count, in `Heroine.getList`, `getXmas` and `EventNode`, so every heroine list had eight entries, all `nil` for the unused room, and the scenes that judge by them failed (`getStar`, `getNum`, `getMyouji` of `nil`). It now copies as many as the receiver holds. Many of the errors that "Why scenes fail alone" above put down to missing state were this bug.

## Method coverage, first measurement (2026-10-17, F4-19, criterion 3 of D-016)

`sweep.py --chain --coverage` with the three automatic answers to choices (`--choice 0`, `1`, `2`), 6,382 runs of the 2,127 scenes, against the 18,948 methods of the decompiled sources (`tools/coverage.py`):

| Runs | Script methods executed |
|---|---|
| choice 0 only | 7,792 of 18,948 (41.1 %) |
| + choice 1 | 8,187 (43.2 %) |
| + choice 2 | 8,373 (44.2 %); image 1,798 of 5,871 (30.6 %), scenario 6,575 of 13,077 (50.3 %) |

More branches add little (3 points): what is missing is not in other answers of the scenes. The 10,575 methods never run (listed by `--missing`) are mostly whole systems that no event scene starts:

| Group | Methods not run | What it is |
|---|---|---|
| Cut-ins (`c01_cutin` ... `c35_cutin`) | about 3,170 (about 90 each) | The 35 cut-in scripts: reached from the cut-in viewer of the extras, not from an event |
| Conversation tables (`k1_kaiwa` ... `k6_kaiwa`) | about 1,740 (280 to 300 each) | The topic dialogues of the conversation mini-game (`Wadai*`), one table per heroine |
| Mahjong (`Player`, `AbstractTaku`, `Tehai`, ...) | about 1,050 | The mini-game |
| Album, extras, `OmakeActorView` | about 480 | The album and the extras menu |
| Scenario scripts of the events themselves | about 2,530 | Branches of scenes (the choice-dependent ones) and developer tools such as `n2fre___19A` (106 methods) |
| Other image classes | about 3,500, of which the kaiwa tables above are part | Menus, load and save screens, config, system code |

Leaving out the cut-ins, the conversation tables, the mahjong, the album and the extras (about 6,300 methods that need their own sessions), the scenes reach about 66 % of what is left. So criterion 3 (95 %) is not reachable by event scenes alone: it needs the recorded replays of F4-23 for the conversation game, the cut-in viewer, the mahjong, the album and extras, and the menus, and then the rest is listed as dead or developer code. Numbers to beat: 44.2 % today.

The 349 scenario scripts that are not event nodes (`sweep.py --non-nodes`) add little: 49 of them (events that other events chain to) run alone; the cut-ins (`c01_cutin` ... `c35_cutin`), the mahjong classes and about 250 more fail when started from the event machine: 100 call `:` on a `StageSet` (the same leftover as `ep00A`), 100 call `fadeIn/4`, `load/3`, `cutIn/3` or `fadeOut/1` on a `CutInWindow` (which has only `setTex` and `setColor`), 14 call `show/0` on a `Calender`. **Hypothesis, not checked:** these scripts expect objects in `K2X_Script`'s variables that only the mode that starts them builds (the cut-in viewer, the album and the extras, `OmakeMain`, the mahjong table), so they cannot be run from a scene list; the way to reach them is the menus that start them, which is the recorded replays of F4-23. Union of the four runs (three answers to the choices plus these): 8,940 of 18,948 script methods (47.2 %).

## Reference screens against PCSX2, first run (2026-10-17, F4-22, criterion 4 of D-016)

`tools/refscreens.py` captures each of the 20 scenes chosen by `pick_refs.py` in PCSX2 (teleport disc, a capture every 3 s for 90 s, the text advanced every 3 s) and in the port (`AMG_SCENE_NOSKIP`, following the chain, a capture every 30 ticks), and for every PCSX2 capture takes the port capture that differs least (mean absolute difference, 0 to 255, on a 160x112 grey copy). Captures that show the same picture in four or more runs are left out (179 of 600): the menu, the name entry and the prologue's map before the teleported scene starts, and the action map PCSX2 returns to when a short scene ends.

Result: **16 of the 20 scenes have a median best match under 7** (1.7 to 6.5: the same picture, the difference is scaling and text timing), among them the conversation game with the new balloons and light balls (`n6sir_ec03E`, 4.3), an ending (`n3skx_xs01B`, 2.6), event pictures (`n5sox_ep01A` 1.7, `n6skx_ep02A` 2.4) and three `ec` scenes. The four others differ for reasons outside the drawing, checked on their worst pairs:

- `n2nka_sf01A` (31.6): the scene starts with `fromKaiwamode(nil)` and keeps the previous background; in PCSX2 that is the prologue map's place (the house at night), in the port the default white (the fast boot skips the map).
- `n1ako_se01C` (40.6): PCSX2's remaining captures are action-map states seen in no other run; the port, following the chain, reaches an ending and the notice screen.
- `n2sir_gd01D` (11.2, 5 captures) and `n0ski_ev02A` (32.4, one capture): too few captures of the scene itself left; the scene is short and its few frames have no counterpart at the port's pace.

Threshold proposed for the criterion: median best match under 8 on the scene's own captures.

Second run, the same day: the port now boots fully (`AMG_SCENE_BOOT=full`: title, menu, name entry and the prologue's map, as the teleported PCSX2 run does) and captures from tick 4500. The scenes that keep the previous background now find the same one: `n2nka_sf01A` went from 31.6 to 2.2 and `n1ako_se01C` from 40.6 to 2.1. **18 of 20 scenes are under 8.** The two left, `n2sir_gd01D` (17.0) and `n0ski_ev02A` (39.7), are too short to compare this way, even captured every second (`refscreens.py pcsx2 SCENE 60 1`): nearly all their PCSX2 captures are the map or the home's room the game shows around the teleported scene, against the prologue map's sky in the port. Inconclusive, not a drawing difference seen.

## Recorded sessions as tests (2026-10-17, F4-23 tooling)

`tools/record_session.ps1 -Name NAME [-FromSave DIR]` starts the window (`build/win-x86/amagami.exe`) with an empty save folder, or a copy of DIR, under `build/replays/NAME/`, and `AMG_INPUT_LOG=1`: every change of the buttons goes to `input.log` as `input TICK MASK`. `python3 tools/replay.py NAME` (WSL) feeds the same buttons at the same frames to `amagami_vm` (`--press`) with a fresh copy of the starting save folder, stops five seconds after the last change (`--ticks`, new), writes the coverage of the whole run to `build/replays/NAME/coverage/session.cov` and a capture of the last frame, and prints OK or the uncaught error. Checked with a synthetic session (the full boot's presses and a press every second): it reaches the prologue's first lines, 728 script methods run. `tools/coverage.py` reads the replays' coverage folders together with the sweeps' (`coverage.py build/coverage* build/replays/*/coverage`).

## `and` and `or` on nil (2026-10-17, confirmed)

The engine's handlers for `and` (`0x0C`, at `0x11C8E0`) and `or` (`0x0D`, at `0x11C950`) compute directly whenever both operands are special constants (nil, true, false: values under 9) and send `&&`/`||` only when one is an integer or an object: `and` answers the left operand when the right one is true and false otherwise, `or` the left operand when the right one is false and true otherwise. The port sent `&&` to nil, which has no such method, so `flag and ...` with a flag never set failed; the mahjong table's `Tehai#draw牌` does exactly that (`flg自摸`), and its thread died after the deal. With the fix the table plays a whole game (`n0fre___18A`: 69,519 ticks, 22 s headless), the coverage goes from 47.2 % to 50.7 % with that one game (part of it from reading the coverage files' Shift-JIS names in `tools/coverage.py`), and the cut-ins run. A full chain sweep after the change: no regression (2,110 DONE, 5 DONE-STATE, the same 5 ERROR).

### Reaching systems no event starts (2026-10-17)

Coverage went from 53.2 % to **61.7 %** of the 18,948 script methods with three kinds of runs, all through probes (`AMG_OVERLAY`, `tools/probe.py`) of the developers' nodes, with no change to the game:

- **Conversation sweep:** the 385 `ec` scenes at the game's pace (`sweep.py --kinds ec --noskip`) with each choice 0 to 8: all DONE, but only +1.7 points. The conversation tables depend on the game state (heroine level, topics seen), not on the choice.
- **Mahjong against other opponents:** `n0fre___18A` calls `loadEvent("MahjongMain", 0)`, and 0 means the opponents 1, 2, 3; `MahjongMain#preLoad` takes an array of three character numbers instead (`Mahjong` passes the menu's choice). A probe passes each group of three of the 35 characters with a cut-in script (`c01_cutin` to `c35_cutin`), and `AMG_SEED` (the start of `Number.rnd`'s sequence; default unchanged) gives each a different deal. 12 games, 10 end on their own (75k to 134k ticks), 2 reach the 200k-tick cap. 1,125 cut-in methods run.
- **Menus with everything unlocked:** `GameParam.setClearBonus(true)` (the clear bonus: `Character.checkAlbum` answers true) and then the menu's own call (`AlbumMain.new().preLoad().scriptMain()`, `FreePlay`, `ConfigMain`, `Mahjong`), driven by random buttons from a fixed seed (`--press`, directions, circle, cross, triangle, square, L1, R1). The extras (`OmakeMain`) fail in the probe because its list of unlocked characters is empty (the main menu only offers it when `OmakeActor.numOf() > 0`): a missing state, not a defect of the port. The mahjong menu ends at once (hypothesis: no opponents unlocked).

**Counting (2026-10-17).** A script method that the port runs as a native (`Object#sleep`, `Integer#toString`, `System.FullGC`) used to be missing from the coverage files, which only marked methods run as bytecode; the machine now also writes `m Class#name/args` for the class that holds each native method run. Arithmetic and comparisons on numbers and the special constants are computed by the instructions themselves (as in the engine), so `Number#+` and the like stay unrun. Classes no script references (the script compiler, unused library classes, the mahjong's debug mode, three developers' macros) are listed with the reason in `tools/coverage.py` (`DEAD`) and left out of the logic figure (D-019). `n0skx_ep00A` needs about 110 million instructions and now gets the endings' budget.

**Random buttons in event scenes (2026-10-17).** Twenty reference scenes run after the full boot with the harness's presses off (`AMG_NO_AUTOPRESS=1`) and random buttons from a fixed seed reach the backlog, the dialogs and the in-game settings. The first batch showed a defect of the port: `TextWindow` sends `suspend` (L1) and `showConfigMenu` (triangle) to `Class.findClass(#MahjongMain)` when that answers a class, and the port's `findClass` loaded any scenario class, so in an event scene those buttons called the mahjong's class methods with no game and the text thread died. `findClass` now answers only loaded classes (the image's, and scenario classes brought in by `loadClass`, `loadEvent` or a constant), which the scripts confirm: `K2X_Script#findEvent` calls `findClass` first and falls back to `loadClass`. Full sweep after the fix: no change. Logic coverage 73.0 % (D-019).

**What the logic figure leaves out, listed (2026-10-17).** `tools/coverage.py` lists three kinds of logic methods apart, as criterion 3 allows ("the rest is listed"): classes no script references (`DEAD`, with the reason each); abstract methods, whose whole body throws `SubClassResponsibility` or `ShouldNotImplement` (56); and methods whose selector no script names anywhere, as a call, a symbol (`#name`, `#'name'`) or a symbol constant (`--unsent FILE` writes them; 210). The last kind is a hypothesis per method: a name built at run time and sent with `perform(Symbol.new(...))` would still reach it; the known builders (`level_`, `type_` in `EventNode`) are excluded, and operators are never counted as unsent because the instructions send them. Logic coverage: **79.8 %** (2,996 of 3,755).

**Slow long runs: two defects found by profiling (2026-10-18, case C2).** A bot run through the calendar slowed down the longer it went (94k ticks in 40 minutes, 10 times slower at the end than at the start). `perf` on a build with debug information pointed at `Thread::schedule_next`, and `AMG_PROGRESS` (now printing live threads and actor threads) showed the rest: (1) the scheduler scanned every thread ever made on each hand-over, a list that only grows; it now walks only the live threads (`VM::live_threads`), the ended ones stay in `VM::threads` because script `Thread` objects hold pointers to theirs; (2) `Thread.killall` did nothing, but the game calls it in `K2X_Script.reset` (a load, a return to the title), which kills every thread but the caller (read at `0x11F540`, confirmed); each reset then left the old actors, stages and windows running and `setup` made thirty more actor threads, up to 240 after 90k ticks. Both are fixed and the actor threads stay at 30; the same run does 382k ticks in 5 minutes. These affect the window too: a long session with several loads got slower with each one. `AMG_METHOD_PROFILE=FILE` writes the script methods by instructions run. A full sweep after both changes: no difference (2,110 DONE, 5 DONE-STATE, the same 17 ERROR and 9 HANG).

## Vita3K as the Vita reference (F4-51)

`tools/hokago-tools/emulators/vita3k_drive.ps1` (the emulator's path from `-Exe` or the environment variable `VITA3K_EXE`) drives the emulator and captures its window **without the window having focus**: keys go to its message queue with `PostMessage` (SDL builds its key state from them) and the picture comes from `PrintWindow` with `PW_RENDERFULLCONTENT`, so the window can be covered or behind others. `-Foreground` brings it to the front and uses real key events and a screen copy instead. Confirmed 2026-10-20 with VS Code in front: two Down presses moved the menu cursor two entries, and the capture shows the game, not black. `powershell -File tools/hokago-tools/emulators/vita3k_drive.ps1 -Plan "20:shot:menu;21:down;22:shot:next" -OutDir build/vita3k`. Seconds count from the start of the call; if Vita3K is running it is reused and left open (`-Close` ends it). Keys follow the emulator's default map: Circle = C (confirm), Cross = X (back), Square = Z, Triangle = V, Start = Enter, Select = Right Shift, d-pad = arrows, left stick = WASD. The captures are 1200x680 (the 960x544 frame stretched to the window; the same aspect). The emulator needs about 20 s from launch to the game's first screen. Confirmed 2026-10-20: it captures the running game (title, menu, the opening's character). Not yet done: lining a Vita3K frame up with the port's frame of the same scene (F4-45, F4-50).

## Vita visual audit: the port against Vita3K (2026-10-23, F4-53)

The PS2 audit (F4-22) never saw what only the Vita data draws (F5-25 was found by eye). Vita3K has no save states, so the PS2 trick (a teleport disc plus a state taken at the title) does not carry over; what replaces it is an `AMAGAMI.IMG` that starts a new game in any scene.

**The teleport image (confirmed in the port).** `python tools/vita_teleport.py make SCENE OUT_IMG` reads the player's `AMAGAMI.IMG`, adds SCENE as a string constant of `GameMain` and makes the one call `self.execEventChain(loc0)` of `GameMain.prolog` pass it (a two-byte `push_local 0` becomes a two-byte `push_const N`), then compresses the image again. Checked in the port with `n6sir_ec03E`: the new game goes through the title, the menu, the name entry (default name), the prologue's map and its "Play event?" bar, and then shows that scene (the conversation game with the topic bubbles), at about tick 1600 with the presses of `tools/vita_audit.py`. `vita_teleport.py check IMG` prints the scene an image starts in. **A first idea does not work:** replacing the constant `n0dea_ev01A` (the node the prologue opens on the map) makes the map answer "this action cannot be selected", because it asks the node whether it can run on day 1 at that time (`EventNode.isExecutable`: day, time, flags) and another scene's node usually cannot. The node stays; only the name that reaches `execEventChain` changes. Scene names need not have 11 characters this way, and the CPKs are not touched. Scenes that need a game in progress (`me`, `st`) are not meaningful, as before.

**Writing the image back (confirmed).** The decompressed image rebuilds identically (`scf verify-image`, 260 classes), but the compressed file is **not** byte-identical to the original even with no change: the packer in `hokago-tools` chooses other matches than the game's own tool (1,046,958 bytes against 1,045,879; the first difference is at byte 0x16). The stream unpacks to the same bytes, also with a decoder window filled with another value (so it never reads the window before it is written). What is **not** proved is that the console's own decoder, run by Vita3K, accepts it: the emulator run will show it (if the game does not start, this is the first suspect).

**Same data on both sides (confirmed by SHA-256).** Vita3K's `ux0/app/PCSG00291` and its patch layer `ux0/patch/PCSG00291` hold an `AMAGAMI.IMG` identical to the player's (in `%LOCALAPPDATA%/open-amagami/game`), and `SCENARIO.CPK` and `graph/GRAPH.CPK` are identical to it. `eboot.bin` is the one difference: the player's copy equals the base package's, and Vita3K runs the patch layer's (1,459,940 against 1,459,204 bytes); what that difference changes is not known, so a difference seen in one scene is checked on another before it is blamed on the port. No `lang/` layer is needed: the player's copy is already the English-patched data. The port runs a folder of links (`build/vita-audit/roots/SCENE/`: hard links to the CPK and the eboot, junctions to `graph` and `sound`, and the scene's image); the player's copy is never written.

**Tools.** `tools/vita_audit.py`: `port SCENE` (a port frame every 30 ticks for 60 s from tick 1560), `states` (keeps the six screens of the new-game flow from the port: title, menu, name entry, its confirmation, the map, the "Play event?" bar), `emu [--check] SCENE...`, `compare`, `selftest`. The scenes come from `python tools/pick_refs.py --vita --min 24 --force n6sir_ec03E,n3skx_xs01B,n5sox_ep01A,n6skx_ep02A,n1ako_se01C` (the Vita sweep's `stubs` column no longer names drawing primitives, so besides the five scenes forced from the PS2 set the categories are kind, heroine and chapter; scenes over 3,000 ticks are left out unless forced). The 24 scenes are in `build/vita-audit/scenes.txt`; several are only 75 to 130 ticks long (about 2 s), which the PS2 audit found too short to compare (F4-22), so the long ones weigh more. **Port side done for all 24 (confirmed by looking at one frame of each):** each one starts in its own scene from a new game; `n0ski_ev02A` (75 ticks) is already back on the map by the frame looked at, so it is probably too short, like on the PS2.

**Emulator side (written, not run).** `emu` drives `vita3k_drive.ps1` in a closed loop instead of a blind timeline: it launches Vita3K (20 s), captures the window every second or two, recognises the new-game screens against the port's frames (a frame is a screen if its difference is under 14 of 255 and clearly the least; checked on the port's own frames, 6 of 6) and presses the key that moves it on (Start at the title, Circle at the menu, Start at the name entry, Circle for "use this name", Circle on the map and its bar). When two captures in a row are none of those screens after the map, the scene has started: it takes a capture every 2 s for 60 s and presses Circle every 3 s, then closes the emulator. Keys go by `PostMessage` to the emulator's window, no focus needed; starting the program may still take the focus for a moment. The flow keys are the ones the port took (its presses are the PS2 masks 2048 and 32 for Start and Circle); that they match Vita3K's keyboard map (Enter, C) is from the 2026-10-20 check of the menu. **Safety (tested on a temporary folder: normal run, failing run, the leftover backup of a crashed run, an impossible restore):** `emu` refuses to start while Vita3K runs; it backs up both `AMAGAMI.IMG` files Vita3K can read and the savedata folder (SHA-256 checked), swaps, and in a `finally` closes only the process it started, restores everything and checks the hashes; a failed restore says so and keeps the backups (`*.amg-backup`). A leftover backup from a crash is put back at the next start. The audit never writes to the player's copy.

**Scoring.** As in F4-22: for each Vita3K frame (resized to 960x544 from the 1200x680 client area) the smallest mean absolute difference (0 to 255, 192x109 grey) against the port's frames of the same scene; the scene's score is the median, with the mean of the worst 10 % and the hottest 32x32 block of the worst pair beside it (where to look first); `compare` prints the scenes worst first and writes `build/vita-audit/report.tsv` and `build/vita-audit/SCENE/worst.png` (Vita3K left, port right). The pipeline was tested on the port's own frames standing in for the emulator's (score 0.1). The threshold of 8 comes from the PS2 and is not shown to suit the Vita.

**Pending.** The emulator run; the report and the numbered list; the fixes. Whether the image `make` writes starts in Vita3K, whether the new-game flow needs more than those keys (a different name entry, an extra confirmation), and whether the emulator's first screens are recognised, are all unverified until that run. The run, with the owner away from the PC: `set VITA3K_EXE=<folder>\Vita3K.exe && python tools/vita_audit.py emu` (all 24 scenes, about 3 minutes each), then `python tools/vita_audit.py compare`; `emu --check` prints the plan and touches nothing.

## The splash screen (2026-10-23, confirmed in the window)

The window opens on a splash before the game's own logos: the site's mascot, "A non-profit fan project.", "Not affiliated with the game's makers." and the site's address (the name of the site identifies the project, so there is no title). It is our own art, made by `python tools/hokago.py make_splash MASCOT.svg port/art/splash.png` (resvg draws the background and the mascot, Pillow the text with the M PLUS Rounded font; the mascot is the site's main one, the girl peeking out of the cup of its home page (`port/art/splash-mascot.svg`: `src/assets/mascot-body.txt` of the site's repository with the eyes and steam drawn still, made 2026-10-25; it was `public/mascot/welcome.svg`, the About page's, before), and the font is `hokago-tools/fonts/MPLUSRounded1c-ExtraBold.ttf` (`--font`); `--lines FILE` changes the text). `win_main.cpp` shows it as a one-item scene as soon as the window is up, while the script data loads on the machine thread, and keeps it at least four seconds with a half-second fade out; a button, a click or a pad press after the first 0.8 s ends it sooner. `AMG_NO_SPLASH=1` or `splash=off` in config.txt turns it off; `AMG_SPLASH=FILE` uses another picture; `art/` is copied beside the program like `skins/`.

## The launcher (2026-10-23, confirmed by running it; D-027)

`amagami.exe` opens a launcher window first (`launcher.cpp`, plain Win32): the list of checks of the game folder (`data_check.cpp`: files present, decrypted copy, sound files, and the hashes of `AMAGAMI.IMG`, `SCENARIO.CPK` and `eboot.bin` against `data/known_versions.txt`; an unknown version is a warning, never an error), **Choose game folder...**, **Settings** and **Start game** (enabled when no check failed). Start closes the launcher and opens the game window. Settings lists every row of `port_settings.cpp` (language from the layers under `<root>/lang`, interface size, render scale, screen filter and shape, button pictures, confirm button, unknown pad, splash screen, launcher at start), each applied and written to config.ini at once, plus choosing the folder again, opening the saves folder and deleting the program's own copy of the game (saves and settings are kept). `--root`, `--import`, `--reimport` and `--start` skip it (development, tests, the recorded sessions); `launcher=skip` in config.ini skips it too while the remembered folder still passes the check, and holding Shift shows it anyway. Where files go is `paths.cpp` (config.ini and saves in `%APPDATA%\open-amagami`, the copy of the game and logs in `%LOCALAPPDATA%\open-amagami`, everything under `portable-data\` beside the program when `portable.txt` is there); a config.txt of an earlier version is taken over once. **English (2026-10-25, checked on the owner's copy by window captures):** a player's copy comes in one of two forms. The English fan patch as Vita3K installs it (`app/PCSG00291`) already holds the translated `AMAGAMI.IMG`, `SCENARIO.CPK` and `graph/GRAPH.CPK`: the owner's imported copy is that one (`data/known_versions.txt` had labelled its hashes "decrypted dump"; now "with the English fan patch v1.1", and the original Japanese files are listed too). The launcher's **Language** line says which it is, and Settings names the base option "This copy's own (English)" or "Original (Japanese)". A Japanese copy gets English with **Add English patch...** (Settings): the player picks the patch's folder (its `repatch` folder, a Vita3K tree, or the three files loose), the first folder up to four levels down with the three files is used, its `AMAGAMI.IMG` is checked against the table (another version only if the player agrees), and the files are copied to `<root>/lang/en/extracted` and chosen (`lang=en`). The patch changes 64 of the 260 system classes, logic included (`K2X_Script`, `EventNode`'s trophies), not only text. **Controls...** (Settings) rebinds the keyboard (technical/input.md). **Shortcuts (2026-10-25, checked by driving the launcher):** after the folder is set up the first time (`import_or_use`, copied or run in place) the program offers an "Amagami" shortcut on the desktop and in the Start menu's Programs folder, once (`shortcut_offered=1` in config.ini) and never in portable mode; **Desktop shortcut** (Settings) makes them again. `shortcut.cpp` writes them with `IShellLinkW`: target the running `amagami.exe`, working folder its own, icon the copy's `game-icon.ico` (see Icons). At every start (not in portable mode, not with developer flags) existing "Amagami" shortcuts that point at another `amagami.exe` (an older version unpacked elsewhere) are pointed at the running one (`shortcut::refresh`, checked with a shortcut aimed at another program). **Updates (2026-10-25, checked with a build posing as v0.1.0):** the build knows its release (`AMG_VERSION`, set by the release workflow from the tag; any other build is `dev` and never asks). The launcher asks GitHub's public releases API once in a thread of its own (`update_check.cpp`, WinHTTP, 5-second timeouts, nothing about the player sent) and, when the newest tag is higher, shows a "Get the new version, vX" button above the others, opening the releases page; the running version is written quietly beside the title. Settings, **Check for updates** (`updates=off`) turns it off. Saves, settings and the imported copy live outside the program's folder, so replacing the program loses nothing; in portable mode they are in `portable-data` beside it, kept when unpacking over it. **Not done:** the zip layout and a static C runtime, the launcher has no picture yet.

**The layout since 2026-10-31 (docs/technical/launcher-design.md).** The owner found the window one long column. It is now one window of about 760x520 at 100 %, wider than tall: a header that never moves (the game's icon, the title, the version, a status line with a coloured mark that names the failed check and its fix, the buttons of the moment: Use the copy found, Choose game folder..., Get the new version; and **Start game**, the default button) and four tabs: **Game files** (the folder, the list of checks, which scrolls, the game's language, Choose game folder..., Add English patch..., Open saves folder, Delete copy of the game...; it comes forward when a check fails), **General** (launcher language, launcher at start, check for updates, splash screen; Desktop shortcut), **Display** (window mode, interface size, render scale, filter, shape, brightness, gamma, vertical sync, frame counter, in two columns) and **Controls** (button pictures, confirm button, unknown pad; Controls... and Pad mapping...). There is no separate Settings window any more; F8 over the game opens the same General, Display and Controls tabs. The launcher is per-monitor DPI aware with the system's message font, so it is sharp at 125 and 150 %; the manifest asks for the common controls 6 (visual styles). Changing the launcher language rebuilds the window at once. Keys: Enter starts, Esc closes, Ctrl+Tab and Ctrl+1..4 change tab. Development switches: `AMG_LAUNCHER_POS=X,Y`, `AMG_LAUNCHER_DPI=N`, `AMG_LAUNCHER_TAB=N`; `tools/capture_launcher.py` captures the launcher's own window off screen with them.

## The title's music started during the logos (2026-10-27, confirmed)

The owner heard the intro music while the brand logos were still on screen; on the Vita it starts at the title (the snow and the heroines). Cause: the script asks for the track at the first logo (tick 75: `play(73)`, `setVolume(48)`, `pause(1)`) and resumes it at the title (`pause(0)`, tick 780). The port loads a track in a thread (about 0.5 to 1 s), and its start routine cleared the pause that the script had set while the track was loading, so the music ran from the first logo. The engine now remembers a pause asked after the last play and applies it when the track is ready (`music_pause_asked_`). It was not the splash screen. `AMG_VOICE_LOG=1` now prints every music order with its tick (`music asked: ...`) and `music ready, paused=N`.

## Window mode and vertical sync (2026-10-27, confirmed for the window mode at start)

Two more rows of the port's settings (F8 over the game, and the launcher's Display tab; not in the game's own Settings screen, which has room for five): **Window mode** (`window`: `windowed` or `fullscreen`, the borderless full screen of F11 and Alt+Enter, which now also write the row) and **Vertical sync** (`vsync`: `on` by default, `off` shows each frame at once and may tear; the game's own 60 ticks a second are paced by the machine, so a faster display does not speed the game up). A row change posts `WM_APP + 1` to the window, whose thread does the change. Checked: a copy of the program with `window=fullscreen` in `portable-data/config.ini` opens covering the screen. Not done yet (F5-23 and F5-12): exclusive full screen, a frame-rate cap, an on-screen frame counter, brightness and gamma, more filters.

## The window opens to the screen it is on (2026-10-27, confirmed on a 1920x1080 display at 150 %)

Before, the window was 960x544 times 2 at a position Windows chose, and the program did not declare DPI awareness: on a display scaled to 150 % Windows stretched it (blurry, and the real size was not what the program thought; a full-screen capture came out 1920x1080 while the program saw 1280x720). Now the game's window (the awareness is per thread, set before the window is created; since 2026-10-31 the launcher's thread is per-monitor aware too and scales its own layout, see launcher-design.md) is per-monitor DPI aware (v2), so every size is in real pixels; moving it to a screen with another scaling follows `WM_DPICHANGED`. The first size is the biggest window of the game's shape (960:544) that takes 80 % of the main screen's work area, centred (1525x864 on that display); after the player moves or resizes it the client size and position are kept in `config.ini` (`ww`, `wh`, `wx`, `wy`, written when the move or resize ends) and used again while that place is still on a screen. `--scale N` keeps meaning N times the frame. Full screen covers the monitor in real pixels, so the picture goes out at the screen's own resolution; the scene is drawn at the render scale (`Auto` follows the window) and the filter scales it to the window.

Three more screen filters, all in the same shader (`d3d_present.cpp`): **Bicubic** (Catmull-Rom, sixteen texels), **Lanczos** (two lobes, normalised) and **Sharpen** (the linear sampler plus a contrast-adaptive pass, the idea of AMD's CAS). They are values of the `filter` row. Checked in the window on that display: all three compile and draw without artefacts; which one looks best is a matter of taste. Brightness (80 to 130 %, a factor) and Gamma (0.8 to 1.3; the picture goes through `pow(c, 1 / gamma)`, so a higher value brightens the middle tones, as in most games' sliders) are rows too, applied last by every presenting shader through a constant buffer; measured on the window: mean luminance 121.6 at the defaults, 97.7 at 80 %, 158.6 at 130 %, 101.8 at gamma 0.8, 144.3 at 1.3. Not done: a frame-rate counter on screen, exclusive full screen.

## The launcher finds the game by itself (2026-10-27, confirmed in the window)

When the folder is not usable (first start, or a moved copy), the launcher searches in a thread for a usable copy where one usually is: a Vita3K install (`%APPDATA%\Vita3K\Vita3K`, its `ux0/app/PCSG00291`), the program's own game folder, the desktop, the documents and the downloads (`find_installed_copies` in `datapath.hpp`, which uses `find_game_root`: breadth first, at most 2,000 folders per base). A big button then says "Use the copy found in your Vita3K" (or the folder's name) and one click checks it and offers the copy (`use_copy`). A folder can also be dropped on the launcher (`WM_DROPFILES`), and the header says in plain words what is needed ("Choose your own decrypted PS Vita copy, or drop its folder here"). Strings are in English, Spanish and Japanese. **A bug found on the way:** the search ran in a thread and `std::filesystem::path::string()` throws for a folder name the code page cannot hold (the desktop had one), which ended the whole program with 0xC0000409; the loop now skips such folders and a search never ends the program. The same call is in `find_game_root`, which the folder picker also uses, so it is fixed there too.

## A report when the program stops by itself (2026-10-27, confirmed by a deliberate crash)

An unhandled exception (`SetUnhandledExceptionFilter`) or a terminate (`std::set_terminate`) writes `crash.txt` in the program's data folder (`%APPDATA%\open-amagami`, or `portable-data` beside the program): the version and bitness, the exception code and where (module and offset), the script's tick and the last 30 lines the scripts printed, and where to send it. A window says so (English, Spanish, Japanese) and offers to open the folder with the file selected; then the program ends (exit code 3). Since the audit below it also covers the game failing to start and a script error, and falls back to the temporary folder. A crash inside the report ends it at once. `AMG_TEST_CRASH=1` stops the program on purpose after three seconds, to see all this. The launcher's header says, once the folder is usable, that F8 opens the options in the game.

## No dead ends: the launcher's checks, the first start and every way the program stops (2026-10-27, audited by experiment; D-039 item 2)

Method: fabricated game folders and configuration files under `build/tmp/<date>-deadends/` (hard links to the real files for the big ones), the real `amagami.exe` run in portable mode, and the windows it showed read back (the launcher is drawn by hand, so it was captured by `PrintWindow`; the message boxes were read from their text). **Confirmed** = seen in the program. All with the 64-bit build; the 32-bit one only builds.

| Case | What the player saw before | After |
|---|---|---|
| no folder yet (first start) | "No folder chosen yet." | says to click the button or drop the folder; the copy found by itself is offered |
| remembered folder gone (moved, deleted, drive off) | **"No folder chosen yet."** (wrong, no name) | "The folder ... is not there any more ... Choose the folder again." (path shortened in the middle to fit) |
| remembered path is a file | the same wrong line | says it is a file |
| empty folder / a file or two missing / game one level down | "Missing: ... Choose the folder of the PS Vita version" (a list of everything, even when the game was in a folder inside) | empty: says so; missing: says what is missing and to choose a folder that has them or copy them in; one level down: names the folder inside |
| path over 240 characters | "Missing" (Windows could not open it) | the program is long-path aware (manifest), and when Windows still refuses the message says the path is long and to move the folder to a shorter place |
| folder name with accents or Japanese letters (the 'ünï ñame 天神' case), also dropped on the window | **failed**: the program's narrow strings were in the system code page, the dropped folder in UTF-8 | works: the manifest makes the code page UTF-8 (Windows 10 1903 or later; on older ones it behaves as before) |
| encrypted archive (`SCENARIO.CPK` not a CPK) | named the fix | unchanged; an empty file or one that cannot be opened is now told apart from "encrypted" |
| encrypted or damaged `eboot.bin` | the raw reason (`unexpected layout`, `unexpected header`) in English | two plain messages, in three languages: still encrypted / damaged or not the game's |
| `AMAGAMI.IMG` empty or cut short, an archive cut short | **passed the check** ("Your game is ready"), then a window "Startup error: read past the end" / "invalid string position" | the check reads `AMAGAMI.IMG` and the two archives' tables (`damaged_files`): "damaged" or "incomplete: it is X MB but the game needs at least Y MB" with what to do. The same check runs before a copy is imported and before `launcher=skip` skips the launcher |
| a failure the check cannot see, at the start | the raw reason in a window, then the program ended; the next start repeated it | window with the reason, the likely cause and what to do, `crash.txt` written, and `launcher=` is written to config.ini so the launcher opens next time (`AMG_TEST_STARTFAIL=1` shows it) |
| a script error while playing | window "Script error" with up to 1,500 characters | the same text (500 characters), plus the report and where it is |
| a crash | window and `crash.txt` (already) | unchanged, plus: when the data folder cannot be written the report goes to the temporary folder (before, the window said "saved in" a file that did not exist), and a crash in the first 30 seconds of loading says the copy may be damaged and opens the launcher next time |
| config.ini with a BOM (Notepad), CRLF, a NUL, garbage, no `=`, empty, or a folder in its place, out-of-range window values | the BOM lost the first line (the remembered folder) | the BOM is skipped; the rest was already fine (launcher shows, no crash) |
| config.ini or its folder not writable | settings silently not kept | one window: the settings could not be saved in that file, the program works but forgets |
| the launcher's check thread throwing (any filesystem error) | would have ended the whole program | caught and shown as a failed check |
| import failures (read, write, finish) | "Could not copy X." | say what to look at (room, a readable folder) |
| the sound sources missing | warning with no remedy | the warning says to copy the `sound` folder next to the other files |
| saves folder is a file, saves are noise or empty | (not looked at) | the game runs to the title for 30 s with no crash and no report; what the game's own scripts then say on loading is **not checked** (the Vita error screens were tested through `AMG_CARD_STATUS`, tests Y5 and Y6) |

New strings are in English, Spanish and Japanese in `ui_text.cpp`; the Japanese of the table grew past its limit in `publish/policy.json` (`ui_text.cpp` 2,800 to 3,800 characters: it is our own translation of the new messages, not game text).

**Not done or not seen (pending):** an access violation while reading a damaged script archive whose table is intact (one `SCENARIO.CPK` with noise in the middle crashed once at the start and not on the next try; the crash report and the launcher-next-time path handle it, but the check does not parse every script); a missing `SDL3.dll` (out of scope, D-039); long-path refusal by Windows could not be tried (this machine has long paths on); a machine without a Vita3K or with a non-Latin Windows language; running out of disk during the import; the pad and sound-device failures (the program plays without sound when no device opens, **unchanged**, not tried). Six rows of the port's settings (`Window mode`, `Lanczos`, ...) still have no Spanish or Japanese (`tools/ui_keys.py`).

## The 64-bit build (2026-10-27, confirmed; D-039)

```
call "...\VC\Auxiliary\Buildcvarsall.bat" x64
cmake -S port -B build/win-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/win-x64
```

The CMake file picks SDL3's `lib/x64` when the pointers are 8 bytes (it downloads the same pinned release when the folder is not in the repository). The engine's 32-bit assumptions turned out to be none that matter: the build had no errors (one warning in the sampler, a pointer truncated to a `long`, fixed). Compared with the 32-bit build: the same ticks and instruction counts on 8 scenes, byte-identical frames on 2, and `tools/sweep_windows.py` with `AMG_BUILD=win-x64` gives 0 differences of 2,136 (2,114 DONE, 5 DONE-STATE, 5 HANG, 12 ERROR: the baseline). `win-x86` stays as a test build; the release's zip name, the workflow, the site and the update check change together at the release.

## The frame counter (2026-10-27, confirmed in the window)

A row of the port's settings (`fps`, off by default): the frames shown in the last second, as "60 FPS" in the top-left corner, drawn by the presenter itself as small squares of a 3x5 font (no texture, a one-pixel shadow so it reads on a white picture; `append_fps_counter` in `win_main.cpp`, on the draw thread, so it measures what reaches the screen and not the game's own 60 ticks). Exclusive full screen is left out on purpose: the borderless full screen covers the whole monitor at its own resolution and changes no video mode, so there is nothing an exclusive mode would give that a player would notice.

## The one-click update (2026-10-27, confirmed against a local server; not yet against a real release)

When the launcher says there is a newer version, its button now offers to install it: after a Yes it downloads the release's `open-amagami-windows-x64.zip` and `SHA256SUMS.txt` (`updater.cpp`, WinHTTP, redirects followed), checks the zip's SHA-256 against the listed one, reads the zip itself (stored or deflate, entries with either slash, no entry outside the folder, never `portable.txt` or `portable-data`), writes every file beside its place as `.new` and only then moves each in, the old one going to `.old` (a running program can be renamed but not overwritten). Any failure puts everything back and offers the download page instead. Then it starts the new program and closes the launcher; the new program deletes the `.old` files when it starts (`updater::cleanup`). The saves and settings are outside the program's folder (or in `portable-data`) and are never touched. Test: `AMG_UPDATE_BASE=http://127.0.0.1:8123/` takes the two files from a local server and `AMG_UPDATE_TEST_TAG=v9.9.9` makes the launcher offer it (both needed; a normal run ignores them). Checked in the launcher by driving its window: a good release is installed (README replaced, one process after the restart, only `amagami.exe.old` left until the next start, then none), and a wrong checksum installs nothing and says so, leaving the folder as it was. **What the checksum does and does not give:** it catches a damaged or incomplete download; it does not prove the release is ours, since the zip and its list come from the same place. Code signing (D-028) is the answer to that and still waits for the identity question; a program in a protected folder (Program Files) cannot be updated this way and is sent to the download page.

## Fit with blurred edges (2026-10-27, confirmed in the window)

A fifth value of the screen-shape row (`aspect=blur`, "Fit, blurred edges"): the picture is fitted as always (the whole frame, nothing cropped), and where the window is wider or taller than the game's 16:9 the bars are filled with the same frame enlarged to cover the window, blurred (forty taps spread over a disc by the golden angle, `ps_blur`) and darkened to 55 %. It is the cheap answer for ultrawide and odd window shapes: nothing in the scenes is moved or invented (the scripts' 480x272 space has no more picture to show; extending backgrounds per scene stays open as F5-5). The brightness and gamma apply to it too.

**Robustness (2026-10-29).** `Graphics::ui_pixels` summed `start + 1024 + pitch * h` in `size_t`, which wraps on x86: sizes read from a mismatched `eboot.bin` (a Vita3K tree's patch layer next to another layer's data) passed the check and the picture loop read outside the buffer (an access violation, x86 only). The sum is 64-bit now and the size is capped at 4096; the picture is missing instead of the program crashing. `AMG_CRASH_TRACE=1` makes `amagami_vm` print the offset of an unhandled exception, to look up in `build/win-x86/amagami_vm.map`.

**Unreadable save data (2026-10-29).** The save files carry an envelope (magic, kind, length, CRC-32), so damage is caught by `read_save`. A file whose envelope is whole but whose contents the game's scripts cannot read (a save from another version of the port) threw `ArrayIndexOutOfBoundsException` in `GameParam.restoreSystemData` at every start: a dead end. `tools/fuzz_save.py` (valid envelope, garbage, zero, 0xFF, short or long payloads; 33 trials): no access violation and no hang, only that script error. The window now moves the `vita_*` files to `unreadable-<time>` in the save folder when the error comes from a restore (`GameParam.restore`, `ShioriData.restore`, `MemoryCardCheck`), says so in the window and the report, and the next start is clean. Confirmed by running the window with a garbage system file. The original game has no such handling; this is the port's.

**Non-Latin paths (2026-10-29, confirmed).** `amagami.exe` (x64) started with `--root` on a folder named with Japanese and accented letters (`テスト ゲーム ñ é\データ`) opens the game window and runs, thanks to the UTF-8 manifest. `amagami_vm.exe` (the headless tool) has no manifest, so its command line loses such characters and it cannot open the folder; it is a development tool and is left so.
