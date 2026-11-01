// The native mouse in the game's menus (docs/technical/mouse-native-design.md, steps 3 and 4): the menus the port knows
// by their script objects, not by their pictures.
//
// A registered menu is found each frame from the script threads' stacks: a thread whose innermost frame is the owner's
// pad loop (MainMenu.scriptMain, GameMenu.scriptMain, GameMenuMap.selectMenu) has its CommandMenu in the owner's `menu`
// field. Its entries are the sprites of CommandMenu.icon, found in the picture by their screen object. When the entry
// under the mouse changes, one short script thread (port/script/PortMouse.scfs) moves the selection there in one step
// with the menu's own CommandMenu.moveCursor(i), plays the owner's move sound and writes the owner's copy of the index:
// the same as one pad move, whatever the distance. A click on the selected entry is confirm for one frame, a right
// click cancel. Screens with no registered menu are left to MenuPointer (the press bursts) until their adapters exist.
#pragma once

#include <functional>

#include "gfx.hpp"
#include "menu_pointer.hpp"

namespace amg {

class MouseMenus {
public:
    // Once per frame, before MenuPointer. `seen` is Graphics::pointer_scene of the last picture, (mx, my) the mouse in
    // its script units. Returns false when no registered menu is on a stack (the caller uses MenuPointer); else true and
    // `mask` is what the mouse presses this frame (engine mask).
    // `down` is the left button held (a slider's drag), `click` its first frame.
    bool update(VM& vm, const Scene& seen, double mx, double my, bool inside, bool down, bool click, bool right_click,
                int wheel_steps, int& mask);
    // The keyboard or a pad was used: a pending confirm is dropped and hover waits for the mouse to move again.
    void yield();
    // The screen object of the found menu's arrow (CommandMenu.cursor), 0 when there is none: hidden in mouse mode.
    int64_t arrow_uid = 0;
    // The port's mark for the selected row, drawn in place of that arrow in mouse mode where the arrow is the only mark of
    // the selection (Settings, the port's options; h 0 elsewhere: the lit entry is the mark). Graphics::mark takes it.
    Graphics::SelectionMark mark;
    // What update saw, in MenuPointer's terms (kind 'c'), for AMG_MOUSE_LOG.
    MenuPointer::Probe probe;

private:
    double last_x_ = -1e9, last_y_ = -1e9;  // far away: the first place counts as a move; 1e9 (after yield) does not
    int asked_ = -1;          // the entry a select thread was started for, until the menu's target is it
    int asked_age_ = 0;
    int confirm_for_ = -1;    // a click on an entry not yet selected: confirm once the menu's index is on it
    int free_frames_ = 0;     // after a confirm or a cancel, frames with nothing pressed
    char screen_ = 0;         // the adapter of the last frame: 'c' CommandMenu, 's' Settings, 'n'/'g' the name entry
    int drag_row_ = -1, drag_goal_ = -1;  // Settings: the slider the button went down on, and the value last asked
    int asked_cx_ = -1, asked_cy_ = -1, asked_cage_ = 0;  // the name grid: the cell a thread was started for
    int confirm_cx_ = -1, confirm_cy_ = -1;  // a click on a cell not yet selected: confirm once the cursor is there
    int pending_cx_ = -1, pending_cy_ = -1;  // a cell clicked while the categories had the cursor
    int pending_cat_ = -1, pending_age_ = 0;  // a category clicked while the grid had the cursor
    bool settings_update(VM& vm, const Scene& seen, const Value& dialog, double mx, double my, bool inside, bool down,
                         bool click, bool right_click, int wheel_steps, int& mask);
    bool drive(int hover, int cur, int n, double mx, double my, bool click, bool right_click, int wheel_steps,
               const std::function<bool(int)>& select, int& mask);
    bool choice_update(VM& vm, const Scene& seen, const Value& sel, double mx, double my, bool inside, bool click,
                       bool right_click, int wheel_steps, int& mask);
    bool topic_update(VM& vm, const Scene& seen, const Value& panel, const Value& target, double mx, double my, bool inside,
                      bool click, bool right_click, int wheel_steps, int& mask);
    bool shiori_update(VM& vm, const Scene& seen, const Value& owner, const Value& favor, const Value& time, double mx,
                       double my, bool inside, bool click, bool right_click, int wheel_steps, int& mask);
    bool gallery_update(VM& vm, const Scene& seen, const Value& gallery, const Value& cursor, double mx, double my,
                        bool inside, bool click, bool right_click, int wheel_steps, int& mask);
    bool name_update(VM& vm, const Scene& seen, const Value& owner, bool grid, double mx, double my, bool inside, bool click,
                     bool right_click, int wheel_steps, int& mask);
};

// PortMouse.mirror(i) (natives.cpp): writes i into the owner's copy of the index (MainMenu.cursor, GameMenu's local)
// for the menu the last select thread was started for, if that owner's loop frame is still there.
void mouse_menus_mirror(VM& vm, const Value& index);

}  // namespace amg
