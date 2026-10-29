# VESPER hardware safety rules (after the 0.7.1 brick, 2026-10-29)

0.7.1 changed how the LCD is fed at power-up (a second band buffer and a wait
loop). It froze a unit before USB came up, so no updater could reach it.

1. `tools/fw/vesper_guard.py` runs in every firmware build. It refuses a build if
   start-up, LCD-transfer, output-stage or panel code differs from the last build
   proven on a real FM-1 (`tools/fw/vesper_verified.json`), or if free RAM drops
   more than 2 KB below it.
2. Such changes can only be built as `VESPER_TEST_BUILD=1`. The updater file is then
   named `...-TEST-SPARE-UNIT-ONLY-Updater.html` and the boot screen shows TEST.
   TEST builds go to one spare unit first, never to a unit Zach depends on.
3. The verified baseline only moves (`vesper_guard.py --verified <heap>`) after Zach
   confirms a build boots, connects to the updater and runs on hardware.
4. No control-task loop may wait on hardware or on the clock; loops are bounded by count.
5. Pending TEST work: `tools/fw/pending/safe-boot-TEST.patch` (hold HOME + PLAY at
   power-on to skip VESPER's screen and sound code so a bad build can be replaced).
