#include "paths.hpp"

#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace amg::paths {

namespace {

std::string env(const char* name) {
    const char* e = std::getenv(name);
    return e && *e ? e : "";
}

std::string join(const std::string& a, const char* b) { return a.empty() ? std::string(b) : a + "/" + b; }

}  // namespace

std::string exe_dir() {
#ifdef _WIN32
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, path, MAX_PATH)) return "";
    std::string dir = path;
    return dir.substr(0, dir.find_last_of(std::string("/") + char(92)));
#else
    char path[4096] = {};
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return "";
    std::string dir(path, size_t(n));
    return dir.substr(0, dir.find_last_of('/'));
#endif
}

namespace {

// Everything beside the program (D-048); off Windows nothing, so the callers keep their relative names.
std::string base() {
#ifdef _WIN32
    const std::string dir = exe_dir();
    if (!dir.empty()) return join(dir, "portable-data");
#endif
    return "";
}

}  // namespace

std::string legacy_dir(bool roaming) {
#ifdef _WIN32
    const std::string root = env(roaming ? "APPDATA" : "LOCALAPPDATA");
    if (!root.empty()) return root + "/open-amagami";
#endif
    (void)roaming;
    return "";
}

std::string config_file() {
    const std::string b = base();
    return b.empty() ? "" : b + "/config.ini";
}

std::string save_dir() {
    const std::string b = base();
    return b.empty() ? "save" : b + "/save";
}

std::string game_dir() {
    const std::string b = base();
    return b.empty() ? "" : b + "/game";
}

std::string log_dir() {
    const std::string b = base();
    return b.empty() ? "" : b + "/logs";
}

}  // namespace amg::paths
