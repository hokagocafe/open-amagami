#include "port_settings.hpp"

#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif
#include <map>
#include <vector>

namespace amg::port_settings {

std::function<std::string(const std::string&)> load;
std::function<void(const std::string&, const std::string&)> save;

namespace {

struct Row {
    const char* key;
    const char* label_en;
    std::vector<const char*> values;  // as written in config.txt; the first is the default
    std::vector<const char*> en;  // the names of the values
    const char* label_ja;         // the same in Japanese, for the Japanese layer (UTF-8); null to keep English
    std::vector<const char*> ja;
    int def = 0;                  // the index of the value used when none is stored (0: the first)
};

// The port's own screen (opened from the game's main menu, port/script/PortMain.scfs) shows the rows in tabs with the look
// of the game's Settings screen: at most four tabs (its tab bar has four places) of at most eight rows (its pages hold
// eight above "Defaults"). Every row of the table is in one tab.
struct Tab {
    const char* en;
    const char* ja;
    std::vector<const char*> keys;
};

const std::vector<Tab>& tab_table() {
    static const std::vector<Tab> table = {
        {"Display", "画面", {"window", "uiscale", "render", "filter", "aspect", "brightness", "gamma", "vsync"}},
        {"Controls", "操作", {"glyphs", "confirm", "fallback"}},
        {"System", "システム", {"fps", "splash", "launcher", "updates"}},
    };
    return table;
}

const std::vector<Row>& rows() {
    static const std::vector<Row> table = {
        {"uiscale", "Interface size", {"100", "90", "80", "70", "60", "50"}, {"100%", "90%", "80%", "70%", "60%", "50%"},
         "画面表示の大きさ", {}},
        {"render", "Render scale", {"auto", "1", "2", "3", "4"}, {"Auto", "1x", "2x", "3x", "4x"},
         "描画の解像度", {"自動", "1倍", "2倍", "3倍", "4倍"}},
        {"filter", "Screen filter", {"smooth", "sharp", "pixels", "bicubic", "lanczos", "sharpen"},
         {"Smooth", "Sharp", "Pixels", "Bicubic", "Lanczos", "Sharpen"},
         "画面フィルター", {"なめらか", "シャープ", "ドット", "バイキュービック", "ランチョス", "鮮明"}},
        {"aspect", "Screen shape", {"fit", "fill", "stretch", "whole", "blur"}, {"Fit", "Fill", "Stretch", "Whole", "Fit, blurred edges"},
         "画面の形", {"収める", "埋める", "引き伸ばし", "全体", "収める(ぼかし)"}},
        {"glyphs", "Button pictures", {"auto", "playstation", "xbox", "keyboard"}, {"Auto", "PlayStation", "Xbox", "Keyboard"},
         "ボタンの絵", {"自動", "PS", "Xbox", "キーボード"}},  // full-width Latin is wide on this layer: "PlayStation" overflows
        {"confirm", "Confirm button", {"auto", "south", "east"},
         {"Auto", "South", "East"}, "決定ボタン", {"自動", "下", "右"}},
        {"fallback", "Unknown pad", {"playstation", "xbox"}, {"PlayStation", "Xbox"}, "不明なパッド", {"PS", "Xbox"}},
        {"window", "Window mode", {"windowed", "fullscreen"}, {"Windowed", "Full screen"},
         "ウィンドウ表示", {"ウィンドウ", "全画面"}},
        {"brightness", "Brightness", {"80", "90", "100", "110", "120", "130"}, {"80%", "90%", "100%", "110%", "120%", "130%"},
         "明るさ", {}, 2},
        {"gamma", "Gamma", {"0.8", "0.9", "1.0", "1.1", "1.2", "1.3"}, {"0.8", "0.9", "1.0", "1.1", "1.2", "1.3"}, "ガンマ", {}, 2},
        {"fps", "Frame counter", {"off", "on"}, {"Off", "On"}, "フレームカウンター", {"オフ", "オン"}},
        {"vsync", "Vertical sync", {"on", "off"}, {"On", "Off"}, "垂直同期", {"オン", "オフ"}},
        {"splash", "Splash screen", {"on", "off"}, {"On", "Off"}, "起動ロゴ", {"オン", "オフ"}},
        {"launcher", "Launcher at start", {"show", "skip"}, {"Show", "Skip (Shift shows it)"}, "起動時のランチャー", {"表示", "省略"}},
        {"updates", "Check for updates", {"on", "off"}, {"On", "Off"}, "更新の確認", {"オン", "オフ"}},
    };
    return table;
}

bool japanese_layer = false;
int row_pitch = 18;

// The game's text is Shift-JIS and its English too: Latin letters are the full-width ones (0x82xx), with the font drawing
// them at their natural width. Japanese rows are written in UTF-8 here and converted; without the Windows converter the
// rows stay in English.
std::string wide(const std::string& text) {
    static const char* const PUNCTUATION = " .,:/-()+!?&'%";
    static const unsigned char CODES[] = {0x40, 0x44, 0x43, 0x46, 0x5E, 0x7C, 0x69, 0x6A, 0x7B, 0x49, 0x48, 0x95, 0x66, 0x93};
    std::string out;
    for (char c : text) {
        unsigned char lead = 0x81, trail = 0x40;
        if (c >= 'A' && c <= 'Z') lead = 0x82, trail = (unsigned char)(0x60 + (c - 'A'));
        else if (c >= 'a' && c <= 'z') lead = 0x82, trail = (unsigned char)(0x81 + (c - 'a'));
        else if (c >= '0' && c <= '9') lead = 0x82, trail = (unsigned char)(0x4F + (c - '0'));
        else if (const char* hit = c ? std::strchr(PUNCTUATION, c) : nullptr) trail = CODES[hit - PUNCTUATION];
        out += char(lead);
        out += char(trail);
    }
    return out;
}

// `utf8` as the game's text: ASCII letters full width, everything else Shift-JIS (code page 932).
std::string encode(const std::string& utf8) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), w.data(), n);
    std::string out;
    for (wchar_t ch : w) {
        if (ch < 0x80) {
            out += wide(std::string(1, char(ch)));
            continue;
        }
        char two[4] = {};
        const int got = WideCharToMultiByte(932, 0, &ch, 1, two, sizeof two, nullptr, nullptr);
        out.append(two, size_t(got > 0 ? got : 0));
    }
    return out;
#else
    return wide(utf8);
#endif
}

std::map<std::string, std::string> memory;  // when there is no window to keep them

std::string current(const Row& r) {
    std::string v = load ? load(r.key) : (memory.count(r.key) ? memory[r.key] : "");
    for (const char* known : r.values)
        if (v == known) return v;
    return r.values[size_t(r.def)];
}

int index_of(const Row& r) {
    std::string v = current(r);
    for (size_t i = 0; i < r.values.size(); ++i)
        if (v == r.values[i]) return int(i);
    return 0;
}

}  // namespace

void set_layer(bool japanese, int pitch) {
    japanese_layer = japanese;
    row_pitch = pitch;
}
int pitch() { return row_pitch; }

namespace {

// The table's index of a row by its key (-1 if none).
int row_of(const char* key) {
    for (size_t i = 0; i < rows().size(); ++i)
        if (std::strcmp(rows()[i].key, key) == 0) return int(i);
    return -1;
}

const Row* row_at(int row) { return row >= 0 && row < int(rows().size()) ? &rows()[size_t(row)] : nullptr; }
const Tab* tab_at(int tab) { return tab >= 0 && tab < int(tab_table().size()) ? &tab_table()[size_t(tab)] : nullptr; }

std::string game_text(const char* en, const char* ja) { return japanese_layer && ja ? encode(ja) : wide(en); }

}  // namespace

std::string menu_label() { return game_text("Port options", "ポートの設定"); }

int tabs() { return int(tab_table().size()); }

std::string tab_title(int tab) {
    const Tab* t = tab_at(tab);
    return t ? game_text(t->en, t->ja) : "";
}

int tab_rows(int tab) {
    const Tab* t = tab_at(tab);
    return t ? int(t->keys.size()) : 0;
}

int tab_row(int tab, int at) {
    const Tab* t = tab_at(tab);
    return t && at >= 0 && at < int(t->keys.size()) ? row_of(t->keys[size_t(at)]) : -1;
}

std::vector<RowInfo> all_rows() {
    std::vector<RowInfo> out;
    for (const Row& r : rows()) {
        RowInfo info;
        info.key = r.key;
        info.label = r.label_en;
        for (const char* v : r.values) info.values.push_back(v);
        for (const char* n : r.en) info.names.push_back(n);
        info.index = index_of(r);
        out.push_back(std::move(info));
    }
    return out;
}

void set_value(const std::string& key, const std::string& value) {
    if (save) save(key, value);
    else memory[key] = value;
}

std::string label(int row) {
    const Row* r = row_at(row);
    return r ? game_text(r->label_en, r->label_ja) : "";
}

std::string text(int row) {
    const Row* r = row_at(row);
    if (!r) return "";
    const size_t value = size_t(index_of(*r));
    if (japanese_layer && r->label_ja) return encode(value < r->ja.size() ? r->ja[value] : r->en.at(value));
    return wide(r->en.at(value));
}

void step(int row, int direction) {
    const Row* r = row_at(row);
    if (!r) return;
    const int n = int(r->values.size());
    set_value(r->key, r->values[size_t(((index_of(*r) + direction) % n + n) % n)]);
}

void reset(int row) {
    if (const Row* r = row_at(row)) set_value(r->key, r->values[size_t(r->def)]);
}

}  // namespace amg::port_settings

