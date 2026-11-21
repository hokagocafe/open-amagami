// The launcher's Win32 backend (docs/technical/launcher-component.md): the window and the settings over the running game. What it knows
// about a game comes through a `ProjectSpec` (launcher_spec.hpp); the state machine is `Core` (launcher_core.hpp).
#pragma once

#include <string>

#include <windows.h>

#include "launcher_spec.hpp"

namespace amg::launcher {

// Runs the launcher window; the folder to run, or "" if the player closed it.
std::string run(HINSTANCE inst, const std::string& root, const ProjectSpec& spec);

// The settings window over the running game (F8).
void show_game_settings(HWND parent, HINSTANCE inst, const ProjectSpec& spec);

}  // namespace amg::launcher
