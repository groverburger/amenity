#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <tuple>

#include "sample.h"

// Identifies a sound: a frame range of a file. Settings and timeline lanes attach to this.
struct SoundKey {
    std::string path;
    size_t start = 0;
    size_t end = 0;

    bool operator<(const SoundKey& o) const {
        return std::tie(path, start, end) < std::tie(o.path, o.start, o.end);
    }
    bool operator==(const SoundKey& o) const {
        return std::tie(path, start, end) == std::tie(o.path, o.start, o.end);
    }
};

// How a sound is shaped. Rendered ahead of time, so nothing here costs the audio thread
// anything, and a bounce sounds exactly like playback.
struct SoundSettings {
    // Pitch and time.
    float pitch = 0.0f;       // semitones
    bool keepLength = false;  // change pitch without changing the length
    bool fit = false;         // match the length to the timeline BPM
    bool fitRepitch = false;  // fit by changing speed, like a record, rather than stretching
    float sourceBpm = 0.0f;   // the sound's own tempo; 0 = unknown
    bool loop = false;

    // Simple controls. Each one writes the detailed settings of its effect, below.
    float tone = 0.0f;  // -1 darker .. +1 thinner
    float punch = 0.0f;
    float crunch = 0.0f;
    float space = 0.0f;
    float echo = 0.0f;

    // Output.
    float level = 0.0f;  // dB
    float pan = 0.0f;    // -1 .. +1
    float fadeInMs = 0.0f;
    float fadeOutMs = 0.0f;

    // Filter. 0 = off, 1 = low-pass, 2 = high-pass, 3 = band-pass.
    int filterType = 0;
    float filterCutoff = 1000.0f;
    float filterResonance = 0.707f;

    float drive = 0.0f;  // 0 .. 1

    float crushBits = 16.0f;  // 16 = off
    float crushDownsample = 1.0f;

    bool compressor = false;
    float compThreshold = -18.0f;  // dB
    float compRatio = 4.0f;
    float compAttackMs = 15.0f;
    float compReleaseMs = 120.0f;
    float compMakeup = 0.0f;  // dB

    float delayMix = 0.0f;
    float delayBeats = 0.75f;
    float delayFeedback = 0.35f;

    float reverbMix = 0.0f;
    float reverbSize = 0.7f;
    float reverbDamping = 0.4f;

    // Bring the detailed settings of each effect in line with the simple controls.
    void applyTone();
    void applyPunch();
    void applyCrunch();
    void applySpace();
    void applyEcho();

    // True if rendering would leave the audio untouched.
    bool isNeutral() const;

    // Everything a render depends on, for telling whether one is out of date.
    std::string signature(double bpm) const;

    std::string serialize() const;
    static SoundSettings parse(const std::string& text);
};

// How a looping sound is cut to a musical length at its own tempo. It's never padded with
// silence, since that would leave a gap each time round. A sound a little short of a whole
// length keeps all of itself, and its tempo is taken to be slightly faster instead.
struct LoopPlan {
    size_t frames = 0;
    double sourceBpm = 0.0;  // the tempo to fit from
};
LoopPlan planLoop(size_t frames, double sourceBpm, uint32_t rate);

// Renders a frame range of a sample through its settings. Runs on a worker thread.
std::shared_ptr<const Sample> renderSound(const Sample& source, size_t start, size_t end,
                                          const SoundSettings& settings, double bpm);
