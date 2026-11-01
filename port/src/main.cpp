// Command line driver for the port's script machine: boots the game's scripts headless, as
// tools/scfvm.py does, so the two can be compared.
//
//   amagami_vm --root <repo> [--steps N] [--press TICK:MASK[:FRAMES]]... [--log FILE] [--save-dir DIR]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <csignal>
#include <iostream>

#include "audio.hpp"
#include "gfx.hpp"
#include "menu_pointer.hpp"
#include "mouse_menus.hpp"
#include "cursor_art.hpp"
#include "sampler.hpp"
#include "vm.hpp"
#ifdef AMG_PERSONAL
#include <memory>
#include "personal/text_extract.hpp"
#endif

#ifdef _WIN32
#include <windows.h>
// AMG_CRASH_TRACE=1: an access violation prints where it happened (offset in the program, to look up in its .map file).
static LONG WINAPI crash_trace(EXCEPTION_POINTERS* e) {
    std::fprintf(stderr, "CRASH code=%08lx at offset 0x%llx\n", e->ExceptionRecord->ExceptionCode,
                 (unsigned long long)((uintptr_t)e->ExceptionRecord->ExceptionAddress - (uintptr_t)GetModuleHandleW(nullptr)));
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

using namespace amg;

// AMG_COVERAGE=DIR: what this run executed, in DIR/<name>.cov (tools/coverage.py reads them): the script methods
// run, the native methods and the primitives used.
// SIGTERM or SIGINT end the run at the next instruction, as a spent budget does, so a long run that is cut still
// writes its coverage and the place where it was.
static amg::VM* g_running = nullptr;
static void stop_run(int) {
    if (g_running) g_running->max_steps = 0;
}

static void write_coverage(amg::VM& vm, const std::string& name) {
    std::ofstream cov(std::string(std::getenv("AMG_COVERAGE")) + "/" + name + ".cov");
    std::vector<std::string> lines;
    for (auto& [cname, cls] : vm.classes)
        for (auto* table : {&cls->imethods, &cls->cmethods})
            for (auto& [key, callable] : *table)
                if (callable.method && callable.method->ran && callable.method->cls == cls.get())
                    lines.push_back("m " + cname + (table == &cls->cmethods ? "." : "#") + callable.method->name + "/" +
                                    std::to_string(callable.method->args));
    for (uint64_t k : vm.natives_used) lines.push_back("n " + vm.names[size_t(k >> 8)] + "/" + std::to_string(int(k & 0xFF)));
    for (auto& [cls, n] : vm.prims_used) lines.push_back("p " + cls->name + "." + std::to_string(n));
    for (auto& [cls, k, on_class] : vm.natives_owned)
        lines.push_back("m " + cls->name + (on_class ? "." : "#") + vm.names[size_t(k >> 8)] + "/" + std::to_string(int(k & 0xFF)));
    std::sort(lines.begin(), lines.end());
    for (auto& l : lines) cov << l << '\n';
}

int main(int argc, char** argv) {
#ifdef _WIN32
    if (std::getenv("AMG_CRASH_TRACE")) SetUnhandledExceptionFilter(crash_trace);
#endif
    std::string root = ".", log, save_dir;
    bool smooth = false;
    bool fork_scenes = false;  // --fork
    int64_t tick_limit = 0;    // --ticks
    bool audio_on = false;     // --audio
    int scene_timeout = 900;
    std::string wav;
    int64_t steps = 5'000'000;
    bool steps_given = false;
    int64_t auto_press_first = 0;
    std::vector<std::string> queue;
    int choice = 0;     // --choice K: presses of down before circle at a choice (the K-th answer)
    std::string scene_list;  // --scene-list FILE: one scene name per line, run one after another
    bool chain = false;
    std::string this_scene;
    std::string scene;  // --scene NAME: run one event scene headless (see docs/technical/port.md)
    std::vector<std::tuple<int64_t, int64_t, int>> presses;
    std::vector<std::pair<int64_t, std::string>> shots;
    struct MouseAt { int64_t tick; double x, y; std::string flags; };
    std::vector<MouseAt> mouse;  // --mouse TICK:X:Y[:FLAGS]
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value after %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--root") root = next();
        else if (a == "--save-dir") save_dir = next();
        else if (a == "--steps") { steps = std::stoll(next()); steps_given = true; }
        else if (a == "--scene") scene = next();
        else if (a == "--scene-list") scene_list = next();
        else if (a == "--chain") chain = true;  // follow the events the scene chains to
        else if (a == "--choice") choice = std::stoi(next());
        else if (a == "--log") log = next();
        else if (a == "--audio") audio_on = true;  // run the sound engine as the window does, without writing it out
        else if (a == "--wav") wav = next();  // render the mixed sound of the run to a WAV file (turns graphics on)
        else if (a == "--ticks") tick_limit = std::stoll(next());  // stop at this frame (replaying a recorded session)
        else if (a == "--fork") fork_scenes = true;  // with --scene-list: boot once, fork a process per scene (not on Windows)
        else if (a == "--scene-timeout") scene_timeout = std::atoi(next().c_str());  // seconds per forked scene
        else if (a == "--smooth") smooth = true;  // bilinear sampling and soft snow, as in the window
        else if (a == "--shot") {
            std::string spec = next();
            size_t colon = spec.find(':');
            if (colon == std::string::npos) {
                std::fprintf(stderr, "--shot wants TICK:FILE\n");
                return 2;
            }
            shots.emplace_back(std::stoll(spec.substr(0, colon)), spec.substr(colon + 1));
        }
        else if (a == "--mouse") {
            // The mouse from this tick on, at X,Y in script units from the middle of the picture; FLAGS: d = left button
            // held (its first tick is the click), r = right click (one tick), u / n = wheel up / down (one notch).
            std::string spec = next();
            MouseAt m{0, 0, 0, ""};
            size_t p1 = spec.find(':'), p2 = spec.find(':', p1 + 1), p3 = spec.find(':', p2 + 1);
            if (p1 == std::string::npos || p2 == std::string::npos) {
                std::fprintf(stderr, "--mouse wants TICK:X:Y[:FLAGS]\n");
                return 2;
            }
            try {
                m.tick = std::stoll(spec.substr(0, p1));
                m.x = std::stod(spec.substr(p1 + 1, p2 - p1 - 1));
                m.y = std::stod(spec.substr(p2 + 1, p3 == std::string::npos ? p3 : p3 - p2 - 1));
            } catch (const std::exception&) {
                std::fprintf(stderr, "--mouse wants TICK:X:Y[:FLAGS] with numbers\n");
                return 2;
            }
            if (p3 != std::string::npos) m.flags = spec.substr(p3 + 1);
            mouse.push_back(m);
        }
        else if (a == "--press") {
            std::string spec = next();
            int64_t parts[3] = {0, 0, 3};
            int n = 0;
            size_t pos = 0;
            while (n < 3 && pos <= spec.size()) {
                size_t colon = spec.find(':', pos);
                parts[n++] = std::stoll(spec.substr(pos, colon == std::string::npos ? colon : colon - pos), nullptr, 0);
                if (colon == std::string::npos) break;
                pos = colon + 1;
            }
            int64_t frames = n > 2 ? parts[2] : 3;
            presses.emplace_back(parts[0], parts[0] + frames - 1, int(parts[1]));
        } else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }

    if (!scene_list.empty()) {
        std::ifstream list(scene_list);
        for (std::string line; std::getline(list, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty()) queue.push_back(line);
        }
        if (queue.empty()) {
            std::fprintf(stderr, "empty scene list\n");
            return 2;
        }
        scene = queue.front();
        this_scene = scene;
    }
    if (!scene.empty()) {
        // The proven way to the first event: title, name entry, the prologue's map (circle twice).
        // From there a pattern of direction then circle answers every choice and moves the text on.
#ifdef _WIN32
        _putenv_s("AMG_SCENE", scene.c_str());
#else
        setenv("AMG_SCENE", scene.c_str(), 1);
#endif
        if (chain) {
#ifdef _WIN32
            _putenv_s("AMG_SCENE_CHAIN", "1");
#else
            setenv("AMG_SCENE_CHAIN", "1", 1);
#endif
        }
        if (!steps_given) steps = 60'000'000;  // a scene's budget unless it is asked otherwise
        bool full_boot = std::getenv("AMG_SCENE_BOOT") != nullptr;
        int64_t first = 60;
        if (full_boot) {
            for (auto p : {std::make_tuple(2600, 2048), std::make_tuple(2900, 2048), std::make_tuple(3400, 32),
                           std::make_tuple(3800, 2048), std::make_tuple(4000, 32), std::make_tuple(4200, 32),
                           std::make_tuple(4410, 32), std::make_tuple(4500, 32)})
                presses.emplace_back(std::get<0>(p), std::get<0>(p) + 2, std::get<1>(p));
            first = 4600;
        }
        auto_press_first = first;
    }
#ifndef _WIN32
    // A forked child has only the thread that forked: no picture may be half-loaded by a helper thread then.
    if (fork_scenes) setenv("AMG_NO_PRELOAD", "1", 1);
#endif
    try {
        VM vm(root);
        vm.save_dir = save_dir;
        vm.fork_scenes = fork_scenes;
        vm.fork_timeout = scene_timeout;
        vm.coverage = std::getenv("AMG_COVERAGE") != nullptr;
        if (!scene.empty()) {
            // AMG_NO_AUTOPRESS: only the --press buttons (menus driven by random input, tools/menu runs).
            const char* no_auto = std::getenv("AMG_NO_AUTOPRESS");
            vm.auto_press.on = !(no_auto && *no_auto);
            vm.auto_press.first = auto_press_first;
            vm.auto_press.choice = choice;
            vm.auto_press.period = 8 * (choice + 3);
        }
        if (!queue.empty()) {
            vm.scene_queue = queue;
            vm.scene_budget = steps;
            steps = int64_t(1) << 60;  // the budget is per scene (the harness sets the limit before each)
        }
        vm.max_steps = steps;
        vm.profile_prims = std::getenv("AMG_PROFILE_PRIMS") != nullptr;
        vm.presses = presses;
        if (const char* a = std::getenv("AMG_ANALOG")) {  // A0,A1,A2,A3[@TICK]: the sticks held still at these values
            std::string spec = a;                          // from TICK on (tests)
            if (size_t at = spec.find('@'); at != std::string::npos) {
                vm.analog_from = std::atoll(spec.c_str() + at + 1);
                spec.resize(at);
            }
            for (int k = 0, at = 0; k < 4 && at <= int(spec.size()); ++k) {
                size_t end = spec.find(',', size_t(at));
                if (end == std::string::npos) end = spec.size();
                vm.analog[k] = std::atoi(spec.substr(size_t(at), end - size_t(at)).c_str());
                at = int(end) + 1;
            }
        }
        if (const char* m = std::getenv("AMG_MONKEY")) {  // SEED:FIRST:MASK,MASK,...
            std::string spec = m;
            size_t a = spec.find(':'), b = spec.find(':', a == std::string::npos ? a : a + 1);
            if (a != std::string::npos && b != std::string::npos) {
                vm.monkey.on = true;
                vm.monkey.seed = std::strtoull(spec.substr(0, a).c_str(), nullptr, 10);
                vm.monkey.first = std::atoll(spec.substr(a + 1, b - a - 1).c_str());
                for (size_t at = b + 1; at < spec.size();) {
                    size_t end = spec.find(',', at);
                    if (end == std::string::npos) end = spec.size();
                    vm.monkey.buttons.push_back(std::atoi(spec.substr(at, end - at).c_str()));
                    at = end + 1;
                }
            }
        }
        std::unique_ptr<AudioEngine> audio;
        std::vector<int16_t> pcm;
        if (!shots.empty() || !wav.empty() || !scene.empty() || audio_on || !mouse.empty()) {  // the screen-object model is on only when a picture or sound is asked for
            install_graphics(vm);
            vm.graphics->smooth = smooth;
            for (auto& s : shots) vm.shots[s.first] = s.second;
            if (!wav.empty() || audio_on) {
                audio = std::make_unique<AudioEngine>(root);
                install_audio(vm, audio.get());
                vm.graphics->mouth_level = [a = audio.get()](int speaker) { return a->mouth_level(speaker); };
                auto previous = vm.on_tick;
                vm.on_tick = [&, previous](VM& v) {
                    if (previous) previous(v);
                    if (wav.empty()) {  // --audio: the engine advances one frame, the sound is dropped
                        int16_t frame[size_t(AUDIO_RATE / 60) * 2];
                        audio->render(frame, size_t(AUDIO_RATE / 60));
                        return;
                    }
                    size_t at = pcm.size();
                    pcm.resize(at + size_t(AUDIO_RATE / 60) * 2);
                    audio->render(&pcm[at], size_t(AUDIO_RATE / 60));
                };
            }
            vm.on_shot = [](VM& v, const std::string& path) {
                if (amg::cursor_art::in_shots() && v.mouse_shown) {  // AMG_MOUSE_CURSOR_IN_SHOTS=1: the arrow where --mouse put it
                    if (ImagePtr arrow = v.graphics->ui_image(20, 0))  // the pointer is the game's arrow (cursor_art.hpp)
                        amg::cursor_art::set_source(arrow->w, arrow->h, arrow->px.data());
                    std::vector<uint8_t> rgb = v.graphics->render();
                    amg::cursor_art::stamp(rgb, SCREEN_W, SCREEN_H, int(SCREEN_W / 2 + v.mouse_at[0] * VIEW_UNIT),
                                           int(SCREEN_H / 2 + v.mouse_at[1] * VIEW_UNIT), amg::cursor_art::size_for(SCREEN_H));
                    Graphics::write_png(path, rgb, SCREEN_W, SCREEN_H);
                } else {
                    v.graphics->save_png(path);
                }
                std::ofstream f(path + ".txt", std::ios::binary);
                f << v.graphics->describe() << '\n';
            };
        }
        if (!mouse.empty() && vm.graphics) {
            // The headless mouse: the same pointer as the window's (menu_pointer.hpp), fed from the list and the last picture.
            std::sort(mouse.begin(), mouse.end(), [](const MouseAt& a, const MouseAt& b) { return a.tick < b.tick; });
            auto previous = vm.on_tick;
            struct MouseState {
                MenuPointer first;   // the press bursts, for the screens with no registered menu
                bool second = false; // the button was down
                bool mode = false;   // mouse mode: set by a new mouse entry, ended by a --press
                MouseMenus menus;    // the registered menus (mouse_menus.hpp)
            };
            auto state = std::make_shared<MouseState>();
            vm.on_tick = [previous, state, mouse](VM& v) {
                if (previous) previous(v);
                const MouseAt* at = nullptr;
                for (const MouseAt& m : mouse)
                    if (m.tick <= v.tick) at = &m;
                v.pointer_prev = v.pointer_mask;
                v.pointer_mask = 0;
                v.graphics->hide_uid = 0;
                v.graphics->mark = Graphics::SelectionMark();
                v.graphics->hide_image = nullptr;
                if (!at) return;
                const bool edge = at->tick == v.tick;  // flags that are one-tick events
                // Mouse mode (mouse-native-design 2.5): a new place or button of the list starts it, a key or pad
                // press (--press) ends it, and hover waits for the next entry of the list.
                if (edge) state->mode = true;
                for (const auto& p : v.presses)
                    if (std::get<0>(p) == v.tick) {
                        state->mode = false;
                        state->menus.yield();
                    }
                const bool down = at->flags.find('d') != std::string::npos;
                const bool click = down && !state->second;
                state->second = down;
                // X:Y are script units of the picture as shown (picture pixels / VIEW_UNIT from the middle), the same as the
                // window's mouse after the letterbox; the pointer compares with the scene as drawn (Graphics::pointer_scene).
                const Scene seen = Graphics::pointer_scene(*v.graphics->scene());
                const double tu = seen.ui_u > 0 ? seen.ui_u : 1.0;
                v.touch.update(down, seen.ui_ax + (at->x - seen.ui_ax) / tu, seen.ui_ay + (at->y - seen.ui_ay) / tu, v.tick);
                v.mouse_shown = true;
                v.mouse_at[0] = at->x;
                v.mouse_at[1] = at->y;
                const Scene* scene = &seen;
                MenuPictures pics;
                pics.cursor = v.graphics->ui_image(20, 0);
                pics.tab = v.graphics->ui_image(47, 0);
                pics.note = v.graphics->ui_image(33, 0);
                pics.back = v.graphics->ui_image(62, 2);
                pics.arrow = v.graphics->ui_image(7, 0);
                pics.map_cursor = v.graphics->ui_image(25, 0);
                for (int k = 0; k < 4; ++k) pics.map_arrow[k] = v.graphics->ui_image(28 + k, 0);
                const int wheel = edge ? (at->flags.find('u') != std::string::npos) - (at->flags.find('n') != std::string::npos) : 0;
                const bool right = edge && at->flags.find('r') != std::string::npos;
                int menu_mask = 0;
                const bool native = state->menus.update(v, *scene, at->x, at->y, true, down, click, right, wheel, menu_mask);
                if (native) {
                    state->first.yield();
                    v.pointer_mask = menu_mask;
                    if (state->mode) {
                        v.graphics->hide_uid = state->menus.arrow_uid;
                        v.graphics->mark = state->menus.mark;
                        v.graphics->hide_image = pics.cursor;  // every menu arrow, not only the adapter's own
                    }
                } else {
                    v.pointer_mask = state->first.update(*scene, pics, at->x, at->y, true, down, click, right, wheel);
                }
                static const bool log = std::getenv("AMG_MOUSE_LOG") != nullptr;
                if (log) {  // one line per tick: the screen kind, the entry the game's cursor is on, the one under the mouse
                    const MenuPointer::Probe& p = native ? state->menus.probe : state->first.probe;
                    // arrow: the game's menu arrows (interface picture 20) drawn this frame in mouse mode (the audit wants 0
                    // on a covered screen, tools/mouse_latency.py --check)
                    int arrows = 0;
                    if (state->mode)
                        for (const DrawItem& it : seen.items)
                            if (it.image && it.image == pics.cursor && it.alpha > 0.1 && it.uid != v.graphics->hide_uid && it.image != v.graphics->hide_image) ++arrows;
                    std::fprintf(stderr, "mouse %lld %c sel=%s%.0f,%.0f tgt=%s%.0f,%.0f on=%d mask=%d arrow=%d\n", (long long)v.tick, p.kind,
                                 p.has_sel ? "" : "!", p.sel_x, p.sel_y, p.has_tgt ? "" : "!", p.tgt_x, p.tgt_y, int(p.on), v.pointer_mask, arrows);
                }
            };
        }
        // AMG_PROGRESS=N: every N ticks a line with the tick, the instructions, the main thread's innermost method and
        // what the scripts printed since the last line, written at once (long runs can be followed while they go).
        if (const char* every = std::getenv("AMG_PROGRESS")) {
            int64_t n = std::max<int64_t>(1, std::atoll(every));
            auto previous = vm.on_tick;
            auto printed = std::make_shared<size_t>(0);
            vm.on_tick = [previous, n, printed](VM& v) {
                if (previous) previous(v);
                if (v.tick % n) return;
                std::string at = "-";
                for (auto& th : v.threads)
                    if (th->label == "main" && !th->frames.empty())
                        at = th->frames.back().method->cls->name + "." + th->frames.back().method->name;
                size_t live = 0, actors = 0;  // live script threads, and those of Actor objects
                for (auto* t : v.live_threads)
                    if (!t->done) {
                        ++live;
                        ClassObj* k = t->receiver.tag == Tag::Obj ? v.class_of(t->receiver) : nullptr;
                        if (k && k->name == "Actor") ++actors;
                    }
                std::printf("PROGRESS tick=%lld steps=%lld at=%s threads=%zu actors=%zu\n", (long long)v.tick,
                            (long long)v.steps, at.c_str(), live, actors);
                for (size_t i = std::min(*printed, v.out.size()); i < v.out.size(); ++i) std::printf("out: %s\n", v.out[i].c_str());
                *printed = v.out.size();
                std::fflush(stdout);
            };
        }
        // AMG_VIEW_LOG=1: every tick, the drawn interface items as the view places them (uid, layer, centre, size,
        // alpha), for tools/scale_stability.py (docs/technical/interface-scale.md, "Stability").
        if (std::getenv("AMG_VIEW_LOG")) {
            auto previous = vm.on_tick;
            vm.on_tick = [previous](VM& v) {
                if (previous) previous(v);
                if (!v.graphics || !v.graphics->scene()) return;
                const Scene seen = Graphics::view_scene(*v.graphics->scene());
                std::string at = "-";
                for (auto& th : v.threads)
                    if (th->label == "main" && !th->frames.empty()) at = th->frames.back().method->cls->name;
                std::printf("V %lld %s %zu\n", (long long)v.tick, at.c_str(), seen.items.size());
                // AMG_VIEW_LOG=2 adds the item's anchor, its box size (screen pixels) and flags (t text, c content, s shrunk).
                static const bool more = std::strcmp(std::getenv("AMG_VIEW_LOG"), "2") == 0;
                for (const DrawItem& it : seen.items)
                    if (it.layer >= UI_FIRST_LAYER && !it.snow && it.alpha > 0.01) {
                        std::printf("%lld %lld %.2f %.2f %.3f %.3f %.2f", (long long)it.uid, (long long)it.layer, it.centre[0],
                                    it.centre[1], it.scale[0], it.scale[1], it.alpha);
                        if (more)
                            std::printf(" %.2f %.2f %.1f %.1f %c%c%c", it.ui_ax, it.ui_ay, std::fabs(it.rect[2] * it.scale[0]),
                                        std::fabs(it.rect[3] * it.scale[1]), it.text ? 't' : '-', it.content ? 'c' : '-',
                                        it.shrunk ? 's' : '-');
                        std::printf("\n");
                    }
            };
        }
        g_running = &vm;
        std::signal(SIGTERM, stop_run);
        std::signal(SIGINT, stop_run);
        if (tick_limit > 0) {
            auto previous = vm.on_tick;
            vm.on_tick = [previous, tick_limit](VM& v) {
                if (previous) previous(v);
                if (v.tick >= tick_limit) v.max_steps = 0;  // every script thread ends at its next instruction
            };
        }
#ifdef AMG_PERSONAL
        // AMG_TEXT_DUMP=1: the on-screen text, when it becomes stable (docs/technical/personal.md); personal build only.
        std::shared_ptr<amg::personal::TextTracker> text_tracker;
        std::shared_ptr<long long> text_tick = std::make_shared<long long>(0);
        if (std::getenv("AMG_TEXT_DUMP")) {
            text_tracker = std::make_shared<amg::personal::TextTracker>();
            text_tracker->history = [text_tick](const std::string& t) {
                std::string flat;
                for (char c : t) flat += c == '\n' ? std::string(" / ") : std::string(1, c);
                std::printf("TEXT tick %lld: %s\n", *text_tick, flat.c_str());
            };
            auto previous = vm.on_tick;
            vm.on_tick = [previous, text_tracker, text_tick](VM& v) {
                if (previous) previous(v);
                *text_tick = v.tick;
                text_tracker->feed(*v.graphics->scene(), v.tick);
            };
        }
#endif
        amg::Sampler sampler;  // AMG_SAMPLE=FILE (sampler.hpp)
        std::string error = vm.run();
#ifdef AMG_PERSONAL
        if (text_tracker) text_tracker->finish();
#endif
        if (!scene.empty() && !(vm.scene_queue.size() > 1 && vm.scene_done)) {
            // The scene that was running: with a list, the last one started (the earlier ones printed
            // their own DONE line).
            const std::string& scene = vm.scene_name.empty() ? this_scene : vm.scene_name;
            const char* status = vm.scene_done ? "DONE" : error.empty() ? "HANG" : "ERROR";
            std::string first = error.substr(0, error.find('\n'));
            std::printf("SCENE %s %s ticks=%lld steps=%lld%s%s\n", scene.c_str(), status, (long long)vm.tick,
                        (long long)vm.steps, vm.scene_done || error.empty() ? "" : " ", vm.scene_done ? "" : first.c_str());
            // Where it stopped (the main thread's innermost frame) and which stubbed primitives it used.
            std::string at, stubs;
            // An uncaught error carries its own trace: the first "at Class.method" line is where it was thrown.
            size_t pos = error.find("    at ");
            if (pos != std::string::npos) {
                size_t end = error.find_first_of(" \n", pos + 7);
                at = error.substr(pos + 7, end == std::string::npos ? std::string::npos : end - pos - 7);
            }
            for (auto& th : vm.threads)
                if (at.empty() && th->label == "main" && !th->frames.empty())
                    at = th->frames.back().method->cls->name + "." + th->frames.back().method->name;
            for (const auto& h : vm.stub_hits) stubs += (stubs.empty() ? "" : ",") + h;
            std::string chained;
            for (const auto& n : vm.chain_names) chained += (chained.empty() ? "" : ">") + n;
            std::string terr;
            for (const auto& e : vm.thread_errors) terr += (terr.empty() ? "" : ",") + e.substr(0, 60);
            std::printf("SCENEINFO %s at=%s stubs=%s chain=%s terr=%s\n", scene.c_str(), at.empty() ? "-" : at.c_str(),
                        stubs.empty() ? "-" : stubs.c_str(), chained.empty() ? "-" : chained.c_str(),
                        terr.empty() ? "-" : terr.c_str());
            if (vm.coverage) write_coverage(vm, scene);
        }
        if (vm.coverage && scene.empty()) write_coverage(vm, "session");  // a replayed session (tools/replay.py)
        // AMG_METHOD_PROFILE=FILE: the script methods by instructions run, most first (where a slow run spends its time).
        if (const char* path = std::getenv("AMG_METHOD_PROFILE")) {
            std::vector<std::pair<uint64_t, std::string>> rows;
            for (auto& [cname, cls] : vm.classes)
                for (auto* table : {&cls->imethods, &cls->cmethods})
                    for (auto& [key, callable] : *table)
                        if (callable.method && callable.method->executed && callable.method->cls == cls.get())
                            rows.push_back({callable.method->executed, cname + (table == &cls->cmethods ? "." : "#") +
                                                                           callable.method->name + "/" +
                                                                           std::to_string(callable.method->args)});
            std::sort(rows.rbegin(), rows.rend());
            std::ofstream out(path);
            for (size_t i = 0; i < rows.size() && i < 200; ++i) out << rows[i].first << ' ' << rows[i].second << '\n';
        }
        if (vm.scene_queue.size() > 1 && vm.scene_done) std::printf("BATCH DONE %zu\n", vm.scene_queue.size());
        std::printf("--- stopped after %lld instructions, tick %lld\n", (long long)vm.steps, (long long)vm.tick);
        if (!error.empty()) std::printf("%s\n", error.c_str());
        size_t from = vm.out.size() > 20 ? vm.out.size() - 20 : 0;
        for (size_t i = from; i < vm.out.size(); ++i) std::printf("out: %s\n", vm.out[i].c_str());
        for (auto& th : vm.threads) {
            if (th->frames.empty()) continue;
            std::printf("thread %s (wake at %lld):\n", th->label.c_str(), (long long)th->wake_tick);
            size_t shown = 0;
            for (size_t i = th->frames.size(); i-- > 0 && shown < 12; ++shown)
                std::printf("    at %s.%s (instruction %d)\n", th->frames[i].method->cls->name.c_str(),
                            th->frames[i].method->name.c_str(), th->frames[i].pc);
        }
        if (!wav.empty()) {  // 16-bit stereo PCM at 44.1 kHz, one frame's worth per script frame
            std::ofstream w(wav, std::ios::binary);
            auto u32 = [&](uint32_t x) { for (int i = 0; i < 4; ++i) w.put(char((x >> (8 * i)) & 0xFF)); };
            auto u16 = [&](uint16_t x) { w.put(char(x & 0xFF)); w.put(char(x >> 8)); };
            uint32_t bytes = uint32_t(pcm.size() * 2);
            w.write("RIFF", 4); u32(36 + bytes); w.write("WAVEfmt ", 8); u32(16); u16(1); u16(2); u32(AUDIO_RATE);
            u32(AUDIO_RATE * 4); u16(4); u16(16); w.write("data", 4); u32(bytes);
            w.write(reinterpret_cast<const char*>(pcm.data()), std::streamsize(bytes));
        }
        if (!log.empty()) {
            std::ofstream f(log, std::ios::binary);
            for (auto& line : vm.out) f << line << "\n";
        }
        std::fflush(stdout);
        std::_Exit(0);  // other script threads are parked on their baton; do not unwind them
    } catch (std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
