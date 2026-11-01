#ifndef NOMINMAX
#define NOMINMAX
#endif
// The launcher window and the settings over the game (F8). Layout: docs/technical/launcher-design.md. The small windows
// opened from the settings and the shared helpers are in launcher_dialogs.cpp.
#include "launcher.hpp"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "data_check.hpp"
#include "datapath.hpp"
#include "launcher_ui.hpp"
#include "paths.hpp"
#include "port_settings.hpp"
#include "shortcut.hpp"
#include "ui_text.hpp"
#include "update_check.hpp"
#include "updater.hpp"

namespace amg::launcher {

namespace {

constexpr UINT WM_CHECKED = WM_APP + 1;
constexpr UINT WM_UPDATE = WM_APP + 2;       // lParam: a new std::string with the newer release's tag
constexpr UINT WM_FOUND = WM_APP + 3;        // lParam: a new std::vector<std::string> of the copies of the game found on the machine
constexpr UINT WM_UPDATE_STEP = WM_APP + 4;  // lParam: a new std::string, what the update is doing now
constexpr UINT WM_UPDATED = WM_APP + 5;      // lParam: a new std::string, "" when the update is in place, else why not
constexpr UINT WM_REBUILD = WM_APP + 6;      // the language or the DPI changed: the controls are made again

// `base_english`: the game folder's own data is the English fan translation (the Vita3K-style patched copy).
std::string language_name(const std::string& code, bool base_english) {
    if (code.empty()) return ui::tr(base_english ? "This copy's own (English)" : "Original (Japanese)");
    if (code == "en") return ui::tr("English (fan patch)");
    return code;
}

// A row of the settings: a label and a drop-down list.
struct Row {
    std::string key, label;
    std::vector<std::string> values, names;  // as stored, as shown (English: translated when shown)
    int index = 0;
    HWND combo = nullptr;
};

// A tab: its title and the controls shown while it is selected.
struct Page {
    std::wstring title;
    std::vector<HWND> controls;
};

// Both windows: the launcher (with the header and the Game files tab) and the settings over the game (`in_game`).
struct Window {
    HINSTANCE inst = nullptr;
    const Services* svc = nullptr;
    bool in_game = false;
    bool closed = false;
    HWND win = nullptr;
    Scale s;
    Fonts fonts;
    HWND tabs = nullptr;
    int tab = 0;
    std::vector<Page> pages;
    std::vector<Row> rows;
    int header_h = 0;  // the header's height (0 over the game)
    int line_h = 16;   // the height of a line of the body font
    COLORREF pane = PANE;  // the colour of the tab's pane as the theme draws it
    // The launcher's own state.
    std::string root, result;
    std::vector<Check> checks;
    bool checking = false;
    int generation = 0;
    HWND start = nullptr, update = nullptr, found = nullptr, fix = nullptr, checks_view = nullptr;
    HICON icon = nullptr;
    std::string update_tag;
    std::string update_text;  // what the update button says while it works ("" for the usual text)
    bool updating = false;
    std::vector<std::string> copies;  // usable copies of the game found on this machine (offered when the folder is not usable)
    bool searched = false;
    int scroll = 0;  // of the list of checks
    int checks_height = -1;  // the list's height when the scroll bar was last set
};

struct Answer {
    int generation;
    std::vector<Check> checks;
};

bool not_usable(const Window& w) { return !w.checking && !usable(w.checks); }

// --- The rows -----------------------------------------------------------------------------------------------------------

Row row_for(const Window& w, const std::string& key) {
    Row r;
    r.key = key;
    if (key == "lang") {  // the game's language: the layers found in the game's folder
        r.label = "Language";
        r.values.push_back("");
        std::error_code ec;
        const std::filesystem::path layers = std::filesystem::u8path(w.root) / "lang";
        if (!w.root.empty() && std::filesystem::is_directory(layers, ec))
            for (const auto& e : std::filesystem::directory_iterator(layers, ec))
                if (e.is_directory(ec)) r.values.push_back(e.path().filename().string());
        const bool base_english = !w.root.empty() && known_label(data_file(w.root, "AMAGAMI.IMG"), "AMAGAMI.IMG").find("English") != std::string::npos;
        const std::string current = w.svc->read_setting("lang");
        for (size_t i = 0; i < r.values.size(); ++i) {
            r.names.push_back(language_name(r.values[i], base_english));
            if (r.values[i] == current) r.index = int(i);
        }
    } else if (key == "ui_lang") {  // the language of these windows
        r.label = "Launcher language";
        r.values = {"", "en", "es", "ja"};
        r.names = {"Automatic (Windows)", "English", "Español", "日本語"};
        const std::string current = w.svc->read_setting("ui_lang");
        for (size_t i = 0; i < r.values.size(); ++i)
            if (r.values[i] == current) r.index = int(i);
    } else {
        for (const port_settings::RowInfo& info : port_settings::all_rows())
            if (info.key == key) {
                r.label = info.label;
                r.values = info.values;
                r.names = info.names;
                r.index = info.index;
            }
    }
    return r;
}

// The keys of the settings tabs. A row of port_settings.cpp that none names goes to General.
const std::vector<std::string> GENERAL = {"ui_lang", "launcher", "updates", "splash"};
const std::vector<std::string> DISPLAY = {"window", "uiscale", "render", "filter", "aspect", "brightness", "gamma", "vsync", "fps"};
const std::vector<std::string> CONTROLS = {"glyphs", "confirm", "fallback"};

std::vector<std::string> general_keys() {
    std::vector<std::string> keys = GENERAL;
    for (const port_settings::RowInfo& info : port_settings::all_rows()) {
        auto named = [&](const std::vector<std::string>& v) { return std::find(v.begin(), v.end(), info.key) != v.end(); };
        if (!named(GENERAL) && !named(DISPLAY) && !named(CONTROLS)) keys.push_back(info.key);
    }
    return keys;
}

// --- Building the window ------------------------------------------------------------------------------------------------

struct Area {
    int x, y, w, h;
};

HWND add(Window& w, Page& page, HWND control) {
    page.controls.push_back(control);
    return control;
}

HWND page_button(Window& w, Page& page, const std::wstring& text, int id, int x, int y, int width) {
    return add(w, page, make_button(w.win, text, id, x, y, width, w.s(30), w.fonts.body));
}

// The rows `keys` in `columns` columns of `area`, from its top; returns the y below the last row.
int add_rows(Window& w, Page& page, const std::vector<std::string>& keys, Area area, int columns) {
    const Scale& s = w.s;
    const int gap = s(24), row_h = s(36);
    const int col_w = (area.w - gap * (columns - 1)) / columns;
    std::vector<Row> made;
    for (const std::string& k : keys) made.push_back(row_for(w, k));
    // The label column is as wide as the longest label of the page; the lists as wide as their longest entry.
    int label_w = s(80);
    for (const Row& r : made) label_w = std::max(label_w, text_width(w.fonts.body, ui::wtr(r.label)) + s(16));
    label_w = std::min(label_w, col_w * 11 / 20);
    int names_w = 0;  // every list of the page as wide as the widest, so the column is even
    for (const Row& r : made)
        for (const std::string& n : r.names) names_w = std::max(names_w, text_width(w.fonts.body, ui::wtr(n)));
    const int per_column = (int(made.size()) + columns - 1) / columns;
    int bottom = area.y;
    for (size_t i = 0; i < made.size(); ++i) {
        Row& r = made[i];
        const int column = int(i) / per_column, place = int(i) % per_column;
        const int x = area.x + column * (col_w + gap), y = area.y + place * row_h;
        const int combo_w = std::min(col_w - label_w, std::max(s(120), names_w + s(40)));
        const int id = ID_ROW0 + int(w.rows.size());
        HWND label = add(w, page, make_label(w.win, ui::wtr(r.label), x, y, label_w - s(8), s(26), w.fonts.body));
        SetWindowLongPtrW(label, GWL_STYLE, GetWindowLongPtrW(label, GWL_STYLE) | SS_CENTERIMAGE | SS_ENDELLIPSIS);
        r.combo = add(w, page, CreateWindowW(L"COMBOBOX", ui::wtr(r.label).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                             x + label_w, y, combo_w, s(300), w.win, HMENU(INT_PTR(id)), w.inst, nullptr));
        set_font(r.combo, w.fonts.body);
        for (const std::string& name : r.names) SendMessageW(r.combo, CB_ADDSTRING, 0, LPARAM(ui::wtr(name).c_str()));
        SendMessageW(r.combo, CB_SETCURSEL, WPARAM(r.index), 0);
        SendMessageW(r.combo, CB_SETDROPPEDWIDTH, WPARAM(std::max(combo_w, names_w + s(24))), 0);
        bottom = std::max(bottom, y + row_h);
        w.rows.push_back(std::move(r));
    }
    return bottom;
}

int button_width(const Window& w, const std::vector<std::wstring>& texts, int least) {
    int width = least;
    for (const std::wstring& t : texts) width = std::max(width, text_width(w.fonts.body, t) + w.s(28));
    return width;
}

void show_page(Window& w, int tab) {
    w.tab = std::clamp(tab, 0, int(w.pages.size()) - 1);
    if (w.tabs) TabCtrl_SetCurSel(w.tabs, w.tab);
    for (size_t p = 0; p < w.pages.size(); ++p)
        for (HWND c : w.pages[p].controls) ShowWindow(c, int(p) == w.tab ? SW_SHOW : SW_HIDE);
}

int game_files_tab(const Window& w) { return w.in_game ? -1 : 0; }

// The header's buttons of the moment, left to right under the status line.
void layout_header(Window& w) {
    if (w.in_game || !w.start) return;
    const Scale& s = w.s;
    const bool bad = not_usable(w);
    ShowWindow(w.fix, bad ? SW_SHOW : SW_HIDE);
    ShowWindow(w.found, bad && !w.copies.empty() ? SW_SHOW : SW_HIDE);
    ShowWindow(w.update, w.update_tag.empty() ? SW_HIDE : SW_SHOW);
    if (!w.update_tag.empty())
        SetWindowTextW(w.update, w.update_text.empty() ? ui::wfmt("Get the new version, {0}", {w.update_tag}).c_str() : widen(w.update_text).c_str());
    RECT start;
    GetWindowRect(w.start, &start);
    MapWindowPoints(nullptr, w.win, reinterpret_cast<POINT*>(&start), 2);
    int x = s(84);
    const int y = s(52) + 2 * w.line_h + s(10), limit = start.left - s(16);
    for (HWND b : {w.found, w.fix, w.update}) {
        if (!IsWindowVisible(b)) continue;
        const int length = GetWindowTextLengthW(b);
        std::wstring text(size_t(length) + 1, L' ');
        GetWindowTextW(b, text.data(), length + 1);
        text.resize(size_t(length));
        const int width = std::min(limit - x, text_width(w.fonts.bold, text) + s(32));
        SetWindowPos(b, nullptr, x, y, std::max(s(60), width), s(30), SWP_NOZORDER | SWP_NOACTIVATE);
        x += width + s(10);
    }
    EnableWindow(w.start, !w.checking && !w.updating && usable(w.checks));
    InvalidateRect(w.win, nullptr, FALSE);
}

void load_icon(Window& w) {
    if (w.icon) DestroyIcon(w.icon);
    w.icon = w.root.empty() ? nullptr : shortcut::load_game_icon(w.root, w.s(48));
#ifdef AMG_HAS_ICON
    if (!w.icon) w.icon = HICON(LoadImageW(w.inst, MAKEINTRESOURCEW(1), IMAGE_ICON, w.s(48), w.s(48), 0));
#endif
}

// The colour the theme gives the tab's pane (white, or a light grey on Windows 11), read from the control drawn off screen,
// so the labels and the list of checks on it take the same colour.
COLORREF pane_colour(HWND tabs) {
    RECT r;
    GetClientRect(tabs, &r);
    if (r.right < 8 || r.bottom < 8) return PANE;
    HDC screen = GetDC(tabs);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, r.right, r.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    SendMessageW(tabs, WM_PRINTCLIENT, WPARAM(mem), PRF_CLIENT | PRF_ERASEBKGND);
    const COLORREF c = GetPixel(mem, r.right / 2, r.bottom - r.bottom / 4);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(tabs, screen);
    return c == CLR_INVALID ? PANE : c;
}

void build(Window& w) {
    // Everything is made again: the first time, after the language changed and after the DPI changed.
    for (HWND c = GetWindow(w.win, GW_CHILD); c; c = GetWindow(w.win, GW_CHILD)) DestroyWindow(c);
    w.pages.clear();
    w.rows.clear();
    w.start = w.update = w.found = w.fix = w.checks_view = w.tabs = nullptr;
    w.checks_height = -1;
    w.fonts.make(w.s.dpi);
    const Scale& s = w.s;
    RECT client;
    GetClientRect(w.win, &client);
    const int W = client.right, H = client.bottom, margin = s(12);
    {
        HDC dc = GetDC(w.win);
        HGDIOBJ old = SelectObject(dc, w.fonts.body);
        TEXTMETRICW tm = {};
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, old);
        ReleaseDC(w.win, dc);
        w.line_h = tm.tmHeight;
    }
    // The header: the title, two lines of status, a row of buttons.
    w.header_h = w.in_game ? 0 : s(52) + 2 * w.line_h + s(10) + s(30) + s(14);

    if (!w.in_game) {
        SetWindowTextW(w.win, L"Amagami");
        const std::wstring start_text = ui::wtr("Start game");
        const int start_w = std::max(s(160), text_width(w.fonts.bold, start_text) + s(40));
        w.found = make_button(w.win, L"", ID_FOUND, 0, 0, s(10), s(30), w.fonts.bold);
        w.fix = make_button(w.win, ui::wtr("Choose game folder..."), ID_FOLDER, 0, 0, s(10), s(30), w.fonts.body);
        w.update = make_button(w.win, L"", ID_UPDATE, 0, 0, s(10), s(30), w.fonts.bold);
        w.start = make_button(w.win, start_text, ID_START, W - s(24) - start_w, s(48), start_w, s(48), w.fonts.bold, true);
        if (!w.copies.empty()) {
            // "Use the copy found in your Vita3K" or in the folder's own name, not the whole path (it does not fit)
            const std::string& found = w.copies.front();
            std::string label = found;
            const size_t cut = found.find_last_of("/\\");
            if (cut != std::string::npos && cut + 1 < found.size()) label = found.substr(cut + 1);
            SetWindowTextW(w.found, found.find("Vita3K") != std::string::npos ? ui::wtr("Use the copy found in your Vita3K").c_str()
                                                                               : ui::wfmt("Use the copy found in {0}", {label}).c_str());
        }
        load_icon(w);
    } else {
        SetWindowTextW(w.win, ui::wtr("Settings").c_str());
    }

    // The pages. Each control is a child of the window, shown while its tab is selected; the tab control is below them.
    const int tabs_y = w.in_game ? margin : w.header_h + s(4);
    const int tabs_bottom = w.in_game ? H - s(56) : H - margin;
    RECT pane = {margin, tabs_y, W - margin, tabs_bottom};
    w.tabs = CreateWindowW(WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, pane.left, pane.top, pane.right - pane.left,
                           pane.bottom - pane.top, w.win, HMENU(INT_PTR(ID_TABS)), w.inst, nullptr);
    set_font(w.tabs, w.fonts.body);
    SendMessageW(w.tabs, TCM_SETPADDING, 0, MAKELPARAM(s(14), s(5)));
    std::vector<std::wstring> titles;
    if (!w.in_game) titles.push_back(ui::wtr("Game files"));
    titles.push_back(ui::wtr("General"));
    titles.push_back(ui::wtr("Display"));
    titles.push_back(ui::wtr("Controls"));
    for (size_t i = 0; i < titles.size(); ++i) {
        TCITEMW item = {};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(titles[i].c_str());
        SendMessageW(w.tabs, TCM_INSERTITEMW, WPARAM(i), LPARAM(&item));
        w.pages.push_back(Page{titles[i], {}});
    }
    w.pane = pane_colour(w.tabs);
    TabCtrl_AdjustRect(w.tabs, FALSE, &pane);
    const Area area = {pane.left + s(12), pane.top + s(12), (pane.right - pane.left) - s(24), (pane.bottom - pane.top) - s(24)};
    const int half = (area.w - s(24)) / 2;
    int p = 0;

    if (!w.in_game) {  // Game files: the list of checks at the left, the language and the folder's buttons at the right
        Page& page = w.pages[size_t(p++)];
        const std::vector<std::wstring> texts = {ui::wtr("Choose game folder..."), ui::wtr("Add English patch..."), ui::wtr("Open saves folder"),
                                                 ui::wtr("Delete copy of the game...")};
        const int right_w = std::min(area.w * 2 / 5, button_width(w, texts, s(200)));
        const int rx = area.x + area.w - right_w;
        w.checks_view = add(w, page, CreateWindowW(L"AmagamiChecks", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL, area.x, area.y, rx - s(16) - area.x, area.h,
                                                   w.win, HMENU(INT_PTR(ID_CHECKS)), w.inst, nullptr));
        SetWindowLongPtrW(w.checks_view, GWLP_USERDATA, LONG_PTR(&w));
        add(w, page, make_label(w.win, ui::wtr("Language"), rx, area.y, right_w, s(20), w.fonts.body));
        Row lang = row_for(w, "lang");
        lang.combo = add(w, page, CreateWindowW(L"COMBOBOX", ui::wtr("Language").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                                rx, area.y + s(22), right_w, s(300), w.win, HMENU(INT_PTR(ID_ROW0 + int(w.rows.size()))), w.inst, nullptr));
        set_font(lang.combo, w.fonts.body);
        for (const std::string& name : lang.names) SendMessageW(lang.combo, CB_ADDSTRING, 0, LPARAM(ui::wtr(name).c_str()));
        SendMessageW(lang.combo, CB_SETCURSEL, WPARAM(lang.index), 0);
        w.rows.push_back(std::move(lang));
        const int ids[] = {ID_FOLDER, ID_ENGLISH, ID_SAVES, ID_DELETE};
        int y = area.y + s(70);
        for (size_t i = 0; i < 4; ++i, y += s(38)) {
            page_button(w, page, texts[i], ids[i], rx, y, right_w);
            if (i == 1) y += s(12);  // the folder's own buttons, then the saves and the copy
        }
    }
    {  // General
        Page& page = w.pages[size_t(p++)];
        const int bottom = add_rows(w, page, general_keys(), {area.x, area.y, half, area.h}, 1);
        const std::wstring text = w.in_game ? ui::wtr("Open saves folder") : ui::wtr("Desktop shortcut");
        page_button(w, page, text, w.in_game ? ID_SAVES : ID_SHORTCUT, area.x, bottom + s(12), button_width(w, {text}, s(160)));
    }
    {  // Display
        Page& page = w.pages[size_t(p++)];
        add_rows(w, page, DISPLAY, area, 2);
    }
    {  // Controls: the rows at the left, the two windows at the right with a line on what each is for
        Page& page = w.pages[size_t(p++)];
        add_rows(w, page, CONTROLS, {area.x, area.y, half, area.h}, 1);
        const int rx = area.x + half + s(24);
        const std::vector<std::wstring> texts = {ui::wtr("Controls..."), ui::wtr("Pad mapping...")};
        const int bw = button_width(w, texts, s(160));
        page_button(w, page, texts[0], ID_CONTROLS, rx, area.y, bw);
        add(w, page, make_label(w.win, ui::wtr("Every key and every pad button of the game."), rx, area.y + s(34), half, s(40), w.fonts.body));
        page_button(w, page, texts[1], ID_PAD, rx, area.y + s(84), bw);
        add(w, page, make_label(w.win, ui::wtr("For a pad the program does not recognise."), rx, area.y + s(118), half, s(40), w.fonts.body));
    }
    if (w.in_game) make_button(w.win, ui::wtr("Close"), ID_QUIT, W - margin - s(110), H - s(44), s(110), s(30), w.fonts.body, true);
    SetWindowPos(w.tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    show_page(w, w.tab);
    layout_header(w);
    InvalidateRect(w.win, nullptr, TRUE);
}

// --- The checks -----------------------------------------------------------------------------------------------------------

void recheck(Window& w) {
    w.checking = true;
    w.checks.clear();
    w.scroll = 0;
    const int generation = ++w.generation;
    const std::string root = w.root;
    HWND hwnd = w.win;
    layout_header(w);
    if (w.checks_view) InvalidateRect(w.checks_view, nullptr, TRUE);
    std::thread([hwnd, root, generation] {
        std::vector<Check> checks;
        try {
            checks = check_data(root);
        } catch (const std::exception& e) {  // a thread that throws would end the whole program
            checks = {{Check::Fail, "Game folder", ui::fmt("This folder could not be checked ({0}). Choose another folder, or check that you can read it.", {e.what()})}};
        } catch (...) {
            checks = {{Check::Fail, "Game folder", ui::fmt("This folder could not be checked ({0}). Choose another folder, or check that you can read it.", {"?"})}};
        }
        auto* a = new Answer{generation, std::move(checks)};
        if (!PostMessageW(hwnd, WM_CHECKED, 0, LPARAM(a))) delete a;
    }).detach();
}

// A round mark with a tick, a cross or an exclamation mark, drawn with lines (the fonts have no check mark that is the
// same everywhere). `status` -1: grey, still checking.
void draw_mark(HDC dc, int x, int y, int size, int status, int pen_w) {
    const COLORREF colour = status < 0 ? RGB(170, 166, 180) : status == Check::Ok ? GOOD : status == Check::Warn ? WARN : BAD;
    HBRUSH brush = CreateSolidBrush(colour);
    HPEN none = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, none);
    Ellipse(dc, x, y, x + size, y + size);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(none);
    DeleteObject(brush);
    if (status < 0) return;
    HPEN white = CreatePen(PS_SOLID, pen_w, RGB(255, 255, 255));
    HGDIOBJ before = SelectObject(dc, white);
    auto at = [&](int px, int py) { return POINT{x + MulDiv(px, size, 22), y + MulDiv(py, size, 22)}; };
    auto line = [&](int x0, int y0, int x1, int y1) {
        const POINT a = at(x0, y0), b = at(x1, y1);
        MoveToEx(dc, a.x, a.y, nullptr);
        LineTo(dc, b.x, b.y);
    };
    if (status == Check::Ok) {
        line(6, 11, 10, 15);
        line(10, 15, 16, 7);
    } else if (status == Check::Fail) {
        line(7, 7, 15, 15);
        line(15, 7, 7, 15);
    } else {
        line(11, 5, 11, 13);
        line(11, 16, 11, 17);
    }
    SelectObject(dc, before);
    DeleteObject(white);
}

// The list of checks: the folder, then each check with its mark, its name and its detail. Returns the height it needs.
int paint_checks(Window& w, HDC dc, const RECT& box, int offset) {
    const Scale& s = w.s;
    SetBkMode(dc, TRANSPARENT);
    int y = box.top - offset;
    SelectObject(dc, w.fonts.body);
    SetTextColor(dc, MUTED);
    {
        const std::wstring folder = w.root.empty() ? ui::wtr("No folder chosen yet.") : ui::wfmt("Folder: {0}", {w.root});
        RECT r = {box.left, y, box.right, y + s(20)};
        DrawTextW(dc, folder.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        y += s(28);
    }
    if (w.checking) {
        RECT r = {box.left, y, box.right, y + s(20)};
        DrawTextW(dc, ui::wtr("Checking your game folder...").c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        y += s(24);
    }
    const int mark = s(20), indent = mark + s(10);
    for (const Check& c : w.checks) {
        draw_mark(dc, box.left, y + s(1), mark, int(c.status), std::max(1, s(2)));
        SelectObject(dc, w.fonts.bold);
        SetTextColor(dc, INK);
        RECT what = {box.left + indent, y, box.right, y + s(22)};
        DrawTextW(dc, ui::wtr(c.what).c_str(), -1, &what, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, w.fonts.body);
        SetTextColor(dc, c.status == Check::Fail ? INK : MUTED);
        const std::wstring detail = widen(c.detail);
        RECT text = {box.left + indent, y + s(21), box.right, y + s(21) + s(400)};
        DrawTextW(dc, detail.c_str(), -1, &text, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        DrawTextW(dc, detail.c_str(), -1, &text, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        y = text.bottom + s(12);
    }
    if (!w.checking && !usable(w.checks)) {  // what can always be done
        SetTextColor(dc, MUTED);
        RECT hint = {box.left, y, box.right, y + s(200)};
        const std::wstring text = ui::wtr("Choose your own decrypted PS Vita copy, or drop its folder here.");
        DrawTextW(dc, text.c_str(), -1, &hint, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        DrawTextW(dc, text.c_str(), -1, &hint, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        y = hint.bottom + s(8);
    }
    return y + offset - box.top;
}

void update_check_scroll(Window& w, int content) {
    RECT r;
    GetClientRect(w.checks_view, &r);
    const int page = r.bottom;
    w.scroll = std::clamp(w.scroll, 0, std::max(0, content - page));
    SCROLLINFO si = {sizeof si};
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = std::max(0, content - 1);
    si.nPage = UINT(page);
    si.nPos = w.scroll;
    SetScrollInfo(w.checks_view, SB_VERT, &si, TRUE);
}

LRESULT CALLBACK checks_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Window* w = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client;
            GetClientRect(hwnd, &client);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, client.right), std::max(1L, client.bottom));
            HGDIOBJ old = SelectObject(mem, bmp);
            HBRUSH brush = CreateSolidBrush(w ? w->pane : PANE);
            FillRect(mem, &client, brush);
            DeleteObject(brush);
            if (w) {
                RECT box = {0, 0, client.right - w->s(4), client.bottom};
                const int content = paint_checks(*w, mem, box, w->scroll);
                if (content != w->checks_height) {
                    w->checks_height = content;
                    update_check_scroll(*w, content);
                }
            }
            BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEWHEEL:
        case WM_VSCROLL:
            if (w) {
                SCROLLINFO si = {sizeof si};
                si.fMask = SIF_ALL;
                GetScrollInfo(hwnd, SB_VERT, &si);
                int pos = w->scroll;
                if (msg == WM_MOUSEWHEEL) pos -= GET_WHEEL_DELTA_WPARAM(wp) * w->s(40) / WHEEL_DELTA;
                else switch (LOWORD(wp)) {
                        case SB_LINEUP: pos -= w->s(20); break;
                        case SB_LINEDOWN: pos += w->s(20); break;
                        case SB_PAGEUP: pos -= int(si.nPage); break;
                        case SB_PAGEDOWN: pos += int(si.nPage); break;
                        case SB_THUMBTRACK:
                        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
                    }
                w->scroll = std::clamp(pos, 0, std::max(0, si.nMax + 1 - int(si.nPage)));
                SetScrollPos(hwnd, SB_VERT, w->scroll, TRUE);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The checks as text, for screen readers (the list is drawn by hand).
void name_checks(Window& w) {
    if (!w.checks_view) return;
    std::wstring all;
    for (const Check& c : w.checks) all += ui::wtr(c.what) + L": " + widen(c.detail) + L"\n";
    SetWindowTextW(w.checks_view, all.c_str());
    InvalidateRect(w.checks_view, nullptr, TRUE);
}

// --- The header ---------------------------------------------------------------------------------------------------------

void paint_header(Window& w, HDC dc, const RECT& client) {
    const Scale& s = w.s;
    RECT header = {0, 0, client.right, w.header_h};
    HBRUSH brush = CreateSolidBrush(BACKGROUND);
    FillRect(dc, &client, brush);
    DeleteObject(brush);
    if (w.in_game) return;
    SetBkMode(dc, TRANSPARENT);
    if (w.icon) DrawIconEx(dc, s(20), s(18), w.icon, s(48), s(48), 0, nullptr, DI_NORMAL);
    RECT start = {client.right, 0, client.right, 0};
    if (w.start) {
        GetWindowRect(w.start, &start);
        MapWindowPoints(nullptr, w.win, reinterpret_cast<POINT*>(&start), 2);
    }
    SelectObject(dc, w.fonts.title);
    SetTextColor(dc, INK);
    RECT r = {s(84), s(10), start.left - s(16), s(50)};
    DrawTextW(dc, L"Amagami", -1, &r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, w.fonts.body);
    SetTextColor(dc, MUTED);
    r = {s(84), s(18), client.right - s(24), s(40)};
    DrawTextW(dc, (L"open-amagami " + widen(update_check::current())).c_str(), -1, &r, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
    // The status: a mark and one or two lines.
    int status = Check::Ok;
    std::wstring text;
    const Check* failed = nullptr;
    for (const Check& c : w.checks)
        if (c.status == Check::Fail && !failed) failed = &c;
    bool warned = false;
    for (const Check& c : w.checks) warned = warned || c.status == Check::Warn;
    if (w.checking) {
        status = -1;
        text = ui::wtr("Checking your game folder...");
    } else if (w.checks.empty()) {
        status = Check::Warn;
        text = ui::wtr("Choose the folder of your own copy of the game to begin.");
    } else if (failed) {
        status = Check::Fail;
        text = ui::wtr(failed->what) + L": " + widen(failed->detail);
    } else {
        status = warned ? Check::Warn : Check::Ok;
        text = ui::wtr("Your game is ready. In the game, F8 opens the options.");
    }
    draw_mark(dc, s(84), s(54) + (w.line_h - s(16)) / 2, s(16), status, std::max(1, s(2)));
    SelectObject(dc, failed ? w.fonts.bold : w.fonts.body);
    SetTextColor(dc, INK);
    RECT line = {s(108), s(52) + s(2), start.left - s(16), s(52) + s(2) + 2 * w.line_h + 1};
    DrawTextW(dc, text.c_str(), -1, &line, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL | DT_NOPREFIX);
    HPEN pen = CreatePen(PS_SOLID, 1, LINE);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, 0, header.bottom - 1, nullptr);
    LineTo(dc, client.right, header.bottom - 1);
    SelectObject(dc, old);
    DeleteObject(pen);
}

// --- The window's messages ----------------------------------------------------------------------------------------------

void choose(Window& w, const std::string& chosen) {
    if (chosen.empty()) return;
    w.root = chosen;
    w.copies.clear();
    load_icon(w);
    PostMessageW(w.win, WM_REBUILD, 0, 0);  // the Language row lists the layers of the new folder
    recheck(w);
}

void command(Window& w, int id, int note) {
    HWND hwnd = w.win;
    if (id >= ID_ROW0 && id < ID_ROW0 + 100) {
        if (note != CBN_SELCHANGE) return;
        Row& row = w.rows[size_t(id - ID_ROW0)];
        const int pick = int(SendMessageW(row.combo, CB_GETCURSEL, 0, 0));
        if (pick < 0 || size_t(pick) >= row.values.size()) return;
        const std::string& value = row.values[size_t(pick)];
        if (row.key == "lang") {
            w.svc->write_setting("lang", value);
        } else if (row.key == "ui_lang") {
            w.svc->write_setting("ui_lang", value);
            ui::init(value);
            PostMessageW(hwnd, WM_REBUILD, 0, 0);  // every text in the new language at once
        } else {
            port_settings::set_value(row.key, value);
        }
        return;
    }
    switch (id) {
        case ID_SAVES: {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::u8path(paths::save_dir()), ec);
            ShellExecuteA(nullptr, "open", paths::save_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case ID_CONTROLS:
            show_controls(hwnd, w.inst, *w.svc);
            break;
        case ID_PAD:
            show_pad_mapping(hwnd, w.inst);
            break;
        case ID_SHORTCUT: {
            const std::string failed = shortcut::create(w.root);
            MessageBoxW(hwnd, failed.empty() ? ui::wtr("Amagami is now on the desktop and in the Start menu.").c_str() : widen(failed).c_str(), L"Amagami",
                        MB_OK | (failed.empty() ? MB_ICONINFORMATION : MB_ICONWARNING));
            break;
        }
        case ID_DELETE: {
            const std::string copy = paths::game_dir();
            std::error_code ec;
            if (copy.empty() || !std::filesystem::exists(copy, ec)) {
                MessageBoxW(hwnd, ui::wtr("There is no copy of the game in the program's own folder.").c_str(), L"Amagami", MB_OK | MB_ICONINFORMATION);
            } else if (MessageBoxW(hwnd, ui::wfmt("Delete the copy of the game in:\n{0}\n\nYour saves and settings are kept. You can copy the game again from your own folder.", {copy}).c_str(),
                                   L"Amagami", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                std::filesystem::remove_all(std::filesystem::u8path(copy), ec);
                if (w.svc->read_setting("root") == copy) w.svc->write_setting("root", "");
                w.root = w.svc->read_setting("root");
                w.searched = false;  // look for a copy again
                load_icon(w);
                PostMessageW(hwnd, WM_REBUILD, 0, 0);
                recheck(w);
            }
            break;
        }
        case ID_ENGLISH:
            if (w.svc->add_english && w.svc->add_english(w.root)) {
                PostMessageW(hwnd, WM_REBUILD, 0, 0);  // the Language row lists the new layer
                recheck(w);                            // and the Language check says so
            }
            break;
        case ID_FOLDER:
            choose(w, w.svc->choose_folder());
            break;
        case ID_FOUND:
            if (!w.copies.empty() && w.svc->use_copy) choose(w, w.svc->use_copy(w.copies.front()));
            break;
        case ID_UPDATE: {
            if (w.updating || w.update_tag.empty()) break;
            const std::wstring ask = ui::wfmt("Download and install {0} now? The program restarts when it is done.", {w.update_tag});
            if (MessageBoxW(hwnd, ask.c_str(), L"Amagami", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                w.updating = true;
                w.update_text = ui::tr("Downloading...");
                layout_header(w);
                std::thread([hwnd, tag = w.update_tag] {
                    const std::string result = updater::install(tag, [hwnd](const std::string& step) {
                        PostMessageW(hwnd, WM_UPDATE_STEP, 0, LPARAM(new std::string(step)));
                    });
                    PostMessageW(hwnd, WM_UPDATED, 0, LPARAM(new std::string(result)));
                }).detach();
            }
            break;
        }
        case ID_START:
            if (w.in_game) {
                DestroyWindow(hwnd);  // Enter over the game: Close
            } else if (w.start && IsWindowEnabled(w.start)) {
                w.result = w.root;
                DestroyWindow(hwnd);
            }
            break;
        case ID_QUIT:
            DestroyWindow(hwnd);
            break;
    }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Window* w = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client;
            GetClientRect(hwnd, &client);
            paint_header(*w, dc, client);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            // On the tab's pane the controls take the pane's colour, in the header the window's.
            static HBRUSH back = CreateSolidBrush(BACKGROUND);
            static HBRUSH pane = nullptr;
            static COLORREF pane_colour = CLR_INVALID;
            if (pane_colour != w->pane) {
                if (pane) DeleteObject(pane);
                pane = CreateSolidBrush(w->pane);
                pane_colour = w->pane;
            }
            RECT r;
            GetWindowRect(HWND(lp), &r);
            MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&r), 2);
            RECT tabs = {};
            if (w->tabs) {
                GetWindowRect(w->tabs, &tabs);
                MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&tabs), 2);
            }
            const bool on_pane = r.top >= tabs.top && r.bottom <= tabs.bottom;
            SetBkColor(HDC(wp), on_pane ? w->pane : BACKGROUND);
            SetTextColor(HDC(wp), INK);
            return LRESULT(on_pane ? pane : back);
        }
        case WM_NOTIFY: {
            const NMHDR* n = reinterpret_cast<const NMHDR*>(lp);
            if (n->hwndFrom == w->tabs && n->code == TCN_SELCHANGE) show_page(*w, TabCtrl_GetCurSel(w->tabs));
            return 0;
        }
        case WM_REBUILD:
            build(*w);
            name_checks(*w);
            return 0;
        case WM_DPICHANGED:
            if (!fixed_dpi()) {
                w->s.dpi = HIWORD(wp);
                const RECT* r = reinterpret_cast<const RECT*>(lp);
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                load_icon(*w);
                build(*w);
                name_checks(*w);
            }
            return 0;
        case WM_UPDATE: {
            std::unique_ptr<std::string> tag(reinterpret_cast<std::string*>(lp));
            w->update_tag = *tag;
            layout_header(*w);
            return 0;
        }
        case WM_UPDATE_STEP: {
            std::unique_ptr<std::string> step(reinterpret_cast<std::string*>(lp));
            w->update_text = *step;
            layout_header(*w);
            return 0;
        }
        case WM_UPDATED: {
            std::unique_ptr<std::string> result(reinterpret_cast<std::string*>(lp));
            w->updating = false;
            w->update_text.clear();
            if (result->empty()) {  // the new files are in place: start the new program and leave this one
                if (!updater::restart())
                    MessageBoxW(hwnd, ui::wtr("The update is installed. Start the program again to use it.").c_str(), L"Amagami", MB_OK | MB_ICONINFORMATION);
                DestroyWindow(hwnd);
            } else {
                layout_header(*w);
                const std::wstring text = widen(*result) + L"\n\n" + ui::wtr("Open the download page instead?");
                if (MessageBoxW(hwnd, text.c_str(), L"Amagami", MB_YESNO | MB_ICONWARNING) == IDYES)
                    ShellExecuteW(nullptr, L"open", update_check::PAGE, nullptr, nullptr, SW_SHOWNORMAL);
            }
            return 0;
        }
        case WM_FOUND: {
            std::unique_ptr<std::vector<std::string>> v(reinterpret_cast<std::vector<std::string>*>(lp));
            if (!v->empty() && !usable(w->checks)) {
                w->copies = *v;
                build(*w);  // the button's text
                name_checks(*w);
            }
            return 0;
        }
        case WM_DROPFILES: {  // a folder dropped on the window
            if (w->svc->use_copy) {
                wchar_t path[MAX_PATH * 2] = {};
                if (DragQueryFileW(HDROP(wp), 0, path, UINT(sizeof path / sizeof path[0])) > 0) choose(*w, w->svc->use_copy(narrow(path)));
            }
            DragFinish(HDROP(wp));
            return 0;
        }
        case WM_CHECKED: {
            std::unique_ptr<Answer> a(reinterpret_cast<Answer*>(lp));
            if (a->generation != w->generation) return 0;
            w->checking = false;
            w->checks = std::move(a->checks);
            name_checks(*w);
            if (!usable(w->checks)) {
                show_page(*w, game_files_tab(*w));  // the full list, with what to do
                if (!w->searched && w->svc->find_copies) {  // look for a copy of the game by ourselves, once
                    w->searched = true;
                    std::thread([hwnd, find = w->svc->find_copies] {
                        auto* v = new std::vector<std::string>(find());
                        if (!PostMessageW(hwnd, WM_FOUND, 0, LPARAM(v))) delete v;
                    }).detach();
                }
            }
            layout_header(*w);
            return 0;
        }
        case WM_COMMAND:
            command(*w, LOWORD(wp), HIWORD(wp));
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            w->closed = true;
            PostMessageW(nullptr, WM_NULL, 0, 0);  // wakes the loop that waits for this window
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void register_classes(HINSTANCE inst) {
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSW wc = {};
    if (!GetClassInfoW(inst, L"AmagamiLauncher", &wc)) {
        wc = {};
        wc.lpfnWndProc = window_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
#ifdef AMG_HAS_ICON
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
#endif
        wc.lpszClassName = L"AmagamiLauncher";
        RegisterClassW(&wc);
    }
    if (!GetClassInfoW(inst, L"AmagamiChecks", &wc)) {
        wc = {};
        wc.lpfnWndProc = checks_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"AmagamiChecks";
        RegisterClassW(&wc);
    }
}

// The window of `width` x `height` client pixels at 96 DPI, scaled, placed and built; not shown yet.
void open(Window& w, HWND parent, int width, int height) {
    const DWORD style = WS_CAPTION | WS_SYSMENU | (parent ? WS_POPUP : WS_MINIMIZEBOX) | WS_CLIPCHILDREN;
    w.s.dpi = dpi_for(parent);
    RECT rc = {0, 0, w.s(width), w.s(height)};
    using AdjustForDpi = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    if (auto adjust = reinterpret_cast<AdjustForDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi")))
        adjust(&rc, style, FALSE, 0, UINT(w.s.dpi));
    else AdjustWindowRect(&rc, style, FALSE);
    w.win = CreateWindowW(L"AmagamiLauncher", L"Amagami", style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, parent, nullptr,
                          w.inst, nullptr);
    SetWindowLongPtrW(w.win, GWLP_USERDATA, LONG_PTR(&w));
    place(w.win);
    w.s.dpi = dpi_for(w.win);  // placed on another screen than the system's: that screen's DPI
    // The client area exactly the size asked for (the frame is the screen's, which differs from the DPI asked for in tests).
    RECT client, frame;
    GetClientRect(w.win, &client);
    GetWindowRect(w.win, &frame);
    SetWindowPos(w.win, nullptr, 0, 0, (frame.right - frame.left) + w.s(width) - client.right, (frame.bottom - frame.top) + w.s(height) - client.bottom,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    place(w.win);
    if (const char* t = std::getenv("AMG_LAUNCHER_TAB")) w.tab = std::atoi(t);  // development: the tab to open on
    build(w);
}

// The launcher's own loop: the dialog keys, plus Ctrl+Tab, Ctrl+Shift+Tab and Ctrl+1..4 for the tabs.
void run_loop(Window& w) {
    MSG m;
    while (!w.closed && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) && !w.pages.empty() && IsChild(w.win, m.hwnd)) {
            const int n = int(w.pages.size());
            int to = -1;
            if (m.wParam == VK_TAB) to = (w.tab + ((GetKeyState(VK_SHIFT) & 0x8000) ? n - 1 : 1)) % n;
            else if (m.wParam >= '1' && m.wParam < WPARAM('1' + n)) to = int(m.wParam - '1');
            if (to >= 0) {
                show_page(w, to);
                if (w.tabs) SetFocus(w.tabs);
                continue;
            }
        }
        if (!IsDialogMessageW(w.win, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}

// The launcher's windows count in real pixels (per-monitor DPI awareness v2, for this thread).
void dpi_aware_thread() {
    using SetThreadDpiFn = HANDLE(WINAPI*)(HANDLE);
    if (auto set_dpi = reinterpret_cast<SetThreadDpiFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")))
        set_dpi(HANDLE(-4));  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
}

}  // namespace

void show_game_settings(HWND parent, HINSTANCE inst, const Services& services) {
    register_classes(inst);
    Window w;
    w.inst = inst;
    w.svc = &services;
    w.in_game = true;
    open(w, parent, 700, 400);
    EnableWindow(parent, FALSE);
    show(w.win);
    run_loop(w);
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    w.fonts.free();
}

std::string run(HINSTANCE inst, const std::string& root, const Services& services) {
    dpi_aware_thread();
    register_classes(inst);
    Window w;
    w.inst = inst;
    w.svc = &services;
    w.root = root;
    open(w, nullptr, 760, 520);
    if (!root.empty()) {  // the title bar and the taskbar show the game's icon from the player's copy
        if (HICON big = shortcut::load_game_icon(root, GetSystemMetrics(SM_CXICON))) SendMessageW(w.win, WM_SETICON, ICON_BIG, LPARAM(big));
        if (HICON little = shortcut::load_game_icon(root, GetSystemMetrics(SM_CXSMICON))) SendMessageW(w.win, WM_SETICON, ICON_SMALL, LPARAM(little));
    }
    DragAcceptFiles(w.win, TRUE);
    if (services.read_setting("updates") != "off") {
        HWND hwnd = w.win;
        std::thread([hwnd] {
            const std::string tag = update_check::latest();
            if (update_check::is_newer(tag) && IsWindow(hwnd)) PostMessageW(hwnd, WM_UPDATE, 0, LPARAM(new std::string(tag)));
        }).detach();
    }
    recheck(w);
    show(w.win);
    run_loop(w);
    w.fonts.free();
    if (w.icon) DestroyIcon(w.icon);
    return w.result;
}

}  // namespace amg::launcher
