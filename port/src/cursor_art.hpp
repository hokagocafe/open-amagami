// The port's mouse cursor (F5-21, mouse-native-design section 3): the game's own menu arrow (interface picture 20),
// turned to point up-left, made at run time at the size of the picture on the screen. Until the game's data is read it
// is a drawn arrow. The window turns it into the system cursor (it is never part of the picture, so no capture of the
// picture holds it); AMG_MOUSE_CURSOR_IN_SHOTS=1 stamps it on the captures too (--shot, AMG_SHOTS, F9).
#pragma once

#include <cstdint>
#include <vector>

namespace amg::cursor_art {

// The cursor's height in pixels for a picture `view_h` pixels high: 24 at the Vita's 544, never under 16.
int size_for(int view_h);
// The game's arrow as read from the data: `w` by `h` RGBA with the PS2's alpha (0..128), pointing right. Any thread;
// a copy is kept. Returns true when it is new (the window then makes its cursor again).
bool set_source(int w, int h, const uint8_t* rgba);
// Whether the game's arrow has been read yet (until then the window shows no cursor rather than a stand-in).
bool has_source();
// The cursor, `size` by `size` RGBA (straight alpha), and its hot spot (the tip).
struct Art {
    int size = 0, hot_x = 1, hot_y = 1;
    std::vector<uint8_t> rgba;
};
Art make(int size);
// Draws the cursor over an RGB picture (`w` by `h`), its tip at (x, y).
void stamp(std::vector<uint8_t>& rgb, int w, int h, int x, int y, int size);
// Whether captures should hold the cursor (AMG_MOUSE_CURSOR_IN_SHOTS=1).
bool in_shots();

}  // namespace amg::cursor_art
