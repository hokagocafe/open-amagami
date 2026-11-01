#include "menu_pointer.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace amg {

namespace {

struct Entry {
    double cx, cy, half_w, half_h;
};

// The engine's buttons.
constexpr int UP = 4096, RIGHT = 8192, DOWN = 16384, LEFT = 32768, CONFIRM = 32, CANCEL = 64, START = 2048, L1 = 4, R1 = 8;

// A row of the Settings screen: a label, then notes (a volume) or a text value, all at one height.
struct Row {
    double y = 0;
    std::vector<double> notes;  // x of each note, left to right
    int lit = 0;                // how many are bright
    double half = 9;            // half the distance between two notes
    int least = 0;              // the least value (a speed starts at one arrow)
    bool value = false;         // has a text value at the right
    bool reset = false;         // the last row, "back to the defaults"
};

// How many presses toward `to` the cursor needs from `from`: the entries' distinct places (rows when `vertical`, else columns)
// beyond `from`, up to `to`. A list or a grid moves one place per press.
int places_between(const std::vector<Entry>& entries, bool vertical, double from, double to) {
    std::vector<double> seen;
    const bool forward = to > from;
    for (const Entry& e : entries) {
        const double v = vertical ? e.cy : e.cx;
        if (forward ? !(v > from + 3 && v <= to + 3) : !(v < from - 3 && v >= to - 3)) continue;
        if (std::none_of(seen.begin(), seen.end(), [&](double w) { return std::fabs(w - v) <= 3; })) seen.push_back(v);
    }
    return int(seen.size());
}

constexpr int SETTLE = 3;  // frames after a burst of presses before the picture is read again: the game moves its cursor in the
                           // frame after the press and animates it over about four; a still cursor after three is at rest

}  // namespace

void MenuPointer::frame_of(const Scene& scene, const DrawItem* ref) {
    const UiPlace p = UiPlace::of(scene, ref);
    u_ = p.u;
    ax_ = p.ax;
    ay_ = p.ay;
}

void MenuPointer::yield() {
    queue_ = queue2_ = 0;
    tracking_ = confirm_pending_ = false;
    act_ = 0;
    dragging_ = false;
    map_goal_ = map_confirm_ = false;
    start_hold_ = 0;
}

void MenuPointer::plan(int dir, int n, int dir2, int n2) {
    queue_dir_ = dir;
    queue_ = std::max(0, n);
    queue2_dir_ = dir2;
    queue2_ = std::max(0, n2);
}

int MenuPointer::next_press(int settle_frames) {
    if (held_) {  // the frame after a press is free, so the next one is new to the game
        held_ = false;
        if (!queue_ && !queue2_) settle_ = settle_frames;
        return 0;
    }
    if (!queue_ && queue2_) {
        queue_dir_ = queue2_dir_;
        queue_ = queue2_;
        queue2_ = 0;
    }
    if (queue_) {
        --queue_;
        held_ = true;
        return queue_dir_;
    }
    if (settle_ > 0) --settle_;
    return 0;
}

bool MenuPointer::still(double x, double y) {
    const bool same = std::fabs(x - rest_x_) < 0.5 && std::fabs(y - rest_y_) < 0.5;
    rest_x_ = x;
    rest_y_ = y;
    return same;
}

int MenuPointer::settings_update(const Scene& scene, const MenuPictures& pics, const DrawItem* cur, double mx, double my,
                                 bool inside, bool down, bool click, int wheel_steps) {
    // The panel's rows: the pictures on the cursor's layer inside the panel (x from -75, y from -75 to 95), one row per height.
    frame_of(scene, cur);
    std::vector<Row> rows;
    auto row_at = [&](double y) -> Row& {
        for (Row& r : rows)
            if (std::fabs(r.y - y) < D(4)) return r;
        rows.push_back(Row());
        rows.back().y = y;
        rows.back().half = D(9);
        return rows.back();
    };
    double back_x = 1e9, back_y = 1e9;
    for (const DrawItem& it : scene.items)
        if (it.image && it.image == pics.back && it.alpha > 0.5) {
            back_x = it.centre[0];
            back_y = it.centre[1];
        }
    for (const DrawItem& it : scene.items) {
        if (&it == cur || !it.image || it.alpha <= 0.5 || it.layer != cur->layer || it.centre[0] < X(-75) || it.centre[1] < Y(-75) || it.centre[1] > Y(95)) continue;
        if (std::fabs(it.centre[0] - back_x) < D(45) && std::fabs(it.centre[1] - back_y) < D(15)) continue;  // the hint, not a row
        Row& r = row_at(it.centre[1]);
        if ((pics.note && it.image == pics.note) || (pics.arrow && it.image == pics.arrow)) {
            if (it.image == pics.arrow) {
                r.half = D(6);
                r.least = 1;
            }
            r.notes.push_back(it.centre[0]);
            if (it.light[0] > 0.75) ++r.lit;
        } else if (it.centre[0] > X(90)) {
            r.value = true;
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.y < b.y; });
    for (Row& r : rows) std::sort(r.notes.begin(), r.notes.end());
    if (!rows.empty() && rows.back().y > Y(80) && !rows.back().value && rows.back().notes.empty()) rows.back().reset = true;
    if (rows.empty()) return 0;
    auto nearest = [&](double y) {
        size_t best = 0;
        for (size_t i = 0; i < rows.size(); ++i)
            if (std::fabs(rows[i].y - y) < std::fabs(rows[best].y - y)) best = i;
        return int(best);
    };
    const int cur_row = nearest(cur->centre[1]);

    // The tabs: left to right; the lit one is the page shown.
    std::vector<const DrawItem*> tabs;
    for (const DrawItem& it : scene.items)
        if (it.image && it.image == pics.tab && it.alpha > 0.5) tabs.push_back(&it);
    std::sort(tabs.begin(), tabs.end(), [](const DrawItem* a, const DrawItem* b) { return a->centre[0] < b->centre[0]; });
    int cur_tab = 0;
    for (size_t i = 0; i < tabs.size(); ++i)
        if (tabs[i]->light[0] > tabs[size_t(cur_tab)]->light[0] + 0.05) cur_tab = int(i);
    int hover_tab = -1;  // a tab under the mouse, or -2 for the L icon, -3 for the R icon
    if (inside && !tabs.empty()) {
        const double ty = tabs[0]->centre[1], hh = D(tabs[0]->rect[3] * 0.5 * 0.5 + 4), hw = D(tabs[0]->rect[2] * 0.5 * 0.5);
        if (std::fabs(my - ty) <= hh) {
            for (size_t i = 0; i < tabs.size(); ++i)
                if (std::fabs(mx - tabs[i]->centre[0]) <= hw) hover_tab = int(i);
            if (hover_tab < 0 && mx < tabs.front()->centre[0] - hw && mx > tabs.front()->centre[0] - hw - D(70)) hover_tab = -2;
            if (hover_tab < 0 && mx > tabs.back()->centre[0] + hw && mx < tabs.back()->centre[0] + hw + D(70)) hover_tab = -3;
        }
    }
    // The row under the mouse.
    int hover_row = -1;
    if (inside && hover_tab == -1 && mx > X(-80) && mx < X(205))
        for (size_t i = 0; i < rows.size(); ++i)
            if (std::fabs(my - rows[i].y) <= D(9)) hover_row = int(i);

    probe.kind = 's';
    probe.has_sel = true;
    probe.sel_x = X(-106);
    probe.sel_y = rows[size_t(cur_row)].y;
    probe.has_tgt = hover_row >= 0;
    if (probe.has_tgt) probe.tgt_y = rows[size_t(hover_row)].y;
    probe.on = probe.has_tgt && hover_row == cur_row;
    const bool moved = std::hypot(mx - last_x_, my - last_y_) > 1.0;
    last_x_ = mx;
    last_y_ = my;
    ++phase_;
    const bool pulse = (phase_ & 1) == 0;  // a tab: a press every second frame, after the cooldown of the page's animation
    if (cooldown_ > 0) --cooldown_;
    const bool settled = still(cur->centre[0], cur->centre[1]);
    // The wheel: over the tabs it changes the page, elsewhere it moves the cursor a row a notch.
    if (wheel_steps != 0 && !busy()) {
        if (hover_tab != -1) {
            act_ = 4;
            act_goal_ = cur_tab + (wheel_steps > 0 ? -1 : 1);
        } else {
            act_ = 0;
            plan(wheel_steps > 0 ? UP : DOWN, std::abs(wheel_steps));
        }
    }

    // A new click chooses the action, from where it fell.
    if (click) {
        act_ = 0;
        if (hover_tab >= 0) {
            act_ = 4;
            act_goal_ = hover_tab;
        } else if (hover_tab == -2 || hover_tab == -3) {
            act_ = 4;
            act_goal_ = cur_tab + (hover_tab == -2 ? -1 : 1);
        } else if (hover_row >= 0) {
            const Row& r = rows[size_t(hover_row)];
            act_y_ = r.y;
            if (r.reset) {
                act_ = 3;
            } else if (!r.notes.empty()) {
                act_ = 1;
                dragging_ = true;
            } else if (r.value || mx < X(100)) {
                act_ = 2;
                act_dir_ = (r.value && mx >= X(100) && mx < X(140)) ? LEFT : RIGHT;  // the left part of the value goes back, the rest forward
            }
        }
    }
    if (!down) dragging_ = false;
    if (act_ == 1 && dragging_)  // the value under the mouse, as a note is 12 wide and 18 apart
        for (const Row& r : rows)
            if (std::fabs(r.y - act_y_) < D(4) && !r.notes.empty()) {
                int goal = r.least;
                for (size_t k = 0; k < r.notes.size(); ++k)
                    if (mx >= r.notes[k] - r.half) goal = int(k) + 1;
                act_goal_ = goal;
            }

    int mask = 0;
    // Hovering moves the cursor to the row (only after the mouse moved, so the pad can still choose).
    if ((act_ == 0 || act_ == 5) && moved && hover_row >= 0) {
        if (act_ == 5 && std::fabs(act_y_ - rows[size_t(hover_row)].y) > D(4)) queue_ = 0;  // another row: what was left is dropped
        act_ = 5;
        act_y_ = rows[size_t(hover_row)].y;
    }
    if (busy()) return next_press(act_ == 1 ? SETTLE + 1 : SETTLE);  // the notes fade in three frames
    if (act_ == 4) {
        if (cur_tab == act_goal_ || act_goal_ < 0 || act_goal_ >= int(tabs.size())) {
            act_ = 0;
        } else if (pulse && cooldown_ == 0) {
            cooldown_ = 12;
            mask = cur_tab < act_goal_ ? R1 : L1;
        }
        return mask;
    }
    if (act_ >= 1) {
        const int want_row = nearest(act_y_);
        if (want_row != cur_row) {  // first the cursor to the row, all the rows between in one go
            if (!settled) return 0;
            plan(want_row < cur_row ? UP : DOWN, std::abs(want_row - cur_row));
            return next_press(SETTLE);
        }
        if (act_ == 5) {
            act_ = 0;
        } else if (act_ == 1) {
            const Row& r = rows[size_t(cur_row)];
            if (r.lit == act_goal_ || act_goal_ < 0) {
                if (!dragging_) act_ = 0;
            } else {
                plan(r.lit < act_goal_ ? RIGHT : LEFT, std::abs(act_goal_ - r.lit));
                return next_press(SETTLE + 1);
            }
        } else {
            mask = act_ == 2 ? act_dir_ : CONFIRM;
            act_ = 0;
            held_ = true;  // the next frame is free
        }
    }
    return mask;
}

// The day map: the cursor sits on one cell of a staggered grid (columns 42 apart, rows 44) and the pad moves it a cell at a time
// (the map scrolls under it at the edge). The mouse is followed by pressing the direction toward it until the cursor's cell holds
// it; a press every eighth frame, as the view takes about six frames to settle; given up when three presses bring it no closer.
int MenuPointer::map_update(const Scene& scene, const MenuPictures& pics, double cx, double cy, double mx, double my, bool inside, bool click,
                            bool gallery) {
    constexpr int TRIANGLE = 16;
    if (gallery && click && inside) {  // the page arrows at the sides of the grid turn the page with the shoulder buttons
        for (const DrawItem& it : scene.items)
            if (it.image == pics.arrow && it.alpha > 0.5 && std::fabs(mx - it.centre[0]) <= D(14) && std::fabs(my - it.centre[1]) <= D(14)) {
                map_goal_ = map_confirm_ = false;
                return it.centre[0] < 0 ? L1 : R1;
            }
    }
    if (!gallery && click && inside) {
        static const int dirs[4] = {UP, DOWN, LEFT, RIGHT};
        for (int k = 0; k < 4; ++k)
            if (pics.map_arrow[k])
                for (const DrawItem& it : scene.items)
                    if (it.image == pics.map_arrow[k] && it.alpha > 0.5 && std::fabs(mx - it.centre[0]) <= D(14) && std::fabs(my - it.centre[1]) <= D(14)) {
                        map_goal_ = map_confirm_ = false;
                        return dirs[k];
                    }
        if (mx > X(100) && my > Y(45)) {
            map_goal_ = map_confirm_ = false;
            return TRIANGLE;
        }  // the corner labelled "Other": the game's menu
    }
    // Where the map is: not the calendar's panel (top left), not the menu's corner (bottom right).
    const bool in_area = gallery ? inside && mx > X(-200) && mx < X(200) && my > Y(-93) && my < Y(106)
                                 : inside && mx > X(-273) && mx < X(147) && my > Y(-60) && my < Y(110) && !(mx < X(-140) && my < Y(-25)) &&
                                       !(mx > X(100) && my > Y(45));
    const double half_w = D(gallery ? 25 : 21), half_h = D(gallery ? 25 : 22);
    probe.kind = gallery ? 'g' : 'd';
    probe.has_sel = true;
    probe.sel_x = cx;
    probe.sel_y = cy;
    probe.has_tgt = in_area;
    probe.tgt_x = mx;
    probe.tgt_y = my;
    probe.on = in_area && std::fabs(mx - cx) <= half_w && std::fabs(my - cy) <= half_h;
    const bool moved = std::hypot(mx - last_x_, my - last_y_) > 1.0;
    last_x_ = mx;
    last_y_ = my;
    if ((moved || click) && in_area) {
        map_goal_ = true;
        map_x_ = mx;
        map_y_ = my;
        map_best_ = 1e9;
        map_stall_ = 0;
        if (click) map_confirm_ = true;
    }
    if (click && !in_area) map_confirm_ = map_goal_ = false;
    if (gallery && busy()) {
        if ((moved || click) && in_area && (std::fabs(mx - plan_x_) > half_w || std::fabs(my - plan_y_) > half_h)) queue_ = queue2_ = 0;
        still(cx, cy);
        return next_press(SETTLE);
    }
    if (!map_goal_) return 0;
    ++phase_;
    const double dx = map_x_ - cx, dy = map_y_ - cy;
    const bool at_rest = !gallery || still(cx, cy);
    if (std::fabs(dx) <= half_w && std::fabs(dy) <= half_h) {
        const bool confirm = map_confirm_;
        map_goal_ = map_confirm_ = false;
        if (confirm) held_ = true;
        return confirm ? CONFIRM : 0;
    }
    if (gallery) {  // the cells between in one go: rows, then columns
        if (!at_rest) return 0;
        plan_x_ = map_x_;
        plan_y_ = map_y_;
        plan(dy > 0 ? DOWN : UP, int(std::lround(std::fabs(dy) / (2 * half_h))), dx > 0 ? RIGHT : LEFT,
             int(std::lround(std::fabs(dx) / (2 * half_w))));
        return next_press(SETTLE);
    }
    if ((phase_ & 7) != 0) return 0;
    const double dist = std::hypot(dx, dy);
    if (dist < map_best_ - 1) {
        map_best_ = dist;
        map_stall_ = 0;
    } else if (++map_stall_ >= 3) {
        map_goal_ = map_confirm_ = false;
        return 0;
    }
    int mask = 0;
    if (std::fabs(dx) > half_w) {
        mask = dx > 0 ? RIGHT : LEFT;
        if (!gallery && std::fabs(dy) >= D(11)) mask |= dy > 0 ? DOWN : UP;  // the grid's script reads one direction at a time
    } else {
        mask = dy > 0 ? DOWN : UP;
    }
    return mask;
}

int MenuPointer::update(const Scene& scene, const MenuPictures& pics, double mx, double my, bool inside, bool down, bool click,
                        bool right_click, int wheel_steps) {
    const ImagePtr& cursor = pics.cursor;
    probe = Probe();
    // The "back" hint (the word at the top right of a menu, and in the Settings screen): a click on it goes back.
    if (click && inside && pics.back)
        for (const DrawItem& it : scene.items) {
            const double k = it.shrunk ? scene.ui_u : 1.0;
            if (it.image == pics.back && it.alpha > 0.5 && std::fabs(mx - (it.centre[0] - 14 * k)) <= 40 * k && std::fabs(my - it.centre[1]) <= 8 * k) return CANCEL;
        }
    // The cursor, and the entries on its layer: sprites of a size a menu entry has, visible, that are not the cursor.
    const DrawItem* cur = nullptr;
    for (const DrawItem& it : scene.items)
        if (cursor && it.image == cursor && it.alpha > 0.5) cur = &it;
    // The name entry's character grid has no picture cursor: its cursor is an orange rectangle (light 1, 0.5, 0.2) laid
    // behind the character it is on, and its cells are the size of that rectangle.
    const DrawItem* grid = nullptr;
    if (!cur)
        for (const DrawItem& it : scene.items)
            if (!it.image && it.alpha > 0.3 && it.light[0] > 0.9 && std::fabs(it.light[1] - 0.5) < 0.1 && std::fabs(it.light[2] - 0.2) < 0.1) grid = &it;
    if (grid) cur = grid;
    frame_of(scene, cur);
    // The Settings screen: tabs and rows, with sliders; it has its own way (settings_update).
    if (cur && !grid && pics.tab) {
        int tabs = 0;
        for (const DrawItem& it : scene.items)
            if (it.image == pics.tab && it.alpha > 0.5) ++tabs;
        if (tabs >= 2) {
            if (right_click) {
                yield();
                return CANCEL;
            }
            return settings_update(scene, pics, cur, mx, my, inside, down, click, wheel_steps);
        }
    }
    // The name entry's "Decide" button (top right, a diamond and a word) is reached with Start on a pad: a click on it presses Start.
    if (grid && click && inside && mx > X(100) && mx < X(172) && my > Y(-137) && my < Y(-101)) start_hold_ = 6;  // the game reads a press that lasts a few frames
    if (start_hold_ > 0) {
        --start_hold_;
        return START;
    }
    if (!cur) {
        tracking_ = confirm_pending_ = false;
        if (pics.map_cursor) {  // the day map: its cursor is four corners
            double sx = 0, sy = 0;
            int n = 0;
            const DrawItem* corner = nullptr;
            for (const DrawItem& it : scene.items)
                if (it.image == pics.map_cursor && it.alpha > 0.05) {
                    sx += it.centre[0];
                    sy += it.centre[1];
                    ++n;
                    corner = &it;
                }
            frame_of(scene, corner);
            int arrows = 0;  // the gallery has the same four corners but no guide arrows
            for (int k = 0; k < 4; ++k)
                for (const DrawItem& it : scene.items)
                    if (pics.map_arrow[k] && it.image == pics.map_arrow[k] && it.alpha > 0.05) ++arrows;
            if (n == 4) {  // with guide arrows the day map; without, the gallery's grid of 50 by 50 unit cells
                if (right_click) {
                    yield();
                    return CANCEL;
                }
                if (wheel_steps != 0 && !busy()) {  // the gallery: a page a notch; the day map: a row
                    if (arrows == 0) return wheel_steps > 0 ? L1 : R1;
                    return wheel_steps > 0 ? UP : DOWN;
                }
                return map_update(scene, pics, sx / 4, sy / 4, mx, my, inside, click, arrows == 0);
            }
        }
        map_goal_ = false;
        phase_ = 0;
        // No cursor on the screen (dialogue, the map, the grids): nothing to aim at, so the buttons stand for the pad's:
        // a click goes on (confirm), the right button goes back (cancel).
        if (right_click) return CANCEL;
        if (click) return CONFIRM;
        return 0;
    }
    const double units = VIEW_SCALE / VIEW_UNIT;  // a texel is this many script units
    std::vector<Entry> entries;
    const double cell_w = grid ? grid->rect[2] * std::fabs(grid->scale[0]) * units : 0, cell_h = grid ? grid->rect[3] * std::fabs(grid->scale[1]) * units : 0;
    for (const DrawItem& it : scene.items) {
        if (&it == cur || !it.image || it.image == cursor || it.alpha <= 0.5 || it.snow) continue;
        if (grid) {  // a character of the grid: on the cursor's layer or the next, as big as a cell, below the top bar (the name fields and the buttons, above y = -90 in script units)
            if (it.layer < cur->layer || it.layer > cur->layer + 1 || it.centre[1] < Y(-90)) continue;
            const double gw = it.rect[2] * std::fabs(it.scale[0]) * units, gh = it.rect[3] * std::fabs(it.scale[1]) * units;
            if (gw < D(6) || gh < D(6) || gw > cell_w * 3 || gh > cell_h * 3) continue;
            entries.push_back({it.centre[0], it.centre[1], cell_w / 2, cell_h / 2});
            continue;
        }
        if (it.layer != cur->layer) continue;
        const double w = it.rect[2] * std::fabs(it.scale[0]) * units, h = it.rect[3] * std::fabs(it.scale[1]) * units;
        const double k = it.shrunk ? scene.ui_u : 1.0;  // the size limits are the game's layout's
        if (w < 12 * k || h < 10 * k || w > 260 * k || h > 90 * k) continue;  // not an entry: a line of text is thinner, a panel larger
        entries.push_back({it.centre[0], it.centre[1], w / 2, h / 2});
    }

    // The entry the cursor is on: the nearest, the cursor usually stands beside its entry, so sideways counts for less.
    const Entry* selected = nullptr;
    double best = 1e18;
    for (const Entry& e : entries) {
        const double dx = (e.cx - cur->centre[0]) * (grid ? 1.0 : 0.3), dy = e.cy - cur->centre[1];
        const double d = dx * dx + dy * dy;
        if (d < best) {
            best = d;
            selected = &e;
        }
    }
    // The entry under the mouse: the smallest that holds it.
    const Entry* target = nullptr;
    if (inside)
        for (const Entry& e : entries)
            if (std::fabs(mx - e.cx) <= e.half_w && std::fabs(my - e.cy) <= e.half_h &&
                (!target || e.half_w * e.half_h < target->half_w * target->half_h))
                target = &e;

    probe.kind = grid ? 'n' : 'm';
    probe.has_sel = selected != nullptr;
    if (selected) {
        probe.sel_x = selected->cx;
        probe.sel_y = selected->cy;
    }
    probe.has_tgt = target != nullptr;
    if (target) {
        probe.tgt_x = target->cx;
        probe.tgt_y = target->cy;
    }
    probe.on = target && selected && target->cx == selected->cx && target->cy == selected->cy;
    const bool moved = std::hypot(mx - last_x_, my - last_y_) > 1.0;
    last_x_ = mx;
    last_y_ = my;
    if (moved && target) tracking_ = true;
    if (!target && !confirm_pending_) tracking_ = false;
    if (click && target) confirm_pending_ = true;

    const bool settled = still(cur->centre[0], cur->centre[1]);
    if (right_click) {
        yield();
        return CANCEL;
    }
    if (busy()) {
        // The mouse went to another entry while presses were still to make: the rest is dropped and planned again at rest.
        if (target && (std::fabs(target->cx - plan_x_) > 0.5 || std::fabs(target->cy - plan_y_) > 0.5)) queue_ = queue2_ = 0;
        return next_press(SETTLE);
    }
    if (wheel_steps != 0) {  // a row a notch
        tracking_ = confirm_pending_ = false;
        plan(wheel_steps > 0 ? UP : DOWN, std::abs(wheel_steps));
        return next_press(SETTLE);
    }

    if (target && selected && (tracking_ || confirm_pending_)) {
        if (target == selected || (target->cx == selected->cx && target->cy == selected->cy)) {
            tracking_ = false;
            if (confirm_pending_) {
                confirm_pending_ = false;
                held_ = true;  // the next frame is free
                return CONFIRM;
            }
            return 0;
        }
        if (!settled) return 0;  // the cursor is still on its way (or the menu is opening): its entry is not known yet
        // All the presses to the entry at once, one every second frame: the rows between, then (a grid) the columns.
        const double dx = target->cx - selected->cx, dy = target->cy - selected->cy;
        plan_x_ = target->cx;
        plan_y_ = target->cy;
        const int rows = places_between(entries, true, selected->cy, target->cy);
        const int cols = places_between(entries, false, selected->cx, target->cx);
        if (grid) plan(dy < 0 ? UP : DOWN, rows, dx < 0 ? LEFT : RIGHT, cols);
        else if (std::fabs(dy) >= std::fabs(dx)) plan(dy < 0 ? UP : DOWN, std::max(1, rows));
        else plan(dx < 0 ? LEFT : RIGHT, std::max(1, cols));
        if (!queue_ && !queue2_) plan(std::fabs(dy) >= std::fabs(dx) ? (dy < 0 ? UP : DOWN) : (dx < 0 ? LEFT : RIGHT), 1);
        return next_press(SETTLE);
    }
    return 0;
}

}  // namespace amg
