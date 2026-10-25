# The executable `SLPS_259.87`

Confidence levels: **confirmed**, **hypothesis**, **pending**. See [disc.md](disc.md).

## Header (confirmed)

- 32-bit, little-endian, MIPS ELF.
- Entry point: `0x100134`.
- A single loadable segment: file offset `0x1000`, address `0x100000`, `0x19B180` bytes in the file and `0x2C9370` in memory.
- No symbol table: no function or variable names at all.

### Container layout (confirmed)

After the loaded image the file holds, in order: a zero-filled gap, `.reginfo` (24 bytes, not loaded even though its header carries an address), the section name table and the section headers. `tools/make_elf.py` rebuilds this container around a rebuilt image and reproduces the original file's SHA-1.

## Sections (confirmed)

| Section | Address | Size | Contents |
|---|---|---|---|
| `.text` | `0x100000` | `0xA7458` (685 KB) | Code |
| `.ctors` | `0x1A7458` | `0x20` | Static constructors |
| `.dtors` | `0x1A7478` | `0x1C` | Static destructors |
| `.reginfo` | `0x1A7494` | `0x18` | MIPS register information |
| `.data` | `0x1A7500` | `0xE07F0` (919 KB) | Initialised data |
| `.eh_frame` | `0x287CF0` | `0xE9C` | C++ exception tables |
| `.jcr` | `0x288B8C` | `0x4` | — |
| `.rodata` | `0x288C00` | `0xDA70` (56 KB) | Read-only data; jump tables |
| `.gcc_except_table` | `0x296680` | `0x60` | C++ exception tables |
| `.sdata` | `0x296700` | `0x4A80` | Small data |
| `.sbss` | `0x29B180` | `0x10C` | Uninitialised small data |
| `.bss` | `0x29B300` | `0x12E070` (1.2 MB) | Uninitialised memory |
| `.mdebug.eabi64` | — | 0 | ABI marker |

Pending: `.data` is larger than the code. What it holds is unknown (task F1-10).

## Code split

| Region | Range | Size | What it is | Functions |
|---|---|---|---|---|
| Own A | `0x100000` to `0x124678` | 149,112 bytes | `K2R` engine, `K2X` layer and virtual machine | 625 |
| CRI | `0x124678` to `0x14FC88` | 177,680 bytes | CRI ADX, AFS and file system | 1,626 |
| Sony and C | `0x14FC88` to `0x17DB28` | 188,064 bytes | Sony libraries, kernel and C library | 985 |
| Own B | `0x17DB28` to `0x1A7458` | 170,288 bytes | More game code, in small functions | 935 |

- **Own code:** 319,400 bytes, 47% of the code, in about 1,560 functions. Function counts come from splat's disassembly.
- **A/CRI boundary (hypothesis, strong).** `func_001244A0` is the last own function: it references `findClass` and is called from region A. The next one, `func_00124678`, is called 20 times, all from CRI code.
- **Sony/B boundary (hypothesis, strong).** `func_0017DB18` is called only from Sony code; from `func_0017DB28` onwards the callers are in region A.
- **CRI/Sony boundary (approximate).** It lies between `0x14FB18` and `0x150208`. Both sides are black boxes, so its exact position does not affect the matching work.
- **Region B** averages 180 bytes per function. The interpretation is that these are inline functions, template instances and virtual methods the linker grouped at the end. Unconfirmed.
- **Pending:** the very start of region A (`_start` is at `0x100134`) may be Sony's startup code rather than game code.

`config/splat.yaml` splits the code at these boundaries, and `tools/progress.py` reports matched bytes per region.

### Method

1. Boundaries: for each function near a boundary, look at which regions call it.
2. Strings: scan `.text` for instruction pairs that build an address (`lui` followed by `addiu` or `ori`), noting when that address is a readable string.
3. Function list: taken from splat's disassembly (`python tools/progress.py --csv build/functions.csv`).

## Compiler (confirmed)

The game's own code was built with **ee-gcc 3.2** (Sony's GCC 3.2 for the Emotion Engine) using **`-O2 -G0 -fno-exceptions`**.

Evidence:

| Check | Result |
|---|---|
| Template name encoding | ee-gcc 2.96 emits `t11CPoolObject1Z12K2R_CFileADX`; ee-gcc 3.2 emits `11CPoolObjectI12K2R_CFileADXE`, which is what the binary contains. This rules out 2.96. |
| Function match | Eleven functions rewritten in C++ (`src/main.cpp`, `src/K2X_Appri.cpp`, `src/K2R_Midi.cpp`) compile to the original bytes, including the padding between them. |
| Whole-image check | With those files linked in place of their disassembly, the rebuilt image is still byte-identical to the original. |
| `-G0` | Variables that live in `.sdata` are accessed through full addresses instead of the global pointer. |
| `-fno-exceptions` | `main` holds a local object with a destructor across a call. With exceptions enabled the compiler adds an unwind path and saves an extra register; the original has neither. |

Open points:

- **Exact build.** Builds `3.2-ee-030926` and `3.2-ee-040921` produce the same output for the functions tried. The Makefile uses `040921`.
- **Per-file flags.** `func_00102EA8` and `func_001034C0` access variables through the global pointer, which `-G0` forbids. Some source files were probably built with a different `-G` value. Unverified.
- **RTTI.** Type information is present in the binary, so `-fno-rtti` is not in use; no matched function exercises it yet.
- The string `Append: GCC2096 SCE3100` belongs to the CRI library (built on 22 November 2006) and describes CRI's compiler, not the game's.

The compiler is a 2004 32-bit Linux binary. It fails to read files from some modern file systems (for instance a Windows drive mounted in WSL), so the build preprocesses with the host `cpp` and pipes the result in.

### Matching conventions learned so far

- The compiler emits two copies of every constructor and destructor (`C2`/`C1`, `D2`/`D1`), plus a deleting destructor (`D0`) for virtual ones. They appear back to back in the binary and all come from one definition in the source.
- A class's virtual table is emitted in the file that defines its first non-inline virtual function. Declaring an earlier virtual function keeps the table out of a file until the class is understood.
- Functions are aligned to 8 bytes.
- For a run of stores at the end of a function, the compiler emits the last source statement first and puts the second-to-last in the delay slot of the return. The source order has to be worked back from that.

## Identified functions (confirmed)

| Address | Symbol | Notes |
|---|---|---|
| `0x100A08`, `0x100A20` | Two MIDI slot helpers | Placeholder names; in `K2R_Midi` |
| `0x100388` | `main` | Creates a `K2X_Appri`, runs it, destroys it, then calls `func_00173610` |
| `0x103BE0` | `K2X_AppriMain::run()` | Not yet decompiled; class name is a placeholder |
| `0x104070`, `0x1040A0` | `K2X_Appri::K2X_Appri()` | Allocates the 1-byte main object |
| `0x1040D0`, `0x104128` | `K2X_Appri::~K2X_Appri()` | Prints `Appri destruct` |
| `0x104180` | `K2X_Appri::run()` | |
| `0x104198`, `0x1041A8`, `0x1041B8` | Virtual destructor of an unidentified class | Virtual table at `0x280190` |
| `0x15CCE8` | `operator delete(void*)` | |
| `0x15CD30` | `operator new(unsigned int)` | |
| `0x168E30` | `printf` | |

### Library functions (black boxes)

Names live in `config/symbol_addrs.txt`. They make the game's own code readable; the functions themselves are not decompiled.

| Group | Named | How | Confidence |
|---|---|---|---|
| Kernel system call stubs (`0x1701C0` to `0x170A50`) | 70 of 138 | Each stub loads a call number and executes `syscall`; names follow the well-known PS2 kernel numbering | Hypothesis: the numbering was applied from memory and should be checked against a reference table. The other 68 stubs are left unnamed |
| CRI middleware | 119 | CRI's error strings include the name of the function that reports them; a function gets a name when all its strings agree and no other function claims it | Confirmed by the strings; names keep CRI's internal lowercase spelling |
| C library and compiler runtime | 13 | Read by hand: `malloc`, `free`, `memalign`, `strcpy`, `strlen`, `sprintf`, `sinf`, `cosf`, `__fixsfsi`, `__extendsfdf2`, and (2026-10-16) `memcpy` (`0x168ACC`, a 32-byte `lq`/`sq` copy loop), `memset` (`0x168C80`, `pcpyh` replicates the byte) and `rand` (`0x168F40`, a 64-bit generator kept at offset `0xA8` of `_impure_ptr`, returning `(x >> 32) & 0x7FFFFFFF`) | Hypothesis, from recognisable code patterns (strong for the last three) |

`K2X_AppriMain::run()` at `0x103BE0` is the main loop: it allocates four stacks with `memalign`, creates four threads with `CreateThread` and then polls them with `ReferThreadStatus`.

## Own code: what is known

### Source files (confirmed)

Two names survive in error messages: `source/K2R_Midi.cc` and `source/K2X_Appri.cc`.

### Classes (confirmed)

The names come from the binary's type information. There are about a hundred classes with the `K2R_` prefix:

| Family | Examples |
|---|---|
| Interfaces | `K2R_IGraphic`, `K2R_IFile`, `K2R_IArcFile`, `K2R_IFont`, `K2R_IMidi`, `K2R_IPicture`, `K2R_ISprite`, `K2R_ITexture`, `K2R_IVoice`, `K2R_IWave`, `K2R_IObject`, `K2R_IDesc`, `K2R_IPicLoader` |
| Files | `K2R_CFile`, `K2R_CFileADX`, `K2R_CArcFile`, `K2R_CArcFileADX`, `K2R_CArcFileAFS`, `K2R_CDescADXF` |
| Pictures and textures | `K2R_CPicture`, `K2R_CPicBack`, `K2R_CPicPacL`, `K2R_CPicPacS`, `K2R_CTexBase`, `K2R_CTexBack`, `K2R_CTexFont`, `K2R_CTexFontCache`, `K2R_CTexPack`, `K2R_CTexWork` |
| Sprites | `K2R_CSprite`, `K2R_CSprBase`, `K2R_CSprComp`, `K2R_CSprTex`, `K2R_CSprRot`, `K2R_CSprScl`, `K2R_CSprClr`, `K2R_CSprStr`, `K2R_CSprVtx` |
| Audio | `K2R_CVoice`, `K2R_CWaveADX`, `K2R_CWaveDeviceADX`, `K2R_CWaveNil`, `K2R_CMidi` |
| Text | `K2R_CFont` |
| Screen objects | `K2R_CObjBalloon`, `K2R_CObjStandPic`, `K2R_CObjActorPic`, `K2R_CObjBackPic`, `K2R_CObjEventPic`, `K2R_CObjExtraPic`, `K2R_CObjStructPic`, `K2R_CObjCutInWindow`, `K2R_CObjClock`, `K2R_CObjMapNavi`, `K2R_CObjNodeIcon`, `K2R_CObjSnow`, `K2R_CObjLightBall`, `K2R_CObjSprFont` |

Unprefixed infrastructure classes: `CPoolObject<>`, `CRawList<>`, `CRawLink<>`, `CObjeCache<>`, `CKnotMem`, `CKnotTex`, `CLinkKnot`, `CPacLink`, `CLocalObject`, `CMemBlock`.

### Virtual machine (hypothesis)

It lives in region A, between `0x118000` and `0x124700`. Its strings give it away:

- Language classes and methods: `Object`, `Class`, `Interface`, `String`, `initialize`, `addMethod`, `addConstant`, `newWithArray`, `findClass`.
- Native class error messages: `String:newFromByteArray…`, `Integer:< right value was…`, `Float:+ right value was not…`.
- Garbage collector: `GC start`, `GCFR start`, `freeObject():Undefined…`.
- Loader: `K2X_Script` and `SCENARIO.ARC`, referenced near `0x114000`.

### Resource paths (confirmed)

The engine builds file names from these patterns: `stand/e%1x_%02d%c`, `stand/x%c_%c%c`, `stand/%s%c`, `bg/%03d_%1d%1d`, `epi/e%1x_%02d%c0`. The extensions are `.tm2` and `.zb`. For sound: `%s%1d.hd`, `%s%1d.bd`, `BGM%02dSQ.sq`.

The picture kinds it knows are `FontChar`, `FontCharEx`, `FontChar2`, `BG_Picture`, `EventPicture`, `ExtraPicture`, `StructPicture` and `ActorPicture`.

### Other hash tables (hypothesis)

A family of near-identical routines around `0x17F3A8` to `0x17FF98` takes a hash stored in an object, reduces it modulo a table size held at `0x1AF688` and walks 20-byte entries. It looks like an object cache, not the `ARC` lookup.

## Tables

### Jump tables in `.rodata` (confirmed)

There are 19 with five or more entries. The largest:

| Address | Entries | Distinct targets | Target range | Interpretation |
|---|---|---|---|---|
| `0x293CD8` | 243 | 78 | `0x163224` to `0x165B9C` | Sony and C region; not the virtual machine |
| `0x28B85C` | 106 | 55 | `0x11BF20` to `0x11C950` | Virtual machine region. Candidate for instruction dispatch; unconfirmed |
| `0x294268` | 89 | 24 | `0x16A740` to `0x16B150` | C library region |
| `0x288C00` | 82 | 10 | `0x100C30` to `0x100EA8` | `K2R_Midi` area |

### Code pointers in `.data` (confirmed)

There are 252 runs of three or more code pointers, with 4,743 pointers and 1,292 distinct targets. They are virtual tables and, probably, native method tables. Several runs have exactly 61 pointers, which suggests a family of classes sharing a 61-method interface.

## Matching a function, step by step (2026-10-15)

Inside WSL, from the repository root:

```sh
python3 tools/progress.py --csv build/functions.csv   # once, after make setup
python3 tools/matchfn.py list --max-size 64          # candidates, smallest first
python3 tools/matchfn.py show 0x0017e220             # the original
python3 tools/matchfn.py compare SRC.cpp 0x0017e220  # compile and diff
```

The function in the source must be named `func_<ADDRESS>` (the tool looks for that name in the object, mangled or not). When `compare` says MATCH, the function is added to the matching `src/` file and its address range is moved to the `cpp` subsegment in `config/splat.yaml`; `make` then confirms that the whole image is still identical to the original.

Masked in the comparison, because they depend on where the code is linked: the target of a jump (`R_MIPS_26`), the halves of an address (`R_MIPS_HI16`, `R_MIPS_LO16`) and global-pointer offsets (`R_MIPS_GPREL16`).

A first check of the loop: `func_0017DBF8` (`return 0`) and `func_0017E220` (`return s->d` for a four-word struct) compile to the original bytes with ee-gcc 3.2-040921.

## Automatic matching of trivial functions (2026-10-15)

`tools/automatch.py` reads every function of the game's own code up to 32 bytes, recognises the shape of its instructions (empty function, constant, getter, setter, float getter/setter) and writes a candidate. `matchfn.py`'s comparison keeps only the instruction-for-instruction matches; those become `src/auto/func_<ADDRESS>.cpp`, declared `extern "C"` under the symbol name of `symbol_addrs.txt` (the assembly and the data refer to them by that name), each in its own `cpp` subsegment of `config/splat.yaml`. `make` confirms that the whole image is still byte-identical.

First run, functions up to 16 bytes: 147 of 149 candidates match; own-code progress goes from 480 to 1,656 bytes (0.52 %), and from 11 to 158 matched functions. The remaining shapes up to 24 bytes (setting the vtable pointer, chained loads and stores) need the class layouts and are the next group. The bulk of the code, functions of tens to hundreds of bytes, is matched one by one with `matchfn.py`.

## Pilot: 48 functions of 24 to 64 bytes by four agents in parallel (2026-10-15)

All 48 matched, 2.1 attempts on average (1 to 6). 46 went into the build; the image stayed identical. Two define a `.sbss` variable of their own and wait in `work/pending/` until the real symbol can be used (the address of that data is part of the code). Lessons, collected from the four reports:

- **Order of stores and loads is scheduled, not copied.** When only the order differs, try the permutations of the statements with a small script and `compare` each; it takes seconds. Declaring temporaries in the order the original assigns registers (not the order of use) fixes register numbers.
- **The interface changes the registers.** Returning a value moves a result from a `t` register to `v0`; operand order in `==` changes which register comes first in `bne`/`xor`; a ternary's polarity decides the `movz` operand order; an `unsigned char`/`unsigned short` parameter gives the `andi`.
- **Idioms.** A vptr store followed by a base constructor is `a->vptr = vt; base(a);` and the compiler makes the tail `j` itself. A call through a table is `((void(*)(O*,int,int))a->vptr[N])(a,b,c)`. A `this`-adjusting thunk is `g(a + (*(int**)a)[-3])`. A `sd ra` with a real `jal` to a base is a real destructor (`struct D : B { ~D() {} }` called as `d->~D()`), not a hand-written one: a plain function would tail-call.
- **Forcing codegen.** An int-to-float bit cast through the stack: `volatile int i; return *(volatile float*)&i;` (a union or plain cast is folded away). A float zero from a register: the chained `v.e = v.b = 0.0f;` (separate statements give `sw zero`). A gp-relative address needs the variable defined in the same file with `__attribute__((section(".sbss")))`; an `extern` stays absolute. Five `nop` of loop padding: `asm volatile("nop")`. A real `jal` where gcc would tail-call: a trailing `asm("");` after the call (a hack; the cause in the original is not known).
- **Virtual bases.** Thunks and conversions through virtual bases match with a class with N virtual bases; the vbase offset in the vtable is -(N+2)*4 (hypothesis for N: the real classes may get these offsets from virtual functions overridden through a virtual base). Take a reference parameter to avoid the null check gcc adds for pointers.
- **The comparison masks addresses, so a wrong name is not seen.** One source used `D_00299CF8` for a datum the original loads from `0x29AEF8`. `tools/integrate.py` resolves every external against what the original actually does and renames such cases.
- **A function's slot includes its padding to 8 bytes.** The subsegment of a function of 28 bytes covers 32; compiling with default alignment pads exactly like the original.

### Putting hand-matched functions into the build

`python3 tools/integrate.py work/pilot/agent1 ...` (see its docstring) re-checks each file against the original, copies it to `src/auto/`, splits the `splat.yaml` subsegments around it (padding to 8 included), writes the linker aliases that give functions and externals their plain names, and leaves `make split && make` to confirm the image is identical. Own code matched after the pilot: 3,440 of 319,400 bytes (1.08 %), 204 functions.

## Which functions use the global pointer (2026-10-15, F0-8c)

`python tools/gp_usage.py` lists the functions of the own-code assembly that address variables as `offset($gp)`. With `-G0` the compiler never does, so those functions come from sources built with another `-G` (the compiler's default is 8). **Hypothesis:** the project's `-G0` holds for the files matched so far, and the files in the runs below were built without it. A matched function that reads a small variable needs the `.sbss`/`.sdata` trick of `docs/technical/executable.md` ("Pilot") or its own `-G` flag.

```
853 functions in the own-code assembly, 154 use $gp

0x100998 - 0x100BC8    2 of   3 functions use $gp
0x101A38 - 0x102568    3 of   9 functions use $gp
0x102EA8 - 0x103658   14 of  19 functions use $gp
0x103E70 - 0x104818    7 of   7 functions use $gp
0x1053C0 - 0x105A78    5 of  11 functions use $gp
0x108AB0 - 0x108AB0    2 of   2 functions use $gp
0x10AC78 - 0x10D8A0   46 of  54 functions use $gp
0x110DB0 - 0x110DB0    1 of   1 functions use $gp
0x112160 - 0x112160    1 of   1 functions use $gp
0x113A48 - 0x113A48    1 of   1 functions use $gp
0x1150F8 - 0x1150F8    1 of   1 functions use $gp
0x117198 - 0x1173F8    2 of   2 functions use $gp
0x118518 - 0x119050    8 of  14 functions use $gp
0x119D50 - 0x11C010   40 of  61 functions use $gp
0x11CA30 - 0x11CA78    4 of   4 functions use $gp
0x11EA48 - 0x11F130    4 of  26 functions use $gp
0x11F9A0 - 0x11F9A0    2 of   2 functions use $gp
0x120F48 - 0x120F48    1 of   1 functions use $gp
0x1217B8 - 0x1217B8    1 of   1 functions use $gp
0x122898 - 0x1236E0    9 of  18 functions use $gp
```

## Composition of the data sections (2026-10-15, F1-10)

`python tools/data_inventory.py`, in bytes:

```
section               bytes   zero  ->code  ->data  float?   other  text B
.ctors/.dtors           168    116      44       0       0       8       0
.data                919536 855496   19076   13328    4492   27144     171
.eh_frame/.jcr         3856    184     328      48      92    3204       0
.rodata               55936   5368    3324     236    6532   40476   28078
.gcc_except_table       128     48       0       8       4      68       0
.sdata                19072   8400     108     428    3292    6844     369

(bytes; each count is words of four bytes)
```

**Confirmed:** 855 KB of the 919 KB of `.data` are zero words (buffers and pools filled at run time); the content is about 64 KB: 19 KB of words that point into the code (vtables and function tables), 13 KB pointing into data, 27 KB of other values. `.rodata` is 56 KB, half of it text (the strings of the engine's messages and file names).

## Library functions the own code calls (2026-10-16, F0-10)

`python tools/lib_callers.py` lists them: the own code (regions A and B) made 460 calls to 142 different library functions that had no name (139 after the three named below; 419 calls) (calls to the functions already named, such as `malloc` or `sprintf`, are not counted). The most called, by number of calls and of own functions that make them:

| Address | Calls | Own functions | What it looks like (hypothesis unless said) |
|---|---|---|---|
| `0x1694B0` | 23 | 13 | Scans a NUL-terminated string 16 bytes at a time (the same trick as `strlen`) and then calls another function, so it is not `strlen` itself: `strdup`-like. Unnamed |
| `0x168F40` | 19 | 5 | `rand` (named) |
| `0x15AC20`, `0x15AAC0`, `0x15ACE0`, `0x15AF88` | 17, 10, 10, 9 | 7, 7, 6, 5 | Wrappers that take one semaphore (`PollSema`) around a shared state at `0x3AD340`: the CRI/Sony file or CD layer. Unnamed |
| `0x15DC70` | 14 | 14 | Reads type information through the virtual table (offsets -4 and -8): the C++ runtime's `__dynamic_cast`, medium confidence. Unnamed |
| `0x168ACC`, `0x168C80` | 12, 10 | 6, 6 | `memcpy`, `memset` (named) |
| `0x150C30` | 11 | 1 | A variadic function (saves `a1` to `a7` on the stack), called eleven times by a single own function. Unnamed |
| `0x167818` | 7 | 5 | Allocates `0x88` bytes and links them at `_impure_ptr + 0x148`: the `atexit` structure of the C library. Unnamed |

`0x1694B0`, the four semaphore wrappers, `0x15DC70`, `0x150C30` and `0x167818` are left without a name until a second reading confirms them. The three new names are in `config/symbol_addrs.txt` and apply to the generated assembly after `make setup`; the image does not change.
