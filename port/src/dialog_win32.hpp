// The dialog service over Windows' message box. `owner` is the window the box is modal to (nullptr: none); the window code sets and clears it.
#pragma once

#include <string>

#include "dialog.hpp"

namespace amg::dialog {

class Win32Service : public Service {
public:
    std::string title = "Game";  // the box's title
    void* owner = nullptr;       // an HWND
    bool topmost = false;
    Reply show(const std::string& message, Buttons buttons, Icon icon, bool default_no) override;
    void set_owner(void* window) override { owner = window; }
};

}  // namespace amg::dialog
