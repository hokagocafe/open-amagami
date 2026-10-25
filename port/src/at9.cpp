#include "at9.hpp"

#include <algorithm>
#include <cstring>

extern "C" {
#include "third_party/libatrac9/libatrac9.h"
}

namespace amg {

namespace {

uint32_t le32(const std::string& d, size_t at) {
    return uint32_t(uint8_t(d[at])) | (uint32_t(uint8_t(d[at + 1])) << 8) | (uint32_t(uint8_t(d[at + 2])) << 16) |
           (uint32_t(uint8_t(d[at + 3])) << 24);
}

}  // namespace

bool decode_atrac9_riff(const std::string& file, Atrac9Result& out) {
    if (file.size() < 12 || file.compare(0, 4, "RIFF") != 0 || file.compare(8, 4, "WAVE") != 0) return false;
    unsigned char config[ATRAC9_CONFIG_DATA_SIZE] = {};
    bool have_config = false;
    size_t data_at = 0, data_size = 0;
    uint32_t fact_samples = 0, fact_delay = 0;
    size_t pos = 12;
    while (pos + 8 <= file.size()) {
        uint32_t size = le32(file, pos + 4);
        size_t body = pos + 8;
        if (body + size > file.size()) size = uint32_t(file.size() - body);
        if (file.compare(pos, 4, "fmt ") == 0 && size >= 48) {
            // WAVE_FORMAT_EXTENSIBLE with the ATRAC9 sub-format: after the 40-byte header come a version word and the
            // four bytes of codec configuration.
            std::memcpy(config, file.data() + body + 44, ATRAC9_CONFIG_DATA_SIZE);
            have_config = true;
        } else if (file.compare(pos, 4, "fact") == 0 && size >= 12) {
            fact_samples = le32(file, body);
            fact_delay = le32(file, body + 8);
        } else if (file.compare(pos, 4, "smpl") == 0 && size >= 36 + 24 && le32(file, body + 28) >= 1) {
            out.looped = true;
            out.loop_start = le32(file, body + 36 + 8);
            out.loop_end = le32(file, body + 36 + 12);
        } else if (file.compare(pos, 4, "data") == 0) {
            data_at = body;
            data_size = size;
        }
        pos = body + size + (size & 1);
    }
    if (!have_config || !data_size) return false;

    void* handle = Atrac9GetHandle();
    if (!handle) return false;
    Atrac9CodecInfo info;
    if (Atrac9InitDecoder(handle, config) < 0 || Atrac9GetCodecInfo(handle, &info) < 0 || info.superframeSize <= 0 ||
        info.frameSamples <= 0 || info.channels <= 0) {
        Atrac9ReleaseHandle(handle);
        return false;
    }
    out.channels = info.channels;
    out.rate = info.samplingRate;
    const size_t frame_shorts = size_t(info.frameSamples) * size_t(info.channels);
    std::vector<int16_t> frame(frame_shorts);
    const unsigned char* base = reinterpret_cast<const unsigned char*>(file.data()) + data_at;
    out.pcm.clear();
    for (size_t sf = 0; sf + size_t(info.superframeSize) <= data_size; sf += size_t(info.superframeSize)) {
        const unsigned char* p = base + sf;
        for (int i = 0; i < info.framesInSuperframe; ++i) {
            int used = 0;
            if (Atrac9Decode(handle, p, frame.data(), &used) < 0) break;
            p += used;
            out.pcm.insert(out.pcm.end(), frame.begin(), frame.end());
        }
    }
    Atrac9ReleaseHandle(handle);
    // LibAtrac9's output is ffmpeg's with the opposite polarity (same samples within one level over a whole track,
    // checked 2026-10-22); negated so that it matches what the converted files had.
    for (int16_t& s : out.pcm) s = int16_t(s == -32768 ? 32767 : -s);
    (void)fact_samples;
    (void)fact_delay;
    return !out.pcm.empty();
}

}  // namespace amg
