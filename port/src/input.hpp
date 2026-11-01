// Gamepads through SDL3, and which family of button pictures to show (docs/technical/input.md, D-025).
//
// The game's buttons are the PS2 engine's mask (VM::buttons_*): square 128, cross 64, circle 32, triangle 16, R1 8, L1 4,
// R2 2, L2 1, start 2048, select 256, d-pad 4096 (up), 8192 (right), 16384 (down), 32768 (left). A pad is read once per frame
// into that mask; the keyboard is read by the window and combined with it by the caller.
#pragma once

#include <array>
#include <functional>
#include <string>

namespace amg::input {

// The family of the pictures shown for the buttons.
enum class Family : int { Keyboard = 0, Xbox = 1, PlayStation = 2 };

struct Settings {
    Family fallback = Family::PlayStation;  // for a pad that is neither an Xbox nor a PlayStation one
    int confirm = -1;                       // where the game's circle (confirm) sits: -1 auto, 0 south, 1 east
    int forced = -1;                        // -1: follow the last device used; else a Family: always show that one
};

// SDL's gamepad subsystem; false if it cannot start (the keyboard still works). `fake` is "", or xbox, ps5, switch or
// generic: attaches a virtual pad of that kind, to test without hardware.
bool init(const Settings& settings, const std::string& fake = "");
void shutdown();
// A pad SDL does not know is added with a line of SDL's mapping database (as the community tools write them). The lines
// are kept in gamecontrollerdb.txt in the folder of config.ini and read at the next start, with the one beside the program.
std::string pad_database_file();
// Checks the lines of `text` (blank lines and # comments are skipped) and appends the new ones to that file. Returns how
// many were added; -1 with `error` set (and nothing written) if a line is not a mapping.
int save_pad_mappings(const std::string& text, std::string& error);

// Changes the settings while running (the game's Settings screen, F5-11).
void set_settings(const Settings& settings);
Settings settings();

// Once per frame: hot plug, then every pad's buttons as the engine's mask; `axes` receives the sticks of the first pad in
// the engine's scale (right x, right y, left x, left y; signed, down larger). Pad activity makes that pad's family current.
int poll_pads(std::array<int, 4>& axes);

// A key was pressed: the keyboard's pictures are current.
void note_keyboard();

// The family to draw now, and a short description for the title bar ("keyboard", "pad: <name> (xbox)").
Family family();
std::string describe();

// A short rumble on the pad in use, in the range 0..1 (the game's ControlPad.actuator); false if there is none.
bool rumble(double strength, int milliseconds);

// The keyboard. The table of keys is here, once: the window turns a key into the game's button with `key_to_mask`, and the key
// pictures say which key to press with `key_label`, so the two cannot disagree. The player rebinds them in the launcher
// (Settings, Controls); config.ini keeps `key_<name>` as a Windows virtual-key code.
struct KeyLabel {
    std::string text;    // what is written on the key ("Z", "Q"), or empty
    std::string symbol;  // or a picture over it: "up", "down", "left", "right", "enter", "backspace", "shift", "space", "tab"
};
struct GameButton {
    int mask;               // the game's button bit
    const char* name;       // in config.ini: key_<name>
    const char* label;      // in the rebinding window
    int default_key;        // a Windows virtual-key code
};
const GameButton* game_buttons(int* count);  // in the order the rebinding window lists them
void load_keys(const std::function<std::string(const std::string&)>& read);

// The pad's buttons (F5-14): each game button reads one of the pad's own buttons or triggers, `source` an index in the list
// below, or -1 for the automatic one (the d-pad, the face buttons by position, the shoulders, the triggers, start and back).
// config.ini: pad_<button name>=<source name>.
int pad_source_count();
const char* pad_source_name(int source);
const char* pad_source_label(int source);
int pad_source(int button);  // `button` indexes game_buttons()
void set_pad_source(int button, int source);
void load_pad_buttons(const std::function<std::string(const std::string&)>& read);
bool active();       // SDL's gamepads are started
// For the rebinding window: the first source pressed since the last call (any pad), or -1. Call once to start from what is held.
int take_pad_press();  // read(name) gives config.ini's value
int key_of(int mask_bit);                    // the key bound to a game button now
bool reserved_key(int virtual_key);          // the window's own keys (Esc, F9, F10, F11): never bound
KeyLabel label_of_key(int virtual_key);      // what a key's picture shows
std::string key_name(int virtual_key);       // a key's name in a window ("Z", "Enter", "Up arrow")
int key_to_mask(int virtual_key);  // the game's button bit for a Windows virtual-key code, 0 for none
KeyLabel key_label(int mask_bit);  // the key that does the game's button `mask_bit`

// Which button, as a position, does the game's `action` (a mask bit) sit on for this family, as the name of the picture
// ("cross", "circle", "square", "triangle", "l1", "r1", "options", "share" for PlayStation; "a", "b", "x", "y", "lb", "rb",
// "menu", "view" for Xbox). Empty for an action with no picture.
std::string picture_of(int mask_bit, Family family);

}  // namespace amg::input
