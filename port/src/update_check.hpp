// Asks GitHub for the newest release of the program, so the launcher can say when there is one. One HTTPS request to the
// public releases API, nothing about the player is sent; `updates=off` in config.ini (Settings, Check for updates) turns it
// off. Windows only.
#pragma once

#include <string>

namespace amg::update_check {

// This program's version, as built ("v0.1.3"; "dev" for a build that is not a tagged release).
const char* current();

// The tag of the newest release ("v0.1.4"), or "" when it cannot be told (offline, a dev build, any error). Blocks: call it
// from a thread of its own.
std::string latest();

// True if the release `tag` is newer than this program.
bool is_newer(const std::string& tag);

// Where the player gets it.
constexpr const wchar_t* PAGE = L"https://github.com/hokagocafe/open-amagami/releases/latest";

}  // namespace amg::update_check
