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

#include "ahx.hpp"
#include "audio.hpp"
#include "gfx.hpp"
#include "vm.hpp"

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
        else if (a == "--adx") {  // development aid: decode one file of an AFS at its own rate: ARCHIVE:INDEX:OUT.wav
            std::string spec = next();
            size_t c1 = spec.find(':'), c2 = spec.rfind(':');
            Afs afs;
            AdxStream s;
            size_t index = c1 == std::string::npos || c1 == c2 ? 0 : size_t(std::stoul(spec.substr(c1 + 1, c2 - c1 - 1)));
            if (c1 == std::string::npos || c1 == c2 || !afs.open(spec.substr(0, c1)) || index >= afs.files.size() ||
                !s.load(afs.path, afs.files[index].first, afs.files[index].second, "mituba")) {
                std::fprintf(stderr, "cannot decode %s\n", spec.c_str());
                return 1;
            }
            std::vector<int16_t> pcm;
            int16_t l, r;
            while (s.next(l, r) && pcm.size() < size_t(s.samples) * 2) {
                pcm.push_back(l);
                pcm.push_back(r);
                if (s.looped && s.position >= s.loop_end) break;
            }
            std::ofstream w(spec.substr(c2 + 1), std::ios::binary);
            auto u32 = [&](uint32_t x) { for (int i = 0; i < 4; ++i) w.put(char((x >> (8 * i)) & 0xFF)); };
            auto u16 = [&](uint16_t x) { w.put(char(x & 0xFF)); w.put(char(x >> 8)); };
            uint32_t nbytes = uint32_t(pcm.size() * 2);
            w.write("RIFF", 4); u32(36 + nbytes); w.write("WAVEfmt ", 8); u32(16); u16(1); u16(2); u32(uint32_t(s.rate));
            u32(uint32_t(s.rate) * 4); u16(4); u16(16); w.write("data", 4); u32(nbytes);
            w.write(reinterpret_cast<const char*>(pcm.data()), std::streamsize(nbytes));
            return 0;
        }
        else if (a == "--se") {  // development aid: list a sound-effect bank and write one note: BANK[:NOTE:OUT.wav]
            std::string spec = next();
            int bank = std::stoi(spec);
            SeBank b;
            if (!b.load(root + "/extracted/SOUND/MUSIC.ARC", bank)) {
                std::fprintf(stderr, "cannot load bank %d\n", bank);
                return 1;
            }
            std::printf("bank %d: %zu samples, %zu splits\n", bank, b.vags.size(), b.splits.size());
            for (size_t i = 0; i < b.vags.size(); ++i)
                std::printf("  vag %2zu: %6zu samples at %d Hz%s\n", i, b.vags[i].pcm->size(), b.vags[i].rate,
                            b.vags[i].looped ? " (loops)" : "");
            size_t c1 = spec.find(':'), c2 = spec.rfind(':');
            if (c1 != std::string::npos && c2 != c1) {
                int note = std::stoi(spec.substr(c1 + 1, c2 - c1 - 1)), volume = 0;
                const SeBank::Sample* sample = b.lookup(note, volume);
                if (!sample) {
                    std::fprintf(stderr, "note %d plays nothing\n", note);
                    return 1;
                }
                std::ofstream w(spec.substr(c2 + 1), std::ios::binary);
                auto u32 = [&](uint32_t x) { for (int i = 0; i < 4; ++i) w.put(char((x >> (8 * i)) & 0xFF)); };
                auto u16 = [&](uint16_t x) { w.put(char(x & 0xFF)); w.put(char(x >> 8)); };
                uint32_t bytes = uint32_t(sample->pcm->size() * 2);
                w.write("RIFF", 4); u32(36 + bytes); w.write("WAVEfmt ", 8); u32(16); u16(1); u16(1);
                u32(uint32_t(sample->rate)); u32(uint32_t(sample->rate) * 2); u16(2); u16(16);
                w.write("data", 4); u32(bytes);
                w.write(reinterpret_cast<const char*>(sample->pcm->data()), std::streamsize(bytes));
                std::printf("note %d: volume %d, %zu samples\n", note, volume, sample->pcm->size());
            }
            return 0;
        }
        else if (a == "--ahx") {  // development aid: decode a voice file: SPEAKER:LINE_FILE_INDEX:OUT.wav
            std::string spec = next();
            size_t c1 = spec.find(':'), c2 = spec.rfind(':');
            if (c1 == std::string::npos || c1 == c2) {
                std::fprintf(stderr, "--ahx wants SPEAKER:INDEX:OUT.wav\n");
                return 2;
            }
            size_t speaker = size_t(std::stoul(spec.substr(0, c1))), index = size_t(std::stoul(spec.substr(c1 + 1, c2 - c1 - 1)));
            Afs outer;
            if (!outer.open(root + "/extracted/SOUND/VOICE.AFS") || speaker >= outer.files.size()) {
                std::fprintf(stderr, "cannot open VOICE.AFS\n");
                return 1;
            }
            std::ifstream f(outer.path, std::ios::binary);
            std::string inner;
            f.seekg(outer.files[speaker].first);
            inner.resize(outer.files[speaker].second);
            f.read(inner.data(), std::streamsize(inner.size()));
            auto le = [&](size_t at) {
                return uint32_t(uint8_t(inner[at])) | (uint32_t(uint8_t(inner[at + 1])) << 8) |
                       (uint32_t(uint8_t(inner[at + 2])) << 16) | (uint32_t(uint8_t(inner[at + 3])) << 24);
            };
            uint32_t count = le(4);
            if (index >= count) {
                std::fprintf(stderr, "speaker has %u files\n", count);
                return 1;
            }
            std::string file = inner.substr(le(8 + 8 * index), le(12 + 8 * index));
            std::vector<int16_t> pcm;
            int rate = 0;
            if (!decode_ahx(file, pcm, rate)) {
                std::fprintf(stderr, "not a decodable AHX file (%zu bytes)\n", file.size());
                return 1;
            }
            std::ofstream w(spec.substr(c2 + 1), std::ios::binary);
            auto u32 = [&](uint32_t x) { for (int i = 0; i < 4; ++i) w.put(char((x >> (8 * i)) & 0xFF)); };
            auto u16 = [&](uint16_t x) { w.put(char(x & 0xFF)); w.put(char(x >> 8)); };
            uint32_t nbytes = uint32_t(pcm.size() * 2);
            w.write("RIFF", 4); u32(36 + nbytes); w.write("WAVEfmt ", 8); u32(16); u16(1); u16(1);
            u32(uint32_t(rate)); u32(uint32_t(rate) * 2); u16(2); u16(16);
            w.write("data", 4); u32(nbytes);
            w.write(reinterpret_cast<const char*>(pcm.data()), std::streamsize(nbytes));
            std::printf("%zu samples at %d Hz\n", pcm.size(), rate);
            return 0;
        }
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
        if (!shots.empty() || !wav.empty() || !scene.empty() || audio_on) {  // the screen-object model is on only when a picture or sound is asked for
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
                v.graphics->save_png(path);
                std::ofstream f(path + ".txt", std::ios::binary);
                f << v.graphics->describe() << '\n';
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
        std::string error = vm.run();
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
