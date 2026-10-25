# Input and button glyphs (F5-9, D-025)

Design of the controller, keyboard and mouse layer and of the button pictures the game shows. **Built 2026-10-23 except
rumble and the community mapping file** (see "Status"); what is confirmed is marked.

## What SDL3 gives us (confirmed from its headers and release 3.4.18, zlib licence)

- **A gamepad API with a built-in database of controllers.** Every pad is presented with the same layout, by *position*
  (`SOUTH`, `EAST`, `WEST`, `NORTH`, shoulders, triggers, sticks, d-pad, back, start), whatever the pad reports itself.
- **The kind of pad:** `SDL_GetGamepadType` (Xbox 360, Xbox One, PS3, PS4, PS5, Switch Pro, Joy-Con, standard, unknown) and
  `SDL_GetRealGamepadType` (the same, seen through Steam Input or an emulator such as DS4Windows).
- **The label of each button:** `SDL_GetGamepadButtonLabel` answers A, B, X, Y or Cross, Circle, Square, Triangle for the pad
  in hand, so the picture shown is the label that is printed on that pad.
- **Hot plug** (`ADDED` and `REMOVED` events), several pads, rumble, the PS4 and PS5 lightbar, and a file of extra
  mappings (`SDL_AddGamepadMappingsFromFile`, the community's `gamecontrollerdb.txt`) for pads it does not know.
- **A virtual joystick** (`SDL_AttachVirtualJoystick`): a pad that does not exist, driven by the program. This is how the
  glyph switching can be tested without a pad.

Only the gamepad part is used. The window, the picture and the sound stay as they are (Win32, Direct3D 11, WinMM).

## Which glyphs a pad gets

| What SDL says the pad is | Glyph family |
|---|---|
| Xbox 360, Xbox One (also a pad that Steam Input or DS4Windows shows as one) | Xbox |
| PS3, PS4, PS5 | PlayStation |
| anything else: Switch, standard, an unknown pad | the **fallback family**: PlayStation by default, selectable (below) |
| the keyboard was the last thing used | keyboard |

The family follows the **last device that was used**: any press on a pad sets its family, any key sets the keyboard. A
glyph already on screen changes at once (see below).

## What each game button is, physically

The game's buttons are the Vita's. The logical action stays the same; where it sits depends on the pad.

| Game button | PlayStation and fallback | Xbox | Keyboard (default, can be changed) |
|---|---|---|---|
| Circle (confirm) | East (Circle) | South (A), or East (B) by setting | Z or Enter |
| Cross (cancel) | South (Cross) | East (B), or South (A) | X or Backspace |
| Square | West | West (X) | A |
| Triangle (menu) | North | North (Y) | S |
| L and R | L1, R1 | LB, RB | Q, W |
| Start, Select | Options, Share | Menu, View | Enter, Backspace |
| Directions | d-pad or left stick | d-pad or left stick | arrows |
| L2, R2 | triggers | triggers | E, R |

The confirm button is a setting (`south`, `east` or `auto`; `auto` is South on Xbox and East elsewhere, as in the games of
each console's own habit). Whatever it is, the glyph drawn is the picture of the **physical** button that does the job.

## How the glyphs are swapped (design)

1. The button textures of the game (the interface pictures 15 to 19, 23 and 24, variant 0: cross, circle, triangle,
   square, d-pad, L, R) come out of `Graphics::ui_image` as an `Image` marked with a **glyph slot**.
2. When the scene is built (`Graphics::scene`), an item whose picture has a slot gets the picture of the current family
   for that slot. Nothing is cached in the sprites, so the glyphs change on screen the moment the family does.
3. The pictures are the PNG of our own art (`port/skins/<family>/`, made by `tools/hokago.py make_button_art`), reduced to
   the size of the original (28x28 and 56x28) as the game places them. Higher resolution waits for the internal render
   scale (F5-3).
4. **Keycaps** are made at run time: the key's picture from `port/skins/kbd/` with the label of the bound key written on
   it by `stb_truetype` (public domain) in a subset of M PLUS Rounded 1c (a few tens of KB). A long label (Enter, Esc) uses
   the wide key; it is centred on the original's centre, so it does not shift the layout.
5. Pictures the scripts never ask for (L2, R2, the mouse) are for the port's own prompts later.

## Settings

A small file, `%APPDATA%\open-amagami\input.ini`: the keyboard binding of each action, `confirm=auto|south|east`, `rumble=on|off`,
`fallback=playstation|xbox` (the family for pads that are neither, so a player with a generic pad can choose the labels that
fit it), `glyphs=auto|keyboard|xbox|playstation` (forcing a family). A window to edit them belongs to the launcher, where the fallback is a selector next to the confirm button; until then the
file is edited by hand and the program says in its title bar which family it shows.

### Keyboard rebinding (F5-14, 2026-10-25)

The launcher's Settings has a **Controls...** button: a window with the fourteen game buttons (the four directions, confirm, cancel, square, triangle, L, R, L2, R2, Start, Select) and the key of each. Clicking one waits for the next key (Esc keeps the old one); a key already bound to another button is refused with a note naming that button, and the window's own keys (Esc, F9, F10, F11) cannot be bound. **Restore defaults** clears them. `config.ini` keeps `key_<name>=<Windows virtual-key code>` (`key_confirm=90` for Z); an empty or unknown value is the default, and a key named twice in a hand-edited file keeps its first button.

The table is `input.cpp` (`BUTTONS`, `load_keys`, `key_to_mask`, `key_label`): the window reads it for the game and the key pictures (`Skins::keycap`) for the prompts, so a rebound key shows its new label in the game. A key picture shows the key's letter or digit, a symbol for the arrows, Enter, Backspace, Shift, Space and Tab, and a short name for the rest (`Ctrl`, `Del`, `F5`, `N4` for the numeric keypad...). Shift, Ctrl and Alt are bound as one key (either side). Pad buttons are not remapped yet.

## Extras this makes possible

- **Rumble:** the Vita scripts call `ControlPad.actuator` 387 times and the port ignores it; SDL can drive the motors
  (a setting, off by default until it is tuned).
- **Hot plug** without restarting; several pads, the last one used wins.
- **A `gamecontrollerdb.txt`** next to the program, loaded if present, for pads that SDL does not know.

## Order of work

1. Add SDL3 (headers and the x86 library with its DLL, `port/third_party/sdl3/`, licence file) and link it. A test that
   opens the subsystem and lists the pads.
2. `input`: the actions, the keyboard bindings, the SDL pad polling (replacing the XInput code), the last-device tracking,
   the family. Tested with a virtual pad of each kind (`--fake-pad xbox|ps5|switch|generic`).
3. `glyphs`: slots in `Graphics`, the family pictures, the swap at scene build. Tested by screenshots of a menu in each
   family.
4. Keycaps with labels.
5. Settings file, rumble, `gamecontrollerdb.txt`.
6. Documentation and decision entry.

## Status (2026-10-23, confirmed by running the program)

- **SDL3 3.4.18** is vendored (`port/src/third_party/sdl3`, headers, x86 library and DLL) and linked; `input.cpp` replaces the XInput
  code. Hot plug works through `SDL_UpdateGamepads` and the list of pads, once per frame.
- **Detection**, tried with virtual pads (`AMG_FAKE_PAD=ps5|ps4|xbox|switch|generic`, `AMG_FAKE_PAD_TAP=<frame>` presses its
  south button once): DualSense and DualShock 4 give PlayStation, Xbox One gives Xbox, Switch Pro and a generic pad give the
  fallback (PlayStation, or Xbox when `fallback=xbox`). The title bar says `input: <name> (<family>)`.
- **Glyphs:** the game's button sprites (textures 15 to 19, 23, 24) are marked with a slot; `Graphics::scene` swaps them for the
  current family's pictures (`port/skins/`, from `make_button_art`, reduced to the sprites' size). Checked on the name-entry
  screen in each family: PlayStation shows Square, Circle, Cross in colour; Xbox shows X, A, B; the keyboard shows keys labelled A,
  Z, X. The d-pad glyph is the "down" picture, as the game's own is. Seen on the name-entry screen in each family (2026-10-23): the L
  and R tabs become the long keys Q and W, L1 and R1, LB and RB; the small diamond the game uses for Select (left) and Start (right)
  becomes Backspace and Enter, the compact Share and Options pills, or the View and Menu discs. The arrow keys' symbols are made
  the same way but not yet seen on a screen that uses them.
- **Keyboard keys:** the table of keys is in `input.cpp` (`key_to_mask`, `key_label`), used by the window and by the pictures, so
  they agree. The label is written on the key with `stb_truetype` in `port/skins/keycap.ttf`.
- **Settings:** `confirm=south|east`, `fallback=xbox|playstation`, `glyphs=keyboard|xbox|playstation` in
  `%APPDATA%\open-amagami\config.txt`, or the environment variables `AMG_CONFIRM`, `AMG_FALLBACK`, `AMG_GLYPHS` (the variable wins). A
  settings window is for the launcher; rebinding the keyboard is not built (the table is fixed).
- **Not done:** rumble (the Vita's `ControlPad.actuator` is not wired to `input::rumble`), the community `gamecontrollerdb.txt`, an x64
  build of SDL.

## The mouse in the menus (2026-10-23, confirmed on the main menu)

`port/src/menu_pointer.cpp` plays the menus like a person with a pad, from the picture: a menu draws its entries as sprites and a
cursor (interface picture 20) on one layer and moves the cursor when sent a direction. The pointer finds the cursor, the entry
nearest to it (the selected one) and the entry under the mouse, and presses the directions until they coincide (a press every
fourth frame); a left click then presses confirm, a right click cancel, the wheel up and down. No script is touched. It follows the
mouse only after it moved, so the keyboard or the pad can still change the selection. Limits: a menu without the cursor sprite
(dialogue, the map, the gallery's own touch) is not played; grids and sliders are not checked.

## Port settings inside the game's Settings screen (2026-10-23, confirmed in the running scene)

The game's own Settings screen (`ConfigDialog`, its pages built by `ConfigPage#initialize(kind, items)`) shows five rows of the port on the "Misc" page, which has room for eight in all with the game's three: interface size (100 to 50 %, `uiscale`, `AMG_UI_SCALE`), render scale (`render`, `AMG_RENDER`), screen filter (smooth, sharp bilinear, pixels; `filter`, `AMG_FILTER`) and screen shape (`aspect`, `AMG_ASPECT`), then button pictures (auto, PlayStation, Xbox, keyboard). The confirm button and the unknown-pad family do not fit: they are read from config.txt (`confirm`, `fallback`) or `AMG_CONFIRM` / `AMG_FALLBACK` only, until the port has a menu of its own (F5-11 A). Changing one writes `config.txt` and applies at once (`input::set_settings`).

How it works, with nothing of the game's data touched on disk:
- `port/script/*.scfs` are our own classes, written in the decompiled-source syntax and compiled with hokago-tools (`scf_src.class_from_source` + `scf.build`) to the `.scf` files beside them (committed; they hold only our code). `PortSwitch` is a row that duck-types the game's `ConfigSwitch` (enable, disable, open, close, fade, set, left, right, getPosY, getAction, default, destruct); `PortHooks.configPage(kind, items)` appends the rows when `kind == 3`; `PortSettings` is an empty class whose class methods (`count`, `label`, `text`, `step`) are native (`port/src/port_settings.cpp`).
- The VM loads every `*.scf` of `script/` (beside the program, or `AMG_SCRIPT_DIR`) with the game's classes. When `ConfigPage` is defined, `VM::patch_config_page` inserts five instructions at the start of `initialize/2` (call `PortHooks.configPage(arg0, arg1)`, pop) and moves every jump and `try` target by five. The patch lives in memory only.
- The game's text is Shift-JIS with the Latin letters full width (0x82xx), also in the English layer; `port_settings.cpp` encodes its English labels that way. Japanese labels are not done.
- The pad's rumble: `ControlPad.actuator` (primitive 6) calls `VM::on_rumble`, which the window wires to `input::rumble`; the game's own "vibration" test row uses it. Not tried on hardware.
- Test without the window: a scene class that runs `ConfigMain.new().scriptMain()` (overlay), `--press` R twice and down; `AMG_SCRIPT_DIR=port/script` for `amagami_vm`.

## Open

- Whether to ship an x64 build of SDL too (the port builds as 32-bit today).
- How the Switch Pro is best treated once there is art for it (its buttons are labelled like Xbox's but sit like
  PlayStation's).
- Whether the lightbar of a PS4 or PS5 pad should follow the game (a colour for the heroine of the moment): a nicety, not planned.
