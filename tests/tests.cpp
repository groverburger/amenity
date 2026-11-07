#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <vector>

#include "engine.h"
#include "sample.h"
#include "slicer.h"
#include "timeline.h"

namespace {

int failures = 0;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (false)

constexpr uint32_t kRate = 48000;

// A ramp from 0 to 1 on the left channel, with its negative on the right.
std::shared_ptr<Sample> rampSample(size_t frames) {
    auto sample = std::make_shared<Sample>();
    sample->sampleRate = kRate;
    sample->stereo.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const float value = static_cast<float>(i) / static_cast<float>(frames);
        sample->stereo[i * 2] = value;
        sample->stereo[i * 2 + 1] = -value;
    }
    finalizeSample(*sample);
    return sample;
}

std::shared_ptr<Sample> constantSample(size_t frames, float value) {
    auto sample = std::make_shared<Sample>();
    sample->sampleRate = kRate;
    sample->stereo.assign(frames * 2, value);
    finalizeSample(*sample);
    return sample;
}

// Decaying noise bursts at known positions, separated by near-silence.
std::vector<float> burstSignal(const std::vector<size_t>& positions, size_t frames) {
    std::mt19937 rng(1234);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    std::vector<float> mono(frames);
    for (float& s : mono) s = noise(rng) * 1e-4f;
    for (size_t position : positions) {
        for (size_t i = 0; i < 4800 && position + i < frames; ++i) {
            mono[position + i] += 0.5f * noise(rng) * std::exp(-static_cast<float>(i) / 800.0f);
        }
    }
    return mono;
}

std::vector<float> renderFrames(Engine& engine, uint32_t frames, uint32_t block = 128) {
    std::vector<float> out(size_t{frames} * 2);
    for (uint32_t done = 0; done < frames;) {
        const uint32_t count = std::min(block, frames - done);
        engine.render(out.data() + size_t{done} * 2, count);
        done += count;
    }
    return out;
}

int activeVoices(const Engine& engine) {
    int active = 0;
    for (int v = 0; v < Engine::kMaxVoices; ++v) active += engine.voicePosition(v) >= 0 ? 1 : 0;
    return active;
}

// The first frame with any sound at or after `from`, or -1.
long firstSound(const std::vector<float>& out, size_t from = 0) {
    for (size_t i = from; i < out.size() / 2; ++i) {
        if (out[i * 2] != 0.0f) return static_cast<long>(i);
    }
    return -1;
}

std::shared_ptr<Sequence> sequenceAt(const std::vector<double>& beats, const Sample& sample, size_t length) {
    auto sequence = std::make_shared<Sequence>();
    uint64_t id = 1;
    for (double beat : beats) {
        SequenceEvent event;
        event.beat = beat;
        event.sample = &sample;
        event.end = length;
        event.id = id++;
        sequence->events.push_back(event);
    }
    return sequence;
}

void testOnsetsFindKnownBursts() {
    std::printf("onsets find known bursts\n");
    const std::vector<size_t> positions{12000, 30000, 48000, 72000, 90000};
    const auto mono = burstSignal(positions, 120000);
    const auto function = computeOnsetFunction(mono);
    const auto markers = pickOnsets(function, mono, kRate, 0.5f);

    // The near-silent lead-in gets no slice of its own, so there's one slice per burst.
    CHECK(markers.size() == positions.size());
    if (markers.size() != positions.size()) return;

    constexpr long kToleranceFrames = 240;  // 5 ms
    for (size_t i = 0; i < positions.size(); ++i) {
        const long error = static_cast<long>(markers[i]) - static_cast<long>(positions[i]);
        CHECK(std::labs(error) <= kToleranceFrames);
        CHECK(markers[i] <= positions[i]);  // must not clip the attack
    }
}

void testAudibleLeadInKeepsItsSlice() {
    std::printf("audible audio before the first onset keeps its slice\n");
    const std::vector<size_t> positions{24000, 60000};
    auto mono = burstSignal(positions, 96000);
    // A quiet steady tone: well above the silence cutoff, with no onset of its own.
    for (size_t i = 0; i < 20000; ++i) {
        mono[i] += 0.08f * std::sin(static_cast<float>(i) * 0.05f) *
                   std::min(1.0f, static_cast<float>(i) / 4000.0f);
    }
    const auto function = computeOnsetFunction(mono);
    const auto markers = pickOnsets(function, mono, kRate, 0.5f);
    CHECK(markers.size() >= 2);
    CHECK(markers.front() == 0);
}

void testSensitivityIsMonotonic() {
    std::printf("higher sensitivity never finds fewer onsets\n");
    std::vector<size_t> positions;
    for (size_t p = 6000; p < 230000; p += 6000) positions.push_back(p);
    auto mono = burstSignal(positions, 240000);
    // Vary the burst levels so that the threshold has something to separate.
    for (size_t i = 0; i < mono.size(); ++i) {
        mono[i] *= (i / 12000) % 2 == 0 ? 1.0f : 0.05f;
    }
    const auto function = computeOnsetFunction(mono);
    size_t previous = 0;
    for (float s = 0.0f; s <= 1.0f; s += 0.1f) {
        const size_t count = pickOnsets(function, mono, kRate, s).size();
        CHECK(count >= previous);
        previous = count;
    }
}

void testShortInputIsSafe() {
    std::printf("input shorter than an analysis frame is safe\n");
    const std::vector<float> mono(100, 0.1f);
    const auto function = computeOnsetFunction(mono);
    const auto markers = pickOnsets(function, mono, kRate, 0.5f);
    CHECK(markers.size() == 1);
    CHECK(markers[0] == 0);
}

void testGrid() {
    std::printf("grid slices are equal and start at zero\n");
    const auto markers = gridSlices(1600, 16);
    CHECK(markers.size() == 16);
    CHECK(markers[0] == 0);
    CHECK(markers[1] == 100);
    CHECK(markers[15] == 1500);
}

void testTriggerPlaysExactFrames() {
    std::printf("trigger plays the exact frame range\n");
    Engine engine(kRate);
    engine.setMasterGain(0.5f);
    const auto sample = rampSample(4800);

    Engine::Trigger trigger;
    trigger.sample = sample.get();
    trigger.start = 1000;
    trigger.end = 2000;
    engine.trigger(trigger);
    const auto out = renderFrames(engine, 1200);

    // Past the short fade-in, output equals source times master gain, on both channels.
    for (size_t i = 100; i < 800; ++i) {
        CHECK(std::fabs(out[i * 2] - sample->stereo[(1000 + i) * 2] * 0.5f) < 1e-6f);
        CHECK(std::fabs(out[i * 2 + 1] - sample->stereo[(1000 + i) * 2 + 1] * 0.5f) < 1e-6f);
    }
    // It stops exactly at the end of the range.
    for (size_t i = 1000; i < 1200; ++i) CHECK(out[i * 2] == 0.0f);
}

void testReverse() {
    std::printf("reverse plays backwards from the end of the range\n");
    Engine engine(kRate);
    engine.setMasterGain(0.5f);
    const auto sample = rampSample(4800);

    Engine::Trigger trigger;
    trigger.sample = sample.get();
    trigger.start = 1000;
    trigger.end = 2000;
    trigger.reverse = true;
    engine.trigger(trigger);
    const auto out = renderFrames(engine, 1000);
    for (size_t i = 100; i < 800; ++i) {
        CHECK(std::fabs(out[i * 2] - sample->stereo[(1999 - i) * 2] * 0.5f) < 1e-6f);
    }
}

void testReverseFromStartOfFileEnds() {
    std::printf("reverse ending at frame zero finishes cleanly\n");
    Engine engine(kRate);
    const auto sample = rampSample(4800);

    Engine::Trigger trigger;
    trigger.sample = sample.get();
    trigger.end = 500;
    trigger.reverse = true;
    engine.trigger(trigger);
    renderFrames(engine, 1000);
    CHECK(activeVoices(engine) == 0);
}

void testChokeOnlyAffectsTheSameSample() {
    std::printf("choke fades the same sample and leaves other samples ringing\n");
    Engine engine(kRate);
    const auto a = rampSample(48000);
    const auto b = rampSample(48000);

    Engine::Trigger trigger;
    trigger.sample = a.get();
    trigger.end = 48000;
    engine.trigger(trigger);
    trigger.sample = b.get();
    engine.trigger(trigger);
    renderFrames(engine, 256);
    CHECK(activeVoices(engine) == 2);

    trigger.sample = a.get();
    trigger.choke = true;
    engine.trigger(trigger);
    renderFrames(engine, 512);  // longer than the 5 ms release
    CHECK(activeVoices(engine) == 2);  // the new hit of a, and b still ringing
}

void testReleaseStopsOnlyThatKey() {
    std::printf("release stops only the matching key\n");
    Engine engine(kRate);
    const auto sample = rampSample(48000);

    Engine::Trigger trigger;
    trigger.sample = sample.get();
    trigger.end = 48000;
    trigger.key = 1;
    engine.trigger(trigger);
    trigger.key = 2;
    engine.trigger(trigger);
    renderFrames(engine, 256);

    engine.release(1);
    renderFrames(engine, 512);
    CHECK(activeVoices(engine) == 1);
}

void testVoiceStealing() {
    std::printf("voice stealing keeps the newest voices\n");
    Engine engine(kRate);
    const auto sample = rampSample(48000);
    Engine::Trigger trigger;
    trigger.sample = sample.get();
    trigger.end = 48000;
    for (int i = 0; i < Engine::kMaxVoices + 5; ++i) {
        trigger.start = static_cast<size_t>(i);
        engine.trigger(trigger);
    }
    renderFrames(engine, 128);
    CHECK(activeVoices(engine) == Engine::kMaxVoices);
}

void testOutputNeverClips() {
    std::printf("many loud voices stay within full scale\n");
    Engine engine(kRate);
    engine.setMasterGain(1.0f);
    const auto sample = constantSample(48000, 1.0f);
    Engine::Trigger trigger;
    trigger.sample = sample.get();
    trigger.end = 48000;
    for (int i = 0; i < 16; ++i) engine.trigger(trigger);
    const auto out = renderFrames(engine, 4096);
    for (float value : out) CHECK(std::fabs(value) <= 1.0f);
}

void testOutOfRangeTriggerIsIgnored() {
    std::printf("out-of-range and sampleless triggers do nothing\n");
    Engine engine(kRate);
    const auto sample = rampSample(1000);
    Engine::Trigger trigger;
    trigger.end = 100;
    engine.trigger(trigger);  // no sample at all
    trigger.sample = sample.get();
    trigger.start = 5000;
    trigger.end = 6000;
    engine.trigger(trigger);
    const auto out = renderFrames(engine, 256);
    for (float value : out) CHECK(value == 0.0f);
}

void testRetiredSampleIsFreedOnlyAfterAcknowledgement() {
    std::printf("a retired sample lives until the audio thread has let go of it\n");
    Engine engine(kRate);
    auto sample = rampSample(48000);
    std::weak_ptr<Sample> watch = sample;

    Engine::Trigger trigger;
    trigger.sample = sample.get();
    trigger.end = 48000;
    engine.trigger(trigger);
    renderFrames(engine, 64);
    CHECK(activeVoices(engine) == 1);

    engine.retireSample(std::move(sample));
    engine.collectRetired();
    CHECK(!watch.expired());  // the audio thread hasn't seen the retirement yet

    renderFrames(engine, 64);
    CHECK(activeVoices(engine) == 0);  // no voice may outlive its sample
    engine.collectRetired();
    CHECK(watch.expired());
}

void testSequenceIsSampleAccurate() {
    std::printf("sequence events land on their exact frame\n");
    Engine engine(kRate);
    engine.setMasterGain(0.5f);
    const auto sample = constantSample(100, 0.5f);

    engine.setTempo(120.0);  // one beat = 24000 frames
    engine.setLoopBeats(16.0);
    engine.setSequence(sequenceAt({0.5, 1.0, 1.25}, *sample, 100));
    engine.play(0.0);
    const auto out = renderFrames(engine, 48000);

    CHECK(firstSound(out) == 12000);
    CHECK(firstSound(out, 12100) == 24000);
    CHECK(firstSound(out, 24100) == 30000);
    CHECK(firstSound(out, 30100) == -1);
}

void testAccuracyDoesNotDependOnBlockSize() {
    std::printf("event timing is the same at any buffer size\n");
    const auto sample = constantSample(100, 0.5f);
    for (uint32_t block : {64u, 128u, 441u, 1024u}) {
        Engine engine(kRate);
        engine.setTempo(120.0);
        engine.setLoopBeats(16.0);
        engine.setSequence(sequenceAt({0.75}, *sample, 100));
        engine.play(0.0);
        const auto out = renderFrames(engine, 24000, block);
        CHECK(firstSound(out) == 18000);
    }
}

void testLoopRepeatsEvents() {
    std::printf("the loop plays each event once per pass\n");
    Engine engine(kRate);
    const auto sample = constantSample(100, 0.5f);
    engine.setTempo(120.0);
    engine.setLoopBeats(1.0);  // loop = 24000 frames
    engine.setSequence(sequenceAt({0.0, 0.5}, *sample, 100));
    engine.play(0.0);
    const auto out = renderFrames(engine, 72000);

    long expected[] = {0, 12000, 24000, 36000, 48000, 60000};
    size_t from = 0;
    for (long frame : expected) {
        CHECK(firstSound(out, from) == frame);
        from = static_cast<size_t>(frame) + 100;
    }
    CHECK(firstSound(out, from) == -1);
}

void testCountInClicksThenStartsOnBeatZero() {
    std::printf("count-in clicks with the metronome off, then starts at beat zero\n");
    Engine engine(kRate);
    engine.setMetronome(false);
    const auto sample = constantSample(100, 0.5f);
    engine.setTempo(120.0);
    engine.setLoopBeats(16.0);
    engine.setSequence(sequenceAt({0.0}, *sample, 100));
    engine.play(-2.0);
    const auto out = renderFrames(engine, 49000);

    CHECK(firstSound(out) >= 0);
    CHECK(firstSound(out) < 10);                         // click on beat -2
    CHECK(firstSound(out, 24000) >= 0);                  // click on beat -1
    CHECK(firstSound(out, 24000) < 24010);
    CHECK(firstSound(out, 47900) == 48000);              // the event on beat 0, with no earlier sound
    CHECK(firstSound(out, 48200) == -1);                 // and no click after the count-in
    CHECK(std::fabs(engine.transportBeat() - 49000.0 / 24000.0 + 2.0) < 1e-6);
}

void testMetronomeClicksOnEachBeat() {
    std::printf("the metronome clicks only when it's on\n");
    Engine engine(kRate);
    engine.setTempo(120.0);
    engine.setLoopBeats(16.0);
    engine.setMetronome(true);
    engine.play(0.0);
    const auto on = renderFrames(engine, 24000 + 100);
    CHECK(firstSound(on) < 10);
    CHECK(firstSound(on, 23000) >= 24000);
    CHECK(firstSound(on, 23000) < 24010);

    Engine silent(kRate);
    silent.setTempo(120.0);
    silent.setLoopBeats(16.0);
    silent.play(0.0);
    CHECK(firstSound(renderFrames(silent, 48000)) == -1);
}

void testSuppressionSkipsExactlyOnePass() {
    std::printf("a suppressed event skips one pass, then plays again\n");
    Engine engine(kRate);
    const auto sample = constantSample(100, 0.5f);
    engine.setTempo(120.0);
    engine.setLoopBeats(1.0);
    engine.play(0.0);
    renderFrames(engine, 128);

    const auto sequence = sequenceAt({0.5}, *sample, 100);
    engine.suppressEvent(sequence->events[0].id, 0.5);
    engine.setSequence(sequence);
    const auto out = renderFrames(engine, 24000);  // passes beat 0.5 once, suppressed
    CHECK(firstSound(out) == -1);
    const auto next = renderFrames(engine, 24000);  // the next pass plays
    CHECK(firstSound(next) >= 0);
}

void testSuppressionOfPassedEventDoesNotSilenceNextPass() {
    std::printf("suppressing an event already behind the playhead changes nothing\n");
    Engine engine(kRate);
    const auto sample = constantSample(100, 0.5f);
    engine.setTempo(120.0);
    engine.setLoopBeats(4.0);
    engine.play(0.0);
    renderFrames(engine, 48000);  // now at beat 2

    const auto sequence = sequenceAt({1.5}, *sample, 100);
    engine.suppressEvent(sequence->events[0].id, 1.5);
    engine.setSequence(sequence);
    // 2 beats to the wrap, then 1.5 more: the event must sound on that first pass.
    const auto out = renderFrames(engine, 24000 * 4);
    CHECK(firstSound(out) >= 0);
}

void testSequenceSwapDoesNotRefireCurrentEvent() {
    std::printf("publishing a new sequence mid-play doesn't retrigger events\n");
    Engine engine(kRate);
    const auto sample = constantSample(100, 0.5f);
    engine.setTempo(120.0);
    engine.setLoopBeats(16.0);
    engine.setSequence(sequenceAt({0.0, 1.0}, *sample, 100));
    engine.play(0.0);
    renderFrames(engine, 24000 + 500);  // beat 1 has fired and finished

    engine.setSequence(sequenceAt({0.0, 1.0, 2.0}, *sample, 100));
    CHECK(firstSound(renderFrames(engine, 20000)) == -1);
}

void testSequenceSwapKeepsAnEventThatIsDueNow() {
    std::printf("publishing a new sequence right before an event doesn't lose it\n");
    // Stop one frame short of the event, then exactly on it, then just after a fired
    // one: each swap must leave the hit sounding exactly once.
    for (uint32_t lead : {23999u, 24000u}) {
        Engine engine(kRate);
        const auto sample = constantSample(100, 0.5f);
        engine.setTempo(120.0);
        engine.setLoopBeats(16.0);
        engine.setSequence(sequenceAt({1.0}, *sample, 100));
        engine.play(0.0);
        renderFrames(engine, lead);

        engine.setSequence(sequenceAt({1.0, 3.0}, *sample, 100));
        const auto out = renderFrames(engine, 2000);
        const long expected = static_cast<long>(24000 - lead);
        CHECK(firstSound(out) == expected);
        CHECK(firstSound(out, static_cast<size_t>(expected) + 100) == -1);
    }
}

void testUnsnappedHitAtPlayheadDoesNotDoubleTrigger() {
    std::printf("an unsnapped hit exactly at the playhead sounds only once\n");
    Engine engine(kRate);
    const auto sample = constantSample(100, 0.5f);
    engine.setTempo(120.0);
    engine.setLoopBeats(16.0);
    engine.play(0.0);
    renderFrames(engine, 6000);

    const auto sequence = sequenceAt({engine.transportBeat()}, *sample, 100);
    engine.suppressEvent(sequence->events[0].id, sequence->events[0].beat);
    engine.setSequence(sequence);
    CHECK(firstSound(renderFrames(engine, 2000)) == -1);
}

void testOldSequenceIsKeptUntilAcknowledged() {
    std::printf("a replaced sequence lives until the audio thread has moved on\n");
    Engine engine(kRate);
    const auto sample = constantSample(100, 0.5f);
    auto first = sequenceAt({0.0}, *sample, 100);
    std::weak_ptr<Sequence> watch = first;
    engine.setSequence(std::move(first));
    renderFrames(engine, 64);

    engine.setSequence(sequenceAt({1.0}, *sample, 100));
    engine.collectRetired();
    CHECK(!watch.expired());
    renderFrames(engine, 64);
    engine.collectRetired();
    CHECK(watch.expired());
}

void testDurationShortensPlayback() {
    std::printf("an event with a duration stops early\n");
    Engine engine(kRate);
    const auto sample = constantSample(48000, 0.5f);
    engine.setTempo(120.0);
    engine.setLoopBeats(16.0);
    auto sequence = sequenceAt({0.0}, *sample, 48000);
    sequence->events[0].durationBeats = 0.25;  // 6000 frames
    engine.setSequence(sequence);
    engine.play(0.0);
    const auto out = renderFrames(engine, 12000);
    CHECK(out[5900 * 2] != 0.0f);
    CHECK(firstSound(out, 6000) == -1);
}

void testBounceMatchesLengthAndTiming() {
    std::printf("a bounce is exactly one loop long, with events in place\n");
    const auto sample = constantSample(100, 0.5f);
    const auto bounced = bounceSequence(sequenceAt({0.5}, *sample, 100), 120.0, 4.0, kRate);
    CHECK(bounced->frames == 96000);
    CHECK(firstSound(bounced->stereo) == 12000);
    CHECK(firstSound(bounced->stereo, 12100) == -1);
}

void testPauseFreezesThenResumesInPlace() {
    std::printf("pause holds a voice and the transport, and resume continues from there\n");
    Engine engine(kRate);
    engine.setMasterGain(1.0f);
    const auto sample = rampSample(48000);
    Engine::Trigger trigger;
    trigger.sample = sample.get();
    trigger.end = 48000;
    engine.trigger(trigger);
    engine.setTempo(120.0);
    engine.setLoopBeats(16.0);
    engine.play(0.0);
    renderFrames(engine, 4800);

    engine.setPaused(true);
    renderFrames(engine, 1000);  // includes the short fade
    CHECK(engine.paused());
    CHECK(!engine.anyVoiceSounding());
    const int64_t heldAt = engine.voicePosition(0);
    const double heldBeat = engine.transportBeat();
    const auto silent = renderFrames(engine, 4800);
    CHECK(firstSound(silent) == -1);
    CHECK(engine.voicePosition(0) == heldAt);  // not moving
    CHECK(engine.transportBeat() == heldBeat);

    engine.setPaused(false);
    const auto resumed = renderFrames(engine, 1000);
    CHECK(!engine.paused());
    CHECK(engine.anyVoiceSounding());
    CHECK(engine.voicePosition(0) == heldAt + 1000);
    CHECK(engine.transportBeat() > heldBeat);
    // Past the fade-in it's the ramp again, picked up from where it stopped.
    CHECK(std::fabs(resumed[500 * 2] - sample->stereo[(static_cast<size_t>(heldAt) + 500) * 2]) < 1e-5f);
}

void testPlayingWhilePausedKeepsThePause() {
    std::printf("new hits sound during a pause, and don't choke what's paused\n");
    Engine engine(kRate);
    const auto sample = rampSample(48000);
    Engine::Trigger trigger;
    trigger.sample = sample.get();
    trigger.end = 48000;
    engine.trigger(trigger);
    renderFrames(engine, 4800);
    engine.setPaused(true);
    renderFrames(engine, 1000);

    trigger.start = 24000;
    trigger.end = 24500;
    trigger.choke = true;
    engine.trigger(trigger);
    CHECK(firstSound(renderFrames(engine, 300)) >= 0);
    renderFrames(engine, 1000);
    CHECK(activeVoices(engine) == 1);  // the paused voice is still there
    CHECK(!engine.anyVoiceSounding());

    engine.setPaused(false);
    renderFrames(engine, 100);
    CHECK(engine.anyVoiceSounding());
}

void testStopDiscardsThePause() {
    std::printf("stopping everything clears a pause, so nothing resumes later\n");
    Engine engine(kRate);
    const auto sample = rampSample(48000);
    Engine::Trigger trigger;
    trigger.sample = sample.get();
    trigger.end = 48000;
    engine.trigger(trigger);
    renderFrames(engine, 1000);
    engine.setPaused(true);
    renderFrames(engine, 1000);
    engine.stopAll();
    renderFrames(engine, 64);
    CHECK(!engine.paused());
    CHECK(activeVoices(engine) == 0);
}

void testLoopRepeatsUntilReleased() {
    std::printf("a looping voice wraps around until it's released\n");
    Engine engine(kRate);
    engine.setMasterGain(1.0f);
    const auto sample = constantSample(1000, 0.5f);
    Engine::Trigger trigger;
    trigger.sample = sample.get();
    trigger.end = 1000;
    trigger.loop = true;
    trigger.key = 7;
    engine.trigger(trigger);
    const auto out = renderFrames(engine, 5000);
    CHECK(activeVoices(engine) == 1);
    CHECK(out[1500 * 2] == 0.5f);  // well into the second pass
    CHECK(out[4500 * 2] == 0.5f);
    engine.release(7);
    renderFrames(engine, 512);
    CHECK(activeVoices(engine) == 0);
}

void testLoopWithLimitStopsOnTime() {
    std::printf("a looping event plays for exactly its duration\n");
    Engine engine(kRate);
    const auto sample = constantSample(1000, 0.5f);
    engine.setTempo(120.0);
    engine.setLoopBeats(16.0);
    auto sequence = sequenceAt({0.0}, *sample, 1000);
    sequence->events[0].loop = true;
    sequence->events[0].durationBeats = 0.25;  // 6000 frames, six passes
    engine.setSequence(sequence);
    engine.play(0.0);
    const auto out = renderFrames(engine, 8000);
    CHECK(out[5000 * 2] != 0.0f);
    CHECK(firstSound(out, 6000) == -1);
    CHECK(activeVoices(engine) == 0);
}

void testChokeGroupSpansProcessedSounds() {
    std::printf("processed sounds from one file still choke each other\n");
    Engine engine(kRate);
    const auto a = rampSample(48000);
    const auto b = rampSample(48000);
    const int group = 0;
    Engine::Trigger trigger;
    trigger.chokeGroup = &group;
    trigger.choke = true;
    trigger.end = 48000;
    trigger.sample = a.get();
    engine.trigger(trigger);
    renderFrames(engine, 128);
    trigger.sample = b.get();
    engine.trigger(trigger);
    renderFrames(engine, 512);
    CHECK(activeVoices(engine) == 1);
}

std::shared_ptr<Sample> sineSample(size_t frames, float frequency, float amplitude = 0.5f) {
    auto sample = std::make_shared<Sample>();
    sample->sampleRate = kRate;
    sample->stereo.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const float value = amplitude * std::sin(2.0f * 3.14159265f * frequency * static_cast<float>(i) / kRate);
        sample->stereo[i * 2] = value;
        sample->stereo[i * 2 + 1] = value;
    }
    finalizeSample(*sample);
    return sample;
}

// Frequency measured from rising zero crossings in the middle of a sound.
double measuredFrequency(const Sample& sample) {
    const size_t from = sample.frames / 4;
    const size_t to = sample.frames * 3 / 4;
    size_t crossings = 0;
    for (size_t i = from + 1; i < to; ++i) {
        if (sample.mono[i - 1] <= 0.0f && sample.mono[i] > 0.0f) ++crossings;
    }
    return static_cast<double>(crossings) * kRate / static_cast<double>(to - from);
}

float rms(const Sample& sample, size_t from, size_t to) {
    double total = 0.0;
    to = std::min(to, sample.frames);
    for (size_t i = from; i < to; ++i) total += static_cast<double>(sample.mono[i]) * static_cast<double>(sample.mono[i]);
    return to > from ? static_cast<float>(std::sqrt(total / static_cast<double>(to - from))) : 0.0f;
}

void testNeutralSettingsRenderNothingNew() {
    std::printf("default settings count as neutral, and any change doesn't\n");
    SoundSettings settings;
    CHECK(settings.isNeutral());
    settings.sourceBpm = 136.0f;  // just knowing the tempo changes nothing
    CHECK(settings.isNeutral());
    settings.pitch = 1.0f;
    CHECK(!settings.isNeutral());
    settings = SoundSettings{};
    settings.loop = true;  // looping with an unknown tempo plays the whole file as it is
    CHECK(settings.isNeutral());
    settings.sourceBpm = 120.0f;  // with a known tempo, it's trimmed to a whole number of bars
    CHECK(!settings.isNeutral());
    settings = SoundSettings{};
    settings.punch = 0.5f;
    settings.applyPunch();
    CHECK(!settings.isNeutral());
    settings.punch = 0.0f;
    settings.applyPunch();
    CHECK(settings.isNeutral());
}

void testRepitchChangesSpeedAndPitchTogether() {
    std::printf("plain pitch changes both pitch and length, like a record\n");
    const auto source = sineSample(48000, 440.0f);
    SoundSettings settings;
    settings.pitch = 12.0f;
    const auto up = renderSound(*source, 0, 48000, settings, 120.0);
    CHECK(up->frames == 24000);
    CHECK(std::fabs(measuredFrequency(*up) - 880.0) < 10.0);
}

void testKeepLengthChangesPitchOnly() {
    std::printf("pitch with keep-length changes pitch and leaves the length alone\n");
    const auto source = sineSample(96000, 440.0f);
    SoundSettings settings;
    settings.pitch = 7.0f;
    settings.keepLength = true;
    const auto up = renderSound(*source, 0, 96000, settings, 120.0);
    CHECK(up->frames == 96000);
    CHECK(std::fabs(measuredFrequency(*up) - 440.0 * std::pow(2.0, 7.0 / 12.0)) < 12.0);
    CHECK(rms(*up, 24000, 72000) > 0.2f);  // not silence
}

void testFitStretchKeepsPitch() {
    std::printf("fitting by stretching changes length to the BPM and keeps pitch\n");
    const auto source = sineSample(96000, 440.0f);
    SoundSettings settings;
    settings.fit = true;
    settings.sourceBpm = 120.0f;
    const auto faster = renderSound(*source, 0, 96000, settings, 180.0);
    CHECK(faster->frames == 64000);
    CHECK(std::fabs(measuredFrequency(*faster) - 440.0) < 12.0);
    CHECK(rms(*faster, 16000, 48000) > 0.2f);
    const auto slower = renderSound(*source, 0, 96000, settings, 60.0);
    CHECK(slower->frames == 192000);
    CHECK(std::fabs(measuredFrequency(*slower) - 440.0) < 12.0);
}

void testFitRepitchChangesPitchWithTempo() {
    std::printf("fitting by re-pitching changes length and pitch together\n");
    const auto source = sineSample(96000, 440.0f);
    SoundSettings settings;
    settings.fit = true;
    settings.fitRepitch = true;
    settings.sourceBpm = 120.0f;
    const auto faster = renderSound(*source, 0, 96000, settings, 240.0);
    CHECK(faster->frames == 48000);
    CHECK(std::fabs(measuredFrequency(*faster) - 880.0) < 10.0);
}

void testFitAndPitchCombine() {
    std::printf("pitch stays independent of a stretch fit\n");
    const auto source = sineSample(96000, 440.0f);
    SoundSettings settings;
    settings.fit = true;
    settings.sourceBpm = 120.0f;
    settings.pitch = 12.0f;
    const auto result = renderSound(*source, 0, 96000, settings, 180.0);
    CHECK(result->frames == 64000);
    CHECK(std::fabs(measuredFrequency(*result) - 880.0) < 15.0);
}

void testStretchHandlesShortSounds() {
    std::printf("stretching a short drum-length sound still produces sound\n");
    // 30 ms, which is shorter than the stretcher's analysis window.
    const auto source = sineSample(1440, 1000.0f);
    SoundSettings settings;
    settings.fit = true;
    settings.sourceBpm = 120.0f;
    const auto result = renderSound(*source, 0, 1440, settings, 100.0);
    CHECK(result->frames == 1728);
    float peak = 0.0f;
    for (float value : result->mono) peak = std::max(peak, std::fabs(value));
    CHECK(peak > 0.05f);
}

void testLoopSnapsToWholeBars() {
    std::printf("a loop trims to a whole number of bars, then fits the BPM exactly\n");
    // 7.5 s at 136 BPM is 17 beats: a bit of tail beyond the 4 bars, like a sampled break.
    const auto source = sineSample(360000, 220.0f);
    SoundSettings settings;
    settings.loop = true;
    settings.sourceBpm = 136.0f;
    const auto trimmed = renderSound(*source, 0, 360000, settings, 136.0);
    const size_t fourBars = static_cast<size_t>(std::llround(16.0 * 60.0 / 136.0 * kRate));
    CHECK(trimmed->frames == fourBars);

    settings.fit = true;
    const auto fitted = renderSound(*source, 0, 360000, settings, 170.0);
    const size_t expected = static_cast<size_t>(std::llround(16.0 * 60.0 / 170.0 * kRate));
    CHECK(std::labs(static_cast<long>(fitted->frames) - static_cast<long>(expected)) <= 1);
}

void testCompressorReducesPeaksOnly() {
    std::printf("the compressor turns down loud parts and leaves quiet ones\n");
    auto source = sineSample(96000, 440.0f, 0.9f);
    for (size_t i = 48000; i < 96000; ++i) {
        source->stereo[i * 2] *= 0.05f;
        source->stereo[i * 2 + 1] *= 0.05f;
    }
    finalizeSample(*source);
    SoundSettings settings;
    settings.compressor = true;
    settings.compThreshold = -12.0f;
    settings.compRatio = 4.0f;
    const auto result = renderSound(*source, 0, 96000, settings, 120.0);
    CHECK(rms(*result, 12000, 40000) < rms(*source, 12000, 40000) * 0.6f);
    CHECK(std::fabs(rms(*result, 84000, 94000) - rms(*source, 84000, 94000)) < 0.003f);
}

void testFilterSeparatesFrequencies() {
    std::printf("tone darkens or thins the sound as expected\n");
    const auto low = sineSample(48000, 100.0f);
    const auto high = sineSample(48000, 8000.0f);
    SoundSettings dark;
    dark.tone = -0.8f;
    dark.applyTone();
    CHECK(dark.filterType == 1);
    CHECK(rms(*renderSound(*low, 0, 48000, dark, 120.0), 4800, 43000) > 0.3f);
    CHECK(rms(*renderSound(*high, 0, 48000, dark, 120.0), 4800, 43000) < 0.02f);
    SoundSettings thin;
    thin.tone = 0.8f;
    thin.applyTone();
    CHECK(thin.filterType == 2);
    CHECK(rms(*renderSound(*low, 0, 48000, thin, 120.0), 4800, 43000) < 0.02f);
    CHECK(rms(*renderSound(*high, 0, 48000, thin, 120.0), 4800, 43000) > 0.3f);
}

void testCrunchQuantizes() {
    std::printf("crunch reduces the bit depth\n");
    const auto source = sineSample(4800, 100.0f);
    SoundSettings settings;
    settings.crushBits = 3.0f;
    const auto result = renderSound(*source, 0, 4800, settings, 120.0);
    std::vector<float> levels;
    for (float value : result->mono) {
        if (std::find(levels.begin(), levels.end(), value) == levels.end()) levels.push_back(value);
    }
    CHECK(levels.size() <= 9);
}

void testEffectsTailExtendsButLoopsDoNot() {
    std::printf("reverb and echo ring on past a hit, but a loop keeps its length\n");
    const auto source = sineSample(9600, 440.0f);
    SoundSettings settings;
    settings.space = 0.6f;
    settings.applySpace();
    const auto ringing = renderSound(*source, 0, 9600, settings, 120.0);
    CHECK(ringing->frames > 9600);
    CHECK(rms(*ringing, 9600, ringing->frames) > 1e-4f);

    SoundSettings echo;
    echo.echo = 0.5f;
    echo.applyEcho();
    const auto repeating = renderSound(*source, 0, 9600, echo, 120.0);
    CHECK(repeating->frames > 9600);
    CHECK(rms(*repeating, 18000, 27000) > 0.02f);  // first repeat, 0.75 beats later

    settings.loop = true;
    const auto looped = renderSound(*source, 0, 9600, settings, 120.0);
    CHECK(looped->frames == 9600);
}

void testPanMovesTheSound() {
    std::printf("pan moves the sound, and center leaves the level alone\n");
    const auto source = constantSample(1000, 0.5f);
    SoundSettings left;
    left.pan = -1.0f;
    const auto panned = renderSound(*source, 0, 1000, left, 120.0);
    CHECK(std::fabs(panned->stereo[500 * 2]) > 0.5f);
    CHECK(std::fabs(panned->stereo[500 * 2 + 1]) < 0.001f);
    SoundSettings center;
    center.level = 0.0f;
    const auto same = renderSound(*source, 0, 1000, center, 120.0);
    CHECK(std::fabs(same->stereo[500 * 2] - 0.5f) < 1e-4f);
}

void testRenderedRangeIsTheRequestedOne() {
    std::printf("a render covers only the sound's own range\n");
    const auto source = rampSample(4800);
    SoundSettings settings;
    settings.level = -0.0001f;  // effectively no change, but not neutral
    const auto result = renderSound(*source, 1000, 2000, settings, 120.0);
    CHECK(result->frames == 1000);
    CHECK(std::fabs(result->stereo[0] - source->stereo[2000]) < 1e-3f);
}

void testTempoEstimate() {
    std::printf("tempo detection finds a steady pulse, and declines without one\n");
    for (double bpm : {90.0, 128.0, 136.0, 174.0}) {
        const size_t frames = kRate * 20;
        std::vector<size_t> hits;
        const double beatFrames = 60.0 / bpm * kRate;
        for (double p = 2000.0; p < static_cast<double>(frames) - 5000.0; p += beatFrames) {
            hits.push_back(static_cast<size_t>(p));
        }
        const auto mono = burstSignal(hits, frames);
        const double found = estimateTempo(computeOnsetFunction(mono), kRate);
        CHECK(std::fabs(found - bpm) <= 1.0);
        if (std::fabs(found - bpm) > 1.0) std::printf("    expected %.1f, got %.1f\n", bpm, found);
    }
    const std::vector<float> tooShort(kRate, 0.1f);
    CHECK(estimateTempo(computeOnsetFunction(tooShort), kRate) == 0.0);
    const std::vector<float> silence(kRate * 10, 0.0f);
    CHECK(estimateTempo(computeOnsetFunction(silence), kRate) == 0.0);
}

void testSnapping() {
    std::printf("snapping rounds to the nearest grid line\n");
    CHECK(snapBeat(1.1, 0.25) == 1.0);
    CHECK(snapBeat(1.13, 0.25) == 1.25);
    CHECK(snapBeat(-0.05, 0.25) == 0.0);
    CHECK(snapBeat(1.137, 0.0) == 1.137);
    CHECK(std::fabs(snapBeat(1.3, 1.0 / 3.0) - 4.0 / 3.0) < 1e-12);
    CHECK(wrapBeat(16.0, 16.0) == 0.0);
    CHECK(wrapBeat(15.5, 16.0) == 15.5);
}

void testAddHitKeepsOrderAndDropsDuplicates() {
    std::printf("hits stay sorted and duplicates on one grid line collapse\n");
    Timeline timeline;
    TimelineEvent event;
    event.path = "a.wav";
    event.end = 10;
    event.beat = 2.0;
    CHECK(addHit(timeline, event) != 0);
    event.beat = 1.0;
    CHECK(addHit(timeline, event) != 0);
    CHECK(addHit(timeline, event) == 0);  // same sound, same beat
    event.path = "b.wav";
    CHECK(addHit(timeline, event) != 0);  // a different sound there is fine
    CHECK(timeline.events.size() == 3);
    CHECK(timeline.events[0].beat == 1.0);
    CHECK(timeline.events[2].beat == 2.0);
}

void testQuantizeAll() {
    std::printf("quantize-all snaps, wraps and merges\n");
    Timeline timeline;
    timeline.bars = 1;
    TimelineEvent event;
    event.path = "a.wav";
    event.end = 10;
    for (double beat : {0.02, 0.98, 1.03, 3.99}) {
        event.beat = beat;
        addHit(timeline, event);
    }
    quantizeAll(timeline, 1.0);
    CHECK(timeline.events.size() == 2);  // 0.98 and 1.03 merge; 3.99 wraps onto 0.02's beat 0
    CHECK(timeline.events[0].beat == 0.0);
    CHECK(timeline.events[1].beat == 1.0);
}

void testSessionRoundTrip() {
    std::printf("a session survives a save and load, including odd file names\n");
    const auto path = std::filesystem::temp_directory_path() / "amenity_test_session.txt";
    Session session;
    session.timeline.bpm = 174.5;
    session.timeline.bars = 8;
    session.quantizeIndex = 3;
    session.metronome = false;
    session.song = "/music/Do's & Don'ts - Song [1].mp3";
    session.kit().keys[7] = "/sounds/snare one.wav";
    session.kits.push_back(Kit{"Jungle kit", {}});
    session.kits[1].keys[2] = "/sounds/amen.wav";
    session.currentKit = 1;
    SoundSettings shaped;
    shaped.pitch = -3.5f;
    shaped.fit = true;
    shaped.sourceBpm = 136.0f;
    shaped.loop = true;
    shaped.filterType = 2;
    shaped.compRatio = 7.5f;
    session.soundSettings[SoundKey{"/sounds/amen.wav", 48000, 96000}] = shaped;
    session.sampleRate = 48000;
    TimelineEvent event;
    event.beat = 1.0 / 3.0;
    event.durationBeats = 0.25;
    event.start = 48000;
    event.end = 96000;
    event.gain = 0.45f;
    event.reverse = true;
    event.choke = true;
    event.path = "/music/Do's & Don'ts - Song [1].mp3";
    addHit(session.timeline, event);
    CHECK(saveSession(session, path.string()));

    Session same;
    CHECK(loadSession(same, path.string(), 48000));
    CHECK(same.timeline.bpm == 174.5);
    CHECK(same.timeline.bars == 8);
    CHECK(same.quantizeIndex == 3);
    CHECK(!same.metronome);
    CHECK(same.song == session.song);
    CHECK(same.kits.size() == 2);
    CHECK(same.currentKit == 1);
    CHECK(same.kits[0].name == "Kit 1");
    CHECK(same.kits[0].keys[7] == "/sounds/snare one.wav");
    CHECK(same.kits[0].keys[0].empty());
    CHECK(same.kits[1].name == "Jungle kit");
    CHECK(same.kits[1].keys[2] == "/sounds/amen.wav");
    {
        const auto found = same.soundSettings.find(SoundKey{"/sounds/amen.wav", 48000, 96000});
        CHECK(found != same.soundSettings.end());
        if (found != same.soundSettings.end()) {
            CHECK(found->second.signature(120.0) == shaped.signature(120.0));
            CHECK(found->second.pitch == -3.5f);
            CHECK(found->second.filterType == 2);
            CHECK(found->second.loop && found->second.fit);
        }
    }
    CHECK(same.timeline.events.size() == 1);
    if (!same.timeline.events.empty()) {
        const TimelineEvent& e = same.timeline.events[0];
        CHECK(e.beat == 1.0 / 3.0);
        CHECK(e.durationBeats == 0.25);
        CHECK(e.start == 48000);
        CHECK(e.end == 96000);
        CHECK(e.gain == 0.45f);
        CHECK(e.reverse && e.choke);
        CHECK(e.path == event.path);
        CHECK(e.id != 0);
    }
    CHECK(same.timeline.nextId > same.timeline.events[0].id);

    // A different device rate rescales positions, so hits still land on the same audio.
    Session rescaled;
    CHECK(loadSession(rescaled, path.string(), 44100));
    CHECK(rescaled.timeline.events[0].start == 44100);
    CHECK(rescaled.timeline.events[0].end == 88200);
    CHECK(rescaled.soundSettings.count(SoundKey{"/sounds/amen.wav", 44100, 88200}) == 1);
    std::filesystem::remove(path);

    Session untouched;
    untouched.timeline.bpm = 99.0;
    CHECK(!loadSession(untouched, "/nonexistent/session", 48000));
    CHECK(untouched.timeline.bpm == 99.0);
}

void testOlderSessionFormatLoads() {
    std::printf("a session from before multiple kits still loads, with its kit intact\n");
    const auto path = std::filesystem::temp_directory_path() / "amenity_test_session_v1.txt";
    {
        std::ofstream file(path);
        file << "amenity-session 1\nrate\t48000\nbpm\t174\nbars\t4\nquantize\t6\nmetronome\t1\n"
             << "song\t/music/amen.wav\nkit\t0\t/sounds/one.wav\nkit\t3\t/sounds/two.wav\n"
             << "event\t1.5\t0\t100\t200\t1\t0\t1\t/music/amen.wav\n";
    }
    Session session;
    int version = 0;
    CHECK(loadSession(session, path.string(), 48000, &version));
    CHECK(version == 1);
    CHECK(session.timeline.bpm == 174.0);
    CHECK(session.kits.size() == 1);
    CHECK(session.currentKit == 0);
    CHECK(session.kits[0].keys[0] == "/sounds/one.wav");
    CHECK(session.kits[0].keys[3] == "/sounds/two.wav");
    CHECK(session.timeline.events.size() == 1);
    std::filesystem::remove(path);
}

void testWavRoundTrip() {
    std::printf("a saved sound reads back identical, and clips copy the right range\n");
    const auto ramp = rampSample(4800);
    const auto clip = extractClip(*ramp, 1000, 2000);
    CHECK(clip->frames == 1000);
    CHECK(clip->stereo[0] == ramp->stereo[2000]);

    const auto path = std::filesystem::temp_directory_path() / "amenity test sound.wav";
    CHECK(writeWav(*clip, path.string()));
    std::string error;
    const auto loaded = loadSample(path.string(), kRate, error);
    CHECK(loaded != nullptr);
    if (loaded) {
        CHECK(loaded->frames == clip->frames);
        CHECK(loaded->stereo == clip->stereo);
    }
    std::filesystem::remove(path);
}

}  // namespace

int main() {
    testOnsetsFindKnownBursts();
    testAudibleLeadInKeepsItsSlice();
    testSensitivityIsMonotonic();
    testShortInputIsSafe();
    testGrid();
    testTriggerPlaysExactFrames();
    testReverse();
    testReverseFromStartOfFileEnds();
    testChokeOnlyAffectsTheSameSample();
    testReleaseStopsOnlyThatKey();
    testVoiceStealing();
    testOutputNeverClips();
    testOutOfRangeTriggerIsIgnored();
    testRetiredSampleIsFreedOnlyAfterAcknowledgement();
    testSequenceIsSampleAccurate();
    testAccuracyDoesNotDependOnBlockSize();
    testLoopRepeatsEvents();
    testCountInClicksThenStartsOnBeatZero();
    testMetronomeClicksOnEachBeat();
    testSuppressionSkipsExactlyOnePass();
    testSuppressionOfPassedEventDoesNotSilenceNextPass();
    testSequenceSwapDoesNotRefireCurrentEvent();
    testSequenceSwapKeepsAnEventThatIsDueNow();
    testUnsnappedHitAtPlayheadDoesNotDoubleTrigger();
    testOldSequenceIsKeptUntilAcknowledged();
    testDurationShortensPlayback();
    testBounceMatchesLengthAndTiming();
    testPauseFreezesThenResumesInPlace();
    testPlayingWhilePausedKeepsThePause();
    testStopDiscardsThePause();
    testLoopRepeatsUntilReleased();
    testLoopWithLimitStopsOnTime();
    testChokeGroupSpansProcessedSounds();
    testNeutralSettingsRenderNothingNew();
    testRepitchChangesSpeedAndPitchTogether();
    testKeepLengthChangesPitchOnly();
    testFitStretchKeepsPitch();
    testFitRepitchChangesPitchWithTempo();
    testFitAndPitchCombine();
    testStretchHandlesShortSounds();
    testLoopSnapsToWholeBars();
    testCompressorReducesPeaksOnly();
    testFilterSeparatesFrequencies();
    testCrunchQuantizes();
    testEffectsTailExtendsButLoopsDoNot();
    testPanMovesTheSound();
    testRenderedRangeIsTheRequestedOne();
    testTempoEstimate();
    testSnapping();
    testAddHitKeepsOrderAndDropsDuplicates();
    testQuantizeAll();
    testSessionRoundTrip();
    testOlderSessionFormatLoads();
    testWavRoundTrip();

    if (failures > 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
