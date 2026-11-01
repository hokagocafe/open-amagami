// The port's own settings, on a screen of their own in the game's Settings look (F5-11): the scripts of port/script draw
// it; this is what they ask. The values live in config.txt; `load` and `save` are the window's, so this part builds without it.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace amg::port_settings {

extern std::function<std::string(const std::string& key)> load;                        // "" if not set
extern std::function<void(const std::string& key, const std::string& value)> save;    // stores it and applies it at once

// Every row, in plain text, for the launcher's settings window (it lists them all, not just the five of the game's screen).
struct RowInfo {
    std::string key, label;
    std::vector<std::string> values;  // as stored in config.ini
    std::vector<std::string> names;   // as shown
    int index = 0;                    // the current one
};
std::vector<RowInfo> all_rows();
void set_value(const std::string& key, const std::string& value);  // stores it (and applies it) through `save`

// What the game's own rows of the loaded layer look like (read from its ConfigSwitch): the language, and the font size
// of a row's title; the port's rows copy both.
void set_layer(bool japanese, int pitch);
int pitch();

// The port's own screen, opened from the game's main menu (port/script/PortMain.scfs): its entry's label, its tabs and
// the rows of each tab. Rows are numbered as the table of all_rows(); every text is in the game's encoding and the
// language of its layer.
std::string menu_label();
int tabs();
std::string tab_title(int tab);
int tab_rows(int tab);
int tab_row(int tab, int at);  // the row shown at place `at` of tab `tab` (-1 if none)

std::string label(int row);  // the row's title, in the language of the game's layer
std::string text(int row);   // the value now, in the same language
void step(int row, int direction);  // to the next (+1) or the previous (-1) value, wrapping
void reset(int row);                // back to the row's default value

}  // namespace amg::port_settings
