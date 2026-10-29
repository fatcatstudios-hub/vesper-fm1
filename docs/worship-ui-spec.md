# WORSHIP firmware — control and page spec (from Zach, 2026-10-29)

Four sound layers per scene. Every page is about those four voices or the global sound.

## Hardware controls
The FM-1 has 14 buttons: OCT-, OCT+, FX, SCL, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ, PLAY, REC.
It also has 7 rotary knobs, none of which press: SELECT, PRESETS, ALGORITHM, K1-K4.
MASTER is an analog pot.

| Control | Job |
|---|---|
| MASTER | Master volume |
| PRESETS | Scroll through presets (scenes); user slots sit at the end of the list |
| ALGORITHM | Choose and highlight the voice (1-4) the ENV / LFO / EDIT / ARP pages show |
| SELECT | Select: scroll through the options on the current page |
| Enter / OK | Confirm, OK, save. **Proposed: SCL**, since SELECT cannot be pressed (open question) |
| K1-K4 | The four parameters shown on the current page |
| PLAY | Play / stop |
| REC | Record |

## Pages (pressing the button again steps to its next page)

| Button | Content |
|---|---|
| HOME | 1st press: crystal orbit (chord in the centre, transposable from here). 2nd: 4-channel mixer. 3rd: master oscilloscope |
| FX | **Global** FX only, never per track. Several pages, 4 parameters each; SELECT scrolls options. FX to add: Microcosm-style glitch / granular stutter, tape (wow, flutter, saturation, age), drive, bit crush, "cloud" shimmer, alongside the existing reverb, delay and chorus |
| ENV | ADSR for each of the 4 voices (ALGORITHM picks the voice) |
| LFO | LFO for each of the 4 voices |
| EDIT | Sound source of each of the 4 voices. Pressing EDIT steps through the edit pages; ALGORITHM picks the voice |
| GLO | Global settings: colour theme, MIDI options, output options, etc. |
| SAVE | Save the sequence easily into open user slots at the end of the preset list |
| ARP | A dedicated arp per voice, for arpeggiating without sequencing |
| SEQ | Build the sequence easily; repeated presses give different edit modes |

## Home orbit details
- The chord in the centre can be adjusted and transposed from the home page.
- Chord names are shown correctly: "Bb", not "BB"; minor as "C#m".

## Process fixes after the first hardware flash (2026-10-29)
- Every build is versioned in its file name (e.g. FM1-WORSHIP-0.6-Updater.html) and shows its version on the start screen and the updater page. The unit had run an older same-named updater file.
- Each build is checked in the emulator before Zach flashes it.
- Restore route that worked: updater page, then Restore original V15, then Force flash, then Connect device (the unit was in update mode, ota-FM1C_...), then Install original V15. After the restore the voices were mono; still to be looked into.
