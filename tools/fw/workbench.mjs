// SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
// Linux port of the packaged part of sdk/Build.ps1 + tools/Build-Web.ps1: the FM1 workbench
// page with this build's firmware (and the SDK's original V15 recovery image) built in.
// Its "Device update" tab installs the firmware over USB MIDI from desktop Chrome/Edge.
// Usage: node tools/fw/workbench.mjs [out.html]   (after tools/fw/build.sh and a simulator build)
// Without FM1_SOURCE_URL the page is marked a local build: for your own devices, not distribution.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '../..');
const read = (f) => fs.readFileSync(path.join(root, f));
const text = (f) => read(f).toString('utf8');
const sha = (b) => crypto.createHash('sha256').update(b).digest('hex');
const version = /^#define FM1_RELEASE_VERSION "(V\d+)"\s*$/m.exec(text('app/version.h'))[1];
const worship = /^#define WORSHIP_VERSION "([0-9.]+)"/m.exec(text('app/version.h'))[1];
const fwName = /^#define FIRMWARE_NAME "([A-Z0-9 ]+)"/m.exec(text('app/version.h'))[1];
const testBuild = process.env.VESPER_TEST_BUILD === '1';
const buildLabel = testBuild ? `${worship}-TEST-SPARE-UNIT-ONLY` : worship;
const product = /^#define FM1_UPDATE_PRODUCT "(FM-1_8\d{6})"/m.exec(text('app/version.h'))[1];
const identity = JSON.parse(text('build/dx7/build-identity.json'));
const firmware = read('build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc');
const stockBytes = read('sdk/firmware/FM-1_015.fwsc');
const stockHash = 'db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a';
if (sha(stockBytes) !== stockHash) throw new Error('SDK original V15 recovery firmware is missing or invalid.');
const stock = { schema: 1, kind: 'stock', product: 'FM-1_015', profile: 'M-VAVE Original V15', family: 7, release_version: 'V15',
  build_id: stockHash, sha256: stockHash, bytes: stockBytes.length,
  loader_sha256: 'ef1f2db89b011fa53e71df363f0122de0ea896618c53fb53e8da676322004541', data: stockBytes.toString('base64') };
const pkg = { schema: 1, product, profile: 'dx7', family: identity.family, release_version: version,
  build_id: identity.build_id, sha256: sha(firmware), bytes: firmware.length,
  loader_sha256: sha(read('sdk/tools/pack/ota.bin')), hardware_validated: false,
  data: firmware.toString('base64'), stock };
if (!/^[0-9a-f]{64}$/.test(pkg.build_id)) throw new Error('Firmware build identity is invalid.');
const wasm = read('build/web/simulator.wasm');
const simulation = JSON.stringify({ schema: 1, release_version: version, sha256: sha(wasm), source_sha256: '',
  sample_bank: 'synthesized-demo', data: wasm.toString('base64') });
const sourceUrl = process.env.FM1_SOURCE_URL || '';
let web = text('sdk/Update.html.in');
const label = "pkg.kind==='stock'?'原厂 V15（FM-1_015）':'官方 Open 固件'";
if (!web.includes(label)) throw new Error('Updater page label not found.');
web = web.replace(label, `pkg.kind==='stock'?'原厂 V15（FM-1_015）':'${fwName} ${buildLabel}'`);
if (!sourceUrl) web = web.replace(/<a\b[^>]*href="\/\*FM1_SOURCE_URL\*\/"[^>]*>(.*?)<\/a>/g, '<span>$1 (local build)</span>');
const html = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
const notices = [`APP: GPLv3 with the FM1 SDK linking exception; no warranty. SDK and firmware resources have separate redistribution terms in THIRD_PARTY.md. Source: ${sourceUrl || 'local build'}`,
  'FM-1 V15 recovery firmware: https://www.m-vave.com/download (PC Firmware / FM-1 V15). Distributed by M-VAVE under Apache-2.0. Retain applicable licenses and notices; components with separate licenses retain their terms. See THIRD_PARTY.md.'];
for (const f of ['THIRD_PARTY.md', 'LICENSE.md']) notices.push(`\n===== ${f} =====\n` + text(f));
const replacements = {
  '/*FM1_SOURCE_URL*/': html(sourceUrl),
  '/*FM1_PAYLOAD*/': 'const FM1_PACKAGE = ' + JSON.stringify(pkg) + ';',
  '/*FM1_SIMULATOR*/': 'const FM1_SIMULATOR = ' + simulation + ';',
  '/*FM1_I18N*/': text('sdk/I18n.js'), '/*FM1_SCRIPT*/': text('sdk/WebUpdate.js'),
  '/*FM1_SIM_SCRIPT*/': text('sdk/Simulator.js'), '/*FM1_SAMPLE_SCRIPT*/': text('sdk/Samples.js'),
  '/*FM1_NOTICES*/': html(notices.join('\n')),
};
for (const [k, v] of Object.entries(replacements)) web = web.split(k).join(v);
if (/\/\*FM1_[A-Z_]+\*\//.test(web)) throw new Error('Unexpanded HTML placeholder.');
const out = process.argv[2] || path.join(root, `build/official-web/FM1-${fwName}-${buildLabel}-Updater.html`);
fs.mkdirSync(path.dirname(out), { recursive: true });
fs.writeFileSync(out, web);
console.log(`${fwName} ${buildLabel} workbench: ${out} (${(web.length / 1048576).toFixed(1)} MiB), firmware ${pkg.sha256.slice(0, 12)}, build ${pkg.build_id.slice(0, 12)}`);
