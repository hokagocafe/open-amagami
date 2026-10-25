# Readable source for `SCF` classes

Confidence levels: **confirmed**, **hypothesis**, **pending**. See [disc.md](disc.md).

`tools/hokago.py scf_src` turns a compiled class into C-like source text and compiles that text back. Together with [scf-bytecode.md](scf-bytecode.md) this is the script toolchain the project set out to build.

```sh
python tools/hokago.py scf_src dec    class.scf class.scfs
python tools/hokago.py scf_src enc    class.scfs class.scf
python tools/hokago.py scf_src verify extracted/SCENARIO.ARC
python tools/hokago.py scf_src verify extracted/AMAGAMI.IMG
```

Decompiled text contains the game's script. Keep it under `scripts-out/`, which git ignores.

## What was verified (confirmed)

- **Every class round-trips.** Decompiling and recompiling all 2,485 classes on the disc and all 258 in the system image gives back the original bytes. A class that cannot be fully structured still round-trips because its unstructured methods are kept as instruction listings (see below).
- **99.7% of methods are structured code**: 18,897 of 18,948. The 51 left are listed under [Not yet structured](#not-yet-structured).
- **The compiler is deterministic.** The compiler's output choices (operand widths, jump sizes, padding) are all recoverable from the bytecode, so the source does not need to carry them.

## Language

### Class

```
class "AbstractKawa" : "Object" flags=0
ivar 6 "nnn立直Pos"
cvar 4 "Hai"
const class "MahjongMain"
const string "Kawa initializeClass Start"

method "initialize" args=1 locals=0 {
    ...
}

classmethod "load" args=0 locals=0 {
    ...
}
```

- `ivar` and `cvar` lines declare variables with their slot numbers.
- `const` lines are the **constant pool, in its stored order**. They are kept because the original order cannot be derived from the code (it follows the order of the original source file, while methods are stored in a different order). The code refers to constants by value, not by position, so editing a literal in the code is enough: a literal that is not in the pool is added to it.
- A method has `args` (its number of parameters) and `locals` (additional local slots).
- Anything after `//` on a line is ignored.

### Names

Parameters are `arg0`, `arg1`, …; further locals are `loc0`, `loc1`, … (the original names were not kept in the file). Instance variables are `self.<name>` using the names declared by the class and its parents, or `self.ivar<N>` when the name is unknown or ambiguous. Class variables are `Class.cvar<N>`.

A name that is not a plain identifier (Japanese method names with symbols, for instance) is written in single quotes: `'get牌'`. Names that cannot be decoded as Shift-JIS are written as `x'hex'`.

### Expressions

| Source | Instruction(s) |
|---|---|
| `5`, `-3`, `100000` | `push_int`, or `push_const` of an integer when it does not fit a signed byte |
| `"text"`, `1.5f`, `[1, 2]`, `#symbol`, `ClassName` | `push_const` of a string, float, array, symbol or class |
| `nil`, `true`, `false`, `self`, `caught` | `push_nil`, `push_true`, `push_false`, `push_self`, `push_caught` |
| `receiver.name(a, b)` | `send` with 2 arguments |
| `super.name(a)` | `send_super` |
| `a[i]`, `a[i] = v` | `index_get`, `index_set` |
| `a + b`, `a - b`, `a * b`, `a / b`, `a % b` | `add` … `mod` |
| `a & b`, `a \| b`, `a ^ b` | `bit_and`, `bit_or`, `bit_xor` |
| `a == b`, `a != b` | `eq_ref`, `ne_ref` (reference comparison, the one the game uses for integers) |
| `a < b`, `a > b`, `a <= b`, `a >= b` | `lt`, `gt`, `le`, `ge` |
| `a eq b`, `a ne b` | `eq`, `ne` (rarely used) |
| `a and b`, `a or b` | `and`, `or` |
| `-a`, `~a`, `!a` | `neg`, `bit_not`, `not` |

Precedence, from loosest to tightest: `or`; `and`; comparisons; `|`; `^`; `&`; `+ -`; `* / %`; unary operators; calls and indexing. All binary operators associate to the left. `-(5)` is the negation of 5 (two instructions), whereas `-5` is the literal.

**`and` and `or` are not short-circuit.** Both operands are always evaluated, because the instruction takes two values from the stack. Conditions that need short-circuiting are written with nested `if`s in the original scripts.

### Statements

```
expression;
target = expression;            // target: argN, locN, self.name, Class.cvarN, a[i]
return;                         // returns self
return expression;
if (condition) { ... }
if (condition) { ... } else { ... }
while (condition) { ... }
do { ... } until (condition);   // repeats until the condition is true
loop { ... }                    // endless; leave with break
break;
continue;
throw expression;
primitive(n);                   // calls native routine n of the current class
```

Every method ends with an implicit `return;`.

### Unstructured methods

A method that cannot be written with the statements above is kept as a listing:

```
method "name" args=2 asm {
    0000: locals 1
    0002: push_local 0
    0003: jump_false16 @0010
    ...
}
```

The instruction syntax is the one described in [scf-bytecode.md](scf-bytecode.md). Offsets at the start of a line are informative; jump operands are absolute offsets.

## How the original compiler generated code (confirmed)

These rules were found by measuring all 2.05 million instructions in the game, and are what the compiler in `scf_src.py` reproduces:

| Choice | Rule |
|---|---|
| Forward jumps (`if`, `else`, `break`, loop exit) | Always the 16-bit form: the target is not known when the jump is emitted |
| Backward jumps (loop back-edge, `continue`) | The 8-bit form when the distance is at least −128, else 16-bit |
| `push_const`, `push_cvar`, `pop_cvar` | 8-bit operand for indices up to 255, 16-bit above |
| `send`, `send_super` | 8-bit operand only for indices up to **127**, 16-bit above (the operand is a signed byte) |
| `nop` | Emitted just before a `send16` when the current offset is odd, so that the 16-bit operand is aligned |
| `push_int` | For values −128 to 127; anything else goes through the constant pool |
| `if … else` | `jump_false` over the then-part; the then-part ends in an unconditional jump over the else-part, **even when the then-part ends in `return`** |
| `while` | Condition at the loop head, `jump_false` to the exit, body, backward jump to the head |
| Method end | Always a final `ret`, even after a `return` |

## Not yet structured

Fifty-one methods stay as listings:

- **`try` / catch** (about 22 methods in 9 classes, such as `Vector.atPut` and `GameMain.scriptMain`). The shape is understood: `try @handler`, body, `end_try`, a jump over the handler, then a handler that tests `caught.isKindOf(Class)` and ends in `rethrow` if no clause matches. It has not been turned into a source construct yet.
- **Constant pools with repeated values** (about 27 methods). The original compiler did not always reuse an existing pool entry, so rebuilding by value picks a different index. Only affects classes with duplicate constants.
- **A few others** that fail a structural check.

These still compile exactly; they are just not readable.

## Pending

- Source syntax for `try` / catch.
- Names for local variables: the file stores none.
- A pretty-printer pass that groups the constant pool less noisily (the pool is long in big scenes).
