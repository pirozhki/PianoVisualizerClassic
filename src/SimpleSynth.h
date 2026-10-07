#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <wrl/client.h>
#include <xaudio2.h>

#include "MidiFile.h"

class SimpleSynth {
public:
    SimpleSynth() = default;
    ~SimpleSynth() { Shutdown(); }

    bool Initialize(std::wstring& error);
    void Shutdown();

    bool IsAvailable() const { return xaudio_ != nullptr && masteringVoice_ != nullptr; }

    // Starts playback from an absolute MIDI position using the same epoch as the visualizer.
    void Start(const MidiSong* song, double baseTime,
               std::chrono::steady_clock::time_point epoch);
    void Stop();

    void SetMasterVolume(float volume);
    float MasterVolume() const { return masterVolume_.load(std::memory_order_acquire); }

    bool RenderWav(const MidiSong& song, const std::wstring& path,
                   float volume, double tailSeconds, std::wstring& error,
                   const std::atomic<bool>* cancelRequested = nullptr);

private:
    static constexpr int kSampleRate = 48000;
    static constexpr int kVoiceCount = 64;
    static constexpr double kBufferSeconds = 6.0;
    static constexpr double kReleaseTailSeconds = 0.50;
    static constexpr double kAttackSeconds = 0.004;
    static constexpr double kReleaseSeconds = 0.065;
    static constexpr int kBaseStep = 4; // One waveform every four semitones.
    static constexpr int kBaseCount = 32; // MIDI 0,4,...,124.

    struct Voice {
        IXAudio2SourceVoice* source = nullptr;
        int noteIndex = -1;
        double noteEnd = 0.0;
        float volume = 0.0f;
        bool releasing = false;
        double attackStart = 0.0;
        double releaseStart = 0.0;
    };

    bool CreateVoices();
    void ReleaseVoices();
    void GenerateNoteBuffers();
    void AudioThreadMain();

    void ResetVoices();
    void StartNote(size_t noteIndex, double time);
    void BeginRelease(Voice& voice, double time);
    int FindVoiceToUse(double time);
    void UpdateReleases(double time);

    static double NoteFrequency(int midiNote);
    static float SynthSample(double time, double frequency, int midiNote, int sampleIndex);
    static int BaseMidiNoteFor(int midiNote);

    Microsoft::WRL::ComPtr<IXAudio2> xaudio_;
    IXAudio2MasteringVoice* masteringVoice_ = nullptr;

    std::array<Voice, kVoiceCount> voices_{};
    std::array<std::vector<int16_t>, kBaseCount> noteBuffers_{};

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<float> masterVolume_{0.75f};
    mutable std::mutex stateMutex_;

    const MidiSong* song_ = nullptr;
    double baseTime_ = 0.0;
    std::chrono::steady_clock::time_point epoch_{};
    float lastAppliedMasterVolume_ = -1.0f;
};
