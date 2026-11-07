# Amenity: design

An instrument for chopping songs and breaks, playing the pieces from a computer
keyboard, designing synths and drums, and capturing it all into loops.

**Core idea: everything is a sound on a key, and anything you play can be captured.**
A chop, a drum hit and a synth patch are the same kind of thing: assignable to a
key, shaped with the same tools, and recordable into a loop.

## Decisions

| Topic | Decision | Why |
| --- | --- | --- |
| Input | Computer keyboard | No hardware needed. MIDI can be added later as a second event source. |
| Main screen | Clip grid plus a simple timeline | Jam on the grid, then record the performance to arrange it. |
| Sound design | Macro knobs plus a mutate button | Fastest route to good sounds. A detail view provides fine control. |
| Stack | C++20, Dear ImGui, GLFW, miniaudio | Light, permissively licensed, and quick to iterate on custom UI. |
| Key layout | Full keyboard: 40 keys per bank | One chopped song can sit under both hands. |
| Auto-chop | Beats, transients, labels, song sections | No large model dependency. Stem separation stays a possible later add-on. |
| Plugins | Built-in engines and effects only | Simpler, stable, and tailored to the macro workflow. |

## Dynamics without velocity

A computer keyboard cannot sense how hard a key is pressed, so dynamics come from:

- Shift for ghost notes (implemented).
- Per-slice level and accent settings (planned).
- The mangle generator, which writes dynamics into patterns (planned).

Many keyboards also drop some combinations of three or more keys. Layouts should
keep sounds that are often played together on keys that register reliably.

## Architecture

```
 main thread                                  audio thread
 ------------------------------------         -----------------------
 GLFW key events ---+
                    +--> Engine::trigger ---> SPSC queue --> Engine::render
 ImGui UI ----------+                                        (voices, soft clip)

 worker threads: decode, onset analysis, library scan
```

`src/engine` has no device or window dependency. Tests call `Engine::render`
directly and inspect exact samples.

### Real-time rules

The audio thread never locks, allocates, frees, or touches files. Specifically:

- All communication uses the single-producer, single-consumer command queue plus atomics.
- Trigger commands carry their own frame range, so the audio thread never reads
  the slice table and markers can be edited during playback.
- Samples are immutable once published. The main thread keeps each sample alive
  until the audio thread acknowledges having moved to a newer one, so a sample is
  never freed on the audio thread.

### Keeping keys independent of frame rate

Waiting on vsync would hold key events for up to a full frame. Instead, vsync is
off and the main loop sleeps in `glfwWaitEventsTimeout` until the next frame is
due. A key press wakes it immediately and goes straight to the engine, so the most
it waits is the time spent drawing a frame, not the frame interval.

### Transport and recording

The transport lives on the audio thread and splits each block at every event, click
and loop point, so timing is sample-accurate at any buffer size. The timeline is
published to it as an immutable `Sequence`, replaced whole on each edit.

A hit sounds immediately when pressed and is stored at its snapped position. If that
position is just ahead of the playhead, the event would sound a second time, so it's
marked to skip one pass.

A new hit cuts off (chokes) only voices from the same file. Break chops still cut each
other, while a kit built from several songs layers naturally.

A pause freezes the voices sounding at that moment and the transport, each keeping its
position, while new hits still play. That lets you jam over a paused song.

### Shaping sounds

Settings belong to a sound (a frame range of a file), not to a key or a hit, so all of
its hits and any kit using it change together, and a timeline lane is simply a sound.

Effects render ahead of time on a worker thread into a processed copy of the sound,
rather than running on the audio thread. This keeps the engine simple, costs no CPU per
voice, and means a bounce matches playback exactly. The tradeoff is that a change applies
from the next hit, rather than to a sound that's already ringing. A replaced copy isn't
freed until nothing is playing it, so tweaking a slider doesn't cut a sound off.

Time-stretching uses Signalsmith Stretch. Its one-shot call returns silence for input
shorter than its analysis window, so sounds are padded with silence and trimmed again.
Fitting uses the sound's own BPM rather than a target length, so all slices of a break
scale by the same ratio and stay in time with each other. A loop is first trimmed to whole
bars at that BPM, so the tail of a sampled break doesn't push it off the grid.

### Saved sounds and sessions

A bookmark is an ordinary WAV file named by the user, so it keeps working if the
source song moves and is usable in other software. The session file refers to
sounds and songs by path, and stores frame positions along with their sample rate.

### Slicing

`computeOnsetFunction` does the slow part once per file: a log-compressed spectral
flux. `pickOnsets` is quick enough to re-run as the sensitivity slider moves. It
uses an adaptive threshold, then refines each onset to the largest energy jump
nearby, backs up about 1 ms so the attack is not clipped, and snaps to a zero
crossing.

## Roadmap

### M1: playable prototype (done)
Load a file, slice by transients or grid, edit markers, view the waveform, and play
slices from the keyboard with low latency. Includes the library browser, choke,
gate, ghost notes, reverse and banks.

### M1.5: bookmarks, kit and timeline (done)
Save slices as named WAV sounds, assign sounds from any song to a custom kit, and
record to a looping timeline with snap, metronome and count-in. Sessions save
automatically, and a loop can be rendered back into a new sound.

### M1.6: shaping, placing and multiple kits (done)
Per-sound pitch, tempo fit, looping and effects, with simple sliders over a detailed
view. Pause and resume, placing hits without recording, and any number of kits.

Not yet: multiple or arranged clips, redo, master-bus effects, moving hits between
lanes, and starting loops in time with the transport when played live.

### M2: smart chopping
- Downbeat detection, and snapping slices to a tempo grid
- Slice labels: kick, snare, hat, vocal, other
- Song section detection for finding loopable phrases
- Background analysis cache for `~/Music`
- OGG (`stb_vorbis`) and optional WebM (FFmpeg) decoding

### M3: break tools
- Per-slice ratchet and choke group
- Mangle dial with a guaranteed path back to the original

### M4: capture and looping
- Multiple loops, each with its own length
- Always-on capture of recent playing
- Resampling the live master output into a new sample

### M5: sound design
- Drum engines: kick, snare, hat, with a blendable sample layer
- Synth engines: wavetable, FM, sample-as-oscillator
- Macro knobs and mutate with undo for synth engines

### M6: clip grid and timeline
- Clip grid, scenes, recording performances to the timeline
- Project save and load, audio export

## Known technical debt

`src/main.cpp` holds all the UI and app logic, and is due to be split up by panel.

## Open questions

- **Synths on a full-keyboard layout:** slices use all 40 keys. Playing synths
  melodically likely needs a toggle that turns the keyboard into a piano.
- **Slice banks versus sections:** long songs produce more slices than one bank
  holds. Banks work for now; M2 could fill banks from detected song sections.
