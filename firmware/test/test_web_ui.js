const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const nodeCrypto = require('node:crypto');
const zlib = require('node:zlib');
const { test } = require('node:test');

const html = fs.readFileSync(path.join(__dirname, '..', 'web_ui.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1]
  .replace(/\binit\(\);\s*$/, '');
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const response = data => ({ ok: true, status: 200,
  headers: new Headers({ 'content-type': 'application/json' }),
  json: async () => data });

function buildAsp(entries) {  const names = entries.map(e => Buffer.from(e.name, 'utf8'));
  const indexLen = entries.reduce((n, e, i) => n + 16 + names[i].length, 0);
  const dataOff = 16 + indexLen;
  const buf = Buffer.alloc(dataOff + entries.reduce((n, e) => n + e.data.length, 0));
  buf.write('AURASP01', 0, 'ascii');
  buf.writeUInt16LE(1, 8);
  buf.writeUInt16LE(entries.length, 10);
  buf.writeUInt32LE(dataOff, 12);
  let pos = 16, off = dataOff;
  entries.forEach((e, i) => {
    buf[pos] = names[i].length;
    buf[pos + 1] = e.vol;
    buf[pos + 2] = e.ch;
    buf[pos + 3] = e.bits;
    buf.writeUInt32LE(e.rate, pos + 4);
    buf.writeUInt32LE(off, pos + 8);
    buf.writeUInt32LE(e.data.length, pos + 12);
    names[i].copy(buf, pos + 16);
    pos += 16 + names[i].length;
    e.data.copy(buf, off);
    off += e.data.length;
  });
  return new Uint8Array(buf);
}

function validPack() {
  return buildAsp([{ name: 'Horn', vol: 100, ch: 1, bits: 16, rate: 22050,
    data: Buffer.from([1, 0, 2, 0]) }]);
}

function packFile(name, bytes, withArrayBuffer = true) {
  const file = { name, size: bytes.length, bytes };
  if (withArrayBuffer) file.arrayBuffer = async () => bytes.buffer;
  return file;
}

function fixture(t) {
  const elements = new Map(), events = {}, timers = new Set(), intervals = new Map();
  let intervalId = 0;
  function element(id) {
    if (!elements.has(id)) elements.set(id, {
      value: '', checked: false, disabled: false, files: [], style: {}, children: [],
      classList: { add() {}, remove() {}, toggle() {} },
      addEventListener() {}, querySelectorAll: () => [], appendChild() {}, prepend() {}
    });
    return elements.get(id);
  }
  const requests = [], xhrs = [];
  const context = {
    console, AbortController, Headers, URL, Date, Promise, TextDecoder, TextEncoder, Blob,
    crypto: { getRandomValues(bytes) { nodeCrypto.randomFillSync(bytes); return bytes; } },
    document: { hidden: false, activeElement: null,
      getElementById: element, createElement: () => element('created'),
      addEventListener: (name, fn) => { events[name] = fn; } },
    location: { hostname: '192.168.100.1', href: '', reloads: 0,
      reload() { this.reloads++; } },
    setTimeout(fn, ms) {
      const timer = setTimeout(() => { timers.delete(timer); fn(); }, ms);
      timers.add(timer); return timer;
    },
    clearTimeout(timer) { timers.delete(timer); clearTimeout(timer); },
    setInterval(fn, ms) { const id = ++intervalId; intervals.set(id, { fn, ms }); return id; },
    clearInterval(id) { intervals.delete(id); },
    confirm: () => true,
    fetch: async (url, options) => {
      requests.push({ url, options });
      const speed = Number(new URL(url, 'http://192.168.100.1').searchParams.get('speed')) || 0;
      return response({ ok: true, speed, forward: true, entries: [], states: [], active: false });
    },
    XMLHttpRequest: class {
      constructor() { this.upload = {}; xhrs.push(this); }
      open(method, url) { this.method = method; this.url = url; }
      setRequestHeader() {}
      send(file) { this.file = file; }
    }
  };
  context.window = context;
  context.addEventListener = (name, fn) => { events[name] = fn; };
  vm.createContext(context);
  vm.runInContext(script, context, { filename: 'web_ui.html' });
  vm.runInContext('log=function(){}; renderMotor=function(){}; loadFreeSpace=function(){};', context);
  t.after(() => { for (const timer of timers) clearTimeout(timer); });
  return { context, element, events, requests, xhrs, intervals };
}

test('API deadline includes a stalled JSON body', async t => {
  const { context } = fixture(t);
  let signal;
  context.fetch = async (url, options) => {
    signal = options.signal;
    return { ok: true, headers: new Headers({ 'content-type': 'application/json' }),
      json: () => new Promise((resolve, reject) => signal.addEventListener('abort', () => {
        const error = new Error('aborted'); error.name = 'AbortError'; reject(error);
      })) };
  };
  await assert.rejects(context.api('/api/device', { timeout: 15 }), /ТАЙМАУТ/);
  assert.equal(signal.aborted, true);
});

test('unknown calibration outcome keeps cancellation available and blocks a new run', async t => {
  const { context, element, requests } = fixture(t);
  let reachable = false;
  context.fetch = async (url, options) => {
    requests.push({ url, options });
    if (!reachable) throw new TypeError('offline');
    return response(url.includes('cancel=1') ? { ok: true } :
      { ok: true, active: false, result: 'cancelled', error: 'none', stored: false });
  };
  await context.startBemfCal();
  await wait(0);
  for (let i = 0; i < 4; i++) await context.loadBemfCal();
  assert.equal(context.bemfDiscoverTries, 0);
  assert.equal(context.bemfUnknown, true);
  assert.equal(element('bemf_cal_btn').disabled, true);
  assert.equal(element('bemf_clear_btn').disabled, true);
  assert.equal(element('bemf_cancel_btn').disabled, false);
  await context.startBemfCal();
  assert.equal(requests.filter(r => r.url.includes('start=1')).length, 1);
  reachable = true;
  await context.cancelBemfCal();
  await wait(0);
  assert.equal(requests.filter(r => r.url.includes('cancel=1')).length, 1);
  assert.equal(context.bemfUnknown, false);
  assert.equal(element('bemf_cal_btn').disabled, false);
  assert.equal(element('bemf_cancel_btn').disabled, true);
});

test('client error reporting consumes failures and is rate limited', async t => {
  const { context, events } = fixture(t);
  let calls = 0;
  context.fetch = async () => { calls++; throw new TypeError('offline'); };
  events.unhandledrejection({ reason: new Error('one failure') });
  await wait(10);
  assert.equal(context.clientLogBusy, false);
  events.unhandledrejection({ reason: new Error('second failure') });
  await wait(10);
  assert.equal(calls, 1);
});

test('stop cancels pending slider dispatch and clears local speed immediately', async t => {
  const { context, element, requests } = fixture(t);
  element('speed').value = '50';
  context.onSpeedInput();
  const stop = context.setStop();
  assert.equal(context.state.motor.speed, 0);
  assert.equal(context.speedTimer, null);
  await stop;
  await wait(280);
  assert.equal(requests.length, 1);
  assert.match(requests[0].url, /speed=0&/);
});

test('commands are serialized and stale replies do not restore stopped speed', async t => {
  const { context, requests } = fixture(t);
  let complete;
  context.fetch = (url, options) => {
    requests.push({ url, options });
    if (url.includes('speed=50&')) return new Promise(resolve => {
      complete = () => resolve(response({ ok: true, speed: 50, forward: true }));
    });
    return Promise.resolve(response({ ok: true, speed: 0, forward: true }));
  };
  context.state.motor.speed = 50;
  context.sendMotorSpeed();
  await wait(0);
  const stop = context.setStop();
  assert.equal(requests.length, 1);
  complete();
  await stop;
  assert.equal(requests.length, 2);
  assert.match(requests[1].url, /speed=0&/);
  assert.equal(context.state.motor.speed, 0);
  assert.equal(context.motorPending, 0);
});

test('Wi-Fi save preserves the password unless removal is explicit', t => {
  const { context, element } = fixture(t);
  element('wifi_ap_ssid').value = 'Decoder';
  element('wifi_ap_ip').value = '192.168.100.1';
  assert.doesNotMatch(context.wifiQuery(), /ap_password/);
  element('wifi_clear_password').checked = true;
  assert.match(context.wifiQuery(), /ap_password_clear=1/);
});

test('destructive action waits for confirmation and cancellation sends nothing', async t => {
  const { context, requests } = fixture(t);
  const reset = context.cvFactoryReset();
  await wait(0);
  assert.equal(requests.length, 0);
  context.dlgAnswer(false);
  await reset;
  assert.equal(requests.length, 0);
});

test('accepting the confirmation performs the destructive action', async t => {
  const { context, requests } = fixture(t);
  const reset = context.cvFactoryReset();
  await wait(0);
  assert.equal(requests.length, 0);
  context.dlgAnswer(true);
  await reset;
  assert.match(requests[0].url, /cv\/write\?index=8&value=8/);
});

test('failed status poll exits without the old six-request timeout chain', async t => {
  const { context } = fixture(t);
  let calls = 0;
  context.api = async () => { calls++; throw new Error('offline'); };
  await context.refreshStatus();
  assert.equal(calls, 1);
  assert.equal(context.statusBusy, false);
});

test('BEMF polling never overlaps', async t => {
  const { context } = fixture(t);
  let calls = 0, finish;
  context.api = () => { calls++; return new Promise(resolve => { finish = resolve; }); };
  const first = context.loadBemfCal();
  await context.loadBemfCal();
  assert.equal(calls, 1);
  finish({ ok: true, active: false });
  await first;
  assert.equal(context.bemfPollBusy, false);
});

test('page initialization discovers active BEMF and starts exactly one poll', async t => {
  const { context, intervals, element } = fixture(t);
  for (const name of ['renderCvList', 'setActiveCv', 'loadCvValues', 'loadFuncMap',
    'loadDevice', 'loadWifi', 'loadVolumes', 'loadLibrary', 'refreshStatus',
    'startStatusPoll', 'startHeartbeat']) context[name] = () => {};
  let gets = 0;
  context.api = async url => {
    assert.equal(url, '/api/bemf/cal'); gets++;
    return { ok: true, active: true, result: 'running', progress: 2, total: 10 };
  };
  context.init(); await wait(0);
  assert.equal(gets, 1);
  assert.equal(intervals.size, 1);
  assert.equal(intervals.get(context.bemfPollTimer).ms, 1000);
  assert.equal(element('bemf_cal_btn').disabled, true);
  assert.equal(element('bemf_clear_btn').disabled, true);
  assert.equal(element('bemf_cancel_btn').disabled, false);
  await context.loadBemfCal();
  assert.equal(intervals.size, 1);
});

test('lost BEMF start response discovers active job without retrying POST', async t => {
  const { context, intervals } = fixture(t);
  const calls = [];
  context.api = async (url, options) => {
    calls.push({ url, options });
    if (options.method === 'POST') throw new Error('lost response');
    return { ok: true, active: true, result: 'running', total: 10 };
  };
  await context.startBemfCal(); await wait(0);
  assert.equal(calls.length, 2);
  assert.match(calls[0].url, /start=1$/);
  assert.equal(calls[0].options.timeout, 3000);
  assert.equal(calls[1].url, '/api/bemf/cal');
  assert.equal(context.bemfActive, true);
  assert.equal(intervals.size, 1);
});

test('unknown BEMF start outcome retries only GET and stops after bounded discovery', async t => {
  const { context, intervals, element } = fixture(t);
  let posts = 0, gets = 0;
  context.api = async (url, options) => {
    if (options.method === 'POST') posts++; else gets++;
    throw new Error('offline');
  };
  await context.startBemfCal(); await wait(0);
  for (let i = 0; i < 4; i++) await context.loadBemfCal();
  assert.equal(posts, 1); assert.equal(gets, 5);
  assert.equal(intervals.size, 0);
  assert.equal(context.bemfDiscoverTries, 0);
  assert.match(element('bemf_status').textContent, /неизвестен/);
});

test('rapid BEMF starts and resets are blocked while POST is pending', async t => {
  const { context, element } = fixture(t);
  let posts = 0, complete;
  context.api = (url, options) => {
    if (options.method !== 'POST') return Promise.resolve({ ok: true, active: true, result: 'running' });
    posts++; return new Promise(resolve => { complete = resolve; });
  };
  const first = context.startBemfCal();
  assert.equal(element('bemf_cal_btn').disabled, true);
  assert.equal(element('bemf_clear_btn').disabled, true);
  await context.startBemfCal(); await context.clearBemfCal();
  assert.equal(posts, 1);
  complete({ ok: true }); await first; await wait(0);
  assert.equal(context.bemfActive, true);
});

test('terminal BEMF failure distinguishes previous stored curve from new success', async t => {
  const { context, element, intervals } = fixture(t);
  const reasons = ['adc', 'rail', 'timeout', 'control', 'curve', 'storage', 'start'];
  for (const error of reasons) {
    context.api = async () => ({ ok: true, active: false, result: 'failed', error,
      stored: true, valid: true, runId: 8 });
    await context.loadBemfCal();
    assert.match(element('bemf_status').textContent, /не удалась:/);
    assert.doesNotMatch(element('bemf_status').textContent, /завершена и сохранена/);
    assert.match(element('bemf_meta').textContent, /предыдущая калибровка/);
  }
  assert.equal(intervals.size, 0);
  context.api = async () => ({ ok: true, result: 'succeeded', active: false, stored: true });
  await context.loadBemfCal();
  assert.match(element('bemf_status').textContent, /Новая калибровка завершена и сохранена/);
  assert.equal(element('bemf_meta').textContent, '');
});

test('saving and terminal cleanup reservations keep polling and disable start/reset', async t => {
  const { context, element, intervals } = fixture(t);
  for (const result of ['saving', 'cancelled', 'failed']) {
    context.api = async () => ({ ok: true, active: true, result, error: 'storage', stored: true });
    await context.loadBemfCal();
    assert.equal(intervals.size, 1);
    assert.equal(element('bemf_cal_btn').disabled, true);
    assert.equal(element('bemf_clear_btn').disabled, true);
    assert.equal(element('bemf_cancel_btn').disabled, false);
    assert.equal(element('bemf_bar').style.display, 'none');
    if (result !== 'saving') assert.match(element('bemf_status').textContent, /очистки/);
  }
});

test('BEMF cancel in rails mode is immediate bodyless POST and never motor STOP', async t => {
  const { context, requests, element } = fixture(t);
  context.state.control_source = 'rails';
  context.fetch = async (url, options) => {
    requests.push({ url, options });
    return response(url.includes('calibrate') ? { ok: true } :
      { ok: true, active: false, result: 'cancelled', error: 'control' });
  };
  context.bemfActive = true; context.renderBemfButtons();
  assert.equal(element('bemf_cancel_btn').disabled, false);
  await context.cancelBemfCal(); await wait(0);
  assert.equal(requests.length, 2);
  assert.equal(requests[0].url, '/api/bemf/calibrate?cancel=1');
  assert.equal(requests[0].options.method, 'POST');
  assert.equal(requests[0].options.body, undefined);
  assert.equal(requests[1].url, '/api/bemf/cal');
  assert.equal(requests.some(r => r.url.includes('/api/motor')), false);
  assert.equal(element('bemf_cancel_btn').disabled, true);
  await context.cancelBemfCal(); assert.equal(requests.length, 2);
});

test('BEMF visibility pauses and resumes one nonoverlapping poll', async t => {
  const { context, events, intervals } = fixture(t);
  context.startStatusPoll = context.startHeartbeat = context.refreshStatus = () => {};
  let calls = 0, complete;
  context.api = () => { calls++; return new Promise(resolve => { complete = resolve; }); };
  context.bemfStartPoll();
  const oldTimer = context.bemfPollTimer;
  context.document.hidden = true; events.visibilitychange();
  assert.equal(intervals.size, 0);
  assert.equal(context.bemfPollActive, true);
  context.document.hidden = false; events.visibilitychange();
  assert.equal(calls, 1); assert.equal(intervals.size, 1);
  assert.notEqual(context.bemfPollTimer, oldTimer);
  await intervals.get(context.bemfPollTimer).fn(); assert.equal(calls, 1);
  complete({ ok: true, active: true, result: 'saving' }); await wait(0);
  assert.equal(intervals.size, 1);
  assert.equal(context.bemfPollBusy, false);
});

test('stale calibration GET cannot erase lost POST discovery', async t => {
  const { context, intervals } = fixture(t);
  let complete, gets = 0;
  context.api = (url, options) => {
    if (options.method === 'POST') return Promise.reject(new Error('lost'));
    gets++;
    if (gets === 1) return new Promise(resolve => { complete = resolve; });
    return Promise.resolve({ ok: true, active: true, result: 'running' });
  };
  const stale = context.loadBemfCal();
  await context.startBemfCal();
  complete({ ok: true, active: false, result: 'idle' }); await stale;
  assert.equal(intervals.size, 1);
  assert.equal(context.bemfDiscoverTries, 5);
  await intervals.get(context.bemfPollTimer).fn();
  assert.equal(context.bemfActive, true);
  assert.equal(gets, 2);
});

test('BEMF mode failure restores actual server state instead of blindly inverting', async t => {
  const { context, element } = fixture(t);
  element('bemf_use').checked = true;
  context.api = async (url, options) => options.method === 'POST' ?
    { ok: false, enabled: true, error: 'storage failed' } : { ok: true, enabled: true };
  await context.setBemfUse();
  assert.equal(element('bemf_use').checked, true);
  assert.equal(element('bemf_use').disabled, false);
});

test('lost mode response reloads actual mode and stale calibration GET cannot undo it', async t => {
  const { context, element } = fixture(t);
  let complete;
  context.api = (url, options) => {
    if (url === '/api/bemf/cal') return new Promise(resolve => { complete = resolve; });
    if (options.method === 'POST') return Promise.reject(new Error('lost'));
    assert.equal(url, '/api/bemf/use');
    return Promise.resolve({ ok: true, enabled: true });
  };
  const stale = context.loadBemfCal();
  element('bemf_use').checked = false;
  await context.setBemfUse();
  assert.equal(element('bemf_use').checked, true);
  complete({ ok: true, active: false, result: 'idle', use: false }); await stale;
  assert.equal(element('bemf_use').checked, true);
});

test('visibility does not restart normal polling during upload', t => {
  const { context, events, intervals } = fixture(t);
  context.uploadBusy = true;
  events.visibilitychange();
  assert.equal(context.statusTimer, null);
  assert.equal(context.heartbeatTimer, null);
  assert.equal(intervals.size, 1);
  assert.equal(intervals.get(context.upPollTimer).ms, 400);
});

test('pack timeout and abort release upload state', async t => {
  const { context, xhrs } = fixture(t);
  context.startHeartbeat = context.refreshStatus = () => {};
  context.showPackStatsDialog = async () => true;
  const bytes = validPack();
  const input = { files: [packFile('pack.asp', bytes)], value: 'pack.asp' };
  await context.uploadPack(input);
  assert.equal(context.uploadBusy, true);
  assert.equal(xhrs[0].url, '/api/audio/pack');
  assert.equal(xhrs[0].timeout, 600000);
  assert.equal(xhrs[0].ontimeout, xhrs[0].onabort);
  xhrs[0].ontimeout();
  assert.equal(context.uploadBusy, false);
  assert.equal(context.upPollTimer, null);
});

test('pack upload accepts only .asp within the size limit', async t => {
  const { context, xhrs } = fixture(t);
  context.startHeartbeat = context.refreshStatus = () => {};
  context.showPackStatsDialog = async () => true;
  await context.uploadPack({ files: [{ name: 'sound.zip', size: 100 }], value: 'sound.zip' });
  assert.equal(context.uploadBusy, false);
  assert.equal(xhrs.length, 0);
  await context.uploadPack({ files: [{ name: 'big.asp', size: 16 * 1024 * 1024 }], value: 'big.asp' });
  assert.equal(context.uploadBusy, false);
  assert.equal(xhrs.length, 0);
  await context.uploadPack({ files: [packFile('pack.ASP', validPack())], value: 'pack.ASP' });
  assert.equal(context.uploadBusy, true);
  assert.equal(xhrs.length, 1);
  assert.equal(xhrs[0].url, '/api/audio/pack');
});

test('pack upload validates the container and respects the stats dialog', async t => {
  const { context, xhrs } = fixture(t);
  context.startHeartbeat = context.refreshStatus = () => {};
  let shown = 0;
  context.showPackStatsDialog = async () => { shown++; return false; };
  await context.uploadPack({ files: [packFile('bad.asp', new Uint8Array(32))], value: 'bad.asp' });
  assert.equal(shown, 0, 'invalid pack is rejected before the dialog');
  assert.equal(context.uploadBusy, false);
  assert.equal(xhrs.length, 0);
  const bytes = validPack();
  await context.uploadPack({ files: [packFile('pack.asp', bytes)], value: 'pack.asp' });
  assert.equal(shown, 1, 'valid pack opens the statistics dialog');
  assert.equal(context.uploadBusy, false, 'cancellation does not start an upload');
  assert.equal(xhrs.length, 0);
});

test('pack upload reads files through FileReader when arrayBuffer is unavailable', async t => {
  const { context, xhrs } = fixture(t);
  context.startHeartbeat = context.refreshStatus = () => {};
  context.showPackStatsDialog = async () => true;
  context.FileReader = class {
    readAsArrayBuffer(file) { this.result = file.bytes.buffer; this.onload(); }
  };
  const bytes = validPack();
  await context.uploadPack({ files: [packFile('pack.asp', bytes, false)], value: 'pack.asp' });
  assert.equal(xhrs.length, 1);
  assert.equal(xhrs[0].url, '/api/audio/pack');
});

test('library rows show index, .wav name, volume and icon play/delete buttons', t => {
  const { context, element } = fixture(t);
  context.libFiles = [{ name: 'Horn', size: 46080, vol: 100 }, { name: 'Bell', size: 1536, vol: 80 }];
  context.lastLibSig = '';
  context.renderLibrary();
  const html = element('sound_lib').innerHTML;
  assert.match(html, /class="idx">1\.</);
  assert.match(html, /class="idx">2\.</);
  assert.match(html, /class="sz"[^>]*>45 КБ</);
  assert.match(html, /class="sz"[^>]*>2 КБ</);
  assert.match(html, /Horn\.wav/);
  assert.match(html, /<button class="btn sm primary"[^>]*>▶<\/button>/);
  assert.match(html, /class="btn sm lib-del"[^>]*><svg/);
});

test('library shows a loader while the first request is pending', async t => {
  const { context, element } = fixture(t);
  let finish;
  context.api = () => new Promise(resolve => { finish = resolve; });
  const pending = context.loadLibrary();
  assert.equal(context.libLoading, true);
  assert.match(element('sound_lib').innerHTML, /spinner|Загрузка звуков/);
  finish({ ok: true, files: [], total: 0, preview: '' });
  await pending;
  assert.equal(context.libLoading, false);
});

test('decoder preview completion clears the stop button even while other audio plays', t => {
  const { context } = fixture(t);
  context.localStorage = { getItem: k => k === 'aura_sound_play' ? 'decoder' : null,
    setItem() {}, removeItem() {} };
  context.libPreview = 'Horn';
  context.applyAudioStatus({ playing: true, preview: 'Horn' });
  assert.equal(context.libPreview, 'Horn');
  context.applyAudioStatus({ playing: true, preview: '' });
  assert.equal(context.libPreview, '');
});

test('lost connection clears a stale sound preview after three failed polls', t => {
  const { context } = fixture(t);
  context.libPreview = 'Horn';
  context.state.audio.preview = 'Horn';
  context.setConnected(false);
  context.setConnected(false);
  context.setConnected(false);
  assert.equal(context.libPreview, '');
  assert.equal(context.state.audio.preview, '');
});

test('PC preview decodes fetched WAV through Web Audio', async t => {
  const { context } = fixture(t);
  let fetched = 0, started = 0;
  context.fetch = async () => { fetched++; return { ok: true, status: 200,
    arrayBuffer: async () => new ArrayBuffer(8) }; };
  context.AudioContext = class {
    constructor() { this.state = 'running'; this.destination = {}; }
    decodeAudioData() { return Promise.resolve({}); }
    createBufferSource() { return { connect() {}, start() { started++; }, stop() {}, disconnect() {}, onended: null }; }
    createGain() { return { gain: { value: 0 }, connect() {}, disconnect() {} }; }
  };
  context.playLibOnPc('Horn');
  await new Promise(resolve => setTimeout(resolve, 10));
  assert.equal(fetched, 1);
  assert.equal(started, 1);
});

test('encryptAspBytes and decryptAspBytes round-trip password-protected sound packs', async t => {
  const { context } = fixture(t);
  const plain = new Uint8Array([65, 85, 82, 65, 83, 80, 48, 49, 1, 0, 1, 0, 32, 0, 0, 0]);
  const enc = await context.encryptAspBytes(plain, 'secret123');
  assert.equal(context.isEncryptedAsp(enc), true);
  assert.equal(Buffer.from(enc).toString('ascii', 0, 8), 'AURAPW02');
  assert.equal(await context.decryptAspBytes(enc, 'wrong'), null, 'Wrong password returns null');
  const tampered = enc.slice(); tampered[60] ^= 1;
  assert.equal(await context.decryptAspBytes(tampered, 'secret123'), null, 'Tampering is rejected');
  const dec = await context.decryptAspBytes(enc, 'secret123');
  assert.deepEqual(Array.from(dec), Array.from(plain), 'Correct password restores original .asp bytes');
});

test('legacy AURAPW01 packs still decrypt and short new passwords are rejected', async t => {
  const { context } = fixture(t);
  const plain = new Uint8Array([65, 85, 82, 65, 83, 80, 48, 49, 9, 9]);
  const pw = 'old-pass';
  const pwBytes = context.utf8Bytes(pw);
  const salt = new Uint8Array([1, 2, 3, 4]);
  const tagInput = new Uint8Array(pwBytes.length + 4);
  tagInput.set(pwBytes); tagInput.set(salt, pwBytes.length);
  const ks = context.prngKeystream(pwBytes, salt, plain.length);
  const legacy = new Uint8Array(16 + plain.length);
  for (let i = 0; i < 8; i++) legacy[i] = 'AURAPW01'.charCodeAt(i);
  legacy.set(salt, 8);
  const tag = context.fnv32(tagInput);
  legacy[12] = tag & 255; legacy[13] = (tag >>> 8) & 255;
  legacy[14] = (tag >>> 16) & 255; legacy[15] = (tag >>> 24) & 255;
  for (let i = 0; i < plain.length; i++) legacy[16 + i] = plain[i] ^ ks[i];
  assert.equal(context.isEncryptedAsp(legacy), true);
  assert.deepEqual(Array.from(await context.decryptAspBytes(legacy, pw)), Array.from(plain));
  await assert.rejects(() => context.encryptAspBytes(plain, 'short'), /at least 8/);
});

test('parseAspPack validates a pack and reports sound and scheme statistics', t => {
  const { context } = fixture(t);
  const pcm = Buffer.from([1, 0, 2, 0]);
  const scheme = Buffer.from(JSON.stringify({
    format: 'sound-graph', id: 'loco', name: 'TEPLOVOZ',
    states: [{ id: 'a' }, { id: 'b' }], transitions: [{ id: 't' }], effects: [{ id: 'e' }]
  }));
  const bytes = buildAsp([
    { name: 'Horn', vol: 80, ch: 1, bits: 16, rate: 22050, data: pcm },
    { name: '__aura_scheme__', vol: 0, ch: 0, bits: 0, rate: 0, data: scheme }
  ]);
  const info = context.parseAspPack(bytes);
  assert.equal(info.audio.length, 1);
  assert.equal(info.audio[0].name, 'Horn');
  assert.equal(info.audio[0].vol, 80);
  assert.equal(info.pcm, 1);
  assert.equal(info.adpcm, 0);
  assert.equal(info.totalData, 4);
  assert.equal(info.rates[22050], 1);
  assert.equal(info.scheme.id, 'loco');
  assert.equal(info.scheme.name, 'TEPLOVOZ');
  assert.equal(info.scheme.states, 2);
  assert.equal(info.scheme.transitions, 1);
  assert.equal(info.scheme.effects, 1);
});

test('parseAspPack rejects non-ASP data and broken payload bounds', t => {
  const { context } = fixture(t);
  assert.throws(() => context.parseAspPack(new Uint8Array(32)), /заголовок/);
  const pcm = Buffer.from([1, 0, 2, 0]);
  const bytes = buildAsp([{ name: 'Horn', vol: 100, ch: 1, bits: 16, rate: 22050, data: pcm }]);
  const trimmed = new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.length - 1);
  assert.throws(() => context.parseAspPack(trimmed), /границы/);
  const padded = new Uint8Array(bytes.length + 2);
  padded.set(bytes);
  assert.throws(() => context.parseAspPack(padded), /размер/);
});

test('schemeBytesForPack rebuilds the asset manifest for the canonical unpacked WAV', t => {
  const { context } = fixture(t);
  const payload = Buffer.from([1, 0, 2, 0]);
  const adpcm = Buffer.alloc(512, 7);
  const project = {
    format: 'sound-graph', id: 'loco', name: 'TEPLOVOZ',
    states: [{ id: 'a', file: 'Horn.wav' }, { id: 'b', file: 'Brake.wav' }], transitions: [], effects: []
  };
  const raw = context.schemeBytesForPack(project, [
    { name: 'Horn', vol: 100, ch: 1, bits: 16, rate: 22050, data: new Uint8Array(payload) },
    { name: 'Brake', vol: 80, ch: 1, bits: 4, rate: 16000, data: new Uint8Array(adpcm) }
  ]);
  const doc = JSON.parse(Buffer.from(raw).toString('utf8'));
  const hdr = Buffer.from(context.packWavHeader(payload.length, 22050, 1, 16));
  assert.equal(hdr.length, 44);
  const crc = (zlib.crc32(Buffer.concat([hdr, payload])) >>> 0).toString(16).padStart(8, '0');
  const adpcmHdr = Buffer.from(context.packWavHeader(adpcm.length, 16000, 1, 4));
  assert.equal(adpcmHdr.length, 60);
  const adpcmCrc = (zlib.crc32(Buffer.concat([adpcmHdr, adpcm])) >>> 0).toString(16).padStart(8, '0');
  assert.deepEqual(doc.assets, [
    { file: 'Horn.wav', size: 48, sampleRate: 22050, channels: 1, bits: 16, durationMs: 1, crc32: crc },
    { file: 'Brake.wav', size: 572, sampleRate: 16000, channels: 1, bits: 4, durationMs: 63, crc32: adpcmCrc }
  ]);
  assert.throws(() => context.schemeBytesForPack(project, []), /отсутствующий звук/);
});

test('packFileBase keeps the locomotive name and strips unsafe filename characters', t => {
  const { context } = fixture(t);
  assert.equal(context.packFileBase('ТЕПЛОВОЗ'), 'ТЕПЛОВОЗ');
  assert.equal(context.packFileBase('Тепловоз ЧМЭ3'), 'Тепловоз ЧМЭ3');
  assert.equal(context.packFileBase('  ТЕПЛОВОЗ / #1  '), 'ТЕПЛОВОЗ_#1');
  assert.equal(context.packFileBase(''), 'sound_pack');
});

test('exportSoundPack builds a validated pack named after the locomotive', async t => {
  const { context } = fixture(t);
  const logs = [];
  context.log = (title, msg) => logs.push(msg);
  context.showSetting = () => {};
  context.loadLibrary = async () => {};
  context.promptPasswordDialog = async () => '';
  let exportStats = null;
  context.showSavePackDialog = async (name, size, downloadInfo, tooBig, repairs) => {
    exportStats = { name, size, download: downloadInfo, tooBig, repairs };
    if (downloadInfo) download = downloadInfo.name;
    return true;
  };
  context.libFiles = [{ name: 'Horn', size: 4, vol: 55 }];
  context.api = async url => {
    if (url === '/api/device') return { ok: true, name: 'ТЕПЛОВОЗ / 1' };
    if (url === '/api/sound/graph/state') return { ok: true, id: 'loco', revision: 2 };
    if (url.indexOf('/api/sound/graph/project') === 0)
      return { ok: true, project: { format: 'sound-graph', id: 'loco', name: 'TEPLOVOZ',
        states: [{ id: 's', file: 'Horn.wav' }], transitions: [], effects: [] } };
    if (url.indexOf('/api/func-map') === 0) return { ok: true, binds: [] };
    if (url.indexOf('/api/aux/cfg') === 0) return { ok: true, aux: {} };
    throw new Error('unexpected ' + url);
  };
  const pcm = new Uint8Array([1, 0, 2, 0]);
  context.fetch = async () => ({ ok: true, arrayBuffer: async () => pcm.buffer,
    headers: { get: n => ({ 'X-Audio-Rate': '22050', 'X-Audio-Channels': '1', 'X-Audio-Bits': '16' })[n] } });
  let captured = null;
  context.Blob = class { constructor(parts) { this.bytes = Buffer.from(parts[0]); captured = this.bytes; } };
  context.URL = { createObjectURL: () => 'blob:x', revokeObjectURL: () => {} };
  context.setTimeout = () => {};
  let download = '';
  context.document.createElement = () => {
    const a = { href: '', click: () => {} };
    Object.defineProperty(a, 'download', { get: () => download, set: v => { download = v; } });
    return a;
  };
  await context.exportSoundPack();
  assert.ok(captured, 'a downloadable pack is produced: ' + logs.join(' | '));
  const info = context.parseAspPack(new Uint8Array(captured));
  assert.equal(info.audio.length, 1);
  assert.equal(info.audio[0].name, 'Horn');
  assert.equal(info.audio[0].vol, 55);
  assert.equal(info.scheme.id, 'loco');
  assert.equal(download, 'TEPLOVOZ.asp');
  assert.ok(exportStats, 'save dialog is shown');
  assert.equal(exportStats.name, 'TEPLOVOZ.asp');
  assert.equal(exportStats.download.name, 'TEPLOVOZ.asp');
  assert.match(exportStats.download.url, /^blob:/);
  assert.equal(info.audio.length, 1);
  assert.equal(info.adpcm, 1, 'PCM is compressed to ADPCM on export');
});

test('OTA timeout and abort release upload state', t => {
  const { context, xhrs } = fixture(t);
  context.startHeartbeat = context.refreshStatus = () => {};
  context.uploadOta({ files: [{ name: 'test.bin', size: 1000 }], value: 'test.bin' });
  assert.equal(context.uploadBusy, true);
  assert.equal(xhrs[0].timeout, 300000);
  xhrs[0].onabort();
  assert.equal(context.uploadBusy, false);
});

test('changed AP address is probed without CORS before navigating', async t => {
  const { context, requests } = fixture(t);
  context.fetch = async (url, options) => {
    requests.push({ url, options }); return { ok: false, type: 'opaque' };
  };
  context.reloadAfterReboot(0, '192.168.101.1');
  await wait(20);
  assert.equal(requests.length, 1);
  assert.equal(requests[0].options.mode, 'no-cors');
  assert.equal(context.location.href, 'http://192.168.101.1/');
});

test('CV filtering hides legacy CVs and collapses speed table CV67..94 until toggled or searched', t => {
  const { context, element } = fixture(t);
  context.renderCvList();
  const initialRows = context.filteredCv();
  for (const index of [12, 15, 16, 21, 23, 28, 66, 95, 105, 106]) {
    assert.equal(initialRows.some(c => c.index === index), true,
      `standard CV ${index} is visible`);
  }
  assert.equal(initialRows.some(c => c.index === 30 || c.index === 114), false, 'Legacy CVs hidden by default');
  assert.equal(initialRows.some(c => c.index >= 67 && c.index <= 94), false, 'Speed table CVs collapsed by default');

  context.toggleSpeedTableCv();
  assert.equal(context.filteredCv().some(c => c.index === 67), true, 'Speed table CVs shown when toggled');

  element('cv_search').value = '114';
  context.onCvSearch();
  assert.equal(context.filteredCv().length, 1);
  assert.equal(context.filteredCv()[0].index, 114, 'Search finds legacy CV 114');
});

