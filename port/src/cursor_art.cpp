#include "cursor_art.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace amg::cursor_art {

std::vector<uint8_t> arrow_rgba(int size);  // the drawn arrow, until the game's is read: the tip at (1, 1)

namespace {

// The arrow's outline, in a unit square (the tip at the origin): a classic pointer with a short tail.
constexpr double SHAPE[][2] = {{0.00, 0.00}, {0.00, 0.80}, {0.20, 0.63}, {0.34, 0.95}, {0.47, 0.89},
                               {0.33, 0.58}, {0.58, 0.58}};
constexpr int N = sizeof SHAPE / sizeof SHAPE[0];

bool inside(double x, double y) {
    bool in = false;
    for (int i = 0, j = N - 1; i < N; j = i++) {
        const double xi = SHAPE[i][0], yi = SHAPE[i][1], xj = SHAPE[j][0], yj = SHAPE[j][1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
}

double edge_distance(double x, double y) {
    double best = 1e9;
    for (int i = 0, j = N - 1; i < N; j = i++) {
        const double ax = SHAPE[j][0], ay = SHAPE[j][1], bx = SHAPE[i][0], by = SHAPE[i][1];
        const double dx = bx - ax, dy = by - ay;
        double t = ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy);
        t = std::clamp(t, 0.0, 1.0);
        best = std::min(best, std::hypot(x - ax - t * dx, y - ay - t * dy));
    }
    return best;
}

}  // namespace

int size_for(int view_h) { return std::max(16, int(std::lround(view_h * 24.0 / 544.0))); }

namespace {

std::mutex source_mutex;
int src_w = 0, src_h = 0;
std::vector<uint8_t> src_px;  // premultiplied RGBA, alpha 0..255

// The game's arrow turned so its tip points up-left (225 degrees, y down), cropped to its opaque part, fitted in
// `size` by `size` with a pixel of margin; the hot spot is the opaque pixel farthest up-left (least x + y).
bool game_arrow(int size, Art& out) {
    std::lock_guard<std::mutex> lk(source_mutex);
    if (src_px.empty()) return false;
    const double c = std::cos(5 * 3.14159265358979 / 4), s = std::sin(5 * 3.14159265358979 / 4);
    const int D = int(std::ceil(std::hypot(src_w, src_h))) + 2;
    auto sample_src = [&](double x, double y, double p[4]) {  // bilinear, premultiplied, outside is clear
        x -= 0.5;
        y -= 0.5;
        const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        const double fx = x - x0, fy = y - y0;
        for (int k = 0; k < 4; ++k) p[k] = 0;
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const int xx = x0 + i, yy = y0 + j;
                if (xx < 0 || yy < 0 || xx >= src_w || yy >= src_h) continue;
                const double wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                const uint8_t* q = &src_px[(size_t(yy) * src_w + xx) * 4];
                for (int k = 0; k < 4; ++k) p[k] += wgt * q[k];
            }
    };
    // canvas point (u, v), centred, back to the source: the inverse turn
    auto sample = [&](double u, double v, double p[4]) {
        const double x = c * u + s * v, y = -s * u + c * v;
        sample_src(x + src_w / 2.0, y + src_h / 2.0, p);
    };
    // the opaque part of the turned arrow, on the canvas
    double bx0 = 1e9, by0 = 1e9, bx1 = -1e9, by1 = -1e9, tip = 1e9, tip_x = 0, tip_y = 0;
    for (int y = 0; y < D; ++y)
        for (int x = 0; x < D; ++x) {
            double p[4];
            const double u = x + 0.5 - D / 2.0, v = y + 0.5 - D / 2.0;
            sample(u, v, p);
            if (p[3] < 24) continue;
            bx0 = std::min(bx0, u - 0.5);
            by0 = std::min(by0, v - 0.5);
            bx1 = std::max(bx1, u + 0.5);
            by1 = std::max(by1, v + 0.5);
            if (p[3] >= 96 && u + v < tip) {
                tip = u + v;
                tip_x = u;
                tip_y = v;
            }
        }
    if (bx1 <= bx0 || by1 <= by0) return false;
    const double k = (size - 2) / std::max(bx1 - bx0, by1 - by0);  // canvas units per output pixel: 1 / k
    out.size = size;
    out.rgba.assign(size_t(size) * size * 4, 0);
    constexpr int SS = 4;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            double acc[4] = {0, 0, 0, 0};
            for (int sy = 0; sy < SS; ++sy)
                for (int sx = 0; sx < SS; ++sx) {
                    double p[4];
                    sample(bx0 + (x - 1 + (sx + 0.5) / SS) / k, by0 + (y - 1 + (sy + 0.5) / SS) / k, p);
                    for (int i = 0; i < 4; ++i) acc[i] += p[i];
                }
            uint8_t* d = &out.rgba[(size_t(y) * size + x) * 4];
            const double a = acc[3] / (SS * SS);
            d[3] = uint8_t(std::lround(std::min(255.0, a)));
            for (int i = 0; i < 3; ++i) d[i] = a > 0 ? uint8_t(std::lround(std::min(255.0, acc[i] / acc[3] * 255.0))) : 0;
        }
    out.hot_x = std::clamp(int(std::floor(1 + (tip_x - bx0) * k)), 0, size - 1);
    out.hot_y = std::clamp(int(std::floor(1 + (tip_y - by0) * k)), 0, size - 1);
    return true;
}

}  // namespace

bool set_source(int w, int h, const uint8_t* rgba) {
    if (w <= 0 || h <= 0 || !rgba) return false;
    std::lock_guard<std::mutex> lk(source_mutex);
    if (!src_px.empty()) return false;
    src_w = w;
    src_h = h;
    src_px.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        const unsigned a = std::min(255u, unsigned(rgba[i * 4 + 3]) * 255u / 128u);
        for (int k = 0; k < 3; ++k) src_px[i * 4 + k] = uint8_t(rgba[i * 4 + k] * a / 255);
        src_px[i * 4 + 3] = uint8_t(a);
    }
    return true;
}

Art make(int size) {
    Art art;
    if (game_arrow(size, art)) return art;
    art.size = size;
    art.rgba = arrow_rgba(size);
    return art;
}

std::vector<uint8_t> arrow_rgba(int size) {
    std::vector<uint8_t> px(size_t(size) * size * 4, 0);
    const double stroke = std::max(1.2, size / 14.0) / size;  // the outline, in units of the square
    const double margin = 1.0 / size;                          // the tip one pixel in, so the outline is whole
    const double scale = 1.0 - 2 * margin;
    constexpr int SS = 4;  // 4 x 4 samples a pixel
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            int fill = 0, line = 0;
            for (int sy = 0; sy < SS; ++sy)
                for (int sx = 0; sx < SS; ++sx) {
                    const double u = ((x + (sx + 0.5) / SS) / size - margin) / scale;
                    const double v = ((y + (sy + 0.5) / SS) / size - margin) / scale;
                    const bool in = inside(u, v);
                    const double d = edge_distance(u, v);
                    if (in && d > stroke) ++fill;
                    else if (in || d <= stroke * 0.6) ++line;
                }
            const int total = fill + line;
            if (!total) continue;
            // White with a faint warm tint, a dark plum outline: the colours of the game's own interface text.
            const double f = double(fill) / total;
            uint8_t* p = &px[(size_t(y) * size + x) * 4];
            p[0] = uint8_t(std::lround(255 * f + 72 * (1 - f)));
            p[1] = uint8_t(std::lround(250 * f + 36 * (1 - f)));
            p[2] = uint8_t(std::lround(252 * f + 56 * (1 - f)));
            p[3] = uint8_t(std::lround(255.0 * total / (SS * SS)));
        }
    return px;
}

void stamp(std::vector<uint8_t>& rgb, int w, int h, int x, int y, int size) {
    const Art made = make(size);
    const std::vector<uint8_t>& art = made.rgba;
    for (int j = 0; j < size; ++j)
        for (int i = 0; i < size; ++i) {
            const int px = x - made.hot_x + i, py = y - made.hot_y + j;
            if (px < 0 || py < 0 || px >= w || py >= h) continue;
            const uint8_t* a = &art[(size_t(j) * size + i) * 4];
            if (!a[3]) continue;
            uint8_t* d = &rgb[(size_t(py) * w + px) * 3];
            for (int c = 0; c < 3; ++c) d[c] = uint8_t((a[c] * a[3] + d[c] * (255 - a[3]) + 127) / 255);
        }
}

bool in_shots() {
    const char* e = std::getenv("AMG_MOUSE_CURSOR_IN_SHOTS");
    return e && std::strcmp(e, "1") == 0;
}

}  // namespace amg::cursor_art
