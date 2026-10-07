#include "MidiFile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& data) : data_(data) {}

    std::size_t pos() const noexcept { return pos_; }
    std::size_t remaining() const noexcept { return data_.size() - pos_; }

    std::uint8_t u8() {
        if (pos_ >= data_.size()) throw std::runtime_error("Unexpected end of file");
        return data_[pos_++];
    }

    std::uint16_t be16() {
        return static_cast<std::uint16_t>(static_cast<std::uint16_t>(u8()) << 8) | u8();
    }

    std::uint32_t be32() {
        return (static_cast<std::uint32_t>(u8()) << 24) |
               (static_cast<std::uint32_t>(u8()) << 16) |
               (static_cast<std::uint32_t>(u8()) << 8) |
               static_cast<std::uint32_t>(u8());
    }

    std::uint32_t vlq() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const std::uint8_t byte = u8();
            value = (value << 7) | (byte & 0x7fu);
            if ((byte & 0x80u) == 0) return value;
        }
        throw std::runtime_error("Invalid MIDI variable-length quantity");
    }

    std::string ascii(std::size_t count) {
        if (remaining() < count) throw std::runtime_error("Unexpected end of file");
        std::string result(reinterpret_cast<const char*>(data_.data() + pos_), count);
        pos_ += count;
        return result;
    }

    void skip(std::size_t count) {
        if (remaining() < count) throw std::runtime_error("Unexpected end of file");
        pos_ += count;
    }

private:
    const std::vector<std::uint8_t>& data_;
    std::size_t pos_ = 0;
};

struct RawEvent {
    std::uint64_t tick = 0;
    std::uint8_t status = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
    bool hasData2 = false;
    bool tempo = false;
    std::uint32_t tempoUsPerQuarter = 500000;
    bool trackName = false;
    std::string name;
};

struct RawTrack {
    std::vector<RawEvent> events;
    std::uint64_t endTick = 0;
    std::string name;
};

struct NoteTickInterval {
    int note = 0;
    int channel = 0;
    int track = 0;
    int velocity = 100;
    std::uint64_t startTick = 0;
    std::uint64_t endTick = 0;
};

std::vector<std::uint8_t> ReadFile(const std::wstring& path) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) throw std::runtime_error("Could not open MIDI file");

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0) throw std::runtime_error("Could not determine MIDI file size");
    file.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) throw std::runtime_error("Failed to read MIDI file");
    }
    return bytes;
}

void ParseTrack(const std::vector<std::uint8_t>& bytes, RawTrack& out) {
    Reader reader(bytes);
    std::uint64_t tick = 0;
    std::uint8_t runningStatus = 0;

    while (reader.remaining() > 0) {
        tick += reader.vlq();

        std::uint8_t status = reader.u8();
        if (status < 0x80u) {
            if (runningStatus < 0x80u || runningStatus >= 0xF0u) {
                throw std::runtime_error("Running status without a previous channel status");
            }
            const std::uint8_t data1 = status;
            status = runningStatus;

            const int type = status & 0xF0;
            if (type == 0xC0 || type == 0xD0) {
                out.events.push_back({tick, status, data1, 0, false, false, 0, false, {}});
            } else {
                const std::uint8_t data2 = reader.u8();
                out.events.push_back({tick, status, data1, data2, true, false, 0, false, {}});
            }
            continue;
        }

        if (status == 0xFFu) {
            const std::uint8_t metaType = reader.u8();
            const std::uint32_t length = reader.vlq();

            if (metaType == 0x51u && length == 3u) {
                const std::uint32_t tempo =
                    (static_cast<std::uint32_t>(reader.u8()) << 16) |
                    (static_cast<std::uint32_t>(reader.u8()) << 8) |
                    static_cast<std::uint32_t>(reader.u8());
                out.events.push_back({tick, 0, 0, 0, false, true, tempo, false, {}});
            } else if (metaType == 0x03u) {
                std::string name;
                name.reserve(length);
                for (std::uint32_t i = 0; i < length; ++i) name.push_back(static_cast<char>(reader.u8()));
                out.name = name;
                out.events.push_back({tick, 0, 0, 0, false, false, 0, true, name});
            } else {
                reader.skip(length);
            }

            runningStatus = 0;
            if (metaType == 0x2Fu) break;
            continue;
        }

        if (status == 0xF0u || status == 0xF7u) {
            reader.skip(reader.vlq());
            runningStatus = 0;
            continue;
        }

        if (status >= 0xF0u) {
            // System common messages are not needed by the visualizer.
            switch (status) {
            case 0xF1u:
            case 0xF3u:
                reader.skip(1);
                break;
            case 0xF2u:
                reader.skip(2);
                break;
            case 0xF6u:
                break;
            default:
                // Real-time bytes and undefined system messages do not carry the
                // channel-note information used by this application.
                break;
            }
            runningStatus = 0;
            continue;
        }

        runningStatus = status;
        const int type = status & 0xF0;
        const std::uint8_t data1 = reader.u8();
        if (type == 0xC0 || type == 0xD0) {
            out.events.push_back({tick, status, data1, 0, false, false, 0, false, {}});
        } else {
            const std::uint8_t data2 = reader.u8();
            out.events.push_back({tick, status, data1, data2, true, false, 0, false, {}});
        }
    }

    out.endTick = tick;
}

std::vector<std::pair<std::uint64_t, std::uint32_t>> BuildTempoMap(const std::vector<RawTrack>& tracks) {
    std::vector<std::pair<std::uint64_t, std::uint32_t>> tempos;
    tempos.emplace_back(0, 500000);

    for (const auto& track : tracks) {
        for (const auto& event : track.events) {
            if (event.tempo && event.tempoUsPerQuarter != 0) {
                tempos.emplace_back(event.tick, event.tempoUsPerQuarter);
            }
        }
    }

    std::sort(tempos.begin(), tempos.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        return a.second < b.second;
    });

    std::vector<std::pair<std::uint64_t, std::uint32_t>> merged;
    merged.reserve(tempos.size());
    for (const auto& tempo : tempos) {
        if (!merged.empty() && merged.back().first == tempo.first) {
            merged.back().second = tempo.second;
        } else {
            merged.push_back(tempo);
        }
    }
    return merged;
}

double TickToSecondsPPQ(
    std::uint64_t tick,
    int ppq,
    const std::vector<std::pair<std::uint64_t, std::uint32_t>>& tempos) {

    if (ppq <= 0) return 0.0;

    double seconds = 0.0;
    std::uint64_t previousTick = 0;
    std::uint32_t tempo = 500000;

    for (const auto& segment : tempos) {
        if (segment.first >= tick) break;
        if (segment.first > previousTick) {
            seconds += static_cast<double>(segment.first - previousTick) *
                       static_cast<double>(tempo) / 1000000.0 /
                       static_cast<double>(ppq);
            previousTick = segment.first;
        }
        tempo = segment.second;
    }

    if (tick > previousTick) {
        seconds += static_cast<double>(tick - previousTick) *
                   static_cast<double>(tempo) / 1000000.0 /
                   static_cast<double>(ppq);
    }
    return seconds;
}

double TickToSeconds(
    std::uint64_t tick,
    const MidiSong& song,
    const std::vector<std::pair<std::uint64_t, std::uint32_t>>& tempos) {

    if (song.smpte) {
        return song.smpteTicksPerSecond > 0.0
            ? static_cast<double>(tick) / song.smpteTicksPerSecond
            : 0.0;
    }
    return TickToSecondsPPQ(tick, song.ticksPerQuarter, tempos);
}

void ExtractNoteIntervals(
    const std::vector<RawTrack>& tracks,
    const MidiSong& song,
    const std::vector<std::pair<std::uint64_t, std::uint32_t>>& tempos,
    std::vector<MidiNote>& notes) {

    for (int trackIndex = 0; trackIndex < static_cast<int>(tracks.size()); ++trackIndex) {
        const RawTrack& track = tracks[static_cast<std::size_t>(trackIndex)];
        std::array<std::vector<std::pair<std::uint64_t, int>>, 16 * 128> active{};

        for (const auto& event : track.events) {
            const int type = event.status & 0xF0;
            if (type != 0x80 && type != 0x90) continue;

            const int channel = event.status & 0x0F;
            const int note = event.data1;
            if (note < 0 || note > 127) continue;

            const bool noteOn = type == 0x90 && event.data2 != 0;
            auto& stack = active[static_cast<std::size_t>(channel * 128 + note)];

            if (noteOn) {
                stack.emplace_back(event.tick, event.data2);
                continue;
            }

            if (stack.empty()) continue;
            const auto [startTick, velocity] = stack.back();
            stack.pop_back();

            MidiNote result;
            result.note = note;
            result.channel = channel;
            result.track = trackIndex;
            result.velocity = velocity;
            result.start = TickToSeconds(startTick, song, tempos);
            result.end = TickToSeconds(event.tick, song, tempos);
            notes.push_back(result);
        }

        // Gracefully terminate notes that were left on at the end of the track.
        for (int channel = 0; channel < 16; ++channel) {
            for (int note = 0; note < 128; ++note) {
                auto& stack = active[static_cast<std::size_t>(channel * 128 + note)];
                while (!stack.empty()) {
                    const auto [startTick, velocity] = stack.back();
                    stack.pop_back();

                    MidiNote result;
                    result.note = note;
                    result.channel = channel;
                    result.track = trackIndex;
                    result.velocity = velocity;
                    result.start = TickToSeconds(startTick, song, tempos);
                    result.end = TickToSeconds(track.endTick, song, tempos);
                    notes.push_back(result);
                }
            }
        }
    }
}

} // namespace

bool LoadMidiFile(const std::wstring& path, MidiSong& outSong, std::string& error) {
    try {
        const auto bytes = ReadFile(path);
        Reader reader(bytes);

        if (reader.ascii(4) != "MThd") {
            throw std::runtime_error("Not a Standard MIDI File (missing MThd)");
        }

        const std::uint32_t headerLength = reader.be32();
        if (headerLength < 6) throw std::runtime_error("Invalid MIDI header");

        const int format = reader.be16();
        const int trackCount = reader.be16();
        const std::uint16_t division = reader.be16();
        if (headerLength > 6) reader.skip(headerLength - 6);

        if (format < 0 || format > 2) throw std::runtime_error("Unsupported MIDI format");
        if (trackCount <= 0) throw std::runtime_error("MIDI contains no tracks");

        MidiSong song;
        song.format = format;
        song.smpte = (division & 0x8000u) != 0;

        if (song.smpte) {
            const std::int8_t fpsCode = static_cast<std::int8_t>((division >> 8) & 0xFFu);
            const int ticksPerFrame = division & 0xFFu;
            const int fps = (fpsCode == -29) ? 29 : -fpsCode;
            if (fps <= 0 || ticksPerFrame <= 0) {
                throw std::runtime_error("Invalid SMPTE division");
            }
            song.smpteTicksPerSecond = static_cast<double>(fps) * ticksPerFrame;
        } else {
            song.ticksPerQuarter = division;
            if (song.ticksPerQuarter <= 0) throw std::runtime_error("Invalid PPQ division");
        }

        std::vector<RawTrack> tracks;
        tracks.reserve(static_cast<std::size_t>(trackCount));

        for (int trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
            if (reader.ascii(4) != "MTrk") {
                throw std::runtime_error("Invalid MIDI track chunk");
            }

            const std::uint32_t length = reader.be32();
            if (reader.remaining() < length) {
                throw std::runtime_error("Truncated MIDI track");
            }

            std::vector<std::uint8_t> trackBytes;
            trackBytes.reserve(length);
            for (std::uint32_t i = 0; i < length; ++i) {
                trackBytes.push_back(reader.u8());
            }

            RawTrack& raw = tracks.emplace_back();
            ParseTrack(trackBytes, raw);
        }

        song.tracks.resize(tracks.size());
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            song.tracks[i].name = tracks[i].name.empty()
                ? "Track " + std::to_string(i + 1)
                : tracks[i].name;
        }

        const auto tempos = BuildTempoMap(tracks);
        ExtractNoteIntervals(tracks, song, tempos, song.notes);

        for (const auto& note : song.notes) {
            if (note.note < 21 || note.note > 108) continue;
            if (note.end < note.start) continue;
            if (note.track >= 0 && note.track < static_cast<int>(song.tracks.size())) {
                ++song.tracks[static_cast<std::size_t>(note.track)].noteCount;
            }
            song.duration = std::max(song.duration, note.end);
        }

        std::sort(song.notes.begin(), song.notes.end(), [](const MidiNote& a, const MidiNote& b) {
            if (a.start != b.start) return a.start < b.start;
            if (a.track != b.track) return a.track < b.track;
            return a.note < b.note;
        });

        if (song.duration <= 0.0) {
            std::uint64_t maxTick = 0;
            for (const auto& track : tracks) maxTick = std::max(maxTick, track.endTick);
            song.duration = TickToSeconds(maxTick, song, tempos);
        }

        outSong = std::move(song);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
