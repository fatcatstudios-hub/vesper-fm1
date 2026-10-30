#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
"""Take JieLi's three SDK files (uboot.boot, cfg_tool.bin, cfg/eq_cfg_hw.bin) out of a Felucca-family .fwsc
(Felucca, or any firmware built with felucca-bsp / fm1-base), the reverse of fm1-base's fm1pkg_make.py, and
keep them only if each is byte for byte JieLi's AC79NN_SDK_V1.2.1 file (the SHA-256 fm1-base pins).

  sdk_from_fwsc.py PACKAGE.fwsc OUT_DIR       then AC79_SDK=OUT_DIR tools/build.sh
"""
import hashlib
import struct
import sys
from pathlib import Path

PINNED = {   # fm1-base tools/build.py SDK_SHA256 (JieLi AC79NN_SDK_V1.2.1_2023-12-13)
    "uboot.boot": "4e3b4c220dc96641cb5a723f41e68ce41d5261ae9434bb33fbd7f2c59976ded4",
    "cfg_tool.bin": "276579954f076886a6a7694f65dc71c034a63a2c204b76749065c0ac7b010d1b",
    "cfg/eq_cfg_hw.bin": "41167491bffed4651750719c973d2758adeb9021a5670d02d6a53c85ed80ea7d",
}


def enc(buf, off, size, key=0xFFFF):
    for i in range(size):
        buf[off + i] ^= key & 0xFF
        key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 0xFFFF


def sfc(buf, off, size, base, key):
    for i in range(0, size, 32):
        enc(buf, off + i, min(size - i, 32), key ^ ((off + i - base) >> 2))


def chipkey_decode(d):
    s = sum(d[:16]) & 0xFF
    s = 0xAA if s >= 0xE0 else 0x55 if s <= 0x10 else s
    return sum(1 << i for i in range(16) if (d[16 + i] ^ d[15 - i]) < s)


def entry(b, o):
    """a 32-byte JLFS entry: (offset, size, name)"""
    _, _, off, size, _, _, _, name = struct.unpack_from("<HHIIBBH16s", b, o)
    return off, size, name.split(b"\0")[0].decode("latin-1")


def logical(raw):
    """the .fwsc minus the identity's marker bytes (one after each of the first 20 0x2F-byte blocks)"""
    out = bytearray()
    for i in range(20):
        out += raw[i * 0x30:i * 0x30 + 0x2F]
    return out + raw[20 * 0x30:]


def flash_bin(raw):
    lg = logical(raw)
    hdr = bytearray(lg[0:0x40]); enc(hdr, 0, 0x40)
    total, nfiles = struct.unpack_from("<IH", hdr, 4)
    for i in range(nfiles):
        e = bytearray(lg[0x40 + i * 0x50:0x40 + (i + 1) * 0x50]); enc(e, 0, 0x50)
        _, _, _, _, off, size, _ = struct.unpack_from("<HHHHIII", e, 0)
        if e[0x40:0x40 + 9] == b"flash.bin":
            return bytes(lg[off:off + size])
    raise SystemExit("no flash.bin in the package (not a Felucca-family .fwsc?)")


def extract(raw):
    f = flash_bin(raw)
    head = bytearray(f[0:0x4000])
    for o in range(0, 0x20 + 4 * 32, 32):
        enc(head, o, 32)
    files, key = {}, None
    for o in range(0x20, 0x20 + 4 * 32, 32):
        off, size, name = entry(head, o)
        if name == "uboot.boot":
            files["uboot.boot"] = bytes(f[off:off + size])
        elif name == "isd_config.ini":
            key = chipkey_decode(f[off:off + 32])
    if key is None or "uboot.boot" not in files:
        raise SystemExit("package head not as fm1pkg writes it")
    area = bytearray(f[0x4000:])
    sfc(area, 0, 32 * 64, 0, key)                       # app_area_head and its entries
    _, blk, _ = entry(area, 0)
    region = bytearray(f[0x4000:0x4000 + blk + 0x400])
    sfc(region, 0, len(region), 0, key)
    for o in range(32, 32 * 8, 32):
        off, size, name = entry(region, o)
        if name == "cfg_tool.bin":
            files["cfg_tool.bin"] = bytes(region[off:off + size])
    _, _, cname = entry(region, blk)
    off, size, name = entry(region, blk + 0x20)
    if cname == "cfg" and name == "eq_cfg_hw.bin":
        files["cfg/eq_cfg_hw.bin"] = bytes(region[blk + off:blk + off + size])
    return files


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    files = extract(Path(sys.argv[1]).read_bytes())
    out, ok = Path(sys.argv[2]) / "cpu" / "wl82" / "tools", True
    for rel, sha in PINNED.items():
        d = files.get(rel)
        got = hashlib.sha256(d).hexdigest() if d is not None else "missing"
        match = got == sha
        ok &= match
        print(f"{'ok  ' if match else 'BAD '} {rel}: {len(d) if d else 0} B {got[:16]}")
        if match:
            (out / rel).parent.mkdir(parents=True, exist_ok=True)
            (out / rel).write_bytes(d)
    if not ok:
        raise SystemExit("sdk_from_fwsc: not JieLi's reference files; nothing usable taken")
    print(f"AC79_SDK={sys.argv[2]}")


if __name__ == "__main__":
    main()
