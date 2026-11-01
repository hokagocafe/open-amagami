// The one-click update: downloads the newest release's zip, checks it against the release's SHA256SUMS.txt, and puts its files
// over the program's own folder (docs/PLAN.md, v0.4). A running program cannot overwrite itself on Windows, but it can be
// renamed: each file that is replaced is first moved to `<name>.old`, and the new program deletes the `.old` files when it
// starts. The saves and the settings are not in the program's folder (or, in portable mode, in `portable-data`, which an update
// never touches). Windows only.
#pragma once

#include <functional>
#include <string>

namespace amg::updater {

// Downloads and installs release `tag` ("v0.2.0"). `progress` is told what it is doing ("Downloading...", "Checking...").
// Returns "" when the files are in place, else a message for the player; on a failure the files are as they were. Blocks: call
// it from a thread of its own.
// For tests, AMG_UPDATE_BASE="http://127.0.0.1:8123/" takes the two files from there instead of the project's GitHub releases.
std::string install(const std::string& tag, const std::function<void(const std::string&)>& progress);

// Starts the program again (the files an update left) with the same folder. True if it started.
bool restart();

// Deletes the `.old` files an update left beside the program (called when the program starts).
void cleanup();

}  // namespace amg::updater
