// The mouse where no native adapter (mouse_menus.hpp) holds the screen: the "no menu" rule of the native mouse
// (docs/technical/mouse-native-design.md, 2.5 and section 6). A click goes on (confirm), the right button goes back
// (cancel), and a click on the "back" hint (the word at the top right of a menu) goes back. Every menu of the game has
// its adapter, so this is what is left for dialogue, the title screen, the gallery's viewer and any screen not read yet.
// (Until 2026-11-14 this class also played the menus by pressing directions in bursts toward the entry under the mouse;
// that went away with the last adapters, step 11.)
#pragma once

#include "gfx.hpp"

namespace amg {

// The pictures the pointer knows by identity (null when the game's data has not been read yet).
struct MenuPictures {
    ImagePtr cursor;  // the menu arrow, interface picture 20 (the arrow filter in mouse mode, the pointer's art)
    ImagePtr back;    // the word "back" of the hint at the top right of a menu (62, variant 2)
};

class MenuPointer {
public:
    // Once per frame, when no adapter took the screen. `scene` is the last picture shown, the mouse at (mx, my) in script
    // units from the middle, `inside` over the picture. Returns the buttons to hold for this frame (engine mask).
    int update(const Scene& scene, const MenuPictures& pics, double mx, double my, bool inside, bool down, bool click,
               bool right_click, int wheel_steps);

    // What the last update saw, for the headless log (--mouse with AMG_MOUSE_LOG), and the native adapters' report in the
    // same terms: the screen kind ('-' none), the entry the game's cursor is on and the one under the mouse (has_* false
    // when there is none), and whether they are the same.
    struct Probe {
        char kind = '-';
        bool has_sel = false, has_tgt = false, on = false;
        double sel_x = 0, sel_y = 0, tgt_x = 0, tgt_y = 0;
    } probe;

    // The keyboard or a pad was used (nothing is pending here; kept for the callers' symmetry with MouseMenus).
    void yield() {}
};

}  // namespace amg
