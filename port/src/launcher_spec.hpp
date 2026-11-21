// What a project tells the launcher component about its game (docs/technical/launcher-component.md). No system header: a backend
// for any system reads this. The Amagami one is filled in launcher_amagami.cpp and win_main.cpp.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "launcher_types.hpp"

namespace amg::launcher {

// A row of the settings: a label and a list of values to choose from.
struct SettingRow {
    std::string key, label;
    std::vector<std::string> values, names;  // as stored, as shown (English: translated when shown)
    int index = 0;                           // the current value
};

struct ProjectSpec {
    // --- Identity.
    std::string name = "Game";  // the window's title and the header's big text
    std::string product;        // beside the version, top right of the header
    std::function<std::string()> version;  // the running version

    // --- Where the player's settings are kept (config.ini): read and write by key.
    std::function<std::string(const std::string& key)> read_setting;
    std::function<void(const std::string& key, const std::string& value)> write_setting;

    // --- The settings rows. A key not named by any tab goes to General.
    std::function<std::vector<SettingRow>()> rows;
    std::function<void(const std::string& key, const std::string& value)> set_row;
    std::vector<std::string> general_keys, display_keys, controls_keys;
    // The named variants of the data (Amagami's language layers) as one row, for the folder `root`; written as the setting `variant_key`.
    std::function<SettingRow(const std::string& root)> variants;
    std::string variant_key = "lang";

    // --- The player's copy: checks, its icon (the path of an .ico, "" when there is none), the usable copies already on the machine
    // (slow: called off the window's thread), what to do with a folder the player chose or dropped (the folder to run, or "" if it was
    // refused or cancelled), and the folder picker.
    std::function<std::vector<Check>(const std::string& root)> verify;
    std::function<std::string(const std::string& root)> icon_file;
    std::function<std::string(const std::string& root)> icon_image;  // the path of the game's own picture (a PNG), "" when there is none
    std::function<std::vector<std::string>()> find_copies;
    std::function<std::string(const std::string& folder)> use_copy;
    std::function<std::string()> choose_folder;

    // --- Actions the project adds: adding a variant to `root` (true if it was added), where the saves are, and the copy of the game the
    // program made of its own ("" when it has none).
    // `source` is the folder to add it from ("": ask for it); true if it was added.
    std::function<bool(const std::string& root, const std::string& source)> add_variant;
    std::string add_variant_label;  // the button's text ("Add English patch...")
    // A shortcut to the program on the desktop, when the player asks: true with the path in `result`, false with the reason.
    std::function<bool(const std::string& root, std::string& result)> desktop_shortcut;
    std::string desktop_shortcut_label;  // the button's text ("Create desktop shortcut")
    std::function<std::string()> saves_dir;
    std::function<std::string()> game_copy_dir;

    // --- Releases. All optional: without `latest` the launcher never looks for an update.
    struct Updates {
        std::function<std::string()> latest;                      // the tag of the newest release ("" if unknown)
        std::function<bool(const std::string& tag)> is_newer;
        std::function<std::string(const std::string& tag, const std::function<void(const std::string&)>& step)> install;  // "" when in place
        std::function<bool()> restart;                           // starts the new program; false if it could not
        std::wstring page;                                       // the download page, opened when the install fails
    } updates;

    // --- Windows the project provides (they depend on its input module): the controls and the pad mapping. The arguments are the
    // backend's native handles (a window and an instance) as opaque pointers.
    std::function<void(void* parent, void* instance)> show_controls, show_pad_mapping;
};

// The old name of the interface, kept for the code that still uses it.
using Services = ProjectSpec;

}  // namespace amg::launcher
