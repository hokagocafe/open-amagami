#include "launcher_cli.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "dialog.hpp"
#include "launcher_actions.hpp"
#include "ui_text.hpp"

namespace amg::launcher {

namespace {

const char* const COMMANDS[] = {"help", "status", "check", "settings", "find", "use", "add-variant", "delete-copy", "shortcut", "update", "start"};

void usage(std::FILE* to, const ProjectSpec& spec) {
    std::fprintf(to,
        "%s %s - commands (every step of the launcher; the window is only one front end)\n"
        "  help                          this text\n"
        "  status                        the remembered folder, whether it is usable, the program's own copy, the saves folder\n"
        "  check [FOLDER]                check a folder (default: the remembered one); exit 0 usable, 1 not\n"
        "  settings list                 every setting as key=value, with its choices\n"
        "  settings get KEY\n"
        "  settings set KEY VALUE        VALUE must be one of the choices\n"
        "  find                          list the usable copies of the game found on this machine; exit 0 if any\n"
        "  use FOLDER [--import|--in-place]   check a folder, copy it into the program's folder or use it where it is, and remember it\n"
        "  add-variant [FOLDER]          add the data variant (the English patch) to the remembered folder, from FOLDER\n"
        "  delete-copy [--yes]           delete the program's own copy of the game\n"
        "  shortcut                      put a shortcut to the program, with the game's icon, on the desktop\n"
        "  update check | install [--yes]\n"
        "  start                         start the game from the remembered folder\n"
        "Questions are answered by --yes, --no, --import (yes) and --in-place (no), or asked on the terminal.\n"
        "Exit codes: 0 done, 1 the answer is no or it failed, 2 wrong use.\n",
        spec.name.c_str(), spec.version ? spec.version().c_str() : "");
}

struct Parsed {
    std::vector<std::string> positional;
    int forced = -1;  // 1 yes, 0 no
};

Parsed parse(const std::vector<std::string>& args, size_t from) {
    Parsed p;
    for (size_t i = from; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--yes" || a == "--import") p.forced = 1;
        else if (a == "--no" || a == "--in-place") p.forced = 0;
        else p.positional.push_back(a);
    }
    return p;
}

const char* tag(Check::Status s) { return s == Check::Ok ? "ok" : s == Check::Warn ? "warn" : "fail"; }

std::string flat(std::string s) {
    for (char& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    return s;
}

// The settings as rows: the project's, its data variants, and the launcher's own language.
std::vector<SettingRow> all_rows(const ProjectSpec& spec) {
    std::vector<SettingRow> rows = spec.rows ? spec.rows() : std::vector<SettingRow>();
    if (spec.variants) rows.push_back(spec.variants(spec.read_setting("root")));
    SettingRow lang;
    lang.key = "ui_lang";
    lang.label = "Launcher language";
    lang.values = {"", "en", "es", "ja"};
    const std::string current = spec.read_setting("ui_lang");
    for (size_t i = 0; i < lang.values.size(); ++i)
        if (lang.values[i] == current) lang.index = int(i);
    rows.push_back(lang);
    return rows;
}

std::string value_of(const SettingRow& r) { return r.index >= 0 && size_t(r.index) < r.values.size() ? r.values[size_t(r.index)] : std::string(); }

bool run_check(const ProjectSpec& spec, const std::string& root, bool print) {
    const std::vector<Check> checks = spec.verify ? spec.verify(root) : std::vector<Check>();
    if (print) {
        for (const Check& c : checks) std::printf("[%s] %s: %s\n", tag(c.status), c.what.c_str(), flat(c.detail).c_str());
        std::printf("usable=%d\n", usable(checks) ? 1 : 0);
    }
    return usable(checks);
}

CliResult done(int code) {
    CliResult r;
    r.handled = true;
    r.exit_code = code;
    return r;
}

}  // namespace

bool looks_like_command(const std::string& word) {
    if (word.empty() || word[0] == '-') return false;
    return std::all_of(word.begin(), word.end(), [](char c) { return (c >= 'a' && c <= 'z') || c == '-'; });
}

int cli_forced_answer(const std::vector<std::string>& args) { return parse(args, 0).forced; }

bool is_cli_command(const std::string& word) {
    return std::find(std::begin(COMMANDS), std::end(COMMANDS), word) != std::end(COMMANDS);
}

CliResult run_cli(const ProjectSpec& spec, const std::vector<std::string>& args) {
    if (args.empty()) return {};
    if (!is_cli_command(args[0])) {
        std::fprintf(stderr, "unknown command: %s (run `help` for the list)\n", args[0].c_str());
        return done(2);
    }
    const std::string& command = args[0];
    const Parsed p = parse(args, 1);
    const std::string root = spec.read_setting("root");

    if (command == "help") {
        usage(stdout, spec);
        return done(0);
    }
    if (command == "status") {
        std::printf("program=%s %s\nroot=%s\n", spec.name.c_str(), spec.version ? spec.version().c_str() : "", root.c_str());
        std::printf("usable=%d\n", !root.empty() && spec.verify && usable(spec.verify(root)) ? 1 : 0);
        const std::string copy = spec.game_copy_dir ? spec.game_copy_dir() : std::string();
        std::printf("own_copy=%s\nsaves=%s\n", copy.c_str(), spec.saves_dir ? spec.saves_dir().c_str() : "");
        return done(0);
    }
    if (command == "check") {
        const std::string target = p.positional.empty() ? root : p.positional[0];
        if (target.empty()) {
            std::fprintf(stderr, "check: no folder given and none remembered; pass a folder or run `use FOLDER`\n");
            return done(2);
        }
        return done(run_check(spec, target, true) ? 0 : 1);
    }
    if (command == "settings") {
        const std::string sub = p.positional.empty() ? "" : p.positional[0];
        const std::vector<SettingRow> rows = all_rows(spec);
        if (sub == "list") {
            for (const SettingRow& r : rows) {
                std::string choices;
                for (size_t i = 0; i < r.values.size(); ++i) choices += (i ? "|" : "") + r.values[i];
                std::printf("%s=%s\tchoices=%s\n", r.key.c_str(), value_of(r).c_str(), choices.c_str());
            }
            return done(0);
        }
        if ((sub == "get" && p.positional.size() == 2) || (sub == "set" && p.positional.size() == 3)) {
            const std::string& key = p.positional[1];
            const auto row = std::find_if(rows.begin(), rows.end(), [&](const SettingRow& r) { return r.key == key; });
            if (row == rows.end()) {
                std::fprintf(stderr, "settings: no setting called %s (see `settings list`)\n", key.c_str());
                return done(2);
            }
            if (sub == "get") {
                std::printf("%s\n", value_of(*row).c_str());
                return done(0);
            }
            const std::string& value = p.positional[2];
            if (std::find(row->values.begin(), row->values.end(), value) == row->values.end()) {
                std::string choices;
                for (size_t i = 0; i < row->values.size(); ++i) choices += (i ? ", " : "") + (row->values[i].empty() ? std::string("(empty)") : row->values[i]);
                std::fprintf(stderr, "settings: %s is not a choice of %s; the choices are: %s\n", value.c_str(), key.c_str(), choices.c_str());
                return done(2);
            }
            if (key == "ui_lang" || (spec.variants && key == spec.variant_key)) spec.write_setting(key, value);
            else if (spec.set_row) spec.set_row(key, value);
            if (key == "ui_lang") ui::init(value);
            std::printf("%s=%s\n", key.c_str(), value.c_str());
            return done(0);
        }
        usage(stderr, spec);
        return done(2);
    }
    if (command == "find") {
        const std::vector<std::string> copies = spec.find_copies ? spec.find_copies() : std::vector<std::string>();
        for (const std::string& c : copies) std::printf("%s\n", c.c_str());
        return done(copies.empty() ? 1 : 0);
    }
    if (command == "use") {
        if (p.positional.size() != 1 || !spec.use_copy) {
            std::fprintf(stderr, "use: give the folder of the game\n");
            return done(2);
        }
        const std::string chosen = spec.use_copy(p.positional[0]);
        if (chosen.empty()) return done(1);
        std::printf("root=%s\n", chosen.c_str());
        return done(0);
    }
    if (command == "add-variant") {
        if (!spec.add_variant) {
            std::fprintf(stderr, "add-variant: this program has no data variant to add\n");
            return done(2);
        }
        return done(spec.add_variant(root, p.positional.empty() ? std::string() : p.positional[0]) ? 0 : 1);
    }
    if (command == "shortcut") {
        if (!spec.desktop_shortcut) {
            std::fprintf(stderr, "shortcut: this program makes no shortcuts\n");
            return done(2);
        }
        std::string result;
        const bool ok = spec.desktop_shortcut(root, result);
        std::fprintf(ok ? stdout : stderr, "%s\n", result.c_str());
        return done(ok ? 0 : 1);
    }
    if (command == "delete-copy") return done(delete_copy(spec) == DeleteResult::Deleted ? 0 : 1);
    if (command == "update") {
        const std::string sub = p.positional.empty() ? "" : p.positional[0];
        if (!spec.updates.latest || (sub != "check" && sub != "install")) {
            if (!spec.updates.latest) std::fprintf(stderr, "update: this program has no update source\n");
            else usage(stderr, spec);
            return done(2);
        }
        const std::string tag_name = spec.updates.latest();
        const bool newer = !tag_name.empty() && spec.updates.is_newer && spec.updates.is_newer(tag_name);
        std::printf("latest=%s newer=%d\n", tag_name.c_str(), newer ? 1 : 0);
        if (sub == "check") return done(newer ? 0 : 1);
        if (!newer) return done(1);
        if (dialog::show(ui::fmt("Download and install {0} now? The program restarts when it is done.", {tag_name}), dialog::Buttons::YesNo, dialog::Icon::Question) != dialog::Reply::Yes) return done(1);
        const std::string result = spec.updates.install(tag_name, [](const std::string& step) { std::printf("%s\n", step.c_str()); });
        if (!result.empty()) {
            std::fprintf(stderr, "update: %s\n", result.c_str());
            return done(1);
        }
        if (!(spec.updates.restart && spec.updates.restart())) std::printf("The update is installed. Start the program again to use it.\n");
        return done(0);
    }
    // start
    if (root.empty() || !run_check(spec, root, false)) {
        std::fprintf(stderr, "start: no usable game folder is remembered; run `use FOLDER` first (`check` says what is wrong)\n");
        return done(1);
    }
    CliResult r = done(0);
    r.start_game = true;
    r.root = root;
    return r;
}

}  // namespace amg::launcher
