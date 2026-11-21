// What the launcher component and a project share: the result of one check of the player's copy. Not tied to any game.
#pragma once

#include <string>
#include <vector>

namespace amg {

struct Check {
    enum Status { Ok, Warn, Fail } status = Ok;
    std::string what;    // "Game data (AMAGAMI.IMG, SCENARIO.CPK, graph)"
    std::string detail;  // what was found, or what to do
};

// True when no check failed (warnings allowed) and there is at least one.
inline bool usable(const std::vector<Check>& checks) {
    for (const Check& c : checks)
        if (c.status == Check::Fail) return false;
    return !checks.empty();
}

}  // namespace amg
