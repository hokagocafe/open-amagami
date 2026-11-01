#include "data_check.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "cpk.hpp"
#include "datapath.hpp"
#include "paths.hpp"
#include "ui_text.hpp"
#include "vm.hpp"

namespace amg {

namespace {

// SHA-256 of whatever `feed` hands over in pieces (it returns the bytes it wrote, 0 at the end), as lower-case hex.
template <class Feed>
std::string sha256_of(Feed&& feed) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buffer(1 << 20);
        for (size_t got; (got = feed(buffer.data(), buffer.size())) > 0;)
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), ULONG(got), 0);
        unsigned char digest[32];
        if (BCryptFinishHash(hash, digest, sizeof digest, 0) == 0) {
            char two[3];
            for (unsigned char byte : digest) {
                std::snprintf(two, sizeof two, "%02x", byte);
                hex += two;
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

// SHA-256 of a file ("" if it cannot be read).
std::string sha256_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    return sha256_of([&](char* into, size_t room) {
        f.read(into, std::streamsize(room));
        return size_t(f.gcount());
    });
}

// SHA-256 of the executable as the plain ELF the port reads: the same bytes whether the copy holds it as the SELF
// `eboot.bin` of a decrypted dump or as a prepared ELF, so both forms of one build match one table entry.
std::string sha256_executable_image(const std::string& root) {
    try {
        const std::string image = vita_exe(root);
        size_t at = 0;
        return sha256_of([&](char* into, size_t room) {
            const size_t n = (std::min)(room, image.size() - at);
            std::memcpy(into, image.data() + at, n);
            at += n;
            return n;
        });
    } catch (const std::exception&) {
        return "";
    }
}

std::string known_versions_file() {
    std::error_code ec;
    const std::string dir = paths::exe_dir();
    for (const char* rel : {"/data/known_versions.txt", "/../../port/data/known_versions.txt"}) {
        const std::string candidate = dir + rel;
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    return "";
}

// `<sha256> <file> <label...>` per line, '#' starts a comment.
struct Known {
    std::string file, label;
};
std::multimap<std::string, Known> read_known() {
    std::multimap<std::string, Known> table;
    std::ifstream f(known_versions_file());
    for (std::string line; std::getline(f, line);) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        std::string hash, file, label;
        in >> hash >> file;
        std::getline(in, label);
        if (!label.empty() && label[0] == ' ') label.erase(0, 1);
        if (!hash.empty() && !file.empty()) table.insert({hash, {file, label}});
    }
    return table;
}

// A path as it fits on a line of the launcher (one word cannot be broken across lines): the start and the end, with the middle
// left out when it is long.
std::string shorten(const std::string& path) {
    if (path.size() <= 70) return path;
    size_t head = 10, tail = path.size() - 55;
    while (head > 0 && (uint8_t(path[head]) & 0xC0) == 0x80) --head;  // not in the middle of a letter
    while (tail < path.size() && (uint8_t(path[tail]) & 0xC0) == 0x80) ++tail;
    return path.substr(0, head) + "..." + path.substr(tail);
}

std::string megabytes(uintmax_t bytes) {
    char text[40];
    std::snprintf(text, sizeof text, "%.1f MB", double(bytes) / 1e6);
    return text;
}

// Whether the three data files are whole, not only present: a file cut short by a copy that did not finish, or damaged, still
// has its name and (for the archives) its first bytes, and would stop the game at its start. "" when they are fine, else the
// problem in plain words with what to do. AMAGAMI.IMG is unpacked and parsed (about a megabyte); the two archives have their
// table of contents read and every entry's end compared with the file's size.
std::string damaged_file(const std::string& root) {
    std::error_code ec;
    try {
        const std::string path = data_file(root, "AMAGAMI.IMG");
        read_image(lzss_unpack(read_file(path)));
    } catch (const std::exception&) {
        return ui::fmt("{0} is damaged: it cannot be read as the game's data. Copy the game's files again from your original and choose the folder again.", {"AMAGAMI.IMG"});
    }
    for (const char* rel : {"SCENARIO.CPK", "graph/GRAPH.CPK"}) {
        const std::string path = data_file(root, rel);
        const uintmax_t size = std::filesystem::file_size(path, ec);
        try {
            uintmax_t end = 0;
            for (const auto& [name, entry] : cpk_read_index(path)) end = (std::max)(end, uintmax_t(entry.offset + entry.size));
            if (!ec && end > size)
                return ui::fmt("{0} is incomplete: it is {1} but the game needs at least {2}. The copy was cut short. Copy the game's files again and choose the folder again.",
                               {rel, megabytes(size), megabytes(end)});
        } catch (const std::exception&) {
            return ui::fmt("{0} is damaged: it cannot be read as the game's data. Copy the game's files again from your original and choose the folder again.", {rel});
        }
    }
    return "";
}

}  // namespace

namespace {

// What the folder lacks, said so that the player knows what to do: the files that are missing, and, when it helps, where the
// game seems to be instead or why Windows may not be reading it.
std::string missing_message(const std::string& root, const std::string& missing) {
    std::error_code ec;
    bool empty = true;
    for (std::filesystem::directory_iterator it(std::filesystem::u8path(root), ec), end; !ec && it != end; it.increment(ec)) {
        empty = false;
        break;
    }
    if (empty)
        return ui::tr("This folder is empty. Choose the folder that holds the game's files (AMAGAMI.IMG, SCENARIO.CPK, graph, eboot.bin).");
    const std::string inner = find_game_root(root);
    if (inner != root && check_game_folder(inner).empty())
        return ui::fmt("The game's files are in a folder inside this one: {0}. Choose that folder.", {shorten(inner)});
    if (root.size() > 240)
        return ui::fmt("The path of this folder is very long ({0} characters) and Windows may refuse to open it. Move the game's folder to a shorter place, such as C:\\Amagami, and choose it again.",
                       {std::to_string(root.size())});
    return ui::fmt("Missing: {0}. Choose the folder that has these files (the PS Vita version, PCSG00291), or copy the missing ones into this folder.", {missing});
}

}  // namespace

std::vector<Check> check_data(const std::string& root) {
    std::vector<Check> out;
    std::error_code ec;
    if (root.empty()) {
        out.push_back({Check::Fail, "Game folder", ui::tr("No folder chosen yet. Click \"Choose game folder...\" or drop the game's folder on this window.")});
        return out;
    }
    if (!std::filesystem::is_directory(std::filesystem::u8path(root), ec)) {
        const bool is_file = std::filesystem::exists(std::filesystem::u8path(root), ec);
        out.push_back({Check::Fail, "Game folder",
                       is_file ? ui::fmt("{0} is a file, not a folder. Choose the folder that holds the game's files.", {shorten(root)})
                               : ui::fmt("The folder {0} is not there any more. It may have been moved, renamed or deleted, or its drive is not connected. Choose the folder again.", {shorten(root)})});
        return out;
    }
    // 1. The data files.
    std::string missing;
    uintmax_t total = 0;
    for (const char* rel : {"AMAGAMI.IMG", "SCENARIO.CPK", "graph/GRAPH.CPK"}) {
        const std::string path = data_file(root, rel);
        if (!std::filesystem::exists(path, ec)) missing += std::string(missing.empty() ? "" : ", ") + rel;
        else total += std::filesystem::file_size(path, ec);
    }
    const std::string executable_path = vita_exe_file(root);
    if (executable_path.empty()) missing += std::string(missing.empty() ? "" : ", ") + "eboot.bin or extracted/eboot_decrypted.elf";
    else total += std::filesystem::file_size(executable_path, ec);
    if (!missing.empty()) {
        out.push_back({Check::Fail, "Game data", missing_message(root, missing)});
        return out;
    }
    out.push_back({Check::Ok, "Game data", ui::fmt("AMAGAMI.IMG, SCENARIO.CPK, graph and the executable are there ({0} checked).", {megabytes(total)})});
    // 2. Readable: not still encrypted by the console, and a decrypted executable.
    const std::string problem = check_game_folder(root);
    if (!problem.empty()) {
        out.push_back({Check::Fail, "Decrypted copy", problem});
        return out;
    }
    // Whole, not only present: a copy cut short or damaged would stop the game at its start.
    if (const std::string damage = damaged_file(root); !damage.empty()) {
        out.push_back({Check::Fail, "Intact files", damage});
        return out;
    }
    out.push_back({Check::Ok, "Decrypted copy", ui::tr("The archives and the executable are readable.")});
    // Some known executable builds are explicitly marked unusable; the English fan patch v1.1 is supported.
    {
        const auto listed = read_known();
        auto range = listed.equal_range(sha256_file(executable_path));
        for (auto it = range.first; it != range.second; ++it)
            if (it->second.label.find("not usable") != std::string::npos) {
                out.push_back({Check::Fail, "Executable",
                               ui::tr("This eboot.bin version is not supported. Choose the original game folder, then add the supported English patch v1.1 in Game files.")});
                return out;
            }
    }
    // 3. The sound sources the engine can actually read, either packed or prepared.
    std::string quiet;
    const auto has_file = [&](const char* rel) {
        return std::filesystem::is_regular_file(data_file(root, rel), ec);
    };
    const bool voices = has_file("sound/VOICE.IDX") &&
                        (has_file("sound/VOICE.CPK") || has_vita_voice_files(root + "/sound/voice") ||
                         has_vita_voice_files(root + "/extracted/sound/voice"));
    if (!voices) quiet += std::string(quiet.empty() ? "" : ", ") + ui::tr("voices");
    if (!has_file("sound/MUSIC.CPK") && !has_vita_wav_files(root, "music"))
        quiet += std::string(quiet.empty() ? "" : ", ") + ui::tr("music");
    if (!has_file("sound/SOUND.CPK") && !has_vita_wav_files(root, "ambience"))
        quiet += std::string(quiet.empty() ? "" : ", ") + ui::tr("ambience");
    for (int bank = 0; bank < 2; ++bank) {
        const std::string acb = "sound/SYSSE" + std::to_string(bank) + ".ACB";
        const std::string wav_dir = root + "/extracted/sound_wav/se" + std::to_string(bank);
        if (!has_file(acb.c_str()) && !has_regular_file_with_extension(wav_dir, ".wav"))
            quiet += std::string(quiet.empty() ? "" : ", ") + ui::fmt("sound effects (bank {0})", {std::to_string(bank)});
    }
    if (quiet.empty()) out.push_back({Check::Ok, "Sound", ui::tr("Voices, music and sound effects are there.")});
    else out.push_back({Check::Warn, "Sound", ui::fmt("Missing readable sources: {0}. The game will run with parts of the sound missing. Copy the game's sound folder next to the other files to fix it.", {quiet})});
    // 4. The version: the hashes of the small files against the known dumps. An unknown one is a warning only.
    const auto known = read_known();
    if (known.empty()) {
        out.push_back({Check::Warn, "Version", ui::tr("The table of known versions is not there, so the files were not compared.")});
        return out;
    }
    int matched = 0, unknown = 0;
    std::string image_label;  // the label of AMAGAMI.IMG: it says the language of the data
    const std::string version_paths[] = {data_file(root, "AMAGAMI.IMG"), data_file(root, "SCENARIO.CPK"), executable_path};
    const std::string version_names[] = {"AMAGAMI.IMG", "SCENARIO.CPK", std::filesystem::path(executable_path).filename().string()};
    for (size_t i = 0; i < 3; ++i) {
        const std::string& name = version_names[i];
        const bool is_executable = i == 2;
        bool hit = false;
        auto matches = [&](const std::string& hash, const std::string& file) {
            auto range = known.equal_range(hash);
            for (auto it = range.first; it != range.second; ++it)
                if (it->second.file == file) {
                    if (file == "AMAGAMI.IMG") image_label = it->second.label;
                    return true;
                }
            return false;
        };
        hit = matches(sha256_file(version_paths[i]), name);
        if (!hit && is_executable) hit = matches(sha256_executable_image(root), "eboot_decrypted.elf");
        (hit ? matched : unknown)++;
    }
    if (unknown == 0) out.push_back({Check::Ok, "Version", ui::tr("Known files (PCSG00291).")});
    else out.push_back({Check::Warn, "Version", ui::fmt("{0} of 3 key files are not in the table of known versions (a patched or different copy). It may still work.", {std::to_string(unknown)})});
    // 5. The language: the data's own, and the layers beside it (Settings, Language).
    const bool english_data = image_label.find("English") != std::string::npos;
    const bool english_layer = std::filesystem::exists(std::filesystem::u8path(root) / "lang" / "en" / "extracted" / "AMAGAMI.IMG", ec);
    if (english_data) out.push_back({Check::Ok, "Language", ui::tr("English: this copy has the fan translation (v1.1) built in.")});
    else if (english_layer) out.push_back({Check::Ok, "Language", ui::tr("Japanese, with the English fan translation added: choose it in Game files, Language.")});
    else if (!image_label.empty())
        out.push_back({Check::Ok, "Language", ui::tr("Japanese. To play in English, add the fan translation in Game files, Add English patch.")});
    return out;
}

std::string damaged_files(const std::string& root) {
    try {
        return damaged_file(root);
    } catch (const std::exception& e) {
        return ui::fmt("This folder could not be checked ({0}). Choose another folder, or check that you can read it.", {e.what()});
    }
}

std::string known_label(const std::string& path, const std::string& name) {
    const auto known = read_known();
    auto range = known.equal_range(sha256_file(path));
    for (auto it = range.first; it != range.second; ++it)
        if (it->second.file == name) return it->second.label;
    return "";
}

bool usable(const std::vector<Check>& checks) {
    for (const Check& c : checks)
        if (c.status == Check::Fail) return false;
    return !checks.empty();
}

}  // namespace amg
