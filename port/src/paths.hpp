// Where the program keeps what is not the program (docs/DECISIONS.md D-048), in one place.
//
//   Windows   config: portable-data\config.ini   saves: portable-data\save
//             game:   portable-data\game          logs:  portable-data\logs      (all beside the program)
//   elsewhere (the command line build in WSL) the same names relative to the working folder, as before
//
// The program's own files (the executable, skins, script, art) are read where the executable is and never written. The
// folders are not created here; the caller creates the one it writes in. `legacy_dir` names the folders earlier versions
// used so the launcher can offer to copy from them; nothing here writes to them.
#pragma once

#include <string>

namespace amg::paths {

std::string exe_dir();   // the folder of the running program ("" if unknown)
std::string config_file();  // config.ini
std::string save_dir();
std::string game_dir();     // the imported copy of the game
std::string log_dir();
std::string legacy_dir(bool roaming);  // %APPDATA%\open-amagami (roaming) or %LOCALAPPDATA%\open-amagami; "" off Windows

}  // namespace amg::paths
