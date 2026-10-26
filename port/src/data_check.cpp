#include "data_check.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "datapath.hpp"
#include "paths.hpp"

namespace amg {

namespace {

// SHA-256 of a file, as lower-case hex ("" if it cannot be read).
std::string sha256_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buffer(1 << 20);
        while (f) {
            f.read(buffer.data(), std::streamsize(buffer.size()));
            const std::streamsize got = f.gcount();
            if (got > 0) BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), ULONG(got), 0);
        }
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

std::string megabytes(uintmax_t bytes) {
    char text[40];
    std::snprintf(text, sizeof text, "%.1f MB", double(bytes) / 1e6);
    return text;
}

}  // namespace

std::vector<Check> check_data(const std::string& root) {
    std::vector<Check> out;
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec)) {
        out.push_back({Check::Fail, "Game folder", "No folder chosen yet."});
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
        out.push_back({Check::Fail, "Game data", "Missing: " + missing + ". Choose the folder of the PS Vita version (PCSG00291)."});
        return out;
    }
    out.push_back({Check::Ok, "Game data", "AMAGAMI.IMG, SCENARIO.CPK, graph and the executable are there (" + megabytes(total) + " checked)."});
    // 2. Readable: not still encrypted by the console, and a decrypted executable.
    const std::string problem = check_game_folder(root);
    if (!problem.empty()) {
        out.push_back({Check::Fail, "Decrypted copy", problem});
        return out;
    }
    out.push_back({Check::Ok, "Decrypted copy", "The archives and the executable are readable."});
    // 3. The sound files: without them the game plays silent, so a warning.
    std::string quiet;
    for (const char* rel : {"sound/VOICE.CPK", "sound/VOICE.IDX", "sound/MUSIC.CPK", "sound/SOUND.CPK", "sound/SYSSE0.ACB", "sound/SYSSE1.ACB"})
        if (!std::filesystem::exists(data_file(root, rel), ec)) quiet += std::string(quiet.empty() ? "" : ", ") + rel;
    if (quiet.empty()) out.push_back({Check::Ok, "Sound", "Voices, music and sound effects are there."});
    else out.push_back({Check::Warn, "Sound", "Missing: " + quiet + ". The game will run with parts of the sound missing."});
    // 4. The version: the hashes of the small files against the known dumps. An unknown one is a warning only.
    const auto known = read_known();
    if (known.empty()) {
        out.push_back({Check::Warn, "Version", "The table of known versions is not there, so the files were not compared."});
        return out;
    }
    int matched = 0, unknown = 0;
    std::string image_label;  // the label of AMAGAMI.IMG: it says the language of the data
    const std::string version_paths[] = {data_file(root, "AMAGAMI.IMG"), data_file(root, "SCENARIO.CPK"), executable_path};
    const std::string version_names[] = {"AMAGAMI.IMG", "SCENARIO.CPK", std::filesystem::path(executable_path).filename().string()};
    for (size_t i = 0; i < 3; ++i) {
        const std::string& name = version_names[i];
        const std::string hash = sha256_file(version_paths[i]);
        bool hit = false;
        auto range = known.equal_range(hash);
        for (auto it = range.first; it != range.second; ++it)
            if (it->second.file == name) {
                hit = true;
                if (name == "AMAGAMI.IMG") image_label = it->second.label;
            }
        (hit ? matched : unknown)++;
    }
    if (unknown == 0) out.push_back({Check::Ok, "Version", "Known files (PCSG00291)."});
    else out.push_back({Check::Warn, "Version", std::to_string(unknown) + " of 3 key files are not in the table of known versions (a patched or different copy). It may still work."});
    // 5. The language: the data's own, and the layers beside it (Settings, Language).
    const bool english_data = image_label.find("English") != std::string::npos;
    const bool english_layer = std::filesystem::exists(std::filesystem::u8path(root) / "lang" / "en" / "extracted" / "AMAGAMI.IMG", ec);
    if (english_data) out.push_back({Check::Ok, "Language", "English: this copy has the fan translation (v1.1) built in."});
    else if (english_layer) out.push_back({Check::Ok, "Language", "Japanese, with the English fan translation added: choose it in Settings, Language."});
    else if (!image_label.empty())
        out.push_back({Check::Ok, "Language", "Japanese. To play in English, add the fan translation in Settings, Add English patch."});
    return out;
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
