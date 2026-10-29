#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# Linux port of sdk/Build.ps1 -FirmwareOnly, using JieLi's Linux toolchain
# (https://pkgman.jieliapp.com/s/linux-toolchain, pi32v2 clang 4.0.1, r3 target).
# Usage: JL=/opt/jieli tools/fw/build.sh            -> build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc
# The manifest pins the Windows 2.5.3 binaries; this Linux build is for emulator and
# development testing, and is not hardware-validated.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
JL="${JL:?set JL to the extracted JieLi Linux toolchain directory}"
BIN="$JL/pi32v2/bin"
BUILD="$ROOT/build/dx7"
rm -rf "$BUILD/obj" "$BUILD/generated"
mkdir -p "$BUILD/obj" "$BUILD/generated"
cd "$BUILD"

PRODUCT=$(sed -n 's/^#define FM1_UPDATE_PRODUCT "\(FM-1_8[0-9]\{6\}\)".*/\1/p' "$ROOT/app/version.h")
[ -n "$PRODUCT" ] || { echo "Set FM1_UPDATE_PRODUCT in app/version.h" >&2; exit 1; }

FLAGS=(-target pi32v2 -integrated-as -mcpu=r3 -mfprev1 -Wuninitialized -Wno-invalid-noreturn
  -fno-common -flto -fallow-pointer-null -fprefer-gnu-section -femulated-tls -Wno-shift-negative-value
  -Wframe-larger-than=2560 -fms-extensions -fno-unwind-tables -ffunction-sections -fdata-sections
  -fmessage-length=0 -cl-single-precision-constant -mllvm -pi32v2-large-program=true -D__GCC_PI32V2__ -O2 ${EXTRA_CFLAGS:-})
for d in sdk/include sdk/include/libc app/synth sdk/config app app/board; do FLAGS+=("-I$ROOT/$d"); done

OBJECTS=()
index=0
while IFS= read -r src; do
  opts=("${FLAGS[@]}")
  case "$src" in app/screen.c|app/transfer.c|app/midi_router.c) opts+=(-fno-lto) ;; esac
  [ "$src" = app/board/panel.c ] && opts+=(-fno-lto -fno-pic)
  opts+=(-fwrapv -fno-fast-math -ffp-contract=off)
  case "$src" in app/project.c|app/transfer.c|app/samples.c|app/screen.c|app/presets.c|app/midi_router.c|app/fm_factory.c|app/fm_library.c) opts+=(-Oz) ;; esac
  [ "$src" = app/screen.c ] && opts+=(-fno-inline-functions)
  out="obj/${index}_$(echo "$src" | sed 's/[^a-zA-Z0-9._-]/_/g').obj"
  index=$((index + 1))
  echo "Compile $src"
  "$BIN/clang" "${opts[@]}" -c "$ROOT/$src" -o "$out"
  OBJECTS+=("$out")
done < <(cd "$ROOT" && find app -type f \( -name '*.c' -o -name '*.S' \) ! -path 'app/synth/*' | LC_ALL=C sort)

# Build identity (same inputs as Build.ps1; the hash only needs to be stable per source).
ID=$( { echo dx7; sha256sum "$ROOT/sdk/manifest.json" | cut -d' ' -f1;
        cd "$ROOT" && find app -type f | LC_ALL=C sort | while read -r f; do echo "$f:$(sha256sum "$f" | cut -d' ' -f1)"; done; } | sha256sum | cut -d' ' -f1)
BYTES=$(echo "$ID" | sed 's/../0x&,/g; s/,$//')
cat > generated/build_identity.c <<EOF
#include "layout.h"
const struct fm1_firmware_layout fm1_layout = {536576U,7U};
const unsigned char fm1_build_id[32] = {$BYTES};
const unsigned char fm1_update_identity[27] = "$PRODUCT";
EOF
"$BIN/clang" "${FLAGS[@]}" -c generated/build_identity.c -o obj/build_identity.obj
OBJECTS+=(obj/build_identity.obj)

python3 - "$ROOT/sdk/config/linker.ld.in" generated/linker <<'EOF'
import sys
s = open(sys.argv[1]).read().replace('@SBC_DECODER@', 'sbc_decoder\n').replace('@ACL_TX_EXTRA@', ' + 1024').replace('@ACL_RX_EXTRA@', ' + 4096')
open(sys.argv[2], 'w').write(s)
EOF

echo "Link with the prebuilt FM1 SDK"
"$BIN/ld" --plugin "$BIN/LLVMgold.so" -o sdk.elf "${OBJECTS[@]}" -flto \
  --plugin-opt=-pi32v2-always-use-itblock=false --plugin-opt=-enable-ipra=true \
  --plugin-opt=-pi32v2-merge-max-offset=4096 --plugin-opt=-pi32v2-enable-simd=true --plugin-opt=mcpu=r3 \
  --plugin-opt=-global-merge-on-const --plugin-opt=-inline-threshold=5 --plugin-opt=-inline-max-allocated-size=32 \
  --plugin-opt=-inline-normal-into-special-section=true --plugin-opt=-dont-used-symbol-list=malloc,free,sprintf,printf,puts,putchar \
  --plugin-opt=-pi32v2-enable-rep-memop --plugin-opt=-emulated-tls --sort-common \
  --plugin-opt=-pi32v2-large-program=true --plugin-opt=-pi32v2-csync-before-rti=true --start-group \
  --whole-archive ../../sdk/lib/libfm1_engine.a --no-whole-archive ../../sdk/lib/libc.a ../../sdk/lib/libcompiler_rt.a \
  --end-group --gc-sections -Tgenerated/linker -M=sdk.map --undefined=connectivity_wl82_bluetooth_start \
  --undefined=audio_app_bluetooth_open --undefined=bt_get_local_name --undefined=bt_get_mac_addr

python3 "$ROOT/tools/fw/check.py" "$BIN/nm" sdk.elf sdk.map ${CHECK_ARGS:-}
for s in text data ram0_data cache_ram_data; do "$BIN/objcopy" -O binary -j ".$s" sdk.elf "generated/$s.bin"; done
cat generated/text.bin generated/data.bin generated/ram0_data.bin generated/cache_ram_data.bin > generated/app.bin
python3 "$ROOT/tools/fw/package.py" generated/app.bin "$ROOT/sdk/tools/pack" generated/FM1_OFFICIAL_OPEN.fwsc "$PRODUCT"
rm -f generated/text.bin generated/data.bin generated/ram0_data.bin generated/cache_ram_data.bin
echo "Firmware: $BUILD/generated/FM1_OFFICIAL_OPEN.fwsc"
