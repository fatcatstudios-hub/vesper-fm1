"""Prepare CC0 VSCO-2 CE recordings for the FM-1 sample bank (32 kHz mono int16).
Usage: python3 -I prepare.py <vsco-2-ce dir> <out dir>"""
import sys, os, wave, numpy as np
from scipy.signal import resample_poly
src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
# (file, slot name, MIDI root, seconds kept)
SET = [
    # VSCO file names for marimba and glock read an octave below the sounding
    # pitch; the slot names here are the measured, sounding notes.
    ("Keys/Upright Nr1/UR1_C3_pp_RR1.wav", "PIANO C3", 48, 2.6),
    ("Keys/Upright Nr1/UR1_C4_pp_RR1.wav", "PIANO C4", 60, 2.6),
    ("Keys/Upright Nr1/UR1_C5_pp_RR1.wav", "PIANO C5", 72, 2.2),
    ("Keys/Upright Nr1/UR1_C6_pp_RR1.wav", "PIANO C6", 84, 1.6),
    ("Percussion/Marimba/Marimba_hit_Outrigger_G2_loud_01.wav", "MARIMBA G3", 55, 1.4),
    ("Percussion/Marimba/Marimba_hit_Outrigger_F3_loud_01.wav", "MARIMBA F4", 65, 1.3),
    ("Percussion/Marimba/Marimba_hit_Outrigger_G4_loud_01.wav", "MARIMBA G5", 79, 1.0),
    ("Percussion/Glock/glock_medium_C5.wav", "GLOCK C6", 84, 2.0),
]
def read(path):
    with open(path, 'rb') as f: data = f.read()
    assert data[:4] == b'RIFF' and data[8:12] == b'WAVE'
    pos, fmt, pcm = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos+4], int.from_bytes(data[pos+4:pos+8], 'little')
        body = data[pos+8:pos+8+size]
        if cid == b'fmt ': fmt = body
        elif cid == b'data': pcm = body
        pos += 8 + size + (size & 1)
    tag, ch, rate = int.from_bytes(fmt[0:2],'little'), int.from_bytes(fmt[2:4],'little'), int.from_bytes(fmt[4:8],'little')
    bits = int.from_bytes(fmt[14:16],'little')
    if tag == 0xFFFE: tag = int.from_bytes(fmt[24:26],'little')
    if tag == 3: x = np.frombuffer(pcm, dtype='<f4' if bits == 32 else '<f8').astype(float)
    elif bits == 16: x = np.frombuffer(pcm, dtype='<i2').astype(float) / 32768
    elif bits == 24:
        b = np.frombuffer(pcm[:len(pcm)//3*3], dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:,0] | (b[:,1] << 8) | (b[:,2] << 16); v = np.where(v >= 1 << 23, v - (1 << 24), v)
        x = v.astype(float) / (1 << 23)
    elif bits == 32: x = np.frombuffer(pcm, dtype='<i4').astype(float) / 2**31
    else: raise ValueError(f'{path}: {bits}-bit')
    return x.reshape(-1, ch).mean(1), rate, bits, ch
def pitch(sig, fs, fmin=60, fmax=2500):
    sig = sig - sig.mean(); n = len(sig)
    c = np.fft.irfft(np.abs(np.fft.rfft(sig, 2*n))**2)[:n]
    lo, hi = int(fs/fmax), int(fs/fmin); k = lo + np.argmax(c[lo:hi])
    a, b, cc = c[k-1], c[k], c[k+1]; return fs / (k + 0.5*(a-cc)/(a-2*b+cc))
def fundamental_cents(x, fs, root):
    """Strongest spectral peak within +-80 cents of the root, in cents."""
    seg = x[int(0.04*fs):int(0.04*fs)+16384]; n = len(seg)
    X = np.abs(np.fft.rfft(seg*np.hanning(n), 8*n)); f = np.fft.rfftfreq(8*n, 1/fs)
    f0 = 440*2**((root-69)/12); band = (f > f0*2**(-80/1200)) & (f < f0*2**(80/1200))
    return 1200*np.log2(f[np.argmax(np.where(band, X, 0))]/f0)
report = []
for path, name, root, keep in SET:
    raw, rate, bits, ch = read(os.path.join(src, path))
    error = fundamental_cents(raw, rate, root)
    # Retune while resampling: treat the take as recorded at rate*2^(-c/1200).
    x = resample_poly(raw, 32000, int(round(rate * 2**(-error/1200))))
    env = np.abs(x); start = max(0, int(np.argmax(env > env.max() * 0.02)) - 32)  # trim silence, keep the attack
    x = x[start:start + int(keep * 32000)]
    fade = int(0.25 * 32000); x[-fade:] *= np.cos(np.linspace(0, np.pi / 2, fade))**2
    after = fundamental_cents(x, 32000, root)
    x = x / np.sqrt(np.mean(x[:int(0.3*32000)]**2)) * 0.12           # match loudness across zones
    x = x / max(1.0, np.abs(x).max() / 0.95)
    pcm = np.round(x * 32767).astype('<i2')
    fn = os.path.join(out, name.replace(' ', '_') + '.raw'); pcm.tofile(fn)
    report.append((name, root, len(pcm), f'{rate}Hz/{bits}bit/{ch}ch', round(error, 1), round(after, 1)))
for r in report: print(f"{r[0]:11} root {r[1]:3}  {r[2]/32000:4.2f}s  from {r[3]:16} measured {r[4]:+6.1f} c -> {r[5]:+5.1f} c")
print(f"total {sum(r[2] for r in report)/32000:.2f} s of 19.55 s")
