#include "shortcut.hpp"

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "paths.hpp"
#include "third_party/stb/stb_image.h"

namespace amg::shortcut {

namespace fs = std::filesystem;

namespace {

void put16(std::vector<uint8_t>& out, uint32_t v) { out.insert(out.end(), {uint8_t(v), uint8_t(v >> 8)}); }
void put32(std::vector<uint8_t>& out, uint32_t v) { put16(out, v & 0xFFFF); put16(out, v >> 16); }

// A one-image .ico (32-bit colour with alpha, the classic bitmap form every Windows reads) from RGBA pixels.
std::vector<uint8_t> ico_from_rgba(const unsigned char* rgba, int w, int h) {
    const uint32_t mask_row = uint32_t((w + 31) / 32 * 4);
    const uint32_t bytes = 40 + uint32_t(w * h * 4) + mask_row * uint32_t(h);
    std::vector<uint8_t> out;
    put16(out, 0); put16(out, 1); put16(out, 1);                                   // ICONDIR: an icon, one image
    out.push_back(uint8_t(w >= 256 ? 0 : w)); out.push_back(uint8_t(h >= 256 ? 0 : h));
    out.push_back(0); out.push_back(0); put16(out, 1); put16(out, 32); put32(out, bytes); put32(out, 22);
    put32(out, 40); put32(out, uint32_t(w)); put32(out, uint32_t(h * 2));          // BITMAPINFOHEADER (colour + mask height)
    put16(out, 1); put16(out, 32); put32(out, 0); put32(out, uint32_t(w * h * 4));
    put32(out, 0); put32(out, 0); put32(out, 0); put32(out, 0);
    for (int y = h - 1; y >= 0; --y)                                               // bottom-up, BGRA
        for (int x = 0; x < w; ++x) {
            const unsigned char* p = rgba + (size_t(y) * size_t(w) + size_t(x)) * 4;
            out.insert(out.end(), {p[2], p[1], p[0], p[3]});
        }
    out.insert(out.end(), size_t(mask_row) * size_t(h), uint8_t(0));               // AND mask unused: alpha decides
    return out;
}

std::vector<uint8_t> read_all(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

}  // namespace

std::string game_icon_file(const std::string& root) {
    std::error_code ec;
    const fs::path png = fs::u8path(root) / "sce_sys" / "icon0.png";
    const std::string games = paths::game_dir();
    if (root.empty() || games.empty() || !fs::exists(png, ec)) return "";
    const std::vector<uint8_t> file = read_all(png);
    int w = 0, h = 0, n = 0;
    unsigned char* rgba = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &n, 4);
    if (!rgba) return "";
    const std::vector<uint8_t> ico = ico_from_rgba(rgba, w, h);
    stbi_image_free(rgba);
    // Beside the imported copy, not inside it: "Delete copy of the game" leaves the window its icon.
    const fs::path out = (fs::u8path(games).parent_path() / "game-icon.ico").make_preferred();
    if (read_all(out) != ico) {
        fs::create_directories(out.parent_path(), ec);
        std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(ico.data()), std::streamsize(ico.size()));
    }
    return fs::exists(out, ec) ? out.u8string() : "";
}

HICON load_game_icon(const std::string& root, int size) {
    const std::string file = game_icon_file(root);
    if (file.empty()) return nullptr;
    return HICON(LoadImageW(nullptr, fs::u8path(file).wstring().c_str(), IMAGE_ICON, size, size, LR_LOADFROMFILE));
}

namespace {

// A .lnk that starts `target` from `work_dir` with the picture of `icon` (a .ico, "" for the program's own). "" when written, else the reason.
std::string write_link(const fs::path& lnk, const fs::path& target, const fs::path& work_dir, const std::string& icon, const std::wstring& description) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string error;
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        error = "the system's shortcut service is not available";
    } else {
        link->SetPath(target.wstring().c_str());
        link->SetWorkingDirectory(work_dir.wstring().c_str());
        link->SetDescription(description.c_str());
        if (!icon.empty()) link->SetIconLocation(fs::u8path(icon).wstring().c_str(), 0);
        IPersistFile* file = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
            if (FAILED(file->Save(lnk.wstring().c_str(), TRUE))) error = "the shortcut could not be written to " + lnk.u8string();
            file->Release();
        } else {
            error = "the system's shortcut service refused the file";
        }
        link->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return error;
}

std::wstring wide(const std::string& text) {
    if (text.empty()) return L"";
    std::wstring out(size_t(MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), int(out.size()));
    return out;
}

// The running program (the .exe file itself), "" if unknown.
fs::path program_file() {
    wchar_t buffer[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, DWORD(sizeof(buffer) / sizeof(buffer[0])));
    return n > 0 && n < sizeof(buffer) / sizeof(buffer[0]) ? fs::path(std::wstring(buffer, n)) : fs::path();
}

}  // namespace

std::string refresh_next_to_program(const std::string& name, const std::string& root) {
    const fs::path exe = program_file();
    const std::string icon = game_icon_file(root);
    if (exe.empty() || icon.empty()) return "";  // without the game's picture a shortcut would add nothing
    return write_link(exe.parent_path() / fs::u8path(name + ".lnk"), exe, exe.parent_path(), icon, wide(name));
}

std::string make_desktop_link(const std::string& name, const std::string& root, std::string& result) {
    const fs::path exe = program_file();
    if (exe.empty()) {
        result = "the program's own file could not be found";
        return "";
    }
    fs::path desktop;
    if (const char* forced = std::getenv("AMG_DESKTOP_DIR"); forced && *forced) {  // tests: never the player's real desktop
        desktop = fs::u8path(forced);
    } else {
        PWSTR known = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &known)) && known) desktop = fs::path(std::wstring(known));
        CoTaskMemFree(known);
    }
    std::error_code ec;
    if (desktop.empty() || !fs::is_directory(desktop, ec)) {
        result = "the desktop folder was not found";
        return "";
    }
    const fs::path lnk = desktop / fs::u8path(name + ".lnk");
    const std::string error = write_link(lnk, exe, exe.parent_path(), game_icon_file(root), wide(name));
    if (!error.empty()) {
        result = error;
        return "";
    }
    result = lnk.u8string();
    return result;
}

}  // namespace amg::shortcut
