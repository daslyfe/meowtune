# MeowTune

A pitch correction plugin (VST3 + AU, macOS universal) in the Meow Terminal look.

- **Pitch display**: the detected input note, cents and frequency in the header, a scrolling graph of the input (muted) against the tuned pitch (white), and the live pitch marked above the keyboard.
- **Scale**: KEY and SCALE pickers (19 scales, plus CUSTOM), or click keys on the C2–C7 keyboard to add or remove a note in every octave; drag across keys to paint. Editing the keyboard switches SCALE to CUSTOM, starting from the scale that was on. `>>ALL` (chromatic) / `>>NONE`. KEY and SCALE are host parameters, so they can be automated or mapped to a MIDI CC like the sliders; KEY does nothing in CUSTOM.
- **TUNE**: AMOUNT (how far the pitch is pulled to the note), GLIDE (retune time, 0 = hard snap), PITCH (shifts the input ±24 semitones before it is tuned, so it lands on different notes).
- **FORMANT**: CORRECT (100% keeps the voice's formants where they were, 0% lets them move with the pitch), SHIFT (moves the formants ±12 semitones without changing pitch).
- **UNISON**: VOICES (1–8 copies), DETUNE (cents either side), SPREAD (stereo width).
- **VOICE**: RANGE, the lowest note it tracks: BASS 70 Hz, TENOR 100 Hz (default), ALTO 150 Hz, SOPRANO 220 Hz. A higher range has less latency; a voice below it is not tuned (it comes out rough). Not automatable, since it changes the latency.
- **OUTPUT**: MIX, OUTPUT dB, input and output meters.
- **MIDI** (polyphonic): SNAP mode, held notes replace the scale while held (any octave). CHORD mode, every held note sings at its own pitch from the one voice, up to 8 at once, with velocity. With no notes held, both modes tune to the scale.
- **SETTINGS**: colours (background, text, accent, muted, border), shared by every MeowTune on the Mac and saved in `~/Library/Application Support/MeowTune/colours.json`.

In Ableton Live, put MeowTune on the vocal track, then on a MIDI track set *MIDI To* to the vocal track and *MeowTune*.

## How it works

`Source/TuneEngine.*` (no JUCE): the input is summed to mono, its pitch found with YIN on a ~12 kHz copy, and pitch marks laid one period apart. Each voice is TD-PSOLA: grains two periods long are re-spaced to the target pitch and resampled to place the formants. The voices are summed to stereo and mixed with the dry signal delayed by the same latency.

Latency is about 3.5 periods of the RANGE's lowest pitch, reported to the host so delay compensation keeps the track in time:

| RANGE | Lowest pitch | Latency at 48 kHz |
|---|---|---|
| BASS | 70 Hz (C#2) | 2411 samples, 50.2 ms |
| TENOR | 100 Hz (G#2) | 1690 samples, 35.2 ms |
| ALTO | 150 Hz (D3) | 1130 samples, 23.5 ms |
| SOPRANO | 220 Hz (A3) | 775 samples, 16.1 ms |

Even SOPRANO is more than is comfortable for a singer monitoring through it. The top of the detection range is 1100 Hz in every range.

## Build

```sh
git submodule update --init
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target MeowTune_VST3 MeowTune_AU
./scripts/install.sh
```

## Tests

| Test | Checks |
|---|---|
| `build-engine/engine_test` (`cmake -S . -B build-engine -DMEOWTUNE_PLUGIN=OFF`) | latency per RANGE, detection 73–880 Hz, snapping to scales at 44.1/48/96 kHz, PITCH, AMOUNT, GLIDE, MIDI SNAP and CHORD, formant correction and shift, levels, unison stereo, tuning just above each RANGE's floor, switching RANGE mid-stream, below-the-floor input staying safe, extreme settings, speed (26x real time at 8 notes x 8 voices) |
| `build/proc_test_artefacts/Release/proc_test` | the editor in a real WKWebView with audio running: held native clicks on keys, drag-painting, pickers, KEY/SCALE moved by the host, RANGE and the reported latency, a slider drag, SNAP/CHORD, tabs, colours saved and reset, host automation, state reload, old sessions' scale choice |
| `build/host_test_artefacts/Release/host_test <vst3> <component>` | the installed plugins at 44.1/48/96 kHz with random block sizes: tuning, host parameters (KEY and SCALE included), RANGE changing the latency the host sees, MIDI in both modes, state round trip, editor |
| `auval -v aumf MTun Meow` | AU validation |
| `node tools/ui_test.mjs` | headless Chrome against a mock: layout at three sizes, wiring, screenshots |

All pass. **Not tried yet:** opening it in Ableton Live or Logic, singing into it, or listening to any of the output. The tests measure pitch, level and spectra; they can't say whether it sounds good.

## Licence

AGPL-3.0-or-later (JUCE's free tier requires it). Font: IBM 3270, BSD-3 (`ui/fonts/LICENSE-3270.txt`). Icon: `Reality_Sea-9` from meowdesignsystem.
