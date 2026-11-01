// A small DEFLATE decoder (RFC 1951) for the one place the port meets compressed data it has no library for: the segments
// of a Vita executable that a decryption tool left compressed. Bit by bit and table free, so it is slow, but those
// segments are a few megabytes read once.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace amg {

namespace inflate_detail {

struct Bits {
    const uint8_t* data;
    size_t size, at = 0;
    uint32_t hold = 0;
    int count = 0;
    int bit() {
        if (count == 0) {
            if (at >= size) throw std::runtime_error("compressed data ends early");
            hold = data[at++];
            count = 8;
        }
        int b = int(hold & 1);
        hold >>= 1;
        --count;
        return b;
    }
    uint32_t bits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v |= uint32_t(bit()) << i;
        return v;
    }
};

// A canonical Huffman code as the counts per length and the symbols in order (the way zlib's "puff" decodes it).
struct Huffman {
    uint16_t count[16] = {};
    uint16_t symbol[288] = {};
    void build(const uint8_t* lengths, int n) {
        for (int i = 0; i < 16; ++i) count[i] = 0;
        for (int i = 0; i < n; ++i) ++count[lengths[i]];
        uint16_t offsets[16] = {};
        for (int len = 1; len < 15; ++len) offsets[len + 1] = uint16_t(offsets[len] + count[len]);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbol[offsets[lengths[i]]++] = uint16_t(i);
    }
    int decode(Bits& in) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= in.bit();
            int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        throw std::runtime_error("bad compressed data");
    }
};

}  // namespace inflate_detail

// Decodes a raw DEFLATE stream; `expected` is the output size when known (0 otherwise).
inline std::string inflate_raw(const uint8_t* data, size_t size, size_t expected = 0) {
    using namespace inflate_detail;
    static const uint16_t LEN_BASE[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t LEN_EXTRA[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t DIST_BASE[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t DIST_EXTRA[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    Bits in{data, size};
    std::string out;
    out.reserve(expected);
    for (bool last = false; !last;) {
        last = in.bit() != 0;
        const uint32_t type = in.bits(2);
        if (type == 0) {
            in.count = 0;
            if (in.at + 4 > size) throw std::runtime_error("compressed data ends early");
            const size_t len = data[in.at] | (size_t(data[in.at + 1]) << 8);
            in.at += 4;
            if (in.at + len > size) throw std::runtime_error("compressed data ends early");
            out.append(reinterpret_cast<const char*>(data + in.at), len);
            in.at += len;
            continue;
        }
        if (type == 3) throw std::runtime_error("bad compressed data");
        Huffman lit, dist;
        uint8_t lengths[320] = {};
        if (type == 1) {
            for (int i = 0; i < 144; ++i) lengths[i] = 8;
            for (int i = 144; i < 256; ++i) lengths[i] = 9;
            for (int i = 256; i < 280; ++i) lengths[i] = 7;
            for (int i = 280; i < 288; ++i) lengths[i] = 8;
            lit.build(lengths, 288);
            uint8_t fixed[30];
            for (uint8_t& l : fixed) l = 5;
            dist.build(fixed, 30);
        } else {
            static const uint8_t ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            const int nlen = int(in.bits(5)) + 257, ndist = int(in.bits(5)) + 1, ncode = int(in.bits(4)) + 4;
            if (nlen > 286 || ndist > 30) throw std::runtime_error("bad compressed data");
            uint8_t code_lengths[19] = {};
            for (int i = 0; i < ncode; ++i) code_lengths[ORDER[i]] = uint8_t(in.bits(3));
            Huffman codes;
            codes.build(code_lengths, 19);
            int index = 0;
            while (index < nlen + ndist) {
                int sym = codes.decode(in);
                if (sym < 16) lengths[index++] = uint8_t(sym);
                else {
                    int repeat, value = 0;
                    if (sym == 16) {
                        if (index == 0) throw std::runtime_error("bad compressed data");
                        value = lengths[index - 1];
                        repeat = 3 + int(in.bits(2));
                    } else if (sym == 17) repeat = 3 + int(in.bits(3));
                    else repeat = 11 + int(in.bits(7));
                    if (index + repeat > nlen + ndist) throw std::runtime_error("bad compressed data");
                    while (repeat--) lengths[index++] = uint8_t(value);
                }
            }
            lit.build(lengths, nlen);
            dist.build(lengths + nlen, ndist);
        }
        for (;;) {
            const int sym = lit.decode(in);
            if (sym < 256) out += char(sym);
            else if (sym == 256) break;
            else {
                const int s = sym - 257;
                if (s >= 29) throw std::runtime_error("bad compressed data");
                const size_t len = LEN_BASE[s] + in.bits(LEN_EXTRA[s]);
                const int d = dist.decode(in);
                if (d >= 30) throw std::runtime_error("bad compressed data");
                const size_t back = DIST_BASE[d] + in.bits(DIST_EXTRA[d]);
                if (back > out.size()) throw std::runtime_error("bad compressed data");
                for (size_t i = 0; i < len; ++i) out += out[out.size() - back];
            }
        }
    }
    return out;
}

// A zlib stream (RFC 1950): two header bytes, the DEFLATE data, a checksum that is not checked.
inline std::string inflate_zlib(const uint8_t* data, size_t size, size_t expected = 0) {
    if (size < 2) throw std::runtime_error("compressed data ends early");
    return inflate_raw(data + 2, size - 2, expected);
}

}  // namespace amg
