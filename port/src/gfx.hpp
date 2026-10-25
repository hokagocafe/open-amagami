// The screen-object model of the port: sprites, backgrounds, text, characters, with a software
// renderer. A C++ rendition of tools/scfvm_gfx.py; see docs/technical/objects.md for what is
// confirmed and what is a hypothesis.
#pragma once
#include <cstdlib>

#include <array>
#include <cstdint>
#include <functional>
#include <future>
#include <deque>
#include <filesystem>
#include "datapath.hpp"
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpk.hpp"
#include "vm.hpp"

namespace amg {

// The game's screen: the PS2's 640x448, or the PS Vita's 960x544 (its scripts lay out for its own screen).
// Set once, before the first frame, by use_screen_of().
inline int SCREEN_W = 640, SCREEN_H = 448;
// The view (Vita): scripts place things in a 480x272 space and its pictures have twice that resolution, so a
// position is drawn at VIEW_UNIT x VIEW_SCALE screen pixels per unit and a picture at VIEW_SCALE pixels per
// texel; K2X_Script.setScrScl and setScrOrgY (primitives 6 and 7) set VIEW_SCALE and VIEW_ORG_Y. On the PS2 the
// view is the identity.
inline double VIEW_UNIT = 1.0, VIEW_SCALE = 1.0, VIEW_ORG_Y = 0.0;
// The size of the interface relative to the picture (1 = the game's own), a setting of the window (F5-4): the items of
// the layers from UI_FIRST_LAYER up shrink about the nearest edge or the middle of the screen; the backgrounds, the
// characters and anything that covers the screen stay as they are. The layer rule is a hypothesis from the event and
// settings screens (backgrounds 0, characters 2, text window 7, settings 4 and up); AMG_UI_LAYER changes it.
inline double UI_SCALE = 1.0;
inline int UI_FIRST_LAYER = 4;
inline void use_screen_of(const std::string& root) {
    bool vita = amg::is_vita(root);
    SCREEN_W = vita ? 960 : 640;
    SCREEN_H = vita ? 544 : 448;
    VIEW_UNIT = vita ? 2.0 : 1.0;
    // Also for the command line tool (the window reads config.txt too): AMG_UI_SCALE in percent, AMG_UI_LAYER.
    if (const char* e = std::getenv("AMG_UI_SCALE")) UI_SCALE = std::atoi(e) / 100.0;
    if (const char* e = std::getenv("AMG_UI_LAYER")) UI_FIRST_LAYER = std::atoi(e);
}

// An image with 8-bit RGBA texels, alpha in the PS2's range 0..128 (0x80 is opaque).
struct Image {
    int w = 0, h = 0;
    int glyph = 0;  // not 0: one of the game's button pictures (Graphics::ui_image); the scene shows the current family's instead
    std::vector<uint8_t> px;  // w*h*4
    const uint8_t* row(int y) const { return px.data() + size_t(y) * size_t(w) * 4; }
};
using ImagePtr = std::shared_ptr<Image>;

struct Tween {
    double remaining = 0.0;
    std::vector<double> target, a, b, current;

    Tween() = default;
    explicit Tween(const std::vector<double>& initial)
        : target(initial), a(initial.size(), 0.0), b(initial.size(), 0.0), current(initial) {}
    void start(const std::vector<double>& values, int mode, double time);
    void step(double dt);
    void finish();
};

struct Vib {
    Tween parts[3];  // base, amplitude, frequency
    std::vector<double> phase;
    bool scale = false;

    Vib() = default;
    explicit Vib(const std::vector<double>& initial, bool scale_ = false);
    void step(double dt);
    std::vector<double> value() const;
    double remaining() const { return parts[0].remaining; }
};

struct Spin {
    Tween base{std::vector<double>{0.0}};
    Tween speed{std::vector<double>{0.0}};
    double turned = 0.0;

    static void start(Tween& t, double value, int mode, double time, double v0);
    void set(int component, double value, int mode, double time);
    void step(double dt);
    double value() const { return base.current[0] + turned; }
};

struct Graphics;

struct SnowFlake {
    double x, y;
    int size;
    double speed, phase;
};

// Everything the renderer needs to draw one object, copied out of the live object. A frame is a
// list of these, so drawing can run on another thread while the machine goes on to the next tick.
struct DrawItem {
    int64_t layer = 0, order = 0;
    ImagePtr image;  // null: a plain rectangle
    double rect[4] = {0, 0, 0, 0};
    double alpha = 1.0;
    double light[3] = {1, 1, 1};
    double centre[2] = {0, 0}, scale[2] = {1, 1};
    double angle = 0.0;  // in units of pi
    bool scale_on_screen = false;
    double follow = 0.0;  // the part of the screen origin (VIEW_ORG_Y) this picture follows: 1 for a background, less for a character, 0 for the interface
    bool ui = false;      // always in the interface view (scale 1), whatever setScrScl says: a Vita Sprite
    bool snow = false;
    bool additive = false;
    bool clipped = false;     // only pixels inside clip (screen pixels: x0, y0, x1, y1) are drawn
    double clip[4] = {0, 0, 0, 0};  // adds its light to the frame (the light balls' glow) instead of covering it
    std::shared_ptr<const std::vector<SnowFlake>> flakes;
    double clock = 0.0;
};

struct Scene {
    std::vector<DrawItem> items;  // back to front
};

struct ScreenObject {
    int64_t uid = 0;
    int64_t layer = 0;
    ImagePtr image;  // null: a plain rectangle
    double rect[4] = {0, 0, 0, 0};
    Vib alpha{std::vector<double>{1.0}}, light{std::vector<double>{1.0, 1.0, 1.0}}, pos{std::vector<double>{0.0, 0.0}},
        zoom{std::vector<double>{1.0, 1.0}, true};
    Spin spin;
    bool paused = false;
    int64_t order = 0;
    std::string name;
    bool scale_on_screen = false;
    double follow = 0.0;        // see DrawItem::follow
    bool ui = false;            // see DrawItem::ui
    int font = 0, palette = 0;  // text characters

    virtual ~ScreenObject() = default;
    virtual std::unique_ptr<ScreenObject> clone() const { return std::make_unique<ScreenObject>(*this); }
    virtual void step(double dt);
    virtual void placement(double centre[2], double scale[2]) const;
    virtual void fill(DrawItem& item) const;
    // What the object draws: one item (fill) for most objects, several for composite ones.
    virtual void fill_all(std::vector<DrawItem>& out) const;
};

struct Graphics {
    explicit Graphics(VM& vm);

    VM& vm;
    // Lip sync: the mouth level (0 closed .. 2 open) of a speaker's playing voice line, or -1. Set by
    // whoever owns the audio engine; unset (headless, no sound) leaves the setMouth phases alone.
    std::function<int(int)> mouth_level;
    // The picture to show in place of the button glyph in slot `slot` (1 cross, 2 circle, 3 triangle, 4 square, 5 d-pad,
    // 6 L, 7 R, 8 the small Start/Select glyph), or null for the game's own. Set by the front end; asked every time the scene is built, so a change of
    // controller shows at once (docs/technical/input.md).
    // `centre_x` is the sprite's centre in script units: the same picture stands for Select on the left and Start on the right.
    std::function<ImagePtr(int, double)> glyph_picker;
    std::string exe;  // the executable's bytes
    std::string clt;  // GRAPH0.CLT
    struct Archive {
        std::string path;
        std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> index;
        bool read(uint32_t hash, std::string& out) const;
    };
    std::vector<Archive> archives;
    // PS Vita release (D-020): set when root/extracted/graph/GRAPH.CPK exists instead of the PS2's
    // GRAPH0.ARC. `exe` then holds the decrypted eboot.bin instead of SLPS_259.87; `archives` stays
    // empty. docs/technical/vita-cpk-format.md has the container and the texture-table trace.
    bool vita = false;
    std::string graph_cpk_path;
    CpkIndex graph_cpk;
    std::map<int64_t, std::unique_ptr<ScreenObject>> objects;
    int64_t next_uid = 1, created = 0;
    std::map<std::string, ImagePtr> images;
    // Big pictures (backgrounds, event pictures, composed characters: 1.1 to 1.5 MB each, one per eyes/mouth/blush
    // state for a character) are kept in a bounded cache, oldest out first; unbounded they filled the 32-bit window's
    // memory after a long session (bad allocation).
    std::deque<std::string> actor_keys;
    size_t cached_bytes = 0;
    void keep_actor(const std::string& key, ImagePtr image);
    std::map<std::string, std::vector<std::array<double, 7>>> layouts;

    struct Record {
        std::string name;
        bool has_name = false;
        int variant = 0;
        double rect[4] = {0, 0, 0, 0};
        int palette = 0;
    };
    Record record(int number) const;
    // Vita: the interface picture a numeric texture id and variant name, decoded out of
    // system/sysgraph.tm2 through the eboot's own tables (null if there is none).
    ImagePtr ui_image(int64_t texture, int64_t variant);
    int ui_entry(int64_t texture, int64_t variant) const;  // 0: none
    const uint8_t* ui_pixels(int entry, uint32_t& w, uint32_t& h, uint32_t& pitch);
    // Texture ids the port's own code names (PS2 engine constants) -> the Vita engine's ids for the
    // same pictures; script-supplied ids are the Vita's already (its system image is its own).
    int64_t engine_tex(int64_t ps2) const;
    uint32_t exe_word(uint32_t ps2_address) const;
    std::string sysgraph;  // system/sysgraph.tm2, decompressed on first use
    ImagePtr image_named(const std::string& name, int palette = 0);
    // Start decoding a picture on another thread so that it is ready when a script first uses it.
    // Only the machine's thread touches the caches; the tasks work on their own data.
    void preload(const std::string& name, int palette = 0);
    bool exists(const std::string& name) const;
    std::map<std::string, std::shared_future<ImagePtr>> pending;
    // What a script's `load` asks for, by the same naming the constructors use.
    bool preload_background(int64_t place, int64_t scene, int64_t mob);
    bool preload_event_picture(int64_t heroine, int64_t number, int64_t variant);
    bool preload_extra(int64_t size, int64_t picture, int64_t variant);
    bool preload_actor(int64_t character, int64_t cloth, int64_t pose);
    bool read_file(const std::string& name, std::string& out) const;

    int64_t add(std::unique_ptr<ScreenObject> obj);
    int64_t new_sprite(int64_t texture, int64_t variant, int64_t layer);
    int64_t new_light_ball(int64_t layer);
    int64_t new_clock(int64_t layer);
    int64_t new_navi(int64_t layer);
    void navi_mark(int64_t uid, int64_t node, int64_t state);
    void set_clock(int64_t uid, int64_t time, double frames);
    int64_t new_balloon(int64_t category, bool mark, int64_t layer);
    int64_t new_cut_in(int64_t layer);
    int64_t new_background(int64_t place, int64_t scene, int64_t mob, int64_t layer);
    int64_t new_event_picture(int64_t heroine, int64_t number, int64_t variant, int64_t layer);
    int64_t new_char(int font, int64_t code, int64_t palette, int64_t layer, double x, double y, double sx, double sy);
    int64_t new_snow(int64_t count, int64_t layer);
    int64_t new_actor(int64_t character, int64_t cloth, int64_t pose, int64_t layer, double ox, double oy);
    int64_t new_extra(int64_t size, int64_t picture, int64_t variant, int64_t layer);
    int64_t clone(int64_t uid);
    ScreenObject* get(int64_t uid);

    // text
    ImagePtr glyph(int font, int palette, int64_t code);
    struct Font {
        int w = 0, h = 0;
        std::string pixels;
        std::vector<std::array<uint8_t, 4>> clut;
        bool loaded = false;
    };
    Font fonts[2];

    // characters
    void actor_files(int64_t character, int64_t cloth, int64_t pose, std::string& body, std::string& face);
    const std::vector<std::array<double, 7>>& layout(const std::string& name);
    ImagePtr actor_canvas(const std::string& body, const std::string& face, int eye, int mouth, double ox, double oy,
                          int blush, int sweat, int drop);
    void actor_mouth(const std::string& face, double ox, double oy, double out[2]);

    // Smooth drawing for the window: bilinear texture sampling and soft-edged snow. Off for the
    // pixel-exact comparison with the reference model.
    bool smooth = false;

    void step(double dt);
    std::shared_ptr<const Scene> scene() const;  // snapshot of what is on screen now
    static std::vector<uint8_t> render_scene(const Scene& scene, bool smooth);
    // The scene after the view transform (script units to screen pixels): what render_scene draws, for another back end.
    static Scene view_scene(const Scene& scene);
    std::vector<uint8_t> render() const;
    void save_png(const std::string& path) const;
    // Any 24-bit RGB picture as a PNG (test aids: the GPU scene at its render scale).
    static void write_png(const std::string& path, const std::vector<uint8_t>& rgb, int w, int h);
    std::string describe() const;

    std::string exe_string(uint32_t address) const;
};

void install_graphics(VM& vm);

}  // namespace amg
