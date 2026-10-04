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
    'loadDevice', 'loadWifi', 'loadVolumes', 'loadTrackList', 'refreshStatus',
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
