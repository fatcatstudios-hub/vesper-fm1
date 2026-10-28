// Headless check of the WORSHIP scenes and chord follow in the browser simulator.
// Usage: node tools/test-worship.mjs build/web/simulator.wasm
import fs from 'node:fs';

const wasm = fs.readFileSync(process.argv[2] || 'build/web/simulator.wasm');
const { instance } = await WebAssembly.instantiate(wasm, {});
const x = instance.exports;
x._initialize?.();
const mem = () => new Uint8Array(x.memory.buffer);
const cstr = (p) => { const b = mem(); let s = ''; while (b[p]) s += String.fromCharCode(b[p++]); return s; };
let failures = 0;
const check = (ok, msg) => { console.log(`${ok ? 'PASS' : 'FAIL'}  ${msg}`); if (!ok) failures++; };

const render = (blocks) => {
  let sum = 0, n = 0, peak = 0;
  for (let i = 0; i < blocks; i++) {
    const p = x.sim_render() / 4;
    const f = new Float32Array(x.memory.buffer, p * 4, 256);
    for (const v of f) { sum += v * v; n++; peak = Math.max(peak, Math.abs(v)); }
  }
  return { rms: Math.sqrt(sum / n), peak };
};
const exportProject = () => {
  if (x.sim_export() !== 0) throw new Error('export failed');
  return mem().slice(x.sim_file(), x.sim_file() + 4064); // struct groove_project
};
const cell = (proj, track, step) => {
  const at = (track * 64 + step) * 4;
  return { note: proj[at], vel: proj[at + 1], bits: proj[at + 2] | (proj[at + 3] << 8) };
};
const NOTE = ['C','C#','D','Eb','E','F','F#','G','Ab','A','Bb','B'];
const name = (n) => NOTE[n % 12] + (Math.floor(n / 12) - 1);

x.sim_boot(1);
render(40);

// 1. Every worship scene builds a valid project.
const names = [];
for (let i = 0; i < 16; i++) {
  names.push(cstr(x.sim_factory_name(i)));
  check(x.sim_factory_project(i) === 0, `scene ${i + 1} "${names[i]}" builds a valid project`);
}

// 2. Load scene 1, start playback, confirm it makes sound.
check(x.sim_preset(0) >= 0, 'load scene 1 from the preset bank');
render(20);
check(cstr(x.sim_project_name()).trim() === names[0], `loaded project is "${names[0]}"`);
x.sim_control(0, 12, 1); render(2); x.sim_control(0, 12, 0); // PLAY
const playing = render(44100 * 4 / 128);
check(playing.rms > 0.003, `scene plays audio (rms ${playing.rms.toFixed(4)}, peak ${playing.peak.toFixed(3)})`);
check(playing.peak < 1.0, 'no hard clipping');

let p = exportProject();
const before = [0, 1, 2].map((t) => cell(p, t, 0));
console.log('      default:', before.map((c) => name(c.note)).join(' / '), '(pad / arp / bass, step 1)');

// 3. Hold A minor (A3 C4 E4 = keys 4, 7, 11 with F3 = key 0) on HOME.
x.sim_open(0);
for (const k of [4, 7, 11]) { x.sim_control(1, k, 1); render(3); }
render(10);
p = exportProject();
const am = [0, 1, 2].map((t) => cell(p, t, 0));
console.log('      A minor:', am.map((c) => name(c.note)).join(' / '));
check(am[0].note % 12 === 9 && (am[0].bits & 31) === 14, 'pad re-pitched to A with the open minor voicing');
check(am[2].note % 12 === 9, 'bass follows to A');
const arpNotes = [...Array(16).keys()].map((s) => cell(p, 1, s)).filter((c) => c.vel).map((c) => c.note % 12);
check(arpNotes.includes(0) && !arpNotes.includes(1), 'arp uses the minor third (C), not C#');

// 4. Release: chord latches and the keys did not trigger notes on the pad track.
for (const k of [4, 7, 11]) x.sim_control(1, k, 0);
render(10);
p = exportProject();
check(cell(p, 0, 0).note % 12 === 9, 'chord latches after release');

// 5. Single key D changes the root but keeps minor; then sus2 (G A D).
x.sim_control(1, 9, 1); render(5); x.sim_control(1, 9, 0); render(5);
p = exportProject();
check(cell(p, 0, 0).note % 12 === 2 && (cell(p, 0, 0).bits & 31) === 14, 'single key D -> D minor (quality kept)');
for (const k of [2, 4, 9]) x.sim_control(1, k, 1);
render(5);
for (const k of [2, 4, 9]) x.sim_control(1, k, 0);
render(5);
p = exportProject();
check(cell(p, 0, 0).note % 12 === 7 && (cell(p, 0, 0).bits & 31) === 15, 'G A D -> G sus2');
const stillPlaying = render(44100 * 2 / 128);
check(stillPlaying.rms > 0.003, `still playing after chord changes (rms ${stillPlaying.rms.toFixed(4)})`);

// 6. Every scene loads and plays.
for (let i = 0; i < 16; i++) {
  x.sim_preset(i); render(10);
  const r = render(44100 * 2 / 128);
  check(r.rms > 0.002 && r.peak < 1, `scene ${String(i + 1).padStart(2)} ${names[i].padEnd(12)} rms ${r.rms.toFixed(4)} peak ${r.peak.toFixed(3)}`);
}
console.log(failures ? `\n${failures} check(s) failed` : '\nAll checks passed');
process.exit(failures ? 1 : 0);
