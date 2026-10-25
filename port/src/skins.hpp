// The button pictures of our own art (tools/hokago.py make_button_art), loaded for the glyph swap (docs/technical/input.md).
//
// `<dir>/<family>/<name>.png`, drawn at four times the size of the game's own button sprites (112x112 for 28x28, 224x112 for
// 56x28); each is reduced by four, with its alpha on the engine's scale (128 is opaque), so that it takes the place of the
// original where the scripts put it. Keyboard keys are made on demand: the key's picture with the label written on it.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "gfx.hpp"

namespace amg {

class Skins {
public:
    // Loads every PNG under `dir`'s family folders (ps, xbox, kbd) and `dir/keycap.ttf`, the font of the keys' labels.
    // The number of pictures, 0 if the folder is not there.
    size_t load(const std::string& dir);
    // `family` is a folder name ("ps", "xbox", "kbd"), `name` a file without its extension; null if there is none.
    ImagePtr get(const std::string& family, const std::string& name) const;
    // A key of the keyboard: `text` written on it, and/or the picture `symbol` (a kbd/sym_* name) over it; `wide` for the
    // long key (the game's L and R are 56 units wide). Made once, then kept. Null without the keyboard pictures.
    ImagePtr keycap(const std::string& text, const std::string& symbol, bool wide) const;

private:
    struct Source {
        int w = 0, h = 0;
        std::vector<unsigned char> rgba;
    };
    std::map<std::string, ImagePtr> images_;
    std::map<std::string, Source> sources_;  // the keyboard pictures at their drawn size, to write labels on
    std::vector<unsigned char> font_;
    mutable std::map<std::string, ImagePtr> keycaps_;
};

// One PNG at its own size, alpha on the engine's scale (the splash screen); null if it cannot be read.
ImagePtr load_picture(const std::string& path);

}  // namespace amg
