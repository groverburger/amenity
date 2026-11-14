// Amenity: window, keyboard handling and UI.
#define GL_SILENCE_DEPRECATION
#if defined(__APPLE__)
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <OpenGL/gl3.h>
#else
#include <GLFW/glfw3.h>  // brings in the system OpenGL header, enough for the few calls made here
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "engine.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "platform.h"
#include "miniaudio.h"
#include "sample.h"
#include "slicer.h"
#include "timeline.h"

namespace fs = std::filesystem;

namespace {

constexpr int kColumns = 10;
constexpr int kRows = 4;
constexpr int kMouseVoiceKey = -2;
constexpr int kAuditionVoiceKey = -1;
constexpr float kGhostGain = 0.45f;
// Cmd on macOS and Ctrl elsewhere runs shortcuts; the other one is left to the system.
#if defined(__APPLE__)
constexpr int kShortcutMod = GLFW_MOD_SUPER;
constexpr int kIgnoredMod = GLFW_MOD_CONTROL;
#else
constexpr int kShortcutMod = GLFW_MOD_CONTROL;
constexpr int kIgnoredMod = GLFW_MOD_SUPER;
#endif
constexpr double kCountInBeats = 4.0;
constexpr size_t kMaxUndo = 100;
constexpr int kNoKitDialog = -1;
constexpr int kNewKitDialog = -2;  // other values rename the kit with that index

struct KeyInfo {
    int glfwKey;
    const char* label;
};

// Four rows of ten, in physical keyboard order.
constexpr std::array<KeyInfo, kKeysPerBank> kKeys{{
    {GLFW_KEY_1, "1"}, {GLFW_KEY_2, "2"}, {GLFW_KEY_3, "3"}, {GLFW_KEY_4, "4"}, {GLFW_KEY_5, "5"},
    {GLFW_KEY_6, "6"}, {GLFW_KEY_7, "7"}, {GLFW_KEY_8, "8"}, {GLFW_KEY_9, "9"}, {GLFW_KEY_0, "0"},
    {GLFW_KEY_Q, "Q"}, {GLFW_KEY_W, "W"}, {GLFW_KEY_E, "E"}, {GLFW_KEY_R, "R"}, {GLFW_KEY_T, "T"},
    {GLFW_KEY_Y, "Y"}, {GLFW_KEY_U, "U"}, {GLFW_KEY_I, "I"}, {GLFW_KEY_O, "O"}, {GLFW_KEY_P, "P"},
    {GLFW_KEY_A, "A"}, {GLFW_KEY_S, "S"}, {GLFW_KEY_D, "D"}, {GLFW_KEY_F, "F"}, {GLFW_KEY_G, "G"},
    {GLFW_KEY_H, "H"}, {GLFW_KEY_J, "J"}, {GLFW_KEY_K, "K"}, {GLFW_KEY_L, "L"}, {GLFW_KEY_SEMICOLON, ";"},
    {GLFW_KEY_Z, "Z"}, {GLFW_KEY_X, "X"}, {GLFW_KEY_C, "C"}, {GLFW_KEY_V, "V"}, {GLFW_KEY_B, "B"},
    {GLFW_KEY_N, "N"}, {GLFW_KEY_M, "M"}, {GLFW_KEY_COMMA, ","}, {GLFW_KEY_PERIOD, "."}, {GLFW_KEY_SLASH, "/"},
}};

namespace colors {
const ImU32 kWaveBg = IM_COL32(16, 17, 21, 255);
const ImU32 kSliceTint = IM_COL32(255, 255, 255, 7);
const ImU32 kLastSlice = IM_COL32(120, 220, 255, 24);
const ImU32 kWave = IM_COL32(236, 174, 66, 255);
const ImU32 kWaveQuiet = IM_COL32(236, 174, 66, 120);
const ImU32 kMarker = IM_COL32(255, 255, 255, 80);
const ImU32 kMarkerHot = IM_COL32(120, 220, 255, 255);
const ImU32 kPlayhead = IM_COL32(120, 220, 255, 255);
const ImU32 kLabel = IM_COL32(255, 255, 255, 190);
const ImU32 kCell = IM_COL32(32, 34, 41, 255);
const ImU32 kCellEmpty = IM_COL32(23, 24, 29, 255);
const ImU32 kCellBorder = IM_COL32(58, 61, 72, 255);
const ImU32 kDim = IM_COL32(255, 255, 255, 90);
const ImU32 kGridBar = IM_COL32(255, 255, 255, 70);
const ImU32 kGridBeat = IM_COL32(255, 255, 255, 34);
const ImU32 kGridSub = IM_COL32(255, 255, 255, 13);
const ImU32 kMissing = IM_COL32(230, 80, 70, 255);
const ImU32 kLaneTint = IM_COL32(255, 255, 255, 6);
}  // namespace colors

struct LoadResult {
    std::shared_ptr<const Sample> sample;
    OnsetFunction onsets;
    double bpm = 0.0;
    std::string error;
};

// A sound with non-neutral settings plays from its own processed copy.
struct RenderEntry {
    std::shared_ptr<const Sample> sample;
    std::string signature;
};

struct RenderResult {
    SoundKey key;
    std::shared_ptr<const Sample> sample;
    std::string signature;
};

using LoadedBatch = std::vector<std::pair<std::string, std::shared_ptr<const Sample>>>;

enum class SliceMode { Transients = 0, Grid = 1 };
enum class KeyView { Slices = 0, Kit = 1 };

// What one undo step brings back. Transport settings only change with an undo when the step
// that saved them changed them too, like opening a saved loop.
struct TimelineSnapshot {
    std::vector<TimelineEvent> events;
    std::string editingLoop;
    bool hasTransport = false;
    double bpm = 0.0;
    int bars = 0;
};

// A playable range of a loaded file.
struct SoundRef {
    std::string path;
    const Sample* sample = nullptr;
    size_t start = 0;
    size_t end = 0;
};

struct App {
    std::unique_ptr<Engine> engine;
    ma_device device{};
    bool deviceRunning = false;
    std::string deviceError;
    uint32_t sampleRate = 48000;

    fs::path dataDir;
    fs::path soundsDir;
    fs::path sessionPath;

    // Every loaded file, by path. Songs, saved sounds and anything the timeline uses.
    std::map<std::string, std::shared_ptr<const Sample>> store;
    std::future<LoadedBatch> batch;
    bool startupLoaded = false;

    // The song open for chopping.
    std::shared_ptr<const Sample> sample;
    OnsetFunction onsets;
    std::vector<size_t> markers;
    std::future<LoadResult> loading;
    std::string pendingPath;
    std::string status;

    SliceMode mode = SliceMode::Transients;
    float sensitivity = 0.5f;
    int gridCount = 32;

    bool choke = true;
    bool gate = false;
    float volume = 0.8f;
    int bank = 0;
    KeyView keyView = KeyView::Slices;
    long lastSlice = -1;
    double songBpm = 0.0;
    size_t songCursor = 0;  // where the whole song plays from

    // The sound the shaping panel is showing.
    SoundKey selected;
    bool hasSelected = false;
    bool previewPending = false;

    // Processed copies of sounds, kept up to date by one background job at a time.
    std::map<SoundKey, RenderEntry> renders;
    std::future<RenderResult> renderJob;
    std::vector<std::shared_ptr<const Sample>> fadingRenders;  // replaced, but maybe still ringing

    std::array<bool, kKeysPerBank> held{};
    std::array<double, kKeysPerBank> lastHit{};
    std::array<uint64_t, kKeysPerBank> heldEvent{};  // the recorded event each held key made
    std::array<double, kKeysPerBank> heldBeat{};
    std::array<SoundKey, kKeysPerBank> latched{};  // looping sounds a second press will stop

    double viewStart = 0.0;
    double viewEnd = 1.0;
    int dragMarker = -1;

    std::vector<fs::path> library;
    std::future<std::vector<fs::path>> scanning;
    std::array<char, 128> filter{};
    bool textFocused = false;

    std::vector<fs::path> sounds;
    std::set<fs::path> editableLoops;  // saved loops that have their arrangement with them
    bool selectSoundsTab = false;

    // Name dialog, for saving a new sound or renaming one.
    bool nameRequested = false;
    bool nameFocus = false;
    bool nameOpen = false;
    std::shared_ptr<Sample> pendingSound;
    bool pendingIsLoop = false;  // a bounce of the timeline, saved with its arrangement
    fs::path replacing;          // the saved loop a bounce can update, if any
    bool replaceLoop = true;
    fs::path renaming;
    std::array<char, 96> nameBuffer{};
    bool addToKit = true;
    SoundSettings pendingSettings;
    int kitDialog = kNoKitDialog;

    // Timeline.
    Session session;
    bool recording = false;
    double playStart = 0.0;
    bool placeMode = false;
    double editCursor = 0.0;  // where Place mode puts the next sound
    bool sequenceDirty = false;
    bool sessionDirty = false;
    double lastSave = 0.0;
    std::vector<TimelineSnapshot> undo;
    std::set<uint64_t> selection;
    std::vector<TimelineEvent> clipboard;
    uint64_t dragEvent = 0;  // the hit grabbed by the mouse
    double dragOffset = 0.0;
    bool dragResize = false;
    std::map<uint64_t, double> dragOrigins;  // where each moving hit started
    bool boxSelecting = false;
    ImVec2 boxStart;
    std::set<uint64_t> boxBase;  // what was selected before a Shift-drag added to it
    std::set<std::string> queuedLoads;  // files to load once the current batch is done
};

App* gApp = nullptr;

void audioCallback(ma_device* device, void* output, const void*, ma_uint32 frames) {
    static_cast<Engine*>(device->pUserData)->render(static_cast<float*>(output), frames);
}

void startAudio(App& app) {
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = 0;  // use the device's native rate, avoiding a resampling step
    config.periodSizeInFrames = 128;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.dataCallback = audioCallback;

    if (ma_device_init(nullptr, &config, &app.device) != MA_SUCCESS) {
        app.deviceError = "No audio output device could be opened";
        app.engine = std::make_unique<Engine>(app.sampleRate);
        return;
    }
    app.sampleRate = app.device.sampleRate;
    app.engine = std::make_unique<Engine>(app.sampleRate);
    app.device.pUserData = app.engine.get();
    if (ma_device_start(&app.device) != MA_SUCCESS) {
        app.deviceError = "The audio output device could not be started";
        ma_device_uninit(&app.device);
        return;
    }
    app.deviceRunning = true;
}

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

const Sample* findLoaded(const App& app, const std::string& path) {
    const auto found = app.store.find(path);
    return found == app.store.end() ? nullptr : found->second.get();
}

// ---- Slices ----------------------------------------------------------------

size_t sliceCount(const App& app) { return app.sample ? app.markers.size() : 0; }

size_t sliceEnd(const App& app, size_t slice) {
    return slice + 1 < app.markers.size() ? app.markers[slice + 1] : app.sample->frames;
}

void reslice(App& app) {
    if (!app.sample) return;
    app.markers = app.mode == SliceMode::Transients
                      ? pickOnsets(app.onsets, app.sample->mono, app.sampleRate, app.sensitivity)
                      : gridSlices(app.sample->frames, app.gridCount);
    app.bank = std::min(app.bank, static_cast<int>((app.markers.size() - 1) / kKeysPerBank));
    app.lastSlice = -1;
}

// ---- Timeline --------------------------------------------------------------

void markSessionDirty(App& app) { app.sessionDirty = true; }

const SoundSettings& settingsFor(const App& app, const SoundKey& key) {
    static const SoundSettings kDefault;
    const auto found = app.session.soundSettings.find(key);
    return found == app.session.soundSettings.end() ? kDefault : found->second;
}

struct Playable {
    const Sample* sample = nullptr;
    size_t start = 0;
    size_t end = 0;
};

// What to actually play for a sound: its processed copy if there is one, or else the source.
Playable resolvePlayable(const App& app, const SoundKey& key) {
    const auto render = app.renders.find(key);
    if (render != app.renders.end() && !settingsFor(app, key).isNeutral()) {
        return {render->second.sample.get(), 0, render->second.sample->frames};
    }
    return {findLoaded(app, key.path), key.start, key.end};
}

std::shared_ptr<const Sequence> buildSequence(const App& app) {
    auto sequence = std::make_shared<Sequence>();
    const double loop = app.session.timeline.loopBeats();
    for (const TimelineEvent& event : app.session.timeline.events) {
        const SoundKey key = event.sound();
        const Playable playable = resolvePlayable(app, key);
        if (!playable.sample || event.beat >= loop) continue;
        SequenceEvent out;
        out.beat = event.beat;
        out.durationBeats = event.durationBeats;
        out.sample = playable.sample;
        out.chokeGroup = findLoaded(app, event.path);
        out.start = playable.start;
        out.end = playable.end;
        out.gain = event.gain;
        out.reverse = event.reverse;
        out.choke = event.choke;
        out.loop = settingsFor(app, key).loop;
        out.id = event.id;
        sequence->events.push_back(out);
    }
    // A dragged event can be out of order until it's dropped.
    std::stable_sort(sequence->events.begin(), sequence->events.end(),
                     [](const SequenceEvent& a, const SequenceEvent& b) { return a.beat < b.beat; });
    return sequence;
}

void publishSequence(App& app) {
    app.sequenceDirty = !app.engine->setSequence(buildSequence(app));  // retried next frame if the queue was full
}

void pushUndo(App& app, bool withTransport = false) {
    TimelineSnapshot snapshot;
    snapshot.events = app.session.timeline.events;
    snapshot.editingLoop = app.session.editingLoop;
    snapshot.hasTransport = withTransport;
    snapshot.bpm = app.session.timeline.bpm;
    snapshot.bars = app.session.timeline.bars;
    app.undo.push_back(std::move(snapshot));
    if (app.undo.size() > kMaxUndo) app.undo.erase(app.undo.begin());
}

bool sameEvents(const std::vector<TimelineEvent>& a, const std::vector<TimelineEvent>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].beat != b[i].beat || a[i].durationBeats != b[i].durationBeats) return false;
    }
    return true;
}

void applyTransportSettings(App& app) {
    app.engine->setTempo(app.session.timeline.bpm);
    app.engine->setLoopBeats(app.session.timeline.loopBeats());
    app.engine->setMetronome(app.session.metronome);
}

void performUndo(App& app) {
    if (app.undo.empty()) return;
    TimelineSnapshot& snapshot = app.undo.back();
    app.session.timeline.events = std::move(snapshot.events);
    app.session.editingLoop = std::move(snapshot.editingLoop);
    if (snapshot.hasTransport) {
        app.session.timeline.bpm = snapshot.bpm;
        app.session.timeline.bars = snapshot.bars;
        applyTransportSettings(app);
    }
    app.undo.pop_back();
    app.selection.clear();
    publishSequence(app);
    markSessionDirty(app);
}

// ---- Editing the timeline --------------------------------------------------

double gridBeats(const App& app) { return kQuantizeOptions[static_cast<size_t>(app.session.quantizeIndex)].beats; }

// How long a hit is drawn and plays for, in beats.
double hitLengthBeats(const App& app, const TimelineEvent& event) {
    if (event.durationBeats > 0.0) return event.durationBeats;
    const Playable playable = resolvePlayable(app, event.sound());
    if (!playable.sample) return 0.0;
    return static_cast<double>(playable.end - playable.start) * app.session.timeline.bpm / 60.0 / app.sampleRate;
}

// A hit's length without any reverb or echo tail: how long the sound itself lasts.
double dryLengthBeats(const App& app, const TimelineEvent& event) {
    if (event.durationBeats > 0.0) return event.durationBeats;
    const SoundSettings& s = settingsFor(app, event.sound());
    const double bpm = app.session.timeline.bpm;
    double frames = static_cast<double>(event.end - event.start);
    double sourceBpm = s.sourceBpm;
    if (s.loop && sourceBpm > 0.0) {
        const LoopPlan plan = planLoop(event.end - event.start, sourceBpm, app.sampleRate);
        frames = static_cast<double>(plan.frames);
        sourceBpm = plan.sourceBpm;
    }
    if (s.fit && sourceBpm > 0.0) {
        frames *= sourceBpm / bpm;  // any pitch change on top keeps this length
    } else if (std::fabs(s.pitch) > 1e-3f && !s.keepLength) {
        frames /= std::pow(2.0, static_cast<double>(s.pitch) / 12.0);
    }
    return frames * bpm / 60.0 / app.sampleRate;
}

// How far after the start of some copied hits a copy should go, so it follows on in time.
double clipShift(const App& app, const std::vector<TimelineEvent>& clip) {
    if (clip.empty()) return 0.0;
    const double grid = gridBeats(app);
    const double step = grid > 0.0 ? grid : 0.25;
    if (clip.size() == 1) {
        // One hit goes straight after itself.
        const double length = std::max(dryLengthBeats(app, clip.front()), step);
        return std::ceil(length / step - 1e-9) * step;
    }
    return musicalLength(clip.back().beat + step);
}

void deleteSelection(App& app) {
    if (app.selection.empty()) return;
    pushUndo(app);
    std::erase_if(app.session.timeline.events, [&](const TimelineEvent& e) { return app.selection.count(e.id) > 0; });
    app.selection.clear();
    publishSequence(app);
    markSessionDirty(app);
}

void copySelection(App& app) {
    if (app.selection.empty()) return;
    app.clipboard = copyEvents(app.session.timeline, app.selection);
    app.status = "Copied " + std::to_string(app.clipboard.size()) + " hit(s). " AMENITY_CMD "V pastes them at the cursor.";
}

void cutSelection(App& app) {
    if (app.selection.empty()) return;
    copySelection(app);
    deleteSelection(app);
}

// Pastes at the cursor, then moves the cursor on, so pasting again lays the copies end to end.
void pasteClipboard(App& app) {
    if (app.clipboard.empty()) {
        app.status = "Nothing copied yet. Select hits and press " AMENITY_CMD "C.";
        return;
    }
    pushUndo(app);
    const std::vector<uint64_t> added = pasteEvents(app.session.timeline, app.clipboard, app.editCursor);
    if (added.empty()) {
        app.undo.pop_back();
        app.status = "Nothing to paste there: it's past the end, or those hits are already there";
        return;
    }
    app.selection = std::set<uint64_t>(added.begin(), added.end());
    const double loop = app.session.timeline.loopBeats();
    app.editCursor = std::min(app.editCursor + clipShift(app, app.clipboard), loop);
    if (app.editCursor > loop - 1e-6) app.editCursor = 0.0;
    publishSequence(app);
    markSessionDirty(app);
}

// Copies the selection to just after itself, like repeating a bar.
void duplicateSelection(App& app) {
    if (app.selection.empty()) return;
    const std::vector<TimelineEvent> clip = copyEvents(app.session.timeline, app.selection);
    double first = app.session.timeline.loopBeats();
    for (const TimelineEvent& event : app.session.timeline.events) {
        if (app.selection.count(event.id)) first = std::min(first, event.beat);
    }
    pushUndo(app);
    const std::vector<uint64_t> added = pasteEvents(app.session.timeline, clip, first + clipShift(app, clip));
    if (added.empty()) {
        app.undo.pop_back();
        app.status = "No room after the selection. Add bars to make space.";
        return;
    }
    if (added.size() < clip.size()) app.status = "Some hits went past the end of the timeline and were left out";
    app.selection = std::set<uint64_t>(added.begin(), added.end());
    publishSequence(app);
    markSessionDirty(app);
}

void selectAll(App& app) {
    app.selection.clear();
    for (const TimelineEvent& event : app.session.timeline.events) app.selection.insert(event.id);
}

void clearLatches(App& app) {
    for (SoundKey& key : app.latched) key = SoundKey{};
}

void stopTransport(App& app) {
    app.engine->stopTransport();
    app.recording = false;
    clearLatches(app);
}

void togglePlay(App& app) {
    if (app.engine->transportPlaying()) {
        if (app.engine->paused()) {
            app.engine->setPaused(false);
        } else {
            stopTransport(app);
        }
    } else {
        app.engine->play(app.playStart);
    }
}

void toggleRecord(App& app) {
    if (app.recording) {
        app.recording = false;  // keeps playing, so a take can be heard straight back
        return;
    }
    pushUndo(app);  // one undo removes the whole take
    app.recording = true;
    app.placeMode = false;
    if (!app.engine->transportPlaying()) app.engine->play(app.playStart - kCountInBeats);
}

void playSong(App& app) {
    if (!app.sample) return;
    Engine::Trigger trigger;
    trigger.sample = app.sample.get();
    trigger.key = kAuditionVoiceKey;
    trigger.start = std::min(app.songCursor, app.sample->frames);
    trigger.end = app.sample->frames;
    trigger.choke = true;
    app.engine->trigger(trigger);
}

// One key does it all: pause what's playing, resume what's paused, or else start the song.
void pauseOrPlaySong(App& app) {
    if (app.engine->paused()) {
        app.engine->setPaused(false);
    } else if (app.engine->anyVoiceSounding() || app.engine->transportPlaying()) {
        app.engine->setPaused(true);
    } else {
        playSong(app);
    }
}

// Stores a live hit on the timeline, snapped to the chosen grid.
uint64_t recordHit(App& app, const SoundRef& sound, float gain, bool reverse, int keyIndex) {
    if (!app.recording || !app.engine->transportPlaying() || app.engine->paused()) return 0;

    Timeline& timeline = app.session.timeline;
    const double loop = timeline.loopBeats();
    const double grid = kQuantizeOptions[static_cast<size_t>(app.session.quantizeIndex)].beats;
    const double now = app.engine->transportBeat();
    const double snappedRaw = snapBeat(now, grid);
    if (snappedRaw < 0.0) return 0;  // played during the count-in, too early to belong to beat 0
    const double snapped = wrapBeat(snappedRaw, loop);

    TimelineEvent event;
    event.beat = snapped;
    event.start = sound.start;
    event.end = sound.end;
    event.gain = gain;
    event.reverse = reverse;
    event.choke = app.choke;
    event.path = sound.path;
    // A loop runs to the end of the timeline, unless a second press stops it sooner.
    if (settingsFor(app, event.sound()).loop) event.durationBeats = std::max(loop - snapped, 1.0 / 64.0);
    const uint64_t id = addHit(timeline, std::move(event));
    if (id == 0) return 0;

    // The hit already sounded live. If it snapped forward, or landed right on the
    // playhead, it would sound a second time without this.
    double ahead = snapped - now;
    if (ahead < 0.0) ahead += loop;
    if (ahead <= grid) app.engine->suppressEvent(id, snapped);
    publishSequence(app);
    markSessionDirty(app);

    if (keyIndex >= 0) {
        app.heldEvent[static_cast<size_t>(keyIndex)] = id;
        app.heldBeat[static_cast<size_t>(keyIndex)] = now;
    }
    return id;
}

// How far Place mode moves on after each sound.
double placeStep(const App& app) {
    const double grid = kQuantizeOptions[static_cast<size_t>(app.session.quantizeIndex)].beats;
    return grid > 0.0 ? grid : 0.25;
}

void addPlacedHit(App& app, const SoundKey& key, double beat, float gain, bool reverse) {
    Timeline& timeline = app.session.timeline;
    TimelineEvent event;
    event.beat = beat;
    event.start = key.start;
    event.end = key.end;
    event.gain = gain;
    event.reverse = reverse;
    event.choke = app.choke;
    event.path = key.path;
    if (settingsFor(app, key).loop) event.durationBeats = std::max(timeline.loopBeats() - beat, 1.0 / 64.0);
    pushUndo(app);
    if (addHit(timeline, std::move(event)) == 0) {
        app.undo.pop_back();  // that exact hit was already there, so nothing changed
        return;
    }
    publishSequence(app);
    markSessionDirty(app);
}

void moveEditCursor(App& app, double by) {
    const double loop = app.session.timeline.loopBeats();
    double next = std::fmod(app.editCursor + by, loop);
    if (next < 0.0) next += loop;
    if (next > loop - 1e-6) next = 0.0;
    app.editCursor = next;
}

void playSound(App& app, const SoundRef& sound, int voiceKey, float gain, bool reverse, bool record,
               int keyIndex = -1, bool allowLoop = false) {
    if (!sound.sample) return;
    const SoundKey key{sound.path, sound.start, sound.end};
    app.selected = key;
    app.hasSelected = true;

    const Playable playable = resolvePlayable(app, key);
    Engine::Trigger trigger;
    trigger.sample = playable.sample;
    trigger.chokeGroup = sound.sample;
    trigger.key = voiceKey;
    trigger.start = playable.start;
    trigger.end = playable.end;
    trigger.gain = gain;
    trigger.reverse = reverse;
    trigger.choke = app.choke;
    trigger.loop = allowLoop && settingsFor(app, key).loop;
    app.engine->trigger(trigger);

    if (record && app.placeMode) {
        addPlacedHit(app, key, app.editCursor, gain, reverse);
        moveEditCursor(app, placeStep(app));
    } else if (record) {
        recordHit(app, sound, gain, reverse, keyIndex);
    }
}

SoundRef sliceSound(const App& app, size_t slice) {
    SoundRef sound;
    if (slice >= sliceCount(app)) return sound;
    sound.path = app.sample->path;
    sound.sample = app.sample.get();
    sound.start = app.markers[slice];
    sound.end = sliceEnd(app, slice);
    return sound;
}

SoundRef fileSound(const App& app, const std::string& path) {
    SoundRef sound;
    sound.path = path;
    sound.sample = findLoaded(app, path);
    if (sound.sample) sound.end = sound.sample->frames;
    return sound;
}

// ---- Saved sounds ----------------------------------------------------------

fs::path arrangementPath(const fs::path& sound);

void refreshSounds(App& app) {
    app.sounds.clear();
    app.editableLoops.clear();
    std::error_code error;
    for (fs::directory_iterator it(app.soundsDir, error), end; !error && it != end; it.increment(error)) {
        if (lowercase(it->path().extension().string()) != ".wav") continue;
        app.sounds.push_back(it->path());
        std::error_code existsError;
        if (fs::exists(arrangementPath(it->path()), existsError)) app.editableLoops.insert(it->path());
    }
    std::sort(app.sounds.begin(), app.sounds.end(), [](const fs::path& a, const fs::path& b) {
        return lowercase(a.stem().string()) < lowercase(b.stem().string());
    });
}

// A saved loop keeps its arrangement next to it, so it can go back on the timeline to edit.
constexpr const char* kArrangementExtension = ".amenity-loop";

fs::path arrangementPath(const fs::path& sound) {
    fs::path path = sound;
    path.replace_extension(kArrangementExtension);
    return path;
}

// Makes a user-entered name safe as a file name.
std::string cleanName(const std::string& name) {
    std::string clean;
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 32) continue;
        clean.push_back(c);
    }
    const auto first = clean.find_first_not_of(" .");
    if (first == std::string::npos) return "Untitled";
    const auto last = clean.find_last_not_of(' ');
    return clean.substr(first, last - first + 1);
}

// Never overwrites: a name already in use gets a number added.
fs::path uniqueSoundPath(const App& app, const std::string& name, const fs::path& ignore = {}) {
    fs::path path = app.soundsDir / (name + ".wav");
    for (int n = 2; fs::exists(path) && path != ignore; ++n) {
        path = app.soundsDir / (name + " " + std::to_string(n) + ".wav");
    }
    return path;
}

int firstEmptyKitKey(const App& app) {
    for (int i = 0; i < kKeysPerBank; ++i) {
        if (app.session.kit().keys[static_cast<size_t>(i)].empty()) return i;
    }
    return -1;
}

// Returns where it was saved, or an empty path if it couldn't be.
fs::path saveNewSound(App& app, const std::shared_ptr<Sample>& sound, const std::string& name, bool addToKit,
                      const SoundSettings& settings = {}) {
    std::error_code error;
    fs::create_directories(app.soundsDir, error);
    const fs::path path = uniqueSoundPath(app, cleanName(name));
    sound->path = path.string();
    sound->name = path.filename().string();
    if (!writeWav(*sound, path.string())) {
        app.status = "Couldn't save " + path.string();
        return {};
    }
    app.store[path.string()] = sound;
    const SoundKey key{path.string(), 0, sound->frames};
    app.session.soundSettings[key] = settings;
    app.selected = key;
    app.hasSelected = true;
    markSessionDirty(app);
    refreshSounds(app);
    app.selectSoundsTab = true;
    app.status = "Saved " + path.filename().string();
    if (addToKit) {
        const int emptyKey = firstEmptyKitKey(app);
        if (emptyKey >= 0) {
            app.session.kit().keys[static_cast<size_t>(emptyKey)] = path.string();
            app.status += " to kit key ";
            app.status += kKeys[static_cast<size_t>(emptyKey)].label;
        } else {
            app.status += " (the kit is full)";
        }
    }
    return path;
}

// Writes what's on the timeline next to a saved loop, so it can be opened and edited later.
bool writeArrangement(const App& app, const fs::path& sound) {
    Session arrangement;
    arrangement.timeline = app.session.timeline;
    arrangement.quantizeIndex = app.session.quantizeIndex;
    arrangement.metronome = app.session.metronome;
    arrangement.sampleRate = app.sampleRate;
    for (const TimelineEvent& event : app.session.timeline.events) {
        const auto found = app.session.soundSettings.find(event.sound());
        if (found != app.session.soundSettings.end()) arrangement.soundSettings[found->first] = found->second;
    }
    return saveSession(arrangement, arrangementPath(sound).string());
}

// Loads files in the background. Anything asked for while a batch is running waits for the next.
void loadPaths(App& app, std::set<std::string> paths) {
    for (auto it = paths.begin(); it != paths.end();) {
        it = findLoaded(app, *it) ? paths.erase(it) : std::next(it);
    }
    if (paths.empty()) return;
    if (app.batch.valid()) {
        app.queuedLoads.insert(paths.begin(), paths.end());
        return;
    }
    app.batch = std::async(std::launch::async, [paths, rate = app.sampleRate] {
        LoadedBatch loaded;
        for (const std::string& path : paths) {
            std::string error;
            std::shared_ptr<const Sample> sample = loadSample(path, rate, error);
            if (sample) loaded.emplace_back(path, std::move(sample));
        }
        return loaded;
    });
}

// Puts a saved loop's arrangement back on the timeline. One undo brings back what was there.
void openLoopForEditing(App& app, const fs::path& sound) {
    Session arrangement;
    if (!loadSession(arrangement, arrangementPath(sound).string(), app.sampleRate)) {
        app.status = "There's no arrangement saved with " + sound.stem().string();
        return;
    }
    pushUndo(app, true);
    Timeline& timeline = app.session.timeline;
    timeline.bpm = arrangement.timeline.bpm;
    timeline.bars = arrangement.timeline.bars;
    timeline.events = std::move(arrangement.timeline.events);
    for (TimelineEvent& event : timeline.events) event.id = timeline.nextId++;
    // Sounds keep any shaping they've had since, which their other uses share.
    for (const auto& [key, settings] : arrangement.soundSettings) app.session.soundSettings.emplace(key, settings);
    app.session.editingLoop = sound.string();
    app.selection.clear();
    app.playStart = 0.0;
    app.editCursor = 0.0;

    std::set<std::string> needed;
    for (const TimelineEvent& event : timeline.events) needed.insert(event.path);
    loadPaths(app, needed);
    applyTransportSettings(app);
    publishSequence(app);
    markSessionDirty(app);
    app.status = "Editing " + sound.stem().string() + ". Save loop updates it everywhere it's used; " AMENITY_CMD "Z puts back what was here.";
}

// Moves settings, processed copies and hits from one frame range of a file to another.
void remapSoundRange(App& app, const std::string& path, size_t oldEnd, size_t newEnd) {
    const SoundKey from{path, 0, oldEnd};
    const SoundKey to{path, 0, newEnd};
    const auto settings = app.session.soundSettings.find(from);
    if (settings != app.session.soundSettings.end() && !(from == to)) {
        SoundSettings moved = settings->second;
        app.session.soundSettings.erase(settings);
        app.session.soundSettings[to] = moved;
    }
    const auto remap = [&](std::vector<TimelineEvent>& events) {
        for (TimelineEvent& event : events) {
            if (event.path == path && event.start == 0 && event.end == oldEnd) event.end = newEnd;
        }
    };
    remap(app.session.timeline.events);
    for (auto& snapshot : app.undo) remap(snapshot.events);
    if (app.selected == from) app.selected = to;
}

// Saves over a loop that was saved before. Kits and anything else using it get the new version.
bool replaceSound(App& app, const fs::path& path, const std::shared_ptr<Sample>& sound) {
    const std::string pathText = path.string();
    sound->path = pathText;
    sound->name = path.filename().string();
    const std::string temporary = pathText + ".saving";
    std::error_code error;
    if (!writeWav(*sound, temporary) || (fs::rename(temporary, path, error), error)) {
        fs::remove(temporary, error);
        app.status = "Couldn't save " + pathText;
        return false;
    }

    size_t oldFrames = sound->frames;
    const auto loaded = app.store.find(pathText);
    if (loaded != app.store.end()) {
        oldFrames = loaded->second->frames;
        app.fadingRenders.push_back(loaded->second);  // freed once nothing is playing it
    }
    app.store[pathText] = sound;
    // Processed copies were made from the old version.
    for (auto it = app.renders.begin(); it != app.renders.end();) {
        if (it->first.path == pathText) {
            app.fadingRenders.push_back(std::move(it->second.sample));
            it = app.renders.erase(it);
        } else {
            ++it;
        }
    }
    remapSoundRange(app, pathText, oldFrames, sound->frames);
    app.selected = SoundKey{pathText, 0, sound->frames};
    app.hasSelected = true;
    publishSequence(app);
    markSessionDirty(app);
    app.status = "Updated " + path.filename().string();
    return true;
}

void openNameDialog(App& app, const std::string& suggestion) {
    std::snprintf(app.nameBuffer.data(), app.nameBuffer.size(), "%s", suggestion.c_str());
    app.nameRequested = true;
}

std::shared_ptr<Sample> clipForSlice(const App& app, size_t slice) {
    return extractClip(*app.sample, app.markers[slice], sliceEnd(app, slice));
}

std::string suggestedSliceName(const App& app, size_t slice) {
    return fs::path(app.sample->name).stem().string().substr(0, 40) + " " + std::to_string(slice + 1);
}

void requestBookmark(App& app) {
    if (app.lastSlice < 0 || static_cast<size_t>(app.lastSlice) >= sliceCount(app)) {
        app.status = "Play a slice first, then bookmark it";
        return;
    }
    const auto slice = static_cast<size_t>(app.lastSlice);
    app.pendingSound = clipForSlice(app, slice);
    app.pendingIsLoop = false;
    // The bookmark starts out sounding just like the slice did.
    app.pendingSettings = settingsFor(app, {app.sample->path, app.markers[slice], sliceEnd(app, slice)});
    if (app.pendingSettings.sourceBpm <= 0.0f) app.pendingSettings.sourceBpm = static_cast<float>(app.songBpm);
    app.renaming.clear();
    openNameDialog(app, suggestedSliceName(app, slice));
}

// Bookmarks the song from its first hit to the end, ready to loop and fit to the BPM.
void requestBookmarkSong(App& app) {
    if (!app.sample || app.markers.empty()) return;
    app.pendingSound = extractClip(*app.sample, app.markers.front(), app.sample->frames);
    app.pendingIsLoop = false;
    app.pendingSettings = SoundSettings{};
    app.pendingSettings.sourceBpm = static_cast<float>(app.songBpm);
    app.renaming.clear();
    openNameDialog(app, fs::path(app.sample->name).stem().string().substr(0, 60));
}

std::shared_ptr<Sample> bounceLoop(const App& app) {
    return bounceSequence(buildSequence(app), app.session.timeline.bpm, app.session.timeline.loopBeats(),
                          app.sampleRate);
}

bool timelineUses(const App& app, const std::string& path) {
    return std::any_of(app.session.timeline.events.begin(), app.session.timeline.events.end(),
                       [&](const TimelineEvent& e) { return e.path == path; });
}

void requestSaveLoop(App& app) {
    app.pendingSound = bounceLoop(app);
    app.pendingIsLoop = true;
    app.pendingSettings = SoundSettings{};
    app.pendingSettings.sourceBpm = static_cast<float>(app.session.timeline.bpm);
    app.renaming.clear();
    app.kitDialog = kNoKitDialog;
    // A loop that was opened for editing can be updated in place, unless it's playing in itself.
    const fs::path editing = app.session.editingLoop;
    app.replacing.clear();
    if (!editing.empty() && fs::exists(editing) && !timelineUses(app, editing.string())) {
        app.replacing = editing;
        app.replaceLoop = true;
        openNameDialog(app, editing.stem().string());
        return;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "Loop %d BPM", static_cast<int>(std::lround(app.session.timeline.bpm)));
    openNameDialog(app, name);
}

// Saves the bounce from the name dialog, along with the arrangement that made it.
void savePendingLoop(App& app) {
    fs::path saved;
    if (!app.replacing.empty() && app.replaceLoop) {
        if (replaceSound(app, app.replacing, app.pendingSound)) saved = app.replacing;
    } else {
        saved = saveNewSound(app, app.pendingSound, app.nameBuffer.data(), app.addToKit, app.pendingSettings);
    }
    if (saved.empty()) return;
    if (!writeArrangement(app, saved)) {
        app.status = "Saved the sound, but couldn't save its arrangement for editing later";
        return;
    }
    app.session.editingLoop = saved.string();
    refreshSounds(app);
    markSessionDirty(app);
}

void renameSound(App& app, const fs::path& from, const std::string& name) {
    const fs::path to = uniqueSoundPath(app, cleanName(name), from);
    if (to == from) return;
    std::error_code error;
    fs::rename(from, to, error);
    if (error) {
        app.status = "Couldn't rename " + from.filename().string();
        return;
    }
    const std::string oldPath = from.string();
    const std::string newPath = to.string();
    const auto loaded = app.store.find(oldPath);
    if (loaded != app.store.end()) {
        auto sample = loaded->second;
        app.store.erase(loaded);
        app.store[newPath] = std::move(sample);
    }
    for (Kit& kit : app.session.kits) {
        for (std::string& key : kit.keys) {
            if (key == oldPath) key = newPath;
        }
    }
    // Settings and processed copies follow the sound to its new name.
    const auto renameKeys = [&](auto& table) {
        std::vector<std::pair<SoundKey, typename std::decay_t<decltype(table)>::mapped_type>> moved;
        for (auto it = table.begin(); it != table.end();) {
            if (it->first.path == oldPath) {
                moved.emplace_back(SoundKey{newPath, it->first.start, it->first.end}, std::move(it->second));
                it = table.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& [key, value] : moved) table[key] = std::move(value);
    };
    renameKeys(app.session.soundSettings);
    renameKeys(app.renders);
    if (app.selected.path == oldPath) app.selected.path = newPath;
    for (TimelineEvent& event : app.session.timeline.events) {
        if (event.path == oldPath) event.path = newPath;
    }
    for (auto& snapshot : app.undo) {
        for (TimelineEvent& event : snapshot.events) {
            if (event.path == oldPath) event.path = newPath;
        }
        if (snapshot.editingLoop == oldPath) snapshot.editingLoop = newPath;
    }
    if (app.session.editingLoop == oldPath) app.session.editingLoop = newPath;
    // A saved loop's arrangement goes with it.
    if (fs::exists(arrangementPath(from))) fs::rename(arrangementPath(from), arrangementPath(to), error);
    refreshSounds(app);
    markSessionDirty(app);
}

void trashSound(App& app, const fs::path& path) {
    std::vector<fs::path> files{path};
    // A saved loop's arrangement goes with it.
    if (fs::exists(arrangementPath(path))) files.push_back(arrangementPath(path));
    if (!platform::moveToTrash(files)) {
        app.status = "Couldn't move " + path.filename().string() + " to the " AMENITY_TRASH;
        return;
    }
    if (app.session.editingLoop == path.string()) app.session.editingLoop.clear();
    for (Kit& kit : app.session.kits) {
        for (std::string& key : kit.keys) {
            if (key == path.string()) key.clear();
        }
    }
    // Stays loaded until exit: an undo may still bring back hits that use it.
    refreshSounds(app);
    markSessionDirty(app);
    app.status = "Moved " + path.filename().string() + " to the " AMENITY_TRASH;
}

// ---- Loading ---------------------------------------------------------------

// Frees loaded files nothing refers to any more, so browsing songs doesn't grow memory.
void sweepStore(App& app) {
    std::set<std::string> used;
    if (app.sample) used.insert(app.sample->path);
    for (const Kit& kit : app.session.kits) used.insert(kit.keys.begin(), kit.keys.end());
    for (const TimelineEvent& event : app.session.timeline.events) used.insert(event.path);
    for (const auto& snapshot : app.undo) {
        for (const TimelineEvent& event : snapshot.events) used.insert(event.path);
    }
    for (const fs::path& sound : app.sounds) used.insert(sound.string());

    for (auto it = app.store.begin(); it != app.store.end();) {
        if (!used.count(it->first) && app.engine->retireSample(it->second)) {
            it = app.store.erase(it);
        } else {
            ++it;
        }
    }
}

void requestLoad(App& app, const std::string& path) {
    if (app.loading.valid()) {
        app.pendingPath = path;  // loaded as soon as the current one finishes
        return;
    }
    app.status = "Loading " + fs::path(path).filename().string() + "...";
    std::shared_ptr<const Sample> existing;
    const auto found = app.store.find(path);
    if (found != app.store.end()) existing = found->second;
    app.loading = std::async(std::launch::async, [path, existing, rate = app.sampleRate] {
        LoadResult result;
        std::shared_ptr<const Sample> sample = existing;
        if (!sample) sample = loadSample(path, rate, result.error);
        if (sample) {
            result.onsets = computeOnsetFunction(sample->mono);
            result.bpm = estimateTempo(result.onsets, rate);
            result.sample = std::move(sample);
        }
        return result;
    });
}

void finishLoadIfReady(App& app) {
    if (!app.loading.valid() ||
        app.loading.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }
    LoadResult result = app.loading.get();
    if (result.sample) {
        app.store[result.sample->path] = result.sample;
        app.sample = std::move(result.sample);
        app.onsets = std::move(result.onsets);
        app.songBpm = result.bpm;
        app.songCursor = 0;
        app.viewStart = 0.0;
        app.viewEnd = static_cast<double>(app.sample->frames);
        app.dragMarker = -1;
        app.bank = 0;
        app.session.song = app.sample->path;
        reslice(app);
        app.status.clear();
        markSessionDirty(app);
        sweepStore(app);
    } else {
        app.status = result.error;
    }
    if (!app.pendingPath.empty()) {
        std::string next;
        next.swap(app.pendingPath);
        requestLoad(app, next);
    }
}

std::vector<fs::path> scanLibrary(const fs::path& root, const fs::path& skip) {
    constexpr std::array kExtensions{".wav", ".mp3", ".flac", ".aif", ".aiff", ".m4a", ".aac"};
    std::vector<fs::path> found;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code entryError;
        if (it->path() == skip) {
            it.disable_recursion_pending();  // saved sounds have their own panel
            continue;
        }
        if (!it->is_regular_file(entryError) || entryError) continue;
        const std::string ext = lowercase(it->path().extension().string());
        if (std::find(kExtensions.begin(), kExtensions.end(), ext) != kExtensions.end()) {
            found.push_back(it->path());
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

// Loads saved sounds and everything the remembered kit and timeline need.
void startStartupLoad(App& app) {
    std::set<std::string> paths;
    for (const fs::path& sound : app.sounds) paths.insert(sound.string());
    for (const Kit& kit : app.session.kits) {
        for (const std::string& key : kit.keys) {
            if (!key.empty()) paths.insert(key);
        }
    }
    for (const TimelineEvent& event : app.session.timeline.events) paths.insert(event.path);
    loadPaths(app, std::move(paths));
    if (!app.batch.valid()) app.startupLoaded = true;
}

void finishStartupLoadIfReady(App& app) {
    if (!app.batch.valid() || app.batch.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    for (auto& [path, sample] : app.batch.get()) app.store.emplace(path, std::move(sample));
    app.startupLoaded = true;
    publishSequence(app);
    if (!app.queuedLoads.empty()) {
        std::set<std::string> next;
        next.swap(app.queuedLoads);
        loadPaths(app, std::move(next));
    }

    std::set<std::string> missing;
    for (const Kit& kit : app.session.kits) {
        for (const std::string& key : kit.keys) {
            if (!key.empty() && !findLoaded(app, key)) missing.insert(key);
        }
    }
    for (const TimelineEvent& event : app.session.timeline.events) {
        if (!findLoaded(app, event.path)) missing.insert(event.path);
    }
    if (!missing.empty()) app.status = std::to_string(missing.size()) + " file(s) used by your session couldn't be found";
}

// Keeps the processed copy of every shaped sound up to date, one job at a time, so that
// tweaking a slider or the BPM never holds up the UI or the audio.
void updateRenders(App& app) {
    bool changed = false;
    if (app.renderJob.valid() && app.renderJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        RenderResult result = app.renderJob.get();
        auto& entry = app.renders[result.key];
        if (entry.sample) app.fadingRenders.push_back(std::move(entry.sample));
        entry.sample = std::move(result.sample);
        entry.signature = std::move(result.signature);
        changed = true;
    }

    // Back to neutral settings means back to playing the source.
    for (auto it = app.renders.begin(); it != app.renders.end();) {
        if (settingsFor(app, it->first).isNeutral()) {
            app.fadingRenders.push_back(std::move(it->second.sample));
            it = app.renders.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) publishSequence(app);

    // A replaced copy may still be ringing, and it can finish before it's freed.
    if (!app.sequenceDirty) {
        std::erase_if(app.fadingRenders, [&](const std::shared_ptr<const Sample>& old) {
            for (int v = 0; v < Engine::kMaxVoices; ++v) {
                if (app.engine->voiceSample(v) == old.get()) return false;
            }
            return app.engine->retireSample(old);
        });
    }

    if (app.renderJob.valid()) return;

    const double bpm = app.session.timeline.bpm;
    for (const auto& [key, settings] : app.session.soundSettings) {
        if (settings.isNeutral()) continue;
        const auto source = app.store.find(key.path);
        if (source == app.store.end()) continue;  // a slice of a song that isn't open right now
        std::string signature = settings.signature(bpm);
        const auto current = app.renders.find(key);
        if (current != app.renders.end() && current->second.signature == signature) continue;
        app.renderJob = std::async(std::launch::async, [key = key, settings = settings, bpm, source = source->second,
                                                        signature = std::move(signature)]() mutable {
            RenderResult result;
            result.sample = renderSound(*source, key.start, key.end, settings, bpm);
            result.key = std::move(key);
            result.signature = std::move(signature);
            return result;
        });
        return;
    }

    // Everything's current, so a slider that was just let go of can be heard.
    if (app.previewPending && app.hasSelected) {
        app.previewPending = false;
        SoundRef sound;
        sound.path = app.selected.path;
        sound.sample = findLoaded(app, app.selected.path);
        sound.start = app.selected.start;
        sound.end = app.selected.end;
        playSound(app, sound, kAuditionVoiceKey, 1.0f, false, false);
    }
}

// ---- Keyboard --------------------------------------------------------------

// Ends a hit recorded while a key was held or a loop was latched, at the current moment.
void finishHeldEvent(App& app, size_t k) {
    if (app.heldEvent[k] == 0) return;
    const double loop = app.session.timeline.loopBeats();
    double length = app.engine->transportBeat() - app.heldBeat[k];
    if (length < 0.0) length += loop;
    for (TimelineEvent& event : app.session.timeline.events) {
        if (event.id == app.heldEvent[k]) event.durationBeats = std::max(length, 1.0 / 64.0);
    }
    app.heldEvent[k] = 0;
    publishSequence(app);
    markSessionDirty(app);
}

SoundRef keySound(const App& app, size_t k) {
    if (app.keyView == KeyView::Kit) return fileSound(app, app.session.kit().keys[k]);
    return sliceSound(app, static_cast<size_t>(app.bank) * kKeysPerBank + k);
}

void hitKey(App& app, size_t k, float gain, bool reverse) {
    const SoundRef sound = keySound(app, k);
    if (!sound.sample) return;
    const int index = static_cast<int>(k);
    const SoundKey key{sound.path, sound.start, sound.end};
    if (app.keyView == KeyView::Slices) {
        app.lastSlice = static_cast<long>(app.bank) * kKeysPerBank + index;
    }

    // Looping sounds latch: the first press starts one, and the next press stops it.
    if (!app.latched[k].path.empty()) {
        app.engine->release(index);
        finishHeldEvent(app, k);
        const bool sameSound = app.latched[k] == key;
        app.latched[k] = SoundKey{};
        if (sameSound) {
            app.selected = key;
            app.hasSelected = true;
            return;
        }
    }

    // A hit that chokes its own file has just ended any loop from that file too.
    if (app.choke) {
        for (size_t other = 0; other < app.latched.size(); ++other) {
            if (app.latched[other].path == sound.path) {
                finishHeldEvent(app, other);
                app.latched[other] = SoundKey{};
            }
        }
    }

    app.lastHit[k] = glfwGetTime();
    // Placing a looping sound is about where it goes, not hearing it repeat forever.
    const bool allowLoop = !app.placeMode;
    playSound(app, sound, index, gain, reverse, true, index, allowLoop);
    if (allowLoop && settingsFor(app, key).loop && !app.gate) app.latched[k] = key;
}

void switchKit(App& app, int kit) {
    app.session.currentKit = std::clamp(kit, 0, static_cast<int>(app.session.kits.size()) - 1);
    app.keyView = KeyView::Kit;
    markSessionDirty(app);
}

void onKey(App& app, int key, int action, int mods) {
    // Holding an arrow keeps moving the Place cursor. Every other key ignores auto-repeat.
    if (action != GLFW_RELEASE && !app.textFocused && (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT)) {
        moveEditCursor(app, key == GLFW_KEY_LEFT ? -placeStep(app) : placeStep(app));
        return;
    }
    if (action == GLFW_REPEAT) return;

    int index = -1;
    for (int i = 0; i < kKeysPerBank; ++i) {
        if (kKeys[static_cast<size_t>(i)].glfwKey == key) index = i;
    }

    if (action == GLFW_RELEASE) {
        // Always handled, even if focus has moved to a text box since the key went down.
        if (index >= 0 && app.held[static_cast<size_t>(index)]) {
            const auto k = static_cast<size_t>(index);
            app.held[k] = false;
            if (app.gate) {
                app.engine->release(index);
                finishHeldEvent(app, k);  // with Gate on, a recorded hit keeps the key's length
            } else if (app.latched[k].path.empty()) {
                app.heldEvent[k] = 0;
            }
        }
        return;
    }
    if (app.textFocused || (mods & kIgnoredMod)) return;
    if (mods & kShortcutMod) {
        switch (key) {
            case GLFW_KEY_Z: performUndo(app); break;
            case GLFW_KEY_C: copySelection(app); break;
            case GLFW_KEY_X: cutSelection(app); break;
            case GLFW_KEY_V: pasteClipboard(app); break;
            case GLFW_KEY_D: duplicateSelection(app); break;
            case GLFW_KEY_A: selectAll(app); break;
            default: break;
        }
        return;
    }

    if (index >= 0) {
        const auto k = static_cast<size_t>(index);
        app.held[k] = true;
        hitKey(app, k, (mods & GLFW_MOD_SHIFT) ? kGhostGain : 1.0f, (mods & GLFW_MOD_ALT) != 0);
        return;
    }

    switch (key) {
        case GLFW_KEY_LEFT_BRACKET:
            app.bank = std::max(0, app.bank - 1);
            break;
        case GLFW_KEY_RIGHT_BRACKET:
            if (sliceCount(app) > 0) {
                app.bank = std::min(app.bank + 1, static_cast<int>((sliceCount(app) - 1) / kKeysPerBank));
            }
            break;
        case GLFW_KEY_SPACE:
            togglePlay(app);
            break;
        case GLFW_KEY_ENTER:
            toggleRecord(app);
            break;
        case GLFW_KEY_TAB:
            app.keyView = app.keyView == KeyView::Slices ? KeyView::Kit : KeyView::Slices;
            break;
        case GLFW_KEY_BACKSLASH:
            requestBookmark(app);
            break;
        case GLFW_KEY_GRAVE_ACCENT:
            pauseOrPlaySong(app);
            break;
        case GLFW_KEY_APOSTROPHE:
            app.placeMode = !app.placeMode;
            if (app.placeMode) app.recording = false;
            break;
        case GLFW_KEY_MINUS:
            switchKit(app, app.session.currentKit - 1);
            break;
        case GLFW_KEY_EQUAL:
            switchKit(app, app.session.currentKit + 1);
            break;
        case GLFW_KEY_BACKSPACE:
        case GLFW_KEY_DELETE:
            deleteSelection(app);
            break;
        case GLFW_KEY_ESCAPE:
            stopTransport(app);
            app.engine->stopAll();
            app.selection.clear();
            break;
        default:
            break;
    }
}

void keyCallback(GLFWwindow*, int key, int, int action, int mods) { onKey(*gApp, key, action, mods); }

void dropCallback(GLFWwindow*, int count, const char** paths) {
    if (count > 0) requestLoad(*gApp, paths[0]);
}

// Shortens a name to fit a width, keeping its end. Bookmarks from one song differ only
// at the end of their names, so that's the part worth showing.
std::string fitName(const std::string& name, float width) {
    if (ImGui::CalcTextSize(name.c_str()).x <= width) return name;
    for (size_t cut = 1; cut < name.size(); ++cut) {
        // Stepping over the middle of any multi-byte character.
        if ((static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) continue;
        std::string shortened = ".." + name.substr(cut);
        if (ImGui::CalcTextSize(shortened.c_str()).x <= width) return shortened;
    }
    return "..";
}

// ---- Waveform ----------------------------------------------------------------

void drawColumns(ImDrawList* draw, const Sample& sample, double firstFrame, double framesPerPixel,
                 float left, float width, float midY, float halfHeight, ImU32 color) {
    const int columns = static_cast<int>(width);
    for (int x = 0; x < columns; ++x) {
        const double from = firstFrame + framesPerPixel * x;
        const double to = from + framesPerPixel;
        if (from >= static_cast<double>(sample.frames)) break;
        float low = 1.0f;
        float high = -1.0f;
        if (framesPerPixel >= Sample::kPeakBlock) {
            const auto a = static_cast<size_t>(from) / Sample::kPeakBlock;
            const auto b = std::min(static_cast<size_t>(to) / Sample::kPeakBlock + 1, sample.peakMin.size());
            for (size_t i = a; i < b; ++i) {
                low = std::min(low, sample.peakMin[i]);
                high = std::max(high, sample.peakMax[i]);
            }
        } else {
            const auto a = static_cast<size_t>(from);
            const auto b = std::min(static_cast<size_t>(std::ceil(to)) + 1, sample.frames);
            for (size_t i = a; i < b; ++i) {
                low = std::min(low, sample.mono[i]);
                high = std::max(high, sample.mono[i]);
            }
        }
        if (high < low) continue;
        const float px = left + static_cast<float>(x) + 0.5f;
        draw->AddLine(ImVec2(px, midY - high * halfHeight),
                      ImVec2(px, midY - low * halfHeight + 1.0f), color);
    }
}

void drawWaveform(App& app, ImVec2 size) {
    ImGui::InvisibleButton("##waveform", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p0, p1, colors::kWaveBg, 4.0f);
    if (!app.sample) {
        const char* hint = app.loading.valid() ? "Loading..." : "Pick a song on the left, or drop one here";
        const ImVec2 textSize = ImGui::CalcTextSize(hint);
        draw->AddText(ImVec2((p0.x + p1.x - textSize.x) * 0.5f, (p0.y + p1.y - textSize.y) * 0.5f),
                      colors::kDim, hint);
        return;
    }

    const Sample& sample = *app.sample;
    const float width = p1.x - p0.x;
    const double total = static_cast<double>(sample.frames);
    const auto frameToX = [&](double frame) {
        return p0.x + static_cast<float>((frame - app.viewStart) / (app.viewEnd - app.viewStart)) * width;
    };
    const auto xToFrame = [&](float x) {
        const double t = static_cast<double>((x - p0.x) / width);
        return std::clamp(app.viewStart + t * (app.viewEnd - app.viewStart), 0.0, total);
    };

    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered();

    // Zoom on the vertical scroll axis, pan on the horizontal one.
    if (hovered && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f)) {
        const double span = app.viewEnd - app.viewStart;
        const double anchor = xToFrame(io.MousePos.x);
        const double newSpan = std::clamp(span * std::pow(0.87, static_cast<double>(io.MouseWheel)), 256.0, total);
        double start = anchor - (anchor - app.viewStart) * (newSpan / span);
        start -= static_cast<double>(io.MouseWheelH) * newSpan * 0.03;
        start = std::clamp(start, 0.0, total - newSpan);
        app.viewStart = start;
        app.viewEnd = start + newSpan;
    }

    // Which marker the mouse is near. The first marker can't be moved or removed.
    int nearMarker = -1;
    if (hovered) {
        float bestDistance = 5.0f;
        for (size_t i = 1; i < app.markers.size(); ++i) {
            const float distance = std::fabs(frameToX(static_cast<double>(app.markers[i])) - io.MousePos.x);
            if (distance < bestDistance) {
                bestDistance = distance;
                nearMarker = static_cast<int>(i);
            }
        }
    }

    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && nearMarker < 0) {
        const auto frame = static_cast<size_t>(xToFrame(io.MousePos.x));
        const auto where = std::lower_bound(app.markers.begin(), app.markers.end(), frame);
        if (where == app.markers.end() || *where != frame) app.markers.insert(where, frame);
    } else if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (io.KeyShift) {
            // Shift-click: hear the whole song from here. The ` key pauses and resumes it.
            app.songCursor = static_cast<size_t>(xToFrame(io.MousePos.x));
            playSong(app);
        } else if (nearMarker >= 0) {
            app.dragMarker = nearMarker;
        } else {
            const auto frame = static_cast<size_t>(xToFrame(io.MousePos.x));
            const auto where = std::upper_bound(app.markers.begin(), app.markers.end(), frame);
            if (where != app.markers.begin()) {  // nothing to play in silence before the first slice
                const auto slice = static_cast<size_t>(where - app.markers.begin()) - 1;
                app.lastSlice = static_cast<long>(slice);
                playSound(app, sliceSound(app, slice), kMouseVoiceKey, 1.0f, false, true);
            }
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && nearMarker >= 0) {
        app.markers.erase(app.markers.begin() + nearMarker);
        app.dragMarker = -1;
        nearMarker = -1;
    }
    if (app.dragMarker > 0 && app.dragMarker < static_cast<int>(app.markers.size())) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            constexpr size_t kMinFrames = 64;
            const size_t index = static_cast<size_t>(app.dragMarker);
            const size_t low = app.markers[index - 1] + kMinFrames;
            const size_t next = index + 1 < app.markers.size() ? app.markers[index + 1] : sample.frames;
            if (next > low + kMinFrames) {
                app.markers[index] = std::clamp(static_cast<size_t>(xToFrame(io.MousePos.x)), low, next - kMinFrames);
            }
        } else {
            app.dragMarker = -1;
        }
    }
    if (nearMarker >= 0 || app.dragMarker > 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    draw->PushClipRect(p0, p1, true);

    for (size_t i = 0; i < app.markers.size(); i += 2) {
        const float a = frameToX(static_cast<double>(app.markers[i]));
        const float b = frameToX(static_cast<double>(sliceEnd(app, i)));
        if (b >= p0.x && a <= p1.x) draw->AddRectFilled(ImVec2(a, p0.y), ImVec2(b, p1.y), colors::kSliceTint);
    }
    if (app.lastSlice >= 0 && static_cast<size_t>(app.lastSlice) < app.markers.size()) {
        const auto slice = static_cast<size_t>(app.lastSlice);
        draw->AddRectFilled(ImVec2(frameToX(static_cast<double>(app.markers[slice])), p0.y),
                            ImVec2(frameToX(static_cast<double>(sliceEnd(app, slice))), p1.y), colors::kLastSlice);
    }

    const float midY = (p0.y + p1.y) * 0.5f;
    const float halfHeight = (p1.y - p0.y) * 0.46f;
    drawColumns(draw, sample, app.viewStart, (app.viewEnd - app.viewStart) / static_cast<double>(width), p0.x,
                width, midY, halfHeight, colors::kWave);

    for (size_t i = 0; i < app.markers.size(); ++i) {
        const float x = frameToX(static_cast<double>(app.markers[i]));
        if (x < p0.x - 40.0f || x > p1.x) continue;
        const bool hot = static_cast<int>(i) == nearMarker || static_cast<int>(i) == app.dragMarker;
        draw->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), hot ? colors::kMarkerHot : colors::kMarker, hot ? 2.0f : 1.0f);

        const float nextX = frameToX(static_cast<double>(sliceEnd(app, i)));
        const bool inBank = static_cast<int>(i / kKeysPerBank) == app.bank;
        if (inBank && nextX - x > 14.0f && app.keyView == KeyView::Slices) {
            draw->AddText(ImVec2(x + 4.0f, p0.y + 3.0f), colors::kLabel, kKeys[i % kKeysPerBank].label);
        }
    }

    if (app.songCursor > 0) {
        const float x = frameToX(static_cast<double>(app.songCursor));
        draw->AddTriangleFilled(ImVec2(x - 5.0f, p0.y), ImVec2(x + 5.0f, p0.y), ImVec2(x, p0.y + 8.0f), colors::kPlayhead);
    }
    for (int v = 0; v < Engine::kMaxVoices; ++v) {
        const int64_t position = app.engine->voicePosition(v);
        if (position < 0 || app.engine->voiceSample(v) != app.sample.get()) continue;
        const float x = frameToX(static_cast<double>(position));
        draw->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), colors::kPlayhead, 1.5f);
    }
    draw->PopClipRect();
}

// ---- Keyboard view -----------------------------------------------------------

// Whether a key's sound is playing right now, and how far through it.
struct KeyActivity {
    float progress = -1.0f;  // 0..1, or -1 when it's not playing
    bool paused = false;
};

KeyActivity keyActivity(const App& app, const SoundRef& sound) {
    KeyActivity activity;
    if (!sound.sample || sound.end <= sound.start) return activity;
    const Playable playable = resolvePlayable(app, {sound.path, sound.start, sound.end});
    for (int v = 0; v < Engine::kMaxVoices; ++v) {
        const int64_t position = app.engine->voicePosition(v);
        if (position < 0) continue;
        const Sample* sample = app.engine->voiceSample(v);
        const auto at = static_cast<size_t>(position);
        float progress = -1.0f;
        // Its processed copy, or the source itself, as when the whole song plays through it.
        if (sample == playable.sample && at >= playable.start && at < playable.end) {
            progress = static_cast<float>(at - playable.start) / static_cast<float>(playable.end - playable.start);
        } else if (sample == sound.sample && at >= sound.start && at < sound.end) {
            progress = static_cast<float>(at - sound.start) / static_cast<float>(sound.end - sound.start);
        }
        if (progress < 0.0f) continue;
        const bool frozen = app.engine->voiceFrozen(v);
        // A voice that's playing wins over one that's paused.
        if (activity.progress < 0.0f || (activity.paused && !frozen)) {
            activity.progress = progress;
            activity.paused = frozen;
        }
    }
    return activity;
}

void drawKeyboard(App& app, ImVec2 size) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    constexpr float kGap = 5.0f;
    constexpr float kStagger = 0.35f;  // each row sits this many keys further right, like a real keyboard
    const float keyWidth = (size.x - kGap * (kColumns - 1)) / (kColumns + kStagger * (kRows - 1));
    const float keyHeight = (size.y - kGap * (kRows - 1)) / kRows;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const double now = glfwGetTime();
    const bool kitView = app.keyView == KeyView::Kit;

    for (int i = 0; i < kKeysPerBank; ++i) {
        const auto k = static_cast<size_t>(i);
        const int row = i / kColumns;
        const int column = i % kColumns;
        const ImVec2 a(origin.x + static_cast<float>(row) * kStagger * keyWidth +
                           static_cast<float>(column) * (keyWidth + kGap),
                       origin.y + static_cast<float>(row) * (keyHeight + kGap));
        const ImVec2 b(a.x + keyWidth, a.y + keyHeight);

        const size_t slice = static_cast<size_t>(app.bank) * kKeysPerBank + k;
        const SoundRef sound = keySound(app, k);
        auto& kitKeys = app.session.kit().keys;
        const bool assigned = kitView && !kitKeys[k].empty();
        const bool playable = sound.sample != nullptr;

        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        ImGui::InvisibleButton("##key", ImVec2(keyWidth, keyHeight));
        if (ImGui::IsItemActivated() && playable) hitKey(app, k, 1.0f, false);
        if (kitView) {
            if (assigned && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                kitKeys[k].clear();
                markSessionDirty(app);
            }
            if (assigned && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("KIT_KEY", &i, sizeof(i));
                ImGui::TextUnformatted(fs::path(kitKeys[k]).stem().string().c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SOUND_PATH")) {
                    kitKeys[k] = std::string(static_cast<const char*>(payload->Data));
                    markSessionDirty(app);
                }
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KIT_KEY")) {
                    const int other = *static_cast<const int*>(payload->Data);
                    std::swap(kitKeys[k], kitKeys[static_cast<size_t>(other)]);
                    markSessionDirty(app);
                }
                ImGui::EndDragDropTarget();
            }
        }
        ImGui::PopID();

        draw->AddRectFilled(a, b, playable || assigned ? colors::kCell : colors::kCellEmpty, 4.0f);

        if (playable && sound.end > sound.start) {
            const float innerLeft = a.x + 4.0f;
            const float innerWidth = keyWidth - 8.0f;
            const float thumbTop = a.y + 20.0f;
            const float thumbHeight = std::max(keyHeight - 24.0f, 4.0f);
            drawColumns(draw, *sound.sample, static_cast<double>(sound.start),
                        static_cast<double>(sound.end - sound.start) / static_cast<double>(innerWidth), innerLeft,
                        innerWidth, thumbTop + thumbHeight * 0.5f, thumbHeight * 0.5f, colors::kWaveQuiet);
        }

        // While its sound plays, a key stays lit and shows how far through it is.
        const KeyActivity activity = keyActivity(app, sound);
        const bool sounding = activity.progress >= 0.0f;
        const ImU32 activeColor = activity.paused ? IM_COL32(236, 174, 66, 255) : colors::kMarkerHot;
        if (sounding) {
            draw->AddRectFilled(a, b, activity.paused ? IM_COL32(236, 174, 66, 34) : IM_COL32(120, 220, 255, 40), 4.0f);
            const float x = a.x + 4.0f + (keyWidth - 8.0f) * activity.progress;
            draw->AddLine(ImVec2(x, a.y + 20.0f), ImVec2(x, b.y - 4.0f), activeColor, 1.5f);
            draw->AddRectFilled(ImVec2(a.x + 2.0f, b.y - 4.0f), ImVec2(x, b.y - 2.0f), activeColor, 1.0f);
        }
        const double sinceHit = now - app.lastHit[k];
        if (playable && app.lastHit[k] > 0.0 && sinceHit < 0.25) {
            const int alpha = static_cast<int>(170.0 * (1.0 - sinceHit / 0.25));
            draw->AddRectFilled(a, b, IM_COL32(120, 220, 255, alpha), 4.0f);
        }
        // A latched loop keeps a bright border until it's stopped.
        const bool lit = app.held[k] || !app.latched[k].path.empty() || sounding;
        draw->AddRect(a, b, lit ? activeColor : colors::kCellBorder, 4.0f, 0, lit ? 2.0f : 1.0f);
        draw->AddText(ImVec2(a.x + 6.0f, a.y + 3.0f), playable ? IM_COL32_WHITE : colors::kDim, kKeys[k].label);

        if (kitView && assigned) {
            const std::string name = fitName(fs::path(kitKeys[k]).stem().string(), keyWidth - 24.0f);
            draw->AddText(ImVec2(a.x + 20.0f, a.y + 3.0f), playable ? colors::kLabel : colors::kMissing, name.c_str());
        } else if (!kitView && playable) {
            char text[16];
            std::snprintf(text, sizeof(text), "%zu", slice + 1);
            const ImVec2 textSize = ImGui::CalcTextSize(text);
            draw->AddText(ImVec2(b.x - textSize.x - 6.0f, a.y + 3.0f), colors::kDim, text);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + size.y));
}

// ---- Left panel ----------------------------------------------------------------

void drawSongs(App& app) {
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", "Search ~/Music", app.filter.data(), app.filter.size());
    if (app.scanning.valid()) {
        ImGui::TextDisabled("Scanning...");
    } else if (app.library.empty()) {
        ImGui::TextDisabled("No audio files found");
    }

    const std::string needle = lowercase(app.filter.data());
    ImGui::BeginChild("files");
    for (const fs::path& path : app.library) {
        const std::string filename = path.filename().string();
        if (!needle.empty() && lowercase(path.string()).find(needle) == std::string::npos) continue;
        const bool selected = app.sample && app.sample->path == path.string();
        ImGui::PushID(path.string().c_str());
        if (ImGui::Selectable(filename.c_str(), selected)) requestLoad(app, path.string());
        ImGui::PopID();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", path.string().c_str());
    }
    ImGui::EndChild();
}

void drawSounds(App& app) {
    if (app.sounds.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("No saved sounds yet. Play a slice and press \\ to bookmark it.");
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::TextDisabled("Click to hear, drag onto a kit key");
    ImGui::BeginChild("sounds");
    fs::path toTrash;
    fs::path toEdit;
    for (const fs::path& path : app.sounds) {
        const std::string pathText = path.string();
        const std::string stem = path.stem().string();
        const bool editable = app.editableLoops.count(path) > 0;
        ImGui::PushID(pathText.c_str());
        const float editWidth = editable ? ImGui::CalcTextSize("Edit").x + 12.0f : 0.0f;
        const bool editing = pathText == app.session.editingLoop;
        if (ImGui::Selectable(stem.c_str(), editing, ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(ImGui::GetContentRegionAvail().x - editWidth, 0.0f))) {
            playSound(app, fileSound(app, pathText), kAuditionVoiceKey, 1.0f, false, false);
        }
        if (ImGui::BeginDragDropSource()) {
            app.keyView = KeyView::Kit;  // the keys need to be showing the kit to drop on
            ImGui::SetDragDropPayload("SOUND_PATH", pathText.c_str(), pathText.size() + 1);
            ImGui::TextUnformatted(stem.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginPopupContextItem("sound menu")) {
            if (ImGui::MenuItem("Add to kit")) {
                const int key = firstEmptyKitKey(app);
                if (key >= 0) {
                    app.session.kit().keys[static_cast<size_t>(key)] = pathText;
                    app.keyView = KeyView::Kit;
                    markSessionDirty(app);
                } else {
                    app.status = "The kit is full";
                }
            }
            if (ImGui::MenuItem("Rename...")) {
                app.pendingSound.reset();
                app.kitDialog = kNoKitDialog;
                app.renaming = path;
                openNameDialog(app, stem);
            }
            if (editable && ImGui::MenuItem("Edit on the timeline")) toEdit = path;
            const bool inUse = timelineUses(app, pathText);
            if (ImGui::MenuItem("Move to " AMENITY_TRASH, nullptr, false, !inUse)) toTrash = path;
            if (inUse) ImGui::SetItemTooltip("Used on the timeline");
            ImGui::EndPopup();
        }
        if (editable) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - editWidth + 4.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 0.0f));
            if (ImGui::SmallButton("Edit")) toEdit = path;
            ImGui::PopStyleVar();
            ImGui::SetItemTooltip("Put this loop's hits back on the timeline to change them. " AMENITY_CMD "Z puts back what was there.");
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (!toTrash.empty()) trashSound(app, toTrash);
    if (!toEdit.empty()) openLoopForEditing(app, toEdit);
}

void drawKits(App& app) {
    Session& session = app.session;
    if (ImGui::Button("New kit")) {
        app.kitDialog = kNewKitDialog;
        app.pendingSound.reset();
        app.renaming.clear();
        openNameDialog(app, "Kit " + std::to_string(session.kits.size() + 1));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("- and = switch");
    ImGui::BeginChild("kits");
    int toDelete = -1;
    int toDuplicate = -1;
    for (int i = 0; i < static_cast<int>(session.kits.size()); ++i) {
        const Kit& kit = session.kits[static_cast<size_t>(i)];
        const auto used = std::count_if(kit.keys.begin(), kit.keys.end(), [](const std::string& key) { return !key.empty(); });
        char label[128];
        std::snprintf(label, sizeof(label), "%s (%ld)###kit%d", kit.name.c_str(), static_cast<long>(used), i);
        if (ImGui::Selectable(label, i == session.currentKit)) switchKit(app, i);
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Rename...")) {
                app.kitDialog = i;
                app.pendingSound.reset();
                app.renaming.clear();
                openNameDialog(app, kit.name);
            }
            if (ImGui::MenuItem("Duplicate")) toDuplicate = i;
            // Your sounds aren't affected, only this arrangement of them. It takes two
            // clicks anyway, since kits have no undo.
            if (ImGui::BeginMenu("Delete", session.kits.size() > 1)) {
                if (ImGui::MenuItem("Yes, delete this kit")) toDelete = i;
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    if (toDuplicate >= 0) {
        Kit copy = session.kits[static_cast<size_t>(toDuplicate)];
        copy.name += " copy";
        session.kits.insert(session.kits.begin() + toDuplicate + 1, std::move(copy));
        switchKit(app, toDuplicate + 1);
    }
    if (toDelete >= 0) {
        session.kits.erase(session.kits.begin() + toDelete);
        if (session.currentKit >= toDelete) session.currentKit = std::max(0, session.currentKit - 1);
        clearLatches(app);
        markSessionDirty(app);
    }
}

// A labelled slider. Right-click resets it. Letting go of it plays the result.
bool shapeSlider(App& app, const char* label, float* value, float low, float high, const char* format, float reset,
                 ImGuiSliderFlags flags = 0) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(84.0f);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::PushID(label);
    bool changed = ImGui::SliderFloat("##slider", value, low, high, format, flags);
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && *value != reset) {
        *value = reset;
        changed = true;
        app.previewPending = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) app.previewPending = true;
    ImGui::PopID();
    return changed;
}

// Finds the tempo a sound is at, so that fitting to the BPM has something to fit from.
float guessSourceBpm(const App& app, const SoundKey& key) {
    if (app.sample && key.path == app.sample->path && app.songBpm > 0.0) return static_cast<float>(app.songBpm);
    const Sample* sample = findLoaded(app, key.path);
    if (sample) {
        const double bpm = estimateTempo(computeOnsetFunction(sample->mono), app.sampleRate);
        if (bpm > 0.0) return static_cast<float>(bpm);
    }
    return static_cast<float>(app.session.timeline.bpm);
}

void drawSoundPanel(App& app, ImVec2 size) {
    ImGui::BeginChild("shape", size, ImGuiChildFlags_Borders);
    if (!app.hasSelected) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Play a sound to shape it here: pitch, tempo, looping and effects.");
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        return;
    }

    const SoundKey key = app.selected;
    SoundSettings s = settingsFor(app, key);
    bool changed = false;

    std::string title = fs::path(key.path).stem().string();
    const Sample* source = findLoaded(app, key.path);
    if (source && !(key.start == 0 && key.end >= source->frames)) {
        char where[24];
        std::snprintf(where, sizeof(where), " @%.2fs", static_cast<double>(key.start) / app.sampleRate);
        title += where;
    }
    ImGui::TextUnformatted(title.c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", title.c_str());

    if (ImGui::Button("Reset")) {
        const float bpm = s.sourceBpm;
        s = SoundSettings{};
        s.sourceBpm = bpm;
        changed = true;
        app.previewPending = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Hear") && source) app.previewPending = true;
    if (app.renderJob.valid()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Rendering...");
    }

    // Whole semitones here, with the cents kept separately under More.
    const float wholePitch = std::round(s.pitch);
    const float cents = (s.pitch - wholePitch) * 100.0f;
    float pitch = wholePitch;
    if (shapeSlider(app, "Pitch", &pitch, -24.0f, 24.0f, "%+.0f st", 0.0f)) {
        s.pitch = std::round(pitch) + cents / 100.0f;
        changed = true;
    }

    const auto seedBpm = [&] {
        if (s.sourceBpm <= 0.0f) s.sourceBpm = guessSourceBpm(app, key);
    };

    ImGui::BeginDisabled(s.fit);
    if (ImGui::Checkbox("Keep length", &s.keepLength)) {
        changed = true;
        app.previewPending = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Change the pitch without speeding it up or slowing it down");
    ImGui::SameLine();
    if (ImGui::Checkbox("Loop", &s.loop)) {
        if (s.loop) seedBpm();
        changed = true;
        app.previewPending = false;  // a loop is started from its key, and a second press stops it
    }
    ImGui::SetItemTooltip("Repeats until you press its key again. Trimmed to whole bars if its tempo is known.");

    if (ImGui::Checkbox("Fit to BPM", &s.fit)) {
        if (s.fit) seedBpm();
        changed = true;
        app.previewPending = true;
    }
    ImGui::SetItemTooltip("Match this sound's tempo to the timeline's");
    if (s.fit) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        int fitMode = s.fitRepitch ? 1 : 0;
        constexpr const char* kFitModes[] = {"Keep pitch", "Re-pitch"};
        if (ImGui::Combo("##fitmode", &fitMode, kFitModes, 2)) {
            s.fitRepitch = fitMode == 1;
            changed = true;
            app.previewPending = true;
        }
        ImGui::SetItemTooltip("Keep pitch: time-stretch. Re-pitch: speed it up like a record, changing pitch too.");
    }
    if (s.fit || s.loop) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Its BPM");
        ImGui::SameLine(84.0f);
        const float buttons = ImGui::GetFrameHeight() * 2.0f + 30.0f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttons);
        if (ImGui::DragFloat("##sourcebpm", &s.sourceBpm, 0.25f, 0.0f, 400.0f, s.sourceBpm > 0.0f ? "%.1f" : "unknown",
                             ImGuiSliderFlags_AlwaysClamp)) {
            changed = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) app.previewPending = !s.loop;
        ImGui::SetItemTooltip("The tempo this sound was played at. If it comes out twice as fast or slow, use the buttons.");
        ImGui::SameLine();
        if (ImGui::Button("/2")) {
            s.sourceBpm *= 0.5f;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("x2")) {
            s.sourceBpm *= 2.0f;
            changed = true;
        }
    }

    ImGui::Separator();

    if (shapeSlider(app, "Level", &s.level, -36.0f, 12.0f, "%+.1f dB", 0.0f)) changed = true;
    if (shapeSlider(app, "Tone", &s.tone, -1.0f, 1.0f, s.tone < -0.005f ? "darker %.2f" : (s.tone > 0.005f ? "thinner %.2f" : "neutral"), 0.0f)) {
        s.applyTone();
        changed = true;
    }
    if (shapeSlider(app, "Punch", &s.punch, 0.0f, 1.0f, "%.2f", 0.0f)) {
        s.applyPunch();
        changed = true;
    }
    ImGui::SetItemTooltip("Compression: evens out the level and thickens the sound");
    if (shapeSlider(app, "Drive", &s.drive, 0.0f, 1.0f, "%.2f", 0.0f)) changed = true;
    ImGui::SetItemTooltip("Warm saturation that turns into distortion");
    if (shapeSlider(app, "Crunch", &s.crunch, 0.0f, 1.0f, "%.2f", 0.0f)) {
        s.applyCrunch();
        changed = true;
    }
    ImGui::SetItemTooltip("Lo-fi: fewer bits and a lower sample rate, like an old sampler");
    if (shapeSlider(app, "Space", &s.space, 0.0f, 1.0f, "%.2f", 0.0f)) {
        s.applySpace();
        changed = true;
    }
    ImGui::SetItemTooltip("Reverb");
    if (shapeSlider(app, "Echo", &s.echo, 0.0f, 1.0f, "%.2f", 0.0f)) {
        s.applyEcho();
        changed = true;
    }
    ImGui::SetItemTooltip("Delay, in time with the BPM");

    if (ImGui::CollapsingHeader("More")) {
        ImGui::SeparatorText("Pitch and output");
        float fine = cents;
        if (shapeSlider(app, "Fine tune", &fine, -50.0f, 50.0f, "%+.0f cents", 0.0f)) {
            s.pitch = wholePitch + fine / 100.0f;
            changed = true;
        }
        if (shapeSlider(app, "Pan", &s.pan, -1.0f, 1.0f, "%.2f", 0.0f)) changed = true;
        if (shapeSlider(app, "Fade in", &s.fadeInMs, 0.0f, 2000.0f, "%.0f ms", 0.0f, ImGuiSliderFlags_Logarithmic)) changed = true;
        if (shapeSlider(app, "Fade out", &s.fadeOutMs, 0.0f, 2000.0f, "%.0f ms", 0.0f, ImGuiSliderFlags_Logarithmic)) changed = true;

        ImGui::SeparatorText("Filter");
        constexpr const char* kFilters[] = {"Off", "Low-pass", "High-pass", "Band-pass"};
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##filter", &s.filterType, kFilters, 4)) {
            changed = true;
            app.previewPending = true;
        }
        if (s.filterType != 0) {
            if (shapeSlider(app, "Cutoff", &s.filterCutoff, 20.0f, 20000.0f, "%.0f Hz", 1000.0f, ImGuiSliderFlags_Logarithmic)) changed = true;
            if (shapeSlider(app, "Resonance", &s.filterResonance, 0.3f, 12.0f, "%.2f", 0.707f, ImGuiSliderFlags_Logarithmic)) changed = true;
        }

        ImGui::SeparatorText("Compressor");
        if (ImGui::Checkbox("On##compressor", &s.compressor)) {
            changed = true;
            app.previewPending = true;
        }
        if (s.compressor) {
            if (shapeSlider(app, "Threshold", &s.compThreshold, -60.0f, 0.0f, "%.1f dB", -18.0f)) changed = true;
            if (shapeSlider(app, "Ratio", &s.compRatio, 1.0f, 20.0f, "%.1f:1", 4.0f)) changed = true;
            if (shapeSlider(app, "Attack", &s.compAttackMs, 0.1f, 200.0f, "%.1f ms", 15.0f, ImGuiSliderFlags_Logarithmic)) changed = true;
            if (shapeSlider(app, "Release", &s.compReleaseMs, 5.0f, 1000.0f, "%.0f ms", 120.0f, ImGuiSliderFlags_Logarithmic)) changed = true;
            if (shapeSlider(app, "Makeup", &s.compMakeup, 0.0f, 30.0f, "%+.1f dB", 0.0f)) changed = true;
        }

        ImGui::SeparatorText("Lo-fi");
        if (shapeSlider(app, "Bits", &s.crushBits, 2.0f, 16.0f, "%.0f", 16.0f)) changed = true;
        if (shapeSlider(app, "Downsample", &s.crushDownsample, 1.0f, 32.0f, "%.0fx", 1.0f)) changed = true;

        ImGui::SeparatorText("Echo");
        if (shapeSlider(app, "Mix##echo", &s.delayMix, 0.0f, 1.0f, "%.2f", 0.0f)) changed = true;
        if (shapeSlider(app, "Beats", &s.delayBeats, 0.0625f, 4.0f, "%.3f", 0.75f, ImGuiSliderFlags_Logarithmic)) changed = true;
        if (shapeSlider(app, "Feedback", &s.delayFeedback, 0.0f, 0.95f, "%.2f", 0.35f)) changed = true;

        ImGui::SeparatorText("Reverb");
        if (shapeSlider(app, "Mix##reverb", &s.reverbMix, 0.0f, 1.0f, "%.2f", 0.0f)) changed = true;
        if (shapeSlider(app, "Size", &s.reverbSize, 0.0f, 1.0f, "%.2f", 0.7f)) changed = true;
        if (shapeSlider(app, "Damping", &s.reverbDamping, 0.0f, 1.0f, "%.2f", 0.4f)) changed = true;

        if (app.sample && key.path == app.sample->path && sliceCount(app) > 1) {
            ImGui::Separator();
            if (ImGui::Button("Copy to all slices of this song")) {
                changed = true;
                for (size_t i = 0; i < sliceCount(app); ++i) {
                    app.session.soundSettings[SoundKey{key.path, app.markers[i], sliceEnd(app, i)}] = s;
                }
            }
        }
    }

    if (changed) {
        app.session.soundSettings[key] = s;
        publishSequence(app);  // the Loop setting applies to the timeline straight away
        markSessionDirty(app);
    }
    ImGui::EndChild();
}

void drawLeftPanel(App& app, ImVec2 size) {
    ImGui::BeginChild("left", size, ImGuiChildFlags_None);
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float browserHeight = std::max(size.y * 0.36f, 120.0f);
    ImGui::BeginChild("browser", ImVec2(0.0f, browserHeight), ImGuiChildFlags_None);
    if (ImGui::BeginTabBar("left tabs")) {
        if (ImGui::BeginTabItem("Songs")) {
            drawSongs(app);
            ImGui::EndTabItem();
        }
        const ImGuiTabItemFlags flags = app.selectSoundsTab ? ImGuiTabItemFlags_SetSelected : 0;
        app.selectSoundsTab = false;
        char label[48];
        std::snprintf(label, sizeof(label), "Sounds (%zu)###sounds", app.sounds.size());
        if (ImGui::BeginTabItem(label, nullptr, flags)) {
            drawSounds(app);
            ImGui::EndTabItem();
        }
        std::snprintf(label, sizeof(label), "Kits (%zu)###kits", app.session.kits.size());
        if (ImGui::BeginTabItem(label)) {
            drawKits(app);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    drawSoundPanel(app, ImVec2(0.0f, std::max(size.y - browserHeight - spacing, 80.0f)));
    ImGui::EndChild();
}

// ---- Toolbars ------------------------------------------------------------------

void drawToolbar(App& app) {
    constexpr const char* kModes[] = {"Transients", "Grid"};
    int mode = static_cast<int>(app.mode);
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::Combo("##mode", &mode, kModes, 2)) {
        app.mode = static_cast<SliceMode>(mode);
        reslice(app);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    if (app.mode == SliceMode::Transients) {
        if (ImGui::SliderFloat("##sensitivity", &app.sensitivity, 0.0f, 1.0f, "Sensitivity %.2f")) reslice(app);
    } else {
        if (ImGui::SliderInt("##grid", &app.gridCount, 2, 128, "%d slices")) reslice(app);
    }
    ImGui::SameLine(0.0f, 14.0f);
    ImGui::Checkbox("Choke", &app.choke);
    ImGui::SetItemTooltip("A new hit cuts off other hits from the same file");
    ImGui::SameLine();
    ImGui::Checkbox("Gate", &app.gate);
    ImGui::SetItemTooltip("Sounds stop when you let go of the key");
    ImGui::SameLine(0.0f, 14.0f);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderFloat("##volume", &app.volume, 0.0f, 1.0f, "Volume %.2f")) {
        app.engine->setMasterGain(app.volume);
    }
    ImGui::SameLine(0.0f, 14.0f);
    int view = static_cast<int>(app.keyView);
    if (ImGui::RadioButton("Slices", view == 0)) view = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Kit", view == 1)) view = 1;
    app.keyView = static_cast<KeyView>(view);
    ImGui::SetItemTooltip("Tab switches between the song's slices and your kit");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo("##kitpicker", app.session.kit().name.c_str())) {
        for (int i = 0; i < static_cast<int>(app.session.kits.size()); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(app.session.kits[static_cast<size_t>(i)].name.c_str(), i == app.session.currentKit)) {
                switchKit(app, i);
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Choose a kit. The - and = keys switch too, and the Kits tab has the rest.");
    ImGui::SameLine(0.0f, 14.0f);
    ImGui::BeginDisabled(app.lastSlice < 0);
    if (ImGui::Button("Bookmark")) requestBookmark(app);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Save the last slice you played as a named sound ( \\ )");
    ImGui::SameLine();
    ImGui::BeginDisabled(!app.sample);
    if (ImGui::Button("Bookmark song")) requestBookmarkSong(app);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Save the song or break as one sound, ready to loop and fit to the BPM");
}

void drawNameDialog(App& app) {
    constexpr const char* kTitle = "Name this sound";
    if (app.nameRequested) {
        ImGui::OpenPopup(kTitle);
        // Drop the typed character that opened this, so it doesn't land in the name.
        ImGui::GetIO().InputQueueCharacters.resize(0);
        app.nameRequested = false;
        app.nameFocus = true;
        app.nameOpen = true;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    const bool isRename = !app.renaming.empty();
    const bool isKit = app.kitDialog != kNoKitDialog;
    if (app.nameFocus) {
        ImGui::SetKeyboardFocusHere();
        app.nameFocus = false;
    }
    const bool replacing = app.pendingIsLoop && !app.replacing.empty();
    if (replacing) {
        ImGui::Checkbox("Update the loop you're editing", &app.replaceLoop);
        ImGui::SetItemTooltip("Kits and anything else using it get the new version. Untick to save a new sound.");
    }
    const bool updating = replacing && app.replaceLoop;
    ImGui::BeginDisabled(updating);
    ImGui::SetNextItemWidth(320.0f);
    bool entered = ImGui::InputText("##name", app.nameBuffer.data(), app.nameBuffer.size(),
                                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (!isRename && !isKit) ImGui::Checkbox("Add to kit", &app.addToKit);
    ImGui::EndDisabled();
    if (updating && ImGui::IsKeyPressed(ImGuiKey_Enter)) entered = true;

    const bool save = ImGui::Button(isKit ? "OK" : (isRename ? "Rename" : (updating ? "Update" : "Save"))) || entered;
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (save) {
        if (app.kitDialog == kNewKitDialog) {
            app.session.kits.emplace_back();
            app.session.kits.back().name = cleanName(app.nameBuffer.data());
            switchKit(app, static_cast<int>(app.session.kits.size()) - 1);
        } else if (isKit) {
            app.session.kits[static_cast<size_t>(app.kitDialog)].name = cleanName(app.nameBuffer.data());
            markSessionDirty(app);
        } else if (isRename) {
            renameSound(app, app.renaming, app.nameBuffer.data());
        } else if (app.pendingSound && app.pendingIsLoop) {
            savePendingLoop(app);
        } else if (app.pendingSound) {
            saveNewSound(app, app.pendingSound, app.nameBuffer.data(), app.addToKit, app.pendingSettings);
        }
    }
    if (save || cancel) {
        app.pendingSound.reset();
        app.pendingIsLoop = false;
        app.replacing.clear();
        app.kitDialog = kNoKitDialog;
        app.renaming.clear();
        app.nameOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---- Timeline view ---------------------------------------------------------------

void drawTransport(App& app) {
    Timeline& timeline = app.session.timeline;
    const bool playing = app.engine->transportPlaying();

    const bool paused = app.engine->paused();
    if (ImGui::Button(playing && !paused ? "Stop" : "Play", ImVec2(52.0f, 0.0f))) togglePlay(app);
    ImGui::SetItemTooltip("Space");
    ImGui::SameLine();
    if (paused) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(70, 130, 170, 255));
    if (ImGui::Button(paused ? "Resume" : "Pause", ImVec2(64.0f, 0.0f))) pauseOrPlaySong(app);
    if (paused) ImGui::PopStyleColor();
    ImGui::SetItemTooltip("` pauses and resumes everything that's playing. With nothing playing, it plays the song.");
    ImGui::SameLine();
    const bool recording = app.recording;
    if (recording) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(205, 60, 50, 255));
    if (ImGui::Button(recording ? "Recording" : "Record", ImVec2(84.0f, 0.0f))) toggleRecord(app);
    if (recording) ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Return. Starting from a stop counts in one bar first.");
    ImGui::SameLine();
    const bool placing = app.placeMode;
    if (placing) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(190, 130, 40, 255));
    if (ImGui::Button("Place", ImVec2(52.0f, 0.0f))) {
        app.placeMode = !app.placeMode;
        if (app.placeMode) app.recording = false;
    }
    if (placing) ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Each key you press drops its sound at the cursor, then moves on by the Snap value. ' toggles it. Left and Right move the cursor.");

    ImGui::SameLine(0.0f, 14.0f);
    float bpm = static_cast<float>(timeline.bpm);
    ImGui::SetNextItemWidth(92.0f);
    if (ImGui::DragFloat("##bpm", &bpm, 0.25f, 20.0f, 400.0f, "%.1f BPM", ImGuiSliderFlags_AlwaysClamp)) {
        timeline.bpm = static_cast<double>(bpm);
        applyTransportSettings(app);
        markSessionDirty(app);
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(84.0f);
    char barsLabel[16];
    std::snprintf(barsLabel, sizeof(barsLabel), "%d bars", timeline.bars);
    if (ImGui::BeginCombo("##bars", barsLabel)) {
        for (int bars : {1, 2, 4, 8, 16, 32, 64}) {
            char label[16];
            std::snprintf(label, sizeof(label), "%d bars", bars);
            if (ImGui::Selectable(label, bars == timeline.bars)) {
                timeline.bars = bars;
                applyTransportSettings(app);
                publishSequence(app);
                markSessionDirty(app);
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(136.0f);
    char snapLabel[32];
    std::snprintf(snapLabel, sizeof(snapLabel), "Snap %s",
                  kQuantizeOptions[static_cast<size_t>(app.session.quantizeIndex)].label);
    if (ImGui::BeginCombo("##snap", snapLabel)) {
        for (int i = 0; i < static_cast<int>(kQuantizeOptions.size()); ++i) {
            if (ImGui::Selectable(kQuantizeOptions[static_cast<size_t>(i)].label, i == app.session.quantizeIndex)) {
                app.session.quantizeIndex = i;
                markSessionDirty(app);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Recorded hits snap to this subdivision of the beat");

    ImGui::SameLine();
    if (ImGui::Checkbox("Click", &app.session.metronome)) {
        applyTransportSettings(app);
        markSessionDirty(app);
    }

    ImGui::SameLine(0.0f, 14.0f);
    const bool hasEvents = !timeline.events.empty();
    ImGui::BeginDisabled(!hasEvents);
    if (ImGui::Button("Quantize all")) {
        pushUndo(app);
        quantizeAll(timeline, kQuantizeOptions[static_cast<size_t>(app.session.quantizeIndex)].beats);
        publishSequence(app);
        markSessionDirty(app);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        pushUndo(app);
        timeline.events.clear();
        app.selection.clear();
        app.session.editingLoop.clear();  // what's built next is a new loop
        publishSequence(app);
        markSessionDirty(app);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save loop")) requestSaveLoop(app);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Save one pass of the loop as a new sound you can play and chop");
    ImGui::SameLine();
    ImGui::BeginDisabled(app.undo.empty());
    if (ImGui::Button("Undo")) performUndo(app);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(AMENITY_CMD "Z");

    if (playing && app.engine->transportBeat() < 0.0) {
        ImGui::SameLine(0.0f, 14.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.42f, 1.0f), "Count-in");
    }
}

struct Lane {
    std::string path;
    size_t start = 0;
    size_t end = 0;
    std::string label;
};

std::string laneLabel(const App& app, const TimelineEvent& event) {
    std::string label = fs::path(event.path).stem().string();
    const Sample* sample = findLoaded(app, event.path);
    if (sample && !(event.start == 0 && event.end >= sample->frames)) {
        char suffix[24];
        std::snprintf(suffix, sizeof(suffix), " @%.2fs", static_cast<double>(event.start) / app.sampleRate);
        label += suffix;
    }
    return label;
}

// The lowest and highest values in a range of frames, from the peak table when it's long.
void peakRange(const Sample& sample, size_t from, size_t to, float& low, float& high) {
    to = std::min(to, sample.frames);
    if (from >= to) return;
    if (to - from >= Sample::kPeakBlock * 2) {
        const size_t last = std::min(to / Sample::kPeakBlock + 1, sample.peakMin.size());
        for (size_t i = from / Sample::kPeakBlock; i < last; ++i) {
            low = std::min(low, sample.peakMin[i]);
            high = std::max(high, sample.peakMax[i]);
        }
    } else {
        for (size_t i = from; i < to; ++i) {
            low = std::min(low, sample.mono[i]);
            high = std::max(high, sample.mono[i]);
        }
    }
}

// Draws a hit's waveform the way it plays: reversed, or repeating if it loops. Anything past
// the hit's length is cut off by the caller's clip rectangle.
void drawHitWave(ImDrawList* draw, const Sample& sample, size_t start, size_t end, bool reverse, bool loop,
                 double framesPerPixel, float x0, float x1, float midY, float halfHeight, ImU32 color) {
    const double length = static_cast<double>(end - start);
    if (end <= start || framesPerPixel <= 0.0) return;
    for (float x = x0; x < x1; x += 1.0f) {
        double a = static_cast<double>(x - x0) * framesPerPixel;
        double b = a + framesPerPixel;
        if (!loop && a >= length) break;
        if (loop) {
            const double wrapped = std::floor(a / length) * length;
            a -= wrapped;
            b -= wrapped;
        }
        b = std::min(b, length);
        size_t from = 0;
        size_t to = 0;
        if (reverse) {
            from = end - static_cast<size_t>(std::ceil(b));
            to = end - static_cast<size_t>(a);
        } else {
            from = start + static_cast<size_t>(a);
            to = start + static_cast<size_t>(std::ceil(b));
        }
        float low = 1.0f;
        float high = -1.0f;
        peakRange(sample, from, std::max(to, from + 1), low, high);
        if (high < low) continue;
        draw->AddLine(ImVec2(x + 0.5f, midY - high * halfHeight), ImVec2(x + 0.5f, midY - low * halfHeight + 1.0f),
                      color);
    }
}

struct HitBox {
    uint64_t id = 0;
    ImVec2 min;
    ImVec2 max;
};

void finishDrag(App& app) {
    Timeline& timeline = app.session.timeline;
    app.dragEvent = 0;
    app.dragResize = false;
    app.dragOrigins.clear();
    std::stable_sort(timeline.events.begin(), timeline.events.end(),
                     [](const TimelineEvent& a, const TimelineEvent& b) { return a.beat < b.beat; });
    // An Option-drag that went nowhere leaves its copies on top of the originals.
    removeDuplicates(timeline);
    std::erase_if(app.selection, [&](uint64_t id) {
        return std::none_of(timeline.events.begin(), timeline.events.end(), [id](const TimelineEvent& e) { return e.id == id; });
    });
    publishSequence(app);
    // A click that didn't move anything isn't worth an undo step.
    if (!app.undo.empty() && sameEvents(app.undo.back().events, timeline.events)) {
        app.undo.pop_back();
    } else {
        markSessionDirty(app);
    }
}

// Option-drag: the selected hits are copied in place, and the copies are what moves.
void copySelectionInPlace(App& app) {
    Timeline& timeline = app.session.timeline;
    std::vector<TimelineEvent> copies;
    std::set<uint64_t> copyIds;
    for (const TimelineEvent& event : timeline.events) {
        if (!app.selection.count(event.id)) continue;
        TimelineEvent copy = event;
        copy.id = timeline.nextId++;
        if (event.id == app.dragEvent) app.dragEvent = copy.id;
        copyIds.insert(copy.id);
        copies.push_back(std::move(copy));
    }
    timeline.events.insert(timeline.events.end(), copies.begin(), copies.end());
    std::stable_sort(timeline.events.begin(), timeline.events.end(),
                     [](const TimelineEvent& a, const TimelineEvent& b) { return a.beat < b.beat; });
    app.selection = std::move(copyIds);
}

void drawTimeline(App& app, ImVec2 size) {
    Timeline& timeline = app.session.timeline;
    const ImGuiIO& io = ImGui::GetIO();
    constexpr float kLabelWidth = 150.0f;
    constexpr float kRulerHeight = 18.0f;
    constexpr float kLaneHeight = 34.0f;
    const double loop = timeline.loopBeats();
    const double grid = gridBeats(app);
    const double lastStart = loop - std::max(grid, 1e-3);

    std::vector<Lane> lanes;
    for (const TimelineEvent& event : timeline.events) {
        if (event.beat >= loop) continue;
        const bool known = std::any_of(lanes.begin(), lanes.end(), [&](const Lane& lane) {
            return lane.path == event.path && lane.start == event.start && lane.end == event.end;
        });
        if (!known) lanes.push_back({event.path, event.start, event.end, laneLabel(app, event)});
    }
    std::sort(lanes.begin(), lanes.end(), [](const Lane& a, const Lane& b) {
        return std::tie(a.path, a.start, a.end) < std::tie(b.path, b.start, b.end);
    });

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float gridLeft = origin.x + kLabelWidth;
    const float gridWidth = std::max(size.x - kLabelWidth, 10.0f);
    const auto beatToX = [&](double beat) { return gridLeft + static_cast<float>(beat / loop) * gridWidth; };
    const auto xToBeat = [&](float x) { return static_cast<double>((x - gridLeft) / gridWidth) * loop; };
    const bool playing = app.engine->transportPlaying();
    const double playhead = app.engine->transportBeat();

    const auto drawGridLines = [&](ImDrawList* draw, float top, float bottom) {
        const float beatWidth = gridWidth / static_cast<float>(loop);
        const double step = grid > 0.0 ? grid : 0.25;
        const bool showSub = beatWidth * static_cast<float>(step) >= 6.0f;
        const auto lines = static_cast<int>(std::llround(loop / step));
        for (int i = 0; i <= lines; ++i) {
            const double beat = static_cast<double>(i) * step;
            const double nearestBeat = std::round(beat);
            const bool onBeat = std::fabs(beat - nearestBeat) < 1e-6;
            const bool onBar = onBeat && static_cast<long>(nearestBeat) % kBeatsPerBar == 0;
            if (!onBeat && !showSub) continue;
            if (onBeat && !onBar && beatWidth < 5.0f) continue;
            const float x = beatToX(beat);
            draw->AddLine(ImVec2(x, top), ImVec2(x, bottom),
                          onBar ? colors::kGridBar : (onBeat ? colors::kGridBeat : colors::kGridSub));
        }
    };

    // Ruler: click or drag to choose where Space starts playing, right-click to go back to the top.
    ImGui::SetCursorScreenPos(ImVec2(gridLeft, origin.y));
    ImGui::InvisibleButton("##ruler", ImVec2(gridWidth, kRulerHeight),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    ImDrawList* windowDraw = ImGui::GetWindowDrawList();
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const double beat = std::clamp(snapBeat(xToBeat(io.MousePos.x), grid), 0.0, loop - 1e-3);
        const bool changed = beat != app.playStart;
        app.playStart = beat;
        app.editCursor = beat;
        if (changed && playing && ImGui::IsItemActivated()) app.engine->play(beat);
        if (ImGui::IsItemActivated() && beat > 0.0) {
            app.status = "Space now plays from here. Right-click the ruler to play from the beginning again.";
        }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        app.playStart = 0.0;
        app.editCursor = 0.0;
    }
    ImGui::SetItemTooltip("Click to choose where Space starts playing. Right-click to start from the beginning.");
    windowDraw->AddRectFilled(ImVec2(gridLeft, origin.y), ImVec2(gridLeft + gridWidth, origin.y + kRulerHeight),
                              colors::kCell);
    const int labelEvery = std::max(1, static_cast<int>(std::ceil(28.0f / (gridWidth / static_cast<float>(timeline.bars)))));
    for (int bar = 0; bar < timeline.bars; bar += labelEvery) {
        const float x = beatToX(static_cast<double>(bar * kBeatsPerBar));
        char text[8];
        std::snprintf(text, sizeof(text), "%d", bar + 1);
        windowDraw->AddText(ImVec2(x + 4.0f, origin.y + 1.0f), colors::kLabel, text);
        windowDraw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + kRulerHeight), colors::kGridBar);
    }
    if (app.placeMode) {
        const float x = beatToX(app.editCursor);
        windowDraw->AddRectFilled(ImVec2(x - 1.5f, origin.y), ImVec2(x + 1.5f, origin.y + kRulerHeight), colors::kWave);
    }
    const float startX = beatToX(app.playStart);
    windowDraw->AddTriangleFilled(ImVec2(startX - 6.0f, origin.y), ImVec2(startX + 6.0f, origin.y),
                                  ImVec2(startX, origin.y + 10.0f), colors::kMarkerHot);

    // Above the lane names: what's selected, or which saved loop this is.
    std::string heading;
    if (!app.selection.empty()) {
        heading = std::to_string(app.selection.size()) + " selected";
    } else if (!app.session.editingLoop.empty()) {
        heading = "Editing " + fs::path(app.session.editingLoop).stem().string();
    }
    if (!heading.empty()) {
        windowDraw->AddText(ImVec2(origin.x + 6.0f, origin.y + 1.0f), app.selection.empty() ? colors::kDim : colors::kLabel,
                            fitName(heading, kLabelWidth - 12.0f).c_str());
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + kRulerHeight));
    const float areaHeight = std::max(size.y - kRulerHeight, 20.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors::kWaveBg);
    ImGui::BeginChild("lanes", ImVec2(size.x, areaHeight), ImGuiChildFlags_None);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 lanesOrigin = ImGui::GetCursorScreenPos();
    const float contentHeight = std::max(kLaneHeight * static_cast<float>(lanes.size()), areaHeight);
    ImGui::InvisibleButton("##lanes", ImVec2(size.x, contentHeight),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool activated = ImGui::IsItemActivated();
    const float bottom = lanesOrigin.y + contentHeight;
    const float visibleTop = lanesOrigin.y + ImGui::GetScrollY();
    const auto laneAt = [&](float y) {
        const float row = (y - lanesOrigin.y) / kLaneHeight;
        return row >= 0.0f && row < static_cast<float>(lanes.size()) ? static_cast<long>(row) : -1L;
    };

    drawGridLines(draw, lanesOrigin.y, bottom);

    for (size_t i = 0; i < lanes.size(); ++i) {
        const float y = lanesOrigin.y + kLaneHeight * static_cast<float>(i);
        if (i % 2 == 0) {
            draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + size.x, y + kLaneHeight), colors::kLaneTint);
        }
        const bool missing = findLoaded(app, lanes[i].path) == nullptr;
        draw->AddText(ImVec2(origin.x + 6.0f, y + (kLaneHeight - ImGui::GetTextLineHeight()) * 0.5f),
                      missing ? colors::kMissing : colors::kLabel, fitName(lanes[i].label, kLabelWidth - 10.0f).c_str());
    }

    if (lanes.empty()) {
        const char* hint = "Press Return to record to the beat, or ' to place sounds one key at a time";
        const ImVec2 textSize = ImGui::CalcTextSize(hint);
        draw->AddText(ImVec2(gridLeft + std::max(0.0f, (gridWidth - textSize.x) * 0.5f),
                             lanesOrigin.y + (areaHeight - textSize.y) * 0.5f),
                      colors::kDim, hint);
    }

    // Where Space starts playing, when that isn't the beginning.
    if (app.playStart > 0.0) {
        draw->AddLine(ImVec2(startX, visibleTop), ImVec2(startX, visibleTop + areaHeight), IM_COL32(120, 220, 255, 110), 1.0f);
    }

    // Hits, each showing its waveform. Drawn in order, so the hit test keeps the topmost.
    uint64_t hoveredEvent = 0;
    bool hoveredEdge = false;
    double hoveredBeat = 0.0;
    std::vector<HitBox> boxes;
    const double framesPerPixel = loop / static_cast<double>(gridWidth) * 60.0 / timeline.bpm * app.sampleRate;
    for (const TimelineEvent& event : timeline.events) {
        if (event.beat >= loop) continue;
        const auto laneIt = std::find_if(lanes.begin(), lanes.end(), [&](const Lane& lane) {
            return lane.path == event.path && lane.start == event.start && lane.end == event.end;
        });
        const size_t lane = static_cast<size_t>(laneIt - lanes.begin());
        const float y = lanesOrigin.y + kLaneHeight * static_cast<float>(lane) + 2.0f;
        const Playable playable = resolvePlayable(app, event.sound());
        // The sound itself is solid, and any reverb or echo ringing on after it is faint.
        const double fullLength = hitLengthBeats(app, event);
        const double dryLength = std::min(dryLengthBeats(app, event), fullLength);
        const float gridRight = gridLeft + gridWidth;
        const float x0 = beatToX(event.beat);
        const float xDry = std::min(std::max(beatToX(event.beat + dryLength), x0 + 4.0f), gridRight);
        const float xFull = std::min(std::max(beatToX(event.beat + fullLength), xDry), gridRight);
        const float x1 = xDry;
        const float y1 = y + kLaneHeight - 4.0f;
        boxes.push_back({event.id, ImVec2(x0, y), ImVec2(x1, y1)});

        const bool selected = app.selection.count(event.id) > 0;
        const bool missing = playable.sample == nullptr;
        const float hue = std::fmod(static_cast<float>(lane) * 0.137f + 0.09f, 1.0f);
        const ImU32 fill = missing ? colors::kMissing
                                   : static_cast<ImU32>(ImColor::HSV(hue, 0.5f, selected ? 0.5f : 0.3f));
        const ImU32 wave = ImColor::HSV(hue, 0.4f, 1.0f, event.gain < 0.9f ? 0.6f : 0.95f);
        if (xFull > xDry) {
            draw->AddRectFilled(ImVec2(xDry - 3.0f, y), ImVec2(xFull, y1), ImColor::HSV(hue, 0.5f, 0.3f, 0.3f), 3.0f);
        }
        draw->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y1), fill, 3.0f);
        if (playable.sample) {
            draw->PushClipRect(ImVec2(x0, y), ImVec2(xFull, y1), true);
            drawHitWave(draw, *playable.sample, playable.start, playable.end, event.reverse,
                        settingsFor(app, event.sound()).loop, framesPerPixel, x0, x1, (y + y1) * 0.5f,
                        (y1 - y) * 0.45f * std::min(event.gain, 1.0f), wave);
            draw->PopClipRect();
        }
        if (selected) {
            draw->AddRect(ImVec2(x0, y), ImVec2(x1, y1), IM_COL32(255, 255, 255, 235), 3.0f, 0, 2.0f);
        } else if (app.hasSelected && event.sound() == app.selected) {
            draw->AddRect(ImVec2(x0, y), ImVec2(x1, y1), IM_COL32(255, 255, 255, 90), 3.0f);
        }
        if (event.reverse) draw->AddTriangleFilled(ImVec2(x0 + 8.0f, y + 2.0f), ImVec2(x0 + 8.0f, y1 - 2.0f),
                                                   ImVec2(x0 + 2.0f, (y + y1) * 0.5f), IM_COL32(0, 0, 0, 170));

        if (hovered && io.MousePos.x >= x0 && io.MousePos.x <= x1 && io.MousePos.y >= y && io.MousePos.y <= y1) {
            hoveredEvent = event.id;
            hoveredBeat = event.beat;
            hoveredEdge = x1 - x0 > 12.0f && io.MousePos.x >= x1 - 6.0f;
        }
    }

    const auto eventWithId = [&](uint64_t id) {
        return std::find_if(timeline.events.begin(), timeline.events.end(),
                            [id](const TimelineEvent& e) { return e.id == id; });
    };
    const double mouseBeat = xToBeat(io.MousePos.x);
    const double clickBeat = std::clamp(snapBeat(mouseBeat, grid), 0.0, lastStart);
    const bool inGrid = io.MousePos.x >= gridLeft;

    // Right-click: a menu for the hits, or for the empty spot.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (hoveredEvent != 0) {
            if (!app.selection.count(hoveredEvent)) app.selection = {hoveredEvent};
            ImGui::OpenPopup("hit menu");
        } else if (inGrid) {
            app.editCursor = clickBeat;
            ImGui::OpenPopup("space menu");
        }
    }
    if (ImGui::BeginPopup("hit menu")) {
        if (ImGui::MenuItem("Copy", AMENITY_CMD "C")) copySelection(app);
        if (ImGui::MenuItem("Cut", AMENITY_CMD "X")) cutSelection(app);
        if (ImGui::MenuItem("Duplicate", AMENITY_CMD "D")) duplicateSelection(app);
        ImGui::Separator();
        if (ImGui::MenuItem(app.selection.size() > 1 ? "Delete hits" : "Delete", "Delete")) deleteSelection(app);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("space menu")) {
        if (ImGui::MenuItem("Paste here", AMENITY_CMD "V", false, !app.clipboard.empty())) pasteClipboard(app);
        if (ImGui::MenuItem("Select all", AMENITY_CMD "A", false, !timeline.events.empty())) selectAll(app);
        if (ImGui::MenuItem("Select none", "Esc", false, !app.selection.empty())) app.selection.clear();
        ImGui::EndPopup();
    }

    if (activated && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (!inGrid) {
            // A lane's name selects all of its hits.
            const long lane = laneAt(io.MousePos.y);
            if (lane >= 0) {
                const Lane& picked = lanes[static_cast<size_t>(lane)];
                if (!io.KeyShift) app.selection.clear();
                for (const TimelineEvent& event : timeline.events) {
                    if (event.path == picked.path && event.start == picked.start && event.end == picked.end) {
                        app.selection.insert(event.id);
                    }
                }
                SoundRef sound;
                sound.path = picked.path;
                sound.sample = findLoaded(app, picked.path);
                sound.start = picked.start;
                sound.end = picked.end;
                playSound(app, sound, kAuditionVoiceKey, 1.0f, false, false);
            }
        } else if (hoveredEvent != 0) {
            if (io.KeyShift && app.selection.count(hoveredEvent)) {
                app.selection.erase(hoveredEvent);
            } else if (io.KeyShift) {
                app.selection.insert(hoveredEvent);
            } else if (!app.selection.count(hoveredEvent)) {
                app.selection = {hoveredEvent};
            }
            if (app.selection.count(hoveredEvent)) {
                pushUndo(app);
                app.dragEvent = hoveredEvent;
                app.dragResize = hoveredEdge;
                app.dragOffset = mouseBeat - hoveredBeat;
                if (!hoveredEdge) {
                    const auto event = eventWithId(hoveredEvent);
                    SoundRef sound;
                    sound.path = event->path;
                    sound.sample = findLoaded(app, event->path);
                    sound.start = event->start;
                    sound.end = event->end;
                    const float gain = event->gain;
                    const bool reverse = event->reverse;
                    if (io.KeyAlt) copySelectionInPlace(app);
                    for (const TimelineEvent& e : timeline.events) {
                        if (app.selection.count(e.id)) app.dragOrigins[e.id] = e.beat;
                    }
                    playSound(app, sound, kAuditionVoiceKey, gain, reverse, false);
                }
            }
        } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            // Double-click a lane to add another hit of its sound there.
            const long lane = laneAt(io.MousePos.y);
            if (lane >= 0) {
                const Lane& picked = lanes[static_cast<size_t>(lane)];
                addPlacedHit(app, {picked.path, picked.start, picked.end}, clickBeat, 1.0f, false);
            }
        } else {
            // Empty space moves the cursor. Dragging from it selects everything it touches.
            if (!io.KeyShift) app.selection.clear();
            app.editCursor = clickBeat;
            app.boxSelecting = true;
            app.boxStart = io.MousePos;
            app.boxBase = app.selection;
        }
    }

    if (app.dragEvent != 0) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            finishDrag(app);
        } else if (app.dragResize) {
            const auto event = eventWithId(app.dragEvent);
            if (event != timeline.events.end()) {
                const double step = grid > 0.0 ? grid : 1.0 / 64.0;
                const double length = std::clamp(snapBeat(mouseBeat, grid) - event->beat, step, loop - event->beat);
                if (length != event->durationBeats) {
                    event->durationBeats = length;
                    publishSequence(app);
                }
            }
        } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
            // Everything selected moves together, keeping its spacing and staying on the timeline.
            const auto anchor = app.dragOrigins.find(app.dragEvent);
            if (anchor != app.dragOrigins.end()) {
                double earliest = loop;
                double latest = 0.0;
                for (const auto& [id, beat] : app.dragOrigins) {
                    earliest = std::min(earliest, beat);
                    latest = std::max(latest, beat);
                }
                const double wanted = snapBeat(mouseBeat - app.dragOffset, grid) - anchor->second;
                const double delta = std::clamp(wanted, -earliest, std::max(lastStart - latest, -earliest));
                bool moved = false;
                for (TimelineEvent& event : timeline.events) {
                    const auto origin = app.dragOrigins.find(event.id);
                    if (origin == app.dragOrigins.end() || event.beat == origin->second + delta) continue;
                    event.beat = origin->second + delta;
                    moved = true;
                }
                if (moved) publishSequence(app);
            }
        }
    }

    if (app.boxSelecting) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            app.boxSelecting = false;
        } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
            const ImVec2 a(std::min(app.boxStart.x, io.MousePos.x), std::min(app.boxStart.y, io.MousePos.y));
            const ImVec2 b(std::max(app.boxStart.x, io.MousePos.x), std::max(app.boxStart.y, io.MousePos.y));
            app.selection = app.boxBase;
            for (const HitBox& box : boxes) {
                if (box.max.x >= a.x && box.min.x <= b.x && box.max.y >= a.y && box.min.y <= b.y) app.selection.insert(box.id);
            }
            draw->AddRectFilled(a, b, IM_COL32(120, 220, 255, 22));
            draw->AddRect(a, b, IM_COL32(120, 220, 255, 150));
        }
    }

    if (hoveredEdge || app.dragResize) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (hoveredEvent != 0 && app.dragEvent == 0) {
        ImGui::SetItemTooltip("Drag to move, " AMENITY_ALT "-drag to copy, Shift-click to add to the selection. "
                              "Drag the right edge to change its length. Right-click for more.");
    } else if (hovered && !inGrid && laneAt(io.MousePos.y) >= 0 && app.dragEvent == 0) {
        ImGui::SetItemTooltip("Click to select all of this sound's hits");
    }

    // The cursor, where pasting and Place mode put things.
    const float cursorX = beatToX(app.editCursor);
    draw->AddLine(ImVec2(cursorX, visibleTop), ImVec2(cursorX, visibleTop + areaHeight),
                  app.placeMode ? colors::kWave : IM_COL32(236, 174, 66, 120), app.placeMode ? 2.0f : 1.0f);

    if (playing && playhead >= 0.0) {
        const float x = beatToX(playhead);
        draw->AddLine(ImVec2(x, visibleTop), ImVec2(x, visibleTop + areaHeight),
                      app.recording ? IM_COL32(255, 90, 75, 255) : colors::kPlayhead, 1.5f);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void drawStatusBar(const App& app) {
    if (!app.deviceError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", app.deviceError.c_str());
    } else if (app.deviceRunning) {
        const ma_uint32 period = app.device.playback.internalPeriodSizeInFrames;
        ImGui::TextDisabled("%.1f ms buffer, pickup %.1f ms", 1000.0 * period / app.sampleRate,
                            static_cast<double>(app.engine->lastPickupMs()));
    }
    ImGui::SameLine();
    if (!app.status.empty()) {
        ImGui::TextUnformatted(app.status.c_str());
    } else {
        ImGui::TextDisabled(
            " |  ` pause  Space play  Return record  ' place  Tab kit  - = kits  \\ bookmark  Esc stop");
    }
}

void drawUi(App& app) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::Begin("Amenity", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float leftWidth = std::min(290.0f, available.x * 0.28f);
    drawLeftPanel(app, ImVec2(leftWidth, available.y));
    ImGui::SameLine();

    ImGui::BeginGroup();
    const float rightWidth = ImGui::GetContentRegionAvail().x;
    if (app.sample) {
        ImGui::TextUnformatted(app.sample->name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%.2f s, %zu slices, bank %d/%d", static_cast<double>(app.sample->frames) / app.sampleRate,
                            sliceCount(app), app.bank + 1,
                            sliceCount(app) > 0 ? static_cast<int>((sliceCount(app) - 1) / kKeysPerBank) + 1 : 1);
        if (app.songBpm > 0.0) {
            ImGui::SameLine();
            ImGui::TextDisabled("~%.1f BPM", app.songBpm);
            ImGui::SameLine();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.0f, 0.0f));
            if (ImGui::SmallButton("Use")) {
                app.session.timeline.bpm = app.songBpm;
                applyTransportSettings(app);
                markSessionDirty(app);
            }
            ImGui::PopStyleVar();
            ImGui::SetItemTooltip("Set the timeline to this song's detected tempo");
        }
    } else {
        ImGui::TextDisabled("No song loaded");
    }
    drawToolbar(app);

    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float lineHeight = ImGui::GetFrameHeightWithSpacing();
    const float statusHeight = ImGui::GetTextLineHeightWithSpacing();
    const float remaining = ImGui::GetContentRegionAvail().y - statusHeight - lineHeight - spacing * 3.0f;
    const float timelineHeight = std::clamp(remaining * 0.36f, 120.0f, 400.0f);
    const float keyboardHeight = std::clamp(remaining * 0.34f, 120.0f, 300.0f);
    const float waveformHeight = std::max(remaining - timelineHeight - keyboardHeight, 50.0f);

    drawWaveform(app, ImVec2(rightWidth, waveformHeight));
    drawKeyboard(app, ImVec2(rightWidth, keyboardHeight));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    drawTransport(app);
    drawTimeline(app, ImVec2(rightWidth, timelineHeight));
    drawStatusBar(app);
    ImGui::EndGroup();

    drawNameDialog(app);
    ImGui::End();
}

void applyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.WindowPadding = ImVec2(10.0f, 10.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    ImVec4* c = style.Colors;
    const ImVec4 accent(0.925f, 0.682f, 0.259f, 1.0f);
    const ImVec4 accentDim(0.925f, 0.682f, 0.259f, 0.28f);
    const ImVec4 accentMid(0.925f, 0.682f, 0.259f, 0.45f);
    c[ImGuiCol_WindowBg] = ImVec4(0.086f, 0.09f, 0.11f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.115f, 0.14f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.148f, 0.18f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.19f, 0.2f, 0.24f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.23f, 0.28f, 1.0f);
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.8f, 0.4f, 1.0f);
    c[ImGuiCol_Button] = accentDim;
    c[ImGuiCol_ButtonHovered] = accentMid;
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = accentDim;
    c[ImGuiCol_HeaderHovered] = accentMid;
    c[ImGuiCol_HeaderActive] = accentMid;
    c[ImGuiCol_Tab] = ImVec4(0.14f, 0.148f, 0.18f, 1.0f);
    c[ImGuiCol_TabHovered] = accentMid;
    c[ImGuiCol_TabSelected] = accentDim;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TextSelectedBg] = accentMid;
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.5f);
}

void writePpm(const std::string& path, int width, int height) {
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << width << " " << height << "\n255\n";
    for (int y = height - 1; y >= 0; --y) {
        out.write(reinterpret_cast<const char*>(&pixels[static_cast<size_t>(y) * width * 3]), width * 3);
    }
}

int runAnalyze(const std::string& path, float sensitivity) {
    std::string error;
    const auto sample = loadSample(path, 48000, error);
    if (!sample) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto function = computeOnsetFunction(sample->mono);
    const auto analyzed = std::chrono::steady_clock::now();
    const auto markers = pickOnsets(function, sample->mono, sample->sampleRate, sensitivity);
    const auto picked = std::chrono::steady_clock::now();
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };

    std::printf("%s: %.3f s, %zu slices at sensitivity %.2f (analysis %.1f ms, pick %.2f ms)\n",
                sample->name.c_str(), static_cast<double>(sample->frames) / sample->sampleRate, markers.size(),
                static_cast<double>(sensitivity), ms(started, analyzed), ms(analyzed, picked));
    std::printf("estimated tempo: %.1f BPM\n", estimateTempo(function, sample->sampleRate));
    for (size_t i = 0; i < markers.size(); ++i) {
        std::printf("%3zu  %8.3f s\n", i + 1, static_cast<double>(markers[i]) / sample->sampleRate);
    }
    return 0;
}

// Drives the whole workflow through the normal code paths, silently, in its own data folder.
// Returns an exit code once finished, or -1 while it's still running.
struct SelfTest {
    std::string capture;
    long startFrame = -1;
    size_t eventsAtStart = 0;
    size_t soundsAtStart = 0;
    size_t tookEvents = 0;
    size_t eventsBeforePlacing = 0;
    size_t placedEvents = 0;
    double placedCursor = 0.0;
    SoundKey shapedKey;
    bool settingsWereRestored = false;
    bool pausedOk = false;
    bool resumedOk = false;
    double pausedBeat = 0.0;
    size_t kitsAtStart = 0;
    bool newKitEmpty = false;
    bool switchedBack = false;
    bool keyLit = false;
};

int stepSelfTest(App& app, SelfTest& test, long frame, int width, int height) {
    if (test.startFrame < 0) {
        if (!app.sample || !app.startupLoaded || app.scanning.valid() || app.loading.valid()) {
            if (frame > 1800) {
                std::fprintf(stderr, "self-test timed out waiting for loading\n");
                return 3;
            }
            return -1;
        }
        test.startFrame = frame;
        test.eventsAtStart = app.session.timeline.events.size();
        test.soundsAtStart = app.sounds.size();
        std::printf("at startup: %zu events, %zu sounds, kit key 1: %s\n", test.eventsAtStart, test.soundsAtStart,
                    app.session.kit().keys[0].empty() ? "empty" : fs::path(app.session.kit().keys[0]).stem().string().c_str());
        app.session.timeline.bpm = 240.0;
        app.session.timeline.bars = 2;
        applyTransportSettings(app);
        publishSequence(app);
        return -1;
    }

    const long t = frame - test.startFrame;
    const auto tap = [&](int key) {
        onKey(app, key, GLFW_PRESS, 0);
        onKey(app, key, GLFW_RELEASE, 0);
    };
    Session& session = app.session;
    if (t == 2 && session.kit().keys[0].empty()) saveNewSound(app, clipForSlice(app, 0), "Self test one", true);
    if (t == 3 && session.kit().keys[1].empty()) saveNewSound(app, clipForSlice(app, 8), "Self test two", true);
    if (t == 4) tap(GLFW_KEY_TAB);
    if (t == 6) tap(GLFW_KEY_ENTER);  // one-bar count-in, one second at this tempo
    if (t == 72) tap(GLFW_KEY_1);
    if (t == 79) tap(GLFW_KEY_2);
    if (t == 88) tap(GLFW_KEY_1);
    if (t == 96) tap(GLFW_KEY_2);
    if (t == 104) tap(GLFW_KEY_TAB);
    if (t == 106) tap(GLFW_KEY_Q);  // a slice straight from the song
    if (t == 110) tap(GLFW_KEY_TAB);
    if (t == 118) tap(GLFW_KEY_ENTER);  // recording stops, playback continues
    if (t == 120) test.tookEvents = session.timeline.events.size() - test.eventsAtStart;

    // Place mode: no recording, and the cursor moves on by the snap value.
    if (t == 122) {
        tap(GLFW_KEY_APOSTROPHE);
        test.eventsBeforePlacing = session.timeline.events.size();
        app.editCursor = 5.0;
        tap(GLFW_KEY_1);
        tap(GLFW_KEY_RIGHT);
        tap(GLFW_KEY_2);
        test.placedCursor = app.editCursor;
        test.placedEvents = session.timeline.events.size() - test.eventsBeforePlacing;
        tap(GLFW_KEY_APOSTROPHE);
    }

    // Shape a sound the way the panel does, then wait for its render.
    if (t == 126) {
        test.shapedKey = {session.kit().keys[0], 0, 0};
        const Sample* sample = findLoaded(app, test.shapedKey.path);
        if (sample) test.shapedKey.end = sample->frames;
        test.settingsWereRestored = !settingsFor(app, test.shapedKey).isNeutral();
        SoundSettings shaped;
        shaped.pitch = 5.0f;
        shaped.fit = true;
        shaped.sourceBpm = 136.0f;
        shaped.punch = 0.5f;
        shaped.applyPunch();
        shaped.space = 0.3f;
        shaped.applySpace();
        session.soundSettings[test.shapedKey] = shaped;
        app.selected = test.shapedKey;
        app.hasSelected = true;
    }
    if (t == 200) writePpm(test.capture, width, height);

    // Pause and resume.
    if (t == 201) tap(GLFW_KEY_GRAVE_ACCENT);
    if (t == 210) {
        test.pausedOk = app.engine->paused();
        test.pausedBeat = app.engine->transportBeat();
    }
    if (t == 218) {
        test.pausedOk = test.pausedOk && app.engine->paused() && app.engine->transportBeat() == test.pausedBeat;
        tap(GLFW_KEY_GRAVE_ACCENT);
    }
    if (t == 226) test.resumedOk = !app.engine->paused() && app.engine->transportBeat() != test.pausedBeat;

    // Kits.
    if (t == 228) {
        test.kitsAtStart = session.kits.size();
        session.kits.emplace_back();
        session.kits.back().name = "Self test kit";
        switchKit(app, static_cast<int>(session.kits.size()) - 1);
        test.newKitEmpty = session.kit().keys[0].empty();
        tap(GLFW_KEY_MINUS);
        test.switchedBack = session.currentKit == static_cast<int>(session.kits.size()) - 2;
    }
    // A key stays lit while its sound plays.
    if (t == 230) {
        app.engine->stopAll();
        switchKit(app, 0);  // earlier runs leave empty test kits behind
        tap(GLFW_KEY_1);
    }
    if (t == 232) test.keyLit = keyActivity(app, keySound(app, 0)).progress >= 0.0f;
    if (t < 235) return -1;

    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        std::printf("%s: %s\n", ok ? "ok" : "FAILED", what);
        if (!ok) ++failures;
    };
    const double grid = kQuantizeOptions[static_cast<size_t>(session.quantizeIndex)].beats;
    bool onGrid = true;
    for (const TimelineEvent& event : session.timeline.events) {
        if (std::fabs(event.beat - snapBeat(event.beat, grid)) > 1e-9) onGrid = false;
    }
    std::printf("recorded %zu new events on a %s grid, worst pickup %.2f ms\n", test.tookEvents,
                kQuantizeOptions[static_cast<size_t>(session.quantizeIndex)].label,
                static_cast<double>(app.engine->maxPickupMs()));
    check(app.deviceRunning, "audio device is running");
    // A repeat run plays the same hits on the same grid lines, which correctly merge, so
    // there it's the restored session that shows recording worked.
    if (test.eventsAtStart == 0) {
        check(test.tookEvents >= 3, "hits were recorded to the timeline");
        check(test.placedEvents == 2, "Place mode added hits without recording");
    } else {
        check(!session.kits[0].keys[0].empty(), "the kit and timeline were restored from the last run");
        check(test.settingsWereRestored, "sound settings were restored from the last run");
    }
    check(onGrid, "every event is on the grid");
    check(std::fabs(test.placedCursor - (5.0 + 3.0 * grid)) < 1e-9, "Place mode moved the cursor by the snap value");

    const auto render = app.renders.find(test.shapedKey);
    check(render != app.renders.end() && !app.renderJob.valid(), "the shaped sound was rendered in the background");
    if (render != app.renders.end()) {
        const Sample* source = findLoaded(app, test.shapedKey.path);
        const double expected = static_cast<double>(source->frames) * 136.0 / session.timeline.bpm;
        check(static_cast<double>(render->second.sample->frames) >= expected - 1.0, "it fits the BPM, with its reverb tail after");
        check(resolvePlayable(app, test.shapedKey).sample == render->second.sample.get(), "keys and the timeline play the render");
    }
    check(test.pausedOk, "pause holds the transport still");
    check(test.resumedOk, "resume continues it");
    check(test.newKitEmpty && test.switchedBack && session.kits.size() == test.kitsAtStart + 1, "kits can be added and switched");

    const auto loop = bounceLoop(app);
    float peak = 0.0f;
    for (float value : loop->stereo) peak = std::max(peak, std::fabs(value));
    check(peak > 0.01f, "the bounced loop contains sound");
    check(!saveNewSound(app, loop, "Self test loop", false).empty(), "the bounced loop saved as a sound");
    check(app.sounds.size() >= test.soundsAtStart + 1, "saved sounds are listed");

    const auto recorded = app.session.timeline.events;
    tap(GLFW_KEY_ESCAPE);
    onKey(app, GLFW_KEY_Z, GLFW_PRESS, kShortcutMod);
    onKey(app, GLFW_KEY_Z, GLFW_RELEASE, kShortcutMod);
    check(app.session.timeline.events.size() + 1 == test.eventsBeforePlacing + test.placedEvents ||
              test.placedEvents == 0,
          "undo removes the last change");
    check(test.keyLit, "a key is lit while its sound plays");

    // Copying hits around: duplicate one, then copy both and paste them a bar later.
    Timeline& timeline = session.timeline;
    timeline.events.clear();
    app.selection.clear();
    const Sample* firstSound = findLoaded(app, session.kits[0].keys[0]);
    if (!firstSound) {
        check(false, "the kit's first sound is loaded");
        return 2;
    }
    const SoundKey first{session.kits[0].keys[0], 0, firstSound->frames};
    addPlacedHit(app, first, 0.0, 1.0f, false);
    selectAll(app);
    onKey(app, GLFW_KEY_D, GLFW_PRESS, kShortcutMod);
    const bool duplicated = timeline.events.size() == 2 && timeline.events[1].beat > 0.0;
    selectAll(app);
    onKey(app, GLFW_KEY_C, GLFW_PRESS, kShortcutMod);
    app.editCursor = 4.0;
    onKey(app, GLFW_KEY_V, GLFW_PRESS, kShortcutMod);
    check(duplicated && timeline.events.size() == 4 && timeline.events[2].beat == 4.0 &&
              timeline.events[3].beat == 4.0 + timeline.events[1].beat,
          "hits can be duplicated, copied and pasted");
    app.selection = {timeline.events[3].id};
    onKey(app, GLFW_KEY_BACKSPACE, GLFW_PRESS, 0);
    check(timeline.events.size() == 3 && app.selection.empty(), "Delete removes the selected hits");

    // A saved loop keeps its arrangement, opens again for editing, and saves over itself.
    app.pendingSound = bounceLoop(app);
    app.pendingIsLoop = true;
    app.replacing.clear();
    std::snprintf(app.nameBuffer.data(), app.nameBuffer.size(), "Self test arrangement");
    app.addToKit = false;
    savePendingLoop(app);
    const fs::path savedLoop = session.editingLoop;
    check(!savedLoop.empty() && fs::exists(arrangementPath(savedLoop)) && app.editableLoops.count(savedLoop),
          "a saved loop keeps its arrangement");
    const double savedBpm = timeline.bpm;
    timeline.events.clear();
    timeline.bpm = 100.0;
    session.editingLoop.clear();
    if (!savedLoop.empty()) openLoopForEditing(app, savedLoop);
    check(timeline.events.size() == 3 && timeline.bpm == savedBpm && session.editingLoop == savedLoop.string(),
          "a saved loop opens on the timeline again");
    timeline.events.pop_back();
    requestSaveLoop(app);
    check(app.replacing == savedLoop && app.replaceLoop, "saving again offers to update it");
    const Sample* before = findLoaded(app, savedLoop.string());
    savePendingLoop(app);
    check(findLoaded(app, savedLoop.string()) != before && session.editingLoop == savedLoop.string(),
          "updating it replaces the sound everywhere");
    Session reread;
    check(loadSession(reread, arrangementPath(savedLoop).string(), app.sampleRate) && reread.timeline.events.size() == 2,
          "and its arrangement");
    app.pendingSound.reset();
    app.pendingIsLoop = false;
    app.replacing.clear();
    app.nameOpen = false;
    performUndo(app);  // back from opening it
    check(timeline.events.empty() && timeline.bpm == 100.0, "undo puts back what was there before opening it");

    app.session.timeline.events = recorded;  // keep the take for the next run's restore check
    app.session.timeline.bpm = savedBpm;
    return failures == 0 ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
    SelfTest selfTest;
    std::string file;
    std::string dataDirOverride;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--analyze" && i + 1 < argc) {
            const std::string path = argv[++i];
            const float sensitivity = i + 1 < argc ? std::strtof(argv[i + 1], nullptr) : 0.5f;
            return runAnalyze(path, sensitivity);
        }
        if (arg == "--self-test" && i + 1 < argc) {
            selfTest.capture = argv[++i];
        } else if (arg == "--data-dir" && i + 1 < argc) {
            dataDirOverride = argv[++i];
        } else {
            file = arg;
        }
    }
    const bool selfTesting = !selfTest.capture.empty();

    platform::init();

    if (!glfwInit()) {
        std::fprintf(stderr, "Couldn't initialize GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(1440, 900, "Amenity", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Couldn't create a window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    // Vsync off: the main loop below paces itself so key presses don't wait for a frame.
    glfwSwapInterval(0);

    App app;
    gApp = &app;
    startAudio(app);
    if (selfTesting) app.volume = 0.0f;
    app.engine->setMasterGain(app.volume);

    const fs::path musicRoot = platform::homeDir() / "Music";
    if (!dataDirOverride.empty()) {
        app.dataDir = dataDirOverride;
    } else if (selfTesting) {
        app.dataDir = fs::temp_directory_path() / "amenity-self-test";  // never the real session
    } else {
        app.dataDir = musicRoot / "Amenity";
    }
    app.soundsDir = app.dataDir / "Sounds";
    app.sessionPath = app.dataDir / "session.amenity";
    std::error_code directoryError;
    fs::create_directories(app.soundsDir, directoryError);
    if (directoryError) app.status = "Couldn't create " + app.soundsDir.string();

    app.session.sampleRate = app.sampleRate;
    int sessionVersion = 0;
    if (loadSession(app.session, app.sessionPath.string(), app.sampleRate, &sessionVersion) && sessionVersion < 2) {
        // The first save will be in the new format, so keep a copy of the old file.
        std::error_code backupError;
        fs::copy_file(app.sessionPath, app.dataDir / "session.v1.backup.amenity", fs::copy_options::skip_existing, backupError);
    }
    app.session.sampleRate = app.sampleRate;
    refreshSounds(app);
    applyTransportSettings(app);
    startStartupLoad(app);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    const fs::path fontPath = platform::uiFontPath();
    if (!fontPath.empty()) io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), 14.0f);
    applyTheme();

    // Installed before ImGui's so ImGui chains to them.
    glfwSetKeyCallback(window, keyCallback);
    glfwSetDropCallback(window, dropCallback);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    app.scanning = std::async(std::launch::async, scanLibrary, musicRoot, musicRoot / "Amenity");
    if (file.empty() && fs::exists(app.session.song)) file = app.session.song;
    const bool songChosen = !file.empty();
    if (songChosen) requestLoad(app, file);

    constexpr double kFrameSeconds = 1.0 / 60.0;
    double nextFrame = glfwGetTime();
    long frame = 0;
    int exitCode = 0;
    std::string shownStatus;
    double statusShownAt = 0.0;

    while (!glfwWindowShouldClose(window)) {
        // Sleep until the next frame is due. A key press wakes this immediately, and its
        // callback goes straight to the engine.
        for (double wait = nextFrame - glfwGetTime(); wait > 0.0; wait = nextFrame - glfwGetTime()) {
            glfwWaitEventsTimeout(wait);
        }
        glfwPollEvents();
        nextFrame = std::max(nextFrame + kFrameSeconds, glfwGetTime());

        finishStartupLoadIfReady(app);
        finishLoadIfReady(app);
        updateRenders(app);
        app.engine->collectRetired();
        if (app.sequenceDirty) publishSequence(app);

        if (app.scanning.valid() && app.scanning.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            app.library = app.scanning.get();
            if (!songChosen && !app.sample && !app.loading.valid()) {
                // With nothing to restore, start on an Amen break if the library has one.
                const auto amen = std::find_if(app.library.begin(), app.library.end(), [](const fs::path& p) {
                    return lowercase(p.filename().string()).find("amen") != std::string::npos;
                });
                if (amen != app.library.end()) requestLoad(app, amen->string());
            }
        }

        // Saved after any change, at most once a second, so a crash loses very little.
        if (app.sessionDirty && glfwGetTime() - app.lastSave > 1.0 && app.dragEvent == 0) {
            if (!saveSession(app.session, app.sessionPath.string())) app.status = "Couldn't save the session";
            app.sessionDirty = false;
            app.lastSave = glfwGetTime();
        }

        // Messages clear after a few seconds, bringing back the key hints. A load in
        // progress keeps its message up until it's finished.
        if (app.status != shownStatus) {
            shownStatus = app.status;
            statusShownAt = glfwGetTime();
        } else if (!app.status.empty() && !app.loading.valid() && glfwGetTime() - statusShownAt > 5.0) {
            app.status.clear();
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        app.textFocused = io.WantTextInput || app.nameOpen;
        drawUi(app);
        ImGui::Render();

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.086f, 0.09f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (selfTesting) {
            const int result = stepSelfTest(app, selfTest, ++frame, width, height);
            if (result >= 0) {
                exitCode = result;
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
        }

        glfwSwapBuffers(window);
    }

    if (!saveSession(app.session, app.sessionPath.string())) {
        std::fprintf(stderr, "Couldn't save the session to %s\n", app.sessionPath.c_str());
    }

    if (app.loading.valid()) app.loading.wait();
    if (app.scanning.valid()) app.scanning.wait();
    if (app.batch.valid()) app.batch.wait();
    if (app.renderJob.valid()) app.renderJob.wait();
    if (app.deviceRunning) ma_device_uninit(&app.device);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return exitCode;
}
