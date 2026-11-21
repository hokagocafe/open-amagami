// The portable layout of every project (docs/DECISIONS.md D-048), as the component's own work: everything is written beside the program,
// so the folder must be writable, and what earlier versions left in the old per-user folders is copied here. Questions go through the
// dialog service, so a window, a script and a terminal each answer them.
#include "launcher_portable.hpp"

#include <filesystem>
#include <fstream>

#include "dialog.hpp"
#include "paths.hpp"
#include "ui_text.hpp"

namespace amg::launcher {

// D-048: everything is written beside the program, so a folder the player cannot write to (Program Files, a read-only
// drive or share) would lose every save without a word. Checked once at the start: a file is made and removed in
// `portable-data`. Returns "" when it works, else the folder's path.
std::string unwritable_data_folder() {
    namespace fs = std::filesystem;
    const std::string cfg = amg::paths::config_file();
    if (cfg.empty()) return "";
    std::error_code ec;
    const fs::path dir = fs::u8path(cfg).parent_path();
    fs::create_directories(dir, ec);
    const fs::path probe = dir / "write-test.tmp";
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        out << "x";
        out.flush();
        if (!out) return dir.u8string();
    }
    fs::remove(probe, ec);
    return "";
}

// First start beside the program (D-048): when earlier versions left settings, saves or the imported game in the old
// %APPDATA% / %LOCALAPPDATA% folders and this folder has no settings yet, everything is copied here (always: the player
// decided so), and then the player is asked whether the old folders should be deleted. They are deleted only on a Yes, only
// when every copy worked, and only folders named open-amagami. Done once: the new settings file marks it.
void offer_legacy_migration(const std::function<void(const std::string&, const std::string&)>& write_setting) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string cfg = amg::paths::config_file();
    if (cfg.empty() || fs::exists(fs::u8path(cfg), ec)) return;
    const fs::path roam = fs::u8path(amg::paths::legacy_dir(true)), local = fs::u8path(amg::paths::legacy_dir(false));
    const fs::path old_cfg = fs::exists(roam / "config.ini", ec) ? roam / "config.ini" : roam / "config.txt";
    const fs::path old_game = local / "game";
    const bool has_roam = !roam.empty() && fs::is_directory(roam, ec), has_local = !local.empty() && fs::is_directory(local, ec);
    if (!has_roam && !has_local) return;
    const fs::path base = fs::u8path(cfg).parent_path();
    dialog::show(ui::fmt("Earlier versions kept your settings, saves and game copy in other folders of this computer:\n{0}\n{1}\n\nThey are being copied next to this program now. It can take a minute; press OK to begin.",
                {roam.u8string(), local.u8string()}), dialog::Buttons::Ok, dialog::Icon::Info);
    std::string failed;
    auto copy_all = [&](const fs::path& from) {  // every entry of the old folder except the settings file, which is rewritten below
        std::error_code e;
        if (!fs::is_directory(from, e)) return;
        for (const fs::directory_entry& entry : fs::directory_iterator(from, e)) {
            const std::string name = entry.path().filename().u8string();
            if (name == "config.ini" || name == "config.txt") continue;
            std::error_code e2;
            if (entry.is_directory(e2)) fs::copy(entry.path(), base / name, fs::copy_options::recursive | fs::copy_options::skip_existing, e2);
            else fs::copy_file(entry.path(), base / name, fs::copy_options::skip_existing, e2);
            if (e2) failed += entry.path().u8string() + " (" + e2.message() + ")\n";
        }
    };
    fs::create_directories(base, ec);
    copy_all(roam);
    copy_all(local);
    if (fs::exists(old_cfg, ec)) {
        // The remembered settings; "root" follows the game copy to its new place.
        std::ifstream in(old_cfg);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == 13) line.pop_back();
            const size_t eq = line.find('=');
            if (eq == std::string::npos || line.compare(0, 13, "legacy_asked=") == 0) continue;
            std::string key = line.substr(0, eq), value = line.substr(eq + 1);
            if (key == "root" && fs::u8path(value).lexically_normal() == old_game.lexically_normal()) {
                fs::path moved = base / "game";
                moved.make_preferred();  // one kind of slash in the remembered path
                value = moved.u8string();
            }
            write_setting(key, value);
        }
    }
    write_setting("legacy_asked", "1");  // creates the settings file if the old one had none, so this is not asked again
    if (!failed.empty()) {
        dialog::show(ui::fmt("Some of it could not be copied, so the old folders are kept:\n{0}", {failed}), dialog::Buttons::Ok, dialog::Icon::Warning);
        return;
    }
    auto deletable = [](const fs::path& p) { return !p.empty() && p.filename() == "open-amagami"; };
    if (dialog::show(ui::fmt("Everything was copied.\n\nDelete the old folders now?\n{0}\n{1}\n\nThe copy next to this program is the one used from now on.",
                    {roam.u8string(), local.u8string()}), dialog::Buttons::YesNo, dialog::Icon::Question, true) == dialog::Reply::Yes) {
        std::error_code e;
        if (deletable(roam)) fs::remove_all(roam, e);
        if (deletable(local)) fs::remove_all(local, e);
    }
}

}  // namespace amg::launcher
