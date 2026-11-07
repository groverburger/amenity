#include "slicer.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace {

constexpr size_t kFrameSize = 1024;
constexpr size_t kHop = 256;

void fft(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float angle = -2.0f * std::numbers::pi_v<float> / static_cast<float>(len);
        const std::complex<float> step(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

// Sharpens a coarse, hop-resolution onset to the biggest local jump in energy, then
// backs up slightly and snaps to a zero crossing so playback starts cleanly.
size_t refineOnset(const std::vector<float>& mono, size_t coarse, uint32_t sampleRate, float floorEnergy) {
    constexpr size_t kWindow = 128;
    constexpr size_t kStep = 16;
    if (mono.size() < kWindow * 2 + kFrameSize) return std::min(coarse, mono.size());

    const size_t first = std::max(coarse, kWindow);
    const size_t last = std::min(coarse + kFrameSize / 2 + kHop, mono.size() - kWindow);

    size_t best = first;
    float bestScore = -1.0f;
    for (size_t pos = first; pos < last; pos += kStep) {
        float before = 0.0f;
        float after = 0.0f;
        for (size_t i = 0; i < kWindow; ++i) {
            before += mono[pos - kWindow + i] * mono[pos - kWindow + i];
            after += mono[pos + i] * mono[pos + i];
        }
        const float score = after / (before + floorEnergy);
        if (score > bestScore) {
            bestScore = score;
            best = pos;
        }
    }

    const size_t preRoll = sampleRate / 1000;
    size_t pos = best > preRoll ? best - preRoll : 0;
    for (size_t i = 0; i < preRoll && pos > 0; ++i, --pos) {
        if ((mono[pos - 1] <= 0.0f) != (mono[pos] <= 0.0f)) break;
    }
    return pos;
}

}  // namespace

OnsetFunction computeOnsetFunction(const std::vector<float>& mono) {
    OnsetFunction result;
    result.hop = kHop;
    result.frameSize = kFrameSize;
    if (mono.size() < kFrameSize) return result;

    std::vector<float> window(kFrameSize);
    for (size_t i = 0; i < kFrameSize; ++i) {
        window[i] = 0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<float> * static_cast<float>(i) /
                                           static_cast<float>(kFrameSize - 1));
    }

    const size_t count = (mono.size() - kFrameSize) / kHop + 1;
    result.flux.assign(count, 0.0f);
    std::vector<std::complex<float>> buffer(kFrameSize);
    std::vector<float> previous(kFrameSize / 2, 0.0f);
    float largest = 0.0f;

    for (size_t t = 0; t < count; ++t) {
        for (size_t i = 0; i < kFrameSize; ++i) buffer[i] = mono[t * kHop + i] * window[i];
        fft(buffer);
        float flux = 0.0f;
        for (size_t k = 0; k < kFrameSize / 2; ++k) {
            const float magnitude = std::log1p(10.0f * std::abs(buffer[k]));
            flux += std::max(0.0f, magnitude - previous[k]);
            previous[k] = magnitude;
        }
        result.flux[t] = flux;
        largest = std::max(largest, flux);
    }
    if (largest > 0.0f) {
        for (float& f : result.flux) f /= largest;
    }
    return result;
}

std::vector<size_t> pickOnsets(const OnsetFunction& onsets, const std::vector<float>& mono,
                               uint32_t sampleRate, float sensitivity, float minGapMs) {
    std::vector<size_t> markers{0};
    const auto& flux = onsets.flux;
    if (flux.empty() || onsets.hop == 0) return markers;

    const float hopsPerSecond = static_cast<float>(sampleRate) / static_cast<float>(onsets.hop);
    const size_t meanRadius = std::max<size_t>(1, static_cast<size_t>(0.15f * hopsPerSecond));
    constexpr size_t kPeakRadius = 3;
    const float delta = 0.30f + (0.01f - 0.30f) * std::clamp(sensitivity, 0.0f, 1.0f);

    // Running sum for the local mean, so a re-pick stays fast on long files.
    std::vector<double> sum(flux.size() + 1, 0.0);
    for (size_t i = 0; i < flux.size(); ++i) sum[i + 1] = sum[i] + flux[i];

    struct Peak {
        size_t frame;
        float strength;
    };
    std::vector<Peak> peaks;
    for (size_t t = 1; t < flux.size(); ++t) {
        const size_t lo = t > meanRadius ? t - meanRadius : 0;
        const size_t hi = std::min(t + meanRadius + 1, flux.size());
        const float mean = static_cast<float>((sum[hi] - sum[lo]) / static_cast<double>(hi - lo));
        if (flux[t] < mean * 1.2f + delta) continue;

        const size_t peakLo = t > kPeakRadius ? t - kPeakRadius : 0;
        const size_t peakHi = std::min(t + kPeakRadius + 1, flux.size());
        const float local = *std::max_element(flux.begin() + peakLo, flux.begin() + peakHi);
        if (flux[t] == local) peaks.push_back({t * onsets.hop, flux[t]});
    }

    // Where peaks are closer than the minimum gap, the stronger one wins.
    const auto minGap = static_cast<size_t>(minGapMs * 0.001f * static_cast<float>(sampleRate));
    std::vector<Peak> kept;
    for (const Peak& peak : peaks) {
        if (!kept.empty() && peak.frame - kept.back().frame < minGap) {
            if (peak.strength > kept.back().strength) kept.back() = peak;
        } else {
            kept.push_back(peak);
        }
    }

    float largestBlock = 0.0f;
    for (size_t i = 0; i + 128 <= mono.size(); i += 128) {
        float energy = 0.0f;
        for (size_t k = 0; k < 128; ++k) energy += mono[i + k] * mono[i + k];
        largestBlock = std::max(largestBlock, energy);
    }
    const float floorEnergy = std::max(largestBlock * 1e-3f, 1e-9f);

    for (const Peak& peak : kept) {
        const size_t frame = refineOnset(mono, peak.frame, sampleRate, floorEnergy);
        // Anything right at the start belongs to the slice that's already at frame 0.
        if (frame < minGap / 2 || frame >= mono.size()) continue;
        if (frame > markers.back()) markers.push_back(frame);
    }

    // Silence before the first hit shouldn't use up a key, so slice 1 starts on the hit.
    if (markers.size() > 1) {
        const auto magnitude = [](float a, float b) { return std::fabs(a) < std::fabs(b); };
        const float filePeak = std::fabs(*std::max_element(mono.begin(), mono.end(), magnitude));
        const auto leadEnd = mono.begin() + static_cast<std::ptrdiff_t>(markers[1]);
        const float leadPeak = std::fabs(*std::max_element(mono.begin(), leadEnd, magnitude));
        if (leadPeak < filePeak * 0.02f) markers.erase(markers.begin());
    }
    return markers;
}

double estimateTempo(const OnsetFunction& onsets, uint32_t sampleRate) {
    const auto& flux = onsets.flux;
    if (onsets.hop == 0 || sampleRate == 0) return 0.0;
    const double hopsPerSecond = static_cast<double>(sampleRate) / static_cast<double>(onsets.hop);
    // At least two seconds, which holds a few beats even at the slowest tempo.
    if (static_cast<double>(flux.size()) < hopsPerSecond * 2.0) return 0.0;

    // Long songs don't get any more accurate past a minute, only slower.
    const size_t count = std::min(flux.size(), static_cast<size_t>(hopsPerSecond * 60.0));
    double mean = 0.0;
    for (size_t i = 0; i < count; ++i) mean += static_cast<double>(flux[i]);
    mean /= static_cast<double>(count);
    std::vector<float> centered(count);
    for (size_t i = 0; i < count; ++i) centered[i] = static_cast<float>(static_cast<double>(flux[i]) - mean);

    // Autocorrelation, normalized per overlapping sample so long lags aren't penalized.
    const size_t maxLag = std::min(count - 1, static_cast<size_t>(hopsPerSecond * 60.0 / 70.0 * 8.0) + 2);
    std::vector<double> correlation(maxLag + 1, 0.0);
    for (size_t lag = 1; lag <= maxLag; ++lag) {
        double total = 0.0;
        for (size_t i = 0; i + lag < count; ++i) {
            total += static_cast<double>(centered[i]) * static_cast<double>(centered[i + lag]);
        }
        correlation[lag] = total / static_cast<double>(count - lag);
    }
    const double energy = [&] {
        double total = 0.0;
        for (float value : centered) total += static_cast<double>(value) * static_cast<double>(value);
        return total / static_cast<double>(count);
    }();
    if (energy <= 0.0) return 0.0;

    const auto at = [&](double lag) {
        const auto whole = static_cast<size_t>(lag);
        if (whole + 1 > maxLag) return 0.0;
        const double t = lag - static_cast<double>(whole);
        return correlation[whole] * (1.0 - t) + correlation[whole + 1] * t;
    };

    // Each candidate scores on its beat and on the multiples of it that fit. The longer lags
    // are what give a precise answer, since a one-hop error there is a small tempo error.
    double bestBpm = 0.0;
    double bestScore = 0.0;
    for (double bpm = 70.0; bpm <= 190.0; bpm += 0.25) {
        const double beatLag = hopsPerSecond * 60.0 / bpm;
        double score = 0.0;
        int terms = 0;
        for (double multiple : {1.0, 2.0, 4.0, 8.0}) {
            if (beatLag * multiple + 1.0 > static_cast<double>(maxLag)) break;
            score += at(beatLag * multiple);
            ++terms;
        }
        if (terms == 0) continue;
        score /= terms;
        // A gentle lean toward typical tempos settles double-or-half ambiguity.
        const double octavesFromCenter = std::log2(bpm / 130.0);
        score *= std::exp(-0.5 * octavesFromCenter * octavesFromCenter / (0.8 * 0.8));
        if (score > bestScore) {
            bestScore = score;
            bestBpm = bpm;
        }
    }
    if (bestScore / energy < 0.05) return 0.0;  // no clear pulse
    return std::round(bestBpm * 2.0) / 2.0;
}

std::vector<size_t> gridSlices(size_t frames, int count) {
    std::vector<size_t> markers;
    count = std::max(count, 1);
    markers.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        markers.push_back(frames * static_cast<size_t>(i) / static_cast<size_t>(count));
    }
    return markers;
}
