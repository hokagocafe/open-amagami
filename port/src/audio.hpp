// Sound for the port, from the PS Vita release: ATRAC9 music and ambience, HCA voices, effects from the game's sound
// banks, all decoded in the port and mixed in software. Formats: docs/technical/vita-cpk-format.md.
#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "cpk.hpp"
#include "vm.hpp"

namespace amg {

constexpr int AUDIO_RATE = 44100;

// A music or ambience track, decoded whole (from an ATRAC9 file of the game's CPK, or a converted WAV) and walked frame by
// frame; it loops on the file's loop points.
struct AdxStream {
    int channels = 0, rate = 0;
    uint32_t samples = 0;
    bool looped = false;
    uint32_t loop_start = 0, loop_end = 0;
    std::vector<int16_t> frames;   // decoded, interleaved
    size_t cursor = 0;             // frame index inside `frames`
    uint32_t position = 0;         // frames consumed so far
    bool ended = false;

    bool next(int16_t& l, int16_t& r);  // false when the stream has ended
    // A plain PCM WAV (tools/vita_audio.py's conversion); its `smpl` chunk gives the loop.
    bool load_wav(const std::string& path);
    // An ATRAC9 RIFF file decoded in memory: the same result as load_wav on its conversion.
    bool load_atrac9(const std::string& riff);
};

// A sound-effect bank: every sample decoded once, and the table that says which sample a note plays.
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
    // `dir` holds one cue_NNN.wav per ACB cue (tools/acb.py extract); a cue's id is the note the effect plays, so each
    // note gets a one-note split.
    bool load_vita(const std::string& dir);
    // The same from the game's own sound bank (SYSSE0.ACB / SYSSE1.ACB; tools/acb.py's reading in C++): the cues, the
    // waveforms of the embedded wave bank, and Sony's VAG or its HE-VAG variant decoded to 16-bit PCM.
    bool load_acb(const std::string& acb_path);
    const Sample* lookup(int note, int& volume) const;
};

class AudioEngine {
public:
    explicit AudioEngine(const std::string& root);

    // Music (sound/MUSIC.CPK), by the game's track number.
    void set_async_tracks(bool on) { async_tracks_ = on; }  // load tracks off the caller's thread (the window)
    void music_play(int track);
    void music_stop();
    void music_pause(bool paused);
    void music_volume(int volume);  // 0..100
    bool music_busy();
    // Ambience (sound/SOUND.CPK).
    void ambience_play(int number);
    void ambience_stop();
    void ambience_pause(bool paused);
    void ambience_volume(int volume);

    // Sound effects: a note of the bank loaded with se_load, like the engine's MIDI note-on.
    void se_load(int bank);
    void se_play(int note, int volume);  // volume 0..100
    void se_stop();

    // Voices: one channel per speaker (the Voice object's device, which is the actor number and the
    // speaker's archive in VOICE.CPK) plus a second one for play2 (backlog and name entry).
    void voice_prepare(int speaker, int number);
    // Reads the voice archives' tables of every speaker on a helper thread (the first line of each speaker otherwise
    // pays for it on the game's thread, about 70 ms). The engine lives as long as the process.
    void warm_voices();
    void voice_play(int speaker, int channel, int number, int volume);  // channel 0 or 1
    void voice_stop(int speaker, int channel);
    void voice_stop_all(int channel);
    void voice_volume(int speaker, int channel, int volume);
    void voice_pause(int speaker, bool paused);
    int voice_length(int speaker, int number);  // in 1/60 s
    // Lip sync: while a line plays on channel 0 for `speaker`, the mouth level 0..2 (closed, half,
    // open) from its lip-sync track at the current play position; -1 when nothing is playing.
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
    // One channel per device: the Vita clamps a device to 37 (0x810071CC), so devices 0 to 37.
    static constexpr int VOICE_DEVICES = 38;
    VoiceChannel voices_[2][VOICE_DEVICES];
    // Devices 15 to 37 all read speaker 15's archive, line n + T[d-15], back to T[d-15] at T[d-14]: the table
    // at 0x810A85F4 in the Vita executable and the word after it (d1-audio-stage.md A5).
    int64_t voice_offsets_[24] = {};
    bool voice_offsets_ok_ = false;
    // The (speaker, line) a device's line `number` plays; false when the device has none.
    bool voice_source(int device, int number, int& speaker, int& line) const;
    std::shared_ptr<std::vector<int16_t>> voice_pcm(int speaker, int number);
    std::shared_ptr<std::string> voice_slp(int speaker, int number);
    int vita_voice_index(int speaker, int number, int kind, bool* patched = nullptr) const;
    std::vector<uint16_t> voice_idx_, vita_voice_patch_idx_;
    std::vector<std::pair<int, std::shared_ptr<std::vector<int16_t>>>> voice_cache_;  // key speaker<<16|number
    std::mutex voice_mutex_;  // guards the archives, the cache and the lines being decoded (decoding itself runs outside it)
    std::map<int, std::shared_future<std::shared_ptr<std::vector<int16_t>>>> voice_inflight_;  // lines being decoded, by key
    SeBank bank_;
    int bank_no_ = -1;                 // which bank bank_ is
    std::map<int, SeBank> shelf_;      // banks loaded ahead (warm_voices), taken by se_load without reading the files
    bool load_se_bank(int bank, SeBank& out);
    std::string root_;

    std::mutex mutex_;
    std::vector<int> track_table_;
    Voice music_voice_, ambience_voice_;
    // Vita tracks are whole decoded files (a few hundred milliseconds for the title music, more for a long one). In the
    // window they load on a thread of their own, so the game does not stall and the sound does not run dry; a newer
    // request, or a stop, makes an older load's result be dropped. Off for the command line tool, whose renders must
    // not depend on timing.
    std::atomic<unsigned> music_request_{0}, ambience_request_{0};
    std::atomic<bool> music_loading_{false}, ambience_loading_{false};
    bool music_pause_asked_ = false;  // a pause that the script asked for after the last play: the track is still loading when the script pauses it (under mutex_)
    bool async_tracks_ = false;

    // Track number -> file index: read from the Vita executable (found at 0x810a818c), used as the index into the
    // converted files (00000.wav, ...) or the CPK.
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
