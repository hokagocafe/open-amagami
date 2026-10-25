// A C++ rendition of tools/cpk.py (TOC layout only; see cpk.hpp).
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "cpk.hpp"

namespace amg {

namespace {

std::string unmask(const std::string& data) {
    if (data.size() >= 4 && std::memcmp(data.data(), "@UTF", 4) == 0) return data;
    std::string out = data;
    uint32_t m = 0x655F, t = 0x4115;
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = char(uint8_t(out[i]) ^ (m & 0xFF));
        m = (m * t) & 0xFFFFFFFFu;
    }
    return out;
}

uint32_t be32(const std::string& d, size_t at) {
    return (uint32_t(uint8_t(d[at])) << 24) | (uint32_t(uint8_t(d[at + 1])) << 16) |
           (uint32_t(uint8_t(d[at + 2])) << 8) | uint32_t(uint8_t(d[at + 3]));
}
uint16_t be16(const std::string& d, size_t at) { return uint16_t((uint8_t(d[at]) << 8) | uint8_t(d[at + 1])); }
uint64_t be64(const std::string& d, size_t at) { return (uint64_t(be32(d, at)) << 32) | uint64_t(be32(d, at + 4)); }

std::string cstr(const std::string& d, size_t at) {
    size_t end = d.find('\0', at);
    return d.substr(at, end - at);
}

// A column's flag byte is a bitfield over its low-nibble type: a name (0x10), a default value inline in
// the schema (0x20), a value per row (0x40). A column with a default stores nothing per row even with
// 0x40 also set (tools/cpk.py, docs/technical/vita-cpk-format.md).
enum ColumnFlag : uint8_t { FLAG_NAME = 0x10, FLAG_DEFAULT = 0x20, FLAG_ROW = 0x40 };

// Reads one scalar/string value; floats and "data" (0x08, 0x09, 0x0B) are skipped over, unused here.
UtfValue read_value(const std::string& table, size_t& pos, uint8_t typ, size_t strings_offset, size_t data_offset) {
    UtfValue v;
    v.present = true;
    switch (typ) {
        case 0x0A: {  // string: u32 offset into the strings area
            uint32_t off = be32(table, pos);
            pos += 4;
            v.is_string = true;
            v.s = cstr(table, strings_offset + off);
            return v;
        }
        case 0x00: case 0x01: v.i = uint8_t(table[pos]); pos += 1; return v;
        case 0x02: case 0x03: v.i = be16(table, pos); pos += 2; return v;
        case 0x04: case 0x05: v.i = int64_t(be32(table, pos)); pos += 4; return v;
        case 0x06: case 0x07: v.i = int64_t(be64(table, pos)); pos += 8; return v;
        case 0x08: pos += 4; v.present = false; return v;
        case 0x09: pos += 8; v.present = false; return v;
        case 0x0B: {  // data: u32 offset into the data area, u32 size
            uint32_t off = be32(table, pos), len = be32(table, pos + 4);
            pos += 8;
            v.is_blob = true;
            v.s = table.substr(data_offset + off, len);
            return v;
        }
        default:
            throw std::runtime_error("cpk: unsupported UTF column type " + std::to_string(typ));
    }
}

struct Column {
    std::string name;
    uint8_t type = 0;
    bool per_row = false;
    UtfValue const_value;
};

UtfTable parse_utf(const std::string& raw) {
    std::string data = unmask(raw);
    if (data.size() < 8 || std::memcmp(data.data(), "@UTF", 4) != 0) throw std::runtime_error("cpk: not a UTF table");
    uint32_t table_size = be32(data, 4);
    std::string table = data.substr(8, table_size);

    uint32_t rows_offset = be32(table, 0), strings_offset = be32(table, 4), data_offset = be32(table, 8);
    uint16_t num_columns = be16(table, 16), row_length = be16(table, 18);
    uint32_t num_rows = be32(table, 20);

    size_t pos = 24;
    std::vector<Column> columns;
    for (int c = 0; c < num_columns; ++c) {
        uint8_t flag = uint8_t(table[pos]);
        pos += 1;
        Column col;
        col.type = flag & 0x0F;
        if (flag & FLAG_NAME) {
            uint32_t name_off = be32(table, pos);
            pos += 4;
            col.name = cstr(table, strings_offset + name_off);
        }
        if (flag & FLAG_DEFAULT) col.const_value = read_value(table, pos, col.type, strings_offset, data_offset);
        col.per_row = (flag & FLAG_ROW) && !(flag & FLAG_DEFAULT);
        columns.push_back(std::move(col));
    }
    if (pos != rows_offset) throw std::runtime_error("cpk: UTF schema does not end where the rows start");

    UtfTable out;
    for (uint32_t r = 0; r < num_rows; ++r) {
        pos = rows_offset + size_t(r) * row_length;
        std::unordered_map<std::string, UtfValue> row;
        for (const Column& col : columns) {
            UtfValue v;
            v = col.per_row ? read_value(table, pos, col.type, strings_offset, data_offset) : col.const_value;
            if (!col.name.empty()) row[col.name] = v;
        }
        out.rows.push_back(std::move(row));
    }
    return out;
}

int64_t get_int(const std::unordered_map<std::string, UtfValue>& row, const char* key, int64_t fallback = 0) {
    auto it = row.find(key);
    if (it == row.end() || !it->second.present) return fallback;
    return it->second.i;
}
std::string get_str(const std::unordered_map<std::string, UtfValue>& row, const char* key) {
    auto it = row.find(key);
    if (it == row.end() || !it->second.present) return "";
    return it->second.s;
}

// A length-prefixed @UTF packet: 4-byte magic (ignored), 4 bytes padding, u64 LE size, then the
// masked table (tools/cpk.py's read_packet).
UtfTable read_packet(std::ifstream& f, uint64_t offset) {
    f.seekg(std::streamoff(offset));
    char header[16];
    f.read(header, 16);
    uint64_t size;
    std::memcpy(&size, header + 8, 8);  // little-endian on all platforms this project targets
    std::string body(size_t(size), '\0');
    f.read(body.data(), std::streamsize(size));
    return parse_utf(body);
}

}  // namespace

UtfTable cpk_parse_utf(const std::string& bytes) { return parse_utf(bytes); }

CpkIndex cpk_read_index(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    char magic[4];
    f.read(magic, 4);
    if (std::memcmp(magic, "CPK ", 4) != 0) throw std::runtime_error(path + ": not a CPK archive");

    UtfTable header_table = read_packet(f, 0);
    const auto& header = header_table.rows.at(0);
    int64_t toc_offset = get_int(header, "TocOffset", -1);
    if (toc_offset < 0) throw std::runtime_error(path + ": no TocOffset (ITOC-mode CPK, not supported here)");
    int64_t content_offset = get_int(header, "ContentOffset", toc_offset);
    int64_t base_offset = std::min(toc_offset, content_offset);

    UtfTable toc = read_packet(f, uint64_t(toc_offset));
    CpkIndex index;
    for (const auto& row : toc.rows) {
        std::string dir = get_str(row, "DirName");
        std::string name = get_str(row, "FileName");
        std::string key = dir.empty() ? name : dir + "/" + name;
        CpkEntry e;
        e.offset = uint64_t(base_offset + get_int(row, "FileOffset"));
        e.size = uint64_t(get_int(row, "FileSize"));
        int64_t extract = get_int(row, "ExtractSize", -1);
        e.extract_size = extract >= 0 ? uint64_t(extract) : e.size;
        index[key] = e;
    }
    return index;
}

namespace {
const int VLE_LENS[4] = {2, 3, 5, 8};
}

std::vector<CpkEntry> cpk_read_entries(const std::string& path, uint64_t start) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    char magic[4];
    f.seekg(std::streamoff(start));
    f.read(magic, 4);
    if (std::memcmp(magic, "CPK ", 4) != 0) throw std::runtime_error(path + ": not a CPK archive");
    UtfTable header_table = read_packet(f, start);
    const auto& header = header_table.rows.at(0);
    std::vector<CpkEntry> out;
    int64_t toc_offset = get_int(header, "TocOffset", -1);
    if (toc_offset >= 0) {  // named entries, in the table's order
        int64_t content_offset = get_int(header, "ContentOffset", toc_offset);
        int64_t base_offset = std::min(toc_offset, content_offset);
        UtfTable toc = read_packet(f, start + uint64_t(toc_offset));
        for (const auto& row : toc.rows) {
            CpkEntry e;
            e.offset = start + uint64_t(base_offset + get_int(row, "FileOffset"));
            e.size = uint64_t(get_int(row, "FileSize"));
            int64_t extract = get_int(row, "ExtractSize", -1);
            e.extract_size = extract >= 0 ? uint64_t(extract) : e.size;
            out.push_back(e);
        }
        return out;
    }
    // The other layout (ItocOffset: MUSIC, SOUND, VOICE and the per-speaker archives): no names, ids in order, files
    // stored back to back from ContentOffset, each padded up to Align (tools/cpk.py, read_itoc_entries).
    int64_t itoc_offset = get_int(header, "ItocOffset", -1);
    if (itoc_offset < 0) throw std::runtime_error(path + ": neither TocOffset nor ItocOffset");
    UtfTable itoc = read_packet(f, start + uint64_t(itoc_offset));
    std::map<int64_t, std::pair<int64_t, int64_t>> by_id;  // id -> size, extract size
    for (const char* column : {"DataL", "DataH"}) {
        auto it = itoc.rows.at(0).find(column);
        if (it == itoc.rows.at(0).end() || !it->second.is_blob || it->second.s.empty()) continue;
        UtfTable rows = parse_utf(it->second.s);
        for (const auto& row : rows.rows) {
            int64_t size = get_int(row, "FileSize");
            int64_t extract = get_int(row, "ExtractSize", -1);
            by_id[get_int(row, "ID")] = {size, extract >= 0 ? extract : size};
        }
    }
    int64_t align = get_int(header, "Align", 2048);
    if (align <= 0) align = 2048;
    uint64_t offset = start + uint64_t(get_int(header, "ContentOffset"));
    for (const auto& [id, sizes] : by_id) {
        CpkEntry e;
        e.offset = offset;
        e.size = uint64_t(sizes.first);
        e.extract_size = uint64_t(sizes.second);
        out.push_back(e);
        offset += (e.size + uint64_t(align) - 1) / uint64_t(align) * uint64_t(align);
    }
    return out;
}

std::string cpk_decompress_crilayla(const std::string& data) {
    if (data.size() < 8 || std::memcmp(data.data(), "CRILAYLA", 8) != 0) return data;
    uint32_t uncompressed_size, header_offset;
    std::memcpy(&uncompressed_size, data.data() + 8, 4);
    std::memcpy(&header_offset, data.data() + 12, 4);

    std::string result(size_t(0x100) + uncompressed_size, '\0');
    std::memcpy(result.data(), data.data() + 16 + header_offset, 0x100);
    const uint8_t* compressed = reinterpret_cast<const uint8_t*>(data.data()) + 16;

    int64_t input_offset = int64_t(header_offset) - 1;
    uint32_t bit_pool = 0;
    int bits_left = 0;
    uint32_t output_end = 0x100 + uncompressed_size - 1;
    uint32_t bytes_output = 0;

    auto get_bits = [&](int n) -> uint32_t {
        uint32_t value = 0;
        int produced = 0;
        while (produced < n) {
            if (bits_left == 0) {
                bit_pool = compressed[input_offset];
                input_offset -= 1;
                bits_left = 8;
            }
            int take = std::min(bits_left, n - produced);
            value = (value << take) | ((bit_pool >> (bits_left - take)) & ((1u << take) - 1));
            bits_left -= take;
            produced += take;
        }
        return value;
    };

    while (bytes_output < uncompressed_size) {
        if (get_bits(1)) {
            uint32_t back_offset = output_end - bytes_output + get_bits(13) + 3;
            uint32_t length = 3;
            int level = 0;
            while (true) {
                int bits = VLE_LENS[std::min(level, 3)];
                uint32_t chunk = get_bits(bits);
                length += chunk;
                if (chunk != (1u << bits) - 1) break;
                level += 1;
            }
            for (uint32_t i = 0; i < length; ++i) {
                result[output_end - bytes_output] = result[back_offset];
                back_offset -= 1;
                bytes_output += 1;
            }
        } else {
            result[output_end - bytes_output] = char(get_bits(8));
            bytes_output += 1;
        }
    }
    return result;
}

bool cpk_read(const std::string& cpk_path, const CpkIndex& index, const std::string& key, std::string& out) {
    auto it = index.find(key);
    if (it == index.end()) return false;
    // One open file per archive, kept: opening costs tens of milliseconds over a network or WSL path, once per
    // picture otherwise. Reads come from the preload threads too, hence the lock (decompression runs outside it).
    static std::mutex lock;
    static std::map<std::string, std::unique_ptr<std::ifstream>> files;
    std::string raw(size_t(it->second.size), '\0');
    {
        std::lock_guard<std::mutex> guard(lock);
        auto& f = files[cpk_path];
        if (!f || !*f) f = std::make_unique<std::ifstream>(cpk_path, std::ios::binary);
        if (!*f) throw std::runtime_error("cannot open " + cpk_path);
        f->clear();
        f->seekg(std::streamoff(it->second.offset));
        f->read(raw.data(), std::streamsize(raw.size()));
    }
    out = raw.size() >= 8 && std::memcmp(raw.data(), "CRILAYLA", 8) == 0 ? cpk_decompress_crilayla(raw) : raw;
    return true;
}

}  // namespace amg
