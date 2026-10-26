// Importing the player's copy of the game into the program's own folder (docs/PLAN.md, "v0.1 data"): the files the
// port reads are copied as they are (it decodes the console's formats itself), so the original can be deleted or moved
// afterwards. Windows only (progress window, free-space check).
#pragma once

#include <cstdint>
#include <string>

namespace amg {

// The folder the imported game lives in: %LOCALAPPDATA%\open-amagami\game.
std::string import_destination();

// Bytes `import_game` would copy from `src` (a folder `check_game_folder` accepts).
uint64_t import_size(const std::string& src);

// Copies the game's files from `src` into `dst`, including nested sound trees and prepared audio, with a progress window.
// A file already there with the same size is kept, so a second import is quick. "" on success, else a message for the
// player (no space, a read error, cancelled).
std::string import_game(const std::string& src, const std::string& dst);

}  // namespace amg
