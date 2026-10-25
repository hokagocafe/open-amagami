// CRI HCA (the PS Vita release's voices) to 16-bit PCM, through the vendored clHCA decoder
// (third_party/clhca). Unencrypted files only: this game's voices carry no key (ciph type 0,
// docs/technical/vita-cpk-format.md).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace amg {

// Decodes a whole file to interleaved samples, without the encoder's delay and padding.
// False if it is not an HCA file or a block does not decode.
bool decode_hca(const std::string& data, std::vector<int16_t>& pcm, int& rate, int& channels);

}  // namespace amg
