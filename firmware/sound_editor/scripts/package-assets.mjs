import { readdir, readFile, writeFile } from 'node:fs/promises';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { gzipSync } from 'node:zlib';
import { createHash } from 'node:crypto';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const dist = join(root, 'dist');
const lock = JSON.parse(await readFile(join(root, 'package-lock.json'), 'utf8'));
const packages = JSON.parse(await readFile(join(dist, 'bundled-packages.json'), 'utf8'));
const notices = [];

// Vite records the packages in emitted chunks, excluding compiler-only dependencies.
for (const name of packages) {
  const path = `node_modules/${name}`;
  const metadata = lock.packages[path];
  if (!metadata) throw new Error(`Bundled package missing from lockfile: ${name}`);
  const directory = join(root, path);
  const licenses = (await readdir(directory)).filter((name) => /^(licen[cs]e|copying|notice)([.-]|$)/i.test(name)).sort();
  if (licenses.length === 0) throw new Error(`Missing license notice: ${path}`);
  notices.push(`=== ${path.replace(/^node_modules\//, '')} ${metadata.version} (${metadata.license ?? 'see notice'}) ===`);
  for (const license of licenses) notices.push(await readFile(join(directory, license), 'utf8'));
}
await writeFile(join(dist, 'THIRD_PARTY_NOTICES.txt'), notices.join('\n\n'));

const assets = [];
async function packageDirectory(directory) {
  for (const entry of (await readdir(directory, { withFileTypes: true })).sort((a, b) => a.name.localeCompare(b.name))) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) {
      await packageDirectory(path);
    } else if (/\.(js|css|html|txt)$/.test(entry.name)) {
      const data = await readFile(path);
      const gzip = gzipSync(data, { level: 9 });
      await writeFile(`${path}.gz`, gzip);
      assets.push({ path: relative(dist, path).replaceAll('\\', '/'), bytes: data.length, gzipBytes: gzip.length });
    }
  }
}
await packageDirectory(dist);
const payload = assets.filter((asset) => /\.(js|css|html)$/.test(asset.path));
const report = {
  gzipLevel: 9,
  assets,
  payloadBytes: payload.reduce((total, asset) => total + asset.bytes, 0),
  payloadGzipBytes: payload.reduce((total, asset) => total + asset.gzipBytes, 0),
  totalGzipBytes: assets.reduce((total, asset) => total + asset.gzipBytes, 0),
};
await writeFile(join(dist, 'asset-sizes.json'), `${JSON.stringify(report, null, 2)}\n`);
const files = {};
async function hashInputs(directory) {
  for (const entry of (await readdir(join(root, directory), { withFileTypes: true })).sort((a, b) => a.name.localeCompare(b.name))) {
    const path = `${directory}/${entry.name}`;
    if (entry.isDirectory()) await hashInputs(path);
    else files[path] = createHash('sha256').update(await readFile(join(root, path))).digest('hex');
  }
}
await hashInputs('src');
await hashInputs('scripts');
await hashInputs('public');
for (const path of ['index.html', 'package.json', 'package-lock.json', 'tsconfig.json', 'vite.config.ts']) files[path] = createHash('sha256').update(await readFile(join(root, path))).digest('hex');
await writeFile(join(dist, 'build-inputs.json'), `${JSON.stringify({ files }, null, 2)}\n`);
console.log(JSON.stringify(report, null, 2));
