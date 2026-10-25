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
#include "datapath.hpp"
#include "game_import.hpp"
#include "input.hpp"
#include "launcher.hpp"
#include "menu_pointer.hpp"
#include "paths.hpp"
#include "port_settings.hpp"
#include <functional>
#include "skins.hpp"
#include "gfx.hpp"
#include "vm.hpp"

using namespace amg;

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
std::atomic<int> menu_held{0};                // what the mouse presses in the menus this frame (menu_pointer.hpp)
HWND window = nullptr;
int scale = 2;
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
    ShowCursor(fullscreen ? FALSE : TRUE);
    InvalidateRect(hwnd, nullptr, TRUE);
}

int key_mask(WPARAM key) { return amg::input::key_to_mask(int(key)); }
struct Viewport {
    int x, y, width, height;
};

Viewport fitted_viewport(const RECT& rc) {
    int width = rc.right, height = rc.bottom;
    if (width * SCREEN_H > height * SCREEN_W)
        width = height * SCREEN_W / SCREEN_H;
    else
        height = width * SCREEN_H / SCREEN_W;
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
    bool yes = MessageBoxW(hwnd, L"Quit the game?\nProgress that has not been saved is lost.", L"Amagami",
                           MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    asking = false;
    return yes;
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
            if (w == VK_F9) {
                shot_request = true;
                return 0;
            }
            if (w == VK_F10) {
                dump_request = true;
                return 0;
            }
            if (w == VK_F11 || (w == VK_RETURN && (l & (1 << 29)))) {  // bit 29: Alt is down
                if (!(l & (1 << 30))) toggle_fullscreen(hwnd);           // bit 30: auto-repeat
                return 0;
            }
            if (key_mask(w)) amg::input::note_keyboard();
            held |= key_mask(w);
            return 0;
        case WM_SYSCHAR:
            return 0;  // no beep for Alt+Enter
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
            return 0;
        case WM_MOUSEMOVE:
            mouse_x = int(short(LOWORD(l)));
            mouse_y = int(short(HIWORD(l)));
            return 0;
        case WM_RBUTTONDOWN:
            mouse_right_click = true;
            return 0;
        case WM_MOUSEWHEEL:
            mouse_wheel += GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
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
            mouse_cancelled = true;
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED:
            if (mouse_down.exchange(0)) mouse_cancelled = true;
            return 0;
        case WM_SIZE:
            if (gpu) gpu->resize(int(LOWORD(l)), int(HIWORD(l)));
            return 0;
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
}

// Sound output: WinMM waveOut with four small buffers refilled from the mixer.
// The sound is made by the machine, one frame's worth (AUDIO_RATE / 60 samples) per script frame, and queued here;
// the device thread only plays the queue. So the voices' and the music's progress, which the scripts read
// (Music.isBusy, the lip sync), follows the script frames exactly as in a headless run (`amagami_vm --audio`), and a
// recorded session replays the same. Before, the device thread rendered at the sound card's pace.
std::mutex g_sound_mutex;
std::deque<int16_t> g_sound;

void queue_sound(const int16_t* samples, size_t count) {
    std::lock_guard<std::mutex> lk(g_sound_mutex);
    g_sound.insert(g_sound.end(), samples, samples + count);
    const size_t limit = size_t(AUDIO_RATE / 4) * 2;  // a quarter of a second: drop the oldest beyond it
    while (g_sound.size() > limit) g_sound.pop_front();
}

void take_sound(int16_t* out, size_t count) {
    std::lock_guard<std::mutex> lk(g_sound_mutex);
    size_t n = std::min(count, g_sound.size());
    std::copy(g_sound.begin(), g_sound.begin() + std::ptrdiff_t(n), out);
    g_sound.erase(g_sound.begin(), g_sound.begin() + std::ptrdiff_t(n));
    std::fill(out + n, out + count, int16_t(0));
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
                SetWindowTextA(window, title);
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
            SetWindowTextA(window, title);
        }
    }
}

static std::string g_save_dir;

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
    if (!gpu) return;
    std::string f = read_setting("filter");
    if (const char* e = std::getenv("AMG_FILTER")) f = e;
    gpu->set_filter(f == "sharp" ? 1 : f == "pixels" ? 2 : 0);
    auto value = [](const char* env, const char* key) {
        const char* e = std::getenv(env);
        return e ? std::string(e) : read_setting(key);
    };
    std::string a = value("AMG_ASPECT", "aspect"), r = value("AMG_RENDER", "render");
    gpu->set_aspect(a == "fill" ? 1 : a == "stretch" ? 2 : a == "whole" ? 3 : 0);
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
        VM vm(root);
        vm.save_dir = g_save_dir;
        vm.max_steps = INT64_MAX;
        vm.profile_prims = std::getenv("AMG_PROFILE_PRIMS") != nullptr;
        if (vm.profile_prims) std::freopen("build/slowprim.log", "w", stderr);
        install_graphics(vm);
        if (size_t n = g_skins.load(find_skins()); n > 0) vm.graphics->glyph_picker = pick_glyph;
        AudioEngine audio(root);
        install_audio(vm, &audio);
        // One frame of sound per script frame, called from the frame hook below (which replaces vm.on_tick).
        static const bool silent = std::getenv("AMG_NO_AUDIO") != nullptr;
        auto sound_frame = [&audio]() {
            int16_t frame[size_t(AUDIO_RATE / 60) * 2];
            audio.render(frame, size_t(AUDIO_RATE / 60));
            if (!silent) queue_sound(frame, size_t(AUDIO_RATE / 60) * 2);
        };
        if (!std::getenv("AMG_NO_AUDIO")) {
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
        std::shared_ptr<const Scene> last_scene;
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
            vm.on_shot = [](VM& v, const std::string& file) { v.graphics->save_png(file); };
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
        vm.on_tick = [&](VM& v) {
            v.graphics->step(1.0);
            sound_frame();
            if (shot_request.exchange(false)) {
                v.graphics->save_png("build/shot.png");
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
            pad_held = amg::input::poll_pads(pad_axes);  // once per frame, as the console reads the pad once per vblank
            {  // the mouse as the touch screen: client pixels to the scripts' units, through the letterboxed picture
                RECT rc;
                GetClientRect(window, &rc);
                const Viewport view = fitted_viewport(rc);
                if (mouse_cancelled.exchange(false) || view.width <= 0 || view.height <= 0) {
                    v.touch.cancel();
                } else {
                    double fx = (mouse_x - view.x) / double(view.width);
                    double fy = (mouse_y - view.y) / double(view.height);
                    double units_w = SCREEN_W / VIEW_UNIT, units_h = SCREEN_H / VIEW_UNIT;
                    v.touch.update(mouse_down.load() != 0, (fx - 0.5) * units_w, (fy - 0.5) * units_h, v.tick);
                    // The menus: the mouse plays them from the picture (the scripts only ask for touch in the gallery).
                    const bool down = mouse_down.load() != 0;
                    const bool click = down && !mouse_was_down;
                    mouse_was_down = down;
                    const bool inside = contains(view, mouse_x, mouse_y);
                    if (last_scene) {
                        menu_held = pointer.update(*last_scene, v.graphics->ui_image(20, 0), (fx - 0.5) * units_w,
                                                   (fy - 0.5) * units_h, inside, click && inside,
                                                   mouse_right_click.exchange(false) && inside, mouse_wheel.exchange(0));
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
                std::lock_guard<std::mutex> lk(scene_mutex);
                pending_scene = std::move(scene);
            }
            scene_ready.notify_one();
            latest_tick = v.tick;
            if (profiling) {
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
        if (splash) {  // the data is loaded; the splash stays at least four seconds, any button or click ends it sooner
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
        }
        std::string error = vm.run();
        if (!error.empty()) {
            std::string text = error.substr(0, 1500);
            MessageBoxA(window, text.c_str(), "Script error", MB_OK | MB_ICONERROR);
        }
    } catch (std::exception& e) {
        MessageBoxA(window, e.what(), "Startup error", MB_OK | MB_ICONERROR);
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
    for (std::string line; std::getline(f, line);) {
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
        for (std::string line; std::getline(f, line);) {
            if (!line.empty() && line.back() == 13) line.pop_back();
            if (line.compare(0, key.size() + 1, key + "=") != 0 && !line.empty()) keep.push_back(line);
        }
    }
    std::ofstream out(path, std::ios::trunc);
    for (const auto& l : keep) out << l << "\n";
    out << key << "=" << value << "\n";
}

// The modern folder dialog (the one of Open file): an address bar to type or paste a path (a network or \\wsl.localhost
// folder too), Quick access and search. Returns the folder in the system code page, as before, or "" if cancelled.
std::string pick_folder(const char* title = "Choose the folder of your PS Vita copy of Amagami (the one with AMAGAMI.IMG and SCENARIO.CPK)") {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string out;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        wchar_t wide[512] = {};
        MultiByteToWideChar(CP_ACP, 0, title, -1, wide, 512);
        dialog->SetTitle(wide);
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
        MessageBoxA(nullptr, "Choose your game folder first.", "Amagami", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    if (amg::known_label(amg::data_file(root, "AMAGAMI.IMG"), "AMAGAMI.IMG").find("English") != std::string::npos) {
        MessageBoxA(nullptr, "Your game folder already has the English translation built in: nothing to add.", "Amagami",
                    MB_OK | MB_ICONINFORMATION);
        return false;
    }
    const std::string picked = pick_folder("Choose the folder of the English fan patch (Amagami EbKore+ English v1.1)");
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
        MessageBoxA(nullptr,
                    "No AMAGAMI.IMG, SCENARIO.CPK and GRAPH.CPK were found together in that folder. Choose the patch's "
                    "folder that holds them (for the NoNpDrm release, its \"repatch\" folder).",
                    "Amagami", MB_OK | MB_ICONWARNING);
        return false;
    }
    const std::string label = amg::known_label(img.u8string(), "AMAGAMI.IMG");
    if (label.find("English") == std::string::npos &&
        MessageBoxA(nullptr,
                    label.find("Japanese") != std::string::npos
                        ? "Those are the original Japanese files, not the translation. Add them anyway?"
                        : "Those files are not the English patch this program knows (v1.1). They may be another version. Add them anyway?",
                    "Amagami", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return false;
    const fs::path dest = fs::u8path(root) / "lang" / "en" / "extracted";
    fs::create_directories(dest / "graph", ec);
    const std::pair<fs::path, fs::path> copies[] = {
        {img, dest / "AMAGAMI.IMG"}, {scenario, dest / "SCENARIO.CPK"}, {graph, dest / "graph" / "GRAPH.CPK"}};
    for (const auto& [from, to] : copies) {
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            MessageBoxA(nullptr, ("Could not copy " + from.u8string() + ":\n" + ec.message()).c_str(), "Amagami", MB_OK | MB_ICONERROR);
            return false;
        }
    }
    write_setting("lang", "en");
    MessageBoxA(nullptr, "The English translation was added and chosen. You can switch back in Settings, Language.", "Amagami",
                MB_OK | MB_ICONINFORMATION);
    return true;
}

// Asks for a folder until one passes the check; "" when the player gives up.
std::string ask_for_copy(std::string candidate) {
    for (;;) {
        if (candidate.empty()) {
            MessageBoxA(nullptr,
                        "This program needs your own copy of the game: the PS Vita version of Amagami (PCSG00291), "
                        "already decrypted. Choose its folder next.",
                        "Amagami", MB_OK | MB_ICONINFORMATION);
        } else {
            std::string problem = amg::check_game_folder(candidate);
            if (problem.empty()) return candidate;
            if (MessageBoxA(nullptr, (problem + "\n\nChoose another folder?").c_str(), "Amagami", MB_YESNO | MB_ICONWARNING) != IDYES)
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
    char text[500];
    std::snprintf(text, sizeof text,
                  "Copy the game into this program's own folder (about %.1f GB)? Then you can delete or move your original "
                  "copy afterwards.\n\nYes: copy it.   No: run it from where it is.",
                  double(amg::import_size(source)) / 1e9);
    int answer = MessageBoxA(nullptr, text, "Amagami", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL) return "";
    if (answer == IDNO) {
        write_setting("root", source);
        offer_shortcuts(source);
        return source;
    }
    std::string failed = amg::import_game(source, dest);
    if (!failed.empty()) {
        MessageBoxA(nullptr, failed.c_str(), "Amagami", MB_OK | MB_ICONERROR);
        return "";
    }
    write_setting("root", dest);
    write_setting("source", source);
    offer_shortcuts(dest);
    return dest;
}

// `import_from`: `--import FOLDER` or `--reimport` (the folder is asked for). Returns the folder to run, or "".
std::string choose_root(const std::string& wanted, const std::string& import_from, bool reimport) {
    if (!wanted.empty()) return amg::check_game_folder(wanted).empty() ? wanted : ask_for_copy(wanted);
    if (reimport || !import_from.empty()) {
        std::string source = ask_for_copy(import_from);
        return source.empty() ? "" : import_or_use(source);
    }
    std::string remembered = read_setting("root");
    if (!remembered.empty() && amg::check_game_folder(remembered).empty()) return remembered;
    // The development layout (`extracted/` in the working folder, run from the repository) still works.
    if (amg::check_game_folder(".").empty()) return ".";
    std::string source = ask_for_copy("");
    return source.empty() ? "" : import_or_use(source);
}

// The port's settings (the rows of the game's Settings screen and the launcher's window) read and write config.ini and apply
// at once.
void install_settings_hooks() {
    amg::port_settings::load = [](const std::string& key) { return read_setting(key); };
    amg::port_settings::save = [](const std::string& key, const std::string& value) {
        write_setting(key, value);
        amg::input::set_settings(input_settings_from([](const char* k) { return read_setting(k); }));
        amg::input::load_keys([](const std::string& k) { return read_setting(k); });
        apply_filter();
    };
    amg::input::load_keys([](const std::string& k) { return read_setting(k); });
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdline, int show) {
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
        // The launcher (D-027) opens unless a developer flag names the folder, or the player set `launcher=skip` (holding Shift
        // shows it anyway) and the remembered folder still passes the check.
        const bool developer = !value_after("--root").empty() || !value_after("--import").empty() ||
                               args.find("--reimport") != std::string::npos || args.find("--start") != std::string::npos;
        std::string remembered = read_setting("root");
        if (remembered.empty() && amg::check_game_folder(".").empty()) remembered = ".";  // the development layout
        const bool skip = developer || (read_setting("launcher") == "skip" && !(GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                                        !remembered.empty() && amg::check_game_folder(remembered).empty());
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
    if (!value_after("--scale").empty()) scale = std::max(1, std::atoi(value_after("--scale").c_str()));

    WNDCLASSW wc = {};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
#ifdef AMG_HAS_ICON
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));  // the program's icon (res/amagami.rc.in), for the title bar and the taskbar
#endif
    wc.lpszClassName = L"AmagamiPort";
    RegisterClassW(&wc);
    amg::use_screen_of(root);
    RECT rc = {0, 0, SCREEN_W * scale, SCREEN_H * scale};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    window = CreateWindowW(L"AmagamiPort", L"Amagami (open-amagami port)", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                           CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    if (window) {
        if (HICON big = amg::shortcut::load_game_icon(root, GetSystemMetrics(SM_CXICON))) SendMessageW(window, WM_SETICON, ICON_BIG, LPARAM(big));
        if (HICON little = amg::shortcut::load_game_icon(root, GetSystemMetrics(SM_CXSMICON)))
            SendMessageW(window, WM_SETICON, ICON_SMALL, LPARAM(little));
    }
    ShowWindow(window, show);
    gpu_scene = value_after("--renderer") != "present";
    if (value_after("--renderer") != "software") {
        gpu = amg::D3DPresenter::create(window, SCREEN_W, SCREEN_H);
        if (!gpu) std::fprintf(stderr, "Direct3D 11 unavailable: using the software output%c", 10);
        apply_filter();
    }

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
