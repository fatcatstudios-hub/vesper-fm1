# VESPER

An ambient worship firmware for the M-VAVE FM-1, by Fat Cat Studios.

![VESPER](docs/images/vesper-boot.png) ![HOME](docs/images/vesper-home.png) ![Mixer](docs/images/vesper-mixer.png) ![FX](docs/images/vesper-fx.png)

Each of VESPER's 16 scenes stacks four lush voices around a key. Typical layers are an arp, a lead, a bass and a swell. Everything stays in a major key you choose. Play the voices live over the scene, record into it, and shape the whole mix with a global effects chain built for pads and atmosphere.

- **Identity:** `FM-1_8837001`
- **Licence:** GPL-3.0 with the FM1 SDK linking exception (see [LICENSE.md](LICENSE.md) and [THIRD_PARTY.md](THIRD_PARTY.md))
- **Built on:** M-VAVE's FM1 Open V14 SDK. The M-VAVE update loader is included, so you can always go back to V15 or move to another firmware.

## Controls

| Control | What it does |
|---|---|
| **HOME** | Press once for the crystal orbit: the four layers turn as rings around the key. Press again for the 4-track mixer. |
| HOME knobs | K1 sets the key, K2 sets **SPACE** (one knob that adds grains, shimmer, reverb, echo and a cleaner low end), K3 sets layer level, K4 sets BPM. |
| **SEL** (hold) | Every knob moves one fine step per click while held. On the mixer, the faders become pan. |
| **ARP** | Switches the keys between setting the key and playing the focused voice live (a green **PLAY** badge shows on HOME). |
| **REC** | Records what you play into the sequence. Works while playing (ARP on). |
| **ALGORITHM** | Picks which of the four voices the EDIT, ENV and LFO pages show. |
| **EDIT** | Five pages per voice: SOUND, TONE, OSC, VOICE and MIX (level, pan, reverb send, echo send). |
| **ENV / LFO** | Amp and filter envelopes; tempo-synced filter motion, autopan and analog drift. |
| **FX** | The global chain, one page per press: **DIRT** → **TONE** → **TEXTURE** → **VERB + DELAY**. |
| **GLO** | Clock and MIDI settings, Bluetooth, swing, click and **THEME** (Crystal, Ember, Forest, Mono). SELECT scrolls; ALGORITHM or K1 changes the value. |
| **SAVE** | Saves to 16 user slots. Open them with HOME → HOME (PRESETS), then turn K1 to switch between USER and FACTORY. |

The global effects:

- **DIRT:** drive (%), bits (16 down to 4), sample rate (kHz) and mix.
- **TONE:** high pass, low pass, a gentle 2:1 compressor and output trim.
- **TEXTURE:** a tempo-synced granular cloud, octave shimmer, stereo width and chorus.
- **VERB + DELAY:** plate reverb and a tape-style stereo echo, with each voice's own sends set on EDIT → MIX.

## Installing

1. Open **FM1-VESPER-1.1.0-Updater.html** (from the release) in Chrome or Edge and connect the FM-1 by USB.
2. Install from M-VAVE's original V15, or from another open firmware.
   - If your unit runs a Felucca-family firmware, restore V15 with that firmware's installer first.
3. **Samples:** the piano, marimba and glockenspiel voices use VESPER's own sample bank (CC0 recordings from VSCO-2 CE; see [docs/SAMPLE-CREDITS.md](docs/SAMPLE-CREDITS.md)). The updater installs it with the firmware.
   - If the FM-1 has no sample bank yet, it is written automatically.
   - If the FM-1 already has samples (V15's or another firmware's), the updater asks first, because writing replaces all 8 slots.
   - The bank is also in the release as `VESPER-SAMPLES.bin`.
4. To leave VESPER, use the same updater page: **Restore original V15**, or install any other firmware.

Back up your projects before switching firmware. Project formats differ between firmwares.

## Building

Build on Linux with the JieLi PI32v2 toolchain (clang 4.0.1):

```
JL=/path/to/jieli-linux-toolchains bash tools/fw/build.sh   # firmware (.fwsc)
node tools/fw/workbench.mjs                                 # updater page
FM1_WASI_SDK=/path/to/wasi-sdk node tools/build-web-linux.mjs  # browser simulator
```

`tools/fw/build.sh` runs a hardware safety gate. It refuses any change to start-up, LCD transfer, audio output or input-scanner code, and any big memory drop, unless you build it as a labelled TEST build for a spare unit. See [docs/vesper-hardware-safety.md](docs/vesper-hardware-safety.md).

Most of VESPER lives in a few places:
- `app/lush.c`: the synth engine and effects
- `app/screen.c`: the user interface
- `app/presets.c`: the scenes

The stock V14 README is kept in [docs/FM1-OPEN-V14-README.md](docs/FM1-OPEN-V14-README.md).

## Credits

- FM-1 hardware and the FM1 Open SDK: M-VAVE.
- Samples: Versilian Studios VSCO-2 Community Edition (CC0).
- FM-1 web emulator used for start-up testing: Joakim Eriksson.
