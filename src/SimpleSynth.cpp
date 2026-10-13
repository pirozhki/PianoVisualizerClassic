#include "SimpleSynth.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <thread>
#include <chrono>
#include <vector>

#pragma comment(lib, "xaudio2.lib")

namespace {

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kTwoPi = 2.0 * kPi;
constexpr int kPartialCount = 8;
constexpr int kStringCount = 3;
constexpr double kWaveAttackSeconds = 0.010;
constexpr double kFastDecayMix = 0.58;
constexpr double kSlowDecayMix = 0.42;

// Keep the fundamental prominent and suppress upper partials. Upper partials
// are attenuated further as pitch rises, where they had become too conspicuous.
constexpr std::array<double, kPartialCount> kPartialLevel = {
    0.72, 0.105, 0.028, 0.0080, 0.0020, 0.00050, 0.00013, 0.00003
};
constexpr std::array<double, kPartialCount> kPartialFastDecay = {
    2.10, 2.60, 3.15, 3.75, 4.35, 5.00, 5.75, 6.50
};
constexpr std::array<double, kPartialCount> kPartialSlowDecay = {
    1.05, 1.40, 1.80, 2.20, 2.65, 3.15, 3.75, 4.40
};
constexpr std::array<double, kPartialCount> kPartialPhase = {
    0.00, 0.37, 1.11, 2.03, 2.77, 4.19, 0.91, 3.47
};
constexpr std::array<double, kStringCount> kStringPhase = {0.0, 0.21, 0.43};

float SmoothStep(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

int PianoStringCount(int midiNote) {
    // Approximate the usual single-string bass, bichord, and trichord ranges.
    if (midiNote < 28) return 1;
    if (midiNote < 52) return 2;
    return 3;
}

double StringDetune(int midiNote, int stringIndex) {
    const int count = PianoStringCount(midiNote);
    if (count <= 1) return 0.0;

    double detune = 0.0;
    if (count == 2) {
        detune = stringIndex == 0 ? -0.00035 : 0.00035;
    } else {
        constexpr std::array<double, kStringCount> detuneByString = {
            -0.00095, 0.0, 0.00080
        };
        detune = detuneByString[static_cast<std::size_t>(stringIndex)];
    }

    // Keep the slight unison beating in the bass/midrange, but make it much
    // less noticeable in the treble, where it can sound like ringing/chorus.
    const double highRegister = std::clamp((static_cast<double>(midiNote) - 48.0) / 60.0, 0.0, 1.0);
    return detune * (1.0 - 0.90 * highRegister);
}

double StringWeight(int midiNote, int stringIndex) {
    const int count = PianoStringCount(midiNote);
    if (count <= 1) return stringIndex == 0 ? 1.0 : 0.0;
    if (count == 2) return stringIndex < 2 ? 0.5 : 0.0;
    constexpr std::array<double, kStringCount> weights = {0.28, 0.44, 0.28};
    return weights[static_cast<std::size_t>(stringIndex)];
}

double StringInharmonicity(int midiNote) {
    // A small, pitch-dependent stiffness term. This is an inexpensive
    // approximation of piano-string inharmonicity, not a full string solver.
    const double pitch = std::clamp(static_cast<double>(midiNote) / 127.0, 0.0, 1.0);
    return 0.00002 + 0.00018 * std::pow(pitch, 1.6);
}

double PartialFrequencyFromStiffness(double fundamental, double stiffness, int partialIndex) {
    const double order = static_cast<double>(partialIndex + 1);
    return fundamental * order * std::sqrt(
        (1.0 + stiffness * order * order) / (1.0 + stiffness));
}

double PartialFrequency(double fundamental, int midiNote, int partialIndex) {
    return PartialFrequencyFromStiffness(
        fundamental, StringInharmonicity(midiNote), partialIndex);
}

double PartialLevelForNote(int midiNote, int partialIndex) {
    const double level = kPartialLevel[static_cast<std::size_t>(partialIndex)];
    if (partialIndex == 0) return level; // Keep the fundamental level unchanged.

    // Progressively darken the tone toward the treble. At MIDI 108 and above,
    // upper partials are only 5% of their already reduced base levels.
    const double highRegister = std::clamp((static_cast<double>(midiNote) - 48.0) / 60.0, 0.0, 1.0);
    const double upperPartialScale = std::pow(0.05, highRegister);
    return level * upperPartialScale;
}

double PartialEnvelope(int partialIndex, double time, int midiNote) {
    const double fast = std::exp(-kPartialFastDecay[static_cast<std::size_t>(partialIndex)] * time);
    const double slow = std::exp(-kPartialSlowDecay[static_cast<std::size_t>(partialIndex)] * time);
    return PartialLevelForNote(midiNote, partialIndex) *
        (kFastDecayMix * fast + kSlowDecayMix * slow);
}

float VelocityFilterAlpha(int velocity) {
    // XAudio2's one-pole low-pass takes its coefficient directly in Frequency.
    // Soft strokes get a lower coefficient (warmer tone); hard strokes keep
    // more upper partials. The same coefficient is used by the offline renderer.
    const float v = static_cast<float>(std::clamp(velocity, 1, 127)) / 127.0f;
    return 0.36f + 0.58f * std::pow(v, 0.85f);
}

double SoundingEnd(const MidiNote& note) {
    // Preserve compatibility with notes constructed outside the MIDI parser.
    return std::max(note.end, note.soundingEnd);
}

} // namespace

bool SimpleSynth::Initialize(std::wstring& error) {
    if (IsAvailable()) return true;

    HRESULT hr = XAudio2Create(
        xaudio_.ReleaseAndGetAddressOf(),
        0,
        XAUDIO2_DEFAULT_PROCESSOR);

    if (FAILED(hr)) {
        error = L"XAudio2Create failed.";
        xaudio_.Reset();
        return false;
    }

    hr = xaudio_->CreateMasteringVoice(
        &masteringVoice_, XAUDIO2_DEFAULT_CHANNELS, kSampleRate,
        0, nullptr, nullptr);

    if (FAILED(hr)) {
        error = L"Failed to create the XAudio2 mastering voice.";
        xaudio_.Reset();
        return false;
    }

    GenerateNoteBuffers();

    if (!CreateVoices()) {
        error = L"Failed to create XAudio2 source voices.";
        ReleaseVoices();
        masteringVoice_->DestroyVoice();
        masteringVoice_ = nullptr;
        xaudio_.Reset();
        return false;
    }

    error.clear();
    return true;
}

void SimpleSynth::Shutdown() {
    Stop();
    ReleaseVoices();

    if (masteringVoice_) {
        masteringVoice_->DestroyVoice();
        masteringVoice_ = nullptr;
    }
    xaudio_.Reset();
}

void SimpleSynth::GenerateNoteBuffers() {
    const int waveSamples = static_cast<int>(kBufferSeconds * kSampleRate);

    for (auto& buffer : noteBuffers_) buffer.clear();

    for (int baseIndex = 0; baseIndex < kBaseCount; ++baseIndex) {
        const int midiNote = std::min(127, baseIndex * kBaseStep);
        auto& buffer = noteBuffers_[static_cast<std::size_t>(baseIndex)];
        buffer.resize(static_cast<std::size_t>(waveSamples));

        const double frequency = NoteFrequency(midiNote);
        std::array<float, kPartialCount * kStringCount> sinPhase{};
        std::array<float, kPartialCount * kStringCount> cosPhase{};
        std::array<float, kPartialCount * kStringCount> sinDelta{};
        std::array<float, kPartialCount * kStringCount> cosDelta{};
        std::array<float, kPartialCount> fastEnvelope{};
        std::array<float, kPartialCount> slowEnvelope{};
        std::array<float, kPartialCount> fastEnvelopeDecay{};
        std::array<float, kPartialCount> slowEnvelopeDecay{};
        const int strings = PianoStringCount(midiNote);
        std::array<float, kStringCount> stringWeights{};
        for (int stringIndex = 0; stringIndex < strings; ++stringIndex) {
            stringWeights[static_cast<std::size_t>(stringIndex)] =
                static_cast<float>(StringWeight(midiNote, stringIndex));
        }

        for (int p = 0; p < kPartialCount; ++p) {
            const float partialLevel = static_cast<float>(PartialLevelForNote(midiNote, p));
            fastEnvelope[p] = partialLevel;
            slowEnvelope[p] = partialLevel;
            fastEnvelopeDecay[p] = static_cast<float>(
                std::exp(-kPartialFastDecay[p] / static_cast<double>(kSampleRate)));
            slowEnvelopeDecay[p] = static_cast<float>(
                std::exp(-kPartialSlowDecay[p] / static_cast<double>(kSampleRate)));

            const double partialFrequency = PartialFrequency(frequency, midiNote, p);
            for (int stringIndex = 0; stringIndex < strings; ++stringIndex) {
                const std::size_t index = static_cast<std::size_t>(p * kStringCount + stringIndex);
                const double detunedFrequency = partialFrequency * (1.0 + StringDetune(midiNote, stringIndex));
                // Base buffers are later pitch-shifted by at most a few semitones.
                // Keep a little more Nyquist headroom than a direct note renderer.
                if (detunedFrequency > 0.40 * static_cast<double>(kSampleRate)) {
                    sinPhase[index] = cosPhase[index] = 0.0f;
                    sinDelta[index] = cosDelta[index] = 0.0f;
                    continue;
                }

                const double omega = kTwoPi * detunedFrequency / static_cast<double>(kSampleRate);
                const double phase = kPartialPhase[p] + kStringPhase[stringIndex] * (p + 1);
                sinDelta[index] = static_cast<float>(std::sin(omega));
                cosDelta[index] = static_cast<float>(std::cos(omega));
                sinPhase[index] = static_cast<float>(std::sin(phase));
                cosPhase[index] = static_cast<float>(std::cos(phase));
            }
        }

        for (int i = 0; i < waveSamples; ++i) {
            const double t = static_cast<double>(i) / kSampleRate;
            const float attack = SmoothStep(static_cast<float>(t / kWaveAttackSeconds));
            float sample = 0.0f;

            for (int p = 0; p < kPartialCount; ++p) {
                const float envelope = static_cast<float>(kFastDecayMix) * fastEnvelope[p] +
                    static_cast<float>(kSlowDecayMix) * slowEnvelope[p];

                for (int stringIndex = 0; stringIndex < strings; ++stringIndex) {
                    const std::size_t index = static_cast<std::size_t>(p * kStringCount + stringIndex);
                    sample += envelope * stringWeights[static_cast<std::size_t>(stringIndex)] * sinPhase[index];

                    const float oldSin = sinPhase[index];
                    sinPhase[index] = oldSin * cosDelta[index] + cosPhase[index] * sinDelta[index];
                    cosPhase[index] = cosPhase[index] * cosDelta[index] - oldSin * sinDelta[index];
                }

                fastEnvelope[p] *= fastEnvelopeDecay[p];
                slowEnvelope[p] *= slowEnvelopeDecay[p];
            }

            // Avoid nonlinear saturation that can make the attack sound buzzy.
            sample *= attack * 0.78f;
            sample = std::clamp(sample, -1.0f, 1.0f);
            buffer[static_cast<std::size_t>(i)] = static_cast<int16_t>(
                std::lrint(sample * 32767.0f));
        }
    }
}

bool SimpleSynth::CreateVoices() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    for (Voice& voice : voices_) {
        const HRESULT hr = xaudio_->CreateSourceVoice(
            &voice.source, &format, XAUDIO2_VOICE_USEFILTER, XAUDIO2_DEFAULT_FREQ_RATIO,
            nullptr, nullptr, nullptr);
        if (FAILED(hr)) return false;
        voice.noteIndex = -1;
        voice.noteEnd = 0.0;
        voice.volume = 0.0f;
        voice.releasing = false;
        voice.attackStart = 0.0;
        voice.releaseStart = 0.0;
    }
    return true;
}

void SimpleSynth::ReleaseVoices() {
    for (Voice& voice : voices_) {
        if (voice.source) {
            voice.source->Stop(0, XAUDIO2_COMMIT_NOW);
            voice.source->FlushSourceBuffers();
            voice.source->DestroyVoice();
            voice.source = nullptr;
        }
        voice.noteIndex = -1;
        voice.noteEnd = 0.0;
        voice.volume = 0.0f;
        voice.releasing = false;
        voice.attackStart = 0.0;
        voice.releaseStart = 0.0;
    }
}

void SimpleSynth::ResetVoices() {
    for (Voice& voice : voices_) {
        if (!voice.source) continue;
        voice.source->Stop(0, XAUDIO2_COMMIT_NOW);
        voice.source->FlushSourceBuffers();
        voice.noteIndex = -1;
        voice.noteEnd = 0.0;
        voice.volume = 0.0f;
        voice.releasing = false;
        voice.attackStart = 0.0;
        voice.releaseStart = 0.0;
    }
}

void SimpleSynth::Start(const MidiSong* song, double baseTime,
                        std::chrono::steady_clock::time_point epoch) {
    Stop();
    if (!song || !IsAvailable() || song->notes.empty()) return;

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        song_ = song;
        baseTime_ = std::clamp(baseTime, 0.0, song->duration);
        epoch_ = epoch;
        ResetVoices();
        lastAppliedMasterVolume_ = -1.0f;
        running_.store(true, std::memory_order_release);
    }

    worker_ = std::thread(&SimpleSynth::AudioThreadMain, this);
}

void SimpleSynth::Stop() {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) worker_.join();

    std::lock_guard<std::mutex> lock(stateMutex_);
    ResetVoices();
    song_ = nullptr;
    baseTime_ = 0.0;
}

void SimpleSynth::SetMasterVolume(float volume) {
    masterVolume_.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_release);
}

float SimpleSynth::SynthSample(double time, double frequency, int midiNote, int sampleIndex) {
    (void)sampleIndex;
    if (time < 0.0 || frequency <= 0.0) return 0.0f;

    float sample = 0.0f;
    const int strings = PianoStringCount(midiNote);
    const double stiffness = StringInharmonicity(midiNote);
    for (int p = 0; p < kPartialCount; ++p) {
        const double partialFrequency = PartialFrequencyFromStiffness(frequency, stiffness, p);
        if (partialFrequency > 0.40 * static_cast<double>(kSampleRate)) continue;
        const double envelope = PartialEnvelope(p, time, midiNote);

        for (int stringIndex = 0; stringIndex < strings; ++stringIndex) {
            const double detunedFrequency = partialFrequency * (1.0 + StringDetune(midiNote, stringIndex));
            const double phase = kTwoPi * detunedFrequency * time +
                kPartialPhase[p] + kStringPhase[stringIndex] * (p + 1);
            sample += static_cast<float>(std::sin(phase) * envelope * StringWeight(midiNote, stringIndex));
        }
    }

    sample *= SmoothStep(static_cast<float>(time / kWaveAttackSeconds)) * 0.78f;
    return std::clamp(sample, -1.0f, 1.0f);
}

double SimpleSynth::NoteFrequency(int midiNote) {
    midiNote = std::clamp(midiNote, 0, 127);
    return 440.0 * std::pow(2.0, (static_cast<double>(midiNote) - 69.0) / 12.0);
}

int SimpleSynth::BaseMidiNoteFor(int midiNote) {
    midiNote = std::clamp(midiNote, 0, 127);
    const int index = std::clamp((midiNote + kBaseStep / 2) / kBaseStep, 0, kBaseCount - 1);
    int baseNote = index * kBaseStep;

    // Keep notes near the single/double/triple-string transitions on a cached
    // waveform with the matching string-count model.
    if (midiNote < 28 && baseNote >= 28) {
        baseNote = 24;
    } else if (midiNote >= 28 && midiNote < 52 && baseNote >= 52) {
        baseNote = 48;
    } else if (midiNote >= 52 && baseNote < 52) {
        baseNote = 52;
    }
    return std::clamp(baseNote, 0, 124);
}

int SimpleSynth::FindVoiceToUse(double time) {
    int freeVoice = -1;
    int bestVoice = 0;
    double bestScore = std::numeric_limits<double>::infinity();

    for (int i = 0; i < kVoiceCount; ++i) {
        const Voice& voice = voices_[static_cast<std::size_t>(i)];
        if (voice.noteIndex < 0) {
            freeVoice = i;
            break;
        }
        const double score = voice.releasing ? voice.releaseStart : voice.noteEnd;
        if (score < bestScore) {
            bestScore = score;
            bestVoice = i;
        }
    }

    (void)time;
    return freeVoice >= 0 ? freeVoice : bestVoice;
}

void SimpleSynth::StartNote(size_t noteIndex, double time) {
    if (!song_ || noteIndex >= song_->notes.size()) return;

    const MidiNote& note = song_->notes[noteIndex];
    if (note.note < 0 || note.note > 127) return;

    const int voiceIndex = FindVoiceToUse(time);
    Voice& voice = voices_[static_cast<std::size_t>(voiceIndex)];
    voice.source->Stop(0, XAUDIO2_COMMIT_NOW);
    voice.source->FlushSourceBuffers();

    const int baseMidi = BaseMidiNoteFor(note.note);
    const int baseIndex = baseMidi / kBaseStep;
    const auto& buffer = noteBuffers_[static_cast<std::size_t>(baseIndex)];
    if (buffer.empty()) return;

    XAUDIO2_BUFFER xbuffer{};
    xbuffer.AudioBytes = static_cast<UINT32>(buffer.size() * sizeof(int16_t));
    xbuffer.pAudioData = reinterpret_cast<const BYTE*>(buffer.data());

    // The precomputed body is a natural decay followed by a very quiet infinite
    // tail loop. This avoids a hard cutoff for long held notes without calculating
    // the oscillators on the real-time audio thread. The offline renderer continues analytically.
    const UINT32 loopSamples = static_cast<UINT32>(
        std::min<std::size_t>(buffer.size(), static_cast<std::size_t>(kReleaseTailSeconds * kSampleRate)));
    if (loopSamples > 0) {
        xbuffer.LoopBegin = static_cast<UINT32>(buffer.size()) - loopSamples;
        xbuffer.LoopLength = loopSamples;
        xbuffer.LoopCount = XAUDIO2_LOOP_INFINITE;
    } else {
        xbuffer.Flags = XAUDIO2_END_OF_STREAM;
    }

    if (FAILED(voice.source->SubmitSourceBuffer(&xbuffer, nullptr))) {
        voice.noteIndex = -1;
        return;
    }

    const double ratio = NoteFrequency(note.note) / NoteFrequency(baseMidi);
    voice.source->SetFrequencyRatio(static_cast<float>(ratio), XAUDIO2_COMMIT_NOW);

    XAUDIO2_FILTER_PARAMETERS toneFilter{};
    toneFilter.Type = LowPassOnePoleFilter;
    toneFilter.Frequency = VelocityFilterAlpha(note.velocity);
    toneFilter.OneOverQ = 1.0f;
    voice.source->SetFilterParameters(&toneFilter, XAUDIO2_COMMIT_NOW);

    voice.volume = 0.18f + 0.72f * std::sqrt(
        static_cast<float>(std::clamp(note.velocity, 1, 127)) / 127.0f);
    voice.source->SetVolume(0.0f, XAUDIO2_COMMIT_NOW);
    voice.source->Start(0, XAUDIO2_COMMIT_NOW);

    voice.noteIndex = static_cast<int>(noteIndex);
    voice.noteEnd = std::max(SoundingEnd(note), note.start + 0.005);
    voice.releasing = false;
    voice.attackStart = time;
    voice.releaseStart = 0.0;
}

void SimpleSynth::BeginRelease(Voice& voice, double time) {
    if (voice.noteIndex < 0 || voice.releasing) return;
    voice.releasing = true;
    voice.releaseStart = time;
}

void SimpleSynth::UpdateReleases(double time) {
    const float masterVolume = masterVolume_.load(std::memory_order_acquire);

    for (Voice& voice : voices_) {
        if (voice.noteIndex < 0) continue;

        if (!voice.releasing) {
            if (time >= voice.noteEnd) {
                BeginRelease(voice, time);
                continue;
            }

            const float attack = SmoothStep(static_cast<float>(
                (time - voice.attackStart) / kAttackSeconds));
            voice.source->SetVolume(voice.volume * masterVolume * attack, XAUDIO2_COMMIT_NOW);
            continue;
        }

        const float u = SmoothStep(static_cast<float>(
            (time - voice.releaseStart) / kReleaseSeconds));
        voice.source->SetVolume(voice.volume * masterVolume * (1.0f - u), XAUDIO2_COMMIT_NOW);

        if (u >= 1.0f) {
            voice.source->Stop(0, XAUDIO2_COMMIT_NOW);
            voice.source->FlushSourceBuffers();
            voice.noteIndex = -1;
            voice.noteEnd = 0.0;
            voice.volume = 0.0f;
            voice.releasing = false;
            voice.attackStart = 0.0;
            voice.releaseStart = 0.0;
        }
    }

    lastAppliedMasterVolume_ = masterVolume;
}

bool SimpleSynth::RenderWav(const MidiSong& song, const std::wstring& path,
                            float volume, double tailSeconds, std::wstring& error,
                            const std::atomic<bool>* cancelRequested) {
    if (song.duration <= 0.0 || song.notes.empty()) {
        error = L"The MIDI song contains no renderable notes.";
        return false;
    }

    if (noteBuffers_[0].empty()) GenerateNoteBuffers();

    volume = std::clamp(volume, 0.0f, 1.0f);
    tailSeconds = std::max(0.0, tailSeconds);

    constexpr int channels = 2;
    constexpr int sampleRate = kSampleRate;
    constexpr float kPeakTarget = 0.98f;
    const double renderDuration = song.duration + tailSeconds;
    const std::uint64_t totalFrames = static_cast<std::uint64_t>(std::ceil(renderDuration * sampleRate));

    if (totalFrames == 0 || totalFrames >
        (std::numeric_limits<std::uint32_t>::max() / (channels * sizeof(std::int16_t)))) {
        error = L"The rendered audio is too long for a standard WAV file.";
        return false;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = L"Failed to create the temporary WAV file.";
        return false;
    }

    constexpr std::size_t kBlockFrames = 4096;
    std::vector<float> mix(kBlockFrames, 0.0f);
    std::vector<std::int16_t> pcm(kBlockFrames * channels);

    // Maintain one small filter state per note so velocity-dependent timbre is
    // also reflected in exported WAV/MP4 audio, without any runtime synth cost.
    std::vector<float> filterStates(song.notes.size(), 0.0f);

    std::vector<double> prefixMaxEnd(song.notes.size(), 0.0);
    double maxEnd = 0.0;
    for (std::size_t i = 0; i < song.notes.size(); ++i) {
        maxEnd = std::max(maxEnd, SoundingEnd(song.notes[i]) + kReleaseSeconds);
        prefixMaxEnd[i] = maxEnd;
    }

    auto renderMixBlock = [&](std::uint64_t blockStartFrame, std::size_t frames) {
        std::fill(mix.begin(), mix.begin() + static_cast<std::ptrdiff_t>(frames), 0.0f);

        const double blockStart = static_cast<double>(blockStartFrame) / sampleRate;
        const double blockEnd = static_cast<double>(blockStartFrame + frames) / sampleRate;
        const auto blockEndIt = std::lower_bound(song.notes.begin(), song.notes.end(), blockEnd,
            [](const MidiNote& note, double time) { return note.start < time; });
        const std::size_t blockEndIndex = static_cast<std::size_t>(
            std::distance(song.notes.begin(), blockEndIt));

        std::size_t firstIndex = 0;
        if (blockEndIndex > 0) {
            const auto firstIt = std::upper_bound(
                prefixMaxEnd.begin(), prefixMaxEnd.begin() + static_cast<std::ptrdiff_t>(blockEndIndex),
                blockStart);
            firstIndex = static_cast<std::size_t>(std::distance(prefixMaxEnd.begin(), firstIt));
        }

        for (std::size_t noteIndex = firstIndex; noteIndex < blockEndIndex; ++noteIndex) {
            const MidiNote& note = song.notes[noteIndex];
            const double audioEnd = SoundingEnd(note);
            if (note.note < 0 || note.note > 127 || audioEnd <= note.start) continue;

            const int baseMidi = BaseMidiNoteFor(note.note);
            const int baseIndex = baseMidi / kBaseStep;
            const auto& wave = noteBuffers_[static_cast<std::size_t>(baseIndex)];
            if (wave.empty()) continue;

            const double ratio = NoteFrequency(note.note) / NoteFrequency(baseMidi);
            const float velocityGain = 0.18f + 0.72f * std::sqrt(
                static_cast<float>(std::clamp(note.velocity, 1, 127)) / 127.0f);
            const float noteGain = velocityGain * volume;
            const float filterAlpha = VelocityFilterAlpha(note.velocity);

            const std::uint64_t noteStartFrame = static_cast<std::uint64_t>(
                std::max(0.0, std::floor(note.start * sampleRate)));
            const std::uint64_t noteEndFrame = static_cast<std::uint64_t>(
                std::max(0.0, std::ceil((audioEnd + kReleaseSeconds) * sampleRate)));
            const std::uint64_t from = std::max(blockStartFrame, noteStartFrame);
            const std::uint64_t to = std::min(blockStartFrame + frames, noteEndFrame);
            if (from >= to) continue;

            const double noteDuration = std::max(0.0, audioEnd - note.start);

            for (std::uint64_t frame = from; frame < to; ++frame) {
                const double t = static_cast<double>(frame) / sampleRate - note.start;
                if (t < 0.0) continue;

                const double sourceTime = t * ratio;
                const double sourcePos = sourceTime * sampleRate;
                float sample = 0.0f;

                if (sourcePos >= 0.0 && sourcePos < static_cast<double>(wave.size() - 1)) {
                    const std::size_t i0 = static_cast<std::size_t>(sourcePos);
                    const float frac = static_cast<float>(sourcePos - static_cast<double>(i0));
                    const float s0 = static_cast<float>(wave[i0]) / 32768.0f;
                    const float s1 = static_cast<float>(wave[i0 + 1]) / 32768.0f;
                    sample = s0 + (s1 - s0) * frac;
                } else {
                    // Beyond the cached waveform body, continue the note analytically.
                    // The inharmonic partials and dual decay envelopes naturally converge toward silence.
                    sample = SynthSample(sourceTime, NoteFrequency(baseMidi), baseMidi,
                                         static_cast<int>(frame - blockStartFrame));
                }

                // A one-pole low-pass is a cheap approximation of the live
                // XAudio2 filter. It makes soft strikes rounder and hard strikes brighter.
                float& filterState = filterStates[noteIndex];
                filterState += filterAlpha * (sample - filterState);
                sample = filterState;

                double envelope = SmoothStep(static_cast<float>(t / kAttackSeconds));
                if (t >= noteDuration) {
                    const double rel = t - noteDuration;
                    const double u = std::clamp(rel / kReleaseSeconds, 0.0, 1.0);
                    envelope *= 1.0 - u * u * (3.0 - 2.0 * u);
                }

                mix[static_cast<std::size_t>(frame - blockStartFrame)] +=
                    sample * static_cast<float>(envelope) * noteGain;
            }
        }
    };

    auto canceled = [&]() {
        return cancelRequested && cancelRequested->load(std::memory_order_acquire);
    };

    float maximumAbsMix = 0.0f;
    for (std::uint64_t blockStartFrame = 0; blockStartFrame < totalFrames; blockStartFrame += kBlockFrames) {
        if (canceled()) {
            out.close();
            error = L"Rendering canceled.";
            return false;
        }

        const std::size_t frames = static_cast<std::size_t>(
            std::min<std::uint64_t>(kBlockFrames, totalFrames - blockStartFrame));
        renderMixBlock(blockStartFrame, frames);
        for (std::size_t i = 0; i < frames; ++i) {
            maximumAbsMix = std::max(maximumAbsMix, std::abs(mix[i]));
        }
    }

    const float renderGain = maximumAbsMix > kPeakTarget
        ? kPeakTarget / maximumAbsMix
        : 1.0f;
    std::fill(filterStates.begin(), filterStates.end(), 0.0f);

    const std::uint32_t dataBytes = static_cast<std::uint32_t>(
        totalFrames * channels * sizeof(std::int16_t));

    auto writeU16 = [&out](std::uint16_t value) {
        const std::uint8_t bytes[2] = {
            static_cast<std::uint8_t>(value & 0xFFu),
            static_cast<std::uint8_t>(value >> 8)
        };
        out.write(reinterpret_cast<const char*>(bytes), 2);
    };
    auto writeU32 = [&out](std::uint32_t value) {
        const std::uint8_t bytes[4] = {
            static_cast<std::uint8_t>(value & 0xFFu),
            static_cast<std::uint8_t>((value >> 8) & 0xFFu),
            static_cast<std::uint8_t>((value >> 16) & 0xFFu),
            static_cast<std::uint8_t>(value >> 24)
        };
        out.write(reinterpret_cast<const char*>(bytes), 4);
    };

    out.write("RIFF", 4);
    writeU32(36u + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeU32(16);
    writeU16(1);
    writeU16(channels);
    writeU32(sampleRate);
    writeU32(sampleRate * channels * sizeof(std::int16_t));
    writeU16(static_cast<std::uint16_t>(channels * sizeof(std::int16_t)));
    writeU16(16);
    out.write("data", 4);
    writeU32(dataBytes);

    for (std::uint64_t blockStartFrame = 0; blockStartFrame < totalFrames; blockStartFrame += kBlockFrames) {
        if (canceled()) {
            out.close();
            error = L"Rendering canceled.";
            return false;
        }

        const std::size_t frames = static_cast<std::size_t>(
            std::min<std::uint64_t>(kBlockFrames, totalFrames - blockStartFrame));
        renderMixBlock(blockStartFrame, frames);

        for (std::size_t i = 0; i < frames; ++i) {
            const float sample = std::clamp(mix[i] * renderGain, -1.0f, 1.0f);
            const std::int16_t value = static_cast<std::int16_t>(std::lrint(sample * 32767.0f));
            pcm[i * 2] = value;
            pcm[i * 2 + 1] = value;
        }

        out.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(
            frames * channels * sizeof(std::int16_t)));
        if (!out) {
            error = L"Failed while writing the temporary WAV file.";
            out.close();
            return false;
        }
    }

    out.close();
    return true;
}

void SimpleSynth::AudioThreadMain() {
    const MidiSong* song = nullptr;
    double baseTime = 0.0;
    std::chrono::steady_clock::time_point epoch{};

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        song = song_;
        baseTime = baseTime_;
        epoch = epoch_;
    }

    if (!song) return;

    auto eventIt = std::lower_bound(song->notes.begin(), song->notes.end(), baseTime,
        [](const MidiNote& note, double time) { return note.start < time; });
    std::size_t nextIndex = static_cast<std::size_t>(std::distance(song->notes.begin(), eventIt));
    // Reconstruct all notes that are still sounding at the requested start time.
    // A note may have received physical note-off already but remain audible under
    // CC64 sustain, so this must use soundingEnd rather than the visual note end.
    for (std::size_t i = 0; i < nextIndex; ++i) {
        if (SoundingEnd(song->notes[i]) > baseTime) StartNote(i, baseTime);
    }

    const auto sleepQuantum = std::chrono::milliseconds(1);
    while (running_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - epoch).count();
        const double time = baseTime + std::max(0.0, elapsed);

        while (nextIndex < song->notes.size() && song->notes[nextIndex].start <= time) {
            StartNote(nextIndex, time);
            ++nextIndex;
        }

        UpdateReleases(time);

        if (time >= song->duration + kReleaseSeconds && nextIndex >= song->notes.size()) {
            running_.store(false, std::memory_order_release);
            break;
        }
        std::this_thread::sleep_for(sleepQuantum);
    }
}
