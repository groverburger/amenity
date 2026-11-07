#include "effects.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <sstream>
#include <vector>

#include "signalsmith-stretch/signalsmith-stretch.h"

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

using Channel = std::vector<float>;

float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }

// ---- Pitch and time ----

Channel resampled(const Channel& in, size_t outLength) {
    Channel out(outLength, 0.0f);
    if (in.empty() || outLength == 0) return out;
    const double step = static_cast<double>(in.size()) / static_cast<double>(outLength);
    const long last = static_cast<long>(in.size()) - 1;
    const auto at = [&](long i) { return in[static_cast<size_t>(std::clamp(i, 0L, last))]; };
    for (size_t i = 0; i < outLength; ++i) {
        const double position = static_cast<double>(i) * step;
        const long whole = static_cast<long>(position);
        const auto t = static_cast<float>(position - static_cast<double>(whole));
        const float a = at(whole - 1), b = at(whole), c = at(whole + 1), d = at(whole + 2);
        out[i] = b + 0.5f * t * (c - a + t * (2.0f * a - 5.0f * b + 4.0f * c - d + t * (3.0f * (b - c) + d - a)));
    }
    return out;
}

// Changes length and pitch independently. The library's one-shot call returns silence for
// input shorter than its analysis window, which most drum hits are, so it's padded with
// silence on both sides and the padding is trimmed from the result.
void stretch(Channel& left, Channel& right, size_t outLength, float semitones, uint32_t sampleRate) {
    if (left.empty() || outLength == 0) {
        left.assign(outLength, 0.0f);
        right.assign(outLength, 0.0f);
        return;
    }
    const double ratio = static_cast<double>(outLength) / static_cast<double>(left.size());
    const auto padIn = static_cast<size_t>(sampleRate) / 2;
    const auto padOut = static_cast<size_t>(std::llround(static_cast<double>(padIn) * ratio));

    std::array<Channel, 2> in;
    in[0].assign(padIn, 0.0f);
    in[1].assign(padIn, 0.0f);
    in[0].insert(in[0].end(), left.begin(), left.end());
    in[1].insert(in[1].end(), right.begin(), right.end());
    in[0].resize(in[0].size() + padIn, 0.0f);
    in[1].resize(in[1].size() + padIn, 0.0f);

    std::array<Channel, 2> out;
    out[0].assign(outLength + padOut * 2, 0.0f);
    out[1].assign(outLength + padOut * 2, 0.0f);

    // A shorter window than the library's default, which keeps drum hits tighter.
    signalsmith::stretch::SignalsmithStretch<float> engine;
    const int block = static_cast<int>(sampleRate * 0.07);
    engine.configure(2, block, block / 4);
    engine.setTransposeSemitones(semitones);

    std::array<float*, 2> inputs{in[0].data(), in[1].data()};
    std::array<float*, 2> outputs{out[0].data(), out[1].data()};
    engine.exact(inputs, static_cast<int>(in[0].size()), outputs, static_cast<int>(out[0].size()));

    left.assign(out[0].begin() + static_cast<std::ptrdiff_t>(padOut),
                out[0].begin() + static_cast<std::ptrdiff_t>(padOut + outLength));
    right.assign(out[1].begin() + static_cast<std::ptrdiff_t>(padOut),
                 out[1].begin() + static_cast<std::ptrdiff_t>(padOut + outLength));
}

// The nearest musically useful loop length: whole bars, or a power of two for short sounds.
double snapLoopBeats(double beats) {
    if (beats >= 3.0) return std::max(4.0, std::round(beats / 4.0) * 4.0);
    return std::max(0.25, std::pow(2.0, std::round(std::log2(std::max(beats, 0.0625)))));
}

void applyPitchAndTime(Channel& left, Channel& right, const SoundSettings& s, double bpm, uint32_t rate) {
    if (s.loop && s.sourceBpm > 0.0f) {
        const double beats = static_cast<double>(left.size()) / rate * s.sourceBpm / 60.0;
        const auto length = static_cast<size_t>(std::llround(snapLoopBeats(beats) * 60.0 / s.sourceBpm * rate));
        left.resize(length, 0.0f);
        right.resize(length, 0.0f);
    }

    const bool hasPitch = std::fabs(s.pitch) > 1e-3f;
    const bool fit = s.fit && s.sourceBpm > 0.0f && bpm > 0.0;
    if (fit) {
        const auto target = static_cast<size_t>(
            std::llround(static_cast<double>(left.size()) * static_cast<double>(s.sourceBpm) / bpm));
        if (s.fitRepitch) {
            left = resampled(left, target);
            right = resampled(right, target);
            if (hasPitch) stretch(left, right, target, s.pitch, rate);
        } else if (target != left.size() || hasPitch) {
            stretch(left, right, target, s.pitch, rate);
        }
    } else if (hasPitch && s.keepLength) {
        stretch(left, right, left.size(), s.pitch, rate);
    } else if (hasPitch) {
        const double speed = std::pow(2.0, static_cast<double>(s.pitch) / 12.0);
        const auto target = static_cast<size_t>(std::llround(static_cast<double>(left.size()) / speed));
        left = resampled(left, target);
        right = resampled(right, target);
    }
}

// ---- Effects ----

void applyFilter(Channel& samples, const SoundSettings& s, uint32_t rate) {
    const float nyquist = static_cast<float>(rate) * 0.49f;
    const float w = 2.0f * kPi * std::clamp(s.filterCutoff, 10.0f, nyquist) / static_cast<float>(rate);
    const float alpha = std::sin(w) / (2.0f * std::max(s.filterResonance, 0.1f));
    const float cosine = std::cos(w);
    float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f;
    if (s.filterType == 1) {
        b0 = (1.0f - cosine) * 0.5f;
        b1 = 1.0f - cosine;
        b2 = b0;
    } else if (s.filterType == 2) {
        b0 = (1.0f + cosine) * 0.5f;
        b1 = -(1.0f + cosine);
        b2 = b0;
    } else {
        b0 = alpha;
        b2 = -alpha;
    }
    const float a0 = 1.0f + alpha;
    const float a1 = -2.0f * cosine;
    const float a2 = 1.0f - alpha;
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
    for (float& sample : samples) {
        const float x = sample;
        const float y = (b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        sample = y;
    }
}

void applyDrive(Channel& samples, float amount) {
    const float gain = 1.0f + 12.0f * amount;
    const float normalize = 1.0f / std::tanh(gain);
    for (float& sample : samples) {
        const float shaped = std::tanh(sample * gain) * normalize;
        sample += (shaped - sample) * std::min(amount * 2.0f, 1.0f);
    }
}

void applyCrush(Channel& samples, float bits, float downsample) {
    const float steps = std::pow(2.0f, bits - 1.0f);
    const int hold = std::max(1, static_cast<int>(downsample));
    float held = 0.0f;
    for (size_t i = 0; i < samples.size(); ++i) {
        if (i % static_cast<size_t>(hold) == 0) held = samples[i];
        samples[i] = bits >= 16.0f ? held : std::round(held * steps) / steps;
    }
}

// Stereo-linked, so the image doesn't wander when one side is louder.
void applyCompressor(Channel& left, Channel& right, const SoundSettings& s, uint32_t rate) {
    const float rateF = static_cast<float>(rate);
    const float attack = std::exp(-1.0f / (std::max(s.compAttackMs, 0.1f) * 0.001f * rateF));
    const float release = std::exp(-1.0f / (std::max(s.compReleaseMs, 1.0f) * 0.001f * rateF));
    const float makeup = dbToGain(s.compMakeup);
    const float slope = 1.0f - 1.0f / std::max(s.compRatio, 1.0f);
    float envelopeDb = -120.0f;
    for (size_t i = 0; i < left.size(); ++i) {
        const float peak = std::max(std::fabs(left[i]), std::fabs(right[i]));
        const float db = 20.0f * std::log10(std::max(peak, 1e-6f));
        const float coefficient = db > envelopeDb ? attack : release;
        envelopeDb = db + coefficient * (envelopeDb - db);
        const float reduction = std::max(envelopeDb - s.compThreshold, 0.0f) * slope;
        const float gain = dbToGain(-reduction) * makeup;
        left[i] *= gain;
        right[i] *= gain;
    }
}

void applyDelay(Channel& left, Channel& right, const SoundSettings& s, double bpm, uint32_t rate) {
    const double beatFrames = 60.0 / std::max(bpm, 1.0) * rate;
    const auto delay = std::max<size_t>(1, static_cast<size_t>(static_cast<double>(s.delayBeats) * beatFrames));
    const Channel dryLeft = left;
    const Channel dryRight = right;
    Channel wetLeft(left.size(), 0.0f);
    Channel wetRight(left.size(), 0.0f);
    // Repeats bounce from side to side.
    for (size_t i = delay; i < left.size(); ++i) {
        wetLeft[i] = dryRight[i - delay] + wetRight[i - delay] * s.delayFeedback;
        wetRight[i] = dryLeft[i - delay] + wetLeft[i - delay] * s.delayFeedback;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        left[i] += wetLeft[i] * s.delayMix;
        right[i] += wetRight[i] * s.delayMix;
    }
}

// A Schroeder-style reverb: parallel damped combs into series all-passes.
void applyReverbChannel(const Channel& input, Channel& output, const SoundSettings& s, uint32_t rate, int spread) {
    constexpr std::array<int, 8> kCombs{1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    constexpr std::array<int, 4> kAllPasses{556, 441, 341, 225};
    const double scale = static_cast<double>(rate) / 44100.0;
    const float feedback = 0.7f + 0.28f * std::clamp(s.reverbSize, 0.0f, 1.0f);
    const float damping = std::clamp(s.reverbDamping, 0.0f, 1.0f) * 0.7f;

    Channel wet(input.size(), 0.0f);
    for (int length : kCombs) {
        Channel buffer(static_cast<size_t>((length + spread) * scale), 0.0f);
        size_t at = 0;
        float low = 0.0f;
        for (size_t i = 0; i < input.size(); ++i) {
            const float out = buffer[at];
            low = out + (low - out) * damping;
            buffer[at] = input[i] * 0.015f + low * feedback;
            at = (at + 1) % buffer.size();
            wet[i] += out;
        }
    }
    for (int length : kAllPasses) {
        Channel buffer(static_cast<size_t>((length + spread) * scale), 0.0f);
        size_t at = 0;
        for (float& sample : wet) {
            const float delayed = buffer[at];
            const float in = sample;
            buffer[at] = in + delayed * 0.5f;
            at = (at + 1) % buffer.size();
            sample = delayed - in;
        }
    }
    for (size_t i = 0; i < input.size(); ++i) output[i] += wet[i] * s.reverbMix;
}

void applyReverb(Channel& left, Channel& right, const SoundSettings& s, uint32_t rate) {
    const Channel dryLeft = left;
    const Channel dryRight = right;
    applyReverbChannel(dryLeft, left, s, rate, 0);
    applyReverbChannel(dryRight, right, s, rate, 23);
}

// How much extra room the echoes and reverb need to ring out.
size_t tailFrames(const SoundSettings& s, double bpm, uint32_t rate) {
    double seconds = 0.0;
    if (s.delayMix > 0.0f) {
        const double repeats = std::log(0.001) / std::log(std::clamp(static_cast<double>(s.delayFeedback), 0.05, 0.95));
        seconds = std::max(seconds, std::min((repeats + 1.0) * static_cast<double>(s.delayBeats) * 60.0 / std::max(bpm, 1.0), 8.0));
    }
    if (s.reverbMix > 0.0f) seconds = std::max(seconds, 1.0 + 4.0 * static_cast<double>(s.reverbSize));
    return static_cast<size_t>(seconds * rate);
}

// ---- Settings as text ----

struct FloatField {
    const char* name;
    float SoundSettings::*member;
};
struct BoolField {
    const char* name;
    bool SoundSettings::*member;
};

constexpr std::array kFloatFields{
    FloatField{"pitch", &SoundSettings::pitch},
    FloatField{"sourceBpm", &SoundSettings::sourceBpm},
    FloatField{"tone", &SoundSettings::tone},
    FloatField{"punch", &SoundSettings::punch},
    FloatField{"crunch", &SoundSettings::crunch},
    FloatField{"space", &SoundSettings::space},
    FloatField{"echo", &SoundSettings::echo},
    FloatField{"level", &SoundSettings::level},
    FloatField{"pan", &SoundSettings::pan},
    FloatField{"fadeIn", &SoundSettings::fadeInMs},
    FloatField{"fadeOut", &SoundSettings::fadeOutMs},
    FloatField{"cutoff", &SoundSettings::filterCutoff},
    FloatField{"resonance", &SoundSettings::filterResonance},
    FloatField{"drive", &SoundSettings::drive},
    FloatField{"bits", &SoundSettings::crushBits},
    FloatField{"downsample", &SoundSettings::crushDownsample},
    FloatField{"threshold", &SoundSettings::compThreshold},
    FloatField{"ratio", &SoundSettings::compRatio},
    FloatField{"attack", &SoundSettings::compAttackMs},
    FloatField{"release", &SoundSettings::compReleaseMs},
    FloatField{"makeup", &SoundSettings::compMakeup},
    FloatField{"delayMix", &SoundSettings::delayMix},
    FloatField{"delayBeats", &SoundSettings::delayBeats},
    FloatField{"feedback", &SoundSettings::delayFeedback},
    FloatField{"reverbMix", &SoundSettings::reverbMix},
    FloatField{"reverbSize", &SoundSettings::reverbSize},
    FloatField{"damping", &SoundSettings::reverbDamping},
};

constexpr std::array kBoolFields{
    BoolField{"keepLength", &SoundSettings::keepLength},
    BoolField{"fit", &SoundSettings::fit},
    BoolField{"fitRepitch", &SoundSettings::fitRepitch},
    BoolField{"loop", &SoundSettings::loop},
    BoolField{"compressor", &SoundSettings::compressor},
};

}  // namespace

void SoundSettings::applyTone() {
    if (std::fabs(tone) < 0.01f) {
        filterType = 0;
    } else if (tone < 0.0f) {
        filterType = 1;
        filterCutoff = 18000.0f * std::pow(150.0f / 18000.0f, -tone);
        filterResonance = 0.707f;
    } else {
        filterType = 2;
        filterCutoff = 30.0f * std::pow(3000.0f / 30.0f, tone);
        filterResonance = 0.707f;
    }
}

void SoundSettings::applyPunch() {
    compressor = punch > 0.0f;
    compThreshold = -6.0f - 26.0f * punch;
    compRatio = 2.0f + 6.0f * punch;
    compAttackMs = 15.0f;  // slow enough to let the attack through, which is where the punch is
    compReleaseMs = 120.0f;
    compMakeup = -compThreshold * (1.0f - 1.0f / compRatio) * 0.5f;
}

void SoundSettings::applyCrunch() {
    crushBits = 16.0f - 12.0f * crunch;
    crushDownsample = 1.0f + 15.0f * crunch * crunch;
}

void SoundSettings::applySpace() {
    reverbMix = 0.5f * space;
    reverbSize = 0.5f + 0.45f * space;
    reverbDamping = 0.4f;
}

void SoundSettings::applyEcho() {
    delayMix = 0.55f * echo;
    delayFeedback = 0.2f + 0.5f * echo;
}

bool SoundSettings::isNeutral() const {
    return std::fabs(pitch) < 1e-3f && !fit && !(loop && sourceBpm > 0.0f) && level == 0.0f && pan == 0.0f &&
           fadeInMs <= 0.0f && fadeOutMs <= 0.0f && filterType == 0 && drive <= 0.0f && crushBits >= 16.0f &&
           crushDownsample <= 1.0f && !compressor && delayMix <= 0.0f && reverbMix <= 0.0f;
}

std::string SoundSettings::signature(double bpm) const {
    std::string text = serialize();
    // Macro positions don't change the sound by themselves, but including them is harmless.
    if (fit || delayMix > 0.0f) {
        text += ";@" + std::to_string(bpm);
    }
    return text;
}

std::string SoundSettings::serialize() const {
    std::ostringstream out;
    out.precision(9);
    for (const FloatField& field : kFloatFields) out << field.name << '=' << this->*field.member << ';';
    for (const BoolField& field : kBoolFields) out << field.name << '=' << (this->*field.member ? 1 : 0) << ';';
    out << "filter=" << filterType;
    return out.str();
}

SoundSettings SoundSettings::parse(const std::string& text) {
    SoundSettings settings;
    size_t from = 0;
    while (from < text.size()) {
        size_t to = text.find(';', from);
        if (to == std::string::npos) to = text.size();
        const size_t equals = text.find('=', from);
        if (equals != std::string::npos && equals < to) {
            const std::string name = text.substr(from, equals - from);
            const std::string value = text.substr(equals + 1, to - equals - 1);
            for (const FloatField& field : kFloatFields) {
                if (name == field.name) settings.*field.member = std::strtof(value.c_str(), nullptr);
            }
            for (const BoolField& field : kBoolFields) {
                if (name == field.name) settings.*field.member = value == "1";
            }
            if (name == "filter") settings.filterType = std::clamp(std::atoi(value.c_str()), 0, 3);
        }
        from = to + 1;
    }
    return settings;
}

std::shared_ptr<const Sample> renderSound(const Sample& source, size_t start, size_t end,
                                          const SoundSettings& s, double bpm) {
    end = std::min(end, source.frames);
    start = std::min(start, end);
    const uint32_t rate = source.sampleRate;

    Channel left(end - start);
    Channel right(end - start);
    for (size_t i = 0; i < left.size(); ++i) {
        left[i] = source.stereo[(start + i) * 2];
        right[i] = source.stereo[(start + i) * 2 + 1];
    }

    applyPitchAndTime(left, right, s, bpm, rate);
    const size_t length = left.size();

    if (s.fadeInMs > 0.0f && length > 0) {
        const size_t fade = std::min(length, static_cast<size_t>(s.fadeInMs * 0.001f * static_cast<float>(rate)));
        for (size_t i = 0; i < fade; ++i) {
            const float gain = static_cast<float>(i) / static_cast<float>(fade);
            left[i] *= gain;
            right[i] *= gain;
        }
    }
    if (s.fadeOutMs > 0.0f && length > 0) {
        const size_t fade = std::min(length, static_cast<size_t>(s.fadeOutMs * 0.001f * static_cast<float>(rate)));
        for (size_t i = 0; i < fade; ++i) {
            const float gain = static_cast<float>(i) / static_cast<float>(fade);
            left[length - 1 - i] *= gain;
            right[length - 1 - i] *= gain;
        }
    }

    const size_t tail = tailFrames(s, bpm, rate);
    left.resize(length + tail, 0.0f);
    right.resize(length + tail, 0.0f);

    if (s.filterType != 0) {
        applyFilter(left, s, rate);
        applyFilter(right, s, rate);
    }
    if (s.drive > 0.0f) {
        applyDrive(left, s.drive);
        applyDrive(right, s.drive);
    }
    if (s.crushBits < 16.0f || s.crushDownsample > 1.0f) {
        applyCrush(left, s.crushBits, s.crushDownsample);
        applyCrush(right, s.crushBits, s.crushDownsample);
    }
    if (s.compressor) applyCompressor(left, right, s, rate);
    if (s.delayMix > 0.0f) applyDelay(left, right, s, bpm, rate);
    if (s.reverbMix > 0.0f) applyReverb(left, right, s, rate);

    const float gain = dbToGain(s.level);
    const float angle = (std::clamp(s.pan, -1.0f, 1.0f) + 1.0f) * 0.25f * kPi;
    const float leftGain = gain * std::cos(angle) * std::numbers::sqrt2_v<float>;
    const float rightGain = gain * std::sin(angle) * std::numbers::sqrt2_v<float>;
    for (size_t i = 0; i < left.size(); ++i) {
        left[i] *= leftGain;
        right[i] *= rightGain;
    }

    if (s.loop) {
        // A loop has to stay exactly on length, so its tail wraps around onto its start.
        for (size_t i = length; i < left.size() && length > 0; ++i) {
            left[(i - length) % length] += left[i];
            right[(i - length) % length] += right[i];
        }
        left.resize(length);
        right.resize(length);
    } else if (tail > 0) {
        // Otherwise drop whatever part of the tail is effectively silent.
        size_t used = left.size();
        while (used > length && std::fabs(left[used - 1]) < 1e-4f && std::fabs(right[used - 1]) < 1e-4f) --used;
        left.resize(used);
        right.resize(used);
    }

    auto result = std::make_shared<Sample>();
    result->sampleRate = rate;
    result->path = source.path;
    result->name = source.name;
    result->stereo.resize(left.size() * 2);
    for (size_t i = 0; i < left.size(); ++i) {
        result->stereo[i * 2] = left[i];
        result->stereo[i * 2 + 1] = right[i];
    }
    finalizeSample(*result);
    return result;
}
