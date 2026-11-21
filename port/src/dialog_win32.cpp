#include "dialog_win32.hpp"

#include <windows.h>

#include "ui_text.hpp"

namespace amg::dialog {

Reply Win32Service::show(const std::string& message, Buttons buttons, Icon icon, bool default_no) {
    UINT flags = buttons == Buttons::Ok ? MB_OK : buttons == Buttons::YesNo ? MB_YESNO : MB_YESNOCANCEL;
    flags |= icon == Icon::Info ? MB_ICONINFORMATION : icon == Icon::Question ? MB_ICONQUESTION : icon == Icon::Warning ? MB_ICONWARNING : MB_ICONERROR;
    if (default_no) flags |= MB_DEFBUTTON2;
    if (topmost) flags |= MB_TOPMOST;
    const int answer = MessageBoxW(HWND(owner), ui::wide(message).c_str(), ui::wide(title).c_str(), flags);
    return answer == IDYES ? Reply::Yes : answer == IDNO ? Reply::No : answer == IDCANCEL ? Reply::Cancel : Reply::Ok;
}

}  // namespace amg::dialog
