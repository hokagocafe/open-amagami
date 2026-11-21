#include "launcher_core.hpp"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <thread>

#include "ui_text.hpp"

namespace amg::launcher {

// The queue the workers fill. A worker holds it through a shared pointer, so one that outlives the Core touches only this.
struct Core::Shared {
    std::mutex lock;
    std::deque<Event> queue;
};

Core::Core() : shared_(std::make_shared<Shared>()) {}

void Core::recheck() {
    checking = true;
    checks.clear();
    const int asked = ++generation;
    std::thread([shared = shared_, root = root, asked, verify = spec ? spec->verify : nullptr, wake = wake] {
        std::vector<Check> found;
        try {
            if (verify) found = verify(root);
        } catch (const std::exception& e) {  // a thread that throws would end the whole program
            found = {{Check::Fail, "Game folder", ui::fmt("This folder could not be checked ({0}). Choose another folder, or check that you can read it.", {e.what()})}};
        } catch (...) {
            found = {{Check::Fail, "Game folder", ui::fmt("This folder could not be checked ({0}). Choose another folder, or check that you can read it.", {"?"})}};
        }
        Event e{Kind::Checked, true, ""};
        e.generation = asked;
        e.checks = std::move(found);
        {
            std::lock_guard<std::mutex> g(shared->lock);
            shared->queue.push_back(std::move(e));
        }
        if (wake) wake();
    }).detach();
}

void Core::search_copies_once() {
    if (searched || !spec || !spec->find_copies) return;
    searched = true;
    std::thread([shared = shared_, find = spec->find_copies, wake = wake] {
        Event e{Kind::Found, true, ""};
        e.list = find();
        {
            std::lock_guard<std::mutex> g(shared->lock);
            shared->queue.push_back(std::move(e));
        }
        if (wake) wake();
    }).detach();
}

void Core::check_for_update() {
    if (!spec || !spec->updates.latest || spec->read_setting("updates") == "off") return;
    std::thread([shared = shared_, latest = spec->updates.latest, is_newer = spec->updates.is_newer, wake = wake] {
        const std::string tag = latest();
        if (is_newer && is_newer(tag)) {
            {
                std::lock_guard<std::mutex> g(shared->lock);
                shared->queue.push_back({Kind::UpdateTag, true, tag});
            }
            if (wake) wake();
        }
    }).detach();
}

void Core::install_update() {
    if (updating || update_tag.empty()) return;
    updating = true;
    update_text = ui::tr("Downloading...");
    std::thread([shared = shared_, tag = update_tag, install = spec ? spec->updates.install : nullptr, wake = wake] {
        auto post = [&](Kind kind, const std::string& text) {
            {
                std::lock_guard<std::mutex> g(shared->lock);
                shared->queue.push_back({kind, true, text});
            }
            if (wake) wake();
        };
        const std::string outcome = !install ? std::string("This program has no update source.") : install(tag, [&](const std::string& step) { post(Kind::UpdateStep, step); });
        post(Kind::Updated, outcome);
    }).detach();
}

std::vector<Core::Event> Core::pump() {
    std::deque<Event> taken;
    {
        std::lock_guard<std::mutex> g(shared_->lock);
        taken.swap(shared_->queue);
    }
    std::vector<Event> applied;
    for (Event& e : taken) {
        switch (e.kind) {
            case Kind::Checked:
                if (e.generation != generation) continue;  // an older check
                checking = false;
                checks = std::move(e.checks);
                break;
            case Kind::Found:
                e.applied = !e.list.empty() && !usable(checks);
                if (e.applied) copies = std::move(e.list);
                break;
            case Kind::UpdateTag:
                update_tag = e.text;
                break;
            case Kind::UpdateStep:
                update_text = e.text;
                break;
            case Kind::Updated:
                updating = false;
                update_text.clear();
                break;
        }
        applied.push_back(std::move(e));
    }
    return applied;
}

bool Core::not_usable() const { return !checking && !usable(checks); }

ViewModel Core::view() const {
    ViewModel v;
    v.checking = checking;
    v.updating = updating;
    v.usable = usable(checks);
    const bool bad = not_usable();
    const Check* failed = nullptr;
    for (const Check& c : checks)
        if (c.status == Check::Fail && !failed) failed = &c;
    bool warned = false;
    for (const Check& c : checks) warned = warned || c.status == Check::Warn;
    if (checking) {
        v.status = -1;
        v.status_text = ui::tr("Checking your game folder...");
    } else if (checks.empty()) {
        v.status = Check::Warn;
        v.status_text = ui::tr("Choose the folder of your own copy of the game to begin.");
    } else if (failed) {
        v.status = Check::Fail;
        v.status_text = failed->what == "Executable" ? ui::tr("Choose the original game folder, then add the English patch.")
                                                     : ui::tr(failed->what) + ": " + failed->detail;
        v.status_bold = true;
    } else {
        v.status = warned ? Check::Warn : Check::Ok;
        v.status_text = ui::tr("Your game is ready. In the game, F8 opens the options.");
    }
    v.fix_label = ui::tr(failed && failed->what == "Executable" ? "Choose original folder..." : "Choose game folder...");
    v.fix_visible = bad;
    v.found_visible = bad && !copies.empty();
    if (!copies.empty()) {  // "Use the copy found in your Vita3K" or in the folder's own name, not the whole path (it does not fit)
        const std::string& found = copies.front();
        std::string label = found;
        const size_t cut = found.find_last_of("/\\");
        if (cut != std::string::npos && cut + 1 < found.size()) label = found.substr(cut + 1);
        v.found_label = found.find("Vita3K") != std::string::npos ? ui::tr("Use the copy found in your Vita3K") : ui::fmt("Use the copy found in {0}", {label});
    }
    v.update_visible = !update_tag.empty();
    if (v.update_visible) v.update_label = update_text.empty() ? ui::fmt("Get the new version, {0}", {update_tag}) : update_text;
    v.start_enabled = !checking && !updating && v.usable;
    return v;
}

void Core::write_dump(const std::string& path) const {
    auto flat = [](std::string s) {
        for (char& c : s)
            if (c == '\n' || c == '\r') c = ' ';
        return s;
    };
    const ViewModel v = view();
    std::ofstream out(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    out << "root=" << root << "\n";
    out << "usable=" << (v.usable ? 1 : 0) << "\n";
    out << "start_enabled=" << (v.start_enabled ? 1 : 0) << "\n";
    out << "fix_visible=" << (v.fix_visible ? 1 : 0) << "\n";
    out << "fix_label=" << v.fix_label << "\n";
    out << "found_visible=" << (v.found_visible ? 1 : 0) << "\n";
    out << "found_label=" << v.found_label << "\n";
    out << "update_visible=" << (v.update_visible ? 1 : 0) << "\n";
    for (const Check& c : checks) out << "check=" << int(c.status) << "|" << c.what << "|" << flat(c.detail) << "\n";
    for (const std::string& c : copies) out << "copy=" << c << "\n";
    out << "settled=1\n";
}

}  // namespace amg::launcher
