# The script virtual machine

Confidence levels: **confirmed** (the reference interpreter runs the real scripts correctly with it), **hypothesis** (read from the executable or inferred, not yet checked against the original at run time), **pending**.

`tools/hokago.py scfvm` is a reference interpreter written in Python. It loads the system image and the scenario classes from the disc, starts the game the way the engine does and runs it headless. It is an executable form of this document and the test bed for the C++ machine of the Windows port.

```sh
python tools/hokago.py scfvm run --steps 15000000
```

## How the game starts (confirmed)

The engine loads `AMAGAMI.IMG`, then creates a thread that runs `SystemStartup.run()`:

```
SystemStartup.run()  ->  Class.findClass(#Application).new().run()
Application.run()    ->  ConfilmDebug.new().scriptMain(); GameMain.new().scriptMain()
```

The whole game hangs from `GameMain.scriptMain`. `ConfilmDebug` offers to start a `DebugViewer` class when one exists; this disc has none, so `Class.findClass` and `K2X_Script.loadClass` return `nil` and it returns at once.

## Values (confirmed)

| Script value | In the original | In the interpreter |
|---|---|---|
| `nil` | word `0` | `None` |
| `false` / `true` | words `4` / `8` | `False` / `True` |
| Integer | odd word, `(n << 1) \| 1` | Python `int` |
| Everything else | pointer to an object whose second word is its class | object instances |

Bare constants in a class's constant table: tag 0 is `nil`, tag 4 is `true` and tag 2 is `false` (confirmed: `Character.checkCom` and `Actor.checkCom` hold tag 4 exactly for the letters that have an `inlineCom` method; with the two swapped no inline text command runs and no character voice plays).

`jump_false` jumps on `nil` and `false` only; zero, empty strings and objects are true. A value's class comes from this table: `nil` → `Nil`, booleans → `True`/`False`, integers → `Integer`, otherwise the class pointer in the object.

## Calls (confirmed)

- **Frame.** A call leaves the receiver and the arguments on the value stack; the frame starts at the receiver. Slot 0 is `self`, slots 1 to *n* are the arguments, then the locals. `locals n` pushes *n* `nil`s.
- **Return.** `ret` returns whatever is in slot 0, and `ret_top` the top of the stack. The caller finds the result where the receiver was.
- **Overloads.** Methods are looked up by name **and argument count**: `StageSet.initialize` exists with 5 and with 2 parameters, `BG_Picture.initialize` with three different counts.
- **Lookup.** The receiver's class, then its parents. A class object is an instance of `Class`: a send to it looks in its own class-side methods and its parents', then in the instance methods of `Class` (`new`, `name`, …).
- **`send_super`.** Starts the lookup in the parent of the class that defines the running method.
- **`primitive n`.** Calls entry *n* of the table that the engine registered for that class. A primitive that returns a value **writes it into slot 0**, so the wrapper `primitive n; ret` returns it. A primitive that leaves slot 0 alone makes the method return `self`, which is how setter chains such as `sprite.setPos(...).setAlpha(...)` work.
- **Built-in classes.** `Object`, `Class`, `String`, the arrays, numbers, booleans, `Math`, `System`, `Thread`: their native methods replace the script wrappers of the same name in the image.

## Operators (confirmed)

The arithmetic and comparison instructions work directly on two integers. In every other case the machine sends the operator as a message to the left operand:

| Instruction | Message | | Instruction | Message |
|---|---|---|---|---|
| `add` `sub` `mul` `div` `mod` | `+` `-` `*` `/` `%` | | `eq` `ne` | `=` `<>` |
| `bit_and` `bit_or` `bit_xor` | `&` `\|` `^` | | `gt` `lt` `ge` `le` | `>` `<` `>=` `<=` |
| `neg` `bit_not` | `-U` `~U` | | `not` | `!U` |
| `and` `or` (on non-booleans) | `&&` `\|\|` | | `index_get` `index_set` | `[]` `atPut` |

The names come from the native methods the engine registers on `Integer`, `Float`, `Boolean` and the collections. `div` and `mod` truncate toward zero, and division by zero raises an error.

## Strings (confirmed)

A string holds 16-bit characters: a two-byte Shift-JIS character is one element, an ASCII character another. `String.[]` returns the code, `length` counts characters and `Integer.asChar` builds a one-character string. Scripts depend on this, for instance to draw a name one character at a time.

## Controller (confirmed)

`ControlPad.data()` is the mask of held buttons and `trig()` the buttons newly pressed this frame:

| Bit | Button | | Bit | Button |
|---|---|---|---|---|
| 1, 2, 4, 8 | L2, R2, L1, R1 | | 256 | Select |
| 16 | Triangle | | 2048 | Start |
| 32 | Circle (confirm) | | 4096, 8192 | Up, Right |
| 64 | Cross (cancel) | | 16384, 32768 | Down, Left |
| 128 | Square | | | |

This is the pad's two status bytes in swapped order. Confirm, cancel and Start are confirmed by driving the menus; the rest follow from the layout.

## Class variables (confirmed)

A class variable and an instance variable of the class object are the **same slot**: `Foo.cvar4` and `self.ivar4` inside a class-side method of `Foo` read and write one table. `StageSet.init()` stores the shared list with `pop_ivar 4` and instances read it back with `push_cvar StageSet 4`. Class objects are not shared with subclasses: each class has its own table, numbered with the parent's slots counted first.

## Exceptions (hypothesis for the details)

- `try @handler` registers a handler; `end_try` removes it.
- A script `throw` (`rethrow`) or a runtime error unwinds to the innermost handler, which starts with `push_caught`.
- A handler tests the exception with `isKindOf` and executes `rethrow` itself when no clause matches, so exceptions the handler does not recognise continue outward.
- The runtime raises an object of class `Error` for a missing method, a missing class and division by zero.

## Threads (hypothesis)

`Thread.new().start(obj)` starts a thread that runs `obj.run()`. Threads are cooperative: they take turns when one calls `sleep(n)`, which waits *n* frames. The interpreter runs them in round-robin order on a virtual clock that advances when every thread is asleep. `K2X_Script.getVBlankCount` returns that clock.

The real game starts about fifty threads during boot (`Actor.run`, `Extra.run`, `StageSet.run`, `TextWindow.run`, `MemoryCard.run`, `Calender.run`, …), each looping on `sleep(1)`.

## What runs today

With one formatted, empty memory card and scripted button presses, the interpreter runs the real scripts from boot into the game:

1. set-up of the stage, the text window, 45 characters and the 4,355-node event map;
2. the notice, the logos and the memory card check;
3. the title sequence and the main menu;
4. "new game": the name entry screen, confirmed with Start;
5. the action map of day 1;
6. the first event script from `SCENARIO.ARC` (`n0dea_ev01A`), which opens the text window and prints the prologue.

Sound, the memory card's data and several kinds of screen object are still stubs. With `--shot` the graphics model (see [objects.md](objects.md)) renders any frame to a PNG.

## Natives the first boot needs (confirmed by running)

| Native | Why |
|---|---|
| `K2X_Script.initVoice` | Must answer true, or `initialize` waits forever |
| `K2X_Script.getVBlankCount` | The frame counter |
| `ControlPad.data`, `trig`, `pressure`, `rapid` | Return a button mask; scripts AND it with bit values (`& 2048` is Start on the title screen). `trig` is the buttons newly pressed this frame |
| `ControlPad.rapid` | The newly pressed buttons, repeating while held. Choice menus read this one, not `trig`, so it cannot be left answering 0. On Vita data the repeat is the engine's (after 20 frames, then every 5: technical/vita-executable.md, "Pad: buttons and auto-repeat"); the PS2's is not read |
| `Object.toString` | The name of the object's class, with nothing around it (`0x119F90`). Event scripts use their own class name as the key of their node in the event map |
| `MemoryCard` `isChanged`, `status`, `createFileSystemPrim`, `hasOldData` | One formatted card: `isChanged` false, `status` 2, `createFileSystemPrim` 0 (no error), `hasOldData` false. Status codes seen in the script: 1 no card, 2 ready, 4 unformatted, 5 and 6 other card states |
| `Class.findClass`, `K2X_Script.loadClass` | Must return `nil` for an unknown class |
| `Object.sleep` | Yields the thread for *n* frames |
| `Thread.start` | Creates a new script thread |

Everything else in the engine can be a no-op that leaves `self` in slot 0 for now. The query primitives that scripts read back (`isDisc`, `isFade`, …) turned out to be *update* routines: `primitive 4` refreshes state and the script then reads an instance variable, so a no-op is a valid answer.

## Pending

- **Memory card data.** Reading and writing save data (`readSystemData`, `readShioriHead`, `readShioriBody`, `writeAllData`, `writeSystemData`) are not implemented, so a new game and loading cannot be tested yet. The save format is task F1-11.
- **Menu input.** The main menu is reached but not driven: the button masks for confirm, cancel and the directions still have to be identified.
- **Remaining primitives.** 174 in total; most are stubs. Their exact behaviour (frame timing, layers, fades) comes from the engine code.
- **Undo the shortcuts.** String concatenation converts any value; `Boolean` methods take values rather than blocks; random numbers use a fixed generator. Each is a guess to be confirmed against the engine.
- **Garbage collection and object limits.** The interpreter relies on Python's.
- **Speed.** Fine for exploration; the port's machine is C++.
