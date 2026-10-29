# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
"""Key image checks from sdk/Build.ps1 Test-Firmware: entry point, audio code in SRAM,
required callbacks present, LUSH audio code in SRAM. Usage: check.py <nm> sdk.elf sdk.map"""
import re, struct, subprocess, sys

nm, elf, mapfile = sys.argv[1:4]
diag = '--diag' in sys.argv[4:]
info = {}
for line in subprocess.run([nm, '--print-size', elf], capture_output=True, text=True, check=True).stdout.splitlines():
    m = re.match(r'^([0-9a-fA-F]+)[ \t]+(([0-9a-fA-F]+)[ \t]+)?([A-Za-z])[ \t]+(\S+)', line)
    if m:
        info[m.group(5)] = (int(m.group(1), 16), int(m.group(3), 16) if m.group(3) else None, m.group(4))
errors = []
data = open(elf, 'rb').read()
entry = struct.unpack_from('<I', data, 24)[0]
if entry != 0x02000120 or info.get('_start', (0,))[0] != entry:
    errors.append(f'entry point {entry:#x} is not the fixed ROM entry')
SRAM = (0x01C00000, 0x01C80000)
audio = ['fm1_audio_irq', 'fm1_engine_render', 'fm1_output_mix', 'fm1_audio_copy',
         'lush_render', 'lush_note', 'lush_owns', 'groove_midi_note', 'groove_midi_mode', 'groove_midi_transport']
for name in audio:
    if name not in info:
        # LTO may inline small audio helpers into their (SRAM) callers.
        if name in ('lush_owns', 'lush_note'):
            continue
        errors.append(f'audio symbol missing: {name}')
    elif not SRAM[0] <= info[name][0] < SRAM[1]:
        errors.append(f'audio code outside internal SRAM: {name} at {info[name][0]:#x}')
for name in ['fm1_build_id', 'fm1_update_identity', 'fm1_app_sysex']:
    if name not in info:
        errors.append(f'required symbol missing: {name}')
if info.get('fm1_build_id', (0, 0))[1] != 32:
    errors.append('fm1_build_id must be 32 bytes')
# Worship scenes and LUSH patch names must be in the image (LTO may inline their functions).
for marker in ((b'T GLASS', b'T BASSES') if diag else (b'DOTTED 8TH', b'QUIET PLACE', b'SANCTUARY', b'NEON SKY')):
    if marker not in data:
        errors.append(f'worship content missing from image: {marker.decode()}')
heap = re.search(r'0x([0-9a-fA-F]+)[^\r\n]*_MALLOC_SIZE[ ]*=', open(mapfile).read())
heap_bytes = int(heap.group(1), 16) if heap else 0
if heap_bytes < 49152:
    errors.append(f'runtime heap {heap_bytes} bytes; need 49152')
lush_sram = sum((v[1] or 0) for k, v in info.items() if (k.startswith('lush') or k.startswith('patches') or k.startswith('modal'))
                and SRAM[0] <= v[0] < SRAM[1])
print(f'check: entry {entry:#x}, heap {heap_bytes} bytes, LUSH symbols in SRAM ~{lush_sram} bytes')
if errors:
    print('\n'.join('FAIL ' + e for e in errors))
    sys.exit(1)
print('check: passed')
