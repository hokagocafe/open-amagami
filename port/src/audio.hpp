// Sound for the port: CRI ADX music and ambience streamed from the AFS archives, mixed in
// software. Formats: docs/technical/audio.md. A C++ rendition of tools/adx.py and tools/afs.py.
#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "cpk.hpp"
#include "vm.hpp"

namespace amg {

constexpr int AUDIO_RATE = 44100;

// An AFS archive: a table of (offset, size); file data is read on demand.
struct Afs {
    std::string path;
    std::vector<std::pair<uint32_t, uint32_t>> files;
    bool open(const std::string& file);
    bool read(size_t index, std::string& out) const;
};

// One ADX file decoded block by block (encoding type 3, 4-bit samples, scale encryption).
struct AdxStream {
    std::string data;           // the header only: audio is read from the file as it is decoded
    std::ifstream file;
    std::streamoff base = 0;    // where the file starts inside its archive
    int channels = 0, block_size = 18, rate = 0;
    uint32_t samples = 0, data_offset = 0;
    bool looped = false;
    uint32_t loop_start = 0, loop_end = 0;
    int coef1 = 0, coef2 = 0;
    bool encrypted = false;
    int xor_start = 0, xor_mult = 0, xor_add = 0;

    // decoding state
    uint32_t block_index = 0;      // next block (of 32 frames) to decode
    int xor_state = 0;
    std::vector<int> h1, h2;
    std::vector<int16_t> frames;   // decoded, interleaved
    size_t cursor = 0;             // frame index inside `frames`
    uint32_t position = 0;         // frames consumed so far
    bool ended = false;

    bool load(const std::string& path, uint32_t offset, uint32_t size, const std::string& key);
    void rewind_to(uint32_t sample);
    bool next(int16_t& l, int16_t& r);  // false when the stream has ended
    void decode_block();

    // PS Vita release (D-020): music/ambience are pre-converted to plain PCM WAV (ATRAC9 has no
    // bit-exact decoder here; ffmpeg does the one-time conversion, docs/technical/vita-cpk-format.md).
    // `next()` then just walks `frames`, no block decoding; it loops on the WAV's `smpl` chunk, which
    // tools/vita_audio.py carries over from the source.
    bool pcm_only = false;
    bool load_wav(const std::string& path);
    // An ATRAC9 RIFF file (PS Vita music and ambience) decoded in memory: the same result as load_wav on its conversion.
    bool load_atrac9(const std::string& riff);
};

// A sound-effect bank (`sys_se<n>.hd` and `.bd` in MUSIC.ARC, Sony's sound bank format): every
// sample decoded from PlayStation ADPCM once, and the table that says which sample a note plays.
struct SeBank {
    struct Sample {
        std::shared_ptr<std::vector<int16_t>> pcm;
        int rate = 0;
        bool looped = false;
        size_t loop_start = 0;
    };
    std::vector<Sample> vags;
    struct Split {
        int low, high, set;
    };
    std::vector<Split> splits;       // program 0: which sample set a note plays
    std::vector<int> set_sample;     // sample set -> sample (SCEISmpl) index
    std::vector<int> sample_vag;     // sample -> vag
    std::vector<int> sample_volume;  // 0..127
    bool load(const std::string& arc_path, int bank);
    // PS Vita release: `dir` holds one cue_NNN.wav per ACB cue (tools/acb.py extract); a cue's id is
    // the note the PS2 bank plays for the same effect, so each note gets a one-note split.
    bool load_vita(const std::string& dir);
    // The same from the game's own sound bank (SYSSE0.ACB / SYSSE1.ACB; tools/acb.py's reading in C++): the cues, the
    // waveforms of the embedded wave bank, and Sony's VAG or its HE-VAG variant decoded to 16-bit PCM.
    bool load_acb(const std::string& acb_path);
    const Sample* lookup(int note, int& volume) const;
};

class AudioEngine {
public:
    explicit AudioEngine(const std::string& root);

    // Music (SOUND/MUSIC.AFS), by the game's track number.
    void music_play(int track);
    void music_stop();
    void music_pause(bool paused);
    void music_volume(int volume);  // 0..100
    bool music_busy();
    // Ambience (SOUND/SOUND.AFS).
    void ambience_play(int number);
    void ambience_stop();
    void ambience_pause(bool paused);
    void ambience_volume(int volume);

    // Sound effects: a note of the bank loaded with se_load, like the engine's MIDI note-on.
    void se_load(int bank);
    void se_play(int note, int volume);  // volume 0..100
    void se_stop();

    // Voices: one channel per speaker (the Voice object's device, which is the actor number and the
    // speaker's archive in VOICE.AFS) plus a second one for play2 (backlog and name entry).
    void voice_prepare(int speaker, int number);
    void voice_play(int speaker, int channel, int number, int volume);  // channel 0 or 1
    void voice_stop(int speaker, int channel);
    void voice_stop_all(int channel);
    void voice_volume(int speaker, int channel, int volume);
    void voice_pause(int speaker, bool paused);
    int voice_length(int speaker, int number);  // in 1/60 s
    // Lip sync: while a line plays on channel 0 for `speaker`, the mouth level 0..2 (closed, half,
    // open) from its .slp track at the current play position; -1 when nothing is playing.
    int mouth_level(int speaker);

    // Called from the audio device thread (or by the test dump): fills `frames` stereo frames.
    void render(int16_t* out, size_t frames);

private:
    struct Voice {
        std::shared_ptr<AdxStream> stream;
        double gain = 1.0;
        bool paused = false;
        double phase = 0.0;  // position between two source frames
        int16_t l0 = 0, r0 = 0, l1 = 0, r1 = 0;
        bool primed = false;
    };
    void mix(Voice& v, std::vector<float>& acc, size_t frames);
    struct EffectVoice {
        std::shared_ptr<std::vector<int16_t>> pcm;
        double position = 0.0, step = 1.0, gain = 1.0;
        bool looped = false;
        size_t loop_start = 0;
    };
    std::vector<EffectVoice> effects_;
    struct VoiceChannel {
        EffectVoice v;
        bool active = false, paused = false;
        std::shared_ptr<std::string> slp;  // lip-sync track, one byte per 400 samples
    };
    VoiceChannel voices_[2][17];
    std::shared_ptr<std::vector<int16_t>> voice_pcm(int speaker, int number);
    std::shared_ptr<std::string> voice_slp(int speaker, int number);
    bool voice_file(int speaker, int number, int kind, std::string& out);
    int vita_voice_index(int speaker, int number, int kind, bool* patched = nullptr) const;
    Afs voice_afs_;
    std::vector<uint16_t> voice_idx_, vita_voice_patch_idx_;
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> voice_tables_;  // per speaker, inner AFS table
    std::vector<std::pair<int, std::shared_ptr<std::vector<int16_t>>>> voice_cache_;  // key speaker<<16|number
    std::mutex voice_mutex_;  // guards the archives and the cache (decoding runs outside mutex_)
    SeBank bank_;
    std::string root_;

    std::mutex mutex_;
    Afs music_, sound_;
    std::vector<int> track_table_;
    Voice music_voice_, ambience_voice_;
    std::string key_ = "mituba";

    // PS Vita release (D-020): set when root/extracted/sound_wav exists (tools/vita_audio.py's
    // output, pre-converted from MUSIC.CPK/SOUND.CPK). Track number -> file index is NOT traced from
    // the Vita executable (unlike the PS2's track_table_ above) — used directly as the index into the
    // converted files (00000.wav, …), a pending hypothesis, not confirmed. See vita-cpk-format.md.
    bool vita_ = false;
    std::string vita_music_dir_, vita_ambience_dir_, vita_voice_dir_;
    // The voices straight from sound/VOICE.CPK (one archive per speaker, read when a line is first needed), used when
    // the prepared `sound/voice` folder is not there. Called with voice_mutex_ held.
    std::string vita_voice_cpk_, vita_voice_patch_cpk_, vita_music_cpk_, vita_ambience_cpk_;
    std::vector<CpkEntry> vita_music_entries_, vita_ambience_entries_;
    // A music or ambience track: the converted `sound_wav` file if it is there, else the ATRAC9 file in its CPK.
    std::shared_ptr<AdxStream> vita_track(const std::string& wav_dir, const std::string& cpk,
                                          std::vector<CpkEntry>& entries, int index);
    std::vector<CpkEntry> vita_voice_outer_;
    std::map<int, std::vector<CpkEntry>> vita_voice_inner_;
    std::vector<CpkEntry> vita_voice_patch_outer_;
    std::map<int, std::vector<CpkEntry>> vita_voice_patch_inner_;
    bool vita_voice_file(int speaker, int index, std::string& out, bool patched = false);
};

void install_audio(VM& vm, AudioEngine* engine);

}  // namespace amg
