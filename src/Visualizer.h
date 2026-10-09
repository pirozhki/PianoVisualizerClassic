#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "MidiFile.h"

class VideoRenderer;

class Visualizer {
public:
    enum EffectFlag : std::uint32_t {
        EffectNone = 0,
        EffectSmokeGlow = 1u << 0,
        EffectSpark = 1u << 1,
        EffectRipple = 1u << 2,
        EffectDiamondRipple = 1u << 3,
        EffectNoteGlow = 1u << 4,
        EffectCrossSpark = 1u << 5,
        EffectWaveLine = 1u << 6,
        EffectSmokeWindow = 1u << 7,
        EffectAmbientParticles = 1u << 8,
        EffectImpactPolygons = 1u << 9,
    };
    using EffectMask = std::uint32_t;

    Visualizer() = default;
    ~Visualizer();

    bool Initialize(HWND hwnd);
    void Shutdown();
    void Resize(UINT width, UINT height);
    void SetSong(const MidiSong* song);
    void SetPlaying(bool playing);
    void Reset();
    void SetTime(double seconds);
    double Time() const { return currentTime_; }
    double Duration() const { return song_ ? song_->duration : 0.0; }
    void SetFallSpeed(float pxPerSecond) { fallSpeed_ = pxPerSecond; }
    float FallSpeed() const { return fallSpeed_; }

    bool LoadBackgroundImage(const std::wstring& path);
    void ResetBackgroundImage();
    void SetBackgroundOpacity(float opacity);
    static constexpr float kDefaultBackgroundOpacity = 0.30f;
    float BackgroundOpacity() const { return backgroundOpacity_; }
    bool HasBackgroundImage() const { return !backgroundPath_.empty(); }

    void SetEffectMask(EffectMask mask);
    void SetNoteGuideLinesEnabled(bool enabled) { showNoteGuides_ = enabled; }
    bool NoteGuideLinesEnabled() const { return showNoteGuides_; }
    EffectMask GetEffectMask() const { return effectMask_; }
    void SetEffectEnabled(EffectFlag flag, bool enabled);
    bool IsEffectEnabled(EffectFlag flag) const { return (effectMask_ & flag) != 0; }
    void SetWaveLineColor(const D2D1::ColorF& color);
    void SetAmbientParticleCount(int count);
    int AmbientParticleCount() const { return ambientParticleCount_; }
    D2D1::ColorF GetWaveLineColor() const { return waveLineColor_; }

    bool SetTrackColor(int track, const D2D1::ColorF& color);
    void ResetTrackColors();
    D2D1::ColorF GetTrackColor(int track) const;
    bool HasCustomTrackColor(int track) const;

    void Render();

private:
    static constexpr int kMaxParticles = 1800;
    static constexpr int kMaxRipples = 512;
    static constexpr int kMaxImpactShards = 480;
    static constexpr int kMaxHitWaves = 96;
    static constexpr int kDefaultPaletteCount = 4;
    static constexpr int kCinemaParticleCount = 16;
    static constexpr float kSmokeEmissionInterval = 0.12f;
    static constexpr int kSmokeParticlesPerBurst = 3;
    static constexpr int kCrossParticlesPerBurst = 2;

    friend class VideoRenderer;

    struct Particle {
        float x = 0.0f;
        float y = 0.0f;
        float vx = 0.0f;
        float vy = 0.0f;
        float life = 0.0f;
        float maxLife = 0.0f;
        float size = 1.0f;
        float maxSize = 1.0f;
        float phase = 0.0f;
        float windPhase = 0.0f;
        float gravity = 0.0f;
        int track = 0;
        bool spark = false;
        bool whiteSpark = false;
        bool crossSpark = false;
    };

    struct ImpactShard {
        float x = 0.0f;
        float y = 0.0f;
        float vx = 0.0f;
        float vy = 0.0f;
        float angle = 0.0f;
        float spin = 0.0f;
        float life = 0.0f;
        float maxLife = 0.0f;
        float size = 1.0f;
        int track = 0;
    };

    struct HitWave {
        float x = 0.0f;
        float age = 0.0f;
        float maxLife = 2.0f;
        float amplitude = 1.33f;
    };

    struct Ripple {
        float x = 0.0f;
        float y = 0.0f;
        float radius = 0.0f;
        float startRadius = 0.0f;
        float maxRadius = 0.0f;
        float life = 0.0f;
        float maxLife = 0.0f;
        float thickness = 1.5f;
        int track = 0;
        bool diamond = false;
    };

    void DiscardDeviceResources();
    bool CreateDeviceResources();
    bool CreateDrawingResources();
    bool CreateBackgroundBitmap();
    bool LoadBackgroundSource();
    bool CreateC4LabelFormat();
    bool CreateEffectBrushes();
    bool CreateSmokeWindowBitmap();
    bool InitializeOffscreen(const Visualizer& source, UINT width, UINT height);
    void ResetEffectSimulation(double time);
    void UpdateActiveNotes();
    void UpdateEffects();
    void SpawnEffect(const MidiNote& note, int ordinal);
    void SpawnSmokeBurst(const MidiNote& note, std::int64_t emissionIndex);
    void SpawnCrossBurst(const MidiNote& note, std::int64_t emissionIndex);
    void DrawEffects(float width, float height);
    void DrawWaveLine(float width);
    void DrawAmbientParticles(float width, float height);
    void SpawnImpactShards(float centerX, int track, std::uint32_t seed);
    void RebuildGeometry(float width, float height);
    void DrawBackground(float width, float height);
    void DrawNoteGuides(float width, float height);
    void DrawNotes(float width, float height);
    void DrawKeyboard(float width, float height);

    size_t TrackBrushIndex(int track) const;
    ID2D1SolidColorBrush* NoteBrushForTrack(int track) const;
    ID2D1SolidColorBrush* GlowBrushForTrack(int track) const;
    ID2D1RadialGradientBrush* SmokeBrushForTrack(int track) const;
    ID2D1LinearGradientBrush* BeamBrushForTrack(int track) const;
    ID2D1LinearGradientBrush* ActiveBlackBrushForTrack(int track) const;
    D2D1::ColorF NoteColorForTrack(int track) const;
    D2D1::ColorF GlowColorForTrack(int track) const;

    float KeyX(int note) const;
    float KeyW(int note) const;
    bool IsBlackKey(int note) const;
    int WhiteIndex(int note) const;

    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> hwndTarget_;
    Microsoft::WRL::ComPtr<IWICBitmap> offscreenBitmap_;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> white_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> black_;
    std::vector<Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>> noteBrushes_;
    std::vector<Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>> glowBrushes_;
    std::vector<Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush>> smokeBrushes_;
    std::vector<Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush>> beamBrushes_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> darkBg_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> waveLineBrush_;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> triangleGeometry_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> whiteKeyBrush_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> blackKeyBrush_;
    std::vector<Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush>> activeBlackBrushes_;

    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> c4LabelFormat_;

    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory_;
    Microsoft::WRL::ComPtr<IWICBitmapSource> backgroundSource_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> backgroundBitmap_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> smokeWindowBitmap_;
    std::wstring backgroundPath_;
    float backgroundOpacity_ = kDefaultBackgroundOpacity;

    std::vector<D2D1::ColorF> trackColors_;
    std::vector<bool> customTrackColors_;

    const MidiSong* song_ = nullptr;
    double currentTime_ = 0.0;
    bool playing_ = false;
    float fallSpeed_ = 380.0f;
    double maxNoteDuration_ = 1.0;

    float width_ = 1.0f;
    float height_ = 1.0f;
    float keyboardHeight_ = 160.0f;
    float hitY_ = 500.0f;

    EffectMask effectMask_ = EffectSmokeGlow | EffectCrossSpark | EffectWaveLine;
    bool showNoteGuides_ = true;
    D2D1::ColorF waveLineColor_ = D2D1::ColorF(0.24f, 0.82f, 0.94f, 1.0f);
    int ambientParticleCount_ = 40;
    std::array<Particle, kMaxParticles> particles_{};
    int particleCount_ = 0;
    std::array<Ripple, kMaxRipples> ripples_{};
    int rippleCount_ = 0;
    std::array<ImpactShard, kMaxImpactShards> impactShards_{};
    int impactShardCount_ = 0;
    std::array<HitWave, kMaxHitWaves> hitWaves_{};
    int hitWaveCount_ = 0;
    double effectSimTime_ = -0.001;
    bool effectInitialized_ = false;
    std::array<int, 128> activeTracks_{};
    std::array<int, 128> activeVelocities_{};
    std::array<int, 128> activeNoteList_{};
    int activeNoteCount_ = 0;
};
