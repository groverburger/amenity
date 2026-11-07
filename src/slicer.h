#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Spectral flux over a whole file. Slow to compute, so it's calculated once per file.
struct OnsetFunction {
    size_t hop = 0;
    size_t frameSize = 0;
    std::vector<float> flux;  // normalized to 0..1
};

OnsetFunction computeOnsetFunction(const std::vector<float>& mono);

// Picks slice start positions from an onset function. Quick enough to re-run while
// a slider moves. sensitivity is 0..1, where higher finds more onsets.
// The result is sorted and begins at frame 0, unless the file opens with near-silence,
// in which case it begins at the first onset.
std::vector<size_t> pickOnsets(const OnsetFunction& onsets, const std::vector<float>& mono,
                               uint32_t sampleRate, float sensitivity, float minGapMs = 50.0f);

// Estimates a tempo from the rhythm of an onset function, between 70 and 190 BPM.
// Returns 0 if there's too little audio or no clear pulse.
double estimateTempo(const OnsetFunction& onsets, uint32_t sampleRate);

// Equal-length slices. The result always begins with frame 0.
std::vector<size_t> gridSlices(size_t frames, int count);
