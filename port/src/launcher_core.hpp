// The launcher component's core (docs/technical/launcher-component.md): the state of the launcher (which folder, what its checks said, the
// copies found, the update), the worker threads that fill it, and the view model a backend draws. No operating-system or UI code: a
// backend for any system owns one `Core`, calls its actions, and when `wake` fires calls `pump()` on its own thread.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "launcher_spec.hpp"

namespace amg::launcher {

// What the backend shows, derived from the state.
struct ViewModel {
    int status = -1;          // the header's mark: -1 still checking, else Check::Ok, Warn or Fail
    std::string status_text;  // the header's line
    bool status_bold = false; // the line is about a failure
    bool usable = false, checking = false, updating = false;
    bool start_enabled = false, fix_visible = false, found_visible = false, update_visible = false;
    std::string fix_label, found_label, update_label;
};

class Core {
public:
    enum class Kind { Checked, Found, UpdateTag, UpdateStep, Updated };
    // One thing a worker finished, after `pump()` applied it to the state (so the backend can react to it).
    struct Event {
        Kind kind;
        bool applied = true;  // Found: copies were taken
        std::string text;     // UpdateTag: the tag; UpdateStep: what it does now; Updated: "" when in place, else why not
        // The worker's payload, taken by `pump()`.
        int generation = 0;                // Checked: the generation asked
        std::vector<Check> checks;         // Checked
        std::vector<std::string> list;     // Found
    };

    const ProjectSpec* spec = nullptr;
    std::function<void()> wake;  // set by the backend: called from a worker thread when `pump()` has something to apply

    // --- State (the backend reads it).
    std::string root, result;  // the folder chosen; the folder to run when the player pressed Start
    std::vector<Check> checks;
    bool checking = false, searched = false, updating = false;
    int generation = 0;
    std::vector<std::string> copies;  // usable copies found on the machine (offered when the folder is not usable)
    std::string update_tag, update_text;

    Core();

    // --- Actions. Each starts a worker that answers through `wake` and `pump()`.
    void recheck();               // check `root` again
    void search_copies_once();    // look for a copy of the game by ourselves, once
    void check_for_update();      // when releases are known and the setting allows it
    void install_update();        // download and install `update_tag`
    // Applies what the workers produced, in order, and returns it. Answers of an older check are dropped.
    std::vector<Event> pump();

    bool not_usable() const;
    ViewModel view() const;
    // Writes the state as text (a test hook: the file the check of a scenario compares).
    void write_dump(const std::string& path) const;

private:
    struct Shared;                       // the queue the workers fill; it outlives the Core while a worker still runs
    std::shared_ptr<Shared> shared_;
};

}  // namespace amg::launcher
