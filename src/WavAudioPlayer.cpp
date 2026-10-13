#include "WavAudioPlayer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace {

constexpr std::uint64_t kMaxDecodedBytes = 256ull * 1024ull * 1024ull;

std::uint16_t ReadU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t ReadU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0])
        | (static_cast<std::uint32_t>(p[1]) << 8)
        | (static_cast<std::uint32_t>(p[2]) << 16)
        | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool FourCC(const std::uint8_t* p, const char* code) {
    return p[0] == static_cast<std::uint8_t>(code[0])
        && p[1] == static_cast<std::uint8_t>(code[1])
        && p[2] == static_cast<std::uint8_t>(code[2])
        && p[3] == static_cast<std::uint8_t>(code[3]);
}

float DecodeSample(const std::uint8_t* p, std::uint16_t format, std::uint16_t bits) {
    if (format == 3) { // IEEE floating point
        if (bits == 32) {
            float value = 0.0f;
            std::memcpy(&value, p, sizeof(value));
            return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
        }
        if (bits == 64) {
            double value = 0.0;
            std::memcpy(&value, p, sizeof(value));
            if (!std::isfinite(value)) return 0.0f;
            return static_cast<float>(std::clamp(value, -1.0, 1.0));
        }
        return 0.0f;
    }

    if (format != 1) return 0.0f; // PCM
    switch (bits) {
    case 8:
        return (static_cast<int>(p[0]) - 128) / 128.0f;
    case 16: {
        const auto raw = static_cast<std::int16_t>(ReadU16(p));
        return raw / 32768.0f;
    }
    case 24: {
        std::int32_t raw = static_cast<std::int32_t>(p[0])
            | (static_cast<std::int32_t>(p[1]) << 8)
            | (static_cast<std::int32_t>(p[2]) << 16);
        if ((raw & 0x00800000) != 0) raw |= static_cast<std::int32_t>(0xFF000000);
        return raw / 8388608.0f;
    }
    case 32: {
        const auto raw = static_cast<std::int32_t>(ReadU32(p));
        return static_cast<float>(static_cast<double>(raw) / 2147483648.0);
    }
    default:
        return 0.0f;
    }
}

std::int16_t EncodePcm16(float sample) {
    const float clamped = std::clamp(sample, -1.0f, 1.0f);
    const long value = std::lround(clamped * (clamped < 0.0f ? 32768.0f : 32767.0f));
    return static_cast<std::int16_t>(std::clamp<long>(value, -32768, 32767));
}

std::wstring FormatWavError(const std::wstring& path, const wchar_t* reason) {
    return L"Could not load WAV file:\n" + path + L"\n\n" + reason;
}

} // namespace

bool WavAudioPlayer::Initialize(std::wstring& error) {
    if (xaudio_ && masteringVoice_ && sourceVoice_) return true;

    HRESULT hr = XAudio2Create(xaudio_.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        error = L"Failed to initialize XAudio2 for WAV playback.";
        xaudio_.Reset();
        return false;
    }
    hr = xaudio_->CreateMasteringVoice(&masteringVoice_);
    if (FAILED(hr)) {
        error = L"Failed to create the WAV audio mastering voice.";
        xaudio_.Reset();
        masteringVoice_ = nullptr;
        return false;
    }
    if (!CreateSourceVoice(&sourceVoice_, error)) {
        masteringVoice_->DestroyVoice();
        masteringVoice_ = nullptr;
        xaudio_.Reset();
        return false;
    }
    sourceVoice_->SetVolume(masterVolume_.load(std::memory_order_acquire));
    return true;
}

bool WavAudioPlayer::CreateSourceVoice(IXAudio2SourceVoice** voice, std::wstring& error) {
    if (!xaudio_) {
        error = L"The WAV audio engine is not initialized.";
        return false;
    }
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    HRESULT hr = xaudio_->CreateSourceVoice(voice, &format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr, nullptr, nullptr);
    if (FAILED(hr)) {
        error = L"Failed to create the WAV audio source voice.";
        *voice = nullptr;
        return false;
    }
    return true;
}

void WavAudioPlayer::Shutdown() {
    Stop();
    if (sourceVoice_) {
        sourceVoice_->DestroyVoice();
        sourceVoice_ = nullptr;
    }
    if (masteringVoice_) {
        masteringVoice_->DestroyVoice();
        masteringVoice_ = nullptr;
    }
    xaudio_.Reset();
    samples_.clear();
    silenceSamples_.clear();
    path_.clear();
}

bool WavAudioPlayer::Load(const std::wstring& path, std::wstring& error) {
    std::vector<std::int16_t> decoded;
    try {
        if (!DecodeWav(path, decoded, error)) return false;
    } catch (const std::bad_alloc&) {
        error = FormatWavError(path, L"There is not enough memory to decode this WAV file.");
        return false;
    }

    if (!xaudio_ && !Initialize(error)) return false;

    IXAudio2SourceVoice* newVoice = nullptr;
    if (!CreateSourceVoice(&newVoice, error)) return false;

    Stop();
    if (sourceVoice_) sourceVoice_->DestroyVoice();
    sourceVoice_ = newVoice;
    samples_ = std::move(decoded);
    path_ = path;
    silenceSamples_.clear();
    sourceVoice_->SetVolume(masterVolume_.load(std::memory_order_acquire));
    return true;
}

void WavAudioPlayer::Unload() {
    Stop();
    if (sourceVoice_) {
        sourceVoice_->DestroyVoice();
        sourceVoice_ = nullptr;
        std::wstring ignoredError;
        (void)CreateSourceVoice(&sourceVoice_, ignoredError);
        if (sourceVoice_) sourceVoice_->SetVolume(masterVolume_.load(std::memory_order_acquire));
    }
    samples_.clear();
    samples_.shrink_to_fit();
    silenceSamples_.clear();
    silenceSamples_.shrink_to_fit();
    path_.clear();
}

double WavAudioPlayer::Duration() const {
    if (samples_.empty()) return 0.0;
    return static_cast<double>(samples_.size()) / (2.0 * kSampleRate);
}

void WavAudioPlayer::SetMasterVolume(float volume) {
    volume = std::clamp(volume, 0.0f, 1.0f);
    masterVolume_.store(volume, std::memory_order_release);
    if (sourceVoice_) sourceVoice_->SetVolume(volume);
}

void WavAudioPlayer::Stop() {
    if (!sourceVoice_) return;
    sourceVoice_->Stop(0);
    sourceVoice_->FlushSourceBuffers();
}

void WavAudioPlayer::Start(double midiBaseTime, int offsetMilliseconds) {
    Stop();
    if (!IsLoaded() || !sourceVoice_) return;

    const double offsetSeconds = std::clamp(offsetMilliseconds,
        kMinOffsetMilliseconds, kMaxOffsetMilliseconds) / 1000.0;
    const double sourceTime = std::max(0.0, midiBaseTime) - offsetSeconds;
    const double sourceDuration = Duration();
    if (sourceTime >= sourceDuration) return;

    const double delaySeconds = std::max(0.0, -sourceTime);
    const double clampedSourceTime = std::max(0.0, sourceTime);
    std::uint64_t startFrame = static_cast<std::uint64_t>(std::floor(clampedSourceTime * kSampleRate));
    const std::uint64_t totalFrames = samples_.size() / 2;
    if (startFrame >= totalFrames) return;

    if (delaySeconds > 0.0) {
        const std::uint64_t delayFrames = static_cast<std::uint64_t>(std::llround(delaySeconds * kSampleRate));
        if (delayFrames == 0 || delayFrames > std::numeric_limits<UINT32>::max() / 4u) return;
        silenceSamples_.assign(static_cast<size_t>(delayFrames) * 2u, 0);

        XAUDIO2_BUFFER silence{};
        silence.AudioBytes = static_cast<UINT32>(silenceSamples_.size() * sizeof(std::int16_t));
        silence.pAudioData = reinterpret_cast<const BYTE*>(silenceSamples_.data());
        if (FAILED(sourceVoice_->SubmitSourceBuffer(&silence))) return;
    } else {
        silenceSamples_.clear();
    }

    const std::uint64_t remainingSamples = (totalFrames - startFrame) * 2u;
    const std::uint64_t remainingBytes = remainingSamples * sizeof(std::int16_t);
    if (remainingBytes == 0 || remainingBytes > std::numeric_limits<UINT32>::max()) {
        sourceVoice_->FlushSourceBuffers();
        return;
    }
    XAUDIO2_BUFFER audio{};
    audio.Flags = XAUDIO2_END_OF_STREAM;
    audio.AudioBytes = static_cast<UINT32>(remainingBytes);
    audio.pAudioData = reinterpret_cast<const BYTE*>(samples_.data() + static_cast<size_t>(startFrame) * 2u);
    if (FAILED(sourceVoice_->SubmitSourceBuffer(&audio))) {
        sourceVoice_->FlushSourceBuffers();
        return;
    }
    sourceVoice_->SetVolume(masterVolume_.load(std::memory_order_acquire));
    sourceVoice_->Start(0);
}

bool WavAudioPlayer::DecodeWav(const std::wstring& path,
                               std::vector<std::int16_t>& output,
                               std::wstring& error) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) {
        error = FormatWavError(path, L"The file could not be opened.");
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileLength = file.tellg();
    file.seekg(0, std::ios::beg);
    if (fileLength < 12) {
        error = FormatWavError(path, L"The file is too small to be a valid RIFF/WAVE file.");
        return false;
    }

    std::uint8_t riff[12]{};
    file.read(reinterpret_cast<char*>(riff), sizeof(riff));
    if (!file || !FourCC(riff, "RIFF") || !FourCC(riff + 8, "WAVE")) {
        error = FormatWavError(path, L"The file is not a standard RIFF/WAVE file.");
        return false;
    }

    std::vector<std::uint8_t> fmt;
    std::vector<std::uint8_t> rawData;
    while (file) {
        const std::streamoff currentPosition = static_cast<std::streamoff>(file.tellg());
        if (currentPosition < 0 || currentPosition + 8 > fileLength) break;
        std::uint8_t chunkHeader[8]{};
        file.read(reinterpret_cast<char*>(chunkHeader), sizeof(chunkHeader));
        if (!file) break;
        const std::uint32_t chunkSize = ReadU32(chunkHeader + 4);
        const std::streamoff chunkStart = file.tellg();
        if (chunkStart < 0 || static_cast<std::uint64_t>(chunkSize) > static_cast<std::uint64_t>(fileLength - chunkStart)) {
            error = FormatWavError(path, L"A WAV chunk extends beyond the end of the file.");
            return false;
        }

        if (FourCC(chunkHeader, "fmt ")) {
            if (chunkSize < 16 || chunkSize > 1024u * 1024u) {
                error = FormatWavError(path, L"The WAV format chunk is invalid.");
                return false;
            }
            fmt.resize(chunkSize);
            file.read(reinterpret_cast<char*>(fmt.data()), static_cast<std::streamsize>(fmt.size()));
            if (!file) {
                error = FormatWavError(path, L"The WAV format chunk could not be read.");
                return false;
            }
        } else if (FourCC(chunkHeader, "data") && rawData.empty()) {
            if (chunkSize == 0 || chunkSize > kMaxDecodedBytes) {
                error = FormatWavError(path, L"The WAV audio data is empty or too large (maximum 256 MB before conversion).");
                return false;
            }
            rawData.resize(chunkSize);
            file.read(reinterpret_cast<char*>(rawData.data()), static_cast<std::streamsize>(rawData.size()));
            if (!file) {
                error = FormatWavError(path, L"The WAV audio data could not be read completely.");
                return false;
            }
        }

        file.clear();
        file.seekg(chunkStart + static_cast<std::streamoff>(chunkSize + (chunkSize & 1u)), std::ios::beg);
    }

    if (fmt.size() < 16 || rawData.empty()) {
        error = FormatWavError(path, L"The WAV file is missing a format or audio-data chunk.");
        return false;
    }

    std::uint16_t format = ReadU16(fmt.data());
    const std::uint16_t channels = ReadU16(fmt.data() + 2);
    const std::uint32_t sampleRate = ReadU32(fmt.data() + 4);
    const std::uint16_t blockAlign = ReadU16(fmt.data() + 12);
    const std::uint16_t bitsPerSample = ReadU16(fmt.data() + 14);
    if (format == 0xFFFE) { // WAVE_FORMAT_EXTENSIBLE; inspect the SubFormat GUID.
        if (fmt.size() < 40) {
            error = FormatWavError(path, L"The extensible WAV format chunk is incomplete.");
            return false;
        }
        const std::uint16_t subFormat = ReadU16(fmt.data() + 24);
        if (subFormat != 1 && subFormat != 3) {
            error = FormatWavError(path, L"This WAV uses a compressed or unsupported audio format.");
            return false;
        }
        format = subFormat;
    }

    const std::uint16_t bytesPerSample = static_cast<std::uint16_t>((bitsPerSample + 7u) / 8u);
    const bool validPcm = format == 1 && (bitsPerSample == 8 || bitsPerSample == 16
        || bitsPerSample == 24 || bitsPerSample == 32);
    const bool validFloat = format == 3 && (bitsPerSample == 32 || bitsPerSample == 64);
    if ((!validPcm && !validFloat) || channels == 0 || channels > 8
        || sampleRate < 1000 || sampleRate > 384000 || bytesPerSample == 0
        || blockAlign < channels * bytesPerSample) {
        error = FormatWavError(path, L"Supported formats are uncompressed PCM (8/16/24/32-bit) or IEEE float (32/64-bit), with 1–8 channels and a valid sample rate.");
        return false;
    }

    const std::uint64_t sourceFrames = rawData.size() / blockAlign;
    if (sourceFrames == 0) {
        error = FormatWavError(path, L"The WAV file contains no complete audio frames.");
        return false;
    }
    const long double desiredFramesValue = static_cast<long double>(sourceFrames) * kSampleRate / sampleRate;
    if (desiredFramesValue < 1.0L || desiredFramesValue * 4.0L > kMaxDecodedBytes) {
        error = FormatWavError(path, L"The converted audio would be empty or exceed the 256 MB memory limit.");
        return false;
    }
    const std::uint64_t destinationFrames = static_cast<std::uint64_t>(std::llround(desiredFramesValue));
    if (destinationFrames == 0 || destinationFrames * 4u > kMaxDecodedBytes) {
        error = FormatWavError(path, L"The converted audio would exceed the 256 MB memory limit.");
        return false;
    }

    auto getStereoFrame = [&](std::uint64_t frame, float& left, float& right) {
        frame = std::min(frame, sourceFrames - 1);
        const std::uint8_t* frameData = rawData.data() + static_cast<size_t>(frame) * blockAlign;
        auto channel = [&](std::uint16_t index) {
            return DecodeSample(frameData + static_cast<size_t>(index) * bytesPerSample, format, bitsPerSample);
        };
        if (channels == 1) {
            left = right = channel(0);
            return;
        }
        left = channel(0);
        right = channel(1);
        if (channels >= 3) {
            const float center = channel(2) * 0.70710678f;
            left += center;
            right += center;
        }
        if (channels >= 5) left += channel(4) * 0.5f;
        if (channels >= 6) right += channel(5) * 0.5f;
        if (channels >= 7) left += channel(6) * 0.5f;
        if (channels >= 8) right += channel(7) * 0.5f;
        left = std::clamp(left, -1.0f, 1.0f);
        right = std::clamp(right, -1.0f, 1.0f);
    };

    output.resize(static_cast<size_t>(destinationFrames) * 2u);
    for (std::uint64_t i = 0; i < destinationFrames; ++i) {
        const double sourcePosition = static_cast<double>(i) * sampleRate / kSampleRate;
        const std::uint64_t first = std::min<std::uint64_t>(static_cast<std::uint64_t>(sourcePosition), sourceFrames - 1);
        const std::uint64_t second = std::min(first + 1, sourceFrames - 1);
        const float fraction = static_cast<float>(sourcePosition - static_cast<double>(first));
        float l0 = 0.0f, r0 = 0.0f, l1 = 0.0f, r1 = 0.0f;
        getStereoFrame(first, l0, r0);
        getStereoFrame(second, l1, r1);
        output[static_cast<size_t>(i) * 2u] = EncodePcm16(l0 + (l1 - l0) * fraction);
        output[static_cast<size_t>(i) * 2u + 1u] = EncodePcm16(r0 + (r1 - r0) * fraction);
    }

    return true;
}
