#include "mouse_menus.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace amg {

namespace {

constexpr int CONFIRM = 32, CANCEL = 64;
// Half the width of a Load/Save slot's box from ScrollList.baseX, in script units (measured on the picture).
constexpr double SHIORI_HALF_W = 150;

// The owners' pad loops the port knows (Vita image; class that defines the method, method). Each moves its CommandMenu
// with moveCursorU/D, keeps a copy of the index (the value those return) and plays sound 32 on a move.
struct Owner {
    const char* cls;
    const char* method;
    int sound;
};
constexpr Owner OWNERS[] = {
    {"MainMenu", "scriptMain", 32},     // the main menu
    {"GameMenu", "scriptMain", 32},     // the in-game menu of the events
    {"GameMenuMap", "selectMenu", 32},  // the menu of the day map (and its prologue, scenario and free-play kinds)
};

Value* field(const Value& obj, const char* name) {
    if (obj.tag != Tag::Obj) return nullptr;
    auto* o = static_cast<ObjObj*>(obj.p.get());
    for (ClassObj* c = o->cls; c; c = c->parent)
        if (c->src)
            for (const auto& [slot, n] : c->src->instance_vars)
                if (n == name && slot >= 0 && size_t(slot) < o->iv.size()) return &o->iv[size_t(slot)];
    return nullptr;
}

int64_t int_field(const Value& obj, const char* name, int64_t fallback = -1) {
    const Value* v = field(obj, name);
    if (!v) return fallback;
    if (v->tag == Tag::Int) return v->i;
    if (v->tag == Tag::Float) return int64_t(v->f);
    return fallback;
}

// Where the owner keeps its copy of the index: the store right after `send moveCursorD` in its loop (pop_ivar or
// pop_local; an argument is a local too). Read from the code once per method; kind 0 when not found.
struct Mirror {
    int kind = 0;  // 1 an instance variable of the owner, 2 a local of the owner's frame
    int slot = 0;
};

Mirror mirror_of(const Method* m) {
    static std::unordered_map<const Method*, Mirror> cache;
    auto hit = cache.find(m);
    if (hit != cache.end()) return hit->second;
    Mirror out;
    const auto& consts = m->cls->src->constants;
    for (size_t i = 0; i + 1 < m->code.size(); ++i) {
        const Instr& in = m->code[i];
        if ((in.op != 0x30 && in.op != 0x32) || in.a != 0 || in.b < 0 || size_t(in.b) >= consts.size()) continue;
        const auto& c = consts[size_t(in.b)];
        if (c.tag != 7 || c.s != "moveCursorD") continue;
        const Instr& st = m->code[i + 1];
        if (st.op == 0x21) out = {1, st.a};
        else if (st.op == 0x23) out = {2, st.a};
        break;
    }
    cache[m] = out;
    return out;
}

struct Found {
    Thread* thread = nullptr;
    size_t frame = 0;
    const Method* method = nullptr;
    Value owner, menu;
    int sound = 32;
};

bool find(VM& vm, Found& f) {
    ClassObj* command_menu = vm.find_class("CommandMenu");
    if (!command_menu) return false;
    for (Thread* t : vm.live_threads) {
        if (t->done || t->frames.empty()) continue;
        const Frame& fr = t->frames.back();
        const Method* m = fr.method;
        if (!m || !m->cls || !m->cls->src) continue;
        const Owner* own = nullptr;
        for (const Owner& o : OWNERS)
            if (m->cls->name == o.cls && m->name == o.method) own = &o;
        if (!own || fr.base >= t->stack.size()) continue;
        const Value& owner = t->stack[fr.base];
        const Value* menu = field(owner, "menu");
        if (!menu || menu->tag != Tag::Obj || !static_cast<ObjObj*>(menu->p.get())->cls->is_kind_of(command_menu)) continue;
        if (mirror_of(m).kind == 0) continue;
        f.thread = t;
        f.frame = t->frames.size() - 1;
        f.method = m;
        f.owner = owner;
        f.menu = *menu;
        f.sound = own->sound;
        return true;
    }
    return false;
}

// The owner whose copy PortMouse.mirror writes: the one the last select thread was started for.
struct Pending {
    Thread* thread = nullptr;
    size_t frame = 0;
    const Method* method = nullptr;
} pending;

// --- helpers for the adapters of steps 6 and 7 ------------------------------------------------------------------------

bool kind_of(VM& vm, const Value& v, const char* name) {
    if (v.tag != Tag::Obj) return false;
    ClassObj* c = vm.find_class(name);
    return c && static_cast<ObjObj*>(v.p.get())->cls->is_kind_of(c);
}

// The elements of an Array, or of a Vector (the game's class: `anArray` up to `tailIndex`).
std::vector<Value> elems(const Value& v) {
    if (v.tag == Tag::Arr) return static_cast<ArrObj*>(v.p.get())->items;
    std::vector<Value> out;
    const Value* arr = field(v, "anArray");
    const int64_t n = int_field(v, "tailIndex", 0);
    if (arr && arr->tag == Tag::Arr) {
        const auto& items = static_cast<ArrObj*>(arr->p.get())->items;
        for (int64_t i = 0; i < n && size_t(i) < items.size(); ++i) out.push_back(items[size_t(i)]);
    }
    return out;
}

// The screen object of a Sprite (its first instance variable), 0 when it is not one.
int64_t uid_of(const Value* sprite) {
    if (!sprite || sprite->tag != Tag::Obj) return 0;
    const auto* o = static_cast<ObjObj*>(sprite->p.get());
    return !o->iv.empty() && o->iv[0].tag == Tag::Int ? o->iv[0].i : 0;
}

const DrawItem* item_of(const Scene& seen, int64_t uid) {
    if (!uid) return nullptr;
    for (const DrawItem& it : seen.items)
        if (it.uid == uid) return &it;
    return nullptr;
}

// The layout of a screen in its own script units, placed where the picture shows it: the interface shrink
// (Scene::ui_*) when the screen's pictures are shrunk (as MenuPointer::X/Y/D).
struct Layout {
    double u = 1, ax = 0, ay = 0;
    double X(double x) const { return ax + (x - ax) * u; }
    double Y(double y) const { return ay + (y - ay) * u; }
    double D(double d) const { return d * u; }
};

Layout layout_of(const Scene& seen, const DrawItem* ref) {
    const UiPlace p = UiPlace::of(seen, ref);  // the anchor of ref's group (docs/technical/interface-scale.md)
    Layout l;
    l.u = p.u;
    l.ax = p.ax;
    l.ay = p.ay;
    return l;
}

// The innermost script frame of a live thread when it is `cls.method`, else null; `t` gets the thread.
const Frame* innermost(VM& vm, const char* cls, const char* method, Thread** t) {
    for (Thread* th : vm.live_threads) {
        if (th->done || th->frames.empty()) continue;
        const Frame& fr = th->frames.back();
        if (!fr.method || !fr.method->cls || fr.method->cls->name != cls || fr.method->name != method) continue;
        if (fr.base >= th->stack.size()) continue;
        *t = th;
        return &fr;
    }
    return nullptr;
}

// The frame of `cls.method` when it is among the four innermost script frames of a live thread (under its own waits: (a loop that
// waits through a script method of its own, as Selector.waitSelect does through Selector.mySleep), else null.
const Frame* near_top(VM& vm, const char* cls, const char* method, Thread** t) {
    for (Thread* th : vm.live_threads) {
        if (th->done || th->frames.empty()) continue;
        for (size_t k = 1; k <= 4 && k <= th->frames.size(); ++k) {
            const Frame& fr = th->frames[th->frames.size() - k];
            if (!fr.method || !fr.method->cls || fr.method->cls->name != cls || fr.method->name != method) continue;
            if (fr.base >= th->stack.size()) continue;
            *t = th;
            return &fr;
        }
    }
    return nullptr;
}

// Half the width and height of an item in script units of the picture (a texture has two texels a unit, a plain
// rectangle is measured in units).
void half_size(const DrawItem& d, double& hw, double& hh) {
    const double k = d.image ? VIEW_SCALE / VIEW_UNIT : 1.0;
    hw = d.rect[2] * std::fabs(d.scale[0]) * k / 2;
    hh = d.rect[3] * std::fabs(d.scale[1]) * k / 2;
}

// A one-shot PortMouse thread: `method` with `args`, run in this tick (as the select of step 3).
bool run_port(VM& vm, const char* method, std::vector<Value> args) {
    ClassObj* cls = vm.find_class("PortMouse");
    if (!cls) return false;
    Thread* t = vm.new_thread(vm.make_obj(cls), method, std::move(args), "thread");
    t->wake_tick = vm.tick;
    t->start();
    return true;
}

bool hit(const DrawItem* d, double mx, double my, double slack = 0) {
    if (!d) return false;
    const double units = VIEW_SCALE / VIEW_UNIT;
    const double hw = d->rect[2] * std::fabs(d->scale[0]) * units / 2 + slack, hh = d->rect[3] * std::fabs(d->scale[1]) * units / 2 + slack;
    return std::fabs(mx - d->centre[0]) <= hw && std::fabs(my - d->centre[1]) <= hh;
}

}  // namespace

void mouse_menus_mirror(VM& vm, const Value& index) {
    Thread* t = pending.thread;
    bool alive = false;
    for (Thread* l : vm.live_threads)
        if (l == t && !l->done) alive = true;
    if (!alive || t->frames.size() <= pending.frame || t->frames[pending.frame].method != pending.method) return;
    const Frame& fr = t->frames[pending.frame];
    const Mirror mr = mirror_of(fr.method);
    if (mr.kind == 1) {
        if (fr.base < t->stack.size() && t->stack[fr.base].tag == Tag::Obj) {
            auto* o = static_cast<ObjObj*>(t->stack[fr.base].p.get());
            if (size_t(mr.slot) < o->iv.size()) o->iv[size_t(mr.slot)] = index;
        }
    } else if (mr.kind == 2) {
        const size_t at = fr.base + 1 + size_t(mr.slot);
        if (at < t->stack.size()) t->stack[at] = index;
    }
}

void MouseMenus::yield() {
    confirm_for_ = -1;
    last_x_ = last_y_ = 1e9;  // hover waits for a real move: the next update records the place, the one after compares
}

bool MouseMenus::update(VM& vm, const Scene& seen, double mx, double my, bool inside, bool down, bool click,
                        bool right_click, int wheel_steps, int& mask) {
    mask = 0;
    probe = MenuPointer::Probe();
    arrow_uid = 0;
    mark = Graphics::SelectionMark();
    if (!down) drag_row_ = -1;
    char screen = 0;
    Found f;
    const bool command = find(vm, f);
    Thread* t = nullptr;
    const Frame* fr = nullptr;
    Value dialog;
    if (!command) {
        fr = near_top(vm, "ConfigMain", "scriptMain", &t);
        if (!fr) fr = near_top(vm, "PortMain", "scriptMain", &t);
        if (fr)  // the dialog is a local of the loop (ConfigMain's loc0, PortMain's after its argument)
            for (size_t k = 1; k <= 4 && fr->base + k < t->stack.size(); ++k)
                if (kind_of(vm, t->stack[fr->base + k], "ConfigDialog")) {
                    dialog = t->stack[fr->base + k];
                    break;
                }
        if (dialog.tag == Tag::Obj) {
            screen = 's';
        } else if (innermost(vm, "ControlPad", "wait", &t)) {
            screen = 'w';  // a question waiting for confirm or cancel (ControlPad.waitOX): no index
        } else if ((fr = near_top(vm, "Selector", "waitSelect", &t))) {
            screen = 'q';  // a choice (TextWindow's answers, and any other Selector grid)
        } else if ((fr = near_top(vm, "KaiwaMain", "wadaiSelect", &t))) {
            screen = 'k';  // the conversation's topics (WadaiPanel)
        } else if ((fr = near_top(vm, "LoadMenu", "selectShiori", &t)) || (fr = near_top(vm, "SaveMenu", "selectShiori", &t))) {
            screen = 'l';  // the Load and Save lists (ShioriList)
        } else if ((fr = near_top(vm, "Gallery", "scriptMain", &t))) {
            screen = 'y';  // the gallery's grid of thumbnails and its tabs
        } else {
            fr = innermost(vm, "NameEntry", "selectChar", &t);
            screen = 'g';  // the name entry, its grid's loop
            if (!fr) {
                fr = innermost(vm, "NameEntry", "scriptMain", &t);
                screen = 'n';  // the name entry, its categories' loop
            }
            if (!fr) screen = 0;
        }
    } else {
        screen = 'c';
    }
    if (screen != screen_) {  // another screen: what was pending belongs to the last one
        asked_ = confirm_for_ = -1;
        drag_row_ = -1;
        asked_cx_ = asked_cy_ = -1;
        confirm_cx_ = -1;
        if (!(screen_ == 'g' && screen == 'n') && !(screen_ == 'n' && screen == 'g')) pending_cat_ = pending_cx_ = -1;
        screen_ = screen;
    }
    if (!screen) {
        static const bool dbg = std::getenv("AMG_MOUSE_STACKS") != nullptr;
        if (dbg)
            for (Thread* th : vm.live_threads)
                if (!th->done && !th->frames.empty()) {
                    std::fprintf(stderr, "stack %lld:", (long long)vm.tick);
                    for (const Frame& x : th->frames)
                        if (x.method && x.method->cls) std::fprintf(stderr, " %s.%s", x.method->cls->name.c_str(), x.method->name.c_str());
                    std::fprintf(stderr, "\n");
                }
        return false;
    }
    if (screen == 'w') {  // click = confirm, right click = cancel, for one frame
        probe.kind = 'w';
        if (free_frames_ > 0) {
            --free_frames_;
        } else if (click || right_click) {
            mask = right_click ? CANCEL : CONFIRM;
            free_frames_ = 1;
        }
        return true;
    }
    if (screen == 'q') return choice_update(vm, seen, t->stack[fr->base], mx, my, inside, click, right_click, wheel_steps, mask);
    if (screen == 'k' && fr->base + 2 < t->stack.size())
        return topic_update(vm, seen, t->stack[fr->base + 1], t->stack[fr->base + 2], mx, my, inside, click, right_click,
                            wheel_steps, mask);
    if (screen == 'l') {  // the owner's list, and its locals 2 (FavorGraph) and 3 (TimeIcon) that a move updates
        const size_t at = fr->base + 1;
        if (at + 3 >= t->stack.size()) return false;
        return shiori_update(vm, seen, t->stack[fr->base], t->stack[at + 2], t->stack[at + 3], mx, my, inside, click,
                             right_click, wheel_steps, mask);
    }
    if (screen == 'y') {
        const size_t cursor_at = fr->base + 1 + 7;  // the grid's cursor sprite: local 7 of Gallery.scriptMain
        if (cursor_at >= t->stack.size()) return false;

        return gallery_update(vm, seen, t->stack[fr->base], t->stack[cursor_at], mx, my, inside, click, right_click,
                              wheel_steps, mask);
    }
    if (screen == 's') return settings_update(vm, seen, dialog, mx, my, inside, down, click, right_click, wheel_steps, mask);
    if (screen == 'g' || screen == 'n')
        return name_update(vm, seen, t->stack[fr->base], screen == 'g', mx, my, inside, click, right_click, wheel_steps, mask);
    probe.kind = 'c';
    if (const Value* cur = field(f.menu, "cursor"); cur && cur->tag == Tag::Obj) {
        const auto* o = static_cast<ObjObj*>(cur->p.get());
        if (!o->iv.empty() && o->iv[0].tag == Tag::Int) arrow_uid = o->iv[0].i;
    }
    const int64_t n = int_field(f.menu, "nofCom", 0), cur_y = int_field(f.menu, "curY"), next_y = int_field(f.menu, "nextCurY");
    const int64_t state = int_field(f.menu, "state"), state_next = int_field(f.menu, "stateNext");
    const Value* avail = field(f.menu, "available");
    // Accepting input: available, not opening (1), closing (2) or disabled (5).
    const bool active = avail && avail->truthy() && state != 1 && state != 2 && state != 5 && state_next != 1 &&
                        state_next != 2 && state_next != 5;

    // The entries where they are drawn: CommandMenu.icon's sprites by their screen object.
    std::unordered_map<int64_t, const DrawItem*> by_uid;
    for (const DrawItem& it : seen.items)
        if (it.uid) by_uid[it.uid] = &it;
    const Value* icons = field(f.menu, "icon");
    const double units = VIEW_SCALE / VIEW_UNIT;
    int target = -1;
    double target_area = 1e18, sel_x = 0, sel_y = 0, tgt_x = 0, tgt_y = 0;
    bool has_sel = false;
    if (icons && icons->tag == Tag::Arr) {
        const auto& items = static_cast<ArrObj*>(icons->p.get())->items;
        for (size_t i = 0; i < items.size() && int64_t(i) < n; ++i) {
            if (items[i].tag != Tag::Obj) continue;
            const auto* o = static_cast<ObjObj*>(items[i].p.get());
            // The port's own entry (PortMenuEntry) holds its stroke as a Sprite in its first field.
            if (!o->iv.empty() && o->iv[0].tag == Tag::Obj) o = static_cast<ObjObj*>(o->iv[0].p.get());
            if (o->iv.empty() || o->iv[0].tag != Tag::Int) continue;
            auto it = by_uid.find(o->iv[0].i);
            if (it == by_uid.end()) continue;
            const DrawItem& d = *it->second;
            const double hw = d.rect[2] * std::fabs(d.scale[0]) * units / 2, hh = d.rect[3] * std::fabs(d.scale[1]) * units / 2;
            if (int64_t(i) == cur_y) {
                has_sel = true;
                sel_x = d.centre[0];
                sel_y = d.centre[1];
            }
            if (!inside || d.alpha <= 0.5 || std::fabs(mx - d.centre[0]) > hw || std::fabs(my - d.centre[1]) > hh) continue;
            if (hw * hh < target_area) {
                target_area = hw * hh;
                target = int(i);
                tgt_x = d.centre[0];
                tgt_y = d.centre[1];
            }
        }
    }
    probe.has_sel = has_sel;
    probe.sel_x = sel_x;
    probe.sel_y = sel_y;
    probe.has_tgt = target >= 0;
    probe.tgt_x = tgt_x;
    probe.tgt_y = tgt_y;
    probe.on = target >= 0 && target == cur_y;

    const bool moved = last_x_ < 1e8 && std::hypot(mx - last_x_, my - last_y_) > 1.0;
    last_x_ = mx;
    last_y_ = my;
    if (asked_ >= 0 && (next_y == asked_ || ++asked_age_ > 10)) asked_ = -1;  // taken (or lost: ask again)
    if (free_frames_ > 0) {
        --free_frames_;
        return true;
    }
    if (right_click) {
        confirm_for_ = -1;
        mask = CANCEL;
        free_frames_ = 1;
        return true;
    }
    if (!active || n <= 0) return true;

    auto select = [&](int i) {
        if (i < 0 || i >= n || i == next_y || i == asked_) return;
        ClassObj* cls = vm.find_class("PortMouse");
        if (!cls) return;
        pending = {f.thread, f.frame, f.method};
        Thread* t = vm.new_thread(vm.make_obj(cls), "select", {f.menu, Value::integer(i), Value::integer(f.sound)}, "thread");
        t->wake_tick = vm.tick;
        t->start();
        asked_ = i;
        asked_age_ = 0;
    };
    if (wheel_steps != 0) {  // an entry a notch, round the ends as the pad does
        const int64_t from = asked_ >= 0 ? asked_ : next_y;
        select(int(((from - wheel_steps) % n + n) % n));
        confirm_for_ = -1;
        return true;
    }
    if (click && target >= 0) {
        if (target == cur_y && next_y == cur_y && asked_ < 0) {
            mask = CONFIRM;
            free_frames_ = 1;
            confirm_for_ = -1;
            return true;
        }
        select(target);
        confirm_for_ = target;
        return true;
    }
    if (confirm_for_ >= 0) {
        if (cur_y == confirm_for_ && next_y == confirm_for_ && asked_ < 0) {
            mask = CONFIRM;
            free_frames_ = 1;
            confirm_for_ = -1;
        }
        return true;
    }
    if (moved && target >= 0) select(target);
    return true;
}

namespace {

double num_field(const Value& obj, const char* name, double fallback = 0) {
    const Value* v = field(obj, name);
    if (!v) return fallback;
    if (v->tag == Tag::Int) return double(v->i);
    if (v->tag == Tag::Float) return v->f;
    return fallback;
}

// A CommandMenu accepting input (as in step 3): available, not opening, closing or disabled.
bool command_active(const Value& menu) {
    const int64_t state = int_field(menu, "state"), state_next = int_field(menu, "stateNext");
    const Value* avail = field(menu, "available");
    return avail && avail->truthy() && state != 1 && state != 2 && state != 5 && state_next != 1 && state_next != 2 &&
           state_next != 5;
}

// The entry of a CommandMenu under the mouse (its icon sprites, the smallest that holds it), -1 when none.
int command_entry_at(const Value& menu, const Scene& seen, double mx, double my) {
    const Value* icons = field(menu, "icon");
    const int64_t n = int_field(menu, "nofCom", 0);
    if (!icons || icons->tag != Tag::Arr) return -1;
    const auto& items = static_cast<ArrObj*>(icons->p.get())->items;
    int target = -1;
    double area = 1e18;
    for (size_t i = 0; i < items.size() && int64_t(i) < n; ++i) {
        const DrawItem* d = item_of(seen, uid_of(&items[i]));
        if (!d || d->alpha <= 0.5 || !hit(d, mx, my)) continue;
        const double a = d->rect[2] * std::fabs(d->scale[0]) * d->rect[3] * std::fabs(d->scale[1]);
        if (a < area) {
            area = a;
            target = int(i);
        }
    }
    return target;
}

}  // namespace

// The Settings screen (ConfigDialog under ConfigMain.scriptMain) and the port's options (PortDialog under
// PortMain.scriptMain): the rows of the page shown (ConfigPage.items, at 12 + posY, as the dialog's own cursor), the
// "Defaults" row after them (at 86), the tabs (ConfigPage.tagBase sprites) and the L/R marks beside them.
bool MouseMenus::settings_update(VM& vm, const Scene& seen, const Value& dialog, double mx, double my, bool inside,
                                 bool down, bool click, bool right_click, int wheel_steps, int& mask) {
    probe.kind = 's';
    const int64_t st = int_field(dialog, "state"), sn = int_field(dialog, "stateNext");
    const bool active = (st == 3 || st == 4) && (sn == 0 || sn == 3 || sn == 4);
    const Value* page_list = field(dialog, "pageList");
    if (!page_list) return true;
    const std::vector<Value> pages = elems(*page_list);
    const int64_t page = int_field(dialog, "page", 0), cur = int_field(dialog, "curY", 0);
    if (page < 0 || size_t(page) >= pages.size()) return true;
    const Value* items = field(pages[size_t(page)], "items");
    if (!items) return true;
    const std::vector<Value> rows = elems(*items);
    const int n = int(rows.size());  // and row n: "Defaults"
    std::vector<const DrawItem*> tabs;
    for (const Value& p : pages) tabs.push_back(item_of(seen, uid_of(field(p, "tagBase"))));
    const DrawItem* cursor = item_of(seen, uid_of(field(dialog, "cursor")));
    // The arrow is the only mark of the selected row here (no row is lit): in mouse mode it gives way to the port's bar,
    // in the colour of the game's selected tab (9, 80, 118) over the panel, across the rows' width (the hover band).
    arrow_uid = uid_of(field(dialog, "cursor"));
    mark.x0 = -86;
    mark.x1 = 196;
    mark.h = 17;
    mark.light[0] = 9 / 255.0;
    mark.light[1] = 80 / 255.0;
    mark.light[2] = 118 / 255.0;
    mark.alpha = 0.8;
    const Layout L = layout_of(seen, cursor ? cursor : (tabs.empty() ? nullptr : tabs[0]));
    auto row_y = [&](int i) { return i < n ? 12.0 + num_field(rows[size_t(i)], "posY") : 86.0; };

    int hover = -1;
    bool back = false;  // the "back" guide at the right of the Defaults line
    if (inside && mx >= L.X(-80) && mx <= L.X(205))
        for (int i = 0; i <= n; ++i)
            if (std::fabs(my - L.Y(row_y(i))) <= L.D(9)) hover = i;
    if (hover == n && mx > L.X(88)) {
        back = mx >= L.X(140);
        hover = -1;
    }
    int hover_tab = -1;  // a tab, or -2 the L mark, -3 the R mark
    for (size_t k = 0; k < tabs.size(); ++k)
        if (inside && tabs[k] && tabs[k]->alpha > 0.5 && hit(tabs[k], mx, my)) hover_tab = int(k);
    if (inside && hover_tab < 0 && !tabs.empty() && tabs.front() && tabs.back() &&
        std::fabs(my - tabs.front()->centre[1]) <= L.D(12)) {
        if (mx < tabs.front()->centre[0] && mx > L.X(-106)) hover_tab = -2;
        if (mx > tabs.back()->centre[0] && mx < L.X(214)) hover_tab = -3;
    }
    probe.has_sel = cur >= 0 && cur <= n;
    probe.sel_x = L.X(-79);
    probe.sel_y = L.Y(row_y(int(cur)));
    probe.has_tgt = hover >= 0;
    probe.tgt_x = mx;
    probe.tgt_y = hover >= 0 ? L.Y(row_y(hover)) : 0;
    probe.on = hover >= 0 && hover == cur;

    const bool moved = last_x_ < 1e8 && std::hypot(mx - last_x_, my - last_y_) > 1.0;
    last_x_ = mx;
    last_y_ = my;
    if (asked_ >= 0 && (cur == asked_ || ++asked_age_ > 10)) asked_ = -1;
    if (free_frames_ > 0) {
        --free_frames_;
        return true;
    }
    if (right_click) {
        mask = CANCEL;
        free_frames_ = 1;
        confirm_for_ = drag_row_ = -1;
        return true;
    }
    if (!active) return true;
    const Value vdialog = dialog;
    auto row = [&](int i) {
        if (i < 0 || i > n || i == cur || i == asked_) return;
        if (run_port(vm, "row", {vdialog, Value::integer(i)})) {
            asked_ = i;
            asked_age_ = 0;
        }
    };
    // A slider's value under the mouse: a volume (ConfigVolume, notes 18 apart, 0 to 5) or a speed (ConfigSpeed,
    // arrows 12 apart, 0 to 3); -1 for another kind of row.
    auto slider = [&](int i, bool& volume) -> int {
        if (i < 0 || i >= n) return -1;
        const Value& it = rows[size_t(i)];
        volume = kind_of(vm, it, "ConfigVolume");
        if (!volume && !kind_of(vm, it, "ConfigSpeed")) return -1;
        const Value* values = field(it, "value");
        if (!values) return -1;
        const std::vector<Value> v = elems(*values);
        static const double VOL[] = {105, 123, 141, 159, 177}, SPD[] = {123, 135, 147, 159};
        int goal = 0;
        for (size_t k = 0; k < v.size() && k < 5; ++k) {
            const DrawItem* d = item_of(seen, uid_of(&v[k]));
            const double x = d ? d->centre[0] : L.X(volume ? VOL[k] : SPD[std::min<size_t>(k, 3)]);
            if (mx >= x - L.D(volume ? 9 : 6)) goal = volume ? int(k) + 1 : int(k);
        }
        return goal;
    };
    if (drag_row_ >= 0 && down) {  // a slider held: its value follows the mouse
        bool volume = false;
        const int goal = slider(drag_row_, volume);
        if (goal >= 0 && goal != drag_goal_ && run_port(vm, volume ? "volume" : "speed", {vdialog, Value::integer(drag_row_), Value::integer(goal)}))
            drag_goal_ = goal;
        return true;
    }
    if (wheel_steps != 0) {  // over the tabs a page a notch, elsewhere a row
        confirm_for_ = -1;
        if (hover_tab != -1) {
            run_port(vm, "tab", {vdialog, Value::integer(wheel_steps > 0 ? -1 : 100)});
        } else {
            const int64_t from = asked_ >= 0 ? asked_ : cur;
            row(int(((from - wheel_steps) % (n + 1) + (n + 1)) % (n + 1)));
        }
        return true;
    }
    if (click) {
        confirm_for_ = -1;
        if (back) {
            mask = CANCEL;
            free_frames_ = 1;
        } else if (hover_tab >= 0) {
            if (hover_tab != page) run_port(vm, "tab", {vdialog, Value::integer(hover_tab)});
        } else if (hover_tab == -2 || hover_tab == -3) {
            run_port(vm, "tab", {vdialog, Value::integer(hover_tab == -2 ? -1 : 100)});
        } else if (hover == n) {  // Defaults: select it, then confirm
            if (cur == n) {
                mask = CONFIRM;
                free_frames_ = 1;
            } else {
                row(n);
                confirm_for_ = n;
            }
        } else if (hover >= 0) {
            bool volume = false;
            const int goal = mx >= L.X(96) ? slider(hover, volume) : -1;
            if (goal >= 0) {  // a note or an arrow: the value there, and a drag from it
                run_port(vm, volume ? "volume" : "speed", {vdialog, Value::integer(hover), Value::integer(goal)});
                drag_row_ = hover;
                drag_goal_ = goal;
                asked_ = hover;
                asked_age_ = 0;
            } else if (mx >= L.X(96)) {  // a text value: its left half back, the rest forward (the dialog's selectValueL/R)
                run_port(vm, "step", {vdialog, Value::integer(hover), Value::integer(mx < L.X(141) ? -1 : 1)});
                asked_ = hover;
                asked_age_ = 0;
            } else if (hover == cur) {  // the label of the selected row: its action, as confirm
                mask = CONFIRM;
                free_frames_ = 1;
            } else {
                row(hover);
            }
        }
        return true;
    }
    if (confirm_for_ >= 0) {
        if (cur == confirm_for_ && asked_ < 0) {
            mask = CONFIRM;
            free_frames_ = 1;
            confirm_for_ = -1;
        }
        return true;
    }
    if (moved && hover >= 0) row(hover);
    return true;
}

// The name entry (NameEntry.scriptMain: the categories, a CommandMenu in `menu`; NameEntry.selectChar: the grid of
// NameEntryList in `list`, its orange cursor at curX/curY of the rows shown from linePos), and the name field
// (NameEntryEdit in `edit`, curX 1 to 6 the characters, 0 Back, 7 Decide; the owner keeps the copy in its curX).
bool MouseMenus::name_update(VM& vm, const Scene& seen, const Value& owner, bool grid, double mx, double my, bool inside,
                             bool click, bool right_click, int wheel_steps, int& mask) {
    probe.kind = 'n';
    const Value* menu = field(owner, "menu");
    const Value* list = field(owner, "list");
    const Value* edit = field(owner, "edit");
    if (!menu || !list || !edit || menu->tag != Tag::Obj || list->tag != Tag::Obj || edit->tag != Tag::Obj) return true;
    arrow_uid = uid_of(field(*menu, "cursor"));
    const DrawItem* gcur = item_of(seen, uid_of(field(*list, "cursor")));
    const Layout L = layout_of(seen, gcur);

    // The grid's cells, as NameEntryList.moveCursor places its cursor.
    const Value* cl = field(*list, "curList");
    const std::vector<Value> lines = cl ? elems(*cl) : std::vector<Value>();
    const int64_t mode = int_field(*list, "mode", 0), line_pos = int_field(*list, "linePos", 0);
    const Value* sp = field(*list, "subPage");
    const bool sub = sp && sp->truthy();
    static const int SHOWN[] = {5, 10, 10, 7, 10, 10};
    const int shown = sub ? 9 : (mode >= 0 && mode < 6 ? SHOWN[mode] : 10);
    const int64_t gx = int_field(*list, "curX", 0), gy = int_field(*list, "curY", 0);
    int hx = -1, hy = -1;
    auto cell = [&](int c, int r, double& x, double& y, double& hw) {
        const Value& line = lines[size_t(line_pos + r)];
        if (mode == 4 && !sub) {
            static const double X4[] = {-60, 3, 75, 138};
            x = X4[std::min(c, 3)];
            y = -77 + 21.0 * r;
            hw = 25;
            return;
        }
        const int len = line.tag == Tag::Str ? int(chars(static_cast<StrObj*>(line.p.get())->b).size()) : 0;
        x = -96 + 9.0 * (15 - len + c * 2) + 1;
        y = sub ? -52 + 21.0 * r : (mode == 0 || mode == 3 || r < 5) ? -76 + 21.0 * r : -70 + 21.0 * r;
        hw = 9;
    };
    double sel_x = 0, sel_y = 0, tgt_x = 0, tgt_y = 0;
    bool has_sel = false;
    for (int r = 0; r < shown && line_pos + r < int64_t(lines.size()); ++r) {
        const Value& line = lines[size_t(line_pos + r)];
        const int cols = line.tag == Tag::Arr ? int(static_cast<ArrObj*>(line.p.get())->items.size())
                         : line.tag == Tag::Str ? int(chars(static_cast<StrObj*>(line.p.get())->b).size()) : 0;
        for (int c = 0; c < cols; ++c) {
            double x, y, hw;
            cell(c, r, x, y, hw);
            if (c == gx && r == gy) {
                has_sel = true;
                sel_x = L.X(x);
                sel_y = L.Y(y);
            }
            if (inside && std::fabs(mx - L.X(x)) <= L.D(hw) && std::fabs(my - L.Y(y)) <= L.D(10.5)) {
                hx = c;
                hy = r;
                tgt_x = L.X(x);
                tgt_y = L.Y(y);
            }
        }
    }
    // The name field: the characters (1 to 6) and the Back (0) and Decide (7) marks with their words.
    static const double EX[] = {-65, -12, 6, 24, 48, 66, 84, 137};
    int he = -1;
    if (inside)
        for (int i = 0; i < 8; ++i)
            if (std::fabs(my - L.Y(-119)) <= L.D(12) && std::fabs(mx - L.X(EX[i] + 1)) <= L.D(i == 0 || i == 7 ? 17 : 9)) he = i;
    if (inside && he < 0) {
        static const char* WORDS[] = {"text0", "text1", "text2", "text3", "text4"};
        for (int k = 0; k < 5; ++k) {
            const DrawItem* d = item_of(seen, uid_of(field(*edit, WORDS[k])));
            if (d && d->alpha > 0.5 && hit(d, mx, my, L.D(2))) he = k < 3 ? 0 : 7;
        }
    }
    const int hc = inside ? command_entry_at(*menu, seen, mx, my) : -1;
    const int64_t ex = int_field(owner, "curX", 1);
    const int64_t cat_y = int_field(*menu, "curY"), cat_next = int_field(*menu, "nextCurY");
    const int64_t n_cat = int_field(*menu, "nofCom", 0);

    probe.has_sel = grid ? has_sel : true;
    probe.sel_x = grid ? sel_x : 0;
    probe.sel_y = grid ? sel_y : double(cat_y);
    probe.has_tgt = grid ? hx >= 0 : hc >= 0;
    probe.tgt_x = grid ? tgt_x : 0;
    probe.tgt_y = grid ? tgt_y : double(hc);
    probe.on = grid ? (hx >= 0 && hx == gx && hy == gy) : (hc >= 0 && hc == cat_y);

    const bool moved = last_x_ < 1e8 && std::hypot(mx - last_x_, my - last_y_) > 1.0;
    last_x_ = mx;
    last_y_ = my;
    if (asked_ >= 0 && (cat_next == asked_ || ++asked_age_ > 10)) asked_ = -1;
    if (asked_cx_ >= 0 && ((gx == asked_cx_ && gy == asked_cy_) || ++asked_cage_ > 10)) asked_cx_ = asked_cy_ = -1;
    if (pending_cat_ >= 0 && ++pending_age_ > 30) pending_cat_ = -1;
    if (free_frames_ > 0) {
        --free_frames_;
        return true;
    }
    if (right_click) {
        mask = CANCEL;
        free_frames_ = 1;
        confirm_for_ = confirm_cx_ = pending_cat_ = pending_cx_ = -1;
        return true;
    }
    const Value vowner = owner, vlist = *list;
    auto name_at = [&](int i) {
        if (i != ex) run_port(vm, "name", {vowner, Value::integer(i)});
    };
    if (!grid) {  // the categories
        if (!command_active(*menu) || n_cat <= 0) return true;
        auto category = [&](int i) {
            if (i < 0 || i >= n_cat || i == cat_next || i == asked_) return;
            if (run_port(vm, "category", {vowner, Value::integer(i)})) {
                asked_ = i;
                asked_age_ = 0;
            }
        };
        if (pending_cat_ >= 0) {  // a category clicked while the grid was in use: there, then into its grid
            category(pending_cat_);
            confirm_for_ = pending_cat_;
            pending_cat_ = -1;
            return true;
        }
        if (wheel_steps != 0) {
            const int64_t from = asked_ >= 0 ? asked_ : cat_next;
            category(int(((from - wheel_steps) % n_cat + n_cat) % n_cat));
            confirm_for_ = -1;
            return true;
        }
        if (click) {
            confirm_for_ = -1;
            if (he >= 0) {
                name_at(he);
            } else if (hc >= 0) {
                if (hc == cat_y && cat_next == cat_y && asked_ < 0) {
                    mask = CONFIRM;
                    free_frames_ = 1;
                } else {
                    category(hc);
                    confirm_for_ = hc;
                }
            } else if (hx >= 0) {  // a character while the categories have the cursor: into the grid, then that one
                mask = CONFIRM;
                free_frames_ = 1;
                pending_cx_ = hx;
                pending_cy_ = hy;
            }
            return true;
        }
        if (confirm_for_ >= 0) {
            if (cat_y == confirm_for_ && cat_next == confirm_for_ && asked_ < 0) {
                mask = CONFIRM;
                free_frames_ = 1;
                confirm_for_ = -1;
            }
            return true;
        }
        if (moved && hc >= 0) category(hc);
        if (moved && he >= 1 && he <= 6) name_at(he);
        return true;
    }
    // The grid.
    const bool list_active = int_field(*list, "state") == 0 && int_field(*list, "stateNext") == 0;
    if (!list_active || lines.empty()) return true;
    auto select_cell = [&](int c, int r) {
        if (c < 0 || (c == gx && r == gy) || (c == asked_cx_ && r == asked_cy_)) return;
        if (run_port(vm, "cell", {vlist, Value::integer(c), Value::integer(r)})) {
            asked_cx_ = c;
            asked_cy_ = r;
            asked_cage_ = 0;
        }
    };
    if (pending_cx_ >= 0) {  // clicked from the categories: the character, then confirm it
        select_cell(pending_cx_, pending_cy_);
        confirm_cx_ = pending_cx_;
        confirm_cy_ = pending_cy_;
        pending_cx_ = -1;
        return true;
    }
    if (wheel_steps != 0) {  // a row a notch (the list scrolls at its ends, as for the pad)
        confirm_cx_ = -1;
        run_port(vm, "scroll", {vlist, Value::integer(wheel_steps > 0 ? -1 : 1)});
        return true;
    }
    if (click) {
        confirm_cx_ = -1;
        if (hx >= 0) {
            if (hx == gx && hy == gy && asked_cx_ < 0) {
                mask = CONFIRM;
                free_frames_ = 1;
            } else {
                select_cell(hx, hy);
                confirm_cx_ = hx;
                confirm_cy_ = hy;
            }
        } else if (he >= 0) {
            name_at(he);
        } else if (hc >= 0) {  // another category: back out of the grid, then that one (on the categories' side)
            mask = CANCEL;
            free_frames_ = 1;
            pending_cat_ = hc;
            pending_age_ = 0;
        }
        return true;
    }
    if (confirm_cx_ >= 0) {
        if (gx == confirm_cx_ && gy == confirm_cy_ && asked_cx_ < 0) {
            mask = CONFIRM;
            free_frames_ = 1;
            confirm_cx_ = -1;
        }
        return true;
    }
    if (moved && hx >= 0) select_cell(hx, hy);
    if (moved && he >= 1 && he <= 6) name_at(he);
    return true;
}

// One index driven by the mouse (steps 8 and 9): hover selects (after a real move), a click on the selected entry is
// confirm, a click on another selects it and confirms once the screen's index is there, right click is cancel, the
// wheel moves an entry a notch (round the ends). `select(i)` starts the screen's PortMouse thread and returns whether
// it did; `cur` is the screen's index now.
bool MouseMenus::drive(int hover, int cur, int n, double mx, double my, bool click, bool right_click, int wheel_steps,
                       const std::function<bool(int)>& select, int& mask) {
    const bool moved = last_x_ < 1e8 && std::hypot(mx - last_x_, my - last_y_) > 1.0;
    last_x_ = mx;
    last_y_ = my;
    probe.on = hover >= 0 && hover == cur;
    if (asked_ >= 0 && (cur == asked_ || ++asked_age_ > 10)) asked_ = -1;
    if (free_frames_ > 0) {
        --free_frames_;
        return true;
    }
    if (right_click) {
        mask = CANCEL;
        free_frames_ = 1;
        confirm_for_ = -1;
        return true;
    }
    auto ask = [&](int i) {
        if (i < 0 || i >= n || i == cur || i == asked_) return;
        if (select(i)) {
            asked_ = i;
            asked_age_ = 0;
        }
    };
    if (wheel_steps != 0 && n > 0) {
        const int from = asked_ >= 0 ? asked_ : cur;
        ask(((from - wheel_steps) % n + n) % n);
        confirm_for_ = -1;
        return true;
    }
    if (click && hover >= 0) {
        if (hover == cur && asked_ < 0) {
            mask = CONFIRM;
            free_frames_ = 1;
            confirm_for_ = -1;
        } else {
            ask(hover);
            confirm_for_ = hover;
        }
        return true;
    }
    if (confirm_for_ >= 0) {
        if (cur == confirm_for_ && asked_ < 0) {
            mask = CONFIRM;
            free_frames_ = 1;
            confirm_for_ = -1;
        }
        return true;
    }
    if (moved && hover >= 0) ask(hover);
    return true;
}

// A choice (Selector.waitSelect: TextWindow's answers to a question, MahjongContinue, MahjongStageSelector): a grid of
// selX + 1 columns and selY + 1 rows, cell (x, y) at orgX + pitchX * x, orgY + pitchY * y, the size of its lit bar
// (`spr`, a plain rectangle that is the mark of the selection: there is no arrow). The index is x * (selY + 1) + y,
// below `limit`, as the loop counts it.
bool MouseMenus::choice_update(VM& vm, const Scene& seen, const Value& sel, double mx, double my, bool inside, bool click,
                               bool right_click, int wheel_steps, int& mask) {
    probe.kind = 'q';
    const DrawItem* bar = item_of(seen, uid_of(field(sel, "spr")));
    const Value* paused = field(sel, "pauseFlag");
    if (!bar || (paused && paused->truthy())) return true;
    const Layout L = layout_of(seen, bar);
    const double ox = num_field(sel, "orgX"), oy = num_field(sel, "orgY"), px = num_field(sel, "pitchX"), py = num_field(sel, "pitchY");
    const int cols = int(int_field(sel, "selX", 0)) + 1, rows = int(int_field(sel, "selY", 0)) + 1;
    const int n = int(std::min<int64_t>(int64_t(cols) * rows, int_field(sel, "limit", 10000)));
    const int cur = int(int_field(sel, "curX", 0) * rows + int_field(sel, "curY", 0));
    double hw, hh;
    half_size(*bar, hw, hh);
    int hover = -1;
    for (int i = 0; i < n; ++i) {
        const double x = L.X(ox + px * (i / rows)), y = L.Y(oy + py * (i % rows));
        if (i == cur) {
            probe.has_sel = true;
            probe.sel_x = x;
            probe.sel_y = y;
        }
        if (inside && std::fabs(mx - x) <= hw && std::fabs(my - y) <= hh) {
            hover = i;
            probe.has_tgt = true;
            probe.tgt_x = x;
            probe.tgt_y = y;
        }
    }
    const Value vsel = sel;
    return drive(hover, cur, n, mx, my, click, right_click, wheel_steps, [&](int i) {
        return run_port(vm, "choice", {vsel, Value::integer(i / rows), Value::integer(i % rows)});
    }, mask);
}

// The conversation's topics (KaiwaMain.wadaiSelect(panel, target)): the ten WadaiIcons of WadaiPanel.wicon, each hit by
// its `base` picture; the index is WadaiPanel.wadai, moved as WadaiPanel.moveCursor does (sound 32, the old icon out
// of focus, the new in, the target set told).
bool MouseMenus::topic_update(VM& vm, const Scene& seen, const Value& panel, const Value& target, double mx, double my,
                              bool inside, bool click, bool right_click, int wheel_steps, int& mask) {
    probe.kind = 'k';
    const Value* icons = field(panel, "wicon");
    if (!icons) return true;
    const std::vector<Value> wicon = elems(*icons);
    const int n = int(wicon.size()), cur = int(int_field(panel, "wadai", 0));
    int hover = -1;
    double area = 1e18;
    for (int i = 0; i < n; ++i) {
        const DrawItem* d = item_of(seen, uid_of(field(wicon[size_t(i)], "base")));
        if (!d) continue;
        if (i == cur) {
            probe.has_sel = true;
            probe.sel_x = d->centre[0];
            probe.sel_y = d->centre[1];
        }
        double hw, hh;
        half_size(*d, hw, hh);
        if (!inside || d->alpha <= 0.5 || std::fabs(mx - d->centre[0]) > hw || std::fabs(my - d->centre[1]) > hh) continue;
        if (hw * hh < area) {
            area = hw * hh;
            hover = i;
            probe.has_tgt = true;
            probe.tgt_x = d->centre[0];
            probe.tgt_y = d->centre[1];
        }
    }
    const Value vpanel = panel, vtarget = target;
    return drive(hover, cur, n, mx, my, click, right_click, wheel_steps, [&](int i) {
        return run_port(vm, "topic", {vpanel, vtarget, Value::integer(i)});
    }, mask);
}

// The Load and Save lists (LoadMenu/SaveMenu.selectShiori over a ShioriList): three slots shown (ScrollList.cur 0 to 2,
// the first shown is `pos`), slot k at baseY + pitch * (k + 1) as ScrollList.run places them, the selected one lit
// (enable), the rest dimmed: the light is the mark, there is no arrow. Hover a slot: PortMouse.shiori moves the
// selection there as up/down do plus what the owner's loop does after a move (sound 32, ShioriData.setNum, the
// calendar's day, the time icon, the favour graph). The wheel scrolls one slot (the list's own up/down, which scroll at
// the ends). A click on the selected slot is confirm (the owner's load or save question).
bool MouseMenus::shiori_update(VM& vm, const Scene& seen, const Value& owner, const Value& favor, const Value& time,
                               double mx, double my, bool inside, bool click, bool right_click, int wheel_steps, int& mask) {
    const Value* list = field(owner, "list");
    if (!list || list->tag != Tag::Obj) return false;
    probe.kind = 'l';
    const int64_t btm = int_field(*list, "listBtm", 0), cur = int_field(*list, "cur", 0);
    const int64_t state = int_field(*list, "state", 0), state_next = int_field(*list, "stateNext", 0);
    const double bx = num_field(*list, "baseX"), by = num_field(*list, "baseY"), pitch = num_field(*list, "pitch");
    const int shown = int(std::min<int64_t>(btm, 2)) + 1;
    const double shift = btm < 2 ? double(1 - btm) : 0.0;
    // The slots' pictures are composites (DialogBox): the hover band is the slot's pitch high and the box's width, read
    // from the widest item of the list's own layer near the slot (none read: the panel's middle third).
    const DrawItem* ref = nullptr;
    for (const DrawItem& d : seen.items)
        if (d.shrunk && !ref) ref = &d;
    const Layout L = layout_of(seen, ref);
    int hover = -1;
    for (int k = 0; k < shown; ++k) {
        const double y = L.Y(by + pitch * (k + 1 + shift)), x = L.X(bx);
        if (k == cur) {
            probe.has_sel = true;
            probe.sel_x = x;
            probe.sel_y = y;
        }
        if (inside && std::fabs(my - y) <= L.D(pitch / 2) && mx >= L.X(bx - SHIORI_HALF_W) && mx <= L.X(bx + SHIORI_HALF_W)) {
            hover = k;
            probe.has_tgt = true;
            probe.tgt_x = x;
            probe.tgt_y = y;
        }
    }
    if (state == 1 || state == 3 || state_next == 1 || state_next == 3) return true;  // opening or closing
    const Value vlist = *list, vfavor = favor, vtime = time;
    if (wheel_steps != 0 && free_frames_ == 0) {
        last_x_ = mx;
        last_y_ = my;
        confirm_for_ = -1;
        run_port(vm, "shioriStep", {vlist, vfavor, vtime, Value::integer(wheel_steps > 0 ? -1 : 1)});
        return true;
    }
    return drive(hover, int(cur), shown, mx, my, click, right_click, 0, [&](int k) {
        return run_port(vm, "shiori", {vlist, vfavor, vtime, Value::integer(k)});
    }, mask);
}

// The gallery (Gallery.scriptMain): its loop moves the cursor and the page itself when Gallery.nextX/nextY or
// nextPage differ from curX/curY and curPage (sound 31 and the cursor's move; sound 21 and pageSelect), the same path the
// game's own touch test (checkTapThumb) takes. So the mouse only writes those fields: one step at any distance. The
// cells are checkTapThumb's (8 columns at x -175 + 50 i, 4 rows at y -69 + 50 j, 50 square, index i * 4 + j); the
// tabs are the sprites tag0..tag2 (pages 0-2, 3-6, 7 on); the wheel turns a page. Only while the grid's cursor shows
// (not in the viewer, not during the opening or the closing).
bool MouseMenus::gallery_update(VM& vm, const Scene& seen, const Value& gallery, const Value& cursor, double mx, double my,
                                bool inside, bool click, bool right_click, int wheel_steps, int& mask) {
    (void)vm;
    const DrawItem* cur_item = item_of(seen, uid_of(field(cursor, "cur0")));  // a FrameCursor: its corner
    if (!cur_item || cur_item->alpha <= 0.5) return false;  // not the grid: the old pointer (the viewer's touch)
    probe.kind = 'y';
    auto set = [&](const char* name, int64_t v) {
        if (Value* f = field(gallery, name)) *f = Value::integer(v);
    };
    const Layout L = layout_of(seen, cur_item);
    const int cur = int(int_field(gallery, "curX", 0) * 4 + int_field(gallery, "curY", 0));
    const int64_t page = int_field(gallery, "curPage", 0), next_page = int_field(gallery, "nextPage", 0);
    const Value* pages = field(gallery, "page");
    const int n_pages = pages ? int(elems(*pages).size()) : 0;
    int hover = -1;
    for (int i = 0; i < 32; ++i) {
        const double x = L.X(-175 + 50.0 * (i / 4)), y = L.Y(-69 + 50.0 * (i % 4));
        if (i == cur) {
            probe.has_sel = true;
            probe.sel_x = x;
            probe.sel_y = y;
        }
        if (inside && std::fabs(mx - x) <= L.D(25) && std::fabs(my - y) <= L.D(25)) {
            hover = i;
            probe.has_tgt = true;
            probe.tgt_x = x;
            probe.tgt_y = y;
        }
    }
    static const char* TAGS[] = {"tag0", "tag1", "tag2"};
    static const int FIRST[] = {0, 3, 7};
    int tab = -1;
    for (int k = 0; k < 3; ++k)
        if (inside && hit(item_of(seen, uid_of(field(gallery, TAGS[k]))), mx, my)) tab = k;
    if (n_pages > 0 && free_frames_ == 0 && (wheel_steps != 0 || (click && tab >= 0))) {
        const int64_t to = tab >= 0 && click ? std::min<int64_t>(FIRST[tab], n_pages - 1)
                                             : ((next_page - wheel_steps) % n_pages + n_pages) % n_pages;
        if (to != page && next_page == page) set("nextPage", to);
        last_x_ = mx;
        last_y_ = my;
        confirm_for_ = -1;
        return true;
    }
    return drive(hover, cur, 32, mx, my, click, right_click, 0, [&](int i) {
        set("nextX", i / 4);
        set("nextY", i % 4);
        return true;
    }, mask);
}

}  // namespace amg
