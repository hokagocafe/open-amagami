#include "menu_pointer.hpp"

#include <cmath>
#include <vector>

namespace amg {

namespace {

struct Entry {
    double cx, cy, half_w, half_h;
};

// The engine's buttons.
constexpr int UP = 4096, RIGHT = 8192, DOWN = 16384, LEFT = 32768, CONFIRM = 32, CANCEL = 64;

}  // namespace

int MenuPointer::update(const Scene& scene, const ImagePtr& cursor, double mx, double my, bool inside, bool click,
                        bool right_click, int wheel_steps) {
    // The cursor, and the entries on its layer: sprites of a size a menu entry has, visible, that are not the cursor.
    const DrawItem* cur = nullptr;
    for (const DrawItem& it : scene.items)
        if (cursor && it.image == cursor && it.alpha > 0.5) cur = &it;
    if (!cur) {
        tracking_ = confirm_pending_ = false;
        phase_ = 0;
        return 0;
    }
    const double units = VIEW_SCALE / VIEW_UNIT;  // a texel is this many script units
    std::vector<Entry> entries;
    for (const DrawItem& it : scene.items) {
        if (&it == cur || it.layer != cur->layer || !it.image || it.image == cursor || it.alpha <= 0.5 || it.snow) continue;
        const double w = it.rect[2] * std::fabs(it.scale[0]) * units, h = it.rect[3] * std::fabs(it.scale[1]) * units;
        if (w < 12 || h < 10 || w > 260 || h > 90) continue;  // not an entry: a line of text is thinner, a panel larger
        entries.push_back({it.centre[0], it.centre[1], w / 2, h / 2});
    }

    // The entry the cursor is on: the nearest, the cursor usually stands beside its entry, so sideways counts for less.
    const Entry* selected = nullptr;
    double best = 1e18;
    for (const Entry& e : entries) {
        const double dx = (e.cx - cur->centre[0]) * 0.3, dy = e.cy - cur->centre[1];
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

    const bool moved = std::hypot(mx - last_x_, my - last_y_) > 1.0;
    last_x_ = mx;
    last_y_ = my;
    if (moved && target) tracking_ = true;
    if (!target && !confirm_pending_) tracking_ = false;
    if (click && target) confirm_pending_ = true;

    ++phase_;
    const bool pulse = (phase_ & 3) == 0;  // a press every fourth frame: the game must see each one end before the next
    int mask = 0;
    if (right_click) mask |= CANCEL;
    if (wheel_steps != 0) mask |= wheel_steps > 0 ? UP : DOWN;
    if (mask) return mask;

    if (target && selected && (tracking_ || confirm_pending_)) {
        if (target == selected || (target->cx == selected->cx && target->cy == selected->cy)) {
            tracking_ = false;
            if (confirm_pending_ && pulse) {
                confirm_pending_ = false;
                return CONFIRM;
            }
            return 0;
        }
        if (!pulse) return 0;
        const double dx = target->cx - selected->cx, dy = target->cy - selected->cy;
        if (std::fabs(dy) >= std::fabs(dx)) return dy < 0 ? UP : DOWN;
        return dx < 0 ? LEFT : RIGHT;
    }
    return 0;
}

}  // namespace amg
