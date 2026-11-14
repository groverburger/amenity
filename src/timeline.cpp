#include "timeline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

constexpr double kSameBeat = 1e-6;
constexpr const char* kHeaderV1 = "amenity-session 1";
constexpr const char* kHeader = "amenity-session 2";

bool sameSound(const TimelineEvent& a, const TimelineEvent& b) {
    return a.path == b.path && a.start == b.start && a.end == b.end;
}

// Splits on tabs into at most `count` fields, so the last one (a path) can't be broken up.
std::vector<std::string> splitFields(const std::string& line, size_t count) {
    std::vector<std::string> fields;
    size_t from = 0;
    while (fields.size() + 1 < count) {
        const size_t tab = line.find('\t', from);
        if (tab == std::string::npos) break;
        fields.push_back(line.substr(from, tab - from));
        from = tab + 1;
    }
    fields.push_back(line.substr(from));
    return fields;
}

}  // namespace

double snapBeat(double beat, double grid) {
    if (grid <= 0.0) return beat;
    return std::round(beat / grid) * grid;
}

double wrapBeat(double beat, double loopBeats) {
    if (loopBeats <= 0.0) return beat;
    return beat >= loopBeats - kSameBeat ? std::max(0.0, beat - loopBeats) : beat;
}

uint64_t addHit(Timeline& timeline, TimelineEvent event) {
    for (const TimelineEvent& existing : timeline.events) {
        if (sameSound(existing, event) && std::fabs(existing.beat - event.beat) < kSameBeat) return 0;
    }
    event.id = timeline.nextId++;
    const auto where = std::upper_bound(timeline.events.begin(), timeline.events.end(), event.beat,
                                        [](double beat, const TimelineEvent& e) { return beat < e.beat; });
    const uint64_t id = event.id;
    timeline.events.insert(where, std::move(event));
    return id;
}

void quantizeAll(Timeline& timeline, double grid) {
    const double loop = timeline.loopBeats();
    std::vector<TimelineEvent> kept;
    kept.reserve(timeline.events.size());
    for (TimelineEvent& event : timeline.events) {
        if (event.beat < loop) event.beat = wrapBeat(snapBeat(event.beat, grid), loop);
        const bool duplicate = std::any_of(kept.begin(), kept.end(), [&](const TimelineEvent& other) {
            return sameSound(other, event) && std::fabs(other.beat - event.beat) < kSameBeat;
        });
        if (!duplicate) kept.push_back(std::move(event));
    }
    std::stable_sort(kept.begin(), kept.end(),
                     [](const TimelineEvent& a, const TimelineEvent& b) { return a.beat < b.beat; });
    timeline.events = std::move(kept);
}

void removeDuplicates(Timeline& timeline) {
    std::vector<TimelineEvent> kept;
    kept.reserve(timeline.events.size());
    for (TimelineEvent& event : timeline.events) {
        const bool duplicate = std::any_of(kept.begin(), kept.end(), [&](const TimelineEvent& other) {
            return sameSound(other, event) && std::fabs(other.beat - event.beat) < kSameBeat;
        });
        if (!duplicate) kept.push_back(std::move(event));
    }
    timeline.events = std::move(kept);
}

std::vector<TimelineEvent> copyEvents(const Timeline& timeline, const std::set<uint64_t>& ids) {
    std::vector<TimelineEvent> clip;
    for (const TimelineEvent& event : timeline.events) {
        if (ids.count(event.id)) clip.push_back(event);
    }
    if (clip.empty()) return clip;
    double first = clip.front().beat;
    for (const TimelineEvent& event : clip) first = std::min(first, event.beat);
    for (TimelineEvent& event : clip) {
        event.beat -= first;
        event.id = 0;
    }
    std::stable_sort(clip.begin(), clip.end(), [](const TimelineEvent& a, const TimelineEvent& b) { return a.beat < b.beat; });
    return clip;
}

std::vector<uint64_t> pasteEvents(Timeline& timeline, const std::vector<TimelineEvent>& clip, double atBeat) {
    std::vector<uint64_t> added;
    const double loop = timeline.loopBeats();
    for (TimelineEvent event : clip) {
        event.beat += atBeat;
        if (event.beat < 0.0 || event.beat >= loop - kSameBeat) continue;
        // A loop that ran to the end of the timeline still does, from its new place.
        if (event.durationBeats > 0.0) event.durationBeats = std::min(event.durationBeats, loop - event.beat);
        const uint64_t id = addHit(timeline, std::move(event));
        if (id != 0) added.push_back(id);
    }
    return added;
}

double musicalLength(double beats) {
    if (beats <= 0.0) return 0.0;
    if (beats > kBeatsPerBar) return std::ceil(beats / kBeatsPerBar - 1e-9) * kBeatsPerBar;
    double length = 1.0 / 16.0;
    while (length < beats - 1e-9) length *= 2.0;
    return length;
}

bool saveSession(const Session& session, const std::string& path) {
    std::ostringstream out;
    out.precision(17);
    out << kHeader << '\n';
    out << "rate\t" << session.sampleRate << '\n';
    out << "bpm\t" << session.timeline.bpm << '\n';
    out << "bars\t" << session.timeline.bars << '\n';
    out << "quantize\t" << session.quantizeIndex << '\n';
    out << "metronome\t" << (session.metronome ? 1 : 0) << '\n';
    if (!session.song.empty()) out << "song\t" << session.song << '\n';
    if (!session.editingLoop.empty()) out << "editingLoop\t" << session.editingLoop << '\n';
    out << "currentKit\t" << session.currentKit << '\n';
    for (const Kit& kit : session.kits) {
        out << "kitName\t" << kit.name << '\n';  // the key lines after it belong to this kit
        for (size_t i = 0; i < kit.keys.size(); ++i) {
            if (!kit.keys[i].empty()) out << "kit\t" << i << '\t' << kit.keys[i] << '\n';
        }
    }
    for (const auto& [sound, settings] : session.soundSettings) {
        out << "sound\t" << sound.start << '\t' << sound.end << '\t' << settings.serialize() << '\t' << sound.path
            << '\n';
    }
    for (const TimelineEvent& e : session.timeline.events) {
        out << "event\t" << e.beat << '\t' << e.durationBeats << '\t' << e.start << '\t' << e.end << '\t' << e.gain
            << '\t' << (e.reverse ? 1 : 0) << '\t' << (e.choke ? 1 : 0) << '\t' << e.path << '\n';
    }

    const std::string temporary = path + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << out.str();
        file.flush();
        if (!file) return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}

namespace {
// Accepts Windows line endings too, in case the file was edited by hand.
bool readLine(std::istream& in, std::string& line) {
    if (!std::getline(in, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}
}  // namespace

bool loadSession(Session& session, const std::string& path, uint32_t targetRate, int* version) {
    std::ifstream file(path, std::ios::binary);
    std::string line;
    if (!file || !readLine(file, line) || (line != kHeader && line != kHeaderV1)) return false;
    if (version) *version = line == kHeader ? 2 : 1;

    Session loaded;
    loaded.kits.clear();
    uint32_t savedRate = targetRate;
    int savedCurrentKit = 0;
    const auto scaled = [&](size_t frame) {
        if (savedRate == targetRate || savedRate == 0) return frame;
        return static_cast<size_t>(static_cast<double>(frame) * targetRate / savedRate);
    };
    while (readLine(file, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string key = line.substr(0, tab);
        const std::string value = line.substr(tab + 1);
        if (key == "rate") {
            savedRate = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "bpm") {
            loaded.timeline.bpm = std::clamp(std::strtod(value.c_str(), nullptr), 20.0, 400.0);
        } else if (key == "bars") {
            loaded.timeline.bars = std::clamp(std::atoi(value.c_str()), 1, 256);
        } else if (key == "quantize") {
            loaded.quantizeIndex = std::clamp(std::atoi(value.c_str()), 0, static_cast<int>(kQuantizeOptions.size()) - 1);
        } else if (key == "metronome") {
            loaded.metronome = std::atoi(value.c_str()) != 0;
        } else if (key == "song") {
            loaded.song = value;
        } else if (key == "editingLoop") {
            loaded.editingLoop = value;
        } else if (key == "kit") {
            const auto fields = splitFields(value, 2);
            if (fields.size() != 2) continue;
            const int index = std::atoi(fields[0].c_str());
            if (loaded.kits.empty()) loaded.kits.emplace_back();  // older files have one unnamed kit
            if (index >= 0 && index < kKeysPerBank) loaded.kits.back().keys[static_cast<size_t>(index)] = fields[1];
        } else if (key == "kitName") {
            loaded.kits.emplace_back();
            loaded.kits.back().name = value;
        } else if (key == "currentKit") {
            savedCurrentKit = std::atoi(value.c_str());
        } else if (key == "sound") {
            const auto fields = splitFields(value, 4);
            if (fields.size() != 4) continue;
            SoundKey sound;
            sound.start = scaled(std::strtoull(fields[0].c_str(), nullptr, 10));
            sound.end = scaled(std::strtoull(fields[1].c_str(), nullptr, 10));
            sound.path = fields[3];
            loaded.soundSettings[sound] = SoundSettings::parse(fields[2]);
        } else if (key == "event") {
            const auto fields = splitFields(value, 8);
            if (fields.size() != 8) continue;
            TimelineEvent event;
            event.beat = std::strtod(fields[0].c_str(), nullptr);
            event.durationBeats = std::strtod(fields[1].c_str(), nullptr);
            event.start = scaled(std::strtoull(fields[2].c_str(), nullptr, 10));
            event.end = scaled(std::strtoull(fields[3].c_str(), nullptr, 10));
            event.gain = std::strtof(fields[4].c_str(), nullptr);
            event.reverse = fields[5] == "1";
            event.choke = fields[6] == "1";
            event.path = fields[7];
            event.id = loaded.timeline.nextId++;
            loaded.timeline.events.push_back(std::move(event));
        }
    }
    std::stable_sort(loaded.timeline.events.begin(), loaded.timeline.events.end(),
                     [](const TimelineEvent& a, const TimelineEvent& b) { return a.beat < b.beat; });
    if (loaded.kits.empty()) loaded.kits.emplace_back();
    loaded.currentKit = std::clamp(savedCurrentKit, 0, static_cast<int>(loaded.kits.size()) - 1);
    loaded.sampleRate = targetRate;
    session = std::move(loaded);
    return true;
}
