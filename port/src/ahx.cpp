// CRI AHX voices: MPEG-2 audio layer II, one channel, 24 kHz, wrapped in an ADX-style header and
// with frames of different lengths (docs/technical/audio.md). The frame decoding follows
// ISO/IEC 13818-3; the constant tables and the 32-point transform in the two .inc files are from
// PL_MPEG (MIT), see their headers.
#include "ahx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace amg {

namespace {

#include "mpa_tables.inc"
#include "mpa_idct.inc"

struct BitReader {
    const uint8_t* data;
    size_t size;  // in bytes
    size_t bit = 0;

    int read(int count) {
        int value = 0;
        while (count-- > 0) {
            size_t byte = bit >> 3;
            int b = byte < size ? (data[byte] >> (7 - (bit & 7))) & 1 : 0;
            value = (value << 1) | b;
            ++bit;
        }
        return value;
    }
    size_t bytes_used() const { return (bit + 7) >> 3; }
    bool overrun() const { return (bit >> 3) > size; }
};

struct MpaState {
    float D[1024];
    float V[1024];
    int v_pos = 0;
    MpaState() {
        std::memcpy(D, PLM_AUDIO_SYNTHESIS_WINDOW, 512 * sizeof(float));
        std::memcpy(D + 512, PLM_AUDIO_SYNTHESIS_WINDOW, 512 * sizeof(float));
        std::memset(V, 0, sizeof V);
    }
};

// One layer II frame of a mono stream at a low sampling frequency (MPEG-2): the allocation table
// of ISO 13818-3 (30 subbands). Appends 1152 samples to `out`.
// Files with the encryption flag (header byte 0x13 = 8) have the scale factor selection of
// subbands 1 to 8 XORed with a 16-bit mask chosen by the selection of subband 0 (found by fitting
// frame sizes over 41,715 frames; masks for this game, hypothesis that they derive from the key).
const uint16_t SCFSI_MASK[4] = {0x0000, 0xD4A5, 0xF605, 0x7FE5};

bool decode_frame(MpaState& st, BitReader& br, bool encrypted, std::vector<float>& out) {
    const int sblimit = 30;
    const plm_quantizer_spec_t* allocation[32] = {};
    int scale_info[32] = {};
    int scale_factor[32][3] = {};
    int sample[32][3] = {};

    for (int sb = 0; sb < sblimit; ++sb) {
        int tab4 = PLM_AUDIO_QUANT_LUT_STEP_3[2][sb];
        int qtab = PLM_AUDIO_QUANT_LUT_STEP_4[tab4 & 15][br.read(tab4 >> 4)];
        allocation[sb] = qtab ? &PLM_AUDIO_QUANT_TAB[qtab - 1] : nullptr;
    }
    for (int sb = 0; sb < sblimit; ++sb)
        if (allocation[sb]) scale_info[sb] = br.read(2);
    if (encrypted)
        for (int sb = 1; sb <= 8; ++sb) scale_info[sb] ^= (SCFSI_MASK[scale_info[0]] >> (2 * (8 - sb))) & 3;
    for (int sb = 0; sb < sblimit; ++sb) {
        if (!allocation[sb]) continue;
        int* sf = scale_factor[sb];
        switch (scale_info[sb]) {
            case 0: sf[0] = br.read(6); sf[1] = br.read(6); sf[2] = br.read(6); break;
            case 1: sf[0] = sf[1] = br.read(6); sf[2] = br.read(6); break;
            case 2: sf[0] = sf[1] = sf[2] = br.read(6); break;
            default: sf[0] = br.read(6); sf[1] = sf[2] = br.read(6); break;
        }
    }

    float U[32];
    for (int part = 0; part < 3; ++part) {
        for (int granule = 0; granule < 4; ++granule) {
            for (int sb = 0; sb < sblimit; ++sb) {
                const plm_quantizer_spec_t* q = allocation[sb];
                int* s = sample[sb];
                if (!q) {
                    s[0] = s[1] = s[2] = 0;
                    continue;
                }
                int sf = scale_factor[sb][part];
                if (sf == 63) {
                    sf = 0;
                } else {
                    int shift = sf / 3;
                    sf = (PLM_AUDIO_SCALEFACTOR_BASE[sf % 3] + ((1 << shift) >> 1)) >> shift;
                }
                int adj = q->levels;
                if (q->group) {  // three samples share one code
                    int val = br.read(q->bits);
                    s[0] = val % adj;
                    val /= adj;
                    s[1] = val % adj;
                    s[2] = val / adj;
                } else {
                    s[0] = br.read(q->bits);
                    s[1] = br.read(q->bits);
                    s[2] = br.read(q->bits);
                }
                int scale = 65536 / (adj + 1);
                adj = ((adj + 1) >> 1) - 1;
                for (int k = 0; k < 3; ++k) {
                    int val = (adj - s[k]) * scale;
                    s[k] = (val * (sf >> 12) + ((val * (sf & 4095) + 2048) >> 12)) >> 12;
                }
            }
            for (int sb = sblimit; sb < 32; ++sb) sample[sb][0] = sample[sb][1] = sample[sb][2] = 0;

            for (int p = 0; p < 3; ++p) {  // synthesis polyphase filter bank
                st.v_pos = (st.v_pos - 64) & 1023;
                plm_audio_idct36(sample, p, st.V, st.v_pos);
                std::memset(U, 0, sizeof U);
                int d_index = 512 - (st.v_pos >> 1);
                int v_index = (st.v_pos % 128) >> 1;
                while (v_index < 1024) {
                    for (int i = 0; i < 32; ++i) U[i] += st.D[d_index++] * st.V[v_index++];
                    v_index += 128 - 32;
                    d_index += 64 - 32;
                }
                d_index -= (512 - 32);
                v_index = (128 - 32 + 1024) - v_index;
                while (v_index < 1024) {
                    for (int i = 0; i < 32; ++i) U[i] += st.D[d_index++] * st.V[v_index++];
                    v_index += 128 - 32;
                    d_index += 64 - 32;
                }
                for (int j = 0; j < 32; ++j) out.push_back(U[j] / -1090519040.0f);
            }
        }
    }
    return !br.overrun();
}

uint32_t be32(const std::string& d, size_t at) {
    return (uint32_t(uint8_t(d[at])) << 24) | (uint32_t(uint8_t(d[at + 1])) << 16) | (uint32_t(uint8_t(d[at + 2])) << 8) |
           uint32_t(uint8_t(d[at + 3]));
}

}  // namespace

bool decode_ahx(const std::string& data, std::vector<int16_t>& pcm, int& rate) {
    if (data.size() < 0x30 || uint8_t(data[0]) != 0x80 || data[1] != 0) return false;
    size_t pos = size_t((uint8_t(data[2]) << 8) | uint8_t(data[3])) + 4;
    int encoding = uint8_t(data[4]);
    int channels = uint8_t(data[7]);
    rate = int(be32(data, 8));
    uint32_t total = be32(data, 12);
    if ((encoding != 0x10 && encoding != 0x11) || channels != 1 || pos + 4 > data.size()) return false;

    bool encrypted = uint8_t(data[0x13]) == 8;
    MpaState st;
    std::vector<float> frame;
    pcm.clear();
    pcm.reserve(total + 1152);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data.data());
    while (pcm.size() < total && pos + 4 <= data.size()) {
        if (bytes[pos] == 0x80 && bytes[pos + 1] == 0x01) break;  // end marker
        // Every frame starts with an MPEG header: 11 sync bits, version, layer 2, no CRC.
        if (!(bytes[pos] == 0xFF && (bytes[pos + 1] & 0xF0) == 0xF0)) {
            size_t next = pos + 1;
            while (next + 4 <= data.size() && !(bytes[next] == 0xFF && (bytes[next + 1] & 0xF0) == 0xF0)) ++next;
            if (next + 4 > data.size()) break;
            pos = next;
            continue;
        }
        BitReader br{bytes + pos + 4, data.size() - pos - 4};
        frame.clear();
        if (!decode_frame(st, br, encrypted, frame)) break;
        for (float f : frame) {
            int v = int(std::lrintf(f * 32767.0f));
            pcm.push_back(int16_t(std::max(-32768, std::min(32767, v))));
        }
        if (std::getenv("AMG_AHX_DEBUG")) {
            size_t used = 4 + br.bytes_used();
            bool next_ok = pos + used + 4 <= data.size() && bytes[pos + used] == 0xFF && (bytes[pos + used + 1] & 0xF0) == 0xF0;
            size_t real = 4;
            while (pos + real + 4 <= data.size() && !(bytes[pos + real] == 0xFF && bytes[pos + real + 1] == 0xF5 && bytes[pos + real + 2] == 0xE0 && bytes[pos + real + 3] == 0xC0)) ++real;
            std::fprintf(stderr, "frame at %zu: consumed %zu bytes, next header at +%zu %s\n", pos, used, real, next_ok ? "(aligned)" : "(MISALIGNED)");
        }
        pos += 4 + br.bytes_used();
    }
    if (pcm.size() > total) pcm.resize(total);
    return !pcm.empty();
}

}  // namespace amg
