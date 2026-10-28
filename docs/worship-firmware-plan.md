# FM-1 WORSHIP Firmware — Plan & Status

Fork of M-VAVE's official **FM1 Open V14** (github.com/m-vave/fm1_open, GPLv3 + SDK linking exception). Goal: a dedicated worship/CCM ambient arp machine. Scroll presets, set BPM, play a chord, and it sounds good with no menu diving.

## Status (Oct 28, 2026)

**Working in the browser simulator (built from the same app C code the device runs):**
- 16 worship scenes replace the factory projects: DOTTED 8TH, OPEN HEAVEN, RISING TIDE, STILL WATER, PULSE KEYS, MORNING, SANCTUARY, HEARTBEAT, GLORY BUILD, WIDE OPEN, ANTHEM 8THS, QUIET PLACE, CASCADE, SPIRIT MOVE, ALTAR CALL, FOREVER.
- Each scene has 4 layers: PAD (open add9 voicing, sustained), ARP, BASS, and a 4th layer (sparse bell, second arp, or soft 808 kick). Tracks 5–8 are muted.
- Arps are written as chord degrees with root/5th/9th/3rd shapes rather than 1-3-5 climbs. There are 8 arp rhythms, 4 bass rhythms, and 3 beats. Accents are fixed, so the groove is identical every loop.
- **Chord follow (HOME page):** hold a chord and every pitched layer re-pitches to it.
  - The lowest key is the root.
  - A major or minor third sets the quality; otherwise a 2nd means sus2 and a 4th means sus4.
  - A single key changes only the root.
  - Only key presses read the chord, so releasing never changes it (latch).
  - The screen shows "CHORD A MIN" etc.
- **LUSH engine (replaces DX7 FM on the worship layers).** Zach's feedback on the FM version was "too FM sounding; needs realism, analog, lushness, pluckiness, organic."
  - Integer-only voice engine in the open audio output stage (`app/lush.c`).
  - The FM engine is set silent on those tracks via MIDI-only mode, and the sequencer's notes are routed to LUSH through `groove_midi_note`.
  - Patches:
    - AIR PAD: 3 detuned band-limited saws, slow filter swell.
    - STRINGS: saw + pulse ensemble.
    - GLASS PLUCK: analog pluck with a fast filter envelope.
    - HARP and FELT KEYS: Karplus-Strong strings, tuned within 1 cent.
    - WARM BASS, SUB.
    - AIR BELL: bright string + octave sine.
  - Analog drift, saturated 4-pole filter.
  - Own effects: Juno-style stereo chorus, Dattorro plate reverb (half rate), dark tempo-synced delay. All driven by the normal FX page (decay, delay time, feedback, level) and per-track sends.
  - DC blocker, reverb low cut, −4 dB bus trim with soft limiter. Scenes sit within about 4 dB of each other.
  - Memory: about 90 KB of delay/reverb/string buffers. Benchmark: about 700 cycles/sample on x86 for a busy scene. The device cost is unknown and must be measured.
- Automated tests pass (45 checks, including polymetric loop lengths and chord-tone correctness of every sequence after a chord change). The native harness, analyzer and benchmark are in `tools/lush/`.

- **Sequence-first redesign** (Zach's feedback: "less pads, more cool sequences, arps, delays and more detuned stuff"):
  - 14 sequence figures and 7 bass figures, 1–3 bars long. Highlights: dotted 8ths, rolling 3-over-4, staircase, octave run, gallop, syncopated lines, dotted rise (5-note cycle over dotted 8ths), 16th pulse, cascade.
  - Layers loop at different lengths (e.g. 16/48/16/32), so the pattern keeps evolving while staying on the grid.
  - Chord-tone range goes up to the 17th for octave jumps.
  - Scenes are now two interlocking sequences + sequenced bass + beat. Only MORNING, SANCTUARY, QUIET PLACE and ALTAR CALL keep a quiet pad.
  - New detuned patches: DETUNE PLUCK (stereo unison, ±19¢), SUPERSAW (stereo, ±27¢), JUNO ARP (detuned pulse pair + chorus), DETUNE BASS. Stereo unison runs two filters per voice (outer saws hard L/R).
  - **Dual-tap delay:** left at the FX time (dotted 8th), right at 4/3 of it (quarter), cross-fed. Delay sends and feedback raised across the scenes.
  - Busiest-case benchmark: about 1,120 cycles/sample on x86 (two stereo sequence layers plus a bass filling all 16 voices). Levers if the device is tight: a voice cap, 2-saw unison, half-rate chorus.

- **Warmth pass** (Zach's feedback: "still sounds very digital and toy like, I need something warm and organic"):
  - Fixed a drift bug that warbled every oscillator's pitch about 65 times a second. Drift is now a slow 0.6–2 s wander plus a fixed random tuning offset per note.
  - Tape echo: wow and flutter on the repeat time, saturation and darkening on every pass.
  - Analog output stage: even/odd saturation and a soft roll-off near 9 kHz.
  - Sequences moved down an octave (ARP from C3, LEAD from C4).
  - Every patch revoiced: 5–10 ms attacks, darker cutoffs, less resonance, more drive, softer plucked strings, bells without the octave "music box" partial.
  - Result: spectral centroid 679 Hz → 430 Hz; 2–5 kHz down 8 dB, above 5 kHz down 13 dB; low mids up about 2.5 dB.

- **Sampled instruments** (Zach: "can we get some samples in there as well"):
  - LUSH sample voice: streams the FM-1's shared sample bank (memory-mapped SEA5, decoded with the SDK's audio-safe decoder and a two-group cache, like the stock sampler) and plays it pitched across the keyboard.
  - It picks the nearest zone by root note, read from each slot's name ("PIANO C4"), so users can load their own instruments.
  - Each note gets a small random start offset and level change (no machine-gun repeats). Notes run through the warm filter and LUSH effects.
  - Factory bank `docs/WORSHIP-SAMPLES.bin`: CC0 VSCO-2 CE soft upright piano (4 zones), marimba (3), glockenspiel (1, played an octave up). 14.7 s of 19.55 s. Tuned within ±3.5 cents across the range; passes the firmware's own bank validator.
  - Used in 10 scenes (MORNING, CASCADE, HEARTBEAT, QUIET PLACE, ALTAR CALL, SANCTUARY, STAIRCASE, DOTTED 8TH, ROLLING 3S, GALLOP). In MORNING the samples carry about 40% of the mix energy.
  - Credits: `docs/SAMPLE-CREDITS.md`.
  - **To load on hardware:** write `WORSHIP-SAMPLES.bin` to the device with the workbench's sample bank restore. Back up the current bank first, since this replaces M-VAVE's factory drum samples (the worship scenes use the 808 engine for drums).

**Not started yet:** shimmer/compression, simple pattern controls, an always-visible key display, and building and flashing real firmware.

## Architecture findings
- **Open (ours to modify):** UI/screen, presets and projects, MIDI routing (including `groove_midi_mode` / `groove_midi_note`, which the engine calls for every sequenced note), and the final audio output stage.
- **Closed binary (`libfm1_engine.a`):** the stock synth engines, sequencer, arp, and stock reverb/delay.
- The device has about 512 KB of internal RAM, no SDRAM, and no floating point in the app code. LUSH is fixed-point and keeps big buffers as int16.
- Samples: 8 slots sharing about 19.5 s. The stock sampler is a drum-pad engine; a pitched multi-sample player would be ours to build in LUSH.
- Clock: internal 30–300 BPM, tap tempo, MIDI clock in (DIN/USB/BLE).

## Build & flash
- **Browser preview:** `tools/build-web-linux.mjs` with WASI SDK 27. Tests: `tools/test-worship.mjs`. Demo: `tools/render-demo.mjs`.
- **Real firmware:** needs **Windows x64 + JieLi toolchain 2.5.3**. It does not build natively on Apple Silicon. Options:
  1. A Windows PC.
  2. Parallels/UTM with x64 emulation (untested).
  3. GitHub Actions on Windows (untested).
- Flash through the FM1 workbench, which can return to stock V15. Back up first. Use one unit as the test unit.
- **Risk to verify on hardware:** the closed engine must call the app's `groove_midi_mode` / `groove_midi_note` the way the simulator does. They are app-provided hooks, so this is expected but unconfirmed.

## Roadmap
1. **Hardware bring-up:** build route, then stock flash, then WORSHIP flash.
2. **Measure CPU** (`fm1_audio_stats` peak ticks, overruns) with the busiest scene. If it's tight:
   - cut the voice count or pad unison,
   - move the filter to 32-bit math,
   - run the chorus at half rate.
3. **Realism pass:** pitched multi-sample voices in LUSH (felt piano / Rhodes / acoustic guitar) from the 19.5 s bank, about 4 zones × ~1 s with loops.
4. **FX:** shimmer and compression on the LUSH bus, within the budget.
5. **Performance screen:**
   - scene, BPM, KEY/CHORD and 4 layer levels always visible,
   - encoders as Reverb / Delay / Tone / Density macros,
   - one button per layer.
6. **Simple pattern controls:** Rhythm, Density, Octave, Feel.
7. **Packaging:** GPL source must stay available. Free vs. paid is Zach's call.

## Open questions for Zach
- Which build route (a Windows PC available?).
- Keys: chords only, or also a lead over the top?
- After listening: which LUSH voices are closest, and which still miss?
- Distribution: free or a paid Fat Cat Studios product?
