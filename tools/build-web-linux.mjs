// SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
// Linux/macOS port of tools/Build-Web.ps1 -LocalPreview.
// Usage: FM1_WASI_SDK=/path/to/wasi-sdk node tools/build-web-linux.mjs [output.html]
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const wasi = process.env.FM1_WASI_SDK;
if (!wasi || !fs.existsSync(path.join(wasi, 'bin/clang'))) {
  console.error('Set FM1_WASI_SDK to an extracted WASI SDK 27 directory.');
  process.exit(1);
}
const output = path.resolve(process.argv[2] || path.join(root, 'build/preview/index.html'));
const sha = (buf) => createHash('sha256').update(buf).digest('hex');

function walk(dir) {
  return fs.readdirSync(dir, { withFileTypes: true }).flatMap((e) => {
    const p = path.join(dir, e.name);
    return e.isDirectory() ? walk(p) : [p];
  });
}
const sources = ['app', 'web', 'third_party/synth']
  .flatMap((d) => walk(path.join(root, d)))
  .map((f) => path.relative(root, f).split(path.sep).join('/'))
  .filter((n) => !n.startsWith('app/board/') && n.endsWith('.c'))
  .sort();

const wasmPath = path.join(root, 'build/web/simulator.wasm');
fs.mkdirSync(path.dirname(wasmPath), { recursive: true });
const options = [
  `--sysroot=${path.join(wasi, 'share/wasi-sysroot')}`,
  '-std=c11', '-O2', '-fwrapv', '-fno-fast-math', '-ffp-contract=off',
  '-DFM1_HOST_TEST', '-DFM1_OPEN_WEB', '-Wall', '-Wextra', '-Werror',
  '-Wno-unused-function', '-Wno-array-parameter', '-mexec-model=reactor',
  ...['web/engine', 'web/synth', 'third_party/synth', 'sdk/include', 'app/synth', 'app']
    .map((d) => '-I' + path.join(root, d)),
  ...sources.map((n) => path.join(root, n)),
  '-Wl,--strip-all', '-Wl,-z,stack-size=131072', '-Wl,--initial-memory=4194304',
  '-Wl,--max-memory=33554432', '-o', wasmPath,
];
console.log(`Compiling ${sources.length} C files to WebAssembly`);
execFileSync(path.join(wasi, 'bin/clang'), options, { stdio: 'inherit' });

const wasm = fs.readFileSync(wasmPath);
const version = /^#define FM1_RELEASE_VERSION "(V\d+)"\s*$/m
  .exec(fs.readFileSync(path.join(root, 'app/version.h'), 'utf8'));
if (!version) throw new Error('Set FM1_RELEASE_VERSION in app/version.h.');
const simulation = JSON.stringify({
  schema: 1, release_version: version[1], sha256: sha(wasm), source_sha256: '',
  sample_bank: 'synthesized-demo', data: wasm.toString('base64'),
});
let web = fs.readFileSync(path.join(root, 'sdk/Update.html.in'), 'utf8');
web = web.replace(/<a\b[^>]*href="\/\*FM1_SOURCE_URL\*\/"[^>]*>(.*?)<\/a>/g, '<span>$1 (local preview)</span>');
const htmlEncode = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
const read = (f) => fs.readFileSync(path.join(root, f), 'utf8');
const notices = ['APP: GPLv3 with the FM1 SDK linking exception; no warranty. SDK and firmware resources have separate redistribution terms in THIRD_PARTY.md. Source: local preview'];
for (const f of ['THIRD_PARTY.md', 'LICENSE.md']) notices.push(`\n===== ${f} =====\n` + read(f));
const replacements = {
  '/*FM1_SOURCE_URL*/': '',
  '/*FM1_PAYLOAD*/': 'const FM1_PACKAGE = null;',
  '/*FM1_SIMULATOR*/': 'const FM1_SIMULATOR = ' + simulation + ';',
  '/*FM1_I18N*/': read('sdk/I18n.js'),
  '/*FM1_SCRIPT*/': read('sdk/WebUpdate.js'),
  '/*FM1_SIM_SCRIPT*/': read('sdk/Simulator.js'),
  '/*FM1_SAMPLE_SCRIPT*/': read('sdk/Samples.js'),
  '/*FM1_NOTICES*/': htmlEncode(notices.join('\n')),
};
for (const [k, v] of Object.entries(replacements)) web = web.split(k).join(v);
if (/\/\*FM1_[A-Z_]+\*\//.test(web)) throw new Error('Unexpanded HTML placeholder.');
fs.mkdirSync(path.dirname(output), { recursive: true });
fs.writeFileSync(output, web);
console.log(`Preview: ${output} (${(web.length / 1024).toFixed(0)} KiB, wasm ${(wasm.length / 1024).toFixed(0)} KiB)`);
