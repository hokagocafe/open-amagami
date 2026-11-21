#include "launcher_sdl.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#include "dialog.hpp"
#include "launcher_actions.hpp"
#include "launcher_core.hpp"
#include "third_party/imgui/backends/imgui_impl_sdl3.h"
#include "third_party/imgui/backends/imgui_impl_sdlrenderer3.h"
#include "third_party/imgui/imgui.h"
#include "third_party/imgui/imgui_internal.h"
#include "third_party/stb/stb_image.h"
#include "ui_text.hpp"

namespace amg::launcher {

namespace {

// The palette of the Win32 launcher, so the two look related.
ImU32 rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
ImVec4 vec(int r, int g, int b, int a = 255) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a / 255.f); }

// The colours in use: the light set or the dark one (apply_palette), the dark one following the system unless the `theme` setting says.
ImVec4 BACKGROUND, PANE, INK, MUTED, BORDER, BUTTON, BUTTON_HOVER, BUTTON_ACTIVE, FRAME, TAB, TAB_HOVER, START, START_HOVER;
ImU32 LINE = 0;
int CLEAR_R = 0, CLEAR_G = 0, CLEAR_B = 0;

void apply_palette(bool dark) {
    if (dark) {
        BACKGROUND = vec(30, 29, 35); PANE = vec(41, 40, 48); INK = vec(232, 230, 240); MUTED = vec(160, 156, 176);
        BORDER = vec(66, 64, 78); BUTTON = vec(60, 58, 74); BUTTON_HOVER = vec(78, 75, 98); BUTTON_ACTIVE = vec(94, 90, 120);
        FRAME = vec(52, 50, 64); TAB = vec(48, 46, 60); TAB_HOVER = vec(78, 75, 98);
        START = vec(40, 130, 80); START_HOVER = vec(52, 156, 96);
        CLEAR_R = 30; CLEAR_G = 29; CLEAR_B = 35;
    } else {
        BACKGROUND = vec(247, 246, 244); PANE = vec(255, 255, 255); INK = vec(40, 35, 62); MUTED = vec(93, 88, 112);
        BORDER = vec(222, 219, 214); BUTTON = vec(232, 230, 238); BUTTON_HOVER = vec(214, 210, 230); BUTTON_ACTIVE = vec(196, 190, 220);
        FRAME = vec(238, 236, 244); TAB = vec(236, 234, 242); TAB_HOVER = vec(214, 210, 230);
        START = vec(46, 150, 90); START_HOVER = vec(38, 132, 78);  // the green of the checks' marks
        CLEAR_R = 247; CLEAR_G = 246; CLEAR_B = 244;
    }
    LINE = IM_COL32(int(BORDER.x * 255), int(BORDER.y * 255), int(BORDER.z * 255), 255);
}

void style(float s);

// --- The dialog service over SDL's message box.
class SdlDialogs : public dialog::Service {
public:
    SDL_Window* owner = nullptr;
    std::string title;
    dialog::Reply show(const std::string& message, dialog::Buttons buttons, dialog::Icon icon, bool default_no) override {
        SDL_MessageBoxButtonData b[3] = {};
        int n = 0;
        const std::string yes = ui::tr("Yes"), no = ui::tr("No"), cancel = ui::tr("Cancel"), ok = ui::tr("OK");
        if (buttons == dialog::Buttons::Ok) {
            b[n++] = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, ok.c_str()};
        } else {
            b[n++] = {default_no ? 0u : SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, yes.c_str()};
            b[n++] = {default_no ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0u, 2, no.c_str()};
            if (buttons == dialog::Buttons::YesNoCancel) b[n++] = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 3, cancel.c_str()};
        }
        SDL_MessageBoxData data = {};
        data.flags = icon == dialog::Icon::Error ? SDL_MESSAGEBOX_ERROR : icon == dialog::Icon::Warning ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION;
        data.window = owner;
        data.title = title.c_str();
        data.message = message.c_str();
        data.numbuttons = n;
        data.buttons = b;
        int id = 0;
        if (!SDL_ShowMessageBox(&data, &id)) return buttons == dialog::Buttons::Ok ? dialog::Reply::Ok : dialog::Reply::No;
        return id == 1 ? (buttons == dialog::Buttons::Ok ? dialog::Reply::Ok : dialog::Reply::Yes) : id == 2 ? dialog::Reply::No : dialog::Reply::Cancel;
    }
    void set_owner(void* window) override { owner = static_cast<SDL_Window*>(window); }
};

// --- What the player may change: the rows of a tab, as the Win32 window lists them.
SettingRow row_of(const ProjectSpec& spec, const std::string& root, const std::string& key) {
    if (key == "theme") {  // the launcher's own: not a setting of the game
        SettingRow r;
        r.key = "theme";
        r.label = "Window colours";
        r.values = {"auto", "light", "dark"};
        r.names = {"Automatic (follows the system)", "Light", "Dark"};
        const std::string current = spec.read_setting ? spec.read_setting("theme") : std::string();
        r.index = current == "light" ? 1 : current == "dark" ? 2 : 0;
        return r;
    }
    if (spec.variants && key == spec.variant_key) return spec.variants(root);
    SettingRow r;
    r.key = key;
    if (key == "ui_lang") {
        r.label = "Launcher language";
        r.values = {"", "en", "es", "ja"};
        r.names = {"Automatic (Windows)", "English", "Español", "日本語"};
        const std::string current = spec.read_setting("ui_lang");
        for (size_t i = 0; i < r.values.size(); ++i)
            if (r.values[i] == current) r.index = int(i);
    } else if (spec.rows) {
        for (const SettingRow& info : spec.rows())
            if (info.key == key) return info;
    }
    return r;
}

std::vector<std::string> general_of(const ProjectSpec& spec) {
    std::vector<std::string> keys = spec.general_keys;
    if (!spec.rows) return keys;
    for (const SettingRow& info : spec.rows()) {
        auto named = [&](const std::vector<std::string>& v) { return std::find(v.begin(), v.end(), info.key) != v.end(); };
        if (!named(spec.general_keys) && !named(spec.display_keys) && !named(spec.controls_keys)) keys.push_back(info.key);
    }
    return keys;
}

SDL_Texture* load_texture(SDL_Renderer* renderer, const std::string& png) {
    if (png.empty()) return nullptr;
    std::ifstream in(std::filesystem::u8path(png), std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), {});
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
    if (!px) return nullptr;
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (tex) {
        SDL_UpdateTexture(tex, nullptr, px, w * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    }
    stbi_image_free(px);
    return tex;
}

std::string find_font() {
    if (const char* e = std::getenv("AMG_LAUNCHER_FONT"); e && *e) return e;
    std::error_code ec;
    for (const char* f : {"C:/Windows/Fonts/YuGothM.ttc", "C:/Windows/Fonts/YuGothR.ttc", "C:/Windows/Fonts/meiryo.ttc", "C:/Windows/Fonts/msgothic.ttc",
                          "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"})
        if (std::filesystem::exists(f, ec)) return f;
    return "";
}

// The round mark of a check: grey while checking, else green, amber or red, with a tick, an exclamation mark or a cross.
void draw_mark(ImDrawList* dl, ImVec2 p, float size, int status) {
    const ImU32 colour = status < 0 ? rgb(170, 166, 180) : status == Check::Ok ? rgb(46, 150, 90) : status == Check::Warn ? rgb(214, 150, 30) : rgb(200, 60, 70);
    const ImVec2 c(p.x + size / 2, p.y + size / 2);
    dl->AddCircleFilled(c, size / 2, colour, 24);
    if (status < 0) return;
    const ImU32 white = rgb(255, 255, 255);
    const float t = std::max(1.5f, size / 11.f);
    auto at = [&](float x, float y) { return ImVec2(p.x + x * size / 22.f, p.y + y * size / 22.f); };
    if (status == Check::Ok) {
        dl->AddLine(at(6, 11), at(10, 15), white, t);
        dl->AddLine(at(10, 15), at(16, 7), white, t);
    } else if (status == Check::Fail) {
        dl->AddLine(at(7, 7), at(15, 15), white, t);
        dl->AddLine(at(15, 7), at(7, 15), white, t);
    } else {
        dl->AddLine(at(11, 5), at(11, 13), white, t);
        dl->AddLine(at(11, 16), at(11, 17), white, t);
    }
}

struct State {
    const ProjectSpec* spec = nullptr;
    Core core;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* icon = nullptr;
    SdlDialogs dialogs;
    float s = 1.0f;
    bool running = true;
    int tab = 0, tab_request = -1;  // the tab shown; the tab a key asked for
    bool settled = false;     // the checks (and the search for copies) have answered
    int frames_settled = 0;   // frames drawn since
    bool dark = false;        // the palette in use
    int theme_poll = 0;
};

// light, dark or the system's: the environment (tests), else the `theme` setting, else automatic.
bool want_dark(const State& st) {
    std::string t;
    if (const char* e = std::getenv("AMG_LAUNCHER_THEME")) t = e;
    else if (st.spec->read_setting) t = st.spec->read_setting("theme");
    if (t == "dark") return true;
    if (t == "light") return false;
    return SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK;
}

void refresh_theme(State& st, bool force) {
    if (!force && (++st.theme_poll % 15) != 0) return;
    const bool dark = want_dark(st);
    if (dark == st.dark && !force) return;
    st.dark = dark;
    apply_palette(dark);
    style(st.s);
}

// The game's picture: in the header and as the window's own icon (the taskbar). Once a game's picture has been loaded it stays until another one
// replaces it, so the program's stand-in does not come back while the folder is checked again or a wrong folder is tried.
void load_icon(State& st) {
    if (st.core.root.empty() || !st.spec->icon_image) return;
    const std::string png = st.spec->icon_image(st.core.root);
    if (png.empty()) return;
    SDL_Texture* tex = load_texture(st.renderer, png);
    if (!tex) return;
    if (st.icon) SDL_DestroyTexture(st.icon);
    st.icon = tex;
    std::ifstream in(std::filesystem::u8path(png), std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), {});
    int w = 0, h = 0, n = 0;
    if (unsigned char* px = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4)) {
        if (SDL_Surface* surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, px, w * 4)) {
            SDL_SetWindowIcon(st.window, surface);
            SDL_DestroySurface(surface);
        }
        stbi_image_free(px);
    }
}

void choose(State& st, const std::string& chosen) {
    if (chosen.empty()) return;
    st.core.root = chosen;
    st.core.copies.clear();
    load_icon(st);
    st.core.recheck();
}

void write_dump(State& st) {
    st.settled = true;
    const char* path = std::getenv("AMG_LAUNCHER_DUMP");
    if (path && *path) st.core.write_dump(path);
    // Test hook: AMG_LAUNCHER_AUTOSTART presses Start by itself once the folder is usable (a check of what happens after the launcher closes).
    if (std::getenv("AMG_LAUNCHER_AUTOSTART") && st.core.view().start_enabled) {
        st.core.result = st.core.root;
        st.running = false;
    }
}

void react(State& st, const Core::Event& e) {
    switch (e.kind) {
        case Core::Kind::Checked:
            if (!usable(st.core.checks)) st.core.search_copies_once();
            if (usable(st.core.checks) || !st.spec->find_copies) write_dump(st);
            break;
        case Core::Kind::Found:
            write_dump(st);
            break;
        case Core::Kind::Updated:
            if (e.text.empty()) {
                if (!(st.spec->updates.restart && st.spec->updates.restart())) dialog::show(ui::tr("The update is installed. Start the program again to use it."));
                st.running = false;
            } else if (dialog::show(e.text + "\n\n" + ui::tr("Open the download page instead?"), dialog::Buttons::YesNo, dialog::Icon::Warning) == dialog::Reply::Yes) {
                SDL_OpenURL(ui::narrow(st.spec->updates.page).c_str());
            }
            break;
        default:
            break;
    }
}

// One setting row: a label and a drop-down list; a change is applied at once, as in the Win32 window.
void row(State& st, const std::string& key, float label_w, float combo_w, bool with_label = true) {
    SettingRow r = row_of(*st.spec, st.core.root, key);
    if (with_label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ui::tr(r.label).c_str());
        ImGui::SameLine(label_w);
    }
    ImGui::SetNextItemWidth(combo_w);
    const std::string shown = r.index >= 0 && size_t(r.index) < r.names.size() ? ui::tr(r.names[size_t(r.index)]) : std::string();
    if (ImGui::BeginCombo(("##" + key).c_str(), shown.c_str())) {
        for (size_t i = 0; i < r.names.size(); ++i)
            if (ImGui::Selectable(ui::tr(r.names[i]).c_str(), int(i) == r.index) && int(i) != r.index) {
                const std::string& value = r.values[i];
                if (st.spec->variants && key == st.spec->variant_key) st.spec->write_setting(key, value);
                else if (key == "theme") st.spec->write_setting("theme", value == "auto" ? "" : value), st.theme_poll = -1;
                else if (key == "ui_lang") {
                    st.spec->write_setting("ui_lang", value);
                    ui::init(value);
                } else if (st.spec->set_row) st.spec->set_row(key, value);
            }
        ImGui::EndCombo();
    }
}

void rows_page(State& st, const std::vector<std::string>& keys) {
    const float label_w = 190.f * st.s, combo_w = 220.f * st.s;
    for (const std::string& k : keys) row(st, k, label_w, combo_w);
}

bool button(const std::string& text, float width = 0.f) { return ImGui::Button(ui::tr(text).c_str(), ImVec2(width, 0)); }

void game_files_tab(State& st, const ViewModel& v) {
    const float right_w = 230.f * st.s;
    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("checks", ImVec2(avail - right_w - 16.f * st.s, 0), ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::string folder = st.core.root.empty() ? ui::tr("No folder chosen yet.") : ui::fmt("Folder: {0}", {st.core.root});
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(MUTED, "%s", folder.c_str());
    ImGui::Spacing();
    if (st.core.checking) {
        ImGui::TextColored(MUTED, "%s", ui::tr("Checking your game folder...").c_str());
    } else {
        const float m = ImGui::GetTextLineHeight();
        for (const Check& c : st.core.checks) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            draw_mark(dl, p, m, c.status);
            ImGui::SetCursorScreenPos(ImVec2(p.x + m + 8.f * st.s, p.y));
            ImGui::BeginGroup();
            ImGui::TextUnformatted(ui::tr(c.what).c_str());
            ImGui::TextColored(MUTED, "%s", c.detail.c_str());
            ImGui::EndGroup();
            ImGui::Spacing();
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted(ui::tr("Language").c_str());
    if (st.spec->variants) row(st, st.spec->variant_key, 0.f, right_w, false);
    ImGui::Spacing();
    ImGui::Spacing();
    if (button("Choose game folder...", right_w) && st.spec->choose_folder) choose(st, st.spec->choose_folder());
    if (st.spec->add_variant && button(st.spec->add_variant_label, right_w)) {
        if (st.spec->add_variant(st.core.root, "")) st.core.recheck();
    }
    ImGui::Spacing();
    if (button("Open saves folder", right_w) && st.spec->saves_dir) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(st.spec->saves_dir()), ec);
        SDL_OpenURL(("file:///" + st.spec->saves_dir()).c_str());
    }
    if (st.spec->desktop_shortcut && !st.core.root.empty() && button(st.spec->desktop_shortcut_label, right_w)) {
        std::string result;
        const bool ok = st.spec->desktop_shortcut(st.core.root, result);
        st.dialogs.set_owner(st.window);
        dialog::show(ok ? ui::fmt("The shortcut is on your desktop:\n{0}", {result}) : ui::fmt("The shortcut could not be made: {0}", {result}),
                     dialog::Buttons::Ok, ok ? dialog::Icon::Info : dialog::Icon::Warning);
        st.dialogs.set_owner(nullptr);
    }
    if (button("Delete copy of the game...", right_w)) {
        st.dialogs.set_owner(st.window);
        if (delete_copy(*st.spec) == DeleteResult::Deleted) {
            st.core.root = st.spec->read_setting("root");
            st.core.searched = false;
            load_icon(st);
            st.core.recheck();
        }
        st.dialogs.set_owner(nullptr);
    }
    ImGui::EndGroup();
    (void)v;
}

void frame(State& st) {
    ImGuiIO& io = ImGui::GetIO();
    refresh_theme(st, false);
    const float s = st.s;
    const ViewModel v = st.core.view();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float W = io.DisplaySize.x;
    const float start_w = std::max(160.f * s, ImGui::CalcTextSize(ui::tr("Start game").c_str()).x + 40.f * s);

    // The header: the game's picture, its name, the status, and the buttons that matter now.
    if (st.icon) {
        ImGui::SetCursorPos(ImVec2(20 * s, 18 * s));
        ImGui::Image((ImTextureID)(intptr_t)st.icon, ImVec2(48 * s, 48 * s));
    }
    ImGui::SetCursorPos(ImVec2(84 * s, 8 * s));
    ImGui::PushFont(nullptr, 26.f * s);
    ImGui::TextUnformatted(st.spec->name.c_str());
    ImGui::PopFont();
    const std::string product = st.spec->product + " " + (st.spec->version ? st.spec->version() : std::string());
    ImGui::SetCursorPos(ImVec2(W - 24 * s - ImGui::CalcTextSize(product.c_str()).x, 18 * s));
    ImGui::TextColored(MUTED, "%s", product.c_str());
    const float line = ImGui::GetTextLineHeightWithSpacing();
    draw_mark(dl, ImVec2(84 * s + ImGui::GetWindowPos().x, ImGui::GetWindowPos().y + 54 * s), 16 * s, v.status);
    ImGui::SetCursorPos(ImVec2(108 * s, 52 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##status", ImVec2(W - start_w - 48 * s - 108 * s, 2 * ImGui::GetTextLineHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(v.status_text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    float x = 84 * s;
    const float y = 52 * s + 2 * line + 10 * s;
    auto header_button = [&](const std::string& label, bool visible, bool& pressed) {
        if (!visible) return;
        ImGui::SetCursorPos(ImVec2(x, y));
        const float w = ImGui::CalcTextSize(label.c_str()).x + 32 * s;
        pressed = ImGui::Button((label + "##header").c_str(), ImVec2(w, 30 * s));
        x += w + 10 * s;
    };
    bool fix = false, found = false, upd = false;
    header_button(v.fix_label, v.fix_visible, fix);
    header_button(v.found_label, v.found_visible, found);
    header_button(v.update_label, v.update_visible, upd);
    if (fix && st.spec->choose_folder) choose(st, st.spec->choose_folder());
    if (found && !st.core.copies.empty() && st.spec->use_copy) choose(st, st.spec->use_copy(st.core.copies.front()));
    if (upd && !st.core.updating && !st.core.update_tag.empty() &&
        dialog::show(ui::fmt("Download and install {0} now? The program restarts when it is done.", {st.core.update_tag}), dialog::Buttons::YesNo, dialog::Icon::Question) == dialog::Reply::Yes)
        st.core.install_update();
    ImGui::SetCursorPos(ImVec2(W - 24 * s - start_w, 48 * s));
    ImGui::BeginDisabled(!v.start_enabled);
    ImGui::PushStyleColor(ImGuiCol_Button, START);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, START_HOVER);
    ImGui::PushStyleColor(ImGuiCol_Text, vec(255, 255, 255));
    const bool start = ImGui::Button((ui::tr("Start game") + "##start").c_str(), ImVec2(start_w, 48 * s));
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    const float header_h = (52 + 30 + 14) * s + 2 * line + 10 * s;
    dl->AddLine(ImVec2(0, header_h), ImVec2(W, header_h), LINE);

    // The tabs.
    ImGui::SetCursorPos(ImVec2(12 * s, header_h + 8 * s));
    ImGui::BeginChild("##pane", ImVec2(W - 24 * s, io.DisplaySize.y - header_h - 20 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14 * s, 6 * s));
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Tab)) st.tab_request = (st.tab + (io.KeyShift ? 3 : 1)) % 4;  // Ctrl+Tab, Ctrl+Shift+Tab
    auto item = [&](int index, const std::string& label) {
        if (!ImGui::BeginTabItem(label.c_str(), nullptr, st.tab_request == index ? ImGuiTabItemFlags_SetSelected : 0)) return false;
        st.tab = index;
        return true;
    };
    if (ImGui::BeginTabBar("##tabs")) {
        if (item(0, ui::tr("Game files"))) {
            game_files_tab(st, v);
            ImGui::EndTabItem();
        }
        if (item(1, ui::tr("General"))) {
            {
                std::vector<std::string> keys = general_of(*st.spec);
                keys.push_back("theme");
                rows_page(st, keys);
            }
            ImGui::EndTabItem();
        }
        if (item(2, ui::tr("Display"))) {
            rows_page(st, st.spec->display_keys);
            ImGui::EndTabItem();
        }
        if (item(3, ui::tr("Controls"))) {
            rows_page(st, st.spec->controls_keys);
            ImGui::Spacing();
            if (st.spec->show_controls && button("Controls...", 180.f * s)) st.spec->show_controls(nullptr, nullptr);
            if (st.spec->show_pad_mapping && button("Pad mapping...", 180.f * s)) st.spec->show_pad_mapping(nullptr, nullptr);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    st.tab_request = -1;
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::End();

    // Keys: Enter starts, Esc closes (unless a drop-down is open).
    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if ((start || (!popup && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) && v.start_enabled) {
        st.core.result = st.core.root;
        st.running = false;
    }
    if (!popup && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) st.running = false;
}

void style(float s) {
    ImGuiStyle& g = ImGui::GetStyle();
    g = ImGuiStyle();
    if (BACKGROUND.w == 0) apply_palette(false);
    ImGui::StyleColorsLight();
    g.WindowRounding = 0;
    g.FrameRounding = 4 * s;
    g.ChildRounding = 4 * s;
    g.TabRounding = 4 * s;
    g.GrabRounding = 4 * s;
    g.ScaleAllSizes(s);
    ImVec4* c = g.Colors;
    c[ImGuiCol_WindowBg] = BACKGROUND;
    c[ImGuiCol_ChildBg] = PANE;
    c[ImGuiCol_Text] = INK;
    c[ImGuiCol_Border] = BORDER;
    c[ImGuiCol_Button] = BUTTON;
    c[ImGuiCol_ButtonHovered] = BUTTON_HOVER;
    c[ImGuiCol_ButtonActive] = BUTTON_ACTIVE;
    c[ImGuiCol_FrameBg] = FRAME;
    c[ImGuiCol_FrameBgHovered] = BUTTON_HOVER;
    c[ImGuiCol_FrameBgActive] = BUTTON_ACTIVE;
    c[ImGuiCol_PopupBg] = PANE;
    c[ImGuiCol_Header] = BUTTON;
    c[ImGuiCol_HeaderHovered] = BUTTON_HOVER;
    c[ImGuiCol_HeaderActive] = BUTTON_ACTIVE;
    c[ImGuiCol_Tab] = TAB;
    c[ImGuiCol_TabHovered] = TAB_HOVER;
    c[ImGuiCol_TabSelected] = PANE;
    c[ImGuiCol_TextDisabled] = MUTED;
}

}  // namespace

std::string run_sdl(const ProjectSpec& spec, const std::string& root, bool* started) {
    if (started) *started = false;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return "";
    State st;
    st.spec = &spec;
    st.core.spec = &spec;
    st.core.root = root;
    float scale = 1.0f;
    if (const char* e = std::getenv("AMG_LAUNCHER_SCALE"); e && std::atof(e) > 0) scale = float(std::atof(e) / 100.0);  // percent, for tests
    else if (const char* d = std::getenv("AMG_LAUNCHER_DPI"); d && std::atoi(d) > 0) scale = float(std::atoi(d)) / 96.f;
    else {
        const SDL_DisplayID id = SDL_GetPrimaryDisplay();
        const float ds = id ? SDL_GetDisplayContentScale(id) : 1.0f;
        if (ds > 0) scale = ds;
    }
    st.s = scale;
    if (!SDL_CreateWindowAndRenderer(spec.name.c_str(), int(760 * scale), int(520 * scale), SDL_WINDOW_RESIZABLE, &st.window, &st.renderer)) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return "";
    }
    if (started) *started = true;
    if (const char* pos = std::getenv("AMG_LAUNCHER_POS")) {  // development: off screen, without taking the focus
        int x = 0, y = 0;
        if (std::sscanf(pos, "%d,%d", &x, &y) == 2) SDL_SetWindowPosition(st.window, x, y);
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyNext = 0;  // Ctrl+Tab is the tabs', not the windows'
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyPrev = 0;
    st.dark = want_dark(st);
    apply_palette(st.dark);
    style(scale);
    const std::string font = find_font();
    if (!font.empty()) io.Fonts->AddFontFromFileTTF(font.c_str(), 16.0f * scale);
    ImGui_ImplSDL3_InitForSDLRenderer(st.window, st.renderer);
    ImGui_ImplSDLRenderer3_Init(st.renderer);

    st.dialogs.title = spec.name;
    dialog::Service* previous_dialogs = &dialog::current();
    if (!std::getenv("AMG_LAUNCHER_UI")) dialog::set(&st.dialogs);  // a script chosen by the environment keeps answering
    load_icon(st);
    st.core.wake = [] {
        SDL_Event e = {};
        e.type = SDL_EVENT_USER;
        SDL_PushEvent(&e);
    };
    st.core.check_for_update();
    st.core.recheck();

    while (st.running) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 33)) {
            do {
                ImGui_ImplSDL3_ProcessEvent(&e);
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) st.running = false;
            } while (SDL_PollEvent(&e));
        }
        for (const Core::Event& ev : st.core.pump()) react(st, ev);
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        frame(st);
        ImGui::Render();
        SDL_SetRenderDrawColor(st.renderer, Uint8(CLEAR_R), Uint8(CLEAR_G), Uint8(CLEAR_B), 255);
        SDL_RenderClear(st.renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), st.renderer);
        // Test hook: AMG_LAUNCHER_SHOT=FILE.bmp saves what is drawn a few frames after the launcher settled.
        if (const char* shot = std::getenv("AMG_LAUNCHER_SHOT"); shot && *shot && st.settled && ++st.frames_settled == 15) {
            if (SDL_Surface* surface = SDL_RenderReadPixels(st.renderer, nullptr)) {
                SDL_SaveBMP(surface, shot);
                SDL_DestroySurface(surface);
            }
        }
        SDL_RenderPresent(st.renderer);
    }

    dialog::set(previous_dialogs);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (st.icon) SDL_DestroyTexture(st.icon);
    SDL_DestroyRenderer(st.renderer);
    SDL_DestroyWindow(st.window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return st.core.result;
}

}  // namespace amg::launcher
