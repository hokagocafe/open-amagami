#include "skins.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "third_party/stb/stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb/stb_truetype.h"

namespace amg {

namespace {

// A picture reduced by `k` in each direction with a box filter, on premultiplied colour so that the edges do not darken;
// the alpha goes to the engine's scale (255 to 128).
ImagePtr reduce(const unsigned char* rgba, int w, int h, int k) {
    auto img = std::make_shared<Image>();
    img->w = w / k;
    img->h = h / k;
    img->px.assign(size_t(img->w) * size_t(img->h) * 4, 0);
    for (int y = 0; y < img->h; ++y)
        for (int x = 0; x < img->w; ++x) {
            double r = 0, g = 0, b = 0, a = 0;
            for (int j = 0; j < k; ++j)
                for (int i = 0; i < k; ++i) {
                    const unsigned char* p = rgba + (size_t(y * k + j) * size_t(w) + size_t(x * k + i)) * 4;
                    double pa = p[3] / 255.0;
                    r += p[0] * pa;
                    g += p[1] * pa;
                    b += p[2] * pa;
                    a += pa;
                }
            double n = double(k * k);
            uint8_t* o = &img->px[(size_t(y) * size_t(img->w) + size_t(x)) * 4];
            if (a > 0) {
                o[0] = uint8_t(r / a + 0.5);
                o[1] = uint8_t(g / a + 0.5);
                o[2] = uint8_t(b / a + 0.5);
            }
            o[3] = uint8_t((a / n) * 128.0 + 0.5);  // 128 is opaque in the engine's pictures
        }
    return img;
}

std::vector<unsigned char> slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// `over` (straight alpha) laid on `base` (same size).
void composite(std::vector<unsigned char>& base, const std::vector<unsigned char>& over) {
    for (size_t i = 0; i + 3 < base.size() && i + 3 < over.size(); i += 4) {
        const double a = over[i + 3] / 255.0;
        for (int c = 0; c < 3; ++c) base[i + size_t(c)] = (unsigned char)(base[i + size_t(c)] * (1 - a) + over[i + size_t(c)] * a + 0.5);
    }
}

}  // namespace

size_t Skins::load(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    images_.clear();
    sources_.clear();
    keycaps_.clear();
    font_ = slurp(fs::path(dir) / "keycap.ttf");
    for (const char* family : {"ps", "xbox", "kbd"}) {
        fs::path folder = fs::path(dir) / family;
        if (!fs::is_directory(folder, ec)) continue;
        for (const auto& e : fs::directory_iterator(folder, ec)) {
            if (!e.is_regular_file(ec) || e.path().extension() != ".png") continue;
            std::vector<unsigned char> file = slurp(e.path());
            int w = 0, h = 0, n = 0;
            unsigned char* rgba = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &n, 4);
            if (!rgba) continue;
            const std::string name = std::string(family) + "/" + e.path().stem().string();
            images_[name] = reduce(rgba, w, h, 4);
            if (std::string(family) == "kbd") {
                Source s;
                s.w = w;
                s.h = h;
                s.rgba.assign(rgba, rgba + size_t(w) * size_t(h) * 4);
                sources_[name] = std::move(s);
            }
            stbi_image_free(rgba);
        }
    }
    return images_.size();
}

ImagePtr load_picture(const std::string& path) {
    std::vector<unsigned char> file;
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return nullptr;
        file.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    int w = 0, h = 0, n = 0;
    unsigned char* rgba = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &n, 4);
    if (!rgba) return nullptr;
    ImagePtr img = reduce(rgba, w, h, 1);
    stbi_image_free(rgba);
    return img;
}

ImagePtr Skins::get(const std::string& family, const std::string& name) const {
    auto it = images_.find(family + "/" + name);
    return it == images_.end() ? nullptr : it->second;
}

ImagePtr Skins::keycap(const std::string& text, const std::string& symbol, bool wide) const {
    const std::string key = text + "|" + symbol + "|" + (wide ? "w" : "n");
    auto cached = keycaps_.find(key);
    if (cached != keycaps_.end()) return cached->second;
    auto base = sources_.find(wide ? "kbd/key_wide" : "kbd/key");
    if (base == sources_.end()) return nullptr;
    std::vector<unsigned char> rgba = base->second.rgba;
    const int w = base->second.w, h = base->second.h;
    if (!symbol.empty()) {  // a picture over the key (arrows, Enter...), centred as drawn
        auto sym = sources_.find("kbd/sym_" + symbol);
        if (sym != sources_.end() && sym->second.w == 112 && sym->second.h == 112) {
            std::vector<unsigned char> placed(rgba.size(), 0);  // the 112x112 picture in the middle of the key
            const int x0 = (w - 112) / 2;
            for (int y = 0; y < 112; ++y)
                std::copy_n(&sym->second.rgba[size_t(y) * 112 * 4], 112 * 4, &placed[(size_t(y) * size_t(w) + size_t(x0)) * 4]);
            composite(rgba, placed);
        }
    }
    stbtt_fontinfo font;
    if (!text.empty() && !font_.empty() && stbtt_InitFont(&font, font_.data(), 0)) {
        // Letters about 58 units tall at the drawn size (14 at the game's), their box centred a little above the key's middle (the
        // face is higher than the body's lip).
        const float scale = stbtt_ScaleForPixelHeight(&font, 80.0f);
        std::vector<unsigned char> layer(rgba.size(), 0);
        int total = 0, top = 0, bottom = 0;
        for (char ch : text) {
            int x0, y0, x1, y1, adv, lsb;
            stbtt_GetCodepointBitmapBox(&font, ch, scale, scale, &x0, &y0, &x1, &y1);
            stbtt_GetCodepointHMetrics(&font, ch, &adv, &lsb);
            total += int(adv * scale);
            top = std::min(top, y0);
            bottom = std::max(bottom, y1);
        }
        int pen = (w - total) / 2;
        const int baseline = h / 2 - 6 - (top + bottom) / 2;
        for (char ch : text) {
            int gw, gh, xo, yo;
            unsigned char* bm = stbtt_GetCodepointBitmap(&font, scale, scale, ch, &gw, &gh, &xo, &yo);
            for (int y = 0; y < gh; ++y)
                for (int x = 0; x < gw; ++x) {
                    const int px = pen + xo + x, py = baseline + yo + y;
                    if (px < 0 || px >= w || py < 0 || py >= h) continue;
                    unsigned char* o = &layer[(size_t(py) * size_t(w) + size_t(px)) * 4];
                    o[0] = o[1] = o[2] = 244;
                    o[3] = std::max(o[3], bm[size_t(y) * size_t(gw) + size_t(x)]);
                }
            stbtt_FreeBitmap(bm, nullptr);
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&font, ch, &adv, &lsb);
            pen += int(adv * scale);
        }
        composite(rgba, layer);
    }
    ImagePtr img = reduce(rgba.data(), w, h, 4);
    keycaps_[key] = img;
    return img;
}

}  // namespace amg
