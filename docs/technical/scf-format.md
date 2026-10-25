# Scripting language, `SCF` format and system image

Confidence levels: **confirmed**, **hypothesis**, **pending**. See [disc.md](disc.md).

Almost all of this document is preliminary. Formalising it is the work of phase 2.

## The language (hypothesis)

The game runs on its own virtual machine, with a Smalltalk-style object-oriented language:

- **One class per file**, with a name, parent class, methods and bytecode.
- **Message sends by name.** Scripts store the names of the methods they call (`fadeIn`, `fadeWait`, `levelUpper`) and the classes they use (`K2X_Script`).
- **Native classes** implemented in C++: `Object`, `Class`, `Interface`, `String`, `Integer`, `Float`, among others.
- **Garbage collector.**
- **System image:** the base classes come precompiled in `AMAGAMI.IMG`.

The original name of the language and of its compiler are unknown.

## `SCENARIO.ARC` inventory (confirmed)

2,485 classes. All share the header `SCF\x1A 00 04`.

### By parent class

| Parent class | Classes |
|---|---|
| `n0_MacroScripts` | 1,790 |
| `n0_HanyouEncount` | 117 |
| `Object` | 17 |
| Others, mostly scene classes such as `n1nka_me00A` | 561 |

### By name

| Pattern | Classes | Interpretation |
|---|---|---|
| `n…` | 2,421 | Scenes |
| `c##_cutin` | 35 | Cut-in windows |
| Others | 29 | A complete mahjong game (`Mahjong`, `Tehai`, `Yama`, `Taku`, `Kawa`, `Hai`, `Naki`, `AgariB…`, `ScoreB…`) and utilities (`Player`, `Select…`, `Number…`, `BitCal…`, `CtrlEx…`) |

Mahjong being written in script indicates that game logic lives in the scripts, not in the executable.

### Scene naming scheme (hypothesis)

Examples: `n1ski_me03A`, `n4nka_ec01B`, `n0fre_gd02A`, `n2sox_ep01A`.

| Part | Values seen | Interpretation |
|---|---|---|
| Digit | 0 to 6 | Heroine; 0 would be shared |
| Three letters | `dea`, `ako`, `sir`, `ski`, `nka` | Relationship level: *deai*, *akogare*, *shiriai*, *suki*, *nakayoshi* |
| | `fre`, `sox`, `soe`, `skx`, `nkx` | Pending |
| Two letters | `me`, `ec`, `se`, `sb`, `sf`, `ep`, `ev`, `xp`, `xd`, `kd`, `gd` | Scene type; pending |
| Number and letter | `01A`, `03B` | Scene number and variant |

## `SCF` format (confirmed)

Little-endian integers. A *string* is a `u16` length followed by that many bytes, not zero-terminated, in Shift-JIS (identifiers mix ASCII and Japanese).

| Order | Field |
|---|---|
| 1 | Magic `SCF\x1A` |
| 2 | `u8` flags: 0, or 8 for a built-in class |
| 3 | `u8` version: 4 |
| 4 | String: class name |
| 5 | String: parent class name |
| 6 | `u16` count, then per instance variable: `u8` slot number, string name |
| 7 | `u16` count, then per class variable: `u8` slot number, string name |
| 8 | `u16` count, then per instance method: string name, `u16` (argument count, hypothesis), `u16` code length, bytecode |
| 9 | `u16` count, then per class method: the same |
| 10 | `u16` count, then the constant pool |

The file ends exactly after the constant pool.

### Constant pool

Each constant is a `u8` tag followed by its data:

| Tag | Data | Meaning |
|---|---|---|
| 0, 2, 4 | none | Not confirmed; probably `nil` and the two booleans |
| 1 | `i32` | Integer |
| 3 | `f32` | Float |
| 5 | string | String literal |
| 6 | string | Reference to a class, by name |
| 7 | string | Symbol: a method name used in a message send |
| 8 | `u16` count, then that many constants | Array literal; can nest |

### Verification

`tools/hokago.py scf` parses a class into its parts and writes it back. All 2,485 classes in `SCENARIO.ARC` and all 258 in the system image rebuild byte-identically, with no bytes left over.

### Bytecode

See [scf-bytecode.md](scf-bytecode.md).

## `AMAGAMI.IMG`: system image

### Compression (confirmed)

The file is a little-endian `u32` with the decompressed size (`0x20E8CE`, 2,156,750 bytes) followed by a classic LZSS stream, the scheme of Haruhiko Okumura's `LZSS.C` with these parameters:

| Parameter | Value |
|---|---|
| Window | 2,048 bytes, initially all zero |
| First write position | `0x7DE` (window size minus the longest match) |
| Flag byte | Read from the least significant bit; 1 = literal byte, 0 = match |
| Match | Two bytes `a b`: window position `a \| ((b & 0xE0) << 3)`, length `(b & 0x1F) + 3` |
| Match length | 3 to 34 bytes |

Positions are absolute within the window, not distances back from the current byte.

The routine is at `0x1183D8` in the executable (`0x1183D0` returns the decompressed size). `tools/hokago.py lzss` implements both directions:

- **Decompression** produces exactly the declared size.
- **Compression** uses the same binary-tree match search as `LZSS.C`, and recompressing the decompressed image gives back the original file byte for byte.

```sh
python tools/hokago.py lzss unpack extracted/AMAGAMI.IMG build/AMAGAMI.IMG.bin
python tools/hokago.py lzss verify extracted/AMAGAMI.IMG
```

### Contents (confirmed)

The decompressed image is a table of 258 classes:

| Offset | Type | Field |
|---|---|---|
| `0` | `u32` | Total size of the image |
| `4` | `u32` | Class count `n` (258) |
| `8` | `u32[n]` | Offset of each class name |
| `8 + 4·n` | `u32[n]` | Offset of each class |
| after | — | The names, zero-terminated, back to back; then the classes, back to back |

Each class is a complete `SCF` file. `tools/hokago.py scf verify-image` splits the image, parses every class and rebuilds the whole image byte-identically.

What is inside:

- **14 built-in classes**, marked with flag 8: `Class`, `Symbol`, `Integer`, `Float`, `Array`, `ByteArray`, `UShortArray`, `Nil`, `True`, `False`, `Thread`, `System`, `Math` and `Compiler`. Their behaviour is mostly native code in the executable.
- **A standard library**: `Object`, collections (`Vector`, `ArrayedCollection`, `Iterator`), streams, and an exception hierarchy (`Throwable`, `Exception`, `Error`, `IOException`, `ArrayIndexOutOfBoundsException`).
- **The skeleton of a compiler written in the language itself**: `Compiler`, a parser-combinator family (`CompilerParser`, `CompilerSequence`, `CompilerAlternate`, `CompilerRepetition`, `CompilerTerminal`), `CompilerAssembler` and a `StreamTokenizer`. It only parses arithmetic expressions (`+ - * / ( )`), so it is not the compiler that built the game. It still hints at the original source syntax: a `class` keyword and C-style `/* */` comments.
- **The game's systems**: `Application`, `GameMain`, `K2X_Script` (base of 27 classes), `Sprite`, `NodeMapView` and `NodeMapPos` (action map), `KaiwaTurn` (conversation), `GameMenuMap`, `ConfigMain`, `SystemSave`, `A0_album`, the mahjong front end, and `n0_MacroScripts`, the base class of the scenes.

This confirms that the action map, the conversation system, menus and saving are script classes, not engine code.

The image is loaded by the function around `0x103700`: it reads the file, decompresses it and hands it to `func_00123008`.
