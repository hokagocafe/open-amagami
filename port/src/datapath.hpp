// Where the game's data is read from.
//
// The port takes a folder (`--root`) in one of two shapes:
//   * the game's own folder as a dump has it (PS Vita: `AMAGAMI.IMG`, `SCENARIO.CPK`, `graph/GRAPH.CPK`, `sound/`,
//     `eboot.bin`), already decrypted: a copy from a console with the title's keys is not readable here;
//   * the prepared tree this project's tools made, with the same files under `extracted/` and a few derived ones
//     (`eboot_decrypted.elf`, converted sound).
// `data_file` finds a file in either. A language layer (`AMG_LANG=en`, `amagami.exe --lang en`, docs/PLAN.md F5-6)
// is looked at first: `<root>/lang/en/extracted/<rel>` or `<root>/lang/en/<rel>`.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace amg {

// `rel` is the path inside the game's folder, e.g. "AMAGAMI.IMG" or "graph/GRAPH.CPK".
inline std::string data_file(const std::string& root, const std::string& rel) {
    std::error_code ec;
    if (const char* lang = std::getenv("AMG_LANG"); lang && *lang) {
        for (const char* sub : {"/extracted/", "/"}) {
            std::string layered = root + "/lang/" + lang + sub + rel;
            if (std::filesystem::exists(layered, ec)) return layered;
        }
    }
    std::string prepared = root + "/extracted/" + rel;
    if (std::filesystem::exists(prepared, ec)) return prepared;
    std::string raw = root + "/" + rel;
    return std::filesystem::exists(raw, ec) ? raw : prepared;
}

inline bool is_vita(const std::string& root) {
    std::error_code ec;
    return std::filesystem::exists(data_file(root, "graph/GRAPH.CPK"), ec);
}

inline std::string vita_exe_file(const std::string& root) {
    std::error_code ec;
    const std::string prepared = root + "/extracted/eboot_decrypted.elf";
    if (std::filesystem::is_regular_file(prepared, ec) && std::filesystem::file_size(prepared, ec) > 0 && !ec)
        return prepared;
    ec.clear();
    const std::string raw = root + "/eboot.bin";
    if (std::filesystem::is_regular_file(raw, ec) && std::filesystem::file_size(raw, ec) > 0 && !ec) return raw;
    return "";
}

// The Vita executable as a plain ELF image: the prepared `eboot_decrypted.elf` if there is one, else `eboot.bin`
// unwrapped in memory. A retail eboot has encrypted segments (flag 1) and cannot be read; a copy whose segments are
// plain (flag 2, as the owner's decrypted package has) is the ELF's own bytes in Sony's SELF container, so the
// unwrap is a copy: the ELF header and program headers, then each segment to its file offset (checked byte for byte
// against the ELF the project's tools made, 2026-10-22).
inline std::string vita_exe(const std::string& root) {
    auto slurp = [](const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    const std::string path = vita_exe_file(root);
    if (path.empty()) throw std::runtime_error("no eboot.bin (or extracted/eboot_decrypted.elf) in " + root);
    std::string elf = slurp(path);
    if (path == root + "/extracted/eboot_decrypted.elf") return elf;
    std::string self = std::move(elf);
    if (self.size() < 0x80 || std::memcmp(self.data(), "SCE\0", 4) != 0)
        throw std::runtime_error("eboot.bin has an unexpected header");
    auto u64 = [&](size_t at) {
        uint64_t v;
        std::memcpy(&v, self.data() + at, 8);
        return v;
    };
    uint64_t elf_size = u64(0x18), elf_off = u64(0x40), phdr_off = u64(0x48), seg_off = u64(0x58);
    if (elf_size > self.size() * 2 || elf_off + 0x34 > self.size() || seg_off > self.size())
        throw std::runtime_error("eboot.bin: unexpected layout");
    elf.assign(size_t(elf_size), '\0');
    std::memcpy(elf.data(), self.data() + elf_off, 0x34);
    uint16_t phnum;
    uint32_t phoff;
    std::memcpy(&phnum, elf.data() + 0x2C, 2);
    std::memcpy(&phoff, elf.data() + 0x1C, 4);
    if (phoff + 32u * phnum > elf.size() || phdr_off + 32u * phnum > self.size())
        throw std::runtime_error("eboot.bin: unexpected program headers");
    std::memcpy(elf.data() + phoff, self.data() + phdr_off, 32u * phnum);
    for (unsigned i = 0; i < phnum; ++i) {
        uint32_t p_offset;
        std::memcpy(&p_offset, self.data() + phdr_off + 32 * i + 4, 4);
        uint64_t so = u64(size_t(seg_off + 0x20 * i)), len = u64(size_t(seg_off + 0x20 * i + 8)),
                 compression = u64(size_t(seg_off + 0x20 * i + 16)), encryption = u64(size_t(seg_off + 0x20 * i + 24));
        if (encryption != 2 || compression != 1)
            throw std::runtime_error(
                "eboot.bin is encrypted or compressed: use a copy whose executable has been decrypted (the port does not decrypt)");
        if (so + len > self.size() || p_offset + len > elf.size()) throw std::runtime_error("eboot.bin: segment out of range");
        std::memcpy(elf.data() + p_offset, self.data() + so, size_t(len));
    }
    return elf;
}

// Whether `root` is a PS Vita copy the port can run, as a message for the player ("" when it is). Checked: the three
// data files exist and are CPK archives (a copy that still has the console's per-title file encryption starts with
// noise instead), and the executable can be unwrapped (a retail eboot has encrypted segments). The sound files are not
// required: without them the game plays silent.
inline std::string check_game_folder(const std::string& root) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return "That folder does not exist.";
    std::string missing;
    for (const char* rel : {"AMAGAMI.IMG", "SCENARIO.CPK", "graph/GRAPH.CPK"})
        if (!std::filesystem::exists(data_file(root, rel), ec)) missing += std::string("\n  ") + rel;
    if (!missing.empty())
        return "This does not look like the game's folder for the PS Vita version (PCSG00291). Missing:" + missing +
               "\n\nChoose the folder that holds AMAGAMI.IMG, SCENARIO.CPK, graph and sound.";
    for (const char* rel : {"SCENARIO.CPK", "graph/GRAPH.CPK"}) {
        std::ifstream f(data_file(root, rel), std::ios::binary);
        char magic[4] = {};
        f.read(magic, 4);
        if (std::memcmp(magic, "CPK ", 4) != 0)
            return std::string(rel) +
                   " is not readable: it is still encrypted by the console's file system. This program does not decrypt "
                   "games; use a copy whose files have been decrypted (a dump made with the usual homebrew tools).";
    }
    try {
        vita_exe(root);
    } catch (const std::exception& e) {
        return std::string(e.what()) +
               "\n\nThe game's executable (eboot.bin) must be a decrypted copy. This program does not decrypt it.";
    }
    return "";
}

}  // namespace amg
