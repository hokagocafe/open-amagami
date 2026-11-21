// The game's icon. The icon is the one in the player's own copy (`sce_sys/icon0.png`, the
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

// A shortcut `<name>.lnk` next to the program's .exe with the game's icon, written again at each start so that it follows the folder when it
// is moved. Quiet: "" when written or when there is nothing to do (no game picture), else the reason. The .exe's own icon is inside the file and
// cannot follow the game; this is the file that does, as the engines of older visual novels do.
std::string refresh_next_to_program(const std::string& name, const std::string& root);

// The same shortcut on the player's desktop, only when asked for. Returns the path written ("" on failure); `result` holds the path or the reason.
// AMG_DESKTOP_DIR names another folder (tests).
std::string make_desktop_link(const std::string& name, const std::string& root, std::string& result);

}  // namespace amg::shortcut
