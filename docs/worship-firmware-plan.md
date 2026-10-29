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

- **Omnisphere pass** (Zach: "still too many pads, and it sounds like a 90's korg synth, I need omnisphere vibes"):
  - **No pads or drones left.** All PAD layers and drone basses are gone; sustained quarter-note bells became dotted 8ths. Measured "static wash" (stretches where the level holds still like a pad): 4% → 0%.
  - **Hybrid voices:** every pluck and sample now carries a pitched-breath "air" halo (noise through a narrow band-pass at the note, an octave up) on its own envelope. It blooms after the attack and settles in about a second, so plucks and samples stop sounding like dry 90s ROMpler presets.
  - New HALO PLUCK (soft synth pluck inside a big halo) and AIR SEQ (pure pitched-breath sequence, a rhythmic texture bed).
  - **Constant motion:** tempo-locked cutoff LFOs per patch (16th to 4 bars, restarting with the transport) and a slow 2-bar auto-pan per voice. Measured: brightness swings about 1.3× over the cycle on a held note.
  - **Shimmer reverb:** an octave-up granular pitch shifter in the plate's feedback, set per scene (20–80). Stable at maximum: the tail decays to silence, and upper-octave energy in the tail rises from 0.16 to 0.91.
  - Levels: scenes within 3.6 dB, peaks ≤ 0.71. CPU: about 1,400 cycles/sample on x86 for a busy halo scene (≈ +25% over the previous pass).

- **Clarity pass** (Zach: "less fuzzy with the sounds, it sounds like everything is super fuzzy"):
  - Measured each stage's off-harmonic energy ("fuzz"). The air halo (filtered noise) was the cause: as loud as the notes themselves (−3 to +3 dB). Drive, the analog stage and the sample codec (56–62 dB SNR) were not.
  - Air is off on all voices except a faint tonal halo on HALO PLUCK. AIR SEQ is removed from the scenes; the oscillator noise ingredient is zeroed.
  - Shimmer is capped at 30 with sin² grain crossfades. Detune is about a third lower on SUPERSAW, DETUNE PLUCK and JUNO ARP.
  - Per voice: piano fuzz −2.7 → −25.7 dB, marimba +2.4 → −18.4, glass pluck −5.2 → −29.0.
  - Whole mix: tones stand 42 dB above the noise floor (was 27); 1–8 kHz spectral flatness 0.033 → 0.010.
  - Levels re-balanced: scenes within 2.9 dB, peaks ≤ 0.72.
  - Diagnostic switches `LUSH_AIR_SCALE`, `LUSH_DRIVE`, `LUSH_ANALOG` are kept for host measurements.

- **Beyond-GM sounds** (Zach: "any other sounds we can do beside the basic GM stuff" → "build it"):
  - **HANDPAN, KALIMBA** (modal synthesis): four tuned sine partials, each with its own decay, all struck together.
    - Handpan partials 1:2:3 plus a faint shimmer; rings about 4 s.
    - Kalimba tine 1 : 5.4 : 13.2 plus a body octave; about 2 s.
    - Both within 1 cent, fuzz about −42 dB. No noise, no sample memory.
  - **VOX OOH, VOX EE** (formant voice): detuned saws through three parallel vowel band-passes that morph with the filter envelope ("ooh"→"aah", "ee"→"oh"), with tempo-locked motion and a 5.2 Hz vibrato fading in to ±14 cents.
  - **MUTED GTR**: plucked-string model with palm-mute damping (−40 dB in 0.28 s) through an amp-style filter. Paired with the dual delay for the dotted-8th guitar sound on keys.
  - **REV PIANO, REV GLOCK**: bank samples played backwards. The start point is computed from the tempo so the swell lands on the attack exactly 2 beats later (measured within 40 ms). Whole SEA chunks are decoded into the voice's string-model memory, so no extra RAM. Note-offs are ignored so swells always land.
  - New S_SWELL sequence (trigger on beat 3) so reverse swells hit the downbeat.
  - Placed in DOTTED 8TH, ROLLING 3S, GALLOP, MORNING, SANCTUARY, CASCADE, QUIET PLACE, SYNC LINES, SPIRIT MOVE, ALTAR CALL. Scenes within 2.9 dB; busy-scene CPU about 930 cycles/sample on x86.

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

## Status (2026-10-28)

- Firmware package built on Linux (`tools/fw/build.sh`): app 453,844 / 453,921 bytes, heap 123 KB, image checks pass. Not yet run on hardware.
- Space cuts committed (86744b2 on `worship`); not pushed. Needs Zach's own GitHub repo, never m-vave/fm1_open.
- Next: load FM1-WORSHIP.fwsc in the web emulator (https://joakimeriksson.github.io/fm1-emu/?mhz=240) via "Open .fwsc…", then check boot, all 16 scenes, chord follow and CPU load.

## Emulator test (2026-10-28, fm1-emu web, 240 MHz)

- Boots cleanly to the overview page. The PRESETS knob loads all 16 scenes; names, BPM, loop lengths and patterns display correctly, and FOLLOW is on.
- Plays and stops normally. Stopped, the CPU idles 80% in every scene (stock firmware: 70-80%).
- CPU idle while playing (share of time free, per-second samples):
  - DOTTED 8TH 41, ROLLING 3S 41, STAIRCASE 15, OCTAVE RUN 17, GALLOP 25, MORNING 19-21
  - SANCTUARY 52, HEARTBEAT 50, GLORY BUILD 54-59, CASCADE 32, ANTHEM 8THS 43-50
  - QUIET PLACE 58-62, SYNC LINES 51, SPIRIT MOVE 39, ALTAR CALL 60, NEON SKY 42-60
- STAIRCASE, OCTAVE RUN and MORNING have the least headroom (15-20% idle). Trim them before flashing hardware.
- Audio dropouts heard in the emulator are the host Mac failing to emulate 240 MHz in real time (load 90-130%), not the firmware.
- Not yet checked: chord follow by ear in the emulator.
