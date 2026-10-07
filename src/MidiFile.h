#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MidiNote {
    int note = 0;
    int channel = 0;
    int track = 0;
    int velocity = 100;
    double start = 0.0;
    double end = 0.0;
};

struct MidiTrackInfo {
    std::string name;
    int noteCount = 0;
};

struct MidiSong {
    int format = 0;
    int ticksPerQuarter = 480;
    bool smpte = false;
    double smpteTicksPerSecond = 0.0;
    double duration = 0.0;
    std::vector<MidiNote> notes;
    std::vector<MidiTrackInfo> tracks;
};

bool LoadMidiFile(const std::wstring& path, MidiSong& outSong, std::string& error);
