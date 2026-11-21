// The launcher's SDL3 backend (docs/technical/launcher-component.md, D-049): an SDL window drawn with Dear ImGui from the core's view model.
// Nothing here is specific to a system beyond SDL; the project's own windows (controls, pad mapping) and its pickers come through the
// ProjectSpec. Selected by the `launcher_ui` setting (`sdl`) or AMG_LAUNCHER_UI_BACKEND=sdl.
#pragma once

#include <string>

#include "launcher_spec.hpp"

namespace amg::launcher {

// Runs the launcher in an SDL window; the folder to run, or "" if the player closed it. `started` (optional) tells whether the window
// could be made at all, so the caller can fall back to another backend.
std::string run_sdl(const ProjectSpec& spec, const std::string& root, bool* started = nullptr);

}  // namespace amg::launcher
