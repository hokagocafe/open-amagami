// The portable layout (D-048) of the launcher component: see launcher_portable.cpp.
#pragma once

#include <functional>
#include <string>

namespace amg::launcher {

// "" when the folder beside the program can be written; else its path (a file is made and removed there).
std::string unwritable_data_folder();

// First start beside the program: when earlier versions left settings, saves or the game in the old per-user folders and this folder has
// no settings yet, copies them here (always), then asks whether the old folders should be deleted (default No; only on Yes, only when every
// copy worked, only folders named open-amagami). `write_setting` is the settings file.
void offer_legacy_migration(const std::function<void(const std::string&, const std::string&)>& write_setting);

}  // namespace amg::launcher
