#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "sample.h"
#include "spsc_queue.h"

// One scheduled hit. Sequences are immutable once handed to the engine.
struct SequenceEvent {
    double beat = 0.0;
    double durationBeats = 0.0;  // 0 plays the whole range
    const Sample* sample = nullptr;
    const void* chokeGroup = nullptr;
    size_t start = 0;
    size_t end = 0;
    float gain = 1.0f;
    bool reverse = false;
    bool choke = false;
    bool loop = false;
    uint64_t id = 0;
};

struct Sequence {
    std::vector<SequenceEvent> events;  // sorted by beat
};

// The real-time audio engine. It has no device or window dependency, so tests can
// call render() directly.
//
// Threading: render() runs on the audio thread and never locks or allocates. All
// other methods belong to one control thread, and reach render() through a queue.
//
// Lifetime: any Sample passed to the engine must stay alive until it's been given to
// retireSample(), which frees it on the control thread once the audio thread is done.
class Engine {
public:
    static constexpr int kMaxVoices = 32;

    struct Trigger {
        const Sample* sample = nullptr;
        // Voices in the same group choke each other. Null means the sample itself, but a
        // processed sound has its own buffer and still belongs with its source file.
        const void* chokeGroup = nullptr;
        int key = -1;  // who started it, so release() can find it later
        size_t start = 0;
        size_t end = 0;
        float gain = 1.0f;
        bool reverse = false;
        bool choke = false;    // fade out other voices in this group
        bool loop = false;     // repeat the range until released
        size_t maxFrames = 0;  // stop after this many frames; 0 = no limit
    };

    explicit Engine(uint32_t sampleRate);

    // Control thread. Each returns false if the queue is full.
    bool trigger(const Trigger& trigger);
    bool release(int key);
    bool stopAll();
    bool retireSample(std::shared_ptr<const Sample> sample);
    bool setSequence(std::shared_ptr<const Sequence> sequence);
    bool setTempo(double bpm);
    bool setLoopBeats(double beats);
    bool play(double fromBeat);  // a negative beat counts in with the metronome
    bool stopTransport();
    // Freezes the voices sounding right now and the transport, in place. New triggers still
    // play. Unpausing continues from the same spot; stopAll() and stopTransport() discard it.
    bool setPaused(bool paused);
    // Skips one upcoming pass of an event, for a hit that already sounded live but
    // whose snapped position is still just ahead of the playhead.
    bool suppressEvent(uint64_t id, double beat);
    void setMasterGain(float gain) { masterGain_.store(gain, std::memory_order_relaxed); }
    void setMetronome(bool on) { metronome_.store(on, std::memory_order_relaxed); }
    void collectRetired();

    // Current voice positions in frames, or -1 for an idle voice.
    int64_t voicePosition(int voice) const { return positions_[voice].load(std::memory_order_relaxed); }
    // For comparing only: the sample a voice is playing.
    const Sample* voiceSample(int voice) const { return voiceSamples_[voice].load(std::memory_order_relaxed); }
    // True for a voice held still by a pause.
    bool voiceFrozen(int voice) const { return voiceFrozen_[voice].load(std::memory_order_relaxed); }
    double transportBeat() const { return publishedBeat_.load(std::memory_order_relaxed); }
    bool transportPlaying() const { return publishedPlaying_.load(std::memory_order_relaxed); }
    bool paused() const { return publishedPaused_.load(std::memory_order_relaxed); }
    // True if any voice is sounding, not counting ones frozen by a pause.
    bool anyVoiceSounding() const { return publishedSounding_.load(std::memory_order_relaxed); }
    // Time from trigger() until the audio thread picked it up.
    float lastPickupMs() const { return lastPickupMs_.load(std::memory_order_relaxed); }
    float maxPickupMs() const { return maxPickupMs_.load(std::memory_order_relaxed); }

    // Audio thread. Writes `frames` interleaved stereo frames.
    void render(float* out, uint32_t frames);

private:
    struct Command {
        enum class Type {
            Trigger, Release, StopAll, Retire, SetSequence, SetTempo, SetLoop, Play, Stop, Suppress, Pause
        } type = Type::StopAll;
        Trigger trigger;
        const Sample* sample = nullptr;
        const Sequence* sequence = nullptr;
        uint64_t generation = 0;
        uint64_t id = 0;
        double value = 0.0;
        int key = -1;
        int64_t sentNs = 0;
    };

    struct Voice {
        bool active = false;
        const Sample* sample = nullptr;
        const void* chokeGroup = nullptr;
        int key = -1;
        size_t start = 0;
        size_t end = 0;
        size_t position = 0;
        size_t age = 0;
        size_t releaseLeft = 0;  // 0 = not releasing
        size_t framesLeft = 0;   // 0 = no limit
        size_t freezeLeft = 0;   // counting down to frozen; 0 = not freezing
        float gain = 1.0f;
        bool reverse = false;
        bool loop = false;
        bool frozen = false;
    };

    void apply(const Command& command);
    void startVoice(const Trigger& trigger);
    void beginRelease(Voice& voice);
    void mixVoices(float* out, uint32_t frames);
    void mixClick(float* out, uint32_t frames);
    double beatsPerFrame() const;
    void findNextEvent(double fromBeat);
    void fireDueEvents(double beatsPerFrame, double halfFrame);

    const uint32_t sampleRate_;
    const size_t fadeInFrames_;
    const size_t fadeOutFrames_;
    const size_t releaseFrames_;

    SpscQueue<Command, 1024> queue_;
    std::array<Voice, kMaxVoices> voices_{};
    std::array<std::atomic<int64_t>, kMaxVoices> positions_{};
    std::array<std::atomic<const Sample*>, kMaxVoices> voiceSamples_{};
    std::array<std::atomic<bool>, kMaxVoices> voiceFrozen_{};
    uint64_t voiceCounter_ = 0;
    std::array<uint64_t, kMaxVoices> voiceOrder_{};

    // Transport, owned by the audio thread.
    const Sequence* sequence_ = nullptr;
    size_t nextEvent_ = 0;
    double firedThrough_ = 0.0;  // the latest event beat handled on this pass of the loop
    bool playing_ = false;
    bool paused_ = false;
    double beat_ = 0.0;
    double bpm_ = 120.0;
    double loopBeats_ = 16.0;
    double nextClick_ = 0.0;
    std::array<uint64_t, 16> suppressed_{};
    size_t suppressedNext_ = 0;

    // Metronome click, owned by the audio thread.
    size_t clickLeft_ = 0;
    size_t clickAge_ = 0;
    float clickFrequency_ = 0.0f;

    std::atomic<float> masterGain_{0.8f};
    std::atomic<bool> metronome_{false};
    std::atomic<double> publishedBeat_{0.0};
    std::atomic<bool> publishedPlaying_{false};
    std::atomic<bool> publishedPaused_{false};
    std::atomic<bool> publishedSounding_{false};
    std::atomic<float> lastPickupMs_{0.0f};
    std::atomic<float> maxPickupMs_{0.0f};

    // Objects the audio thread may still be using, each freed on the control thread
    // once the audio thread has acknowledged the generation paired with it.
    std::vector<std::pair<uint64_t, std::shared_ptr<const void>>> retained_;
    std::shared_ptr<const Sequence> currentSequence_;
    uint64_t nextGeneration_ = 1;
    std::atomic<uint64_t> appliedGeneration_{0};
};

// Renders one pass of a sequence offline, exactly as the engine would play it.
std::shared_ptr<Sample> bounceSequence(std::shared_ptr<const Sequence> sequence, double bpm, double loopBeats,
                                       uint32_t sampleRate);
