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
- **Not done:** an x64
  build of SDL.

## The mouse (design of 2026-10-31, F5-8 / F5-21)

**Native menus (2026-10-31, steps 3 to 5 of technical/mouse-native-design.md; confirmed headless).** On the main menu and the
map / in-game menu (`CommandMenu` owned by `MainMenu`, `GameMenu`, `GameMenuMap`) the mouse no longer sends presses:
`port/src/mouse_menus.cpp` finds the menu from the script threads' stacks, hit-tests its entry sprites by screen object, and a
new entry under the mouse starts one `PortMouse.select` script thread (`CommandMenu.moveCursor(i)`, sound 32, the owner's copy
of the index): one move of any length, 2 frames from the mouse to the index (was 5 to 9). Click on the selected entry =
confirm for one frame; on another entry = select, then confirm; right click = cancel; wheel = one entry a notch. Mouse mode
(a real move, a click, the wheel; ended by any key or pad button, after which hover waits for the next move) hides that
menu's arrow; the lit entry stays. Every other screen still uses the bursts below until its adapter exists. The cursor is
now the game's own arrow (interface picture 20) turned to point up-left, hot spot at its tip (`cursor_art::make`); the
drawn arrow described next is only the fallback before the game's data is read.

**Settings, port options and name entry (2026-10-31, steps 6 and 7; confirmed headless).** Same mechanism, three more
adapters in `mouse_menus.cpp`, each a `PortMouse` method (`port/script/PortMouse.scfs`) run as a one-shot thread:
- *Settings* (`ConfigDialog`, a local of `ConfigMain.scriptMain`) and *port options* (`PortDialog`, a local of
  `PortMain.scriptMain`): rows from `ConfigPage.items` at `12 + posY`, "Defaults" at 86, tabs = `ConfigPage.tagBase`
  sprites. Hover a row: `PortMouse.row` (the body of `moveCursorU` with a target: sound 31, `curY`, `clast[page]`,
  `setButtonGuide`, `stateNext = 3`), 1 frame at any distance. Click a volume note or a speed arrow: `PortMouse.volume/speed`
  calls the row's own `left`/`right` the needed times in one call (the value read with the row's getter), one sound 21;
  holding the button drags the value. Click a text value: its left half `selectValueL`, the rest `selectValueR`. Click the
  label of the selected row: confirm (its action); "Defaults": select, then confirm. Click a tab: `changeTag(i)`; the L/R
  marks and the wheel over the tabs: `changeTagL/R`; wheel elsewhere: a row a notch. Right click or the "back" guide: cancel.
  The rows have no highlight of their own (all titles at light 1), so the arrow is the only mark of the selected row: in
  mouse mode it is hidden and the port draws a translucent bar in the selected tab's blue behind the row instead
  (`Graphics::mark`, step A of the design; it follows the arrow's move and fade). Keys and pad bring the arrow back.
- *Name entry* (`NameEntry.scriptMain`: categories; `NameEntry.selectChar`: the grid). Categories: hover =
  `PortMouse.category` (sound 32, `menu.moveCursor(i)` into the owner's `curY`, `list.openPage`), the arrow hidden as on the
  main menu. Grid: cells computed as `NameEntryList.moveCursor` places its orange cursor (row strings, `linePos`, `mode`,
  `subPage`); hover = `PortMouse.cell` (sound 31, `curX`/`curY`, `moveCursor(3)`), 1 frame; click on the cursor's cell =
  confirm (enters the character); click on a kana while the categories have the cursor = confirm into the grid, then that
  kana; click on a category from the grid = cancel out, then that category. Name field: hover a character place moves the
  field cursor (`PortMouse.name`, `edit.moveCursor(i)` and the owner's `curX`); click on Back (place 0) or Decide (place 7)
  moves there, and the owner's loop asks its own question. Wheel in the grid: `moveCursorU/D` (scrolls).
- *Questions waiting for O / X* (`ControlPad.wait` innermost, e.g. "Decide on this name?"): click = confirm, right click =
  cancel (the old rule, now also when the old pointer would have seen a stale grid behind the question).
Latency (`tools/mouse_latency.py`): Settings rows 1 frame (was 11 / 8 with bursts), port options 1, name categories 2, grid
1, all with 0 presses. Replays `mouse_settings_values`, `mouse_port_options`, `mouse_name_entry`, `mouse_name_entry_ui80`;
`mouse_settings` and `mouse_settings_ui80` re-blessed.

**Gallery, choices, topics, Load / Save (2026-10-31, steps B and C of the design).** One shared rule
(`MouseMenus::drive`) and four adapters: the gallery grid and tabs write `Gallery.nextX/nextY/nextPage`, which its own loop
turns into the move or page change (1 frame, 0 presses; confirmed headless); choices (`Selector.waitSelect`,
`PortMouse.choice`), conversation topics (`KaiwaMain.wadaiSelect`, `PortMouse.topic`) and Load/Save
(`selectShiori`, `PortMouse.shiori/shioriStep`) are built but not reached headless yet. Details, the coverage table and the
audit (`tools/mouse_latency.py ROOT --check all`) in technical/mouse-native-design.md. `AMG_MOUSE_LOG` kinds added: `y`
gallery, `q` choice, `k` topics, `l` Load/Save.

**The cursor (built; the window part not tried by hand).** The port draws its own arrow (`port/src/cursor_art.cpp`: a white
arrow with a dark plum outline, rasterised at run time with 4 x 4 samples a pixel) and the window makes it the system cursor of
its client area (`WM_SETCURSOR`), rebuilt at the size of the picture on every `WM_SIZE` (24 pixels at the Vita's 544 lines,
48 at 1088, never under 16). Being the system's cursor it follows the mouse with no frame of delay and is never part of the
picture, so no capture holds it: not the replays, not `--shot`, not `AMG_SHOTS`, not F9. With `AMG_MOUSE_CURSOR_IN_SHOTS=1` the
captures stamp the same arrow where the mouse is (headless: where `--mouse` put it). Policy: shown when the mouse really moves
(Windows's repeated `WM_MOUSEMOVE` at the same place is ignored) or a mouse button or the wheel is used; hidden at once when a
game key or a pad button is pressed (the machine's thread posts `WM_AMG_HIDE_CURSOR` for the pad), and after three seconds of
rest when no menu the pointer plays is on the screen (dialogue, events); in a menu it stays. Full screen follows the same rule
(the old `ShowCursor(FALSE)` in full screen is gone). The click that brings the window to the front is eaten
(`WM_MOUSEACTIVATE` answers `MA_ACTIVATEANDEAT`), so it does not confirm anything; a button pressed outside and dragged in
is not a click (no `WM_LBUTTONDOWN` arrives); losing the focus clears the button, the right click, the wheel and everything
the pointer still had to press.

**Response: bursts of presses, not a hook (confirmed headless).** `port/src/menu_pointer.cpp` still plays the menus like a
person with a pad, from the picture (the entries and the cursor, interface picture 20, on one layer). A direct hook that set
the menu's index was considered and not done: the index is kept twice (`MainMenu.cursor` mirrors `CommandMenu.curY`,
`ConfigDialog.curY` is copied into `clast[page]`), the move runs the script's own state 3 (sound, button guide, animation), and
the scripts already accept a press every second frame, which the pointer now uses. The scripts read `ControlPad.trig`/`rapid`
once a frame and a press is new only on its first frame (`VM::buttons_pressed`), so one frame held and one free is the fastest
a pad can be. When the game's cursor is at rest (its picture did not move since the last frame), the pointer counts the
presses to the entry under the mouse (the distinct rows, and on a grid the columns, between the two; `places_between`) and
sends them all, one every second frame, then waits three frames and reads the picture again (a closed loop: a wrong count is
corrected, never overshot blindly). If the mouse goes to another entry during a burst, the rest is dropped and planned again.
A click on the entry under the cursor confirms on that frame; the right button cancels at once and drops the burst; the wheel
moves a row a notch (in the Settings screen over the tabs it changes the tab; in the gallery it turns the page, L1 / R1).
Volume and speed sliders get the whole difference of notes in one burst (a frame more of rest, as the notes fade in three).
A key or a pad button pressed while a burst runs drops it (`MenuPointer::yield`), so they take over at once. The day map keeps
its older rule (a press every eighth frame: the view scrolls). No script is touched.

**Latency, measured** (`python tools/mouse_latency.py ROOT NAME`: frames from the tick the mouse reaches an entry to the first
tick the game's cursor is drawn on it; the sessions are in `tests/replays/`):

| screen, move | before (a press every 4th frame) | now |
|---|---|---|
| main menu, 2 entries down (`mouse_mainmenu`) | 9 | 5 |
| main menu, 4 down / 3 up (`mouse_menu_far`) | 17 / 13 | 9 / 6 |
| Settings rows, 5 down / 4 up (`mouse_settings`) | 21 / 17 | 11 / 8 |
| gallery grid, 4 cells (`mouse_gallery`) | 17 | 10 |
| gallery grid, 9 cells / 2 back (`mouse_gallery_far`) | 37 / 7 | 20 / 5 |

The number of presses is the same before and after (no overshoot). Part of what is left is the game's own animation of its
cursor (four frames in `CommandMenu` and `ConfigDialog`).

**Headless.** `AMG_MOUSE_LOG=1` with `--mouse` prints one line a tick: the screen kind the pointer saw (`m` menu, `n` name grid,
`s` Settings, `d` day map, `g` gallery, `c` a registered menu of mouse_menus.cpp, `-` none), the entry the cursor is on, the one under the mouse and the buttons pressed.
A `--mouse` entry without flags is a hover only.

## The port's own screen, from the game's main menu (F5-11; 2026-10-31, confirmed headless on both layers)

The port's options are a sixth entry of the game's main menu, under Settings: "Port options" on the English layer, [Japanese word] on the Japanese one. It opens a screen in the look of the game's Settings screen (the same panel, tab bar with L/R, rows, arrow cursor, "Defaults" and the cancel button's "Back"), with the port's fifteen rows of `port_settings.cpp` in three tabs of at most eight: **Display** / [Japanese word] (window mode, interface size, render scale, screen filter, screen shape, brightness, gamma, vertical sync), **Controls** / [Japanese word] (button pictures, confirm button, unknown pad) and **System** / [Japanese word] (frame counter, splash screen, launcher at start, check for updates). Up and down move, left and right change the value (written to config.ini and applied at once through `port_settings::save`, as before), L and R change the tab, confirm on "Defaults" puts that tab's rows back to their defaults, cancel goes back to the menu. The game's own Settings screen is untouched: the paged rows the port used to add to its Misc tab (2026-10-27 to 2026-10-30) are gone.

How it works, with nothing of the game's data touched on disk:
- `port/script/*.scfs` are our own classes, written in the decompiled-source syntax and compiled by `python tools/port_scripts.py GAME_ROOT` (hokago-tools' `scf_src.class_from_source` + `scf.build`; the game's image is read only to resolve our classes' parents) to the `.scf` files beside them (committed; they hold only our code). A method's `locals=` counts its arguments too (`args=1` with three locals is `locals=4`); too small a count gives no error, the extra locals are simply lost.
- The VM loads every `*.scf` of `script/` (beside the program, or `AMG_SCRIPT_DIR`) with the game's classes, and counts them as the image's for `ControlPad` (the image's pad layout; classes outside the image are read as scenario scripts with the PS2 layout, which made "down" read as cancel).
- **The patch (`VM::patch_main_menu`).** When `MainMenu` is defined, three calls are inserted into it, each found by the shape of its code, not by an offset: in `makeMenu`, before `CommandMenu.new(items, ...)`, `items = PortHooks.mainItems(self.menuCom, items)` (one more item, command 8, with a placeholder sprite); after `self.menu = ...`, `PortHooks.mainMenu(self.menu)` (the last entry's sprite is replaced by `PortMenuEntry`); in `scriptMain`, after `choice = self.menuCom[self.cursor]`, `PortHooks.mainSelect(choice, self.menu)`, which runs `PortMain` for command 8. The game's own commands are 0 to 7, so its chain of `if` falls through for 8 and goes on as after any entry. Jumps past an insertion point move with the code; a jump to the point itself lands on the inserted call (the loop of `makeMenu` exits there). If any of the three is not found nothing is patched. Without the port's scripts the class is not touched.
- **The entry (`PortMenuEntry`).** The menu's entries are pictures with the word drawn in (interface texture 42: 0 [Japanese word], 1 [Japanese word], 2 album, 3 [Japanese word], 6 free play, 8 [Japanese word], 10 [Japanese word]; 4 [Japanese word], 5 [Japanese word], 7 [Japanese word] and 9 [Japanese word] are unused on the Vita; 11 and up are empty). The port's entry is texture 42's variant 1004, which the port makes itself (`Graphics::blank_stroke`): the stroke of the unused [Japanese word] entry with its lettering painted out (white pixels inside the stroke and the dark outline within three pixels of them, filled row by row from the stroke's colours left and right), with the label in the game's font on top (`TextLineC`, the layer's row size less one). It answers what `CommandMenu` asks of an entry's sprite (setAlpha, setPos, move, fadeLight, destruct), so the menu animates, dims and highlights it like the others. The menu's own geometry is kept (pitch 24 from y -64): six entries end at y 56 of the 136 below the centre. With every extra unlocked (eight of the game's, nine in all) the pitch is set to 22 so the last one stays on the screen (not seen: needs a save with everything unlocked).
- **The screen.** `PortMain` (a `K2X_Script`) dims the menu, runs a loop like the game's `ConfigMain` on a `PortDialog`, and enables the menu again. `PortDialog` derives from the game's `ConfigDialog`: its `initialize` runs the game's (panel, "Defaults", guides and cursor, in the layer's own look and text), then destroys the game's pages and puts the port's in their place, one `PortPage` per tab with `PortSwitch` rows from y -76 at the game's 18. `PortPage` derives from `ConfigPage` and only replaces the tab's title. The game's instance variables are reached by name (`PortSettings.field` / `setField`, native), so a release or layer with other slot numbers still works; none of our classes that derive from the game's has instance variables of its own. `PortSwitch` duck-types the game's `ConfigSwitch`; its `default` resets the row (`PortSettings.reset`).
- `PortSettings` is an empty class whose class methods are native (`natives.cpp`): `menuLabel`, `tabs`, `tabTitle`, `tabRows`, `tabRow`, `label`, `text`, `step`, `reset`, `pitch`, `field`, `setField`. The tab table is in `port_settings.cpp` (`tab_table`): at most four tabs (the tab bar has four places) of at most eight rows; every row of the table should be in one tab.
- The game's text is Shift-JIS with the Latin letters full width (0x82xx), also in the English layer; `port_settings.cpp` encodes its English labels that way. On the Japanese layer the rows are in Japanese (UTF-8 in `port_settings.cpp`, converted to Shift-JIS with the Windows code page 932); full-width Latin is wide there, so "PlayStation" is "PS" on that layer. The layer, and the font size of its rows, are read from the game's own `ConfigSwitch` constants the first time a text is asked for (`port_settings::set_layer`): the original Japanese uses 13, the English patch a smaller one.
- The pad's rumble: `ControlPad.actuator` (primitive 6) calls `VM::on_rumble`, which the window wires to `input::rumble`; the game's own "vibration" test row uses it. Not tried on hardware.
- `PortMessage` (2026-10-31) is a port class run by a native rather than by the patch: `MemoryCard` primitive 10 starts it in its own script thread (`VM::new_thread`, as `Thread.start` does) and 11 waits on it; it shows the card's message in the game's `ConfirmDialog` (save.md, "The card's message box").
- Tests: `tests/replays/port_menu_main.json` (the six entries, the arrow on the port's) and `port_menu_tabs.json` (each tab, a value changed, "Defaults", back). Checked by eye on the Japanese copy and the owner's English copy.
- Open: the mouse pointer (`menu_pointer.cpp`) finds entries as sprites near the cursor; whether it takes the port's entry was not checked. The English label fills its stroke to the edge.

**Pad buttons (2026-10-26).** The launcher's Settings, Controls...: every game button has its key and its pad button side by side. A pad button is one of SDL's standard buttons or a trigger (`input::pad_source_*`); "Automatic" is the old rule (d-pad, face buttons by position with the circle on south or east, shoulders, triggers, start, back). config.ini keeps `pad_<button>=<south|east|west|north|leftshoulder|rightshoulder|lefttrigger|righttrigger|back|start|dpup|dpdown|dpleft|dpright|leftstick|rightstick>`. The window starts SDL's pads itself if the game has not (`input::init`/`shutdown`) and reads them by a 30 ms timer while a row waits. The left stick as a d-pad stays on whatever is chosen.

**Pad database (2026-10-26; the box in the launcher the same day).** The launcher's Settings window has **Pad mapping...**: paste mapping lines, they are checked and appended to `gamecontrollerdb.txt` in the folder of `config.ini` (`input::save_pad_mappings`), which the game reads at start next to the one beside the program. At start, a `gamecontrollerdb.txt` beside the program (SDL's format) is added to SDL's own mappings (`SDL_AddGamepadMappingsFromFile`), so a pad SDL does not know can be used; the player writes or downloads the line. Not in the launcher yet.

## The mouse in the Settings screen, on the day map and elsewhere (2026-10-27, F5-21; confirmed in the window unless marked)

Driven in the real window (x86 and x64 builds, messages posted to the window, no focus taken), each result read from a capture and from the scene's object list (F9).

**Settings screen** (`MenuPointer::settings_update`, chosen when the scene has two or more tab pictures, interface 47). It does not use the generic "nearest picture" rule: it builds the rows from the pictures on the cursor's layer inside the panel (one row per height, 18 units apart) and reads what each holds. Hover moves the cursor to the row under the mouse (after the mouse moved, as on the other menus); a click acts by the kind of row:
- *Volume rows* (notes, interface 33, a note 12 wide every 18): a click on a note sets the value to that note (1 to 5), a click left of the first note sets 0; holding the button and dragging sets the value under the mouse while the button is down, even over other rows (the row is kept). Done by pressing left or right until the number of bright notes equals the goal.
- *Speed rows* (arrows, interface 7, 12 apart; the text speed and the message speed): same, the least value is 1.
- *Switch rows and the port's rows* (a text value at the right, x above 90; the port's own screen has the same panel and tab pictures, so these rules should apply to it too: hypothesis, not checked in the window): a click on the label or on the right half of the value presses right, on the left part of the value (x from 100 to 140 in script units) presses left. Both are seen changing a value; at the end of a list the value does not wrap (that is the game's own rule for a pad).
- *Reset row* (the last row, at the bottom, no value): a click presses confirm.
- *Tabs* (the three pictures, and the L and R pictures beside them): a click on a tab presses L or R, one press every 12 frames (the page's animation), until the lit tab is the one clicked; a click on the L or R picture steps one tab. Seen going to Text and Misc.
- *The "back" word* (interface 62, variant 2, at the bottom right of the screen and of every menu's hint at the top right): a click on it, or on its button picture beside it, presses cancel. Seen leaving the Settings screen (the save question appears, as with the pad). This works in every screen that draws that word (checked on the Settings screen).
Right click and the wheel work as before.

**Day map** (`MenuPointer::map_update`). The map has no picture cursor but four corner pictures (interface 25) and four guide arrows (interface 28 to 31); the script moves the cursor a cell at a time (a staggered grid, columns 42 apart, rows 44, `NodeMap.move`) and scrolls the map under it. The pointer takes the mean of the four corners as the cursor and, when the mouse moves inside the map area, presses the direction toward it (right or left, with up or down as well when it is 11 or more units off in height, else up or down alone) every eighth frame, as the view takes about six frames to settle, until the cursor's cell holds the mouse; a click does the same and presses confirm on arrival. It gives up when three presses bring it no closer (the edge of the map). A click on a guide arrow presses that direction once; a click in the bottom right corner ("Other") presses triangle, which opens the game's menu (Load, Settings, Title). Seen on the prologue's map: the cursor follows the mouse to the cell under it (checked at three places), a click on the home icon from far away goes there and opens "Decide on this action?", the arrow moves the cursor a row, the corner opens the menu. The prompts (confirm and cancel at the bottom, no corners or arrows of their own) keep the older rule: a click confirms, the right button cancels. **Not checked:** days with several nodes (the prologue has one), the hint, and the rows being "nearest hexagon" at the cell edges (the cell is taken as 42 by 44 units, a rectangle, so a mouse near a corner of the hexagon may land on the neighbour).

**Gallery** (2026-10-29, `map_update` with `gallery`). The gallery's grid has the same four corner pictures as the map but no guide arrows, and its script reads only the pad (`ControlPad` in `AlbumMain`; the earlier idea that it read touch was wrong). With four corners and no arrows the pointer treats the picture as a grid of 50 by 50 unit cells (the cursor is the mean of the corners): hover presses the rows then the columns between in one burst (one press every second frame, see "The mouse" above) until the cursor's cell holds the mouse; a click does that and confirms; the two page arrows at the sides (interface 7) press L1 or R1. **Confirmed** headless (`mouse_gallery`: the cursor goes to the cell under the mouse and a click opens the picture; `mouse_gallery_page`: the right arrow turns the page). Not checked: the tabs at the top by click.

**Headless mouse.** `amagami_vm --mouse TICK:X:Y[:FLAGS]` (X, Y in script units from the middle of the picture; `d` left button held, `r` right click, `u` and `n` the wheel) runs the same `MenuPointer` as the window from the last picture, so a replay session can have `"mouse": [[tick, x, y, "d"], ...]` and the mouse is covered by the regression net (`tests/replays/mouse_*.json`).

**At another interface size (2026-10-31, steps 1 and 2 of technical/mouse-native-design.md; confirmed headless).** The
pointer compares with the picture as the player sees it: `Graphics::pointer_scene` runs the same view transform as the
renderer (`view_of`: `VIEW_UNIT`, `VIEW_SCALE`, the PS2 `scale_on_screen` correction and the anchored interface shrink,
technical/interface-scale.md) and gives each item back with its drawn centre in script units of the picture (picture pixels /
`VIEW_UNIT` from the middle), its scale times the shrink, `shrunk` set, the factor (`Scene::ui_u`) and **the item's own
anchor** (`DrawItem::ui_ax/ui_ay`, its group's, taken from the group's principal item since 2026-10-31 so that animated
members such as the Settings' L1/R1 marks do not sway the panel: interface-scale.md, "Stability"). An item wholly outside the screen or its own clip gets alpha 0 there, so a
parked entry is never under the mouse. The mouse is mapped only from window pixels to picture pixels (the letterbox), in the window and in the headless
`--mouse` alike (X, Y of `--mouse` are those picture units: at 100 % the same as before). The numbers `menu_pointer.cpp` knows
by heart (the Settings panel's bounds and row pitch, the Decide button, the day map's area and cell, the "back" hint's box,
the size limits of an entry) are layout positions; `MenuPointer::X/Y/D` (and `mouse_menus.cpp`'s `Layout`) map them through
`UiPlace::of(scene, ref)` (gfx.hpp): the factor and the anchor of the screen's reference picture (the cursor, the grid
rectangle, a map corner, the dialog), which is the anchor of the group the layout belongs to. The scripts' own touch (gallery
swipes) still works in layout units, so the window and the headless tool pass it the mouse taken back through the shrink of
the frame's largest group (`Scene::ui_ax/ui_ay`; approximate on screens with several groups). Replays:
`mouse_mainmenu_ui80`, `mouse_mainmenu_ui60`, `mouse_settings_ui80`, `mouse_name_entry_ui80`, `mouse_gallery_ui80` (the
sessions at 100 % with the mouse aimed where the anchored shrink draws each entry, measured on the captures) end with the same
instruction count as their 100 % twins, so they reach the same entries. (The gallery's pictures are on layers below the
interface and are not shrunk.) At 100 % every replay is unchanged.

**Others.** The Load screen could not be reached (no save in the test data), so it is **not checked**; it is an ordinary menu with the menu cursor and should follow the generic rule.

## Open

- Whether to ship an x64 build of SDL too (the port builds as 32-bit today).
- How the Switch Pro is best treated once there is art for it (its buttons are labelled like Xbox's but sit like
  PlayStation's).
- Whether the lightbar of a PS4 or PS5 pad should follow the game (a colour for the heroine of the moment): a nicety, not planned.

## The mouse on the name entry's grid, and on every screen shape (2026-10-27, confirmed in the window)

**The grid.** The name entry has three zones with their own cursors: the list of categories (the interface picture 20, as in every menu), the bar on top, and the grid of characters, whose cursor is not a picture but an orange rectangle (light 1, 0.5, 0.2) behind the character it is on. `MenuPointer` now takes that rectangle as a cursor when there is no picture cursor: the characters on its layer or the next, below the top bar (script y above -90 are the name fields and the buttons), are the entries, each as big as the rectangle; the mouse over one presses the directions until the rectangle is there, and a click confirms. Driven in the real window: the pointer over a kana moves the rectangle to it, a click picks it and the candidates open, as with the pad.

**A bug found on the way.** The window mapped the mouse to the picture as if the screen shape were always "Fit", so with Fill, Stretch, Whole or the blurred edges every click landed in the wrong place. `fitted_viewport` now uses the same arithmetic as the presenter (`g_aspect`, set with the row).

**The Decide button and a DPI bug (2026-10-27, confirmed in the window).** On the name entry a click on the "Decide" button (top right) presses Start for six frames, as the pad does; checked: the click opens "Decide on this name?", and confirming goes on to the map. The first try showed a worse bug that the DPI change had brought in: the machine's thread is not DPI aware, so its `GetClientRect` answered in scaled units (1016 for a window of 1525 real pixels on a display at 150 %) while the mouse arrives in real pixels, and every mouse position was off. The window's own thread now keeps the client size (`g_client_w`, `g_client_h`, set on `WM_SIZE`) and the machine reads that. **The earlier "confirmed" note on the grid above was wrong about where the pointer landed** (it chose a kana by luck of the wrong scale); with the right size the pointer lands where the mouse is.

**Sticks order (2026-10-29).** On the Vita the engine's `analog(0..3)` are the left stick (x, y) then the right stick (x, y), and `analog(4)` is the left stick's length (read twice, technical/d1-menus-and-pad.md). The port had the PS2's order (right stick first); `input.cpp` now gives the Vita's. Effect: the day map reads the left stick's deflection, and the album's tall pictures scroll with the right stick's vertical axis. Not seen in the game with a real pad.
