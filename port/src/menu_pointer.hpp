// The mouse in the game's menus (F5-8): it plays them like a person with a pad, from what is on the screen.
//
// A menu of the game draws its entries as sprites and a cursor sprite (the interface picture 20) on the same layer, and moves
// the cursor when it is sent up, down, left or right. The pointer reads that picture, finds the entry under the mouse and the
// entry the cursor is on, and presses the directions until they are the same; a click presses confirm, a right click cancel,
// the wheel up and down. The scripts are not touched, so every menu that has the cursor works.
#pragma once

#include "gfx.hpp"

namespace amg {

// The pictures the pointer knows by identity (null when the game's data has not been read yet).
struct MenuPictures {
    ImagePtr cursor;  // the menu cursor, interface picture 20
    ImagePtr tab;     // a tab of the Settings screen (47)
    ImagePtr note;    // a note of a volume row of the Settings screen (33)
    ImagePtr map_cursor;  // one of the four corners of the cursor of the day map (25)
    ImagePtr map_arrow[4];  // the guide arrows around it: up, down, left, right (28 to 31)
    ImagePtr arrow;   // an arrow of a speed row of the Settings screen (7)
    ImagePtr back;    // the word "back" of the hint at the top right and in the Settings screen (62, variant 2)
};

class MenuPointer {
public:
    // Once per frame, before the buttons are read. `scene` is the last picture shown, `cursor` the cursor's image, the mouse is
    // at (mx, my) in script units from the middle of the screen, `inside` says it is over the picture. Returns the buttons to
    // hold for this frame, in the engine's mask (0 when there is no menu on the screen or nothing to do).
    // `down` is the left button held (a drag), `click` its first frame.
    int update(const Scene& scene, const MenuPictures& pics, double mx, double my, bool inside, bool down, bool click,
               bool right_click, int wheel_steps);

    // What the last update saw, for the headless log (--mouse with AMG_MOUSE_LOG): the screen kind ('-' none, 'm' a menu
    // with the cursor sprite, 'n' the name grid, 's' Settings, 'd' the day map, 'g' the gallery grid), the entry the game's
    // cursor is on and the one under the mouse (has_* false when there is none), and whether they are the same.
    struct Probe {
        char kind = '-';
        bool has_sel = false, has_tgt = false, on = false;
        double sel_x = 0, sel_y = 0, tgt_x = 0, tgt_y = 0;
    } probe;

    // The keyboard or a pad was used: the pointer drops what it was still to press, so they take over at once.
    void yield();

private:
    // The places and distances this file knows by number (the Settings panel, the Decide button, the day map's area) are in
    // the layout of the game's screen; the interface-size shrink moves them as it moves the pictures. X, Y and D give them
    // where the player sees them: the scene's shrink (Scene::ui_*) when the screen's pictures are shrunk, else unchanged.
    double u_ = 1.0, ax_ = 0.0, ay_ = 0.0;
    double X(double x) const { return ax_ + (x - ax_) * u_; }
    double Y(double y) const { return ay_ + (y - ay_) * u_; }
    double D(double d) const { return d * u_; }
    void frame_of(const Scene& scene, const struct DrawItem* ref);
    // The presses still to make toward the entry under the mouse: one every second frame (a frame held, a frame free, which is
    // as fast as the scripts can tell two presses apart: they read `ControlPad.trig`/`rapid` once a frame, and a press is new
    // only on the frame it starts), first `queue_` of `queue_dir_`, then `queue2_` of `queue2_dir_`.
    int queue_dir_ = 0, queue_ = 0, queue2_dir_ = 0, queue2_ = 0;
    bool held_ = false;      // the last frame held a queued press (this one must be free)
    int settle_ = 0;         // frames to wait after the last press before the picture is trusted again
    double rest_x_ = 1e9, rest_y_ = 1e9;  // the game's cursor last frame: it is still when it has not moved since
    bool busy() const { return queue_ > 0 || queue2_ > 0 || held_ || settle_ > 0; }
    int next_press(int settle_frames);
    void plan(int dir, int n, int dir2 = 0, int n2 = 0);
    bool still(double x, double y);
    double plan_x_ = 0, plan_y_ = 0;  // the entry (or the gallery's mouse) the planned presses go to
    double last_x_ = 0, last_y_ = 0;
    bool tracking_ = false;      // the mouse moved onto an entry and the cursor is still on its way
    bool confirm_pending_ = false;
    int phase_ = 0;
    int map_update(const Scene& scene, const MenuPictures& pics, double cx, double cy, double mx, double my, bool inside, bool click,
                   bool gallery);
    int settings_update(const Scene& scene, const MenuPictures& pics, const struct DrawItem* cur, double mx, double my, bool inside,
                        bool down, bool click, int wheel_steps);
    // The Settings screen: what a click or a drag on a row or a tab is still to do.
    int act_ = 0;            // 0 nothing, 1 slider to a value, 2 press a direction, 3 confirm, 4 change tab
    double act_y_ = 0;       // the row the action belongs to
    int act_dir_ = 0;        // the direction of act 2
    int act_goal_ = -1;      // the value of act 1, the tab of act 4
    bool dragging_ = false;  // the left button went down on a slider and is still down
    bool map_goal_ = false;  // the day map: the cursor is on its way to the mouse
    bool map_confirm_ = false;
    double map_x_ = 0, map_y_ = 0, map_best_ = 1e9;
    int map_stall_ = 0;
    int cooldown_ = 0;       // frames to wait after changing a tab
    int start_hold_ = 0;         // frames left of the Start press a click on the name entry's Decide button makes
};

}  // namespace amg
