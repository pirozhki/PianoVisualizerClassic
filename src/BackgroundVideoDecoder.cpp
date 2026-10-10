#define NOMINMAX
#include "BackgroundVideoDecoder.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <utility>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr double kHnsPerSecond = 10000000.0;
// Small time changes are efficiently handled by continuing to read sequentially.
// Larger forward jumps (and every backward jump) should use Media Foundation's
// indexed seek instead of decoding every intervening frame.
constexpr double kForwardSeekThresholdSeconds = 0.45;
constexpr double kTimeComparisonEpsilonSeconds = 0.0001;

enum class SampleReadResult { Sample, EndOfStream, Error };

struct VideoFrame {
    std::vector<std::uint8_t> pixels;
    LONGLONG timeHns = 0;
    bool valid = false;
};

std::wstring HResultMessage(const wchar_t* prefix, HRESULT hr) {
    wchar_t buffer[64]{};
    swprintf_s(buffer, L" (HRESULT 0x%08X)", static_cast<unsigned int>(hr));
    return std::wstring(prefix) + buffer;
}

} // namespace

BackgroundVideoDecoder::~BackgroundVideoDecoder() {
    Close();
}

bool BackgroundVideoDecoder::Open(const std::wstring& path, std::wstring& error) {
    Close();
    if (path.empty()) {
        error = L"No video file was selected.";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = path;
        error_.clear();
        stopRequested_ = false;
        initializationComplete_ = false;
        initializationSucceeded_ = false;
        workerFailed_ = false;
        frameReady_ = false;
        width_ = height_ = 0;
        durationHns_ = 0;
        requestedTime_ = 0.0;
        requestSerial_ = completedSerial_ = 0;
        publishedFrameVersion_ = 0;
        publishedFrameTimeHns_ = -1;
        publishedFrame_.clear();
    }

    try {
        worker_ = std::thread(&BackgroundVideoDecoder::WorkerMain, this);
    } catch (...) {
        error = L"Failed to start the video decoder thread.";
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&] { return initializationComplete_; });
        if (!initializationSucceeded_) {
            error = error_.empty() ? L"Media Foundation could not open this video." : error_;
            lock.unlock();
            Close();
            return false;
        }
    }
    return true;
}

void BackgroundVideoDecoder::Close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();

    std::lock_guard<std::mutex> lock(mutex_);
    path_.clear();
    error_.clear();
    stopRequested_ = false;
    initializationComplete_ = false;
    initializationSucceeded_ = false;
    workerFailed_ = false;
    frameReady_ = false;
    width_ = height_ = 0;
    durationHns_ = 0;
    requestedTime_ = 0.0;
    requestSerial_ = completedSerial_ = 0;
    publishedFrameVersion_ = 0;
    publishedFrameTimeHns_ = -1;
    publishedFrame_.clear();
}

bool BackgroundVideoDecoder::GetFrameAtTime(
    double seconds,
    bool waitForFrame,
    std::vector<std::uint8_t>& pixels,
    UINT& width,
    UINT& height,
    std::uint64_t& frameVersion,
    bool& changed,
    std::wstring& error) {
    changed = false;
    std::unique_lock<std::mutex> lock(mutex_);
    if (!initializationSucceeded_ || workerFailed_ || stopRequested_) {
        error = error_.empty() ? L"The video decoder is not available." : error_;
        return false;
    }

    requestedTime_ = std::max(0.0, seconds);
    const std::uint64_t serial = ++requestSerial_;
    condition_.notify_all();

    if (waitForFrame) {
        condition_.wait(lock, [&] {
            return stopRequested_ || workerFailed_ || completedSerial_ >= serial;
        });
        if (workerFailed_ || stopRequested_) {
            error = error_.empty() ? L"Video frame decoding failed." : error_;
            return false;
        }
    }

    width = width_;
    height = height_;
    if (frameReady_ && publishedFrameVersion_ != frameVersion) {
        pixels = publishedFrame_;
        frameVersion = publishedFrameVersion_;
        changed = true;
    }
    return true;
}

void BackgroundVideoDecoder::WorkerMain() {
    HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(comResult);
    HRESULT hr = FAILED(comResult) && comResult != RPC_E_CHANGED_MODE
        ? comResult : MFStartup(MF_VERSION, MFSTARTUP_FULL);
    bool mfStarted = SUCCEEDED(hr);
    ComPtr<IMFSourceReader> reader;
    UINT videoWidth = 0;
    UINT videoHeight = 0;
    LONG stride = 0;
    LONGLONG durationHns = 0;
    std::wstring openError;

    if (SUCCEEDED(hr)) {
        std::wstring path;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            path = path_;
        }
        ComPtr<IMFAttributes> readerAttributes;
        hr = MFCreateAttributes(readerAttributes.ReleaseAndGetAddressOf(), 1);
        if (SUCCEEDED(hr)) {
            // RGB32 requires color-space conversion for the common YUV formats.
            // Advanced processing supports this conversion and can use a hardware
            // video processor when Windows has one available.
            hr = readerAttributes->SetUINT32(
                MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        }
        if (SUCCEEDED(hr)) {
            hr = MFCreateSourceReaderFromURL(
                path.c_str(), readerAttributes.Get(), reader.ReleaseAndGetAddressOf());
        }
        if (FAILED(hr)) openError = HResultMessage(L"Media Foundation could not open or configure the video file", hr);
    } else {
        openError = HResultMessage(L"Media Foundation initialization failed", hr);
    }

    if (SUCCEEDED(hr)) {
        hr = reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (SUCCEEDED(hr)) {
            hr = reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        }
        if (FAILED(hr)) openError = HResultMessage(L"Could not select the video stream", hr);
    }

    ComPtr<IMFMediaType> outputType;
    if (SUCCEEDED(hr)) {
        hr = MFCreateMediaType(outputType.ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr)) hr = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(hr)) {
            hr = reader->SetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outputType.Get());
        }
        if (FAILED(hr)) openError = HResultMessage(L"The video format could not be decoded", hr);
    }

    ComPtr<IMFMediaType> currentType;
    if (SUCCEEDED(hr)) {
        hr = reader->GetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, currentType.ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr)) {
            hr = MFGetAttributeSize(currentType.Get(), MF_MT_FRAME_SIZE,
                                    &videoWidth, &videoHeight);
        }
        if (FAILED(hr) || videoWidth == 0 || videoHeight == 0) {
            if (SUCCEEDED(hr)) hr = E_FAIL;
            openError = HResultMessage(L"Could not read the video dimensions", hr);
        }
    }

    if (SUCCEEDED(hr)) {
        UINT32 strideAttribute = 0;
        if (SUCCEEDED(currentType->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideAttribute))) {
            stride = static_cast<LONG>(strideAttribute);
        } else {
            stride = static_cast<LONG>(videoWidth * 4u);
        }
        if (stride == 0) stride = static_cast<LONG>(videoWidth * 4u);
        const LONGLONG absStride = std::llabs(static_cast<long long>(stride));
        if (absStride < static_cast<LONGLONG>(videoWidth) * 4LL) {
            stride = static_cast<LONG>(videoWidth * 4u);
        }

        PROPVARIANT duration;
        PropVariantInit(&duration);
        if (SUCCEEDED(reader->GetPresentationAttribute(
                MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration))) {
            if (duration.vt == VT_UI8) durationHns = static_cast<LONGLONG>(duration.uhVal.QuadPart);
            else if (duration.vt == VT_I8) durationHns = duration.hVal.QuadPart;
        }
        PropVariantClear(&duration);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        initializationComplete_ = true;
        initializationSucceeded_ = SUCCEEDED(hr);
        if (FAILED(hr)) {
            error_ = openError.empty() ? HResultMessage(L"Failed to initialize video decoding", hr) : openError;
        } else {
            width_ = videoWidth;
            height_ = videoHeight;
            durationHns_ = durationHns;
        }
    }
    condition_.notify_all();

    if (FAILED(hr)) {
        currentType.Reset();
        outputType.Reset();
        reader.Reset();
        if (mfStarted) MFShutdown();
        if (comInitialized) CoUninitialize();
        return;
    }

    const LONG absStride = static_cast<LONG>(std::llabs(static_cast<long long>(stride)));
    const std::size_t rowBytes = static_cast<std::size_t>(videoWidth) * 4u;
    const std::size_t requiredBytes = static_cast<std::size_t>(absStride) * videoHeight;

    auto shouldStop = [&]() {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopRequested_;
    };

    auto readNextFrame = [&](VideoFrame& output, std::wstring& readError) -> SampleReadResult {
        for (int attempt = 0; attempt < 64; ++attempt) {
            if (shouldStop()) return SampleReadResult::EndOfStream;
            DWORD streamIndex = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            HRESULT readHr = reader->ReadSample(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags,
                &timestamp, sample.ReleaseAndGetAddressOf());
            if (FAILED(readHr)) {
                readError = HResultMessage(L"Failed to read a video frame", readHr);
                return SampleReadResult::Error;
            }
            // Some sources may return the final sample together with the
            // end-of-stream flag; consume that sample before treating the next
            // empty read as EOF.
            if (!sample) {
                if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                    return SampleReadResult::EndOfStream;
                }
                continue;
            }

            ComPtr<IMFMediaBuffer> buffer;
            readHr = sample->ConvertToContiguousBuffer(buffer.ReleaseAndGetAddressOf());
            if (FAILED(readHr)) {
                readError = HResultMessage(L"Failed to access a decoded video frame", readHr);
                return SampleReadResult::Error;
            }

            BYTE* source = nullptr;
            DWORD maxLength = 0;
            DWORD currentLength = 0;
            readHr = buffer->Lock(&source, &maxLength, &currentLength);
            if (FAILED(readHr)) {
                readError = HResultMessage(L"Failed to lock a decoded video frame", readHr);
                return SampleReadResult::Error;
            }
            if (currentLength < requiredBytes) {
                buffer->Unlock();
                readError = L"The video decoder returned an incomplete frame.";
                return SampleReadResult::Error;
            }

            output.pixels.resize(rowBytes * videoHeight);
            for (UINT y = 0; y < videoHeight; ++y) {
                const UINT sourceY = stride < 0 ? (videoHeight - 1u - y) : y;
                const BYTE* sourceRow = source + static_cast<std::size_t>(sourceY) * absStride;
                BYTE* destinationRow = output.pixels.data() + static_cast<std::size_t>(y) * rowBytes;
                std::memcpy(destinationRow, sourceRow, rowBytes);
                // MFVideoFormat_RGB32 stores BGRX. Make the unused X channel
                // opaque so Direct2D can consume it as premultiplied BGRA.
                for (UINT x = 0; x < videoWidth; ++x) destinationRow[static_cast<std::size_t>(x) * 4u + 3u] = 255;
            }
            buffer->Unlock();
            output.timeHns = timestamp;
            output.valid = true;
            return SampleReadResult::Sample;
        }
        readError = L"The video decoder did not produce a frame.";
        return SampleReadResult::Error;
    };

    bool endOfStreamReached = false;

    auto seekTo = [&](double targetSeconds, VideoFrame& current, VideoFrame& pending,
                      std::wstring& seekError) -> bool {
        PROPVARIANT position;
        PropVariantInit(&position);
        position.vt = VT_I8;
        double clamped = std::max(0.0, targetSeconds);
        if (durationHns > 0) {
            const double durationSeconds = static_cast<double>(durationHns) / kHnsPerSecond;
            if (clamped >= durationSeconds - kTimeComparisonEpsilonSeconds) {
                // Seeking exactly to duration can land after the final sample.
                // Start a little before EOF, then decode forward to the actual
                // last frame while the requested target remains at duration.
                clamped = std::max(0.0, durationSeconds - 0.5);
            }
        }
        position.hVal.QuadPart = static_cast<LONGLONG>(clamped * kHnsPerSecond);
        const HRESULT seekHr = reader->SetCurrentPosition(GUID_NULL, position);
        PropVariantClear(&position);
        if (FAILED(seekHr)) {
            seekError = HResultMessage(L"Could not seek within the background video", seekHr);
            return false;
        }
        current = VideoFrame{};
        pending = VideoFrame{};
        endOfStreamReached = false;
        return true;
    };

    auto normalizeTime = [&](double seconds) {
        seconds = std::max(0.0, seconds);
        if (durationHns > 0) {
            const double durationSeconds = static_cast<double>(durationHns) / kHnsPerSecond;
            if (durationSeconds > 0.0) {
                // Do not loop a short video. Clamp the requested timestamp to
                // its end and keep publishing the final decoded frame for any
                // subsequent MIDI/render timestamps.
                seconds = std::min(seconds, durationSeconds);
            }
        }
        return seconds;
    };

    VideoFrame currentFrame;
    VideoFrame pendingFrame;
    double lastTargetSeconds = -1.0;

    while (!shouldStop()) {
        double targetSeconds = 0.0;
        std::uint64_t activeSerial = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [&] {
                return stopRequested_ || requestSerial_ > completedSerial_;
            });
            if (stopRequested_) break;
            activeSerial = requestSerial_; // Coalesce stale preview requests.
            targetSeconds = normalizeTime(requestedTime_);
        }

        std::wstring decodeError;
        bool decodeOk = true;
        if (lastTargetSeconds >= 0.0) {
            const double targetDelta = targetSeconds - lastTargetSeconds;
            const bool movedBackward = targetDelta < -kTimeComparisonEpsilonSeconds;
            const bool jumpedForward = targetDelta >= kForwardSeekThresholdSeconds;
            if (movedBackward || jumpedForward) {
                decodeOk = seekTo(targetSeconds, currentFrame, pendingFrame, decodeError);
            }
        }

        while (decodeOk && !shouldStop()) {
            // If preview time advanced while decoding, use the newest requested
            // target rather than allowing a queue of stale frames to accumulate.
            bool hasNewRequest = false;
            double latestTargetSeconds = targetSeconds;
            {
                // Never hold mutex_ while seeking/decoding. SetCurrentPosition
                // may block, and keeping the lock here would prevent the UI
                // from submitting a newer scrub position for coalescing.
                std::lock_guard<std::mutex> lock(mutex_);
                if (requestSerial_ > activeSerial) {
                    activeSerial = requestSerial_;
                    latestTargetSeconds = normalizeTime(requestedTime_);
                    hasNewRequest = true;
                }
            }
            if (hasNewRequest) {
                const double targetDelta = latestTargetSeconds - targetSeconds;
                const bool movedBackward = targetDelta < -kTimeComparisonEpsilonSeconds;
                const bool jumpedForward = targetDelta >= kForwardSeekThresholdSeconds;
                if ((movedBackward || jumpedForward) &&
                    !seekTo(latestTargetSeconds, currentFrame, pendingFrame, decodeError)) {
                    decodeOk = false;
                    break;
                }
                targetSeconds = latestTargetSeconds;
            }

            if (!currentFrame.valid) {
                VideoFrame next;
                if (pendingFrame.valid) {
                    next = std::move(pendingFrame);
                    pendingFrame = VideoFrame{};
                } else {
                    if (endOfStreamReached) break;
                    const auto result = readNextFrame(next, decodeError);
                    if (result == SampleReadResult::Error) { decodeOk = false; break; }
                    if (result == SampleReadResult::EndOfStream) {
                        endOfStreamReached = true;
                        break;
                    }
                }
                currentFrame = std::move(next);
                if (currentFrame.valid &&
                    static_cast<double>(currentFrame.timeHns) / kHnsPerSecond > targetSeconds) {
                    break;
                }
                continue;
            }

            if (pendingFrame.valid) {
                if (static_cast<double>(pendingFrame.timeHns) / kHnsPerSecond <= targetSeconds) {
                    currentFrame = std::move(pendingFrame);
                    pendingFrame = VideoFrame{};
                    continue;
                }
                break;
            }

            if (endOfStreamReached) break;
            VideoFrame next;
            const auto result = readNextFrame(next, decodeError);
            if (result == SampleReadResult::Error) { decodeOk = false; break; }
            if (result == SampleReadResult::EndOfStream) {
                endOfStreamReached = true;
                break;
            }
            if (static_cast<double>(next.timeHns) / kHnsPerSecond <= targetSeconds) {
                currentFrame = std::move(next);
                continue;
            }
            pendingFrame = std::move(next);
            break;
        }

        if (!decodeOk) {
            std::lock_guard<std::mutex> lock(mutex_);
            workerFailed_ = true;
            error_ = decodeError.empty() ? L"Video frame decoding failed." : decodeError;
            completedSerial_ = requestSerial_;
            condition_.notify_all();
            break;
        }

        if (currentFrame.valid) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!frameReady_ || publishedFrameTimeHns_ != currentFrame.timeHns) {
                publishedFrame_ = currentFrame.pixels;
                publishedFrameTimeHns_ = currentFrame.timeHns;
                ++publishedFrameVersion_;
                frameReady_ = true;
            }
            completedSerial_ = std::max(completedSerial_, activeSerial);
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            completedSerial_ = std::max(completedSerial_, activeSerial);
        }
        lastTargetSeconds = targetSeconds;
        condition_.notify_all();
    }

    currentType.Reset();
    outputType.Reset();
    reader.Reset();
    if (mfStarted) MFShutdown();
    if (comInitialized) CoUninitialize();
}
