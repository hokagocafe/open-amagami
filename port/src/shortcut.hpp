// The game's icon and the shortcuts to the program. The icon is the one in the player's own copy (`sce_sys/icon0.png`, the
// Vita's home screen icon), turned into a .ico in the program's folder: nothing of the game ships with the program (D-012).
// Windows only.
#pragma once

#include <string>

#include <windows.h>

namespace amg::shortcut {

// The .ico made from `<root>/sce_sys/icon0.png` (written once, again when the picture changes); "" if the copy has none.
std::string game_icon_file(const std::string& root);

// The game's icon at `size` pixels for a window (title bar, taskbar), or nullptr; the caller keeps it for the program's life.
HICON load_game_icon(const std::string& root, int size);

// Creates (or refreshes) "Amagami" shortcuts to this program on the desktop and in the Start menu's Programs folder, with the
// game's icon from `root` (the program's own icon if the copy has none); "" when both were made, otherwise what failed.
std::string create(const std::string& root);

// Offers the shortcuts once (after the first setup), unless in portable mode or already asked. `mark_asked` remembers it.
void offer_once(const std::string& root, bool asked, void (*mark_asked)());

}  // namespace amg::shortcut
