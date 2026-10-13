#define UNICODE
#define _UNICODE
#define NOMINMAX

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "MidiFile.h"
#include "Visualizer.h"
#include "SimpleSynth.h"
#include "WavAudioPlayer.h"
#include "VideoRenderer.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "xaudio2.lib")

namespace {

constexpr UINT_PTR TIMER_CONTROL_REFRESH = 2;
constexpr UINT_PTR TIMER_SETTINGS_SAVE = 3;
constexpr int kPositionSliderMax = 10000;
constexpr int kDefaultTailMilliseconds = 3000;
constexpr int kMaximumTailMilliseconds = 60000;

HINSTANCE g_instance = nullptr;
HWND g_visualizerHwnd = nullptr;
HWND g_controlHwnd = nullptr;
Visualizer g_visualizer;
MidiSong g_song;
SimpleSynth g_simpleSynth;
WavAudioPlayer g_wavAudio;
int g_wavOffsetMilliseconds = 0;
int g_tailMilliseconds = kDefaultTailMilliseconds;
bool g_playing = false;
bool g_effectTailRunning = false;
std::chrono::steady_clock::time_point g_effectTailStart;
bool g_positionDragging = false;
bool g_resumeAfterSeek = false;
bool g_volumeDragging = false;
std::wstring g_fileName;
std::chrono::steady_clock::time_point g_playbackEpoch;
double g_playbackBaseTime = 0.0;
COLORREF g_customColors[16] = {};
std::wstring g_midiPath;
std::wstring g_iniPath;
bool g_settingsLoaded = false;
bool g_settingsDirty = false;
bool g_audioEnabledPreference = true;
int g_controlDialogHeightDlu = 420;
// Restored once at startup; positions are saved in screen coordinates.
int g_visualizerStartupX = CW_USEDEFAULT;
int g_visualizerStartupY = CW_USEDEFAULT;
int g_controlStartupX = 80;
int g_controlStartupY = 80;

void UpdateControlUi();
void CommitTailMillisecondsFromControl(HWND hwnd);
void MarkSettingsDirty();
std::wstring BaseName(const std::wstring& path);
POINT ClampWindowPositionToWorkArea(int x, int y, int width, int height);

enum class ControlPage { Background, Effects, Audio };

void ResizeControlDialogForPage(HWND hwnd, ControlPage page) {
    // Keep the standard tab control around its entire page area, not just its
    // header strip. The page controls are dialog children, which is the normal
    // Win32 tab-control layout, but remain visually inside this frame.
    constexpr int kTabTopDlu = 139;
    constexpr int kDialogBottomMarginDlu = 8;
    int targetHeightDlu = 420;
    switch (page) {
    case ControlPage::Background: targetHeightDlu = 340; break;
    case ControlPage::Effects:    targetHeightDlu = 450; break;
    case ControlPage::Audio:      targetHeightDlu = 340; break;
    }

    const int deltaDlu = targetHeightDlu - g_controlDialogHeightDlu;
    if (deltaDlu != 0) {
        RECT delta{0, 0, 0, deltaDlu};
        if (MapDialogRect(hwnd, &delta)) {
            RECT windowRect{};
            if (GetWindowRect(hwnd, &windowRect)) {
                const LONG currentHeight = windowRect.bottom - windowRect.top;
                const LONG newHeight = std::max<LONG>(1, currentHeight + delta.bottom);
                SetWindowPos(hwnd, nullptr, 0, 0,
                             static_cast<int>(windowRect.right - windowRect.left),
                             static_cast<int>(newHeight),
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        g_controlDialogHeightDlu = targetHeightDlu;
    }

    const HWND tab = GetDlgItem(hwnd, IDC_SETTINGS_TAB);
    if (tab) {
        RECT tabDlu{0, 0, 422, targetHeightDlu - kTabTopDlu - kDialogBottomMarginDlu};
        if (MapDialogRect(hwnd, &tabDlu)) {
            SetWindowPos(tab, nullptr, 0, 0, tabDlu.right, tabDlu.bottom,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    // Intentionally do not recenter: switching pages preserves the window's
    // top-left position chosen by the user.
}

void ShowControlPage(HWND hwnd, ControlPage page) {
    const int backgroundControls[] = {
        IDC_BTN_BACKGROUND_IMAGE, IDC_BTN_BACKGROUND_VIDEO, IDC_BTN_BACKGROUND_RESET,
        IDC_LABEL_BACKGROUND, IDC_LABEL_BACKGROUND_VIDEO,
        IDC_LABEL_OPACITY, IDC_BG_OPACITY_SLIDER, IDC_LABEL_BG_OPACITY,
        IDC_LABEL_VIDEO_OPACITY_SECTION, IDC_VIDEO_OPACITY_SLIDER, IDC_LABEL_VIDEO_OPACITY,
        IDC_LABEL_VIDEO_OFFSET_SECTION, IDC_VIDEO_OFFSET_SLIDER, IDC_LABEL_VIDEO_OFFSET
    };
    const int effectsControls[] = {
        IDC_EFFECT_SMOKE, IDC_EFFECT_WATER_DROPLETS, IDC_EFFECT_NOTE_GLOW, IDC_EFFECT_SMOKE_WINDOW,
        IDC_EFFECT_AMBIENT_PARTICLES, IDC_EFFECT_RIPPLE, IDC_EFFECT_DIAMOND_RIPPLE,
        IDC_EFFECT_CROSS_SPARK, IDC_EFFECT_WAVE_LINE, IDC_BTN_WAVE_COLOR,
        IDC_EFFECT_IMPACT_POLYGONS, IDC_EFFECT_KEYBED_GLOW, IDC_NOTE_GUIDES, IDC_LABEL_PARTICLE_COUNT,
        IDC_AMBIENT_PARTICLES_SLIDER, IDC_LABEL_AMBIENT_PARTICLES,
        IDC_LABEL_NOTE_GLOW_STRENGTH, IDC_NOTE_GLOW_INTENSITY_SLIDER,
        IDC_LABEL_NOTE_GLOW_INTENSITY, IDC_WAVE_LINE_GLOW,
        IDC_LABEL_WAVE_LINE_AMPLITUDE, IDC_WAVE_LINE_AMPLITUDE_SLIDER,
        IDC_VALUE_WAVE_LINE_AMPLITUDE,
        IDC_LABEL_TRACK_SECTION,
        IDC_TRACK_COMBO, IDC_BTN_TRACK_COLOR, IDC_BTN_TRACK_RESET, IDC_LABEL_TRACK_COLOR
    };
    const int audioControls[] = {
        IDC_ENABLE_AUDIO, IDC_LABEL_VOLUME_SECTION, IDC_VOLUME_SLIDER, IDC_LABEL_VOLUME,
        IDC_LABEL_AUDIO_STATUS, IDC_BTN_WAV_OPEN, IDC_BTN_WAV_UNLOAD,
        IDC_LABEL_WAV_FILE, IDC_LABEL_WAV_OFFSET_SECTION, IDC_WAV_OFFSET_SLIDER,
        IDC_LABEL_WAV_OFFSET
    };

    auto setGroupVisibility = [hwnd, page](const int* ids, size_t count, ControlPage group) {
        const int show = page == group ? SW_SHOW : SW_HIDE;
        for (size_t i = 0; i < count; ++i) {
            const HWND control = GetDlgItem(hwnd, ids[i]);
            if (control) ShowWindow(control, show);
        }
    };
    setGroupVisibility(backgroundControls, std::size(backgroundControls), ControlPage::Background);
    setGroupVisibility(effectsControls, std::size(effectsControls), ControlPage::Effects);
    setGroupVisibility(audioControls, std::size(audioControls), ControlPage::Audio);

    const HWND tab = GetDlgItem(hwnd, IDC_SETTINGS_TAB);
    if (tab) {
        int index = 0;
        switch (page) {
        case ControlPage::Background: index = 0; break;
        case ControlPage::Effects:    index = 1; break;
        case ControlPage::Audio:      index = 2; break;
        }
        TabCtrl_SetCurSel(tab, index);
    }
    ResizeControlDialogForPage(hwnd, page);
}

std::wstring GetIniFilePath() {
    // Keep the INI beside the executable so portable copies of the app can
    // carry their settings with them.
    std::wstring modulePath(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(),
                                            static_cast<DWORD>(modulePath.size()));
    if (length == 0 || length >= modulePath.size()) {
        return L"PianoVisualizerClassic.ini";
    }
    modulePath.resize(length);
    const size_t separator = modulePath.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return L"PianoVisualizerClassic.ini";
    modulePath.resize(separator + 1);
    modulePath += L"PianoVisualizerClassic.ini";
    return modulePath;
}

int ReadIniInt(const wchar_t* section, const wchar_t* key, int defaultValue) {
    return GetPrivateProfileIntW(section, key, defaultValue, g_iniPath.c_str());
}

std::wstring ReadIniString(const wchar_t* section, const wchar_t* key) {
    std::wstring value(32768, L'\0');
    const DWORD length = GetPrivateProfileStringW(section, key, L"", value.data(),
                                                  static_cast<DWORD>(value.size()),
                                                  g_iniPath.c_str());
    value.resize(length);
    return value;
}

bool WriteIniString(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    return WritePrivateProfileStringW(section, key, value.c_str(), g_iniPath.c_str()) != FALSE;
}

bool WriteIniInt(const wchar_t* section, const wchar_t* key, int value) {
    return WriteIniString(section, key, std::to_wstring(value));
}

POINT ClampWindowPositionToWorkArea(int x, int y, int width, int height) {
    // RECT coordinates and POINT members are LONG on Windows. Keep the work-area
    // calculations in LONG so std::min/max/clamp receive a single consistent type.
    POINT point{static_cast<LONG>(x), static_cast<LONG>(y)};
    const HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return point;

    const LONG workWidth = std::max<LONG>(1L, info.rcWork.right - info.rcWork.left);
    const LONG workHeight = std::max<LONG>(1L, info.rcWork.bottom - info.rcWork.top);
    const LONG visibleWidth = std::min<LONG>(
        std::max<LONG>(1L, static_cast<LONG>(width)), workWidth);
    const LONG visibleHeight = std::min<LONG>(
        std::max<LONG>(1L, static_cast<LONG>(height)), workHeight);
    const LONG maxX = info.rcWork.right - visibleWidth;
    const LONG maxY = info.rcWork.bottom - visibleHeight;
    point.x = std::clamp<LONG>(static_cast<LONG>(x), info.rcWork.left,
                                std::max<LONG>(info.rcWork.left, maxX));
    point.y = std::clamp<LONG>(static_cast<LONG>(y), info.rcWork.top,
                                std::max<LONG>(info.rcWork.top, maxY));
    return point;
}

int ColorChannelToByte(float channel) {
    return std::clamp(static_cast<int>(channel * 255.0f + 0.5f), 0, 255);
}

std::wstring TrackColorSectionForPath(const std::wstring& path) {
    // A separate section per MIDI file keeps custom colors from leaking into
    // unrelated songs. Store the full path in that section as a collision check.
    std::wstring normalized = path;
    if (!normalized.empty()) {
        CharUpperBuffW(normalized.data(), static_cast<DWORD>(normalized.size()));
    }
    std::uint64_t hash = 14695981039346656037ull;
    for (wchar_t ch : normalized) {
        hash ^= static_cast<std::uint16_t>(ch);
        hash *= 1099511628211ull;
    }
    wchar_t section[48]{};
    swprintf_s(section, L"TrackColors_%016llX", static_cast<unsigned long long>(hash));
    return section;
}

size_t GetCurrentMidiTrackCount() {
    size_t count = g_song.tracks.size();
    for (const auto& note : g_song.notes) {
        if (note.track >= 0) count = std::max(count, static_cast<size_t>(note.track + 1));
    }
    // Avoid pathological INI files if a malformed MIDI contains an extreme
    // track index. Normal files are far below this limit.
    return std::min<size_t>(count, 1024);
}

void LoadTrackColorsForCurrentMidi() {
    if (!g_settingsLoaded || g_midiPath.empty() || g_song.duration <= 0.0) return;
    const std::wstring section = TrackColorSectionForPath(g_midiPath);
    std::wstring savedPath = ReadIniString(section.c_str(), L"MidiPath");
    std::wstring currentPath = g_midiPath;
    if (!savedPath.empty()) CharUpperBuffW(savedPath.data(), static_cast<DWORD>(savedPath.size()));
    if (!currentPath.empty()) CharUpperBuffW(currentPath.data(), static_cast<DWORD>(currentPath.size()));
    if (savedPath != currentPath) return;

    const size_t savedCount = static_cast<size_t>(std::clamp(
        ReadIniInt(section.c_str(), L"TrackCount", 0), 0, 1024));
    const size_t count = std::min(savedCount, GetCurrentMidiTrackCount());
    for (size_t track = 0; track < count; ++track) {
        const std::wstring enabledKey = L"Custom" + std::to_wstring(track);
        if (ReadIniInt(section.c_str(), enabledKey.c_str(), 0) == 0) continue;
        const std::wstring colorKey = L"Color" + std::to_wstring(track);
        const COLORREF color = static_cast<COLORREF>(std::clamp(
            ReadIniInt(section.c_str(), colorKey.c_str(), 0), 0, 0x00FFFFFF));
        g_visualizer.SetTrackColor(static_cast<int>(track), D2D1::ColorF(
            GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
            GetBValue(color) / 255.0f, 1.0f));
    }
}

void LoadSettings() {
    g_iniPath = GetIniFilePath();

    g_tailMilliseconds = std::clamp(
        ReadIniInt(L"Playback", L"TailMilliseconds", kDefaultTailMilliseconds),
        0, kMaximumTailMilliseconds);
    g_visualizer.SetTailDurationSeconds(g_tailMilliseconds / 1000.0);

    const int defaultFallSpeed = static_cast<int>(g_visualizer.FallSpeed() + 0.5f);
    g_visualizer.SetFallSpeed(static_cast<float>(std::clamp(
        ReadIniInt(L"Display", L"FallSpeed", defaultFallSpeed), 150, 700)));

    const int defaultOpacity = static_cast<int>(Visualizer::kDefaultBackgroundOpacity * 100.0f + 0.5f);
    const int legacyOpacity = std::clamp(ReadIniInt(L"Background", L"Opacity", defaultOpacity), 0, 100);
    int imageOpacity = ReadIniInt(L"Background", L"ImageOpacity", -1);
    int videoOpacity = ReadIniInt(L"Background", L"VideoOpacity", -1);
    const int videoOffset = std::clamp(
        ReadIniInt(L"Background", L"VideoOffsetMilliseconds", 0),
        Visualizer::kMinVideoOffsetMilliseconds, Visualizer::kMaxVideoOffsetMilliseconds);

    constexpr Visualizer::EffectMask kAllEffects =
        Visualizer::EffectSmokeGlow | Visualizer::EffectWaterDroplets | Visualizer::EffectRipple |
        Visualizer::EffectDiamondRipple | Visualizer::EffectNoteGlow |
        Visualizer::EffectCrossSpark | Visualizer::EffectWaveLine |
        Visualizer::EffectSmokeWindow | Visualizer::EffectAmbientParticles |
        Visualizer::EffectImpactPolygons | Visualizer::EffectKeybedGlow;
    const int defaultMask = static_cast<int>(g_visualizer.GetEffectMask());
    auto mask = static_cast<Visualizer::EffectMask>(
        ReadIniInt(L"Effects", L"Mask", defaultMask)) & kAllEffects;
    // Existing INI files predate Keybed Glow. Enable it by default on upgrade,
    // while persisting the explicit checkbox state independently thereafter.
    const bool keybedGlowEnabled = ReadIniInt(L"Effects", L"KeybedGlowEnabled", 1) != 0;
    if (keybedGlowEnabled) mask |= Visualizer::EffectKeybedGlow;
    else mask &= ~static_cast<Visualizer::EffectMask>(Visualizer::EffectKeybedGlow);
    g_visualizer.SetEffectMask(mask);
    g_visualizer.SetNoteGuideLinesEnabled(
        ReadIniInt(L"Effects", L"NoteGuides", g_visualizer.NoteGuideLinesEnabled() ? 1 : 0) != 0);
    g_visualizer.SetAmbientParticleCount(std::clamp(
        ReadIniInt(L"Effects", L"AmbientParticleCount", g_visualizer.AmbientParticleCount()), 0, 200));
    g_visualizer.SetNoteGlowIntensity(std::clamp(
        ReadIniInt(L"Effects", L"NoteGlowIntensity", 100), 0, 100) / 100.0f);
    g_visualizer.SetWaveLineGlowEnabled(
        ReadIniInt(L"Effects", L"WaveLineGlowEnabled", 1) != 0);
    g_visualizer.SetWaveLineAmplitude(
        std::clamp(ReadIniInt(L"Effects", L"WaveLineAmplitude", 108), 0, 200) / 50.0f);

    const auto defaultWaveColor = g_visualizer.GetWaveLineColor();
    const int red = std::clamp(ReadIniInt(L"Effects", L"WaveLineRed",
                                          ColorChannelToByte(defaultWaveColor.r)), 0, 255);
    const int green = std::clamp(ReadIniInt(L"Effects", L"WaveLineGreen",
                                            ColorChannelToByte(defaultWaveColor.g)), 0, 255);
    const int blue = std::clamp(ReadIniInt(L"Effects", L"WaveLineBlue",
                                           ColorChannelToByte(defaultWaveColor.b)), 0, 255);
    g_visualizer.SetWaveLineColor(D2D1::ColorF(red / 255.0f, green / 255.0f, blue / 255.0f, 1.0f));

    const int volume = std::clamp(ReadIniInt(L"Audio", L"Volume",
        static_cast<int>(g_simpleSynth.MasterVolume() * 100.0f + 0.5f)), 0, 100);
    g_simpleSynth.SetMasterVolume(volume / 100.0f);
    g_wavAudio.SetMasterVolume(volume / 100.0f);
    g_audioEnabledPreference = ReadIniInt(L"Audio", L"Enabled", 1) != 0;
    g_wavOffsetMilliseconds = std::clamp(
        ReadIniInt(L"Audio", L"WavOffsetMilliseconds", 0),
        WavAudioPlayer::kMinOffsetMilliseconds, WavAudioPlayer::kMaxOffsetMilliseconds);
    const std::wstring savedWavPath = ReadIniString(L"Audio", L"WavPath");
    bool audioPreferenceOverriddenByWav = false;
    if (!savedWavPath.empty()) {
        const DWORD wavAttributes = GetFileAttributesW(savedWavPath.c_str());
        if (wavAttributes != INVALID_FILE_ATTRIBUTES && !(wavAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            std::wstring ignoredWavError;
            if (g_wavAudio.Load(savedWavPath, ignoredWavError)) {
                // A WAV restored from the INI becomes the selected source on
                // startup. The checkbox remains enabled so the user can switch
                // explicitly back to the internal synth.
                audioPreferenceOverriddenByWav = g_audioEnabledPreference;
                g_audioEnabledPreference = false;
            }
        }
    }

    for (size_t i = 0; i < std::size(g_customColors); ++i) {
        const std::wstring key = L"CustomColor" + std::to_wstring(i);
        g_customColors[i] = static_cast<COLORREF>(std::clamp(
            ReadIniInt(L"ColorPicker", key.c_str(), static_cast<int>(g_customColors[i])),
            0, 0x00FFFFFF));
    }

    // Restore image and video as independent layers. Migrate legacy INI keys
    // (Path / IsVideo / Opacity) when the newer layer-specific keys are absent.
    std::wstring imagePath = ReadIniString(L"Background", L"ImagePath");
    std::wstring videoPath = ReadIniString(L"Background", L"VideoPath");
    const std::wstring legacyBackgroundPath = ReadIniString(L"Background", L"Path");
    const bool legacyBackgroundIsVideo = ReadIniInt(L"Background", L"IsVideo", 0) != 0;
    if (imagePath.empty() && videoPath.empty() && !legacyBackgroundPath.empty()) {
        if (legacyBackgroundIsVideo) videoPath = legacyBackgroundPath;
        else imagePath = legacyBackgroundPath;
    }
    if (imageOpacity < 0) imageOpacity = (!legacyBackgroundIsVideo && !legacyBackgroundPath.empty())
        ? legacyOpacity : defaultOpacity;
    if (videoOpacity < 0) videoOpacity = (legacyBackgroundIsVideo && !legacyBackgroundPath.empty())
        ? legacyOpacity : defaultOpacity;
    imageOpacity = std::clamp(imageOpacity, 0, 100);
    videoOpacity = std::clamp(videoOpacity, 0, 100);

    auto isExistingFile = [](const std::wstring& path) {
        if (path.empty()) return false;
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    };
    if (isExistingFile(imagePath)) {
        if (g_visualizer.LoadBackgroundImage(imagePath)) {
            g_visualizer.SetBackgroundImageOpacity(imageOpacity / 100.0f);
        }
    }
    if (isExistingFile(videoPath)) {
        std::wstring ignoredError;
        (void)g_visualizer.LoadBackgroundVideo(videoPath, ignoredError);
    }
    // Loading a background file sets its normal per-file defaults, so reapply
    // the saved values after both loads. Also restore them when no file is
    // currently loaded (for example, when no background file has been selected).
    g_visualizer.SetBackgroundImageOpacity(imageOpacity / 100.0f);
    g_visualizer.SetBackgroundVideoOpacity(videoOpacity / 100.0f);
    g_visualizer.SetVideoOffsetMilliseconds(videoOffset);

    g_settingsLoaded = true;
    g_settingsDirty = audioPreferenceOverriddenByWav;

    // Restore the last MIDI after the INI state is active so its per-song track
    // colors can be restored too. A missing/invalid MIDI never blocks startup.
    const std::wstring savedMidiPath = ReadIniString(L"Midi", L"Path");
    if (isExistingFile(savedMidiPath)) {
        MidiSong restoredSong;
        std::string midiError;
        if (LoadMidiFile(savedMidiPath, restoredSong, midiError)) {
            g_song = std::move(restoredSong);
            g_midiPath = savedMidiPath;
            g_fileName = BaseName(savedMidiPath);
            g_visualizer.SetSong(&g_song);
            LoadTrackColorsForCurrentMidi();
            g_visualizer.SetTime(0.0);
            g_playbackBaseTime = 0.0;
            g_playbackEpoch = std::chrono::steady_clock::now();
            if (g_visualizerHwnd) {
                SetWindowTextW(g_visualizerHwnd,
                    (L"Piano Visualizer Classic - " + g_fileName).c_str());
            }
        }
    }
}

void SaveSettings(bool force = false) {
    if (!g_settingsLoaded || g_iniPath.empty() || (!force && !g_settingsDirty)) return;

    bool success = true;
    auto writeInt = [&](const wchar_t* section, const wchar_t* key, int value) {
        if (!WriteIniInt(section, key, value)) success = false;
    };
    auto writeString = [&](const wchar_t* section, const wchar_t* key, const std::wstring& value) {
        if (!WriteIniString(section, key, value)) success = false;
    };
    auto writeNormalWindowPosition = [&](HWND window, const wchar_t* xKey, const wchar_t* yKey) {
        // Keep the last normal position while a window is maximized or minimized.
        if (!window || !IsWindow(window) || IsZoomed(window) || IsIconic(window)) return;
        RECT rect{};
        if (GetWindowRect(window, &rect)) {
            writeInt(L"WindowPosition", xKey, rect.left);
            writeInt(L"WindowPosition", yKey, rect.top);
        }
    };
    writeNormalWindowPosition(g_visualizerHwnd, L"VisualizerX", L"VisualizerY");
    writeNormalWindowPosition(g_controlHwnd, L"ControlX", L"ControlY");

    writeInt(L"Display", L"FallSpeed", static_cast<int>(g_visualizer.FallSpeed() + 0.5f));
    writeInt(L"Playback", L"TailMilliseconds", g_tailMilliseconds);
    writeInt(L"Background", L"ImageOpacity",
             static_cast<int>(g_visualizer.BackgroundImageOpacity() * 100.0f + 0.5f));
    writeInt(L"Background", L"VideoOpacity",
             static_cast<int>(g_visualizer.BackgroundVideoOpacity() * 100.0f + 0.5f));
    writeString(L"Background", L"ImagePath", g_visualizer.BackgroundImagePath());
    writeString(L"Background", L"VideoPath", g_visualizer.BackgroundVideoPath());
    // Retain legacy keys for graceful downgrade compatibility.
    const bool legacyVideo = !g_visualizer.HasBackgroundImage() && g_visualizer.HasBackgroundVideo();
    writeString(L"Background", L"Path", legacyVideo
        ? g_visualizer.BackgroundVideoPath() : g_visualizer.BackgroundImagePath());
    writeInt(L"Background", L"IsVideo", legacyVideo ? 1 : 0);
    writeInt(L"Background", L"Opacity", static_cast<int>(
        (legacyVideo ? g_visualizer.BackgroundVideoOpacity() : g_visualizer.BackgroundImageOpacity())
        * 100.0f + 0.5f));
    writeInt(L"Background", L"VideoOffsetMilliseconds", g_visualizer.VideoOffsetMilliseconds());
    writeString(L"Midi", L"Path", g_midiPath);

    writeInt(L"Effects", L"Mask", static_cast<int>(g_visualizer.GetEffectMask()));
    writeInt(L"Effects", L"KeybedGlowEnabled",
             g_visualizer.IsEffectEnabled(Visualizer::EffectKeybedGlow) ? 1 : 0);
    writeInt(L"Effects", L"NoteGuides", g_visualizer.NoteGuideLinesEnabled() ? 1 : 0);
    writeInt(L"Effects", L"AmbientParticleCount", g_visualizer.AmbientParticleCount());
    writeInt(L"Effects", L"NoteGlowIntensity",
             static_cast<int>(g_visualizer.NoteGlowIntensity() * 100.0f + 0.5f));
    writeInt(L"Effects", L"WaveLineGlowEnabled", g_visualizer.WaveLineGlowEnabled() ? 1 : 0);
    writeInt(L"Effects", L"WaveLineAmplitude",
             static_cast<int>(g_visualizer.WaveLineAmplitude() * 50.0f + 0.5f));
    // Migrate older INI files by removing the retired intensity setting.
    (void)WritePrivateProfileStringW(L"Effects", L"WaveLineGlowIntensity", nullptr, g_iniPath.c_str());
    const auto waveColor = g_visualizer.GetWaveLineColor();
    writeInt(L"Effects", L"WaveLineRed", ColorChannelToByte(waveColor.r));
    writeInt(L"Effects", L"WaveLineGreen", ColorChannelToByte(waveColor.g));
    writeInt(L"Effects", L"WaveLineBlue", ColorChannelToByte(waveColor.b));

    bool audioEnabled = g_audioEnabledPreference;
    if (!g_wavAudio.IsLoaded() && g_controlHwnd && g_simpleSynth.IsAvailable()) {
        const HWND audioCheck = GetDlgItem(g_controlHwnd, IDC_ENABLE_AUDIO);
        if (audioCheck) audioEnabled = SendMessageW(audioCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    g_audioEnabledPreference = audioEnabled;
    writeInt(L"Audio", L"Enabled", audioEnabled ? 1 : 0);
    writeInt(L"Audio", L"Volume",
             static_cast<int>(g_simpleSynth.MasterVolume() * 100.0f + 0.5f));
    writeString(L"Audio", L"WavPath", g_wavAudio.IsLoaded() ? g_wavAudio.Path() : L"");
    writeInt(L"Audio", L"WavOffsetMilliseconds", g_wavOffsetMilliseconds);

    for (size_t i = 0; i < std::size(g_customColors); ++i) {
        const std::wstring key = L"CustomColor" + std::to_wstring(i);
        writeInt(L"ColorPicker", key.c_str(), static_cast<int>(g_customColors[i] & 0x00FFFFFF));
    }

    if (!g_midiPath.empty() && g_song.duration > 0.0) {
        const std::wstring section = TrackColorSectionForPath(g_midiPath);
        writeString(section.c_str(), L"MidiPath", g_midiPath);
        const size_t trackCount = GetCurrentMidiTrackCount();
        writeInt(section.c_str(), L"TrackCount", static_cast<int>(trackCount));
        for (size_t track = 0; track < trackCount; ++track) {
            const std::wstring enabledKey = L"Custom" + std::to_wstring(track);
            const bool custom = g_visualizer.HasCustomTrackColor(static_cast<int>(track));
            writeInt(section.c_str(), enabledKey.c_str(), custom ? 1 : 0);
            if (custom) {
                const auto color = g_visualizer.GetTrackColor(static_cast<int>(track));
                const COLORREF rgb = RGB(ColorChannelToByte(color.r),
                                         ColorChannelToByte(color.g),
                                         ColorChannelToByte(color.b));
                const std::wstring colorKey = L"Color" + std::to_wstring(track);
                writeInt(section.c_str(), colorKey.c_str(), static_cast<int>(rgb & 0x00FFFFFF));
            }
        }
    }

    // Flush profile API's cached writes before clearing the dirty flag.
    if (!WritePrivateProfileStringW(nullptr, nullptr, nullptr, g_iniPath.c_str())) success = false;
    g_settingsDirty = !success;
    if (success && g_controlHwnd) KillTimer(g_controlHwnd, TIMER_SETTINGS_SAVE);
}

void MarkSettingsDirty() {
    if (!g_settingsLoaded) return;
    g_settingsDirty = true;
    if (g_controlHwnd) SetTimer(g_controlHwnd, TIMER_SETTINGS_SAVE, 500, nullptr);
}

std::wstring BaseName(const std::wstring& path) {
    const size_t i = path.find_last_of(L"\\/");
    return (i == std::wstring::npos) ? path : path.substr(i + 1);
}

std::wstring FormatTime(double seconds) {
    seconds = std::max(0.0, seconds);
    const int total = static_cast<int>(seconds);
    const int minutes = total / 60;
    const int secs = total % 60;
    wchar_t buf[64]{};
    swprintf_s(buf, L"%d:%02d", minutes, secs);
    return buf;
}

std::wstring FormatVideoOffset(int milliseconds) {
    std::wstring result = milliseconds > 0 ? L"+" : L"";
    result += std::to_wstring(milliseconds);
    result += L" ms";
    return result;
}

void SetControlText(int id, const std::wstring& text) {
    if (!g_controlHwnd) return;
    HWND control = GetDlgItem(g_controlHwnd, id);
    if (!control) return;
    wchar_t current[512]{};
    GetWindowTextW(control, current, static_cast<int>(std::size(current)));
    if (text == current) return;
    SetWindowTextW(control, text.c_str());
}

void SetRenderDialogText(HWND hwnd, int id, const std::wstring& text) {
    HWND control = GetDlgItem(hwnd, id);
    if (control) SetWindowTextW(control, text.c_str());
}

bool IsAudioEnabled() {
    if (!g_controlHwnd) return g_audioEnabledPreference;
    HWND check = GetDlgItem(g_controlHwnd, IDC_ENABLE_AUDIO);
    return check && SendMessageW(check, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void StartAudioPlayback() {
    if (g_song.duration <= 0.0) return;

    // The checkbox is the source selector whenever a WAV is loaded:
    // checked = internal synth, unchecked = external WAV (if available).
    if (g_audioEnabledPreference) {
        g_wavAudio.Stop();
        if (!g_simpleSynth.IsAvailable()) return;
        g_simpleSynth.Start(&g_song, g_visualizer.Time(), g_playbackEpoch);
        return;
    }

    g_simpleSynth.Stop();
    if (g_wavAudio.IsLoaded()) {
        g_wavAudio.Start(g_visualizer.Time(), g_wavOffsetMilliseconds);
    }
}

void StopAudioPlayback() {
    g_simpleSynth.Stop();
    g_wavAudio.Stop();
}

double CurrentPlaybackTimelineTime() {
    if (g_playing && g_visualizer.Duration() > 0.0) {
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - g_playbackEpoch).count();
        return std::clamp(g_playbackBaseTime + std::max(0.0, elapsed),
                          0.0, g_visualizer.Duration());
    }
    return g_visualizer.Time();
}

void RestartAudioAtCurrentPosition() {
    if (!g_playing) return;
    const auto now = std::chrono::steady_clock::now();
    const double currentTime = CurrentPlaybackTimelineTime();
    StopAudioPlayback();
    g_visualizer.SetTime(currentTime);
    if (currentTime >= g_visualizer.Duration() - 1e-6) {
        g_playing = false;
        g_effectTailRunning = true;
        g_effectTailStart = now;
        g_visualizer.SetPlaying(false);
        UpdateControlUi();
        return;
    }
    g_playbackBaseTime = currentTime;
    g_playbackEpoch = now;
    StartAudioPlayback();
}

void RestartAudioForSourceChange() {
    if (g_playing) {
        RestartAudioAtCurrentPosition();
        return;
    }

    StopAudioPlayback();
    if (g_effectTailRunning && g_song.duration > 0.0) {
        // During the visual tail, keep the tail clock independent while
        // starting the newly selected source at the current timeline position.
        g_playbackBaseTime = g_visualizer.Time();
        g_playbackEpoch = std::chrono::steady_clock::now();
        StartAudioPlayback();
    }
}

void KeepWindowsTogetherInZOrder(HWND active) {
    if (!g_visualizerHwnd || !g_controlHwnd ||
        !IsWindow(g_visualizerHwnd) || !IsWindow(g_controlHwnd)) return;

    const HWND other = (active == g_visualizerHwnd) ? g_controlHwnd : g_visualizerHwnd;
    SetWindowPos(other, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowPos(active, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void UpdatePositionSlider() {
    if (!g_controlHwnd || g_positionDragging || g_visualizer.Duration() <= 0.0) return;
    const double duration = g_visualizer.Duration();
    const double time = std::clamp(g_visualizer.Time(), 0.0, duration);
    const int pos = static_cast<int>(time / duration * kPositionSliderMax + 0.5);
    SendDlgItemMessageW(g_controlHwnd, IDC_POSITION_SLIDER, TBM_SETPOS, TRUE, pos);
}

std::wstring NarrowToWide(const std::string& text) {
    if (text.empty()) return L"";
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                     text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length > 0) {
        std::wstring result(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            text.data(), static_cast<int>(text.size()), result.data(), length);
        return result;
    }
    length = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return L"";
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

int GetSelectedTrack() {
    if (!g_controlHwnd) return -1;
    const HWND combo = GetDlgItem(g_controlHwnd, IDC_TRACK_COMBO);
    if (!combo) return -1;
    const LRESULT sel = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) return -1;
    const LRESULT data = SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(sel), 0);
    return data == CB_ERR ? -1 : static_cast<int>(data);
}

std::wstring FormatTrackColorLabel(int track) {
    if (track < 0) return L"No track selected";
    const auto color = g_visualizer.GetTrackColor(track);
    wchar_t buf[80]{};
    swprintf_s(buf, L"#%02X%02X%02X%s",
               static_cast<int>(color.r * 255.0f + 0.5f),
               static_cast<int>(color.g * 255.0f + 0.5f),
               static_cast<int>(color.b * 255.0f + 0.5f),
               g_visualizer.HasCustomTrackColor(track) ? L" (custom)" : L" (default)");
    return buf;
}

void OpenWaveLineColor(HWND owner) {
    const auto color = g_visualizer.GetWaveLineColor();
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = owner;
    cc.rgbResult = RGB(static_cast<int>(color.r * 255.0f + 0.5f),
                       static_cast<int>(color.g * 255.0f + 0.5f),
                       static_cast<int>(color.b * 255.0f + 0.5f));
    cc.lpCustColors = g_customColors;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return;

    g_visualizer.SetWaveLineColor(D2D1::ColorF(
        GetRValue(cc.rgbResult) / 255.0f,
        GetGValue(cc.rgbResult) / 255.0f,
        GetBValue(cc.rgbResult) / 255.0f,
        1.0f));
    MarkSettingsDirty();
    InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
}

void UpdateTrackColorUi() {
    if (!g_controlHwnd) return;
    const int track = GetSelectedTrack();
    SetControlText(IDC_LABEL_TRACK_COLOR, FormatTrackColorLabel(track));
    const bool enabled = track >= 0 && g_song.duration > 0.0;
    EnableWindow(GetDlgItem(g_controlHwnd, IDC_BTN_TRACK_COLOR), enabled);
    EnableWindow(GetDlgItem(g_controlHwnd, IDC_BTN_TRACK_RESET), enabled);
}

void RebuildTrackCombo() {
    if (!g_controlHwnd) return;
    const HWND combo = GetDlgItem(g_controlHwnd, IDC_TRACK_COMBO);
    if (!combo) return;

    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int visibleTrackCount = 0;
    int maxTrackIndex = static_cast<int>(g_song.tracks.size());
    for (const auto& note : g_song.notes) {
        if (note.track >= 0) maxTrackIndex = std::max(maxTrackIndex, note.track + 1);
    }

    for (int track = 0; track < maxTrackIndex; ++track) {
        int noteCount = 0;
        if (track < static_cast<int>(g_song.tracks.size())) {
            noteCount = g_song.tracks[static_cast<size_t>(track)].noteCount;
        }
        if (noteCount <= 0) {
            for (const auto& note : g_song.notes) if (note.track == track) ++noteCount;
        }
        if (noteCount <= 0) continue;

        std::wstring label = L"Track " + std::to_wstring(visibleTrackCount + 1);
        if (track < static_cast<int>(g_song.tracks.size())) {
            const std::wstring name = NarrowToWide(g_song.tracks[static_cast<size_t>(track)].name);
            if (!name.empty()) label += L" - " + name;
        }
        const LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0,
                                            reinterpret_cast<LPARAM>(label.c_str()));
        if (index != CB_ERR && index != CB_ERRSPACE) {
            SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(track));
            ++visibleTrackCount;
        }
    }

    if (visibleTrackCount > 0) {
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        SendMessageW(combo, CB_SETDROPPEDWIDTH, 320, 0);
    }
    EnableWindow(combo, visibleTrackCount > 0);
    UpdateTrackColorUi();
}

void OpenTrackColor(HWND owner) {
    const int track = GetSelectedTrack();
    if (track < 0) return;

    const auto color = g_visualizer.GetTrackColor(track);
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = owner;
    cc.rgbResult = RGB(static_cast<int>(color.r * 255.0f + 0.5f),
                       static_cast<int>(color.g * 255.0f + 0.5f),
                       static_cast<int>(color.b * 255.0f + 0.5f));
    cc.lpCustColors = g_customColors;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return;

    g_visualizer.SetTrackColor(track, D2D1::ColorF(
        GetRValue(cc.rgbResult) / 255.0f,
        GetGValue(cc.rgbResult) / 255.0f,
        GetBValue(cc.rgbResult) / 255.0f,
        1.0f));
    // Save both the color picker's reusable palette and this MIDI file's
    // custom track color when the debounced settings save runs.
    MarkSettingsDirty();
    UpdateTrackColorUi();
    InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
}

void UpdateControlUi() {
    if (!g_controlHwnd) return;

    SetControlText(IDC_LABEL_FILE, g_song.duration > 0.0 ? g_fileName : L"(No MIDI loaded)");
    // Keep the status as Playing throughout the configured tail. The player
    // becomes Stopped only after the tail renderer finishes and clears this flag.
    SetControlText(IDC_LABEL_STATUS, g_song.duration > 0.0
        ? ((g_playing || g_effectTailRunning) ? L"Playing" : L"Stopped") : L"No MIDI");

    const double duration = g_visualizer.Duration();
    const double time = std::clamp(g_visualizer.Time(), 0.0, std::max(0.0, duration));
    SetControlText(IDC_LABEL_TIME, FormatTime(time) + L" / " + FormatTime(duration));
    SetControlText(IDC_LABEL_SPEED,
                   std::to_wstring(static_cast<int>(g_visualizer.FallSpeed())) + L" px/s");
    SetControlText(IDC_LABEL_BACKGROUND, g_visualizer.HasBackgroundImage()
        ? BaseName(g_visualizer.BackgroundImagePath()) : L"(No image)");
    SetControlText(IDC_LABEL_BACKGROUND_VIDEO, g_visualizer.HasBackgroundVideo()
        ? BaseName(g_visualizer.BackgroundVideoPath()) : L"(No video)");
    UpdateTrackColorUi();

    HWND audioCheck = GetDlgItem(g_controlHwnd, IDC_ENABLE_AUDIO);
    const bool wavLoaded = g_wavAudio.IsLoaded();
    const bool synthAvailable = g_simpleSynth.IsAvailable();
    if (audioCheck) {
        // Do not gray the selector merely because a WAV is loaded. If the
        // synthesizer is available, the user can switch sources at any time.
        EnableWindow(audioCheck, synthAvailable ? TRUE : FALSE);
        if (!synthAvailable) g_audioEnabledPreference = false;
        SendMessageW(audioCheck, BM_SETCHECK,
            g_audioEnabledPreference ? BST_CHECKED : BST_UNCHECKED, 0);
        if (!synthAvailable) {
            SetControlText(IDC_LABEL_AUDIO_STATUS,
                           wavLoaded ? L"WAV selected; internal synth unavailable"
                                     : L"Internal audio is unavailable.");
        } else if (wavLoaded && g_audioEnabledPreference) {
            SetControlText(IDC_LABEL_AUDIO_STATUS,
                           L"Internal synth selected; uncheck to use WAV");
        } else if (wavLoaded) {
            SetControlText(IDC_LABEL_AUDIO_STATUS,
                           L"WAV selected; check to use internal synth");
        } else {
            SetControlText(IDC_LABEL_AUDIO_STATUS,
                           L"Internal simple piano synth (XAudio2)");
        }
    }

    SetControlText(IDC_LABEL_WAV_FILE, wavLoaded ? BaseName(g_wavAudio.Path()) : L"(No WAV loaded)");
    EnableWindow(GetDlgItem(g_controlHwnd, IDC_BTN_WAV_UNLOAD), wavLoaded ? TRUE : FALSE);
    EnableWindow(GetDlgItem(g_controlHwnd, IDC_WAV_OFFSET_SLIDER), wavLoaded ? TRUE : FALSE);
    SendDlgItemMessageW(g_controlHwnd, IDC_WAV_OFFSET_SLIDER, TBM_SETPOS, TRUE,
                        g_wavOffsetMilliseconds);
    SetControlText(IDC_LABEL_WAV_OFFSET, FormatVideoOffset(g_wavOffsetMilliseconds));

    const int volume = static_cast<int>(g_simpleSynth.MasterVolume() * 100.0f + 0.5f);
    SetControlText(IDC_LABEL_VOLUME, std::to_wstring(volume) + L"%");
    if (!g_volumeDragging) {
        SendDlgItemMessageW(g_controlHwnd, IDC_VOLUME_SLIDER, TBM_SETPOS, TRUE, volume);
    }

    const bool hasBackgroundImage = g_visualizer.HasBackgroundImage();
    const int imageOpacity = static_cast<int>(g_visualizer.BackgroundImageOpacity() * 100.0f + 0.5f);
    // Keep displaying the stored values when no file is loaded. The
    // controls become inactive, but unloading a file must not reset the
    // user's opacity settings.
    SetControlText(IDC_LABEL_BG_OPACITY, std::to_wstring(imageOpacity) + L"%");
    EnableWindow(GetDlgItem(g_controlHwnd, IDC_BG_OPACITY_SLIDER), hasBackgroundImage ? TRUE : FALSE);
    SendDlgItemMessageW(g_controlHwnd, IDC_BG_OPACITY_SLIDER, TBM_SETPOS, TRUE, imageOpacity);

    const bool hasBackgroundVideo = g_visualizer.HasBackgroundVideo();
    const int videoOpacity = static_cast<int>(g_visualizer.BackgroundVideoOpacity() * 100.0f + 0.5f);
    SetControlText(IDC_LABEL_VIDEO_OPACITY, std::to_wstring(videoOpacity) + L"%");
    EnableWindow(GetDlgItem(g_controlHwnd, IDC_VIDEO_OPACITY_SLIDER), hasBackgroundVideo ? TRUE : FALSE);
    SendDlgItemMessageW(g_controlHwnd, IDC_VIDEO_OPACITY_SLIDER, TBM_SETPOS, TRUE, videoOpacity);
    EnableWindow(GetDlgItem(g_controlHwnd, IDC_BTN_BACKGROUND_RESET),
                  g_visualizer.HasBackground() ? TRUE : FALSE);

    EnableWindow(GetDlgItem(g_controlHwnd, IDC_VIDEO_OFFSET_SLIDER),
                 hasBackgroundVideo ? TRUE : FALSE);
    SendDlgItemMessageW(g_controlHwnd, IDC_VIDEO_OFFSET_SLIDER, TBM_SETPOS, TRUE,
                        g_visualizer.VideoOffsetMilliseconds());
    SetControlText(IDC_LABEL_VIDEO_OFFSET,
                   FormatVideoOffset(g_visualizer.VideoOffsetMilliseconds()));

    const bool hasSong = duration > 0.0;
    auto setEnabledIfChanged = [&](int id, bool enabled) {
        HWND control = GetDlgItem(g_controlHwnd, id);
        if (control && IsWindowEnabled(control) != enabled) EnableWindow(control, enabled);
    };
    setEnabledIfChanged(IDC_BTN_PLAY, hasSong);
    setEnabledIfChanged(IDC_BTN_RESTART, hasSong);
    setEnabledIfChanged(IDC_BTN_RENDER, hasSong);
    setEnabledIfChanged(IDC_POSITION_SLIDER, hasSong);
    SetControlText(IDC_BTN_PLAY, g_playing ? L"Pause" : L"Play");
    if (hasSong) UpdatePositionSlider();
    else SendDlgItemMessageW(g_controlHwnd, IDC_POSITION_SLIDER, TBM_SETPOS, TRUE, 0);
}

void TogglePlay() {
    if (g_visualizer.Duration() <= 0.0) return;
    g_effectTailRunning = false;
    if (g_playing) {
        g_playing = false;
        g_visualizer.SetPlaying(false);
        StopAudioPlayback();
    } else {
        if (g_visualizer.Time() >= g_visualizer.Duration() - 1e-6) g_visualizer.SetTime(0.0);
        g_playing = true;
        g_visualizer.SetPlaying(true);
        g_playbackBaseTime = g_visualizer.Time();
        g_playbackEpoch = std::chrono::steady_clock::now();
        StartAudioPlayback();
    }
    UpdateControlUi();
    InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
}

void ResetPlayback() {
    StopAudioPlayback();
    g_visualizer.Reset();
    g_playing = false;
    g_effectTailRunning = false;
    g_visualizer.SetPlaying(false);
    g_playbackBaseTime = 0.0;
    g_playbackEpoch = std::chrono::steady_clock::now();
    UpdateControlUi();
    InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
}

bool CreateTempWavPath(std::wstring& path) {
    wchar_t tempDir[MAX_PATH]{};
    const DWORD len = GetTempPathW(MAX_PATH, tempDir);
    if (len == 0 || len >= MAX_PATH) return false;
    wchar_t tempFile[MAX_PATH]{};
    if (GetTempFileNameW(tempDir, L"PVW", 0, tempFile) == 0) return false;
    path = tempFile;
    return true;
}

void CommitBackgroundSelection(HWND owner, const wchar_t* path, bool video) {
    bool loaded = false;
    std::wstring error;
    if (video) {
        loaded = g_visualizer.LoadBackgroundVideo(path, error);
    } else {
        loaded = g_visualizer.LoadBackgroundImage(path);
    }
    if (!loaded) {
        const wchar_t* message = video
            ? L"Failed to load the background video. MP4/H.264 is recommended; supported formats depend on the codecs installed in Windows."
            : L"Failed to load the background image.";
        if (!error.empty()) {
            std::wstring detailed = std::wstring(message) + L"\n\n" + error;
            MessageBoxW(owner, detailed.c_str(), L"Piano Visualizer Classic", MB_ICONERROR);
        } else {
            MessageBoxW(owner, message, L"Piano Visualizer Classic", MB_ICONERROR);
        }
        return;
    }
    SetControlText(video ? IDC_LABEL_BACKGROUND_VIDEO : IDC_LABEL_BACKGROUND, BaseName(path));
    MarkSettingsDirty();
    UpdateControlUi();
    InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
}

void OpenBackgroundImage(HWND owner) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;
    CommitBackgroundSelection(owner, path, false);
}

void OpenBackgroundVideo(HWND owner) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Video Files\0*.mp4;*.m4v;*.mov;*.wmv;*.avi;*.mkv;*.mpg;*.mpeg\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;
    CommitBackgroundSelection(owner, path, true);
}

void ResetBackground(HWND owner) {
    (void)owner;
    g_visualizer.ResetBackgroundImage();
    SetControlText(IDC_LABEL_BACKGROUND, L"(No image)");
    SetControlText(IDC_LABEL_BACKGROUND_VIDEO, L"(No video)");
    MarkSettingsDirty();
    UpdateControlUi();
    InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
}

void OpenWavAudio(HWND owner) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"WAV Audio Files\0*.wav;*.wave\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;

    // The WAV sync offset is a user preference independent of the selected
    // file. Loading a different WAV intentionally leaves it unchanged.
    const int savedWavOffsetMilliseconds = g_wavOffsetMilliseconds;
    std::wstring error;
    if (!g_wavAudio.Load(path, error)) {
        MessageBoxW(owner, error.empty() ? L"Failed to load the WAV audio file." : error.c_str(),
                    L"WAV Audio", MB_ICONERROR);
        return;
    }
    g_wavOffsetMilliseconds = std::clamp(savedWavOffsetMilliseconds,
        WavAudioPlayer::kMinOffsetMilliseconds, WavAudioPlayer::kMaxOffsetMilliseconds);
    g_wavAudio.SetMasterVolume(g_simpleSynth.MasterVolume());
    // Loading a WAV selects it automatically. The still-enabled checkbox lets
    // the user explicitly switch to the internal synth whenever desired.
    g_audioEnabledPreference = false;
    if (g_controlHwnd) {
        SendDlgItemMessageW(g_controlHwnd, IDC_ENABLE_AUDIO, BM_SETCHECK, BST_UNCHECKED, 0);
    }
    MarkSettingsDirty();
    RestartAudioForSourceChange();
    UpdateControlUi();
}

void UnloadWavAudio() {
    if (!g_wavAudio.IsLoaded()) return;
    g_wavAudio.Unload();
    MarkSettingsDirty();
    RestartAudioForSourceChange();
    UpdateControlUi();
}

void OpenMidi(HWND owner) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"MIDI Files\0*.mid;*.midi\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;

    MidiSong newSong;
    std::string error;
    if (!LoadMidiFile(path, newSong, error)) {
        MessageBoxA(owner, error.c_str(), "MIDI Error", MB_ICONERROR);
        return;
    }

    // Save custom colors for the previous song before replacing its track map.
    SaveSettings();
    StopAudioPlayback();
    g_playing = false;
    g_visualizer.SetPlaying(false);
    g_song = std::move(newSong);
    g_midiPath = path;
    g_fileName = BaseName(path);
    g_effectTailRunning = false;
    g_visualizer.SetSong(&g_song);
    LoadTrackColorsForCurrentMidi();
    RebuildTrackCombo();
    g_visualizer.SetTime(0.0);
    g_playbackBaseTime = 0.0;
    MarkSettingsDirty();
    g_playbackEpoch = std::chrono::steady_clock::now();
    SetWindowTextW(g_visualizerHwnd, (L"Piano Visualizer Classic - " + g_fileName).c_str());
    UpdateControlUi();
    InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
}

constexpr UINT WM_RENDER_PROGRESS = WM_APP + 20;
constexpr UINT WM_RENDER_FINISHED = WM_APP + 21;
constexpr UINT_PTR TIMER_RENDER_CANCEL = 10;

struct RenderTask {
    VideoRenderOptions options;
    std::atomic<bool> cancelRequested{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> success{false};
    std::mutex mutex;
    int percent = 0;
    std::wstring status = L"Preparing...";
    std::wstring error;
    HWND dialog = nullptr;
    std::thread worker;
};

void SetRenderTaskStatus(RenderTask& task, int percent, const std::wstring& status) {
    {
        std::lock_guard<std::mutex> lock(task.mutex);
        task.percent = std::clamp(percent, 0, 100);
        task.status = status;
    }
    if (task.dialog) PostMessageW(task.dialog, WM_RENDER_PROGRESS, 0, 0);
}

void RenderWorkerMain(RenderTask* task) {
    // D2D/WIC are apartment/thread-affine here. The worker owns its own factories
    // and render context, so initialize COM explicitly on this thread.
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        std::lock_guard<std::mutex> lock(task->mutex);
        task->error = L"Failed to initialize COM on the video render thread.";
        task->success.store(false, std::memory_order_release);
        task->finished.store(true, std::memory_order_release);
        if (task->dialog) PostMessageW(task->dialog, WM_RENDER_FINISHED, 0, 0);
        return;
    }

    std::wstring error;
    bool success = false;
    std::wstring tempWav;

    if (task->options.includeAudio) {
        SetRenderTaskStatus(*task, 0, L"Rendering audio...");

        bool audioReady = false;
        if (!task->options.audioInputPath.empty()) {
            const DWORD attributes = GetFileAttributesW(task->options.audioInputPath.c_str());
            audioReady = attributes != INVALID_FILE_ATTRIBUTES
                && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
            if (!audioReady) error = L"The selected WAV file could not be found for rendering.";
        } else {
            audioReady = CreateTempWavPath(tempWav);
            if (audioReady) {
                task->options.temporaryWavPath = tempWav;
                audioReady = g_simpleSynth.RenderWav(
                    g_song, tempWav, task->options.audioVolume,
                    task->options.tailSeconds, error, &task->cancelRequested);
            } else {
                error = L"Failed to create a temporary WAV file.";
            }
        }
        if (!task->options.audioInputPath.empty() && audioReady) {
            task->options.temporaryWavPath = task->options.audioInputPath;
        }

        if (!audioReady) {
            if (!tempWav.empty()) DeleteFileW(tempWav.c_str());
            {
                std::lock_guard<std::mutex> lock(task->mutex);
                task->error = error;
            }
            task->success.store(false, std::memory_order_release);
            task->finished.store(true, std::memory_order_release);
            if (task->dialog) PostMessageW(task->dialog, WM_RENDER_FINISHED, 0, 0);
            if (SUCCEEDED(comHr)) CoUninitialize();
            return;
        }
    }

    if (!task->cancelRequested.load(std::memory_order_acquire)) {
        VideoRenderer renderer;
        success = renderer.RenderMp4(
            g_visualizer, g_song, &g_simpleSynth, task->options,
            [task](int percent, const std::wstring& status) {
                SetRenderTaskStatus(*task, percent, status);
            }, error, &task->cancelRequested);
    } else {
        error = L"Rendering canceled.";
    }

    if (!tempWav.empty()) DeleteFileW(tempWav.c_str());

    {
        std::lock_guard<std::mutex> lock(task->mutex);
        task->error = error;
    }
    task->success.store(success, std::memory_order_release);
    task->finished.store(true, std::memory_order_release);
    if (task->dialog) PostMessageW(task->dialog, WM_RENDER_FINISHED, 0, 0);

    if (SUCCEEDED(comHr)) CoUninitialize();
}

INT_PTR CALLBACK RenderCancelDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
INT_PTR CALLBACK RenderDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

void OpenRenderDialog(HWND owner) {
    if (g_song.duration <= 0.0) return;

    if (g_playing) {
        g_playing = false;
        g_visualizer.SetPlaying(false);
        StopAudioPlayback();
        UpdateControlUi();
    }

    DialogBoxParamW(g_instance, MAKEINTRESOURCEW(IDD_RENDER_DIALOG), owner, RenderDlgProc, 0);
}

LRESULT CALLBACK VisualizerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_visualizerHwnd = hwnd;
        if (!g_visualizer.Initialize(hwnd)) {
            MessageBoxW(hwnd, L"Failed to initialize Direct2D.", L"Piano Visualizer Classic", MB_ICONERROR);
            return -1;
        }
        RECT rc{};
        GetClientRect(hwnd, &rc);
        g_visualizer.Resize(static_cast<UINT>(std::max<LONG>(1, rc.right)),
                            static_cast<UINT>(std::max<LONG>(1, rc.bottom)));
        return 0;
    }
    case WM_MOVE:
        MarkSettingsDirty();
        return 0;
    case WM_SIZE:
        g_visualizer.Resize(std::max<UINT>(1, LOWORD(lp)), std::max<UINT>(1, HIWORD(lp)));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_ACTIVE || LOWORD(wp) == WA_CLICKACTIVE) KeepWindowsTogetherInZOrder(hwnd);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        g_visualizer.Render();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        // Save before the native window starts tearing down, so GetWindowRect
        // still reports the user's last normal position.
        SaveSettings(true);
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_DESTROY:
        if (g_controlHwnd && IsWindow(g_controlHwnd)) {
            CommitTailMillisecondsFromControl(g_controlHwnd);
        }
        SaveSettings(true);
        StopAudioPlayback();
        g_visualizer.Shutdown();
        if (g_controlHwnd) {
            HWND control = g_controlHwnd;
            g_controlHwnd = nullptr;
            DestroyWindow(control);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

INT_PTR CALLBACK RenderCancelDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* task = reinterpret_cast<RenderTask*>(GetWindowLongPtrW(hwnd, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG:
        task = reinterpret_cast<RenderTask*>(lp);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(task));
        task->dialog = hwnd;
        SendDlgItemMessageW(hwnd, IDC_RENDER_CANCEL_PROGRESS, PBM_SETRANGE32, 0, 100);
        SendDlgItemMessageW(hwnd, IDC_RENDER_CANCEL_PROGRESS, PBM_SETPOS, 0, 0);
        SetDlgItemTextW(hwnd, IDC_RENDER_CANCEL_STATUS, L"Preparing...");
        SetTimer(hwnd, TIMER_RENDER_CANCEL, 100, nullptr);
        task->worker = std::thread(RenderWorkerMain, task);
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_RENDER_ABORT) {
            if (task && !task->finished.load(std::memory_order_acquire)) {
                task->cancelRequested.store(true, std::memory_order_release);
                EnableWindow(GetDlgItem(hwnd, IDC_RENDER_ABORT), FALSE);
                SetDlgItemTextW(hwnd, IDC_RENDER_CANCEL_STATUS, L"Cancelling...");
            }
            return TRUE;
        }
        break;

    case WM_TIMER:
        if (wp == TIMER_RENDER_CANCEL && task) {
            if (task->finished.load(std::memory_order_acquire)) {
                EndDialog(hwnd, task->success.load(std::memory_order_acquire) ? IDOK : IDCANCEL);
                return TRUE;
            }

            int percent = 0;
            std::wstring status;
            {
                std::lock_guard<std::mutex> lock(task->mutex);
                percent = task->percent;
                status = task->status;
            }
            SendDlgItemMessageW(hwnd, IDC_RENDER_CANCEL_PROGRESS, PBM_SETPOS, percent, 0);
            SetDlgItemTextW(hwnd, IDC_RENDER_CANCEL_STATUS, status.c_str());
            return TRUE;
        }
        break;

    case WM_RENDER_PROGRESS:
        if (task) {
            int percent = 0;
            std::wstring status;
            {
                std::lock_guard<std::mutex> lock(task->mutex);
                percent = task->percent;
                status = task->status;
            }
            SendDlgItemMessageW(hwnd, IDC_RENDER_CANCEL_PROGRESS, PBM_SETPOS, percent, 0);
            SetDlgItemTextW(hwnd, IDC_RENDER_CANCEL_STATUS, status.c_str());
            return TRUE;
        }
        break;

    case WM_RENDER_FINISHED:
        if (task) {
            EndDialog(hwnd, task->success.load(std::memory_order_acquire) ? IDOK : IDCANCEL);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        if (task && !task->finished.load(std::memory_order_acquire)) {
            task->cancelRequested.store(true, std::memory_order_release);
            EnableWindow(GetDlgItem(hwnd, IDC_RENDER_ABORT), FALSE);
            SetDlgItemTextW(hwnd, IDC_RENDER_CANCEL_STATUS, L"Cancelling...");
            return TRUE;
        }
        EndDialog(hwnd, IDCANCEL);
        return TRUE;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_RENDER_CANCEL);
        if (task) task->dialog = nullptr;
        return TRUE;
    }
    return FALSE;
}

INT_PTR CALLBACK RenderDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        SetDlgItemInt(hwnd, IDC_RENDER_WIDTH, 1280, FALSE);
        SetDlgItemInt(hwnd, IDC_RENDER_HEIGHT, 720, FALSE);
        std::wstring ffmpegPath;
        const bool ffmpegAvailable = VideoRenderer::FindFfmpeg(ffmpegPath);
        const bool audioAvailable = g_wavAudio.IsLoaded()
            || (g_simpleSynth.IsAvailable() && IsAudioEnabled());
        SendDlgItemMessageW(hwnd, IDC_RENDER_PROGRESS, PBM_SETRANGE32, 0, 100);
        SendDlgItemMessageW(hwnd, IDC_RENDER_PROGRESS, PBM_SETPOS, 0, 0);
        SendDlgItemMessageW(hwnd, IDC_RENDER_AUDIO, BM_SETCHECK,
                             audioAvailable && (g_wavAudio.IsLoaded() || IsAudioEnabled())
                                 ? BST_CHECKED : BST_UNCHECKED, 0);
        SetRenderDialogText(hwnd, IDC_RENDER_AUDIO_VOLUME,
                            std::to_wstring(static_cast<int>(g_simpleSynth.MasterVolume() * 100.0f + 0.5f)) + L"%");
        SetRenderDialogText(hwnd, IDC_RENDER_STATUS,
                            ffmpegAvailable ? L"Ready"
                                            : L"ffmpeg.exe was not found. Put it beside the EXE or add it to PATH.");
        EnableWindow(GetDlgItem(hwnd, IDC_RENDER_RENDER), ffmpegAvailable);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_RENDER_BROWSE: {
            wchar_t path[MAX_PATH]{};
            lstrcpynW(path, L"PianoVisualizerClassic.mp4", MAX_PATH);
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"MP4 Video\0*.mp4\0All Files\0*.*\0";
            ofn.lpstrFile = path;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrDefExt = L"mp4";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
            if (GetSaveFileNameW(&ofn)) SetDlgItemTextW(hwnd, IDC_RENDER_OUTPUT, path);
            return TRUE;
        }
        case IDC_RENDER_RENDER: {
            BOOL okWidth = FALSE, okHeight = FALSE;
            const UINT width = GetDlgItemInt(hwnd, IDC_RENDER_WIDTH, &okWidth, FALSE);
            const UINT height = GetDlgItemInt(hwnd, IDC_RENDER_HEIGHT, &okHeight, FALSE);
            if (!okWidth || !okHeight || width < 64 || height < 64 || (width & 1u) || (height & 1u)) {
                MessageBoxW(hwnd, L"Width and height must be even values of at least 64 pixels.",
                            L"Render Video", MB_ICONWARNING);
                return TRUE;
            }

            wchar_t output[MAX_PATH]{};
            GetDlgItemTextW(hwnd, IDC_RENDER_OUTPUT, output, MAX_PATH);
            std::wstring outputPath = output;
            if (outputPath.empty()) {
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_RENDER_BROWSE, BN_CLICKED), 0);
                GetDlgItemTextW(hwnd, IDC_RENDER_OUTPUT, output, MAX_PATH);
                outputPath = output;
            }
            if (outputPath.empty()) return TRUE;

            std::wstring ffmpegPath;
            if (!VideoRenderer::FindFfmpeg(ffmpegPath)) {
                MessageBoxW(hwnd,
                            L"ffmpeg.exe was not found. Put ffmpeg.exe beside the application EXE or add it to PATH.",
                            L"Render Video", MB_ICONERROR);
                return TRUE;
            }

            const bool includeAudio =
                SendDlgItemMessageW(hwnd, IDC_RENDER_AUDIO, BM_GETCHECK, 0, 0) == BST_CHECKED;

            RenderTask task;
            task.options.width = width;
            task.options.height = height;
            task.options.fps = 60.0;
            task.options.tailSeconds = g_tailMilliseconds / 1000.0;
            task.options.fallSpeed = g_visualizer.FallSpeed();
            task.options.includeAudio = includeAudio;
            task.options.audioVolume = g_simpleSynth.MasterVolume();
            // Follow the same source selection as live playback. If the WAV is
            // loaded but the synth checkbox is checked, render synth audio instead.
            if (g_wavAudio.IsLoaded() && !g_audioEnabledPreference) {
                task.options.audioInputPath = g_wavAudio.Path();
                task.options.audioOffsetMilliseconds = g_wavOffsetMilliseconds;
            }
            task.options.outputPath = outputPath;
            task.options.ffmpegPath = ffmpegPath;

            EnableWindow(GetDlgItem(hwnd, IDC_RENDER_RENDER), FALSE);
            SetRenderDialogText(hwnd, IDC_RENDER_STATUS, L"Starting renderer...");
            SendDlgItemMessageW(hwnd, IDC_RENDER_PROGRESS, PBM_SETPOS, 0, 0);
            UpdateWindow(hwnd);

            // The worker now uses an independent Visualizer/render context.
            // The live window does not need to be disabled or have its resources
            // swapped out while the MP4 is being rendered.
            const INT_PTR cancelDialogResult = DialogBoxParamW(
                g_instance, MAKEINTRESOURCEW(IDD_RENDER_CANCEL_DIALOG), hwnd,
                RenderCancelDlgProc, reinterpret_cast<LPARAM>(&task));
            (void)cancelDialogResult;

            if (task.worker.joinable()) task.worker.join();

            std::wstring taskError;
            {
                std::lock_guard<std::mutex> lock(task.mutex);
                taskError = task.error;
            }

            const bool success = task.success.load(std::memory_order_acquire);
            const bool canceled = task.cancelRequested.load(std::memory_order_acquire) && !success;

            if (success) {
                SetRenderDialogText(hwnd, IDC_RENDER_STATUS, L"Completed.");
                SendDlgItemMessageW(hwnd, IDC_RENDER_PROGRESS, PBM_SETPOS, 100, 0);
                MessageBoxW(hwnd, L"The MP4 video has been rendered successfully.",
                            L"Piano Visualizer Classic", MB_ICONINFORMATION);
                EndDialog(hwnd, IDOK);
                return TRUE;
            }

            if (canceled || taskError == L"Rendering canceled.") {
                SetRenderDialogText(hwnd, IDC_RENDER_STATUS, L"Rendering canceled.");
                EnableWindow(GetDlgItem(hwnd, IDC_RENDER_RENDER), TRUE);
                return TRUE;
            }

            SetRenderDialogText(hwnd, IDC_RENDER_STATUS, L"Rendering failed.");
            EnableWindow(GetDlgItem(hwnd, IDC_RENDER_RENDER), TRUE);
            MessageBoxW(hwnd, taskError.empty() ? L"Video rendering failed." : taskError.c_str(),
                        L"Piano Visualizer Classic", MB_ICONERROR);
            return TRUE;
        }
        case IDC_RENDER_CANCEL:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(hwnd, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

void CommitTailMillisecondsFromControl(HWND hwnd) {
    BOOL translated = FALSE;
    const UINT value = GetDlgItemInt(hwnd, IDC_EDIT_TAIL_MS, &translated, FALSE);
    if (!translated || value > static_cast<UINT>(kMaximumTailMilliseconds)) {
        // Restore the last valid value if the field is empty or outside the
        // supported range. The edit limit alone cannot enforce the upper bound.
        SetDlgItemInt(hwnd, IDC_EDIT_TAIL_MS, static_cast<UINT>(g_tailMilliseconds), FALSE);
        return;
    }
    const int newValue = static_cast<int>(value);
    if (newValue == g_tailMilliseconds) return;
    g_tailMilliseconds = newValue;
    g_visualizer.SetTailDurationSeconds(g_tailMilliseconds / 1000.0);
    MarkSettingsDirty();
    if (g_visualizerHwnd) InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
}

INT_PTR CALLBACK ControlDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        g_controlHwnd = hwnd;
        SendDlgItemMessageW(hwnd, IDC_EDIT_TAIL_MS, EM_LIMITTEXT, 5, 0);
        SetDlgItemInt(hwnd, IDC_EDIT_TAIL_MS, static_cast<UINT>(g_tailMilliseconds), FALSE);
        SendDlgItemMessageW(hwnd, IDC_SPEED_SLIDER, TBM_SETRANGE, TRUE, MAKELONG(150, 700));
        SendDlgItemMessageW(hwnd, IDC_SPEED_SLIDER, TBM_SETPAGESIZE, 0, 50);
        SendDlgItemMessageW(hwnd, IDC_SPEED_SLIDER, TBM_SETPOS, TRUE,
                            static_cast<LPARAM>(g_visualizer.FallSpeed()));

        SendDlgItemMessageW(hwnd, IDC_POSITION_SLIDER, TBM_SETRANGE, TRUE,
                             MAKELONG(0, kPositionSliderMax));
        SendDlgItemMessageW(hwnd, IDC_POSITION_SLIDER, TBM_SETPAGESIZE, 0, 500);
        SendDlgItemMessageW(hwnd, IDC_POSITION_SLIDER, TBM_SETTICFREQ, 1000, 0);

        SendDlgItemMessageW(hwnd, IDC_BG_OPACITY_SLIDER, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendDlgItemMessageW(hwnd, IDC_BG_OPACITY_SLIDER, TBM_SETPAGESIZE, 0, 10);
        SendDlgItemMessageW(hwnd, IDC_BG_OPACITY_SLIDER, TBM_SETTICFREQ, 10, 0);
        SendDlgItemMessageW(hwnd, IDC_BG_OPACITY_SLIDER, TBM_SETPOS, TRUE,
                            static_cast<LPARAM>(g_visualizer.BackgroundImageOpacity() * 100.0f + 0.5f));
        SendDlgItemMessageW(hwnd, IDC_VIDEO_OPACITY_SLIDER, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendDlgItemMessageW(hwnd, IDC_VIDEO_OPACITY_SLIDER, TBM_SETPAGESIZE, 0, 10);
        SendDlgItemMessageW(hwnd, IDC_VIDEO_OPACITY_SLIDER, TBM_SETTICFREQ, 10, 0);
        SendDlgItemMessageW(hwnd, IDC_VIDEO_OPACITY_SLIDER, TBM_SETPOS, TRUE,
                            static_cast<LPARAM>(g_visualizer.BackgroundVideoOpacity() * 100.0f + 0.5f));

        SendDlgItemMessageW(hwnd, IDC_VIDEO_OFFSET_SLIDER, TBM_SETRANGE, TRUE,
                            MAKELONG(Visualizer::kMinVideoOffsetMilliseconds,
                                     Visualizer::kMaxVideoOffsetMilliseconds));
        SendDlgItemMessageW(hwnd, IDC_VIDEO_OFFSET_SLIDER, TBM_SETLINESIZE, 0, 1);
        SendDlgItemMessageW(hwnd, IDC_VIDEO_OFFSET_SLIDER, TBM_SETPAGESIZE, 0, 100);
        SendDlgItemMessageW(hwnd, IDC_VIDEO_OFFSET_SLIDER, TBM_SETTICFREQ, 1000, 0);
        SendDlgItemMessageW(hwnd, IDC_VIDEO_OFFSET_SLIDER, TBM_SETPOS, TRUE, 0);
        SetControlText(IDC_LABEL_VIDEO_OFFSET, L"N/A");

        SendDlgItemMessageW(hwnd, IDC_AMBIENT_PARTICLES_SLIDER, TBM_SETRANGE, TRUE, MAKELONG(0, 200));
        SendDlgItemMessageW(hwnd, IDC_AMBIENT_PARTICLES_SLIDER, TBM_SETPAGESIZE, 0, 10);
        SendDlgItemMessageW(hwnd, IDC_AMBIENT_PARTICLES_SLIDER, TBM_SETTICFREQ, 25, 0);
        SendDlgItemMessageW(hwnd, IDC_AMBIENT_PARTICLES_SLIDER, TBM_SETPOS, TRUE,
                            static_cast<LPARAM>(g_visualizer.AmbientParticleCount()));
        SetControlText(IDC_LABEL_AMBIENT_PARTICLES,
                       std::to_wstring(g_visualizer.AmbientParticleCount()));

        SendDlgItemMessageW(hwnd, IDC_NOTE_GLOW_INTENSITY_SLIDER, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendDlgItemMessageW(hwnd, IDC_NOTE_GLOW_INTENSITY_SLIDER, TBM_SETPAGESIZE, 0, 10);
        SendDlgItemMessageW(hwnd, IDC_NOTE_GLOW_INTENSITY_SLIDER, TBM_SETTICFREQ, 10, 0);
        const int noteGlowIntensity = static_cast<int>(g_visualizer.NoteGlowIntensity() * 100.0f + 0.5f);
        SendDlgItemMessageW(hwnd, IDC_NOTE_GLOW_INTENSITY_SLIDER, TBM_SETPOS, TRUE, noteGlowIntensity);
        SetControlText(IDC_LABEL_NOTE_GLOW_INTENSITY, std::to_wstring(noteGlowIntensity) + L"%");

        SendDlgItemMessageW(hwnd, IDC_WAVE_LINE_GLOW, BM_SETCHECK,
            g_visualizer.WaveLineGlowEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);

        SendDlgItemMessageW(hwnd, IDC_WAVE_LINE_AMPLITUDE_SLIDER,
                            TBM_SETRANGE, TRUE, MAKELONG(0, 200));
        SendDlgItemMessageW(hwnd, IDC_WAVE_LINE_AMPLITUDE_SLIDER,
                            TBM_SETPAGESIZE, 0, 10);
        SendDlgItemMessageW(hwnd, IDC_WAVE_LINE_AMPLITUDE_SLIDER,
                            TBM_SETTICFREQ, 20, 0);
        const int waveAmplitude = static_cast<int>(
            g_visualizer.WaveLineAmplitude() * 50.0f + 0.5f);
        SendDlgItemMessageW(hwnd, IDC_WAVE_LINE_AMPLITUDE_SLIDER,
                            TBM_SETPOS, TRUE, waveAmplitude);
        SetControlText(IDC_VALUE_WAVE_LINE_AMPLITUDE,
                       std::to_wstring(waveAmplitude) + L"%");

        SendDlgItemMessageW(hwnd, IDC_VOLUME_SLIDER, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendDlgItemMessageW(hwnd, IDC_VOLUME_SLIDER, TBM_SETPOS, TRUE,
                            static_cast<LPARAM>(g_simpleSynth.MasterVolume() * 100.0f + 0.5f));
        SendDlgItemMessageW(hwnd, IDC_VOLUME_SLIDER, TBM_SETTICFREQ, 10, 0);

        SendDlgItemMessageW(hwnd, IDC_WAV_OFFSET_SLIDER, TBM_SETRANGE, TRUE,
            MAKELONG(WavAudioPlayer::kMinOffsetMilliseconds, WavAudioPlayer::kMaxOffsetMilliseconds));
        SendDlgItemMessageW(hwnd, IDC_WAV_OFFSET_SLIDER, TBM_SETLINESIZE, 0, 1);
        SendDlgItemMessageW(hwnd, IDC_WAV_OFFSET_SLIDER, TBM_SETPAGESIZE, 0, 100);
        SendDlgItemMessageW(hwnd, IDC_WAV_OFFSET_SLIDER, TBM_SETTICFREQ, 1000, 0);
        SendDlgItemMessageW(hwnd, IDC_WAV_OFFSET_SLIDER, TBM_SETPOS, TRUE, g_wavOffsetMilliseconds);

        auto setEffectCheck = [&](int id, Visualizer::EffectFlag effect) {
            SendDlgItemMessageW(hwnd, id, BM_SETCHECK,
                g_visualizer.IsEffectEnabled(effect) ? BST_CHECKED : BST_UNCHECKED, 0);
        };
        setEffectCheck(IDC_EFFECT_SMOKE, Visualizer::EffectSmokeGlow);
        setEffectCheck(IDC_EFFECT_WATER_DROPLETS, Visualizer::EffectWaterDroplets);
        setEffectCheck(IDC_EFFECT_RIPPLE, Visualizer::EffectRipple);
        setEffectCheck(IDC_EFFECT_DIAMOND_RIPPLE, Visualizer::EffectDiamondRipple);
        setEffectCheck(IDC_EFFECT_NOTE_GLOW, Visualizer::EffectNoteGlow);
        setEffectCheck(IDC_EFFECT_CROSS_SPARK, Visualizer::EffectCrossSpark);
        setEffectCheck(IDC_EFFECT_WAVE_LINE, Visualizer::EffectWaveLine);
        setEffectCheck(IDC_EFFECT_SMOKE_WINDOW, Visualizer::EffectSmokeWindow);
        setEffectCheck(IDC_EFFECT_AMBIENT_PARTICLES, Visualizer::EffectAmbientParticles);
        setEffectCheck(IDC_EFFECT_IMPACT_POLYGONS, Visualizer::EffectImpactPolygons);
        setEffectCheck(IDC_EFFECT_KEYBED_GLOW, Visualizer::EffectKeybedGlow);
        EnableWindow(GetDlgItem(hwnd, IDC_NOTE_GLOW_INTENSITY_SLIDER),
            g_visualizer.IsEffectEnabled(Visualizer::EffectNoteGlow));
        SendDlgItemMessageW(hwnd, IDC_NOTE_GUIDES, BM_SETCHECK,
            g_visualizer.NoteGuideLinesEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
        SetControlText(IDC_LABEL_BACKGROUND, g_visualizer.HasBackgroundImage()
            ? BaseName(g_visualizer.BackgroundImagePath()) : L"(No image)");
        SetControlText(IDC_LABEL_BACKGROUND_VIDEO, g_visualizer.HasBackgroundVideo()
            ? BaseName(g_visualizer.BackgroundVideoPath()) : L"(No video)");

        EnableWindow(GetDlgItem(hwnd, IDC_TRACK_COMBO), FALSE);
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_TRACK_COLOR), FALSE);
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_TRACK_RESET), FALSE);
        EnableWindow(GetDlgItem(hwnd, IDC_BG_OPACITY_SLIDER), FALSE);
        EnableWindow(GetDlgItem(hwnd, IDC_VIDEO_OPACITY_SLIDER), FALSE);
        EnableWindow(GetDlgItem(hwnd, IDC_VIDEO_OFFSET_SLIDER), FALSE);
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_BACKGROUND_RESET), FALSE);
        SendDlgItemMessageW(hwnd, IDC_ENABLE_AUDIO, BM_SETCHECK,
            g_audioEnabledPreference ? BST_CHECKED : BST_UNCHECKED, 0);
        // Use the native Windows tab control. The normal workflow starts with
        // MIDI (always visible above these tabs), then Background, Effects,
        // and optional external WAV audio.
        if (HWND tab = GetDlgItem(hwnd, IDC_SETTINGS_TAB)) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<LPWSTR>(L"Background");
            TabCtrl_InsertItem(tab, 0, &item);
            item.pszText = const_cast<LPWSTR>(L"Effects / Colors");
            TabCtrl_InsertItem(tab, 1, &item);
            item.pszText = const_cast<LPWSTR>(L"Audio / WAV");
            TabCtrl_InsertItem(tab, 2, &item);
            TabCtrl_SetCurSel(tab, 0);
        }
        ShowControlPage(hwnd, ControlPage::Background);
        UpdateControlUi();
        if (g_settingsDirty) SetTimer(hwnd, TIMER_SETTINGS_SAVE, 500, nullptr);
        SetTimer(hwnd, TIMER_CONTROL_REFRESH, 100, nullptr);
        return TRUE;
    }
    case WM_MOVE:
        MarkSettingsDirty();
        return TRUE;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_ACTIVE || LOWORD(wp) == WA_CLICKACTIVE) KeepWindowsTogetherInZOrder(hwnd);
        return TRUE;
    case WM_HSCROLL: {
        HWND control = reinterpret_cast<HWND>(lp);
        if (!control) break;
        if (control == GetDlgItem(hwnd, IDC_SPEED_SLIDER)) {
            const int speed = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
            g_visualizer.SetFallSpeed(static_cast<float>(speed));
            SetControlText(IDC_LABEL_SPEED, std::to_wstring(speed) + L" px/s");
            MarkSettingsDirty();
            InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
            return TRUE;
        }
        if (control == GetDlgItem(hwnd, IDC_BG_OPACITY_SLIDER)) {
            const int opacity = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
            g_visualizer.SetBackgroundImageOpacity(opacity / 100.0f);
            SetControlText(IDC_LABEL_BG_OPACITY, std::to_wstring(opacity) + L"%");
            MarkSettingsDirty();
            InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
            return TRUE;
        }
        if (control == GetDlgItem(hwnd, IDC_VIDEO_OPACITY_SLIDER)) {
            const int opacity = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
            g_visualizer.SetBackgroundVideoOpacity(opacity / 100.0f);
            SetControlText(IDC_LABEL_VIDEO_OPACITY, std::to_wstring(opacity) + L"%");
            MarkSettingsDirty();
            InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
            return TRUE;
        }
        if (control == GetDlgItem(hwnd, IDC_VIDEO_OFFSET_SLIDER)) {
            const int offset = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
            g_visualizer.SetVideoOffsetMilliseconds(offset);
            SetControlText(IDC_LABEL_VIDEO_OFFSET, FormatVideoOffset(offset));
            MarkSettingsDirty();
            InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
            return TRUE;
        }
        if (control == GetDlgItem(hwnd, IDC_WAV_OFFSET_SLIDER)) {
            const int offset = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
            g_wavOffsetMilliseconds = std::clamp(offset,
                WavAudioPlayer::kMinOffsetMilliseconds, WavAudioPlayer::kMaxOffsetMilliseconds);
            SetControlText(IDC_LABEL_WAV_OFFSET, g_wavAudio.IsLoaded()
                ? FormatVideoOffset(g_wavOffsetMilliseconds) : L"N/A");
            MarkSettingsDirty();
            if (wp != TB_THUMBTRACK) RestartAudioAtCurrentPosition();
            return TRUE;
        }
        if (control == GetDlgItem(hwnd, IDC_AMBIENT_PARTICLES_SLIDER)) {
            const int count = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
            g_visualizer.SetAmbientParticleCount(count);
            SetControlText(IDC_LABEL_AMBIENT_PARTICLES, std::to_wstring(count));
            MarkSettingsDirty();
            InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
            return TRUE;
        }
        if (control == GetDlgItem(hwnd, IDC_NOTE_GLOW_INTENSITY_SLIDER)) {
            const int intensity = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
            g_visualizer.SetNoteGlowIntensity(intensity / 100.0f);
            SetControlText(IDC_LABEL_NOTE_GLOW_INTENSITY, std::to_wstring(intensity) + L"%");
            MarkSettingsDirty();
            InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
            return TRUE;
        }
        if (control == GetDlgItem(hwnd, IDC_WAVE_LINE_AMPLITUDE_SLIDER)) {
            const int amplitude = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
            g_visualizer.SetWaveLineAmplitude(amplitude / 50.0f);
            SetControlText(IDC_VALUE_WAVE_LINE_AMPLITUDE,
                           std::to_wstring(amplitude) + L"%");
            MarkSettingsDirty();
            InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
            return TRUE;
        }
        if (control == GetDlgItem(hwnd, IDC_VOLUME_SLIDER)) {
            g_volumeDragging = true;
            const int volume = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
            g_simpleSynth.SetMasterVolume(volume / 100.0f);
            g_wavAudio.SetMasterVolume(volume / 100.0f);
            SetControlText(IDC_LABEL_VOLUME, std::to_wstring(volume) + L"%");
            MarkSettingsDirty();
            if (wp == TB_ENDTRACK) g_volumeDragging = false;
            return TRUE;
        }
        if (control == GetDlgItem(hwnd, IDC_POSITION_SLIDER)) {
            if (!g_positionDragging) {
                g_effectTailRunning = false;
                g_positionDragging = true;
                g_visualizer.SetInteractiveSeeking(true);
                g_resumeAfterSeek = g_playing;
                if (g_resumeAfterSeek) StopAudioPlayback();
            }
            const int pos = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
            const double ratio = static_cast<double>(pos) / kPositionSliderMax;
            const double seekTime = ratio * g_visualizer.Duration();
            g_visualizer.SetTime(seekTime);
            SetControlText(IDC_LABEL_TIME,
                           FormatTime(g_visualizer.Time()) + L" / " + FormatTime(g_visualizer.Duration()));
            InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
            if (wp == TB_THUMBPOSITION || wp == TB_ENDTRACK) {
                const bool resume = g_resumeAfterSeek;
                g_positionDragging = false;
                g_visualizer.SetInteractiveSeeking(false);
                g_resumeAfterSeek = false;
                if (resume) {
                    g_playing = true;
                    g_visualizer.SetPlaying(true);
                    g_playbackBaseTime = g_visualizer.Time();
                    g_playbackEpoch = std::chrono::steady_clock::now();
                    StartAudioPlayback();
                }
                UpdateControlUi();
            }
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lp);
        if (header && header->idFrom == IDC_SETTINGS_TAB && header->code == TCN_SELCHANGE) {
            switch (TabCtrl_GetCurSel(header->hwndFrom)) {
            case 0: ShowControlPage(hwnd, ControlPage::Background); break;
            case 1: ShowControlPage(hwnd, ControlPage::Effects); break;
            case 2: ShowControlPage(hwnd, ControlPage::Audio); break;
            }
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_EDIT_TAIL_MS && HIWORD(wp) == EN_KILLFOCUS) {
            CommitTailMillisecondsFromControl(hwnd);
            return TRUE;
        }
        // Treat Enter in the numeric field as "apply", not as a request to
        // close the modeless control window.
        if (LOWORD(wp) == IDOK) {
            CommitTailMillisecondsFromControl(hwnd);
            return TRUE;
        }
        switch (LOWORD(wp)) {
        case IDC_BTN_OPEN:
            OpenMidi(hwnd);
            return TRUE;
        case IDC_BTN_WAV_OPEN:
            OpenWavAudio(hwnd);
            return TRUE;
        case IDC_BTN_WAV_UNLOAD:
            UnloadWavAudio();
            return TRUE;
        case IDC_BTN_BACKGROUND_IMAGE:
            OpenBackgroundImage(hwnd);
            return TRUE;
        case IDC_BTN_BACKGROUND_VIDEO:
            OpenBackgroundVideo(hwnd);
            return TRUE;
        case IDC_BTN_BACKGROUND_RESET:
            ResetBackground(hwnd);
            return TRUE;
        case IDC_BTN_RENDER:
            OpenRenderDialog(hwnd);
            return TRUE;
        case IDC_ENABLE_AUDIO:
            if (HIWORD(wp) == BN_CLICKED) {
                g_audioEnabledPreference = IsAudioEnabled();
                MarkSettingsDirty();
                // Switching the checkbox switches the live audio source too:
                // checked selects the MIDI synth; unchecked selects the WAV if
                // loaded. Restart at the current timeline point to avoid jumps.
                if (g_playing) {
                    RestartAudioAtCurrentPosition();
                } else if (g_effectTailRunning) {
                    StopAudioPlayback();
                    g_playbackBaseTime = g_visualizer.Time();
                    g_playbackEpoch = std::chrono::steady_clock::now();
                    StartAudioPlayback();
                } else {
                    StopAudioPlayback();
                }
                UpdateControlUi();
                return TRUE;
            }
            break;
        case IDC_BTN_PLAY:
            TogglePlay();
            return TRUE;
        case IDC_BTN_RESTART:
            ResetPlayback();
            return TRUE;
        case IDC_TRACK_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                UpdateTrackColorUi();
                return TRUE;
            }
            break;
        case IDC_BTN_TRACK_COLOR:
            if (HIWORD(wp) == BN_CLICKED) {
                OpenTrackColor(hwnd);
                return TRUE;
            }
            break;
        case IDC_BTN_TRACK_RESET:
            if (HIWORD(wp) == BN_CLICKED) {
                g_visualizer.ResetTrackColors();
                MarkSettingsDirty();
                UpdateTrackColorUi();
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_EFFECT_SMOKE:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_EFFECT_SMOKE), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetEffectEnabled(Visualizer::EffectSmokeGlow, checked);
                MarkSettingsDirty();
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_EFFECT_WATER_DROPLETS:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_EFFECT_WATER_DROPLETS), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetEffectEnabled(Visualizer::EffectWaterDroplets, checked);
                MarkSettingsDirty();
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_EFFECT_RIPPLE:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_EFFECT_RIPPLE), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetEffectEnabled(Visualizer::EffectRipple, checked);
                MarkSettingsDirty();
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_EFFECT_DIAMOND_RIPPLE:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_EFFECT_DIAMOND_RIPPLE), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetEffectEnabled(Visualizer::EffectDiamondRipple, checked);
                MarkSettingsDirty();
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_EFFECT_NOTE_GLOW:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_EFFECT_NOTE_GLOW), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetEffectEnabled(Visualizer::EffectNoteGlow, checked);
                EnableWindow(GetDlgItem(hwnd, IDC_NOTE_GLOW_INTENSITY_SLIDER), checked);
                MarkSettingsDirty();
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_EFFECT_CROSS_SPARK:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_EFFECT_CROSS_SPARK), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetEffectEnabled(Visualizer::EffectCrossSpark, checked);
                MarkSettingsDirty();
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_WAVE_LINE_GLOW:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_WAVE_LINE_GLOW), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetWaveLineGlowEnabled(checked);
                MarkSettingsDirty();
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_EFFECT_WAVE_LINE:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_EFFECT_WAVE_LINE), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetEffectEnabled(Visualizer::EffectWaveLine, checked);
                MarkSettingsDirty();
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_BTN_WAVE_COLOR:
            if (HIWORD(wp) == BN_CLICKED) {
                OpenWaveLineColor(hwnd);
                return TRUE;
            }
            break;
        case IDC_EFFECT_SMOKE_WINDOW:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_EFFECT_SMOKE_WINDOW), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetEffectEnabled(Visualizer::EffectSmokeWindow, checked);
                MarkSettingsDirty();
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_EFFECT_AMBIENT_PARTICLES:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_EFFECT_AMBIENT_PARTICLES), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetEffectEnabled(Visualizer::EffectAmbientParticles, checked);
                MarkSettingsDirty();
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_EFFECT_IMPACT_POLYGONS:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_EFFECT_IMPACT_POLYGONS), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetEffectEnabled(Visualizer::EffectImpactPolygons, checked);
                MarkSettingsDirty();
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_EFFECT_KEYBED_GLOW:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_EFFECT_KEYBED_GLOW), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetEffectEnabled(Visualizer::EffectKeybedGlow, checked);
                MarkSettingsDirty();
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_NOTE_GUIDES:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_NOTE_GUIDES), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetNoteGuideLinesEnabled(checked);
                MarkSettingsDirty();
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        }
        break;
    case WM_TIMER:
        if (wp == TIMER_SETTINGS_SAVE) {
            KillTimer(hwnd, TIMER_SETTINGS_SAVE);
            SaveSettings();
            return TRUE;
        }
        if (wp == TIMER_CONTROL_REFRESH) {
            UpdateControlUi();
            return TRUE;
        }
        break;
    case WM_CLOSE:
        CommitTailMillisecondsFromControl(hwnd);
        KillTimer(hwnd, TIMER_CONTROL_REFRESH);
        KillTimer(hwnd, TIMER_SETTINGS_SAVE);
        SaveSettings();
        if (g_visualizerHwnd) PostMessageW(g_visualizerHwnd, WM_CLOSE, 0, 0);
        else DestroyWindow(hwnd);
        return TRUE;
    }
    return FALSE;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    g_instance = hInstance;

    // Read window coordinates before creating either top-level window.
    g_iniPath = GetIniFilePath();
    g_visualizerStartupX = ReadIniInt(L"WindowPosition", L"VisualizerX", CW_USEDEFAULT);
    g_visualizerStartupY = ReadIniInt(L"WindowPosition", L"VisualizerY", CW_USEDEFAULT);
    if (g_visualizerStartupX == CW_USEDEFAULT || g_visualizerStartupY == CW_USEDEFAULT) {
        g_visualizerStartupX = CW_USEDEFAULT;
        g_visualizerStartupY = CW_USEDEFAULT;
    }
    g_controlStartupX = ReadIniInt(L"WindowPosition", L"ControlX", 80);
    g_controlStartupY = ReadIniInt(L"WindowPosition", L"ControlY", 80);
    if (g_controlStartupX == CW_USEDEFAULT || g_controlStartupY == CW_USEDEFAULT) {
        g_controlStartupX = 80;
        g_controlStartupY = 80;
    }

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        MessageBoxW(nullptr, L"Failed to initialize COM.", L"Piano Visualizer Classic", MB_ICONERROR);
        return 1;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&icc);

    std::wstring audioError;
    g_simpleSynth.Initialize(audioError);
    g_simpleSynth.SetMasterVolume(0.75f);
    std::wstring wavAudioError;
    if (g_wavAudio.Initialize(wavAudioError)) {
        g_wavAudio.SetMasterVolume(g_simpleSynth.MasterVolume());
    }

    WNDCLASSW wc{};
    wc.hInstance = hInstance;
    wc.lpfnWndProc = VisualizerWndProc;
    wc.lpszClassName = L"PianoVisualizerClassic";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_PIANOVISUALIZER));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    int visualizerX = g_visualizerStartupX;
    int visualizerY = g_visualizerStartupY;
    if (visualizerX != CW_USEDEFAULT && visualizerY != CW_USEDEFAULT) {
        const POINT clamped = ClampWindowPositionToWorkArea(visualizerX, visualizerY, 1280, 760);
        visualizerX = clamped.x;
        visualizerY = clamped.y;
    }
    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Piano Visualizer Classic", WS_OVERLAPPEDWINDOW,
        visualizerX, visualizerY, 1280, 760,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        g_wavAudio.Shutdown();
        g_simpleSynth.Shutdown();
        if (SUCCEEDED(comHr) || comHr == S_FALSE) CoUninitialize();
        return 1;
    }

    // WM_CREATE has initialized the renderer. Restore saved preferences before
    // showing the windows so the first visible frame uses the saved settings.
    LoadSettings();

    g_visualizerHwnd = hwnd;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    g_controlHwnd = CreateDialogParamW(
        hInstance, MAKEINTRESOURCEW(IDD_CONTROL_DIALOG), nullptr, ControlDlgProc, 0);
    if (!g_controlHwnd) {
        MessageBoxW(hwnd, L"Failed to create the control dialog.", L"Piano Visualizer Classic", MB_ICONERROR);
        DestroyWindow(hwnd);
        g_wavAudio.Shutdown();
        g_simpleSynth.Shutdown();
        g_visualizer.Shutdown();
        if (SUCCEEDED(comHr) || comHr == S_FALSE) CoUninitialize();
        return 1;
    }

    RECT controlRect{};
    GetWindowRect(g_controlHwnd, &controlRect);
    const int controlWidth = std::max<LONG>(1, controlRect.right - controlRect.left);
    const int controlHeight = std::max<LONG>(1, controlRect.bottom - controlRect.top);
    const POINT controlPosition = ClampWindowPositionToWorkArea(
        g_controlStartupX, g_controlStartupY, controlWidth, controlHeight);
    SetWindowPos(g_controlHwnd, HWND_TOP, controlPosition.x, controlPosition.y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(g_controlHwnd, SW_SHOW);
    UpdateWindow(g_controlHwnd);
    SetForegroundWindow(hwnd);

    using Clock = std::chrono::steady_clock;
    constexpr auto kFramePeriod = std::chrono::microseconds(16667);
    g_playbackEpoch = Clock::now();
    auto nextFrame = Clock::now();
    int result = 0;
    bool running = true;

    while (running) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                result = static_cast<int>(msg.wParam);
                running = false;
                break;
            }
            if (g_controlHwnd && IsDialogMessageW(g_controlHwnd, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        const auto now = Clock::now();
        if ((g_playing || g_effectTailRunning) && !g_positionDragging && now >= nextFrame) {
            if (g_playing) {
                double elapsed = std::chrono::duration<double>(now - g_playbackEpoch).count();
                if (elapsed < 0.0) elapsed = 0.0;
                const double nextTime = g_playbackBaseTime + elapsed;

                if (g_visualizer.Duration() > 0.0 && nextTime >= g_visualizer.Duration()) {
                    g_visualizer.SetTime(g_visualizer.Duration());
                    g_playing = false;
                    g_effectTailRunning = true;
                    g_effectTailStart = now;
                    g_visualizer.SetPlaying(false);
                    if (g_wavAudio.IsLoaded() && !g_audioEnabledPreference) {
                        // Leave the selected WAV running during the configured
                        // visual/effect tail. If the internal synth is selected,
                        // stop all audio sources instead of letting the unused WAV run.
                        g_simpleSynth.Stop();
                    } else {
                        StopAudioPlayback();
                    }
                    UpdateControlUi();
                } else {
                    g_visualizer.SetTime(nextTime);
                }
            } else if (g_effectTailRunning) {
                const double tailElapsed = std::chrono::duration<double>(now - g_effectTailStart).count();
                const double tailDuration = g_tailMilliseconds / 1000.0;
                g_visualizer.SetTime(g_visualizer.Duration() + std::min(tailElapsed, tailDuration));
                if (tailElapsed >= tailDuration) {
                    g_effectTailRunning = false;
                    StopAudioPlayback();
                    UpdateControlUi();
                }
            }

            do { nextFrame += kFramePeriod; } while (nextFrame <= now);
            g_visualizer.Render();
        }

        const auto afterNow = Clock::now();
        if (nextFrame > afterNow) {
            auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(nextFrame - afterNow).count();
            if (waitMs < 1) waitMs = 1;
            if (waitMs > 16) waitMs = 16;
            MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(waitMs),
                                        QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }

    StopAudioPlayback();
    g_wavAudio.Shutdown();
    g_simpleSynth.Shutdown();
    g_visualizer.Shutdown();
    g_song = MidiSong{};
    if (SUCCEEDED(comHr) || comHr == S_FALSE) CoUninitialize();
    return result;
}
