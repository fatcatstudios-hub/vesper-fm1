import numpy as np, subprocess, wave, sys
names=["","AIR PAD","STRINGS","GLASS PLUCK","HARP","FELT KEYS","WARM BASS","SUB","AIR BELL"]
def render(patch,note,on,total,rev=0,dly=0,path='t.wav'):
    subprocess.run(['./lab',str(patch),str(note),str(on),str(total),str(rev),str(dly),path],check=True)
    w=wave.open(path); x=np.frombuffer(w.readframes(w.getnframes()),dtype=np.int16).reshape(-1,2).astype(float)/32768
    return x
def pitch(sig,fs=44100,fmin=40,fmax=2000):
    sig=sig-sig.mean(); n=len(sig)
    c=np.fft.irfft(np.abs(np.fft.rfft(sig,2*n))**2)[:n]
    lo,hi=int(fs/fmax),int(fs/fmin); k=lo+np.argmax(c[lo:hi])
    a,b,cc=c[k-1],c[k],c[k+1]; d=0.5*(a-cc)/(a-2*b+cc)
    return fs/(k+d)
def cents(f,note): return 1200*np.log2(f/(440*2**((note-69)/12)))
print(f"{'patch':12} {'note':>4} {'peak':>6} {'rms':>6} {'centroid':>8} {'tune¢':>6} {'tail':>8} {'dc':>7}")
for p in range(1,9):
    for note in (48,72) if p not in (6,7) else (36,48):
        x=render(p,note,2.5,6.0)
        m=x.mean(1); fs=44100
        seg=m[int(0.8*fs):int(1.6*fs)] if p in (1,2,6,7) else m[int(0.05*fs):int(0.45*fs)]
        f=pitch(seg)
        spec=np.abs(np.fft.rfft(seg*np.hanning(len(seg)))); fr=np.fft.rfftfreq(len(seg),1/fs)
        cen=(spec*fr).sum()/spec.sum()
        tail=np.abs(m[-4410:]).max()
        print(f"{names[p]:12} {note:4} {np.abs(x).max():6.3f} {np.sqrt((seg**2).mean()):6.3f} {cen:8.0f} {cents(f,note):6.1f} {tail:8.5f} {m.mean():7.4f}")
