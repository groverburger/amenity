#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Decoded audio. Immutable once it has been handed to the engine.
struct Sample {
    static constexpr size_t kPeakBlock = 256;

    std::string path;
    std::string name;
    uint32_t sampleRate = 0;
    size_t frames = 0;
    std::vector<float> stereo;   // interleaved L, R
    std::vector<float> mono;     // mixdown used for analysis and display
    std::vector<float> peakMin;  // per kPeakBlock frames of mono
    std::vector<float> peakMax;
};

// Builds the derived fields (frames, mono, peaks) from `stereo` and `sampleRate`.
void finalizeSample(Sample& sample);

// Decodes a file and converts it to stereo at targetRate.
// Returns null and sets `error` if the file can't be read.
std::shared_ptr<Sample> loadSample(const std::string& path, uint32_t targetRate, std::string& error);

// Copies a frame range into a new, standalone sample.
std::shared_ptr<Sample> extractClip(const Sample& source, size_t start, size_t end);

// Writes a 32-bit float WAV file.
bool writeWav(const Sample& sample, const std::string& path);
