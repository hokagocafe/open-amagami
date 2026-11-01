// The words of the program's own windows (the launcher, its dialogs, the messages of the game window), in English,
// Spanish and Japanese. The English text is the key: `tr("Start game")` gives the text in the language in use, or the
// English one if there is no translation. A text with values has `{0}`, `{1}` where they go (`fmt`).
#pragma once

#include <initializer_list>
#include <string>

namespace amg::ui {

enum class Lang { En, Es, Ja };

// `setting` is the `ui_lang` setting: "" (follow Windows), "en", "es" or "ja".
void init(const std::string& setting);
Lang language();

std::string tr(const std::string& english);                     // UTF-8
std::wstring wtr(const std::string& english);                   // for the Windows calls
std::string fmt(const std::string& english, std::initializer_list<std::string> values);   // translated, values put in
std::wstring wfmt(const std::string& english, std::initializer_list<std::string> values);
std::wstring wide(const std::string& utf8);                     // UTF-8 (or, if it is not, the system code page) to UTF-16
std::string narrow(const std::wstring& text);                   // UTF-16 to UTF-8

}  // namespace amg::ui
