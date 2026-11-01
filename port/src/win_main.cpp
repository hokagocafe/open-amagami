// The Windows front end: a window that shows the screen-object model's frame, keyboard input and
// frame pacing at 60 Hz. The script machine runs on a worker thread; this thread owns the window.
//
//   amagami.exe [--root <folder with extracted/>] [--scale N] [--save-dir <folder>] [--lang CODE] [--renderer software|present|gpu]
//
// Keys: arrows = d-pad, Z = circle (confirm), X = cross (cancel), A = square, S = triangle,
// Enter = start, Backspace = select, Q/W = L1/R1, E/R = L2/R2, Esc = quit. Alt+Enter or F11 toggles
// borderless full screen. An XInput pad works alongside the keyboard (see pad_mask).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <deque>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "audio.hpp"
#include "d3d_present.hpp"
#include "data_check.hpp"
#include "shortcut.hpp"
#include "update_check.hpp"
#include "updater.hpp"
#ifdef AMG_PERSONAL
#include "personal/text_window.hpp"
#endif
#include "datapath.hpp"
#include "game_import.hpp"
#include "input.hpp"
#include "launcher.hpp"
#include "menu_pointer.hpp"
#include "mouse_menus.hpp"
#include "cursor_art.hpp"
#include "sampler.hpp"
#include "paths.hpp"
#include "port_settings.hpp"
#include "ui_text.hpp"
#include <functional>
#include "skins.hpp"
#include "gfx.hpp"
#include "vm.hpp"

using namespace amg;

// The report the program leaves when it stops (below, with the crash filter): `save_report` writes crash.txt in the data folder
// and returns its path ("" if it could not be written anywhere); `tell_report` is the window that says so.
std::string save_report(const std::string& headline);
void tell_report(const std::wstring& message, const std::string& path);  // `message`, then where the report is and an offer to open it

namespace {

// The rates and frame counter in the title bar are for development: a release shows only the name, unless AMG_STATS=1.
bool show_stats() {
    static const bool on = std::getenv("AMG_STATS") ? std::getenv("AMG_STATS")[0] == '1' : std::string(amg::update_check::current()) == "dev";
    return on;
}

}  // namespace

namespace {

// Windows 10 and 11 slow down processes that are minimized or in the background ("EcoQoS") and
// ignore their timer resolution request. A game that paces itself at 60 Hz must opt out of both,
// or it runs slowly whenever its window has no focus. The constants are written out because older
// SDK headers do not have them.
void opt_out_of_throttling(bool whole_process) {
    struct State {
        ULONG Version;
        ULONG ControlMask;
        ULONG StateMask;
    } state = {1, 0x1 | 0x4, 0};  // execution speed | ignore timer resolution; state 0 = do not throttle
    if (whole_process) {
        // ProcessPowerThrottling = 4
        SetProcessInformation(GetCurrentProcess(), static_cast<PROCESS_INFORMATION_CLASS>(4), &state, sizeof state);
    } else {
        // ThreadPowerThrottling = 3, execution speed only
        state.ControlMask = 0x1;
        SetThreadInformation(GetCurrentThread(), static_cast<THREAD_INFORMATION_CLASS>(3), &state, sizeof state);
    }
}

std::mutex frame_mutex;
std::vector<uint8_t> frame_bgr;  // SCREEN_W x SCREEN_H, 24-bit
std::atomic<bool> quit{false};
std::atomic<bool> dump_request{false};
std::atomic<bool> shot_request{false};  // F9: write build/shot.png and the object list (debugging aid)  // F10: write build/threads.txt (debugging aid)
std::atomic<int> held{0};
// The mouse as the Vita's touch screen (VM::touch): the left button and the cursor in client pixels.
std::atomic<int> mouse_down{0}, mouse_x{0}, mouse_y{0};
std::atomic<bool> mouse_cancelled{false};
std::atomic<bool> mouse_right_click{false};  // an edge, taken by the machine once per frame
std::atomic<int> mouse_wheel{0};              // wheel notches since the last frame, up positive
std::atomic<int> menu_held{0};
std::atomic<bool> menu_on_screen{false};      // the pointer sees a menu it plays: the cursor stays while the mouse rests
// Mouse mode (docs/technical/mouse-native-design.md 2.5): set by a real move, a click or the wheel, ended by a key or a
// pad button. While it is on, the arrow of a registered menu is not drawn (Scene::hide_uid).
std::atomic<bool> mouse_mode{false};

// The mouse cursor (docs/technical/input.md, "The mouse"): the port's arrow at the size of the picture, set by WM_SETCURSOR.
// Shown when the mouse moves or a button of it is used; hidden when a key or a pad button is pressed, and after three seconds
// of rest when no menu is on the screen. It is the system's cursor, so no capture of the picture holds it.
HCURSOR g_cursor = nullptr;
int g_cursor_size = 0;
bool g_cursor_shown = true;
DWORD g_cursor_moved = 0;
int g_cursor_last_x = -1, g_cursor_last_y = -1;
constexpr UINT_PTR CURSOR_TIMER = 7;
constexpr DWORD CURSOR_REST_MS = 3000;                // what the mouse presses in the menus this frame (menu_pointer.hpp)
HWND window = nullptr;
std::atomic<ULONGLONG> g_game_began{0};  // GetTickCount64 when the game started to load: a crash soon after is most often a damaged copy
std::atomic<VM*> g_vm{nullptr};  // the running machine, for the crash report
// The window's client size in real pixels, kept by the window's own thread (WM_SIZE). The machine's thread is not DPI aware, so
// its own GetClientRect answers in scaled units (1016 for a 1525 pixel window at 150 %) while the mouse arrives in real pixels.
std::atomic<int> g_client_w{960}, g_client_h{544};
int scale = 2;
bool scale_given = false;  // --scale on the command line: that many times the frame, whatever the screen
// The window's output: Direct3D 11 (docs/technical/gpu-renderer.md) unless `--renderer software` or no device.
std::unique_ptr<amg::D3DPresenter> gpu;
bool gpu_scene = true;  // the GPU draws the scene too (`--renderer present`: only presents the CPU's frame)

// The pads: read once per frame by amg::input (SDL3, input.hpp) into the engine's mask, with the sticks in the engine's
// scale (VM::analog). The keyboard is `held`, read by the window.
std::atomic<int> pad_held{0};
std::array<int, 4> pad_axes{};

// Borderless full screen on the monitor that holds the window; the frame is letterboxed in paint.
bool fullscreen = false;
WINDOWPLACEMENT windowed_place = {sizeof(WINDOWPLACEMENT)};

void toggle_fullscreen(HWND hwnd) {
    if (!fullscreen) {
        MONITORINFO mi = {sizeof mi};
        if (!GetWindowPlacement(hwnd, &windowed_place) ||
            !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        fullscreen = true;
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(hwnd, &windowed_place);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen = false;
    }
    InvalidateRect(hwnd, nullptr, TRUE);
}

// The Window mode row (and F11 or Alt+Enter) put the window in or out of full screen; the row's value follows.
constexpr UINT WM_AMG_WINDOW_MODE = WM_APP + 1;  // wParam: 1 full screen, 0 windowed
constexpr UINT WM_AMG_HIDE_CURSOR = WM_APP + 2;  // a pad button was pressed (the machine's thread posts it)
constexpr UINT WM_AMG_CURSOR_ART = WM_APP + 3;   // the game's arrow was read: the cursor is made again from it
void write_setting(const std::string& key, const std::string& value);

int key_mask(WPARAM key) { return amg::input::key_to_mask(int(key)); }
struct Viewport {
    int x, y, width, height;
};

std::atomic<int> g_aspect{0};  // the Screen shape row as the presenter has it: 0 fit, 1 fill, 2 stretch, 3 whole multiples, 4 fit over a blur

// Where the frame is in the window: the same arithmetic as the presenter's draw(), so a click lands on what it points at.
Viewport fitted_viewport(const RECT& rc) {
    int width = rc.right, height = rc.bottom;
    const int mode = g_aspect.load();
    if (mode == 2) {
        // stretch: the whole window
    } else if (mode == 1) {  // fill: the larger scale, the overflow is cropped (the frame is bigger than the window)
        if (width * SCREEN_H < height * SCREEN_W) width = height * SCREEN_W / SCREEN_H;
        else height = width * SCREEN_H / SCREEN_W;
    } else {
        if (width * SCREEN_H > height * SCREEN_W) width = height * SCREEN_W / SCREEN_H;
        else height = width * SCREEN_H / SCREEN_W;
        if (mode == 3 && width >= SCREEN_W && height >= SCREEN_H) {  // whole multiples of the frame
            const int k = std::min(width / SCREEN_W, height / SCREEN_H);
            width = k * SCREEN_W;
            height = k * SCREEN_H;
        }
    }
    return {(rc.right - width) / 2, (rc.bottom - height) / 2, width, height};
}

bool contains(const Viewport& view, int x, int y) {
    return x >= view.x && x < view.x + view.width && y >= view.y && y < view.y + view.height;
}

// Esc and the close button ask before closing (F5-10); the machine ending on its own (quit already set) does not.
bool confirm_quit(HWND hwnd) {
    static bool asking = false;  // the dialog pumps messages: a second Esc must not open a second one
    if (quit) return true;
    if (asking) return false;
    asking = true;
    held = 0;
    mouse_down = 0;
    bool yes = MessageBoxW(hwnd, ui::wtr("Quit the game?\nProgress that has not been saved is lost.").c_str(), L"Amagami",
                           MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    asking = false;
    return yes;
}

void open_game_settings(HWND hwnd);  // F8, defined below

bool pointer_over(HWND hwnd) {
    POINT at;
    return GetCursorPos(&at) && WindowFromPoint(at) == hwnd;
}

void show_cursor(HWND hwnd, bool on) {
    if (on) g_cursor_moved = GetTickCount();
    if (on == g_cursor_shown) return;
    g_cursor_shown = on;
    if (pointer_over(hwnd)) SetCursor(on ? g_cursor : nullptr);
}

// The arrow at the size of the picture in the window (cursor_art.hpp); made again when the window changes size.
void make_cursor(HWND hwnd, const Viewport& view) {
    const int size = amg::cursor_art::size_for(view.height);
    if (size == g_cursor_size && g_cursor) return;
    const amg::cursor_art::Art art_made = amg::cursor_art::make(size);
    const std::vector<uint8_t>& art = art_made.rgba;
    BITMAPV5HEADER bh = {};
    bh.bV5Size = sizeof bh;
    bh.bV5Width = size;
    bh.bV5Height = -size;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bh), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return;
    auto* out = static_cast<uint8_t*>(bits);
    for (size_t i = 0; i < size_t(size) * size; ++i) {
        out[i * 4 + 0] = art[i * 4 + 2];
        out[i * 4 + 1] = art[i * 4 + 1];
        out[i * 4 + 2] = art[i * 4 + 0];
        out[i * 4 + 3] = art[i * 4 + 3];
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = {FALSE, DWORD(art_made.hot_x), DWORD(art_made.hot_y), mask, color};
    HCURSOR made = HCURSOR(CreateIconIndirect(&ii));
    DeleteObject(color);
    DeleteObject(mask);
    if (!made) return;
    HCURSOR old = g_cursor;
    g_cursor = made;
    g_cursor_size = size;
    if (g_cursor_shown && pointer_over(hwnd)) SetCursor(g_cursor);
    if (old) DestroyCursor(old);
    (void)hwnd;
}

// A capture of the picture; with AMG_MOUSE_CURSOR_IN_SHOTS=1 and the cursor shown, the arrow where the mouse is.
void save_shot(VM& v, const std::string& file) {
    if (!amg::cursor_art::in_shots() || !g_cursor_shown) {
        v.graphics->save_png(file);
        return;
    }
    RECT rc = {0, 0, g_client_w.load(), g_client_h.load()};
    const Viewport view = fitted_viewport(rc);
    std::vector<uint8_t> rgb = v.graphics->render();
    if (view.width > 0 && view.height > 0)
        amg::cursor_art::stamp(rgb, SCREEN_W, SCREEN_H, (mouse_x - view.x) * SCREEN_W / view.width,
                               (mouse_y - view.y) * SCREEN_H / view.height, amg::cursor_art::size_for(SCREEN_H));
    Graphics::write_png(file, rgb, SCREEN_W, SCREEN_H);
}

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_CLOSE:
            if (confirm_quit(hwnd)) DestroyWindow(hwnd);
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (w == VK_ESCAPE) {
                if (!(l & (1 << 30)) && confirm_quit(hwnd)) DestroyWindow(hwnd);  // bit 30: auto-repeat
                return 0;
            }
#ifdef AMG_PERSONAL
            if ((w == VK_F6 || w == VK_F7) && !(l & (1 << 30))) {
                amg::personal::hotkey(unsigned(w));
                return 0;
            }
#endif
            if (w == VK_F8) {
                if (!(l & (1 << 30))) open_game_settings(hwnd);
                return 0;
            }
            if (w == VK_F9) {
                shot_request = true;
                return 0;
            }
            if (w == VK_F10) {
                dump_request = true;
                return 0;
            }
            if (w == VK_F11 || (w == VK_RETURN && (l & (1 << 29)))) {  // bit 29: Alt is down
                if (!(l & (1 << 30))) {  // bit 30: auto-repeat
                    toggle_fullscreen(hwnd);
                    write_setting("window", fullscreen ? "fullscreen" : "windowed");
                }
                return 0;
            }
            if (key_mask(w)) {
                amg::input::note_keyboard();
                mouse_mode = false;
                show_cursor(hwnd, false);  // the keyboard takes over: the cursor goes until the mouse moves
            }
            held |= key_mask(w);
            return 0;
        case WM_SYSCHAR:
            return 0;  // no beep for Alt+Enter
        case WM_AMG_WINDOW_MODE:
            if ((w != 0) != fullscreen) toggle_fullscreen(hwnd);
            return 0;
        case WM_AMG_HIDE_CURSOR:
            show_cursor(hwnd, false);
            return 0;
        case WM_AMG_CURSOR_ART: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            g_cursor_size = 0;  // made again, from the game's arrow
            if (rc.right > 0 && rc.bottom > 0) make_cursor(hwnd, fitted_viewport(rc));
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                SetCursor(g_cursor_shown ? (g_cursor ? g_cursor : LoadCursor(nullptr, IDC_ARROW)) : nullptr);
                return TRUE;
            }
            break;
        case WM_TIMER:
            if (w == CURSOR_TIMER && g_cursor_shown && !menu_on_screen.load() && GetTickCount() - g_cursor_moved > CURSOR_REST_MS &&
                mouse_down.load() == 0)
                show_cursor(hwnd, false);
            return 0;
        case WM_MOUSEACTIVATE:  // the click that brings the window to the front only does that: it is not a click in the game
            if (LOWORD(l) == HTCLIENT && GetForegroundWindow() != hwnd) return MA_ACTIVATEANDEAT;
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            held &= ~key_mask(w);
            return 0;
        case WM_LBUTTONDOWN:
            mouse_x = int(short(LOWORD(l)));
            mouse_y = int(short(HIWORD(l)));
            {
                RECT rc;
                GetClientRect(hwnd, &rc);
                if (!contains(fitted_viewport(rc), mouse_x, mouse_y)) return 0;
            }
            mouse_cancelled = false;
            SetCapture(hwnd);
            mouse_down = 1;
            mouse_mode = true;
            show_cursor(hwnd, true);
            return 0;
        case WM_MOUSEMOVE:
            mouse_x = int(short(LOWORD(l)));
            mouse_y = int(short(HIWORD(l)));
            // Windows also sends a move when nothing moved (a window over it went away): only a real one shows the cursor.
            if (mouse_x != g_cursor_last_x || mouse_y != g_cursor_last_y) {
                if (g_cursor_last_x >= 0) {
                    show_cursor(hwnd, true);
                    mouse_mode = true;
                }
                g_cursor_last_x = mouse_x;
                g_cursor_last_y = mouse_y;
            }
            return 0;
        case WM_RBUTTONDOWN:
            mouse_right_click = true;
            mouse_mode = true;
            show_cursor(hwnd, true);
            return 0;
        case WM_MOUSEWHEEL:
            mouse_wheel += GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
            mouse_mode = true;
            show_cursor(hwnd, true);
            return 0;
        case WM_LBUTTONUP:
            mouse_x = int(short(LOWORD(l)));
            mouse_y = int(short(HIWORD(l)));
            mouse_down = 0;
            ReleaseCapture();
            return 0;
        case WM_KILLFOCUS:
            held = 0;
            mouse_down = 0;
            mouse_right_click = false;
            mouse_wheel = 0;
            mouse_cancelled = true;  // the machine's thread also drops what the pointer was still to press
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED:
            if (mouse_down.exchange(0)) mouse_cancelled = true;
            return 0;
        case WM_SIZE:
            g_client_w = int(LOWORD(l));
            g_client_h = int(HIWORD(l));
            if (gpu) gpu->resize(int(LOWORD(l)), int(HIWORD(l)));
            if (LOWORD(l) > 0 && HIWORD(l) > 0) {
                RECT rc = {0, 0, int(LOWORD(l)), int(HIWORD(l))};
                make_cursor(hwnd, fitted_viewport(rc));
            }
            return 0;
        case WM_EXITSIZEMOVE:  // the player has moved or resized the window: it opens like that next time
            if (!fullscreen && !IsIconic(hwnd) && !IsZoomed(hwnd)) {
                RECT cr;
                POINT at = {0, 0};
                GetClientRect(hwnd, &cr);
                ClientToScreen(hwnd, &at);
                write_setting("ww", std::to_string(cr.right));
                write_setting("wh", std::to_string(cr.bottom));
                write_setting("wx", std::to_string(at.x));
                write_setting("wy", std::to_string(at.y));
            }
            return 0;
        case WM_DPICHANGED: {  // dragged to a screen with another scaling: Windows proposes the rectangle
            const RECT* r = reinterpret_cast<const RECT*>(l);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (gpu) {
                gpu->redraw();
                EndPaint(hwnd, &ps);
                return 0;
            }
            RECT rc;
            GetClientRect(hwnd, &rc);
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = SCREEN_W;
            bi.bmiHeader.biHeight = -SCREEN_H;  // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 24;
            bi.bmiHeader.biCompression = BI_RGB;
            std::vector<uint8_t> local;
            {
                std::lock_guard<std::mutex> lk(frame_mutex);
                local = frame_bgr;  // copy, then paint without holding the lock
            }
            // Largest picture with the game's aspect ratio, centred; the rest is black.
            const Viewport view = fitted_viewport(rc);
            int w_out = view.width, h_out = view.height, x0 = view.x, y0 = view.y;
            HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            RECT bars[4] = {{0, 0, rc.right, y0}, {0, y0 + h_out, rc.right, rc.bottom},
                            {0, y0, x0, y0 + h_out}, {x0 + w_out, y0, rc.right, y0 + h_out}};
            for (RECT& bar : bars)
                if (bar.right > bar.left && bar.bottom > bar.top) FillRect(dc, &bar, black);
            if (local.empty()) {
                RECT all = {x0, y0, x0 + w_out, y0 + h_out};
                FillRect(dc, &all, black);
            } else {
                SetStretchBltMode(dc, HALFTONE);  // smooth (bilinear-like) at every size
                SetBrushOrgEx(dc, 0, 0, nullptr);
                StretchDIBits(dc, x0, y0, w_out, h_out, 0, 0, SCREEN_W, SCREEN_H, local.data(), &bi,
                              DIB_RGB_COLORS, SRCCOPY);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            quit = true;
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, w, l);
    }
    return DefWindowProcW(hwnd, msg, w, l);  // a case that only breaks (the mouse code's early exits) must not fall off the end
}

// Sound output: WinMM waveOut with four small buffers refilled from the mixer.
// The sound is made by the machine, one frame's worth (AUDIO_RATE / 60 samples) per script frame, and queued here;
// the device thread only plays the queue. So the voices' and the music's progress, which the scripts read
// (Music.isBusy, the lip sync), follows the script frames exactly as in a headless run (`amagami_vm --audio`), and a
// recorded session replays the same. Before, the device thread rendered at the sound card's pace.
std::mutex g_sound_mutex;
std::deque<int16_t> g_sound;

// A slow frame (a picture loading, a font drawn for the first time: 60 to 100 ms) stops the machine, and with it the
// sound that is made per frame. The queue keeps a cushion so the device does not run dry meanwhile: `g_target` samples
// (two per frame) are kept queued, rendered a little ahead; each time the device does run dry the cushion grows by 30 ms,
// up to 150 ms, so a machine that stalls more gets more room, and one that never does keeps the sound close to the picture.
constexpr size_t CUSHION_MIN = size_t(AUDIO_RATE) * 2 * 50 / 1000, CUSHION_MAX = size_t(AUDIO_RATE) * 2 * 150 / 1000,
                 CUSHION_STEP = size_t(AUDIO_RATE) * 2 * 30 / 1000;
std::atomic<size_t> g_target{CUSHION_MIN};
std::atomic<bool> g_paused{false};  // the port's settings are open over the game: the machine waits
std::atomic<bool> g_hidden{false};  // the game runs behind the splash: it makes no sound yet
std::atomic<long long> g_borrowed{0};  // frames of sound made ahead during a long stretch of script, to be skipped later
std::atomic<long long> g_frame{0};  // the script frame, for the log of underruns
bool g_sound_started = false;  // under g_sound_mutex: the game has queued sound at least once

void queue_sound(const int16_t* samples, size_t count) {
    std::lock_guard<std::mutex> lk(g_sound_mutex);
    g_sound_started = true;
    g_sound.insert(g_sound.end(), samples, samples + count);
    const size_t limit = size_t(AUDIO_RATE / 4) * 2;  // a quarter of a second: drop the oldest beyond it
    while (g_sound.size() > limit) g_sound.pop_front();
}

size_t queued_sound() {
    std::lock_guard<std::mutex> lk(g_sound_mutex);
    return g_sound.size();
}

void take_sound(int16_t* out, size_t count) {
    std::lock_guard<std::mutex> lk(g_sound_mutex);
    size_t n = std::min(count, g_sound.size());
    std::copy(g_sound.begin(), g_sound.begin() + std::ptrdiff_t(n), out);
    g_sound.erase(g_sound.begin(), g_sound.begin() + std::ptrdiff_t(n));
    std::fill(out + n, out + count, int16_t(0));
    if (n < count && g_sound_started && !g_paused) {
        static const bool report = std::getenv("AMG_PROFILE") != nullptr;  // each time the device ran dry
        if (report) std::fprintf(stderr, "UNDERRUN %zu of %zu samples, cushion %zu at frame %lld\n", n, count, g_target.load(), g_frame.load());
        g_target = std::min(CUSHION_MAX, g_target.load() + CUSHION_STEP);
    }
}

void audio_thread(AudioEngine* engine) {
    (void)engine;
    opt_out_of_throttling(false);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);  // a gap in the sound is worse than a late frame
    WAVEFORMATEX fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = AUDIO_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = AUDIO_RATE * 4;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HWAVEOUT out = nullptr;
    if (waveOutOpen(&out, WAVE_MAPPER, &fmt, reinterpret_cast<DWORD_PTR>(event), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR)
        return;  // no sound device: play silently
    const int COUNT = 4, FRAMES = 1024;
    std::vector<std::vector<int16_t>> data(COUNT, std::vector<int16_t>(size_t(FRAMES) * 2));
    WAVEHDR hdr[COUNT] = {};
    for (int i = 0; i < COUNT; ++i) {
        hdr[i].lpData = reinterpret_cast<LPSTR>(data[size_t(i)].data());
        hdr[i].dwBufferLength = FRAMES * 4;
        waveOutPrepareHeader(out, &hdr[i], sizeof(WAVEHDR));
        take_sound(data[size_t(i)].data(), size_t(FRAMES) * 2);
        waveOutWrite(out, &hdr[i], sizeof(WAVEHDR));
    }
    while (!quit) {
        WaitForSingleObject(event, 50);
        for (int i = 0; i < COUNT; ++i) {
            if (hdr[i].dwFlags & WHDR_DONE) {
                take_sound(data[size_t(i)].data(), size_t(FRAMES) * 2);
                hdr[i].dwFlags &= ~DWORD(WHDR_DONE);
                waveOutWrite(out, &hdr[i], sizeof(WAVEHDR));
            }
        }
    }
    waveOutReset(out);
    for (int i = 0; i < COUNT; ++i) waveOutUnprepareHeader(out, &hdr[i], sizeof(WAVEHDR));
    waveOutClose(out);
}

// What the machine hands to the drawing thread: the newest snapshot of the screen. If the drawing
// thread is behind, older snapshots are simply replaced, so a slow frame costs a frame on screen and
// never slows the game down.
std::mutex scene_mutex;
std::condition_variable scene_ready;
std::shared_ptr<const Scene> pending_scene;
std::atomic<bool> smooth_drawing{true};
std::atomic<long long> latest_tick{0};
// Test aids (docs/technical/port.md): AMG_PRESSES="TICK:MASK:FRAMES;..." presses buttons by script, as --press does for the
// command line tool. The mask is the PS2 layout of the scenario scripts (start 2048, circle 32, down 16384, R1 8).
std::vector<std::tuple<long long, long long, int>> script_presses;
int scripted_buttons() {
    int mask = 0;
    const long long t = latest_tick.load();
    for (auto& p : script_presses)
        if (t >= std::get<0>(p) && t <= std::get<1>(p)) mask |= std::get<2>(p);
    return mask;
}
std::atomic<double> sim_fps{0.0}, sim_worst_ms{0.0};
std::atomic<bool> g_show_fps{false};  // the Frame counter row: the frames shown per second, in the corner

// The counter is drawn as little squares (a 3x5 font of the digits and the letters of "FPS"), so it needs no texture.
void append_fps_counter(Scene& scene, double fps) {
    static const char* const GLYPHS[][5] = {
        {"111", "101", "101", "101", "111"}, {"010", "110", "010", "010", "111"}, {"111", "001", "111", "100", "111"},
        {"111", "001", "111", "001", "111"}, {"101", "101", "111", "001", "001"}, {"111", "100", "111", "001", "111"},
        {"111", "100", "111", "101", "111"}, {"111", "001", "010", "010", "010"}, {"111", "101", "111", "101", "111"},
        {"111", "101", "111", "001", "111"}, {"111", "100", "110", "100", "100"}, {"111", "101", "111", "100", "100"},
        {"111", "100", "111", "001", "111"}, {"000", "000", "000", "000", "010"}};  // 0-9, F, P, S, the point
    char text[16];
    std::snprintf(text, sizeof text, "%.0f FPS", fps);
    const double pixel = 3.0, gap = pixel;
    double x = -SCREEN_W / 2.0 + 14.0;
    const double y0 = -SCREEN_H / 2.0 + 12.0;
    auto square = [&](double px, double py, double alpha) {
        DrawItem d;
        d.layer = 1000;
        d.order = INT64_MAX;
        d.rect[2] = d.rect[3] = pixel;
        d.centre[0] = px + pixel / 2;
        d.centre[1] = py + pixel / 2;
        d.alpha = alpha;
        d.light[0] = d.light[1] = d.light[2] = alpha > 0.9 ? 1.0 : 0.0;
        d.ui = true;
        scene.items.push_back(d);
    };
    for (const char* ch = text; *ch; ++ch) {
        int g = *ch >= '0' && *ch <= '9' ? *ch - '0' : *ch == 'F' ? 10 : *ch == 'P' ? 11 : *ch == 'S' ? 12 : -1;
        if (g >= 0) {
            for (int row = 0; row < 5; ++row)
                for (int col = 0; col < 3; ++col) {
                    if (GLYPHS[g][row][col] != '1') continue;
                    square(x + col * pixel + 1, y0 + row * pixel + 1, 0.55);  // a shadow, so it reads on a white picture
                    square(x + col * pixel, y0 + row * pixel, 1.0);
                }
        }
        x += 3 * pixel + gap;
    }
}

void draw_thread() {
    opt_out_of_throttling(false);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    using clock = std::chrono::steady_clock;
    std::vector<uint8_t> back;
    int shown = 0;
    double draw_total = 0.0;
    auto window_start = clock::now();
    while (!quit) {
        std::shared_ptr<const Scene> scene;
        {
            std::unique_lock<std::mutex> lk(scene_mutex);
            scene_ready.wait_for(lk, std::chrono::milliseconds(100), [] { return pending_scene != nullptr || quit; });
            scene.swap(pending_scene);
        }
        if (!scene || IsIconic(window)) continue;  // nothing new, or nothing to show while minimized
        auto t0 = clock::now();
        if (gpu && gpu_scene) {
            Scene viewed = Graphics::view_scene(*scene);
            draw_total += std::chrono::duration<double, std::milli>(clock::now() - t0).count();
            if (g_show_fps) {  // frames shown in the last second
                static auto counted_from = clock::now();
                static int counted = 0;
                static double last_fps = 0.0;
                ++counted;
                const double span = std::chrono::duration<double>(clock::now() - counted_from).count();
                if (span >= 1.0) {
                    last_fps = counted / span;
                    counted = 0;
                    counted_from = clock::now();
                }
                append_fps_counter(viewed, last_fps);
            }
            gpu->present_scene(viewed);
            {  // AMG_GPU_SHOT="TICK:FILE": the scene as the GPU drew it, at the render scale (test aid)
                static const char* spec = std::getenv("AMG_GPU_SHOT");
                static bool done = false;
                if (spec && !done) {
                    const std::string s = spec;
                    const size_t colon = s.find(':');
                    if (colon != std::string::npos && latest_tick.load() >= std::atoll(s.substr(0, colon).c_str())) {
                        std::vector<uint8_t> px;
                        int w = 0, h = 0;
                        if (gpu->read_scene_scaled(px, w, h)) {
                            Graphics::write_png(s.substr(colon + 1), px, w, h);
                            done = true;
                        }
                    }
                }
            }
            // AMG_GPU_COMPARE=1: every 120th scene is also drawn by the CPU and the two pictures are compared (stderr).
            static const bool compare = std::getenv("AMG_GPU_COMPARE") != nullptr;
            static long compared = 0;
            if (compare && ++compared % 120 == 0) {
                std::vector<uint8_t> a = Graphics::render_scene(*scene, true), b;
                if (gpu->read_scene(b) && a.size() == b.size()) {
                    long long sum = 0;
                    int worst = 0;
                    size_t over = 0;
                    for (size_t i = 0; i < a.size(); ++i) {
                        int d = std::abs(int(a[i]) - int(b[i]));
                        sum += d;
                        worst = std::max(worst, d);
                        if (d > 8) ++over;
                    }
                    std::fprintf(stderr, "GPUCMP items=%zu mean=%.3f max=%d over8=%.3f%% uploaded=%zuKB%c", scene->items.size(),
                                 double(sum) / double(a.size()), worst, 100.0 * double(over) / double(a.size()),
                                 gpu->take_uploaded_bytes() / 1024, 10);
                }
            }
            if (++shown == 60) {
                auto t = clock::now();
                double shown_fps = 60.0 / std::chrono::duration<double>(t - window_start).count();
                window_start = t;
                char title[260];
                std::snprintf(title, sizeof title,
                              "Amagami (open-amagami port) - game %.1f fps, shown %.1f fps, gpu scene, input: %s, frame %lld",
                              sim_fps.load(), shown_fps, amg::input::describe().c_str(), latest_tick.load());
                if (show_stats()) SetWindowTextA(window, title);
                draw_total = 0.0;
                shown = 0;
            }
            continue;
        }
        std::vector<uint8_t> rgb = Graphics::render_scene(*scene, smooth_drawing);
        back.resize(rgb.size());
        for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
            back[i] = rgb[i + 2];
            back[i + 1] = rgb[i + 1];
            back[i + 2] = rgb[i];
        }
        draw_total += std::chrono::duration<double, std::milli>(clock::now() - t0).count();  // the drawing, not the wait for vsync
        if (gpu) gpu->present(back.data());
        {
            std::lock_guard<std::mutex> lk(frame_mutex);
            frame_bgr.swap(back);
        }
        if (!gpu) InvalidateRect(window, nullptr, FALSE);
        if (++shown == 60) {  // the rates and the cost of drawing, in the title bar
            auto t = clock::now();
            double shown_fps = 60.0 / std::chrono::duration<double>(t - window_start).count();
            window_start = t;
            char title[160];
            std::snprintf(title, sizeof title,
                          "Amagami (open-amagami port) - game %.1f fps, shown %.1f fps, draw %.1f ms, worst %.1f ms, frame %lld",
                          sim_fps.load(), shown_fps, draw_total / 60.0, sim_worst_ms.load(), latest_tick.load());
            sim_worst_ms = 0.0;
            draw_total = 0.0;
            shown = 0;
            if (show_stats()) SetWindowTextA(window, title);
        }
    }
}

static std::string g_save_dir;

// The pictures to decode ahead at the start: one name per line in warm.txt, beside config.ini.
static std::vector<std::string> read_warm_list(const std::string& path) {
    std::vector<std::string> names;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == 13 || line.back() == ' ')) line.pop_back();
        if (!line.empty() && names.size() < 12) names.push_back(line);
    }
    return names;
}

static void write_warm_list(const std::string& path, const std::vector<std::string>& names) {
    std::ofstream out(path, std::ios::trunc);
    for (const auto& name : names) out << name << char(10);
}

std::string read_setting(const std::string& key);  // config.txt, defined below
void write_setting(const std::string& key, const std::string& value);

// The window's scaling filter from config.txt (AMG_FILTER wins at start): smooth, sharp (sharp bilinear), pixels.
void apply_filter() {
    {  // the interface size works on both back ends
        const char* e = std::getenv("AMG_UI_SCALE");
        int percent = std::atoi((e ? std::string(e) : read_setting("uiscale")).c_str());
        UI_SCALE = percent >= 40 && percent <= 100 ? percent / 100.0 : 1.0;
        if (const char* l = std::getenv("AMG_UI_LAYER")) UI_FIRST_LAYER = std::atoi(l);
    }
    // The window mode works on both back ends; it is done by the window's own thread.
    if (window && (read_setting("window") == "fullscreen") != fullscreen)
        PostMessageW(window, WM_AMG_WINDOW_MODE, read_setting("window") == "fullscreen" ? 1 : 0, 0);
    if (!gpu) return;
    std::string f = read_setting("filter");
    if (const char* e = std::getenv("AMG_FILTER")) f = e;
    gpu->set_filter(f == "sharp" ? 1 : f == "pixels" ? 2 : f == "bicubic" ? 3 : f == "lanczos" ? 4 : f == "sharpen" ? 5 : 0);
    gpu->set_vsync(read_setting("vsync") != "off");
    g_show_fps = read_setting("fps") == "on";
    {
        const double b = std::atof(read_setting("brightness").c_str()), g = std::atof(read_setting("gamma").c_str());
        gpu->set_tone(b >= 50 && b <= 200 ? float(b / 100.0) : 1.0f, g >= 0.5 && g <= 2.0 ? float(g) : 1.0f);
    }
    auto value = [](const char* env, const char* key) {
        const char* e = std::getenv(env);
        return e ? std::string(e) : read_setting(key);
    };
    std::string a = value("AMG_ASPECT", "aspect"), r = value("AMG_RENDER", "render");
    const int aspect_mode = a == "fill" ? 1 : a == "stretch" ? 2 : a == "whole" ? 3 : a == "blur" ? 4 : 0;
    g_aspect = aspect_mode;
    gpu->set_aspect(aspect_mode);
    gpu->set_render_scale(r.size() == 1 && r[0] >= '1' && r[0] <= '4' ? r[0] - '0' : 0);
}

// The splash picture (our own art, made by `hokago make_splash`): `art/splash.png` beside the program, else the source's.
std::string find_splash() {
    if (const char* e = std::getenv("AMG_SPLASH")) return e;
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    dir = dir.substr(0, dir.find_last_of("\\/"));
    for (const char* rel : {"/art/splash.png", "/../../port/art/splash.png"}) {
        std::string candidate = dir + rel;
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return "";
}

// Where the port's own script classes are: `script/` beside the program, else the source tree's.
std::string find_scripts() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    dir = dir.substr(0, dir.find_last_of(std::string("/") + char(92)));
    for (const char* rel : {"/script", "/../../port/script"}) {
        std::string candidate = dir + rel;
        if (std::filesystem::is_directory(candidate)) return candidate;
    }
    return "";
}

// The input settings from config.txt (a variable of the environment wins at start, config.txt rules once the player changes one).
amg::input::Settings input_settings_from(const std::function<std::string(const char*)>& get) {
    amg::input::Settings in;
    std::string confirm = get("confirm"), fallback = get("fallback"), glyphs = get("glyphs");
    in.confirm = confirm == "south" ? 0 : confirm == "east" ? 1 : -1;
    in.fallback = fallback == "xbox" ? amg::input::Family::Xbox : amg::input::Family::PlayStation;
    in.forced = glyphs == "keyboard" ? 0 : glyphs == "xbox" ? 1 : glyphs == "playstation" ? 2 : -1;
    return in;
}

// Where the button pictures are: AMG_SKINS, else `skins/` beside the program, else the source tree's (a build in build/win-x86).
std::string find_skins() {
    if (const char* e = std::getenv("AMG_SKINS")) return e;
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    dir = dir.substr(0, dir.find_last_of("\\/"));
    for (const char* rel : {"/skins", "/../../port/skins"}) {
        std::string candidate = dir + rel;
        if (std::filesystem::is_directory(candidate)) return candidate;
    }
    return "";
}

// The picture for the game's button glyph in `slot` (Graphics::glyph_picker): the one of the controller in use.
amg::Skins g_skins;

ImagePtr pick_glyph(int slot, double centre_x) {
    using amg::input::Family;
    const Family family = amg::input::family();
    // The game's action behind each slot; slot 8 is Start (right) or Select (left), told apart by where the sprite is.
    static const int BIT[] = {0, 64, 32, 16, 128, 0, 4, 8, 0};
    const int bit = slot == 8 ? (centre_x < 0 ? 256 : 2048) : BIT[slot];
    if (family == Family::Keyboard) {  // a key with its label; the arrow keys' picture for the d-pad
        if (slot == 5) return g_skins.get("ps", "dpad_down");
        amg::input::KeyLabel k = amg::input::key_label(bit);
        return g_skins.keycap(k.text, k.symbol, slot == 6 || slot == 7);  // the game's L and R are the long sprites
    }
    std::string name = slot == 5 ? "dpad_down" : amg::input::picture_of(bit, family);
    if (slot == 8 && family != Family::Xbox) name += "_small";  // PlayStation's Options and Share are pills: the compact ones
    if (name.empty()) return nullptr;
    return g_skins.get(family == Family::Xbox ? "xbox" : "ps", name);
}

void machine_thread(std::string root) {
    g_game_began = GetTickCount64();
    opt_out_of_throttling(false);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    try {
        // The splash screen comes first: on screen at once, while the data loads, and kept until its time is up.
        using splash_clock = std::chrono::steady_clock;
        const auto splash_start = splash_clock::now();
        ImagePtr splash;
        if (!std::getenv("AMG_NO_SPLASH") && read_setting("splash") != "off") splash = amg::load_picture(find_splash());
        auto show_splash = [&](double alpha) {
            auto scene = std::make_shared<Scene>();
            DrawItem item;
            item.image = splash;
            item.rect[2] = splash->w;
            item.rect[3] = splash->h;
            item.alpha = alpha;
            scene->items.push_back(item);
            {
                std::lock_guard<std::mutex> lk(scene_mutex);
                pending_scene = std::move(scene);
            }
            scene_ready.notify_one();
        };
        if (splash) show_splash(1.0);
        smooth_drawing = !std::getenv("AMG_NO_SMOOTH");  // switches for finding what costs time
        std::thread(draw_thread).detach();  // started here so that the splash is on screen while the data loads
        VM::script_dir = find_scripts();
        if (std::getenv("AMG_TEST_STARTFAIL")) throw std::runtime_error("test: the game's data could not be read");  // to see the startup failure's window
        VM vm(root);
        g_vm = &vm;
        struct ForgetMachine {  // the crash report reads the machine: it must not find it after it is gone
            ~ForgetMachine() { g_vm = nullptr; }
        } forget_machine;
        vm.save_dir = g_save_dir;
        vm.max_steps = INT64_MAX;
        vm.profile_prims = std::getenv("AMG_PROFILE_PRIMS") != nullptr;
        if (vm.profile_prims) std::freopen("build/slowprim.log", "w", stderr);
        install_graphics(vm);
        if (size_t n = g_skins.load(find_skins()); n > 0) vm.graphics->glyph_picker = pick_glyph;
        // The backgrounds the game waited for last time (and the first ones of the title, measured on a Japanese copy)
        // are decoded now, one after the other, while the splash is up: the menus then find them ready.
        const std::string warm_path = std::filesystem::path(amg::paths::config_file()).parent_path().string() + "/warm.txt";
        std::vector<std::string> warm_names = read_warm_list(warm_path);
        for (const char* seed : {"bg/139_11", "bg/139_20", "bg/130_10"})
            if (std::find(warm_names.begin(), warm_names.end(), seed) == warm_names.end()) warm_names.push_back(seed);
        if (!std::getenv("AMG_NO_WARM")) vm.graphics->warm(warm_names);  // AMG_NO_WARM: for timing experiments
        // What the game waits for goes first in the list for the next start (at most 12 names); the program may end
        // at any moment, so the file is written when a name is added.
        vm.graphics->on_missed = [warm_path, warm_names](const std::vector<std::string>& missed) {
            std::vector<std::string> keep = missed;
            for (const auto& name : warm_names)
                if (std::find(keep.begin(), keep.end(), name) == keep.end()) keep.push_back(name);
            if (keep.size() > 12) keep.resize(12);
            write_warm_list(warm_path, keep);
        };
        AudioEngine audio(root);
        audio.set_async_tracks(true);  // a track loads off the game's thread: no stall, no gap in the sound
        audio.warm_voices();
        install_audio(vm, &audio);
        // One frame of sound per script frame, called from the frame hook below (which replaces vm.on_tick).
        static const bool silent = std::getenv("AMG_NO_AUDIO") != nullptr;
        auto sound_frame = [&audio]() {
            int16_t frame[size_t(AUDIO_RATE / 60) * 2];
            // Sound made ahead during a long stretch of script is paid back here, so the sound stays with the picture.
            if (g_borrowed > 0 && queued_sound() >= g_target) {
                --g_borrowed;
                return;
            }
            audio.render(frame, size_t(AUDIO_RATE / 60));
            if (silent) return;
            queue_sound(frame, size_t(AUDIO_RATE / 60) * 2);
            // Below the cushion (after a stall, or at the start): make a few frames more now, never more than four a frame.
            for (int extra = 0; extra < 4 && queued_sound() < g_target; ++extra) {
                audio.render(frame, size_t(AUDIO_RATE / 60));
                queue_sound(frame, size_t(AUDIO_RATE / 60) * 2);
            }
        };
        if (!std::getenv("AMG_NO_AUDIO")) {
            // A script can run for half a second without a frame (a table built at the start): the sound goes on meanwhile.
            vm.on_busy = [&audio](VM&) {
                int16_t frame[size_t(AUDIO_RATE / 60) * 2];
                if (g_hidden || queued_sound() >= CUSHION_MAX) return;
                audio.render(frame, size_t(AUDIO_RATE / 60));
                queue_sound(frame, size_t(AUDIO_RATE / 60) * 2);
                ++g_borrowed;
            };
            std::thread(audio_thread, &audio).detach();
            vm.graphics->mouth_level = [&audio](int speaker) { return audio.mouth_level(speaker); };
        }
        {  // the pads, through SDL. Each setting: the environment variable, else config.txt (docs/technical/input.md)
            auto setting = [](const char* env, const char* key) -> std::string {
                if (const char* e = std::getenv(env)) return e;
                return read_setting(key);
            };
            amg::input::Settings in = input_settings_from([&](const char* key) {
                return setting(key == std::string("confirm") ? "AMG_CONFIRM" : key == std::string("fallback") ? "AMG_FALLBACK" : "AMG_GLYPHS", key);
            });
            const char* fake = std::getenv("AMG_FAKE_PAD");
            amg::input::init(in, fake ? fake : "");
        }
        amg::MenuPointer pointer;
        amg::MouseMenus menus;
        std::shared_ptr<const Scene> last_scene;
#ifdef AMG_PERSONAL
        {  // the text window (docs/technical/personal.md): personal build only, off unless AMG_TEXT_WINDOW=1 or textwin=on
            amg::personal::Hooks hooks;
            hooks.read_setting = [](const std::string& key) { return read_setting(key); };
            hooks.write_setting = [](const std::string& key, const std::string& value) { write_setting(key, value); };
            if (amg::personal::wanted(hooks)) amg::personal::start(hooks);
        }
#endif
        bool mouse_was_down = false;
        vm.on_rumble = [](double strength, bool on) {
            if (on) amg::input::rumble(strength, 250);
        };
        vm.live_buttons = [] { return held.load() | pad_held.load() | menu_held.load() | scripted_buttons(); };
        {  // AMG_PRESSES and AMG_SHOTS="TICK:FILE;..." (the screen as the software renderer draws it at that frame)
            auto each = [](const char* env, auto&& use) {
                const char* e = std::getenv(env);
                if (!e) return;
                std::string all = e;
                size_t from = 0;
                while (from < all.size()) {
                    size_t semi = all.find(';', from);
                    if (semi == std::string::npos) semi = all.size();
                    use(all.substr(from, semi - from));
                    from = semi + 1;
                }
            };
            each("AMG_PRESSES", [](const std::string& s) {
                long long tick = 0, frames = 3;
                int mask = 0;
                if (std::sscanf(s.c_str(), "%lld:%i:%lld", &tick, &mask, &frames) >= 2) script_presses.emplace_back(tick, tick + frames - 1, mask);
            });
            each("AMG_SHOTS", [&vm](const std::string& s) {
                size_t colon = s.find(':');
                if (colon != std::string::npos) vm.shots[std::atoll(s.substr(0, colon).c_str())] = s.substr(colon + 1);
            });
            vm.graphics->smooth = smooth_drawing;  // the shots are drawn as the window draws them: bilinear, not nearest
            vm.on_shot = [](VM& v, const std::string& file) { save_shot(v, file); };
        }

        using clock = std::chrono::steady_clock;
        auto next = clock::now();
        const auto period = std::chrono::microseconds(16667);  // 60 Hz
        static const bool profiling = std::getenv("AMG_PROFILE") != nullptr;
        std::vector<std::array<float, 3>> profile;  // AMG_PROFILE: interval, frame, instructions
        int64_t last_steps = 0;
        auto prev = clock::now();
        int frames = 0;
        double worst = 0.0;
        auto rate_start = clock::now();
        // The splash stays up at least four seconds (any button or click ends it sooner). The game's first frames run
        // behind it, unseen and unpaced, up to the long stretch of script right after them (about half a second, the
        // tables built at the start), so the picture does not freeze when the game appears.
        const int HIDDEN_TICKS = 31;
        bool splash_over = !splash;
        g_hidden = !splash_over;
        auto wait_splash = [&]() {
            const double hold = 4.0, fade = 0.5;
            double skipped_at = -1.0;
            for (;;) {
                const double t = std::chrono::duration<double>(splash_clock::now() - splash_start).count();
                std::array<int, 4> axes{};
                const bool pressed = (held.load() | amg::input::poll_pads(axes) | menu_held.load()) != 0 || mouse_down.load() != 0;
                if (skipped_at < 0 && t > 0.8 && pressed) skipped_at = t;
                const double end = skipped_at >= 0 ? std::min(hold, skipped_at + 0.3) : hold;
                if (quit || t >= end) break;
                const double left = end - t;
                show_splash(std::min(1.0, left / (skipped_at >= 0 ? 0.3 : fade)));
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        };
        vm.on_tick = [&](VM& v) {
            if (g_paused) {  // the port's settings are open: the game waits, and goes on from here
                while (g_paused && !quit) std::this_thread::sleep_for(std::chrono::milliseconds(16));
                next = clock::now();
                prev = next;
            }
            if (!splash_over) {
                if (quit) {
                    v.max_steps = 0;
                    return;
                }
                if (v.tick < HIDDEN_TICKS) {  // nothing drawn, no sound queued, no waiting: only the machine runs
                    v.graphics->step(1.0);
                    int16_t frame[size_t(AUDIO_RATE / 60) * 2];
                    audio.render(frame, size_t(AUDIO_RATE / 60));
                    return;
                }
                wait_splash();
                splash_over = true;
                g_hidden = false;
                next = clock::now();
                prev = next;
            }
            const auto seg0 = clock::now();
            v.graphics->step(1.0);
            const auto seg1 = clock::now();
            sound_frame();
            const auto seg2 = clock::now();
            if (shot_request.exchange(false)) {
                save_shot(v, "build/shot.png");
                if (std::FILE* f = std::fopen("build/shot.png.txt", "w")) {
                    std::string d = v.graphics->describe();
                    std::fwrite(d.data(), 1, d.size(), f);
                    std::fclose(f);
                }
            }
            if (dump_request.exchange(false)) {
                if (std::FILE* f = std::fopen("build/threads.txt", "w")) {
                    std::fprintf(f, "tick %lld\n", (long long)v.tick);
                    size_t from = v.out.size() > 40 ? v.out.size() - 40 : 0;
                    for (size_t i = from; i < v.out.size(); ++i) std::fprintf(f, "out: %s\n", v.out[i].c_str());
                    for (auto& th : v.threads) {
                        if (th->frames.empty()) continue;
                        std::fprintf(f, "thread %s (wake at %lld):\n", th->label.c_str(), (long long)th->wake_tick);
                        size_t shown = 0;
                        for (size_t i = th->frames.size(); i-- > 0 && shown < 12; ++shown)
                            std::fprintf(f, "    at %s.%s (instruction %d)\n", th->frames[i].method->cls->name.c_str(),
                                         th->frames[i].method->name.c_str(), th->frames[i].pc);
                    }
                    std::fclose(f);
                }
            }
            const auto seg3 = clock::now();
            pad_held = amg::input::poll_pads(pad_axes);  // once per frame, as the console reads the pad once per vblank
            const auto seg4 = clock::now();
            {  // the mouse as the touch screen: client pixels to the scripts' units, through the letterboxed picture
                RECT rc = {0, 0, g_client_w.load(), g_client_h.load()};
                const Viewport view = fitted_viewport(rc);
                {  // a key or a pad button takes over from the mouse: what the pointer still had to press is dropped
                    static int last_others = 0;
                    const int others = held.load() | pad_held.load();
                    if (others & ~last_others) {
                        pointer.yield();
                        menus.yield();
                        mouse_mode = false;
                        if (pad_held.load() & ~last_others) PostMessageW(window, WM_AMG_HIDE_CURSOR, 0, 0);
                    }
                    last_others = others;
                }
                if (mouse_cancelled.exchange(false) || view.width <= 0 || view.height <= 0) {
                    v.touch.cancel();
                    pointer.yield();
                    menus.yield();
                    v.graphics->hide_uid = 0;
                    v.graphics->hide_image = nullptr;
                    menu_held = 0;
                    mouse_was_down = false;
                } else {
                    // Window pixels to picture pixels (the only inverse: the letterbox), then script units of the picture
                    // as the player sees it. The pointer compares with the scene after the view transform and the
                    // interface shrink (Graphics::pointer_scene), so the hit test is where the entries are drawn.
                    double fx = (mouse_x - view.x) / double(view.width);
                    double fy = (mouse_y - view.y) / double(view.height);
                    double units_w = SCREEN_W / VIEW_UNIT, units_h = SCREEN_H / VIEW_UNIT;
                    const double mxu = (fx - 0.5) * units_w, myu = (fy - 0.5) * units_h;
                    amg::Scene seen;
                    if (last_scene) seen = Graphics::pointer_scene(*last_scene);
                    // The scripts' own touch tests (the gallery) are in the layout's units: back through the shrink.
                    const double tu = seen.ui_u > 0 ? seen.ui_u : 1.0;
                    v.touch.update(mouse_down.load() != 0, seen.ui_ax + (mxu - seen.ui_ax) / tu, seen.ui_ay + (myu - seen.ui_ay) / tu, v.tick);
                    // The menus: the mouse plays them from the picture (the scripts only ask for touch in the gallery).
                    const bool down = mouse_down.load() != 0;
                    const bool click = down && !mouse_was_down;
                    mouse_was_down = down;
                    const bool inside = contains(view, mouse_x, mouse_y);
                    if (last_scene) {
                        amg::MenuPictures pics;
                        pics.cursor = v.graphics->ui_image(20, 0);
                        if (pics.cursor && amg::cursor_art::set_source(pics.cursor->w, pics.cursor->h, pics.cursor->px.data()))
                            PostMessageW(window, WM_AMG_CURSOR_ART, 0, 0);  // the pointer becomes the game's arrow
                        pics.tab = v.graphics->ui_image(47, 0);
                        pics.note = v.graphics->ui_image(33, 0);
                        pics.back = v.graphics->ui_image(62, 2);
                        pics.arrow = v.graphics->ui_image(7, 0);
                        pics.map_cursor = v.graphics->ui_image(25, 0);
                        for (int k = 0; k < 4; ++k) pics.map_arrow[k] = v.graphics->ui_image(28 + k, 0);
                        const bool right = mouse_right_click.exchange(false) && inside;
                        const int wheel = mouse_wheel.exchange(0);
                        int native_mask = 0;
                        if (menus.update(v, seen, mxu, myu, inside, down, click && inside, right, wheel, native_mask)) {
                            pointer.yield();  // a registered menu (mouse_menus.hpp): one direct move, no bursts
                            menu_held = native_mask;
                            menu_on_screen = true;
                            v.graphics->hide_uid = mouse_mode.load() ? menus.arrow_uid : 0;
                            v.graphics->mark = mouse_mode.load() ? menus.mark : amg::Graphics::SelectionMark();
                            v.graphics->hide_image = mouse_mode.load() ? pics.cursor : nullptr;
                        } else {
                            v.graphics->hide_uid = 0;
                            v.graphics->hide_image = nullptr;
                            menu_held = pointer.update(seen, pics, mxu, myu, inside, down, click && inside, right, wheel);
                            menu_on_screen = pointer.probe.kind != '-';
                        }
                    }
                }
            }
            for (int k = 0; k < 4; ++k) v.analog[k] = pad_axes[size_t(k)];
            {  // AMG_INPUT_LOG: one line per change of the buttons, for replaying a session with --press
                static const bool input_log = std::getenv("AMG_INPUT_LOG") != nullptr;
                static int last_mask = 0;
                int mask = held.load() | pad_held.load();
                if (input_log && mask != last_mask) {
                    std::fprintf(stderr, "input %lld %d\n", (long long)v.tick, mask);
                    std::fflush(stderr);
                }
                last_mask = mask;
            }
            if (quit) {
                v.max_steps = 0;  // every script thread ends at its next instruction
                return;
            }
            auto t0 = clock::now();
            double interval_ms = std::chrono::duration<double, std::milli>(t0 - prev).count();
            prev = t0;
            worst = std::max(worst, interval_ms);
            // Publish a snapshot of the screen for the drawing thread, replacing any it has not taken.
            {
                std::shared_ptr<const Scene> scene = v.graphics->scene();
                last_scene = scene;  // the mouse reads it in the next frame (menu_pointer.hpp)
#ifdef AMG_PERSONAL
                amg::personal::feed(*scene, v.tick);
#endif
                std::lock_guard<std::mutex> lk(scene_mutex);
                pending_scene = std::move(scene);
            }
            scene_ready.notify_one();
            latest_tick = v.tick;
            g_frame = v.tick;
            if (profiling) {
                auto ms = [](clock::time_point x, clock::time_point y) { return std::chrono::duration<double, std::milli>(y - x).count(); };
                if (interval_ms > 30)  // where a long frame went: step, sound, shots, pads, scene (the rest of it ran in the scripts)
                    std::fprintf(stderr, "SLOWFRAME %lld %.1f ms: step %.1f sound %.1f pads %.1f total-in-tick %.1f\n", (long long)v.tick,
                                 interval_ms, ms(seg0, seg1), ms(seg1, seg2), ms(seg3, seg4), ms(seg0, clock::now()));
                if (v.profile_prims) {  // HOTFRAME: where a frame of many instructions went (the counts restart every frame)
                    const bool hot = v.steps - last_steps > 100000;
                    std::vector<std::pair<uint64_t, std::string>> rows;
                    for (auto& [cname, cls] : v.classes)
                        for (auto* table : {&cls->imethods, &cls->cmethods})
                            for (auto& [key, callable] : *table)
                                if (Method* m = callable.method.get(); m && m->cls == cls.get() && m->executed) {
                                    if (hot) rows.push_back({m->executed, cname + (table == &cls->cmethods ? "." : "#") + m->name});
                                    m->executed = 0;
                                }
                    std::sort(rows.rbegin(), rows.rend());
                    for (size_t i = 0; i < rows.size() && i < 6; ++i)
                        std::fprintf(stderr, "HOTFRAME %lld %llu %s\n", (long long)v.tick, (unsigned long long)rows[i].first, rows[i].second.c_str());
                }
                profile.push_back({float(interval_ms), float(v.tick), float(v.steps - last_steps)});
                last_steps = v.steps;
                if (profile.size() % 600 == 0) {
                    std::FILE* f = std::fopen("build/profile.csv", "w");
                    if (f) {
                        for (auto& q : profile) std::fprintf(f, "%d,%.2f,0,0,0,%d\n", int(q[1]), q[0], int(q[2]));
                        std::fclose(f);
                    }
                }
            }
            if (++frames == 60) {
                auto t = clock::now();
                sim_fps = 60.0 / std::chrono::duration<double>(t - rate_start).count();
                sim_worst_ms = std::max(sim_worst_ms.load(), worst);
                rate_start = t;
                frames = 0;
                worst = 0.0;
            }
            // Turbo for testing: while Tab is held (window focused) the game runs unpaced.
            if (GetForegroundWindow() == window && (GetAsyncKeyState(VK_TAB) & 0x8000)) {
                next = clock::now();
                return;
            }
            next += period;
            auto now = clock::now();
            // Sleep most of the wait, then spin: the system timer alone is too coarse for 60 Hz.
            if (next - now > std::chrono::milliseconds(2)) std::this_thread::sleep_until(next - std::chrono::milliseconds(2));
            while (clock::now() < next) {
            }
            if (now - next > period * 5) next = clock::now();  // fell far behind: do not try to catch up
        };
        std::string error;
        {
            amg::Sampler sampler;  // AMG_SAMPLE=FILE (sampler.hpp)
            error = vm.run();
        }
        if (!error.empty()) {  // an error in the game's scripts: a report with its text, and a window that says where it is
            // A save whose file is whole but whose contents the game cannot read would stop every start in the same place:
            // the save files are moved aside (not deleted) so that the next start is a clean one.
            std::string moved;
            if (error.find("GameParam.restore") != std::string::npos || error.find("ShioriData.restore") != std::string::npos ||
                error.find("MemoryCardCheck") != std::string::npos) {
                std::error_code ec;
                // The folder the game itself reads (natives.cpp's save_dir): --save-dir, then AMG_SAVE_DIR, then the default.
                const char* forced = std::getenv("AMG_SAVE_DIR");
                const std::string chosen = !vm.save_dir.empty() ? vm.save_dir : (forced && *forced ? std::string(forced) : amg::paths::save_dir());
                const std::filesystem::path dir = std::filesystem::u8path(chosen);
                const std::filesystem::path aside = dir / ("unreadable-" + std::to_string(std::time(nullptr)));
                // native() and increment(ec): a file name the system code page cannot show must not throw here.
                std::filesystem::directory_iterator it(dir, ec), end;
                while (!ec && it != end) {
                    const std::filesystem::path file = it->path();
                    std::error_code ec2;
                    if (it->is_regular_file(ec2) && file.filename().native().rfind(L"vita_", 0) == 0) {
                        std::filesystem::create_directories(aside, ec2);
                        std::filesystem::rename(file, aside / file.filename(), ec2);
                        if (!ec2) moved = aside.u8string();
                    }
                    it.increment(ec);
                }
            }
            const std::string path = save_report("A script stopped the game: " + error.substr(0, 1500) + (moved.empty() ? "" : "\nThe save files were moved to " + moved));
            tell_report(moved.empty() ? ui::wfmt("The game stopped because of an error in its scripts:\n{0}", {error.substr(0, 500)})
                                      : ui::wfmt("The game could not read its save data and stopped:\n{0}\n\nThe save files were moved to:\n{1}\n\nThe next start is a clean one; copy the files back if you want to try them again.",
                                                 {error.substr(0, 300), moved}),
                        path);
        }
    } catch (std::exception& e) {
        // Most often a copy of the game that passed the launcher's look but is damaged. The launcher opens at the next start.
        write_setting("launcher", "");
        const std::string path = save_report(std::string("The game could not start: ") + e.what());
        tell_report(ui::wfmt("The game could not start:\n{0}\n\nThe copy of the game may be damaged or incomplete. The launcher opens the next time you start the program: choose the game's folder again, or copy the game's files again.",
                             {std::string(e.what()).substr(0, 400)}),
                    path);
    }
    quit = true;
    scene_ready.notify_all();
    if (window) PostMessage(window, WM_CLOSE, 0, 0);
}

// --- Which game folder to run ---------------------------------------------------------------------
// The player's copy is imported once into the program's own folder (game_import.cpp), so the original can be deleted or
// moved. Which folder runs comes from `--root` (as it is, for development), else the imported one remembered in
// %APPDATA%\open-amagami\config.txt, else the player is asked for their copy: it is checked (`check_game_folder`),
// problems are explained in plain words, and the player chooses whether to import it or run it in place.
std::string config_file() {
    // config.ini (D-027, src/paths.hpp); a config.txt of an earlier version in the same folder is taken over once.
    const std::string path = amg::paths::config_file();
    if (path.empty()) return "";
    std::error_code ec;
    const std::filesystem::path file = std::filesystem::u8path(path);
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path old = file.parent_path() / "config.txt";
    if (!std::filesystem::exists(file, ec) && std::filesystem::exists(old, ec)) std::filesystem::copy_file(old, file, ec);
    return path;
}

std::string read_setting(const std::string& key) {
    std::ifstream f(config_file());
    bool first = true;
    for (std::string line; std::getline(f, line);) {
        if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);  // a byte order mark: an editor added it
        first = false;
        if (!line.empty() && line.back() == 13) line.pop_back();
        if (line.compare(0, key.size() + 1, key + "=") == 0) return line.substr(key.size() + 1);
    }
    return "";
}

void write_setting(const std::string& key, const std::string& value) {
    std::string path = config_file();
    if (path.empty()) return;
    std::vector<std::string> keep;
    {
        std::ifstream f(path);
        bool first = true;
        for (std::string line; std::getline(f, line);) {
            if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);  // a byte order mark: an editor added it
            first = false;
            if (!line.empty() && line.back() == 13) line.pop_back();
            if (line.compare(0, key.size() + 1, key + "=") != 0 && !line.empty()) keep.push_back(line);
        }
    }
    std::ofstream out(path, std::ios::trunc);
    for (const auto& l : keep) out << l << "\n";
    out << key << "=" << value << "\n";
    out.flush();
    static std::atomic<bool> told{false};  // once: the settings are not kept, and the player should know why
    if (!out && !told.exchange(true))
        MessageBoxW(nullptr, ui::wfmt("Your settings could not be saved in:\n{0}\n\nThe folder may be read-only. The program works, but your choices will not be remembered.", {path}).c_str(),
                    L"Amagami", MB_OK | MB_ICONWARNING | MB_TOPMOST);
}

// The modern folder dialog (the one of Open file): an address bar to type or paste a path (a network or \\wsl.localhost
// folder too), Quick access and search. Returns the folder in the system code page, as before, or "" if cancelled.
// A message box with `text` (UTF-8) and Amagami as its title; the button pressed.
int say(const std::string& text, UINT flags) { return MessageBoxW(nullptr, ui::wide(text).c_str(), L"Amagami", flags); }

// Why `root` cannot be played ("" when it can): the checks of the folder, then whether its files are whole.
std::string folder_problem(const std::string& root) {
    std::string problem = amg::check_game_folder(root);
    return problem.empty() ? amg::damaged_files(root) : problem;
}

std::string pick_folder(const std::string& title = ui::tr("Choose the folder of your PS Vita copy of Amagami (the one with AMAGAMI.IMG and SCENARIO.CPK)")) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string out;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(ui::wide(title).c_str());
        IShellItem* item = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            char narrow[MAX_PATH * 2] = {};
            if (WideCharToMultiByte(CP_ACP, 0, path, -1, narrow, int(sizeof narrow), nullptr, nullptr) > 0) out = narrow;
        }
        CoTaskMemFree(path);
        if (item) item->Release();
        dialog->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return out;
}

// The English fan patch (F5-18) as the language layer `<root>/lang/en/extracted`: the player picks the patch's folder (its
// `repatch` folder, a Vita3K-style `app`/`patch` tree, or the three files loose); the folder below it that holds AMAGAMI.IMG,
// SCENARIO.CPK and GRAPH.CPK is used. Its AMAGAMI.IMG should be the English one of port/data/known_versions.txt; another
// one is taken only if the player agrees. Returns true when the layer was added (and chosen).
bool add_english_patch(const std::string& root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (root.empty()) {
        say(ui::tr("Choose your game folder first."), MB_OK | MB_ICONINFORMATION);
        return false;
    }
    if (amg::known_label(amg::data_file(root, "AMAGAMI.IMG"), "AMAGAMI.IMG").find("English") != std::string::npos) {
        say(ui::tr("Your game folder already has the English translation built in: nothing to add."), MB_OK | MB_ICONINFORMATION);
        return false;
    }
    const std::string picked = pick_folder(ui::tr("Choose the folder of the English fan patch (Amagami EbKore+ English v1.1)"));
    if (picked.empty()) return false;
    // The first folder (the picked one, then below it, a few levels deep) with the three files.
    fs::path img, scenario, graph;
    auto try_dir = [&](const fs::path& d) {
        fs::path g = fs::exists(d / "graph" / "GRAPH.CPK", ec) ? d / "graph" / "GRAPH.CPK" : d / "GRAPH.CPK";
        if (fs::exists(d / "AMAGAMI.IMG", ec) && fs::exists(d / "SCENARIO.CPK", ec) && fs::exists(g, ec)) {
            img = d / "AMAGAMI.IMG";
            scenario = d / "SCENARIO.CPK";
            graph = g;
            return true;
        }
        return false;
    };
    const fs::path base = fs::u8path(picked);
    if (!try_dir(base))
        for (auto it = fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it.depth() > 4) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_directory(ec) && try_dir(it->path())) break;
        }
    if (img.empty()) {
        say(ui::tr("No AMAGAMI.IMG, SCENARIO.CPK and GRAPH.CPK were found together in that folder. Choose the patch's folder that holds them (for the NoNpDrm release, its \"repatch\" folder)."),
            MB_OK | MB_ICONWARNING);
        return false;
    }
    const std::string label = amg::known_label(img.u8string(), "AMAGAMI.IMG");
    if (label.find("English") == std::string::npos &&
        say(ui::tr(label.find("Japanese") != std::string::npos
                       ? "Those are the original Japanese files, not the translation. Add them anyway?"
                       : "Those files are not the English patch this program knows (v1.1). They may be another version. Add them anyway?"),
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return false;
    const fs::path dest = fs::u8path(root) / "lang" / "en" / "extracted";
    fs::create_directories(dest / "graph", ec);
    const std::pair<fs::path, fs::path> copies[] = {
        {img, dest / "AMAGAMI.IMG"}, {scenario, dest / "SCENARIO.CPK"}, {graph, dest / "graph" / "GRAPH.CPK"}};
    for (const auto& [from, to] : copies) {
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            say(ui::fmt("Could not copy {0}:\n{1}", {from.u8string(), ec.message()}), MB_OK | MB_ICONERROR);
            return false;
        }
    }
    write_setting("lang", "en");
    say(ui::tr("The English translation was added and chosen. You can switch back in Game files, Language."), MB_OK | MB_ICONINFORMATION);
    return true;
}

// Asks for a folder until one passes the check; "" when the player gives up.
std::string ask_for_copy(std::string candidate) {
    for (;;) {
        if (candidate.empty()) {
            say(ui::tr("This program needs your own copy of the game: the PS Vita version of Amagami (PCSG00291), already decrypted. Choose its folder next."),
                MB_OK | MB_ICONINFORMATION);
        } else {
            candidate = amg::find_game_root(candidate);  // the player may have picked a folder above the game's own
            std::string problem = folder_problem(candidate);
            if (problem.empty()) return candidate;
            if (say(ui::fmt("{0}\n\nChoose another folder?", {problem}), MB_YESNO | MB_ICONWARNING) != IDYES)
                return "";
        }
        candidate = pick_folder();
        if (candidate.empty()) return "";
    }
}

// Once the game is set up the first time, the desktop and Start menu shortcuts are offered (once; not in portable mode).
void offer_shortcuts(const std::string& root) {
    amg::shortcut::offer_once(root, read_setting("shortcut_offered") == "1", [] { write_setting("shortcut_offered", "1"); });
}

// Imports `source` into the program's folder after asking; returns the folder to run ("" if the player cancelled).
std::string import_or_use(const std::string& source) {
    std::string dest = amg::import_destination();
    if (dest.empty()) return source;
    char gigabytes[32];
    std::snprintf(gigabytes, sizeof gigabytes, "%.1f", double(amg::import_size(source)) / 1e9);
    int answer = say(ui::fmt("Copy the game into this program's own folder (about {0} GB)? Then you can delete or move your original copy afterwards.\n\nYes: copy it.   No: run it from where it is.", {gigabytes}),
                     MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL) return "";
    if (answer == IDNO) {
        write_setting("root", source);
        offer_shortcuts(source);
        return source;
    }
    std::string failed = amg::import_game(source, dest);
    if (!failed.empty()) {
        say(failed, MB_OK | MB_ICONERROR);
        return "";
    }
    write_setting("root", dest);
    write_setting("source", source);
    offer_shortcuts(dest);
    return dest;
}

// `import_from`: `--import FOLDER` or `--reimport` (the folder is asked for). Returns the folder to run, or "".
std::string choose_root(const std::string& wanted, const std::string& import_from, bool reimport) {
    if (!wanted.empty()) return folder_problem(wanted).empty() ? wanted : ask_for_copy(wanted);
    if (reimport || !import_from.empty()) {
        std::string source = ask_for_copy(import_from);
        return source.empty() ? "" : import_or_use(source);
    }
    std::string remembered = read_setting("root");
    if (!remembered.empty() && folder_problem(remembered).empty()) return remembered;
    // The development layout (`extracted/` in the working folder, run from the repository) still works.
    if (amg::check_game_folder(".").empty()) return ".";
    std::string source = ask_for_copy("");
    return source.empty() ? "" : import_or_use(source);
}

// The port's settings (the rows of the game's Settings screen and the launcher's window) read and write config.ini and apply
// at once.
void open_game_settings(HWND hwnd) {
    static bool open = false;  // the window pumps messages: a second F8 must not open a second one
    if (open) return;
    open = true;
    held = 0;
    mouse_down = 0;
    g_paused = true;
    amg::launcher::Services services;
    services.read_setting = [](const std::string& key) { return read_setting(key); };
    services.write_setting = [](const std::string& key, const std::string& value) { write_setting(key, value); };
    amg::launcher::show_game_settings(hwnd, HINSTANCE(GetModuleHandleW(nullptr)), services);
    g_paused = false;
    open = false;
}

void install_settings_hooks() {
    amg::port_settings::load = [](const std::string& key) { return read_setting(key); };
    amg::port_settings::save = [](const std::string& key, const std::string& value) {
        write_setting(key, value);
        amg::input::set_settings(input_settings_from([](const char* k) { return read_setting(k); }));
        amg::input::load_keys([](const std::string& k) { return read_setting(k); });
        amg::input::load_pad_buttons([](const std::string& k) { return read_setting(k); });
        apply_filter();
    };
    amg::input::load_keys([](const std::string& k) { return read_setting(k); });
    amg::input::load_pad_buttons([](const std::string& k) { return read_setting(k); });
}

}  // namespace

// What the player is left with when the program stops by itself: a file with what is known (the version, where it stopped,
// the last lines the scripts printed) in the program's data folder, and a window that says so and offers to open the folder.
std::atomic<bool> g_reporting{false};

// Writes crash.txt (the version, `headline`, where the scripts were and the last lines they printed, where to send it) in the
// program's data folder, or in the temporary folder when that one cannot be written. Returns the file's path, "" if neither worked.
std::string save_report(const std::string& headline) {
    std::string folder = amg::paths::config_file();
    folder = folder.empty() ? std::string(".") : folder.substr(0, folder.find_last_of("/\\"));
    std::vector<std::string> folders = {folder};
    wchar_t temp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temp) > 0) folders.push_back(amg::ui::narrow(temp));
    for (const std::string& dir : folders) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
        std::string path = dir + (dir.empty() || dir.back() == '/' || dir.back() == '\\' ? "" : "/") + "crash.txt";
        for (char& ch : path)
            if (ch == '/') ch = '\\';  // one kind of slash in what the player reads
        std::FILE* f = _wfopen(amg::ui::wide(path).c_str(), L"w");
        if (!f) continue;
        std::fprintf(f, "open-amagami %s (%s)%c", amg::update_check::current(), sizeof(void*) == 8 ? "64-bit" : "32-bit", 10);
        std::fprintf(f, "%s%c", headline.c_str(), 10);
        try {
            if (VM* vm = g_vm.load()) {
                std::fprintf(f, "script tick %lld%c", (long long)vm->tick, 10);
                const size_t from = vm->out.size() > 30 ? vm->out.size() - 30 : 0;
                for (size_t i = from; i < vm->out.size(); ++i) std::fprintf(f, "out: %s%c", vm->out[i].c_str(), 10);
            }
        } catch (...) {
        }
        std::fprintf(f, "Please send this file with a description of what you were doing: https://github.com/hokagocafe/open-amagami/issues%c", 10);
        std::fclose(f);
        return path;
    }
    return "";
}

// `message`, then (when the report was saved) where it is and an offer to open that folder; when it could not be saved, said so.
void tell_report(const std::wstring& message, const std::string& path) {
    const std::wstring tail = path.empty() ? amg::ui::wtr("A report could not be saved: the program's data folder cannot be written.")
                                           : amg::ui::wfmt("A report of what happened was saved in:\n{0}\n\nOpen that folder?", {path});
    const std::wstring text = message + L"\n\n" + tail;
    if (MessageBoxW(nullptr, text.c_str(), L"Amagami", (path.empty() ? MB_OK : MB_YESNO) | MB_ICONERROR | MB_TOPMOST) == IDYES && !path.empty()) {
        const std::wstring select = L"/select,\"" + amg::ui::wide(path) + L"\"";
        std::wstring fixed = select;
        for (wchar_t& ch : fixed)
            if (ch == L'/' && &ch != &fixed[7]) ch = L'\\';
        ShellExecuteW(nullptr, L"open", L"explorer.exe", fixed.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

[[noreturn]] void report_crash(const char* what, DWORD code, void* where) {
    if (g_reporting.exchange(true)) ExitProcess(3);  // a crash while reporting a crash
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(where), &module);
    wchar_t name[MAX_PATH] = L"?";
    if (module) GetModuleFileNameW(module, name, MAX_PATH);
    const std::wstring file = name;
    char headline[600];
    std::snprintf(headline, sizeof headline, "%s, code 0x%08lX, at %s+0x%llX", what, (unsigned long)code,
                  amg::ui::narrow(file.substr(file.find_last_of(L"\\/") + 1)).c_str(),
                  (unsigned long long)(reinterpret_cast<uintptr_t>(where) - reinterpret_cast<uintptr_t>(module)));
    std::wstring message = amg::ui::wtr("The program stopped unexpectedly.");
    if (GetTickCount64() - g_game_began.load() < 30000) {  // within the first 30 seconds of loading: most often a damaged copy of the game
        write_setting("launcher", "");
        message += L"\n\n" + amg::ui::wtr("It stopped right at the start, so the copy of the game may be damaged. The launcher opens the next time you start the program: choose the game's folder again, or copy the game's files again.");
    }
    tell_report(message, save_report(headline));
    ExitProcess(3);
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    report_crash("The program crashed", info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
}

void crash_terminate() { report_crash("The program ended on an unhandled error", 0xE0000001, reinterpret_cast<void*>(&crash_terminate)); }

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdline, int show) {
    SetUnhandledExceptionFilter(crash_filter);
    amg::updater::cleanup();  // the files an update moved aside
    std::set_terminate(crash_terminate);
    std::string root = ".";
    std::string args = cmdline ? cmdline : "";
    auto value_after = [&](const std::string& flag) -> std::string {
        size_t at = args.find(flag);
        if (at == std::string::npos) return "";
        size_t from = at + flag.size();
        while (from < args.size() && args[from] == ' ') ++from;
        size_t to = from;
        if (from < args.size() && args[from] == '"') {
            ++from;
            to = args.find('"', from);
        } else {
            to = args.find(' ', from);
        }
        return args.substr(from, to == std::string::npos ? std::string::npos : to - from);
    };
    {
        install_settings_hooks();
        amg::ui::init(read_setting("ui_lang"));  // the words of the launcher and its messages
        // The launcher (D-027) opens unless a developer flag names the folder, or the player set `launcher=skip` (holding Shift
        // shows it anyway) and the remembered folder still passes the check.
        const bool developer = !value_after("--root").empty() || !value_after("--import").empty() ||
                               args.find("--reimport") != std::string::npos || args.find("--start") != std::string::npos;
        std::string remembered = read_setting("root");
        if (remembered.empty() && amg::check_game_folder(".").empty()) remembered = ".";  // the development layout
        if (!developer) amg::shortcut::refresh(remembered);  // shortcuts left pointing at an older version's folder
        const bool skip = developer || (read_setting("launcher") == "skip" && !(GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                                        !remembered.empty() && folder_problem(remembered).empty());
        std::string chosen;
        if (skip) {
            chosen = choose_root(value_after("--root"), value_after("--import"), args.find("--reimport") != std::string::npos);
        } else {
            amg::launcher::Services services;
            services.read_setting = [](const std::string& key) { return read_setting(key); };
            services.write_setting = [](const std::string& key, const std::string& value) { write_setting(key, value); };
            services.choose_folder = [] {
                std::string source = ask_for_copy("");
                return source.empty() ? std::string() : import_or_use(source);
            };
            services.add_english = [](const std::string& r) { return add_english_patch(r); };
            services.find_copies = [] {
                // Where a copy of the game is likely to be: a Vita3K install, the program's own game folder, the desktop,
                // the documents and the downloads.
                auto env = [](const char* name) {
                    const char* v = std::getenv(name);
                    return v ? std::string(v) : std::string();
                };
                std::vector<std::string> bases;
                if (!env("APPDATA").empty()) bases.push_back(env("APPDATA") + "/Vita3K/Vita3K");
                bases.push_back(amg::import_destination());
                const std::string home = env("USERPROFILE");
                if (!home.empty())
                    for (const char* sub : {"/Desktop", "/Downloads", "/Documents"}) bases.push_back(home + sub);
                return amg::find_installed_copies(bases, read_setting("root"));
            };
            services.use_copy = [](const std::string& folder) {
                const std::string source = amg::find_game_root(folder);
                const std::string problem = folder_problem(source);
                if (!problem.empty()) {
                    say(problem, MB_OK | MB_ICONWARNING);
                    return std::string();
                }
                return import_or_use(source);
            };
            chosen = amg::launcher::run(inst, remembered, services);
        }
        if (chosen.empty()) return 0;
        root = chosen;
        // A copy imported by an earlier version lacks the home screen icon: take it from the original folder if it is still there.
        std::error_code ec;
        const std::filesystem::path icon = std::filesystem::u8path(root) / "sce_sys" / "icon0.png";
        const std::string source = read_setting("source");
        if (!source.empty() && !std::filesystem::exists(icon, ec)) {
            const std::filesystem::path from = std::filesystem::u8path(source) / "sce_sys" / "icon0.png";
            if (std::filesystem::exists(from, ec)) {
                std::filesystem::create_directories(icon.parent_path(), ec);
                std::filesystem::copy_file(from, icon, ec);
            }
        }
    }
    if (!value_after("--save-dir").empty()) g_save_dir = value_after("--save-dir");
    {
        std::string lang = !value_after("--lang").empty() ? value_after("--lang") : read_setting("lang");
        if (!lang.empty()) _putenv_s("AMG_LANG", lang.c_str());
    }
    if (args.find("--skip-prologue") != std::string::npos) _putenv("AMG_SKIP_PROLOGUE=1");
    if (!value_after("--scale").empty()) {
        scale = std::max(1, std::atoi(value_after("--scale").c_str()));
        scale_given = true;
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
#ifdef AMG_HAS_ICON
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));  // the program's icon (res/amagami.rc.in), for the title bar and the taskbar
#endif
    wc.lpszClassName = L"AmagamiPort";
    RegisterClassW(&wc);
    amg::read_screen_settings();
    // The game's window counts in real pixels (per-monitor DPI awareness, for this thread only: the launcher before it lays
    // out in pixels and is left to Windows' scaling). Without it a display set to 150 % stretched the window.
    using SetThreadDpiFn = HANDLE(WINAPI*)(HANDLE);
    using SystemDpiFn = UINT(WINAPI*)();
    using AdjustForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    HMODULE user = GetModuleHandleW(L"user32.dll");
    if (auto set_dpi = reinterpret_cast<SetThreadDpiFn>(GetProcAddress(user, "SetThreadDpiAwarenessContext")))
        set_dpi(HANDLE(-4));  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    UINT dpi = 96;
    if (auto system_dpi = reinterpret_cast<SystemDpiFn>(GetProcAddress(user, "GetDpiForSystem"))) dpi = system_dpi();
    // The first size: what the player left last time (kept while it is still on a screen), else the biggest window of the
    // game's shape that takes 80 % of the main screen's work area, centred; `--scale N` is N times the frame.
    int cw = SCREEN_W * scale, ch = SCREEN_H * scale, wx = CW_USEDEFAULT, wy = CW_USEDEFAULT;
    if (!scale_given) {
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        const int work_w = mi.rcWork.right - mi.rcWork.left, work_h = mi.rcWork.bottom - mi.rcWork.top;
        const double k = std::max(0.5, std::min(work_w * 0.8 / SCREEN_W, work_h * 0.8 / SCREEN_H));
        cw = int(SCREEN_W * k + 0.5);
        ch = int(SCREEN_H * k + 0.5);
        wx = mi.rcWork.left + (work_w - cw) / 2;
        wy = mi.rcWork.top + (work_h - ch) / 2;
        const int sw = std::atoi(read_setting("ww").c_str()), sh = std::atoi(read_setting("wh").c_str());
        const int sx = std::atoi(read_setting("wx").c_str()), sy = std::atoi(read_setting("wy").c_str());
        RECT remembered = {sx, sy, sx + sw, sy + sh};
        if (sw >= 320 && sh >= 180 && MonitorFromRect(&remembered, MONITOR_DEFAULTTONULL)) {
            cw = sw;
            ch = sh;
            wx = sx;
            wy = sy;
        }
    }
    RECT rc = {0, 0, cw, ch};
    if (auto adjust = reinterpret_cast<AdjustForDpiFn>(GetProcAddress(user, "AdjustWindowRectExForDpi")))
        adjust(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    else
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    if (wx != CW_USEDEFAULT) {  // the position was of the client area: the frame goes around it
        wx += rc.left;
        wy += rc.top;
    }
    window = CreateWindowW(L"AmagamiPort", L"Amagami", WS_OVERLAPPEDWINDOW, wx, wy, rc.right - rc.left, rc.bottom - rc.top,
                           nullptr, nullptr, inst, nullptr);
    if (window) {
        if (HICON big = amg::shortcut::load_game_icon(root, GetSystemMetrics(SM_CXICON))) SendMessageW(window, WM_SETICON, ICON_BIG, LPARAM(big));
        if (HICON little = amg::shortcut::load_game_icon(root, GetSystemMetrics(SM_CXSMICON)))
            SendMessageW(window, WM_SETICON, ICON_SMALL, LPARAM(little));
    }
    {
        RECT client;
        GetClientRect(window, &client);  // here, on the window's own thread
        g_client_w = client.right;
        g_client_h = client.bottom;
    }
    ShowWindow(window, show);
    SetTimer(window, CURSOR_TIMER, 250, nullptr);  // the cursor's rest (WM_TIMER)
    gpu_scene = value_after("--renderer") != "present";
    if (value_after("--renderer") != "software") {
        gpu = amg::D3DPresenter::create(window, SCREEN_W, SCREEN_H);
        if (!gpu) std::fprintf(stderr, "Direct3D 11 unavailable: using the software output%c", 10);
        apply_filter();
    }

    if (std::getenv("AMG_TEST_CRASH"))  // for testing the report: stops the program on purpose after three seconds
        std::thread([] {
            Sleep(3000);
            *static_cast<volatile int*>(nullptr) = 1;
        }).detach();
    opt_out_of_throttling(true);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    timeBeginPeriod(1);  // 1 ms timer resolution for frame pacing
    std::thread worker(machine_thread, root);
    worker.detach();

    MSG m;
    while (GetMessage(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    ExitProcess(0);  // script threads are parked on their baton; do not unwind them
}
