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
constexpr std::array<double, 6> kPartialMultiplier = {
    1.0000, 2.0001, 3.0004, 4.0010, 5.0018, 6.0028
};
constexpr std::array<double, 6> kPartialLevel = {
    0.72, 0.25, 0.13, 0.080, 0.050, 0.030
};
constexpr std::array<double, 6> kPartialDecay = {
    1.55, 2.15, 2.75, 3.35, 4.05, 4.80
};
constexpr std::array<double, 6> kPartialPhase = {
    0.00, 0.37, 1.11, 2.03, 2.77, 4.19
};

float SmoothStep(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
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
        std::array<float, 6> sinPhase{};
        std::array<float, 6> cosPhase{};
        std::array<float, 6> sinDelta{};
        std::array<float, 6> cosDelta{};
        std::array<float, 6> envelope{};
        std::array<float, 6> envelopeDecay{};

        for (int p = 0; p < 6; ++p) {
            const double partialFrequency = frequency * kPartialMultiplier[p];
            if (partialFrequency > 0.45 * static_cast<double>(kSampleRate)) {
                envelope[p] = 0.0f;
                envelopeDecay[p] = 1.0f;
                continue;
            }

            const double omega = kTwoPi * partialFrequency / static_cast<double>(kSampleRate);
            sinDelta[p] = static_cast<float>(std::sin(omega));
            cosDelta[p] = static_cast<float>(std::cos(omega));
            sinPhase[p] = static_cast<float>(std::sin(kPartialPhase[p]));
            cosPhase[p] = static_cast<float>(std::cos(kPartialPhase[p]));
            envelope[p] = static_cast<float>(kPartialLevel[p]);
            envelopeDecay[p] = static_cast<float>(
                std::exp(-kPartialDecay[p] / static_cast<double>(kSampleRate)));
        }

        for (int i = 0; i < waveSamples; ++i) {
            const double t = static_cast<double>(i) / kSampleRate;
            const float attack = SmoothStep(static_cast<float>(t / 0.008));
            float sample = 0.0f;

            for (int p = 0; p < 6; ++p) {
                sample += envelope[p] * sinPhase[p];
                const float oldSin = sinPhase[p];
                sinPhase[p] = oldSin * cosDelta[p] + cosPhase[p] * sinDelta[p];
                cosPhase[p] = cosPhase[p] * cosDelta[p] - oldSin * sinDelta[p];
                envelope[p] *= envelopeDecay[p];
            }

            sample *= attack;
            sample = static_cast<float>(std::tanh(sample * 1.15f));
            sample = std::clamp(sample * 0.82f, -1.0f, 1.0f);
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
            &voice.source, &format, 0, XAUDIO2_DEFAULT_FREQ_RATIO,
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
    (void)midiNote;
    (void)sampleIndex;
    if (time < 0.0 || frequency <= 0.0) return 0.0f;

    float sample = 0.0f;
    for (int p = 0; p < 6; ++p) {
        const double partialFrequency = frequency * kPartialMultiplier[p];
        if (partialFrequency > 0.45 * static_cast<double>(kSampleRate)) continue;
        const double phase = kTwoPi * partialFrequency * time + kPartialPhase[p];
        const double envelope = kPartialLevel[p] * std::exp(-kPartialDecay[p] * time);
        sample += static_cast<float>(std::sin(phase) * envelope);
    }

    const float attack = SmoothStep(static_cast<float>(time / 0.008));
    sample *= attack;
    sample = static_cast<float>(std::tanh(sample * 1.15f));
    return std::clamp(sample * 0.82f, -1.0f, 1.0f);
}

double SimpleSynth::NoteFrequency(int midiNote) {
    midiNote = std::clamp(midiNote, 0, 127);
    return 440.0 * std::pow(2.0, (static_cast<double>(midiNote) - 69.0) / 12.0);
}

int SimpleSynth::BaseMidiNoteFor(int midiNote) {
    midiNote = std::clamp(midiNote, 0, 127);
    const int index = std::clamp((midiNote + kBaseStep / 2) / kBaseStep, 0, kBaseCount - 1);
    return index * kBaseStep;
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

    // The six-second body is now a normal decay followed by a very quiet infinite
    // tail loop. This removes the hard six-second cutoff for long held notes while
    // keeping memory use modest. The offline renderer continues analytically instead.
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

    voice.volume = 0.18f + 0.72f * std::sqrt(
        static_cast<float>(std::clamp(note.velocity, 1, 127)) / 127.0f);
    voice.source->SetVolume(0.0f, XAUDIO2_COMMIT_NOW);
    voice.source->Start(0, XAUDIO2_COMMIT_NOW);

    voice.noteIndex = static_cast<int>(noteIndex);
    voice.noteEnd = std::max(note.end, note.start + 0.005);
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

    std::vector<double> prefixMaxEnd(song.notes.size(), 0.0);
    double maxEnd = 0.0;
    for (std::size_t i = 0; i < song.notes.size(); ++i) {
        maxEnd = std::max(maxEnd, song.notes[i].end + kReleaseSeconds);
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

        for (auto it = song.notes.begin() + static_cast<std::ptrdiff_t>(firstIndex);
             it != blockEndIt; ++it) {
            const MidiNote& note = *it;
            if (note.note < 0 || note.note > 127 || note.end <= note.start) continue;

            const int baseMidi = BaseMidiNoteFor(note.note);
            const int baseIndex = baseMidi / kBaseStep;
            const auto& wave = noteBuffers_[static_cast<std::size_t>(baseIndex)];
            if (wave.empty()) continue;

            const double ratio = NoteFrequency(note.note) / NoteFrequency(baseMidi);
            const float velocityGain = 0.18f + 0.72f * std::sqrt(
                static_cast<float>(std::clamp(note.velocity, 1, 127)) / 127.0f);
            const float noteGain = velocityGain * volume;

            const std::uint64_t noteStartFrame = static_cast<std::uint64_t>(
                std::max(0.0, std::floor(note.start * sampleRate)));
            const std::uint64_t noteEndFrame = static_cast<std::uint64_t>(
                std::max(0.0, std::ceil((note.end + kReleaseSeconds) * sampleRate)));
            const std::uint64_t from = std::max(blockStartFrame, noteStartFrame);
            const std::uint64_t to = std::min(blockStartFrame + frames, noteEndFrame);
            if (from >= to) continue;

            const double noteDuration = std::max(0.0, note.end - note.start);

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
                    // Beyond the cached six-second body, continue the note analytically.
                    // The exponential partial envelopes naturally converge toward silence.
                    sample = SynthSample(sourceTime, NoteFrequency(baseMidi), baseMidi,
                                         static_cast<int>(frame - blockStartFrame));
                }

                double envelope = 1.0;
                if (t >= noteDuration) {
                    const double rel = t - noteDuration;
                    const double u = std::clamp(rel / kReleaseSeconds, 0.0, 1.0);
                    envelope = 1.0 - u * u * (3.0 - 2.0 * u);
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
    std::size_t firstActive = nextIndex;

    while (firstActive > 0 && song->notes[firstActive - 1].end > baseTime) --firstActive;
    for (std::size_t i = firstActive; i < nextIndex; ++i) {
        if (song->notes[i].end > baseTime) StartNote(i, baseTime);
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
