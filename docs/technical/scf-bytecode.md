# `SCF` bytecode and the virtual machine

Confidence levels: **confirmed**, **hypothesis**, **pending**. See [disc.md](disc.md).

The container that holds this bytecode is described in [scf-format.md](scf-format.md).

## Where it comes from

The instruction set was read from the interpreter loop in the executable, `func_0011C010` (2,480 bytes). The loop fetches one opcode byte and dispatches through two jump tables: `0x28B9CC` for opcodes `0x00` to `0x0D` and `0x28B894` for `0x20` to `0x6D`. Opcodes `0x10` to `0x1F` are handled inline, with the integer cases in `func_0011BEC8`.

**Check (confirmed):** with the table below, `tools/hokago.py scf_asm` disassembles every method of every class (2,485 classes on disc and 258 in the system image; 18,948 methods; 2,050,823 instructions). No unknown opcode appears, every jump lands on an instruction boundary, and reassembling the text gives back the original bytes.

That proves the opcode lengths and operand layouts. The *meaning* given to each instruction comes from reading the handlers and is marked where it is only a hypothesis.

## Machine model

A stack machine with tagged values.

### Values (confirmed)

| Value | Meaning |
|---|---|
| `0` | `nil` |
| `4` | `false` |
| `8` | `true` |
| Odd | Small integer: `(n << 1) \| 1` |
| Other | Pointer to an object |

### Interpreter state (hypothesis for the names)

Offsets into the structure passed to the loop:

| Offset | Field |
|---|---|
| `0x00` | Status flags |
| `0x0C` | Current method |
| `0x10` | Current class; its constant pool is at `+0x0C` |
| `0x14` | Caught exception |
| `0x18` | Value set by the `line` instruction |
| `0x1C` | Program counter |
| `0x20` | Call stack pointer (grows down) |
| `0x24` | Value stack pointer (grows up) |
| `0x28` | Frame pointer: slot 0 is `self`, then arguments and locals |
| `0x2C` | Number of active `try` blocks (at most 30) |
| `0x30` | Saved call stack pointer of each `try` block |

An object keeps its instance variables in a table at `+0x08`; variable `n` is at `4 + 4·n` in that table.

## Instructions

Operands follow the opcode. `u8` and `u16` are unsigned, `s8` is signed, and `rel8`/`rel16` are signed offsets **relative to the address of the opcode itself**. "k" is an index into the class's constant pool.

| Opcode | Mnemonic | Operands | Effect |
|---|---|---|---|
| `00` | `nop` | | Nothing |
| `01` | `nop1` | `u8` | Skips its operand (unused in the game) |
| `02` | `ret` | | Return; the result is whatever is in the `self` slot |
| `03` | `ret_top` | | Return the top of the stack |
| `04` | `dup` | | Duplicate the top |
| `05` | `pop` | | Discard the top |
| `06` | `ne_ref` | | Pop two; push `true` if they are different references |
| `07` | `eq_ref` | | Pop two; push `true` if they are the same reference |
| `08` / `09` | `push_cvar` | k (`u8` / `u16`), slot `u8` | Push variable `slot` of the class named by constant k |
| `0A` / `0B` | `pop_cvar` | k (`u8` / `u16`), slot `u8` | Pop into that variable |
| `0C` | `and` | | Boolean and; sends a message instead if an operand is not a boolean |
| `0D` | `or` | | Boolean or; same fallback |
| `10` to `14` | `add` `sub` `mul` `div` `mod` | | Integer arithmetic |
| `15` to `17` | `bit_and` `bit_or` `bit_xor` | | Integer bit operations |
| `18` to `1D` | `eq` `gt` `lt` `ge` `le` `ne` | | Integer comparisons |
| `1E` | `neg` | | Integer negation |
| `1F` | `bit_not` | | Integer bitwise not |
| `20` | `push_ivar` | n `u8` | Push instance variable n of `self` |
| `21` | `pop_ivar` | n `u8` | Pop into instance variable n |
| `22` | `push_local` | n `u8` | Push argument or local n |
| `23` | `pop_local` | n `u8` | Pop into argument or local n |
| `24` | `push_int` | `s8` | Push a small integer |
| `25` | `push_nil` | | |
| `26` | `push_self` | | |
| `28` | `push_true` | | |
| `29` | `push_false` | | |
| `2A` | `push_caught` | | Push the caught exception (hypothesis) |
| `2B` | `not` | | Boolean not; sends a message if the operand is not a boolean |
| `30` / `32` | `send` | argc `u8`, k (`u8` / `u16`) | Send the message whose name is constant k, with argc arguments |
| `31` / `33` | `send_super` | argc `u8`, k (`u8` / `u16`) | The same, starting the method lookup in the parent class (hypothesis) |
| `34` | `primitive` | n `u8` | Call native routine n |
| `40` / `41` | `jump` | `rel8` / `rel16` | Unconditional jump |
| `44` / `45` | `jump_false` | `rel8` / `rel16` | Pop; jump if it is `false` or `nil` |
| `50` / `51` | `push_const` | k (`u8` / `u16`) | Push constant k |
| `64` | `index_get` | | Element read, sent as a one-argument message |
| `65` | `index_set` | | Element write, sent as a two-argument message |
| `68` | `clear_caught` | | Clear the caught exception (hypothesis) |
| `69` | `try` | `rel16` | Enter a protected block whose handler is at the target |
| `6A` | `end_try` | | Leave the protected block |
| `6B` | `rethrow` | | Raise the exception on top of the stack: `throw` in the source form (hypothesis; most uses are not inside a `try`) |
| `6C` | `locals` | n `u8` | Reserve n locals, set to `nil`. First instruction of every method |
| `6D` | `line` | `u16` | Record a number in the interpreter state, probably a source line (unused in the game) |

Every other opcode is invalid and raises an error. `60` to `63` are explicit error traps.

### Notes

- **Arithmetic falls back to messages.** `add` and the other operators only work directly on two small integers. Otherwise the machine sends the corresponding operator message to the left operand, which is how string concatenation and float arithmetic work.
- **Booleans in `ne_ref`/`eq_ref`.** The handler for `06` pushes `false` when the two values are equal, so `06` is "different" and `07` is "same".
- **Usage.** The most common instructions are `send` (490,335), `pop`, `push_cvar`, `push_int` and `push_const`. `primitive` appears only 224 times in the whole game, with indices 0 to 25.

Those call sites reach 174 distinct native routines; see [natives.md](natives.md).

## Text form

`tools/hokago.py scf_asm dis` writes a class as text:

```
class "AbstractKawa" : "Object" flags=0
ivar 0 "nnnMe"
cvar 4 "Hai"
const class "Array"    ; #6
const symbol "new"    ; #7

method "initialize" args=1
  0000: locals 0
  0010: push_local 0
  0012: pop_ivar 0
  0018: push_const 6    ; class "Array"
  001a: push_int 24
  001c: send 1 7    ; symbol "new"
  001f: pop_ivar 1
  0041: ret
end
```

- Text files are UTF-8; names and strings are converted from and to Shift-JIS. A string that would not survive the conversion is written as hex (`x"…"`).
- Jump targets are written as absolute offsets (`@0045`).
- Offsets at the start of each line and everything after `;` are ignored when assembling.

```sh
python tools/hokago.py scf_asm dis    class.scf class.txt
python tools/hokago.py scf_asm asm    class.txt class.scf
python tools/hokago.py scf_asm verify extracted/SCENARIO.ARC
python tools/hokago.py scf_asm verify extracted/AMAGAMI.IMG
```

Disassembled classes contain the game's script. Keep them under `scripts-out/`, which is ignored by git.

For a readable form see [scf-source.md](scf-source.md).

## Pending

- Match the interpreter loop and its helpers in C++ (F2-2): `func_0011C010`, `func_0011BEC8`, the message send `func_0011BAD8`, the super send `func_0011BB70` and the primitive call `func_0011BE38`.
- The operator message names, held in a table at `0x3A7AC8` that is filled at start-up.
- Which constant-pool tag (0, 2 or 4) is `nil`, `true` and `false`.
- The meaning of the per-method `args` field, which ranges from 0 to 15.
