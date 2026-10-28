// Render an audio demo of WORSHIP scenes with chord follow, straight from the simulator.
// Usage: node tools/render-demo.mjs build/web/simulator.wasm out.wav
import fs from 'node:fs';

const { instance } = await WebAssembly.instantiate(fs.readFileSync(process.argv[2]), {});
const x = instance.exports;
x._initialize?.();
const out = [];
const render = (seconds, keep = true) => {
  for (let i = 0, n = Math.round(seconds * 44100 / 128); i < n; i++) {
    const f = new Float32Array(x.memory.buffer, x.sim_render(), 256);
    if (keep) out.push(Float32Array.from(f));
  }
};
// Keys: 0 = F3. Each chord is pressed briefly; it latches after release.
const KEYS = { E: [11, 15, 18], 'C#m': [8, 11, 15], A: [4, 8, 11], B: [6, 10, 13], 'F#m': [1, 4, 8], Am: [4, 7, 11], F: [0, 4, 7],
               Esus2: [11, 13, 18], G: [2, 6, 9], Em: [11, 14, 18], C: [7, 11, 14], D: [9, 13, 16],
               Bm: [6, 9, 13], Ab: [3, 7, 10], Fm: [0, 3, 7], Db: [8, 12, 15], Eb: [10, 14, 17],
               Dm: [9, 12, 16], Bb: [5, 9, 12],
               'G#m': [3, 6, 10], 'F#': [1, 5, 8] };
const chord = (name) => {
  for (const k of KEYS[name]) x.sim_control(1, k, 1);
  render(0.03); for (const k of KEYS[name]) x.sim_control(1, k, 0);
};
const scene = (slot, bpm, progression, barsEach) => {
  x.sim_preset(slot); render(0.2, false);
  x.sim_open(0);
  chord(progression[0]);
  x.sim_control(0, 12, 1); render(0.02); x.sim_control(0, 12, 0); // PLAY
  const bar = 240 / bpm;
  for (const c of progression) { chord(c); render(bar * barsEach); }
  x.sim_control(0, 12, 1); render(0.02); x.sim_control(0, 12, 0); // stop, let FX ring
  render(3);
};
x.sim_boot(1); render(0.5, false);
scene(0, 72, ['E', 'C#m', 'A', 'B'], 2);       // DOTTED 8TH  - muted guitar 8ths into the dual delay, halo 3s, bass, reverse glock swells
scene(5, 70, ['C', 'Am', 'F', 'G'], 2);        // MORNING     - kalimba 8ths, marimba rise, warm bass, "ooh-aah" vocal chops
scene(11, 60, ['A', 'F#m', 'D', 'E'], 2);      // QUIET PLACE - handpan, sparse piano, sub, reverse piano swells
scene(1, 76, ['G', 'Em', 'C', 'D'], 2);        // ROLLING 3S  - detune pluck 3s, handpan, warm bass, heartbeat
scene(6, 66, ['F', 'Dm', 'Bb', 'C'], 2);       // SANCTUARY   - halo 3s, glock rise, warm bass, reverse piano swells
scene(4, 88, ['B', 'G#m', 'E', 'F#'], 2);      // GALLOP      - detune gallop, "ee-oh" vocal, bass, four on the floor

let peak = 0; for (const b of out) for (const v of b) peak = Math.max(peak, Math.abs(v));
const gain = peak > 0 ? 0.89 / peak : 1;
const samples = out.length * 256, data = Buffer.alloc(44 + samples * 2);
data.write('RIFF', 0); data.writeUInt32LE(36 + samples * 2, 4); data.write('WAVEfmt ', 8);
data.writeUInt32LE(16, 16); data.writeUInt16LE(1, 20); data.writeUInt16LE(2, 22);
data.writeUInt32LE(44100, 24); data.writeUInt32LE(44100 * 4, 28); data.writeUInt16LE(4, 32);
data.writeUInt16LE(16, 34); data.write('data', 36); data.writeUInt32LE(samples * 2, 40);
let at = 44;
for (const b of out) for (const v of b) { data.writeInt16LE(Math.round(Math.max(-1, Math.min(1, v * gain)) * 32767), at); at += 2; }
fs.writeFileSync(process.argv[3], data);
console.log(`${process.argv[3]}: ${(samples / 2 / 44100).toFixed(1)} s, normalized from peak ${peak.toFixed(3)}`);
