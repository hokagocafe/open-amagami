# Screen objects: the engine behind the graphics primitives

Confidence levels: **confirmed** (read from the code and cross-checked by a tool), **hypothesis**, **pending**. See [disc.md](disc.md).

This is what the script classes `Sprite`, `BG_Picture`, `ActorPicture`, `FontChar` and the rest drive. It is the part of the engine the Windows port has to reproduce exactly, because scripts time everything against it.

## From a script call to engine code (confirmed)

1. The script method runs `primitive n`, which calls a short native routine (see [natives.md](natives.md)).
2. That routine does not touch the object. It fills a **command** with the object's id (`vmUID`), a pointer to a virtual member function and the arguments, and hands it to the object manager (`func_00189B70` on the singleton returned by `func_00108AB0`).
3. The manager finds the object with that id and calls the member function on it.

So every graphics primitive is "call virtual method *k* of object *id*". Arguments travel in small structures; floats are read from the script's `Float` objects (value at `+8`) and integers are untagged.

`python tools/vtables.py . --objects` rebuilds the whole table below from the binary: it reads the virtual tables and their type information, and extracts the member-function pointer each primitive uses.

## Class hierarchy (confirmed)

All screen objects implement one interface of about 59 virtual methods. `K2R_CObjDef` supplies a default for each; subclasses override what they support.

```
K2R_IObject
└─ K2R_CObjBase                (manager bookkeeping, 9 methods)
   └─ K2R_CObjDef              (defaults for all 59)
      ├─ K2R_CObjSpr
      │  ├─ K2R_CObjSprTex              a sprite with a texture
      │  │  ├─ K2R_CObjSprTexN          plain
      │  │  ├─ K2R_CObjSprTexS          can scale
      │  │  │  ├─ K2R_CObjStandExt
      │  │  │  └─ K2R_CObjCutInWinBase
      │  │  ├─ K2R_CObjSprTexR          can scale and rotate
      │  │  ├─ K2R_CObjMapNavi, K2R_CObjLightBall, K2R_CObjSnowFlake, K2R_CObjBalParts
      │  ├─ K2R_CObjSprSldS, K2R_CObjSprFont
      │  └─ K2R_CObjBackPic
      │     └─ K2R_CObjEventPic ─ K2R_CObjEventPicEx
      ├─ K2R_CObjStandPic
      │  ├─ K2R_CObjActorPic
      │  └─ K2R_CObjExtraPic
      ├─ K2R_CObjStructPic, K2R_CObjCutInWindow, K2R_CObjBalloon
      └─ K2R_CObjClock, K2R_CObjNodeIcon, K2R_CObjSnow
```

The executable has 99 classes with a virtual table in total (`python tools/vtables.py .`). `config/symbol_addrs.txt` names every virtual method as `<Class>__<method>`, 864 functions.

## Virtual methods reached by primitives (confirmed)

| Slot | Method | Script primitives |
|---|---|---|
| 9 | step | (called every frame with the elapsed time, not from scripts) |
| 10 | draw | (not from scripts) |
| 11 | `destruct` | `destruct` |
| 14 | `setLayer` | `setLayer` |
| 15 | `setTex` | `setTex` |
| 16, 17 | `setPic` | `setPic`, `NodeMapIcon.set` |
| 18 | `setTexRect` | `setTexRect` |
| 21 | `fade` | `fade`, `vibAlpha` |
| 22 | `fadeLight` | `fadeLight`, `vibLight` |
| 23 | `move` | `move`, `vibPos`, `moveCam` |
| 24 | `zoom` | `zoom`, `vibScale` |
| 25 | `spin` | `spin`, `ClockIcon.setTime` |
| 26 | `turnCam` | `SnowEffect.turnCam` |
| 27 | `setColor` | `CutInWindow.setColor` |
| 34 | `skip` | `skip` |
| 35 | `pause` | `pause` |
| 36 | `update` | `isFade`, `isDisc`, `isMove`, `isZoom`: copies the remaining times back to the script object |
| 37 | `setDrawTarget` | `ActorPicture.setDrawTarget` |
| 39 to 48 | `setBlush`, `isBlushAct`, `setSweat`, `isSweatAct`, `setFace`, `setEye`, `setMouth`, `getMouthOfs`, `lipSync`, `getDist` | The `ActorPicture` methods of the same names |
| 49 | `initialize` | `StandExt.initialize` |
| 51 | `setCode` | `FontChar.setCode` |
| 52 to 56 | `open`, `close`, `show`, `hide`, `action` | `NodeMapIcon` |
| 57 | `setMark` | `NodeMapNaviBase.setMark` |
| 58 | `setAmount` | `SnowEffect.setAmount` |

`vibAlpha`, `vibPos` and the other `vib…` primitives use the same slots as `fade`, `move` and so on: the first field of the argument block says which component of the animation they set (see below).

## Animations (confirmed)

Every animated property is a small structure, here called a *tween*:

| Offset | Field |
|---|---|
| `0x00` | Time remaining, in frames (float) |
| `0x04` | Target value |
| `0x08` | Coefficient `a` |
| `0x0C` | Coefficient `b` |
| `0x10` | Current value |

Two- and three-component tweens (position, scale, colour) keep one remaining time and then target, `a`, `b` and current for each component.

### Starting one

`start(value, mode, time)` (`func_00194D78` for one component):

```
target = value
if time <= 0:
    current = value; remaining = 0
else:
    remaining = time
    d = value - current
    mode 0:  a = 0;         b = d / time          linear
    mode 1:  a = d / time²; b = 0                 fast first, slowing down
    mode 2:  a = -d / time²; b = 2·d / time       slow first, speeding up
```

### Advancing one

Each frame, with `dt` the elapsed time in frames:

```
if remaining > 0:
    remaining = max(remaining - dt, 0)
    current = target - (a · remaining² + b · remaining)
```

So the value is a function of the time *remaining*, not an accumulation, and it lands exactly on the target.

### Vibration

A property is a group of three tweens plus a phase per component:

| Tween | Set by | Meaning |
|---|---|---|
| base | `fade`, `move`, `zoom`, `fadeLight` (component 0) | The value itself |
| amplitude | `vib…` (component 1) | Size of the oscillation |
| frequency | `vib…` (component 2) | Cycles per second, at 60 frames per second |

Each frame the phase grows by `dt`, wrapping when `phase · frequency ≥ 60`, and:

```
wave = sin(phase · frequency · π / 30) · amplitude

alpha, position, colour:  value = base + wave
scale:                    value = base · (1 + wave)    if wave ≥ 0
                          value = base / (1 − wave)    if wave < 0
```

The alpha group is at `+0x70` of a sprite, colour at `+0xB0`, position at `+0x150` and scale at `+0x1C4`. The step routines are `func_001952D0`, `func_00195478`, `func_00195828` and `func_00195AC8`.

### What the script sees

`Sprite.isFade()` and its siblings run the `update` method, which copies the four remaining times (alpha, colour, position, scale) into the script object's `fadeRemain`, `discRemain`, `moveRemain` and `zoomRemain`, and then test them against zero. That is how `wait()` loops end.

## Script-side argument order (confirmed)

| Script call | Meaning |
|---|---|
| `fade(alpha, mode, time, component)` | |
| `move(x, y, mode, time, component)` | |
| `zoom(sx, sy, mode, time, component)` | |
| `fadeLight(r, g, b, mode, time, component)` | |
| `setAlpha(a)`, `setPos(x, y)`, `setScale(sx, sy)`, `setLight(r, g, b)` | The same with time 0: immediate |

## Texture table (confirmed)

`Sprite.new(n, …)` takes a texture number. The engine has a table of 320 records at `0x1AFD24` in `.data`, 36 bytes each; texture number *n* is record *n − 1*.

| Offset | Type | Field |
|---|---|---|
| `0x00` | pointer | Name, a path inside `GRAPH0.ARC` without `.tm2`; may contain `%1d` |
| `0x04` | `u32` | 1 when the name has a `%1d` variant, else 0 |
| `0x08` | `f32`, `f32` | Left and top of the rectangle inside the image |
| `0x10` | `f32`, `f32` | Width and height |
| `0x18` | `u32` | Flags (`0x100` in every record seen) |
| `0x1C` | `u32` | Number of the shared palette in `GRAPH0.CLT` (hypothesis; 0 for the title sprites, which render correctly with it) |
| `0x20` | `i32` | −1, or the record whose image this one is a rectangle of |

The sizes agree with the TIM2 files: record 76, `idou/calendar_base`, is 112×88, as is the image.

On the title screen the script uses textures 73 to 76: `sysgraph/rogo1heart`, `sysgraph/rogo1snow`, `sysgraph/txt_start` and `sysgraph/copy`.

## Rotation (confirmed)

Rotation has its own group (`func_001951F0` to start, `func_00195E00` to step): a base angle, a spin speed and the angle turned so far.

- Angles are in units of π radians: 2.0 is a full turn.
- `spin(angle, mode, time, 0)` moves the base angle like any tween and stops any continuous spin.
- `spin(speed, mode, time, 1)` sets a continuous spin, in turns per second: each frame the turned angle grows by `dt · speed / 30` and wraps at ±2.0.
- The angle used for drawing is `base + turned`.

## Drawing (confirmed for the geometry; hypothesis for layer order)

Each screen-object class draws through virtual slot 12 (confirmed by the font: `K2R_CObjSprFont`'s slot 12 is the draw at `0x19AAF0`). The classes that override it: `K2R_CObjSpr` `0x18A4E0`, `K2R_CObjSprTex` `0x18ACE8` (and its N/R/S variants), `K2R_CObjExtraPic` `0x18D210`, `K2R_CObjStructPic` `0x1980D0`, `K2R_CObjActorPic` `0x198A88`, `K2R_CObjEventPicEx` `0x19A668`, `K2R_CObjSprFont` `0x19AAF0`, `K2R_CObjBalloon` `0x19B8E8`, `K2R_CObjLightBall` `0x19C320`, `K2R_CObjClock` `0x19CB58`, `K2R_CObjNodeIcon` `0x19E2B8`, `K2R_CObjMapNavi` `0x1A26A8`, `K2R_CObjSnowFlake` `0x1A5C80` (slot 9 is the per-frame step). The picture classes draw each sprite component with a third argument 0 or 1; 1 sets the context bit of the chip's primitive (`0x200`, the second drawing context, the work buffer where a character is assembled). These methods are being read one by one against the port (PLAN F4-24): an extra draw like the font's edge copy is what they are checked for.


The sprite components' `draw` methods (virtual slot 29 of the `K2R_CSpr…` classes, code at `0x105E30` to `0x108AB0`) build one rectangle for the graphics chip:

- **Coordinates are relative to the centre of the screen**, y downwards; the screen is 640×448. The engine multiplies by 16 for the chip's fixed point and by 8 instead of 16 vertically in interlaced field mode.
- **A sprite is centred on its position.** Its size is the texture rectangle times the scale.
- **Rotation** is about the position (plus an optional pivot offset): for a corner at `(lx, ly)` from the centre, `X = cos·lx + sin·ly`, `Y = cos·ly − sin·lx`. A positive angle therefore turns the picture counter-clockwise on screen.
- **Colour** ("light") multiplies the texture: 1.0 is neutral (`0x80` to the chip); for an untextured rectangle it is the colour itself, scaled by 255.
- **Alpha** 1.0 is opaque (`0x80` to the chip).
- **Layers** are drawn from the lowest number to the highest. This is inferred from how scripts use them (screen fades are on layers 17 and 18) and holds in every scene compared with the emulator so far (background, characters, text window, fades). What the code shows: a sprite component's draw method asks the display-list manager (`func_00105A28`, virtual slot at `+0x14`) for a packet in the bucket numbered by the object's layer, so the layer is a bucket of an ordering table. Inside one layer the **last object created is drawn first** (so the first created ends up on top). The code that does this has not been read; it is established by comparison with the emulator: the action map's bar creates its text and buttons before its background, and in the game the text is in front. With first-created-first the model drew the background over the text and dimmed it. Hypothesis for the mechanism: packets are prepended to the bucket's list.
- **Characters are depth-sorted.** `func_0010A800` collects up to 200 standing pictures with a message carrying a pointer to an array of `SSortZ` records (a float and an identifier), bubble-sorts them by the float, descending, and then sends each a layer change to `i + 2`. The float is the object's `getDist` value (field `0x88` of `K2R_CObjStandPic`). So when several characters are on the stage, layers 2 and up are assigned from their distance, and what the script sets with `setLayer(2)` is only the starting value. Whether a larger distance means nearer or farther is not settled; the model does not sort yet.

`Sprite.new(0, layer)` creates an untextured rectangle; `setTexRect(0, 0, 640, 448)` gives it its size. That is how the engine does full-screen colour fades.

`BG_Picture.new(place, scene, mob, layer)` shows the picture `bg/<place:03>_<scene><mob>`.

## Text (confirmed for the font; hypothesis for the palettes)

Text is drawn one character per object. `FontChar.new(code, palette, layer, x, y, scaleX, scaleY)` creates a character and `setCode(code)` changes it; it animates like any sprite.

- **Font files.** `font/K2_font28_24.tm2` and `font/K2_font28_24im.tm2` in `GRAPH0.ARC`: 672×4384 pixels, 4 bits per pixel, a 64-entry palette.
- **One glyph per image row.** A glyph is 24 pixels wide and 28 high; its 672 pixels are stored as one row of the image, so glyph *n* is row *n*. The engine copies glyphs from there into a cache texture as they are needed (`K2R_CFont`, `K2R_CTexFontCache`).
- **Glyph number from the character code** (`func_00186E18`), for a Shift-JIS code with lead byte `hi` and trail byte `lo`:

  ```
  n = 0
  if 0x81 <= hi < 0xA0:  n = (hi - 0x81) * 188
  if hi >= 0xE0:         n = (hi - 0xE0) * 188 + 31 * 188
  if 0x40 <= lo < 0x7F:  n += lo - 0x40
  if 0x80 <= lo < 0xFE:  n += lo - 0x41
  ```

- **Palette.** The second argument selects colours. The model takes it modulo 4 as one of four 16-colour palettes of the font file; scripts also pass 4, so this is not the whole story.
- `FontChar2` has its own native table; the model assumes it uses the second font file.

A consequence for the virtual machine: **a string is a sequence of 16-bit characters**, not bytes. The native `String.[]` reads a 16-bit array and `length` counts characters, so a two-byte Shift-JIS character is one element. The name entry screen passes `name[i]` straight to `FontChar.new`.

## Texture names and packing (confirmed)

- A texture name may contain `%1d` or `%03d`; the second argument of `Sprite.new` fills it in (`menu_main%1d`, `txt_%03d`).
- The left and top of the table's rectangle are where the engine packs the texture in video memory, not an offset inside the file. The last field is 0 for packed textures and −1 otherwise. The file is always used whole.

## Background pictures: scale and rotation order (hypothesis)

Scripts rotate backgrounds a quarter turn and scale them unevenly (`spin(0.5)`, `setScale(1.5, 1.3)`, `setPos(0, 192)`). Those numbers cover the screen exactly, with the picture's edge on the screen's edge, only if the picture is turned first and then scaled along the screen's axes; scaling before turning leaves black bars. The model does that for `BG_Picture`. `K2R_CObjBackPic`'s own drawing code has not been read yet. Plain sprites scale first and then turn, as their draw routine shows.

## Standing pictures: characters and whole pictures (confirmed)

`ActorPicture` (a character with a face) and `ExtraPicture` (one whole picture) are `K2R_CObjActorPic` and `K2R_CObjExtraPic`, both built on `K2R_CObjStandPic`. The placement below was read from the code and checked against the game running in PCSX2 (see the end of this page).

### Files

- **Character**: `ActorPicture.new(character, cloth, pose, layer)`. A table at `0x28ADF8` maps the character number to a row (characters 15 to 26 and 37 share row 35), and a table at `0x1B2A28` has, for each row and each of 26 poses, two names: a body and a face. The body file is `stand/<body><cloth letter>` (`a` for cloth 0), the face file `stand/<face>`. Several poses of a character share one face file.
- **Whole picture**: `ExtraPicture.new(size, picture, variant, layer)` loads `stand/x<l or s>_<letter><letter>`: `l` when `size` is 0, then `A` + picture and `a` + variant.

Each is a sheet (`…x.tm2`) plus a layout (`…z.zb`), see [images.md](images.md).

### The canvas

The parts are first put together on a canvas of 512×768 and the canvas is then drawn as one sprite, so that fading a character does not show its parts through each other. (The engine draws the parts into a work buffer through the graphics chip's second drawing context, and the sprite samples that buffer.)

- A layout record lands on the canvas at its destination plus an **offset** that the script passes in. For characters it comes from a table in the `ActorPicture` script: a base of (−164, −180), the same for everyone, plus a per-pose correction that is zero except for one pose of character 8. For whole pictures it is zero.
- The body contributes all its records of group 0.
- The face contributes exactly three records, one each from groups 1, 2 and 3: the two eyes and the mouth.

### Faces

A face file holds 9 faces × 3 frames in each group: frame 0 open, 1 half, 2 closed. The record used is number `3 × face + frame` within its group.

- `setFace(eyes, mouth)` picks the face for the eyes and for the mouth separately; 0 leaves that one as it is, *n* selects face *n* − 1.
- Eyes and mouth each have a **phase** 0 to 3 that maps to frames 0, 1, 2, 1 (open, half, closed, half), so that stepping the phase forward plays close-then-open. A new picture starts with eyes at phase 0 and mouth at phase 2.
- `setEye(mode, time)` and `setMouth(mode, time)` queue a number of steps: mode 0 opens, 1 goes to half, 2 closes, 3 or more blinks (eyes: a full cycle from open; mouth: always four steps). With a time of 0 the steps happen at once; otherwise one step is taken every `time` frames. The eyes only move while the picture is fully opaque.
- The script's `autoEyeBlink` runs a thread that calls `setEye(3, 1.0)` every 150 to 330 frames.
- With `lipSync` on (the default) and a voice playing for that character, the mouth frame comes from the sound level instead: frame `2 − level`.
- **Blush and sweat** are two more sprites cut from the body's sheet: the body's one record of group 1 (a patch over the cheeks, about 100×35) and its one record of group 3 (a small drop, 6×16). Their opacity starts at 0.
  - `setBlush(target, time)` eases the blush's opacity to `target`; with a time of 0 it jumps. The easing is quadratic: with `left` frames remaining, `opacity = target − accel × left²` and `accel = (target − start) / time²`.
  - `setSweat(on, time)` does the same towards 1 or 0. Turning it on also makes the drop fall: it starts 32 pixels higher and settles at its place with the same easing. Turning it off only fades it.
  - The blush is drawn between the body and the face, the drop over the face. Which record belongs to which sprite follows from the order of the engine's setup (hypothesis for the picture's draw order; the group numbers are confirmed). Not yet compared with a frame of the emulator.

### Placement

The canvas is centred on the object's position and scaled about a fixed point of the screen, (0, −30) from the centre: the engine sets the sprite's pivot offset to `(0, −30) − position` every frame. On top of that, `move` adds a constant to every position it is given: **(0, 92) for characters, (0, 128) for whole pictures**. So a canvas point *p*, measured from the canvas centre, appears at

```
screen = (0, −30) + scale × (p + position + shift − (0, −30))
```

`getMouthOfs` uses the same formula with the centre of the first mouth record as *p*; scripts use it to place the breath effect.

`setDrawTarget` attaches a character to another object, which then supplies position, scale and opacity, and clips the canvas to what that object shows. The code has been read but is not modelled; the scripts seen so far pass no target.

## Snow (read on the Vita 2026-10-26)

`SnowEffect` is `K2R_CObjSnow` with one `K2R_CObjSnowFlake` per flake. The script side is the presets of `SnowEffect.scfs` (`initialize(mode, count, d1, d2, d3, d4, size)` plus `setCamDir` and `setCamPos`; `setAmount(count, time)`; `fade(alpha, mode, time)`; `isFade`, `isMove` and `isTrun` poll the three tweens). The engine side (flakes in a 3D box in front of a camera, the projection, the fades and the death at the ground) is written in [vita-executable.md](vita-executable.md), "Snow". The port follows it; the flake's picture is a guess. The PS2's functions (`0x1A3430`, `0x1A5D48`) were never read.

## Copies and weekday names (confirmed)

- **`initialize(uid)` makes a copy.** The script method `clone()` of `Sprite`, `BG_Picture`, `ExtraPicture`, `ActorPicture` and `ClockIcon` is `Class.new(vmUID)`, and that primitive builds a new object that starts as a copy of the old one (the copy constructors are at `0x19A320` and similar) and then lives its own life. Scripts depend on it: the action map takes `getPic()` of the stage, which is a clone, and sets the clone's alpha to 0 while the original stays visible. Treating it as a second handle to the same object made the sky disappear.
- **Texture 80, `idou/calendar_%s`, is the only texture whose variant is a name.** The variant is a weekday number and the engine looks the name up in a table of seven 8-byte strings at `0x29A348`: `sun`, `mon`, `tue`, `wed`, `thu`, `fri`, `sat`.
- **`EventPicture`** is a `BG_Picture` that loads `epi/e<heroine in hex>_<number:02><letter>0` (`func_0010A500` formats it) and has two extra primitives for the mouth. The stage's picture on the map is not one; it is the name-entry background, kept.

## Node icons on the action map (confirmed for the composition; hypothesis where marked)

`NodeMapView` keeps a 9 x 6 array of `NodeMapIcon`s and lays them on a staggered grid: column pitch 42, row pitch 44, odd columns half a row lower, origin (-252, -34) from the screen centre. The script passes coordinates times 10,000; the primitives divide (`show`, `0x10FD90`). `NodeMapIcon.set(code)` takes a 16-bit code from `EventNode.getIcon` plus `0x8000` when the map grid option is on.

The engine object (`K2R_CObjNodeIcon`) owns five child sprites, **all centred on the cell**: hexagon base, glow, symbol, marker and an extra. `setPic` (`0x19EA30`, read in full) picks their textures:

| Bits | Meaning |
|---|---|
| 15 | Grid option: hexagon `node_hex<kind>1` instead of `node_hex<kind>0`; for kind 0, the grid tile `node_hex01` |
| 12 to 14 | Kind. 0: a plain picture, `node_flag%02d` with the low 12 bits as its number (0: none) |
| 8 to 11 | Symbol type (0: nothing). Row of the symbol table: type 2 → 0, types 1 and 3 → sub + 1, 4 to 12 → the type, 13 and up → 8 (`node_ep0` + sub + type - 13) |
| 7 | Hexagon base on. The symbol table is `0x28AB98` with it and `0x28A9F8` without |
| 5 to 6 | Sub: with the hexagon and state < 6 and marker ≠ 1, sub 2 adds the glow `node_hexg`, sub 1 `node_hexh` |
| 2 to 4 | State: the column of the symbol table (13 rows × 8 ints). Rows 7, 9 and 11 (`node_de…`, `node_g…`) add kind - 1 from state 2 on |
| 0 to 1 | Marker: 1 `node_break`, 2 `node_new0`, 3 `node_new1` |

**Clock (confirmed).** Kind 0 with picture 0 (code `0x0000`) is not empty: `setPic` builds a `K2R_CObjClock` child (constructor `0x1908C8`, the same one `ClockIcon.new` uses). `ClockIcon.setTime(time, frames)` (class primitive 2, `0x110158`) sends a turn to every clock (it addresses the object type `0x20`, not one object) with the value of the table at `0x28B398` for that time of day: half hours, negative (`dayProc`'s times: `休み1` index 5 is -19, 9:30; `昼休み` index 8 is -25, 12:30). `K2R_CObjClock::spin` (`0x19D440`, read): long hand at the value, short hand at value / 12; hour = -value / 2. Two faces: the back one takes its variant from a 24-entry table at `0x28A938`, the front one from `0x28A998` (0 blue, 1 red, 2 grey), and the front face's opacity interpolates the table at `0x28A8D8` (1 at 5 h and 16 h, fading over an hour: dawn and dusk).

An apparent difference against PCSX2 (short hand) was two different times of day, 10:30 on the port and 12:30 on the emulator capture.

`show(x, y, scale)` places every child at once; NEW marks sit 14 pixels lower (`0x19FD84`). `open(x, y)` brings them in over 9 frames (hypothesis for the curve: zoom 0.7 to 1 and fade in). Not modelled yet: the glow's exact alpha (0.5 used), the pulsing zoom of symbols in states 4 and 5 (constants 0.7 and 0.3 in `show`), and `close`'s animation (a fade). `action` is empty in the engine.

Checked against PCSX2 on the prologue's map (code `0x7516`: snowflake `node_pr0` with NEW below it): same icon at the same place.

## Check against the real game

`tools/hokago.py scfvm_gfx` implements this model on top of the reference interpreter, with a software renderer:

```sh
python tools/hokago.py scfvm run --steps 36000000 --press 2600:2048 --press 2900:2048 --press 3400:32 --press 3800:2048 --press 4000:32 --press 4200:32 --press 4500:32 --shot 2800:build/shots/title.png --shot 5400:build/shots/dialogue.png
```

Each `--shot` writes the frame as a PNG and a text file listing every object on screen, back to front.

What the model draws, driving the real scripts with scripted button presses:

| Frame | Screen |
|---|---|
| 30 | The opening notice |
| 2800 | The title screen: sky picture turned and scaled, logo, spinning snowflake, blinking start prompt, copyright |
| 3300 | The main menu, with its four entries, cursor and button hints |
| 3700 | Name entry: the kana grid drawn with the game's font |
| 4400 | The action map of day 1 (frame, calendar, prompts and node icons) |
| 5400 | The first line of the prologue in the text window |

Compared with PCSX2:

- The notice screen and the text window match, down to the line breaks of the dialogue.
- The first character of the prologue, at a known position and scale, lands within a pixel or two of where the emulator draws her. The whole picture of the title's opening agrees to about ten pixels, which is the error of matching a moving picture by eye.
- The action map matches in layout, colours, weekday and bar. At the time (2026-10-13) the node icons were not drawn and the location picture, its name and the two counters at the bottom left differed; all of it was fixed on 2026-10-15 (F4-9, F4-11, F4-16).
- Not drawn the same way yet: the snow (approximated), the colour of the speaker's name.

`tools/hokago.py compare_png` puts a model screenshot beside an emulator capture (or their difference) at the same scale.

`tools/hokago-tools/emulators/pcsx2_drive.ps1` launches PCSX2 with the disc, presses keys on a timeline (Start, confirm, and an occasional Down for choices) and saves its window every few seconds. It reaches the first character scene about six minutes after launch; a save state made there with the emulator's own key (F1) skips the wait, and `-LoadStateAt <seconds>` presses F3 to load slot 1 (the scene is up two seconds later). The keys are posted straight to the emulator window (`WM_KEYDOWN`/`WM_KEYUP`), so it needs no focus; set `$script:Background = $false` to fall back to global key events. The key codes follow one user's bindings and need editing for another set-up.

Screenshots are game material: keep them under `build/`, which git ignores.

## Pending

- Compare more scenes with PCSX2; so far the comparison is by eye and by measuring a few points.
- Layer order and blending modes, read from the code.
- The third field of `Sprite.new`, the texture-table flags and the pivot offset.
- Comparing blush and sweat against the emulator. (Lip sync from the voice level is done: F4-8c. `setDrawTarget` is `ActorPicture.25`, still a stub: see `natives.md`.)
- `EventPicture`, `StructPicture` and `StandExt`.
- Character spacing; cut-in windows; the real snow particle system. (The composition of node icons is done: F4-11.) The topic icons `Balloon` and `LightBall` (the dream scene shows floating light balls and stars that the port does not draw) are listed in `natives.md`.

## The conversation game's topic objects (2026-10-17)

Read from the executable; checked against PCSX2 on `n6sir_ec03E` (the bubbles match in shape, rim colours and `?`).

- **`K2R_CObjLightBall`** (`WadaiBall`, constructor `0x190168`, step `0x19C550`, confirmed): three sprites of `kaiwa/tama0..2` (texture record 209, the shared interface palette of the record), drawn by adding light. The core stays white; the middle glow is tinted `(light + 1) / 2` and moves a sixth of the way to the ball's position each frame (`(5p + previous) / 6`); the outer glow is tinted by the light and follows the middle one the same way, so a moving ball leaves a short trail.
- **`K2R_CObjBalloon`** (`WadaiTarget`, `WadaiIcon`; constructor `0x18F5F8`, parts `K2R_CObjBalParts` `0x19B0D8` and `0x1A59B8`): six parts on a hexagon at `(+-13.5, -31.5)`, `(+-21, 0)`, `(+-13.5, 31.5)`, each three sprites of `maru/maru128` (record 240): one in the category's colour at the full offset, one black at `(1.04/1.5, 1.18/1.33)` of it and one white at `(1/1.5, 1/1.33)`; colours from a table of twelve at `0x28A850` (10 grey for a hidden topic, 11 black for a shadow); `maru/hatena` (record 241) is the `?`. Each part gets random phases (`rand() % 50`, `% 20`, `% 60`) for a wobble. **The port draws an approximation**: the coloured and white layers, the `?`, no black layer and no wobble, with the discs' size fitted to the emulator's picture. **On the Vita data (2026-10-23, hypothesis)** the bubble is not six discs: interface texture 215 is one irregular blob of 32x60 (variant 0, plain white) and the same blob with the `?` in it (variant 2, found by dumping the interface pictures with `AMG_DUMP_UI=FOLDER`). The port draws the blob with the object's own light (the script colours each bubble) at 0.93 times, and the white one, unlit (with the `?` while the topic is hidden), at 0.87 times on top. **Checked against Vita3K on 2026-10-24 (F4-53, `n6sir_ec03E`, the emulator's own screenshot):** a bubble is about 28x53 pixels with a rim about 1 to 2 pixels, the spacing and the colours agree; the engine's own sizes are not read, and there is no wobble. The earlier drawing, the PS2 construction with the Vita's picture, gave blobs with spiky parts away from the body. **Read 2026-10-25 (F4-59, confirmed from the code):** `Balloon`'s constructor primitive calls `0x8100B20E`, which builds one `K2R_CObjBalParts` (vtable `0x810C1818`) with two sprites of interface texture 215 at the object's own transform and the same size: variant 0, lit by the object's light (the category colour), and variant 1, or variant 2 (the `?`) when the bubble has a mark, unlit. Its draw (`0x8108FE64`) draws both, its step (`0x8108FF00`) is the plain tween and vibration step. So the rim is in the pictures (variant 1 is the white body inside the variant 0 outline), not a scale: the port's fitted 0.93 and 0.87 are gone and it draws variants 0 and 1 at scale 1. The wobble is the script's: `WadaiTarget` gives each bubble `vibPos(0, 1.5, 0, 0, (250 + rnd(100)) / 1000, 0, 0)`, a vertical swing of 1.5 units at a random frequency, which the port already runs. Checked against Vita3K on `n1ako_ec04A` (the four `?` bubbles): the same picture, 0.7 of 255 in the bubbles' box on a matching frame.

## Event pictures with a moving mouth (2026-10-17, confirmed)

The factory at `0x109820` formats `stand/e%1x_%02d%c` (heroine, number, variant letter) and, when that name plus `x.tm2` exists, builds a `K2R_CObjEventPicEx` (constructor `0x18EE68`) instead of a plain event picture. 211 pictures have one: a sheet `stand/eX_NNvx.tm2` and a layout `stand/eX_NNvz.zb` of three records of group 3, the open, half-open and closed mouth, each a patch at its place on the 640x448 picture (checked offline on `e3_06a`: frame 0 is identical to the picture's own mouth, frames 1 and 2 are the half-open and closed one at the same place). `setMouth` (`0x19A790`) and lip sync step the mouth as on a character: phases 0 to 3 shown as frames 0, 1, 2, 1 (table at `0x29A318`), starting at phase 2 (closed); while the speaker's voice plays the level picks frame `2 - level`. The port builds `EventPicExObject` the same way.

## Staff roll memories and snow amount (2026-10-17)

`K2R_CObjStructPic::setPic2` (`0x198208`) formats `stand/x%c_%c%c` like an extra picture, and the primitive's arguments are an extra picture's: the port builds an extra picture and turns it with Sprite's spin; the engine draws it through its own sprite string (`K2R_CSprStr`), not modelled. `SnowEffect#setAmount(count, time)` changes the number of flakes linearly over `time` frames. `turnCam` and `moveCam` belong to the engine's 3D snow (`K2R_CObjSnow`), which the port replaces with a flat approximation, so they do nothing.

## Mahjong cut-ins (2026-10-17)

The mahjong table (`n0fre___18A`, a developers' node that opens it) now plays a whole game headless (see port.md for the `and`/`or` fix that let it), so the cut-ins run: a heroine's reaction in a square window at `(180, -80)`, `(0, -100)` or `(-180, -80)` (`n0_MacroScripts#setCIW`). `K2R_CObjCutInWindow` (factory `0x1090C0`, constructor `0x18C2E8`) has three children: the frame `mahjong/cut_frame` (record 320, 224x224) and two layers of the pattern `maru/cut_tex%1d` (record 319), chosen with `setTex(319, n)` and coloured by `setColor` (two colours); `ActorPicture#setDrawTarget(uid)` makes a character draw into the window instead of the screen (event scenes call it with 0). **The port approximates it** (compared with PCSX2 on the same table): the pattern's grey mixes the two colours (the tone is close, not exact), the character is drawn as on the screen, enlarged 1.25 times about the screen point (24, -17) placed at the window's centre, and cut to the inside of the frame (renderer: `DrawItem::clip`). The engine renders the character into its own target; that is not modelled.

## The text's dark edge and the breath puff (2026-10-17, reported by the owner against PCSX2)

- **Text edge (confirmed by reading the code).** The glyphs of `font/K2_font28_24` are bare white masks with soft edges; the font's fourth palette is dark grey (63, 63, 63). `K2R_CObjSprFont`'s draw (`0x19AAF0`) draws a character of palette 0 to 2 a second time, 2.0 to the right and down, in palette 3 (a character of palette 3 gets the copy in palette 0); the copy lies under the glyph and gives the text its dark edge. The port did not draw it; it does now (`CharObject`).
- **Breath puff.** `Breath`'s puffs (`StandExt`, `sysgraph/breath%1d`) go in front of the character they come from, by a depth of +0.001; the port has no depth sort among characters, and created after the character the puff was drawn behind it. It is now drawn on top of its layer (approximation: a character further in front in the same layer would be covered too).

## History clock and event-graph minimap (2026-10-17)

- **`ClockIcon` (confirmed from the code, not compared with the emulator).** A script object of its own built by the same constructor as the map's clock (`0x1908C8`): back face, front face, long hand and short hand of `clock%1d`/`hand` (records 175 to 177) on one centre. `initialize(time, layer)` and `setTime(time, frames)` use the map clock's tables (`0x28B398` value per time of day, faces `0x28A938`/`0x28A998`, front opacity `0x28A8D8`); the class `setTime` turns every clock, these included.
- **`NodeMapNaviBase` (`K2R_CObjMapNavi`, confirmed from the code for the marks).** The picture `navi/navi_0` (record 201, 136x144, palette 4). `setMark(node, state)` (`0x1A29E8`) writes a 2x2 block of palette indices into the picture: node n is column n % 65, row n / 65, at x = 2 * column + 3, y = 5 + 2 * row, one pixel higher in odd columns; the index is 247 for a negative state, otherwise `{252, 253, 251, 254, 249, 248, 250, 255}[state]` (`0x28AE58`). The draw method (`0x1A26A8`) copies the picture between four textures in turn (double buffering); the port keeps a work copy and publishes it once per frame. Shown only with the map grid option.

