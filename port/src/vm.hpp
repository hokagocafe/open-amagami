// The script virtual machine of the Windows port.
//
// This is a C++ rendition of tools/scfvm.py, the reference interpreter, which is the executable
// specification (docs/technical/scf-vm.md). Where the two differ it is a bug here, except for the
// points listed under "Known differences" in docs/technical/port.md.
#pragma once

#include <cmath>
#include <condition_variable>
#include <set>
#include <unordered_set>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace amg {

struct VM;
struct Graphics;
struct Thread;
struct ClassObj;
struct Method;

// ---------------------------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------------------------

enum class Tag : uint8_t { Nil, True, False, Int, Float, Str, Sym, Arr, Obj, Class };

struct Heap {
    // Creation order: Object.hashCode answers this, so hash tables iterate the same way on every
    // platform (an address would differ between builds and runs).
    uint32_t serial = next_serial();
    virtual ~Heap() = default;
    static uint32_t next_serial() {
        static uint32_t counter = 0;
        return ++counter;
    }
};

struct Value {
    Tag tag = Tag::Nil;
    union {
        int64_t i;
        double f;
    };
    std::shared_ptr<Heap> p;

    Value() : i(0) {}
    static Value nil() { return Value(); }
    static Value boolean(bool b) {
        Value v;
        v.tag = b ? Tag::True : Tag::False;
        return v;
    }
    static Value integer(int64_t n) {
        Value v;
        v.tag = Tag::Int;
        v.i = n;
        return v;
    }
    static Value real(double d) {
        Value v;
        v.tag = Tag::Float;
        v.f = d;
        return v;
    }
    bool is_int() const { return tag == Tag::Int; }
    bool is_float() const { return tag == Tag::Float; }
    bool is_bool() const { return tag == Tag::True || tag == Tag::False; }
    bool truthy() const { return tag != Tag::Nil && tag != Tag::False; }
};

struct StrObj : Heap {
    std::string b;  // Shift-JIS bytes, as stored on disc
};

struct SymObj : Heap {
    std::string name;
    int id = 0;
};

struct ArrObj : Heap {
    ClassObj* cls = nullptr;
    std::vector<Value> items;
};

struct ObjObj : Heap {
    ClassObj* cls = nullptr;
    std::vector<Value> iv;
    std::string message;        // error objects
    Thread* thread = nullptr;   // Thread objects
    std::vector<std::string> trace;
    bool has_trace = false;
};

using PrimFn = std::function<Value(VM&, Thread&, std::vector<Value>& stack, size_t base, Method&)>;
using NativeFn = std::function<Value(VM&, Thread&, Value& recv, std::vector<Value>& args)>;

struct Instr {
    uint8_t op;
    int32_t a;
    int32_t b;
};

struct Method : Heap {
    ClassObj* cls = nullptr;
    std::string name;
    int args = 0;
    std::vector<Instr> code;
    bool ran = false;  // a frame of it was pushed (coverage, AMG_COVERAGE)
    uint64_t executed = 0;  // instructions run in it (AMG_METHOD_PROFILE)
};

struct Callable {
    std::shared_ptr<Method> method;  // a script method, or
    NativeFn native;                 // a built-in
    int native_id = 0;               // 1: Object.toString (text_of must not call it back)
    explicit operator bool() const { return method != nullptr || static_cast<bool>(native); }
};

struct ConstantSource;

struct ClassSource {
    uint8_t flags = 0;
    std::string name, parent;
    std::vector<std::pair<int, std::string>> instance_vars, class_vars;
    struct MethodSource {
        std::string name;
        int args;
        std::string code;
    };
    std::vector<MethodSource> methods, class_methods;
    struct Constant {
        uint8_t tag = 0;
        int32_t i = 0;
        float f = 0;
        std::string s;
        std::vector<Constant> items;
    };
    std::vector<Constant> constants;
};

struct ClassObj : Heap {
    std::string name;
    ClassObj* parent = nullptr;
    int size = 0;
    std::unordered_map<uint64_t, Callable> imethods, cmethods;
    std::vector<Value> consts;
    bool consts_ready = false;
    std::shared_ptr<ClassSource> src;
    std::map<int, PrimFn> prims;
    // Class variables and the class object's own instance variables are one table.
    std::vector<Value> iv = std::vector<Value>(256);

    bool is_kind_of(const ClassObj* other) const {
        for (const ClassObj* c = this; c; c = c->parent)
            if (c == other) return true;
        return false;
    }
};

struct ScriptThrow {
    Value value;
    std::vector<std::string> trace;
};

struct MissingNative {
    std::string what;
};

struct VMExit {};

// ---------------------------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------------------------

struct Frame {
    Method* method;
    int pc;
    size_t base;
};

struct TryEntry {
    size_t depth;
    int handler;
    size_t height;
};

struct Thread {
    VM* vm = nullptr;
    std::string label;
    std::vector<Value> stack;
    std::vector<Frame> frames;
    std::vector<TryEntry> tries;
    Value caught;
    Value receiver;
    std::string name;
    std::vector<Value> args;
    int64_t wake_tick = 0;
    bool done = false;
    Thread* parent = nullptr;
    Value object;  // the script-visible Thread object, if any
    std::thread os;
    std::condition_variable wake;  // signalled when this thread receives the baton
    void* fiber = nullptr;         // Windows: each script thread is a fiber of the machine's one OS thread
    bool fiber_freed = false;
    std::vector<std::shared_ptr<Method>> keep;

    void start();
    void sleep(int64_t frames);
    Value invoke(Value recv, const std::shared_ptr<Method>& method, std::vector<Value> args);

    void body();  // runs the script thread's method and handles whatever it throws
private:
    void schedule_next(bool finished);
    void wait_for_turn();
    Value execute(size_t stop_depth);
    Value loop(size_t stop_depth);
    void arith(uint8_t op);
};

// ---------------------------------------------------------------------------------------------
// The machine
// ---------------------------------------------------------------------------------------------

struct VM {
    explicit VM(std::string root);

    std::string root;
    // Scene harness (`amagami_vm --scene NAME`): one event scene is run in the game's skip mode and
    // the machine stops when it returns.
    // Automatic input for the scene harness, computed from the tick (no list to scan): in cycles of
    // `period` ticks, choice+1 presses of down (8 ticks apart) and then a circle.
    struct AutoPress {
        bool on = false;
        int64_t first = 60, period = 24;
        int choice = 0;
        int mask_at(int64_t t) const {
            if (!on || t < first) return 0;
            int64_t phase = (t - first) % period, k = phase / 8, r = phase % 8;
            if (r >= 3) return 0;
            if (k <= choice) return 16384;
            return k == choice + 1 ? 32 : 0;
        }
    } auto_press;
    // Random buttons from a seed (AMG_MONKEY=SEED:FIRST:MASK,MASK,...; repeat a mask to weigh it): from tick FIRST, one
    // button of the list is held for 4 ticks in every slot of 20, chosen by hashing the seed and the slot. A function
    // of the tick, so long runs need no list of presses (tools: the menu and game bots).
    struct Monkey {
        bool on = false;
        uint64_t seed = 0;
        int64_t first = 0;
        std::vector<int> buttons;
        int mask_at(int64_t t) const {
            if (!on || t < first || buttons.empty()) return 0;
            int64_t slot = (t - first) / 20, r = (t - first) % 20;
            if (r >= 4) return 0;
            uint64_t h = (seed + uint64_t(slot)) * 0x9E3779B97F4A7C15ull;
            h ^= h >> 31;
            h *= 0xBF58476D1CE4E5B9ull;
            h ^= h >> 29;
            return buttons[size_t(h % buttons.size())];
        }
    } monkey;
    // Scenes run one after another in one process (`--scene-list FILE`): the boot is most of a scene's cost.
    std::vector<std::string> scene_queue;
    int64_t scene_budget = 0;  // instruction budget of each scene of the queue
    std::set<std::string> stub_hits;  // "Class.index" of every stubbed primitive that ran (test harness report)
    bool scene_chain = false;               // --chain: let the chain of events go on until it ends
    std::vector<std::string> chain_names;   // the events the chain went through, in order
    std::string scene_name;
    bool scene_started = false, scene_done = false;
    std::string save_dir;  // empty: AMG_SAVE_DIR, else %APPDATA%\open-amagami\save (Windows) or ./save
    std::unordered_map<std::string, std::shared_ptr<ClassSource>> sources;
    std::unordered_map<std::string, std::shared_ptr<ClassObj>> classes;
    std::unordered_map<std::string, std::shared_ptr<SymObj>> symbols;
    std::vector<std::string> out;
    std::vector<std::unique_ptr<Thread>> threads;
    // The threads that have not ended, and those that ended but still hold a fiber. Every thread stays in `threads`
    // (script Thread objects keep a pointer to theirs), but the scheduler walks only these: scanning every thread
    // ever made on each hand-over grew with the length of the session and slowed long runs down (profiled).
    std::vector<Thread*> live_threads, ended_threads;
    int64_t tick = 0;
    int64_t steps = 0, max_steps = 5'000'000;
    std::string error;
    std::vector<std::string> thread_errors;
    // Coverage (AMG_COVERAGE=DIR): which native methods (selector key) and primitives (class, number) ran; script
    // methods carry their own flag. Written per scene by main.
    bool coverage = false;
    std::unordered_set<uint64_t> natives_used;
    std::set<std::string> image_classes;  // the system image's classes, always loaded (Class.findClass)
    // The class that holds each native method run (coverage: a script method the port runs natively counts as run).
    std::set<std::tuple<const ClassObj*, uint64_t, bool>> natives_owned;
    std::set<std::pair<const ClassObj*, int>> prims_used;
    bool fork_scenes = false, fork_child = false;  // --fork: one boot, one forked process per scene
    int fork_timeout = 900;                         // seconds of wall clock per forked scene  // uncaught exceptions that ended a secondary thread
    bool finished = false;
    Thread* current = nullptr;
    void* driver_fiber = nullptr;  // the fiber that called run()
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::tuple<int64_t, int64_t, int>> presses;  // first tick, last tick, button mask
    std::function<void(VM&)> on_tick;
    std::map<int64_t, std::string> shots;  // frame -> file: save the screen when the clock reaches it
    std::function<void(VM&, const std::string&)> on_shot;
    Graphics* graphics = nullptr;
    bool profile_prims = false;  // print primitives that take long (AMG_PROFILE_PRIMS)
    // Live input (a window). When set it replaces the scripted `presses`; the held mask is sampled
    // once per frame and a button counts as newly pressed on the frame it goes down.
    std::function<int()> live_buttons;
    int live_held = 0, live_pressed = 0;
    // The Vita pad's auto-repeat (0x81001AEC): frames the same nonzero mask has been held, 0 to 20, then 21 to 24
    // and back to 20; `rapid` adds the held mask on every frame it is 20 (first repeat after 20 frames, then every 5).
    int rapid_count = 0, rapid_last = 0;
    // The PS2's (func_00100A48, 0x100B10): the count goes up while the mask is unchanged (it is reset when it changes);
    // at 21 `rapid` is the held mask and the count drops by 4. First repeat after 21 frames, then every 4.
    int rapid_count_ps2 = 0;
    bool rapid_fire_ps2 = false;
    int buttons_rapid(bool ps2_layout = false) const;
    // The analog sticks as the engine keeps them (func_00100A48): the pad's raw byte minus 128, no dead zone;
    // 0 and 1 the right stick (horizontal, vertical), 2 and 3 the left one. 0 is the centre.
    int analog[4] = {0, 0, 0, 0};
    int64_t analog_from = 0;  // the sticks read as centred before this tick (AMG_ANALOG's @TICK, tests)

    // The Vita's touch screen, fed by the mouse (F5-8): a left press, its movement and its release, in the
    // scripts' units (480x272, origin at the centre). `update` runs once per frame, like the pad. A release that
    // moved little and quickly is a tap; a press that moves more than a few units is a drag, with an id that
    // counts up (0 means none). Pinch and rotation never start: no script uses them.
    struct Touch {
        bool down = false, drag_active = false;
        double x = 0, y = 0, press_x = 0, press_y = 0;
        int64_t press_tick = 0, tap_tick = -100;
        double tap_x = 0, tap_y = 0;
        int drag_id = 0, ended_id = 0, next_id = 1;
        void cancel() {
            down = false;
            drag_active = false;
            drag_id = 0;
            ended_id = 0;
            tap_tick = -100;
        }
        void update(bool now_down, double nx, double ny, int64_t tick) {
            if (now_down && !down) {
                press_x = nx;
                press_y = ny;
                press_tick = tick;
            }
            if (now_down && !drag_active && std::hypot(nx - press_x, ny - press_y) > 6.0) {
                drag_active = true;
                drag_id = next_id++;
                ended_id = 0;
            }
            if (!now_down && down) {
                if (drag_active) {
                    drag_active = false;
                    ended_id = drag_id;
                } else if (tick - press_tick <= 40) {
                    tap_tick = tick;
                    tap_x = nx;
                    tap_y = ny;
                }
            }
            if (now_down || down) {
                x = nx;
                y = ny;
            }
            down = now_down;
        }
    } touch;

    // names
    int intern(const std::string& name);
    std::vector<std::string> names;
    std::unordered_map<std::string, int> name_ids;
    static uint64_t key(int name_id, int argc) { return (uint64_t(name_id) << 8) | uint64_t(argc & 0xFF); }

    // classes
    ClassObj* find_class(const std::string& name);
    ClassObj* define_class(const std::shared_ptr<ClassSource>& src);
    const std::vector<Value>& constants(ClassObj* cls);
    Value constant(const ClassSource::Constant& c);
    ClassObj* class_of(const Value& v);
    Value symbol(const std::string& name);

    // objects
    Value make_str(std::string s);
    Value make_arr(ClassObj* cls, std::vector<Value> items);
    Value make_obj(ClassObj* cls);
    Value error_object(const std::string& cls, const std::string& message);
    [[noreturn]] void raise(const std::string& message, const std::string& cls = "Error");

    // calling
    Callable lookup(const Value& recv, int name_id, int argc);
    Value send(Thread& th, Value recv, const std::string& name, std::vector<Value> args);
    Value call(Value recv, const Callable& c, std::vector<Value> args, Thread* th);
    Value instantiate(ClassObj* cls, std::vector<Value> args, Thread* th);

    // threads
    Thread* new_thread(Value receiver, const std::string& name, std::vector<Value> args, const std::string& label);
    std::string run();
    void advance(int64_t to);

    // input
    // `ps2_layout`: hand the scripts the PS2 engine's mask as it is. The Vita's scenario scripts (mahjong's table, the
    // events) still test it, while its system image tests SceCtrl's (see to_script_buttons).
    int buttons_held(bool ps2_layout = false) const;
    int buttons_pressed(bool ps2_layout = false) const;
    // The PS Vita system image reads the buttons in SceCtrl's layout (circle 0x2000, up 0x10, ...), which is the
    // PS2 engine's mask with its two bytes swapped; every input source of the port speaks the PS2's.
    bool vita_buttons = false;
    int to_script_buttons(int ps2_mask) const;

    // loading
    // The port's own script classes (port/script/*.scf, compiled with hokago-tools): where they are, "" for none. They are
    // loaded with the game's and hook into its screens (F5-11); set before the VM is made.
    static std::string script_dir;
    // ControlPad.actuator(strength 0..255, on): the pad's rumble (the Settings screen tests it); null when there is no pad.
    std::function<void(double strength, bool on)> on_rumble;
    void load_sources();
    void patch_config_page(ClassObj* cls);
    void install_builtins();
};

bool same(const Value& x, const Value& y);
std::string text_of(VM& vm, Thread& th, const Value& v);
std::string describe(VM& vm, const Value& v);

// loader.cpp
std::string lzss_unpack(const std::string& data);
bool parse_scf(const std::string& data, size_t& pos, ClassSource& out);
std::vector<std::shared_ptr<ClassSource>> read_image(const std::string& image);
std::vector<std::string> read_arc(const std::string& data);
std::string read_file(const std::string& path);

// natives.cpp
void install_natives(VM& vm);

// text helpers (strings are arrays of 16-bit characters; Shift-JIS pairs are one code)
std::vector<int> chars(const std::string& data);
std::string unchars(const std::vector<int>& codes);

}  // namespace amg
