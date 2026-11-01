#ifndef NOMINMAX
#define NOMINMAX
#endif
// The launcher's shared helpers and the two small windows opened from its settings: Controls (F5-14) and Pad mapping (F5-15).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "input.hpp"
#include "launcher_ui.hpp"
#include "ui_text.hpp"

namespace amg::launcher {

// --- Helpers ------------------------------------------------------------------------------------------------------------

namespace {

template <typename F>
F user32_function(const char* name) {
    return reinterpret_cast<F>(GetProcAddress(GetModuleHandleW(L"user32.dll"), name));
}

int forced_dpi() {
    const char* e = std::getenv("AMG_LAUNCHER_DPI");
    const int v = e ? std::atoi(e) : 0;
    return v >= 48 && v <= 480 ? v : 0;
}

bool forced_pos(int& x, int& y) {
    const char* e = std::getenv("AMG_LAUNCHER_POS");
    return e && std::sscanf(e, "%d,%d", &x, &y) == 2;
}

}  // namespace

bool fixed_dpi() { return forced_dpi() != 0; }

int dpi_for(HWND w) {
    if (const int f = forced_dpi()) return f;
    if (w)
        if (auto get = user32_function<UINT(WINAPI*)(HWND)>("GetDpiForWindow"))
            if (const UINT d = get(w)) return int(d);
    if (auto sys = user32_function<UINT(WINAPI*)()>("GetDpiForSystem")) return int(sys());
    HDC dc = GetDC(nullptr);
    const int d = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    return d > 0 ? d : 96;
}

HFONT message_font(int dpi, bool bold, double times) {
    NONCLIENTMETRICSW m = {};
    m.cbSize = sizeof m;
    using ForDpi = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    bool got = false;
    if (auto for_dpi = user32_function<ForDpi>("SystemParametersInfoForDpi")) got = for_dpi(SPI_GETNONCLIENTMETRICS, sizeof m, &m, 0, UINT(dpi)) != 0;
    if (!got) {
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof m, &m, 0)) {
            HDC dc = GetDC(nullptr);
            const int system = GetDeviceCaps(dc, LOGPIXELSY);
            ReleaseDC(nullptr, dc);
            m.lfMessageFont.lfHeight = MulDiv(m.lfMessageFont.lfHeight, dpi, system > 0 ? system : 96);
        } else {
            m.lfMessageFont = {};
            m.lfMessageFont.lfHeight = -MulDiv(9, dpi, 72);
            lstrcpyW(m.lfMessageFont.lfFaceName, L"Segoe UI");
        }
    }
    LOGFONTW f = m.lfMessageFont;
    f.lfHeight = LONG(f.lfHeight * times);
    if (bold) f.lfWeight = FW_SEMIBOLD;
    f.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW(&f);
}

void Fonts::make(int dpi) {
    free();
    body = message_font(dpi, false);
    bold = message_font(dpi, true);
    title = message_font(dpi, true, 2.0);
}

void Fonts::free() {
    for (HFONT* f : {&body, &bold, &title})
        if (*f) {
            DeleteObject(*f);
            *f = nullptr;
        }
}

void place(HWND w) {
    RECT r;
    GetWindowRect(w, &r);
    int x = 0, y = 0;
    if (!forced_pos(x, y)) {
        HWND owner = GetWindow(w, GW_OWNER);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(owner ? owner : w, MONITOR_DEFAULTTOPRIMARY), &mi);
        const RECT& a = mi.rcWork;
        x = a.left + ((a.right - a.left) - (r.right - r.left)) / 2;
        y = a.top + ((a.bottom - a.top) - (r.bottom - r.top)) / 3;
    }
    SetWindowPos(w, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void show(HWND w) {
    int x, y;
    if (forced_pos(x, y)) {
        ShowWindow(w, SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(w, SW_SHOW);
        SetForegroundWindow(w);
    }
}

std::wstring widen(const std::string& text) { return ui::wide(text); }
std::string narrow(const std::wstring& text) { return ui::narrow(text); }

void set_font(HWND w, HFONT f) { SendMessageW(w, WM_SETFONT, WPARAM(f), TRUE); }

HWND make_button(HWND parent, const std::wstring& text, int id, int x, int y, int w, int h, HFONT font, bool primary) {
    HWND b = CreateWindowW(L"BUTTON", text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | (primary ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), x, y,
                           w, h, parent, HMENU(INT_PTR(id)), nullptr, nullptr);
    set_font(b, font);
    return b;
}

HWND make_label(HWND parent, const std::wstring& text, int x, int y, int w, int h, HFONT font) {
    HWND s = CreateWindowW(L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, x, y, w, h, parent, nullptr, nullptr, nullptr);
    set_font(s, font);
    return s;
}

int text_width(HFONT font, const std::wstring& text) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, font);
    SIZE s = {};
    GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &s);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return s.cx;
}

void pump(HWND window, const bool& closed) {
    MSG m;
    while (!closed && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}

namespace {

LRESULT plain_static(WPARAM w) {
    static HBRUSH brush = CreateSolidBrush(BACKGROUND);
    SetBkColor(HDC(w), BACKGROUND);
    SetTextColor(HDC(w), INK);
    return LRESULT(brush);
}

void register_class(HINSTANCE inst, const wchar_t* name, WNDPROC proc) {
    WNDCLASSW wc = {};
    if (GetClassInfoW(inst, name, &wc)) return;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(BACKGROUND);
    wc.lpszClassName = name;
    RegisterClassW(&wc);
}

// A popup of `width` x `height` client pixels (already scaled), owned by `parent`, placed but not shown.
HWND make_popup(HINSTANCE inst, const wchar_t* cls, const std::wstring& title, HWND parent, int width, int height, int dpi) {
    RECT rc = {0, 0, width, height};
    using AdjustForDpi = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    if (auto adjust = user32_function<AdjustForDpi>("AdjustWindowRectExForDpi")) adjust(&rc, WS_CAPTION | WS_SYSMENU, FALSE, 0, UINT(dpi));
    else AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU, FALSE);
    HWND win = CreateWindowW(cls, title.c_str(), WS_CAPTION | WS_SYSMENU | WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                             rc.bottom - rc.top, parent, nullptr, inst, nullptr);
    RECT client, frame;  // the client area exactly the size asked for, whatever the frame's DPI
    GetClientRect(win, &client);
    GetWindowRect(win, &frame);
    SetWindowPos(win, nullptr, 0, 0, (frame.right - frame.left) + width - client.right, (frame.bottom - frame.top) + height - client.bottom,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    place(win);
    return win;
}

// --- The controls window: the keyboard's keys and the pad's buttons (F5-14) ------------------------------------------------

struct Controls {
    const Services* svc = nullptr;
    bool closed = false;
    int waiting = -1;      // the row that takes the next key, or -1
    int waiting_pad = -1;  // the row that takes the next pad button, or -1
    std::vector<HWND> keys;
    std::vector<HWND> pads;  // the pad button of each row
    HWND note = nullptr;
};

std::wstring key_text(int vk) { return ui::wtr(input::key_name(vk)); }

void show_keys(Controls& c) {
    int count = 0;
    const input::GameButton* buttons = input::game_buttons(&count);
    for (int i = 0; i < count; ++i)
        SetWindowTextW(c.keys[size_t(i)], i == c.waiting ? ui::wtr("Press a key...").c_str() : key_text(input::key_of(buttons[i].mask)).c_str());
}

// The pad button of every row: the one chosen, or "Automatic".
void show_pads(Controls& c) {
    int count = 0;
    input::game_buttons(&count);
    for (int i = 0; i < count; ++i) {
        std::wstring text;
        if (i == c.waiting_pad) text = ui::wtr("Press a pad button...");
        else if (input::pad_source(i) >= 0) text = ui::wtr(input::pad_source_label(input::pad_source(i)));
        else text = ui::wtr("Automatic");
        SetWindowTextW(c.pads[size_t(i)], text.c_str());
    }
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
        note = ui::wfmt("{0} belongs to the window (Esc quits, F8 settings, F9 screenshot, F10 dump, F11 full screen).", {ui::tr(input::key_name(vk))});
    } else {
        int other = -1;
        for (int i = 0; i < count; ++i)
            if (i != row && input::key_of(buttons[i].mask) == vk) other = i;
        if (other >= 0) {
            note = ui::wfmt("{0} is already {1}. Give that one another key first.", {ui::tr(input::key_name(vk)), ui::tr(buttons[other].label)});
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
            if (id >= ID_PADKEY0 && id < ID_PADKEY0 + int(c->pads.size())) {
                c->waiting = -1;
                c->waiting_pad = id - ID_PADKEY0;
                input::take_pad_press();  // what is held now does not count
                SetWindowTextW(c->note, ui::wtr("Press the button of your pad for this one (Esc: keep the old one; right click: automatic).").c_str());
                show_keys(*c);
                show_pads(*c);
            } else if (id >= ID_KEY0 && id < ID_KEY0 + int(c->keys.size())) {
                c->waiting = id - ID_KEY0;
                c->waiting_pad = -1;
                show_pads(*c);
                SetWindowTextW(c->note, ui::wtr("Press the key for this button (Esc: keep the old one).").c_str());
                show_keys(*c);
            } else if (id == ID_DEFAULTS) {
                int count = 0;
                const input::GameButton* buttons = input::game_buttons(&count);
                for (int i = 0; i < count; ++i) {
                    c->svc->write_setting(std::string("key_") + buttons[i].name, "");
                    c->svc->write_setting(std::string("pad_") + buttons[i].name, "");
                }
                input::load_keys(c->svc->read_setting);
                input::load_pad_buttons(c->svc->read_setting);
                c->waiting = -1;
                c->waiting_pad = -1;
                show_pads(*c);
                SetWindowTextW(c->note, ui::wtr("The default keys are back.").c_str());
                show_keys(*c);
            } else if (id == ID_QUIT) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_TIMER:
            if (c && c->waiting_pad >= 0) {
                const int source = input::take_pad_press();
                if (source >= 0) {
                    int count = 0;
                    const input::GameButton* buttons = input::game_buttons(&count);
                    c->svc->write_setting(std::string("pad_") + buttons[c->waiting_pad].name, input::pad_source_name(source));
                    input::load_pad_buttons(c->svc->read_setting);
                    c->waiting_pad = -1;
                    SetWindowTextW(c->note, L"");
                    show_pads(*c);
                }
            }
            return 0;
        case WM_CTLCOLORSTATIC:
            return plain_static(w);
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            if (c) c->closed = true;
            PostMessageW(nullptr, WM_NULL, 0, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

// --- The pad mapping window: a line of SDL's controller database for a pad it does not know (F5-15) ----------------------

struct PadMapping {
    bool closed = false;
    HWND text = nullptr;
};

LRESULT CALLBACK pad_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    PadMapping* p = reinterpret_cast<PadMapping*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_COMMAND: {
            const int id = LOWORD(w);
            if (id == ID_START && p) {
                const int length = GetWindowTextLengthW(p->text);
                std::wstring wide(size_t(length) + 1, L' ');
                GetWindowTextW(p->text, wide.data(), length + 1);
                wide.resize(size_t(length));
                const std::string text = narrow(wide);
                std::string error;
                const int added = input::save_pad_mappings(text, error);
                if (added < 0) {
                    MessageBoxW(hwnd, ui::wtr(error).c_str(), L"Amagami", MB_OK | MB_ICONWARNING);
                } else {
                    MessageBoxW(hwnd,
                                added == 0 ? ui::wtr("Those mappings were already saved.").c_str() : ui::wtr("Saved. The pad is used the next time the game starts.").c_str(),
                                L"Amagami", MB_OK | MB_ICONINFORMATION);
                    DestroyWindow(hwnd);
                }
            } else if (id == ID_QUIT) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC:
            return plain_static(w);
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (p) p->closed = true;
            PostMessageW(nullptr, WM_NULL, 0, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

}  // namespace

void show_controls(HWND parent, HINSTANCE inst, const Services& svc) {
    register_class(inst, L"AmagamiControls", controls_proc);
    Controls c;
    c.svc = &svc;
    input::load_keys(svc.read_setting);
    input::load_pad_buttons(svc.read_setting);
    const bool own_pads = !input::active();  // the pads are read here to take the button the player presses
    if (own_pads) {
        const char* fake = std::getenv("AMG_FAKE_PAD");  // a virtual pad, to try this without one
        input::init(input::Settings(), fake ? fake : "");
    }
    const Scale s{dpi_for(parent)};
    Fonts fonts;
    fonts.make(s.dpi);
    int count = 0;
    const input::GameButton* buttons = input::game_buttons(&count);
    const int rows = (count + 1) / 2;
    // Two columns of rows: the name, the key, the pad's button. The name is as wide as the longest in this language.
    int name_w = s(100);
    for (int i = 0; i < count; ++i) name_w = std::max(name_w, text_width(fonts.body, ui::wtr(buttons[i].label)) + s(12));
    const int key_w = s(130), pad_w = s(190), gap = s(10), margin = s(24), row_h = s(32), top = s(20);
    const int column = name_w + key_w + gap + pad_w + s(40);
    const int width = 2 * margin + 2 * column - s(40), height = top + rows * row_h + s(110);
    HWND win = make_popup(inst, L"AmagamiControls", ui::wtr("Controls"), parent, width, height, s.dpi);
    SetWindowLongPtrW(win, GWLP_USERDATA, LONG_PTR(&c));
    for (int i = 0; i < count; ++i) {
        const int x = margin + (i / rows) * column, y = top + (i % rows) * row_h;
        make_label(win, ui::wtr(buttons[i].label), x, y + s(5), name_w, s(22), fonts.body);
        c.keys.push_back(make_button(win, L"", ID_KEY0 + i, x + name_w, y, key_w, s(28), fonts.body));
        c.pads.push_back(make_button(win, L"", ID_PADKEY0 + i, x + name_w + key_w + gap, y, pad_w, s(28), fonts.body));
    }
    const int bottom = top + rows * row_h + s(8);
    c.note = make_label(win, ui::wtr("Click the key or the pad button of a row, then press the new one. Right click on a pad button: automatic."), margin,
                        bottom, width - 2 * margin, s(40), fonts.body);
    const std::wstring defaults = ui::wtr("Restore defaults");
    make_button(win, defaults, ID_DEFAULTS, margin, bottom + s(50), std::max(s(150), text_width(fonts.body, defaults) + s(32)), s(30), fonts.body);
    make_button(win, ui::wtr("Close"), ID_QUIT, width - margin - s(110), bottom + s(50), s(110), s(30), fonts.body, true);
    show_keys(c);
    show_pads(c);
    SetTimer(win, 1, 30, nullptr);  // the pad button the player presses while a row waits for one
    EnableWindow(parent, FALSE);
    show(win);
    // Its own message loop: while a row waits, every key goes to it (the dialog handling would take arrows, Tab and Enter).
    MSG m;
    while (!c.closed && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (c.waiting_pad >= 0 && (m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN)) {  // a key while a pad button is awaited
            if (m.wParam == VK_ESCAPE) {
                c.waiting_pad = -1;
                SetWindowTextW(c.note, L"");
                show_pads(c);
            }
            continue;
        }
        if (m.message == WM_RBUTTONUP) {  // a right click on a pad button: back to the automatic one
            for (size_t i = 0; i < c.pads.size(); ++i)
                if (m.hwnd == c.pads[i]) {
                    int n = 0;
                    const input::GameButton* bs = input::game_buttons(&n);
                    svc.write_setting(std::string("pad_") + bs[i].name, "");
                    input::load_pad_buttons(svc.read_setting);
                    c.waiting_pad = -1;
                    show_pads(c);
                }
        }
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
    if (own_pads) input::shutdown();
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    fonts.free();
}

void show_pad_mapping(HWND parent, HINSTANCE inst) {
    register_class(inst, L"AmagamiPad", pad_proc);
    PadMapping p;
    const Scale s{dpi_for(parent)};
    Fonts fonts;
    fonts.make(s.dpi);
    const int width = s(560), height = s(340), margin = s(24);
    HWND win = make_popup(inst, L"AmagamiPad", ui::wtr("Pad mapping"), parent, width, height, s.dpi);
    SetWindowLongPtrW(win, GWLP_USERDATA, LONG_PTR(&p));
    make_label(win,
               ui::wtr("If your pad is not recognised, paste its mapping line here (one per line): the kind the SDL2 Gamepad Tool makes, or a line of the community's gamecontrollerdb.txt. It starts with a long code, then the pad's name, then entries such as a:b0. The line is kept and used the next time the game starts."),
               margin, s(16), width - 2 * margin, s(84), fonts.body);
    p.text = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                           margin, s(106), width - 2 * margin, s(170), win, HMENU(INT_PTR(ID_PAD_TEXT)), inst, nullptr);
    set_font(p.text, fonts.body);
    make_button(win, ui::wtr("Save"), ID_START, width - margin - s(232), height - s(50), s(110), s(30), fonts.body, true);
    make_button(win, ui::wtr("Cancel"), ID_QUIT, width - margin - s(110), height - s(50), s(110), s(30), fonts.body);
    EnableWindow(parent, FALSE);
    show(win);
    SetFocus(p.text);
    pump(win, p.closed);
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    fonts.free();
}

}  // namespace amg::launcher
