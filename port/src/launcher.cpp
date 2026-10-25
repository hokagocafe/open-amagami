#include "launcher.hpp"

#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "data_check.hpp"
#include "datapath.hpp"
#include "input.hpp"
#include "paths.hpp"
#include "port_settings.hpp"
#include "shortcut.hpp"

namespace amg::launcher {

namespace {

constexpr UINT WM_CHECKED = WM_APP + 1;
enum {
    ID_START = IDOK, ID_QUIT = IDCANCEL, ID_SETTINGS = 10, ID_FOLDER = 11, ID_DELETE = 12, ID_SAVES = 13, ID_CONTROLS = 14,
    ID_DEFAULTS = 15, ID_ENGLISH = 16, ID_SHORTCUT = 17, ID_ROW0 = 100, ID_KEY0 = 300
};

const COLORREF BACKGROUND = RGB(247, 246, 244), INK = RGB(40, 35, 62), MUTED = RGB(93, 88, 112), GOOD = RGB(46, 150, 90),
               WARN = RGB(214, 150, 30), BAD = RGB(200, 60, 70);

std::wstring widen(const std::string& text) {
    if (text.empty()) return L"";
    int n = MultiByteToWideChar(CP_ACP, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.data(), int(text.size()), out.data(), n);
    return out;
}

HFONT make_font(int points, bool bold) {
    return CreateFontW(-MulDiv(points, 96, 72), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

void set_font(HWND w, HFONT f) { SendMessageW(w, WM_SETFONT, WPARAM(f), TRUE); }

HWND make_button(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h, HFONT font, bool primary = false) {
    HWND b = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | (primary ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), x, y,
                           w, h, parent, HMENU(INT_PTR(id)), nullptr, nullptr);
    set_font(b, font);
    return b;
}

void centre_on_screen(HWND w) {
    RECT r;
    GetWindowRect(w, &r);
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(w, nullptr, (sw - (r.right - r.left)) / 2, (sh - (r.bottom - r.top)) / 3, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

// Runs messages for `window` until `closed` is set; a nested window can end without ending the program.
void pump(HWND window, const bool& closed) {
    MSG m;
    while (!closed && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}

// --- The controls window: the keyboard's keys (F5-14) ----------------------------------------------------------------

struct Controls {
    const Services* svc = nullptr;
    bool closed = false;
    int waiting = -1;  // the row that takes the next key, or -1
    std::vector<HWND> keys;
    HWND note = nullptr;
};

// The game's icon from the player's copy on the title bar and the taskbar (the program's own when the copy has none).
void set_game_icon(HWND win, const std::string& root) {
    if (HICON big = shortcut::load_game_icon(root, GetSystemMetrics(SM_CXICON))) SendMessageW(win, WM_SETICON, ICON_BIG, LPARAM(big));
    if (HICON little = shortcut::load_game_icon(root, GetSystemMetrics(SM_CXSMICON))) SendMessageW(win, WM_SETICON, ICON_SMALL, LPARAM(little));
}

std::wstring key_text(int vk) { return widen(input::key_name(vk)); }

void show_keys(Controls& c) {
    int count = 0;
    const input::GameButton* buttons = input::game_buttons(&count);
    for (int i = 0; i < count; ++i)
        SetWindowTextW(c.keys[size_t(i)], i == c.waiting ? L"Press a key..." : key_text(input::key_of(buttons[i].mask)).c_str());
}

// A key pressed while a row waits for one: Esc gives up; a reserved key or one bound to another button is refused.
void take_key(Controls& c, int vk) {
    int count = 0;
    const input::GameButton* buttons = input::game_buttons(&count);
    const int row = c.waiting;
    c.waiting = -1;
    std::wstring note;
    if (vk == VK_ESCAPE) {
        note = L"";
    } else if (input::reserved_key(vk)) {
        note = key_text(vk) + L" belongs to the window (Esc quits, F9 screenshot, F10 dump, F11 full screen).";
    } else {
        int other = -1;
        for (int i = 0; i < count; ++i)
            if (i != row && input::key_of(buttons[i].mask) == vk) other = i;
        if (other >= 0) {
            note = key_text(vk) + L" is already " + widen(buttons[other].label) + L". Give that one another key first.";
        } else {
            c.svc->write_setting(std::string("key_") + buttons[row].name, std::to_string(vk));
            input::load_keys(c.svc->read_setting);
        }
    }
    SetWindowTextW(c.note, note.c_str());
    show_keys(c);
}

LRESULT CALLBACK controls_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    Controls* c = reinterpret_cast<Controls*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_COMMAND: {
            const int id = LOWORD(w);
            if (!c) return 0;
            if (id >= ID_KEY0 && id < ID_KEY0 + int(c->keys.size())) {
                c->waiting = id - ID_KEY0;
                SetWindowTextW(c->note, L"Press the key for this button (Esc: keep the old one).");
                show_keys(*c);
            } else if (id == ID_DEFAULTS) {
                int count = 0;
                const input::GameButton* buttons = input::game_buttons(&count);
                for (int i = 0; i < count; ++i) c->svc->write_setting(std::string("key_") + buttons[i].name, "");
                input::load_keys(c->svc->read_setting);
                c->waiting = -1;
                SetWindowTextW(c->note, L"The default keys are back.");
                show_keys(*c);
            } else if (id == ID_QUIT) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            static HBRUSH brush = CreateSolidBrush(BACKGROUND);
            SetBkColor(HDC(w), BACKGROUND);
            SetTextColor(HDC(w), INK);
            return LRESULT(brush);
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (c) c->closed = true;
            PostMessageW(nullptr, WM_NULL, 0, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

void show_controls(HWND parent, HINSTANCE inst, const Services& svc, HFONT body) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = controls_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(BACKGROUND);
        wc.lpszClassName = L"AmagamiControls";
        RegisterClassW(&wc);
        registered = true;
    }
    Controls c;
    c.svc = &svc;
    input::load_keys(svc.read_setting);
    int count = 0;
    const input::GameButton* buttons = input::game_buttons(&count);
    const int row_h = 32, top = 20, width = 440, rows = (count + 1) / 2;
    const int height = top + rows * row_h + 110;
    RECT rc = {0, 0, width * 3 / 2, height};
    AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU, FALSE);
    HWND win = CreateWindowW(L"AmagamiControls", L"Keyboard controls", WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE, CW_USEDEFAULT,
                             CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, parent, nullptr, inst, nullptr);
    SetWindowLongPtrW(win, GWLP_USERDATA, LONG_PTR(&c));
    centre_on_screen(win);
    const int column = width * 3 / 4;  // two columns of buttons
    for (int i = 0; i < count; ++i) {
        const int x = 24 + (i / rows) * column, y = top + (i % rows) * row_h;
        HWND label = CreateWindowW(L"STATIC", widen(buttons[i].label).c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, x, y + 5, 150, 22, win,
                                   nullptr, inst, nullptr);
        set_font(label, body);
        c.keys.push_back(make_button(win, L"", ID_KEY0 + i, x + 150, y, 130, 28, body));
    }
    const int bottom = top + rows * row_h + 8;
    c.note = CreateWindowW(L"STATIC", L"Click a button's key, then press the new key.", WS_CHILD | WS_VISIBLE | SS_LEFT, 24, bottom,
                           width * 3 / 2 - 48, 40, win, nullptr, inst, nullptr);
    set_font(c.note, body);
    make_button(win, L"Restore defaults", ID_DEFAULTS, 24, bottom + 50, 150, 30, body);
    make_button(win, L"Close", ID_QUIT, width * 3 / 2 - 24 - 110, bottom + 50, 110, 30, body, true);
    show_keys(c);
    EnableWindow(parent, FALSE);
    // Its own message loop: while a row waits, every key goes to it (the dialog handling would take arrows, Tab and Enter).
    MSG m;
    while (!c.closed && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (c.waiting >= 0 && (m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN)) {
            take_key(c, int(m.wParam));
            continue;
        }
        if (c.waiting >= 0 && (m.message == WM_KEYUP || m.message == WM_SYSKEYUP || m.message == WM_CHAR || m.message == WM_SYSCHAR))
            continue;
        if (!IsDialogMessageW(win, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
}

// --- The settings window -------------------------------------------------------------------------------------------

struct Settings {
    const Services* svc = nullptr;
    std::string root;
    bool closed = false;
    bool data_changed = false;  // the folder was removed or chosen again: the launcher checks again
    HFONT body = nullptr;
    struct Row {
        std::string key;
        std::vector<std::string> values;
        HWND combo = nullptr;
    };
    std::vector<Row> rows;
};

// `base_english`: the game folder's own data is the English fan translation (the Vita3K-style patched copy).
std::string language_name(const std::string& code, bool base_english) {
    if (code.empty()) return base_english ? "This copy's own (English)" : "Original (Japanese)";
    if (code == "en") return "English (fan patch)";
    return code;
}

LRESULT CALLBACK settings_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    Settings* s = reinterpret_cast<Settings*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_COMMAND: {
            const int id = LOWORD(w), note = HIWORD(w);
            if (id >= ID_ROW0 && id < ID_ROW0 + 100 && note == CBN_SELCHANGE && s) {
                Settings::Row& row = s->rows[size_t(id - ID_ROW0)];
                const int pick = int(SendMessageW(row.combo, CB_GETCURSEL, 0, 0));
                if (pick >= 0 && size_t(pick) < row.values.size()) {
                    if (row.key == "lang") s->svc->write_setting("lang", row.values[size_t(pick)]);
                    else port_settings::set_value(row.key, row.values[size_t(pick)]);
                }
            } else if (id == ID_SAVES) {
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::u8path(paths::save_dir()), ec);
                ShellExecuteA(nullptr, "open", paths::save_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else if (id == ID_DELETE && s) {
                const std::string copy = paths::game_dir();
                std::error_code ec;
                if (copy.empty() || !std::filesystem::exists(copy, ec)) {
                    MessageBoxW(hwnd, L"There is no copy of the game in the program's own folder.", L"Amagami", MB_OK | MB_ICONINFORMATION);
                } else if (MessageBoxW(hwnd, (L"Delete the copy of the game in:\n" + widen(copy) + L"\n\nYour saves and settings are kept. "
                                              L"You can copy the game again from your own folder.").c_str(),
                                       L"Amagami", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                    std::filesystem::remove_all(std::filesystem::u8path(copy), ec);
                    if (s->svc->read_setting("root") == copy) s->svc->write_setting("root", "");
                    s->data_changed = true;
                }
            } else if (id == ID_ENGLISH && s) {
                if (s->svc->add_english && s->svc->add_english(s->root)) {
                    s->data_changed = true;  // the launcher checks again (the Language line)
                    DestroyWindow(hwnd);     // reopened, the Language row lists the new layer
                }
            } else if (id == ID_SHORTCUT) {
                const std::string failed = shortcut::create(s->root);
                MessageBoxW(hwnd, failed.empty() ? L"Amagami is now on the desktop and in the Start menu." : widen(failed).c_str(), L"Amagami",
                            MB_OK | (failed.empty() ? MB_ICONINFORMATION : MB_ICONWARNING));
            } else if (id == ID_CONTROLS && s) {
                show_controls(hwnd, HINSTANCE(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)), *s->svc, s->body);
            } else if (id == ID_FOLDER && s) {
                const std::string chosen = s->svc->choose_folder();
                if (!chosen.empty()) s->data_changed = true;
            } else if (id == ID_QUIT) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            static HBRUSH brush = CreateSolidBrush(BACKGROUND);
            SetBkColor(HDC(w), BACKGROUND);
            SetTextColor(HDC(w), INK);
            return LRESULT(brush);
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (s) s->closed = true;
            PostMessageW(nullptr, WM_NULL, 0, 0);  // wakes the loop that waits for this window
            return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

// Returns true if the data (folder) changed while the window was open.
bool show_settings(HWND parent, HINSTANCE inst, const Services& svc, const std::string& root, HFONT body, HFONT bold) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = settings_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(BACKGROUND);
        wc.lpszClassName = L"AmagamiSettings";
        RegisterClassW(&wc);
        registered = true;
    }
    Settings s;
    s.svc = &svc;
    s.root = root;
    s.body = body;
    // The rows: the language first (the layers found in the game's folder), then the port's own.
    struct Entry {
        std::string key, label;
        std::vector<std::string> values, names;
        int index = 0;
    };
    std::vector<Entry> entries;
    {
        Entry lang;
        lang.key = "lang";
        lang.label = "Language";
        lang.values.push_back("");
        std::error_code ec;
        const std::filesystem::path layers = std::filesystem::u8path(root) / "lang";
        if (std::filesystem::is_directory(layers, ec))
            for (const auto& e : std::filesystem::directory_iterator(layers, ec))
                if (e.is_directory(ec)) lang.values.push_back(e.path().filename().string());
        const bool base_english = known_label(data_file(root, "AMAGAMI.IMG"), "AMAGAMI.IMG").find("English") != std::string::npos;
        const std::string current = svc.read_setting("lang");
        for (size_t i = 0; i < lang.values.size(); ++i) {
            lang.names.push_back(language_name(lang.values[i], base_english));
            if (lang.values[i] == current) lang.index = int(i);
        }
        entries.push_back(std::move(lang));
    }
    for (const port_settings::RowInfo& r : port_settings::all_rows()) {
        Entry e;
        e.key = r.key;
        e.label = r.label;
        e.values = r.values;
        e.names = r.names;
        e.index = r.index;
        entries.push_back(std::move(e));
    }
    const int row_h = 34, top = 20, width = 500;
    const int height = top + int(entries.size()) * row_h + 160;
    RECT rc = {0, 0, width, height};
    AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU, FALSE);
    HWND win = CreateWindowW(L"AmagamiSettings", L"Settings", WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top, parent, nullptr, inst, nullptr);
    SetWindowLongPtrW(win, GWLP_USERDATA, LONG_PTR(&s));
    centre_on_screen(win);
    int y = top;
    for (size_t i = 0; i < entries.size(); ++i, y += row_h) {
        HWND label = CreateWindowW(L"STATIC", widen(entries[i].label).c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 24, y + 4, 200, 22, win,
                                   nullptr, inst, nullptr);
        set_font(label, body);
        HWND combo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 240, y, 236, 200, win,
                                   HMENU(INT_PTR(ID_ROW0 + int(i))), inst, nullptr);
        set_font(combo, body);
        for (const std::string& name : entries[i].names) SendMessageW(combo, CB_ADDSTRING, 0, LPARAM(widen(name).c_str()));
        SendMessageW(combo, CB_SETCURSEL, WPARAM(entries[i].index), 0);
        Settings::Row row;
        row.key = entries[i].key;
        row.values = entries[i].values;
        row.combo = combo;
        s.rows.push_back(std::move(row));
    }
    y += 12;
    make_button(win, L"Choose game folder...", ID_FOLDER, 24, y, 170, 30, body);
    make_button(win, L"Open saves folder", ID_SAVES, 206, y, 140, 30, body);
    make_button(win, L"Controls...", ID_CONTROLS, 358, y, 118, 30, body);
    make_button(win, L"Delete copy of the game...", ID_DELETE, 24, y + 40, 200, 30, body);
    make_button(win, L"Add English patch...", ID_ENGLISH, 234, y + 40, 122, 30, body);
    make_button(win, L"Desktop shortcut", ID_SHORTCUT, 24, y + 80, 200, 30, body);
    make_button(win, L"Close", ID_QUIT, width - 24 - 110, y + 80, 110, 30, body, true);
    (void)bold;
    EnableWindow(parent, FALSE);
    pump(win, s.closed);
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    return s.data_changed;
}

// --- The launcher window ---------------------------------------------------------------------------------------------

struct Launcher {
    HINSTANCE inst = nullptr;
    const Services* svc = nullptr;
    std::string root, result;
    std::vector<Check> checks;
    bool checking = false;
    bool closed = false;
    int generation = 0;
    HWND win = nullptr, start = nullptr;
    HFONT body = nullptr, bold = nullptr, title = nullptr;
};

struct Answer {
    int generation;
    std::vector<Check> checks;
};

void recheck(Launcher& l) {
    l.checking = true;
    l.checks.clear();
    const int generation = ++l.generation;
    const std::string root = l.root;
    HWND hwnd = l.win;
    EnableWindow(l.start, FALSE);
    InvalidateRect(hwnd, nullptr, TRUE);
    std::thread([hwnd, root, generation] {
        auto* a = new Answer{generation, check_data(root)};
        if (!PostMessageW(hwnd, WM_CHECKED, 0, LPARAM(a))) delete a;
    }).detach();
}

void paint(Launcher& l, HDC dc, const RECT& client) {
    SetBkMode(dc, TRANSPARENT);
    const int margin = 24;
    SelectObject(dc, l.title);
    SetTextColor(dc, INK);
    RECT r = {margin, 16, client.right - margin, 56};
    DrawTextW(dc, L"Amagami", -1, &r, DT_LEFT | DT_SINGLELINE);
    SelectObject(dc, l.body);
    SetTextColor(dc, MUTED);
    r = {margin, 52, client.right - margin, 76};
    DrawTextW(dc, l.checking ? L"Checking your game folder..." : L"Your game folder", -1, &r, DT_LEFT | DT_SINGLELINE);
    int y = 86;
    for (const Check& c : l.checks) {
        const COLORREF colour = c.status == Check::Ok ? GOOD : c.status == Check::Warn ? WARN : BAD;
        HBRUSH brush = CreateSolidBrush(colour);
        HGDIOBJ old = SelectObject(dc, brush);
        HPEN pen = CreatePen(PS_NULL, 0, 0);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        Ellipse(dc, margin, y + 2, margin + 22, y + 24);
        SelectObject(dc, old_pen);
        SelectObject(dc, old);
        DeleteObject(pen);
        DeleteObject(brush);
        // The mark inside the circle, drawn with lines (the fonts have no check mark that is the same everywhere).
        HPEN white = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        HGDIOBJ before = SelectObject(dc, white);
        const int cx = margin, cy = y + 2;
        if (c.status == Check::Ok) {
            MoveToEx(dc, cx + 6, cy + 11, nullptr);
            LineTo(dc, cx + 10, cy + 15);
            LineTo(dc, cx + 16, cy + 7);
        } else if (c.status == Check::Fail) {
            MoveToEx(dc, cx + 7, cy + 7, nullptr);
            LineTo(dc, cx + 15, cy + 15);
            MoveToEx(dc, cx + 15, cy + 7, nullptr);
            LineTo(dc, cx + 7, cy + 15);
        } else {
            MoveToEx(dc, cx + 11, cy + 5, nullptr);
            LineTo(dc, cx + 11, cy + 13);
            MoveToEx(dc, cx + 11, cy + 16, nullptr);
            LineTo(dc, cx + 11, cy + 17);
        }
        SelectObject(dc, before);
        DeleteObject(white);
        SelectObject(dc, l.bold);
        SetTextColor(dc, INK);
        RECT what = {margin + 34, y, client.right - margin, y + 22};
        DrawTextW(dc, widen(c.what).c_str(), -1, &what, DT_LEFT | DT_SINGLELINE);
        SelectObject(dc, l.body);
        SetTextColor(dc, MUTED);
        const std::wstring detail = widen(c.detail);
        RECT text = {margin + 34, y + 22, client.right - margin, client.bottom};
        DrawTextW(dc, detail.c_str(), -1, &text, DT_LEFT | DT_WORDBREAK | DT_CALCRECT);
        DrawTextW(dc, detail.c_str(), -1, &text, DT_LEFT | DT_WORDBREAK);
        y = text.bottom + 14;
    }
    if (!l.checking && l.root.empty() && l.checks.empty()) {
        SetTextColor(dc, INK);
        RECT t = {margin, y, client.right - margin, client.bottom};
        DrawTextW(dc, L"Choose the folder of your own copy of the game to begin.", -1, &t, DT_LEFT | DT_WORDBREAK);
    }
}

LRESULT CALLBACK launcher_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM lp) {
    Launcher* l = reinterpret_cast<Launcher*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (l) {
                RECT client;
                GetClientRect(hwnd, &client);
                HDC mem = CreateCompatibleDC(dc);
                HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
                HGDIOBJ old = SelectObject(mem, bmp);
                HBRUSH brush = CreateSolidBrush(BACKGROUND);
                FillRect(mem, &client, brush);
                DeleteObject(brush);
                paint(*l, mem, client);
                BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CHECKED: {
            std::unique_ptr<Answer> a(reinterpret_cast<Answer*>(lp));
            if (l && a->generation == l->generation) {
                l->checking = false;
                l->checks = std::move(a->checks);
                EnableWindow(l->start, usable(l->checks));
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }
        case WM_COMMAND: {
            if (!l) return 0;
            const int id = LOWORD(w);
            if (id == ID_START && l->start && IsWindowEnabled(l->start)) {
                l->result = l->root;
                DestroyWindow(hwnd);
            } else if (id == ID_QUIT) {
                DestroyWindow(hwnd);
            } else if (id == ID_FOLDER) {
                const std::string chosen = l->svc->choose_folder();
                if (!chosen.empty()) {
                    l->root = chosen;
                    recheck(*l);
                }
            } else if (id == ID_SETTINGS) {
                if (show_settings(hwnd, l->inst, *l->svc, l->root, l->body, l->bold)) {
                    l->root = l->svc->read_setting("root");
                    recheck(*l);
                }
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (l) l->closed = true;
            PostMessageW(nullptr, WM_NULL, 0, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, w, lp);
}

}  // namespace

std::string run(HINSTANCE inst, const std::string& root, const Services& services) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = launcher_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
#ifdef AMG_HAS_ICON
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
#endif
    wc.lpszClassName = L"AmagamiLauncher";
    RegisterClassW(&wc);

    Launcher l;
    l.inst = inst;
    l.svc = &services;
    l.root = root;
    l.body = make_font(10, false);
    l.bold = make_font(10, true);
    l.title = make_font(22, true);
    const int width = 560, height = 470;
    RECT rc = {0, 0, width, height};
    AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    l.win = CreateWindowW(L"AmagamiLauncher", L"Amagami", WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                          rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    SetWindowLongPtrW(l.win, GWLP_USERDATA, LONG_PTR(&l));
    set_game_icon(l.win, root);
    centre_on_screen(l.win);
    const int by = height - 24 - 36;
    make_button(l.win, L"Choose game folder...", ID_FOLDER, 24, by, 176, 36, l.body);
    make_button(l.win, L"Settings", ID_SETTINGS, width - 24 - 130 - 12 - 110, by, 110, 36, l.body);
    l.start = make_button(l.win, L"Start game", ID_START, width - 24 - 130, by, 130, 36, l.bold, true);
    recheck(l);
    SetForegroundWindow(l.win);
    pump(l.win, l.closed);
    DeleteObject(l.body);
    DeleteObject(l.bold);
    DeleteObject(l.title);
    return l.result;
}

}  // namespace amg::launcher
