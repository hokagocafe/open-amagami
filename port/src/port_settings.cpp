#include "port_settings.hpp"

#include <cstring>
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
};

// The first MENU_ROWS rows are in the game's Settings screen (its Misc page has room for five more beside the game's three);
// the others are read from config.txt and the environment only, until the port has a menu of its own (F5-11 A).
constexpr int MENU_ROWS = 5;

const std::vector<Row>& rows() {
    static const std::vector<Row> table = {
        {"uiscale", "Interface size", {"100", "90", "80", "70", "60", "50"}, {"100%", "90%", "80%", "70%", "60%", "50%"}},
        {"render", "Render scale", {"auto", "1", "2", "3", "4"}, {"Auto", "1x", "2x", "3x", "4x"}},
        {"filter", "Screen filter", {"smooth", "sharp", "pixels"}, {"Smooth", "Sharp", "Pixels"}},
        {"aspect", "Screen shape", {"fit", "fill", "stretch", "whole"}, {"Fit", "Fill", "Stretch", "Whole"}},
        {"glyphs", "Button pictures", {"auto", "playstation", "xbox", "keyboard"},
         {"Auto", "PlayStation", "Xbox", "Keyboard"}},
        {"confirm", "Confirm button", {"auto", "south", "east"},
         {"Auto", "South", "East"}},
        {"fallback", "Unknown pad", {"playstation", "xbox"}, {"PlayStation", "Xbox"}},
        {"splash", "Splash screen", {"on", "off"}, {"On", "Off"}},
        {"launcher", "Launcher at start", {"show", "skip"}, {"Show", "Skip (Shift shows it)"}},
    };
    return table;
}

// The game's text is Shift-JIS and its English too: Latin letters are the full-width ones (0x82xx), with the font drawing
// them at their natural width. The rows are in English only for now; the Japanese layer shows English in the same way.
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

std::map<std::string, std::string> memory;  // when there is no window to keep them

std::string current(const Row& r) {
    std::string v = load ? load(r.key) : (memory.count(r.key) ? memory[r.key] : "");
    for (const char* known : r.values)
        if (v == known) return v;
    return r.values[0];
}

int index_of(const Row& r) {
    std::string v = current(r);
    for (size_t i = 0; i < r.values.size(); ++i)
        if (v == r.values[i]) return int(i);
    return 0;
}

}  // namespace

int count() { return MENU_ROWS; }

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
    const Row& r = rows().at(size_t(row));
    return wide(r.label_en);
}

std::string text(int row) {
    const Row& r = rows().at(size_t(row));
    return wide(r.en.at(size_t(index_of(r))));
}

void step(int row, int direction) {
    const Row& r = rows().at(size_t(row));
    int n = int(r.values.size());
    const char* value = r.values[size_t((index_of(r) + direction + n) % n)];
    if (save) save(r.key, value);
    else memory[r.key] = value;
}

}  // namespace amg::port_settings
