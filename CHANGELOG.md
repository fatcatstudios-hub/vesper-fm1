# VESPER changelog

## 2.0.1 (2026-10-30)
- Identity `FM-1_8837201`, the FM-1 Firmware Hub's pattern for VESPER (2.0.0 reported `FM-1_8837020000`). Nothing else changed.

## 2.0.0 (2026-10-30)
VESPER is rebuilt on fm1-base and felucca-bsp instead of M-VAVE's FM1 Open SDK. The scenes, sounds and screens are the same.
- **Samples built in.** Piano, marimba and glockenspiel are part of the firmware. There is no separate sample install, and the Firmware Hub installs VESPER directly.
- **User presets:**
  - Hold SEL and turn PRESETS to step through your saved presets.
  - A saved preset keeps its scene, sounds, FX and SPACE, even when it is renamed. In 1.x, renaming lost them.
- **Screen:** the panel is tuned as 1.x tuned it. Update screens are in VESPER's colours.
- **Audio:**
  - Lighter on the chip: the sample decoder and the voices' maths are rewritten, with identical sound.
  - Under heavy load, the oldest synth voices are released first instead of the sound breaking up.
- Bluetooth MIDI is not in 2.0 yet.
- Projects saved in 1.x do not carry over.

## 1.1.0 (2026-10-29)
- Smoother screen, about 8 KB more free memory, SEQ with 5 pages, and VESPER's own identity `FM-1_8837001`.

## 1.0.0 (2026-10-29)
First release.
