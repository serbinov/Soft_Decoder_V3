import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { gunzipSync } from 'node:zlib';
import { createHash } from 'node:crypto';
import { readdir } from 'node:fs/promises';
import test from 'node:test';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const dist = join(root, 'dist');

test('production assets use the local sound-editor route without CDN URLs', async () => {
  const html = await readFile(join(dist, 'index.html'), 'utf8');
  assert.match(html, /src="\/sound-editor\/assets\/.+\.js"/);
  assert.match(html, /href="\/sound-editor\/assets\/.+\.css"/);
  assert.doesNotMatch(html, /(?:src|href)="(?:https?:)?\/\//);
  const report = JSON.parse(await readFile(join(dist, 'asset-sizes.json'), 'utf8'));
  for (const asset of report.assets.filter((asset) => /\.(js|css)$/.test(asset.path))) {
    const content = await readFile(join(dist, asset.path), 'utf8');
    assert.doesNotMatch(content, /(?:import\s*\(|url\(\s*["']?)(?:https?:)?\/\//);
  }
});

test('gzip artifacts and the resource report match the production files', async () => {
  const report = JSON.parse(await readFile(join(dist, 'asset-sizes.json'), 'utf8'));
  for (const asset of report.assets) {
    const raw = await readFile(join(dist, asset.path));
    const gzip = await readFile(join(dist, `${asset.path}.gz`));
    assert.equal(raw.length, asset.bytes);
    assert.equal(gzip.length, asset.gzipBytes);
    assert.deepEqual(gunzipSync(gzip), raw);
  }
  assert.equal(report.totalGzipBytes, report.assets.reduce((sum, asset) => sum + asset.gzipBytes, 0));
  assert.equal(report.payloadGzipBytes, report.assets.filter((asset) => /\.(js|css|html)$/.test(asset.path)).reduce((sum, asset) => sum + asset.gzipBytes, 0));
});

test('production distribution retains the MIT dependency notices', async () => {
  const notices = await readFile(join(dist, 'THIRD_PARTY_NOTICES.txt'), 'utf8');
  const packages = JSON.parse(await readFile(join(dist, 'bundled-packages.json'), 'utf8'));
  for (const name of packages) assert.ok(notices.includes(`=== ${name} `), `Missing notice: ${name}`);
  assert.match(notices, /@xyflow\/svelte 1\.7\.0/);
  assert.match(notices, /@xyflow\/system/);
  assert.match(notices, /svelte 5\.57\.1/);
  assert.match(notices, /Permission is hereby granted/);
  assert.match(notices, /copyright notice and this permission notice/);
});

test('production build stamp hashes the complete source and build inputs', async () => {
  const stamp = JSON.parse(await readFile(join(dist, 'build-inputs.json'), 'utf8'));
  const expected = ['index.html', 'package.json', 'package-lock.json', 'tsconfig.json', 'vite.config.ts'];
  async function collect(directory) {
    for (const entry of await readdir(join(root, directory), { withFileTypes: true })) {
      const path = `${directory}/${entry.name}`;
      if (entry.isDirectory()) await collect(path); else expected.push(path);
    }
  }
  await collect('src'); await collect('scripts');
  assert.deepEqual(Object.keys(stamp.files).sort(), expected.sort());
  for (const [path, hash] of Object.entries(stamp.files)) assert.equal(createHash('sha256').update(await readFile(join(root, path))).digest('hex'), hash);
});
