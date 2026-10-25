#include "audio.hpp"

#include "ahx.hpp"
#include "at9.hpp"
#include "datapath.hpp"
#include "hca.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <thread>

namespace amg {

namespace {

uint32_t be32(const std::string& d, size_t at) {
    return (uint32_t(uint8_t(d[at])) << 24) | (uint32_t(uint8_t(d[at + 1])) << 16) | (uint32_t(uint8_t(d[at + 2])) << 8) |
           uint32_t(uint8_t(d[at + 3]));
}
uint16_t be16(const std::string& d, size_t at) { return uint16_t((uint8_t(d[at]) << 8) | uint8_t(d[at + 1])); }
uint32_t le32(const std::string& d, size_t at) {
    return uint32_t(uint8_t(d[at])) | (uint32_t(uint8_t(d[at + 1])) << 8) | (uint32_t(uint8_t(d[at + 2])) << 16) |
           (uint32_t(uint8_t(d[at + 3])) << 24);
}

// The 1,024 primes above 2^14 that CRI's key derivation indexes (docs/technical/audio.md).
const std::vector<int>& key_primes() {
    static std::vector<int> primes;
    if (primes.empty()) {
        for (int n = 0x4001; primes.size() < 0x400; n += 2) {
            bool prime = true;
            for (int d = 3; d * d <= n; d += 2)
                if (n % d == 0) {
                    prime = false;
                    break;
                }
            if (prime) primes.push_back(n);
        }
    }
    return primes;
}

void derive_key(const std::string& text, int& start, int& mult, int& add) {
    const auto& p = key_primes();
    start = p[0x100];
    mult = p[0x200];
    add = p[0x300];
    for (unsigned char c : text) {
        int k = p[size_t(c) + 0x80];
        start = p[size_t(start) * size_t(k) % 0x400];
        mult = p[size_t(mult) * size_t(k) % 0x400];
        add = p[size_t(add) * size_t(k) % 0x400];
    }
}

}  // namespace

bool Afs::open(const std::string& file) {
    path = file;
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    char head[8];
    f.read(head, 8);
    if (std::memcmp(head, "AFS\0", 4) != 0) return false;
    uint32_t n = le32(std::string(head, 8), 4);
    std::string table(size_t(n) * 8, '\0');
    f.read(table.data(), std::streamsize(table.size()));
    files.clear();
    for (uint32_t i = 0; i < n; ++i) files.emplace_back(le32(table, i * 8), le32(table, i * 8 + 4));
    return true;
}

bool Afs::read(size_t index, std::string& out) const {
    if (index >= files.size()) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(files[index].first);
    out.resize(files[index].second);
    f.read(out.data(), std::streamsize(out.size()));
    return true;
}

bool AdxStream::load(const std::string& path, uint32_t offset, uint32_t size, const std::string& key) {
    file.open(path, std::ios::binary);
    if (!file) return false;
    base = std::streamoff(offset);
    data.resize(std::min<uint32_t>(size, 0x200));
    file.seekg(base);
    file.read(data.data(), std::streamsize(data.size()));
    if (data.size() < 0x40 || uint8_t(data[0]) != 0x80 || data[1] != 0) return false;
    uint32_t copyright = be16(data, 2);
    int encoding = uint8_t(data[4]);
    block_size = uint8_t(data[5]);
    int bits = uint8_t(data[6]);
    channels = uint8_t(data[7]);
    rate = int(be32(data, 8));
    samples = be32(data, 12);
    int highpass = be16(data, 16);
    int version = uint8_t(data[18]);
    int flags = uint8_t(data[19]);
    if (encoding != 3 || bits != 4 || channels < 1 || channels > 2) return false;
    data_offset = copyright + 4;
    encrypted = (flags & 0x08) != 0;
    size_t loop_base = version == 3 ? 0x18 : version == 4 ? 0x24 : 0;
    if (loop_base && loop_base + 0x14 <= copyright) {
        if (be32(data, loop_base) != 0) {
            looped = true;
            loop_start = be32(data, loop_base + 4);
            loop_end = be32(data, loop_base + 12);
        }
    }
    double a = std::sqrt(2.0) - std::cos(2.0 * 3.14159265358979323846 * highpass / rate);
    double b = std::sqrt(2.0) - 1.0;
    double c = (a - std::sqrt((a + b) * (a - b))) / b;
    coef1 = int(std::lrint(c * 8192.0));
    coef2 = int(std::lrint(-(c * c) * 4096.0));
    if (encrypted) derive_key(key, xor_start, xor_mult, xor_add);
    rewind_to(0);
    return true;
}

void AdxStream::rewind_to(uint32_t sample) {
    block_index = sample / 32;
    xor_state = xor_start;
    if (encrypted)
        for (uint32_t i = 0; i < block_index * uint32_t(channels); ++i) xor_state = (xor_state * xor_mult + xor_add) & 0x7FFF;
    h1.assign(size_t(channels), 0);
    h2.assign(size_t(channels), 0);
    frames.clear();
    cursor = 0;
    position = block_index * 32;
    ended = false;
    decode_block();
    cursor = std::min<size_t>(sample % 32, frames.size() / size_t(channels));
    position = sample;
}

void AdxStream::decode_block() {
    frames.clear();
    cursor = 0;
    uint32_t done = block_index * 32;
    if (done >= samples) return;
    uint32_t count = std::min<uint32_t>(32, samples - done);
    std::streamoff pos = base + std::streamoff(data_offset) + std::streamoff(block_index) * block_size * channels;
    frames.assign(size_t(count) * size_t(channels), 0);
    uint8_t raw[2 * 64];
    file.clear();
    file.seekg(pos);
    for (int ch = 0; ch < channels; ++ch) {
        file.read(reinterpret_cast<char*>(raw), block_size);
        if (file.gcount() != block_size) {
            frames.clear();
            return;
        }
        const uint8_t* block = raw;
        int scale = ((((block[0] << 8) | block[1]) ^ xor_state) & 0x1FFF) + 1;
        if (encrypted) xor_state = (xor_state * xor_mult + xor_add) & 0x7FFF;
        int s1 = h1[size_t(ch)], s2 = h2[size_t(ch)];
        for (uint32_t i = 0; i < count; ++i) {
            int byte = block[2 + (i >> 1)];
            int nibble = (i & 1) ? (byte & 0x0F) : (byte >> 4);
            if (nibble & 8) nibble -= 16;
            int sample = nibble * scale + ((coef1 * s1 + coef2 * s2) >> 12);
            sample = std::max(-32768, std::min(32767, sample));
            frames[size_t(i) * size_t(channels) + size_t(ch)] = int16_t(sample);
            s2 = s1;
            s1 = sample;
        }
        h1[size_t(ch)] = s1;
        h2[size_t(ch)] = s2;
    }
    ++block_index;
}

bool AdxStream::load_wav(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string riff((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (riff.size() < 12 || riff.compare(0, 4, "RIFF") != 0 || riff.compare(8, 4, "WAVE") != 0) return false;
    uint16_t ch = 0, bits = 0;
    uint32_t sample_rate = 0;
    size_t pos = 12;
    frames.clear();
    while (pos + 8 <= riff.size()) {
        std::string tag = riff.substr(pos, 4);
        uint32_t size = le32(riff, pos + 4);
        size_t body = pos + 8;
        if (body + size > riff.size()) break;
        if (tag == "fmt ") {
            ch = uint16_t(uint8_t(riff[body + 2]) | (uint8_t(riff[body + 3]) << 8));
            sample_rate = le32(riff, body + 4);
            bits = uint16_t(uint8_t(riff[body + 14]) | (uint8_t(riff[body + 15]) << 8));
        } else if (tag == "data") {
            if (bits != 16) return false;  // ffmpeg is told pcm_s16le; nothing else expected here
            size_t n = size / 2;
            frames.resize(n);
            std::memcpy(frames.data(), riff.data() + body, n * 2);
        } else if (tag == "smpl" && size >= 36) {
            // tools/vita_audio.py copies this over from the source ATRAC9/HCA RIFF, which ffmpeg
            // otherwise drops: standard WAV loop points, one 24-byte loop starting at offset 36.
            uint32_t num_loops = le32(riff, body + 28);
            if (num_loops >= 1) {
                looped = true;
                loop_start = le32(riff, body + 36 + 8);
                loop_end = le32(riff, body + 36 + 12);
            }
        }
        pos = body + size + (size & 1);  // chunks are word-aligned
    }
    if (ch == 0 || frames.empty()) return false;
    channels = ch;
    rate = int(sample_rate);
    samples = uint32_t(frames.size() / size_t(channels));
    cursor = 0;
    position = 0;
    ended = false;
    pcm_only = true;
    return true;
}

bool AdxStream::load_atrac9(const std::string& riff) {
    Atrac9Result r;
    if (!decode_atrac9_riff(riff, r)) return false;
    frames = std::move(r.pcm);
    channels = r.channels;
    rate = r.rate;
    looped = r.looped;
    loop_start = r.loop_start;
    loop_end = r.loop_end;
    samples = uint32_t(frames.size() / size_t(channels));
    cursor = 0;
    position = 0;
    ended = false;
    pcm_only = true;
    return true;
}

bool AdxStream::next(int16_t& l, int16_t& r) {
    if (ended) return false;
    if (pcm_only) {
        if (looped && position >= loop_end) {
            cursor = size_t(loop_start);
            position = loop_start;
        }
        if (cursor >= frames.size() / size_t(channels)) {
            if (looped) {
                cursor = size_t(loop_start);
                position = loop_start;
            } else {
                ended = true;
                return false;
            }
        }
        l = frames[cursor * size_t(channels)];
        r = channels > 1 ? frames[cursor * size_t(channels) + 1] : l;
        ++cursor;
        ++position;
        return true;
    }
    if (looped && position >= loop_end) {
        rewind_to(loop_start);
    }
    if (cursor >= frames.size() / size_t(channels)) {
        decode_block();
        if (frames.empty()) {
            ended = true;
            return false;
        }
    }
    l = frames[cursor * size_t(channels)];
    r = channels > 1 ? frames[cursor * size_t(channels) + 1] : l;
    ++cursor;
    ++position;
    if (!looped && position >= samples) ended = true;
    return true;
}

// ---------------------------------------------------------------------------------------------

namespace {

uint32_t name_hash(const std::string& name) {
    uint32_t h = 0;
    for (unsigned char c : name) h = h * 16301u + c;
    return h;
}

// One entry of an ARC archive by file name (docs/technical/arc-format.md).
bool arc_entry(const std::string& path, const std::string& name, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char head[8];
    f.read(head, 8);
    uint32_t n = le32(std::string(head, 8), 0);
    f.seekg(8 + 4 * std::streamoff(n));
    std::string table(16 * size_t(n), '\0');
    f.read(table.data(), std::streamsize(table.size()));
    uint32_t h = name_hash(name);
    for (uint32_t i = 0; i < n; ++i) {
        if (le32(table, 16 * i + 4) != h) continue;
        uint32_t off = le32(table, 16 * i + 8), size = le32(table, 16 * i + 12);
        out.resize(size);
        f.seekg(off);
        f.read(out.data(), std::streamsize(size));
        return true;
    }
    return false;
}

// PlayStation SPU ADPCM: 16-byte blocks, a shift and filter byte, a flag byte, 28 samples of 4 bits.
// Flag bit 0 ends the sample; bit 1 says the end loops back, bit 2 marks the loop start.
void decode_vag(const std::string& bd, size_t offset, std::vector<int16_t>& pcm, bool& looped, size_t& loop_start) {
    static const int F0[5] = {0, 60, 115, 98, 122}, F1[5] = {0, 0, -52, -55, -60};
    int s1 = 0, s2 = 0;
    looped = false;
    loop_start = 0;
    for (size_t pos = offset; pos + 16 <= bd.size(); pos += 16) {
        int head = uint8_t(bd[pos]), flags = uint8_t(bd[pos + 1]);
        int shift = head & 0x0F, filter = std::min((head >> 4) & 0x0F, 4);
        if (flags & 4) loop_start = pcm.size();
        for (int i = 0; i < 28; ++i) {
            int byte = uint8_t(bd[pos + 2 + size_t(i / 2)]);
            int nibble = (i & 1) ? (byte >> 4) : (byte & 0x0F);
            int sample = (nibble << 12);
            if (sample & 0x8000) sample -= 0x10000;
            sample >>= shift;
            sample += (s1 * F0[filter] + s2 * F1[filter] + 32) >> 6;
            sample = std::max(-32768, std::min(32767, sample));
            pcm.push_back(int16_t(sample));
            s2 = s1;
            s1 = sample;
        }
        if (flags & 1) {
            looped = (flags & 2) != 0;
            break;
        }
    }
}

}  // namespace

bool SeBank::load(const std::string& arc_path, int bank) {
    std::string hd, bd;
    std::string stem = "sys_se" + std::to_string(bank);
    if (!arc_entry(arc_path, stem + ".hd", hd) || !arc_entry(arc_path, stem + ".bd", bd)) return false;
    if (hd.size() < 0x50 || hd.compare(0, 8, "IECSsreV") != 0) return false;
    // Chunks: each one is a magic, a size, a highest index and a table of offsets (relative to the
    // chunk) to its records.
    auto chunk_offset = [&](const char* magic) -> size_t {
        size_t pos = 0;
        while (pos + 16 <= hd.size()) {
            uint32_t size = le32(hd, pos + 8);
            if (hd.compare(pos, 8, magic) == 0) return pos;
            if (size == 0) break;
            pos += size;
        }
        return std::string::npos;
    };
    size_t vagi = chunk_offset("IECSigaV"), smpl = chunk_offset("IECSlpmS"), sset = chunk_offset("IECStesS"),
           prog = chunk_offset("IECSgorP");
    if (vagi == std::string::npos || smpl == std::string::npos || sset == std::string::npos ||
        prog == std::string::npos)
        return false;
    auto record = [&](size_t chunk, uint32_t index) { return chunk + le32(hd, chunk + 16 + 4 * size_t(index)); };
    uint32_t vag_count = le32(hd, vagi + 12) + 1, smpl_count = le32(hd, smpl + 12) + 1,
             sset_count = le32(hd, sset + 12) + 1;
    vags.clear();
    for (uint32_t i = 0; i < vag_count; ++i) {
        size_t r = record(vagi, i);
        Sample s;
        s.pcm = std::make_shared<std::vector<int16_t>>();
        uint32_t offset = le32(hd, r);
        s.rate = uint8_t(hd[r + 4]) | (uint8_t(hd[r + 5]) << 8);
        decode_vag(bd, offset, *s.pcm, s.looped, s.loop_start);
        vags.push_back(std::move(s));
    }
    sample_vag.clear();
    sample_volume.clear();
    for (uint32_t i = 0; i < smpl_count; ++i) {
        size_t r = record(smpl, i);
        sample_vag.push_back(uint8_t(hd[r]) | (uint8_t(hd[r + 1]) << 8));
        sample_volume.push_back(uint8_t(hd[r + 16]));  // hypothesis: volume of the sample, 0..127
    }
    set_sample.clear();
    for (uint32_t i = 0; i < sset_count; ++i) {
        size_t r = record(sset, i);
        set_sample.push_back(uint8_t(hd[r + 4]) | (uint8_t(hd[r + 5]) << 8));
    }
    // Program 0: a parameter block (split block address, split count, split size, volume, ...) and the
    // split blocks. Each split has a sample set number and a range of notes.
    size_t program = record(prog, 0);
    int split_count = uint8_t(hd[program + 4]), split_size = uint8_t(hd[program + 5]);
    size_t split_at = program + le32(hd, program);  // the split blocks' address, relative to the program block
    splits.clear();
    for (int i = 0; i < split_count; ++i) {
        size_t r = split_at + size_t(i) * size_t(split_size);
        if (r + 6 > hd.size()) break;
        splits.push_back({uint8_t(hd[r + 2]), uint8_t(hd[r + 4]), uint8_t(hd[r]) | (uint8_t(hd[r + 1]) << 8)});
    }
    return true;
}

bool SeBank::load_vita(const std::string& dir) {
    vags.clear();
    splits.clear();
    set_sample.clear();
    sample_vag.clear();
    sample_volume.clear();
    for (int note = 0; note < 128; ++note) {
        char name[32];
        std::snprintf(name, sizeof name, "/cue_%03d.wav", note);
        AdxStream wav;
        if (!wav.load_wav(dir + name) || wav.channels != 1) continue;
        int index = int(vags.size());
        Sample s;
        s.pcm = std::make_shared<std::vector<int16_t>>(std::move(wav.frames));
        s.rate = wav.rate;
        s.looped = wav.looped;
        s.loop_start = wav.loop_start;
        vags.push_back(std::move(s));
        splits.push_back({note, note, index});
        set_sample.push_back(index);
        sample_vag.push_back(index);
        sample_volume.push_back(127);
    }
    return !vags.empty();
}

namespace {

const double HEVAG_COEFS[128][4] =
#include "hevag_coefs.inc"
    ;

const int VAG_F0[5] = {0, 60, 115, 98, 122}, VAG_F1[5] = {0, 0, -52, -55, -60};

// Sony's PlayStation ADPCM (the PS2's VAG, which the PS2 bank decoder in this file also reads), or the Vita's HE-VAG
// (version 0x20001: a 7-bit filter index over 128 four-tap filters). tools/acb.py is the reference.
std::vector<int16_t> decode_vita_vag(const std::string& d, size_t pos, bool hevag, bool& looped, size_t& loop_start) {
    std::vector<int16_t> pcm;
    int s1 = 0, s2 = 0;
    int h1 = 0, h2 = 0, h3 = 0, h4 = 0;
    looped = false;
    loop_start = 0;
    size_t start = 0;
    bool have_start = false;
    while (pos + 16 <= d.size()) {
        int head = uint8_t(d[pos]), flags = uint8_t(d[pos + 1]);
        int shift = head & 0x0F;
        int flag = hevag ? (flags & 0x0F) : flags;
        if (flag & 4) {
            start = pcm.size();
            have_start = true;
        }
        int filter = hevag ? ((flags & 0xF0) | (head >> 4)) : std::min((head >> 4) & 0x0F, 4);
        if (hevag && filter > 127) filter = 0;
        for (int i = 0; i < 28; ++i) {
            int byte = uint8_t(d[pos + 2 + size_t(i / 2)]);
            int nibble = (i & 1) ? (byte >> 4) : (byte & 0x0F);
            if (hevag) {
                int sample = 0;
                if (flag < 7) {
                    int code = (nibble & 8) ? nibble - 16 : nibble;
                    code = (code * 4096) >> shift;  // (code << 12) >> shift
                    const double* c = HEVAG_COEFS[filter];
                    sample = code + int(h1 * c[0] + h2 * c[1] + h3 * c[2] + h4 * c[3]);  // truncated first, as the C
                }
                pcm.push_back(int16_t(std::max(-32768, std::min(32767, sample))));
                h4 = h3;
                h3 = h2;
                h2 = h1;
                h1 = sample;
            } else {
                int s = nibble << 12;
                if (s & 0x8000) s -= 0x10000;
                s >>= shift;
                s += (s1 * VAG_F0[filter] + s2 * VAG_F1[filter] + 32) >> 6;
                s = std::max(-32768, std::min(32767, s));
                pcm.push_back(int16_t(s));
                s2 = s1;
                s1 = s;
            }
        }
        pos += 16;
        if (flag & 1) {
            if (flag & 2) {
                looped = true;
                loop_start = have_start ? start : 0;
            }
            break;
        }
    }
    return pcm;
}

uint32_t be32_at(const std::string& d, size_t at) {
    return (uint32_t(uint8_t(d[at])) << 24) | (uint32_t(uint8_t(d[at + 1])) << 16) | (uint32_t(uint8_t(d[at + 2])) << 8) |
           uint32_t(uint8_t(d[at + 3]));
}
uint16_t be16_at(const std::string& d, size_t at) { return uint16_t((uint8_t(d[at]) << 8) | uint8_t(d[at + 1])); }
uint32_t le32_at(const std::string& d, size_t at) {
    return uint32_t(uint8_t(d[at])) | (uint32_t(uint8_t(d[at + 1])) << 8) | (uint32_t(uint8_t(d[at + 2])) << 16) |
           (uint32_t(uint8_t(d[at + 3])) << 24);
}

}  // namespace

bool SeBank::load_acb(const std::string& acb_path) {
    vags.clear();
    splits.clear();
    set_sample.clear();
    sample_vag.clear();
    sample_volume.clear();
    std::ifstream f(acb_path, std::ios::binary);
    if (!f) return false;
    std::string file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    UtfTable bank_rows;
    try {
        bank_rows = cpk_parse_utf(file);
    } catch (const std::exception&) {
        return false;
    }
    if (bank_rows.rows.empty()) return false;
    const auto& bank = bank_rows.rows[0];
    auto blob = [&](const char* column) -> const std::string* {
        auto it = bank.find(column);
        return it != bank.end() && it->second.is_blob ? &it->second.s : nullptr;
    };
    const std::string *cue_blob = blob("CueTable"), *synth_blob = blob("SynthTable"), *awb = blob("AwbFile");
    if (!cue_blob || !synth_blob || !awb || awb->size() < 16 || awb->compare(0, 4, "AFS2") != 0) return false;
    UtfTable cues, synths;
    try {
        cues = cpk_parse_utf(*cue_blob);
        synths = cpk_parse_utf(*synth_blob);
    } catch (const std::exception&) {
        return false;
    }
    // The wave bank: AFS2, a u16 id table and a u32 offset table (one more than the files).
    uint32_t count = le32_at(*awb, 8);
    size_t offsets_at = 16 + size_t(count) * 2;
    if (offsets_at + 4 * (size_t(count) + 1) > awb->size()) return false;
    // A cue's waveform: CueTable -> SynthTable (ReferenceItems are u16 type, u16 index pairs; type 1 a waveform,
    // type 2 another synth) -> the sample's index in the wave bank.
    std::function<void(size_t, std::vector<int>&, int)> waves = [&](size_t synth, std::vector<int>& out, int depth) {
        if (synth >= synths.rows.size() || depth > 8) return;
        auto it = synths.rows[synth].find("ReferenceItems");
        if (it == synths.rows[synth].end() || !it->second.is_blob) return;
        const std::string& items = it->second.s;
        for (size_t k = 0; k + 4 <= items.size(); k += 4) {
            int kind = be16_at(items, k), index = be16_at(items, k + 2);
            if (kind == 1) out.push_back(index);
            else if (kind == 2) waves(size_t(index), out, depth + 1);
        }
    };
    for (const auto& cue : cues.rows) {
        auto get = [&](const char* key) {
            auto it = cue.find(key);
            return it != cue.end() && it->second.present ? it->second.i : int64_t(-1);
        };
        int64_t id = get("CueId"), type = get("ReferenceType"), index = get("ReferenceIndex");
        if (id < 0 || id > 127 || type != 2 || index < 0) continue;
        std::vector<int> found;
        waves(size_t(index), found, 0);
        if (found.empty() || found[0] < 0 || uint32_t(found[0]) >= count) continue;
        size_t start = le32_at(*awb, offsets_at + 4 * size_t(found[0])), end = le32_at(*awb, offsets_at + 4 * size_t(found[0]) + 4);
        if (start >= end || end > awb->size()) continue;
        std::string sample = awb->substr(start, end - start);
        size_t vag = sample.find("VAGp");
        if (vag == std::string::npos || vag + 48 > sample.size()) continue;
        uint32_t version = be32_at(sample, vag + 4), rate = be32_at(sample, vag + 16);
        bool looped = false;
        size_t loop_start = 0;
        std::vector<int16_t> pcm = decode_vita_vag(sample, vag + 48, version == 0x20001, looped, loop_start);
        if (pcm.empty()) continue;
        int slot = int(vags.size());
        Sample s;
        s.pcm = std::make_shared<std::vector<int16_t>>(std::move(pcm));
        s.rate = int(rate);
        s.looped = looped;
        s.loop_start = loop_start;
        vags.push_back(std::move(s));
        splits.push_back({int(id), int(id), slot});
        set_sample.push_back(slot);
        sample_vag.push_back(slot);
        sample_volume.push_back(127);
    }
    return !vags.empty();
}

const SeBank::Sample* SeBank::lookup(int note, int& volume) const {
    for (const Split& s : splits) {
        if (note < s.low || note > s.high) continue;
        if (s.set < 0 || size_t(s.set) >= set_sample.size()) continue;
        int sample = set_sample[size_t(s.set)];
        if (sample < 0 || size_t(sample) >= sample_vag.size()) continue;
        int vag = sample_vag[size_t(sample)];
        if (vag < 0 || size_t(vag) >= vags.size()) continue;
        volume = sample_volume[size_t(sample)];
        return &vags[size_t(vag)];
    }
    return nullptr;
}

AudioEngine::AudioEngine(const std::string& root) : root_(root) {
    vita_music_dir_ = root + "/extracted/sound_wav/music";
    vita_ambience_dir_ = root + "/extracted/sound_wav/ambience";
    vita_music_cpk_ = amg::data_file(root, "sound/MUSIC.CPK");
    vita_ambience_cpk_ = amg::data_file(root, "sound/SOUND.CPK");
    vita_ = amg::is_vita(root);
    if (vita_) {
        // Track number -> index in the converted tracks: the same shape as the PS2's track_table_
        // below, traced to a different address in the Vita executable (F4-40, vita-cpk-format.md) —
        // found by following the one reference to the literal path "app0:./sound/MUSIC.CPK" to the
        // table lookup two calls up the chain. 145 entries, -1 (and garbage past the 145th) for none.
        std::string exe = amg::vita_exe(root);
        {
            // The table sits in the code segment (file offset 0xe0, VA 0x81000000; see
            // port/src/gfx.cpp's vita_va_to_off for the full segment layout).
            constexpr uint32_t CODE_OFF = 0xe0, CODE_VA = 0x81000000, TABLE_VA = 0x810a818c;
            size_t at = CODE_OFF + (TABLE_VA - CODE_VA);
            for (int i = 0; i < 145 && at + 4 <= exe.size(); ++i, at += 4) {
                const unsigned char* b = reinterpret_cast<const unsigned char*>(exe.data()) + at;
                track_table_.push_back(int32_t(uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) |
                                               (uint32_t(b[3]) << 24)));
            }
        }
        // The voices as VOICE.CPK holds them (one folder per speaker of its inner archives, files by index: HCA
        // lines and their lip-sync tracks), decoded when a line is first needed. AMG_VITA_VOICE_DIR moves them.
        const char* voice_dir = std::getenv("AMG_VITA_VOICE_DIR");
        vita_voice_dir_ = voice_dir && *voice_dir ? std::string(voice_dir) : root + "/extracted/sound/voice";
        vita_voice_cpk_ = amg::data_file(root, "sound/VOICE.CPK");
        // VOICE.IDX (speaker, line) -> an index into that speaker's own archive is confirmed
        // byte-identical between the PS2 and Vita releases (F4-41, vita-cpk-format.md) — same file,
        // reused as-is. It maps directly onto the Vita per-speaker CPKs' sequential file IDs (checked
        // against speaker 0: 73 indices, 0 to 72, no gaps, matching Voice00.cpk's 73 files exactly).
        std::ifstream idx(amg::data_file(root, "sound/VOICE.IDX"), std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(idx)), std::istreambuf_iterator<char>());
        for (size_t i = 0; i + 1 < raw.size(); i += 2)
            voice_idx_.push_back(uint16_t(uint8_t(raw[i]) | (uint8_t(raw[i + 1]) << 8)));
        music_voice_.gain = 1.0;
        ambience_voice_.gain = 1.0;
        se_load(0);  // as below: the first effect bank, loaded from the start
        return;
    }
    music_.open(root + "/extracted/SOUND/MUSIC.AFS");
    sound_.open(root + "/extracted/SOUND/SOUND.AFS");
    // Track number -> index in MUSIC.AFS: a table of 145 words at 0x28B080 in the executable
    // (-1 for none). Read from the disc's executable so no game data is stored in the repository.
    std::ifstream f(root + "/extracted/SLPS_259.87", std::ios::binary);
    if (f) {
        f.seekg(0x28B080 - 0x100000 + 0x1000);
        for (int i = 0; i < 145; ++i) {
            unsigned char b[4];
            f.read(reinterpret_cast<char*>(b), 4);
            track_table_.push_back(int32_t(uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) |
                                           (uint32_t(b[3]) << 24)));
        }
    }
    music_voice_.gain = 1.0;
    ambience_voice_.gain = 1.0;
    voice_afs_.open(root + "/extracted/SOUND/VOICE.AFS");
    {
        std::ifstream idx(root + "/extracted/SOUND/VOICE.IDX", std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(idx)), std::istreambuf_iterator<char>());
        for (size_t i = 0; i + 1 < raw.size(); i += 2)
            voice_idx_.push_back(uint16_t(uint8_t(raw[i]) | (uint8_t(raw[i + 1]) << 8)));
    }
    se_load(0);  // the engine starts with the first effect bank loaded; only the mahjong game loads another
}

std::shared_ptr<AdxStream> AudioEngine::vita_track(const std::string& wav_dir, const std::string& cpk,
                                                   std::vector<CpkEntry>& entries, int index) {
    auto s = std::make_shared<AdxStream>();
    char name[32];
    std::snprintf(name, sizeof name, "/%05d.wav", index);
    if (s->load_wav(wav_dir + name)) return s;
    std::error_code ec;
    if (index < 0 || !std::filesystem::exists(cpk, ec)) return nullptr;
    std::string riff;
    {
        std::lock_guard<std::mutex> lk(voice_mutex_);  // the entry tables are filled once, and read by one thread at a time
        try {
            if (entries.empty()) entries = cpk_read_entries(cpk);
        } catch (const std::exception&) {
            return nullptr;
        }
        if (size_t(index) >= entries.size()) return nullptr;
        std::ifstream f(cpk, std::ios::binary);
        if (!f) return nullptr;
        f.seekg(std::streamoff(entries[size_t(index)].offset));
        riff.assign(size_t(entries[size_t(index)].size), '\0');
        f.read(riff.data(), std::streamsize(riff.size()));
    }
    return s->load_atrac9(riff) ? s : nullptr;
}

void AudioEngine::music_play(int track) {
    if (vita_) {
        // The whole track is read first (15 MB or so), outside the mixer's lock, so the sound thread keeps mixing.
        std::shared_ptr<AdxStream> s;
        if (track >= 0 && size_t(track) < track_table_.size() && track_table_[size_t(track)] >= 0) {
            s = vita_track(vita_music_dir_, vita_music_cpk_, vita_music_entries_, track_table_[size_t(track)]);
            if (std::getenv("AMG_VOICE_LOG")) std::fprintf(stderr, "music play track=%d file=%d %s\n", track, track_table_[size_t(track)], s ? "ok" : "missing");
        }
        std::lock_guard<std::mutex> lk(mutex_);
        music_voice_.stream = s;
        if (!s) return;
        music_voice_.paused = false;
        music_voice_.primed = false;
        music_voice_.phase = 0.0;
        return;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    music_voice_.stream.reset();
    if (track < 0 || size_t(track) >= track_table_.size() || track_table_[size_t(track)] < 0) return;
    const auto& entry = music_.files[size_t(track_table_[size_t(track)])];
    auto s = std::make_shared<AdxStream>();
    if (!s->load(music_.path, entry.first, entry.second, key_)) return;
    music_voice_.stream = s;
    music_voice_.paused = false;
    music_voice_.primed = false;
    music_voice_.phase = 0.0;
}

void AudioEngine::music_stop() {
    std::lock_guard<std::mutex> lk(mutex_);
    music_voice_.stream.reset();
}
void AudioEngine::music_pause(bool paused) {
    std::lock_guard<std::mutex> lk(mutex_);
    music_voice_.paused = paused;
}
void AudioEngine::music_volume(int volume) {
    std::lock_guard<std::mutex> lk(mutex_);
    // FIDELITY: read 0x810057C6 (the Vita's Music.setVolumePrim): volume * 0.008 to the player, no clamp, so 100 is 0.8;
    // voices and the ambience take volume * 0.01 (0x8107DB28, 0x81006F8A), effects volume * 0.006 (0x81006EBC).
    music_voice_.gain = vita_ ? std::max(0, volume) * 0.008 : std::max(0, std::min(100, volume)) / 100.0;
}
bool AudioEngine::music_busy() {
    std::lock_guard<std::mutex> lk(mutex_);
    return music_voice_.stream && !music_voice_.stream->ended;
}

void AudioEngine::ambience_play(int number) {
    if (vita_) {
        std::shared_ptr<AdxStream> s;  // read outside the mixer's lock, as music_play
        if (number >= 1) {
            s = vita_track(vita_ambience_dir_, vita_ambience_cpk_, vita_ambience_entries_, number - 1);  // ENV0001 is entry 0 (hypothesis)
        }
        std::lock_guard<std::mutex> lk(mutex_);
        ambience_voice_.stream = s;
        if (!s) return;
        ambience_voice_.paused = false;
        ambience_voice_.primed = false;
        ambience_voice_.phase = 0.0;
        return;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    ambience_voice_.stream.reset();
    if (number < 1 || size_t(number - 1) >= sound_.files.size()) return;  // ENV0001 is entry 0 (hypothesis)
    const auto& entry = sound_.files[size_t(number - 1)];
    auto s = std::make_shared<AdxStream>();
    if (!s->load(sound_.path, entry.first, entry.second, key_)) return;
    ambience_voice_.stream = s;
    ambience_voice_.paused = false;
    ambience_voice_.primed = false;
    ambience_voice_.phase = 0.0;
}
void AudioEngine::ambience_stop() {
    std::lock_guard<std::mutex> lk(mutex_);
    ambience_voice_.stream.reset();
}
void AudioEngine::ambience_pause(bool paused) {
    std::lock_guard<std::mutex> lk(mutex_);
    ambience_voice_.paused = paused;
}
void AudioEngine::ambience_volume(int volume) {
    std::lock_guard<std::mutex> lk(mutex_);
    ambience_voice_.gain = std::max(0, std::min(100, volume)) / 100.0;
}

void AudioEngine::se_load(int bank) {
    SeBank loaded;
    bool ok = vita_ ? (loaded.load_vita(root_ + "/extracted/sound_wav/se" + std::to_string(bank)) ||
                       loaded.load_acb(amg::data_file(root_, "sound/SYSSE" + std::to_string(bank) + ".ACB")))
                    : loaded.load(root_ + "/extracted/SOUND/MUSIC.ARC", bank);
    if (std::getenv("AMG_VOICE_LOG")) std::fprintf(stderr, "se load bank=%d %s (%zu samples)\n", bank, ok ? "ok" : "failed", loaded.vags.size());
    if (!ok) return;
    std::lock_guard<std::mutex> lk(mutex_);
    bank_ = std::move(loaded);
    effects_.clear();
}

void AudioEngine::se_play(int note, int volume) {
    std::lock_guard<std::mutex> lk(mutex_);
    int sample_volume = 127;
    const SeBank::Sample* s = bank_.lookup(note & 0x7F, sample_volume);
    static const bool log = std::getenv("AMG_VOICE_LOG") != nullptr;
    if (log) std::fprintf(stderr, "se play note=%d volume=%d %s\n", note, volume, s && s->pcm && !s->pcm->empty() ? "ok" : "no sample");
    if (!s || !s->pcm || s->pcm->empty()) return;
    EffectVoice v;
    v.pcm = s->pcm;
    v.step = double(s->rate > 0 ? s->rate : 44100) / double(AUDIO_RATE);
    v.gain = (vita_ ? std::max(0, volume) * 0.006 : std::max(0, std::min(100, volume)) / 100.0) * (sample_volume / 127.0);
    v.looped = s->looped;
    v.loop_start = s->loop_start;
    if (effects_.size() >= 24) effects_.erase(effects_.begin());
    effects_.push_back(std::move(v));
}

void AudioEngine::se_stop() {
    std::lock_guard<std::mutex> lk(mutex_);
    effects_.clear();
}

// The decoded line `number` of `speaker`: VOICE.IDX gives the file inside the speaker's archive
// (docs/technical/audio.md). Decoded lines are kept in a small cache.
// Reads file `kind` (0 .slp, 1 .ahx) of a voice line through VOICE.IDX. voice_mutex_ must be held.
bool AudioEngine::voice_file(int speaker, int number, int kind, std::string& out) {
    if (speaker < 0 || speaker >= 16 || number < 0 || number >= 10000) return false;
    size_t group_at = size_t(speaker) * 100 + size_t(number / 100);
    if (group_at >= voice_idx_.size() || voice_idx_[group_at] == 0xFFFF) return false;
    size_t file_at = 1600 + size_t(voice_idx_[group_at]) * 200 + size_t(number % 100) * 2 + size_t(kind);
    if (file_at >= voice_idx_.size() || voice_idx_[file_at] == 0xFFFF) return false;
    if (size_t(speaker) >= voice_afs_.files.size()) return false;
    if (voice_tables_.empty()) voice_tables_.resize(voice_afs_.files.size());
    auto& table = voice_tables_[size_t(speaker)];
    std::ifstream f(voice_afs_.path, std::ios::binary);
    uint32_t base = voice_afs_.files[size_t(speaker)].first;
    if (table.empty()) {
        char head[8];
        f.seekg(base);
        f.read(head, 8);
        uint32_t n = le32(std::string(head, 8), 4);
        std::string raw(8 * size_t(n), '\0');
        f.read(raw.data(), std::streamsize(raw.size()));
        for (uint32_t i = 0; i < n; ++i) table.emplace_back(le32(raw, 8 * i), le32(raw, 8 * i + 4));
    }
    size_t index = voice_idx_[file_at];
    if (index >= table.size()) return false;
    out.assign(table[index].second, '\0');
    f.seekg(std::streamoff(base) + table[index].first);
    f.read(out.data(), std::streamsize(out.size()));
    return bool(f);
}

std::shared_ptr<std::vector<int16_t>> AudioEngine::voice_pcm(int speaker, int number) {
    if (speaker < 0 || speaker >= 16 || number < 0 || number >= 10000) return nullptr;
    int key = (speaker << 16) | number;
    std::lock_guard<std::mutex> lk(voice_mutex_);
    for (auto& [k, pcm] : voice_cache_)
        if (k == key) return pcm;
    auto pcm = std::make_shared<std::vector<int16_t>>();
    if (vita_) {
        int index = vita_voice_index(speaker, number, 1);
        if (index < 0) return nullptr;
        std::string file;
        if (!vita_voice_file(speaker, index, file)) return nullptr;
        int rate = 0, channels = 0;
        if (!decode_hca(file, *pcm, rate, channels) || channels != 1) return nullptr;
    } else {
        std::string file;
        if (!voice_file(speaker, number, 1, file)) return nullptr;
        int rate = 0;
        if (!decode_ahx(file, *pcm, rate)) return nullptr;
    }
    if (voice_cache_.size() >= 32) voice_cache_.erase(voice_cache_.begin());
    voice_cache_.emplace_back(key, pcm);
    return pcm;
}

bool AudioEngine::vita_voice_file(int speaker, int index, std::string& out) {
    char path[64];
    std::snprintf(path, sizeof path, "/speaker_%02d/%05d.bin", speaker, index);
    {
        std::ifstream f(vita_voice_dir_ + path, std::ios::binary);
        if (f) {
            out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            return true;
        }
    }
    std::error_code ec;
    if (vita_voice_cpk_.empty() || !std::filesystem::exists(vita_voice_cpk_, ec)) return false;
    try {
        if (vita_voice_outer_.empty()) vita_voice_outer_ = cpk_read_entries(vita_voice_cpk_);
        if (speaker < 0 || size_t(speaker) >= vita_voice_outer_.size()) return false;
        auto it = vita_voice_inner_.find(speaker);
        if (it == vita_voice_inner_.end())
            it = vita_voice_inner_.emplace(speaker, cpk_read_entries(vita_voice_cpk_, vita_voice_outer_[size_t(speaker)].offset)).first;
        if (index < 0 || size_t(index) >= it->second.size()) return false;
        const CpkEntry& e = it->second[size_t(index)];
        std::ifstream f(vita_voice_cpk_, std::ios::binary);
        if (!f) return false;
        f.seekg(std::streamoff(e.offset));
        out.assign(size_t(e.size), '\0');
        f.read(out.data(), std::streamsize(out.size()));
        return bool(f);
    } catch (const std::exception&) {
        return false;
    }
}

std::shared_ptr<std::string> AudioEngine::voice_slp(int speaker, int number) {
    std::lock_guard<std::mutex> lk(voice_mutex_);
    auto slp = std::make_shared<std::string>();
    if (vita_) {
        // Kept like the lines (voice_prepare reads both ahead); voice_mutex_ is held.
        static std::vector<std::pair<int, std::shared_ptr<std::string>>> cache;
        int key = (speaker << 16) | number;
        for (auto& [k, cached] : cache)
            if (k == key) return cached;
        int index = vita_voice_index(speaker, number, 0);
        if (index < 0) return nullptr;
        if (!vita_voice_file(speaker, index, *slp)) return nullptr;
        if (cache.size() >= 32) cache.erase(cache.begin());
        cache.emplace_back(key, slp);
        return slp;
    }
    if (!voice_file(speaker, number, 0, *slp)) return nullptr;
    return slp;
}

// VOICE.IDX, (speaker, line, kind) -> index into the speaker's own archive; kind 0 is the line's
// lip-sync track, 1 its audio. -1 when the line has none.
int AudioEngine::vita_voice_index(int speaker, int number, int kind) const {
    if (speaker < 0 || speaker >= 16 || number < 0 || number >= 10000) return -1;
    size_t group_at = size_t(speaker) * 100 + size_t(number / 100);
    if (group_at >= voice_idx_.size() || voice_idx_[group_at] == 0xFFFF) return -1;
    size_t file_at = 1600 + size_t(voice_idx_[group_at]) * 200 + size_t(number % 100) * 2 + size_t(kind);
    if (file_at >= voice_idx_.size() || voice_idx_[file_at] == 0xFFFF) return -1;
    return voice_idx_[file_at];
}

void AudioEngine::voice_prepare(int speaker, int number) {
    if (std::getenv("AMG_VOICE_LOG")) std::fprintf(stderr, "voice setup speaker=%d number=%d\n", speaker, number);
    if (vita_) {
        // Read on a helper thread so the game does not wait on the disk (the Vita's voices are plain WAV files,
        // possibly on a slow disk, AMG_VITA_VOICE_DIR): the line and its lip-sync track land in the cache, and
        // a play that comes first just waits for the rest of the read. The engine lives as long as the process.
        std::thread([this, speaker, number] {
            voice_pcm(speaker, number);
            voice_slp(speaker, number);
        }).detach();
        return;
    }
    voice_pcm(speaker, number);
}

void AudioEngine::voice_play(int speaker, int channel, int number, int volume) {
    static const bool log = std::getenv("AMG_VOICE_LOG") != nullptr;
    if (log) std::fprintf(stderr, "voice play speaker=%d channel=%d number=%d volume=%d\n", speaker, channel, number, volume);
    auto pcm = voice_pcm(speaker, number);
    if (log) std::fprintf(stderr, "  -> %zu samples\n", pcm ? pcm->size() : size_t(0));
    std::shared_ptr<std::string> slp = (channel & 1) == 0 ? voice_slp(speaker, number) : nullptr;
    std::lock_guard<std::mutex> lk(mutex_);
    if (speaker < 0 || speaker > 16) return;
    VoiceChannel& c = voices_[channel & 1][speaker];
    c.active = pcm && !pcm->empty();
    c.paused = false;
    if (!c.active) return;
    c.v = EffectVoice();
    c.v.pcm = pcm;
    c.slp = slp;
    // AHX is fixed at 24000 Hz; the Vita's HCA voices decode at 48000 Hz (confirmed on real files,
    // docs/technical/vita-cpk-format.md) — not stored per voice, just the two known platform rates.
    c.v.step = (vita_ ? 48000.0 : 24000.0) / double(AUDIO_RATE);
    c.v.gain = std::max(0, std::min(100, volume)) / 100.0;
}

void AudioEngine::voice_stop(int speaker, int channel) {
    if (std::getenv("AMG_VOICE_LOG")) std::fprintf(stderr, "voice stop speaker=%d channel=%d\n", speaker, channel);
    std::lock_guard<std::mutex> lk(mutex_);
    if (speaker >= 0 && speaker <= 16) voices_[channel & 1][speaker].active = false;
}

void AudioEngine::voice_stop_all(int channel) {
    if (std::getenv("AMG_VOICE_LOG")) std::fprintf(stderr, "voice stop all channel=%d\n", channel);
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& c : voices_[channel & 1]) c.active = false;
}

void AudioEngine::voice_volume(int speaker, int channel, int volume) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (speaker >= 0 && speaker <= 16)
        voices_[channel & 1][speaker].v.gain = std::max(0, std::min(100, volume)) / 100.0;
}

void AudioEngine::voice_pause(int speaker, bool paused) {
    if (std::getenv("AMG_VOICE_LOG")) std::fprintf(stderr, "voice pause speaker=%d paused=%d\n", speaker, int(paused));
    std::lock_guard<std::mutex> lk(mutex_);
    if (speaker >= 0 && speaker <= 16) voices_[0][speaker].paused = paused;
}

int AudioEngine::mouth_level(int speaker) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (speaker < 0 || speaker > 16) return -1;
    const VoiceChannel& c = voices_[0][speaker];
    if (!c.active || !c.slp) return -1;
    // One .slp byte per 1/60 s: 400 samples of the PS2's 24 kHz AHX, 800 of the Vita's 48 kHz HCA (the
    // tracks themselves are byte-identical between the two releases).
    size_t at = size_t(c.v.position) / (vita_ ? 800 : 400);
    if (at >= c.slp->size()) return -1;
    return uint8_t((*c.slp)[at]) & 3;
}

int AudioEngine::voice_length(int speaker, int number) {
    auto pcm = voice_pcm(speaker, number);
    return pcm ? int(pcm->size() * 60 / (vita_ ? 48000 : 24000)) : 0;  // hypothesis: the scripts count video frames
}

void AudioEngine::mix(Voice& v, std::vector<float>& acc, size_t frames) {
    if (!v.stream || v.paused) return;
    AdxStream& s = *v.stream;
    double step = double(s.rate) / double(AUDIO_RATE);
    if (!v.primed) {
        if (!s.next(v.l1, v.r1)) return;
        v.l0 = v.l1;
        v.r0 = v.r1;
        v.primed = true;
        v.phase = 0.0;
    }
    float g = float(v.gain) / 32768.0f;
    for (size_t i = 0; i < frames; ++i) {
        float t = float(v.phase);
        acc[i * 2] += (v.l0 + (v.l1 - v.l0) * t) * g;
        acc[i * 2 + 1] += (v.r0 + (v.r1 - v.r0) * t) * g;
        v.phase += step;
        while (v.phase >= 1.0) {
            v.phase -= 1.0;
            v.l0 = v.l1;
            v.r0 = v.r1;
            if (!s.next(v.l1, v.r1)) {
                // The stream ran out: finish this call with silence and let the owner drop it.
                v.l1 = v.l0 = 0;
                v.r1 = v.r0 = 0;
            }
        }
    }
}

void AudioEngine::render(int16_t* out, size_t frames) {
    std::vector<float> acc(frames * 2, 0.0f);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        mix(music_voice_, acc, frames);
        mix(ambience_voice_, acc, frames);
        for (auto it = effects_.begin(); it != effects_.end();) {
            EffectVoice& e = *it;
            const std::vector<int16_t>& pcm = *e.pcm;
            float g = float(e.gain) / 32768.0f;
            bool finished = false;
            for (size_t i = 0; i < frames; ++i) {
                size_t at = size_t(e.position);
                if (at + 1 >= pcm.size()) {
                    if (e.looped && e.loop_start + 1 < pcm.size()) {
                        e.position = double(e.loop_start);
                        at = e.loop_start;
                    } else {
                        finished = true;
                        break;
                    }
                }
                float t = float(e.position - double(at));
                float s = (pcm[at] + (pcm[at + 1] - pcm[at]) * t) * g;
                acc[i * 2] += s;
                acc[i * 2 + 1] += s;
                e.position += e.step;
            }
            it = finished ? effects_.erase(it) : it + 1;
        }
        for (auto& row : voices_)
            for (VoiceChannel& c : row) {
                if (!c.active || c.paused) continue;
                const std::vector<int16_t>& pcm = *c.v.pcm;
                float g = float(c.v.gain) / 32768.0f;
                for (size_t i = 0; i < frames; ++i) {
                    size_t at = size_t(c.v.position);
                    if (at + 1 >= pcm.size()) {
                        c.active = false;
                        if (std::getenv("AMG_VOICE_LOG")) std::fprintf(stderr, "voice ended at %.0f of %zu\n", c.v.position, pcm.size());
                        break;
                    }
                    float t = float(c.v.position - double(at));
                    float s = (pcm[at] + (pcm[at + 1] - pcm[at]) * t) * g;
                    acc[i * 2] += s;
                    acc[i * 2 + 1] += s;
                    c.v.position += c.v.step;
                }
            }
    }
    for (size_t i = 0; i < frames * 2; ++i) {
        float v = std::max(-1.0f, std::min(1.0f, acc[i]));
        out[i] = int16_t(v * 32767.0f);
    }
}

// ---------------------------------------------------------------------------------------------
// Script primitives
// ---------------------------------------------------------------------------------------------

void install_audio(VM& vm, AudioEngine* engine) {
    auto prim = [&](const char* cls, int n, PrimFn fn) {
        ClassObj* c = vm.find_class(cls);
        if (c) c->prims[n] = std::move(fn);
    };
    auto arg_int = [](std::vector<Value>& st, size_t b, size_t n) -> int {
        const Value& v = st.at(b + 1 + n);
        return v.tag == Tag::Int ? int(v.i) : v.tag == Tag::Float ? int(v.f) : 0;
    };
    auto truthy = [](const Value& v) {
        return !(v.tag == Tag::Nil || v.tag == Tag::False || (v.tag == Tag::Int && v.i == 0));
    };
    // Music: setup(track) prepares, playPrim(track) starts, stopPrim, pause(flag), setVolumePrim(0..100), isBusy.
    prim("Music", 0, [](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) { return st[b]; });
    prim("Music", 1, [engine, arg_int](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->music_play(arg_int(st, b, 0));
        return st[b];
    });
    prim("Music", 2, [engine](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->music_stop();
        return st[b];
    });
    prim("Music", 3, [engine, truthy](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->music_pause(truthy(st.at(b + 1)));
        return st[b];
    });
    prim("Music", 4, [engine, arg_int](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->music_volume(arg_int(st, b, 0));
        return st[b];
    });
    prim("Music", 5, [engine](VM&, Thread&, std::vector<Value>&, size_t, Method&) -> Value {
        return Value::boolean(engine->music_busy());
    });
    // Sound: playPrim(number) is the ambience loop; loadSE(bank) and playSEPrim(note, volume) are the effects.
    prim("Sound", 0, [engine, arg_int](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->se_load(arg_int(st, b, 0));
        return st[b];
    });
    prim("Sound", 1, [engine, arg_int](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->se_play(arg_int(st, b, 0), arg_int(st, b, 1));
        return st[b];
    });
    prim("Sound", 2, [engine, arg_int](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->ambience_play(arg_int(st, b, 0));
        return st[b];
    });
    prim("Sound", 3, [engine](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->ambience_stop();
        return st[b];
    });
    prim("Sound", 4, [engine, truthy](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->ambience_pause(truthy(st.at(b + 1)));
        return st[b];
    });
    prim("Sound", 5, [engine, arg_int](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->ambience_volume(arg_int(st, b, 0));
        return st[b];
    });
    // Voice (instance primitives; ivar 0 is the device, the speaker number): setup(n) 0,
    // playPrim(n, volume) 1, stopPrim 2, setVolumePrim(v) 3, pause(flag) 4, length(n) 5,
    // play2Prim(n, volume) 6, stop2 7, setVolume2Prim(v) 8; class stop 9 and stop2 10.
    auto device = [](std::vector<Value>& st, size_t b) -> int {
        const Value& self = st[b];
        if (self.tag != Tag::Obj || !self.p) return -1;
        auto* o = static_cast<ObjObj*>(self.p.get());
        return !o->iv.empty() && o->iv[0].tag == Tag::Int ? int(o->iv[0].i) : -1;
    };
    prim("Voice", 0, [engine, arg_int, device](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->voice_prepare(device(st, b), arg_int(st, b, 0));
        return st[b];
    });
    prim("Voice", 1, [engine, arg_int, device](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->voice_play(device(st, b), 0, arg_int(st, b, 0), arg_int(st, b, 1));
        return st[b];
    });
    prim("Voice", 2, [engine, device](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->voice_stop(device(st, b), 0);
        return st[b];
    });
    prim("Voice", 3, [engine, arg_int, device](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->voice_volume(device(st, b), 0, arg_int(st, b, 0));
        return st[b];
    });
    prim("Voice", 4, [engine, truthy, device](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->voice_pause(device(st, b), truthy(st.at(b + 1)));
        return st[b];
    });
    prim("Voice", 5, [engine, arg_int, device](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        return Value::integer(engine->voice_length(device(st, b), arg_int(st, b, 0)));
    });
    prim("Voice", 6, [engine, arg_int, device](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->voice_play(device(st, b), 1, arg_int(st, b, 0), arg_int(st, b, 1));
        return st[b];
    });
    prim("Voice", 7, [engine, device](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->voice_stop(device(st, b), 1);
        return st[b];
    });
    prim("Voice", 8, [engine, arg_int, device](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->voice_volume(device(st, b), 1, arg_int(st, b, 0));
        return st[b];
    });
    prim("Voice", 9, [engine](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->voice_stop_all(0);
        return st[b];
    });
    prim("Voice", 10, [engine](VM&, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        engine->voice_stop_all(1);
        return st[b];
    });
}

}  // namespace amg
