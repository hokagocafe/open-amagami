#include "input.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <mutex>
#include <vector>

namespace amg::input {

namespace {

struct Pad {
    SDL_JoystickID id = 0;
    SDL_Gamepad* handle = nullptr;
    Family family = Family::PlayStation;
    int last_mask = 0;
    std::string name;
};

Settings g_settings;
std::vector<Pad> g_pads;
std::atomic<int> g_family{int(Family::Keyboard)};  // what the last device used asks for
std::mutex g_name_lock;
std::string g_name = "keyboard";
bool g_started = false;
SDL_Joystick* g_fake = nullptr;
int g_fake_frame = 0, g_fake_tap_at = -1;

Family family_of_type(SDL_GamepadType type) {
    switch (type) {
        case SDL_GAMEPAD_TYPE_XBOX360:
        case SDL_GAMEPAD_TYPE_XBOXONE:
            return Family::Xbox;
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5:
            return Family::PlayStation;
        default:
            return g_settings.fallback;  // Switch, standard, unknown: the family the player chose for them
    }
}

// The game's circle (confirm) sits on the south button when asked, or on an Xbox pad by default; east elsewhere.
bool confirm_on_south(Family family) {
    if (g_settings.confirm >= 0) return g_settings.confirm == 0;
    return family == Family::Xbox;
}

int read_pad(Pad& pad, std::array<int, 4>& axes, bool& deflected) {
    SDL_Gamepad* g = pad.handle;
    auto button = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(g, b); };
    int mask = 0;
    const bool south_confirm = confirm_on_south(pad.family);
    if (button(south_confirm ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST)) mask |= 32;   // circle: confirm
    if (button(south_confirm ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH)) mask |= 64;   // cross: cancel
    if (button(SDL_GAMEPAD_BUTTON_WEST)) mask |= 128;                                              // square
    if (button(SDL_GAMEPAD_BUTTON_NORTH)) mask |= 16;                                              // triangle
    if (button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) mask |= 4;                                       // L1
    if (button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) mask |= 8;                                      // R1
    if (button(SDL_GAMEPAD_BUTTON_START)) mask |= 2048;
    if (button(SDL_GAMEPAD_BUTTON_BACK)) mask |= 256;
    if (button(SDL_GAMEPAD_BUTTON_DPAD_UP)) mask |= 4096;
    if (button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) mask |= 8192;
    if (button(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) mask |= 16384;
    if (button(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) mask |= 32768;
    const int TRIGGER = 4000;  // about an eighth of the travel (SDL: 0..32767)
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TRIGGER) mask |= 1;   // L2
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER) mask |= 2;  // R2
    int lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
    int rx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX), ry = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY);
    const int DEAD = 16000;  // about half way: the stick acts as a d-pad, so it must be deliberate
    if (ly < -DEAD) mask |= 4096;
    if (ly > DEAD) mask |= 16384;
    if (lx < -DEAD) mask |= 32768;
    if (lx > DEAD) mask |= 8192;
    // SDL's y grows downwards, as the engine's bytes do (docs/technical/scf-vm.md): -32768..32767 to -128..127
    auto scale = [](int v) { int x = v / 256; return x < -128 ? -128 : x > 127 ? 127 : x; };
    axes = {scale(rx), scale(ry), scale(lx), scale(ly)};
    deflected = std::abs(lx) > DEAD || std::abs(ly) > DEAD || std::abs(rx) > DEAD || std::abs(ry) > DEAD;
    return mask;
}

void attach_virtual(const std::string& kind) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Virtual pad";
    if (kind == "ps5") {
        desc.vendor_id = 0x054C;
        desc.product_id = 0x0CE6;
        desc.name = "DualSense Wireless Controller";
    } else if (kind == "ps4") {
        desc.vendor_id = 0x054C;
        desc.product_id = 0x09CC;
        desc.name = "DualShock 4 Wireless Controller";
    } else if (kind == "xbox") {
        desc.vendor_id = 0x045E;
        desc.product_id = 0x02EA;
        desc.name = "Xbox One Controller";
    } else if (kind == "switch") {
        desc.vendor_id = 0x057E;
        desc.product_id = 0x2009;
        desc.name = "Nintendo Switch Pro Controller";
    } else {  // generic
        desc.vendor_id = 0x1234;
        desc.product_id = 0x5678;
        desc.name = "Generic USB Gamepad";
    }
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    if (id) g_fake = SDL_OpenJoystick(id);
    if (const char* tap = std::getenv("AMG_FAKE_PAD_TAP")) g_fake_tap_at = std::atoi(tap);
}

void refresh_list() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (auto it = g_pads.begin(); it != g_pads.end();) {  // pads that went away
        bool still = false;
        for (int i = 0; i < count; ++i) still = still || ids[i] == it->id;
        if (!still) {
            SDL_CloseGamepad(it->handle);
            it = g_pads.erase(it);
        } else {
            ++it;
        }
    }
    for (int i = 0; i < count; ++i) {  // pads that came
        bool known = false;
        for (const Pad& p : g_pads) known = known || p.id == ids[i];
        if (known) continue;
        Pad pad;
        pad.id = ids[i];
        pad.handle = SDL_OpenGamepad(ids[i]);
        if (!pad.handle) continue;
        pad.family = family_of_type(SDL_GetGamepadType(pad.handle));
        const char* name = SDL_GetGamepadName(pad.handle);
        pad.name = name ? name : "gamepad";
        if (std::getenv("AMG_INPUT_LOG"))
            std::fprintf(stderr, "pad: %s type=%d family=%d\n", pad.name.c_str(), int(SDL_GetGamepadType(pad.handle)),
                         int(pad.family));
        g_pads.push_back(std::move(pad));
    }
    SDL_free(ids);
}

}  // namespace

namespace {

// The game's buttons in the order the rebinding window lists them, with their config.ini names and default keys (Windows
// virtual-key codes: arrows 0x25 to 0x28, Backspace 0x08, Enter 0x0D, letters their capital's code).
const GameButton BUTTONS[] = {
    {4096, "up", "Up", 0x26},          {16384, "down", "Down", 0x28},      {32768, "left", "Left", 0x25},
    {8192, "right", "Right", 0x27},    {32, "confirm", "Confirm (circle)", 'Z'}, {64, "cancel", "Cancel (cross)", 'X'},
    {128, "square", "Square", 'A'},    {16, "triangle", "Triangle", 'S'},  {4, "l", "L", 'Q'},
    {8, "r", "R", 'W'},                {1, "l2", "L2", 'E'},               {2, "r2", "R2", 'R'},
    {2048, "start", "Start", 0x0D},    {256, "select", "Select", 0x08},
};
constexpr int BUTTON_COUNT = int(sizeof(BUTTONS) / sizeof(BUTTONS[0]));
int g_keys[BUTTON_COUNT] = {};  // the key of each BUTTONS entry; 0 until load_keys (then the defaults)

void ensure_keys() {
    if (g_keys[0]) return;
    for (int i = 0; i < BUTTON_COUNT; ++i) g_keys[i] = BUTTONS[i].default_key;
}

}  // namespace

const GameButton* game_buttons(int* count) {
    *count = BUTTON_COUNT;
    return BUTTONS;
}

bool reserved_key(int vk) {
    return vk == 0x1B || vk == 0x78 || vk == 0x79 || vk == 0x7A;  // Esc (quit), F9 (screenshot), F10 (dump), F11 (full screen)
}

KeyLabel label_of_key(int vk) {
    switch (vk) {
        case 0x26: return {"", "up"};
        case 0x28: return {"", "down"};
        case 0x25: return {"", "left"};
        case 0x27: return {"", "right"};
        case 0x0D: return {"", "enter"};
        case 0x08: return {"", "backspace"};
        case 0x10: case 0xA0: case 0xA1: return {"", "shift"};
        case 0x20: return {"", "space"};
        case 0x09: return {"", "tab"};
        case 0x11: case 0xA2: case 0xA3: return {"Ctrl", ""};
        case 0x12: case 0xA4: case 0xA5: return {"Alt", ""};
        case 0x2D: return {"Ins", ""};
        case 0x2E: return {"Del", ""};
        case 0x24: return {"Home", ""};
        case 0x23: return {"End", ""};
        case 0x21: return {"PgUp", ""};
        case 0x22: return {"PgDn", ""};
        case 0xBA: return {";", ""};
        case 0xBB: return {"=", ""};
        case 0xBC: return {",", ""};
        case 0xBD: return {"-", ""};
        case 0xBE: return {".", ""};
        case 0xBF: return {"/", ""};
        case 0xC0: return {"`", ""};
        case 0xDB: return {"[", ""};
        case 0xDC: return {"\\", ""};
        case 0xDD: return {"]", ""};
        case 0xDE: return {"'", ""};
        default: break;
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return {std::string(1, char(vk)), ""};
    if (vk >= 0x60 && vk <= 0x69) return {"N" + std::to_string(vk - 0x60), ""};  // the numeric keypad
    if (vk >= 0x70 && vk <= 0x87) return {"F" + std::to_string(vk - 0x6F), ""};
    return {"?", ""};
}

std::string key_name(int vk) {
    KeyLabel k = label_of_key(vk);
    if (!k.text.empty()) return k.text;
    static const std::pair<const char*, const char*> NAMES[] = {
        {"up", "Up arrow"}, {"down", "Down arrow"}, {"left", "Left arrow"}, {"right", "Right arrow"}, {"enter", "Enter"},
        {"backspace", "Backspace"}, {"shift", "Shift"}, {"space", "Space"}, {"tab", "Tab"}};
    for (auto& [symbol, name] : NAMES)
        if (k.symbol == symbol) return name;
    return "?";
}

void load_keys(const std::function<std::string(const std::string&)>& read) {
    ensure_keys();
    for (int i = 0; i < BUTTON_COUNT; ++i) {
        int vk = std::atoi(read(std::string("key_") + BUTTONS[i].name).c_str());
        g_keys[i] = vk > 0 && vk < 256 && !reserved_key(vk) ? vk : BUTTONS[i].default_key;
    }
    // A key named twice (a hand-edited file) keeps its first button; the later ones go back to their defaults if free.
    for (int i = 0; i < BUTTON_COUNT; ++i)
        for (int j = 0; j < i; ++j)
            if (g_keys[i] == g_keys[j]) g_keys[i] = BUTTONS[i].default_key;
}

int key_of(int mask_bit) {
    ensure_keys();
    for (int i = 0; i < BUTTON_COUNT; ++i)
        if (BUTTONS[i].mask == mask_bit) return g_keys[i];
    return 0;
}

int key_to_mask(int virtual_key) {
    ensure_keys();
    int mask = 0;
    for (int i = 0; i < BUTTON_COUNT; ++i)
        if (g_keys[i] == virtual_key) mask |= BUTTONS[i].mask;
    // Shift, Ctrl and Alt come as their generic codes in WM_KEYDOWN: a binding to the left or right one still works.
    if (!mask && (virtual_key == 0x10 || virtual_key == 0x11 || virtual_key == 0x12)) {
        const int sides[3][2] = {{0xA0, 0xA1}, {0xA2, 0xA3}, {0xA4, 0xA5}};
        for (int s : sides[virtual_key - 0x10]) mask |= key_to_mask(s);
    }
    return mask;
}

KeyLabel key_label(int mask_bit) {
    int vk = key_of(mask_bit);
    return vk ? label_of_key(vk) : KeyLabel{};
}

void set_settings(const Settings& settings) { g_settings = settings; }
Settings settings() { return g_settings; }

bool init(const Settings& settings, const std::string& fake) {
    g_settings = settings;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL gamepads unavailable: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetGamepadEventsEnabled(false);  // the state is polled once per frame; nothing reads the event queue
    g_started = true;
    if (!fake.empty()) attach_virtual(fake);
    return true;
}

void shutdown() {
    if (!g_started) return;
    for (Pad& p : g_pads) SDL_CloseGamepad(p.handle);
    g_pads.clear();
    if (g_fake) SDL_CloseJoystick(g_fake);
    g_fake = nullptr;
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    g_started = false;
}

int poll_pads(std::array<int, 4>& axes) {
    axes = {};
    if (!g_started) return 0;
    if (g_fake && g_fake_tap_at >= 0) {  // a scripted press of the virtual pad's south button, for tests
        ++g_fake_frame;
        SDL_SetJoystickVirtualButton(g_fake, SDL_GAMEPAD_BUTTON_SOUTH,
                                     g_fake_frame >= g_fake_tap_at && g_fake_frame < g_fake_tap_at + 8);
    }
    SDL_UpdateGamepads();
    refresh_list();
    int mask = 0;
    bool took_axes = false;
    for (Pad& pad : g_pads) {
        std::array<int, 4> a{};
        bool deflected = false;
        int m = read_pad(pad, a, deflected);
        if (m != pad.last_mask || deflected) {  // activity: this pad's pictures become the current ones
            g_family = int(pad.family);
            std::lock_guard<std::mutex> lk(g_name_lock);
            g_name = pad.name + (pad.family == Family::Xbox ? " (xbox)" : " (playstation)");
        }
        pad.last_mask = m;
        mask |= m;
        if (!took_axes && deflected) {
            axes = a;
            took_axes = true;
        }
    }
    return mask;
}

void note_keyboard() {
    if (g_family.exchange(int(Family::Keyboard)) != int(Family::Keyboard)) {
        std::lock_guard<std::mutex> lk(g_name_lock);
        g_name = "keyboard";
    }
}

Family family() {
    if (g_settings.forced >= 0) return Family(g_settings.forced);
    return Family(g_family.load());
}

std::string describe() {
    std::lock_guard<std::mutex> lk(g_name_lock);
    return g_settings.forced >= 0 ? g_name + " [forced]" : g_name;
}

bool rumble(double strength, int milliseconds) {
    if (!g_started || g_pads.empty()) return false;
    const Uint16 s = Uint16(std::max(0.0, std::min(1.0, strength)) * 65535.0);
    return SDL_RumbleGamepad(g_pads.back().handle, s, s, Uint32(milliseconds));
}

std::string picture_of(int mask_bit, Family family) {
    if (family == Family::Keyboard) return "";
    const bool xbox = family == Family::Xbox;
    const bool south = confirm_on_south(family);
    switch (mask_bit) {
        case 32:  // the game's circle: confirm
            return xbox ? (south ? "a" : "b") : (south ? "cross" : "circle");
        case 64:  // the game's cross: cancel
            return xbox ? (south ? "b" : "a") : (south ? "circle" : "cross");
        case 128: return xbox ? "x" : "square";
        case 16: return xbox ? "y" : "triangle";
        case 4: return xbox ? "lb" : "l1";
        case 8: return xbox ? "rb" : "r1";
        case 2048: return xbox ? "menu" : "options";
        case 256: return xbox ? "view" : "share";
        default: return "";
    }
}

}  // namespace amg::input
