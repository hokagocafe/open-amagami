// The launcher component's description of Amagami (docs/technical/launcher-component.md): everything the component needs to know about
// this game, as data and callbacks. What needs the program's own windows (choosing a folder and importing it, adding the English patch) is
// set by win_main.cpp after this.
#pragma once

#include <functional>
#include <string>

#include "launcher_spec.hpp"

namespace amg::launcher {

// Fills `spec` with Amagami's identity, settings, checks, saves, copy, icon and updates. `read` and `write` are the settings file.
// `spec` must outlive the windows opened with it.
void fill_amagami_spec(ProjectSpec& spec, std::function<std::string(const std::string&)> read,
                       std::function<void(const std::string&, const std::string&)> write);

}  // namespace amg::launcher
