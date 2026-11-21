#include "launcher_amagami.hpp"

#include <filesystem>

#include "data_check.hpp"
#include "datapath.hpp"
#include "launcher_ui.hpp"
#include "paths.hpp"
#include "port_settings.hpp"
#include "shortcut.hpp"
#include "ui_text.hpp"
#include "update_check.hpp"
#include "updater.hpp"

namespace amg::launcher {

namespace {

// `base_english`: the game folder's own data is the English fan translation (the Vita3K-style patched copy).
std::string language_name(const std::string& code, bool base_english) {
    if (code.empty()) return ui::tr(base_english ? "This copy's own (English)" : "Original (Japanese)");
    if (code == "en") return ui::tr("English (fan patch)");
    return code;
}

}  // namespace

void fill_amagami_spec(ProjectSpec& spec, std::function<std::string(const std::string&)> read,
                       std::function<void(const std::string&, const std::string&)> write) {
    spec.name = "Amagami";
    spec.product = "open-amagami";
    spec.version = [] { return std::string(update_check::current()); };
    spec.read_setting = read;
    spec.write_setting = write;

    spec.rows = [] {
        std::vector<SettingRow> out;
        for (const port_settings::RowInfo& info : port_settings::all_rows()) out.push_back({info.key, info.label, info.values, info.names, info.index});
        return out;
    };
    spec.set_row = [](const std::string& key, const std::string& value) { port_settings::set_value(key, value); };
    spec.general_keys = {"ui_lang", "launcher", "updates", "splash"};
    spec.display_keys = {"window", "uiscale", "render", "filter", "aspect", "brightness", "gamma", "vsync", "fps"};
    spec.controls_keys = {"glyphs", "confirm", "fallback"};

    // The game's language: the layers found in the game's folder.
    spec.variant_key = "lang";
    spec.variants = [read](const std::string& root) {
        SettingRow r;
        r.key = "lang";
        r.label = "Language";
        r.values.push_back("");
        std::error_code ec;
        const std::filesystem::path layers = std::filesystem::u8path(root) / "lang";
        if (!root.empty() && std::filesystem::is_directory(layers, ec))
            for (const auto& e : std::filesystem::directory_iterator(layers, ec))
                if (e.is_directory(ec)) r.values.push_back(e.path().filename().string());
        const bool base_english = !root.empty() && known_label(data_file(root, "AMAGAMI.IMG"), "AMAGAMI.IMG").find("English") != std::string::npos;
        const std::string current = read("lang");
        for (size_t i = 0; i < r.values.size(); ++i) {
            r.names.push_back(language_name(r.values[i], base_english));
            if (r.values[i] == current) r.index = int(i);
        }
        return r;
    };
    spec.add_variant_label = "Add English patch...";

    spec.verify = [](const std::string& root) { return check_data(root); };
    spec.icon_file = [](const std::string& root) { return shortcut::game_icon_file(root); };
    spec.icon_image = [](const std::string& root) {
        std::error_code ec;
        const std::filesystem::path png = std::filesystem::u8path(root) / "sce_sys" / "icon0.png";
        return std::filesystem::exists(png, ec) ? png.u8string() : std::string();
    };

    spec.desktop_shortcut_label = "Create desktop shortcut";
    spec.desktop_shortcut = [](const std::string& root, std::string& result) {
        return !shortcut::make_desktop_link("Amagami", root, result).empty();
    };
    spec.saves_dir = [] { return paths::save_dir(); };
    spec.game_copy_dir = [] { return paths::game_dir(); };

    spec.updates.latest = [] { return update_check::latest(); };
    spec.updates.is_newer = [](const std::string& tag) { return update_check::is_newer(tag); };
    spec.updates.install = [](const std::string& tag, const std::function<void(const std::string&)>& step) { return updater::install(tag, step); };
    spec.updates.restart = [] { return updater::restart(); };
    spec.updates.page = update_check::PAGE;

    // The windows that depend on the port's input module.
    ProjectSpec* self = &spec;
    spec.show_controls = [self](void* parent, void* inst) { launcher::show_controls(HWND(parent), HINSTANCE(inst), *self); };
    spec.show_pad_mapping = [](void* parent, void* inst) { launcher::show_pad_mapping(HWND(parent), HINSTANCE(inst)); };
}

}  // namespace amg::launcher
