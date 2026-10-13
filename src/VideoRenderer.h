#pragma once

#include <atomic>
#include <functional>
#include <string>

class Visualizer;
class SimpleSynth;
struct MidiSong;

struct VideoRenderOptions {
    unsigned int width = 1280;
    unsigned int height = 720;
    double fps = 60.0;
    double tailSeconds = 3.0;
    float fallSpeed = 380.0f;
    bool includeAudio = true;
    float audioVolume = 0.75f;
    // When non-empty, use a user-supplied WAV instead of rendering MIDI audio.
    std::wstring audioInputPath;
    int audioOffsetMilliseconds = 0;
    std::wstring outputPath;
    std::wstring ffmpegPath;
    std::wstring temporaryWavPath;
};

class VideoRenderer {
public:
    using ProgressCallback = std::function<void(int percent, const std::wstring& status)>;

    bool RenderMp4(const Visualizer& visualizer,
                   const MidiSong& song,
                   SimpleSynth* synth,
                   const VideoRenderOptions& options,
                   const ProgressCallback& progress,
                   std::wstring& error,
                   const std::atomic<bool>* cancelRequested = nullptr);

    static bool FindFfmpeg(std::wstring& path);
};
