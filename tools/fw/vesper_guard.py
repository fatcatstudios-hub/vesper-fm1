#!/usr/bin/env python3
"""VESPER hardware-safety gate, run by build.sh after linking.

A build that changes start-up, LCD transfer or output-stage code, or that uses
more RAM than a build already proven on a real FM-1, can stop a unit from
booting. One did (0.7.1). Such builds are refused unless VESPER_TEST_BUILD=1,
which marks the version as a TEST build for a spare unit only.

  vesper_guard.py <heap_bytes>        check (build.sh calls this)
  vesper_guard.py --verified <heap>   record the current code as hardware-verified,
                                      only after Zach confirms the build on a real unit
"""
import hashlib, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STATE = os.path.join(ROOT, 'tools', 'fw', 'vesper_verified.json')
# Code that runs at power-up or talks to the LCD / output hardware.
CRITICAL = [
    ('app/screen.c', r'^static void paint_flush\(void\)'),
    ('app/screen.c', r'^void groove_screen_init\(void\)'),
    ('app/screen.c', r'^static __attribute__\(\(noinline\)\) void paint_fill\('),
    ('app/screen.c', r'^static void paint_mark\('),
    ('app/audio_output.c', r'^FM1_AUDIO_CODE void fm1_output_mix\('),
    ('app/board/panel.c', None),
    ('app/board/fm1_input.h', None),   # key/encoder scanner, runs in the scan interrupt
    ('app/board/device.c', None),      # timers and the 10 kHz scan tick
    ('app/project.c', r'^void groove_project_init\(void\)'),
    ('app/project.c', r'^void groove_project_poll\(void\)'),
]
DECLS = [('app/screen.c', r'^static uint16_t \*?paint_pixels\b.*$'), ('app/screen.c', r'^static uint16_t paint_buffers\[.*$')]


def body(path, start):
    text = open(os.path.join(ROOT, path), encoding='utf-8').read()
    if start is None:
        return text
    m = re.search(start, text, re.M)
    if not m:
        return '<missing>'
    end = text.find('\n}\n', m.start())
    return text[m.start():end + 3]


def fingerprint():
    parts = {f'{p}:{s}': hashlib.sha256(body(p, s).encode()).hexdigest()[:16] for p, s in CRITICAL}
    for p, s in DECLS:
        text = open(os.path.join(ROOT, p), encoding='utf-8').read()
        m = re.search(s, text, re.M)
        parts[f'{p}:{s}'] = hashlib.sha256((m.group(0) if m else '<missing>').encode()).hexdigest()[:16]
    return parts


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--verified':
        json.dump({'heap': int(sys.argv[2]), 'code': fingerprint()}, open(STATE, 'w'), indent=1)
        print('vesper guard: recorded current critical code as hardware-verified')
        return 0
    heap = int(sys.argv[1])
    state = json.load(open(STATE))
    changed = [k for k, v in fingerprint().items() if state['code'].get(k) != v]
    problems = [f'changed start-up / LCD / output code: {k}' for k in changed]
    if heap < state['heap'] - 2048:
        problems.append(f'heap {heap} bytes is more than 2 KB below the verified {state["heap"]}')
    if not problems:
        print('vesper guard: critical code matches the hardware-verified build')
        return 0
    if os.environ.get('VESPER_TEST_BUILD') == '1':
        for p in problems:
            print('vesper guard (TEST build, spare unit only):', p)
        return 0
    for p in problems:
        print('vesper guard: REFUSED -', p, file=sys.stderr)
    print('Build it as VESPER_TEST_BUILD=1 for a spare unit, or keep the verified code.', file=sys.stderr)
    return 1


sys.exit(main())
