#include "game_import.hpp"

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

#include "datapath.hpp"
#include "paths.hpp"
#include "ui_text.hpp"

namespace amg {

namespace {

namespace fs = std::filesystem;

struct Item {
    std::string from, to;  // `to` is relative to the destination
    uint64_t size = 0;
};

// What the port reads: the three data files, the executable, and everything in `sound/`.
std::vector<Item> plan(const std::string& src) {
    std::vector<Item> items;
    std::set<std::string> destinations;
    std::error_code ec;
    auto add = [&](const std::string& from, const std::string& to) {
        if (!fs::is_regular_file(from, ec) || !destinations.insert(to).second) return;
        Item it;
        it.from = from;
        it.to = to;
        it.size = uint64_t(fs::file_size(from, ec));
        items.push_back(std::move(it));
    };
    auto add_tree = [&](const std::string& folder, const std::string& destination) {
        if (!fs::is_directory(folder, ec)) return;
        for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string relative = it->path().lexically_relative(folder).generic_string();
            add(it->path().string(), destination + relative);
        }
    };
    add(data_file(src, "AMAGAMI.IMG"), "AMAGAMI.IMG");
    add(data_file(src, "SCENARIO.CPK"), "SCENARIO.CPK");
    add(data_file(src, "graph/GRAPH.CPK"), "graph/GRAPH.CPK");
    const std::string executable = vita_exe_file(src);
    if (!executable.empty()) {
        const bool prepared = fs::path(executable).filename() == "eboot_decrypted.elf";
        add(executable, prepared ? "extracted/eboot_decrypted.elf" : "eboot.bin");
    }
    add(data_file(src, "sce_sys/icon0.png"), "sce_sys/icon0.png");  // the home screen icon: the shortcuts and windows use it
    add_tree(src + "/patch/PCSG00291/sound", "sound/");
    add_tree(src + "/app/PCSG00291/sound", "sound/");
    add_tree(src + "/sound", "sound/");
    add_tree(src + "/extracted/sound", "sound/");
    add_tree(src + "/extracted/sound_wav", "extracted/sound_wav/");
    return items;
}

struct Progress {
    HWND window = nullptr, bar = nullptr, label = nullptr;
    bool cancelled = false;
};

LRESULT CALLBACK progress_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_CLOSE) {
        auto* p = reinterpret_cast<Progress*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (p) p->cancelled = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

void pump() {
    MSG m;
    while (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
}

void show(Progress& p, uint64_t done, uint64_t total, const std::string& what) {
    int percent = total ? int(done * 100 / total) : 100;
    SendMessage(p.bar, PBM_SETPOS, WPARAM(percent), 0);
    char got[32], of[32];
    std::snprintf(got, sizeof got, "%.2f", double(done) / 1e9);
    std::snprintf(of, sizeof of, "%.2f", double(total) / 1e9);
    SetWindowTextW(p.label, ui::wfmt("{0}\n{1} of {2} GB", {what, got, of}).c_str());
    SetWindowTextW(p.window, ui::wfmt("Amagami: importing the game, {0} %", {std::to_string(percent)}).c_str());
    pump();
}

}  // namespace

std::string import_destination() {
    return paths::game_dir();
}

uint64_t import_size(const std::string& src) {
    uint64_t total = 0;
    for (const Item& it : plan(src)) total += it.size;
    return total;
}

std::string import_game(const std::string& src, const std::string& dst) {
    std::vector<Item> items = plan(src);
    uint64_t total = 0;
    for (const Item& it : items) total += it.size;
    std::error_code ec;
    fs::create_directories(dst, ec);
    ULARGE_INTEGER free_bytes = {};
    if (GetDiskFreeSpaceExA(dst.c_str(), &free_bytes, nullptr, nullptr) && free_bytes.QuadPart < total + (200ull << 20))
        return ui::fmt("There is not enough free space for the game's files (about {0} GB needed) in {1}.",
                       {std::to_string(total / 1000000000ull + 1), dst});

    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = progress_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AmagamiImport";
    RegisterClassW(&wc);
    Progress p;
    p.window = CreateWindowW(L"AmagamiImport", ui::wtr("Amagami: importing the game").c_str(), WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, 520, 150, nullptr, nullptr, wc.hInstance, nullptr);
    p.label = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 12, 480, 40, p.window, nullptr, wc.hInstance, nullptr);
    p.bar = CreateWindowW(PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 16, 62, 470, 22, p.window, nullptr, wc.hInstance, nullptr);
    SetWindowLongPtrW(p.window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&p));

    std::string result;
    uint64_t done = 0;
    std::vector<char> buffer(4 << 20);
    for (const Item& it : items) {
        std::string target = dst + "/" + it.to;
        fs::create_directories(fs::path(target).parent_path(), ec);
        if (fs::exists(target, ec) && uint64_t(fs::file_size(target, ec)) == it.size) {  // already imported
            done += it.size;
            show(p, done, total, it.to + ui::tr(" (already there)"));
            continue;
        }
        std::ifstream in(it.from, std::ios::binary);
        std::ofstream out(target + ".part", std::ios::binary | std::ios::trunc);
        if (!in || !out) {
            result = ui::fmt("Could not copy {0}. Check that your folder can be read and that the disk has room, then try again.", {it.to});
            break;
        }
        uint64_t left = it.size;
        while (left > 0) {
            size_t n = size_t(std::min<uint64_t>(left, buffer.size()));
            in.read(buffer.data(), std::streamsize(n));
            if (size_t(in.gcount()) != n) {
                result = ui::fmt("Could not read {0} from your copy. If the copy is damaged, copy the game's files again.", {it.to});
                break;
            }
            out.write(buffer.data(), std::streamsize(n));
            if (!out) {
                result = ui::fmt("Could not write {0} (is the disk full?).", {it.to});
                break;
            }
            left -= n;
            done += n;
            show(p, done, total, it.to);
            if (p.cancelled) {
                result = ui::tr("Cancelled.");
                break;
            }
        }
        in.close();
        out.close();
        if (!result.empty()) {
            fs::remove(target + ".part", ec);
            break;
        }
        fs::rename(target + ".part", target, ec);
        if (ec) {
            result = ui::fmt("Could not finish {0}. Check that the disk has room and that no other program is using the folder, then try again.", {it.to});
            break;
        }
    }
    DestroyWindow(p.window);
    pump();
    return result;
}

}  // namespace amg
