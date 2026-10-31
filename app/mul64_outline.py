#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
"""VESPER 2's IR pass (fm1.toml ir_pass): every 64-bit multiply goes out of line.

felucca-bsp docs/CPU.md: on the FM-1, a 32x32->64-bit multiply inlined beside memory loads (16-bit ones above
all) now and then leaves a register a bit off, and the firmware crashes; the same multiply in a function of its
own (noinline) is clear of it. VESPER's DSP (LUSH, the engine's mixer) is full of them, so this pass, run on each
unit's IR before it is optimised, turns each `mul i64` into a call:
  sext i32 * sext i32 (or a constant that fits)  ->  vesper_smul32(i32, i32)
  zext i32 * zext i32                            ->  vesper_umul32(i32, i32)
  anything else                                  ->  vesper_mul64(i64, i64)
The helpers are in vesper/libc.c (noinline), the one unit this pass leaves alone.

  mul64_outline.py IN.ll OUT.ll [UNIT]
"""
import re
import sys

SKIP_UNITS = {"vesper/libc.c"}
MUL = re.compile(r"^(\s*)(%[\w.\-]+) = mul (?:nuw )?(?:nsw )?(?:nuw )?i64 ([^,]+), (.+?)(\s*(?:,\s*!.*)?)$")
DEF = re.compile(r"^\s*(%[\w.\-]+) = (sext|zext) i32 (%[\w.\-]+|-?\d+) to i64")


def operand(v, defs):
    """(kind, i32 value) for an i64 operand: 's'/'u' extended from i32, 'c' a constant, None otherwise"""
    v = v.strip()
    if re.fullmatch(r"-?\d+", v):
        n = int(v)
        return ("c", n) if -(1 << 31) <= n < (1 << 31) else None
    d = defs.get(v)
    return (("s" if d[0] == "sext" else "u"), d[1]) if d else None


def run(src, unit):
    if unit in SKIP_UNITS:
        return src, 0
    out, defs, used, count = [], {}, set(), 0
    for line in src.splitlines():
        if line.startswith("define "):
            defs = {}
        m = DEF.match(line)
        if m:
            defs[m.group(1)] = (m.group(2), m.group(3))
        m = MUL.match(line)
        if m:
            ind, dst, a, b, tail = m.groups()
            oa, ob = operand(a, defs), operand(b, defs)
            kinds = {oa[0] if oa else None, ob[0] if ob else None}
            if oa and ob and kinds <= {"s", "c"} and kinds != {"c"}:
                fn, args = "vesper_smul32", f"i32 {oa[1]}, i32 {ob[1]}"
            elif oa and ob and kinds == {"u"}:
                fn, args = "vesper_umul32", f"i32 {oa[1]}, i32 {ob[1]}"
            elif oa and ob and kinds == {"u", "c"} and all(o[0] == "u" or o[1] >= 0 for o in (oa, ob)):
                fn, args = "vesper_umul32", f"i32 {oa[1]}, i32 {ob[1]}"
            else:
                fn, args = "vesper_mul64", f"i64 {a.strip()}, i64 {b.strip()}"
            used.add(fn)
            count += 1
            line = f"{ind}{dst} = call i64 @{fn}({args}){tail}"
        out.append(line)
    decl = {"vesper_smul32": "declare i64 @vesper_smul32(i32, i32)",
            "vesper_umul32": "declare i64 @vesper_umul32(i32, i32)",
            "vesper_mul64": "declare i64 @vesper_mul64(i64, i64)"}
    text = "\n".join(out) + "\n"
    for fn in sorted(used):
        if not re.search(r"^declare .*@" + fn + r"\(", text, re.M) and not re.search(r"^define .*@" + fn + r"\(", text, re.M):
            text += decl[fn] + "\n"
    return text, count


def main():
    src = open(sys.argv[1]).read()
    unit = sys.argv[3] if len(sys.argv) > 3 else ""
    text, n = run(src, unit)
    open(sys.argv[2], "w").write(text)
    if n:
        print(f"mul64_outline: {unit or 'unity'}: {n} out of line")


if __name__ == "__main__":
    main()
