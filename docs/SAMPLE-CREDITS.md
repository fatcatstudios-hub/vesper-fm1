# WORSHIP sample bank — credits

`WORSHIP-SAMPLES.bin` is an FM-1 sample bank (workbench backup format, 8 slots,
14.7 s, 32 kHz SEA5). All recordings are from the **Versilian Studios VSCO-2
Community Edition** (https://github.com/sgossner/VSCO-2-CE), released under
**CC0 1.0** (public domain). Credit is encouraged, not required.

| Slot | Name | Source |
|---|---|---|
| 1–4 | PIANO C3 / C4 / C5 / C6 | Upright Nr. 1, pp dynamic. Upright piano sampled by Simon Dalzell (Ivy Audio) |
| 5–7 | MARIMBA G3 / F4 / G5 | Marimba, outrigger mallets |
| 8 | GLOCK C6 | Glockenspiel, medium mallet |

Processing (`tools/samples/`): mono sum, measured-fundamental retune (all zones
within ±3 cents; the glock was recorded about 12 cents sharp), 32 kHz resample,
silence trim, fade-out, loudness match, SEA5 encode, bank pack with CRC.
VSCO file names for marimba and glockenspiel read an octave below the sounding
pitch; slot names here are the measured sounding notes.

LUSH reads the zone root from the end of each slot name, so you can replace
these with your own samples in the workbench — name them like `RHODES C4`.
