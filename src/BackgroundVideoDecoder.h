#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Decodes video frames on a dedicated worker thread. The consumer can either
// request a frame asynchronously (realtime preview) or wait for the requested
// timestamp (deterministic offline rendering).
class BackgroundVideoDecoder {
public:
    BackgroundVideoDecoder() = default;
    ~BackgroundVideoDecoder();

    BackgroundVideoDecoder(const BackgroundVideoDecoder&) = delete;
    BackgroundVideoDecoder& operator=(const BackgroundVideoDecoder&) = delete;

    bool Open(const std::wstring& path, std::wstring& error);
    void Close();

    bool GetFrameAtTime(double seconds,
                        bool waitForFrame,
                        std::vector<std::uint8_t>& pixels,
                        UINT& width,
                        UINT& height,
                        std::uint64_t& frameVersion,
                        bool& changed,
                        std::wstring& error);

private:
    void WorkerMain();

    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::wstring path_;
    std::wstring error_;
    bool stopRequested_ = false;
    bool initializationComplete_ = false;
    bool initializationSucceeded_ = false;
    bool workerFailed_ = false;
    bool frameReady_ = false;
    UINT width_ = 0;
    UINT height_ = 0;
    LONGLONG durationHns_ = 0;
    double requestedTime_ = 0.0;
    std::uint64_t requestSerial_ = 0;
    std::uint64_t completedSerial_ = 0;
    std::uint64_t publishedFrameVersion_ = 0;
    LONGLONG publishedFrameTimeHns_ = -1;
    std::vector<std::uint8_t> publishedFrame_;
};
