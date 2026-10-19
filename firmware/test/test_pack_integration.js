const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const zlib = require('node:zlib');
const { spawnSync } = require('node:child_process');
const { test } = require('node:test');

const ROOT = path.resolve(__dirname, '..');
const TOOL = path.join(ROOT, 'tools', 'pack_sounds.py');
const HTML = fs.readFileSync(path.join(ROOT, 'web_ui.html'), 'utf8');
const SCRIPT = HTML.match(/<script>([\s\S]*?)<\/script>/)[1].replace(/\binit\(\);\s*$/, '');
const PYTHON = process.env.PYTHON || 'python';

function tmpDir(name) {
  return fs.mkdtempSync(path.join(os.tmpdir(), 'aura-' + name + '-'));
}

function runPython(args, expectOk = true) {
  const r = spawnSync(PYTHON, [TOOL, ...args], { encoding: 'utf8', cwd: ROOT });
  if (expectOk && r.status !== 0) {
    throw new Error('python pack_sounds failed (' + r.status + '):\n' + (r.stdout || '') + (r.stderr || ''));
  }
  return r;
}

function pcmPayload(samples = 512) {
  const u8 = new Uint8Array(samples * 2);
  for (let i = 0; i < samples; i++) {
    const v = ((i * 97) % 2000) - 1000;
    u8[i * 2] = v & 0xff;
    u8[i * 2 + 1] = (v >> 8) & 0xff;
  }
  return u8;
}

function writePcmWav(file, rate, channels, samples) {
  const dataLen = samples * 2 * channels;
  const b = Buffer.alloc(44 + dataLen);
  b.write('RIFF', 0);
  b.writeUInt32LE(36 + dataLen, 4);
  b.write('WAVE', 8);
  b.write('fmt ', 12);
  b.writeUInt32LE(16, 16);
  b.writeUInt16LE(1, 20);
  b.writeUInt16LE(channels, 22);
  b.writeUInt32LE(rate, 24);
  b.writeUInt32LE(rate * channels * 2, 28);
  b.writeUInt16LE(channels * 2, 32);
  b.writeUInt16LE(16, 34);
  b.write('data', 36);
  b.writeUInt32LE(dataLen, 40);
  for (let i = 0; i < samples; i++) {
    const v = ((i * 97) % 2000) - 1000;
    for (let c = 0; c < channels; c++) b.writeInt16LE(v, 44 + (i * channels + c) * 2);
  }
  fs.writeFileSync(file, b);
}

function readWavInfo(file) {
  const b = fs.readFileSync(file);
  assert.equal(b.toString('ascii', 0, 4), 'RIFF');
  assert.equal(b.toString('ascii', 8, 12), 'WAVE');
  const fmt = b.indexOf(Buffer.from('fmt '), 12, 'ascii');
  assert.ok(fmt > 0, 'fmt chunk present');
  const data = b.indexOf(Buffer.from('data'), 12, 'ascii');
  assert.ok(data > 0, 'data chunk present');
  const format = b.readUInt16LE(fmt + 8);
  const channels = b.readUInt16LE(fmt + 10);
  const rate = b.readUInt32LE(fmt + 12);
  const bits = format === 0x0011 ? 4 : b.readUInt16LE(fmt + 22);
  const dataLen = b.readUInt32LE(data + 4);
  const durationMs = bits === 4
    ? Math.floor(Math.floor(dataLen / 256) * 505 * 1000 / rate)
    : Math.floor(dataLen * 1000 / (rate * channels * 2));
  return {
    size: b.length,
    format,
    channels,
    rate,
    bits,
    durationMs,
    crc32: (zlib.crc32(b) >>> 0).toString(16).padStart(8, '0'),
    headerBytes: data + 8
  };
}

function harness() {
  const elements = new Map();
  let blob = null;
  let download = '';
  const logs = [];
  class MockBlob {
    constructor(parts) {
      this.bytes = Buffer.from(parts[0]);
      blob = this.bytes;
    }
  }
  function element(id) {
    if (!elements.has(id)) elements.set(id, {
      value: '', textContent: '', innerHTML: '', style: {}, children: [], files: [],
      classList: { add() {}, remove() {}, toggle() {} },
      addEventListener() {}, querySelectorAll: () => [], appendChild() {}, prepend() {}
    });
    return elements.get(id);
  }
  const ctx = {
    console, AbortController, Headers, URL, Date, Promise, TextDecoder, TextEncoder,
    Blob: MockBlob,
    document: {
      hidden: false, getElementById: element,
      createElement: () => {
        const a = { href: '', click() {} };
        Object.defineProperty(a, 'download', { get: () => download, set: v => { download = v; } });
        return a;
      },
      addEventListener() {}
    },
    location: { hostname: 'device', href: '' },
    setTimeout() {}, clearTimeout() {}, setInterval() { return 1; }, clearInterval() {},
    confirm: () => true, fetch: null,
    XMLHttpRequest: class {
      constructor() {
        this.upload = {};
        this.status = 200;
        this.responseText = '{"ok":true,"files":2}';
        this.sent = null;
      }
      open(method, url) { this.method = method; this.url = url; }
      send(body) { this.sent = Buffer.from(body.bytes || body); }
      abort() {}
    }
  };
  ctx.window = ctx;
  ctx.addEventListener = () => {};
  vm.createContext(ctx);
  vm.runInContext(SCRIPT, ctx);
  ctx.log = (tag, msg) => logs.push(msg);
  ctx.showSetting = () => {};
  ctx.loadLibrary = async () => {};
  ctx.loadFreeSpace = () => {};
  ctx.startHeartbeat = ctx.refreshStatus = ctx.stopStatusPoll = ctx.stopHeartbeat =
    ctx.startStatusPoll = ctx.stopUpPoll = ctx.startUpPoll = () => {};
  ctx.localStorage = { removeItem() {} };
  ctx.URL = { createObjectURL: () => 'blob:test', revokeObjectURL() {} };
  return {
    ctx, logs, element,
    get blob() { return blob; },
    get download() { return download; },
    set download(v) { download = v; },
    reset() { blob = null; download = ''; logs.length = 0; }
  };
}

function deviceStubs(h, opts = {}) {
  const pcm = pcmPayload();
  const adpcm = new Uint8Array(256 * 8);
  for (let i = 0; i < adpcm.length; i++) adpcm[i] = (i * 13) & 0xff;
  const lofi = new Uint8Array(1000);
  for (let i = 0; i < lofi.length; i++) lofi[i] = (i * 7) & 0xff;
  const unused = pcmPayload(256);
  h.ctx.libFiles = [
    { name: 'Horn', size: pcm.length, vol: 55 },
    { name: 'Brake', size: adpcm.length, vol: 80 },
    { name: 'LoFi', size: lofi.length, vol: 90 },
    { name: 'Unused', size: unused.length, vol: 100 }
  ];
  h.ctx.libTotal = h.ctx.libFiles.length;
  h.ctx.lastLibSig = '';
  h.ctx.api = async url => {
    if (url === '/api/device') return { ok: true, name: opts.deviceName || 'ТЭМ2-1234' };
    if (url === '/api/sound/graph/state') return { ok: true, id: 'packtest', revision: 3 };
    if (url.indexOf('/api/sound/graph/project') === 0) {
      return {
        ok: true,
        project: {
          format: 'sound-graph', schemaVersion: 1, id: 'packtest', name: 'Pack Test',
          states: [
            { id: 'idle', file: 'Horn.wav', loop: true, volume: 100, rate: 1000 },
            { id: 'move', file: 'Brake.wav', loop: false, volume: 100, rate: 1000 },
            { id: 'announce', file: 'LoFi.wav', loop: false, volume: 100, rate: 1000 }
          ],
          transitions: [{ id: 'go', source: 'idle', target: 'move', priority: 1, timing: 'immediate',
            condition: { type: 'engine_on' } }],
          effects: [], assets: [], editor: { zoom: 1 }
        }
      };
    }
    if (url.indexOf('/api/func-map') === 0) return { ok: true, binds: [] };
    if (url.indexOf('/api/aux/cfg') === 0) return { ok: true, aux: {} };
    throw new Error('unexpected api ' + url);
  };
  h.ctx.fetch = async url => {
    const s = String(url);
    const ad = s.indexOf('Brake') >= 0;
    const lf = s.indexOf('LoFi') >= 0;
    const un = s.indexOf('Unused') >= 0;
    const data = un ? unused : (lf ? lofi : (ad ? adpcm : pcm));
    return {
      ok: true,
      arrayBuffer: async () => data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength),
      headers: { get: n => ({
        'X-Audio-Rate': lf ? '15625' : (ad ? '16000' : '22050'),
        'X-Audio-Channels': '1',
        'X-Audio-Bits': lf ? '8' : (ad ? '4' : '16')
      })[n] }
    };
  };
  h.ctx.promptPasswordDialog = async () => opts.password || '';
}

async function exportPack(h, password = '', confirm = true) {
  deviceStubs(h, { password });
  h.reset();
  let stats = null;
  h.ctx.showSavePackDialog = async (name, size, downloadInfo, tooBig, repairs) => {
    stats = { name, size, download: downloadInfo, tooBig, repairs };
    if (!confirm) return false;
    if (downloadInfo) h.download = downloadInfo.name;
    return true;
  };
  await h.ctx.exportSoundPack();
  return { bytes: h.blob, name: h.download, stats };
}

function importPack(h, bytes, fileName, password = '') {
  let stats = null;
  h.ctx.promptPasswordDialog = async () => password;
  h.ctx.showPackStatsDialog = async (name, size, info, encrypted, mode) => {
    stats = { name, size, info, encrypted, mode };
    return true;
  };
  const file = {
    name: fileName,
    size: bytes.length,
    arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength)
  };
  return h.ctx.uploadPack({ files: [file], value: '' }).then(() => stats);
}

function lastXhr(ctx) {
  const xhrs = [];
  const Original = ctx.XMLHttpRequest;
  ctx.XMLHttpRequest = class extends Original {
    constructor() { super(); xhrs.push(this); }
  };
  return xhrs;
}

test('JS export -> Python unpack: real files, scheme manifest and volumes', async t => {
  const dir = tmpDir('export');
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const h = harness();
  const { bytes, name, stats } = await exportPack(h, '');
  assert.ok(bytes, 'export produced a blob: ' + h.logs.join(' | '));
  assert.equal(name, 'Pack Test.asp');
  assert.ok(stats, 'save dialog shown');
  assert.equal(stats.name, 'Pack Test.asp');
  assert.equal(stats.download.name, 'Pack Test.asp');
  assert.match(stats.download.url, /^blob:/);
  const info = h.ctx.parseAspPack(new Uint8Array(bytes));
  assert.equal(info.audio.length, 4);
  assert.equal(info.adpcm, 4, 'all PCM (16 and 8 bit) is compressed to ADPCM on export');
  assert.equal(info.pcm, 0);
  assert.equal(info.scheme.id, 'packtest');
  const asp = path.join(dir, 'pack.asp');
  fs.writeFileSync(asp, bytes);
  const out = path.join(dir, 'out');
  runPython([asp, '--unpack', '-o', out]);

  for (const file of ['Horn.wav', 'Brake.wav', 'LoFi.wav', 'Unused.wav', 'scheme.json', 'volumes.txt']) {
    assert.ok(fs.existsSync(path.join(out, file)), file + ' extracted');
  }
  const scheme = JSON.parse(fs.readFileSync(path.join(out, 'scheme.json'), 'utf8'));
  assert.equal(scheme.id, 'packtest');
  assert.equal(scheme.name, 'Pack Test');
  assert.equal(scheme.assets.length, 3);
  for (const asset of scheme.assets) {
    const info = readWavInfo(path.join(out, asset.file));
    assert.equal(asset.size, info.size, asset.file + ' size');
    assert.equal(asset.crc32, info.crc32, asset.file + ' crc32');
    assert.equal(asset.sampleRate, info.rate, asset.file + ' rate');
    assert.equal(asset.channels, info.channels, asset.file + ' channels');
    assert.equal(asset.bits, info.bits, asset.file + ' bits');
    assert.equal(asset.durationMs, info.durationMs, asset.file + ' duration');
  }
  assert.deepEqual(
    fs.readFileSync(path.join(out, 'volumes.txt'), 'utf8').trim().split(/\r?\n/).sort(),
    ['Brake=80', 'Horn=55', 'LoFi=90', 'Unused=100']
  );
  assert.equal(fs.existsSync(path.join(out, 'Unused.wav')), true, 'all library sounds are packed');
  const lofi = readWavInfo(path.join(out, 'LoFi.wav'));
  assert.equal(lofi.bits, 4);
  assert.equal(lofi.rate, 15625);
});

test('8-bit PCM device sounds are converted to ADPCM on export', () => {
  const h = harness();
  const data = new Uint8Array([0, 128, 255, 64]);
  const e = h.ctx.packPrepareEntry('LoFi', 100, 1, 8, 15625, data);
  assert.equal(e.bits, 4);
  assert.equal(e.ch, 1);
  assert.equal(e.data.length % 256, 0);
  assert.equal(e.data[0], 0x00, 'first sample -128*256 little-endian low byte');
  assert.equal(e.data[1], 0x80, 'first sample -128*256 little-endian high byte');
});

test('IMA ADPCM encoder matches pack_sounds.py byte-for-byte', () => {
  const h = harness();
  const pcm = pcmPayload(2000);
  const js = Buffer.from(h.ctx.imaEncodePcm16(pcm, 1));
  const py = `import sys,struct; sys.path.insert(0,${JSON.stringify(path.join(ROOT, 'tools'))}); import pack_sounds as p; b=sys.stdin.buffer.read(); sys.stdout.write(p._ima_encode(list(struct.unpack('<%dh'%(len(b)//2),b))).hex())`;
  const r = spawnSync(PYTHON, ['-c', py], { input: pcm, encoding: 'utf8' });
  assert.equal(r.status, 0, r.stderr);
  assert.equal(js.toString('hex'), r.stdout.trim());
});

test('JS encrypted export -> Python unpack with the same password', async t => {
  const dir = tmpDir('encrypted-export');
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const h = harness();
  const { bytes } = await exportPack(h, 'secret-42');
  assert.equal(bytes.toString('ascii', 0, 8), 'AURAPW01');
  const asp = path.join(dir, 'pack.asp');
  fs.writeFileSync(asp, bytes);
  const out = path.join(dir, 'out');
  runPython([asp, '--unpack', '--password', 'secret-42', '-o', out]);
  const scheme = JSON.parse(fs.readFileSync(path.join(out, 'scheme.json'), 'utf8'));
  assert.equal(scheme.id, 'packtest');
  assert.ok(fs.existsSync(path.join(out, 'Horn.wav')));
});

test('JS export can be cancelled from the save dialog without downloading', async t => {
  const h = harness();
  const { bytes, name, stats } = await exportPack(h, '', false);
  assert.ok(stats, 'save dialog is shown before the download');
  assert.ok(bytes, 'the pack was built before asking for confirmation');
  assert.equal(name, '', 'no download is started');
  assert.ok(h.logs.join(' ').indexOf('отменён') >= 0, 'cancellation is reported');
});

test('Python pack -> JS import: statistics and exact upload body (PCM16)', async t => {
  const dir = tmpDir('import-pcm');
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  writePcmWav(path.join(dir, 'Horn.wav'), 22050, 1, 512);
  writePcmWav(path.join(dir, 'Brake.wav'), 16000, 1, 512);
  fs.writeFileSync(path.join(dir, 'scheme.json'), JSON.stringify({
    format: 'sound-graph', schemaVersion: 1, id: 'packtest', name: 'Pack Test',
    engine: { entry: 'idle', fn: 1 }, hysteresis: 2,
    states: [
      { id: 'idle', name: 'Idle', file: 'Horn.wav', loop: true, volume: 100, rate: 1000 },
      { id: 'move', name: 'Move', file: 'Brake.wav', loop: false, volume: 100, rate: 1000 }
    ],
    transitions: [{ id: 'go', source: 'idle', target: 'move', priority: 1, timing: 'immediate',
      condition: { type: 'engine_on' } }],
    effects: [], assets: [], editor: { zoom: 1 }
  }));
  const asp = path.join(dir, 'pack.asp');
  runPython([dir, '--pcm', '-o', asp]);
  const bytes = new Uint8Array(fs.readFileSync(asp));

  const h = harness();
  const xhrs = lastXhr(h.ctx);
  const stats = await importPack(h, bytes, 'pack.asp');
  assert.ok(stats, 'stats dialog shown');
  assert.equal(stats.encrypted, false);
  assert.equal(stats.info.audio.length, 2);
  assert.equal(stats.info.pcm, 2);
  assert.equal(stats.info.adpcm, 0);
  assert.equal(stats.info.scheme.id, 'packtest');
  assert.equal(stats.info.scheme.states, 2);
  assert.equal(stats.info.scheme.transitions, 1);
  assert.equal(xhrs.length, 1);
  assert.equal(xhrs[0].url, '/api/audio/pack');
  assert.deepEqual(xhrs[0].sent, Buffer.from(bytes));
});

test('Python pack -> JS import: statistics and exact upload body (IMA ADPCM)', async t => {
  const dir = tmpDir('import-adpcm');
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  writePcmWav(path.join(dir, 'Horn.wav'), 22050, 1, 512);
  writePcmWav(path.join(dir, 'Brake.wav'), 16000, 1, 512);
  fs.writeFileSync(path.join(dir, 'scheme.json'), JSON.stringify({
    format: 'sound-graph', schemaVersion: 1, id: 'packtest', name: 'Pack Test',
    states: [
      { id: 'idle', file: 'Horn.wav' },
      { id: 'move', file: 'Brake.wav' }
    ],
    transitions: [], effects: []
  }));
  const asp = path.join(dir, 'pack.asp');
  runPython([dir, '-o', asp]);
  const bytes = new Uint8Array(fs.readFileSync(asp));

  const h = harness();
  const xhrs = lastXhr(h.ctx);
  const stats = await importPack(h, bytes, 'pack.asp');
  assert.ok(stats);
  assert.equal(stats.info.adpcm, 2);
  assert.equal(stats.info.pcm, 0);
  assert.equal(stats.info.scheme.states, 2);
  assert.deepEqual(xhrs[0].sent, Buffer.from(bytes));
});

test('Python encrypted pack -> JS password prompt and import of decrypted bytes', async t => {
  const dir = tmpDir('import-encrypted');
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  writePcmWav(path.join(dir, 'Horn.wav'), 22050, 1, 512);
  fs.writeFileSync(path.join(dir, 'scheme.json'), JSON.stringify({
    format: 'sound-graph', schemaVersion: 1, id: 'packtest', name: 'Pack Test',
    states: [{ id: 'idle', file: 'Horn.wav' }], transitions: [], effects: []
  }));
  const asp = path.join(dir, 'pack.asp');
  runPython([dir, '-o', asp, '--password', 'secret-42']);
  const bytes = new Uint8Array(fs.readFileSync(asp));
  assert.equal(Buffer.from(bytes).toString('ascii', 0, 8), 'AURAPW01');

  const h = harness();
  const xhrs = lastXhr(h.ctx);
  const stats = await importPack(h, bytes, 'pack.asp', 'secret-42');
  assert.ok(stats);
  assert.equal(stats.encrypted, true);
  assert.equal(stats.info.audio.length, 1);
  assert.equal(stats.info.scheme.id, 'packtest');
  assert.equal(xhrs[0].sent.toString('ascii', 0, 8), 'AURASP01', 'device receives the decrypted container');
  const parsed = h.ctx.parseAspPack(new Uint8Array(xhrs[0].sent));
  assert.equal(parsed.audio[0].name, 'Horn');
});
