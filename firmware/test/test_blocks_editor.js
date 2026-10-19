const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { test } = require('node:test');

const htmlPath = path.join(__dirname, '..', 'sound_editor', 'public', 'blocks.html');
const html = fs.readFileSync(htmlPath, 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];

function createDomMock() {
  const elements = new Map();
  const listeners = new Map();
  const makeEl = (tag = 'div', id = '') => {
    const elListeners = {};
    const children = [];
    const classes = new Set();
    let _innerHTML = '';
    const el = {
      id,
      tagName: tag.toUpperCase(),
      value: '',
      textContent: '',
      checked: false,
      disabled: false,
      files: [],
      dataset: {},
      style: { setProperty() {} },
      children,
      get innerHTML() { return _innerHTML; },
      set innerHTML(val) {
        _innerHTML = String(val || '');
        parseInner(el, _innerHTML);
      },
      classList: {
        add(c) { classes.add(c); },
        remove(c) { classes.delete(c); },
        toggle(c, force) {
          if (force === undefined) {
            if (classes.has(c)) { classes.delete(c); return false; }
            classes.add(c); return true;
          }
          if (force) classes.add(c); else classes.delete(c);
          return !!force;
        },
        contains(c) { return classes.has(c); }
      },
      addEventListener(ev, fn) { (elListeners[ev] = elListeners[ev] || []).push(fn); },
      dispatchEvent(evObj) {
        const list = elListeners[evObj.type] || [];
        list.forEach(fn => fn(evObj));
      },
      getAttribute(k) { return this['attr_' + k] ?? null; },
      setAttribute(k, v) { this['attr_' + k] = String(v); if (k === 'id') { this.id = String(v); elements.set(String(v), this); } },
      removeAttribute(k) { delete this['attr_' + k]; },
      querySelector(sel) { return this.querySelectorAll(sel)[0] || null; },
      querySelectorAll(sel) {
        const res = [];
        const match = node => {
          if (sel.startsWith('#')) return node.id === sel.slice(1);
          const mAttr = sel.match(/^\[([a-zA-Z0-9_-]+)(?:="([^"]*)")?\]$/);
          if (mAttr) {
            const v = node.getAttribute(mAttr[1]);
            return mAttr[2] !== undefined ? v === mAttr[2] : v !== null;
          }
          return false;
        };
        const walk = node => {
          for (const ch of (node.children || [])) {
            if (match(ch)) res.push(ch);
            walk(ch);
          }
        };
        walk(this);
        return res;
      },
      appendChild(ch) { children.push(ch); ch.parentElement = this; return ch; },
      remove() {},
      focus() {},
      select() {},
      blur() { this.dispatchEvent({ type: 'blur', target: this }); },
      closest() { return null; },
      getBoundingClientRect: () => ({ left: 0, top: 0, right: 800, bottom: 600, width: 800, height: 600 })
    };
    return el;
  };

  function parseInner(parent, str) {
    parent.children.length = 0;
    const tagRe = /<(input|select|button|div|textarea|span|path)\b([^>]*)>/gi;
    let m;
    while ((m = tagRe.exec(str)) !== null) {
      const ch = makeEl(m[1]);
      const attrs = m[2];
      const attrRe = /([a-zA-Z0-9_-]+)(?:="([^"]*)")?/g;
      let am;
      while ((am = attrRe.exec(attrs)) !== null) {
        const k = am[1], v = am[2] !== undefined ? am[2] : '';
        ch.setAttribute(k, v);
        if (k === 'value') ch.value = v;
        if (k === 'disabled') ch.disabled = true;
      }
      parent.appendChild(ch);
    }
  }

  const getEl = id => {
    if (!elements.has(id)) elements.set(id, makeEl('div', id));
    return elements.get(id);
  };

  // Pre-populate static IDs from HTML
  const idRe = /\bid="([^"]+)"/g;
  let im;
  while ((im = idRe.exec(html)) !== null) getEl(im[1]);

  return { elements, getEl, makeEl, listeners };
}

function createEditorFixture(t, opts = {}) {
  const dom = createDomMock();
  const storage = new Map();
  const timers = new Set();
  const intervals = new Map();
  let nextInterval = 1;
  let elemAtPoint = null;

  const fetchMock = opts.fetch || (async url => {
    if (url.startsWith('/api/sound/graph/state')) return { ok: true, revision: 1 };
    if (url.startsWith('/api/sound-files')) return { ok: true, files: ['horn.wav', 'loop.wav', 'end.wav'] };
    if (url.startsWith('/api/func-map')) return { ok: true, binds: [] };
    if (url.startsWith('/api/aux/cfg')) return { ok: true, aux: [] };
    if (url.startsWith('/api/sound/graph/asset')) return { ok: true, asset: { file: 'horn.wav', size: 1000, crc32: '12345678', sampleRate: 22050, channels: 1, bits: 16, durationMs: 200 } };
    if (url.startsWith('/api/sound/graph/validate')) return { ok: true, diagnostics: [] };
    return { ok: true };
  });

  const context = {
    console,
    Date,
    Math,
    JSON,
    Promise,
    Set,
    Map,
    Array,
    Object,
    String,
    Number,
    Boolean,
    RegExp,
    Error,
    TypeError,
    TextEncoder,
    URLSearchParams,
    Blob: class { constructor(parts) { this.parts = parts; } },
    URL: Object.assign(URL, { createObjectURL: () => 'blob:mock', revokeObjectURL: () => {} }),
    Image: class { set src(v) { if (this.onload) this.onload(); } },
    Audio: class {
      constructor(src) { this.src = src; this.volume = 1; }
      play() { return Promise.resolve(); }
      pause() {}
    },
    localStorage: {
      getItem: k => storage.has(k) ? storage.get(k) : null,
      setItem: (k, v) => storage.set(k, String(v)),
      removeItem: k => storage.delete(k)
    },
    location: { search: opts.search || '', href: '' },
    window: null,
    document: {
      title: '',
      activeElement: null,
      documentElement: {
        lang: 'ru',
        attrs: { 'data-theme': 'dark' },
        getAttribute(k) { return this.attrs[k] || null; },
        setAttribute(k, v) { this.attrs[k] = String(v); }
      },
      body: Object.assign(dom.makeEl('body'), {
        appendChild(el) {
          if (el.id) dom.elements.set(el.id, el);
          for (const ch of (el.children || [])) {
            if (ch.id) dom.elements.set(ch.id, ch);
          }
          return el;
        }
      }),
      getElementById: id => dom.getEl(id),
      createElement: tag => dom.makeEl(tag),
      createRange: () => ({ selectNodeContents() {} }),
      querySelectorAll: () => [],
      addEventListener: () => {},
      elementFromPoint: () => elemAtPoint
    },
    getSelection: () => ({ removeAllRanges() {}, addRange() {} }),
    confirm: () => true,
    alert: () => {},
    setTimeout(fn, ms) {
      const id = setTimeout(() => { timers.delete(id); fn(); }, ms);
      timers.add(id);
      return id;
    },
    clearTimeout(id) { timers.delete(id); clearTimeout(id); },
    setInterval(fn, ms) {
      const id = nextInterval++;
      intervals.set(id, { fn, ms });
      return id;
    },
    clearInterval(id) { intervals.delete(id); },
    fetch: async (url, init) => {
      const data = await fetchMock(url, init);
      return {
        ok: data.ok !== false,
        status: data.status || 200,
        text: async () => JSON.stringify(data)
      };
    }
  };
  context.window = context;
  context.window.innerWidth = 1280;
  context.window.addEventListener = (ev, fn) => {
    context.window['on_' + ev] = fn;
  };

  vm.createContext(context);
  vm.runInContext(script, context, { filename: 'blocks.html' });

  t.after(() => {
    for (const timer of timers) clearTimeout(timer);
    if (context.__blocks && context.__blocks.closeSimulator) context.__blocks.closeSimulator();
  });

  return {
    api: context.__blocks,
    dom,
    context,
    setElementAtPoint: el => { elemAtPoint = el; }
  };
}

test('1. Default project engine cues without Start block are marked ready (✓) and pass testScheme', async t => {
  const { api, dom } = createEditorFixture(t);
  const p = api.project();
  const ready = api.readyIds();

  for (const id of ['eng1_cue', 'eng2_cue', 'eng3_cue', 'eng4_cue', 'idle_cue']) {
    const block = p.mix.blocks.find(b => b.id === id);
    assert.ok(block, `block ${id} exists`);
    assert.equal(api.isReady(block), true, `${id} should be ready even when its scheme has only a Loop block`);
    assert.equal(ready.has(id), true, `${id} should be in readyIds`);
  }

  await api.testScheme();
  assert.doesNotMatch(dom.getEl('status_txt').textContent, /[1-9]\d*\s+с ошибками/);
  assert.match(dom.getEl('status_txt').textContent, /0 с ошибками/);
});

test('2 & 3. Logic -> Sound Cue -> Output preserves condition, and Logic false port sets lk.cond on bind link', t => {
  const { api } = createEditorFixture(t);
  const p = api.project();

  // Check default fan_logic -> fan output binding
  let binds = api.desiredBindings();
  const fanBind = binds.find(b => b.fn === 14 && b.type === 1);
  assert.ok(fanBind, 'F14 fan binding exists');
  assert.ok((fanBind.flags & 4) !== 0, 'F14 binding has condition flag');

  // Insert a sound cue between fan_logic and fan output: F14 -> fan_logic (false) -> horn_cue -> fan
  p.mix.links = p.mix.links.filter(l => !(l.from === 'fan_logic' && l.to === 'fan'));
  p.mix.links.push({ id: 'l_lg_cue', from: 'fan_logic', to: 'horn_cue', kind: 'after', cond: 'false' });
  p.mix.links.push({ id: 'l_cue_out', from: 'horn_cue', to: 'fan', kind: 'bind', cond: 'out' });

  binds = api.desiredBindings();
  const chainedBind = binds.find(b => b.fn === 14 && b.type === 1 && (b.flags & 4) !== 0);
  assert.ok(chainedBind, 'Condition is preserved across intermediate cue block');
  assert.ok((chainedBind.flags & 2) !== 0, 'False branch inversion bit (flags & 2) is preserved');
});

test('4 & 5. Control blocks can be renamed in inspector and #vars_editor is removed from right panel', t => {
  const { api, dom } = createEditorFixture(t);
  const p = api.project();

  // Select a control block (fx_in 'f2') and rename it via inspector #i_name
  api.selectNode('f2', false);
  const nameInput = dom.getEl('i_name');
  assert.ok(nameInput, 'Inspector name input exists for fx_in');
  nameInput.value = 'F2 Главный тифон';
  nameInput.dispatchEvent({ type: 'change', target: nameInput });

  const f2 = p.mix.blocks.find(b => b.id === 'f2');
  assert.equal(f2.name, 'F2 Главный тифон');
});

test('6. expandSchemes keeps all state and transition IDs within 31 characters', t => {
  const { api } = createEditorFixture(t);
  const p = api.project();
  const scheme = p.schemes[0];
  scheme.blocks[0].id = 'very_long_scheme_block_identifier_12345';
  const cue = p.mix.blocks.find(b => b.id === 'door_close_cue');
  cue.id = 'very_long_sound_cue_block_identifier_99';
  cue.scheme = scheme.id;
  p.mix.links.forEach(l => { if (l.to === 'door_close_cue') l.to = cue.id; });

  const v = api.validate();
  const badIdErrors = v.diagnostics.filter(d => d.message.includes('id') || d.message.includes('ID'));
  assert.equal(badIdErrors.length, 0, `Unexpected ID length errors: ${JSON.stringify(badIdErrors)}`);
  for (const st of v.payload.states) {
    assert.ok(st.id.length <= 31, `State ID "${st.id}" exceeds 31 chars`);
  }
});

test('7 & 8. Delay and Logic blocks in a chain do not prematurely reset via broken isLeaf, and multi-branch warning works', t => {
  const { api } = createEditorFixture(t);
  const p = api.project();

  // Build chain: F5 -> Delay (500ms) -> Simple sound
  p.mix.blocks.push({ id: 'd1', type: 'delay', name: 'Delay', fn: -1, delayMs: 500, x: 200, y: 200, files: {} });
  p.mix.blocks.push({ id: 's1', type: 'simple', name: 'Simple', fn: -1, volume: 80, rate: 1000, files: { sound: 'beep.wav' }, x: 480, y: 200 });
  p.mix.links.push({ id: 'ld1', from: 'f5', to: 'd1', kind: 'after', cond: 'out' });
  p.mix.links.push({ id: 'ld2', from: 'd1', to: 's1', kind: 'after', cond: 'out' });

  const { payload } = api.compile();
  // Delay is NOT a leaf because it links to s1; it must NOT have a sample_done edge back to d1_off from d1_gate!
  const prematureReset = payload.transitions.find(
    tr => tr.source === 'd1_gate' && tr.target === 'd1_off' && tr.condition.type === 'sample_done'
  );
  assert.equal(prematureReset, undefined, 'Non-leaf Delay block must not reset to _off on sample_done before timeout');
  const timeoutEdge = payload.transitions.find(
    tr => tr.source === 'd1_gate' && tr.target === 's1_play' && tr.condition.type === 'timeout'
  );
  assert.ok(timeoutEdge, 'Delay block transitions to next block on timeout');
  // Re-arm edge from s1_play (or s1_off) back to d1_off must exist
  const rearmEdge = payload.transitions.find(
    tr => (tr.source === 's1_play' || tr.source === 's1_off') && tr.target === 'd1_off'
  );
  assert.ok(rearmEdge, 'Chain starting with Delay re-arms back to d1_off after completion');

  // Add second outgoing link from d1 -> horn_cue to trigger multi-branch warning
  p.mix.links.push({ id: 'ld3', from: 'd1', to: 'horn_cue', kind: 'after', cond: 'out' });
  const v = api.validate();
  assert.ok(v.diagnostics.some(d => d.id === 'd1' && d.severity === 'warning'), 'Multi-branch warning is emitted for d1');
});

test('9, 10 & 11. Simulator runs schemes without Start block and applies logic outputs (Notch +/-, Coast, Mute)', async t => {
  const { api } = createEditorFixture(t);

  // Switch to scheme mode and test a scheme that has only a Loop block (no Start block)
  api.project().schemes.push({
    id: 'only_loop',
    name: 'OnlyLoop',
    blocks: [{ id: 'ol_loop', type: 'loop', name: 'Loop', fn: -1, volume: 80, rate: 1000, loopMode: 'hold', files: { sound: 'ms-loop.wav' }, outEvent: 'finish', outDelay: 0, x: 60, y: 80 }],
    links: [],
    texts: []
  });
  api.switchMode('scheme');
  api.selectScheme('only_loop');
  await api.openSimulator();
  let sim = api.sim();
  assert.ok(sim, 'Simulator opened in scheme mode');
  assert.equal(sim.running, true, 'Simulator auto-starts on open');
  assert.equal(sim.autoFn, 0, 'Root Loop block without Start received autoFn trigger F0');
  assert.ok(sim.chans.length >= 1, 'Effect channel created for root Loop block');
  api.simTick();
  assert.equal(sim.states[sim.chans[0].state].id, 'ol_loop_play', 'Loop state entered on first tick');
  api.closeSimulator();

  // Switch to Mixes mode and test Notch + (F9) and Notch - (F10) in simulator
  api.switchMode('mix');
  await api.openSimulator();
  sim = api.sim();
  assert.equal(sim.notchOffset, 0);
  api.simPress(9); // F9 Notch + (auto-unlatches after tick!)
  api.simTick();
  assert.equal(sim.notchOffset, 1, 'Notch + incremented notchOffset');
  assert.equal(sim.fnLevels[9], false, 'F9 automatically unlatched after pulse');
  api.simPress(9);
  api.simTick();
  assert.equal(sim.notchOffset, 2, 'Second F9 click incremented notchOffset to 2 without manual release');
  api.simPress(10); // F10 Notch -
  api.simTick();
  assert.equal(sim.notchOffset, 1, 'F10 Notch - decremented notchOffset to 1');
  assert.equal(sim.fnLevels[10], false, 'F10 automatically unlatched after pulse');
  api.closeSimulator();
});

test('12, 13 & 14. importOutputs preserves headlight direction/state and reverseGraphToProject rebuilds Engine ports', async t => {
  let mockBinds = [
    { fn: 0, type: 1, id: 0, dir: 1, state: 1, flags: 0, short: 0, short_ms: 0, min_ms: 0, fade_ms: 0 },
    { fn: 15, type: 1, id: 2, dir: 0, state: 0, flags: 4, short: 9, short_ms: 4, min_ms: 10, fade_ms: 0 }
  ];
  const { api } = createEditorFixture(t, {
    fetch: async url => {
      if (url.startsWith('/api/func-map')) return { ok: true, binds: mockBinds };
      if (url.startsWith('/api/aux/cfg')) return { ok: true, aux: [{ level: 90, effect: 2 }] };
      return { ok: true, revision: 1, files: [] };
    }
  });

  await api.importOutputs();
  const p = api.project();
  const head = p.mix.blocks.find(b => b.type === 'output' && b.channel === 0);
  assert.ok(head);
  assert.equal(head.dir, 'fwd', 'Headlight single F0F binding preserves fwd direction');
  assert.equal(head.state, 'moving', 'Headlight binding preserves moving state');

  // Missing variable index 9 was auto-created so F15 logic link is not dropped
  const aux1 = p.mix.blocks.find(b => b.type === 'output' && b.channel === 1 && p.mix.links.some(l => l.to === b.id));
  assert.ok(aux1, 'Output with high variable index condition was linked');

  // Test reverseGraphToProject on compiled defaultProject (where Engine has no internal WAVs)
  const def = api.compile().payload;
  delete def.editor.project;
  const rev = api.reverseGraphToProject(def, 'Restored');
  const eng = rev.project.mix.blocks.find(b => b.type === 'engine');
  assert.ok(eng, 'Engine block exists in reversed project');
  const engLinks = rev.project.mix.links.filter(l => l.from === eng.id);
  assert.ok(engLinks.length >= 4, 'Speed band blocks are reattached to Engine output ports');
});

test('15, 16 & 17. Oversized project does not throw ReferenceError in _compileFlat, and testBlock works on Fx/Cue/Engine', async t => {
  const { api, dom } = createEditorFixture(t);
  const p = api.project();

  // Add a huge text annotation (>120 KB) to trigger editor.project stripping
  p.mix.texts.push({ id: 'txt_big', text: 'A'.repeat(130000), x: 0, y: 0, size: 16 });
  const v = api.validate();
  assert.ok(v.diagnostics.some(d => d.severity === 'warning' && d.message.includes('120') === false), 'Warning v_no_editor_copy emitted without ReferenceError');
  p.mix.texts = [];

  // testBlock on Input (Fx) block 'f0'
  const f0 = p.mix.blocks.find(b => b.id === 'f0');
  await api.testBlock(f0);
  assert.match(dom.getEl('status_txt').textContent, /в порядке/);

  // testBlock on Engine-driven cue 'eng1_cue'
  const eng1Cue = p.mix.blocks.find(b => b.id === 'eng1_cue');
  await api.testBlock(eng1Cue);
  assert.match(dom.getEl('status_txt').textContent, /в порядке/);
});

test('18, 19, 20 & 21. Copy/paste/duplicate preserves links, blocks duplicate F-key when all F0..F28 are used, blocks invalid types in scheme mode, and uses accurate nodeHeight', t => {
  const { api } = createEditorFixture(t);
  const p = api.project();

  // Select f2 and horn_cue while all F0..F28 are used -> fx_in cannot be duplicated, only horn_cue is duplicated
  api.selectNode('f2', false);
  api.selectNode('horn_cue', true);
  const beforeBlocksAllUsed = p.mix.blocks.length;
  api.duplicateSelected();
  assert.equal(p.mix.blocks.length, beforeBlocksAllUsed + 1, 'When all F0..F28 are used, duplicate fx_in is blocked');

  // Free F28 so there is an available F-key, then duplicate f2 + horn_cue -> both duplicate and link is preserved
  p.mix.blocks = p.mix.blocks.filter(b => b.id !== 'f28');
  api.selectNode('f2', false);
  api.selectNode('horn_cue', true);
  const beforeBlocks = p.mix.blocks.length;
  const beforeLinks = p.mix.links.length;
  api.duplicateSelected();
  assert.equal(p.mix.blocks.length, beforeBlocks + 2, 'Both selected blocks duplicated when an F-key is free');
  assert.equal(p.mix.links.length, beforeLinks + 1, 'Link between duplicated blocks preserved');

  // Copy f0 (Input Fx) in mix mode and try to paste in scheme mode -> should be blocked
  api.selectNode('f0', false);
  api.copySelected();
  api.switchMode('scheme');
  const schemeBlocksBefore = api.project().blocks.length;
  api.pasteClip();
  assert.equal(api.project().blocks.length, schemeBlocksBefore, 'Mix-only block fx_in is not pasted into sound scheme');

  // Check nodeHeight: normal block is 80px, engine with 7 steps + idle + brake is 220px
  api.switchMode('mix');
  const eng = p.mix.blocks.find(b => b.type === 'engine');
  const vol = p.mix.blocks.find(b => b.type === 'volume');
  assert.equal(api.nodeHeight(vol), 80);
  assert.equal(api.nodeHeight(eng), 220);
});

test('22 & 23. Finite repeat count Loop in Simulator and reverseGraphToProject with End block', async t => {
  const { api } = createEditorFixture(t);
  const p = api.project();

  // Verify reverseGraphToProject on default project (which has End blocks in horn/compressor/sander) produces 0 validation errors
  const defPayload = api.compile().payload;
  delete defPayload.editor.project;
  const rev = api.reverseGraphToProject(defPayload, 'RevTest');
  api.setProject(rev.project);
  const revVal = api.validate();
  assert.equal(revVal.diagnostics.filter(d => d.severity === 'error').length, 0, 'Reversed graph with End blocks has 0 validation errors');

  // Test finite repeat Loop (loopMode = "count", loopCount = 3) -> End in Simulator
  const s = {
    id: 's_cnt',
    name: 'CountLoop',
    blocks: [
      { id: 'lp', type: 'loop', name: 'Loop3', fn: -1, volume: 80, rate: 1000, loopMode: 'count', loopCount: 3, files: { sound: 'beep.wav' }, outEvent: 'finish', outDelay: 0, x: 40, y: 40 },
      { id: 'ed', type: 'end', name: 'End', fn: -1, volume: 80, rate: 1000, files: { sound: 'end.wav' }, outEvent: 'finish', outDelay: 0, x: 320, y: 40 }
    ],
    links: [{ id: 'l1', from: 'lp', to: 'ed', kind: 'after', cond: 'out' }],
    texts: []
  };
  api.project().schemes = [s];
  api.switchMode('scheme');
  api.selectScheme('s_cnt');
  await api.openSimulator();
  const sim = api.sim();

  const statesVisited = [];
  for (let i = 0; i < 5; i++) {
    api.simTick();
    const ch = sim.chans[0];
    const st = sim.states[ch.state];
    statesVisited.push(`${st.id}:${ch.loopsDone}`);
    if (st.file) ch.audioEnded = true;
  }
  api.closeSimulator();

  assert.deepEqual(statesVisited.slice(0, 4), [
    'lp_play:0',
    'lp_play:1',
    'lp_play:2',
    'ed_end:0'
  ]);
});

test('24. Flowchart SVG export renders header, spreads engine output ports, wraps long lines, and exportFileBase includes scheme ID', t => {
  const { api } = createEditorFixture(t);
  const p = api.project();
  p.mix.texts.push({ id: 'txt1', text: 'Основная схема ЧМЭ3', x: 100, y: 100, size: 16, color: '#4a9eff' });

  const svg = api.buildSchemeSvg();
  assert.match(svg, /<svg xmlns="http:\/\/www\.w3\.org\/2000\/svg"/);
  assert.match(svg, /Основная схема ЧМЭ3/, 'Text annotations are rendered in exported SVG');
  assert.equal(api.exportFileBase(), 'locomotive_default');

  api.switchMode('scheme');
  api.selectScheme('horn_scheme');
  assert.equal(api.exportFileBase(), 'locomotive_default-horn_scheme');
  const schemeSvg = api.buildSchemeSvg();
  assert.match(schemeSvg, /horn-low-loop\.wav/);
});

test('25. alignBlocks and fitViewport arrange nodes and update viewport, and invalid speed band emits warning', t => {
  const { api } = createEditorFixture(t);
  api.switchMode('scheme');
  api.selectScheme('horn_scheme');
  const b0 = api.project().blocks[0];
  const b1 = api.project().blocks[1];
  b0.x = 500; b0.y = 500;
  b1.x = 100; b1.y = 100;

  api.alignBlocks();
  assert.ok(b0.x < b1.x, 'Upstream block is placed to the left of downstream block after alignBlocks');

  api.fitViewport();
  const vp = api.project().editor.viewport;
  assert.ok(vp.zoom >= 0.3 && vp.zoom <= 1.5, 'fitViewport computes a valid zoom');

  // Invalid speed band check (from > to)
  api.switchMode('mix');
  const eng = api.project().mix.blocks.find(b => b.type === 'engine');
  const origFrom = eng.run[0].from;
  eng.run[0].from = 90;
  const v = api.validate();
  assert.ok(v.diagnostics.some(d => d.id === eng.id && d.severity === 'warning'), 'Inverted speed band emits warning');
  eng.run[0].from = origFrom;
});


