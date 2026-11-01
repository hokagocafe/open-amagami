#include "shortcut.hpp"

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "paths.hpp"
#include "ui_text.hpp"
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

// Writes one .lnk at `link` pointing at `exe`, run from the program's folder, with the icon `icon`.
bool write_link(const std::wstring& link, const std::wstring& exe, const std::wstring& dir, const std::wstring& icon) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&sl))))
        return false;
    sl->SetPath(exe.c_str());
    sl->SetWorkingDirectory(dir.c_str());
    sl->SetIconLocation(icon.c_str(), 0);
    sl->SetDescription(L"Play Amagami (open-amagami)");
    IPersistFile* pf = nullptr;
    bool ok = SUCCEEDED(sl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pf))) && SUCCEEDED(pf->Save(link.c_str(), TRUE));
    if (pf) pf->Release();
    sl->Release();
    return ok;
}

std::wstring known_folder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
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
    // Beside the imported copy, not inside it: "Delete copy of the game" leaves the shortcuts their icon.
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

std::string create(const std::string& root) {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return ui::tr("The program's own path could not be read.");
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    const std::string icon_file = game_icon_file(root);
    const std::wstring icon = icon_file.empty() ? std::wstring(exe) : fs::u8path(icon_file).wstring();
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string failed;
    const std::wstring desktop = known_folder(FOLDERID_Desktop), programs = known_folder(FOLDERID_Programs);
    if (desktop.empty() || !write_link(desktop + L"\\Amagami.lnk", exe, dir, icon)) failed += ui::tr("The desktop shortcut could not be made.") + " ";
    if (programs.empty() || !write_link(programs + L"\\Amagami.lnk", exe, dir, icon)) failed += ui::tr("The Start menu shortcut could not be made.");
    if (SUCCEEDED(co)) CoUninitialize();
    return failed;
}

void refresh(const std::string& root) {
    if (paths::portable()) return;
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return;
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring icon;
    for (REFKNOWNFOLDERID folder : {FOLDERID_Desktop, FOLDERID_Programs}) {
        const std::wstring base = known_folder(folder);
        if (base.empty()) continue;
        const std::wstring link = base + L"\\Amagami.lnk";
        std::error_code ec;
        if (!fs::exists(link, ec)) continue;
        IShellLinkW* sl = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&sl)))) continue;
        IPersistFile* pf = nullptr;
        wchar_t target[MAX_PATH] = {};
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pf))) && SUCCEEDED(pf->Load(link.c_str(), STGM_READ)))
            sl->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH);
        if (pf) pf->Release();
        sl->Release();
        if (target[0] && lstrcmpiW(target, exe) != 0) {
            if (icon.empty()) {
                const std::string file = game_icon_file(root);
                icon = file.empty() ? std::wstring(exe) : fs::u8path(file).wstring();
            }
            write_link(link, exe, dir, icon);
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

void offer_once(const std::string& root, bool asked, void (*mark_asked)()) {
    if (asked || paths::portable()) return;
    mark_asked();
    if (MessageBoxW(nullptr, ui::wtr("Put a shortcut to Amagami on the desktop and in the Start menu?\n\nYou can also make them later in General, Desktop shortcut.").c_str(),
                    L"Amagami", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    const std::string failed = create(root);
    if (!failed.empty()) MessageBoxW(nullptr, ui::wide(failed).c_str(), L"Amagami", MB_OK | MB_ICONWARNING);
}

}  // namespace amg::shortcut
