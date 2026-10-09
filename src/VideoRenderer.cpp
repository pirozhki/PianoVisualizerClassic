#include "VideoRenderer.h"

#include <windows.h>
#include <wincodec.h>
#include <d2d1.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "MidiFile.h"
#include "SimpleSynth.h"
#include "Visualizer.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace {

std::wstring QuoteArg(const std::wstring& value) {
    std::wstring result = L"\"" + value + L"\"";
    return result;
}

bool WriteAll(HANDLE handle, const void* data, DWORD bytes,
              const std::atomic<bool>* cancelRequested) {
    const auto* source = static_cast<const std::uint8_t*>(data);
    DWORD remaining = bytes;
    while (remaining > 0) {
        if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) return false;
        DWORD written = 0;
        if (!WriteFile(handle, source, remaining, &written, nullptr)) return false;
        if (written == 0) return false;
        source += written;
        remaining -= written;
    }
    return true;
}

bool CreateNullHandle(HANDLE& handle) {
    handle = CreateFileW(
        L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    return handle != INVALID_HANDLE_VALUE;
}

bool LaunchFfmpeg(const VideoRenderOptions& options,
                  std::uint64_t frameCount,
                  PROCESS_INFORMATION& pi,
                  HANDLE& pipeWrite,
                  std::wstring& error) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE pipeRead = nullptr;
    pipeWrite = nullptr;
    if (!CreatePipe(&pipeRead, &pipeWrite, &sa, 0)) {
        error = L"Failed to create the FFmpeg input pipe.";
        return false;
    }
    if (!SetHandleInformation(pipeWrite, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(pipeRead);
        CloseHandle(pipeWrite);
        pipeRead = nullptr;
        pipeWrite = nullptr;
        error = L"Failed to configure the FFmpeg input pipe.";
        return false;
    }

    HANDLE nullOut = INVALID_HANDLE_VALUE;
    if (!CreateNullHandle(nullOut)) {
        CloseHandle(pipeRead);
        CloseHandle(pipeWrite);
        pipeRead = nullptr;
        pipeWrite = nullptr;
        error = L"Failed to create the FFmpeg output handle.";
        return false;
    }

    std::wstring args = QuoteArg(options.ffmpegPath)
        + L" -hide_banner -loglevel error -y"
        + L" -f rawvideo -pix_fmt bgra"
        + L" -video_size " + std::to_wstring(options.width) + L"x" + std::to_wstring(options.height)
        + L" -framerate " + std::to_wstring(static_cast<int>(std::lround(options.fps)))
        + L" -i pipe:0";

    if (options.includeAudio) {
        args += L" -i " + QuoteArg(options.temporaryWavPath);
        args += L" -map 0:v:0 -map 1:a:0";
    } else {
        args += L" -map 0:v:0 -an";
    }

    args += L" -frames:v " + std::to_wstring(frameCount)
        + L" -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p"
        + L" -movflags +faststart";

    if (options.includeAudio) args += L" -c:a aac -b:a 192k -shortest";
    args += L" " + QuoteArg(options.outputPath);

    std::vector<wchar_t> mutableCommand(args.begin(), args.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = pipeRead;
    si.hStdOutput = nullOut;
    si.hStdError = nullOut;

    pi = PROCESS_INFORMATION{};
    const BOOL ok = CreateProcessW(
        options.ffmpegPath.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    CloseHandle(pipeRead);
    pipeRead = nullptr;
    CloseHandle(nullOut);

    if (!ok) {
        CloseHandle(pipeWrite);
        pipeWrite = nullptr;
        error = L"Failed to start FFmpeg.\nMake sure ffmpeg.exe is available.";
        return false;
    }
    return true;
}

} // namespace

bool VideoRenderer::FindFfmpeg(std::wstring& path) {
    wchar_t exePath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::filesystem::path candidate(exePath);
        candidate = candidate.parent_path() / L"ffmpeg.exe";
        if (std::filesystem::exists(candidate)) {
            path = candidate.wstring();
            return true;
        }
    }

    wchar_t found[MAX_PATH] = {};
    const DWORD result = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr);
    if (result > 0 && result < MAX_PATH) {
        path.assign(found, result);
        return true;
    }
    return false;
}

bool VideoRenderer::RenderMp4(const Visualizer& visualizer,
                              const MidiSong& song,
                              SimpleSynth* synth,
                              const VideoRenderOptions& options,
                              const ProgressCallback& progress,
                              std::wstring& error,
                              const std::atomic<bool>* cancelRequested) {
    if (song.duration <= 0.0 || song.notes.empty()) {
        error = L"The MIDI file contains no renderable notes.";
        return false;
    }
    if (options.width < 64 || options.height < 64 ||
        options.width > 16384 || options.height > 16384 ||
        (options.width & 1u) != 0u || (options.height & 1u) != 0u) {
        error = L"Video width and height must be even values between 64 and 16384.";
        return false;
    }
    if (options.fps < 1.0 || options.fps > 120.0) {
        error = L"The video frame rate must be between 1 and 120 FPS.";
        return false;
    }
    if (options.fallSpeed < 1.0f || options.fallSpeed > 5000.0f) {
        error = L"The note fall speed must be between 1 and 5000 px/s.";
        return false;
    }
    if (options.outputPath.empty()) {
        error = L"No output file was selected.";
        return false;
    }
    if (options.ffmpegPath.empty() || !std::filesystem::exists(options.ffmpegPath)) {
        error = L"ffmpeg.exe could not be found.";
        return false;
    }
    if (options.includeAudio && !synth) {
        error = L"The internal audio synthesizer is unavailable.";
        return false;
    }
    if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
        error = L"Rendering canceled.";
        return false;
    }

    const double renderDuration = song.duration + kTailSeconds;
    const std::uint64_t frameCount = static_cast<std::uint64_t>(
        std::ceil(renderDuration * options.fps));
    if (frameCount == 0 || frameCount > 10000000ULL) {
        error = L"The requested video is too long to render.";
        return false;
    }

    if (progress) progress(0, L"Preparing video renderer...");

    // The render worker owns this complete Visualizer instance. Its D2D factory,
    // WIC factory, render target, brushes, background bitmap, and effect state are
    // all created and destroyed on the same worker thread. The live visualizer is
    // never mutated and never shares device-dependent resources with this object.
    Visualizer renderVisualizer;
    if (!renderVisualizer.InitializeOffscreen(
            visualizer, options.width, options.height)) {
        error = L"Failed to initialize the worker-thread Direct2D/WIC renderer.";
        return false;
    }
    // Pass the render-time speed explicitly instead of relying only on the
    // copied live Visualizer state.  This guarantees that the MP4 uses exactly
    // the Fall Speed selected in the control dialog.
    // Fall speed is exposed in pixels/second, but the live window and the
    // rendered video can have different note-field heights. Scale the speed
    // by the render/live hit-line ratio so that the time it takes a note to
    // travel from the top of the field to the keyboard stays the same.
    const float liveFallDistance = std::max(1.0f, visualizer.hitY_);
    const float renderFallDistance = std::max(1.0f, renderVisualizer.hitY_);
    const float normalizedFallSpeed =
        options.fallSpeed * (renderFallDistance / liveFallDistance);
    renderVisualizer.SetFallSpeed(normalizedFallSpeed);

    PROCESS_INFORMATION pi{};
    HANDLE ffmpegPipe = nullptr;
    if (!LaunchFfmpeg(options, frameCount, pi, ffmpegPipe, error)) {
        renderVisualizer.Shutdown();
        return false;
    }

    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(options.width) *
        static_cast<std::size_t>(options.height) * 4u);
    const std::uint32_t stride = options.width * 4u;
    bool writeOk = true;
    bool canceled = false;

    for (std::uint64_t frame = 0; frame < frameCount; ++frame) {
        if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
            canceled = true;
            break;
        }

        const double time = static_cast<double>(frame) / options.fps;
        renderVisualizer.currentTime_ = std::min(time, renderDuration);
        renderVisualizer.UpdateActiveNotes();
        renderVisualizer.UpdateEffects();

        renderVisualizer.target_->BeginDraw();
        renderVisualizer.DrawBackground(renderVisualizer.width_, renderVisualizer.height_);
        // Keep White Particles behind notes in offline frames as in realtime.
        if (renderVisualizer.IsEffectEnabled(Visualizer::EffectAmbientParticles)) {
            renderVisualizer.DrawAmbientParticles(renderVisualizer.width_, renderVisualizer.height_);
        }
        renderVisualizer.DrawNoteGuides(renderVisualizer.width_, renderVisualizer.height_);
        renderVisualizer.DrawNotes(renderVisualizer.width_, renderVisualizer.height_);
        renderVisualizer.DrawKeyboard(renderVisualizer.width_, renderVisualizer.height_);
        renderVisualizer.DrawEffects(renderVisualizer.width_, renderVisualizer.height_);
        const HRESULT drawHr = renderVisualizer.target_->EndDraw();

        if (FAILED(drawHr)) {
            writeOk = false;
            error = L"Direct2D failed while rendering a video frame.";
            break;
        }

        WICRect rect{0, 0, static_cast<INT>(options.width), static_cast<INT>(options.height)};
        const HRESULT copyHr = renderVisualizer.offscreenBitmap_->CopyPixels(
            &rect, stride, static_cast<UINT>(pixels.size()), pixels.data());
        if (FAILED(copyHr)) {
            writeOk = false;
            error = L"Failed while copying the rendered video frame.";
            break;
        }

        if (!WriteAll(ffmpegPipe, pixels.data(), static_cast<DWORD>(pixels.size()), cancelRequested)) {
            if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
                canceled = true;
            } else {
                writeOk = false;
                error = L"Failed while sending video frames to FFmpeg.";
            }
            break;
        }

        if (progress && ((frame % 6u) == 0u || frame + 1 == frameCount)) {
            const int percent = static_cast<int>(((frame + 1) * 100ULL) / frameCount);
            progress(percent, L"Rendering video... " + std::to_wstring(percent) + L"%");
        }
    }

    CloseHandle(ffmpegPipe);
    ffmpegPipe = nullptr;

    if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) canceled = true;

    DWORD wait = WAIT_TIMEOUT;
    if (canceled) {
        wait = WaitForSingleObject(pi.hProcess, 100);
        if (wait == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 1);
            wait = WaitForSingleObject(pi.hProcess, 2000);
        }
    } else {
        while ((wait = WaitForSingleObject(pi.hProcess, 50)) == WAIT_TIMEOUT) {
            if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) {
                canceled = true;
                TerminateProcess(pi.hProcess, 1);
                wait = WaitForSingleObject(pi.hProcess, 2000);
                break;
            }
        }
    }

    DWORD exitCode = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    renderVisualizer.Shutdown();

    if (canceled) {
        error = L"Rendering canceled.";
        DeleteFileW(options.outputPath.c_str());
        return false;
    }
    if (writeOk && exitCode == 0) {
        if (progress) progress(100, L"Video rendering completed.");
        return true;
    }
    if (writeOk && exitCode != 0) {
        error = L"FFmpeg failed to encode the MP4 file.";
    }
    return false;
}
