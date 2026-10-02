const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { test } = require('node:test');

const html = fs.readFileSync(path.join(__dirname, '..', 'web_ui.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1]
  .replace(/\binit\(\);\s*$/, '');
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const response = data => ({ ok: true, status: 200,
  headers: new Headers({ 'content-type': 'application/json' }),
  json: async () => data });

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
    console, AbortController, Headers, URL, Date, Promise,
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

test('scheme deletion waits for confirmation and cancellation sends nothing', async t => {
  const { context, element, requests } = fixture(t);
  element('snd_proj').value = 'existing';
  const deletion = context.deleteScheme();
  await wait(0);
  assert.equal(requests.length, 0);
  context.dlgAnswer(false);
  await deletion;
  assert.equal(requests.length, 0);
});

test('scheme overwrite waits for confirmation', async t => {
  const { context, element, requests, xhrs } = fixture(t);
  element('snd_file').files = [{ name: 'existing.mds', size: 100 }];
  context.sndProjects = [{ name: 'existing' }];
  const upload = context.uploadScheme();
  await wait(0);
  assert.equal(requests.length, 0);
  assert.equal(xhrs.length, 0);
  context.dlgAnswer(false);
  await upload;
  assert.equal(context.uploadBusy, false);
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

test('visibility does not restart normal polling during upload', t => {
  const { context, events, intervals } = fixture(t);
  context.uploadBusy = true;
  events.visibilitychange();
  assert.equal(context.statusTimer, null);
  assert.equal(context.heartbeatTimer, null);
  assert.equal(intervals.size, 1);
  assert.equal(intervals.get(context.upPollTimer).ms, 400);
});

test('WAV timeout and abort release upload state', t => {
  const { context, xhrs } = fixture(t);
  context.startHeartbeat = context.refreshStatus = () => {};
  const input = { files: [{ name: 'test.wav', size: 1000 }], value: 'test.wav' };
  context.uploadTrack(1, input);
  assert.equal(context.uploadBusy, true);
  assert.equal(xhrs[0].timeout, 300000);
  assert.equal(xhrs[0].ontimeout, xhrs[0].onabort);
  xhrs[0].ontimeout();
  assert.equal(context.uploadBusy, false);
  assert.equal(context.upPollTimer, null);
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
