// Decoder for CRI AHX voice files (MPEG-2 audio layer II in an ADX-style container).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace amg {

// Decodes the whole file to signed 16-bit mono samples. `rate` receives the sample rate from the
// header (24,000 Hz for the game's voices). Returns false if the data is not an AHX file.
bool decode_ahx(const std::string& data, std::vector<int16_t>& pcm, int& rate);

}  // namespace amg
