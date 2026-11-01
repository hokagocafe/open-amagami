// Screen objects and the software renderer. Mirrors tools/scfvm_gfx.py closely, including the
// order of floating point operations, so that frames can be compared pixel by pixel.
#include "gfx.hpp"
#include "datapath.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <stdexcept>

namespace amg {

namespace {

constexpr double PI = 3.14159265358979323846;

uint32_t le32(const std::string& d, size_t at) {
    if (at + 4 > d.size()) throw std::runtime_error("read past the end");
    return uint32_t(uint8_t(d[at])) | (uint32_t(uint8_t(d[at + 1])) << 8) | (uint32_t(uint8_t(d[at + 2])) << 16) |
           (uint32_t(uint8_t(d[at + 3])) << 24);
}
uint16_t le16(const std::string& d, size_t at) { return uint16_t(uint8_t(d.at(at)) | (uint8_t(d.at(at + 1)) << 8)); }
float lef(const std::string& d, size_t at) {
    uint32_t v = le32(d, at);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// "menu_main%1d", "txt_%03d", "node_flag%02d": one decimal field with optional zero padding.
std::string format_number(const std::string& pattern, int64_t value) {
    std::string out;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%') {
            out += pattern[i];
            continue;
        }
        size_t j = i + 1;
        bool zero = false;
        if (j < pattern.size() && pattern[j] == '0') {
            zero = true;
            ++j;
        }
        int width = 0;
        while (j < pattern.size() && pattern[j] >= '0' && pattern[j] <= '9') width = width * 10 + (pattern[j++] - '0');
        if (j < pattern.size() && pattern[j] == 'd') {
            std::string digits = std::to_string(value);
            while (int(digits.size()) < width) digits.insert(digits.begin(), zero ? '0' : ' ');
            out += digits;
            i = j;
        } else {
            out += pattern[i];
        }
    }
    return out;
}

// Every GRAPH.CPK picture (docs/technical/vita-cpk-format.md): a 256-colour palette (1,024 bytes, alpha 0-255),
// then one 8-bit index per pixel, row by row, no header; the width is fixed per directory and the height follows
// from the size. The .gxt files (bg/, epi/, body/) keep their palette as BGRA, the .tm2 ones (face/, eface/,
// font/, and system/sysgraph.tm2's entries) as RGBA.
ImagePtr decode_gxt(const std::string& data, int width, bool bgr) {
    if (data.size() < 1024 || width <= 0) return nullptr;
    const uint8_t* pal = reinterpret_cast<const uint8_t*>(data.data());
    int h = int((data.size() - 1024) / size_t(width));
    auto img = std::make_shared<Image>();
    img->w = width;
    img->h = h;
    img->px.resize(size_t(width) * size_t(h) * 4);
    // The palette expanded once to four bytes per entry (alpha in the 0..128 scale, 0x80 opaque), then one copy per
    // texel: these pictures are 1.5 million texels and are decoded in the middle of a scene.
    uint8_t lut[256][4];
    for (int i = 0; i < 256; ++i) {
        const uint8_t* c = pal + 4 * i;
        lut[i][0] = bgr ? c[2] : c[0];
        lut[i][1] = c[1];
        lut[i][2] = bgr ? c[0] : c[2];
        lut[i][3] = uint8_t((unsigned(c[3]) * 128 + 127) / 255);
    }
    const uint8_t* index = pal + 1024;
    uint8_t* out = img->px.data();
    for (size_t n = 0, count = size_t(width) * size_t(h); n < count; ++n) std::memcpy(out + n * 4, lut[index[n]], 4);
    return img;
}

// The file's on-disk extension, its width and its palette order, by directory.
bool vita_gxt_layout(const std::string& name, std::string& ext, int& width, bool& bgr) {
    if (name.compare(0, 3, "bg/") == 0 || name.compare(0, 4, "epi/") == 0) {
        ext = ".gxt";
        width = 960;  // 960x720
        bgr = true;
        return true;
    }
    if (name.compare(0, 5, "body/") == 0) {
        ext = ".gxt";
        width = 1024;  // 1024x1536
        bgr = true;
        return true;
    }
    if (name.compare(0, 5, "face/") == 0 || name.compare(0, 6, "eface/") == 0 || name.compare(0, 5, "font/") == 0) {
        ext = ".tm2";
        width = 1024;  // face/, eface/ 1024x1024; font/ 1024 wide, 4-bit (vita-cpk-format.md, confirmed)
        bgr = false;
        return true;
    }
    return false;
}

// --- PNG output: stored deflate blocks, so no compressor is needed -------------------------------

uint32_t crc_table_entry(uint32_t n) {
    uint32_t c = n;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    return c;
}

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) table[i] = crc_table_entry(i);
        ready = true;
    }
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put32be(std::string& s, uint32_t v) {
    for (int i = 3; i >= 0; --i) s.push_back(char((v >> (8 * i)) & 0xFF));
}

void chunk(std::string& out, const char* kind, const std::string& body) {
    put32be(out, uint32_t(body.size()));
    std::string tagged = std::string(kind, 4) + body;
    out += tagged;
    put32be(out, crc32(reinterpret_cast<const uint8_t*>(tagged.data()), tagged.size()));
}

std::string zlib_stored(const std::string& raw) {
    std::string out = "\x78\x01";
    size_t pos = 0;
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) {
        a = (a + c) % 65521u;
        b = (b + a) % 65521u;
    }
    do {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + n >= raw.size();
        out.push_back(last ? 1 : 0);
        out.push_back(char(n & 0xFF));
        out.push_back(char(n >> 8));
        out.push_back(char(~n & 0xFF));
        out.push_back(char((~n >> 8) & 0xFF));
        out.append(raw, pos, n);
        pos += n;
    } while (pos < raw.size());
    put32be(out, (b << 16) | a);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Animation
// ---------------------------------------------------------------------------------------------

void Tween::start(const std::vector<double>& values, int mode, double time) {
    target = values;
    if (time <= 0) {
        current = values;
        remaining = 0.0;
        return;
    }
    remaining = time;
    for (size_t i = 0; i < values.size(); ++i) {
        double d = values[i] - current.at(i);
        if (mode == 1) {
            a.at(i) = d / (time * time);
            b.at(i) = 0.0;
        } else if (mode == 2) {
            a.at(i) = -d / (time * time);
            b.at(i) = 2.0 * d / time;
        } else {
            a.at(i) = 0.0;
            b.at(i) = d / time;
        }
    }
}

void Tween::step(double dt) {
    if (remaining > 0) {
        remaining = std::max(remaining - dt, 0.0);
        double r = remaining;
        for (size_t i = 0; i < current.size(); ++i) current[i] = target.at(i) - (a.at(i) * r * r + b.at(i) * r);
    }
}

void Tween::finish() {
    if (remaining > 0) {
        remaining = 0.0;
        current = target;
    }
}

Vib::Vib(const std::vector<double>& initial, bool scale_, bool position_) : scale(scale_), position(position_) {
    parts[0] = Tween(initial);
    parts[1] = Tween(std::vector<double>(initial.size(), 0.0));
    parts[2] = Tween(std::vector<double>(initial.size(), 0.0));
    phase.assign(initial.size(), 0.0);
}

void Vib::step(double dt) {
    for (auto& p : parts) p.step(dt);
    const auto& freq = parts[2].current;
    for (size_t i = 0; i < phase.size(); ++i) {
        if (freq.at(i) > 0) {
            phase[i] += dt;
            if (phase[i] * freq[i] >= 60.0) phase[i] -= 60.0 / freq[i];
        } else {
            phase[i] = 0.0;
        }
    }
}

std::vector<double> Vib::value() const {
    const auto& base = parts[0].current;
    const auto& amp = parts[1].current;
    const auto& freq = parts[2].current;
    std::vector<double> out;
    if (position && base.size() == 2) {
        // FIDELITY: read 0x8108E152 and 0x8108E1D0 (ActorPicture's step), the same in the 18 object steps with a position
        // wave (vita-executable.md): both waves are added to Y, none to X.
        auto wave = [&](size_t i) { return std::sin(phase.at(i) * freq.at(i) * PI / 30.0) * amp.at(i); };
        return {base[0], base[1] + wave(0) + wave(1)};
    }
    for (size_t i = 0; i < base.size(); ++i) {
        double wave = std::sin(phase.at(i) * freq.at(i) * PI / 30.0) * amp.at(i);
        if (!scale) out.push_back(base[i] + wave);
        else if (wave >= 0) out.push_back(base[i] * (1.0 + wave));
        else out.push_back(base[i] / (1.0 - wave));
    }
    return out;
}

void Spin::start(Tween& t, double value, int mode, double time, double v0) {
    t.target = {value};
    if (time <= 0) {
        t.current = {value};
        t.remaining = 0.0;
        return;
    }
    t.remaining = time;
    double d = value - t.current.at(0);
    if (mode == 1) {
        t.a = {d / (time * time) - v0 / time};
        t.b = {0.0};
    } else if (mode == 2) {
        t.a = {-d / (time * time) + v0 / time};
        t.b = {2.0 * d / time};
    } else {
        t.a = {0.0};
        t.b = {d / time};
    }
}

void Spin::set(int component, double value, int mode, double time) {
    if (component == 1) {
        start(speed, value, mode, time, 0.0);
    } else if (component == 0) {
        start(base, value, mode, time, speed.current.at(0) / 30.0);
        speed = Tween(std::vector<double>{0.0});
        turned = 0.0;
    }
}

void Spin::step(double dt) {
    speed.step(dt);
    double s = speed.current.at(0);
    if (s > 0) {
        turned += dt * s / 30.0;
        if (turned >= 2.0) turned -= 2.0;
    } else if (s < 0) {
        turned += dt * s / 30.0;
        if (turned <= -2.0) turned += 2.0;
    }
    base.step(dt);
}

// ---------------------------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------------------------

void ScreenObject::step(double dt) {
    if (paused) return;
    alpha.step(dt);
    light.step(dt);
    pos.step(dt);
    zoom.step(dt);
    spin.step(dt);
    if (sway_phase >= 0) {
        sway_phase += dt;
        if (sway_phase >= 60.0) sway_phase -= 60.0;
    }
}

void ScreenObject::fill(DrawItem& item) const {
    item.layer = layer;
    item.order = order;
    item.image = image;
    for (int i = 0; i < 4; ++i) item.rect[i] = rect[i];
    item.alpha = alpha.value()[0];
    auto l = light.value();
    for (int i = 0; i < 3; ++i) item.light[i] = l[size_t(i)];
    placement(item.centre, item.scale);
    if (sway_phase >= 30.0) item.centre[1] -= 3.0;  // FIDELITY: read 0x81095000 (NodeIcon step)
    item.angle = spin.value();
    item.scale_on_screen = scale_on_screen;
    item.follow = follow;
    item.ui = ui;
}

void ScreenObject::fill_all(std::vector<DrawItem>& out) const {
    DrawItem item;
    fill(item);
    if (item.alpha > 0) out.push_back(std::move(item));  // nothing to draw otherwise
}

void ScreenObject::placement(double centre[2], double scale[2]) const {
    auto p = pos.value();
    auto z = zoom.value();
    centre[0] = p[0];
    centre[1] = p[1];
    scale[0] = z[0];
    scale[1] = z[1];
}

namespace {

// SnowEffect (K2R_CObjSnow with a K2R_CObjSnowFlake each), read from the Vita executable (docs/technical/vita-executable.md,
// "Snow manager"): flakes live in a 3D box in front of a camera and are projected to the screen. A flake is born at a random
// place (x within +-d1/200, y within [-d2/100, 0), z within [d3/100, (d3 + d4)/100)) with a random size and a speed down
// (+y) of size * the preset's size * about 1/12 a frame, a tenth of that sideways and in depth; it dies when y passes 0.
// While fewer flakes live than the target, a new one is born with chance 1/4 each frame; setAmount moves the target
// linearly. The camera looks along (-sin yaw cos pitch, -sin pitch, cos yaw cos pitch) from the moveCam position, and the
// projection is x * 512 / depth on both axes (then * 0.75 into the scripts' units).
// FIDELITY: read 0x81095BF8 (the manager's step), 0x8109572C (a flake's step), 0x810818D8 (birth), 0x8100CDAE (creation),
// 0x81016CA0 and 0x81016EB6 (the camera and projection matrices), 0x81016482 (the transform).
// Each flake is a sprite at the projected place, in the world view (the flake's object points at the scale and origin that
// setScrScl and setScrOrgY write, 0x810DE4D8 and 0x810DFFAC), turned by its random angle. Its picture is interface texture
// 245, variant 0 (a 16-pixel white dot), read in the birth code (0x810818D8 takes it from the texture-id table
// 0x810DDF94 at index 0x3D4 / 4). The blending is the usual one (not read). The random generator is the port's own, so
// the places differ from a console run, not their distribution.
struct SnowObject : ScreenObject {
    struct Flake {
        double p[3], v[3], size, angle;
    };
    std::vector<Flake> flakes;
    struct Shown {
        double x, y, scale, alpha, angle;  // the sprite's centre in the scripts' units, its scale, alpha and turn (units of pi)
    };
    std::vector<Shown> view;  // the flakes as they are drawn, rebuilt every step
    ImagePtr dot;
    int64_t d[4] = {10000, 8000, 1000, 8000};
    double preset_size = 0.18;
    double target = 0, ramp_from = 0, ramp_to = 0, ramp_left = 0, ramp_total = 0;  // setAmount
    struct Tween {
        double v = 0, from = 0, to = 0, left = 0, total = 0;
        void set(double value, double time) {
            from = v;
            to = value;
            left = total = time;
            if (time <= 0) v = value;
        }
        void step(double dt) {
            if (left <= 0) return;
            left = std::max(0.0, left - dt);
            v = left > 0 ? to - (to - from) * left / total : to;
        }
    };
    Tween yaw, pitch, cam[3];
    uint32_t rng = 7;

    SnowObject(int64_t id, int64_t lay, int64_t count, const int64_t dist[4], double size, ImagePtr picture) {
        uid = id;
        dot = std::move(picture);
        layer = lay;
        rect[2] = rect[3] = 1;
        for (int i = 0; i < 4; ++i) d[i] = dist[i];
        preset_size = size;
        target = ramp_to = double(count);
        while (int64_t(flakes.size()) < count) birth();  // the creator fills the target at once
    }
    std::unique_ptr<ScreenObject> clone() const override { return std::make_unique<SnowObject>(*this); }
    int64_t rand_() {  // 0 to 2^31 - 1, as the C library's would give
        rng = rng * 1103515245u + 12345u;
        return int64_t((rng >> 1) & 0x7FFFFFFF);
    }
    int64_t mod(int64_t n) { return n > 0 ? rand_() % n : 0; }
    // FIDELITY: read 0x81096106 (docs/technical/d1-snow.md): the first fill spreads the flakes over the whole box, a flake
    // born later to refill the target starts at the top of it (y = -d[1] / 100), so it falls the whole height.
    void birth(bool at_top = false) {
        Flake f;
        f.p[0] = double(mod(d[0]) - d[0] / 2) / 100.0;
        f.p[1] = at_top ? double(-d[1]) / 100.0 : double(mod(d[1]) - d[1]) / 100.0;
        f.p[2] = (double(d[2]) + double(mod(d[3]))) / 100.0;
        f.angle = double(mod(2000)) / 1000.0;
        f.size = double(mod(50) + mod(50) + 100) * 0.12;
        const double vy = f.size * preset_size * double(mod(30) - mod(30) + 100) / 1200.0;
        f.v[1] = vy;
        f.v[0] = vy * double(mod(100) - mod(100)) / 1000.0;
        f.v[2] = vy * double(mod(100) - mod(100)) / 1000.0;
        flakes.push_back(f);
    }
    void set_amount(double count, double time) {
        ramp_from = target;
        ramp_to = std::max(0.0, count);
        ramp_left = ramp_total = time;
        if (time <= 0) target = ramp_to;
    }
    void turn(double a, double b, double time) {  // turnCam(yaw, pitch, mode, time), in degrees
        yaw.set(a, time);
        pitch.set(b, time);
    }
    void move(double x, double y, double z, double time) {
        cam[0].set(x, time);
        cam[1].set(y, time);
        cam[2].set(z, time);
    }
    void step(double dt) override {
        ScreenObject::step(dt);
        if (ramp_left > 0) {
            ramp_left = std::max(0.0, ramp_left - dt);
            target = ramp_left > 0 ? double(int64_t(ramp_to - (ramp_to - ramp_from) * ramp_left / ramp_total)) : ramp_to;
        }
        yaw.step(dt);
        pitch.step(dt);
        for (auto& c : cam) c.step(dt);
        if (double(flakes.size()) < target && mod(4) == 0) birth(true);

        // The camera: turned by pitch about x and yaw about y (the angles go in negated), as the engine builds its matrix.
        const double rad = 3.14159265358979 / 180.0;
        const double ca = std::cos(-pitch.v * rad), sa = std::sin(-pitch.v * rad), cb = std::cos(-yaw.v * rad), sb = std::sin(-yaw.v * rad);
        const double fwd[3] = {-sb * ca, -sa, ca * cb}, up[3] = {-sb * sa, ca, cb * sa};
        auto cross = [](const double* a, const double* b, double* o) {
            o[0] = a[1] * b[2] - a[2] * b[1];
            o[1] = a[2] * b[0] - a[0] * b[2];
            o[2] = a[0] * b[1] - a[1] * b[0];
        };
        auto norm = [](double* a) {
            const double n = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            if (n > 0)
                for (int i = 0; i < 3; ++i) a[i] /= n;
        };
        double ax[3], ay[3], az[3] = {fwd[0], fwd[1], fwd[2]};
        cross(up, fwd, ax);
        norm(ax);
        norm(az);
        cross(az, ax, ay);

        view.clear();
        for (size_t i = 0; i < flakes.size();) {
            Flake& f = flakes[i];
            for (int k = 0; k < 3; ++k) f.p[k] += f.v[k] * dt;
            if (f.p[1] > 0) {  // reached the ground
                flakes[i] = flakes.back();
                flakes.pop_back();
                continue;
            }
            ++i;
            const double rel[3] = {f.p[0] - cam[0].v, f.p[1] - cam[1].v, f.p[2] - cam[2].v};
            const double depth = az[0] * rel[0] + az[1] * rel[1] + az[2] * rel[2];
            if (depth <= 0) continue;
            const double sx = 512.0 * (ax[0] * rel[0] + ax[1] * rel[1] + ax[2] * rel[2]) / depth;
            const double sy = 512.0 * (ay[0] * rel[0] + ay[1] * rel[1] + ay[2] * rel[2]) / depth;
            const double zs = 65533.0000152 / depth + 1.0 / 65535.0;  // the depth as the engine's clip space keeps it
            if (std::fabs(sx) > 400 || std::fabs(sy) > 300 || zs > 24000) continue;
            const double scale = std::max(f.size / depth, 0.1875);
            double a = zs > 20000 ? (24000 - zs) / 1000 : (zs < 3000 ? zs / 3000 : 1.0);
            if (f.p[1] > -2) a *= -0.5 * f.p[1];  // fades out in the last two units above the ground
            a = std::min(a, 1.0);
            if (a <= 0) continue;
            view.push_back({sx * 0.75, sy * 0.75, scale, a, f.angle});
        }
    }
    void fill_all(std::vector<DrawItem>& out) const override {
        DrawItem base;
        fill(base);
        if (base.alpha <= 0 || !dot) return;
        for (const Shown& f : view) {
            DrawItem item = base;
            item.image = dot;
            item.rect[0] = item.rect[1] = 0;
            item.rect[2] = dot->w;
            item.rect[3] = dot->h;
            item.centre[0] = f.x;
            item.centre[1] = f.y;
            item.scale[0] = item.scale[1] = f.scale;
            item.alpha = base.alpha * f.alpha;
            item.angle = f.angle;
            item.follow = 1.0;
            item.snow = true;
            out.push_back(std::move(item));
        }
    }
};

// K2R_CObjLightBall (read from 0x190168, 0x19C550): three sprites of `kaiwa/tama0..2` (a white core and two
// glows, opaque grey pictures drawn by adding light) on one centre. Each frame the middle glow moves a sixth of the
// way to the ball's position, the outer glow a sixth of the way to the middle one, so a moving ball leaves a short
// trail; the middle glow is tinted (light + 1) / 2, the outer one by the light itself, the core stays white.
struct LightBallObject : ScreenObject {
    ImagePtr parts[3];
    double trail[2][2] = {{0, 0}, {0, 0}};
    bool started = false;

    std::unique_ptr<ScreenObject> clone() const override { return std::make_unique<LightBallObject>(*this); }
    void step(double dt) override {
        ScreenObject::step(dt);
        std::vector<double> p = pos.value();
        if (!started) {
            for (auto& t : trail) t[0] = p[0], t[1] = p[1];
            started = true;
            return;
        }
        for (int n = std::max(1, int(dt + 0.5)); n > 0; --n)
            for (int i = 0; i < 2; ++i) {
                trail[0][i] = (p[size_t(i)] * 5.0 + trail[0][i]) / 6.0;
                trail[1][i] = (trail[0][i] * 5.0 + trail[1][i]) / 6.0;
            }
    }
    void fill_all(std::vector<DrawItem>& out) const override {
        DrawItem base;
        fill(base);
        if (base.alpha <= 0) return;
        std::vector<double> p = pos.value();
        for (int k = 0; k < 3; ++k) {
            if (!parts[k]) continue;
            DrawItem item = base;
            item.image = parts[k];
            item.rect[0] = item.rect[1] = 0;
            item.rect[2] = parts[k]->w;
            item.rect[3] = parts[k]->h;
            item.additive = true;
            if (k > 0) {
                item.centre[0] += (started ? trail[k - 1][0] : p[0]) - p[0];
                item.centre[1] += (started ? trail[k - 1][1] : p[1]) - p[1];
            }
            for (int c = 0; c < 3; ++c)
                item.light[c] = k == 0 ? 1.0 : (k == 1 ? (base.light[c] + 1.0) * 0.5 : base.light[c]);
            out.push_back(std::move(item));
        }
    }
};

// K2R_CObjClock as a script object of its own (ClockIcon, the history screen; constructor 0x1908C8, the same one the
// map's node icons use for their clock): the back face, the front face (its opacity follows the hour: dawn and dusk),
// the long hand and the short hand, all on the object's centre. Drawn back face first, hands on top.
struct ClockObject : ScreenObject {
    ScreenObject parts[4];  // back face, front face, long hand, short hand
    double front_alpha = 1.0;
    // The faces follow the time of day as it turns (Vita: spin 0x81091418 and step 0x810917E4, +0x20C value, +0x210
    // target, +0x214 frames left, +0x218 rate): the value moves linearly to the target and every frame picks the faces
    // and the front opacity again. Set by Graphics::set_clock on Vita data; the PS2's step is not read (faces at once).
    bool follow = false;
    double value = 0, target = 0, left = 0, rate = 0;
    double alpha_table[24] = {};
    int back_table[24] = {}, front_table[24] = {};
    ImagePtr face_image[3];
    std::string face_name[3];

    void pick_faces() {
        double hour = -value * 0.5;
        int whole = int(hour);
        double frac = hour - whole;
        int h0 = ((whole % 24) + 24) % 24, h1 = (h0 + 1) % 24;
        front_alpha = alpha_table[h0] * (1.0 - frac) + alpha_table[h1] * frac;
        for (int k = 0; k < 2; ++k) {
            int variant = (k == 0 ? back_table : front_table)[h0];
            if (variant < 0 || variant > 2 || !face_image[variant]) continue;
            parts[k].image = face_image[variant];
            parts[k].name = face_name[variant];
        }
    }

    std::unique_ptr<ScreenObject> clone() const override { return std::make_unique<ClockObject>(*this); }
    void step(double dt) override {
        ScreenObject::step(dt);
        parts[2].spin.step(dt);
        parts[3].spin.step(dt);
        if (follow && left != 0) {
            left = std::max(0.0, left - dt);
            value = target - rate * left;
            pick_faces();
        }
    }
    void fill_all(std::vector<DrawItem>& out) const override {
        DrawItem base;
        fill(base);
        if (base.alpha <= 0) return;
        for (int k = 0; k < 4; ++k) {
            if (!parts[k].image) continue;
            DrawItem item;
            parts[k].fill(item);
            item.layer = base.layer;
            item.order = base.order;
            item.centre[0] = base.centre[0];
            item.centre[1] = base.centre[1];
            item.scale[0] = base.scale[0];
            item.scale[1] = base.scale[1];
            item.alpha = base.alpha * (k == 1 ? front_alpha : 1.0);
            for (int c = 0; c < 3; ++c) item.light[c] = base.light[c];
            out.push_back(std::move(item));
        }
    }
};

// K2R_CObjMapNavi (NodeMapNaviBase; setMark 0x1A29E8, draw 0x1A26A8): the event graph's minimap, `navi/navi_0`
// (record 201, 136x144, palette 4) with a 2x2 dot per node written into the picture's pixels. Node n is column n % 65,
// row n / 65; its dot is at x = 2 * column + 3, y = 5 + 2 * row, one pixel higher in odd columns (the hexagonal grid).
// The dot takes palette index 247 for a negative state, otherwise the table at 0x28AE58 (state 0..7: 252, 253, 251,
// 254, 249, 248, 250, 255). The engine double-buffers the texture; here the marks go to a work copy that is published
// once per frame, so the renderer never sees a picture being written.
struct NaviObject : ScreenObject {
    std::shared_ptr<Image> work;
    std::array<std::array<uint8_t, 4>, 256> colours{};
    bool dirty = false;
    // Dot size and where node 0's dot is. The Vita's map (260x260, setMark 0x81095138) writes 4x4 dots, x = 4 * column
    // and y = 4 * row - 4, two pixels higher in odd columns, from a base the picture's own grid puts at (2, 10)
    // (hypothesis: the base pointer comes from the texture object, not traced).
    int cell = 2, x0 = 3, y0 = 5, odd_up = 1;

    std::unique_ptr<ScreenObject> clone() const override {
        auto copy = std::make_unique<NaviObject>(*this);
        if (work) copy->work = std::make_shared<Image>(*work);
        return copy;
    }
    void mark(int64_t node, int64_t state) {
        static const uint8_t table[8] = {252, 253, 251, 254, 249, 248, 250, 255};  // 0x28AE58
        if (!work || node < 0) return;
        int col = int(node % 65), row = int(node / 65);
        int x = cell * col + x0, y = y0 + cell * row - odd_up * (col & 1);
        uint8_t index = state < 0 ? 247 : table[state & 7];
        for (int dy = 0; dy < cell; ++dy)
            for (int dx = 0; dx < cell; ++dx) {
                int px = x + dx, py = y + dy;
                if (px < 0 || py < 0 || px >= work->w || py >= work->h) continue;
                std::memcpy(&work->px[(size_t(py) * size_t(work->w) + size_t(px)) * 4], colours[index].data(), 4);
            }
        dirty = true;
    }
    void step(double dt) override {
        ScreenObject::step(dt);
        if (dirty && work) {
            image = std::make_shared<Image>(*work);
            dirty = false;
        }
    }
};

// The topic bubble of the conversation game (Balloon). On the Vita it is two sprites of interface texture 215, the rim lit by the
// object's light and a white body, with the `?` (variant 2) when the topic is hidden; the code is in `fill`. (The PS2 drew six
// parts of `maru/maru128` in two sizes, objects.md; the Vita pictures hold that look, so nothing is modelled of it.)
struct BalloonObject : ScreenObject {
    ImagePtr disc, mark, inner;
    double colour[3] = {1, 1, 1};
    bool show_mark = false;

    std::unique_ptr<ScreenObject> clone() const override { return std::make_unique<BalloonObject>(*this); }
    void fill_all(std::vector<DrawItem>& out) const override {
        DrawItem base;
        fill(base);
        if (base.alpha <= 0 || !disc) return;
        {
            // FIDELITY: read 0x8100B20E (Balloon's constructor; the part class K2R_CObjBalParts, vtable 0x810C1818) and
            // 0x8108FE64 (its draw), 0x8108FF00 (its step). Two sprites of interface texture 215 at the object's own
            // transform, the same size: variant 0 lit by the object's light (the rim's colour), then variant 1, or 2 (the
            // `?`) with a mark, unlit. The rim is in the pictures. The wobble is the script's (WadaiTarget's vibPos).
            static const double white_light[3] = {1, 1, 1};
            // The script colours the bubble with the object's own light (WadaiTarget sets it per topic): the rim takes it, the
            // white body does not.
            auto layer = [&](const ImagePtr& img, double scale, const double rgb[3], bool lit) {
                DrawItem item = base;
                item.image = img;
                item.rect[0] = item.rect[1] = 0;
                item.rect[2] = img->w;
                item.rect[3] = img->h;
                item.scale[0] *= scale;
                item.scale[1] *= scale;
                for (int c = 0; c < 3; ++c) item.light[c] = rgb[c] * (lit ? base.light[c] : 1.0);
                out.push_back(std::move(item));
            };
            layer(disc, 1.0, colour, true);
            layer(show_mark && mark ? mark : inner ? inner : disc, 1.0, white_light, false);
            return;
        }
    }
};

struct ActorObject : ScreenObject {
    static constexpr int CANVAS_W = 512, CANVAS_H = 768;
    static constexpr double PIVOT[2] = {0.0, -30.0};
    static constexpr double ACTOR_SHIFT[2] = {0.0, 92.0};
    static constexpr double EXTRA_SHIFT[2] = {0.0, 128.0};
    static constexpr int FRAMES[4] = {0, 1, 2, 1};
    static constexpr double SWEAT_FALL = 32.0;

    Graphics* gfx = nullptr;
    std::string body, face;
    bool has_body = false, has_face = false;
    double offset[2] = {0, 0};
    double shift[2] = {ACTOR_SHIFT[0], ACTOR_SHIFT[1]};
    bool extra = false;  // an extra picture (set_extra), not a character
    int eye_face = 0, mouth_face = 0;
    int eye_phase = 0, mouth_phase = 2;
    int eye_steps = 0, mouth_steps = 0;
    double eye_time = 1.0, mouth_time = 1.0, eye_timer = 0.0, mouth_timer = 0.0;
    bool lip_sync = true;
    int64_t character = -1;  // also the speaker of its voice lines (Actor's cnum == anum)
    int lip_level = -1;      // last mouth level from the audio engine, -1 when not speaking
    int mouth_frame = 2;     // Vita: the mouth frame last shown (+0x22), the lip-synced one included
    int64_t draw_target = 0; // setDrawTarget: the uid of the cut-in window that draws this character, 0 on the screen
    double blush = 0, blush_target = 0, blush_left = 0, blush_accel = 0;
    double sweat = 0, sweat_target = 0, sweat_left = 0, sweat_accel = 0, sweat_drop = 0, sweat_drop_accel = 0;

    ActorObject(Graphics* g, int64_t id, int64_t lay) : gfx(g) {
        uid = id;
        layer = lay;
        // FIDELITY: read 0x8108D382: on the Vita a character points at the world view's origin itself (follow 1). The
        // PS2 value is the old fit (F4-53), kept for the PS2 and for extra pictures (set_extra), not traced on Vita.
        follow = 1.0;
        rect[2] = CANVAS_W * 2;  // the Vita's pictures have twice the resolution
        rect[3] = CANVAS_H * 2;
    }
    std::unique_ptr<ScreenObject> clone() const override { return std::make_unique<ActorObject>(*this); }

    void refresh() {
        int eye = 3 * eye_face + FRAMES[eye_phase];
        int mouth = 3 * mouth_face + (lip_level >= 0 ? 2 - lip_level : FRAMES[mouth_phase]);
        auto quantise = [](double v) { return std::max(0, std::min(16, int(v * 16 + 0.5))); };
        image = gfx->actor_canvas(has_body ? body : std::string(), has_face ? face : std::string(), eye, mouth,
                                  0.0, 0.0, quantise(blush), quantise(sweat),
                                  int(std::nearbyint(sweat_drop)));
    }
    void set_picture(int64_t character, int64_t cloth, int64_t pose, double ox, double oy) {
        gfx->actor_files(character, cloth, pose, body, face);
        this->character = character;
        has_body = !body.empty();
        has_face = !face.empty();
        offset[0] = ox;
        offset[1] = oy;
        name = "actor " + (has_body ? body : std::string("None"));
        refresh();
    }
    void set_extra(int64_t size, int64_t picture, int64_t variant) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "body/x%s_%c%c", size ? "s" : "l", char(0x41 + picture), char(0x61 + variant));
        body = buf;
        has_body = true;
        face.clear();
        has_face = false;
        name = "extra " + body;
        offset[0] = offset[1] = 0.0;
        shift[0] = shift[1] = 0.0;
        extra = true;
        follow = 1.0;  // FIDELITY: read 0x81081D2A: the extra's body points at the world view, unconditionally
        refresh();
    }
    void position(double out[2]) const {
        auto p = pos.value();
        out[0] = p[0] + shift[0];
        out[1] = p[1] + shift[1];
    }
    void placement(double centre[2], double scale[2]) const override {
        double p[2];
        position(p);
        auto z = zoom.value();
        centre[0] = PIVOT[0] + (p[0] - PIVOT[0]) * z[0];
        centre[1] = PIVOT[1] + (p[1] - PIVOT[1]) * z[1];
        scale[0] = z[0];
        scale[1] = z[1];
        if (extra) {
            // FIDELITY: read 0x8108BB0C (ExtraPicture's step): X = P.x * 0.5, Y = P.y * 0.5 - 8 (base 0x810AD928) plus
            // ofsX/ofsY, scale S = Z * (1.5, 1.6071428) (= 1.5 * 480/448), about (0, -22.5) (0x810AD930), as the
            // characters. In the port's canvas that is (0.75, 45/56) of the zoom; the album zooms its thumbnail sheet
            // by exactly the inverse (4/3, 56/45).
            scale[0] *= 0.75;
            scale[1] *= 45.0 / 56.0;
            auto P = pos.value();
            centre[0] = (P[0] * 0.5 + offset[0]) * 1.5 * z[0];
            centre[1] = -22.5 + (P[1] * 0.5 - 8.0 + offset[1] + 22.5) * (1.5 * 480.0 / 448.0) * z[1];
        } else {
            // FIDELITY: read 0x8108E1DA..0x8108E530 (ActorPicture's step, screen path) and 0x810101F0 (the body's draw).
            // The Vita runs the PS2's Actor scripts unchanged and maps their position P and zoom Z itself:
            // X = P.x * 0.75 / 1.25, Y = P.y * 0.75 * 1.05 / 1.25 + 48 (0x810AD940), scale S = Z * (1.215, 1.215 * 1.05),
            // zoomed about (0, -22.5) (0x810AD938). The 1024x1536 picture is drawn at 3.2 texels per unit
            // (0x810AD948, 1/6.4 for the half size); the port's canvas is read at 2, hence 2 / 3.2. The PS2's
            // ACTOR_SHIFT and PIVOT do not apply. The script's ofsX/ofsY (ivars 5 and 6, which setPic fills from its
            // tables) are the floats at +0x14/+0x18 that the step adds to X and Y here, in units, before the scale; on
            // the Vita only one pose has one (-32), and the canvas does not take it.
            auto P = pos.value();
            double X = P[0] * 0.75 / 1.25 + offset[0], Y = P[1] * 0.75 * 1.05 / 1.25 + 48.0 + offset[1];
            double sx = 1.215 * z[0], sy = 1.215 * 1.05 * z[1];
            centre[0] = X * sx;
            centre[1] = -22.5 + (Y + 22.5) * sy;
            scale[0] = sx * 2.0 / 3.2;
            scale[1] = sy * 2.0 / 3.2;
        }
    }
    // FIDELITY: read 0x8108DC98..0x8108DD60 and 0x8108E5E0 (ActorPicture's step with a cut-in target, vita-executable.md
    // "Cut-in geometry"); measured against Vita3K 2026-10-29 (n0fre___18A, the mahjong's first cut-ins). The window W
    // (units) and its zoom Tz replace the world view: S = (0.9072, 0.95256) * Z * Tz, X0 = 0.6 * P.x + ofsX,
    // Y0 = 0.63 * P.y + 48 + ofsY + 3, centre = W + X0 * S, with no -22.5 pivot.
    void cut_in_placement(const double W[2], const double Tz[2], double centre[2], double scale[2]) const {
        auto P = pos.value();
        auto z = zoom.value();
        double sx = 0.9072 * z[0] * Tz[0], sy = 0.95256 * z[1] * Tz[1];
        centre[0] = W[0] + (P[0] * 0.6 + offset[0]) * sx;
        centre[1] = W[1] + (P[1] * 0.63 + 48.0 + offset[1] + 3.0) * sy;
        scale[0] = sx * 2.0 / 3.2;
        scale[1] = sy * 2.0 / 3.2;
    }
    static void steps_for(int mode, int phase, bool is_eye, int pending, int& out_phase, int& out_steps) {
        auto py_mod = [](int a, int m) { return ((a % m) + m) % m; };
        if (mode == 0) {
            if (phase == 1) phase = 3;
            out_phase = phase;
            out_steps = py_mod(4 - phase, 4);
        } else if (mode == 1) {
            out_phase = phase;
            out_steps = (phase & 1) ^ 1;
        } else if (mode == 2) {
            if (phase == 3) phase = 1;
            out_phase = phase;
            out_steps = py_mod(2 - phase, 4);
        } else if (is_eye) {  // blink
            if (pending) {
                out_phase = phase;
                out_steps = pending;
            } else {
                if (phase == 3) phase = 1;
                out_phase = phase;
                out_steps = (2 - phase) * 2;
            }
        } else {
            out_phase = phase;
            out_steps = 4;
        }
    }
    void set_eye(int mode, double time) {
        steps_for(mode, eye_phase, true, eye_steps, eye_phase, eye_steps);
        if (time <= 0) {
            eye_phase = (eye_phase + eye_steps) % 4;
            eye_steps = 0;
            refresh();
        } else {
            eye_timer = 0.0;
            eye_time = time;
        }
    }
    void set_mouth(int mode, double time) {
        steps_for(mode, mouth_phase, false, mouth_steps, mouth_phase, mouth_steps);
        if (time <= 0) {
            mouth_phase = (mouth_phase + mouth_steps) % 4;
            mouth_steps = 0;
            refresh();
        } else {
            mouth_timer = 0.0;
            mouth_time = time;
        }
    }
    void set_face(int64_t eyes, int64_t mouth) {
        if (eyes) eye_face = int(eyes - 1);
        if (mouth) mouth_face = int(mouth - 1);
        refresh();
    }
    void set_blush(double target, double time) {
        blush_target = target;
        if (time <= 0) {
            blush = target;
            blush_left = 0.0;
        } else {
            blush_left = time;
            blush_accel = (target - blush) / (time * time);
        }
    }
    void set_sweat(bool on, double time) {
        double target = on ? 1.0 : 0.0;
        sweat_target = target;
        if (time <= 0) {
            sweat = target;
            sweat_left = 0.0;
            sweat_drop = 0.0;
            return;
        }
        // FIDELITY: read 0x8108CEDC (d1-face-animation.md, two readings): turning the sweat on with a time restarts the
        // fade from 0, and turning it off with a time takes the drop away at once.
        if (on) sweat = 0.0;
        sweat_left = time;
        sweat_accel = (target - sweat) / (time * time);
        if (on) {
            sweat_drop = SWEAT_FALL;
            sweat_drop_accel = SWEAT_FALL / (time * time);
        } else {
            sweat_drop = 0.0;
            sweat_drop_accel = 0.0;
        }
    }
    void step(double dt) override {
        if (paused) return;
        ScreenObject::step(dt);
        bool changed = false;
        if (blush_left > 0) {
            blush_left = std::max(blush_left - dt, 0.0);
            blush = blush_target - blush_accel * std::pow(blush_left, 2.0);
            changed = true;
        }
        if (sweat_left > 0) {
            sweat_left = std::max(sweat_left - dt, 0.0);
            sweat = sweat_target - sweat_accel * std::pow(sweat_left, 2.0);
            if (sweat_drop_accel != 0.0) sweat_drop = sweat_drop_accel * std::pow(sweat_left, 2.0);
            changed = true;
        }
        if (eye_steps && alpha.value()[0] >= 1.0) {
            if (eye_timer < eye_time) {
                eye_timer += dt;
            } else {
                eye_timer -= eye_time;
                eye_phase = (eye_phase + 1) & 3;
                --eye_steps;
                changed = true;
            }
        }
        int level = lip_sync && gfx->mouth_level && character >= 0 ? std::min(gfx->mouth_level(int(character)), 2) : -1;  // a level over 2 would index the frames out of range (review 2026-10-30)
        {
            // FIDELITY: read 0x8108E6xx..0x8108E7A2 (ActorPicture's step, the mouth): while the voice drives the mouth,
            // pending setMouth steps are taken at once and the frame shown is 2 - level; once it stops, if the phase's
            // frame is not the last frame shown, the phase becomes that frame and setMouth(the phase's old frame,
            // int(time)) animates back to what the script had asked for.
            if (level >= 0) {
                mouth_phase = (mouth_phase + mouth_steps) & 3;
                mouth_steps = 0;
                mouth_timer = 0.0;
                mouth_frame = 2 - level;
            } else if (!mouth_steps) {
                if (FRAMES[mouth_phase] != mouth_frame) {
                    int back = FRAMES[mouth_phase];
                    mouth_phase = mouth_frame;
                    set_mouth(back, double(unsigned(std::min(std::max(mouth_time, 0.0), 65535.0)) & 0xFFFF));
                    changed = true;
                }
            } else if (mouth_timer < mouth_time) {
                mouth_timer += dt;
            } else {
                mouth_timer -= mouth_time;
                mouth_phase = (mouth_phase + 1) & 3;
                mouth_frame = FRAMES[mouth_phase];
                --mouth_steps;
                changed = true;
            }
        }
        if (level != lip_level) {
            lip_level = level;
            changed = true;
        }
        if (changed) refresh();
    }
    void mouth_position(double out[2]) const {
        double mouth[2];
        gfx->actor_mouth(has_face ? face : std::string(), offset[0], offset[1], mouth);
        double p[2];
        position(p);
        auto z = zoom.value();
        for (int i = 0; i < 2; ++i) out[i] = (p[i] + mouth[i] - PIVOT[i]) * z[i] + PIVOT[i];
    }
};

// K2R_CObjEventPicEx (factory 0x109820, constructor 0x18EE68, setMouth 0x19A790): an event picture whose mouth
// moves. The engine makes one when `stand/e<heroine>_<number><variant>` has a sheet (`x.tm2`) and a layout (`z.zb`):
// three records of group 3, the closed, half and open mouth, each a patch at its place on the 640x448 picture.
// setMouth and lip sync work as on a character (phases 0..3 shown as frames 0, 1, 2, 1; the voice level picks
// 2 - level while the speaker talks).
struct EventPicExObject : ScreenObject {
    Graphics* gfx = nullptr;
    ImagePtr sheet;
    std::vector<std::array<double, 7>> frames;
    int mouth_phase = 2, mouth_steps = 0;
    double mouth_timer = 0.0, mouth_time = 0.0;
    bool lip_sync = true;
    int64_t character = -1;
    int lip_level = -1;
    int mouth_frame = 2;  // Vita: the mouth frame last shown (+0x278), the lip-synced one included

    std::unique_ptr<ScreenObject> clone() const override { return std::make_unique<EventPicExObject>(*this); }
    void set_mouth(int mode, double time) {
        ActorObject::steps_for(mode, mouth_phase, false, mouth_steps, mouth_phase, mouth_steps);
        if (time <= 0) {
            mouth_phase = (mouth_phase + mouth_steps) % 4;
            mouth_steps = 0;
        } else {
            mouth_timer = 0.0;
            mouth_time = time;
        }
    }
    void step(double dt) override {
        if (paused) return;
        ScreenObject::step(dt);
        // FIDELITY: read 0x8108A65E..0x8108A768 (EventPicture's step, the mouth; d1-leftovers.md 2.3, two readings):
        // the same path as ActorObject's mouth. While the voice drives it, pending steps are folded at once and the
        // frame shown is 2 - level; afterwards the mouth animates from the last frame shown back to the phase's frame.
        int level = lip_sync && gfx && gfx->mouth_level && character >= 0 ? std::min(gfx->mouth_level(int(character)), 2) : -1;  // a level over 2 would index the frames out of range (review 2026-10-30)
        if (level >= 0) {
            mouth_phase = (mouth_phase + mouth_steps) & 3;
            mouth_steps = 0;
            mouth_timer = 0.0;
            mouth_frame = 2 - level;
        } else if (!mouth_steps) {
            if (ActorObject::FRAMES[mouth_phase] != mouth_frame) {
                int back = ActorObject::FRAMES[mouth_phase];
                mouth_phase = mouth_frame;
                set_mouth(back, double(unsigned(std::min(std::max(mouth_time, 0.0), 65535.0)) & 0xFFFF));
            }
        } else if (mouth_timer < mouth_time) {
            mouth_timer += dt;
        } else {
            mouth_timer -= mouth_time;
            mouth_phase = (mouth_phase + 1) & 3;
            mouth_frame = ActorObject::FRAMES[mouth_phase];
            --mouth_steps;
        }
        lip_level = level;
    }
    void fill_all(std::vector<DrawItem>& out) const override {
        DrawItem base;
        fill(base);
        if (base.alpha <= 0) return;
        out.push_back(base);
        if (!sheet || frames.size() < 3) return;
        const auto& r = frames[size_t(lip_level >= 0 ? 2 - lip_level : ActorObject::FRAMES[mouth_phase & 3])];
        DrawItem patch = base;
        patch.image = sheet;
        patch.rect[0] = r[2];
        patch.rect[1] = r[3];
        patch.rect[2] = r[4];
        patch.rect[3] = r[5];
        patch.centre[0] += (r[0] + r[4] * 0.5 - base.rect[2] * 0.5) * base.scale[0];
        patch.centre[1] += (r[1] + r[5] * 0.5 - base.rect[3] * 0.5) * base.scale[1];
        out.push_back(std::move(patch));
    }
};

// K2R_CObjCutInWindow (Vita factory 0x8100AE44, vtable 0x810C1720; PS2 factory 0x1090C0, constructor 0x18C2E8): the
// window of a mahjong cut-in. The engine builds three children: the frame `mahjong/cut_frame` (texture 321) and two layers
// of the background pattern `maru/cut_tex%1d` (texture 322); setTex picks the first layer's variant and
// setColor(r1, g1, b1, r2, g2, b2) colours the two layers. FIDELITY: read 0x8108F194 and 0x8100AE44: the window is 268
// pixels at scale 1 (the constant 268.0): the frame is drawn 1:1 and the 128-pixel pattern is stretched to 268 (the PS2's
// 224 and 200 were its own sizes). A character whose setDrawTarget names the window is drawn inside it (0x8108D300: the
// view of scale 1.0 and the target's origin instead of the world view): ActorObject::cut_in_placement, read and
// checked against Vita3K 2026-10-29 (fidelity.md, item 2). The two pattern layers are two stretched sprites (0x8108FA40):
// the base (texture 322 variant 0, colour 2) and over it the variant setTex chose (colour 1); the order is a hypothesis
// that agrees with the Vita3K capture. The clip is the frame's whole quad (0x8108FDD4, a stencil); that it keeps the
// inside is a hypothesis (Vita3K draws the body outside the window, probably its own stencil fault).
struct CutInWindowObject : ScreenObject {
    Graphics* gfx = nullptr;
    ImagePtr frame, base_pattern;
    double c1[3] = {1, 1, 1}, c2[3] = {1, 1, 1};
    static constexpr double SIZE = 268.0;   // FIDELITY: read 0x8108F194 (the frame's size and the window's at scale 1)

    std::unique_ptr<ScreenObject> clone() const override { return std::make_unique<CutInWindowObject>(*this); }
    void fill_all(std::vector<DrawItem>& out) const override {
        DrawItem base;
        fill(base);
        if (base.alpha <= 0) return;
        // The clip is in units around the screen centre (the renderer scales it by VIEW_UNIT); SIZE is in pixels.
        double half = SIZE * 0.5 / VIEW_UNIT;
        double clip[4] = {SCREEN_W / 2.0 + base.centre[0] - half * base.scale[0],
                          SCREEN_H / 2.0 + base.centre[1] - half * base.scale[1],
                          SCREEN_W / 2.0 + base.centre[0] + half * base.scale[0],
                          SCREEN_H / 2.0 + base.centre[1] + half * base.scale[1]};
        auto pattern = [&](const ImagePtr& img, const double* colour) {
            if (!img) return;
            DrawItem item = base;
            item.image = img;
            item.rect[0] = item.rect[1] = 0;
            item.rect[2] = img->w;
            item.rect[3] = img->h;
            item.scale[0] *= SIZE / img->w;
            item.scale[1] *= SIZE / img->h;
            for (int c = 0; c < 3; ++c) item.light[c] = colour[c];
            out.push_back(std::move(item));
        };
        pattern(base_pattern, c2);
        pattern(image, c1);
        if (gfx)
            for (const auto& [id, obj] : gfx->objects) {
                auto* actor = dynamic_cast<const ActorObject*>(obj.get());
                if (!actor || actor->draw_target != uid) continue;
                std::vector<DrawItem> drawn;
                actor->fill_all(drawn);
                // The parts follow the body with the same scale in both views, so the screen drawing is moved and
                // scaled as a whole from the screen placement to the window's.
                double sc[2], ss[2], cc[2], cs[2];
                actor->placement(cs, ss);
                actor->cut_in_placement(base.centre, base.scale, cc, sc);
                for (auto& item : drawn) {
                    for (int i = 0; i < 2; ++i) {
                        double k = ss[i] != 0.0 ? sc[i] / ss[i] : 0.0;
                        item.centre[i] = cc[i] + (item.centre[i] - cs[i]) * k;
                        item.scale[i] *= k;
                    }
                    item.follow = base.follow;
                    item.ui = base.ui;
                    item.alpha *= base.alpha;
                    item.layer = layer;
                    item.order = order;
                    item.clipped = true;
                    std::copy(clip, clip + 4, item.clip);
                    out.push_back(std::move(item));
                }
            }
        if (frame) {
            DrawItem item = base;
            item.image = frame;
            item.rect[0] = item.rect[1] = 0;
            item.rect[2] = frame->w;
            item.rect[3] = frame->h;
            item.scale[0] *= SIZE / frame->w;
            item.scale[1] *= SIZE / frame->h;
            out.push_back(std::move(item));
        }
    }
};
constexpr double CutInWindowObject::SIZE;

constexpr double ActorObject::PIVOT[2];
constexpr double ActorObject::ACTOR_SHIFT[2];
constexpr double ActorObject::EXTRA_SHIFT[2];
constexpr int ActorObject::FRAMES[4];

}  // namespace

// ---------------------------------------------------------------------------------------------
// Graphics
// ---------------------------------------------------------------------------------------------

Graphics::Graphics(VM& v) : vm(v) {
    graph_cpk_path = amg::data_file(vm.root, "graph/GRAPH.CPK");
    read_screen_settings();
    graph_cpk = cpk_read_index(graph_cpk_path);
    exe = amg::vita_exe(vm.root);
    // The two font sheets are read and converted the first time a character is drawn: a 25 to 30 ms stall in the middle
    // of the first scene with text. Done here, while the game is still loading.
    try {
        glyph(0, 0, 0x8140);
        glyph(1, 0, 0x8140);
    } catch (const std::exception&) {
    }
}

std::string Graphics::exe_string(uint32_t address) const {
    size_t start = amg::vita_va_to_off(exe, address, "string");
    size_t end = exe.find('\0', start);
    return exe.substr(start, end - start);
}

// Vita interface pictures (docs/technical/vita-cpk-format.md, F4-43). The eboot keeps, per texture
// id (the PS2's record number), a list of pointers per variant to an 8-byte record whose first word
// is an entry of sysgraph.tm2; a size table gives each entry's width, height and row pitch. An entry
// is a 256-colour RGBA palette (alpha 0-255) and then 8-bit indices, row by row.
int Graphics::ui_entry(int64_t texture, int64_t variant) const {
    constexpr uint32_t TEXTURES = 0x810ddf94, TEXTURE_COUNT = 337;
    constexpr uint32_t RECORDS = 0x810ac158, RECORD_COUNT = 762;
    if (texture < 0 || texture >= TEXTURE_COUNT) return 0;
    auto word = [this](uint32_t va) { return le32(exe, amg::vita_va_to_off(exe, va, "texture table")); };
    uint32_t variants = word(TEXTURES + 4 * uint32_t(texture));
    auto record_at = [&](int64_t v) -> uint32_t {
        if (v < 0 || v > 255) return 0;
        uint32_t r = word(variants + 4 * uint32_t(v));
        bool ok = r >= RECORDS && r < RECORDS + 8 * RECORD_COUNT && (r - RECORDS) % 8 == 0;
        return ok ? r : 0;
    };
    uint32_t rec = record_at(variant);
    if (!rec) rec = record_at(0);
    if (!rec) return 0;
    uint32_t entry = word(rec);
    return entry < RECORD_COUNT ? int(entry) : 0;  // entry 0 is empty
}

const uint8_t* Graphics::ui_pixels(int entry, uint32_t& w, uint32_t& h, uint32_t& pitch) {
    constexpr uint32_t SIZES = 0x810adf44;  // per entry: flag, width, height, pitch
    if (entry <= 0) return nullptr;
    if (sysgraph.empty() && !read_file("system/sysgraph.tm2", sysgraph)) return nullptr;
    auto word = [this](uint32_t va) { return le32(exe, amg::vita_va_to_off(exe, va, "texture sizes")); };
    uint32_t start = le32(sysgraph, 4 * size_t(entry));
    w = word(SIZES + 16 * uint32_t(entry) + 4);
    h = word(SIZES + 16 * uint32_t(entry) + 8);
    pitch = word(SIZES + 16 * uint32_t(entry) + 12);
    if (w == 0 || h == 0 || w > 4096 || h > 4096 || pitch < w || uint64_t(start) + 1024 + uint64_t(pitch) * h > sysgraph.size()) return nullptr;  // 64-bit sums: garbage sizes must not wrap on x86
    return reinterpret_cast<const uint8_t*>(sysgraph.data()) + start;  // palette, then the indices
}

// Read from the Vita engine's own code (docs/technical/vita-cpk-format.md, F4-43): the node icons
// (kind + 0x4F/0x56, marks 0x5E/0x5F: PS2 - 3), the flags, clock and node extras (0xA4 + low, 0xA8,
// clock 0xA9..0xAB, 0xAB + extra: PS2 - 6), the light ball (202), the map (194). The cut-in's frame
// (321) and texture (322) are read from the factory 0x8100AE44 (movw #0x141, #0x142).
int64_t Graphics::engine_tex(int64_t ps2) const {
    if (ps2 >= 0x52 && ps2 <= 0x62) return ps2 - 3;
    if (ps2 >= 0xAA && ps2 <= 0xB4) return ps2 - 6;
    switch (ps2) {
        case 201: return 194;
        case 209: return 202;
        case 319: return 322;
        case 320: return 321;
        default: return ps2;
    }
}

// A word of one of the PS2 executable's constant tables the port reads; on Vita, the same table in
// eboot.bin (found byte for byte, or for the node tables with the Vita's own texture ids).
uint32_t Graphics::exe_word(uint32_t addr) const {
    static const struct { uint32_t ps2, vita, size; } tables[] = {
        {0x28B398, 0x810ADEA4, 56},     // clock: value per time of day
        {0x28A8D8, 0x810AD964, 96},     // clock: front face opacity per hour
        {0x28A938, 0x810AD9C4, 96},     // clock: back face per hour
        {0x28A998, 0x810ADA24, 96},     // clock: front face per hour
        {0x28A9F8, 0x810ADA84, 0x1A0},  // node icons, grid
        {0x28AB98, 0x810ADC24, 0x1A0},  // node icons, hexagons
    };
    for (const auto& t : tables)
        if (addr >= t.ps2 && addr + 4 <= t.ps2 + t.size)
            return le32(exe, amg::vita_va_to_off(exe, t.vita + (addr - t.ps2), "engine table"));
    return 0;
}

// A port-made picture (F5-11): the brush stroke of one of the main menu's entries (interface texture 42) with its
// lettering painted out, so that the port's own entry can carry its label in the game's font on the game's stroke. Asked
// for as variant 1000 + the entry's own variant. The lettering is white with a dark outline; inside the stroke, pixels
// like those are replaced row by row by the stroke's colour, interpolated between the nearest pixels left and right.
ImagePtr Graphics::blank_stroke(int64_t texture, int64_t variant) {
    const std::string key = "blank#" + std::to_string(texture) + ":" + std::to_string(variant);
    auto it = images.find(key);
    if (it != images.end()) return it->second;
    ImagePtr base = ui_image(texture, variant);
    if (!base) return nullptr;
    auto img = std::make_shared<Image>(*base);
    const int w = img->w, h = img->h;
    auto px = [&](int x, int y) { return &img->px[(size_t(y) * size_t(w) + size_t(x)) * 4]; };
    auto solid = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && px(x, y)[3] >= 120; };
    // The white of the letters, inside the stroke; then their outline: darker pixels at most 3 pixels from that white (the
    // stroke's own dark rim is further from any letter and stays).
    std::vector<uint8_t> white(size_t(w) * size_t(h), 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!solid(x, y) || !solid(x - 6, y) || !solid(x + 6, y) || !solid(x, y - 3) || !solid(x, y + 3)) continue;
            const uint8_t* p = px(x, y);
            if (std::min({p[0], p[1], p[2]}) > 150) white[size_t(y) * size_t(w) + size_t(x)] = 1;
        }
    std::vector<uint8_t> grown(white);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!white[size_t(y) * size_t(w) + size_t(x)]) continue;
            for (int dy = -3; dy <= 3; ++dy)
                for (int dx = -3; dx <= 3; ++dx) {
                    const int u = x + dx, v = y + dy;
                    if (!solid(u, v)) continue;
                    const uint8_t* p = px(u, v);
                    if ((p[0] * 3 + p[1] * 6 + p[2]) / 10 < 110 || (std::abs(dx) <= 1 && std::abs(dy) <= 1))
                        grown[size_t(v) * size_t(w) + size_t(u)] = 1;
                }
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!grown[size_t(y) * size_t(w) + size_t(x)]) continue;
            int l = x, r = x;
            while (l >= 0 && grown[size_t(y) * size_t(w) + size_t(l)]) --l;
            while (r < w && grown[size_t(y) * size_t(w) + size_t(r)]) ++r;
            const bool has_l = l >= 0 && solid(l, y), has_r = r < w && solid(r, y);
            if (!has_l && !has_r) continue;
            const uint8_t* a = has_l ? px(l, y) : px(r, y);
            const uint8_t* b = has_r ? px(r, y) : px(l, y);
            const double t = has_l && has_r ? double(x - l) / double(r - l) : 0.0;
            uint8_t* d = px(x, y);
            for (int c = 0; c < 3; ++c) d[c] = uint8_t(a[c] + (b[c] - a[c]) * t + 0.5);
        }
    images[key] = img;
    return img;
}

ImagePtr Graphics::ui_image(int64_t texture, int64_t variant) {
    if (variant >= 1000) return blank_stroke(texture, variant - 1000);
    // The button glyphs of the interface (textures 15 to 19, 23 and 24, variant 0) come out marked with a slot.
    int slot = 0;
    {
        // texture, variant, slot
        static const int GLYPH_TEXTURES[][3] = {{15, 0, 1}, {16, 0, 2}, {17, 0, 3}, {18, 0, 4}, {19, 0, 5}, {23, 0, 6}, {24, 0, 7},
                                                {61, 10, 8}};
        for (const auto& g : GLYPH_TEXTURES)
            if (texture == g[0] && variant == g[1]) slot = g[2];
    }
    std::string glyph_key = "glyph#" + std::to_string(slot);
    if (slot) {
        auto cached = images.find(glyph_key);
        if (cached != images.end()) return cached->second;
    }
    int entry = ui_entry(texture, variant);
    if (entry <= 0) return nullptr;
    std::string key = "sysgraph#" + std::to_string(entry);
    auto it = images.find(key);
    if (it != images.end()) return it->second;
    uint32_t w = 0, h = 0, pitch = 0;
    const uint8_t* pal = ui_pixels(entry, w, h, pitch);
    if (!pal) return nullptr;
    const uint8_t* idx = pal + 1024;
    auto img = std::make_shared<Image>();
    img->w = int(w);
    img->h = int(h);
    img->px.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* c = pal + 4 * idx[size_t(y) * pitch + x];
            uint8_t* dst = &img->px[(size_t(y) * w + x) * 4];
            dst[0] = c[0];
            dst[1] = c[1];
            dst[2] = c[2];
            dst[3] = uint8_t((unsigned(c[3]) * 128 + 127) / 255);  // PS2 alpha scale, as decode_gxt
        }
    images[key] = img;
    if (const char* dump = std::getenv("AMG_DUMP_UI")) {  // a debugging aid: every interface picture, as PNG, when first made
        std::string raw;
        for (uint32_t y = 0; y < h; ++y) {
            raw.push_back(char(0));
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t* q = &img->px[(size_t(y) * w + x) * 4];
                raw.append({char(q[0]), char(q[1]), char(q[2]), char(std::min(255, q[3] * 255 / 128))});
            }
        }
        const unsigned char signature[8] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
        std::string out(reinterpret_cast<const char*>(signature), 8), ihdr;
        const char depth_and_type[5] = {8, 6, 0, 0, 0};
        put32be(ihdr, int(w));
        put32be(ihdr, int(h));
        ihdr.append(depth_and_type, 5);
        chunk(out, "IHDR", ihdr);
        chunk(out, "IDAT", zlib_stored(raw));
        chunk(out, "IEND", "");
        std::ofstream f(std::string(dump) + "/ui_" + std::to_string(texture) + "_" + std::to_string(variant) + ".png", std::ios::binary);
        f.write(out.data(), std::streamsize(out.size()));
    }
    if (slot) {
        auto marked = std::make_shared<Image>(*img);
        marked->glyph = slot;
        images[glyph_key] = marked;
        return marked;
    }
    return img;
}

bool Graphics::read_file(const std::string& name, std::string& out) const {
    return cpk_read(graph_cpk_path, graph_cpk, name, out);
}

static ImagePtr load_picture(const Graphics& g, const std::string& name, int palette) {
    (void)palette;
    std::string ext;
    int width;
    bool has_alpha;
    if (!vita_gxt_layout(name, ext, width, has_alpha)) return nullptr;  // system/: not identified (F4-39)
    std::string data;
    if (!g.read_file(name + ext, data)) return nullptr;
    return decode_gxt(data, width, has_alpha);
}

ImagePtr Graphics::image_named(const std::string& name, int palette) {
    std::string key = name + "#" + std::to_string(palette);
    auto it = images.find(key);
    if (it != images.end()) return it->second;
    ImagePtr img;
    const auto began = std::chrono::steady_clock::now();
    auto waiting = pending.find(key);
    const bool was_preloaded = waiting != pending.end();
    if (was_preloaded) {
        img = waiting->second.get();
        pending.erase(waiting);
    } else {
        img = load_picture(*this, name, palette);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    if (!was_preloaded && ms > 2.0 && img && (name.rfind("bg/", 0) == 0 || name.rfind("epi/", 0) == 0) &&
        std::find(missed.begin(), missed.end(), name) == missed.end()) {
        missed.push_back(name);
        if (on_missed) on_missed(missed);
    }
    static const bool log_loads = std::getenv("AMG_PROFILE_PRIMS") != nullptr;  // which pictures the scripts wait for
    if (log_loads && ms > 1.0)
        std::fprintf(stderr, "SLOWPICTURE %s %.1f ms (%s)\n", name.c_str(), ms, was_preloaded ? "preload not finished" : "not preloaded");
    keep_actor(key, img);
    return img;
}

bool Graphics::exists(const std::string& name) const {
    std::string ext;
    int width;
    bool has_alpha;
    if (!vita_gxt_layout(name, ext, width, has_alpha)) return false;
    return graph_cpk.count(name + ext) != 0;
}

Graphics::~Graphics() { warm_stop = true; }

void Graphics::warm(const std::vector<std::string>& names) {
    struct Job {
        std::string name;
        std::promise<ImagePtr> done;
    };
    auto jobs = std::make_shared<std::vector<Job>>();
    for (const auto& name : names) {
        const std::string key = name + "#0";
        if (!exists(name) || images.count(key) || pending.count(key)) continue;
        Job job{name, {}};
        pending[key] = job.done.get_future().share();
        jobs->push_back(std::move(job));
    }
    if (jobs->empty()) return;
    warm_task = std::async(std::launch::async, [this, jobs] {
        for (auto& job : *jobs) job.done.set_value(warm_stop ? nullptr : load_picture(*this, job.name, 0));
    });
}

void Graphics::preload(const std::string& name, int palette) {
    std::string key = name + "#" + std::to_string(palette);
    static const bool disabled = std::getenv("AMG_NO_PRELOAD") != nullptr;  // for timing experiments
    if (disabled || images.count(key) || pending.count(key) || !exists(name)) return;
    pending[key] = std::async(std::launch::async, [this, name, palette] { return load_picture(*this, name, palette); }).share();
}

bool Graphics::preload_background(int64_t place, int64_t scene, int64_t mob) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "bg/%03lld_%lld%lld", (long long)place, (long long)scene, (long long)mob);
    preload(buf);
    return exists(buf);
}

bool Graphics::preload_event_picture(int64_t heroine, int64_t number, int64_t variant) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "epi/e%llx_%02lld%c0", (unsigned long long)heroine, (long long)number,
                  char(0x61 + variant));
    preload(buf);
    return exists(buf);
}

bool Graphics::preload_extra(int64_t size, int64_t picture, int64_t variant) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "body/x%s_%c%cx", size ? "s" : "l", char(0x41 + picture), char(0x61 + variant));
    // The picture is shown through actor_canvas, which pastes it into a canvas: 8 MB of work that is done here too,
    // off the game's thread, so showing the picture finds it ready.
    const std::string stem(buf, std::strlen(buf) - 1);
    if (!exists(buf)) return false;
    if (!pending_whole.count(stem) && !images.count(buf + std::string("#0"))) {
        static const bool disabled = std::getenv("AMG_NO_PRELOAD") != nullptr;
        if (!disabled)
            pending_whole[stem] = std::async(std::launch::async, [this, name = std::string(buf)] {
                                      return compose_whole(load_picture(*this, name, 0));
                                  }).share();
    }
    return true;
}

bool Graphics::preload_actor(int64_t character, int64_t cloth, int64_t pose) {
    std::string body, face;
    actor_files(character, cloth, pose, body, face);
    if (!body.empty()) preload(body + "x");
    if (!face.empty()) preload(face + "x");
    return !body.empty() && exists(body + "x");
}

int64_t Graphics::new_light_ball(int64_t layer) {
    auto obj = std::make_unique<LightBallObject>();
    obj->layer = layer;
    for (int k = 0; k < 3; ++k) obj->parts[k] = ui_image(engine_tex(209), k);
    obj->rect[2] = obj->rect[3] = 48;
    return add(std::move(obj));
}

int64_t Graphics::new_navi(int64_t layer) {
    auto obj = std::make_unique<NaviObject>();
    obj->layer = layer;
    {
        uint32_t w = 0, h = 0, pitch = 0;
        const uint8_t* pal = ui_pixels(ui_entry(engine_tex(201), 0), w, h, pitch);
        if (pal) {
            for (int i = 0; i < 256; ++i) {
                const uint8_t* c = pal + 4 * i;
                obj->colours[size_t(i)] = {c[0], c[1], c[2], uint8_t((unsigned(c[3]) * 128 + 127) / 255)};
            }
            auto img = std::make_shared<Image>();
            img->w = int(w);
            img->h = int(h);
            img->px.resize(size_t(w) * h * 4);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                    std::memcpy(&img->px[(size_t(y) * w + x) * 4], obj->colours[pal[1024 + size_t(y) * pitch + x]].data(), 4);
            obj->work = img;
            obj->image = std::make_shared<Image>(*img);
            obj->rect[2] = w;
            obj->rect[3] = h;
            obj->cell = 4;
            obj->x0 = 2;
            obj->y0 = 6;
            obj->odd_up = 2;
        }
        return add(std::move(obj));
    }
}

void Graphics::navi_mark(int64_t uid, int64_t node, int64_t state) {
    if (auto* navi = dynamic_cast<NaviObject*>(get(uid))) navi->mark(node, state);
}

int64_t Graphics::new_clock(int64_t layer) {
    auto obj = std::make_unique<ClockObject>();
    obj->layer = layer;
    const int64_t tex[4] = {engine_tex(0xAF), engine_tex(0xAF), engine_tex(0xB0), engine_tex(0xB1)};  // clock%1d (175), long hand (176), short hand (177)
    for (int k = 0; k < 4; ++k) {
        int64_t tmp = new_sprite(tex[k], 0, layer);
        if (ScreenObject* t = get(tmp)) obj->parts[k] = *t;
        objects.erase(tmp);
    }
    obj->rect[2] = obj->parts[0].rect[2];
    obj->rect[3] = obj->parts[0].rect[3];
    return add(std::move(obj));
}

// The clock at a time of day (the table at 0x28B398, half hours, negative), as K2R_CObjClock::spin (0x19D440) sets it:
// long hand at the value, short hand at value / 12; the hour -value / 2 picks the faces (back 0x28A938, front 0x28A998)
// and the front face's opacity (0x28A8D8, interpolated).
void Graphics::set_clock(int64_t uid, int64_t time, double frames) {
    auto* clock = dynamic_cast<ClockObject*>(get(uid));
    if (!clock || time < 0 || time >= 14) return;
    auto word = [this](uint32_t addr, bool real) -> double {
        uint32_t w = exe_word(addr);
        if (real) {
            float f;
            std::memcpy(&f, &w, 4);
            return f;
        }
        return int32_t(w);
    };
    double v = word(0x28B398 + 4 * uint32_t(time), true);
    clock->parts[2].spin.set(0, v, 0, frames);
    clock->parts[3].spin.set(0, v / 12.0, 0, frames);
    {  // the Vita's tables (0x810AD964, 0x810AD9C4, 0x810ADA24) are the PS2's, checked word for word
        if (!clock->follow) {
            for (int h = 0; h < 24; ++h) {
                clock->alpha_table[h] = word(0x28A8D8 + 4 * uint32_t(h), true);
                clock->back_table[h] = int(word(0x28A938 + 4 * uint32_t(h), false));
                clock->front_table[h] = int(word(0x28A998 + 4 * uint32_t(h), false));
            }
            for (int k = 0; k < 3; ++k) {
                int64_t tmp = new_sprite(engine_tex(0xAF), k, clock->layer);
                if (ScreenObject* t = get(tmp)) {
                    clock->face_image[k] = t->image;
                    clock->face_name[k] = t->name;
                }
                objects.erase(tmp);
            }
            clock->follow = true;
        }
        clock->target = v;
        if (frames > 0) {
            clock->rate = (v - clock->value) / frames;
            clock->left = frames;
        } else {
            clock->value = v;
            clock->left = 0;
        }
        clock->pick_faces();
    }
}

int64_t Graphics::new_balloon(int64_t category, bool mark, int64_t layer) {
    static const double colours[12][3] = {
        {1.0, 1.0, 1.0}, {0.3059, 0.8157, 0.8980}, {0.2039, 0.0627, 1.0}, {0.5059, 0.8824, 0.3176},
        {0.9569, 0.9059, 0.2471}, {0.8980, 0.5098, 0.3059}, {0.8510, 0.2039, 1.0}, {1.0, 0.3294, 0.7569},
        {0.8980, 0.3137, 0.3059}, {0.4431, 0.4431, 0.7451}, {0.6, 0.6, 0.6}, {0.0, 0.0, 0.0}};  // 0x28A850
    auto obj = std::make_unique<BalloonObject>();
    obj->layer = layer;
    int c = int(std::max<int64_t>(0, std::min<int64_t>(11, category)));
    for (int i = 0; i < 3; ++i) obj->colour[i] = colours[c][i];
    obj->show_mark = mark;
    obj->disc = ui_image(215, 0);  // the balloon: texture 215, variants 0, 1, 2 (code 0x8100B20E)
    obj->mark = ui_image(215, 2);
    obj->inner = ui_image(215, 1);
    obj->rect[2] = 128;
    obj->rect[3] = 192;
    return add(std::move(obj));
}

int64_t Graphics::new_cut_in(int64_t layer) {
    auto obj = std::make_unique<CutInWindowObject>();
    obj->gfx = this;
    obj->layer = layer;
    obj->frame = ui_image(engine_tex(320), 0);
    obj->image = ui_image(engine_tex(319), 0);
    obj->base_pattern = ui_image(engine_tex(319), 0);  // the second layer: variant 0, never changed (0x8100AE44)
    obj->rect[2] = obj->rect[3] = CutInWindowObject::SIZE;
    return add(std::move(obj));
}

int64_t Graphics::add(std::unique_ptr<ScreenObject> obj) {
    int64_t uid = next_uid++;
    obj->uid = uid;
    ++created;
    obj->order = created;
    objects[uid] = std::move(obj);
    return uid;
}

int64_t Graphics::new_sprite(int64_t texture, int64_t variant, int64_t layer) {
    auto obj = std::make_unique<ScreenObject>();
    obj->layer = layer;
    obj->ui = true;  // FIDELITY: read 0x81008124 and 0x8100880E, see view_of
    if (texture == 0) {
        obj->rect[2] = SCREEN_W / VIEW_UNIT;
        obj->rect[3] = SCREEN_H / VIEW_UNIT;
        return add(std::move(obj));
    }
    obj->image = ui_image(texture, variant);
    obj->rect[2] = obj->image ? obj->image->w : 0;
    obj->rect[3] = obj->image ? obj->image->h : 0;
    obj->name = std::to_string(texture) + ":" + std::to_string(variant);
    return add(std::move(obj));
}

int64_t Graphics::new_background(int64_t place, int64_t scene, int64_t mob, int64_t layer) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "bg/%03lld_%lld%lld", (long long)place, (long long)scene, (long long)mob);
    auto obj = std::make_unique<ScreenObject>();
    obj->layer = layer;
    obj->image = image_named(buf);
    obj->rect[2] = obj->image ? obj->image->w : SCREEN_W;
    obj->rect[3] = obj->image ? obj->image->h : SCREEN_H;
    obj->name = buf;
    obj->scale_on_screen = true;
    obj->follow = 1.0;
    if (place >= 0 && place < 400) {
        // FIDELITY: read 0x81009262 (BG_Picture's creator): a byte per place at 0x810A8660 puts that background in the
        // interface view (origin 0) instead of the world's: the map's frame (200), the menus' pictures (139 to 149,
        // 207 to 216...). Event pictures are always in the world view (0x81009964). Not done yet: the interface view's
        // scale is 1 on the Vita, while the port scales every picture by the screen scale (only differs at 0.775).
        size_t at = amg::vita_va_to_off(exe, 0x810A8660 + uint32_t(place), "place table");
        if (at < exe.size() && exe[at] != 0) obj->follow = 0.0;
    }
    return add(std::move(obj));
}

int64_t Graphics::new_event_picture(int64_t heroine, int64_t number, int64_t variant, int64_t layer) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "epi/e%llx_%02lld%c0", (unsigned long long)heroine, (long long)number,
                  char(0x61 + variant));
    char ex[64];
    std::snprintf(ex, sizeof ex, "stand/e%llx_%02lld%c", (unsigned long long)heroine, (long long)number, char(0x61 + variant));
    std::unique_ptr<ScreenObject> obj;
    if (exists(std::string(ex) + "x")) {
        auto pic = std::make_unique<EventPicExObject>();
        pic->gfx = this;
        pic->sheet = image_named(std::string(ex) + "x");
        for (const auto& r : layout(ex))
            if (r[6] == 3) pic->frames.push_back(r);
        obj = std::move(pic);
    } else {
        obj = std::make_unique<ScreenObject>();
    }
    obj->layer = layer;
    obj->image = image_named(buf);
    obj->rect[2] = obj->image ? obj->image->w : SCREEN_W;
    obj->rect[3] = obj->image ? obj->image->h : SCREEN_H;
    obj->name = buf;
    obj->scale_on_screen = true;
    obj->follow = 1.0;
    return add(std::move(obj));
}

static int glyph_index(int64_t code) {
    int hi = int(code >> 8), lo = int(code & 0xFF);
    int index = 0;
    if (hi >= 0x81 && hi < 0xA0) index = (hi - 0x81) * 188;
    if (hi >= 0xE0) index = (hi - 0xE0) * 188 + 31 * 188;
    if (lo >= 0x40 && lo < 0x7F) index += lo - 0x40;
    if (lo >= 0x80 && lo < 0xFE) index += lo - 0x41;
    return index;
}

ImagePtr Graphics::glyph(int font, int palette, int64_t code) {
    std::string key = "glyph#" + std::to_string(font) + "#" + std::to_string(((palette % 4) + 4) % 4) + "#" +
                      std::to_string(code);
    auto it = images.find(key);
    if (it != images.end()) return it->second;
    Font& f = fonts[font];
    {
        // font/AMV_font18_21(im).tm2: four 16-colour palettes (white, blue, pink, dark grey, as the PS2's) in the
        // first 1,024 bytes, then 4-bit pixels, 1,024 wide; glyph n is the 36x42 cell n % 28, n / 28 (the eboot's
        // glyph lookup 0x8101389E: the same Shift-JIS index as the PS2, 28 per row, cell height 42).
        static const char* vita_names[2] = {"font/AMV_font18_21", "font/AMV_font18_21im"};
        constexpr int GW = 36, GH = 42, PER_ROW = 28, ROW_BYTES = 512;
        if (!f.loaded) {
            std::string data;
            if (!read_file(std::string(vita_names[font & 1]) + ".tm2", data) || data.size() < 1024)
                throw std::runtime_error("font missing");
            for (int i = 0; i < 64; ++i) {
                const uint8_t* c = reinterpret_cast<const uint8_t*>(data.data()) + 4 * i;
                f.clut.push_back({c[0], c[1], c[2], uint8_t((unsigned(c[3]) * 128 + 127) / 255)});
            }
            f.pixels = data.substr(1024);
            f.w = 1024;
            f.h = int(f.pixels.size() / ROW_BYTES);
            f.loaded = true;
        }
        ImagePtr img;
        int index = glyph_index(code);
        int x0 = (index % PER_ROW) * GW, y0 = (index / PER_ROW) * GH;
        if (y0 + GH <= f.h) {
            int pal = ((palette % 4) + 4) % 4;
            auto im = std::make_shared<Image>();
            im->w = GW;
            im->h = GH;
            im->px.resize(size_t(GW * GH * 4));
            for (int y = 0; y < GH; ++y)
                for (int x = 0; x < GW; ++x) {
                    uint8_t byte = uint8_t(f.pixels[size_t(y0 + y) * ROW_BYTES + size_t(x0 + x) / 2]);
                    int idx = ((x0 + x) & 1) ? (byte >> 4) : (byte & 0x0F);
                    std::memcpy(&im->px[size_t(y * GW + x) * 4], f.clut[size_t(pal * 16 + idx)].data(), 4);
                }
            img = im;
        }
        images[key] = img;
        return img;
    }
}

namespace {
// K2R_CObjSprFont's draw (0x19AAF0, confirmed by reading it): a character of palette 0 to 2 is drawn a second time
// 2.0 to the right and down in palette 3 (dark grey), and one of palette 3 in palette 0; that copy is the dark edge
// the text has on the console. The engine registers it after the character, and later sprites of one object are
// drawn first (objects.md), so it lies under the glyph.
// FIDELITY: read 0x81087DA4 (the Vita's draw, K2R_CObjSprFont slot 12): the copy is 1.0 to the right and down, black
// (0x810C0E98) under a character of palette 0 to 2 and white (0x810C0EA4) under one of palette 3, and not lit by the
// object's light.
// FIDELITY: read 0x8100865A and 0x8100F074 (docs/technical/d1-text-window.md): a character of palette 3 takes the font's
// first (white) palette, multiplied by 0.2 when drawn (0x810C0EB0); the font's own fourth palette is not used.
// The tint and the first palette are for palette % 4 == 3 (palettes 7 and 11 as well: d1-survey.md, two readings).
static int vita_font_palette(int64_t palette) {
    int p = int(((palette % 4) + 4) % 4);
    return p < 3 ? p : 0;
}

struct CharObject : ScreenObject {
    ImagePtr shadow;
    bool black_shadow = false;
    std::unique_ptr<ScreenObject> clone() const override { return std::make_unique<CharObject>(*this); }
    void fill_all(std::vector<DrawItem>& out) const override {
        DrawItem base;
        fill(base);
        if (base.alpha <= 0) return;
        if (shadow) {
            DrawItem item = base;
            item.image = shadow;
            item.centre[0] += 1.0;
            item.centre[1] += 1.0;
            for (double& c : item.light) c = black_shadow ? 0.0 : 1.0;
            out.push_back(std::move(item));
        }
        if (((palette % 4) + 4) % 4 == 3)  // both 0x81008742 and 0x81087E4A test palette % 4 >= 3 (d1-survey.md)
            for (double& c : base.light) c *= 0.2;
#ifdef AMG_PERSONAL
        if (name.size() > 5) base.char_code = int32_t(std::strtol(name.c_str() + 5, nullptr, 16));  // "char XXXX"
#endif
        out.push_back(std::move(base));
    }
};
}  // namespace

int64_t Graphics::new_char(int font, int64_t code, int64_t palette, int64_t layer, double x, double y, double sx,
                           double sy) {
    auto obj = std::make_unique<CharObject>();
    obj->layer = layer;
    obj->image = glyph(font, vita_font_palette(palette), code);
    if (palette >= 0 && palette < 4) obj->shadow = glyph(font, 0, code);
    obj->black_shadow = palette < 3;
    obj->rect[2] = 36;  // the glyph cell: 36x42
    obj->rect[3] = 42;
    obj->font = font;
    obj->palette = int(palette);
    char buf[32];
    std::snprintf(buf, sizeof buf, "char %04llX", (unsigned long long)code);
    obj->name = buf;
    obj->pos.parts[0].start({x, y}, 0, 0);
    obj->zoom.parts[0].start({sx, sy}, 0, 0);
    return add(std::move(obj));
}

int64_t Graphics::new_snow(int64_t count, int64_t layer, const int64_t dist[4], double size) {
    auto obj = std::make_unique<SnowObject>(0, layer, count, dist, size, ui_image(245, 0));
    obj->name = "snow x" + std::to_string(count);
    return add(std::move(obj));
}

int64_t Graphics::new_actor(int64_t character, int64_t cloth, int64_t pose, int64_t layer, double ox, double oy) {
    auto obj = std::make_unique<ActorObject>(this, 0, layer);
    obj->set_picture(character, cloth, pose, ox, oy);
    return add(std::move(obj));
}

int64_t Graphics::new_extra(int64_t size, int64_t picture, int64_t variant, int64_t layer) {
    auto obj = std::make_unique<ActorObject>(this, 0, layer);
    obj->set_extra(size, picture, variant);
    return add(std::move(obj));
}

int64_t Graphics::clone(int64_t uid) {
    auto it = objects.find(uid);
    if (it == objects.end()) return 0;
    return add(it->second->clone());
}

ScreenObject* Graphics::get(int64_t uid) {
    auto it = objects.find(uid);
    return it == objects.end() ? nullptr : it->second.get();
}

void Graphics::actor_files(int64_t character, int64_t cloth, int64_t pose, std::string& body, std::string& face) {
    body.clear();
    face.clear();
    {
        // F4-37 (docs/technical/vita-cpk-format.md): the PS2's two-level table at other addresses. Each entry names
        // the whole picture stem: "b1_Aa" is the PS2's pose stem "b1_A" plus its cloth letter "a", so pose picks
        // the *12 term and cloth the +1 term (the PS2 appends char('a' + cloth) the same way). Nothing is
        // appended; the files are stem + "x"
        // (picture) and stem + "z.zb" (face layout), as on the PS2.
        constexpr uint32_t TABLE_A = 0x810ac0b2, TABLE_B = 0x810ca9cc, GROUP = 264, OUTFIT_STRIDE = 12;
        size_t at = amg::vita_va_to_off(exe, uint32_t(TABLE_A + 2 * uint32_t(character)), "character table");
        uint32_t row = le16(exe, at);
        uint32_t index = row * GROUP + uint32_t(pose) * OUTFIT_STRIDE + uint32_t(cloth);
        size_t entry = amg::vita_va_to_off(exe, uint32_t(TABLE_B + 8 * index), "character entry");
        uint32_t b = le32(exe, entry), f = le32(exe, entry + 4);
        if (b) body = "body/" + exe_string(b);
        if (f) face = "face/" + exe_string(f);
        return;
    }
}

const std::vector<std::array<double, 7>>& Graphics::layout(const std::string& name) {
    auto it = layouts.find(name);
    if (it != layouts.end()) return it->second;
    std::vector<std::array<double, 7>> out;
    std::string data;
    bool found = !name.empty() && read_file(name + "z.zb", data);
    if (found) {
        for (size_t i = 0; i + 32 <= data.size(); i += 32) {
            std::array<double, 7> r;
            for (int k = 0; k < 6; ++k) r[size_t(k)] = double(lef(data, i + 4 * size_t(k)));
            r[6] = double(le32(data, i + 24));
            out.push_back(r);
        }
    }
    return layouts.emplace(name, std::move(out)).first->second;
}

void Graphics::actor_mouth(const std::string& face, double ox, double oy, double out[2]) {
    const auto& records = layout(face);
    for (const auto& r : records) {
        if (r[6] >= 3) {
            // On the Vita the layout is in its double-resolution texels: halved back to the scripts' units.
            double k = 0.5;
            out[0] = ox + (r[0] + (r[4] - 1.0) * 0.5) * k - ActorObject::CANVAS_W * 0.5;
            out[1] = oy + (r[1] + (r[5] - 1.0) * 0.5) * k - ActorObject::CANVAS_H * 0.5;
            return;
        }
    }
    out[0] = out[1] = 0.0;
}

namespace {

void paste(Image& canvas, const Image& sheet, const std::vector<std::array<double, 7>>& records, double ox, double oy,
           int fade = 256) {
    for (const auto& r : records) {
        int dx = int(r[0] + ox), dy = int(r[1] + oy), sx = int(r[2]), sy = int(r[3]), w = int(r[4]), h = int(r[5]);
        int x0 = std::max(dx, 0), x1 = std::min(dx + w, canvas.w);
        if (x0 >= x1) continue;
        for (int j = 0; j < h; ++j) {
            int y = dy + j;
            if (!(y >= 0 && y < canvas.h && sy + j < sheet.h)) continue;
            // The source slice is cut short by the end of the sheet's row, as a Python slice is.
            int s0 = sx + x0 - dx;
            int s1 = std::min(sx + x1 - dx, sheet.w);
            if (s0 >= s1) continue;
            const uint8_t* src = sheet.row(sy + j) + size_t(s0) * 4;
            int n = s1 - s0;
            bool any = false, all_opaque = true;
            for (int i = 0; i < n; ++i) {
                uint8_t a = src[i * 4 + 3];
                if (a) any = true;
                if (a < 0x80) all_opaque = false;
            }
            if (!any) continue;
            uint8_t* dst = canvas.px.data() + (size_t(y) * size_t(canvas.w) + size_t(x0)) * 4;
            if (all_opaque && fade >= 256) {
                std::memcpy(dst, src, size_t(n) * 4);
                continue;
            }
            for (int i = 0; i < n; ++i) {
                int a = src[i * 4 + 3];
                if (!a) continue;
                int a2 = (std::min(a * 2, 256) * fade) >> 8;
                for (int c = 0; c < 3; ++c)
                    dst[i * 4 + c] = uint8_t((src[i * 4 + c] * a2 + dst[i * 4 + c] * (256 - a2)) >> 8);
                dst[i * 4 + 3] = uint8_t(std::max<int>(dst[i * 4 + 3], a));
            }
        }
    }
}

}  // namespace

// A whole picture pasted at the origin of an empty actor canvas (what actor_canvas makes of a body with no face).
ImagePtr Graphics::compose_whole(const ImagePtr& sheet) {
    if (!sheet) return nullptr;
    auto canvas = std::make_shared<Image>();
    canvas->w = 2 * ActorObject::CANVAS_W;
    canvas->h = 2 * ActorObject::CANVAS_H;
    canvas->px.assign(size_t(canvas->w) * size_t(canvas->h) * 4, 0);
    std::vector<std::array<double, 7>> whole = {{0, 0, 0, 0, double(sheet->w), double(sheet->h), 0}};
    paste(*canvas, *sheet, whole, 0.0, 0.0);
    return canvas;
}

ImagePtr Graphics::actor_canvas(const std::string& body, const std::string& face, int eye, int mouth, double ox,
                                double oy, int blush, int sweat, int drop) {
    char kbuf[64];
    std::snprintf(kbuf, sizeof kbuf, "#%d#%d#%.17g#%.17g#%d#%d#%d", eye, mouth, ox, oy, blush, sweat, drop);
    std::string key = "actor#" + body + "#" + face + kbuf;
    auto it = images.find(key);
    if (it != images.end()) return it->second;

    {
        // The Vita's body is one whole picture (no layout of its own); its face layout (face/<stem>z.zb) has the
        // eyes in groups 1 and 2, the mouth in 3, as the PS2's, and the sweat drop (4) and blush (5) that the PS2
        // keeps in the body's layout. Offsets come in the scripts' units: twice that in texels.
        const int W = 2 * ActorObject::CANVAS_W, H = 2 * ActorObject::CANVAS_H;
        double vx = 2 * ox, vy = 2 * oy;
        if (face.empty() && !body.empty() && vx == 0.0 && vy == 0.0) {
            auto ready = pending_whole.find(body);
            if (ready != pending_whole.end()) {
                ImagePtr whole = ready->second.get();
                pending_whole.erase(ready);
                if (whole) {
                    keep_actor(key, whole);
                    return whole;
                }
            }
        }
        auto canvas = std::make_shared<Image>();
        canvas->w = W;
        canvas->h = H;
        canvas->px.assign(size_t(W) * size_t(H) * 4, 0);
        if (ImagePtr sheet = body.empty() ? nullptr : image_named(body + "x")) {
            std::vector<std::array<double, 7>> whole = {{0, 0, 0, 0, double(sheet->w), double(sheet->h), 0}};
            paste(*canvas, *sheet, whole, vx, vy);
        }
        if (!face.empty()) {
            ImagePtr face_sheet = image_named(face + "x");
            const auto& records = layout(face);
            auto nth = [&](int group, int index) -> const std::array<double, 7>* {
                int seen = 0;
                for (const auto& r : records)
                    if (int(r[6]) == group && seen++ == index) return &r;
                return nullptr;
            };
            if (face_sheet) {
                if (blush)
                    if (auto* r = nth(5, 0)) paste(*canvas, *face_sheet, {*r}, vx, vy, blush * 16);
                std::vector<std::array<double, 7>> parts;
                for (auto* r : {nth(1, eye), nth(2, eye), nth(3, mouth)})
                    if (r) parts.push_back(*r);
                paste(*canvas, *face_sheet, parts, vx, vy);
                if (sweat)
                    if (auto* r = nth(4, 0)) paste(*canvas, *face_sheet, {*r}, vx, vy - 2 * drop, sweat * 16);
            }
        }
        keep_actor(key, canvas);
        return canvas;
    }
}

void Graphics::keep_actor(const std::string& key, ImagePtr image) {
    // Pictures over 256 KB (backgrounds, event pictures, composed characters) count against a budget of 384 MB,
    // oldest out first; the small ones (glyphs, interface parts) stay. An object that shows a picture keeps its
    // own reference, so dropping it from the cache never takes it off the screen.
    size_t bytes = image ? image->px.size() : 0;
    if (bytes >= 256 * 1024 && !images.count(key)) {
        actor_keys.push_back(key);
        cached_bytes += bytes;
    }
    images[key] = std::move(image);
    const size_t BUDGET = size_t(384) << 20;
    while (cached_bytes > BUDGET && !actor_keys.empty()) {
        auto it = images.find(actor_keys.front());
        if (it != images.end()) {
            if (it->second) cached_bytes -= std::min(cached_bytes, it->second->px.size());
            images.erase(it);
        }
        actor_keys.pop_front();
    }
}

void Graphics::step(double dt) {
    ++steps;
    for (auto& [uid, obj] : objects) obj->step(dt);
}

namespace {

void draw(std::vector<uint8_t>& frame, const DrawItem& obj, bool smooth, int ymin, int ymax) {
    double alpha = obj.alpha;
    if (alpha <= 0) return;
    alpha = std::min(alpha, 1.0);
    const double* light = obj.light;
    const double* centre = obj.centre;
    const double* scale = obj.scale;
    double cx = centre[0], cy = centre[1], sx = scale[0], sy = scale[1];
    double rx = obj.rect[0], ry = obj.rect[1], rw = obj.rect[2], rh = obj.rect[3];
    double w = rw * std::fabs(sx), h = rh * std::fabs(sy);
    if (w <= 0 || h <= 0) return;
    // Engine (sprite draw at 0x106308): X = cos*lx + sin*ly, Y = cos*ly - sin*lx, so a positive
    // angle turns the picture counter-clockwise on screen. The loop below uses the inverse.
    double angle = obj.angle * PI;
    double cos_a = std::cos(angle), sin_a = -std::sin(angle);
    double centre_x = SCREEN_W / 2.0 + cx, centre_y = SCREEN_H / 2.0 + cy;
    double asx = std::fabs(sx), asy = std::fabs(sy);
    double half_w, half_h, ux, uy, vx, vy;
    if (obj.scale_on_screen) {
        half_w = (std::fabs(rw * cos_a) + std::fabs(rh * sin_a)) / 2 * asx;
        half_h = (std::fabs(rw * sin_a) + std::fabs(rh * cos_a)) / 2 * asy;
        ux = cos_a / asx;
        uy = sin_a / asy;
        vx = -sin_a / asx;
        vy = cos_a / asy;
    } else {
        half_w = (std::fabs(w * cos_a) + std::fabs(h * sin_a)) / 2;
        half_h = (std::fabs(w * sin_a) + std::fabs(h * cos_a)) / 2;
        ux = cos_a * rw / w;
        uy = sin_a * rw / w;
        vx = -sin_a * rh / h;
        vy = cos_a * rh / h;
    }
    auto clampi = [](double v, int lo, int hi) {
        double f = std::floor(v + 0.5);
        if (f < lo) return lo;
        if (f > hi) return hi;
        return int(f);
    };
    int x0 = clampi(centre_x - half_w, 0, SCREEN_W), x1 = clampi(centre_x + half_w, 0, SCREEN_W);
    int y0 = std::max(ymin, clampi(centre_y - half_h, 0, SCREEN_H)), y1 = std::min(ymax, clampi(centre_y + half_h, 0, SCREEN_H));
    if (obj.clipped) {
        x0 = std::max(x0, int(obj.clip[0]));
        x1 = std::min(x1, int(obj.clip[2]));
        y0 = std::max(y0, int(obj.clip[1]));
        y1 = std::min(y1, int(obj.clip[3]));
    }
    if (x0 >= x1 || y0 >= y1) return;

    const Image* image = obj.image.get();
    if (smooth && image) {
        // Drawn at 1:1 with texel centres on pixel centres, bilinear is nearest: skip the filter.
        double u0 = rw / 2 + (x0 + 0.5 - centre_x) * ux + (y0 + 0.5 - centre_y) * uy;
        double v0 = rh / 2 + (x0 + 0.5 - centre_x) * vx + (y0 + 0.5 - centre_y) * vy;
        bool unit = std::fabs(ux - 1) < 1e-9 && std::fabs(uy) < 1e-9 && std::fabs(vx) < 1e-9 && std::fabs(vy - 1) < 1e-9;
        if (unit && std::fabs(u0 - std::floor(u0) - 0.5) < 1e-6 && std::fabs(v0 - std::floor(v0) - 0.5) < 1e-6) smooth = false;
    }
    if (smooth && image) {
        // Fixed-point bilinear sampling, clamped to the sprite's rectangle (the window's path).
        // A negative light (vibLight around a low base) draws black: the Vita's fragment shader is texel x colour,
        // saturated by the 8-bit target (Vita3K's shader log). It used to wrap through uint8_t.
        int lr_ = std::max(0, int(light[0] * 256)), lg_ = std::max(0, int(light[1] * 256)), lb_ = std::max(0, int(light[2] * 256));
        int alpha256 = int(alpha * 256);
        int lo_u = int(rx), hi_u = std::min(int(rx + rw) - 1, image->w - 1);
        int lo_v = int(ry), hi_v = std::min(int(ry + rh) - 1, image->h - 1);
        const int64_t ONE = 65536;
        int64_t dU = int64_t(std::llround(ux * double(ONE))), dV = int64_t(std::llround(vx * double(ONE)));
        for (int y = y0; y < y1; ++y) {
            double dy = y + 0.5 - centre_y, dx = x0 + 0.5 - centre_x;
            double u = rw / 2 + dx * ux + dy * uy, v = rh / 2 + dx * vx + dy * vy;
            int64_t U = int64_t(std::llround((rx + u - 0.5) * double(ONE)));
            int64_t V = int64_t(std::llround((ry + v - 0.5) * double(ONE)));
            double uu = u, vv = v;
            uint8_t* row = frame.data() + size_t(y) * SCREEN_W * 3 + size_t(x0) * 3;
            for (int x = x0; x < x1; ++x, row += 3, U += dU, V += dV, uu += ux, vv += vx) {
                if (!(0 <= uu && uu < rw && 0 <= vv && vv < rh)) continue;
                int u0 = int(U >> 16), v0 = int(V >> 16);
                int fu = int((U >> 8) & 255), fv = int((V >> 8) & 255);
                int ua = u0 < lo_u ? lo_u : (u0 > hi_u ? hi_u : u0);
                int ub = u0 + 1 < lo_u ? lo_u : (u0 + 1 > hi_u ? hi_u : u0 + 1);
                int va = v0 < lo_v ? lo_v : (v0 > hi_v ? hi_v : v0);
                int vb = v0 + 1 < lo_v ? lo_v : (v0 + 1 > hi_v ? hi_v : v0 + 1);
                const uint8_t* p00 = image->row(va) + size_t(ua) * 4;
                const uint8_t* p10 = image->row(va) + size_t(ub) * 4;
                const uint8_t* p01 = image->row(vb) + size_t(ua) * 4;
                const uint8_t* p11 = image->row(vb) + size_t(ub) * 4;
                if ((p00[3] | p10[3] | p01[3] | p11[3]) == 0) continue;  // transparent all around
                int tex[4];
                for (int c = 0; c < 4; ++c) {
                    int top = p00[c] * 256 + (p10[c] - p00[c]) * fu;
                    int bot = p01[c] * 256 + (p11[c] - p01[c]) * fu;
                    tex[c] = (top * 256 + (bot - top) * fv + 32768) >> 16;
                }
                int a = (tex[3] * alpha256) >> 7;  // 0x80 in the texture is opaque
                if (a <= 0) continue;
                if (a > 256) a = 256;
                if (obj.additive) {
                    for (int c = 0; c < 3; ++c) {
                        int l = c == 0 ? lr_ : (c == 1 ? lg_ : lb_);
                        row[c] = uint8_t(std::min(row[c] + ((std::min((tex[c] * l) >> 8, 255) * a) >> 8), 255));
                    }
                    continue;
                }
                if (a == 256) {
                    row[0] = uint8_t(std::min((tex[0] * lr_) >> 8, 255));
                    row[1] = uint8_t(std::min((tex[1] * lg_) >> 8, 255));
                    row[2] = uint8_t(std::min((tex[2] * lb_) >> 8, 255));
                    continue;
                }
                int ia = 256 - a;
                int r = std::min((tex[0] * lr_) >> 8, 255), g = std::min((tex[1] * lg_) >> 8, 255),
                    b = std::min((tex[2] * lb_) >> 8, 255);
                row[0] = uint8_t((r * a + row[0] * ia) >> 8);
                row[1] = uint8_t((g * a + row[1] * ia) >> 8);
                row[2] = uint8_t((b * a + row[2] * ia) >> 8);
            }
        }
        return;
    }
    int plain[3] = {0, 0, 0};
    int lr = 256, lg = 256, lb = 256;
    if (!image) {
        for (int c = 0; c < 3; ++c) plain[c] = std::max(0, std::min(255, int(light[size_t(c)] * 255)));
    } else {
        lr = std::max(0, int(light[0] * 256));
        lg = std::max(0, int(light[1] * 256));
        lb = std::max(0, int(light[2] * 256));
    }
    for (int y = y0; y < y1; ++y) {
        double dy = y + 0.5 - centre_y;
        double dx = x0 + 0.5 - centre_x;
        double u = rw / 2 + dx * ux + dy * uy;
        double v = rh / 2 + dx * vx + dy * vy;
        uint8_t* row = frame.data() + size_t(y) * SCREEN_W * 3;
        for (int x = x0 * 3; x < x1 * 3; x += 3) {
            if (0 <= u && u < rw && 0 <= v && v < rh) {
                int r, g, b, a;
                if (!image) {
                    r = plain[0];
                    g = plain[1];
                    b = plain[2];
                    a = int(alpha * 256);
                } else {
                    int ui = int(rx + u), vi = int(ry + v);
                    if (0 <= ui && ui < image->w && 0 <= vi && vi < image->h) {
                        int tex[4];
                        if (smooth) {
                            // Bilinear between texel centres, clamped to the sprite's rectangle.
                            double su = rx + u - 0.5, sv = ry + v - 0.5;
                            int u0 = int(std::floor(su)), v0 = int(std::floor(sv));
                            int fu = int((su - u0) * 256.0), fv = int((sv - v0) * 256.0);
                            int lo_u = int(rx), hi_u = std::min(int(rx + rw) - 1, image->w - 1);
                            int lo_v = int(ry), hi_v = std::min(int(ry + rh) - 1, image->h - 1);
                            int ua = std::max(lo_u, std::min(u0, hi_u)), ub = std::max(lo_u, std::min(u0 + 1, hi_u));
                            int va = std::max(lo_v, std::min(v0, hi_v)), vb = std::max(lo_v, std::min(v0 + 1, hi_v));
                            const uint8_t* p00 = image->row(va) + size_t(ua) * 4;
                            const uint8_t* p10 = image->row(va) + size_t(ub) * 4;
                            const uint8_t* p01 = image->row(vb) + size_t(ua) * 4;
                            const uint8_t* p11 = image->row(vb) + size_t(ub) * 4;
                            for (int c = 0; c < 4; ++c) {
                                int top = p00[c] * 256 + (p10[c] - p00[c]) * fu;
                                int bot = p01[c] * 256 + (p11[c] - p01[c]) * fu;
                                tex[c] = (top * 256 + (bot - top) * fv + 32768) >> 16;
                            }
                        } else {
                            const uint8_t* s = image->row(vi) + size_t(ui) * 4;
                            for (int c = 0; c < 4; ++c) tex[c] = s[c];
                        }
                        a = int(tex[3] * alpha * 2);
                        r = std::min((tex[0] * lr) >> 8, 255);
                        g = std::min((tex[1] * lg) >> 8, 255);
                        b = std::min((tex[2] * lb) >> 8, 255);
                    } else {
                        a = 0;
                        r = g = b = 0;
                    }
                }
                if (a > 0 && obj.additive) {
                    if (a > 256) a = 256;
                    row[x] = uint8_t(std::min(row[x] + ((r * a) >> 8), 255));
                    row[x + 1] = uint8_t(std::min(row[x + 1] + ((g * a) >> 8), 255));
                    row[x + 2] = uint8_t(std::min(row[x + 2] + ((b * a) >> 8), 255));
                } else if (a > 0) {
                    if (a > 256) a = 256;
                    int ia = 256 - a;
                    row[x] = uint8_t((r * a + row[x] * ia) >> 8);
                    row[x + 1] = uint8_t((g * a + row[x + 1] * ia) >> 8);
                    row[x + 2] = uint8_t((b * a + row[x + 2] * ia) >> 8);
                }
            }
            u += ux;
            v += vx;
        }
    }
}

}  // namespace

std::shared_ptr<const Scene> Graphics::scene() const {
    auto out = std::make_shared<Scene>();
    out->items.reserve(objects.size());
    out->hide_uid = hide_uid;
    out->hide_image = hide_image;
    out->stamp = steps;
    // FIDELITY (docs/technical/d1-draw-order.md, read twice; a Vita3K capture of two overlapping characters is still to
    // do, so AMG_VITA_DRAW_ORDER=1 turns it on): the engine sorts the characters of a layer by the zoom's x, the larger in
    // front (0x8100DB4E), whatever the order they were made in. The orders the characters had are handed out again by
    // that key; everything else keeps its own.
    static const bool vita_order = std::getenv("AMG_VITA_DRAW_ORDER") != nullptr;
    std::map<int64_t, int64_t> order_of;  // uid -> the order to draw it with
    if (vita_order) {
        std::map<int64_t, std::vector<const ActorObject*>> by_layer;
        for (const auto& [uid, obj] : objects)
            if (auto* actor = dynamic_cast<const ActorObject*>(obj.get()); actor && !actor->draw_target) by_layer[actor->layer].push_back(actor);
        for (auto& [layer, actors] : by_layer) {
            std::vector<int64_t> orders;
            for (auto* a : actors) orders.push_back(a->order);
            std::sort(orders.begin(), orders.end());  // the smallest order is drawn last: the front
            std::stable_sort(actors.begin(), actors.end(), [](const ActorObject* a, const ActorObject* b) {
                return a->zoom.parts[0].current.at(0) > b->zoom.parts[0].current.at(0);  // the larger zoom first: the front
            });
            for (size_t i = 0; i < actors.size(); ++i) order_of[actors[i]->uid] = orders[i];
        }
    }
    for (const auto& [uid, obj] : objects) {
        auto* actor = dynamic_cast<const ActorObject*>(obj.get());
        if (actor && actor->draw_target && objects.count(actor->draw_target)) continue;  // drawn by its cut-in window
        const size_t from = out->items.size();
        obj->fill_all(out->items);
        const bool text = obj->name.rfind("char ", 0) == 0;  // a character of text (new_char)
        // a picture of the game's content (background, event picture, gallery or extras picture, character): never interface
        const std::string& nm = obj->name;
        const bool content = nm.rfind("bg/", 0) == 0 || nm.rfind("epi/", 0) == 0 || nm.rfind("extra ", 0) == 0 ||
                             nm.rfind("actor ", 0) == 0;
        for (size_t i = from; i < out->items.size(); ++i) {
            out->items[i].uid = uid;
            out->items[i].text = text;
            out->items[i].content = content;
        }
        if (auto it = order_of.find(uid); it != order_of.end())
            for (size_t i = from; i < out->items.size(); ++i) out->items[i].order = it->second;
    }
    if (hide_uid && mark.h > 0) {  // the selection mark in place of the hidden arrow (gfx.hpp, SelectionMark)
        for (size_t i = 0; i < out->items.size(); ++i) {
            const DrawItem& arrow = out->items[i];
            if (arrow.uid != hide_uid) continue;
            DrawItem bar;
            bar.layer = arrow.layer;
            bar.order = arrow.order;  // same key, made after it: drawn right after the arrow, before the older rows
            bar.ui = arrow.ui;
            bar.follow = arrow.follow;
            bar.rect[2] = mark.x1 - mark.x0;
            bar.rect[3] = mark.h;
            bar.centre[0] = (mark.x0 + mark.x1) / 2;
            bar.centre[1] = arrow.centre[1];
            for (int c = 0; c < 3; ++c) bar.light[c] = mark.light[c];
            bar.alpha = mark.alpha * std::min(1.0, arrow.alpha);
            out->items.insert(out->items.begin() + std::ptrdiff_t(i) + 1, bar);
            break;
        }
    }
    // Back to front by layer; inside a layer the last created is drawn first (docs/technical/objects.md).
    std::stable_sort(out->items.begin(), out->items.end(), [](const DrawItem& a, const DrawItem& b) {
        if (a.layer != b.layer) return a.layer < b.layer;
        return -a.order < -b.order;
    });
    if (glyph_picker) {  // the button glyphs: the current controller's pictures in place of the game's
        for (DrawItem& it : out->items) {
            if (!it.image || !it.image->glyph) continue;
            if (ImagePtr skin = glyph_picker(it.image->glyph, it.centre[0])) {
                it.image = skin;
                it.rect[0] = it.rect[1] = 0;
                it.rect[2] = skin->w;
                it.rect[3] = skin->h;
            }
        }
    }
    return out;
}

std::vector<uint8_t> Graphics::render() const { return render_scene(*scene(), smooth); }

// The view transform (gfx.hpp, VIEW_*): logical positions to screen pixels. The origin offset (VIEW_ORG_Y) set by
// setScrOrgY moves the world pictures (backgrounds, event pictures, characters: DrawItem::follow) and not the interface:
// applied to everything it moved the title and the main menu 32 pixels off, applied to the world only the events line up
// with Vita3K (docs/technical/port.md, Vita visual audit).
static Scene view_of(const Scene& scene) {
    Scene out = scene;
    double k = VIEW_UNIT * VIEW_SCALE, half_w = SCREEN_W / 2.0, half_h = SCREEN_H / 2.0;
    for (DrawItem& it : out.items) {
        // The Vita scales a turned picture along its own axes (the PS2's scale_on_screen pictures, backgrounds and
        // event pictures, scale along the screen's). FIDELITY: read 0x81088CB0 and 0x81089D80 (BG_Picture's and
        // EventPicture's step): the zoom is corrected by the turn for the PS2's pixel aspect, x by
        // sqrt(sin^2 * (448/480)^2 + cos^2) and y by sqrt(sin^2 * (480/448)^2 + cos^2); the Vita scripts compensate
        // where they need a square picture (TitleLogo's 1.7142857 x 1.4933 = 1.6 x 1.6). And 0x8100250C (their
        // move, which setPos calls) stores the position as (x * 0.75, y * 0.75 * 480 / 448). Measured on the menu's
        // sky (bg/122_10 turned, zoom 1.5 x 1.3, setPos(0, 192)) in Vita3K: 1.40 x 1.40, centre 172 units down;
        // this gives 1.40 x 1.39 and 170.
        if (it.scale_on_screen) {
            it.centre[0] *= 0.75;
            it.centre[1] *= 0.75 * 480.0 / 448.0;
            if (it.angle != 0.0) {
                double s = std::sin(it.angle * PI), c = std::cos(it.angle * PI);
                it.scale[0] *= std::sqrt(s * s * (448.0 / 480.0) * (448.0 / 480.0) + c * c);
                it.scale[1] *= std::sqrt(s * s * (480.0 / 448.0) * (480.0 / 448.0) + c * c);
            }
        }
        it.scale_on_screen = false;
        // FIDELITY: read 0x81008124 and 0x8100880E (Sprite's two creators): a sprite always points at the interface
        // view, scale 1 and origin 0, as the backgrounds of the interface places do. So a picture that follows no
        // origin (follow 0) keeps scale 1 when setScreen(false) sets 0.775, and so does a sprite with no texture
        // (K2X_Script's black side bars, Sprite.new(0, 7), which cover the edges outside the 0.775 picture: Vita3K
        // shows them at x 0-107 and 852-959). Other plain rectangles keep the screen scale (not traced).
        const double vs = (it.ui || (it.image && it.follow == 0.0)) ? 1.0 : VIEW_SCALE, kk = VIEW_UNIT * vs;
        it.centre[0] *= kk;
        // FIDELITY: read 0x8100E138 (a world rectangle) and 0x81097248 (an object's view). A world object points at the
        // scale and Y origin that setScrScl and setScrOrgY write (0x810DE4D8, 0x810DFFAC), an interface object at a fixed
        // scale 1 and origin 0; the origin is added after the scale, in units (y' = org + y * scale). The X origin
        // (0x810DFFA8) is set to 0 once and never written again. setScreen's pairs (1, 16) and (0.775, 0) give the same
        // picture as adding it before the scale, which is what the port did (fitted on n6sir_ec03E).
        it.centre[1] = (it.centre[1] * vs + it.follow * VIEW_ORG_Y) * VIEW_UNIT;
        double size = it.image ? vs : kk;
        it.scale[0] *= size;
        it.scale[1] *= size;
        if (it.clipped) {
            it.clip[0] = half_w + (it.clip[0] - half_w) * kk;
            it.clip[2] = half_w + (it.clip[2] - half_w) * kk;
            it.clip[1] = half_h + (it.clip[1] - half_h) * kk;
            it.clip[3] = half_h + (it.clip[3] - half_h) * kk;
        }
    }
    if (UI_SCALE != 1.0) {
        const double u = UI_SCALE;
        auto is_interface = [&](const DrawItem& it) {
            if (it.snow || it.content || it.layer < UI_FIRST_LAYER) return false;
            const double w = std::fabs(it.rect[2]) * it.scale[0], h = std::fabs(it.rect[3]) * it.scale[1];
            // covers the screen: a background, a fade, a side bar (a full-width text window is interface)
            return !(h >= 0.9 * SCREEN_H || (w >= 0.9 * SCREEN_W && h >= 0.6 * SCREEN_H));
        };
        // Anchored shrink (docs/technical/interface-scale.md). The visible interface items are put in groups (items whose
        // boxes overlap, and groups whose boxes overlap, so a panel and what is drawn on it are one piece); each group
        // shrinks about its own anchor, the point that keeps the ratio of its free margins to the screen's edges: on each
        // axis a = lo + size * m_lo / (m_lo + m_hi), the margins clamped at 0 (plus a small constant so that a group as
        // wide as the screen anchors at its middle with no jump). So what touches the left edge stays at the left edge,
        // a corner stays in its corner, what is centred stays centred, groups never cross (the anchors keep the groups'
        // order) and what lies outside the screen keeps its anchor on its own outer edge and stays outside.
        const size_t n = out.items.size();
        std::vector<int> group(n, -1);
        struct Box { double x0, y0, x1, y1, area, th = 0.0; int64_t l0 = 0, l1 = 0; };  // th: line height if only text; l0..l1: layers
        std::vector<Box> boxes;  // per group
        std::vector<char> kind;  // per group: 1 live, 0 merged away, 2 a lone item (hidden or outside the screen)
        auto box_of = [&](const DrawItem& it) {
            const double hw = std::fabs(it.rect[2] * it.scale[0]) / 2, hh = std::fabs(it.rect[3] * it.scale[1]) / 2;
            return Box{it.centre[0] - hw, it.centre[1] - hh, it.centre[0] + hw, it.centre[1] + hh, 4 * hw * hh,
                       it.text ? 2 * hh : 0.0, it.layer, it.layer};
        };
        // When two boxes are one piece (interface-scale.md, "Groups"): one lies at least half inside the other (a panel and
        // its rows, a stroke and its letters, the text window and its text); or they touch and neither spans the screen's
        // width and their layers are next to each other (a glyph and its label, the pieces of a panel's frame, the topic
        // wheel's icons; a bar across the screen must not swallow the calendar it touches, a dialog on layer 12 must not
        // swallow the main-menu column on layer 10 beside it); or both are text and the gap is under a line (the lines of one
        // paragraph, also on a full-screen page).
        const double pad = 2.0 * VIEW_UNIT;
        auto meet = [&](const Box& a, const Box& b) {
            const double ix = std::min(a.x1, b.x1) - std::max(a.x0, b.x0), iy = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
            const double aa = (a.x1 - a.x0) * (a.y1 - a.y0), ab = (b.x1 - b.x0) * (b.y1 - b.y0);
            if (ix > 0 && iy > 0 && ix * iy >= 0.5 * std::min(aa, ab)) return true;
            if (a.th > 0 && b.th > 0) {
                const double p = std::max(pad, std::min(a.th, b.th));
                if (ix > -p && iy > -p) return true;
            }
            auto spans = [&](const Box& c) { return c.x1 - c.x0 >= 0.9 * SCREEN_W; };
            const bool near_layers = a.l0 <= b.l1 + 1 && b.l0 <= a.l1 + 1;
            return near_layers && !spans(a) && !spans(b) && ix > -pad && iy > -pad;
        };
        auto on_screen = [&](const Box& b) { return b.x1 > -half_w && b.x0 < half_w && b.y1 > -half_h && b.y0 < half_h; };
        for (size_t i = 0; i < n; ++i) {
            const DrawItem& it = out.items[i];
            if (!is_interface(it)) continue;
            Box b = box_of(it);
            // Hidden items and items wholly outside the screen join nobody (a hidden panel would chain unrelated items; a
            // parked entry must not drag a panel on screen): each is its own group.
            if (it.alpha <= 0.01 || !on_screen(b)) {
                group[i] = int(boxes.size());
                boxes.push_back(b);
                kind.push_back(2);
                continue;
            }
            int g = int(boxes.size());
            boxes.push_back(b);
            kind.push_back(1);
            group[i] = g;
            // Merge every live group the (growing) group g meets, until none is left.
            for (bool again = true; again;) {
                again = false;
                for (size_t j = 0; j < boxes.size(); ++j) {
                    if (int(j) == g || kind[j] != 1 || !meet(boxes[j], boxes[size_t(g)])) continue;
                    for (size_t m = 0; m < i; ++m) if (group[m] == int(j)) group[m] = g;
                    Box& G = boxes[size_t(g)];
                    G = {std::min(G.x0, boxes[j].x0), std::min(G.y0, boxes[j].y0), std::max(G.x1, boxes[j].x1),
                         std::max(G.y1, boxes[j].y1), G.area + boxes[j].area,
                         (G.th > 0 && boxes[j].th > 0) ? std::min(G.th, boxes[j].th) : 0.0,
                         std::min(G.l0, boxes[j].l0), std::max(G.l1, boxes[j].l1)};
                    kind[j] = 0;
                    again = true;
                }
            }
        }
        // The anchor comes from the box of the group's PRINCIPAL item (interface-scale.md, "Stability"): its largest member,
        // ties (within CORE of that area) to the lowest uid. Other members (the L1/R1 marks that slide beside the tabs, a
        // bobbing arrow, a mark that fades in, entries sliding in one by one) do not change it, so the group stays still.
        constexpr double CORE = 0.9;  // within this share of the largest area: a tie, the oldest (lowest uid) wins
        std::vector<double> big(boxes.size(), 0.0);
        std::vector<Box> core(boxes.size(), Box{1e30, 1e30, -1e30, -1e30, 0.0});
        for (size_t i = 0; i < n; ++i)
            if (group[i] >= 0) big[size_t(group[i])] = std::max(big[size_t(group[i])], box_of(out.items[i]).area);
        // Per axis: the low side shared by the most tied members (a column's entries aligned at the left), the box of the
        // oldest of those, so entries nudged by the cursor or sliding in or out do not count; if no two share it, the
        // oldest tied member's.
        std::vector<std::vector<std::pair<int64_t, Box>>> ties(boxes.size());
        for (size_t i = 0; i < n; ++i) {
            if (group[i] < 0) continue;
            const Box b = box_of(out.items[i]);
            if (b.area >= CORE * big[size_t(group[i])]) ties[size_t(group[i])].push_back({out.items[i].uid, b});
        }
        for (size_t g = 0; g < boxes.size(); ++g) {
            auto& t = ties[g];
            if (t.empty()) continue;
            std::sort(t.begin(), t.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (int axis = 0; axis < 2; ++axis) {
                auto lo = [&](const Box& b) { return axis ? b.y0 : b.x0; };
                const Box* best = &t[0].second;
                size_t most = 1;
                for (const auto& c : t) {
                    size_t same = 0;
                    for (const auto& d : t) same += std::fabs(lo(d.second) - lo(c.second)) < 0.5;
                    if (same > most) { most = same; best = &c.second; }
                }
                if (axis) { core[g].y0 = best->y0; core[g].y1 = best->y1; }
                else { core[g].x0 = best->x0; core[g].x1 = best->x1; }
            }
        }
        const double eps = 2.0 * VIEW_UNIT;
        auto anchor = [&](double lo, double hi, double half) {
            // A live group's box is cut to the screen: a member sliding in from beyond an edge does not move the anchor
            // (it stays on the edge while the box crosses it). A lone item wholly outside keeps its own box.
            if (hi > -half && lo < half) { lo = std::max(lo, -half); hi = std::min(hi, half); }
            const double m_lo = std::max(0.0, lo + half), m_hi = std::max(0.0, half - hi);
            return lo + (hi - lo) * (m_lo + eps) / (m_lo + m_hi + 2 * eps);
        };
        std::vector<double> gax(boxes.size()), gay(boxes.size());
        double biggest = -1;
        out.ui_u = u;
        // Anchor memory (interface-scale.md, "Stability"). A file-level state, keyed by the uid of each live group's
        // principal: its own box, the anchor shown, and what is left of an easing. It advances once per distinct scene
        // stamp; every other pass over the same frame reads the previous frame's state without changing it, so the
        // renderer and the pointer see the same anchors. Rule: while the former principal is still in the group with its
        // box unchanged, the group keeps its anchor (another member that grows over it or starts to overlap does not take
        // over); when the principal moves its anchor follows it at once (a sliding menu slides smoothly); when it is
        // replaced (removed, or moved while another takes over), the anchor eases from the old to the new one over EASE
        // frames instead of jumping. Entries of groups that are gone are dropped at the next frame.
        struct Hold { double bx0, by0, bx1, by1, ax, ay, ox, oy; int left; int64_t principal; };  // b*: the member's own box
        static std::unordered_map<int64_t, Hold> prev, cur;
        static int64_t cur_stamp = -1;
        constexpr int EASE = 8;
        static const bool hold_on = [] { const char* e = std::getenv("AMG_UI_HOLD"); return !e || std::strcmp(e, "0") != 0; }();
        const bool remember = hold_on && scene.stamp != 0;  // AMG_UI_HOLD=0: no memory, to measure what it changes
        if (remember && scene.stamp != cur_stamp) {
            prev.swap(cur);
            cur.clear();
            cur_stamp = scene.stamp;
        }
        std::vector<std::vector<size_t>> members(boxes.size());
        for (size_t i = 0; i < n; ++i)
            if (group[i] >= 0) members[size_t(group[i])].push_back(i);
        auto same_box = [](const Hold& h, const Box& b) {
            return std::fabs(h.bx0 - b.x0) < 0.25 && std::fabs(h.by0 - b.y0) < 0.25 && std::fabs(h.bx1 - b.x1) < 0.25 &&
                   std::fabs(h.by1 - b.y1) < 0.25;
        };
        for (size_t g = 0; g < boxes.size(); ++g) {
            if (kind[g] == 0) continue;
            const Box& c = core[g].x0 <= core[g].x1 ? core[g] : boxes[g];
            gax[g] = anchor(c.x0, c.x1, half_w);
            gay[g] = anchor(c.y0, c.y1, half_h);
            if (remember && kind[g] == 1 && !ties[g].empty()) {
                int64_t p = ties[g][0].first;
                Box pb = ties[g][0].second;
                // A former principal among the members: one whose box is unchanged first, then the current principal
                // itself, then any (the lowest uid, members being in drawing order: the first met).
                const Hold* old = nullptr;
                int64_t old_uid = 0;
                int score = -1;
                for (size_t i : members[g]) {
                    auto f = prev.find(out.items[i].uid);
                    if (f == prev.end()) continue;
                    const bool was_principal = f->second.principal == out.items[i].uid;
                    const int s = same_box(f->second, box_of(out.items[i])) ? 2 + was_principal
                                : (was_principal && out.items[i].uid == p ? 1 : 0);
                    if (s > score) { score = s; old = &f->second; old_uid = out.items[i].uid; }
                }
                Hold h{pb.x0, pb.y0, pb.x1, pb.y1, gax[g], gay[g], 0.0, 0.0, 0, p};
                if (score >= 2) {
                    // Keep the anchor while a member stands still, and any easing left as it is: an easing advances only on
                    // frames where the whole group moves, so it never moves a still piece (interface-scale.md, "Event
                    // screens": the paragraph that types in, the page mark left alone while the page slides out).
                    h = *old;
                    p = old_uid;
                } else if (old && old_uid == p) {
                    h.ox = old->ox;  // the same principal moved: follow it, finishing any easing
                    h.oy = old->oy;
                    h.left = old->left > 0 ? old->left - 1 : 0;
                } else if (old) {
                    // replaced: ease from the anchor shown so far to the new one
                    const double sx = old->ax + old->ox * old->left / EASE, sy = old->ay + old->oy * old->left / EASE;
                    h.ox = sx - gax[g];
                    h.oy = sy - gay[g];
                    h.left = EASE - 1;
                }
                h.principal = p;
                for (size_t i : members[g]) {  // every member carries the group's anchor, so any survivor can hand it on
                    const Box b = box_of(out.items[i]);
                    cur[out.items[i].uid] = Hold{b.x0, b.y0, b.x1, b.y1, h.ax, h.ay, h.ox, h.oy, h.left, p};
                }
                gax[g] = h.ax + h.ox * h.left / EASE;
                gay[g] = h.ay + h.oy * h.left / EASE;
            }
            if (kind[g] == 1 && boxes[g].area > biggest) {
                biggest = boxes[g].area;
                out.ui_ax = gax[g];
                out.ui_ay = gay[g];
            }
        }
        // No frame clip any more: an item outside the screen stays outside (its anchor is on its outer edge), and the
        // picture's own edge cuts what straddles it, as at 100 %. Only the item's own clip moves with it.
        for (size_t i = 0; i < n; ++i) {
            if (group[i] < 0) continue;
            DrawItem& it = out.items[i];
            const double ax = gax[size_t(group[i])], ay = gay[size_t(group[i])];
            it.shrunk = true;
            it.ui_ax = ax;
            it.ui_ay = ay;
            it.centre[0] = ax + (it.centre[0] - ax) * u;
            it.centre[1] = ay + (it.centre[1] - ay) * u;
            it.scale[0] *= u;
            it.scale[1] *= u;
            if (it.clipped) {
                const double cx = half_w + ax, cy = half_h + ay;  // the anchor in screen pixels from the corner
                it.clip[0] = cx + (it.clip[0] - cx) * u;
                it.clip[2] = cx + (it.clip[2] - cx) * u;
                it.clip[1] = cy + (it.clip[1] - cy) * u;
                it.clip[3] = cy + (it.clip[3] - cy) * u;
            }
        }
    }
    // AMG_VIEWSTAT=1: where the items sit horizontally, to judge how the layout would take a wider screen
    // (docs/PLAN.md F5-4). Counts per item and frame; printed once at exit.
    static const bool stat_on = std::getenv("AMG_VIEWSTAT") != nullptr;
    if (stat_on) {
        struct Stat {
            long frames = 0, full = 0, left = 0, right = 0, centre = 0, text = 0;
            void print() const {
                long n = full + left + right + centre + text;
                if (n) std::fprintf(stderr, "VIEWSTAT frames=%ld items/frame=%.1f full=%.1f%% left=%.1f%% right=%.1f%% centre=%.1f%% rect=%.1f%%\n",
                                    frames, double(n) / frames, 100.0 * full / n, 100.0 * left / n, 100.0 * right / n, 100.0 * centre / n, 100.0 * text / n);
            }
        };
        static Stat st;
        ++st.frames;
        if (st.frames % 600 == 0 || st.frames <= 5) st.print();  // the process may end without destructors, so no print at exit
        for (const DrawItem& it : out.items) {
            if (it.snow) continue;
            if (!it.image) { ++st.text; continue; }
            double hw = std::fabs(it.rect[2]) / 2.0 * it.scale[0];
            if (hw * 2 >= 0.9 * SCREEN_W) ++st.full;
            else if (it.centre[0] - hw <= -0.8 * half_w) ++st.left;
            else if (it.centre[0] + hw >= 0.8 * half_w) ++st.right;
            else ++st.centre;
        }
    }
    return out;
}

// Mouse mode (docs/technical/mouse-native-design.md 2.5): the arrow of the menu the mouse drives is not drawn; the lit
// entry is the mark of the selection. Only the drawing leaves it out: the pointer still sees it.
static void drop_hidden(Scene& s) {
    if (!s.hide_uid && !s.hide_image) return;
    s.items.erase(std::remove_if(s.items.begin(), s.items.end(), [&](const DrawItem& it) { return it.uid == s.hide_uid || (s.hide_image && it.image == s.hide_image); }),
                  s.items.end());
}

Scene Graphics::view_scene(const Scene& scene) {
    bool identity = VIEW_UNIT == 1.0 && VIEW_SCALE == 1.0 && UI_SCALE == 1.0;
    Scene out = identity ? scene : view_of(scene);
    drop_hidden(out);
    return out;
}

Scene Graphics::pointer_scene(const Scene& scene) {
    // view_of keeps the items one for one, in order. The scale stays in the scene's own measure (the pointer turns texels
    // and units into script units itself) with the shrink applied, so at 100 % the pointer sees what it always saw.
    if (VIEW_UNIT == 1.0 && VIEW_SCALE == 1.0 && UI_SCALE == 1.0) return scene;  // the view is the identity
    Scene viewed = view_of(scene);
    Scene out = scene;
    out.ui_u = viewed.ui_u;
    out.ui_ax = viewed.ui_ax / VIEW_UNIT;
    out.ui_ay = viewed.ui_ay / VIEW_UNIT;
    for (size_t i = 0; i < out.items.size() && i < viewed.items.size(); ++i) {
        DrawItem& it = out.items[i];
        const DrawItem& v = viewed.items[i];
        it.centre[0] = v.centre[0] / VIEW_UNIT;
        it.centre[1] = v.centre[1] / VIEW_UNIT;
        it.shrunk = v.shrunk;
        it.ui_ax = v.ui_ax / VIEW_UNIT;
        it.ui_ay = v.ui_ay / VIEW_UNIT;
        if (v.shrunk) {
            // wholly outside the screen (an entry parked beyond the edge): not drawn, so not under the mouse
            const double hw = std::fabs(v.rect[2] * v.scale[0]) / 2, hh = std::fabs(v.rect[3] * v.scale[1]) / 2;
            if (v.centre[0] + hw <= -SCREEN_W / 2.0 || v.centre[0] - hw >= SCREEN_W / 2.0 ||
                v.centre[1] + hh <= -SCREEN_H / 2.0 || v.centre[1] - hh >= SCREEN_H / 2.0)
                it.alpha = 0.0;
            it.scale[0] *= viewed.ui_u;
            it.scale[1] *= viewed.ui_u;
        }
        it.clipped = v.clipped;
        for (int k = 0; k < 4; ++k) it.clip[k] = v.clip[k];
        if (v.clipped) {  // wholly outside its own clip: not drawn, so not under the mouse
            const double x = SCREEN_W / 2.0 + v.centre[0], y = SCREEN_H / 2.0 + v.centre[1];
            const double hw = std::fabs(v.rect[2] * v.scale[0]) / 2, hh = std::fabs(v.rect[3] * v.scale[1]) / 2;
            if (x + hw <= v.clip[0] || x - hw >= v.clip[2] || y + hh <= v.clip[1] || y - hh >= v.clip[3]) it.alpha = 0.0;
        }
    }
    return out;
}

std::vector<uint8_t> Graphics::render_scene(const Scene& scene_in, bool smooth) {
    // AMG_RENDERBENCH=1: milliseconds spent in this call (view transform and drawing), one line per call (F5-7).
    static const bool bench_on = std::getenv("AMG_RENDERBENCH") != nullptr;
    struct Bench {
        bool on;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        size_t items;
        ~Bench() {
            if (on) std::fprintf(stderr, "RENDERBENCH items=%zu ms=%.2f\n", items,
                                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
    } bench{bench_on, std::chrono::steady_clock::now(), scene_in.items.size()};
    bool identity = VIEW_UNIT == 1.0 && VIEW_SCALE == 1.0 && UI_SCALE == 1.0;
    Scene viewed;
    if (!identity) viewed = view_of(scene_in);
    else if (scene_in.hide_uid || scene_in.hide_image) viewed = scene_in;
    drop_hidden(viewed);
    const Scene& scene = (identity && !scene_in.hide_uid && !scene_in.hide_image) ? scene_in : viewed;
    std::vector<uint8_t> frame(size_t(SCREEN_W) * SCREEN_H * 3, 0);
    // Horizontal bands on separate threads: every pixel still receives the objects in the same order,
    // so the picture is identical to a single-threaded render.
    static const int bands = [] {
        const char* e = std::getenv("AMG_BANDS");  // for timing experiments
        int n = e ? std::atoi(e) : 4;
        return n < 1 ? 1 : (n > 8 ? 8 : n);
    }();
    auto paint_band = [&](int ymin, int ymax) {
        for (const DrawItem& item : scene.items) {
            draw(frame, item, smooth, ymin, ymax);
        }
    };
    std::vector<std::thread> workers;
    for (int i = 1; i < bands; ++i) workers.emplace_back(paint_band, SCREEN_H * i / bands, SCREEN_H * (i + 1) / bands);
    paint_band(0, SCREEN_H / bands);
    for (auto& w : workers) w.join();
    return frame;
}

void Graphics::save_png(const std::string& path) const { write_png(path, render(), SCREEN_W, SCREEN_H); }

void Graphics::write_png(const std::string& path, const std::vector<uint8_t>& rgb, int w, int h) {
    std::string raw;
    raw.reserve(rgb.size() + h);
    for (int y = 0; y < h; ++y) {
        raw.push_back('\0');
        raw.append(reinterpret_cast<const char*>(rgb.data()) + size_t(y) * w * 3, size_t(w) * 3);
    }
    std::string out("\x89PNG\r\n\x1a\n", 8);
    std::string ihdr;
    put32be(ihdr, w);
    put32be(ihdr, h);
    ihdr += std::string("\x08\x02\x00\x00\x00", 5);
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", zlib_stored(raw));
    chunk(out, "IEND", "");
    std::ofstream f(path, std::ios::binary);
    f.write(out.data(), std::streamsize(out.size()));
}

std::string Graphics::describe() const {
    std::vector<const ScreenObject*> list;
    for (const auto& [uid, obj] : objects) list.push_back(obj.get());
    std::sort(list.begin(), list.end(), [](const ScreenObject* a, const ScreenObject* b) {
        if (a->layer != b->layer) return a->layer < b->layer;
        return a->order < b->order;
    });
    std::string out;
    char buf[512];
    for (size_t i = 0; i < list.size(); ++i) {
        const ScreenObject* o = list[i];
        auto l = o->light.value();
        auto p = o->pos.value();
        auto z = o->zoom.value();
        std::string name = o->name.empty() ? "(plain)" : o->name;
        std::snprintf(buf, sizeof buf,
                      "layer %3lld uid %4lld %-28s alpha %.2f light %.2f/%.2f/%.2f pos (%.1f, %.1f) zoom (%.2f, %.2f) "
                      "rect (%d, %d, %d, %d)",
                      (long long)o->layer, (long long)o->uid, name.c_str(), o->alpha.value()[0], l[0], l[1], l[2], p[0],
                      p[1], z[0], z[1], int(o->rect[0]), int(o->rect[1]), int(o->rect[2]), int(o->rect[3]));
        if (i) out += "\n";
        out += buf;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------------------------

namespace {

double number(const Value& v) {
    if (v.tag == Tag::Int) return double(v.i);
    if (v.tag == Tag::Float) return v.f;
    return 0.0;
}

int64_t int_of(const Value& v) { return v.tag == Tag::Int ? v.i : 0; }

}  // namespace

void install_graphics(VM& vm) {
    auto gfx = std::make_shared<Graphics>(vm);
    static std::shared_ptr<Graphics> keep;  // lives for the program
    keep = gfx;
    Graphics* g = gfx.get();
    vm.graphics = g;

    auto arg = [](std::vector<Value>& st, size_t b, size_t n) -> Value& { return st.at(b + 1 + n); };
    auto self_iv = [](std::vector<Value>& st, size_t b) -> std::vector<Value>& {
        return static_cast<ObjObj*>(st[b].p.get())->iv;
    };
    auto target = [g, self_iv](std::vector<Value>& st, size_t b) -> ScreenObject* {
        const Value& uid = self_iv(st, b).at(0);
        return uid.tag == Tag::Int ? g->get(uid.i) : nullptr;
    };
    auto prim = [&](const char* cls, int n, PrimFn fn) {
        ClassObj* c = vm.find_class(cls);
        if (!c) throw std::runtime_error(std::string("class missing: ") + cls);
        c->prims[n] = std::move(fn);
    };

    PrimFn update = [g, target, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        ScreenObject* obj = target(st, b);
        auto& iv = self_iv(st, b);
        if (obj) {
            iv.at(1) = Value::integer(int64_t(obj->alpha.remaining()));
            iv.at(2) = Value::integer(int64_t(obj->light.remaining()));
            iv.at(3) = Value::integer(int64_t(obj->pos.remaining()));
            iv.at(4) = Value::integer(int64_t(obj->zoom.remaining()));
        } else {
            for (size_t i = 1; i <= 4; ++i)
                if (iv.at(i).tag != Tag::Int && iv.at(i).tag != Tag::Float) iv[i] = Value::integer(0);
        }
        return st[b];
    };
    // NodeMapIcon: one cell of the action map's hexagon grid (K2R_CObjNodeIcon, 0x19DEC8 to 0x1A26A8).
    // The engine object owns up to five child sprites, all centred on the cell: hexagon base, glow,
    // symbol, marker and an extra. `set(code)` picks their textures (K2R_CObjNodeIcon::setPic,
    // 0x19EA30, read in full; see objects.md). Vita (vtable 0x810C1B0C, read 2026-10-27): `show(x, y, a)` places them at
    // once with `a` as the opacity (the cells at the grid's edge fade out; the zoom stays 1), `open` fades them in over
    // 9 frames linearly and zooms the symbol and the marker from 0 to 1 the same way (the hexagon and the glow do not
    // zoom), `close` fades out and zooms the marker to 0. The marker of the NEW codes sits 10 units lower.
    struct NodeIcon {
        int64_t layer = 0;
        int tex[8] = {0, 0, 0, 0, 0, 0, 0, 0};  // hexagon, glow, symbol (clock: back face), marker, extra; clock long hand, short hand, front face
        int variant[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        int64_t kids[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        double x = 0, y = 0, scale = 1.0;
        double opacity = 1.0;       // the third argument of show (0 to 1)
        bool dim = false;           // symbol at 0.7 of the opacity (states 4 and 5 of the map's own symbols), 0x81093258
        bool sway = false;          // symbol bobbing (states 3 and 5), 0x81095000
        bool visible = false;
        double front_alpha = 0.0;  // clock: opacity of the front face
    };
    auto icons = std::make_shared<std::map<int64_t, NodeIcon>>();
    // The map's clock: every clock child turns together (ClockIcon.setTime sends to the clock type).
    auto clock_value = std::make_shared<double>(0.0);
    auto icon_of = [icons, self_iv](std::vector<Value>& st, size_t b) -> NodeIcon* {
        auto& iv = self_iv(st, b);
        if (iv.at(0).tag != Tag::Int || iv[0].i == 0) return nullptr;
        auto it = icons->find(iv[0].i);
        return it == icons->end() ? nullptr : &it->second;
    };
    auto table_entry = [g](uint32_t addr) -> int { return int(int32_t(g->exe_word(addr))); };
    auto icon_drop = [g](NodeIcon& ic) {
        for (int64_t& k : ic.kids) {
            if (k) g->objects.erase(k);
            k = 0;
        }
    };
    auto clock_hands = [g, clock_value](NodeIcon& ic, double time) {
        // K2R_CObjClock::spin (0x19D440, read): long hand at the value, short hand at value / 12. The
        // hour is -value / 2; it picks the faces from two 24-entry tables (back 0x28A938, front
        // 0x28A998: 0 blue, 1 red, 2 grey) and the front face's opacity by interpolating the table at
        // 0x28A8D8 (dawn and dusk).
        if (!ic.kids[5] && !ic.kids[6]) return;
        auto word = [g](uint32_t addr, bool real) -> double {
            uint32_t w = g->exe_word(addr);
            if (real) {
                float f;
                std::memcpy(&f, &w, 4);
                return f;
            }
            return int32_t(w);
        };
        double v = *clock_value;
        if (ScreenObject* o = ic.kids[5] ? g->get(ic.kids[5]) : nullptr) o->spin.set(0, v, 0, time);
        if (ScreenObject* o = ic.kids[6] ? g->get(ic.kids[6]) : nullptr) o->spin.set(0, v / 12.0, 0, time);
        double hour = -v * 0.5;
        int whole = int(hour);
        double frac = hour - whole;
        int h0 = ((whole % 24) + 24) % 24, h1 = (h0 + 1) % 24;
        double alpha = word(0x28A8D8 + 4 * uint32_t(h0), true) * (1.0 - frac) + word(0x28A8D8 + 4 * uint32_t(h1), true) * frac;
        int back = int(word(0x28A938 + 4 * uint32_t(h0), false)), front = int(word(0x28A998 + 4 * uint32_t(h0), false));
        for (int k : {2, 7}) {
            ScreenObject* o = ic.kids[k] ? g->get(ic.kids[k]) : nullptr;
            if (!o) continue;
            int64_t tmp = g->new_sprite(g->engine_tex(0xAF), k == 2 ? back : front, o->layer);
            if (ScreenObject* t = g->get(tmp)) {
                o->image = t->image;
                o->name = t->name;
            }
            g->objects.erase(tmp);
            if (k == 7) o->light.parts[0].start({1.0, 1.0, 1.0}, 0, 0);
        }
        ic.front_alpha = alpha;
        if (ScreenObject* o = ic.kids[7] ? g->get(ic.kids[7]) : nullptr) o->alpha.parts[0].start({ic.visible ? alpha : 0.0}, 0, 0);
    };
    auto icon_place = [g](NodeIcon& ic, double alpha_time, double zoom_from) {
        for (int i = 0; i < 8; ++i) {
            ScreenObject* o = ic.kids[i] ? g->get(ic.kids[i]) : nullptr;
            if (!o) continue;
            double alpha = ic.visible ? ic.opacity * (i == 1 ? 0.5 : i == 7 ? ic.front_alpha : i == 2 && ic.dim ? 0.7 : 1.0) : 0.0;
            // The NEW marks (marker codes 2 and 3) sit 10 units below the centre (open and show, 0x81093CD4, 0x81093258).
            double dy = (i == 3 && ic.tex[3] >= g->engine_tex(0xAC)) ? 10.0 : 0.0;
            o->pos.parts[0].start({ic.x, ic.y + dy}, 0, 0);
            o->sway_phase = (i == 2 && ic.sway) ? std::max(o->sway_phase, 0.0) : -1.0;
            if (zoom_from > 0 && (i == 2 || i == 3)) {  // opening: the symbol and the marker grow from nothing
                o->zoom.parts[0].start({0.0, 0.0}, 0, 0);
                o->zoom.parts[0].start({ic.scale, ic.scale}, 0, alpha_time);
            } else {
                o->zoom.parts[0].start({ic.scale, ic.scale}, 0, 0);
            }
            if (zoom_from > 0 && alpha_time > 0) o->alpha.parts[0].start({0.0}, 0, 0);
            o->alpha.parts[0].start({alpha}, 0, alpha_time);
        }
    };
    auto icon_build = [g, icon_drop, icon_place, clock_hands](NodeIcon& ic) {
        icon_drop(ic);
        // Inside a layer the last created is drawn first, so the hexagon (created last) ends up at
        // the back and the marker in front.
        for (int i : {6, 5, 7, 4, 3, 2, 1, 0}) {
            if (ic.tex[i] <= 0) continue;
            ic.kids[i] = g->new_sprite(ic.tex[i], ic.variant[i], ic.layer);
            if (ScreenObject* o = g->get(ic.kids[i])) o->alpha.parts[0].start({0.0}, 0, 0);
        }
        icon_place(ic, 0, 0);
        clock_hands(ic, 0);
    };
    auto set_code = [g, table_entry](NodeIcon& ic, int code, int extra) {
        for (int i = 0; i < 8; ++i) ic.tex[i] = ic.variant[i] = 0;
        ic.dim = ic.sway = false;
        bool grid = (code >> 15) & 1;
        int kind = (code >> 12) & 7;
        if (kind == 0) {  // a plain picture: node_flag<n>, and the grid tile when the grid is on
            ic.tex[0] = grid ? g->engine_tex(0x52) : 0;
            if (code & 0xFFF) {
                ic.tex[2] = g->engine_tex(0xAE);
                ic.variant[2] = code & 0xFFF;
            } else {
                // Picture 0 is the clock: setPic builds a K2R_CObjClock child (constructor 0x1908C8).
                // Its parts: face clock%1d (175), long hand (176), short hand (177).
                // Face clock0 (blue), as on the emulator's day-1 map; clock1 (red) and clock2 (grey)
                // are probably other states (hypothesis, not traced yet).
                ic.tex[2] = g->engine_tex(0xAF);  // back face; the variants follow the hour (clock_hands)
                ic.tex[7] = g->engine_tex(0xAF);  // front face
                ic.tex[5] = g->engine_tex(0xB0);
                ic.tex[6] = g->engine_tex(0xB1);
            }
            return;
        }
        int type = (code >> 8) & 15;
        if (type == 0) return;
        int sub = (code >> 5) & 3, state = (code >> 2) & 7, low = code & 3;
        // 0x810929AE: the symbol is dimmed in states 4 and 5 and bobs in states 3 and 5, when the picture is not a flat one,
        // the type is not 3, 4 or 11 and the marker is not 1 (hypothesis: that "not flat" is every code of kind other than 0)
        bool plain = type == 3 || type == 4 || type == 11 || low == 1;
        ic.dim = !plain && (state == 4 || state == 5);
        ic.sway = !plain && (state == 3 || state == 5);
        int s4 = sub, s5 = 0;
        if (type >= 13) {
            s5 = sub + type - 13;
            s4 = 0;
        }
        int row = type == 2 ? 0 : type < 4 ? s4 + 1 : type < 13 ? type : 8;
        bool hex = (code >> 7) & 1;
        uint32_t table = 0x28A9F8;
        if (hex) {
            ic.tex[0] = g->engine_tex(kind + (grid ? 0x52 : 0x59));
            table = 0x28AB98;
            if (state < 6 && low != 1) {
                if (s4 == 2) ic.tex[1] = g->engine_tex(0x61);
                else if (s4 == 1) ic.tex[1] = g->engine_tex(0x62);
            }
        }
        int base = table_entry(table + uint32_t(4 * (row * 8 + state)));
        if (base > 0) {
            int tex = base;
            if (row == 8) tex = base + s5;
            else if (state >= 2 && (row == 7 || row == 9 || row == 11)) tex = base + kind - 1;
            ic.tex[2] = tex;
        }
        if (low) ic.tex[3] = g->engine_tex(low + 0xAA);
        if (extra) ic.tex[4] = g->engine_tex((extra & 3) + 0xB1);
    };
    auto unit = [arg](std::vector<Value>& st, size_t b, size_t n) { return number(arg(st, b, n)) / 10000.0; };
    if (ClassObj* c = vm.find_class("NodeMapIcon")) {
        c->prims[0] = [g, icons, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
            auto& iv = self_iv(st, b);
            if (iv.at(0).tag != Tag::Int || iv[0].i == 0 || !icons->count(iv[0].i)) {
                int64_t id = g->next_uid++;
                NodeIcon ic;
                ic.layer = iv.at(1).tag == Tag::Int ? iv[1].i : 0;
                (*icons)[id] = ic;
                iv[0] = Value::integer(id);
            }
            return st[b];
        };
        c->prims[1] = [icons, icon_drop, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
            auto& iv = self_iv(st, b);
            if (iv.at(0).tag == Tag::Int) {
                auto it = icons->find(iv[0].i);
                if (it != icons->end()) {
                    icon_drop(it->second);
                    icons->erase(it);
                }
            }
            iv[0] = Value::integer(0);
            return st[b];
        };
        c->prims[2] = [icon_of, set_code, icon_build, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
            if (NodeIcon* ic = icon_of(st, b)) {
                const Value& v = arg(st, b, 0);
                set_code(*ic, v.tag == Tag::Int ? int(v.i & 0xFFFF) : 0x1000, v.tag == Tag::Int ? int((v.i >> 16) & 0xFFFF) : 0);  // the high half is the extra (prim 2, 0x10FA60)
                if (std::getenv("AMG_ICON_LOG") && v.tag == Tag::Int && (v.i & 0x7FFF) != 0x1000) std::fprintf(stderr, "icon code %04llx -> %d %d %d %d %d\n", (long long)v.i, ic->tex[0], ic->tex[1], ic->tex[2], ic->tex[3], ic->tex[4]);
                icon_build(*ic);
            }
            return st[b];
        };
        c->prims[3] = [icon_of, icon_place, unit](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
            if (NodeIcon* ic = icon_of(st, b)) {
                ic->x = unit(st, b, 0);
                ic->y = unit(st, b, 1);
                ic->scale = 1.0;
                ic->opacity = 1.0;
                ic->visible = true;
                icon_place(*ic, 9.0, 1.0);
            }
            return st[b];
        };
        c->prims[4] = [g, icon_of, icon_place](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
            if (NodeIcon* ic = icon_of(st, b)) {
                ic->visible = false;
                icon_place(*ic, 9.0, 0);
                if (ScreenObject* o = ic->kids[3] ? g->get(ic->kids[3]) : nullptr) o->zoom.parts[0].start({0.0, 0.0}, 0, 9.0);  // close: the marker shrinks
            }
            return st[b];
        };
        c->prims[5] = [icon_of, icon_place, unit](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
            if (NodeIcon* ic = icon_of(st, b)) {
                ic->x = unit(st, b, 0);
                ic->y = unit(st, b, 1);
                ic->scale = 1.0;
                ic->opacity = std::max(0.0, std::min(1.0, unit(st, b, 2)));  // the third argument is the opacity
                ic->visible = true;
                icon_place(*ic, 0, 0);
            }
            return st[b];
        };
        c->prims[6] = [icon_of, icon_place](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
            if (NodeIcon* ic = icon_of(st, b)) {
                ic->visible = false;
                icon_place(*ic, 0, 0);
            }
            return st[b];
        };
        c->prims[7] = [](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value { return st[b]; };  // empty in the engine
    }
    // ClockIcon.setTime(time, frames) (class primitive 2, 0x110158): turn every clock to the value of
    // the table at 0x28B398 for that time of day.
    if (ClassObj* c = vm.find_class("ClockIcon"))
        c->prims[2] = [g, icons, clock_value, clock_hands, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
            int64_t t = int_of(arg(st, b, 0));
            double frames = number(arg(st, b, 1));
            for (auto& [uid, obj] : g->objects)  // the clock type: the history screen's clocks turn too
                if (dynamic_cast<ClockObject*>(obj.get())) g->set_clock(uid, t, frames);
            if (t >= 0 && t < 14) {
                uint32_t w = g->exe_word(uint32_t(0x28B398 + 4 * t));
                float f = 0;
                std::memcpy(&f, &w, 4);
                *clock_value = double(f);
                for (auto& [id, ic] : *icons) clock_hands(ic, frames);
            }
            return st[b];
        };

    // K2X_Script.resetPrim (called by K2X_Script.reset on load and restart): the engine drops every
    // screen object before the scripts rebuild their scene; without it the old fade layers stay on top.
    if (ClassObj* k2x = vm.find_class("K2X_Script"))
        k2x->prims[0] = [g](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
            g->objects.clear();
            return st[b];
        };
    PrimFn destruct = [g, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        auto& iv = self_iv(st, b);
        if (iv.at(0).tag == Tag::Int) g->objects.erase(iv[0].i);
        iv[0] = Value::integer(0);
        return st[b];
    };
    PrimFn set_layer = [g, target, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        ScreenObject* obj = target(st, b);
        if (obj) {
            obj->layer = int_of(arg(st, b, 0));
            ++g->created;
            obj->order = g->created;
        }
        return st[b];
    };
    PrimFn attach = [g, arg, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        // initialize(uid): what `clone()` calls; the engine's copy constructors build a new object
        // that starts as a copy and then lives its own life.
        self_iv(st, b).at(0) = Value::integer(g->clone(int_of(arg(st, b, 0))));
        return st[b];
    };
    auto animate = [target, arg](int group, size_t count, bool has_component) -> PrimFn {
        return [=](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
            ScreenObject* obj = target(st, b);
            if (obj) {
                std::vector<double> values;
                for (size_t i = 0; i < count; ++i) values.push_back(number(arg(st, b, i)));
                const Value& mode_v = arg(st, b, count);
                int mode = mode_v.tag == Tag::Int ? int(mode_v.i) : -1;
                double time = number(arg(st, b, count + 1));
                int64_t component = 0;
                bool ok = true;
                if (has_component) {
                    const Value& c = arg(st, b, count + 2);
                    ok = c.tag == Tag::Int;
                    component = ok ? c.i : 0;
                }
                if (ok && component >= 0 && component <= 2) {
                    Vib* v = group == 0 ? &obj->alpha : group == 1 ? &obj->light : group == 2 ? &obj->pos : &obj->zoom;
                    v->parts[component].start(values, mode, time);
                }
            }
            return st[b];
        };
    };
    auto vibrate = [target, arg](int group, size_t count, int freq_count = -1) -> PrimFn {
        size_t fc = freq_count < 0 ? count : size_t(freq_count);
        return [=](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
            ScreenObject* obj = target(st, b);
            if (obj) {
                Vib* v = group == 0 ? &obj->alpha : group == 1 ? &obj->light : group == 2 ? &obj->pos : &obj->zoom;
                double time = number(arg(st, b, count + fc + 2));
                std::vector<double> amps;
                for (size_t i = 0; i < count; ++i) amps.push_back(number(arg(st, b, i)));
                const Value& m1 = arg(st, b, count);
                v->parts[1].start(amps, m1.tag == Tag::Int ? int(m1.i) : -1, time);
                std::vector<double> freqs;
                for (size_t i = 0; i < fc; ++i) freqs.push_back(number(arg(st, b, count + 1 + i)));
                if (fc < count) {
                    std::vector<double> rep;
                    for (size_t k = 0; k < count; ++k) rep.insert(rep.end(), freqs.begin(), freqs.end());
                    freqs = rep;
                }
                const Value& m2 = arg(st, b, count + fc + 1);
                v->parts[2].start(freqs, m2.tag == Tag::Int ? int(m2.i) : -1, time);
            }
            return st[b];
        };
    };
    PrimFn spin = [target, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        ScreenObject* obj = target(st, b);
        if (obj) {
            const Value& comp = arg(st, b, 3);
            const Value& mode = arg(st, b, 1);
            if (comp.tag == Tag::Int)
                obj->spin.set(int(comp.i), number(arg(st, b, 0)), mode.tag == Tag::Int ? int(mode.i) : -1,
                              number(arg(st, b, 2)));
        }
        return st[b];
    };
    PrimFn skip = [target](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        ScreenObject* obj = target(st, b);
        if (obj) {
            for (Vib* v : {&obj->alpha, &obj->light, &obj->pos, &obj->zoom})
                for (auto& p : v->parts) p.finish();
        }
        return st[b];
    };
    PrimFn pause = [target, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        ScreenObject* obj = target(st, b);
        if (obj) {
            const Value& v = arg(st, b, 0);
            bool truthy = !(v.tag == Tag::Nil || v.tag == Tag::False || (v.tag == Tag::Int && v.i == 0) ||
                            (v.tag == Tag::Float && v.f == 0.0));
            obj->paused = truthy;
        }
        return st[b];
    };
    PrimFn set_tex_rect = [target, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        ScreenObject* obj = target(st, b);
        if (obj) {
            // In units; a Vita picture has two texels per unit (a plain rectangle is scaled by the view instead).
            double texels = obj->image ? VIEW_UNIT : 1.0;
            for (size_t i = 0; i < 4; ++i) obj->rect[i] = number(arg(st, b, i)) * texels;
        }
        return st[b];
    };

    // --- Sprite --------------------------------------------------------------------------------
    prim("Sprite", 1, [g, arg, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        self_iv(st, b).at(0) =
            Value::integer(g->new_sprite(int_of(arg(st, b, 0)), int_of(arg(st, b, 1)), int_of(arg(st, b, 2))));
        return st[b];
    });
    prim("Sprite", 2, attach);
    prim("Sprite", 3, destruct);
    // setTex(texture, variant): the same picture lookup as the constructor, applied to a live object
    // (NodeMapArea uses it to swap the location thumbnail).
    prim("Sprite", 4, [g, arg, target](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        ScreenObject* obj = target(st, b);
        if (obj) {
            int64_t tmp = g->new_sprite(int_of(arg(st, b, 0)), int_of(arg(st, b, 1)), obj->layer);
            if (ScreenObject* t = g->get(tmp)) {
                obj->image = t->image;
                for (int i = 0; i < 4; ++i) obj->rect[i] = t->rect[i];
                obj->name = t->name;
            }
            g->objects.erase(tmp);
        }
        return st[b];
    });
    prim("Sprite", 5, set_tex_rect);
    prim("Sprite", 6, set_layer);
    prim("Sprite", 7, update);
    prim("Sprite", 8, animate(1, 3, true));
    prim("Sprite", 9, animate(0, 1, true));
    prim("Sprite", 10, animate(2, 2, true));
    prim("Sprite", 11, animate(3, 2, true));
    prim("Sprite", 12, spin);
    prim("Sprite", 13, vibrate(0, 1));
    prim("Sprite", 14, vibrate(1, 3, 1));
    prim("Sprite", 15, vibrate(2, 2));
    prim("Sprite", 16, vibrate(3, 2));
    prim("Sprite", 17, skip);
    prim("Sprite", 18, pause);

    // --- FontChar, FontCharEx, FontChar2 --------------------------------------------------------
    auto char_new = [g, arg, self_iv](int font) -> PrimFn {
        return [=](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
            self_iv(st, b).at(0) = Value::integer(g->new_char(
                font, int_of(arg(st, b, 0)), int_of(arg(st, b, 1)), int_of(arg(st, b, 2)), number(arg(st, b, 3)),
                number(arg(st, b, 4)), number(arg(st, b, 5)), number(arg(st, b, 6))));
            return st[b];
        };
    };
    prim("FontChar", 0, char_new(0));
    prim("FontChar", 1, destruct);
    prim("FontChar", 2, update);
    prim("FontChar", 3, animate(0, 1, false));
    prim("FontChar", 4, animate(2, 2, false));
    prim("FontChar", 5, animate(3, 2, false));
    prim("FontChar", 6, [g, target, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        ScreenObject* obj = target(st, b);
        if (obj) {
            int64_t code = int_of(arg(st, b, 0));
            obj->image = g->glyph(obj->font, vita_font_palette(obj->palette), code);
            if (auto* c = dynamic_cast<CharObject*>(obj); c && c->shadow)
                c->shadow = g->glyph(obj->font, obj->palette < 3 ? 3 : 0, code);
            char buf[32];
            std::snprintf(buf, sizeof buf, "char %04llX", (unsigned long long)code);
            obj->name = buf;
        }
        return st[b];
    });
    prim("FontChar", 7, vibrate(0, 1));
    prim("FontChar", 8, vibrate(2, 2));
    prim("FontChar", 9, vibrate(3, 2));
    prim("FontChar", 10, animate(1, 3, false));
    prim("FontCharEx", 0, char_new(0));
    prim("FontChar2", 0, char_new(1));
    prim("FontChar2", 1, set_layer);

    // --- BG_Picture ----------------------------------------------------------------------------
    prim("BG_Picture", 1, [g, arg, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        self_iv(st, b).at(0) = Value::integer(g->new_background(int_of(arg(st, b, 0)), int_of(arg(st, b, 1)),
                                                                int_of(arg(st, b, 2)), int_of(arg(st, b, 3))));
        return st[b];
    });
    prim("BG_Picture", 2, attach);
    prim("BG_Picture", 3, destruct);
    prim("BG_Picture", 4, update);
    prim("BG_Picture", 6, set_layer);
    prim("BG_Picture", 7, animate(1, 3, false));
    prim("BG_Picture", 8, animate(0, 1, false));
    prim("BG_Picture", 9, animate(2, 2, false));
    prim("BG_Picture", 10, animate(3, 2, false));
    prim("BG_Picture", 11, vibrate(0, 1));
    prim("BG_Picture", 12, vibrate(2, 2));
    prim("BG_Picture", 13, vibrate(3, 2));
    prim("BG_Picture", 14, spin);
    prim("BG_Picture", 15, skip);
    prim("BG_Picture", 16, pause);

    // --- ActorPicture --------------------------------------------------------------------------
    auto actor_of = [target](std::vector<Value>& st, size_t b) -> ActorObject* {
        return dynamic_cast<ActorObject*>(target(st, b));
    };
    auto offset_of = [self_iv](std::vector<Value>& st, size_t b, double out[2]) {
        auto& iv = self_iv(st, b);
        out[0] = number(iv.at(5));
        out[1] = number(iv.at(6));
    };
    // isBlushAct, isSweatAct: the primitive writes the time left of the blush or sweat animation into the object's
    // blushRemain (8) or sweatRemain (7), which the script then compares with 0. No script calls them.
    prim("ActorPicture", 12, [actor_of, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (ActorObject* a = actor_of(st, b)) self_iv(st, b).at(8) = Value::real(a->blush_left);
        return st[b];
    });
    prim("ActorPicture", 14, [actor_of, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (ActorObject* a = actor_of(st, b)) self_iv(st, b).at(7) = Value::real(a->sweat_left);
        return st[b];
    });
    prim("ActorPicture", 0, [g, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        return Value::boolean(g->preload_actor(int_of(arg(st, b, 0)), int_of(arg(st, b, 1)), int_of(arg(st, b, 2))));
    });
    prim("ActorPicture", 1, [g, arg, self_iv, offset_of](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        const Value& source = arg(st, b, 4);
        bool has_source = !(source.tag == Tag::Nil || source.tag == Tag::False || (source.tag == Tag::Int && source.i == 0));
        if (has_source) {
            self_iv(st, b).at(0) = Value::integer(g->clone(int_of(source)));
        } else {
            double off[2];
            offset_of(st, b, off);
            self_iv(st, b).at(0) = Value::integer(g->new_actor(int_of(arg(st, b, 0)), int_of(arg(st, b, 1)),
                                                                int_of(arg(st, b, 2)), int_of(arg(st, b, 3)),
                                                                off[0], off[1]));
        }
        return st[b];
    });
    prim("ActorPicture", 2, destruct);
    prim("ActorPicture", 3, update);
    prim("ActorPicture", 4, [actor_of, arg, offset_of](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (ActorObject* a = actor_of(st, b)) {
            double off[2];
            offset_of(st, b, off);
            a->set_picture(int_of(arg(st, b, 0)), int_of(arg(st, b, 1)), int_of(arg(st, b, 2)), off[0], off[1]);
        }
        return st[b];
    });
    prim("ActorPicture", 5, set_layer);
    prim("ActorPicture", 6, animate(1, 3, false));
    prim("ActorPicture", 7, animate(0, 1, false));
    prim("ActorPicture", 8, animate(2, 2, false));
    prim("ActorPicture", 9, animate(3, 2, false));
    prim("ActorPicture", 10, [actor_of, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (ActorObject* a = actor_of(st, b)) a->set_face(int_of(arg(st, b, 0)), int_of(arg(st, b, 1)));
        return st[b];
    });
    prim("ActorPicture", 11, [actor_of, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (ActorObject* a = actor_of(st, b)) a->set_blush(number(arg(st, b, 0)), number(arg(st, b, 1)));
        return st[b];
    });
    prim("ActorPicture", 13, [actor_of, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (ActorObject* a = actor_of(st, b)) {
            const Value& v = arg(st, b, 0);
            bool on = !(v.tag == Tag::Nil || v.tag == Tag::False || (v.tag == Tag::Int && v.i == 0));
            a->set_sweat(on, number(arg(st, b, 1)));
        }
        return st[b];
    });
    prim("ActorPicture", 15, [actor_of, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (ActorObject* a = actor_of(st, b)) a->set_eye(int(int_of(arg(st, b, 0))), number(arg(st, b, 1)));
        return st[b];
    });
    prim("ActorPicture", 16, [actor_of, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (ActorObject* a = actor_of(st, b)) a->set_mouth(int(int_of(arg(st, b, 0))), number(arg(st, b, 1)));
        return st[b];
    });
    prim("ActorPicture", 17, [actor_of, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (ActorObject* a = actor_of(st, b)) {
            const Value& v = arg(st, b, 0);
            a->lip_sync = !(v.tag == Tag::Nil || v.tag == Tag::False || (v.tag == Tag::Int && v.i == 0));
            if (!a->lip_sync && a->lip_level >= 0) {
                a->lip_level = -1;
                a->refresh();
            }
        }
        return st[b];
    });
    prim("ActorPicture", 18, vibrate(0, 1));
    prim("ActorPicture", 19, vibrate(2, 2));
    prim("ActorPicture", 20, vibrate(3, 2));
    prim("ActorPicture", 21, skip);
    prim("ActorPicture", 22, [actor_of, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (ActorObject* a = actor_of(st, b)) {
            double m[2];
            a->mouth_position(m);
            auto& iv = self_iv(st, b);
            iv.at(9) = Value::real(m[0]);
            iv.at(10) = Value::real(m[1]);
        }
        return st[b];
    });
    prim("ActorPicture", 23, [target](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        ScreenObject* obj = target(st, b);
        return Value::real(obj ? obj->zoom.value()[0] : 1.0);
    });
    prim("ActorPicture", 24, pause);

    // --- ExtraPicture --------------------------------------------------------------------------
    prim("ExtraPicture", 0, [g, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        return Value::boolean(g->preload_extra(int_of(arg(st, b, 0)), int_of(arg(st, b, 1)), int_of(arg(st, b, 2))));
    });
    prim("ExtraPicture", 1, [g, arg, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        const Value& source = arg(st, b, 4);
        bool has_source = !(source.tag == Tag::Nil || source.tag == Tag::False || (source.tag == Tag::Int && source.i == 0));
        if (has_source)
            self_iv(st, b).at(0) = Value::integer(g->clone(int_of(source)));
        else
            self_iv(st, b).at(0) = Value::integer(g->new_extra(int_of(arg(st, b, 0)), int_of(arg(st, b, 1)),
                                                               int_of(arg(st, b, 2)), int_of(arg(st, b, 3))));
        return st[b];
    });
    prim("ExtraPicture", 2, destruct);
    prim("ExtraPicture", 3, update);
    prim("ExtraPicture", 4, [actor_of, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (ActorObject* a = actor_of(st, b))
            a->set_extra(int_of(arg(st, b, 0)), int_of(arg(st, b, 1)), int_of(arg(st, b, 2)));
        return st[b];
    });
    prim("ExtraPicture", 5, set_layer);
    prim("ExtraPicture", 6, animate(1, 3, true));
    prim("ExtraPicture", 7, animate(0, 1, false));
    prim("ExtraPicture", 8, animate(2, 2, false));
    prim("ExtraPicture", 9, animate(3, 2, false));
    prim("ExtraPicture", 10, vibrate(0, 1));
    prim("ExtraPicture", 11, vibrate(2, 2));
    prim("ExtraPicture", 12, vibrate(3, 2));
    prim("ExtraPicture", 13, skip);
    prim("ExtraPicture", 14, pause);

    // --- StructPicture: the memories that drift across the staff roll (Ending) ---------------------
    // K2R_CObjStructPic::setPic2 (0x198208) formats the same `stand/x%c_%c%c` as an extra picture, so the object is
    // one (initialize(size, picture, variant, layer, source) at 0x114F18 has ExtraPicture's arguments); spin
    // (0x1984F0) turns it as Sprite#spin does. APPROXIMATION: the engine draws it through its own sprite string
    // (K2R_CSprStr), not modelled; the picture, placement, fades and turning are the extra picture's.
    if (ClassObj* c = vm.find_class("ExtraPicture")) {
        auto it = c->prims.find(1);
        if (it != c->prims.end()) prim("StructPicture", 0, it->second);
    }
    prim("StructPicture", 1, spin);

    // --- Small ones (2026-10-17, read from the natives at 0x10D988, 0x112530, 0x10F528) -------------------------
    // Sprite.load(texture, variant): the engine loads the picture and answers whether that worked; loaded on demand.
    prim("Sprite", 0, [](VM&, Thread&, std::vector<Value>&, size_t, Method&) -> Value { return Value::boolean(true); });
    // BG_Picture#setPic(place, scene, mob): another background in a live object (as Sprite#setTex).
    prim("BG_Picture", 5, [g, arg, target](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (ScreenObject* obj = target(st, b)) {
            int64_t tmp = g->new_background(int_of(arg(st, b, 0)), int_of(arg(st, b, 1)), int_of(arg(st, b, 2)), obj->layer);
            if (ScreenObject* t = g->get(tmp)) {
                obj->image = t->image;
                for (int i = 0; i < 4; ++i) obj->rect[i] = t->rect[i];
                obj->name = t->name;
            }
            g->objects.erase(tmp);
        }
        return st[b];
    });
    // StandExt(texture, variant, layer, depth): a sprite of the texture table (Breath uses 242, sysgraph/breath%1d)
    // that the engine sorts among the characters by `depth`. APPROXIMATION: the depth sort among characters is not
    // modelled (as for the characters themselves, see objects.md); the puff is drawn on top of its layer.
    prim("StandExt", 0, [g, arg, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        int64_t uid = g->new_sprite(int_of(arg(st, b, 0)), int_of(arg(st, b, 1)), int_of(arg(st, b, 2)));
        // Breath passes the character's depth plus 0.001: in front of that character. Without the depth sort, the
        // puff goes in front of everything in its layer (order 0 is drawn last there); created after the character,
        // it was drawn behind it (reported against PCSX2, 2026-10-17).
        if (ScreenObject* o = g->get(uid)) o->order = 0;
        self_iv(st, b).at(0) = Value::integer(uid);
        return st[b];
    });

    // --- CutInWindow (the mahjong's cut-ins) and ActorPicture#setDrawTarget ----------------------------
    prim("CutInWindow", 0, [g, arg, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        self_iv(st, b).at(0) = Value::integer(g->new_cut_in(int_of(arg(st, b, 0))));
        return st[b];
    });
    prim("CutInWindow", 1, [target, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (auto* w = dynamic_cast<CutInWindowObject*>(target(st, b)))
            for (int i = 0; i < 3; ++i) {
                w->c1[i] = number(arg(st, b, i));
                w->c2[i] = number(arg(st, b, 3 + i));
            }
        return st[b];
    });
    prim("ActorPicture", 25, [target, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (auto* a = dynamic_cast<ActorObject*>(target(st, b))) {
            const Value& v = arg(st, b, 0);
            a->draw_target = v.tag == Tag::Int ? v.i : 0;
        }
        return st[b];
    });

    // --- Balloon: the topic bubbles of the conversation game (WadaiTarget, WadaiIcon) --------------
    prim("Balloon", 0, [](VM&, Thread&, std::vector<Value>&, size_t, Method&) -> Value { return Value::boolean(true); });
    // initialize(category, mark, layer): the third argument is the layer; the second says whether the `?` shows.
    // The Vita's is initialize(mark, layer): no colour category (its balloons are the white ones, category 0).
    prim("Balloon", 1, [g, arg, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        const Value& m = arg(st, b, 0);
        bool mark = !(m.tag == Tag::Nil || m.tag == Tag::False || (m.tag == Tag::Int && m.i == 0));
        int64_t category = 0, layer = int_of(arg(st, b, 1));
        self_iv(st, b).at(0) = Value::integer(g->new_balloon(category, mark, layer));
        return st[b];
    });

    // --- LightBall: the light ball of the conversation topics (WadaiBall) ---------------------------
    // load(n): the engine loads the ball's pictures and answers whether that worked; they are loaded on demand here.
    prim("LightBall", 0, [](VM&, Thread&, std::vector<Value>&, size_t, Method&) -> Value { return Value::boolean(true); });
    // new(layer): the argument is the layer, as the 11 of the Sprite.new(..., 11) beside it in WadaiBall.
    prim("LightBall", 1, [g, arg, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        self_iv(st, b).at(0) = Value::integer(g->new_light_ball(int_of(arg(st, b, 0))));
        return st[b];
    });

    // --- NodeMapNaviBase: the event graph's minimap (shown when the map grid option is on) ------------
    prim("NodeMapNaviBase", 0, [](VM&, Thread&, std::vector<Value>&, size_t, Method&) -> Value { return Value::boolean(true); });
    prim("NodeMapNaviBase", 1, [g, arg, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        self_iv(st, b).at(0) = Value::integer(g->new_navi(int_of(arg(st, b, 0))));
        return st[b];
    });
    prim("NodeMapNaviBase", 2, [g, arg, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        auto& iv = self_iv(st, b);
        if (iv.at(0).tag == Tag::Int) g->navi_mark(iv[0].i, int_of(arg(st, b, 0)), int_of(arg(st, b, 1)));
        return st[b];
    });

    // --- ClockIcon: a clock of its own (the history screen, HistoryIcon) ---------------------------
    prim("ClockIcon", 0, [](VM&, Thread&, std::vector<Value>&, size_t, Method&) -> Value { return Value::boolean(true); });
    prim("ClockIcon", 1, [g, arg, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        // initialize(time, layer); initialize(uid) is clone(), a copy of that clock (objects.md)
        Value a0 = arg(st, b, 0), a1 = arg(st, b, 1);
        if (a1.tag == Tag::Nil && a0.tag == Tag::Int && a0.i > 0 && g->get(a0.i)) {
            auto copy = g->get(a0.i)->clone();
            self_iv(st, b).at(0) = Value::integer(g->add(std::move(copy)));
            return st[b];
        }
        // HistoryIcon calls new() first and initialize(time, layer) afterwards: nothing to build yet, then one clock.
        if (a0.tag == Tag::Nil && a1.tag == Tag::Nil) return st[b];
        auto& iv = self_iv(st, b);
        if (iv.at(0).tag == Tag::Int && iv[0].i > 0) g->objects.erase(iv[0].i);
        int64_t uid = g->new_clock(a1.tag == Tag::Int ? a1.i : 0);
        iv[0] = Value::integer(uid);
        if (a0.tag == Tag::Int) g->set_clock(uid, a0.i, 0);
        return st[b];
    });
    prim("ClockIcon", 3, [g, arg, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        auto& iv = self_iv(st, b);
        if (iv.at(0).tag == Tag::Int) g->set_clock(iv[0].i, int_of(arg(st, b, 0)), number(arg(st, b, 1)));
        return st[b];
    });

    // --- SnowEffect --------------------------------------------------------------
    prim("SnowEffect", 0, [g, arg, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        // initialize(mode, count, d1, d2, d3, d4, size): the flake mode, how many, the box's four distances, the flakes' size
        const int64_t dist[4] = {int64_t(number(arg(st, b, 2))), int64_t(number(arg(st, b, 3))), int64_t(number(arg(st, b, 4))),
                                 int64_t(number(arg(st, b, 5)))};
        self_iv(st, b).at(0) = Value::integer(g->new_snow(int64_t(number(arg(st, b, 1))), 3, dist, number(arg(st, b, 6))));
        return st[b];
    });
    prim("SnowEffect", 1, destruct);
    // setAmount(count, time): the target number of flakes, reached linearly over `time` frames. turnCam(yaw, pitch, mode,
    // time) and moveCam(x, y, z, mode, time) turn and move the camera (the modes are not told apart: every script call
    // uses 0, and the move is linear).
    prim("SnowEffect", 3, [target, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (auto* sn = dynamic_cast<SnowObject*>(target(st, b))) sn->set_amount(number(arg(st, b, 0)), number(arg(st, b, 1)));
        return st[b];
    });
    prim("SnowEffect", 6, [target, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (auto* sn = dynamic_cast<SnowObject*>(target(st, b))) sn->turn(number(arg(st, b, 0)), number(arg(st, b, 1)), number(arg(st, b, 3)));
        return st[b];
    });
    prim("SnowEffect", 7, [target, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (auto* sn = dynamic_cast<SnowObject*>(target(st, b)))
            sn->move(number(arg(st, b, 0)), number(arg(st, b, 1)), number(arg(st, b, 2)), number(arg(st, b, 4)));
        return st[b];
    });
    prim("SnowEffect", 2, set_layer);
    prim("SnowEffect", 4, update);
    prim("SnowEffect", 5, animate(0, 1, false));
    prim("SnowEffect", 8, skip);

    // --- EventPicture (the rest of its methods are BG_Picture's) --------------------------------
    prim("EventPicture", 0, [g, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        return Value::boolean(g->preload_event_picture(int_of(arg(st, b, 0)), int_of(arg(st, b, 1)), int_of(arg(st, b, 2))));
    });
    prim("BG_Picture", 0, [g, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        return Value::boolean(g->preload_background(int_of(arg(st, b, 0)), int_of(arg(st, b, 1)), int_of(arg(st, b, 2))));
    });
    prim("EventPicture", 1, [g, arg, self_iv](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        self_iv(st, b).at(0) = Value::integer(g->new_event_picture(int_of(arg(st, b, 0)), int_of(arg(st, b, 1)),
                                                                   int_of(arg(st, b, 2)), int_of(arg(st, b, 3))));
        return st[b];
    });

    // setMouth(character, mode, time) and lipSync(character, on): the script calls them only when the character is
    // this picture's (EventPicture#setMouth); a picture without a mouth sheet ignores them, as the engine's base class.
    prim("EventPicture", 2, [target, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (auto* p = dynamic_cast<EventPicExObject*>(target(st, b))) {
            p->character = int_of(arg(st, b, 0));
            p->set_mouth(int(int_of(arg(st, b, 1))), number(arg(st, b, 2)));
        }
        return st[b];
    });
    prim("EventPicture", 3, [target, arg](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (auto* p = dynamic_cast<EventPicExObject*>(target(st, b))) {
            p->character = int_of(arg(st, b, 0));
            const Value& v = arg(st, b, 1);
            p->lip_sync = !(v.tag == Tag::Nil || v.tag == Tag::False || (v.tag == Tag::Int && v.i == 0));
        }
        return st[b];
    });

    // PS Vita system image: ExtraPicture gained spin (13), which moves skip and pause to 14 and 15; the new
    // GalleryPicture has ExtraPicture's methods with the same numbers (docs/technical/vita-cpk-format.md).
    {
        prim("ExtraPicture", 13, spin);
        prim("ExtraPicture", 14, skip);
        prim("ExtraPicture", 15, pause);
        ClassObj* extra = vm.find_class("ExtraPicture");
        ClassObj* gallery = vm.find_class("GalleryPicture");
        if (extra && gallery)
            for (const auto& [n, fn] : extra->prims) gallery->prims[n] = fn;
    }

    vm.on_tick = [g](VM&) { g->step(1.0); };
}

}  // namespace amg
