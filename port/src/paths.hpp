// Where the program keeps what is not the program (docs/DECISIONS.md D-027), in one place.
//
//   normal    config:  %APPDATA%\open-amagami\config.ini   saves: %APPDATA%\open-amagami\save
//             data:    %LOCALAPPDATA%\open-amagami\game    logs:  %LOCALAPPDATA%\open-amagami\logs
//   portable  all of it under `portable-data\` beside the program, when a file `portable.txt` is there too
//
// The program's own files (the executable, skins, script, art) are read where the executable is and never written. The
// folders are not created here; the caller creates the one it writes in. Off Windows (the command line build in WSL) the
// same names are relative to the working folder, as before.
#pragma once

#include <string>

namespace amg::paths {

std::string exe_dir();   // the folder of the running program ("" if unknown)
bool portable();         // `portable.txt` beside the program
std::string config_file();  // config.ini
std::string save_dir();
std::string game_dir();     // the imported copy of the game
std::string log_dir();

}  // namespace amg::paths
