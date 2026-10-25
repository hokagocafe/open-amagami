// The launcher window (docs/DECISIONS.md D-027): the list of checks of the player's game folder, a Start game button, a
// Settings button and the choice of the folder. Windows only.
#pragma once

#include <functional>
#include <string>

#include <windows.h>

namespace amg::launcher {

struct Services {
    std::function<std::string(const std::string& key)> read_setting;
    std::function<void(const std::string& key, const std::string& value)> write_setting;
    // Asks for the game's folder, checks it, offers to copy it; the folder to run, or "" if the player gave up.
    std::function<std::string()> choose_folder;
    // Asks for the English fan patch and adds it to `root` as the layer lang/en; true if it was added.
    std::function<bool(const std::string& root)> add_english;
};

// Shows the launcher with `root` (the remembered folder, maybe empty) and returns the folder to run once the player presses
// Start game, or "" if the window was closed.
std::string run(HINSTANCE inst, const std::string& root, const Services& services);

}  // namespace amg::launcher
