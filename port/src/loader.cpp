// Reading the disc's script data: the LZSS system image, ARC archives and SCF classes.
// Formats: docs/technical/scf-format.md and docs/technical/arc-format.md.
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "vm.hpp"

namespace amg {

namespace {

struct Reader {
    const std::string& d;
    size_t pos;
    Reader(const std::string& data, size_t at) : d(data), pos(at) {}
    uint8_t u8() {
        if (pos + 1 > d.size()) throw std::runtime_error("read past the end");
        return uint8_t(d[pos++]);
    }
    uint16_t u16() {
        uint16_t lo = u8();
        uint16_t hi = u8();
        return uint16_t(lo | (hi << 8));
    }
    std::string raw(size_t n) {
        if (pos + n > d.size()) throw std::runtime_error("read past the end");
        std::string s = d.substr(pos, n);
        pos += n;
        return s;
    }
    std::string str() { return raw(u16()); }
};

uint32_t le32(const std::string& d, size_t at) {
    if (at + 4 > d.size()) throw std::runtime_error("read past the end");
    return uint32_t(uint8_t(d[at])) | (uint32_t(uint8_t(d[at + 1])) << 8) | (uint32_t(uint8_t(d[at + 2])) << 16) |
           (uint32_t(uint8_t(d[at + 3])) << 24);
}

ClassSource::Constant read_constant(Reader& r) {
    ClassSource::Constant c;
    c.tag = r.u8();
    switch (c.tag) {
        case 0: case 2: case 4:
            break;
        case 1: {
            std::string raw = r.raw(4);
            int32_t v;
            std::memcpy(&v, raw.data(), 4);
            c.i = v;
            break;
        }
        case 3: {
            std::string raw = r.raw(4);
            std::memcpy(&c.f, raw.data(), 4);
            break;
        }
        case 5: case 6: case 7:
            c.s = r.str();
            break;
        case 8: {
            int n = r.u16();
            for (int i = 0; i < n; ++i) c.items.push_back(read_constant(r));
            break;
        }
        default:
            throw std::runtime_error("unknown constant tag");
    }
    return c;
}

}  // namespace

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A little-endian u32 with the decompressed size, then LZSS with a 2048-byte window and matches
// of 3 to 34 bytes (Okumura's LZSS.C).
std::string lzss_unpack(const std::string& data) {
    const int N = 2048, F = 34, THRESHOLD = 2;
    size_t size = le32(data, 0);
    std::string out;
    out.reserve(size);
    std::vector<uint8_t> window(N, 0);
    int r = N - F;
    size_t i = 4;
    unsigned flags = 0;
    while (out.size() < size) {
        flags >>= 1;
        if (!(flags & 0x100)) {
            flags = uint8_t(data.at(i++)) | 0xFF00u;
        }
        if (flags & 1) {
            uint8_t c = uint8_t(data.at(i++));
            out.push_back(char(c));
            window[r] = c;
            r = (r + 1) & (N - 1);
        } else {
            int b0 = uint8_t(data.at(i)), b1 = uint8_t(data.at(i + 1));
            i += 2;
            int pos = b0 | ((b1 & 0xE0) << 3);
            int length = (b1 & 0x1F) + THRESHOLD + 1;
            for (int k = 0; k < length && out.size() < size; ++k) {
                uint8_t c = window[(pos + k) & (N - 1)];
                out.push_back(char(c));
                window[r] = c;
                r = (r + 1) & (N - 1);
            }
        }
    }
    return out;
}

bool parse_scf(const std::string& data, size_t& pos, ClassSource& cls) {
    Reader r(data, pos);
    if (r.raw(4) != std::string("SCF\x1a", 4)) return false;
    cls.flags = r.u8();
    int version = r.u8();
    if (version != 4) return false;
    cls.name = r.str();
    cls.parent = r.str();
    for (auto* vars : {&cls.instance_vars, &cls.class_vars}) {
        int n = r.u16();
        for (int i = 0; i < n; ++i) {
            int slot = r.u8();
            vars->emplace_back(slot, r.str());
        }
    }
    for (auto* list : {&cls.methods, &cls.class_methods}) {
        int n = r.u16();
        for (int i = 0; i < n; ++i) {
            ClassSource::MethodSource m;
            m.name = r.str();
            m.args = r.u16();
            m.code = r.raw(r.u16());
            list->push_back(std::move(m));
        }
    }
    int n = r.u16();
    for (int i = 0; i < n; ++i) cls.constants.push_back(read_constant(r));
    pos = r.pos;
    return true;
}

std::vector<std::shared_ptr<ClassSource>> read_image(const std::string& image) {
    size_t size = le32(image, 0), count = le32(image, 4);
    if (size != image.size()) throw std::runtime_error("image size field does not match");
    std::vector<std::shared_ptr<ClassSource>> out;
    for (size_t i = 0; i < count; ++i) {
        size_t offset = le32(image, 8 + 4 * count + 4 * i);
        auto cls = std::make_shared<ClassSource>();
        if (!parse_scf(image, offset, *cls)) throw std::runtime_error("bad class in the image");
        out.push_back(cls);
    }
    return out;
}

}  // namespace amg
