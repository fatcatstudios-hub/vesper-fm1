"""Pack SEA5 slots into an FM-1 sample bank file (workbench backup format).
Usage: python3 -I bank.py <dir with .sea/.raw> <out.bin>"""
import sys, os, struct, zlib
src, out = sys.argv[1], sys.argv[2]
ORDER = ["PIANO C3", "PIANO C4", "PIANO C5", "PIANO C6", "MARIMBA G3", "MARIMBA F4", "MARIMBA G5", "GLOCK C6"]
HEADER, CAPACITY = 512, 446464
slots = []
for name in ORDER:
    base = os.path.join(src, name.replace(' ', '_'))
    frames = os.path.getsize(base + '.raw') // 2
    data = open(base + '.sea', 'rb').read()
    assert len(data) == (frames + 319) // 320 * 228
    assert all(data[p:p+4] == b'\x01\x45\x14\x5a' for p in range(0, len(data), 228))
    slots.append((name, frames, data))
size = HEADER + sum(len(d) for _, _, d in slots)
assert size <= CAPACITY, size
bank = bytearray(size)
struct.pack_into('<IIIIIIII', bank, 0, 0x53314d46, 1, size, 0, 0, 32000, 0, 0)
offset = HEADER
for i in range(16):
    if i < len(slots):
        name, frames, data = slots[i]
        struct.pack_into('<III', bank, 32 + i*28, offset, frames, len(data))
        bank[32 + i*28 + 12: 32 + i*28 + 12 + len(name)] = name.encode('ascii')
        bank[offset:offset+len(data)] = data; offset += len(data)
    else:
        struct.pack_into('<III', bank, 32 + i*28, offset, 0, 0)
crc = zlib.crc32(bytes(bank[0:12])); crc = zlib.crc32(b'\0' * 8, crc); crc = zlib.crc32(bytes(bank[20:]), crc)
struct.pack_into('<I', bank, 12, crc)
open(out, 'wb').write(bank)
print(f"{out}: {size} bytes, {sum(f for _, f, _ in slots)/32000:.2f} s in {len(slots)} slots")
