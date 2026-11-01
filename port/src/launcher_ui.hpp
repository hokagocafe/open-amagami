// What the launcher's windows share (launcher.cpp, launcher_dialogs.cpp): the colours, the DPI scale and the fonts, and the
// small windows opened from the settings (Controls, Pad mapping). Windows only; see docs/technical/launcher-design.md.
#pragma once

#include <string>

#include <windows.h>

#include "launcher.hpp"

namespace amg::launcher {

const COLORREF BACKGROUND = RGB(247, 246, 244), PANE = RGB(255, 255, 255), INK = RGB(40, 35, 62), MUTED = RGB(93, 88, 112),
               GOOD = RGB(46, 150, 90), WARN = RGB(214, 150, 30), BAD = RGB(200, 60, 70), LINE = RGB(222, 219, 214);

enum {
    ID_START = IDOK, ID_QUIT = IDCANCEL, ID_FOLDER = 11, ID_DELETE = 12, ID_SAVES = 13, ID_CONTROLS = 14, ID_DEFAULTS = 15,
    ID_ENGLISH = 16, ID_SHORTCUT = 17, ID_UPDATE = 18, ID_PAD = 19, ID_PAD_TEXT = 20, ID_FOUND = 21, ID_TABS = 22, ID_CHECKS = 23,
    ID_ROW0 = 100, ID_KEY0 = 300, ID_PADKEY0 = 400
};

// Sizes are written for 96 DPI and scaled to the window's.
struct Scale {
    int dpi = 96;
    int operator()(int v) const { return MulDiv(v, dpi, 96); }
};

// The DPI to lay out for: AMG_LAUNCHER_DPI (development) or the window's (the system's before it exists).
int dpi_for(HWND w);
// Whether AMG_LAUNCHER_DPI fixes the DPI (then WM_DPICHANGED is ignored).
bool fixed_dpi();

// The system's message font at `dpi`, `times` its size, semi-bold or not.
HFONT message_font(int dpi, bool bold, double times = 1.0);

struct Fonts {
    HFONT body = nullptr, bold = nullptr, title = nullptr;
    void make(int dpi);
    void free();
};

// Places a new top-level window: AMG_LAUNCHER_POS=X,Y (development: off screen, without taking the focus) or centred on the
// screen it is on, a third from the top.
void place(HWND w);
// Shows a window placed by place(): activated, unless AMG_LAUNCHER_POS is set.
void show(HWND w);

std::wstring widen(const std::string& text);
std::string narrow(const std::wstring& text);
void set_font(HWND w, HFONT f);
HWND make_button(HWND parent, const std::wstring& text, int id, int x, int y, int w, int h, HFONT font, bool primary = false);
HWND make_label(HWND parent, const std::wstring& text, int x, int y, int w, int h, HFONT font);
// The width of `text` in `font`, in pixels.
int text_width(HFONT font, const std::wstring& text);

// Runs messages until `closed` is set: a nested window can end without ending the program.
void pump(HWND window, const bool& closed);

// The windows opened from the settings (launcher_dialogs.cpp). They disable `parent` while open.
void show_controls(HWND parent, HINSTANCE inst, const Services& svc);
void show_pad_mapping(HWND parent, HINSTANCE inst);

}  // namespace amg::launcher
