#include "launcher_actions.hpp"

#include <filesystem>

#include "dialog.hpp"
#include "ui_text.hpp"

namespace amg::launcher {

DeleteResult delete_copy(const ProjectSpec& spec) {
    const std::string copy = spec.game_copy_dir ? spec.game_copy_dir() : std::string();
    std::error_code ec;
    if (copy.empty() || !std::filesystem::exists(std::filesystem::u8path(copy), ec)) {
        dialog::show(ui::tr("There is no copy of the game in the program's own folder."), dialog::Buttons::Ok, dialog::Icon::Info);
        return DeleteResult::NoCopy;
    }
    if (dialog::show(ui::fmt("Delete the copy of the game in:\n{0}\n\nYour saves and settings are kept. You can copy the game again from your own folder.", {copy}),
                     dialog::Buttons::YesNo, dialog::Icon::Question, true) != dialog::Reply::Yes)
        return DeleteResult::Declined;
    std::filesystem::remove_all(std::filesystem::u8path(copy), ec);
    // The remembered folder is forgotten when it was this copy; compared as paths, not as text (one may use `\` and the other `/`).
    const std::string remembered = spec.read_setting("root");
    if (!remembered.empty() && std::filesystem::u8path(remembered).lexically_normal() == std::filesystem::u8path(copy).lexically_normal()) spec.write_setting("root", "");
    return DeleteResult::Deleted;
}

}  // namespace amg::launcher
