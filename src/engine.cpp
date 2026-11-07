#include "engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numbers>

namespace {

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Leaves anything below the threshold untouched and never exceeds 1.0.
float softClip(float x) {
    constexpr float kThreshold = 0.9f;
    const float magnitude = std::fabs(x);
    if (magnitude <= kThreshold) return x;
    const float shaped = kThreshold + (1.0f - kThreshold) *
                                          std::tanh((magnitude - kThreshold) / (1.0f - kThreshold));
    return std::copysign(shaped, x);
}

}  // namespace

Engine::Engine(uint32_t sampleRate)
    : sampleRate_(sampleRate),
      fadeInFrames_(std::max<size_t>(1, sampleRate / 4000)),    // 0.25 ms: keeps attacks sharp
      fadeOutFrames_(std::max<size_t>(1, sampleRate / 500)),    // 2 ms: at a slice's natural end
      releaseFrames_(std::max<size_t>(1, sampleRate / 200)) {   // 5 ms: when cut off early
    for (auto& position : positions_) position.store(-1, std::memory_order_relaxed);
    for (auto& sample : voiceSamples_) sample.store(nullptr, std::memory_order_relaxed);
}

bool Engine::trigger(const Trigger& trigger) {
    Command command;
    command.type = Command::Type::Trigger;
    command.trigger = trigger;
    command.sentNs = nowNs();
    return queue_.push(command);
}

bool Engine::release(int key) {
    Command command;
    command.type = Command::Type::Release;
    command.key = key;
    return queue_.push(command);
}

bool Engine::stopAll() {
    Command command;
    command.type = Command::Type::StopAll;
    return queue_.push(command);
}

bool Engine::retireSample(std::shared_ptr<const Sample> sample) {
    Command command;
    command.type = Command::Type::Retire;
    command.sample = sample.get();
    command.generation = nextGeneration_;
    if (!queue_.push(command)) return false;
    retained_.emplace_back(nextGeneration_++, std::move(sample));
    return true;
}

bool Engine::setSequence(std::shared_ptr<const Sequence> sequence) {
    Command command;
    command.type = Command::Type::SetSequence;
    command.sequence = sequence.get();
    command.generation = nextGeneration_;
    if (!queue_.push(command)) return false;
    if (currentSequence_) retained_.emplace_back(nextGeneration_, std::move(currentSequence_));
    ++nextGeneration_;
    currentSequence_ = std::move(sequence);
    return true;
}

bool Engine::setTempo(double bpm) {
    Command command;
    command.type = Command::Type::SetTempo;
    command.value = bpm;
    return queue_.push(command);
}

bool Engine::setLoopBeats(double beats) {
    Command command;
    command.type = Command::Type::SetLoop;
    command.value = beats;
    return queue_.push(command);
}

bool Engine::play(double fromBeat) {
    Command command;
    command.type = Command::Type::Play;
    command.value = fromBeat;
    return queue_.push(command);
}

bool Engine::stopTransport() {
    Command command;
    command.type = Command::Type::Stop;
    return queue_.push(command);
}

bool Engine::setPaused(bool paused) {
    Command command;
    command.type = Command::Type::Pause;
    command.value = paused ? 1.0 : 0.0;
    return queue_.push(command);
}

bool Engine::suppressEvent(uint64_t id, double beat) {
    Command command;
    command.type = Command::Type::Suppress;
    command.id = id;
    command.value = beat;
    return queue_.push(command);
}

void Engine::collectRetired() {
    const uint64_t applied = appliedGeneration_.load(std::memory_order_acquire);
    std::erase_if(retained_, [applied](const auto& entry) { return entry.first <= applied; });
}

void Engine::beginRelease(Voice& voice) {
    if (voice.active && voice.releaseLeft == 0) voice.releaseLeft = releaseFrames_;
}

void Engine::startVoice(const Trigger& trigger) {
    if (!trigger.sample) return;
    const size_t end = std::min(trigger.end, trigger.sample->frames);
    if (trigger.start >= end) return;

    const void* group = trigger.chokeGroup ? trigger.chokeGroup : trigger.sample;
    if (trigger.choke) {
        for (Voice& voice : voices_) {
            // Paused voices are left alone, so playing while paused can't lose the pause.
            if (voice.chokeGroup == group && !voice.frozen && voice.freezeLeft == 0) beginRelease(voice);
        }
    }

    // Take an idle voice, or else steal the oldest.
    int slot = 0;
    for (int i = 0; i < kMaxVoices; ++i) {
        if (!voices_[i].active) {
            slot = i;
            break;
        }
        if (voiceOrder_[i] < voiceOrder_[slot]) slot = i;
    }

    Voice& voice = voices_[slot];
    voice = Voice{};
    voice.active = true;
    voice.sample = trigger.sample;
    voice.chokeGroup = group;
    voice.loop = trigger.loop;
    voice.framesLeft = trigger.maxFrames;
    voice.key = trigger.key;
    voice.start = trigger.start;
    voice.end = end;
    voice.position = trigger.reverse ? end - 1 : trigger.start;
    voice.gain = trigger.gain;
    voice.reverse = trigger.reverse;
    voiceOrder_[slot] = ++voiceCounter_;
}

double Engine::beatsPerFrame() const { return bpm_ / 60.0 / static_cast<double>(sampleRate_); }

void Engine::findNextEvent(double fromBeat) {
    nextEvent_ = 0;
    if (!sequence_) return;
    const auto& events = sequence_->events;
    const auto found = std::lower_bound(events.begin(), events.end(), fromBeat,
                                        [](const SequenceEvent& e, double beat) { return e.beat < beat; });
    nextEvent_ = static_cast<size_t>(found - events.begin());
}

void Engine::apply(const Command& command) {
    switch (command.type) {
        case Command::Type::Trigger: {
            const float pickupMs = static_cast<float>(nowNs() - command.sentNs) * 1e-6f;
            lastPickupMs_.store(pickupMs, std::memory_order_relaxed);
            if (pickupMs > maxPickupMs_.load(std::memory_order_relaxed)) {
                maxPickupMs_.store(pickupMs, std::memory_order_relaxed);
            }
            startVoice(command.trigger);
            break;
        }
        case Command::Type::Release:
            for (Voice& voice : voices_) {
                if (voice.key == command.key) beginRelease(voice);
            }
            break;
        case Command::Type::StopAll:
            for (Voice& voice : voices_) {
                if (voice.frozen) voice.active = false;  // already silent, so no fade is needed
                beginRelease(voice);
            }
            paused_ = false;
            break;
        case Command::Type::Pause:
            paused_ = command.value != 0.0;
            for (Voice& voice : voices_) {
                if (!voice.active) continue;
                if (paused_) {
                    if (!voice.frozen && voice.releaseLeft == 0) voice.freezeLeft = releaseFrames_;
                } else if (voice.frozen || voice.freezeLeft > 0) {
                    voice.frozen = false;
                    voice.freezeLeft = 0;
                    voice.age = 0;  // fades back in rather than clicking
                }
            }
            break;
        case Command::Type::Retire:
            for (Voice& voice : voices_) {
                if (voice.sample == command.sample) voice.active = false;
            }
            appliedGeneration_.store(command.generation, std::memory_order_release);
            break;
        case Command::Type::SetSequence:
            sequence_ = command.sequence;
            // Resume after what's already been handled. An event that's due right now
            // hasn't fired yet, so the new sequence still has to play it.
            findNextEvent(std::max(beat_ - beatsPerFrame() * 0.5, firedThrough_ + 1e-9));
            appliedGeneration_.store(command.generation, std::memory_order_release);
            break;
        case Command::Type::SetTempo:
            bpm_ = command.value;
            break;
        case Command::Type::SetLoop:
            loopBeats_ = command.value;
            break;
        case Command::Type::Play:
            playing_ = true;
            paused_ = false;
            for (Voice& voice : voices_) {
                if (voice.frozen) voice.active = false;
            }
            beat_ = command.value;
            nextClick_ = std::ceil(beat_);
            firedThrough_ = -std::numeric_limits<double>::infinity();
            suppressed_.fill(0);
            findNextEvent(beat_);
            break;
        case Command::Type::Stop:
            playing_ = false;
            paused_ = false;
            suppressed_.fill(0);
            for (Voice& voice : voices_) {
                if (voice.frozen) voice.active = false;
                beginRelease(voice);
            }
            break;
        case Command::Type::Suppress: {
            // If the playhead has already gone past it, there's nothing left to skip,
            // and suppressing would silence it on the next pass instead.
            double ahead = command.value - beat_;
            if (ahead < 0.0) ahead += loopBeats_;
            if (playing_ && ahead >= 0.0 && ahead <= 1.0) {
                suppressed_[suppressedNext_] = command.id;
                suppressedNext_ = (suppressedNext_ + 1) % suppressed_.size();
            }
            break;
        }
    }
}

void Engine::fireDueEvents(double framesToBeats, double halfFrame) {
    if (!sequence_) return;
    const auto& events = sequence_->events;
    while (nextEvent_ < events.size() && events[nextEvent_].beat <= beat_ + halfFrame) {
        const SequenceEvent& event = events[nextEvent_++];
        firedThrough_ = event.beat;

        const auto skip = std::find(suppressed_.begin(), suppressed_.end(), event.id);
        if (skip != suppressed_.end()) {
            *skip = 0;
            continue;
        }

        Trigger trigger;
        trigger.sample = event.sample;
        trigger.chokeGroup = event.chokeGroup;
        trigger.key = -3;
        trigger.start = event.start;
        trigger.end = event.end;
        trigger.gain = event.gain;
        trigger.reverse = event.reverse;
        trigger.choke = event.choke;
        trigger.loop = event.loop;
        if (event.durationBeats > 0.0) {
            trigger.maxFrames = std::max<size_t>(1, static_cast<size_t>(event.durationBeats / framesToBeats));
        }
        startVoice(trigger);
    }
}

void Engine::mixVoices(float* out, uint32_t frames) {
    for (Voice& voice : voices_) {
        if (!voice.active || voice.frozen) continue;
        const float* source = voice.sample->stereo.data();
        for (uint32_t i = 0; i < frames; ++i) {
            const size_t remaining =
                voice.reverse ? voice.position - voice.start + 1 : voice.end - voice.position;

            float envelope = voice.gain;
            if (voice.age < fadeInFrames_) {
                envelope *= static_cast<float>(voice.age + 1) / static_cast<float>(fadeInFrames_);
            }
            if (remaining < fadeOutFrames_) {
                envelope *= static_cast<float>(remaining) / static_cast<float>(fadeOutFrames_);
            }
            if (voice.framesLeft > 0 && voice.framesLeft < fadeOutFrames_) {
                envelope *= static_cast<float>(voice.framesLeft) / static_cast<float>(fadeOutFrames_);
            }
            if (voice.releaseLeft > 0) {
                envelope *= static_cast<float>(voice.releaseLeft) / static_cast<float>(releaseFrames_);
            }
            if (voice.freezeLeft > 0) {
                envelope *= static_cast<float>(voice.freezeLeft) / static_cast<float>(releaseFrames_);
            }

            out[i * 2] += source[voice.position * 2] * envelope;
            out[i * 2 + 1] += source[voice.position * 2 + 1] * envelope;
            ++voice.age;

            if (voice.releaseLeft > 0 && --voice.releaseLeft == 0) {
                voice.active = false;
                break;
            }
            if (voice.framesLeft > 0 && --voice.framesLeft == 0) {
                voice.active = false;
                break;
            }
            if (remaining == 1) {
                if (!voice.loop) {
                    voice.active = false;
                    break;
                }
                voice.position = voice.reverse ? voice.end - 1 : voice.start;
                voice.age = 0;  // the same tiny fade-in as a new hit, keeping the loop point clean
            } else {
                voice.reverse ? --voice.position : ++voice.position;
            }
            if (voice.freezeLeft > 0 && --voice.freezeLeft == 0) {
                voice.frozen = true;
                break;
            }
        }
    }
}

void Engine::mixClick(float* out, uint32_t frames) {
    constexpr float kClickGain = 0.4f;
    const size_t length = sampleRate_ / 40;  // 25 ms
    const float step = 2.0f * std::numbers::pi_v<float> * clickFrequency_ / static_cast<float>(sampleRate_);
    for (uint32_t i = 0; i < frames && clickLeft_ > 0; ++i) {
        const float envelope = static_cast<float>(clickLeft_) / static_cast<float>(length);
        const float value = std::sin(step * static_cast<float>(clickAge_)) * envelope * kClickGain;
        out[i * 2] += value;
        out[i * 2 + 1] += value;
        ++clickAge_;
        --clickLeft_;
    }
}

void Engine::render(float* out, uint32_t frames) {
    Command command;
    while (queue_.pop(command)) apply(command);

    std::fill(out, out + size_t{frames} * 2, 0.0f);

    const double framesToBeats = beatsPerFrame();
    const double halfFrame = framesToBeats * 0.5;

    // Split the block at each event, click and loop point, so each lands on its exact frame.
    uint32_t done = 0;
    while (done < frames) {
        uint32_t count = frames - done;
        const bool advancing = playing_ && !paused_;
        if (advancing) {
            if (beat_ >= loopBeats_ - halfFrame) {
                beat_ -= loopBeats_;
                firedThrough_ = -std::numeric_limits<double>::infinity();
                nextClick_ = std::ceil(beat_ - halfFrame);
                findNextEvent(beat_ - halfFrame);
            }
            fireDueEvents(framesToBeats, halfFrame);

            if (beat_ >= nextClick_ - halfFrame) {
                // The count-in always clicks, even with the metronome off.
                if (nextClick_ < 0.0 || metronome_.load(std::memory_order_relaxed)) {
                    const double barPosition = std::fmod(std::fmod(nextClick_, 4.0) + 4.0, 4.0);
                    clickFrequency_ = barPosition < 0.5 ? 1760.0f : 1175.0f;
                    clickLeft_ = sampleRate_ / 40;
                    clickAge_ = 0;
                }
                nextClick_ += 1.0;
            }

            double boundary = std::min(loopBeats_, nextClick_);
            if (sequence_ && nextEvent_ < sequence_->events.size()) {
                boundary = std::min(boundary, sequence_->events[nextEvent_].beat);
            }
            // Nearest frame, matching the half-frame window things fire in above. Rounding
            // up instead would let tiny floating-point errors make events a frame late.
            const double toBoundary = std::round((boundary - beat_) / framesToBeats);
            if (toBoundary < static_cast<double>(count)) {
                count = static_cast<uint32_t>(std::max(toBoundary, 1.0));
            }
        }

        mixVoices(out + size_t{done} * 2, count);
        mixClick(out + size_t{done} * 2, count);
        if (advancing) beat_ += static_cast<double>(count) * framesToBeats;
        done += count;
    }

    const float gain = masterGain_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < size_t{frames} * 2; ++i) out[i] = softClip(out[i] * gain);

    bool sounding = false;
    for (int v = 0; v < kMaxVoices; ++v) {
        const Voice& voice = voices_[v];
        positions_[v].store(voice.active ? static_cast<int64_t>(voice.position) : -1, std::memory_order_relaxed);
        voiceSamples_[v].store(voice.active ? voice.sample : nullptr, std::memory_order_relaxed);
        sounding = sounding || (voice.active && !voice.frozen);
    }
    publishedBeat_.store(beat_, std::memory_order_relaxed);
    publishedPlaying_.store(playing_, std::memory_order_relaxed);
    publishedPaused_.store(paused_, std::memory_order_relaxed);
    publishedSounding_.store(sounding, std::memory_order_relaxed);
}

std::shared_ptr<Sample> bounceSequence(std::shared_ptr<const Sequence> sequence, double bpm, double loopBeats,
                                       uint32_t sampleRate) {
    auto result = std::make_shared<Sample>();
    result->sampleRate = sampleRate;
    const auto frames = static_cast<size_t>(std::llround(loopBeats * 60.0 / bpm * sampleRate));
    result->stereo.resize(frames * 2);

    auto engine = std::make_unique<Engine>(sampleRate);
    engine->setMasterGain(1.0f);
    engine->setTempo(bpm);
    engine->setLoopBeats(loopBeats);
    engine->setSequence(std::move(sequence));
    engine->play(0.0);

    constexpr size_t kBlock = 512;
    for (size_t done = 0; done < frames; done += kBlock) {
        engine->render(result->stereo.data() + done * 2, static_cast<uint32_t>(std::min(kBlock, frames - done)));
    }

    // Anything still ringing at the end of the bar fades out rather than clicking.
    const size_t fade = std::min<size_t>(frames, sampleRate / 500);
    for (size_t i = 0; i < fade; ++i) {
        const float gain = static_cast<float>(i) / static_cast<float>(fade);
        result->stereo[(frames - 1 - i) * 2] *= gain;
        result->stereo[(frames - 1 - i) * 2 + 1] *= gain;
    }
    finalizeSample(*result);
    return result;
}
