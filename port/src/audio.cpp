#include "audio.hpp"

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
#include <chrono>
#include <thread>

namespace amg {

namespace {

uint32_t le32(const std::string& d, size_t at) {
    if (at + 4 > d.size()) return 0;
    return uint32_t(uint8_t(d[at])) | (uint32_t(uint8_t(d[at + 1])) << 8) | (uint32_t(uint8_t(d[at + 2])) << 16) |
           (uint32_t(uint8_t(d[at + 3])) << 24);
}

}  // namespace


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
    return true;
}

bool AdxStream::next(int16_t& l, int16_t& r) {
    if (ended) return false;
    if (looped && position >= loop_end) {
        cursor = size_t(loop_start);
        position = loop_start;
    }
    if (cursor >= frames.size() / size_t(channels)) {
        if (!looped) {
            ended = true;
            return false;
        }
        cursor = size_t(loop_start);
        position = loop_start;
    }
    l = frames[cursor * size_t(channels)];
    r = channels > 1 ? frames[cursor * size_t(channels) + 1] : l;
    ++cursor;
    ++position;
    return true;
}

// ---------------------------------------------------------------------------------------------

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
    // Track number -> index in the converted tracks, found in the Vita executable (F4-40, vita-cpk-format.md) by
    // following the one reference to the literal path "app0:./sound/MUSIC.CPK" to the table lookup two calls up the
    // chain. 145 entries, -1 (and garbage past the 145th) for none.
    std::string exe = amg::vita_exe(root);
    {
        constexpr uint32_t TABLE_VA = 0x810a818c;
        size_t at = amg::vita_va_to_off(exe, TABLE_VA, "music table");
        for (int i = 0; i < 145 && at + 4 <= exe.size(); ++i, at += 4) {
            const unsigned char* b = reinterpret_cast<const unsigned char*>(exe.data()) + at;
            track_table_.push_back(int32_t(uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) |
                                           (uint32_t(b[3]) << 24)));
        }
    }
    {
        // Line offsets of the voice devices 15 to 37 (d1-audio-stage.md A5): 23 words and the one after them,
        // which device 37 reads as its upper bound.
        constexpr uint32_t OFFSETS_VA = 0x810a85f4;
        size_t at = amg::vita_va_to_off(exe, OFFSETS_VA, "voice line offsets");
        if (at + 4 * 24 <= exe.size()) {
            for (int i = 0; i < 24; ++i, at += 4) {
                const unsigned char* b = reinterpret_cast<const unsigned char*>(exe.data()) + at;
                voice_offsets_[i] = int32_t(uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) |
                                            (uint32_t(b[3]) << 24));
            }
            voice_offsets_ok_ = true;
        }
    }
    // The voices as VOICE.CPK holds them (one folder per speaker of its inner archives, files by index: HCA
    // lines and their lip-sync tracks), decoded when a line is first needed. AMG_VITA_VOICE_DIR moves them.
    const char* voice_dir = std::getenv("AMG_VITA_VOICE_DIR");
    vita_voice_dir_ = voice_dir && *voice_dir ? std::string(voice_dir) : amg::vita_voice_directory(root);
    vita_voice_cpk_ = amg::data_file(root, "sound/VOICE.CPK");
    vita_voice_patch_cpk_ = amg::data_file(root, "sound/VOICE_P.CPK");
    // VOICE.IDX (speaker, line) -> an index into that speaker's own archive (F4-41, vita-cpk-format.md). It maps
    // directly onto the per-speaker CPKs' sequential file IDs (checked against speaker 0: 73 indices, 0 to 72, no gaps,
    // matching Voice00.cpk's 73 files exactly).
    auto read_voice_index = [](const std::string& path) {
        std::vector<uint16_t> index;
        std::ifstream idx(path, std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(idx)), std::istreambuf_iterator<char>());
        for (size_t i = 0; i + 1 < raw.size(); i += 2)
            index.push_back(uint16_t(uint8_t(raw[i]) | (uint8_t(raw[i + 1]) << 8)));
        return index;
    };
    voice_idx_ = read_voice_index(amg::data_file(root, "sound/VOICE.IDX"));
    vita_voice_patch_idx_ = read_voice_index(amg::data_file(root, "sound/VOICE_P.IDX"));
    music_voice_.gain = 1.0;
    ambience_voice_.gain = 1.0;
    se_load(0);  // the first effect bank, loaded from the start; only the mahjong game loads another
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
    {
        const bool known = track >= 0 && size_t(track) < track_table_.size() && track_table_[size_t(track)] >= 0;
        // Reads the whole track first (15 MB or so) and decodes it, outside the mixer's lock so the sound thread keeps mixing.
        auto load = [this, track, known]() -> std::shared_ptr<AdxStream> {
            if (!known) return nullptr;
            const auto started = std::chrono::steady_clock::now();
            auto s = vita_track(vita_music_dir_, vita_music_cpk_, vita_music_entries_, track_table_[size_t(track)]);
            if (std::getenv("AMG_VOICE_LOG"))
                std::fprintf(stderr, "music play track=%d file=%d %s (%lld ms)\n", track, track_table_[size_t(track)],
                             s ? "ok" : "missing",
                             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
            return s;
        };
        auto start = [this](const std::shared_ptr<AdxStream>& s) {
            music_voice_.stream = s;
            if (!s) return;
            // The title's music is asked for at the first logo and paused at once (the script resumes it at the title: pause(1)
            // at tick 75, pause(0) at tick 780); the track loads meanwhile, so the pause arrives before it starts and must stay.
            music_voice_.paused = music_pause_asked_;
            if (std::getenv("AMG_VOICE_LOG")) std::fprintf(stderr, "music ready, paused=%d\n", int(music_voice_.paused));
            music_voice_.primed = false;
            music_voice_.phase = 0.0;
        };
        const unsigned request = ++music_request_;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            music_pause_asked_ = false;  // a new play starts running; a pause asked after it is kept for the start
        }
        if (!async_tracks_) {
            auto s = load();
            std::lock_guard<std::mutex> lk(mutex_);
            start(s);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mutex_);
            music_voice_.stream.reset();  // the old track stops now; the new one starts when it is ready
        }
        if (!known) {
            music_loading_ = false;
            return;
        }
        music_loading_ = true;
        std::thread([this, load, start, request] {
            auto s = load();
            std::lock_guard<std::mutex> lk(mutex_);
            if (request != music_request_) return;  // asked again, or stopped, meanwhile
            start(s);
            music_loading_ = false;
        }).detach();
        return;
    }
}

void AudioEngine::music_stop() {
    std::lock_guard<std::mutex> lk(mutex_);
    ++music_request_;
    music_loading_ = false;
    music_voice_.stream.reset();
}
void AudioEngine::music_pause(bool paused) {
    std::lock_guard<std::mutex> lk(mutex_);
    music_pause_asked_ = paused;
    music_voice_.paused = paused;
}
void AudioEngine::music_volume(int volume) {
    std::lock_guard<std::mutex> lk(mutex_);
    // FIDELITY: read 0x810057C6 (the Vita's Music.setVolumePrim): volume * 0.008 to the player, no clamp, so 100 is 0.8;
    // voices and the ambience take volume * 0.01 (0x8107DB28, 0x81006F8A), effects volume * 0.006 (0x81006EBC).
    music_voice_.gain = std::max(0, volume) * 0.008;
}
bool AudioEngine::music_busy() {
    std::lock_guard<std::mutex> lk(mutex_);
    return music_loading_ || (music_voice_.stream && !music_voice_.stream->ended);
}

void AudioEngine::ambience_play(int number) {
    {
        auto load = [this, number]() -> std::shared_ptr<AdxStream> {
            if (number < 1) return nullptr;
            return vita_track(vita_ambience_dir_, vita_ambience_cpk_, vita_ambience_entries_, number - 1);  // ENV0001 is entry 0 (hypothesis)
        };
        auto start = [this](const std::shared_ptr<AdxStream>& s) {
            ambience_voice_.stream = s;
            if (!s) return;
            ambience_voice_.paused = false;
            ambience_voice_.primed = false;
            ambience_voice_.phase = 0.0;
        };
        const unsigned request = ++ambience_request_;
        if (!async_tracks_) {
            auto s = load();
            std::lock_guard<std::mutex> lk(mutex_);
            start(s);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mutex_);
            ambience_voice_.stream.reset();
        }
        if (number < 1) return;
        ambience_loading_ = true;
        std::thread([this, load, start, request] {
            auto s = load();
            std::lock_guard<std::mutex> lk(mutex_);
            if (request != ambience_request_) return;
            start(s);
            ambience_loading_ = false;
        }).detach();
        return;
    }
}

void AudioEngine::ambience_stop() {
    std::lock_guard<std::mutex> lk(mutex_);
    ++ambience_request_;
    ambience_loading_ = false;
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

bool AudioEngine::load_se_bank(int bank, SeBank& out) {
    bool ok = out.load_vita(root_ + "/extracted/sound_wav/se" + std::to_string(bank)) ||
              out.load_acb(amg::data_file(root_, "sound/SYSSE" + std::to_string(bank) + ".ACB"));
    if (std::getenv("AMG_VOICE_LOG")) std::fprintf(stderr, "se load bank=%d %s (%zu samples)\n", bank, ok ? "ok" : "failed", out.vags.size());
    return ok;
}

// The bank in use goes to the shelf and the asked one comes off it (or is read, if warm_voices has not got to it): reading
// the second bank took 35 to 45 ms, in the middle of a frame, the first time the mahjong game was opened.
void AudioEngine::se_load(int bank) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (bank == bank_no_) {  // the same bank again: nothing to read
            effects_.clear();
            return;
        }
        auto shelved = shelf_.find(bank);
        if (shelved != shelf_.end()) {
            if (bank_no_ >= 0) shelf_[bank_no_] = std::move(bank_);
            bank_ = std::move(shelf_[bank]);
            shelf_.erase(bank);
            bank_no_ = bank;
            effects_.clear();
            return;
        }
    }
    SeBank loaded;
    if (!load_se_bank(bank, loaded)) return;
    std::lock_guard<std::mutex> lk(mutex_);
    if (bank_no_ >= 0 && bank_no_ != bank) shelf_[bank_no_] = std::move(bank_);
    bank_ = std::move(loaded);
    bank_no_ = bank;
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
    v.gain = std::max(0, volume) * 0.006 * (sample_volume / 127.0);
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
// (docs/technical/vita-cpk-format.md). Decoded lines are kept in a small cache.
std::shared_ptr<std::vector<int16_t>> AudioEngine::voice_pcm(int speaker, int number) {
    if (speaker < 0 || speaker >= 16 || number < 0 || number >= 10000) return nullptr;
    int key = (speaker << 16) | number;
    std::promise<std::shared_ptr<std::vector<int16_t>>> done;
    std::shared_future<std::shared_ptr<std::vector<int16_t>>> waiting;
    std::string file;
    {
        std::unique_lock<std::mutex> lk(voice_mutex_);
        for (auto& [k, pcm] : voice_cache_)
            if (k == key) return pcm;
        auto running = voice_inflight_.find(key);
        if (running != voice_inflight_.end()) {  // another thread is decoding this line: wait for it, not for the lock
            waiting = running->second;
        } else {
            bool patched = false;
            int index = vita_voice_index(speaker, number, 1, &patched);
            if (index < 0 || !vita_voice_file(speaker, index, file, patched)) return nullptr;
            voice_inflight_[key] = done.get_future().share();
        }
    }
    if (waiting.valid()) return waiting.get();
    // The decoding (tens of milliseconds) is outside the lock: a line asked for meanwhile does not wait for this one.
    auto pcm = std::make_shared<std::vector<int16_t>>();
    int rate = 0, channels = 0;
    if (!decode_hca(file, *pcm, rate, channels) || channels != 1) pcm = nullptr;
    {
        std::lock_guard<std::mutex> lk(voice_mutex_);
        if (pcm) {
            if (voice_cache_.size() >= 32) voice_cache_.erase(voice_cache_.begin());
            voice_cache_.emplace_back(key, pcm);
        }
        voice_inflight_.erase(key);
    }
    done.set_value(pcm);
    return pcm;
}

bool AudioEngine::vita_voice_file(int speaker, int index, std::string& out, bool patched) {
    char path[64];
    std::snprintf(path, sizeof path, "/speaker_%02d/%05d.bin", speaker, index);
    if (!patched) {
        std::ifstream f(vita_voice_dir_ + path, std::ios::binary);
        if (f) {
            out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            return true;
        }
    }
    std::error_code ec;
    const std::string& cpk = patched ? vita_voice_patch_cpk_ : vita_voice_cpk_;
    if (cpk.empty() || !std::filesystem::is_regular_file(cpk, ec)) return false;
    try {
        auto& outer = patched ? vita_voice_patch_outer_ : vita_voice_outer_;
        auto& inner = patched ? vita_voice_patch_inner_ : vita_voice_inner_;
        if (outer.empty()) outer = cpk_read_entries(cpk);
        if (speaker < 0 || size_t(speaker) >= outer.size()) return false;
        auto it = inner.find(speaker);
        if (it == inner.end())
            it = inner.emplace(speaker, cpk_read_entries(cpk, outer[size_t(speaker)].offset)).first;
        if (index < 0 || size_t(index) >= it->second.size()) return false;
        const CpkEntry& e = it->second[size_t(index)];
        std::ifstream f(cpk, std::ios::binary);
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
    {
        // Kept like the lines (voice_prepare reads both ahead); voice_mutex_ is held.
        static std::vector<std::pair<int, std::shared_ptr<std::string>>> cache;
        int key = (speaker << 16) | number;
        for (auto& [k, cached] : cache)
            if (k == key) return cached;
        bool patched = false;
        int index = vita_voice_index(speaker, number, 0, &patched);
        if (index < 0) return nullptr;
        if (!vita_voice_file(speaker, index, *slp, patched)) return nullptr;
        if (cache.size() >= 32) cache.erase(cache.begin());
        cache.emplace_back(key, slp);
        return slp;
    }
}

// VOICE.IDX, (speaker, line, kind) -> index into the speaker's own archive; kind 0 is the line's
// lip-sync track, 1 its audio. -1 when the line has none.
int AudioEngine::vita_voice_index(int speaker, int number, int kind, bool* patched) const {
    if (speaker < 0 || speaker >= 16 || number < 0 || number >= 10000) return -1;
    size_t group_at = size_t(speaker) * 100 + size_t(number / 100);
    const auto lookup = [group_at, number, kind](const std::vector<uint16_t>& index) {
        if (group_at >= index.size() || index[group_at] == 0xFFFF) return -1;
        size_t file_at = 1600 + size_t(index[group_at]) * 200 + size_t(number % 100) * 2 + size_t(kind);
        if (file_at >= index.size() || index[file_at] == 0xFFFF) return -1;
        return int(index[file_at]);
    };
    std::error_code ec;
    if (!vita_voice_patch_idx_.empty() && std::filesystem::is_regular_file(vita_voice_patch_cpk_, ec)) {
        const int patch_index = lookup(vita_voice_patch_idx_);
        if (patch_index >= 0) {
            if (patched) *patched = true;
            return patch_index;
        }
    }
    if (patched) *patched = false;
    return lookup(voice_idx_);
}

// A device's line on the Vita (0x810071CC and the Voice primitives at 0x810072DC to 0x8100742C): the device is clamped
// to 37; devices below 15 are their own speaker, devices 15 to 37 read speaker 15's archive at n + T[d-15], and a
// line at or past T[d-14] becomes T[d-15]. Device 26 (T[11] = 8800 > T[12]) always lands on 8800, a line speaker 15
// does not have, so it is silent, as on the Vita. Device 37 would read its bound from the word after the table
// (pending, d1-leftovers.md 4.3); it is kept silent.
bool AudioEngine::voice_source(int device, int number, int& speaker, int& line) const {
    if (device < 0 || number < 0) return false;  // a negative line would land on the previous device's lines (review 2026-10-30)
    const int d = std::min(device, VOICE_DEVICES - 1);
    if (d < 15) {
        speaker = d;
        line = number;
        return true;
    }
    if (!voice_offsets_ok_ || d == 37) return false;
    int64_t n = int64_t(number) + voice_offsets_[d - 15];
    if (n >= voice_offsets_[d - 14]) n = voice_offsets_[d - 15];
    if (n < 0 || n >= 10000) return false;
    speaker = 15;
    line = int(n);
    return true;
}

void AudioEngine::voice_prepare(int device, int number) {
    if (std::getenv("AMG_VOICE_LOG")) std::fprintf(stderr, "voice setup device=%d number=%d\n", device, number);
    int speaker = 0;
    if (!voice_source(device, number, speaker, number)) return;
    {
        // Read on a helper thread so the game does not wait on the disk (the Vita's voices are plain WAV files,
        // possibly on a slow disk, AMG_VITA_VOICE_DIR): the line and its lip-sync track land in the cache, and
        // a play that comes first just waits for the rest of the read. The engine lives as long as the process.
        std::thread([this, speaker, number] {
            voice_pcm(speaker, number);
            voice_slp(speaker, number);
        }).detach();
    }
}

void AudioEngine::warm_voices() {
    std::thread([this] {
        for (int bank = 0; bank < 4; ++bank) {  // the other effect banks, ready for se_load
            {
                std::lock_guard<std::mutex> lk(mutex_);
                if (bank == bank_no_ || shelf_.count(bank)) continue;
            }
            SeBank loaded;
            if (!load_se_bank(bank, loaded)) continue;
            std::lock_guard<std::mutex> lk(mutex_);
            if (bank != bank_no_ && !shelf_.count(bank)) shelf_[bank] = std::move(loaded);
        }
        for (int speaker = 0; speaker < 16; ++speaker) {
            std::lock_guard<std::mutex> lk(voice_mutex_);  // one speaker at a time: a line asked for meanwhile waits for at most one
            std::string unused;
            vita_voice_file(speaker, 0, unused, false);
            if (!vita_voice_patch_idx_.empty()) vita_voice_file(speaker, 0, unused, true);
        }
    }).detach();
}

void AudioEngine::voice_play(int device, int channel, int number, int volume) {
    static const bool log = std::getenv("AMG_VOICE_LOG") != nullptr;
    if (log) std::fprintf(stderr, "voice play device=%d channel=%d number=%d volume=%d\n", device, channel, number, volume);
    if (device < 0) return;
    int speaker = -1, line = -1;
    std::shared_ptr<std::vector<int16_t>> pcm;
    std::shared_ptr<std::string> slp;
    if (voice_source(device, number, speaker, line)) {
        pcm = voice_pcm(speaker, line);
        if ((channel & 1) == 0) slp = voice_slp(speaker, line);
    }
    if (log) std::fprintf(stderr, "  -> speaker %d line %d, %zu samples\n", speaker, line, pcm ? pcm->size() : size_t(0));
    std::lock_guard<std::mutex> lk(mutex_);
    VoiceChannel& c = voices_[channel & 1][std::min(device, VOICE_DEVICES - 1)];
    c.active = pcm && !pcm->empty();
    c.paused = false;
    if (!c.active) return;
    c.v = EffectVoice();
    c.v.pcm = pcm;
    c.slp = slp;
    // The HCA voices decode at 48000 Hz (confirmed on real files, docs/technical/vita-cpk-format.md).
    c.v.step = 48000.0 / double(AUDIO_RATE);
    c.v.gain = std::max(0, std::min(100, volume)) / 100.0;
}

void AudioEngine::voice_stop(int speaker, int channel) {
    if (std::getenv("AMG_VOICE_LOG")) std::fprintf(stderr, "voice stop speaker=%d channel=%d\n", speaker, channel);
    std::lock_guard<std::mutex> lk(mutex_);
    if (speaker >= 0) voices_[channel & 1][std::min(speaker, VOICE_DEVICES - 1)].active = false;
}

void AudioEngine::voice_stop_all(int channel) {
    if (std::getenv("AMG_VOICE_LOG")) std::fprintf(stderr, "voice stop all channel=%d\n", channel);
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& c : voices_[channel & 1]) c.active = false;
}

void AudioEngine::voice_volume(int speaker, int channel, int volume) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (speaker >= 0)
        voices_[channel & 1][std::min(speaker, VOICE_DEVICES - 1)].v.gain = std::max(0, std::min(100, volume)) / 100.0;
}

void AudioEngine::voice_pause(int speaker, bool paused) {
    if (std::getenv("AMG_VOICE_LOG")) std::fprintf(stderr, "voice pause speaker=%d paused=%d\n", speaker, int(paused));
    std::lock_guard<std::mutex> lk(mutex_);
    if (speaker >= 0) voices_[0][std::min(speaker, VOICE_DEVICES - 1)].paused = paused;
}

int AudioEngine::mouth_level(int speaker) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (speaker < 0) return -1;
    const VoiceChannel& c = voices_[0][std::min(speaker, VOICE_DEVICES - 1)];
    if (!c.active || !c.slp) return -1;
    // One .slp byte per 1/60 s: 800 samples of the 48 kHz HCA.
    size_t at = size_t(c.v.position) / 800;
    if (at >= c.slp->size()) return -1;
    return uint8_t((*c.slp)[at]) & 3;
}

// The Vita answers max(slp bytes - 18, 12) (0x810073D4; binder method 0x24 is the lip-sync track's byte size, one
// byte per 1/60 s, which is the line's length in frames: d1-leftovers.md 4.2), and 0 without a line.
int AudioEngine::voice_length(int device, int number) {
    int speaker = 0, line = 0;
    if (!voice_source(device, number, speaker, line)) return 0;
    auto slp = voice_slp(speaker, line);
    return slp ? std::max(int(slp->size()) - 18, 12) : 0;
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
    prim("Music", 1, [engine, arg_int](VM& vm, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (std::getenv("AMG_VOICE_LOG")) std::fprintf(stderr, "music asked: track %lld at tick %lld\n", (long long)arg_int(st, b, 0), (long long)vm.tick);
        engine->music_play(arg_int(st, b, 0));
        return st[b];
    });
    prim("Music", 2, [engine](VM& vm, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (std::getenv("AMG_VOICE_LOG")) std::fprintf(stderr, "music asked: stop at tick %lld\n", (long long)vm.tick);
        engine->music_stop();
        return st[b];
    });
    prim("Music", 3, [engine, truthy](VM& vm, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (std::getenv("AMG_VOICE_LOG")) std::fprintf(stderr, "music asked: pause %d at tick %lld\n", int(truthy(st.at(b + 1))), (long long)vm.tick);
        engine->music_pause(truthy(st.at(b + 1)));
        return st[b];
    });
    prim("Music", 4, [engine, arg_int](VM& vm, Thread&, std::vector<Value>& st, size_t b, Method&) -> Value {
        if (std::getenv("AMG_VOICE_LOG")) std::fprintf(stderr, "music asked: volume %lld at tick %lld\n", (long long)arg_int(st, b, 0), (long long)vm.tick);
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
