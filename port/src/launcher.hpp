// The launcher window (docs/DECISIONS.md D-027): the list of checks of the player's game folder, a Start game button, a
// Settings button and the choice of the folder. Windows only.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <windows.h>

namespace amg::launcher {

struct Services {
    std::function<std::string(const std::string& key)> read_setting;
    std::function<void(const std::string& key, const std::string& value)> write_setting;
    // Asks for the game's folder, checks it, offers to copy it; the folder to run, or "" if the player gave up.
    std::function<std::string()> choose_folder;
    // Asks for the English fan patch and adds it to `root` as the layer lang/en; true if it was added.
    std::function<bool(const std::string& root)> add_english;
    // The usable copies of the game already on the machine (a Vita3K install, the desktop...); slow: called off the window's thread.
    std::function<std::vector<std::string>()> find_copies;
    // Checks a folder the player dropped or accepted, offers to copy it; the folder to run, or "" if it was refused or cancelled.
    std::function<std::string(const std::string& folder)> use_copy;
};

// Shows the launcher with `root` (the remembered folder, maybe empty) and returns the folder to run once the player presses
// Start game, or "" if the window was closed.
std::string run(HINSTANCE inst, const std::string& root, const Services& services);

// The port's settings over the running game (F8 in its window): the rows that apply at once, the controls and the pad mapping.
// Returns when the player closes it.
void show_game_settings(HWND parent, HINSTANCE inst, const Services& services);

}  // namespace amg::launcher
