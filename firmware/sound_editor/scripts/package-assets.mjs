// Dependency-free build for the offline sound block editor.
//
// The editor is a single self-contained HTML file (public/blocks.html) with no
// runtime dependencies. This script copies it to dist/, writes the gzip copies,
// the size report and the source stamp that tools/gen_sound_editor.py verifies
// before the firmware embeds the assets. It needs only a stock Node.js runtime.

import { mkdir, readdir, readFile, rm, writeFile } from 'node:fs/promises';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { gzipSync } from 'node:zlib';
import { createHash } from 'node:crypto';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const source = join(root, 'public');
const dist = join(root, 'dist');

await rm(dist, { recursive: true, force: true });
await mkdir(dist, { recursive: true });

const assets = [];
async function packageDirectory(directory) {
  for (const entry of (await readdir(directory, { withFileTypes: true })).sort((a, b) => a.name.localeCompare(b.name))) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) { await packageDirectory(path); continue; }
    if (!/\.(js|css|html|txt)$/.test(entry.name)) continue;
    const data = await readFile(path);
    const target = join(dist, relative(source, path));
    await mkdir(dirname(target), { recursive: true });
    await writeFile(target, data);
    const gzip = gzipSync(data, { level: 9 });
    await writeFile(`${target}.gz`, gzip);
    assets.push({ path: relative(dist, target).replaceAll('\\', '/'), bytes: data.length, gzipBytes: gzip.length });
  }
}
await packageDirectory(source);

const payload = assets.filter((asset) => /\.(js|css|html)$/.test(asset.path));
const report = {
  gzipLevel: 9,
  assets,
  payloadBytes: payload.reduce((total, asset) => total + asset.bytes, 0),
  payloadGzipBytes: payload.reduce((total, asset) => total + asset.gzipBytes, 0),
  totalGzipBytes: assets.reduce((total, asset) => total + asset.gzipBytes, 0),
};
await writeFile(join(dist, 'asset-sizes.json'), `${JSON.stringify(report, null, 2)}\n`);

// The block editor bundles no third-party libraries; keep the notice file in
// place so the firmware asset set keeps a stable shape.
await writeFile(join(dist, 'THIRD_PARTY_NOTICES.txt'),
  'AURA-X sound block editor\n\nThis editor is original code and bundles no third-party libraries.\n');

// Source stamp consumed by tools/gen_sound_editor.py: any change to these files
// after a build makes the firmware build reject the stale bundle.
const files = {};
async function hashInputs(directory) {
  for (const entry of (await readdir(join(root, directory), { withFileTypes: true })).sort((a, b) => a.name.localeCompare(b.name))) {
    const path = `${directory}/${entry.name}`;
    if (entry.isDirectory()) await hashInputs(path);
    else files[path] = createHash('sha256').update(await readFile(join(root, path))).digest('hex');
  }
}
await hashInputs('public');
await hashInputs('scripts');
files['package.json'] = createHash('sha256').update(await readFile(join(root, 'package.json'))).digest('hex');
await writeFile(join(dist, 'build-inputs.json'), `${JSON.stringify({ files }, null, 2)}\n`);

console.log(JSON.stringify(report, null, 2));
