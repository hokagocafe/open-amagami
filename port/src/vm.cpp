// The machine: class table, calling convention, threads and the interpreter loop.
// Mirrors tools/scfvm.py instruction by instruction.
#include "vm.hpp"
#include "cpk.hpp"
#include "datapath.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <stdexcept>

#if defined(_WIN32) && !defined(AMG_NO_FIBERS)
#define AMG_FIBERS 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif !defined(AMG_NO_FIBERS)
// Elsewhere the same fiber calls on top of ucontext: every script thread is a coroutine of one OS thread,
// so a hand-over is one swapcontext instead of a trip through the kernel's scheduler (with one OS thread
// per script thread the sweeps spent three times longer in the kernel than in the interpreter), and the
// process can be forked between scenes (an OS thread does not survive a fork).
#define AMG_FIBERS 1
#define CALLBACK
#include <ucontext.h>
#include <cstdlib>
namespace {
struct Coroutine {
    ucontext_t ctx;
    void* stack = nullptr;
    void (*entry)(void*) = nullptr;
    void* arg = nullptr;
};
Coroutine* running_coroutine = nullptr;

void coroutine_start() {
    Coroutine* c = running_coroutine;
    c->entry(c->arg);
}

void* ConvertThreadToFiber(void*) {
    running_coroutine = new Coroutine;
    return running_coroutine;
}
void ConvertFiberToThread() {
    delete running_coroutine;
    running_coroutine = nullptr;
}
void* GetCurrentFiber() { return running_coroutine; }
void* CreateFiber(size_t size, void (*entry)(void*), void* arg) {
    auto* c = new Coroutine;
    c->entry = entry;
    c->arg = arg;
    c->stack = std::malloc(size);
    getcontext(&c->ctx);
    c->ctx.uc_stack.ss_sp = c->stack;
    c->ctx.uc_stack.ss_size = size;
    c->ctx.uc_link = nullptr;
    makecontext(&c->ctx, coroutine_start, 0);
    return c;
}
void SwitchToFiber(void* to) {
    Coroutine* from = running_coroutine;
    running_coroutine = static_cast<Coroutine*>(to);
    swapcontext(&from->ctx, &running_coroutine->ctx);
}
void DeleteFiber(void* f) {
    auto* c = static_cast<Coroutine*>(f);
    std::free(c->stack);
    delete c;
}
}  // namespace
#endif

namespace amg {

namespace {

// Instruction table (docs/technical/scf-bytecode.md): operand kinds per opcode.
enum Kind : uint8_t { U8, S8, U16, J8, J16 };

struct OpInfo {
    bool valid;
    const char* name;
    uint8_t count;
    Kind kinds[2];
};

OpInfo opinfo(uint8_t op) {
    switch (op) {
        case 0x00: return {true, "nop", 0, {}};
        case 0x01: return {true, "nop1", 1, {U8}};
        case 0x02: return {true, "ret", 0, {}};
        case 0x03: return {true, "ret_top", 0, {}};
        case 0x04: return {true, "dup", 0, {}};
        case 0x05: return {true, "pop", 0, {}};
        case 0x06: return {true, "ne_ref", 0, {}};
        case 0x07: return {true, "eq_ref", 0, {}};
        case 0x08: return {true, "push_cvar", 2, {U8, U8}};
        case 0x09: return {true, "push_cvar16", 2, {U16, U8}};
        case 0x0A: return {true, "pop_cvar", 2, {U8, U8}};
        case 0x0B: return {true, "pop_cvar16", 2, {U16, U8}};
        case 0x0C: return {true, "and", 0, {}};
        case 0x0D: return {true, "or", 0, {}};
        case 0x1E: return {true, "neg", 0, {}};
        case 0x1F: return {true, "bit_not", 0, {}};
        case 0x20: return {true, "push_ivar", 1, {U8}};
        case 0x21: return {true, "pop_ivar", 1, {U8}};
        case 0x22: return {true, "push_local", 1, {U8}};
        case 0x23: return {true, "pop_local", 1, {U8}};
        case 0x24: return {true, "push_int", 1, {S8}};
        case 0x25: return {true, "push_nil", 0, {}};
        case 0x26: return {true, "push_self", 0, {}};
        case 0x28: return {true, "push_true", 0, {}};
        case 0x29: return {true, "push_false", 0, {}};
        case 0x2A: return {true, "push_caught", 0, {}};
        case 0x2B: return {true, "not", 0, {}};
        case 0x30: return {true, "send", 2, {U8, U8}};
        case 0x31: return {true, "send_super", 2, {U8, U8}};
        case 0x32: return {true, "send16", 2, {U8, U16}};
        case 0x33: return {true, "send_super16", 2, {U8, U16}};
        case 0x34: return {true, "primitive", 1, {U8}};
        case 0x40: return {true, "jump", 1, {J8}};
        case 0x41: return {true, "jump16", 1, {J16}};
        case 0x44: return {true, "jump_false", 1, {J8}};
        case 0x45: return {true, "jump_false16", 1, {J16}};
        case 0x50: return {true, "push_const", 1, {U8}};
        case 0x51: return {true, "push_const16", 1, {U16}};
        case 0x64: return {true, "index_get", 0, {}};
        case 0x65: return {true, "index_set", 0, {}};
        case 0x68: return {true, "clear_caught", 0, {}};
        case 0x69: return {true, "try", 1, {J16}};
        case 0x6A: return {true, "end_try", 0, {}};
        case 0x6B: return {true, "rethrow", 0, {}};
        case 0x6C: return {true, "locals", 1, {U8}};
        case 0x6D: return {true, "line", 1, {U16}};
        default: break;
    }
    if (op >= 0x10 && op <= 0x1D) return {true, "arith", 0, {}};
    return {false, "", 0, {}};
}

int32_t read_operand(const std::string& code, size_t& pos, Kind kind) {
    switch (kind) {
        case U8: return uint8_t(code.at(pos++));
        case S8:
        case J8: return int8_t(code.at(pos++));
        case U16: {
            int v = uint8_t(code.at(pos)) | (uint8_t(code.at(pos + 1)) << 8);
            pos += 2;
            return v;
        }
        case J16: {
            int v = int16_t(uint16_t(uint8_t(code.at(pos)) | (uint8_t(code.at(pos + 1)) << 8)));
            pos += 2;
            return v;
        }
    }
    return 0;
}

// Bytecode to instructions, jump targets turned into instruction indices.
std::shared_ptr<Method> decode_method(ClassObj* cls, const ClassSource::MethodSource& entry) {
    auto method = std::make_shared<Method>();
    method->cls = cls;
    method->name = entry.name;
    method->args = entry.args;
    const std::string& code = entry.code;

    struct Raw {
        size_t offset;
        uint8_t op;
        int32_t a, b;
        bool jump;
    };
    std::vector<Raw> raw;
    size_t pos = 0;
    while (pos < code.size()) {
        Raw r{pos, uint8_t(code[pos]), 0, 0, false};
        OpInfo info = opinfo(r.op);
        if (!info.valid) throw std::runtime_error("unknown opcode in " + entry.name);
        ++pos;
        int32_t operands[2] = {0, 0};
        for (int i = 0; i < info.count; ++i) {
            Kind kind = info.kinds[i];
            operands[i] = read_operand(code, pos, kind);
            if (kind == J8 || kind == J16) {
                operands[i] += int32_t(r.offset);  // absolute offset
                r.jump = true;
            }
        }
        r.a = operands[0];
        r.b = operands[1];
        raw.push_back(r);
    }
    std::unordered_map<size_t, int> at;
    for (size_t i = 0; i < raw.size(); ++i) at[raw[i].offset] = int(i);
    at[code.size()] = int(raw.size());
    for (const Raw& r : raw) {
        int32_t a = r.a;
        if (r.jump) a = at.at(size_t(a));
        method->code.push_back({r.op, a, r.b});
    }
    return method;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// VM: classes
// ---------------------------------------------------------------------------------------------

std::string VM::script_dir;

VM::VM(std::string root_) : root(std::move(root_)) {
    vita_buttons = amg::is_vita(root);
    load_sources();
    install_builtins();
}

int VM::intern(const std::string& name) {
    auto it = name_ids.find(name);
    if (it != name_ids.end()) return it->second;
    int id = int(names.size());
    names.push_back(name);
    name_ids[name] = id;
    return id;
}

void VM::load_sources() {
    std::string image = lzss_unpack(read_file(amg::data_file(root, "AMAGAMI.IMG")));
    for (auto& cls : read_image(image)) {
        sources[cls->name] = cls;
        image_classes.insert(cls->name);
    }
    // A Vita root (D-020) has SCENARIO.CPK instead of SCENARIO.ARC: one .scf per entry, the same
    // format (docs/technical/vita-cpk-format.md).
    std::string scenario_cpk = amg::data_file(root, "SCENARIO.CPK");
    if (std::filesystem::exists(scenario_cpk)) {
        // Read once, whole (8 MB): opening the file per entry costs most of a minute over a network or WSL path.
        CpkIndex index = cpk_read_index(scenario_cpk);
        std::string archive = read_file(scenario_cpk);
        for (const auto& [name, entry] : index) {
            if (name.size() < 4 || name.compare(name.size() - 4, 4, ".scf") != 0) continue;
            if (entry.offset + entry.size > archive.size()) continue;
            std::string blob = archive.substr(size_t(entry.offset), size_t(entry.size));
            if (entry.size != entry.extract_size) blob = cpk_decompress_crilayla(blob);
            auto cls = std::make_shared<ClassSource>();
            size_t pos = 0;
            if (!parse_scf(blob, pos, *cls)) {
                if (std::getenv("AMG_LOADER_DEBUG")) std::fprintf(stderr, "scenario script not parsed: %s%c", name.c_str(), 10);
                continue;
            }
            sources.emplace(cls->name, cls);  // setdefault
        }
    } else if (std::filesystem::exists(root + "/extracted/SCENARIO.ARC")) {
        for (const std::string& blob : read_arc(read_file(root + "/extracted/SCENARIO.ARC"))) {
            auto cls = std::make_shared<ClassSource>();
            size_t pos = 0;
            if (!parse_scf(blob, pos, *cls)) {
                if (std::getenv("AMG_LOADER_DEBUG")) std::fprintf(stderr, "scenario script not parsed: %zu bytes at %zu%c", blob.size(), pos, 10);
                continue;
            }
            sources.emplace(cls->name, cls);  // setdefault
        }
    }
    // The port's own classes (PortHooks, PortSwitch, PortSettings): ours, not the game's.
    std::string own = script_dir;
    if (const char* dir = std::getenv("AMG_SCRIPT_DIR")) own = dir;
    if (!own.empty() && std::filesystem::exists(own)) {
        for (const auto& entry : std::filesystem::directory_iterator(own)) {
            if (entry.path().extension() != ".scf") continue;
            auto cls = std::make_shared<ClassSource>();
            size_t pos = 0;
            if (parse_scf(read_file(entry.path().string()), pos, *cls)) sources[cls->name] = cls;
        }
    }
    // AMG_OVERLAY=DIR: every DIR/*.scf replaces the class of the same name (image or scenario). For probes: a
    // class decompiled with tools/scf_src.py, edited (a few System.println) and compiled back, without
    // rebuilding the archives.
    if (const char* dir = std::getenv("AMG_OVERLAY")) {
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.path().extension() != ".scf") continue;
            std::string blob = read_file(entry.path().string());
            auto cls = std::make_shared<ClassSource>();
            size_t pos = 0;
            if (parse_scf(blob, pos, *cls)) {
                sources[cls->name] = cls;
                std::fprintf(stderr, "overlay: %s%c", cls->name.c_str(), 10);
            }
        }
    }
}

Value VM::symbol(const std::string& name) {
    auto it = symbols.find(name);
    if (it == symbols.end()) {
        auto s = std::make_shared<SymObj>();
        s->name = name;
        s->id = intern(name);
        it = symbols.emplace(name, s).first;
    }
    Value v;
    v.tag = Tag::Sym;
    v.p = it->second;
    return v;
}

ClassObj* VM::find_class(const std::string& name) {
    auto it = classes.find(name);
    if (it != classes.end()) return it->second.get();
    auto src = sources.find(name);
    if (src == sources.end()) return nullptr;
    return define_class(src->second);
}

ClassObj* VM::define_class(const std::shared_ptr<ClassSource>& src) {
    ClassObj* parent = src->parent.empty() ? nullptr : find_class(src->parent);
    int size = 0;
    for (auto& v : src->instance_vars) size = std::max(size, v.first + 1);
    if (parent) size = std::max(size, parent->size);
    ClassObj* cls;
    auto it = classes.find(src->name);
    if (it == classes.end()) {
        auto c = std::make_shared<ClassObj>();
        c->name = src->name;
        it = classes.emplace(src->name, c).first;
    }
    cls = it->second.get();
    cls->parent = parent;
    cls->size = std::max(size, cls->size);
    cls->src = src;
    for (auto& entry : src->methods) {
        Callable c;
        c.method = decode_method(cls, entry);
        cls->imethods[key(intern(entry.name), entry.args)] = c;
    }
    for (auto& entry : src->class_methods) {
        Callable c;
        c.method = decode_method(cls, entry);
        cls->cmethods[key(intern(entry.name), entry.args)] = c;
    }
    if (src->name == "ConfigPage" && sources.count("PortHooks")) patch_config_page(cls);
    return cls;
}

// F5-11: the game's Settings screen builds its pages in ConfigPage#initialize(kind, items). The port's rows are added by
// calling PortHooks.configPage(kind, items) first (docs/technical/port-settings.md). Instructions are inserted, so every
// jump target moves; the player's own data is patched in memory and nothing of it is written.
void VM::patch_config_page(ClassObj* cls) {
    auto it = cls->imethods.find(key(intern("initialize"), 2));
    if (it == cls->imethods.end() || !it->second.method) return;
    auto& consts = cls->src->constants;
    auto add = [&](uint8_t tag, const char* text) {
        ClassSource::Constant c;
        c.tag = tag;
        c.s = text;
        consts.push_back(c);
        return int(consts.size()) - 1;
    };
    const int hooks = add(6, "PortHooks"), hook = add(7, "configPage");
    const std::vector<Instr> head = {{0x51, hooks, 0}, {0x22, 0, 0}, {0x22, 1, 0}, {0x32, 2, hook}, {0x05, 0, 0}};
    auto& code = it->second.method->code;
    for (Instr& in : code)
        if (in.op == 0x40 || in.op == 0x41 || in.op == 0x44 || in.op == 0x45 || in.op == 0x69) in.a += int(head.size());
    code.insert(code.begin(), head.begin(), head.end());
}

Value VM::make_str(std::string s) {
    auto o = std::make_shared<StrObj>();
    o->b = std::move(s);
    Value v;
    v.tag = Tag::Str;
    v.p = o;
    return v;
}

Value VM::make_arr(ClassObj* cls, std::vector<Value> items) {
    auto o = std::make_shared<ArrObj>();
    o->cls = cls;
    o->items = std::move(items);
    Value v;
    v.tag = Tag::Arr;
    v.p = o;
    return v;
}

Value VM::make_obj(ClassObj* cls) {
    auto o = std::make_shared<ObjObj>();
    o->cls = cls;
    o->iv.assign(size_t(cls->size), Value());
    Value v;
    v.tag = Tag::Obj;
    v.p = o;
    return v;
}

Value VM::error_object(const std::string& cls_name, const std::string& message) {
    ClassObj* cls = find_class(cls_name);
    if (!cls) cls = find_class("Error");
    if (!cls) cls = find_class("Object");
    Value v = make_obj(cls);
    static_cast<ObjObj*>(v.p.get())->message = message;
    return v;
}

void VM::raise(const std::string& message, const std::string& cls) {
    throw ScriptThrow{error_object(cls, message), {}};
}

Value VM::constant(const ClassSource::Constant& c) {
    switch (c.tag) {
        case 1: return Value::integer(c.i);
        case 3: return Value::real(double(c.f));
        case 5: return make_str(c.s);
        case 6: {
            ClassObj* cls = find_class(c.s);
            if (!cls) raise("class not found: " + c.s);
            Value v;
            v.tag = Tag::Class;
            v.p = classes.at(c.s);
            return v;
        }
        case 7: return symbol(c.s);
        case 8: {
            std::vector<Value> items;
            for (auto& it : c.items) items.push_back(constant(it));
            return make_arr(find_class("Array"), std::move(items));
        }
        // Bare constant tags: 4 is true and 2 is false (confirmed: the inline command tables of
        // Character.checkCom and Actor.checkCom hold tag 4 exactly for the letters with an inlineCom method).
        case 2: return Value::boolean(false);
        case 4: return Value::boolean(true);
        default: return Value();
    }
}

const std::vector<Value>& VM::constants(ClassObj* cls) {
    if (!cls->consts_ready) {
        std::vector<Value> out;
        for (auto& c : cls->src->constants) out.push_back(constant(c));
        cls->consts = std::move(out);
        cls->consts_ready = true;
    }
    return cls->consts;
}

ClassObj* VM::class_of(const Value& v) {
    switch (v.tag) {
        case Tag::Nil: return find_class("Nil");
        case Tag::True: return find_class("True");
        case Tag::False: return find_class("False");
        case Tag::Int: return find_class("Integer");
        case Tag::Float: return find_class("Float");
        case Tag::Str: return find_class("String");
        case Tag::Sym: return find_class("Symbol");
        case Tag::Arr: return static_cast<ArrObj*>(v.p.get())->cls;
        case Tag::Class: return find_class("Class");
        case Tag::Obj: return static_cast<ObjObj*>(v.p.get())->cls;
    }
    return nullptr;
}

Callable VM::lookup(const Value& recv, int name_id, int argc) {
    uint64_t k = key(name_id, argc);
    ClassObj* cls;
    if (recv.tag == Tag::Class) {
        for (ClassObj* c = static_cast<ClassObj*>(recv.p.get()); c; c = c->parent) {
            auto it = c->cmethods.find(k);
            if (it != c->cmethods.end()) return it->second;
        }
        cls = find_class("Class");
    } else {
        cls = class_of(recv);
    }
    for (ClassObj* c = cls; c; c = c->parent) {
        auto it = c->imethods.find(k);
        if (it != c->imethods.end()) return it->second;
    }
    return Callable{};
}

Value VM::call(Value recv, const Callable& c, std::vector<Value> args, Thread* th) {
    if (!th) th = current;
    if (c.native) return c.native(*this, *th, recv, args);
    return th->invoke(recv, c.method, std::move(args));
}

Value VM::send(Thread& th, Value recv, const std::string& name, std::vector<Value> args) {
    Callable c = lookup(recv, intern(name), int(args.size()));
    if (!c) {
        ClassObj* k = class_of(recv);
        raise("no method " + name + "/" + std::to_string(args.size()) + " for <class " + (k ? k->name : "?") + ">");
    }
    return call(recv, c, std::move(args), &th);
}

Value VM::instantiate(ClassObj* cls, std::vector<Value> args, Thread* th) {
    Value obj = make_obj(cls);
    Callable init = lookup(obj, intern("initialize"), int(args.size()));
    if (!init) raise("no initialize/" + std::to_string(args.size()) + " in <class " + cls->name + ">");
    call(obj, init, std::move(args), th);
    return obj;
}

// PS2 mask (L2 R2 L1 R1 triangle circle cross square in the low byte, select L3 R3 start up right down left in the
// high one) to SceCtrl's (the same two bytes the other way round). The Vita's shoulder buttons are SceCtrl's
// L/R triggers (0x100, 0x200): the PS2's L1/R1 go there too.
int VM::to_script_buttons(int m) const {
    if (!vita_buttons) return m;
    int v = ((m & 0xFF) << 8) | ((m >> 8) & 0xFF);
    if (m & 0x4) v |= 0x100;
    if (m & 0x8) v |= 0x200;
    return v;
}

int VM::buttons_held(bool ps2_layout) const {
    auto layout = [&](int m) { return ps2_layout ? m : to_script_buttons(m); };
    if (live_buttons) return layout(live_held);
    int mask = 0;
    for (auto& p : presses)
        if (std::get<0>(p) <= tick && tick <= std::get<1>(p)) mask |= std::get<2>(p);
    return layout(mask | auto_press.mask_at(tick) | monkey.mask_at(tick));
}

int VM::buttons_pressed(bool ps2_layout) const {
    auto layout = [&](int m) { return ps2_layout ? m : to_script_buttons(m); };
    if (live_buttons) return layout(live_pressed);
    int mask = 0;
    for (auto& p : presses)
        if (std::get<0>(p) == tick) mask |= std::get<2>(p);
    return layout(mask | (auto_press.mask_at(tick) & ~auto_press.mask_at(tick - 1)) |
                  (monkey.mask_at(tick) & ~monkey.mask_at(tick - 1)));
}

int VM::buttons_rapid(bool ps2_layout) const {
    int held = buttons_held(ps2_layout);
    if (!vita_buttons) return rapid_fire_ps2 ? held : buttons_pressed(ps2_layout);
    return buttons_pressed(ps2_layout) | (rapid_count == 20 ? held : 0);
}

void VM::advance(int64_t to) {
    while (tick < to) {
        ++tick;
        if (live_buttons) {
            int held = live_buttons();
            live_pressed = held & ~live_held;
            live_held = held;
        }
        int held = buttons_held(true);
        rapid_fire_ps2 = false;
        if (held != rapid_last) rapid_count_ps2 = 0;
        else if (++rapid_count_ps2 >= 21) {
            rapid_fire_ps2 = true;
            rapid_count_ps2 -= 4;
        }
        if (held != rapid_last || !held) rapid_count = 0;
        else rapid_count = rapid_count < 20 ? rapid_count + 1 : (rapid_count + 1 >= 25 ? 20 : rapid_count + 1);
        rapid_last = held;
        if (on_tick) on_tick(*this);
        auto shot = shots.find(tick);
        if (shot != shots.end() && graphics && on_shot) on_shot(*this, shot->second);
    }
}

void VM::install_builtins() { install_natives(*this); }

Thread* VM::new_thread(Value receiver, const std::string& name, std::vector<Value> args, const std::string& label) {
    auto th = std::make_unique<Thread>();
    th->vm = this;
    th->receiver = std::move(receiver);
    th->name = name;
    th->args = std::move(args);
    th->label = label;
    threads.push_back(std::move(th));
    live_threads.push_back(threads.back().get());
    return threads.back().get();
}

std::string VM::run() {
    ClassObj* startup = find_class("SystemStartup");
    Thread* main = new_thread(Value(), "run", {}, "main");
    current = main;
    Value obj = instantiate(startup, {}, nullptr);
    main->receiver = obj;
#ifdef AMG_FIBERS
    // All script threads are fibers of this OS thread: a hand-over is one SwitchToFiber, with no
    // trip through the system scheduler.
    driver_fiber = ConvertThreadToFiber(nullptr);
    main->start();
    SwitchToFiber(main->fiber);
    for (auto& t : threads)
        if (t->fiber && !t->fiber_freed) {
            DeleteFiber(t->fiber);
            t->fiber_freed = true;
        }
    ConvertFiberToThread();
#else
    main->start();
    {
        std::unique_lock<std::mutex> lk(mutex);
        cv.notify_all();
        cv.wait(lk, [&] { return finished; });
    }
#endif
    return error;
}

// ---------------------------------------------------------------------------------------------
// Threads. Each script thread runs on its own OS thread and exactly one runs at a time: the
// baton is `VM::current`. A thread that sleeps (or ends) hands the baton to the next one that is
// due, round robin, advancing the frame clock when everything is asleep.
// ---------------------------------------------------------------------------------------------

#ifdef AMG_FIBERS
static void CALLBACK fiber_entry(void* p) {
    Thread* t = static_cast<Thread*>(p);
    t->body();
    // body() ends in a switch to another fiber and never returns here.
}

void Thread::start() { fiber = CreateFiber(512 * 1024, fiber_entry, this); }

void Thread::wait_for_turn() {}
#else
void Thread::start() {
    os = std::thread([this] {
        wait_for_turn();
        if (vm->finished) return;
        body();
    });
    os.detach();
}

void Thread::wait_for_turn() {
    std::unique_lock<std::mutex> lk(vm->mutex);
    wake.wait(lk, [&] { return vm->current == this || vm->finished; });
}
#endif

std::string where_of(const std::vector<Frame>& frames) {
    std::string s;
    for (size_t i = frames.size(); i-- > 0;) {
        const Frame& f = frames[i];
        s += "\n    at " + f.method->cls->name + "." + f.method->name + " (instruction " + std::to_string(f.pc) + ")";
    }
    return s;
}

void Thread::body() {
    try {
        vm->send(*this, receiver, name, args);
    } catch (ScriptThrow& t) {
        std::string where;
        const std::vector<std::string>& tr = t.trace;
        for (size_t i = tr.size(); i-- > 0;) where += "\n    at " + tr[i];
        if (label != "main") {
            // A thread that dies does not stop the game (hypothesis: the shipped Music.fade(0, 0) divides by zero
            // in its thread and the scene goes on). It is counted and shown, not fatal.
            vm->thread_errors.push_back(describe(*vm, t.value) + (tr.empty() ? "" : " @ " + tr.back()));
            std::fprintf(stderr, "thread ended by an uncaught exception: %s%s\n", describe(*vm, t.value).c_str(), where.c_str());
        } else {
            vm->error = "uncaught script exception in thread " + label + ": " + describe(*vm, t.value) + where;
        }
    } catch (MissingNative& e) {
        vm->error = "missing native: " + e.what;
    } catch (VMExit&) {
    } catch (std::exception& e) {
        vm->error = "interpreter error in thread " + label + ": " + e.what() + where_of(frames);
    }
    done = true;
    schedule_next(true);
}

void Thread::schedule_next(bool finished) {
    VM& v = *vm;
    // The live threads, and the ended ones moved aside (they keep their fiber until it can be freed).
    std::vector<Thread*> alive;
    alive.reserve(v.live_threads.size());
    for (Thread* t : v.live_threads)
        (t->done ? v.ended_threads : alive).push_back(t);
    v.live_threads = alive;
#ifdef AMG_FIBERS
    // Free the fibers of threads that ended or were killed (not the one we are running on).
    void* running = GetCurrentFiber();
    std::vector<Thread*> keep;
    for (Thread* t : v.ended_threads) {
        if (t->fiber && !t->fiber_freed && t->fiber != running) {
            DeleteFiber(t->fiber);
            t->fiber_freed = true;
        }
        if (t->fiber && !t->fiber_freed) keep.push_back(t);
    }
    v.ended_threads.swap(keep);
#else
    v.ended_threads.clear();
#endif
    if (alive.empty() || !v.error.empty()) {
#ifdef AMG_FIBERS
        v.finished = true;
        SwitchToFiber(v.driver_fiber);  // back to run()
        return;
#else
        std::lock_guard<std::mutex> lk(v.mutex);
        v.finished = true;
        v.cv.notify_all();
        for (auto& t : v.threads) t->wake.notify_all();
        return;
#endif
    }
    // Round robin from the next thread; advance virtual time when all are sleeping.
    auto it = std::find(alive.begin(), alive.end(), this);
    std::vector<Thread*> order;
    if (it != alive.end()) {
        size_t start = size_t(it - alive.begin() + 1) % alive.size();
        for (size_t i = 0; i < alive.size(); ++i) order.push_back(alive[(start + i) % alive.size()]);
    } else {
        order = alive;
    }
    for (;;) {
        for (Thread* t : order) {
            if (t->wake_tick <= v.tick) {
                if (t == this && !finished) {
                    v.current = t;
                    return;
                }
#ifdef AMG_FIBERS
                v.current = t;
                SwitchToFiber(t->fiber);  // when this thread is resumed it continues from here
                return;
#else
                {
                    std::lock_guard<std::mutex> lk(v.mutex);
                    v.current = t;
                }
                t->wake.notify_one();
                if (!finished) wait_for_turn();
                return;
#endif
            }
        }
        int64_t soonest = alive[0]->wake_tick;
        for (Thread* t : alive) soonest = std::min(soonest, t->wake_tick);
        v.advance(soonest);
    }
}

void Thread::sleep(int64_t frames) {
    wake_tick = vm->tick + std::max<int64_t>(1, frames);
    schedule_next(false);
}

Value Thread::invoke(Value recv, const std::shared_ptr<Method>& method, std::vector<Value> a) {
    size_t base = stack.size();
    stack.push_back(std::move(recv));
    for (auto& x : a) stack.push_back(std::move(x));
    size_t depth = frames.size();
    method->ran = true;
    frames.push_back({method.get(), 0, base});
    return execute(depth);
}

Value Thread::execute(size_t stop_depth) {
    for (;;) {
        try {
            return loop(stop_depth);
        } catch (ScriptThrow& t) {
            if (t.trace.empty() && t.value.tag == Tag::Obj && static_cast<ObjObj*>(t.value.p.get())->has_trace) {
                t.trace = static_cast<ObjObj*>(t.value.p.get())->trace;  // a rethrow keeps the first trace
            } else if (t.trace.empty()) {
                for (size_t i = 0; i < frames.size(); ++i)
                    t.trace.push_back(frames[i].method->cls->name + "." + frames[i].method->name + " (instruction " +
                                      std::to_string(frames[i].pc) + ")");
                if (t.value.tag == Tag::Obj) {
                    auto* o = static_cast<ObjObj*>(t.value.p.get());
                    if (!o->has_trace) {
                        o->trace = t.trace;
                        o->has_trace = true;
                    }
                }
            } else if (t.value.tag == Tag::Obj) {
                auto* o = static_cast<ObjObj*>(t.value.p.get());
                if (o->has_trace) t.trace = o->trace;
            }
            bool handled = false;
            while (!tries.empty() && tries.back().depth >= stop_depth) {
                TryEntry e = tries.back();
                tries.pop_back();
                if (e.depth < frames.size()) {
                    frames.resize(e.depth + 1);
                    stack.resize(e.height);
                    frames[e.depth].pc = e.handler;
                    caught = t.value;
                    handled = true;
                    break;
                }
            }
            if (!handled) {
                frames.resize(std::min(frames.size(), stop_depth));
                throw;
            }
        }
    }
}

static std::vector<Value>& ivars(const Value& recv) {
    if (recv.tag == Tag::Class) return static_cast<ClassObj*>(recv.p.get())->iv;
    if (recv.tag == Tag::Obj) return static_cast<ObjObj*>(recv.p.get())->iv;
    throw std::runtime_error("instance variable access on a value that has none");
}

bool same(const Value& x, const Value& y) {
    if (x.tag == Tag::Int && y.tag == Tag::Int) return x.i == y.i;
    if (x.tag != y.tag) return false;
    switch (x.tag) {
        case Tag::Nil: case Tag::True: case Tag::False: return true;
        case Tag::Float: return x.f == y.f;
        default: return x.p == y.p;
    }
}

Value Thread::loop(size_t stop_depth) {
    VM& v = *vm;
    for (;;) {
        Method* method = frames.back().method;
        size_t base = frames.back().base;
        const std::vector<Instr>& code = method->code;
        ClassObj* cls = method->cls;
        int pc = frames.back().pc;
        for (;;) {
            const Instr in = code[size_t(pc)];
            ++pc;
            if (++v.steps > v.max_steps) throw VMExit{};
            ++method->executed;
            switch (in.op) {
                case 0x22: stack.push_back(stack[base + 1 + size_t(in.a)]); break;
                case 0x50: case 0x51: stack.push_back(v.constants(cls)[size_t(in.a)]); break;
                case 0x24: stack.push_back(Value::integer(in.a)); break;
                case 0x20: stack.push_back(ivars(stack[base]).at(size_t(in.a))); break;
                case 0x21: {
                    Value val = std::move(stack.back());
                    stack.pop_back();
                    ivars(stack[base]).at(size_t(in.a)) = std::move(val);
                    break;
                }
                case 0x23: {
                    stack[base + 1 + size_t(in.a)] = std::move(stack.back());
                    stack.pop_back();
                    break;
                }
                case 0x05: stack.pop_back(); break;
                case 0x26: stack.push_back(stack[base]); break;
                case 0x30: case 0x31: case 0x32: case 0x33: {
                    const Value& sym = v.constants(cls)[size_t(in.b)];
                    int name_id = static_cast<SymObj*>(sym.p.get())->id;
                    int argc = in.a;
                    Value recv = argc ? stack[stack.size() - size_t(argc) - 1] : stack.back();
                    Callable hit;
                    if (in.op & 1) {  // send_super: start in the parent of the class defining this method
                        uint64_t k = VM::key(name_id, argc);
                        for (ClassObj* c = cls->parent; c && !hit; c = c->parent) {
                            auto& table = recv.tag == Tag::Class ? c->cmethods : c->imethods;
                            auto it = table.find(k);
                            if (it != table.end()) hit = it->second;
                        }
                    } else {
                        hit = v.lookup(recv, name_id, argc);
                    }
                    if (!hit) {
                        frames.back().pc = pc;
                        ClassObj* k = v.class_of(recv);
                        v.raise("no method " + static_cast<SymObj*>(sym.p.get())->name + "/" + std::to_string(argc) +
                                " for <class " + (k ? k->name : "?") + ">");
                    }
                    if (hit.native) {
                        if (v.coverage) v.natives_used.insert(VM::key(name_id, argc));
                        if (v.coverage) {
                            uint64_t k = VM::key(name_id, argc);
                            bool on_class = recv.tag == Tag::Class;
                            ClassObj* start = (in.op & 1) ? cls->parent : (on_class ? static_cast<ClassObj*>(recv.p.get()) : v.class_of(recv));
                            bool found = false;
                            for (ClassObj* c = start; c && !found; c = c->parent) {
                                auto& table = on_class ? c->cmethods : c->imethods;
                                if (table.count(k)) {
                                    v.natives_owned.insert({c, k, on_class});
                                    found = true;
                                }
                            }
                            // A class receiver without such a class method uses the instance methods of Class.
                            if (!found && on_class)
                                for (ClassObj* c = v.find_class("Class"); c && !found; c = c->parent)
                                    if (c->imethods.count(k)) {
                                        v.natives_owned.insert({c, k, false});
                                        found = true;
                                    }
                        }
                        std::vector<Value> args(stack.end() - argc, stack.end());
                        frames.back().pc = pc;
                        Value result = hit.native(v, *this, recv, args);
                        stack.resize(stack.size() - size_t(argc) - 1);
                        stack.push_back(std::move(result));
                    } else {
                        frames.back().pc = pc;
                        hit.method->ran = true;
                        frames.push_back({hit.method.get(), 0, stack.size() - size_t(argc) - 1});
                        goto next_frame;
                    }
                    break;
                }
                case 0x02: case 0x03: {
                    Value result = in.op == 0x02 ? stack[base] : stack.back();
                    stack.resize(base);
                    stack.push_back(std::move(result));
                    frames.pop_back();
                    if (frames.size() <= stop_depth) {
                        Value r = std::move(stack.back());
                        stack.pop_back();
                        return r;
                    }
                    goto next_frame;
                }
                case 0x6C: stack.resize(stack.size() + size_t(in.a)); break;
                case 0x45: case 0x44: {
                    Value val = std::move(stack.back());
                    stack.pop_back();
                    if (!val.truthy()) pc = in.a;
                    break;
                }
                case 0x41: case 0x40: pc = in.a; break;
                case 0x25: stack.emplace_back(); break;
                case 0x28: stack.push_back(Value::boolean(true)); break;
                case 0x29: stack.push_back(Value::boolean(false)); break;
                case 0x08: case 0x09: {
                    const Value& c = v.constants(cls)[size_t(in.a)];
                    stack.push_back(static_cast<ClassObj*>(c.p.get())->iv[size_t(in.b)]);
                    break;
                }
                case 0x0A: case 0x0B: {
                    const Value& c = v.constants(cls)[size_t(in.a)];
                    static_cast<ClassObj*>(c.p.get())->iv[size_t(in.b)] = std::move(stack.back());
                    stack.pop_back();
                    break;
                }
                case 0x0C: case 0x0D: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16:
                case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
                case 0x2B:
                    frames.back().pc = pc;
                    arith(in.op);
                    break;
                case 0x07: {
                    Value y = std::move(stack.back());
                    stack.pop_back();
                    Value x = std::move(stack.back());
                    stack.pop_back();
                    stack.push_back(Value::boolean(same(x, y)));
                    break;
                }
                case 0x06: {
                    Value y = std::move(stack.back());
                    stack.pop_back();
                    Value x = std::move(stack.back());
                    stack.pop_back();
                    stack.push_back(Value::boolean(!same(x, y)));
                    break;
                }
                case 0x64: {
                    frames.back().pc = pc;
                    Value i = std::move(stack.back());
                    stack.pop_back();
                    Value o = std::move(stack.back());
                    stack.pop_back();
                    stack.push_back(v.send(*this, o, "[]", {i}));
                    break;
                }
                case 0x65: {
                    frames.back().pc = pc;
                    Value val = std::move(stack.back());
                    stack.pop_back();
                    Value i = std::move(stack.back());
                    stack.pop_back();
                    Value o = std::move(stack.back());
                    stack.pop_back();
                    stack.push_back(v.send(*this, o, "atPut", {i, val}));
                    break;
                }
                case 0x34: {
                    frames.back().pc = pc;
                    auto it = cls->prims.find(in.a);
                    if (it == cls->prims.end()) throw MissingNative{cls->name + " primitive " + std::to_string(in.a)};
                    if (v.coverage) v.prims_used.insert({cls, int(in.a)});
                    auto began = v.profile_prims ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
                    Value result = it->second(v, *this, stack, base, *method);
                    if (v.profile_prims) {
                        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
                        if (ms > 3.0)
                            std::fprintf(stderr, "SLOWPRIM %s.%d %.1f ms at frame %lld\n", cls->name.c_str(), in.a, ms, (long long)v.tick);
                    }
                    stack[base] = std::move(result);
                    break;
                }
                case 0x2A: stack.push_back(caught); break;
                case 0x68: caught = Value(); break;
                case 0x6B: {
                    frames.back().pc = pc;
                    Value val = std::move(stack.back());
                    stack.pop_back();
                    throw ScriptThrow{val, {}};
                }
                case 0x69: tries.push_back({frames.size() - 1, in.a, stack.size()}); break;
                case 0x6A: tries.pop_back(); break;
                case 0x04: stack.push_back(stack.back()); break;
                case 0x00: case 0x01: case 0x6D: break;
                default:
                    throw std::runtime_error("unimplemented opcode");
            }
        }
    next_frame:;
    }
}

void Thread::arith(uint8_t op) {
    VM& v = *vm;
    auto pop = [&] {
        Value x = std::move(stack.back());
        stack.pop_back();
        return x;
    };
    if (op == 0x1E || op == 0x1F || op == 0x2B) {
        Value x = pop();
        if (op == 0x1E && x.tag == Tag::Int) {
            stack.push_back(Value::integer(-x.i));
        } else if (op == 0x1F && x.tag == Tag::Int) {
            stack.push_back(Value::integer(~x.i));
        } else if (op == 0x2B && x.is_bool()) {
            stack.push_back(Value::boolean(x.tag == Tag::False));
        } else {
            const char* name = op == 0x1E ? "-U" : op == 0x1F ? "~U" : "!U";
            stack.push_back(v.send(*this, x, name, {}));
        }
        return;
    }
    Value y = pop();
    Value x = pop();
    if (op == 0x0C || op == 0x0D) {
        // The engine's handlers (0x11C8E0, 0x11C950, confirmed by reading them) compute directly whenever both
        // operands are special constants (nil, true or false), and send `&&`/`||` only when one is an integer or
        // an object. `and` answers the left operand when the right one is true, else false; `or` answers the
        // left operand when the right one is false, else true. So `nil and x` is not an error (the mahjong's
        // Tehai#draw牌 tests a flag that is never set).
        auto special = [](const Value& v) { return v.tag == Tag::Nil || v.tag == Tag::True || v.tag == Tag::False; };
        if (special(x) && special(y)) {
            if (op == 0x0C) stack.push_back(y.tag == Tag::True ? x : Value::boolean(false));
            else stack.push_back(y.tag == Tag::False ? x : Value::boolean(true));
        } else {
            stack.push_back(v.send(*this, x, op == 0x0C ? "&&" : "||", {y}));
        }
        return;
    }
    if (x.tag == Tag::Int && y.tag == Tag::Int) {
        int64_t a = x.i, b = y.i;
        Value r;
        switch (op) {
            case 0x10: r = Value::integer(a + b); break;
            case 0x11: r = Value::integer(a - b); break;
            case 0x12: r = Value::integer(a * b); break;
            case 0x13: case 0x14: {
                if (b == 0) v.raise("division by zero");
                int64_t q = (a < 0 ? -a : a) / (b < 0 ? -b : b);
                q = ((a >= 0) == (b >= 0)) ? q : -q;
                r = Value::integer(op == 0x13 ? q : a - q * b);
                break;
            }
            case 0x15: r = Value::integer(a & b); break;
            case 0x16: r = Value::integer(a | b); break;
            case 0x17: r = Value::integer(a ^ b); break;
            case 0x18: r = Value::boolean(a == b); break;
            case 0x19: r = Value::boolean(a > b); break;
            case 0x1A: r = Value::boolean(a < b); break;
            case 0x1B: r = Value::boolean(a >= b); break;
            case 0x1C: r = Value::boolean(a <= b); break;
            default: r = Value::boolean(a != b); break;
        }
        stack.push_back(std::move(r));
        return;
    }
    static const char* names[] = {"+", "-", "*", "/", "%", "&", "|", "^", "=", ">", "<", ">=", "<=", "<>"};
    stack.push_back(v.send(*this, x, names[op - 0x10], {y}));
}

std::string describe(VM& vm, const Value& value) {
    (void)vm;
    if (value.tag == Tag::Obj) {
        auto* o = static_cast<ObjObj*>(value.p.get());
        return o->cls->name + ": " + o->message;
    }
    switch (value.tag) {
        case Tag::Nil: return "None";
        case Tag::True: return "True";
        case Tag::False: return "False";
        case Tag::Int: return std::to_string(value.i);
        default: return "<value>";
    }
}

// ---------------------------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------------------------

std::vector<int> chars(const std::string& data) {
    std::vector<int> out;
    size_t i = 0, n = data.size();
    while (i < n) {
        int c = uint8_t(data[i]);
        if (((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) && i + 1 < n) {
            out.push_back((c << 8) | uint8_t(data[i + 1]));
            i += 2;
        } else {
            out.push_back(c);
            ++i;
        }
    }
    return out;
}

std::string unchars(const std::vector<int>& codes) {
    std::string out;
    for (int code : codes) {
        if (code > 0xFF) {
            out.push_back(char((code >> 8) & 0xFF));
            out.push_back(char(code & 0xFF));
        } else {
            out.push_back(char(code & 0xFF));
        }
    }
    return out;
}

}  // namespace amg
