# The native interface

Confidence levels: **confirmed**, **hypothesis**, **pending**. See [disc.md](disc.md).

This is the boundary between the scripts and the engine: every routine written in C++ that script code can reach. It is the specification the Windows port has to implement.

## Size of the interface

| Mechanism | Routines | Where |
|---|---|---|
| `primitive n` instruction | 174, in 23 classes | Graphics, sound, input, memory card and a few engine services |
| Native methods registered by name | 215, set up by 12 functions | The language's built-in classes: `Object`, `String`, arrays, numbers, `Thread`, `System`, `Math` and others |

Everything else the game does is script code running on those 389 routines.

`python tools/natives.py .` prints the full catalogue with addresses. It needs the disassembly (`make setup`) and the extracted disc.

## Primitives (confirmed)

### Mechanism

1. At start-up the engine looks a class up by name (`func_0011A9E8`) and attaches a descriptor to it (`func_001232C8`).
2. A descriptor is two words: the number of routines and a pointer to a table of function pointers. All 23 descriptors are in `.sdata`.
3. The class object remembers which table it got in its `primitiveIndex` instance variable.
4. The `primitive n` instruction (`func_0011BE38`) takes the class of the running method, finds its table and calls entry `n`. An index beyond the table raises an error.

A scan of `.sdata` for descriptor-shaped data finds exactly the same 23 tables as following the registration calls, so none is missing.

### Classes

| Class | Routines | Area |
|---|---|---|
| `Sprite` | 19 | Generic sprite: texture, layer, fade, move, zoom, spin, shake |
| `BG_Picture` | 17 | Background |
| `ExtraPicture` | 15 | Extra full-screen picture |
| `EventPicture` | 4 | Event picture: mouth and lip sync |
| `ActorPicture` | 26 | Standing character: face, blush, sweat, eyes, mouth, lip sync |
| `StructPicture` | 2 | |
| `FontChar`, `FontChar2`, `FontCharEx` | 11, 2, 1 | Text characters |
| `Balloon`, `CutInWindow`, `LightBall`, `StandExt`, `SnowEffect` | 2, 2, 2, 1, 9 | Screen objects and effects |
| `NodeMapIcon`, `NodeMapNaviBase`, `ClockIcon` | 8, 3, 4 | Action map and clock |
| `Sound`, `Music`, `Voice` | 6, 6, 11 | Audio |
| `ControlPad` | 7 | Controller |
| `MemoryCard` | 11 | Saving |
| `K2X_Script` | 5 | Reset, file-read status, class loading, voice set-up, frame counter |

### Naming

Each primitive is called from a script method that is usually a thin wrapper, which gives the native routine its name: entry 10 of `Sprite`'s table is called only from `Sprite.move`, entry 5 of `Music`'s from `Music.isBusy`. `config/symbol_addrs.txt` names all 174 as `<Class>_prim<index>_<method>`.

Several picture classes share one routine for `isDisc`, `isZoom`, `isMove` and `isFade`; the script passes which one it wants.

## Native methods of the built-in classes (confirmed for the mechanism)

The built-in classes are created in C++ (`func_00123B20`) and their native methods added one by one with `func_00123C80(class, name, argument count, function)`. Twelve set-up functions do this, one per group of classes:

| Set-up function | Methods | Recognisable contents |
|---|---|---|
| `func_0011A368` | 17 | `Object`, `Class`, `Interface`: `getClass`, `clone`, `hashCode`, `isKindOf`, `perform…`, `sleep` |
| `func_0011ADD0` | 3 | `Symbol` |
| `func_0011D8B8` | 17 | `String`: `length`, `[]`, `atPut`, `+`, `=`, `append`, `toByteArray` |
| `func_0011E0A0`, `func_0011E878`, `func_0011F130` | 10, 12, 20 | Array-like classes: `new`, `[]`, `atPut`, `length`, `copyFrom` |
| `func_0011F850` | 8 | `Thread` |
| `func_00120F48` | 65 | Numbers: `Integer` and `Float` operators and conversions |
| `func_001217B8` | 32 | |
| `func_00122010` | 8 | |
| `func_001223E8` | 7 | `System`: `print`, `println`, `getLine`, `gc` |
| `func_001244A0` | 16 | |

Which class each set-up function belongs to is read off its method names and is a hypothesis where the table says so little.

## Pending

- **Five built-in classes use `primitive` without a descriptor table**: `Object`, `Class`, `Interface`, `Thread` and `System`. Their indices follow the order in which their native methods are registered, so the same routines are probably reachable both ways; how the table is built is not read yet.
- **Arguments and results** of each routine: how many values it takes from the stack and what it leaves. Needed before implementing them in the port.
- **Decompiling the routines themselves** is phase 3 work.

## Coverage in the port (2026-10-16)

`python tools/prim_coverage.py` counts the `primitive(N)` calls of the scripts per class and checks which ones `port/src/` implements; the rest answer with a stub that does nothing and returns `self`. The built-in classes `Class`, `Interface`, `Object`, `System` and `Thread` are registered as native methods and left out.

```
ActorPicture             26   23  12,14,25
BG_Picture               17   16  5
Balloon                   2    0  0,1
ClockIcon                 4    1  0,1,3
CutInWindow               2    0  0,1
EventPicture              4    2  2,3
LightBall                 2    0  0,1
NodeMapNaviBase           3    0  0,1,2
SnowEffect                9    6  3,6,7
Sprite                   19   18  0
StandExt                  1    0  0
StructPicture             2    0  0,1

149 of 174 primitives used by the scripts are implemented in the port
```

`ControlPad.4` and `.6` (vibration, auto-repeat reset) are no-ops on purpose and count as implemented. Most of the missing ones are initialisers or loaders (`load`, `initialize` of a class the port builds itself), which do nothing the scenes need. `python tools/sweep.py --report` ranks the stubs that scenes actually reach.

## The 25 primitives still stubbed: what each is for (2026-10-16, F4-21)

Read from the scripts that call them (`scripts-out/image/`); how many scenes reach each is from the first full sweep (isolated, 2,136 scenes). Certainty: the role of each class is **confirmed** by its constructor calls; what each looks like on screen is **pending** (needs PCSX2 or the ISO's pictures).

| Primitive | Script method | Who uses it | Scenes that reach it |
|---|---|---|---|
| `ActorPicture.25` | `setDrawTarget/1` | `Actor`: every character is drawn into a target chosen by `Actor.drawTarget` (also the cut-in actors) | 1,566 |
| `Balloon.1`, `Balloon.0` | constructor `(category, mark, layer)`, class `load/1` | **Done 2026-10-17** (approximation, see objects.md): the `?` bubbles of the conversation game (`WadaiTarget`) and the shadow under a topic icon (`WadaiIcon`, category 11) | 178 |
| `LightBall.1`, `LightBall.0` | constructor `(layer)`, class `load/1` | **Done 2026-10-17** (read from the code): `WadaiBall`, the flying light ball of a topic | 178 |
| `EventPicture.2` | `setMouth/3` (only if `charNum` matches) | **Done 2026-10-17**: the mouth of an event picture that has a sheet `stand/eX_NNv` (211 of them), see objects.md | 95 |
| `EventPicture.3` | `lipSync/2` | **Done 2026-10-17**, same | 15 |
| `SnowEffect.3`, `.6`, `.7` | `setAmount/2`, `turnCam/4`, `moveCam/5` | `Ending` (`setAmount(200, 1200)`), the `xd` endings (`n3skx_xd01D` and similar) | 37, 44, 44 |
| `StructPicture.0`, `.1` | constructor `(0, 6, 0, 1)`, `spin/4` | `Ending`: the pictures of the staff roll | 24 |
| `CutInWindow.0`, `.1` | constructor `(8, 1)` (three in `K2X_Script`), `setColor/6` | the cut-in windows of the 35 cut-in scenes | 2 |
| `StandExt.0` | constructor `(242, n, ...)` | `Breath`: the breath puff in the cold scenes | not reached in a scene run |
| `ClockIcon.0/.1/.3`, `NodeMapNaviBase.0/.1/.2` | class `load`, constructor, `setTime/2`, `setMark/2` | the map's clock and navigation marks (`NodeMap`, `NodeMapNavi`, `GameMain`, `HistoryIcon`) | the map is drawn by another path in the port (F2-11); these two classes are stubbed |
| `BG_Picture.5` | `setPic/3` | `Ending`, `Extra`, `Actor` | few |
| `Sprite.0` | class `load/2` | every class that loads its pictures | loading, answered by the port |
| `ActorPicture.12`, `.14` | `isBlushAct/0`, `isSweatAct/0` | no caller in the scripts (dead code) | 0 |

What follows for the order of work (hypothesis, to be confirmed with the pictures): `ActorPicture.25` is reached by almost every scene, and the port already draws characters, so it may be enough to record the target without drawing differently; `WadaiIcon` and `WadaiBall` (178 scenes) are the topic icons that PCSX2 shows over a character, so they are the biggest visible gap; the snow camera, `StructPicture` and `CutInWindow` matter to 24 to 44 scenes each; `ActorPicture.12/.14` and `Sprite.0` can be documented as no-ops. One example already seen: the dream scene in PCSX2 shows floating light balls and stars that the port does not draw.

### Status on 2026-10-17: 174 of 174

Implemented since the table above: `Balloon.0/.1`, `LightBall.0/.1`, `EventPicture.2/.3`, `Sprite.0`, `BG_Picture.5`, `StandExt.0` (without the depth sort), `ActorPicture.12/.14`, `StructPicture.0/.1` (as an extra picture that turns), `SnowEffect.3` (amount over time); `SnowEffect.6/.7` (the 3D snow's camera) and `ControlPad.4/.6` are no-ops on purpose, with the reason in the code. The nine left belong to systems that no event scene starts, so the scene sweeps cannot test them:

All 174 are now implemented or documented no-ops. The last ones, which no event scene reaches, were checked with probes (`tools/probe.py`):

| Primitives | System | How it is done |
|---|---|---|
| `CutInWindow.0/.1`, `ActorPicture.25` (`setDrawTarget`) | The mahjong's cut-ins (the heroines' reaction windows during a game) | An approximation compared with PCSX2 on the same table: see objects.md |
| `ClockIcon.0/.1/.3` | The history screen's clock (`HistoryIcon`): `initialize(time, layer)`, `setTime(time, frames)`; the class `setTime` turns every clock | `K2R_CObjClock`'s faces and hands as one object, the same tables as the map's clock (objects.md). Checked with a probe: two clocks drawn at the asked times. Not yet compared with the emulator |
| `NodeMapNaviBase.0/.1/.2` | The event graph's minimap on the action map, shown when the map grid option is on (`NodeMapNavi#restart`) | `navi/navi_0` with a 2x2 dot per node (`setMark`, `0x1A29E8`, read: position and palette indices in objects.md). Checked with a probe that forces it on: the dots appear and the map's cursor sits on the current node. The colours are the palette's (confirmed index table); not yet compared with the emulator |
