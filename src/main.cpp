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
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "MidiFile.h"
#include "Visualizer.h"
#include "SimpleSynth.h"
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
constexpr int kPositionSliderMax = 10000;

HINSTANCE g_instance = nullptr;
HWND g_visualizerHwnd = nullptr;
HWND g_controlHwnd = nullptr;
Visualizer g_visualizer;
MidiSong g_song;
SimpleSynth g_simpleSynth;
bool g_playing = false;
bool g_positionDragging = false;
bool g_resumeAfterSeek = false;
bool g_volumeDragging = false;
std::wstring g_fileName;
std::chrono::steady_clock::time_point g_playbackEpoch;
double g_playbackBaseTime = 0.0;
COLORREF g_customColors[16] = {};

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
    if (!g_controlHwnd) return true;
    HWND check = GetDlgItem(g_controlHwnd, IDC_ENABLE_AUDIO);
    return check && SendMessageW(check, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void StartAudioPlayback() {
    if (!IsAudioEnabled() || !g_simpleSynth.IsAvailable() || g_song.duration <= 0.0) return;
    g_simpleSynth.Start(&g_song, g_visualizer.Time(), g_playbackEpoch);
}

void StopAudioPlayback() {
    g_simpleSynth.Stop();
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
    UpdateTrackColorUi();
    InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
}

void UpdateControlUi() {
    if (!g_controlHwnd) return;

    SetControlText(IDC_LABEL_FILE, g_song.duration > 0.0 ? g_fileName : L"(No MIDI loaded)");
    SetControlText(IDC_LABEL_STATUS, g_song.duration > 0.0
        ? (g_playing ? L"Playing" : L"Stopped") : L"No MIDI");

    const double duration = g_visualizer.Duration();
    const double time = std::clamp(g_visualizer.Time(), 0.0, std::max(0.0, duration));
    SetControlText(IDC_LABEL_TIME, FormatTime(time) + L" / " + FormatTime(duration));
    SetControlText(IDC_LABEL_SPEED,
                   std::to_wstring(static_cast<int>(g_visualizer.FallSpeed())) + L" px/s");
    UpdateTrackColorUi();

    HWND audioCheck = GetDlgItem(g_controlHwnd, IDC_ENABLE_AUDIO);
    if (audioCheck) {
        const bool available = g_simpleSynth.IsAvailable();
        EnableWindow(audioCheck, available);
        if (!available) SendMessageW(audioCheck, BM_SETCHECK, BST_UNCHECKED, 0);
        SetControlText(IDC_LABEL_AUDIO_STATUS,
                       available ? L"Internal simple piano synth (XAudio2)"
                                 : L"Internal audio is unavailable.");
    }

    const int volume = static_cast<int>(g_simpleSynth.MasterVolume() * 100.0f + 0.5f);
    SetControlText(IDC_LABEL_VOLUME, std::to_wstring(volume) + L"%");
    if (!g_volumeDragging) {
        SendDlgItemMessageW(g_controlHwnd, IDC_VOLUME_SLIDER, TBM_SETPOS, TRUE, volume);
    }

    if (g_visualizer.HasBackgroundImage()) {
        const int opacity = static_cast<int>(g_visualizer.BackgroundOpacity() * 100.0f + 0.5f);
        SetControlText(IDC_LABEL_BG_OPACITY, std::to_wstring(opacity) + L"%");
        EnableWindow(GetDlgItem(g_controlHwnd, IDC_BG_OPACITY_SLIDER), TRUE);
        SendDlgItemMessageW(g_controlHwnd, IDC_BG_OPACITY_SLIDER, TBM_SETPOS, TRUE, opacity);
    } else {
        SetControlText(IDC_LABEL_BG_OPACITY, L"0%");
        EnableWindow(GetDlgItem(g_controlHwnd, IDC_BG_OPACITY_SLIDER), FALSE);
        SendDlgItemMessageW(g_controlHwnd, IDC_BG_OPACITY_SLIDER, TBM_SETPOS, TRUE, 0);
    }

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

void OpenBackground(HWND owner) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;

    if (!g_visualizer.LoadBackgroundImage(path)) {
        MessageBoxW(owner, L"Failed to load the background image.", L"Piano Visualizer Classic", MB_ICONERROR);
        return;
    }
    SetControlText(IDC_LABEL_BACKGROUND, BaseName(path));
    UpdateControlUi();
    InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
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

    StopAudioPlayback();
    g_playing = false;
    g_visualizer.SetPlaying(false);
    g_song = std::move(newSong);
    g_fileName = BaseName(path);
    g_visualizer.SetSong(&g_song);
    RebuildTrackCombo();
    g_visualizer.SetTime(0.0);
    g_playbackBaseTime = 0.0;
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

        bool audioReady = CreateTempWavPath(tempWav);
        if (audioReady) {
            task->options.temporaryWavPath = tempWav;
            audioReady = g_simpleSynth.RenderWav(
                g_song, tempWav, task->options.audioVolume,
                VideoRenderer::kTailSeconds, error, &task->cancelRequested);
        } else {
            error = L"Failed to create a temporary WAV file.";
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
    case WM_DESTROY:
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
        SetDlgItemInt(hwnd, IDC_RENDER_WIDTH, 1920, FALSE);
        SetDlgItemInt(hwnd, IDC_RENDER_HEIGHT, 1080, FALSE);
        std::wstring ffmpegPath;
        const bool ffmpegAvailable = VideoRenderer::FindFfmpeg(ffmpegPath);
        const bool audioAvailable = g_simpleSynth.IsAvailable();
        SendDlgItemMessageW(hwnd, IDC_RENDER_PROGRESS, PBM_SETRANGE32, 0, 100);
        SendDlgItemMessageW(hwnd, IDC_RENDER_PROGRESS, PBM_SETPOS, 0, 0);
        SendDlgItemMessageW(hwnd, IDC_RENDER_AUDIO, BM_SETCHECK,
                             audioAvailable && IsAudioEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
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
            task.options.fallSpeed = g_visualizer.FallSpeed();
            task.options.includeAudio = includeAudio;
            task.options.audioVolume = g_simpleSynth.MasterVolume();
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

INT_PTR CALLBACK ControlDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        g_controlHwnd = hwnd;
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
                            static_cast<LPARAM>(g_visualizer.BackgroundOpacity() * 100.0f + 0.5f));

        SendDlgItemMessageW(hwnd, IDC_VOLUME_SLIDER, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendDlgItemMessageW(hwnd, IDC_VOLUME_SLIDER, TBM_SETPOS, TRUE,
                            static_cast<LPARAM>(g_simpleSynth.MasterVolume() * 100.0f + 0.5f));
        SendDlgItemMessageW(hwnd, IDC_VOLUME_SLIDER, TBM_SETTICFREQ, 10, 0);

        SendDlgItemMessageW(hwnd, IDC_EFFECT_SMOKE, BM_SETCHECK, BST_CHECKED, 0);
        SendDlgItemMessageW(hwnd, IDC_EFFECT_SPARK, BM_SETCHECK, BST_UNCHECKED, 0);
        SendDlgItemMessageW(hwnd, IDC_EFFECT_RIPPLE, BM_SETCHECK, BST_UNCHECKED, 0);
        SendDlgItemMessageW(hwnd, IDC_EFFECT_DIAMOND_RIPPLE, BM_SETCHECK, BST_UNCHECKED, 0);
        SendDlgItemMessageW(hwnd, IDC_EFFECT_NOTE_GLOW, BM_SETCHECK, BST_UNCHECKED, 0);
        SendDlgItemMessageW(hwnd, IDC_EFFECT_CROSS_SPARK, BM_SETCHECK, BST_CHECKED, 0);
        SendDlgItemMessageW(hwnd, IDC_NOTE_GUIDES, BM_SETCHECK, BST_CHECKED, 0);
        g_visualizer.SetEffectMask(Visualizer::EffectSmokeGlow | Visualizer::EffectCrossSpark);
        g_visualizer.SetNoteGuideLinesEnabled(true);

        EnableWindow(GetDlgItem(hwnd, IDC_TRACK_COMBO), FALSE);
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_TRACK_COLOR), FALSE);
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_TRACK_RESET), FALSE);
        EnableWindow(GetDlgItem(hwnd, IDC_BG_OPACITY_SLIDER), FALSE);
        SendDlgItemMessageW(hwnd, IDC_ENABLE_AUDIO, BM_SETCHECK, BST_CHECKED, 0);
        UpdateControlUi();
        SetTimer(hwnd, TIMER_CONTROL_REFRESH, 100, nullptr);
        return TRUE;
    }
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
            InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
            return TRUE;
        }
        if (control == GetDlgItem(hwnd, IDC_BG_OPACITY_SLIDER)) {
            const int opacity = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
            g_visualizer.SetBackgroundOpacity(opacity / 100.0f);
            SetControlText(IDC_LABEL_BG_OPACITY, std::to_wstring(opacity) + L"%");
            InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
            return TRUE;
        }
        if (control == GetDlgItem(hwnd, IDC_VOLUME_SLIDER)) {
            g_volumeDragging = true;
            const int volume = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
            g_simpleSynth.SetMasterVolume(volume / 100.0f);
            SetControlText(IDC_LABEL_VOLUME, std::to_wstring(volume) + L"%");
            if (wp == TB_ENDTRACK) g_volumeDragging = false;
            return TRUE;
        }
        if (control == GetDlgItem(hwnd, IDC_POSITION_SLIDER)) {
            if (!g_positionDragging) {
                g_positionDragging = true;
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
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BTN_OPEN:
            OpenMidi(hwnd);
            return TRUE;
        case IDC_BTN_BACKGROUND:
            OpenBackground(hwnd);
            return TRUE;
        case IDC_BTN_RENDER:
            OpenRenderDialog(hwnd);
            return TRUE;
        case IDC_ENABLE_AUDIO:
            if (HIWORD(wp) == BN_CLICKED) {
                if (IsAudioEnabled()) {
                    if (g_playing) {
                        // Keep the visualizer exactly where it is.  The previous
                        // implementation reset only the epoch, so elapsed time was
                        // measured from the old playback start and the song jumped
                        // backwards.  Rebase both values to the current position.
                        g_playbackBaseTime = g_visualizer.Time();
                        g_playbackEpoch = std::chrono::steady_clock::now();
                        StartAudioPlayback();
                    }
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
                UpdateTrackColorUi();
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_EFFECT_SMOKE:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_EFFECT_SMOKE), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetEffectEnabled(Visualizer::EffectSmokeGlow, checked);
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_EFFECT_SPARK:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_EFFECT_SPARK), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetEffectEnabled(Visualizer::EffectSpark, checked);
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_EFFECT_RIPPLE:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_EFFECT_RIPPLE), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetEffectEnabled(Visualizer::EffectRipple, checked);
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_EFFECT_DIAMOND_RIPPLE:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_EFFECT_DIAMOND_RIPPLE), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetEffectEnabled(Visualizer::EffectDiamondRipple, checked);
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_EFFECT_NOTE_GLOW:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_EFFECT_NOTE_GLOW), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetEffectEnabled(Visualizer::EffectNoteGlow, checked);
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_EFFECT_CROSS_SPARK:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_EFFECT_CROSS_SPARK), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetEffectEnabled(Visualizer::EffectCrossSpark, checked);
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDC_NOTE_GUIDES:
            if (HIWORD(wp) == BN_CLICKED) {
                const bool checked = SendMessageW(GetDlgItem(hwnd, IDC_NOTE_GUIDES), BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_visualizer.SetNoteGuideLinesEnabled(checked);
                InvalidateRect(g_visualizerHwnd, nullptr, FALSE);
                return TRUE;
            }
            break;
        }
        break;
    case WM_TIMER:
        if (wp == TIMER_CONTROL_REFRESH) {
            UpdateControlUi();
            return TRUE;
        }
        break;
    case WM_CLOSE:
        KillTimer(hwnd, TIMER_CONTROL_REFRESH);
        if (g_visualizerHwnd) PostMessageW(g_visualizerHwnd, WM_CLOSE, 0, 0);
        else DestroyWindow(hwnd);
        return TRUE;
    }
    return FALSE;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    g_instance = hInstance;

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        MessageBoxW(nullptr, L"Failed to initialize COM.", L"Piano Visualizer Classic", MB_ICONERROR);
        return 1;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    std::wstring audioError;
    g_simpleSynth.Initialize(audioError);
    g_simpleSynth.SetMasterVolume(0.75f);

    WNDCLASSW wc{};
    wc.hInstance = hInstance;
    wc.lpfnWndProc = VisualizerWndProc;
    wc.lpszClassName = L"PianoVisualizerClassic";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_PIANOVISUALIZER));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Piano Visualizer Classic", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 760,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        g_simpleSynth.Shutdown();
        if (SUCCEEDED(comHr) || comHr == S_FALSE) CoUninitialize();
        return 1;
    }

    g_visualizerHwnd = hwnd;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    g_controlHwnd = CreateDialogParamW(
        hInstance, MAKEINTRESOURCEW(IDD_CONTROL_DIALOG), nullptr, ControlDlgProc, 0);
    if (!g_controlHwnd) {
        MessageBoxW(hwnd, L"Failed to create the control dialog.", L"Piano Visualizer Classic", MB_ICONERROR);
        DestroyWindow(hwnd);
        g_simpleSynth.Shutdown();
        g_visualizer.Shutdown();
        if (SUCCEEDED(comHr) || comHr == S_FALSE) CoUninitialize();
        return 1;
    }

    ShowWindow(g_controlHwnd, SW_SHOW);
    UpdateWindow(g_controlHwnd);
    SetWindowPos(g_controlHwnd, HWND_TOP, 80, 80, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
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
        if (g_playing && !g_positionDragging && now >= nextFrame) {
            double elapsed = std::chrono::duration<double>(now - g_playbackEpoch).count();
            if (elapsed < 0.0) elapsed = 0.0;
            double nextTime = g_playbackBaseTime + elapsed;
            if (nextTime > g_visualizer.Duration()) nextTime = g_visualizer.Duration();

            if (g_visualizer.Duration() > 0.0 && nextTime >= g_visualizer.Duration()) {
                g_visualizer.SetTime(g_visualizer.Duration());
                g_playing = false;
                g_visualizer.SetPlaying(false);
                StopAudioPlayback();
                UpdateControlUi();
            } else {
                g_visualizer.SetTime(nextTime);
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
    g_simpleSynth.Shutdown();
    g_visualizer.Shutdown();
    g_song = MidiSong{};
    if (SUCCEEDED(comHr) || comHr == S_FALSE) CoUninitialize();
    return result;
}
