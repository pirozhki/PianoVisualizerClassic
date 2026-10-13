#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>
#include <wrl/client.h>
#include <xaudio2.h>

// Loads ordinary PCM/IEEE-float WAV files, converts them to 48 kHz stereo PCM,
// and plays them against the same timeline as the MIDI visualizer.
class WavAudioPlayer {
public:
    WavAudioPlayer() = default;
    ~WavAudioPlayer() { Shutdown(); }

    bool Initialize(std::wstring& error);
    void Shutdown();

    bool Load(const std::wstring& path, std::wstring& error);
    void Unload();
    bool IsLoaded() const { return !samples_.empty() && !path_.empty(); }
    const std::wstring& Path() const { return path_; }
    double Duration() const;

    void SetMasterVolume(float volume);
    float MasterVolume() const { return masterVolume_.load(std::memory_order_acquire); }

    // offsetMilliseconds > 0 delays the WAV relative to MIDI; negative values
    // advance it. midiBaseTime is the visualizer timeline at the instant playback
    // starts (or resumes after a seek).
    void Start(double midiBaseTime, int offsetMilliseconds);
    void Stop();

    static constexpr int kSampleRate = 48000;
    static constexpr int kMinOffsetMilliseconds = -10000;
    static constexpr int kMaxOffsetMilliseconds = 10000;

private:
    static bool DecodeWav(const std::wstring& path,
                          std::vector<std::int16_t>& output,
                          std::wstring& error);
    bool CreateSourceVoice(IXAudio2SourceVoice** voice, std::wstring& error);

    Microsoft::WRL::ComPtr<IXAudio2> xaudio_;
    IXAudio2MasteringVoice* masteringVoice_ = nullptr;
    IXAudio2SourceVoice* sourceVoice_ = nullptr;
    std::vector<std::int16_t> samples_;
    std::vector<std::int16_t> silenceSamples_;
    std::wstring path_;
    std::atomic<float> masterVolume_{0.75f};
};
