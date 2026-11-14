# Amenity

Chop songs and breaks into slices, build your own keyboard of sounds, and record
them to a looping timeline that snaps to the beat. This is an early prototype. See
[docs/DESIGN.md](docs/DESIGN.md) for the design and roadmap.

## Build and run

Requires macOS, CMake 3.24+ and a C++20 compiler. Dependencies download on the first build.

```sh
cmake -S . -B build
cmake --build build -j
./build/amenity                 # reopens your last song, or the first Amen break in ~/Music
./build/amenity path/to/file    # or opens a specific file
```

Supported formats: WAV, MP3, FLAC, AIFF, M4A, AAC.

## Making a loop

1. **Chop:** pick a song under **Songs**, then play its slices with your keyboard.
2. **Bookmark:** after playing a slice you like, press `\` and name it. It's saved as a
   WAV and added to the next free key in your kit.
3. **Build a kit:** repeat with other songs, then press `Tab` to see your kit. Drag sounds
   from the **Sounds** tab onto keys, or drag between keys to rearrange them.
4. **Record:** set the BPM, bars and snap, then press `Return`. After a one-bar count-in,
   everything you play lands on the timeline, snapped to the grid. It loops so you can
   keep layering. Press `Return` again to stop recording while it keeps playing.
5. **Save:** **Save loop** renders one pass as a new sound you can play or even chop. Its
   hits are saved with it, so you can change it later: press **Edit** next to it under
   **Sounds** to put them back on the timeline, then **Save loop** again to update it.
   Kits and anything else using the loop get the new version, and Cmd+Z puts back whatever
   was on the timeline before you opened it.

**Placing without recording:** press `'` for Place mode. Each key you press drops its sound
at the cursor and moves on by the snap value. `Left` and `Right` move the cursor, which also
makes rests, and clicking the timeline sets it. Double-clicking a lane adds another hit of
that sound.

**Moving and copying hits:** click a hit to select it, Shift-click to add more, or drag across
empty space to select everything you drag over. Clicking a lane's name selects all of that
sound's hits. Drag to move the selection, or hold Option while you drag to copy it instead.
Cmd+D repeats the selection straight after itself, which is quick for turning one bar into
four. Cmd+C copies and Cmd+V pastes at the cursor (the orange line), then moves the cursor on,
so pasting again lays the copies end to end. Right-click for the same commands.

**Where playback starts:** Space plays from the blue marker on the ruler. Click the ruler to
move it, and right-click the ruler to put it back at the beginning.

**Looping a break to your BPM:** press **Bookmark song**, select it under **Sounds**, then
turn on **Loop** and **Fit to BPM**. Press its key to start it and again to stop it.

## Shaping sounds

The panel on the left shapes whichever sound you played last, and every hit of that sound
on the timeline follows along. Letting go of a slider plays the result, and right-clicking
one resets it.

| Control | What it does |
| --- | --- |
| Pitch | Up or down in semitones. On its own this also changes the speed, like a record. |
| Keep length | Changes the pitch without changing the speed |
| Loop | Repeats until you press the key again, trimmed to whole bars if its BPM is known |
| Fit to BPM | Matches its tempo to the timeline: **Keep pitch** stretches it, **Re-pitch** speeds it up like a record |
| Its BPM | The sound's own tempo, detected for you. If a fit is twice as fast or slow, use `/2` or `x2`. |
| Level, Tone | Volume, and darker or thinner |
| Punch | Compression |
| Drive, Crunch | Saturation, and old-sampler lo-fi |
| Space, Echo | Reverb, and a delay in time with the BPM |

**More** opens the full detail: fine tuning, pan, fades, filter type, every compressor
setting, bit depth, echo timing and reverb size. Moving a simple slider rewrites the detail
settings of its own effect.

Changes render in the background and apply from the next hit, so a sound that's already
ringing finishes as it was.

A loop never has silence added to it. If a sound is a little short of a whole number of bars
at its BPM, it's treated as exactly that many bars, played a touch faster. Other lengths are
trimmed to the nearest 16th note, and a loop with no BPM loses any silence at its end.

## Keys

| Input | Action |
| --- | --- |
| `1`-`0`, `Q`-`P`, `A`-`;`, `Z`-`/` | Play a slice or kit sound |
| Shift + key | Ghost note (quieter) |
| Option + key | Play in reverse |
| `` ` `` | Pause or resume everything playing. With nothing playing, plays the song. |
| `Tab` | Switch between the song's slices and your kit |
| `-` and `=` | Previous or next kit |
| `'` | Place mode on or off |
| `Left` and `Right` | Move the Place cursor |
| `\` | Bookmark the last slice played |
| Cmd+C, Cmd+X, Cmd+V | Copy, cut or paste timeline hits. Pasting goes at the cursor. |
| Cmd+D | Repeat the selected hits straight after themselves |
| Cmd+A | Select every hit |
| Delete | Delete the selected hits |
| `Space` | Play or stop the timeline |
| `Return` | Start or stop recording |
| Cmd+Z | Undo. One undo removes a whole recorded take. |
| `[` and `]` | Previous or next bank, when a song has more than 40 slices |
| `Esc` | Stop everything, discarding a pause, and deselect |

A key lights up while its sound is playing, with a line showing how far through it is. It turns
orange while paused.

**Choke** makes a new hit cut off other hits from the same file, so sounds from different
songs layer. **Gate** stops a sound when you release its key, and recording keeps that length.

## Mouse

| Where | Action | Result |
| --- | --- | --- |
| Waveform | Click | Play the slice under the cursor |
| Waveform | Shift-click | Play the whole song from there |
| Waveform | Drag a marker | Move it |
| Waveform | Double-click | Add a marker |
| Waveform | Right-click a marker | Remove it |
| Waveform | Scroll up/down, sideways | Zoom, pan |
| Kit key | Right-click | Clear the key |
| Sounds tab | Right-click a sound | Add to kit, rename, or move to the Trash |
| Kits tab | Right-click a kit | Rename, duplicate or delete it |
| Timeline | Click a hit | Select it and hear it. Shift-click adds to the selection. |
| Timeline | Drag a hit | Move the selected hits, snapped to the grid |
| Timeline | Option-drag a hit | Copy the selected hits |
| Timeline | Drag a hit's right edge | Change how long it plays |
| Timeline | Click empty space | Move the cursor there and deselect |
| Timeline | Drag across empty space | Select the hits inside |
| Timeline | Click a lane's name | Select all of that sound's hits |
| Timeline | Double-click a lane | Add a hit of that sound |
| Timeline | Right-click | Copy, cut, duplicate, delete or paste |
| Timeline ruler | Click | Choose where playback starts |
| Timeline ruler | Right-click | Play from the beginning again |
| Sounds tab | Edit | Put a saved loop's hits back on the timeline |

Moving the sensitivity slider or switching slice mode recalculates the slices and
discards manual marker edits. Hits already recorded aren't affected.

## Where things are saved

Everything is under `~/Music/Amenity`:

- `Sounds/`: your bookmarks and saved loops, as ordinary WAV files. A saved loop also has an
  `.amenity-loop` file beside it holding its hits, which renaming or trashing it takes along.
- `session.amenity`: your kits, timeline and sound settings, saved automatically.
- `session.v1.backup.amenity`: a copy of your session from before multiple kits.

The session refers to songs by their location. If you move or rename a song in Finder,
hits from it turn red on the timeline and stay silent until it's back in place.

## Tests and tools

```sh
./build/amenity_tests                        # engine, timeline and slicer tests, no audio device needed
./build/amenity --analyze path/to/file 0.5   # print slice times at a given sensitivity
./build/amenity --self-test out.ppm [file]   # silent end-to-end run in a temp folder, saves a screenshot
```
