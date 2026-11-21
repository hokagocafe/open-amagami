// The questions and notices the program puts to the player (docs/technical/launcher-component.md, the UI service). Code that needs
// to tell or ask something calls `dialog::show`; what answers is chosen once by the program:
//   - a window system's message box (dialog_win32.cpp),
//   - a terminal (`make_console`): the message is printed, a question is answered by a flag or asked on the terminal,
//   - a script (`make_scripted`): the answers come from a list, and each box is written to a log (for the launcher's outcome check).
// No system header here.
#pragma once

#include <memory>
#include <string>

namespace amg::dialog {

enum class Buttons { Ok, YesNo, YesNoCancel };
enum class Icon { Info, Question, Warning, Error };
enum class Reply { Ok, Yes, No, Cancel };

class Service {
public:
    virtual ~Service() = default;
    // Shows `message` (UTF-8, may hold line breaks) and returns what the player answered. `default_no`: the safe answer is the default one.
    virtual Reply show(const std::string& message, Buttons buttons, Icon icon, bool default_no) = 0;
    // The window the next boxes are modal to (a backend's native handle; nullptr for none). Services without windows ignore it.
    virtual void set_owner(void*) {}
};

// The service in use. Not owned; the program sets it first thing and it must outlive every call.
void set(Service* service);
Service& current();

inline Reply show(const std::string& message, Buttons buttons = Buttons::Ok, Icon icon = Icon::Info, bool default_no = false) {
    return current().show(message, buttons, icon, default_no);
}

// A terminal: notices go to standard output (warnings and errors to standard error); a question is answered by `forced` (1 yes,
// 0 no) when it is not -1, else asked on the terminal, and when there is no terminal the safe answer (no, cancel) is taken.
std::unique_ptr<Service> make_console(int forced);

// A script: `answers` is a comma-separated list ("ok,yes,no"), one per box in the order they appear (an answer that does not fit
// the box becomes its first button; with the list used up, the first button). Every box is appended to `log_path` as
// "message | buttons | answer" with its line breaks turned into spaces.
std::unique_ptr<Service> make_scripted(const std::string& answers, const std::string& log_path);

}  // namespace amg::dialog
