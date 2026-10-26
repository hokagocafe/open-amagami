// Where the game's data is read from.
//
// The port takes a folder (`--root`) in one of these shapes:
//   * the game's own folder as a dump has it (PS Vita: `AMAGAMI.IMG`, `SCENARIO.CPK`, `graph/GRAPH.CPK`, `sound/`,
//     `eboot.bin`), already decrypted: a copy from a console with the title's keys is not readable here;
//   * a Vita3K export root with `app/PCSG00291/` and an optional `patch/PCSG00291/` overlay;
//   * the prepared tree this project's tools made, with the same files under `extracted/` and a few derived ones
//     (`eboot_decrypted.elf`, converted sound).
// `data_file` finds a file in the direct, Vita3K, or prepared layout. A language layer (`AMG_LANG=en`,
// `amagami.exe --lang en`, docs/PLAN.md F5-6) is looked at first: `<root>/lang/en/extracted/<rel>` or
// `<root>/lang/en/<rel>`.
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
    if (std::filesystem::exists(raw, ec)) return raw;
    const std::string patched = root + "/patch/PCSG00291/" + rel;
    if (std::filesystem::exists(patched, ec)) return patched;
    const std::string installed = root + "/app/PCSG00291/" + rel;
    return std::filesystem::exists(installed, ec) ? installed : prepared;
}

inline bool is_vita(const std::string& root) {
    std::error_code ec;
    return std::filesystem::exists(data_file(root, "graph/GRAPH.CPK"), ec);
}

inline std::string vita_exe_file(const std::string& root) {
    std::error_code ec;
    // The port's static Vita tables match the installed app executable; a patch eboot can relocate or replace them.
    for (const std::string& path : {root + "/extracted/eboot_decrypted.elf", root + "/eboot.bin",
                                    root + "/app/PCSG00291/eboot.bin", root + "/patch/PCSG00291/eboot.bin"}) {
        ec.clear();
        if (std::filesystem::is_regular_file(path, ec) && std::filesystem::file_size(path, ec) > 0 && !ec) return path;
    }
    return "";
}

inline bool has_regular_file_with_extension(const std::string& dir, const std::string& extension) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return false;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == extension) return true;
    return false;
}

inline bool has_vita_voice_files(const std::string& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return false;
    for (std::filesystem::directory_iterator speakers(dir, ec), end; !ec && speakers != end; speakers.increment(ec)) {
        if (!speakers->is_directory(ec)) continue;
        for (std::filesystem::directory_iterator files(speakers->path(), ec), file_end; !ec && files != file_end;
             files.increment(ec))
            if (files->is_regular_file(ec) && files->path().extension() == ".bin") return true;
        if (ec) return false;
    }
    return false;
}

inline std::string vita_voice_directory(const std::string& root) {
    for (const std::string& dir : {root + "/extracted/sound/voice", root + "/sound/voice",
                                    root + "/patch/PCSG00291/sound/voice", root + "/app/PCSG00291/sound/voice"})
        if (has_vita_voice_files(dir)) return dir;
    return root + "/extracted/sound/voice";
}

inline bool has_vita_wav_files(const std::string& root, const std::string& subdir) {
    return has_regular_file_with_extension(root + "/extracted/sound_wav/" + subdir, ".wav");
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

inline size_t vita_va_to_off(const std::string& elf, uint32_t va, const char* source) {
    auto u16 = [&](size_t at) {
        if (at + 2 > elf.size()) throw std::runtime_error("eboot.bin: truncated ELF header");
        return uint16_t(uint8_t(elf[at]) | (uint16_t(uint8_t(elf[at + 1])) << 8));
    };
    auto u32 = [&](size_t at) {
        if (at + 4 > elf.size()) throw std::runtime_error("eboot.bin: truncated ELF header");
        return uint32_t(uint8_t(elf[at])) | (uint32_t(uint8_t(elf[at + 1])) << 8) |
               (uint32_t(uint8_t(elf[at + 2])) << 16) | (uint32_t(uint8_t(elf[at + 3])) << 24);
    };
    if (elf.size() < 52 || std::memcmp(elf.data(), "\x7f" "ELF", 4) != 0 || uint8_t(elf[4]) != 1 ||
        uint8_t(elf[5]) != 1)
        throw std::runtime_error("eboot.bin: expected a 32-bit little-endian ELF");
    const uint32_t phoff = u32(28);
    const uint16_t phentsize = u16(42), phnum = u16(44);
    if (phentsize < 32 || uint64_t(phoff) + uint64_t(phentsize) * phnum > elf.size())
        throw std::runtime_error("eboot.bin: invalid ELF program headers");
    for (uint16_t i = 0; i < phnum; ++i) {
        const size_t at = size_t(phoff) + size_t(phentsize) * i;
        if (u32(at) != 1) continue;
        const uint32_t offset = u32(at + 4), vaddr = u32(at + 8), filesz = u32(at + 16);
        if (va >= vaddr && uint64_t(va) < uint64_t(vaddr) + filesz) {
            const uint64_t file_offset = uint64_t(offset) + (va - vaddr);
            if (file_offset >= elf.size()) throw std::runtime_error("eboot.bin: address is outside the ELF file");
            return size_t(file_offset);
        }
    }
    throw std::runtime_error(std::string("vita: ") + source + " address " + std::to_string(va) +
                             " is not in a known eboot.bin segment");
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
