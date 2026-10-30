# VESPER 2 (in progress)

VESPER, the ambient worship firmware for the M-VAVE FM-1 by Fat Cat Studios, rebuilt on
[fm1-base](https://github.com/charlesvestal/fm1-base) and felucca-bsp instead of M-VAVE's FM1 Open SDK.

**Why:** VESPER 1.x sat on M-VAVE's closed engine library, which filled the flash. On fm1-base the
app slot is 581 KB, so VESPER's piano, marimba and glockenspiel samples are built into the firmware.
There is nothing extra to install, and the FM-1 Firmware Hub can install it directly.

**How:** VESPER's own code is the 1.1 code, unchanged (`vesper/app`), with the open FM1 engine that
the browser simulator already used (`vesper/web`, `vesper/third_party`). `vesper/port.c` gives that
code the SDK calls it was written against, on fm1-base's `plat.h`, and `app.c` is the fm1-base
contract. Same scenes, same screens, same sound.

## Building

```
git clone --recursive -b v2 https://github.com/fatcatstudios-hub/vesper-fm1 vesper2
fm1-base/bsp/tools/get_sdk.sh ~/ac79-sdk       # JieLi's AC79 SDK (three files go in the package)
JIELI_TOOLCHAIN=/path/to/jieli-linux-toolchains AC79_SDK=~/ac79-sdk tools/build.sh            # dev build
JIELI_TOOLCHAIN=... AC79_SDK=... tools/build.sh --release 2.0.0                                # release
tools/build.sh --host                          # the simulator: build/host/vesper_host SCRIPT OUTDIR
```

`tools/build.sh` refuses to produce a package unless the SDK files are JieLi's reference ones.

## Safety notes

- Every 64-bit multiply is compiled out of line (`tools/mul64_outline.py`, felucca-bsp docs/CPU.md).
- Projects and settings live in fm1-base's data area (0x97000..0xAD000). The sample bank is read-only
  and part of the firmware.
- Install path for testing: VESPER 1.1, then Restore V15, then install 2.0 from V15, then test, then
  back to V15 with the 2.0 installer.

Licence: GPL-3.0 with the FM1 SDK linking exception (VESPER, the FM1 Open engine). fm1-base and
felucca-bsp are MPL-2.0. The samples are CC0 (VSCO-2 CE, see SAMPLE-CREDITS.md).
