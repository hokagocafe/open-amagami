// The PS Vita release's music and ambience: ATRAC9 in RIFF files (MUSIC.CPK, SOUND.CPK), decoded with the vendored
// LibAtrac9 (third_party/libatrac9). The result is what `tools/vita_audio.py` made with ffmpeg: 16-bit PCM, and the
// loop points of the file's `smpl` chunk.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace amg {

struct Atrac9Result {
    std::vector<int16_t> pcm;  // interleaved
    int channels = 0, rate = 0;
    bool looped = false;
    uint32_t loop_start = 0, loop_end = 0;  // in sample frames
};

bool decode_atrac9_riff(const std::string& file, Atrac9Result& out);

}  // namespace amg
