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
    float fallSpeed = 380.0f;
    bool includeAudio = true;
    float audioVolume = 0.75f;
    std::wstring outputPath;
    std::wstring ffmpegPath;
    std::wstring temporaryWavPath;
};

class VideoRenderer {
public:
    using ProgressCallback = std::function<void(int percent, const std::wstring& status)>;

    static constexpr double kTailSeconds = 3.00;

    bool RenderMp4(const Visualizer& visualizer,
                   const MidiSong& song,
                   SimpleSynth* synth,
                   const VideoRenderOptions& options,
                   const ProgressCallback& progress,
                   std::wstring& error,
                   const std::atomic<bool>* cancelRequested = nullptr);

    static bool FindFfmpeg(std::wstring& path);
};
