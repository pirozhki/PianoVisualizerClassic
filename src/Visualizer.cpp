#define NOMINMAX
#include "Visualizer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kFirstKey = 21;
constexpr int kLastKey = 108;
constexpr int kWhiteKeys = 52;
constexpr int kPaletteCount = 4;

struct Palette {
    D2D1::ColorF main;
    D2D1::ColorF glow;
};

const Palette kPalettes[kPaletteCount] = {
    {{0.94f, 0.27f, 0.29f, 1.0f}, {1.00f, 0.42f, 0.48f, 1.0f}},
    {{0.23f, 0.51f, 0.96f, 1.0f}, {0.38f, 0.65f, 1.00f, 1.0f}},
    {{0.28f, 0.86f, 0.42f, 1.0f}, {0.50f, 1.00f, 0.62f, 1.0f}},
    {{0.96f, 0.61f, 0.04f, 1.0f}, {1.00f, 0.74f, 0.15f, 1.0f}},
};

float Clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

float Velocity01(int velocity) {
    return Clamp01(static_cast<float>(velocity) / 127.0f);
}

std::uint32_t NextRandom(std::uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

float Random01(std::uint32_t& state) {
    return static_cast<float>((NextRandom(state) >> 8) & 0x00FFFFFFu) / 16777216.0f;
}

float VelocityOpacity(int velocity) {
    return 0.34f + 0.66f * Velocity01(velocity);
}

int TrackIndex(int track) {
    int value = track % kPaletteCount;
    if (value < 0) value += kPaletteCount;
    return value;
}

ComPtr<IWICImagingFactory> CreateWicFactory() {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()));
    }
    return factory;
}

} // namespace

Visualizer::~Visualizer() {
    Shutdown();
}

void Visualizer::Shutdown() {
    DiscardDeviceResources();
    backgroundSource_.Reset();
    c4LabelFormat_.Reset();
    dwriteFactory_.Reset();
    wicFactory_.Reset();
    factory_.Reset();
    offscreenBitmap_.Reset();
    song_ = nullptr;
    hwnd_ = nullptr;
    particleCount_ = 0;
    rippleCount_ = 0;
    effectInitialized_ = false;
}

bool Visualizer::Initialize(HWND hwnd) {
    Shutdown();
    hwnd_ = hwnd;

    if (FAILED(D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        IID_PPV_ARGS(factory_.ReleaseAndGetAddressOf())))) {
        return false;
    }

    wicFactory_ = CreateWicFactory();
    if (!wicFactory_) return false;

    return CreateDeviceResources();
}

bool Visualizer::InitializeOffscreen(const Visualizer& source, UINT width, UINT height) {
    // This object is constructed and owned entirely by the render worker thread.
    // It copies only immutable/user-facing visual state from the live visualizer;
    // no live D2D/WIC resource is shared across threads.
    Shutdown();

    hwnd_ = nullptr;
    song_ = source.song_;
    trackColors_ = source.trackColors_;
    customTrackColors_ = source.customTrackColors_;
    fallSpeed_ = source.fallSpeed_;
    maxNoteDuration_ = source.maxNoteDuration_;
    effectMask_ = source.effectMask_;
    showNoteGuides_ = source.showNoteGuides_;
    backgroundPath_ = source.backgroundPath_;
    backgroundOpacity_ = source.backgroundOpacity_;

    width_ = static_cast<float>(std::max<UINT>(1, width));
    height_ = static_cast<float>(std::max<UINT>(1, height));
    RebuildGeometry(width_, height_);
    currentTime_ = 0.0;
    playing_ = true;
    ResetEffectSimulation(-0.001);

    if (FAILED(D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        IID_PPV_ARGS(factory_.ReleaseAndGetAddressOf())))) {
        return false;
    }

    wicFactory_ = CreateWicFactory();
    if (!wicFactory_) return false;

    HRESULT hr = wicFactory_->CreateBitmap(
        static_cast<UINT>(width_), static_cast<UINT>(height_),
        GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
        offscreenBitmap_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    const auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED),
        0.0f, 0.0f,
        D2D1_RENDER_TARGET_USAGE_NONE,
        D2D1_FEATURE_LEVEL_DEFAULT);

    hr = factory_->CreateWicBitmapRenderTarget(
        offscreenBitmap_.Get(), props, target_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        const auto softwareProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED),
            0.0f, 0.0f,
            D2D1_RENDER_TARGET_USAGE_NONE,
            D2D1_FEATURE_LEVEL_DEFAULT);
        hr = factory_->CreateWicBitmapRenderTarget(
            offscreenBitmap_.Get(), softwareProps, target_.ReleaseAndGetAddressOf());
    }
    if (FAILED(hr)) return false;

    if (!CreateDrawingResources()) return false;
    if (!backgroundPath_.empty() && (!LoadBackgroundSource() || !CreateBackgroundBitmap())) {
        return false;
    }

    return true;
}

bool Visualizer::CreateDeviceResources() {
    if (target_) return true;
    if (!hwnd_ || !factory_) return false;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const UINT w = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
    const UINT h = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));

    const auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_IGNORE),
        0.0f, 0.0f,
        D2D1_RENDER_TARGET_USAGE_NONE,
        D2D1_FEATURE_LEVEL_DEFAULT);
    const auto hwndProps = D2D1::HwndRenderTargetProperties(
        hwnd_, D2D1::SizeU(w, h));

    if (FAILED(factory_->CreateHwndRenderTarget(
        props, hwndProps, hwndTarget_.ReleaseAndGetAddressOf()))) {
        return false;
    }

    target_ = hwndTarget_;
    if (!CreateDrawingResources()) {
        DiscardDeviceResources();
        return false;
    }

    if (backgroundPath_.empty()) return true;
    if (!backgroundSource_ && !LoadBackgroundSource()) return true;
    if (FAILED(CreateBackgroundBitmap())) backgroundBitmap_.Reset();
    return true;
}

bool Visualizer::CreateDrawingResources() {
    if (!target_) return false;

    if (FAILED(target_->CreateSolidColorBrush(
        D2D1::ColorF(D2D1::ColorF::White), white_.ReleaseAndGetAddressOf())) ||
        FAILED(target_->CreateSolidColorBrush(
            D2D1::ColorF(0.02f, 0.03f, 0.05f, 1.0f), black_.ReleaseAndGetAddressOf())) ||
        FAILED(target_->CreateSolidColorBrush(
            D2D1::ColorF(0.01f, 0.015f, 0.025f, 1.0f), darkBg_.ReleaseAndGetAddressOf()))) {
        return false;
    }

    if (!dwriteFactory_) {
        HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwriteFactory_.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) return false;
    }
    if (!CreateC4LabelFormat()) return false;

    {
        D2D1_GRADIENT_STOP stops[] = {
            {0.00f, D2D1::ColorF(0.98f, 0.98f, 0.98f, 1.0f)},
            {0.12f, D2D1::ColorF(0.90f, 0.91f, 0.93f, 1.0f)},
            {0.72f, D2D1::ColorF(0.80f, 0.83f, 0.87f, 1.0f)},
            {1.00f, D2D1::ColorF(0.61f, 0.64f, 0.68f, 1.0f)},
        };
        ComPtr<ID2D1GradientStopCollection> collection;
        if (FAILED(target_->CreateGradientStopCollection(
            stops, 4, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
            collection.ReleaseAndGetAddressOf()))) return false;
        if (FAILED(target_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, 1.0f)),
            collection.Get(), whiteKeyBrush_.ReleaseAndGetAddressOf()))) return false;
    }

    {
        D2D1_GRADIENT_STOP stops[] = {
            {0.00f, D2D1::ColorF(0.20f, 0.23f, 0.28f, 1.0f)},
            {0.18f, D2D1::ColorF(0.09f, 0.11f, 0.14f, 1.0f)},
            {1.00f, D2D1::ColorF(0.02f, 0.025f, 0.04f, 1.0f)},
        };
        ComPtr<ID2D1GradientStopCollection> collection;
        if (FAILED(target_->CreateGradientStopCollection(
            stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
            collection.ReleaseAndGetAddressOf()))) return false;
        if (FAILED(target_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, 1.0f)),
            collection.Get(), blackKeyBrush_.ReleaseAndGetAddressOf()))) return false;
    }

    const std::size_t brushCount = std::max<std::size_t>(kPaletteCount, trackColors_.size());
    noteBrushes_.clear();
    glowBrushes_.clear();
    smokeBrushes_.clear();
    beamBrushes_.clear();
    activeBlackBrushes_.clear();
    noteBrushes_.resize(brushCount);
    glowBrushes_.resize(brushCount);
    smokeBrushes_.resize(brushCount);
    beamBrushes_.resize(brushCount);
    activeBlackBrushes_.resize(brushCount);

    for (std::size_t i = 0; i < brushCount; ++i) {
        if (FAILED(target_->CreateSolidColorBrush(
            NoteColorForTrack(static_cast<int>(i)), noteBrushes_[i].ReleaseAndGetAddressOf())) ||
            FAILED(target_->CreateSolidColorBrush(
                GlowColorForTrack(static_cast<int>(i)), glowBrushes_[i].ReleaseAndGetAddressOf()))) {
            return false;
        }
    }

    return CreateEffectBrushes();
}

bool Visualizer::CreateEffectBrushes() {
    if (!target_) return false;
    if (smokeBrushes_.size() != noteBrushes_.size()) smokeBrushes_.resize(noteBrushes_.size());
    if (beamBrushes_.size() != noteBrushes_.size()) beamBrushes_.resize(noteBrushes_.size());
    if (activeBlackBrushes_.size() != noteBrushes_.size()) activeBlackBrushes_.resize(noteBrushes_.size());

    for (size_t i = 0; i < noteBrushes_.size(); ++i) {
        const int track = static_cast<int>(i);
        const D2D1::ColorF mainColor = NoteColorForTrack(track);
        const D2D1::ColorF glowColor = GlowColorForTrack(track);

        D2D1_GRADIENT_STOP smokeStops[3] = {
            {0.0f, D2D1::ColorF(glowColor.r, glowColor.g, glowColor.b, 1.0f)},
            {0.40f, D2D1::ColorF(glowColor.r, glowColor.g, glowColor.b, 1.0f)},
            {1.0f, D2D1::ColorF(glowColor.r, glowColor.g, glowColor.b, 0.0f)},
        };
        ComPtr<ID2D1GradientStopCollection> smokeCollection;
        if (FAILED(target_->CreateGradientStopCollection(
            smokeStops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
            smokeCollection.ReleaseAndGetAddressOf()))) return false;

        if (FAILED(target_->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(
                D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, 0.0f), 1.0f, 1.0f),
            smokeCollection.Get(), smokeBrushes_[i].ReleaseAndGetAddressOf()))) return false;

        // Active white keys fade from the track glow into transparency.  The
        // static cool-gray key underneath therefore becomes progressively more
        // visible toward the bottom instead of being overwritten by white.
        D2D1_GRADIENT_STOP beamStops[4] = {
            {0.0f, D2D1::ColorF(mainColor.r, mainColor.g, mainColor.b, 0.98f)},
            {0.42f, D2D1::ColorF(glowColor.r, glowColor.g, glowColor.b, 0.72f)},
            {0.74f, D2D1::ColorF(mainColor.r, mainColor.g, mainColor.b, 0.30f)},
            {1.0f, D2D1::ColorF(mainColor.r, mainColor.g, mainColor.b, 0.0f)},
        };
        ComPtr<ID2D1GradientStopCollection> beamCollection;
        if (FAILED(target_->CreateGradientStopCollection(
            beamStops, 4, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
            beamCollection.ReleaseAndGetAddressOf()))) return false;
        if (FAILED(target_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, 1.0f)),
            beamCollection.Get(), beamBrushes_[i].ReleaseAndGetAddressOf()))) return false;

        const D2D1::ColorF darkColor(
            mainColor.r * 0.34f, mainColor.g * 0.34f, mainColor.b * 0.34f, 1.0f);
        // Active black keys use the same transparent fade, revealing the black
        // metallic key beneath more strongly toward the lower part of the key.
        D2D1_GRADIENT_STOP blackActiveStops[4] = {
            {0.0f, D2D1::ColorF(mainColor.r, mainColor.g, mainColor.b, 0.98f)},
            {0.34f, D2D1::ColorF(glowColor.r, glowColor.g, glowColor.b, 0.72f)},
            {0.72f, D2D1::ColorF(darkColor.r, darkColor.g, darkColor.b, 0.28f)},
            {1.0f, D2D1::ColorF(darkColor.r, darkColor.g, darkColor.b, 0.0f)},
        };
        ComPtr<ID2D1GradientStopCollection> blackActiveCollection;
        if (FAILED(target_->CreateGradientStopCollection(
            blackActiveStops, 4, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
            blackActiveCollection.ReleaseAndGetAddressOf()))) return false;
        if (FAILED(target_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, 1.0f)),
            blackActiveCollection.Get(), activeBlackBrushes_[i].ReleaseAndGetAddressOf()))) return false;
    }
    return true;
}

void Visualizer::DiscardDeviceResources() {
    backgroundBitmap_.Reset();
    beamBrushes_.clear();
    activeBlackBrushes_.clear();
    whiteKeyBrush_.Reset();
    blackKeyBrush_.Reset();
    smokeBrushes_.clear();
    noteBrushes_.clear();
    glowBrushes_.clear();
    target_.Reset();
    hwndTarget_.Reset();
    white_.Reset();
    black_.Reset();
    darkBg_.Reset();
    offscreenBitmap_.Reset();
}

void Visualizer::Resize(UINT width, UINT height) {
    width_ = static_cast<float>(std::max<UINT>(1, width));
    height_ = static_cast<float>(std::max<UINT>(1, height));
    if (hwndTarget_) {
        hwndTarget_->Resize(D2D1::SizeU(static_cast<UINT>(width_), static_cast<UINT>(height_)));
    }
    RebuildGeometry(width_, height_);
    if (dwriteFactory_ && target_) {
        CreateC4LabelFormat();
    }
}


void Visualizer::RebuildGeometry(float width, float height) {
    keyboardHeight_ = std::max(110.0f, std::min(220.0f, height * 0.20f));
    hitY_ = height - keyboardHeight_;
}

bool Visualizer::CreateC4LabelFormat() {
    if (!dwriteFactory_) return false;

    // Scale the C4 label with the keyboard itself. Keep it slightly larger
    // than the previous version while staying comfortably inside the white key.
    const float fontSize = std::max(11.0f, std::min(18.0f,
        12.0f * (keyboardHeight_ / 160.0f)));

    c4LabelFormat_.Reset();
    HRESULT hr = dwriteFactory_->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, fontSize, L"ja-JP",
        c4LabelFormat_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;
    c4LabelFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    c4LabelFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    c4LabelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return true;
}

void Visualizer::SetSong(const MidiSong* song) {
    song_ = song;
    maxNoteDuration_ = 1.0;

    size_t trackCount = 0;
    if (song_) {
        trackCount = song_->tracks.size();
        for (const auto& note : song_->notes) {
            maxNoteDuration_ = std::max(maxNoteDuration_, note.end - note.start);
            if (note.track >= 0) trackCount = std::max(trackCount, static_cast<size_t>(note.track + 1));
        }
    }
    if (trackCount == 0 && song_) trackCount = 1;

    trackColors_.assign(trackCount, D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
    customTrackColors_.assign(trackCount, false);

    int visibleTrackIndex = 0;
    for (int track = 0; track < static_cast<int>(trackCount); ++track) {
        int noteCount = 0;
        if (song_ && track < static_cast<int>(song_->tracks.size())) {
            noteCount = song_->tracks[static_cast<size_t>(track)].noteCount;
        }
        if (noteCount <= 0 && song_) {
            for (const auto& note : song_->notes) if (note.track == track) ++noteCount;
        }
        if (noteCount > 0) {
            trackColors_[static_cast<size_t>(track)] = kPalettes[TrackIndex(visibleTrackIndex)].main;
            ++visibleTrackIndex;
        }
    }

    currentTime_ = 0.0;
    playing_ = false;
    ResetEffectSimulation(-0.001);

    if (target_ && hwnd_) {
        DiscardDeviceResources();
        CreateDeviceResources();
    }
}

void Visualizer::SetPlaying(bool playing) {
    playing_ = playing;
}

void Visualizer::Reset() {
    currentTime_ = 0.0;
    playing_ = false;
    ResetEffectSimulation(-0.001);
}

void Visualizer::SetTime(double seconds) {
    if (!song_) {
        currentTime_ = 0.0;
        ResetEffectSimulation(-0.001);
        return;
    }

    double clamped = std::clamp(seconds, -2.0, song_->duration);
    if (!effectInitialized_ || std::abs(clamped - currentTime_) > 0.20) {
        ResetEffectSimulation(clamped - 0.001);
    }
    currentTime_ = clamped;
}

void Visualizer::SetEffectMask(EffectMask mask) {
    effectMask_ = mask;
    ResetEffectSimulation(currentTime_ - 0.001);
}

void Visualizer::SetEffectEnabled(EffectFlag flag, bool enabled) {
    if (enabled) effectMask_ |= static_cast<EffectMask>(flag);
    else effectMask_ &= ~static_cast<EffectMask>(flag);
    ResetEffectSimulation(currentTime_ - 0.001);
}

bool Visualizer::SetTrackColor(int track, const D2D1::ColorF& color) {
    if (track < 0) return false;
    const size_t index = static_cast<size_t>(track);
    if (index >= trackColors_.size()) {
        trackColors_.resize(index + 1, D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
        customTrackColors_.resize(index + 1, false);
    }
    trackColors_[index] = D2D1::ColorF(Clamp01(color.r), Clamp01(color.g), Clamp01(color.b), 1.0f);
    customTrackColors_[index] = true;

    if (target_) {
        if (index >= noteBrushes_.size()) {
            if (hwnd_) {
                DiscardDeviceResources();
                if (!CreateDeviceResources()) return false;
            }
        } else {
            noteBrushes_[index]->SetColor(NoteColorForTrack(track));
            glowBrushes_[index]->SetColor(GlowColorForTrack(track));
            if (!CreateEffectBrushes()) return false;
        }
    }
    return true;
}

void Visualizer::ResetTrackColors() {
    for (size_t i = 0; i < customTrackColors_.size(); ++i) customTrackColors_[i] = false;
    if (!target_) return;

    for (size_t i = 0; i < noteBrushes_.size(); ++i) {
        noteBrushes_[i]->SetColor(NoteColorForTrack(static_cast<int>(i)));
        glowBrushes_[i]->SetColor(GlowColorForTrack(static_cast<int>(i)));
    }
    CreateEffectBrushes();
}

D2D1::ColorF Visualizer::GetTrackColor(int track) const {
    return NoteColorForTrack(track);
}

bool Visualizer::HasCustomTrackColor(int track) const {
    if (track < 0) return false;
    const size_t index = static_cast<size_t>(track);
    return index < customTrackColors_.size() && customTrackColors_[index];
}

bool Visualizer::LoadBackgroundSource() {
    if (!wicFactory_ || backgroundPath_.empty()) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wicFactory_->CreateDecoderFromFilename(
        backgroundPath_.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, decoder.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    ComPtr<IWICFormatConverter> converter;
    hr = wicFactory_->CreateFormatConverter(converter.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    hr = converter->Initialize(
        frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return false;

    backgroundSource_ = converter;
    return true;
}

bool Visualizer::LoadBackgroundImage(const std::wstring& path) {
    if (path.empty()) return false;

    backgroundPath_ = path;
    backgroundSource_.Reset();
    backgroundBitmap_.Reset();

    if (!LoadBackgroundSource()) {
        backgroundPath_.clear();
        return false;
    }

    if (target_ && FAILED(CreateBackgroundBitmap())) {
        backgroundSource_.Reset();
        backgroundPath_.clear();
        return false;
    }
    return true;
}

bool Visualizer::CreateBackgroundBitmap() {
    if (!target_ || !backgroundSource_) return false;
    backgroundBitmap_.Reset();
    return SUCCEEDED(target_->CreateBitmapFromWicBitmap(
        backgroundSource_.Get(), nullptr, backgroundBitmap_.ReleaseAndGetAddressOf()));
}

void Visualizer::SetBackgroundOpacity(float opacity) {
    backgroundOpacity_ = Clamp01(opacity);
}

bool Visualizer::IsBlackKey(int note) const {
    switch (note % 12) {
    case 1:
    case 3:
    case 6:
    case 8:
    case 10:
        return true;
    default:
        return false;
    }
}

int Visualizer::WhiteIndex(int note) const {
    int index = 0;
    for (int n = kFirstKey; n < note; ++n) if (!IsBlackKey(n)) ++index;
    return index;
}

float Visualizer::KeyX(int note) const {
    const float whiteW = width_ / kWhiteKeys;
    if (!IsBlackKey(note)) return WhiteIndex(note) * whiteW;

    // A black key sits between the preceding and following white keys.
    // WhiteIndex(note) is already the number of white keys before this
    // semitone, so the black key center is at that white-key boundary.
    return WhiteIndex(note) * whiteW - (whiteW * 0.62f) * 0.5f;
}

float Visualizer::KeyW(int note) const {
    const float whiteW = width_ / kWhiteKeys;
    return IsBlackKey(note) ? whiteW * 0.62f : whiteW;
}

D2D1::ColorF Visualizer::NoteColorForTrack(int track) const {
    if (track >= 0) {
        const size_t index = static_cast<size_t>(track);
        if (index < trackColors_.size()) return trackColors_[index];
    }
    return kPalettes[TrackIndex(track)].main;
}

D2D1::ColorF Visualizer::GlowColorForTrack(int track) const {
    const D2D1::ColorF base = NoteColorForTrack(track);
    return D2D1::ColorF(
        Lerp(base.r, 1.0f, 0.24f),
        Lerp(base.g, 1.0f, 0.24f),
        Lerp(base.b, 1.0f, 0.24f),
        1.0f);
}

size_t Visualizer::TrackBrushIndex(int track) const {
    if (noteBrushes_.empty()) return 0;
    if (track < 0) return 0;
    const size_t index = static_cast<size_t>(track);
    return index < noteBrushes_.size() ? index : index % noteBrushes_.size();
}

ID2D1SolidColorBrush* Visualizer::NoteBrushForTrack(int track) const {
    return noteBrushes_[TrackBrushIndex(track)].Get();
}

ID2D1SolidColorBrush* Visualizer::GlowBrushForTrack(int track) const {
    return glowBrushes_[TrackBrushIndex(track)].Get();
}

ID2D1RadialGradientBrush* Visualizer::SmokeBrushForTrack(int track) const {
    return smokeBrushes_[TrackBrushIndex(track)].Get();
}

ID2D1LinearGradientBrush* Visualizer::BeamBrushForTrack(int track) const {
    return beamBrushes_[TrackBrushIndex(track)].Get();
}

ID2D1LinearGradientBrush* Visualizer::ActiveBlackBrushForTrack(int track) const {
    return activeBlackBrushes_[TrackBrushIndex(track)].Get();
}

void Visualizer::DrawBackground(float width, float height) {
    target_->Clear(D2D1::ColorF(0.005f, 0.008f, 0.014f));
    if (backgroundBitmap_ && backgroundOpacity_ > 0.0f) {
        target_->DrawBitmap(
            backgroundBitmap_.Get(), D2D1::RectF(0.0f, 0.0f, width, height),
            backgroundOpacity_, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
}

void Visualizer::DrawNoteGuides(float width, float height) {
    (void)height;
    if (!target_ || !showNoteGuides_) return;
    // B-to-C octave boundaries are intentionally more visible than before,
    // while remaining weaker than the falling notes. The setting is user-
    // configurable from the control dialog and is also copied into video
    // rendering through InitializeOffscreen().
    white_->SetOpacity(width >= 900.0f ? 0.25f : 0.22f);
    const float lineWidth = width >= 1600.0f ? 1.6f : (width >= 900.0f ? 1.4f : 1.1f);
    for (int note = kFirstKey; note <= kLastKey; ++note) {
        if (note % 12 != 0 || note == kFirstKey) continue;
        const float x = KeyX(note);
        if (x <= 0.0f || x >= width) continue;
        target_->DrawLine(
            D2D1::Point2F(x, 0.0f), D2D1::Point2F(x, hitY_), white_.Get(), lineWidth);
    }
    white_->SetOpacity(1.0f);
}

void Visualizer::DrawNotes(float width, float height) {
    (void)width;
    (void)height;
    if (!song_ || song_->notes.empty()) return;

    const double lead = hitY_ / fallSpeed_;
    const double minTime = currentTime_ - maxNoteDuration_ - 0.1;
    const double maxTime = currentTime_ + lead + 0.1;
    auto firstIt = std::lower_bound(song_->notes.begin(), song_->notes.end(), minTime,
        [](const MidiNote& n, double t) { return n.start < t; });
    auto lastIt = std::lower_bound(song_->notes.begin(), song_->notes.end(), maxTime,
        [](const MidiNote& n, double t) { return n.start < t; });

    for (auto it = firstIt; it != lastIt; ++it) {
        const MidiNote& n = *it;
        if (n.note < kFirstKey || n.note > kLastKey) continue;

        const bool blackKey = IsBlackKey(n.note);
        const float x0 = KeyX(n.note) + (blackKey ? 2.0f : 1.7f);
        const float w = std::max(2.0f, KeyW(n.note) - (blackKey ? 4.0f : 3.4f));
        const float rawBottom = hitY_ - static_cast<float>((n.start - currentTime_) * fallSpeed_);
        const float noteHeight = std::max(4.0f, static_cast<float>((n.end - n.start) * fallSpeed_));
        const float rawTop = rawBottom - noteHeight;

        // The note must enter through its leading/top edge.  The previous code
        // clamped the bottom edge to 12.5px even while the whole note was still
        // above the viewport, producing a tiny one-pixel "tip" before the body
        // visibly started falling.  Clip the note only after it actually crosses
        // the top edge so its visible height grows smoothly from zero.
        constexpr float kTopClip = 12.0f;
        const float top = std::max(kTopClip, rawTop);
        const float bottom = std::min(hitY_ + 10.0f, rawBottom);
        if (rawBottom <= kTopClip || bottom <= top) continue;

        ID2D1SolidColorBrush* brush = NoteBrushForTrack(n.track);
        const float velocity01 = Velocity01(n.velocity);
        const float velocityOpacity = VelocityOpacity(n.velocity);

        if (IsEffectEnabled(EffectNoteGlow)) {
            // Make the glow deliberately strong and distribute it around the entire
            // note rectangle.  The shell layers are opaque enough to remain clearly
            // visible even on bright note fills and on long sustained notes.
            const float intensity = 0.92f + 0.08f * velocity01;
            const float radius = std::min(5.0f, w * 0.16f);
            ID2D1SolidColorBrush* glow = GlowBrushForTrack(n.track);
            const float pads[] = {16.0f, 12.0f, 9.0f, 6.0f, 3.0f};
            const float opacities[] = {0.075f, 0.095f, 0.13f, 0.18f, 0.25f};
            for (int i = 0; i < 5; ++i) {
                const float pad = pads[i];
                glow->SetOpacity(opacities[i] * intensity);
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        D2D1::RectF(x0 - pad, top - pad, x0 + w + pad, bottom + pad),
                        radius + pad * 0.70f, radius + pad * 0.70f),
                    glow);
            }
            glow->SetOpacity(0.46f * intensity);
            target_->DrawRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(x0 - 2.0f, top - 2.0f, x0 + w + 2.0f, bottom + 2.0f),
                    radius + 1.2f, radius + 1.2f),
                glow, 2.0f);
        }

        brush->SetOpacity(velocityOpacity);
        const float radius = std::min(4.0f, w * 0.16f);
        const auto rr = D2D1::RoundedRect(D2D1::RectF(x0, top, x0 + w, bottom), radius, radius);
        target_->FillRoundedRectangle(rr, brush);

        white_->SetOpacity(0.18f);
        if (w > 6.0f) {
            target_->FillRectangle(
                D2D1::RectF(x0 + w * 0.18f, top + 1.0f,
                            x0 + w * 0.31f, bottom - 1.0f), white_.Get());
        }
        white_->SetOpacity(0.40f + 0.40f * velocity01);
        target_->DrawRoundedRectangle(rr, white_.Get(), std::min(1.6f, w * 0.08f));
        brush->SetOpacity(1.0f);
    }
}

void Visualizer::UpdateActiveNotes() {
    activeTracks_.fill(-1);
    activeVelocities_.fill(0);
    activeNoteCount_ = 0;
    if (!song_ || song_->notes.empty()) return;

    const double t = currentTime_;
    // Search far enough back to include long-held notes.  The previous
    // predecessor-walk could stop at a short note that had already ended,
    // accidentally skipping an older note that was still being held.
    const double searchStart = std::max(0.0, t - maxNoteDuration_ - 0.001);
    auto it = std::lower_bound(song_->notes.begin(), song_->notes.end(), searchStart,
        [](const MidiNote& n, double time) { return n.start < time; });

    for (; it != song_->notes.end(); ++it) {
        if (it->start > t + 0.05) break;
        if (it->note < kFirstKey || it->note > kLastKey) continue;
        if (!(it->start <= t && t < it->end)) continue;
        if (activeTracks_[it->note] < 0) {
            activeNoteList_[activeNoteCount_++] = it->note;
            activeTracks_[it->note] = it->track;
            activeVelocities_[it->note] = it->velocity;
        }
    }
}

void Visualizer::SpawnEffect(const MidiNote& note, int ordinal) {
    if (effectMask_ == EffectNone || note.note < kFirstKey || note.note > kLastKey) return;

    const float keyX = KeyX(note.note);
    const float noteKeyW = KeyW(note.note);
    const float whiteW = width_ / static_cast<float>(kWhiteKeys);
    const float effectWidth = IsBlackKey(note.note) ? whiteW : noteKeyW;
    const float centerX = keyX + noteKeyW * 0.5f;

    std::uint32_t seed = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(std::llround(note.start * 1000.0)) * 2654435761ULL) ^
        (static_cast<std::uint64_t>(note.note) * 2246822519ULL) ^
        (static_cast<std::uint64_t>(ordinal + 1) * 3266489917ULL));

    if (IsEffectEnabled(EffectRipple) && rippleCount_ < kMaxRipples) {
        Ripple& r = ripples_[rippleCount_++];
        r.x = centerX + (Random01(seed) - 0.5f) * effectWidth * 0.24f;
        r.y = hitY_ + 1.0f;
        r.radius = std::max(1.5f, effectWidth * 0.08f);
        r.startRadius = r.radius;
        r.maxRadius = std::max(14.0f, effectWidth * 1.85f);
        r.life = 0.0f;
        r.maxLife = 0.62f + Random01(seed) * 0.16f;
        r.thickness = std::max(1.0f, effectWidth * 0.11f);
        r.track = note.track;
        r.diamond = false;
    }
    if (IsEffectEnabled(EffectDiamondRipple) && rippleCount_ < kMaxRipples) {
        Ripple& r = ripples_[rippleCount_++];
        r.x = centerX + (Random01(seed) - 0.5f) * effectWidth * 0.24f;
        r.y = hitY_ + 1.0f;
        r.radius = std::max(1.5f, effectWidth * 0.08f);
        r.startRadius = r.radius;
        r.maxRadius = std::max(14.0f, effectWidth * 1.85f);
        r.life = 0.0f;
        r.maxLife = 0.62f + Random01(seed) * 0.16f;
        r.thickness = std::max(1.0f, effectWidth * 0.11f);
        r.track = note.track;
        r.diamond = true;
    }

    if (IsEffectEnabled(EffectSpark)) {
        for (int i = 0; i < kCinemaParticleCount && particleCount_ < kMaxParticles; ++i) {
            Particle& p = particles_[particleCount_++];
            const float scale = 0.85f + Random01(seed) * 0.45f;
            const float xRandom = 0.20f + Random01(seed) * 0.60f;
            const float angle = (-3.14159265358979323846f * 0.95f) +
                Random01(seed) * (3.14159265358979323846f * 0.90f);
            const float speed = (Random01(seed) * 260.0f + 110.0f) * scale;
            p.x = (centerX - effectWidth * 0.5f) + effectWidth * xRandom;
            p.y = hitY_ + 2.0f;
            p.vx = std::cos(angle) * speed;
            p.vy = std::sin(angle) * speed;
            p.life = 0.28f + Random01(seed) * 0.46f;
            p.maxLife = p.life;
            p.size = (1.2f + Random01(seed) * 3.2f) * scale;
            p.maxSize = p.size;
            p.gravity = 520.0f * scale;
            p.track = note.track;
            p.phase = 0.0f;
            p.windPhase = 0.0f;
            p.spark = true;
            p.whiteSpark = ((i % 4) == 0);
            p.crossSpark = false;
        }
    }
}

void Visualizer::SpawnSmokeBurst(const MidiNote& note, std::int64_t emissionIndex) {
    if (!IsEffectEnabled(EffectSmokeGlow) || particleCount_ >= kMaxParticles) return;
    if (note.note < kFirstKey || note.note > kLastKey) return;

    const float keyX = KeyX(note.note);
    const float noteKeyW = KeyW(note.note);
    const float whiteW = width_ / static_cast<float>(kWhiteKeys);
    const float effectWidth = IsBlackKey(note.note) ? whiteW : noteKeyW;
    const float centerX = keyX + noteKeyW * 0.5f;
    const std::uint64_t startMs = static_cast<std::uint64_t>(std::llround(note.start * 1000.0));
    std::uint32_t seed = static_cast<std::uint32_t>(
        (startMs * 2654435761ULL) ^
        (static_cast<std::uint64_t>(note.note) * 2246822519ULL) ^
        (static_cast<std::uint64_t>(note.track + 1) * 3266489917ULL) ^
        (static_cast<std::uint64_t>(emissionIndex + 0x9e3779b97f4a7c15LL) * 668265263ULL));

    for (int i = 0; i < kSmokeParticlesPerBurst && particleCount_ < kMaxParticles; ++i) {
        Particle& p = particles_[particleCount_++];
        const float scale = 0.9f + Random01(seed) * 0.5f;
        const float xRandom = 0.15f + Random01(seed) * 0.70f;
        const float angle = -3.14159265358979323846f * 0.5f +
            (Random01(seed) - 0.5f) * 0.35f;
        const float speed = (Random01(seed) * 110.0f + 70.0f) * scale;
        p.x = (centerX - effectWidth * 0.5f) + effectWidth * xRandom;
        p.y = hitY_ - 2.0f;
        p.vx = std::cos(angle) * speed;
        p.vy = std::sin(angle) * speed;
        p.life = 1.20f + Random01(seed) * 0.80f;
        p.maxLife = p.life;
        p.size = (2.2f + Random01(seed) * 3.8f) * scale;
        p.maxSize = p.size * (1.8f + Random01(seed) * 0.9f);
        p.gravity = 130.0f;
        p.track = note.track;
        p.phase = 0.0f;
        p.windPhase = Random01(seed) * 6.2831853071795864769f;
        p.spark = false;
        p.whiteSpark = false;
        p.crossSpark = false;
    }
}

void Visualizer::SpawnCrossBurst(const MidiNote& note, std::int64_t emissionIndex) {
    if (!IsEffectEnabled(EffectCrossSpark) || particleCount_ >= kMaxParticles) return;
    if (note.note < kFirstKey || note.note > kLastKey) return;

    const float keyX = KeyX(note.note);
    const float noteKeyW = KeyW(note.note);
    const float whiteW = width_ / static_cast<float>(kWhiteKeys);
    const float effectWidth = IsBlackKey(note.note) ? whiteW : noteKeyW;
    const float centerX = keyX + noteKeyW * 0.5f;
    const std::uint64_t startMs = static_cast<std::uint64_t>(std::llround(note.start * 1000.0));
    std::uint32_t seed = static_cast<std::uint32_t>(
        (startMs * 2246822519ULL) ^
        (static_cast<std::uint64_t>(note.note) * 3266489917ULL) ^
        (static_cast<std::uint64_t>(note.track + 1) * 668265263ULL) ^
        (static_cast<std::uint64_t>(emissionIndex + 0x9e3779b97f4a7c15LL) * 2654435761ULL));

    for (int i = 0; i < kCrossParticlesPerBurst && particleCount_ < kMaxParticles; ++i) {
        Particle& p = particles_[particleCount_++];
        const float scale = 0.8f + Random01(seed) * 0.55f;
        const float xRandom = 0.18f + Random01(seed) * 0.64f;
        const float angle = -3.14159265358979323846f * 0.5f +
            (Random01(seed) - 0.5f) * 0.48f;
        const float speed = (Random01(seed) * 70.0f + 35.0f) * scale;
        p.x = (centerX - effectWidth * 0.5f) + effectWidth * xRandom;
        p.y = hitY_ - 2.0f;
        p.vx = std::cos(angle) * speed;
        p.vy = std::sin(angle) * speed;
        p.life = 0.70f + Random01(seed) * 0.75f;
        p.maxLife = p.life;
        p.size = (0.75f + Random01(seed) * 1.15f) * scale;
        p.maxSize = p.size * 1.25f;
        p.gravity = 90.0f * scale;
        p.track = note.track;
        p.phase = 0.0f;
        p.windPhase = Random01(seed) * 6.2831853071795864769f;
        p.spark = false;
        p.whiteSpark = false;
        p.crossSpark = true;
    }
}

void Visualizer::ResetEffectSimulation(double time) {
    particleCount_ = 0;
    rippleCount_ = 0;
    effectSimTime_ = time;
    effectInitialized_ = true;
}

void Visualizer::UpdateEffects() {
    if (effectMask_ == EffectNone || !song_) return;

    const double time = currentTime_;
    if (!effectInitialized_ || time < effectSimTime_ || time - effectSimTime_ > 0.25) {
        ResetEffectSimulation(time - 0.001);
        return;
    }

    const double previousTime = effectSimTime_;
    const double dt = std::min(0.033, std::max(0.0, time - previousTime));

    auto firstIt = std::upper_bound(song_->notes.begin(), song_->notes.end(), previousTime,
        [](double t, const MidiNote& n) { return t < n.start; });
    auto lastIt = std::upper_bound(song_->notes.begin(), song_->notes.end(), time,
        [](double t, const MidiNote& n) { return t < n.start; });

    int ordinal = 0;
    for (auto it = firstIt; it != lastIt; ++it, ++ordinal) {
        if (it->start > previousTime && it->start <= time) SpawnEffect(*it, ordinal);
    }

    auto emitHeldParticles = [&](bool enabled, auto&& spawn) {
        if (!enabled) return;
        const double searchStart = std::max(0.0,
            previousTime - maxNoteDuration_ - kSmokeEmissionInterval);
        auto first = std::lower_bound(song_->notes.begin(), song_->notes.end(), searchStart,
            [](const MidiNote& n, double t) { return n.start < t; });
        auto last = std::upper_bound(song_->notes.begin(), song_->notes.end(), time,
            [](double t, const MidiNote& n) { return t < n.start; });

        for (auto it = first; it != last; ++it) {
            const MidiNote& note = *it;
            if (note.note < kFirstKey || note.note > kLastKey) continue;
            if (note.end <= previousTime || note.start > time) continue;

            const double interval = static_cast<double>(kSmokeEmissionInterval);
            const double emitStart = std::max(previousTime, note.start);
            const double emitEnd = std::min(time, note.end);
            if (emitEnd <= emitStart) continue;

            const auto firstIndex = static_cast<std::int64_t>(
                std::ceil((emitStart - note.start) / interval - 1e-9));
            const auto lastIndex = static_cast<std::int64_t>(
                std::floor((emitEnd - note.start) / interval + 1e-9));
            if (lastIndex < firstIndex) continue;

            for (std::int64_t emissionIndex = std::max<std::int64_t>(0, firstIndex);
                 emissionIndex <= lastIndex; ++emissionIndex) {
                spawn(note, emissionIndex);
                if (particleCount_ >= kMaxParticles) return;
            }
        }
    };

    emitHeldParticles(IsEffectEnabled(EffectSmokeGlow),
        [this](const MidiNote& note, std::int64_t emissionIndex) {
            SpawnSmokeBurst(note, emissionIndex);
        });
    emitHeldParticles(IsEffectEnabled(EffectCrossSpark),
        [this](const MidiNote& note, std::int64_t emissionIndex) {
            SpawnCrossBurst(note, emissionIndex);
        });

    for (int i = particleCount_ - 1; i >= 0; --i) {
        Particle& p = particles_[i];
        p.life -= static_cast<float>(dt);
        if (p.life <= 0.0f) {
            --particleCount_;
            if (i != particleCount_) particles_[i] = particles_[particleCount_];
            continue;
        }

        const float d = static_cast<float>(dt);
        if (p.spark) {
            p.vx *= std::pow(0.985f, d * 60.0f);
            p.vy *= std::pow(0.985f, d * 60.0f);
            p.x += p.vx * d;
            p.y += p.vy * d;
            p.vy += p.gravity * d;
        } else {
            const float windForce = std::sin(p.life * 6.0f + p.windPhase) * 35.0f + 28.0f;
            p.vx += windForce * d;
            p.vx *= std::pow(0.96f, d * 60.0f);
            p.vy *= std::pow(0.96f, d * 60.0f);
            p.x += p.vx * d;
            p.y += p.vy * d;
            p.vy -= p.gravity * d;
        }
    }

    if (IsEffectEnabled(EffectRipple) || IsEffectEnabled(EffectDiamondRipple)) {
        for (int i = rippleCount_ - 1; i >= 0; --i) {
            Ripple& ripple = ripples_[i];
            ripple.life += static_cast<float>(dt);
            if (ripple.life >= ripple.maxLife) {
                --rippleCount_;
                if (i != rippleCount_) ripples_[i] = ripples_[rippleCount_];
                continue;
            }
            const float u = Clamp01(ripple.life / ripple.maxLife);
            const float eased = 1.0f - (1.0f - u) * (1.0f - u);
            ripple.radius = Lerp(ripple.startRadius, ripple.maxRadius, eased);
        }
    } else {
        rippleCount_ = 0;
    }
    effectSimTime_ = time;
}

void Visualizer::DrawEffects(float width, float height) {
    if (effectMask_ == EffectNone || !song_) return;

    const bool drawUpperEffects =
        IsEffectEnabled(EffectSmokeGlow) ||
        IsEffectEnabled(EffectCrossSpark) ||
        IsEffectEnabled(EffectSpark);

    if (drawUpperEffects) {
        target_->PushAxisAlignedClip(
            D2D1::RectF(0.0f, 0.0f, width, hitY_),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        if (IsEffectEnabled(EffectSmokeGlow)) {
            for (int i = 0; i < particleCount_; ++i) {
                const Particle& p = particles_[i];
                if (p.spark || p.crossSpark) continue;
                const float progress = 1.0f - (p.life / p.maxLife);
                const float currentSize = p.size +
                    (p.maxSize - p.size) * std::sin(
                        progress * 3.14159265358979323846f * 0.5f);
                const float alpha = std::sin(progress * 3.14159265358979323846f) * 0.40f;
                if (alpha <= 0.0f) continue;

                ID2D1RadialGradientBrush* smoke = SmokeBrushForTrack(p.track);
                smoke->SetCenter(D2D1::Point2F(p.x, p.y));
                smoke->SetGradientOriginOffset(D2D1::Point2F(0.0f, 0.0f));
                smoke->SetRadiusX(std::max(1.0f, currentSize));
                smoke->SetRadiusY(std::max(1.0f, currentSize));
                smoke->SetOpacity(alpha);
                target_->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(p.x, p.y), currentSize * 0.70f,
                                  currentSize * 1.30f), smoke);
            }
        }

        if (IsEffectEnabled(EffectCrossSpark)) {
            for (int i = 0; i < particleCount_; ++i) {
                const Particle& p = particles_[i];
                if (!p.crossSpark || p.life <= 0.0f) continue;
                const float a = Clamp01(p.life / p.maxLife);
                const float grow = 0.75f + 0.25f * (1.0f - a);
                const float half = std::max(1.0f, p.size * grow);
                const float halo = half * 3.2f;
                ID2D1SolidColorBrush* glow = GlowBrushForTrack(p.track);
                glow->SetOpacity(a * 0.26f);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(p.x, p.y), halo, halo), glow);
                white_->SetOpacity(a * 0.76f);
                const float line = std::max(1.0f, half * 0.55f);
                target_->DrawLine(D2D1::Point2F(p.x - half * 2.2f, p.y),
                                  D2D1::Point2F(p.x + half * 2.2f, p.y), white_.Get(), line);
                target_->DrawLine(D2D1::Point2F(p.x, p.y - half * 2.2f),
                                  D2D1::Point2F(p.x, p.y + half * 2.2f), white_.Get(), line);
                white_->SetOpacity(a * 0.95f);
                target_->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(p.x, p.y),
                                  std::max(0.45f, half * 0.38f),
                                  std::max(0.45f, half * 0.38f)), white_.Get());
            }
        }

        if (IsEffectEnabled(EffectSpark)) {
            for (int i = 0; i < particleCount_; ++i) {
                const Particle& p = particles_[i];
                if (!p.spark || p.life <= 0.0f) continue;
                const float a = p.life / p.maxLife;
                if (p.whiteSpark) {
                    white_->SetOpacity(a * 0.22f);
                    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(p.x, p.y),
                                      p.size * 2.2f, p.size * 2.2f), white_.Get());
                    white_->SetOpacity(a * 0.98f);
                    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(p.x, p.y),
                                      p.size, p.size), white_.Get());
                } else {
                    ID2D1SolidColorBrush* glow = GlowBrushForTrack(p.track);
                    glow->SetOpacity(a * 0.22f);
                    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(p.x, p.y),
                                      p.size * 2.2f, p.size * 2.2f), glow);
                    glow->SetOpacity(a * 0.98f);
                    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(p.x, p.y),
                                      p.size, p.size), glow);
                }
                white_->SetOpacity(a * 0.88f);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(p.x, p.y),
                                  std::max(0.8f, p.size * 0.30f),
                                  std::max(0.8f, p.size * 0.30f)), white_.Get());
            }
        }

        white_->SetOpacity(1.0f);
        target_->PopAxisAlignedClip();
    }

    if (IsEffectEnabled(EffectRipple) || IsEffectEnabled(EffectDiamondRipple)) {
        target_->PushAxisAlignedClip(
            D2D1::RectF(0.0f, 0.0f, width, height),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        if (IsEffectEnabled(EffectRipple)) {
            for (int i = 0; i < rippleCount_; ++i) {
                const Ripple& ripple = ripples_[i];
                if (ripple.diamond) continue;
                const float u = Clamp01(ripple.life / ripple.maxLife);
                const float fade = 1.0f - u;
                const float fade2 = fade * fade;
                ID2D1SolidColorBrush* brush = GlowBrushForTrack(ripple.track);
                brush->SetColor(GlowColorForTrack(ripple.track));

                const auto ellipse = D2D1::Ellipse(
                    D2D1::Point2F(ripple.x, ripple.y), ripple.radius, ripple.radius);

                // Soft outer halo, followed by a brighter core line.  Drawing
                // several progressively thinner rings keeps the ripple legible
                // without making the center look like a solid disk.
                brush->SetOpacity(fade2 * 0.10f);
                target_->DrawEllipse(ellipse, brush, ripple.thickness + 7.0f);
                brush->SetOpacity(fade2 * 0.20f);
                target_->DrawEllipse(ellipse, brush, ripple.thickness + 4.0f);
                brush->SetOpacity(fade2 * 0.34f);
                target_->DrawEllipse(ellipse, brush, ripple.thickness + 2.0f);
                brush->SetOpacity(fade2 * 0.95f);
                target_->DrawEllipse(ellipse, brush, ripple.thickness);
                brush->SetOpacity(fade2 * 0.22f);
                target_->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(ripple.x, ripple.y),
                                  ripple.radius * 0.72f, ripple.radius * 0.72f), brush);
            }
        }

        if (IsEffectEnabled(EffectDiamondRipple)) {
            for (int i = 0; i < rippleCount_; ++i) {
                const Ripple& ripple = ripples_[i];
                if (!ripple.diamond) continue;
                const float u = Clamp01(ripple.life / ripple.maxLife);
                const float fade = 1.0f - u;
                const float fade2 = fade * fade;
                const float radius = ripple.radius;
                ID2D1SolidColorBrush* brush = GlowBrushForTrack(ripple.track);
                brush->SetColor(GlowColorForTrack(ripple.track));
                const auto top = D2D1::Point2F(ripple.x, ripple.y - radius);
                const auto right = D2D1::Point2F(ripple.x + radius, ripple.y);
                const auto bottom = D2D1::Point2F(ripple.x, ripple.y + radius);
                const auto left = D2D1::Point2F(ripple.x - radius, ripple.y);

                // Match the circular ripple's halo treatment for the diamond
                // variant so both effects have the same visual weight.
                brush->SetOpacity(fade2 * 0.10f);
                target_->DrawLine(top, right, brush, ripple.thickness + 7.0f);
                target_->DrawLine(right, bottom, brush, ripple.thickness + 7.0f);
                target_->DrawLine(bottom, left, brush, ripple.thickness + 7.0f);
                target_->DrawLine(left, top, brush, ripple.thickness + 7.0f);
                brush->SetOpacity(fade2 * 0.20f);
                target_->DrawLine(top, right, brush, ripple.thickness + 4.0f);
                target_->DrawLine(right, bottom, brush, ripple.thickness + 4.0f);
                target_->DrawLine(bottom, left, brush, ripple.thickness + 4.0f);
                target_->DrawLine(left, top, brush, ripple.thickness + 4.0f);
                brush->SetOpacity(fade2 * 0.34f);
                target_->DrawLine(top, right, brush, ripple.thickness + 2.0f);
                target_->DrawLine(right, bottom, brush, ripple.thickness + 2.0f);
                target_->DrawLine(bottom, left, brush, ripple.thickness + 2.0f);
                target_->DrawLine(left, top, brush, ripple.thickness + 2.0f);
                brush->SetOpacity(fade2 * 0.95f);
                target_->DrawLine(top, right, brush, ripple.thickness);
                target_->DrawLine(right, bottom, brush, ripple.thickness);
                target_->DrawLine(bottom, left, brush, ripple.thickness);
                target_->DrawLine(left, top, brush, ripple.thickness);
            }
        }

        target_->PopAxisAlignedClip();
    }

    white_->SetOpacity(1.0f);
}

void Visualizer::DrawKeyboard(float width, float height) {
    darkBg_->SetColor(D2D1::ColorF(0.025f, 0.03f, 0.045f, 1.0f));
    darkBg_->SetOpacity(0.98f);
    target_->FillRectangle(D2D1::RectF(0.0f, hitY_ - 3.0f, width, height), darkBg_.Get());

    white_->SetOpacity(0.82f);
    target_->DrawLine(D2D1::Point2F(0.0f, hitY_ - 0.5f),
                      D2D1::Point2F(width, hitY_ - 0.5f), white_.Get(), 1.2f);

    if (whiteKeyBrush_) {
        whiteKeyBrush_->SetOpacity(1.0f);
        for (int n = kFirstKey; n <= kLastKey; ++n) {
            if (IsBlackKey(n)) continue;
            const float x = KeyX(n);
            const float w = KeyW(n);
            whiteKeyBrush_->SetStartPoint(D2D1::Point2F(x, hitY_));
            whiteKeyBrush_->SetEndPoint(D2D1::Point2F(x, height));
            target_->FillRectangle(
                D2D1::RectF(x + 0.5f, hitY_, x + w - 0.5f, height), whiteKeyBrush_.Get());
            black_->SetOpacity(0.30f);
            target_->DrawLine(
                D2D1::Point2F(x + w - 0.5f, hitY_ + 1.0f),
                D2D1::Point2F(x + w - 0.5f, height - 1.0f), black_.Get(), 1.0f);
            white_->SetOpacity(0.30f);
            target_->DrawLine(
                D2D1::Point2F(x + 1.0f, height - 1.5f),
                D2D1::Point2F(x + w - 1.0f, height - 1.5f), white_.Get(), 1.5f);
        }
    }

    // IMPORTANT: the active white-key fill is the final fill for white keys.
    // No static bevel/highlight is drawn after it, preventing the small white/gray
    // patch above an illuminated key that existed in earlier revisions.
    for (int i = 0; i < activeNoteCount_; ++i) {
        const int note = activeNoteList_[i];
        if (IsBlackKey(note)) continue;
        const int track = activeTracks_[note];
        const float x = KeyX(note) + 1.0f;
        const float w = std::max(2.0f, KeyW(note) - 2.0f);
        ID2D1LinearGradientBrush* gradient = BeamBrushForTrack(track);
        gradient->SetStartPoint(D2D1::Point2F(x, hitY_));
        gradient->SetEndPoint(D2D1::Point2F(x, hitY_ + keyboardHeight_));
        gradient->SetOpacity(VelocityOpacity(activeVelocities_[note]) * 0.98f);
        target_->FillRectangle(
            D2D1::RectF(x, hitY_, x + w, hitY_ + keyboardHeight_), gradient);
    }

    for (int n = kFirstKey; n <= kLastKey; ++n) {
        if (!IsBlackKey(n)) continue;
        const float x = KeyX(n);
        const float w = KeyW(n);
        const float h = keyboardHeight_ * 0.62f;
        const float r = std::min(3.5f, w * 0.12f);
        const auto keyRect = D2D1::RoundedRect(
            D2D1::RectF(x, hitY_, x + w, hitY_ + h), r, r);

        black_->SetOpacity(0.36f);
        target_->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(x + 0.8f, hitY_ + 2.8f,
                            x + w + 0.8f, hitY_ + h + 2.8f), r, r), black_.Get());

        if (blackKeyBrush_) {
            blackKeyBrush_->SetStartPoint(D2D1::Point2F(x, hitY_));
            blackKeyBrush_->SetEndPoint(D2D1::Point2F(x, hitY_ + h));
            blackKeyBrush_->SetOpacity(1.0f);
            target_->FillRoundedRectangle(keyRect, blackKeyBrush_.Get());
        } else {
            black_->SetOpacity(1.0f);
            target_->FillRoundedRectangle(keyRect, black_.Get());
        }

        const int track = activeTracks_[n];
        white_->SetOpacity(track >= 0 ? 0.76f : 0.22f);
        target_->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(x + 0.9f, hitY_ + 0.9f,
                            x + w - 0.9f,
                            hitY_ + std::max(3.0f, h * 0.12f)),
                std::min(2.0f, r), std::min(2.0f, r)), white_.Get());

        const bool active = track >= 0 &&
            static_cast<size_t>(track) < activeBlackBrushes_.size() &&
            activeBlackBrushes_[TrackBrushIndex(track)];
        if (active) {
            const float velocity01 = Velocity01(activeVelocities_[n]);
            ID2D1LinearGradientBrush* activeGradient = ActiveBlackBrushForTrack(track);
            activeGradient->SetStartPoint(D2D1::Point2F(x, hitY_));
            activeGradient->SetEndPoint(D2D1::Point2F(x, hitY_ + h));
            activeGradient->SetOpacity(VelocityOpacity(activeVelocities_[n]) * 0.98f);
            target_->FillRoundedRectangle(keyRect, activeGradient);

            glowBrushes_[TrackBrushIndex(track)]->SetOpacity(0.20f + 0.20f * velocity01);
            target_->DrawRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(x - 1.2f, hitY_ - 1.0f,
                                x + w + 1.2f, hitY_ + h + 1.2f),
                    r + 1.0f, r + 1.0f),
                glowBrushes_[TrackBrushIndex(track)].Get(), 1.0f);
        }
    }

    if (c4LabelFormat_) {
        // C4 is MIDI note 60.  Use the exact same key-position calculation as
        // the keyboard itself so the label can never drift onto B3.
        constexpr int kC4Note = 60;
        const float x = KeyX(kC4Note);
        const float w = KeyW(kC4Note);
        const auto labelRect = D2D1::RectF(
            x + 0.5f, hitY_ + keyboardHeight_ - 30.0f,
            x + w - 0.5f, hitY_ + keyboardHeight_ - 5.0f);
        black_->SetOpacity(0.48f);
        const wchar_t label[] = L"C4";
        target_->DrawTextW(
            label, static_cast<UINT32>(std::size(label) - 1), c4LabelFormat_.Get(),
            &labelRect, black_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    white_->SetOpacity(1.0f);
    black_->SetOpacity(1.0f);
}

void Visualizer::Render() {
    if (!hwnd_ || !CreateDeviceResources()) return;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const LONG clientW = std::max<LONG>(1, rc.right - rc.left);
    const LONG clientH = std::max<LONG>(1, rc.bottom - rc.top);
    const float w = static_cast<float>(clientW);
    const float h = static_cast<float>(clientH);

    if (std::abs(w - width_) > 0.5f || std::abs(h - height_) > 0.5f) {
        Resize(static_cast<UINT>(clientW), static_cast<UINT>(clientH));
    }

    UpdateActiveNotes();
    UpdateEffects();

    target_->BeginDraw();
    DrawBackground(w, h);
    DrawNoteGuides(w, h);
    DrawNotes(w, h);
    DrawKeyboard(w, h);
    DrawEffects(w, h);

    const HRESULT hr = target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) DiscardDeviceResources();
}
