// The mouse in the game's menus (F5-8): it plays them like a person with a pad, from what is on the screen.
//
// A menu of the game draws its entries as sprites and a cursor sprite (the interface picture 20) on the same layer, and moves
// the cursor when it is sent up, down, left or right. The pointer reads that picture, finds the entry under the mouse and the
// entry the cursor is on, and presses the directions until they are the same; a click presses confirm, a right click cancel,
// the wheel up and down. The scripts are not touched, so every menu that has the cursor works.
#pragma once

#include "gfx.hpp"

namespace amg {

class MenuPointer {
public:
    // Once per frame, before the buttons are read. `scene` is the last picture shown, `cursor` the cursor's image, the mouse is
    // at (mx, my) in script units from the middle of the screen, `inside` says it is over the picture. Returns the buttons to
    // hold for this frame, in the engine's mask (0 when there is no menu on the screen or nothing to do).
    int update(const Scene& scene, const ImagePtr& cursor, double mx, double my, bool inside, bool click, bool right_click,
               int wheel_steps);

private:
    double last_x_ = 0, last_y_ = 0;
    bool tracking_ = false;      // the mouse moved onto an entry and the cursor is still on its way
    bool confirm_pending_ = false;
    int phase_ = 0;
};

}  // namespace amg
