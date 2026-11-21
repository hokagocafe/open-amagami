// The command line of the launcher component (docs/technical/launcher-component.md): the first front end. Everything the launcher
// window does is a command here, with text a script can read and exit codes, and `start` hands the folder back so the program starts the
// game. No system header: the program decides when to call it (a first argument that names a command) and where output goes.
#pragma once

#include <string>
#include <vector>

#include "launcher_spec.hpp"

namespace amg::launcher {

struct CliResult {
    bool handled = false;     // the first argument was a command
    int exit_code = 0;
    bool start_game = false;  // `start`: run the game from `root`
    std::string root;
};

// Whether `word` names a command (help, status, check, settings, find, use, add-variant, delete-copy, update, start).
bool is_cli_command(const std::string& word);

// Whether `word` is meant as a command (only letters and dashes, not starting with a dash), so that a mistyped one is an error and
// not a window opening unasked. A path (a folder dropped on the program) has more characters and is not.
bool looks_like_command(const std::string& word);

// Runs the command in `args` (the arguments after the program's name). The program sets the dialog service first (`dialog::make_console`
// with `cli_forced_answer(args)`): the flags `--yes`, `--no`, `--import` (yes) and `--in-place` (no) answer the questions, else they are
// asked on the terminal.
// 1 for --yes or --import, 0 for --no or --in-place, -1 when neither is given.
int cli_forced_answer(const std::vector<std::string>& args);

CliResult run_cli(const ProjectSpec& spec, const std::vector<std::string>& args);

}  // namespace amg::launcher
