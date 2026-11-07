#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "effects.h"

inline constexpr int kKeysPerBank = 40;
inline constexpr int kBeatsPerBar = 4;

struct QuantizeOption {
    const char* label;
    double beats;  // grid size; 0 means no snapping
};

inline constexpr std::array<QuantizeOption, 8> kQuantizeOptions{{
    {"Off", 0.0},
    {"1/4", 1.0},
    {"1/8", 0.5},
    {"1/8 triplet", 1.0 / 3.0},
    {"1/16", 0.25},
    {"1/16 triplet", 1.0 / 6.0},
    {"1/32", 0.125},
    {"1/64", 0.0625},
}};

struct TimelineEvent {
    uint64_t id = 0;
    double beat = 0.0;
    double durationBeats = 0.0;  // 0 plays the whole sound
    size_t start = 0;
    size_t end = 0;
    float gain = 1.0f;
    bool reverse = false;
    bool choke = false;
    std::string path;

    SoundKey sound() const { return {path, start, end}; }
};

struct Timeline {
    double bpm = 136.0;
    int bars = 4;
    std::vector<TimelineEvent> events;
    uint64_t nextId = 1;

    double loopBeats() const { return static_cast<double>(bars * kBeatsPerBar); }
};

// Rounds a beat to the nearest grid line. A grid of 0 leaves it unchanged.
double snapBeat(double beat, double grid);

// Maps a beat that has reached or passed the end of the loop back onto it.
double wrapBeat(double beat, double loopBeats);

// Adds a hit, keeping events sorted. Returns its id, or 0 if the same sound is
// already at that exact position, as happens when two presses snap to one grid line.
uint64_t addHit(Timeline& timeline, TimelineEvent event);

// Snaps all events inside the loop to a grid, removing any that end up duplicated.
void quantizeAll(Timeline& timeline, double grid);

struct Kit {
    std::string name = "Kit 1";
    std::array<std::string, kKeysPerBank> keys;  // sound path per key; empty = unassigned
};

// Everything that's remembered between runs.
struct Session {
    Timeline timeline;
    std::vector<Kit> kits{Kit{}};  // always at least one
    int currentKit = 0;
    std::map<SoundKey, SoundSettings> soundSettings;
    int quantizeIndex = 4;  // 1/16
    bool metronome = true;
    std::string song;
    uint32_t sampleRate = 48000;  // the rate that frame positions are expressed in

    Kit& kit() { return kits[static_cast<size_t>(currentKit)]; }
    const Kit& kit() const { return kits[static_cast<size_t>(currentKit)]; }
};

// Writes atomically, so a crash mid-save can't damage the previous session.
bool saveSession(const Session& session, const std::string& path);

// Returns false if the file is missing or isn't a session file, leaving `session` alone.
// Frame positions are converted to targetRate. Older session formats load too; if
// `version` is given, it receives the format of the file.
bool loadSession(Session& session, const std::string& path, uint32_t targetRate, int* version = nullptr);
