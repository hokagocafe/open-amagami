// The port's own settings as rows of the game's Settings screen (F5-11, option B): the scripts PortSwitch and PortHooks (port/script)
// draw them; this is what they ask. The values live in config.txt; `load` and `save` are the window's, so this part builds without it.
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

int count();
std::string label(int row);  // the row's title, in the language of the game's layer
std::string text(int row);   // the value now, in the same language
void step(int row, int direction);  // to the next (+1) or the previous (-1) value, wrapping

}  // namespace amg::port_settings
