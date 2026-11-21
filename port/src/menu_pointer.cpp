#include "menu_pointer.hpp"

#include <cmath>

namespace amg {

namespace {

constexpr int CONFIRM = 32, CANCEL = 64;  // the engine's buttons

}  // namespace

int MenuPointer::update(const Scene& scene, const MenuPictures& pics, double mx, double my, bool inside, bool down,
                        bool click, bool right_click, int wheel_steps) {
    (void)down;
    (void)wheel_steps;
    probe = Probe();
    // The "back" hint: a click on it goes back.
    if (click && inside && pics.back)
        for (const DrawItem& it : scene.items) {
            const double k = it.shrunk ? scene.ui_u : 1.0;
            if (it.image == pics.back && it.alpha > 0.5 && std::fabs(mx - (it.centre[0] - 14 * k)) <= 40 * k &&
                std::fabs(my - it.centre[1]) <= 8 * k)
                return CANCEL;
        }
    if (right_click) return CANCEL;
    if (click) return CONFIRM;
    return 0;
}

}  // namespace amg
