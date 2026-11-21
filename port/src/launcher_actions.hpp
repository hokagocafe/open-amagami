// Actions of the launcher that a window and the command line share (docs/technical/launcher-component.md). They ask through the
// dialog service, so each front end answers in its own way.
#pragma once

#include "launcher_spec.hpp"

namespace amg::launcher {

enum class DeleteResult { NoCopy, Declined, Deleted };

// Deletes the copy of the game the program made of its own, after asking. When it was the remembered folder, that is forgotten.
DeleteResult delete_copy(const ProjectSpec& spec);

}  // namespace amg::launcher
