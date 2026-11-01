// The list of checks of the player's game folder, for the launcher (docs/DECISIONS.md D-027): not one verdict but a line per
// thing, each OK, a warning (the program can go on) or a failure (it cannot).
#pragma once

#include <string>
#include <vector>

namespace amg {

struct Check {
    enum Status { Ok, Warn, Fail } status = Ok;
    std::string what;    // "Game data (AMAGAMI.IMG, SCENARIO.CPK, graph)"
    std::string detail;  // what was found, or what to do
};

// Checks `root` (a folder in the raw Vita layout, or the prepared one). The files are read for the hashes of the small ones
// (the images and scripts, the executable), so this takes a moment: call it away from the window's thread.
std::vector<Check> check_data(const std::string& root);

// Whether the three data files are whole (not cut short, not damaged), as a message for the player ("" when they are). Reads the
// small file and the tables of the two archives: a moment, so not on the window's thread when it can be avoided.
std::string damaged_files(const std::string& root);

// The label of `path` in the table of known versions as the file `name` ("AMAGAMI.IMG"), or "" if it is not there.
std::string known_label(const std::string& path, const std::string& name);

// True when no check failed (warnings allowed).
bool usable(const std::vector<Check>& checks);

}  // namespace amg
