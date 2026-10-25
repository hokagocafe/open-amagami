// Built-in classes and headless stubs. Mirrors tools/scfvm_natives.py; the registration table
// (names, argument counts, class or instance side) comes from the executable, see
// docs/technical/natives.md. Behaviour is inferred and therefore a hypothesis until checked.
#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include "datapath.hpp"
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "gfx.hpp"
#include "paths.hpp"
#include "port_settings.hpp"
#include "vm.hpp"

namespace amg {

namespace {

std::string py_repr(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
    char buf[40];
    for (int prec = 1; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, d);
        if (std::strtod(buf, nullptr) == d) break;
    }
    std::string s = buf;
    // Python writes 1e+16 as 1e+16 and 100000.0 as 100000.0; %g agrees except for the trailing ".0".
    if (s.find_first_of(".en") == std::string::npos) s += ".0";
    return s;
}

StrObj* as_str(const Value& v) { return static_cast<StrObj*>(v.p.get()); }
ArrObj* as_arr(const Value& v) { return static_cast<ArrObj*>(v.p.get()); }
ObjObj* as_obj(const Value& v) { return static_cast<ObjObj*>(v.p.get()); }
ClassObj* as_cls(const Value& v) { return static_cast<ClassObj*>(v.p.get()); }
SymObj* as_sym(const Value& v) { return static_cast<SymObj*>(v.p.get()); }

bool is_number(const Value& v) { return v.tag == Tag::Int || v.tag == Tag::Float; }
double num(const Value& v) { return v.tag == Tag::Int ? double(v.i) : v.f; }

// --- Save files: the memory card as a folder (docs/technical/save.md) --------------------------
// Every file is a 16-byte header ("OASV", version, kind, payload length, CRC-32) followed by the
// exact bytes the script handed over. Writes go to a temporary file renamed over the old one.

constexpr int SAVE_SLOTS = 20, SAVE_HEAD_SIZE = 256;
enum SaveKind : uint16_t { KIND_SYSTEM = 0, KIND_HEAD = 1, KIND_BODY = 2 };
// Nonzero results of the primitives. The scripts only test `== 0`; the PS2 codes are unknown.
constexpr int SAVE_ERR_MISSING = -1, SAVE_ERR_BAD = -2, SAVE_ERR_IO = -3, SAVE_ERR_ARG = -4;

uint32_t crc32(const std::string& d) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : d) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

std::filesystem::path save_dir(const VM& vm) {
    if (!vm.save_dir.empty()) return std::filesystem::u8path(vm.save_dir);
    if (const char* e = std::getenv("AMG_SAVE_DIR"); e && *e) return e;
    return amg::paths::save_dir();
}

std::filesystem::path slot_file(const VM& vm, int slot, const char* ext) {
    char name[32];
    std::snprintf(name, sizeof name, "slot%02d.%s", slot, ext);
    return save_dir(vm) / name;
}

// The end of one Serializer record (the scripts' Serializer class): tagged values until a 0 byte. 'N' nil and 'B'
// boolean take one byte, 'I' integer and 'F' float four, 'S' a 4-byte little-endian length and the bytes, 'A' a
// nested record. False if the record runs past the data or has an unknown tag.
bool serial_end(const std::string& d, size_t pos, size_t& end) {
    while (pos < d.size()) {
        char tag = d[pos++];
        if (tag == 0) {
            end = pos;
            return true;
        }
        if (tag == 'N' || tag == 'B') {
            pos += 1;
        } else if (tag == 'I' || tag == 'F') {
            pos += 4;
        } else if (tag == 'S') {
            if (pos + 4 > d.size()) return false;
            pos += 4 + (size_t(uint8_t(d[pos])) | (size_t(uint8_t(d[pos + 1])) << 8) | (size_t(uint8_t(d[pos + 2])) << 16) |
                        (size_t(uint8_t(d[pos + 3])) << 24));
        } else if (tag == 'A') {
            if (!serial_end(d, pos, pos)) return false;
        } else {
            return false;
        }
    }
    return false;
}

void put_le(std::string& s, uint32_t v, int n) {
    for (int i = 0; i < n; ++i) s.push_back(char((v >> (8 * i)) & 0xFF));
}
uint32_t get_le(const std::string& s, size_t at, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) v |= uint32_t(uint8_t(s[at + i])) << (8 * i);
    return v;
}

// Reads a save file's payload: 0, SAVE_ERR_MISSING or SAVE_ERR_BAD.
int read_save(const std::filesystem::path& p, SaveKind kind, std::string& payload) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return SAVE_ERR_MISSING;
    std::string d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 16 || d.compare(0, 4, "OASV") != 0 || get_le(d, 4, 2) != 1 || get_le(d, 6, 2) != kind)
        return SAVE_ERR_BAD;
    if (d.size() != 16 + size_t(get_le(d, 8, 4))) return SAVE_ERR_BAD;
    payload = d.substr(16);
    return crc32(payload) == get_le(d, 12, 4) ? 0 : SAVE_ERR_BAD;
}

// Writes next to the final name, then renames over it, so a crash leaves the old file whole.
bool write_save(const std::filesystem::path& p, SaveKind kind, const std::string& payload) {
    std::string d = "OASV";
    put_le(d, 1, 2);
    put_le(d, kind, 2);
    put_le(d, uint32_t(payload.size()), 4);
    put_le(d, crc32(payload), 4);
    d += payload;
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::filesystem::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(d.data(), std::streamsize(d.size()));
        f.flush();
        if (!f) return false;
    }
    std::filesystem::rename(tmp, p, ec);  // replaces an existing file (MoveFileEx on Windows)
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool is_byte_array(VM& vm, const Value& v) {
    return v.tag == Tag::Arr && vm.class_of(v)->is_kind_of(vm.find_class("ByteArray"));
}

std::string bytes_of(const Value& v) {
    std::string s;
    for (auto& x : as_arr(v)->items) s.push_back(char(x.i & 0xFF));
    return s;
}

// Copies a payload into part of a script buffer, zero beyond it. A payload larger than the room
// fails rather than being cut.
int fill_buffer(const Value& v, const std::string& payload, size_t at = 0, size_t room = SIZE_MAX) {
    auto& items = as_arr(v)->items;
    if (at > items.size()) return SAVE_ERR_ARG;
    room = std::min(room, items.size() - at);
    if (payload.size() > room) return SAVE_ERR_BAD;
    for (size_t i = 0; i < room; ++i) items[at + i] = Value::integer(i < payload.size() ? uint8_t(payload[i]) : 0);
    return 0;
}

}  // namespace

std::string text_of(VM& vm, Thread& th, const Value& v) {
    switch (v.tag) {
        case Tag::Str: return as_str(v)->b;
        case Tag::Nil: return "nil";
        case Tag::True: return "true";
        case Tag::False: return "false";
        case Tag::Int: return std::to_string(v.i);
        case Tag::Float: return py_repr(v.f);
        case Tag::Sym: return as_sym(v)->name;
        case Tag::Class: return as_cls(v)->name;
        case Tag::Arr: {
            std::string s = "[";
            bool first = true;
            for (auto& x : as_arr(v)->items) {
                if (!first) s += ", ";
                first = false;
                s += text_of(vm, th, x);
            }
            return s + "]";
        }
        case Tag::Obj: break;
    }
    Callable m = vm.lookup(v, vm.intern("toString"), 0);
    if (m && m.native_id != 1) {
        Value result = vm.call(v, m, {}, &th);
        if (result.tag == Tag::Str) return as_str(result)->b;
    }
    return "<" + as_obj(v)->cls->name + ">";
}

void install_natives(VM& vm) {
    auto cls_ = [&](const std::string& name) -> ClassObj* {
        ClassObj* c = vm.find_class(name);
        if (!c) throw std::runtime_error("built-in class " + name + " is not in the system image");
        return c;
    };
    for (const char* n : {"Object", "Class", "Interface", "Symbol", "String", "Array", "ByteArray", "UShortArray",
                          "Number", "Integer", "Float", "Boolean", "True", "False", "Nil", "Math", "System", "Thread",
                          "Throwable", "Exception", "Error"})
        vm.find_class(n);

    auto reg = [&](const char* cls, const char* name, int argc, NativeFn fn, bool class_side = false, int id = 0) {
        ClassObj* c = cls_(cls);
        Callable k;
        k.native = std::move(fn);
        k.native_id = id;
        (class_side ? c->cmethods : c->imethods)[VM::key(vm.intern(name), argc)] = std::move(k);
    };

#define NATIVE [](VM & vm, Thread & th, Value & r, std::vector<Value> & a) -> Value
#define UNUSED (void)vm; (void)th; (void)r; (void)a

    auto initialize = NATIVE {
        UNUSED;
        return r;
    };

    // --- Object, Nil ---------------------------------------------------------------------------
    reg("Object", "initialize", 0, initialize);
    reg("Object", "=", 1, NATIVE {
        UNUSED;
        return Value::boolean(same(r, a[0]));
    });
    reg("Object", "<>", 1, NATIVE {
        UNUSED;
        return Value::boolean(!same(r, a[0]));
    });
    reg("Object", "getClass", 0, NATIVE {
        UNUSED;
        Value v;
        ClassObj* c = vm.class_of(r);
        v.tag = Tag::Class;
        v.p = vm.classes.at(c->name);
        return v;
    });
    auto clone = NATIVE {
        UNUSED;
        if (r.tag == Tag::Obj) {
            Value c = vm.make_obj(as_obj(r)->cls);
            as_obj(c)->iv = as_obj(r)->iv;
            as_obj(c)->message = as_obj(r)->message;
            return c;
        }
        if (r.tag == Tag::Str) return vm.make_str(as_str(r)->b);
        if (r.tag == Tag::Arr) return vm.make_arr(as_arr(r)->cls, as_arr(r)->items);
        return r;
    };
    reg("Object", "clone", 0, clone);
    reg("Object", "toString", 0, NATIVE {
        UNUSED;
        if (r.tag == Tag::Obj) return vm.make_str(as_obj(r)->cls->name);  // engine (0x119F90)
        return vm.make_str(text_of(vm, th, r));
    }, false, 1);
    reg("Object", "hashCode", 0, NATIVE {
        UNUSED;
        if (r.tag == Tag::Int) return r;
        return Value::integer(r.p ? int64_t(r.p->serial & 0x3FFFFFFF) : 0);
    });
    auto sleep = NATIVE {
        UNUSED;
        th.sleep(a[0].tag == Tag::Int ? a[0].i : 1);
        return r;
    };
    reg("Object", "sleep", 1, sleep);
    reg("Object", "isKindOf", 1, NATIVE {
        UNUSED;
        return Value::boolean(a[0].tag == Tag::Class && vm.class_of(r)->is_kind_of(as_cls(a[0])));
    });
    auto perform = NATIVE {
        UNUSED;
        static const bool trace = std::getenv("AMG_VOICE_LOG") != nullptr;
        if (trace && as_sym(a[0])->name.rfind("inlineCom", 0) == 0)
            std::fprintf(stderr, "inline %s on %s\n", as_sym(a[0])->name.c_str(), r.tag == Tag::Obj ? as_obj(r)->cls->name.c_str() : "?");
        return vm.send(th, r, as_sym(a[0])->name, std::vector<Value>(a.begin() + 1, a.end()));
    };
    reg("Object", "perform", 1, perform);
    reg("Object", "performWith", 2, perform);
    reg("Object", "performWith", 3, perform);
    reg("Object", "performWithArguments", 2, NATIVE {
        UNUSED;
        return vm.send(th, r, as_sym(a[0])->name, as_arr(a[1])->items);
    });
    reg("Nil", "initialize", 0, initialize);
    reg("Nil", "clone", 0, clone);
    reg("Nil", "sleep", 1, sleep);
    reg("Nil", "toString", 0, NATIVE {
        UNUSED;
        return vm.make_str("nil");
    });

    // --- Class ---------------------------------------------------------------------------------
    for (int argc = 0; argc <= 8; ++argc)
        reg("Class", "new", argc, NATIVE {
            UNUSED;
            return vm.instantiate(as_cls(r), a, &th);
        });
    reg("Class", "newWithArray", 1, NATIVE {
        UNUSED;
        return vm.instantiate(as_cls(r), as_arr(a[0])->items, &th);
    });
    reg("Class", "name", 0, NATIVE {
        UNUSED;
        return vm.make_str(as_cls(r)->name);
    });
    reg("Class", "findClass", 1, NATIVE {
        UNUSED;
        std::string name = a[0].tag == Tag::Sym ? as_sym(a[0])->name : as_str(a[0])->b;
        // Only a class that is loaded: the image's, or a scenario class already brought in (loadClass, loadEvent, a
        // constant). TextWindow asks for MahjongMain to pause a game on L1 and open its settings on triangle; on the PS2
        // the class is only there while the mahjong runs, so in an event scene the buttons do nothing. The scripts confirm
        // that findClass does not load: K2X_Script#findEvent calls it first and falls back to loadClass when it is nil. Finding it there called the class methods with no game and failed.
        if (!vm.classes.count(name) && !vm.image_classes.count(name)) return Value();
        ClassObj* c = vm.find_class(name);
        if (!c) return Value();  // nil when there is no such class
        Value v;
        v.tag = Tag::Class;
        v.p = vm.classes.at(name);
        return v;
    }, true);
    reg("Class", "initialize", 0, initialize);

    // --- Symbol --------------------------------------------------------------------------------
    reg("Symbol", "new", 1, NATIVE {
        UNUSED;
        return vm.symbol(as_str(a[0])->b);
    }, true);
    reg("Symbol", "getSymbolIfExist", 1, NATIVE {
        UNUSED;
        auto it = vm.symbols.find(as_str(a[0])->b);
        if (it == vm.symbols.end()) return Value();
        return vm.symbol(it->first);
    }, true);
    reg("Symbol", "toString", 0, NATIVE {
        UNUSED;
        return vm.make_str(as_sym(r)->name);
    });

    // --- String --------------------------------------------------------------------------------
    reg("String", "initialize", 0, initialize);
    reg("String", "initialize", 1, initialize);
    reg("String", "new", 0, NATIVE {
        UNUSED;
        return vm.make_str("");
    }, true);
    reg("String", "new", 1, NATIVE {
        UNUSED;
        if (a[0].tag == Tag::Int) return vm.make_str(std::string(size_t(std::max<int64_t>(0, a[0].i)), ' '));
        return vm.make_str(a[0].tag == Tag::Str ? as_str(a[0])->b : text_of(vm, th, a[0]));
    }, true);
    reg("String", "clone", 0, NATIVE {
        UNUSED;
        return vm.make_str(as_str(r)->b);
    });
    reg("String", "hashCode", 0, NATIVE {
        UNUSED;
        // The engine's (0x11D250): over the 16-bit characters, h = h + (h ^ c); keep 30 bits. HashMap
        // walks its table in hash order, so this decides the order of every table keyed by strings.
        uint32_t h = 0;
        for (int c : chars(as_str(r)->b)) h += h ^ uint32_t(c & 0xFFFF);
        return Value::integer(h & 0x3FFFFFFF);
    });
    reg("String", "length", 0, NATIVE {
        UNUSED;
        return Value::integer(int64_t(chars(as_str(r)->b).size()));
    });
    reg("String", "toString", 0, NATIVE {
        UNUSED;
        return r;
    });
    reg("String", "+", 1, NATIVE {
        UNUSED;
        return vm.make_str(as_str(r)->b + text_of(vm, th, a[0]));
    });
    reg("String", "=", 1, NATIVE {
        UNUSED;
        return Value::boolean(a[0].tag == Tag::Str && as_str(a[0])->b == as_str(r)->b);
    });
    reg("String", "<>", 1, NATIVE {
        UNUSED;
        return Value::boolean(!(a[0].tag == Tag::Str && as_str(a[0])->b == as_str(r)->b));
    });
    reg("String", "[]", 1, NATIVE {
        UNUSED;
        std::vector<int> codes = chars(as_str(r)->b);
        if (a[0].tag != Tag::Int || a[0].i < 0 || a[0].i >= int64_t(codes.size()))
            vm.raise("String index out of range", "ArrayIndexOutOfBoundsException");
        return Value::integer(codes[size_t(a[0].i)]);
    });
    reg("String", "atPut", 2, NATIVE {
        UNUSED;
        std::vector<int> codes = chars(as_str(r)->b);
        if (a[0].tag != Tag::Int || a[0].i < 0 || a[0].i >= int64_t(codes.size()))
            vm.raise("String index out of range", "ArrayIndexOutOfBoundsException");
        codes[size_t(a[0].i)] = int(a[1].i & 0xFFFF);
        as_str(r)->b = unchars(codes);
        return a[1];
    });
    reg("String", "append", 1, NATIVE {
        UNUSED;
        as_str(r)->b += text_of(vm, th, a[0]);
        return r;
    });
    reg("String", "setLength", 1, NATIVE {
        UNUSED;
        std::vector<int> codes = chars(as_str(r)->b);
        size_t n = size_t(a[0].i);
        codes.resize(n, 0x20);
        as_str(r)->b = unchars(codes);
        return r;
    });
    reg("String", "chararray", 0, NATIVE {
        UNUSED;
        std::vector<Value> items;
        for (int c : chars(as_str(r)->b)) items.push_back(Value::integer(c));
        return vm.make_arr(vm.find_class("UShortArray"), std::move(items));
    });
    reg("String", "toByteArray", 0, NATIVE {
        UNUSED;
        std::vector<Value> items;
        for (unsigned char c : as_str(r)->b) items.push_back(Value::integer(c));
        return vm.make_arr(vm.find_class("ByteArray"), std::move(items));
    });
    reg("String", "copyFromByteArray", 1, NATIVE {
        UNUSED;
        std::string s;
        for (auto& x : as_arr(a[0])->items) s.push_back(char(x.i));
        as_str(r)->b = s;
        return r;
    });
    reg("String", "newFromByteArray", 1, NATIVE {
        UNUSED;
        std::string s;
        for (auto& x : as_arr(a[0])->items) s.push_back(char(x.i));
        return vm.make_str(s);
    }, true);

    // --- Arrays --------------------------------------------------------------------------------
    for (auto [name, zero] : {std::pair<const char*, int>{"Array", -1}, {"ByteArray", 0}, {"UShortArray", 0}}) {
        int z = zero;
        reg(name, "new", 1, [z](VM& vm, Thread&, Value& r, std::vector<Value>& a) -> Value {
            if (a[0].tag != Tag::Int) vm.raise("Array:new argument is not Integer");
            Value fill = z < 0 ? Value() : Value::integer(z);
            return vm.make_arr(as_cls(r), std::vector<Value>(size_t(std::max<int64_t>(0, a[0].i)), fill));
        }, true);
        reg(name, "initialize", 0, initialize);
        reg(name, "initialize", 1, initialize);
        reg(name, "clone", 0, NATIVE {
            UNUSED;
            return vm.make_arr(as_arr(r)->cls, as_arr(r)->items);
        });
        reg(name, "[]", 1, NATIVE {
            UNUSED;
            auto& items = as_arr(r)->items;
            if (a[0].tag != Tag::Int || a[0].i < 0 || a[0].i >= int64_t(items.size()))
                vm.raise("array index out of range", "ArrayIndexOutOfBoundsException");
            return items[size_t(a[0].i)];
        });
        reg(name, "atPut", 2, NATIVE {
            UNUSED;
            auto& items = as_arr(r)->items;
            if (a[0].tag != Tag::Int || a[0].i < 0 || a[0].i >= int64_t(items.size()))
                vm.raise("array index out of range", "ArrayIndexOutOfBoundsException");
            items[size_t(a[0].i)] = a[1];
            return a[1];
        });
        reg(name, "length", 0, NATIVE {
            UNUSED;
            return Value::integer(int64_t(as_arr(r)->items.size()));
        });
        reg(name, "append", 2, NATIVE {
            UNUSED;
            auto& dst = as_arr(r)->items;
            std::vector<Value> src;
            if (a[1].tag == Tag::Arr) {
                src = as_arr(a[1])->items;
            } else {
                for (unsigned char c : as_str(a[1])->b) src.push_back(Value::integer(c));
            }
            size_t pos = size_t(a[0].i);
            if (dst.size() < pos + src.size()) dst.resize(pos + src.size());
            for (size_t i = 0; i < src.size(); ++i) dst[pos + i] = src[i];
            return r;
        });
        reg(name, "equals", 1, NATIVE {
            UNUSED;
            if (a[0].tag != Tag::Arr) return Value::boolean(false);
            auto& x = as_arr(a[0])->items;
            auto& y = as_arr(r)->items;
            if (x.size() != y.size()) return Value::boolean(false);
            for (size_t i = 0; i < x.size(); ++i)
                if (!same(x[i], y[i]) && !(x[i].tag == Tag::Str && y[i].tag == Tag::Str && as_str(x[i])->b == as_str(y[i])->b))
                    return Value::boolean(false);
            return Value::boolean(true);
        });
        reg(name, "copyFrom", 1, NATIVE {
            UNUSED;
            auto& src = as_arr(a[0])->items;
            auto& dst = as_arr(r)->items;
            // Copies as many as the receiver holds. `Heroine.getList` builds `Array.new(count).copyFrom(vector.image())`
            // from a vector whose array has room for eight: the receiver must stay `count` long (confirmed: with
            // growth every list had eight nil entries and the scenes that judge by `getList(3).length()` failed).
            size_t n = std::min(dst.size(), src.size());
            for (size_t i = 0; i < n; ++i) dst[i] = src[i];
            return r;
        });
    }

    // --- Numbers -------------------------------------------------------------------------------
    for (const char* cname : {"Integer", "Float"}) {
        auto arith = [&](const char* op, int kind) {
            reg(cname, op, 1, [kind, cname](VM& vm, Thread&, Value& r, std::vector<Value>& a) -> Value {
                const Value& y = a[0];
                if (!is_number(y)) {
                    // `=` and `<>` answer false and true for anything that is not a number (nil, an object); only the
                    // other operators raise (Integer's natives at 0x11FEC0, 0x1201F8 and Float's at 0x120D00, read).
                    // RenaiMapParam#check compares a position with a nil one after a load.
                    if (kind == 9) return Value::boolean(false);
                    if (kind == 10) return Value::boolean(true);
                    vm.raise(std::string(cname) + ": right value was not Number");
                }
                bool ints = r.tag == Tag::Int && y.tag == Tag::Int;
                if (ints) {
                    int64_t x = r.i, z = y.i;
                    switch (kind) {
                        case 0: return Value::integer(x + z);
                        case 1: return Value::integer(x - z);
                        case 2: return Value::integer(x * z);
                        case 3: case 4: {
                            if (z == 0) throw std::runtime_error("ZeroDivisionError");
                            int64_t q = (x < 0 ? -x : x) / (z < 0 ? -z : z);
                            q = ((x >= 0) == (z >= 0)) ? q : -q;
                            return Value::integer(kind == 3 ? q : x - q * z);
                        }
                        case 5: return Value::boolean(x > z);
                        case 6: return Value::boolean(x < z);
                        case 7: return Value::boolean(x >= z);
                        case 8: return Value::boolean(x <= z);
                        case 9: return Value::boolean(x == z);
                        default: return Value::boolean(x != z);
                    }
                }
                double x = num(r), z = num(y);
                switch (kind) {
                    case 0: return Value::real(x + z);
                    case 1: return Value::real(x - z);
                    case 2: return Value::real(x * z);
                    case 3:
                        if (z == 0) throw std::runtime_error("ZeroDivisionError");
                        return Value::real(x / z);
                    case 4: return Value::real(std::fmod(x, z));
                    case 5: return Value::boolean(x > z);
                    case 6: return Value::boolean(x < z);
                    case 7: return Value::boolean(x >= z);
                    case 8: return Value::boolean(x <= z);
                    case 9: return Value::boolean(x == z);
                    default: return Value::boolean(x != z);
                }
            });
        };
        arith("+", 0); arith("-", 1); arith("*", 2); arith("/", 3); arith("%", 4); arith(">", 5); arith("<", 6);
        arith(">=", 7); arith("<=", 8); arith("=", 9); arith("<>", 10);
        reg(cname, "-U", 0, NATIVE {
            UNUSED;
            return r.tag == Tag::Int ? Value::integer(-r.i) : Value::real(-r.f);
        });
        reg(cname, "toString", 0, NATIVE {
            UNUSED;
            return vm.make_str(text_of(vm, th, r));
        });
        reg(cname, "toInteger", 0, NATIVE {
            UNUSED;
            return r.tag == Tag::Int ? r : Value::integer(int64_t(r.f));
        });
        reg(cname, "toFloat", 0, NATIVE {
            UNUSED;
            return Value::real(num(r));
        });
    }
    auto bitop = [&](const char* op, int kind) {
        reg("Integer", op, 1, [kind](VM& vm, Thread&, Value& r, std::vector<Value>& a) -> Value {
            if (a[0].tag != Tag::Int) vm.raise("Integer: right value was not Number");
            int64_t x = r.i, y = a[0].i;
            switch (kind) {
                case 0: return Value::integer(x & y);
                case 1: return Value::integer(x | y);
                case 2: return Value::integer(x ^ y);
                case 3: return Value::integer(y >= 0 && y < 63 ? (x << y) : 0);
                default: return Value::integer(y >= 0 && y < 63 ? (x >> y) : (x < 0 ? -1 : 0));
            }
        });
    };
    bitop("&", 0); bitop("|", 1); bitop("^", 2); bitop("shiftL", 3); bitop("shiftR", 4);
    reg("Integer", "~U", 0, NATIVE {
        UNUSED;
        return Value::integer(~r.i);
    });
    reg("Integer", "asChar", 0, NATIVE {
        UNUSED;
        return vm.make_str(unchars({int(r.i & 0xFFFF)}));
    });
    reg("Integer", "toByteArray", 0, NATIVE {
        UNUSED;
        int32_t v = int32_t(r.i);
        unsigned char b[4];
        std::memcpy(b, &v, 4);
        std::vector<Value> items;
        for (unsigned char c : b) items.push_back(Value::integer(c));
        return vm.make_arr(vm.find_class("ByteArray"), std::move(items));
    });
    // The inverses of `toByteArray`, used by `Serializer.parseInteger` / `parseFloat` when a save
    // is read back. Little-endian, like `toByteArray` (and like the PS2).
    auto first4 = [](VM& vm, const Value& r) {
        auto& items = as_arr(r)->items;
        if (items.size() < 4) vm.raise("ByteArray shorter than 4 bytes", "ArrayIndexOutOfBoundsException");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(items[size_t(i)].i & 0xFF) << (8 * i);
        return v;
    };
    reg("ByteArray", "asInteger", 0, [first4](VM& vm, Thread&, Value& r, std::vector<Value>&) -> Value {
        return Value::integer(int32_t(first4(vm, r)));
    });
    reg("ByteArray", "asFloat", 0, [first4](VM& vm, Thread&, Value& r, std::vector<Value>&) -> Value {
        uint32_t u = first4(vm, r);
        float f;
        std::memcpy(&f, &u, 4);
        return Value::real(f);
    });
    reg("Float", "toByteArray", 0, NATIVE {
        UNUSED;
        float v = float(r.f);
        unsigned char b[4];
        std::memcpy(b, &v, 4);
        std::vector<Value> items;
        for (unsigned char c : b) items.push_back(Value::integer(c));
        return vm.make_arr(vm.find_class("ByteArray"), std::move(items));
    });
    reg("Float", "initialize", 1, initialize);
    reg("Integer", "initialize", 1, initialize);

    // AMG_SEED starts the sequence elsewhere (tests: another mahjong deal); the default keeps runs reproducible.
    static int64_t seed = std::getenv("AMG_SEED") ? std::atoll(std::getenv("AMG_SEED")) : 12345;
    reg("Number", "rnd", 1, NATIVE {
        UNUSED;
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF;
        return Value::integer(a[0].tag == Tag::Int && a[0].i > 0 ? seed % a[0].i : 0);
    }, true);

    // --- Booleans ------------------------------------------------------------------------------
    for (const char* cname : {"True", "False"}) {
        auto truth = [](const Value& v) { return v.tag == Tag::True; };
        (void)truth;
        reg(cname, "&&", 1, NATIVE {
            UNUSED;
            return Value::boolean(r.tag == Tag::True && a[0].tag == Tag::True);
        });
        reg(cname, "||", 1, NATIVE {
            UNUSED;
            return Value::boolean(r.tag == Tag::True || a[0].tag == Tag::True);
        });
        reg(cname, "&", 1, NATIVE {
            UNUSED;
            return Value::boolean(r.tag == Tag::True && a[0].tag == Tag::True);
        });
        reg(cname, "|", 1, NATIVE {
            UNUSED;
            return Value::boolean(r.tag == Tag::True || a[0].tag == Tag::True);
        });
        reg(cname, "^", 1, NATIVE {
            UNUSED;
            return Value::boolean((r.tag == Tag::True) != (a[0].tag == Tag::True));
        });
        reg(cname, "!U", 0, NATIVE {
            UNUSED;
            return Value::boolean(r.tag != Tag::True);
        });
        reg(cname, "ifTrue", 1, NATIVE {
            UNUSED;
            return r.tag == Tag::True ? a[0] : Value();
        });
        reg(cname, "ifFalse", 1, NATIVE {
            UNUSED;
            return r.tag == Tag::True ? Value() : a[0];
        });
        reg(cname, "ifTrueIfFalse", 2, NATIVE {
            UNUSED;
            return r.tag == Tag::True ? a[0] : a[1];
        });
        reg(cname, "ifFalseIfTrue", 2, NATIVE {
            UNUSED;
            return r.tag == Tag::True ? a[1] : a[0];
        });
        reg(cname, "toString", 0, NATIVE {
            UNUSED;
            return vm.make_str(r.tag == Tag::True ? "true" : "false");
        });
        reg(cname, "clone", 0, NATIVE {
            UNUSED;
            return r;
        });
    }

    // --- Math, System --------------------------------------------------------------------------
    auto math1 = [&](const char* name, double (*fn)(double)) {
        reg("Math", name, 1, [fn](VM&, Thread&, Value&, std::vector<Value>& a) -> Value {
            return Value::real(fn(num(a[0])));
        }, true);
    };
    math1("sin", std::sin); math1("cos", std::cos); math1("tan", std::tan); math1("asin", std::asin);
    math1("acos", std::acos); math1("atan", std::atan); math1("sqrt", std::sqrt);
    reg("Math", "abs", 1, NATIVE {
        UNUSED;
        return a[0].tag == Tag::Int ? Value::integer(a[0].i < 0 ? -a[0].i : a[0].i) : Value::real(std::fabs(a[0].f));
    }, true);

    auto print = NATIVE {
        UNUSED;
        vm.out.push_back(text_of(vm, th, a[0]));
        return Value();
    };
    // --- PortSettings: the port's rows in the game's Settings screen (port/script, src/port_settings.hpp) ---------------
    if (vm.sources.count("PortSettings")) {
        reg("PortSettings", "count", 0, NATIVE {
            UNUSED;
            return Value::integer(amg::port_settings::count());
        }, true);
        reg("PortSettings", "label", 1, NATIVE {
            UNUSED;
            return vm.make_str(amg::port_settings::label(int(a[0].i)));
        }, true);
        reg("PortSettings", "text", 1, NATIVE {
            UNUSED;
            return vm.make_str(amg::port_settings::text(int(a[0].i)));
        }, true);
        reg("PortSettings", "step", 2, NATIVE {
            UNUSED;
            amg::port_settings::step(int(a[0].i), int(a[1].i));
            return Value();
        }, true);
    }
    reg("System", "print", 1, print, true);
    reg("System", "println", 1, print, true);
    for (const char* n : {"getLine", "gc", "FullGC", "gcfr"})
        reg("System", n, 0, NATIVE {
            UNUSED;
            return Value();
        }, true);
    reg("System", "initialize", 0, initialize);

    // --- Thread --------------------------------------------------------------------------------
    reg("Thread", "currentThread", 0, NATIVE {
        UNUSED;
        if (th.object.tag == Tag::Nil) {
            th.object = vm.make_obj(vm.find_class("Thread"));
            as_obj(th.object)->thread = &th;
        }
        return th.object;
    }, true);
    // killall (0x11F540, read): every script thread but the caller ends. K2X_Script.reset (a load, back to the title)
    // calls it before setup builds the actors, stages and windows again; doing nothing here left the old ones
    // running, thirty more actor threads at each reset, and long sessions slowed down (case C2).
    reg("Thread", "killall", 0, NATIVE {
        UNUSED;
        for (Thread* t : vm.live_threads)
            if (t != &th) t->done = true;
        return Value();
    }, true);
    reg("Thread", "initialize", 0, initialize);
    reg("Thread", "start", 1, NATIVE {
        UNUSED;
        Thread* t = vm.new_thread(a[0], "run", {}, "thread");
        t->object = r;
        as_obj(r)->thread = t;
        t->parent = &th;
        t->wake_tick = vm.tick;
        t->start();
        return r;
    });
    reg("Thread", "kill", 0, NATIVE {
        UNUSED;
        if (as_obj(r)->thread) as_obj(r)->thread->done = true;
        return r;
    });
    reg("Thread", "parent", 0, NATIVE {
        UNUSED;
        Thread* t = as_obj(r)->thread;
        return t && t->parent ? t->parent->object : Value();
    });
    reg("Thread", "detach", 0, NATIVE {
        UNUSED;
        return r;
    });
    reg("Thread", "isHalt", 0, NATIVE {
        UNUSED;
        Thread* t = as_obj(r)->thread;
        return Value::boolean(t && t->done);
    });

    // Debug aid (AMG_SKIP_PROLOGUE, or --skip-prologue in the window): the prologue scene runs in the
    // game's own skip mode (GameParam.isSkip answers true while it runs), so it goes by quickly but
    // still does everything it does in play, such as opening the heroines' nodes for day 1.
    if (std::getenv("AMG_SKIP_PROLOGUE")) {
        static bool forcing = false;
        ClassObj* k2x = vm.find_class("K2X_Script");
        ClassObj* param = vm.find_class("GameParam");
        if (k2x && param) {
            auto chain = k2x->imethods.find(VM::key(vm.intern("execEventChain"), 1));
            auto skip = param->cmethods.find(VM::key(vm.intern("isSkip"), 0));
            if (chain != k2x->imethods.end() && chain->second.method && skip != param->cmethods.end() &&
                skip->second.method) {
                std::shared_ptr<Method> original_chain = chain->second.method, original_skip = skip->second.method;
                Callable wrap_chain;
                wrap_chain.native = [original_chain](VM&, Thread& th, Value& r, std::vector<Value>& a) -> Value {
                    bool prologue = !a.empty() && a[0].tag == Tag::Str && as_str(a[0])->b == "n0dea_ev01A";
                    if (prologue) forcing = true;
                    Value result = th.invoke(r, original_chain, a);
                    if (prologue) forcing = false;
                    return result;
                };
                chain->second = wrap_chain;
                Callable wrap_skip;
                wrap_skip.native = [original_skip](VM&, Thread& th, Value& r, std::vector<Value>& a) -> Value {
                    return forcing ? Value::boolean(true) : th.invoke(r, original_skip, a);
                };
                skip->second = wrap_skip;
            }
        }
    }

    // Scene harness (AMG_SCENE=<event name>): the first event the game starts after the title is
    // replaced by that scene, which runs in the game's skip mode (GameParam.isSkip true), and the
    // machine stops when it returns. Used by `amagami_vm --scene`.
    if (const char* scene = std::getenv("AMG_SCENE")) {
        static bool force_skip = false;
        vm.scene_name = scene;
        // Fast boot: the title screens, the menu and the prologue's map answer at once (AMG_SCENE_BOOT=full
        // keeps the real ones). Nothing the scene needs comes from them: the first event is reached the
        // same way, with the defaults the game's own start sets up.
        if (!std::getenv("AMG_SCENE_BOOT")) {
            auto answer = [&vm](const char* cls, bool class_side, const char* name, int argc, Value result) {
                ClassObj* c = vm.find_class(cls);
                if (!c) return;
                auto& table = class_side ? c->cmethods : c->imethods;
                auto it = table.find(VM::key(vm.intern(name), argc));
                if (it == table.end()) return;
                Callable fixed;
                fixed.native = [result](VM&, Thread&, Value&, std::vector<Value>&) -> Value { return result; };
                it->second = fixed;
            };
            answer("TitleLogo", false, "scriptMain", 1, Value());
            answer("MainMenu", false, "scriptMain", 0, Value::boolean(true));
            answer("NodeMapMain", false, "selectProlog", 0, vm.make_str("n0dea_ev01A"));
        }
        ClassObj* k2x = vm.find_class("K2X_Script");
        ClassObj* param = vm.find_class("GameParam");
        if (k2x && param) {
            auto exec = k2x->imethods.find(VM::key(vm.intern("execEvent"), 1));
            auto skip = param->cmethods.find(VM::key(vm.intern("isSkip"), 0));
            if (exec != k2x->imethods.end() && exec->second.method && skip != param->cmethods.end() &&
                skip->second.method) {
                std::shared_ptr<Method> original_exec = exec->second.method, original_skip = skip->second.method;
                // AMG_SCENE_STATE=fan: a game in progress instead of a new one. The mid-chapter scenes choose among
                // the heroines by their level (getList(3) likes, getList(4) friends), which is empty in a new
                // game. Heroine 1 likes the player (level 2, star 2), 2 and 3 are friends (level 2, star 1), the
                // others are acquaintances (level 1); heroines 1 and 2 are the Christmas candidates. `level` and `star` are
                // found by name (instance variables 29 and 32 on the PS2, 30 and 33 in the Vita's system image).
                auto preset_state = [](VM& v, Thread& th) {
                    ClassObj* hero = v.find_class("Heroine");
                    if (!hero) return;
                    auto get = hero->cmethods.find(VM::key(v.intern("getObj"), 1));
                    if (get == hero->cmethods.end() || !get->second.method) return;
                    int level_slot = -1, star_slot = -1;
                    if (hero->src)
                        for (const auto& [slot, name] : hero->src->instance_vars) {
                            if (name == "level") level_slot = slot;
                            if (name == "star") star_slot = slot;
                        }
                    if (level_slot < 0 || star_slot < 0) return;
                    Value cls;
                    cls.tag = Tag::Class;
                    cls.p = v.classes.at("Heroine");
                    for (int n = 1; n <= 8; ++n) {
                        Value h = th.invoke(cls, get->second.method, {Value::integer(n - 1)});
                        if (h.tag != Tag::Obj) continue;
                        auto* o = static_cast<ObjObj*>(h.p.get());
                        int level = n == 1 ? 2 : n <= 3 ? 2 : 1, star = n == 1 ? 2 : n <= 3 ? 1 : 0;
                        o->iv[size_t(level_slot)] = Value::integer(level);
                        o->iv[size_t(star_slot)] = Value::integer(star);
                        // Heroines 1 and 2 are the two Christmas candidates (`Heroine.getXmas`), which the
                        // December scenes (`skx_me03*`) read as a pair.
                        if (n <= 2) v.send(th, h, "setXmas", {});
                    }
                };
                const bool state_fan = std::getenv("AMG_SCENE_STATE") && std::string(std::getenv("AMG_SCENE_STATE")) == "fan";
                Callable wrap_exec;
                wrap_exec.native = [original_exec, preset_state, state_fan](VM& v, Thread& th, Value& r, std::vector<Value>& a) -> Value {
                    if (state_fan && !v.scene_started) preset_state(v, th);
#ifndef _WIN32
                    // --fork: the game booted once and stands at its first event. Each scene of the list runs in a
                    // copy of this process (fork), so it starts from exactly this state, as if alone, and the boot
                    // (most of a small scene's instructions) is not repeated. The parent only waits; a child that
                    // runs out of wall-clock time is killed by its alarm and reported here.
                    if (v.fork_scenes && !v.scene_started && !v.fork_child) {
                        std::vector<std::string> queue = v.scene_queue;
                        if (queue.empty()) queue.push_back(v.scene_name);
                        for (const std::string& name : queue) {
                            std::fflush(stdout);
                            std::fflush(stderr);
                            pid_t pid = fork();
                            if (pid == 0) {
                                v.fork_child = true;
                                v.scene_queue = {name};
                                v.scene_name = name;
                                if (v.fork_timeout > 0) alarm(unsigned(v.fork_timeout));
                                break;
                            }
                            int status = 0;
                            waitpid(pid, &status, 0);
                            if (WIFSIGNALED(status)) {
                                int sig = WTERMSIG(status);
                                std::printf("SCENE %s %s ticks=0 steps=0 signal %d\n", name.c_str(),
                                            sig == SIGALRM ? "TIMEOUT" : "CRASH", sig);
                                std::fflush(stdout);
                            }
                        }
                        if (!v.fork_child) {
                            std::fflush(stdout);
                            std::_Exit(0);
                        }
                    }
#endif
                    if (v.scene_chain) {
                        // The first event is the scene asked for; the ones it chains to follow as in the game.
                        bool first = !v.scene_started;
                        v.scene_started = true;
                        if (first) {
                            force_skip = true;
                            if (!a.empty()) a[0] = v.make_str(v.scene_name);
                        }
                        if (!a.empty() && a[0].tag == Tag::Str) v.chain_names.push_back(as_str(a[0])->b);
                        return th.invoke(r, original_exec, a);
                    }
                    if (v.scene_started) return th.invoke(r, original_exec, a);
                    v.scene_started = true;
                    force_skip = true;
                    Value result;
                    if (v.scene_queue.empty()) v.scene_queue.push_back(v.scene_name);
                    // One scene or a list of them: after each one that ends normally the next starts in the
                    // same machine. A scene that fails or hangs ends the process (main prints its line) and
                    // the driver starts a new one with the rest.
                    for (size_t i = 0; i < v.scene_queue.size(); ++i) {
                        v.scene_name = v.scene_queue[i];
                        int64_t steps_before = v.steps, tick_before = v.tick;
                        if (v.scene_budget > 0) {
                            // The staff roll of an ending lasts about 100 million instructions (98 M measured).
                            const std::string& n = v.scene_name;
                            // n0skx_ep00A (the prologue's long last scene) needs about 110 million.
                            bool ending = (n.size() > 8 && n[5] == '_' && n[6] == 'x' && (n[7] == 'd' || n[7] == 'p' || n[7] == 's')) ||
                                          n == "n0skx_ep00A";
                            v.max_steps = v.steps + (ending ? std::max<int64_t>(v.scene_budget, 150'000'000) : v.scene_budget);
                        }
                        v.stub_hits.clear();
                        std::vector<Value> args = {v.make_str(v.scene_name)};
                        // A script that is not a node of the event graph (a scene another one chains to,
                        // a cut-in) has no read record, and execEvent would fail on it. It is run the way
                        // execEvent runs one: build the class, then scriptMain(block) until it answers
                        // something that is not an integer.
                        Value event_node;
                        if (ClassObj* en = v.find_class("EventNode")) {
                            event_node.tag = Tag::Class;
                            event_node.p = v.classes.at("EventNode");
                            (void)en;
                        }
                        bool is_node = false;
                        if (event_node.tag == Tag::Class) {
                            Value node = v.send(th, event_node, "getNode", args);
                            is_node = node.tag != Tag::Nil;
                        }
                        if (is_node) {
                            result = th.invoke(r, original_exec, args);
                        } else {
                            if (std::getenv("AMG_LOADER_DEBUG"))
                                std::fprintf(stderr, "direct run of %s: source %zu, class %p%c", v.scene_name.c_str(),
                                             v.sources.count(v.scene_name), (void*)v.find_class(v.scene_name), 10);
                            Value found = v.send(th, r, "findEvent", args);
                            if (found.tag == Tag::Nil) {
                                v.raise("no event class named " + v.scene_name);
                            }
                            Value object = v.send(th, found, "new", {});
                            Value block = Value::integer(0);
                            for (;;) {
                                result = v.send(th, object, "scriptMain", {block});
                                if (result.tag != Tag::Int) break;
                                block = result;
                            }
                        }
                        if (v.scene_queue.size() > 1) {
                            std::string stubs;
                            for (const auto& h : v.stub_hits) stubs += (stubs.empty() ? "" : ",") + h;
                            std::printf("SCENE %s DONE ticks=%lld steps=%lld\n", v.scene_name.c_str(),
                                        (long long)(v.tick - tick_before), (long long)(v.steps - steps_before));
                            std::printf("SCENEINFO %s at=- stubs=%s\n", v.scene_name.c_str(),
                                        stubs.empty() ? "-" : stubs.c_str());
                            std::fflush(stdout);
                        }
                    }
                    force_skip = false;
                    v.scene_done = true;
                    v.max_steps = 0;  // every script thread ends at its next instruction
                    return result;
                };
                exec->second = wrap_exec;
                // With --chain the run ends when the chain does (execEventChain returns).
                auto chain_it = k2x->imethods.find(VM::key(vm.intern("execEventChain"), 1));
                if (std::getenv("AMG_SCENE_CHAIN") && chain_it != k2x->imethods.end() && chain_it->second.method) {
                    vm.scene_chain = true;
                    std::shared_ptr<Method> original_chain = chain_it->second.method;
                    Callable wrap_chain;
                    wrap_chain.native = [original_chain](VM& v, Thread& th, Value& r, std::vector<Value>& a) -> Value {
                        Value result = th.invoke(r, original_chain, a);
                        if (v.scene_started) {
                            force_skip = false;
                            v.scene_done = true;
                            v.max_steps = 0;
                        }
                        return result;
                    };
                    chain_it->second = wrap_chain;
                }
                Callable wrap_skip;
                // AMG_SCENE_NOSKIP: the scene runs at the game's own pace (reference screens against PCSX2).
                static const bool no_skip = std::getenv("AMG_SCENE_NOSKIP") != nullptr;
                wrap_skip.native = [original_skip](VM&, Thread& th, Value& r, std::vector<Value>& a) -> Value {
                    return force_skip && !no_skip ? Value::boolean(true) : th.invoke(r, original_skip, a);
                };
                skip->second = wrap_skip;
            }
        }
    }

    // --- Engine primitives: headless stubs -----------------------------------------------------
    static const std::pair<const char*, int> counts[] = {
        {"ActorPicture", 26}, {"BG_Picture", 17}, {"Balloon", 2}, {"ClockIcon", 4}, {"ControlPad", 7},
        {"CutInWindow", 2}, {"EventPicture", 4}, {"ExtraPicture", 15}, {"FontChar", 11}, {"FontChar2", 2},
        {"FontCharEx", 1}, {"K2X_Script", 5}, {"LightBall", 2}, {"MemoryCard", 11}, {"Music", 6},
        {"NodeMapIcon", 8}, {"NodeMapNaviBase", 3}, {"SnowEffect", 9}, {"Sound", 6}, {"Sprite", 19},
        {"StandExt", 1}, {"StructPicture", 2}, {"Voice", 11}};
    // A primitive that does not touch slot 0 returns `self`, which lets script wrappers chain
    // (`sprite.setPos(...).setAlpha(...)`). Some only update state that the script reads back.
    PrimFn stub = [](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value { return st[b]; };
    for (auto& [name, count] : counts) {
        ClassObj* c = vm.find_class(name);
        if (!c) continue;
        for (int n = 0; n < count; ++n) {
            // The same stub, but it leaves a trace: which stubbed primitives a run reaches is what
            // `amagami_vm --scene` reports, and what decides which to implement first.
            std::string label = std::string(name) + "." + std::to_string(n);
            c->prims[n] = [label](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
                v.stub_hits.insert(label);
                return st[b];
            };
        }
    }
    auto prim = [&](const char* cls, int n, PrimFn fn) {
        ClassObj* c = vm.find_class(cls);
        if (c) c->prims[n] = std::move(fn);
    };
    // Scripted input and a formatted memory card.
    // On the Vita, a scenario script (not a class of the system image) that reads the pad gets the PS2's mask: in the
    // emulator the mahjong table moves its cursor with the d-pad and opens its menu with triangle, which only fits
    // the PS2 values its scripts test (`& 32768`, `& 16`), while the system image tests SceCtrl's. How the
    // eboot tells them apart is not known: the caller's class is the rule used here (hypothesis, F4-52).
    auto caller_is_scenario = [](VM& v, Thread& t) {
        if (!v.vita_buttons || t.frames.size() < 2) return false;
        const Method* caller = t.frames[t.frames.size() - 2].method;
        return caller && caller->cls && !v.image_classes.count(caller->cls->name);
    };
    prim("ControlPad", 0, [caller_is_scenario](VM& v, Thread& t, std::vector<Value>&, size_t, Method&) {
        return Value::integer(v.buttons_held(caller_is_scenario(v, t)));
    });
    prim("ControlPad", 1, [caller_is_scenario](VM& v, Thread& t, std::vector<Value>&, size_t, Method&) {
        return Value::integer(v.buttons_pressed(caller_is_scenario(v, t)));
    });
    prim("ControlPad", 3, [](VM&, Thread&, std::vector<Value>&, size_t, Method&) { return Value::integer(0); });
    // 5 rapid: the newly pressed buttons plus the engine's auto-repeat, the PS2's or the Vita's (VM::buttons_rapid).
    prim("ControlPad", 5, [caller_is_scenario](VM& v, Thread& t, std::vector<Value>&, size_t, Method&) {
        return Value::integer(v.buttons_rapid(caller_is_scenario(v, t)));
    });
    // 2 analog(n) (0x10C7D8, read; Vita 0x81001D50): axis n for 0 to 3, as a signed value centred on 0; on Vita
    // 4 and 5 are the left and right stick's length, int(sqrt(x*x + y*y)) (the map reads analog(4)), anything else
    // 0. It used to answer 128, a stick pushed fully over, which scrolled the album's tall pictures on their own
    // (A0_album#UpDown).
    prim("ControlPad", 2, [](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) {
        int64_t n = st[b + 1].tag == Tag::Int ? st[b + 1].i : 0;
        auto axis = [&](int64_t k) { return v.tick < v.analog_from ? 0 : v.analog[k]; };
        if (n >= 0 && n < 4) return Value::integer(axis(n));
        if (v.vita_buttons && (n == 4 || n == 5)) {
            double x = axis((n - 4) * 2), y = axis((n - 4) * 2 + 1);
            return Value::integer(int64_t(float(std::sqrt(float(x * x + y * y)))));
        }
        return Value::integer(0);
    });
    // 4 actPausePrim(level, on): vibration (ControlPad.scfs, two arguments). 6 resetRapid (no arguments): restarts the
    // auto-repeat count (Vita 0x8107CB44 clears it). They used to be the other way round, so rumble never ran.
    prim("ControlPad", 4, [](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) {
        if (v.on_rumble && st.size() > b + 2) v.on_rumble(double(st[b + 1].i) / 255.0, st[b + 2].i != 0);
        return st[b];
    });
    prim("ControlPad", 6, [](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) {
        v.rapid_count = v.rapid_count_ps2 = 0;
        return st[b];
    });
    // The image declares `ByteArray : Object`, but the save scripts need it to be an
    // ArrayedCollection: `StringInputStream#initialize` rejects anything else (so every
    // `Serializer.restore` of a card buffer would throw) and `Serializer.serialize` stores a nested
    // ByteArray (the `morning` blob) through that test. The PS2 engine must link it there itself.
    if (ClassObj* ba = vm.find_class("ByteArray"); ba && ba->parent == vm.find_class("Object"))
        if (ClassObj* ac = vm.find_class("ArrayedCollection")) ba->parent = ac;
    // The memory card is a folder of files (docs/technical/save.md). Arguments start at st[b + 1].
    // 0 isChanged: the "card" never changes under the game.
    // Voice.length(n) without a sound engine: no voice, so no text pacing to follow (frames).
    prim("Voice", 5, [](VM&, Thread&, std::vector<Value>&, size_t, Method&) { return Value::integer(0); });
    prim("MemoryCard", 0, [](VM&, Thread&, std::vector<Value>&, size_t, Method&) { return Value::boolean(false); });
    // 1 status: 2 (ready, ebKore+ save present) once system.bin exists, else 6 (usable, no save yet).
    prim("MemoryCard", 1, [](VM& v, Thread&, std::vector<Value>&, size_t, Method&) {
        // AMG_CARD_STATUS=N forces the answer (tests of the Vita's card error paths, tests/cases/Y5, Y6: 7 broken,
        // 3 or 5 no room, 1 no card); the real card never reports them here.
        if (const char* forced = std::getenv("AMG_CARD_STATUS")) return Value::integer(std::atoi(forced));
        std::error_code ec;
        bool any = std::filesystem::exists(save_dir(v) / "system.bin", ec) ||
                   std::filesystem::exists(save_dir(v) / "vita_system.bin", ec);  // the Vita's own (see below)
        return Value::integer(any ? 2 : 6);
    });
    // 2 createFileSystemPrim: create the folder.
    prim("MemoryCard", 2, [](VM& v, Thread&, std::vector<Value>&, size_t, Method&) {
        std::error_code ec;
        std::filesystem::create_directories(save_dir(v), ec);
        return Value::integer(std::filesystem::is_directory(save_dir(v), ec) ? 0 : SAVE_ERR_IO);
    });
    // 3 format: nothing to format.
    prim("MemoryCard", 3, [](VM&, Thread&, std::vector<Value>&, size_t, Method&) { return Value::integer(0); });
    // 4 readSystemData(buf)
    prim("MemoryCard", 4, [](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) {
        if (!is_byte_array(v, st[b + 1])) return Value::integer(SAVE_ERR_ARG);
        std::string payload;
        int rc = read_save(save_dir(v) / "system.bin", KIND_SYSTEM, payload);
        return Value::integer(rc ? rc : fill_buffer(st[b + 1], payload));
    });
    // 5 readShioriHeadPrim(buf): 20 headers of 256 bytes; a missing slot stays zero (unused).
    prim("MemoryCard", 5, [](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) {
        if (!is_byte_array(v, st[b + 1])) return Value::integer(SAVE_ERR_ARG);
        for (int s = 0; s < SAVE_SLOTS; ++s) {
            std::string payload;
            int rc = read_save(slot_file(v, s, "head"), KIND_HEAD, payload);
            if (rc == SAVE_ERR_MISSING) payload.clear();
            else if (rc) return Value::integer(rc);
            rc = fill_buffer(st[b + 1], payload, size_t(s) * SAVE_HEAD_SIZE, SAVE_HEAD_SIZE);
            if (rc) return Value::integer(rc);
        }
        return Value::integer(0);
    });
    // 6 writeAllData(slot, head, body, system)
    prim("MemoryCard", 6, [](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) {
        const Value& slot = st[b + 1];
        if (slot.tag != Tag::Int || slot.i < 0 || slot.i >= SAVE_SLOTS) return Value::integer(SAVE_ERR_ARG);
        for (int k = 2; k <= 4; ++k)
            if (!is_byte_array(v, st[b + k])) return Value::integer(SAVE_ERR_ARG);
        std::string head = bytes_of(st[b + 2]);
        if (head.size() > size_t(SAVE_HEAD_SIZE)) return Value::integer(SAVE_ERR_BAD);
        // The engine (0x10CAE0, read) takes the console's clock and writes month, day, hour and minute into the
        // serialized header, over the low bytes of ShioriData's four `update` integers (offsets 2, 7, 12 and 17),
        // which the script leaves at 0; the load screen shows them as MM/DD hh:mm. The port uses the PC's local time.
        if (head.size() >= 18) {
            std::time_t now = std::time(nullptr);
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            head[2] = char(local.tm_mon + 1);
            head[7] = char(local.tm_mday);
            head[12] = char(local.tm_hour);
            head[17] = char(local.tm_min);
        }
        int s = int(slot.i);
        // Body first and system last: a header only ever points at a body already on disk.
        bool ok = write_save(slot_file(v, s, "body"), KIND_BODY, bytes_of(st[b + 3])) &&
                  write_save(slot_file(v, s, "head"), KIND_HEAD, head) &&
                  write_save(save_dir(v) / "system.bin", KIND_SYSTEM, bytes_of(st[b + 4]));
        return Value::integer(ok ? 0 : SAVE_ERR_IO);
    });
    // 7 readShioriBody(slot, buf)
    prim("MemoryCard", 7, [](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) {
        const Value& slot = st[b + 1];
        if (slot.tag != Tag::Int || slot.i < 0 || slot.i >= SAVE_SLOTS || !is_byte_array(v, st[b + 2]))
            return Value::integer(SAVE_ERR_ARG);
        std::string payload;
        int rc = read_save(slot_file(v, int(slot.i), "body"), KIND_BODY, payload);
        return Value::integer(rc ? rc : fill_buffer(st[b + 2], payload));
    });
    // 8 writeSystemData(buf)
    prim("MemoryCard", 8, [](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) {
        if (!is_byte_array(v, st[b + 1])) return Value::integer(SAVE_ERR_ARG);
        bool ok = write_save(save_dir(v) / "system.bin", KIND_SYSTEM, bytes_of(st[b + 1]));
        return Value::integer(ok ? 0 : SAVE_ERR_IO);
    });
    // 9 hasOldData, 10 succeedOldData: no import of original Amagami saves yet.
    prim("MemoryCard", 9, [](VM&, Thread&, std::vector<Value>&, size_t, Method&) { return Value::boolean(false); });
    prim("MemoryCard", 10, [](VM&, Thread&, std::vector<Value>&, size_t, Method&) { return Value::integer(SAVE_ERR_MISSING); });
    // PS Vita system image (D-020): MemoryCard has its own primitive numbers and signatures (read from its
    // methods and their callers, docs/technical/vita-cpk-format.md). The trailing flag some take (a "quiet"
    // switch, hypothesis) is ignored. The Vita keeps its own files (vita_*), so a save made with it never
    // overwrites the PS2's in the same folder; when it has none yet, it reads the PS2's (see primitive 6).
    if (amg::is_vita(vm.root)) {
        auto vita_file = [](const VM& v, const std::string& name) { return save_dir(v) / ("vita_" + name); };
        // The system data: the Vita's file, else the PS2's (the same 49,152-byte layout).
        auto read_system = [vita_file](const VM& v, std::string& payload) {
            int rc = read_save(vita_file(v, "system.bin"), KIND_SYSTEM, payload);
            return rc == SAVE_ERR_MISSING ? read_save(save_dir(v) / "system.bin", KIND_SYSTEM, payload) : rc;
        };
        // 2 checkData(flag): the script then asks the card itself (checkData().isOK()).
        prim("MemoryCard", 2, [](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) { return st[b]; });
        // 3 createFileSystemPrim(system, flag) and 4 writeSystemData(buf, flag): the system data.
        PrimFn write_system = [vita_file](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) {
            if (!is_byte_array(v, st[b + 1])) return Value::integer(SAVE_ERR_ARG);
            std::error_code ec;
            std::filesystem::create_directories(save_dir(v), ec);
            bool ok = write_save(vita_file(v, "system.bin"), KIND_SYSTEM, bytes_of(st[b + 1]));
            return Value::integer(ok ? 0 : SAVE_ERR_IO);
        };
        prim("MemoryCard", 3, write_system);
        prim("MemoryCard", 4, write_system);
        // 5 readSystemData(buf, flag)
        prim("MemoryCard", 5, [read_system](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) {
            if (!is_byte_array(v, st[b + 1])) return Value::integer(SAVE_ERR_ARG);
            std::string payload;
            int rc = read_system(v, payload);
            return Value::integer(rc ? rc : fill_buffer(st[b + 1], payload));
        });
        // 6 readShioriHeadPrim(buf, flag): the Vita keeps every slot's header in one Serializer record,
        // [slot 0, ..., slot 19, checksums] (ShioriData.serialize); the PS2, one 256-byte record per slot. Without
        // a Vita file the PS2's are put together into that shape: each slot's record nested as an array ('A' and
        // the record, which ends in its own 0), a missing slot as nil ('N' and one byte, which the Vita
        // initialises), then the 20 checksums as zeros (only readShioriBody gets them, and it does not check them).
        prim("MemoryCard", 6, [vita_file](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) {
            if (!is_byte_array(v, st[b + 1])) return Value::integer(SAVE_ERR_ARG);
            std::string payload;
            int rc = read_save(vita_file(v, "heads.bin"), KIND_HEAD, payload);
            if (rc == SAVE_ERR_MISSING) {
                payload.clear();
                for (int s = 0; s < SAVE_SLOTS; ++s) {
                    std::string head;
                    size_t end = 0;
                    if (read_save(slot_file(v, s, "head"), KIND_HEAD, head) == 0 && serial_end(head, 0, end)) {
                        payload += 'A';
                        payload += head.substr(0, end);
                    } else {
                        payload += 'N';
                        payload += '\0';
                    }
                }
                payload += 'A';
                for (int s = 0; s < SAVE_SLOTS; ++s) {
                    payload += 'I';
                    payload += std::string(4, '\0');
                }
                payload += '\0';
                payload += '\0';
                rc = 0;
            }
            return Value::integer(rc ? rc : fill_buffer(st[b + 1], payload));
        });
        // 7 writeShioriData(heads, body, slot): every slot's header (ShioriData.serialize) and this slot's body.
        prim("MemoryCard", 7, [vita_file](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) {
            const Value& slot = st[b + 3];
            if (slot.tag != Tag::Int || slot.i < 0 || slot.i >= SAVE_SLOTS) return Value::integer(SAVE_ERR_ARG);
            if (!is_byte_array(v, st[b + 1]) || !is_byte_array(v, st[b + 2])) return Value::integer(SAVE_ERR_ARG);
            char body[32];
            std::snprintf(body, sizeof body, "slot%02d.body", int(slot.i));
            // Body first: the headers only ever point at a body already on disk.
            bool ok = write_save(vita_file(v, body), KIND_BODY, bytes_of(st[b + 2])) &&
                      write_save(vita_file(v, "heads.bin"), KIND_HEAD, bytes_of(st[b + 1]));
            return Value::integer(ok ? 0 : SAVE_ERR_IO);
        });
        // 8 readShioriBody(buf, slot, checksum): the Vita's file, else the PS2's (256 bytes shorter; the rest of
        // the buffer stays zero; hypothesis that the Vita reads it). The checksum is not checked.
        prim("MemoryCard", 8, [vita_file](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) {
            const Value& slot = st[b + 2];
            if (slot.tag != Tag::Int || slot.i < 0 || slot.i >= SAVE_SLOTS || !is_byte_array(v, st[b + 1]))
                return Value::integer(SAVE_ERR_ARG);
            char body[32];
            std::snprintf(body, sizeof body, "slot%02d.body", int(slot.i));
            std::string payload;
            int rc = read_save(vita_file(v, body), KIND_BODY, payload);
            if (rc == SAVE_ERR_MISSING) rc = read_save(slot_file(v, int(slot.i), "body"), KIND_BODY, payload);
            return Value::integer(rc ? rc : fill_buffer(st[b + 1], payload));
        });
        // 9 selectDeleteAllSaveData: the console's "delete broken data" dialog; nothing is deleted here.
        prim("MemoryCard", 9, [](VM&, Thread&, std::vector<Value>&, size_t, Method&) { return Value::integer(0); });
        // 10 displaySysMsg, 11 waitSysMsgEnd: the console's own message box; none, so it ends at once.
        prim("MemoryCard", 10, [](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) { return st[b]; });
        prim("MemoryCard", 11, [](VM&, Thread&, std::vector<Value>&, size_t, Method&) { return Value::boolean(false); });
    }
    prim("K2X_Script", 1, [](VM&, Thread&, std::vector<Value>&, size_t, Method&) { return Value::boolean(false); });
    prim("K2X_Script", 2, [](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        const Value& arg = st[b + 1];
        std::string name = arg.tag == Tag::Sym ? as_sym(arg)->name : as_str(arg)->b;
        ClassObj* c = v.find_class(name);
        if (!c) return Value();
        Value out;
        out.tag = Tag::Class;
        out.p = v.classes.at(name);
        return out;
    });
    prim("K2X_Script", 4, [](VM& v, Thread&, std::vector<Value>&, size_t, Method&) { return Value::integer(v.tick); });
    prim("K2X_Script", 3, [](VM&, Thread&, std::vector<Value>&, size_t, Method&) { return Value::boolean(true); });
    // 5 to 8 (getClock, setScrScl, setScrOrgY, unlockTrophy) do not exist on the PS2: the Vita system image's
    // K2X_Script has five class methods the PS2's doesn't (docs/technical/vita-cpk-format.md). Screen scale and
    // Y origin are the Vita's own display setup, handled by the port's own window instead; trophies have no PC
    // equivalent. Intentional no-ops, not stubs; setScreen (the sixth) calls into these but has no primitive of
    // its own to register.
    // 5 getClock: the save's time stamp, which the Vita's ShioriData.serialize puts in its `update` field and the
    // load list shows as MM/DD hh:mm: [month, day, hour, minute], the PS2 engine's order (it wrote them into the
    // serialized header itself). The PC's local time.
    prim("K2X_Script", 5, [](VM& v, Thread&, std::vector<Value>&, size_t, Method&) {
        std::time_t now = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        return v.make_arr(v.find_class("Array"), {Value::integer(local.tm_mon + 1), Value::integer(local.tm_mday),
                                                  Value::integer(local.tm_hour), Value::integer(local.tm_min)});
    });
    // 6 setScrScl(scale), 7 setScrOrgY(units): the Vita's view (gfx.hpp, VIEW_SCALE and VIEW_ORG_Y).
    prim("K2X_Script", 6, [](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) {
        const Value& a = st[b + 1];
        if (a.tag == Tag::Float) VIEW_SCALE = a.f;
        else if (a.tag == Tag::Int) VIEW_SCALE = double(a.i);
        if (std::getenv("AMG_TRACE_VIEW")) std::fprintf(stderr, "VIEW tick %lld setScrScl %g%c", (long long)v.tick, VIEW_SCALE, 10);
        return st[b];
    });
    prim("K2X_Script", 7, [](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) {
        const Value& a = st[b + 1];
        if (a.tag == Tag::Float) VIEW_ORG_Y = a.f;
        else if (a.tag == Tag::Int) VIEW_ORG_Y = double(a.i);
        if (std::getenv("AMG_TRACE_VIEW")) std::fprintf(stderr, "VIEW tick %lld setScrOrgY %g%c", (long long)v.tick, VIEW_ORG_Y, 10);
        return st[b];
    });
    prim("K2X_Script", 8, [](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) { return st[b]; });

    // PS Vita system image: primitive numbers that moved (found by matching method names between the two images,
    // docs/technical/vita-cpk-format.md) and the classes only the Vita has.
    if (amg::is_vita(vm.root)) {
        // ControlPad (class side): Vita 2 rapid, 3 resetRapid, 4 pressure, 5 analog, 6 actPausePrim; the PS2 has
        // them at 5, 6, 3, 2, 4.
        if (ClassObj* c = vm.find_class("ControlPad")) {
            std::map<int, PrimFn> ps2 = c->prims;
            const int from[][2] = {{2, 5}, {3, 6}, {4, 3}, {5, 2}, {6, 4}};
            for (const auto& m : from)
                if (ps2.count(m[1])) c->prims[m[0]] = ps2[m[1]];
        }
        // TouchPanel: the touch screen, played by the mouse (VM::touch, F5-8). Rectangles are (x, y, width,
        // height) in the scripts' units, origin at the centre of the screen and (x, y) the rectangle's CENTRE
        // (hypothesis: the gallery's thumbnail columns are -175, -125 ... 175, symmetric about 0, and its drag
        // asks for (0, 0, w, h)).
        // 0 checkTap(rect): a tap this frame inside the rect (consumed by the first rect that asks).
        // 3 checkDrag(rect): the id of the drag that began inside the rect, 0 if none; 4 checkDragEnd(id).
        // 5 getDragPressedPos, 6 getDragCurrentPos, 7 getDragDeltaVec: (id, axis), axis 0 horizontal and 1 vertical
        // (hypothesis: the gallery's script, which uses them, was read only as bytecode).
        // 1 checkTap2, 2 checkHold, 8 and 9 pinch, 14 and 15 rotation never start (no script asks for them); the
        // positions and amounts of the pinch and the rotation (10 to 13, 16 to 19) answer 0.
        auto in_rect = [](const std::vector<Value>& st, size_t b, double px, double py) {
            double x = num(st[b + 1]), y = num(st[b + 2]), w = num(st[b + 3]), h = num(st[b + 4]);
            return px >= x - w / 2 && px < x + w / 2 && py >= y - h / 2 && py < y + h / 2;
        };
        prim("TouchPanel", 0, [in_rect](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) {
            bool hit = v.touch.tap_tick >= v.tick - 1 && in_rect(st, b, v.touch.tap_x, v.touch.tap_y);
            if (hit) v.touch.tap_tick = -100;
            return Value::boolean(hit);
        });
        prim("TouchPanel", 3, [in_rect](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) {
            bool begun = v.touch.drag_active && in_rect(st, b, v.touch.press_x, v.touch.press_y);
            // A float: the English patch's `getScreenDrag` keeps the id in a variable it starts at 0.0 and tests with `==`,
            // which is false for an integer 0 (found 2026-10-22); the gallery's `> 0` tests work on a float too.
            return Value::real(begun ? double(v.touch.drag_id) : 0.0);
        });
        prim("TouchPanel", 4, [](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) {
            // True once the drag with this id has ended; false for an id that never was one (nil, 0): the English
            // patch's `getScreenDrag` calls it with a variable it has not set yet and goes on to subtract from nothing
            // if it answers true (found 2026-10-22).
            const Value& arg = st[b + 1];
            int64_t id = (arg.tag == Tag::Int || arg.tag == Tag::Float) ? int64_t(num(arg)) : 0;
            return Value::boolean(id != 0 && !(v.touch.drag_active && id == v.touch.drag_id));
        });
        for (int n : {5, 6, 7})
            prim("TouchPanel", n, [n](VM& v, Thread&, std::vector<Value>& st, size_t b, Method&) {
                bool vertical = num(st[b + 2]) != 0;
                double press = vertical ? v.touch.press_y : v.touch.press_x, now = vertical ? v.touch.y : v.touch.x;
                return Value::real(n == 5 ? press : n == 6 ? now : now - press);
            });
        for (int n : {1, 2, 8, 9, 14, 15})
            prim("TouchPanel", n, [](VM&, Thread&, std::vector<Value>&, size_t, Method&) { return Value::boolean(false); });
        for (int n : {10, 11, 12, 13, 16, 17, 18, 19})
            prim("TouchPanel", n, [](VM&, Thread&, std::vector<Value>&, size_t, Method&) { return Value::integer(0); });
    }
}

}  // namespace amg
