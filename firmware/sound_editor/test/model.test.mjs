import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import { CONDITIONS, DEFAULT_LIMITS, clone, deleteState, dieselTemplate, emptyProject, parseProject, positionOf, setPosition, validateProject } from '../src/model.ts';

const verdicts = JSON.parse(await readFile(new URL('./fixtures/verdicts.json', import.meta.url), 'utf8'));
for (const fixture of verdicts) test(`C/TS fixture: ${fixture.file} = ${fixture.valid}`, async () => {
  const text = await readFile(new URL(`./fixtures/${fixture.file}`, import.meta.url), 'utf8');
  let valid;
  try { valid = !validateProject(parseProject(text)).some(error => error.severity !== 'warning'); } catch { valid = false; }
  assert.equal(valid, fixture.valid);
});

test('diesel/held-horn template is a valid draft, missing WAVs block Apply', () => {
  const graph = dieselTemplate();
  assert.deepEqual(validateProject(graph).filter(error => error.severity !== 'warning'), []);
  assert.equal(graph.effects[0].fn, 2);
  assert.ok(validateProject(graph, DEFAULT_LIMITS, true).some(error => error.code === 'asset'));
  for (const file of new Set(graph.states.map(state => state.file).filter(Boolean))) graph.assets.push({ file, size: 44144, crc32: '12345678', sampleRate: 22050, channels: 1, bits: 16, durationMs: 1000 });
  assert.deepEqual(validateProject(graph, DEFAULT_LIMITS, true), []);
  assert.ok(graph.transitions.some(edge => edge.source === 'horn_start' && edge.condition.type === 'fn_off'));
});

test('typed condition ranges and all F0..F28 values', () => {
  for (const type of CONDITIONS) for (const fn of [0, 28]) {
    const graph = emptyProject();
    graph.transitions.push({ id: 'edge', source: 'off', target: 'off', priority: 0, timing: 'after_sample', condition: type.startsWith('fn_') ? { type, fn } : ['speed', 'accel', 'decel'].includes(type) ? { type, min: 0, max: 255 } : { type } });
    assert.deepEqual(validateProject(graph).filter(error => error.severity !== 'warning'), []);
  }
  const graph = dieselTemplate(); graph.transitions[0].condition = { type: 'speed', min: 250, max: 1 };
  assert.ok(validateProject(graph).some(error => error.code === 'range'));
  graph.transitions[0].condition = { type: 'fn_on', fn: 29 };
  assert.ok(validateProject(graph).some(error => error.code === 'schema'));
});

test('bounded import rejects malformed fields, unsafe paths and duplicate JSON keys without throwing validator', () => {
  for (const value of [null, [], {}, { ...emptyProject(), states: [null] }, { ...emptyProject(), transitions: [null] }, { ...emptyProject(), editor: null }]) {
    assert.ok(validateProject(value).some(error => error.code === 'schema'));
    assert.throws(() => parseProject(JSON.stringify(value)));
  }
  assert.throws(() => parseProject('{"format":"sound-graph","format":"other"}'), /Дублирующийся/);
  assert.throws(() => parseProject('['.repeat(18) + '0' + ']'.repeat(18)), /вложенност/);
  assert.throws(() => parseProject(' '.repeat(131073)), /лимит/);
  assert.throws(() => parseProject(JSON.stringify({ ...emptyProject(), name: '\u0000' })));
  assert.throws(() => parseProject(JSON.stringify({ ...emptyProject(), name: '\ud800' })));
  assert.throws(() => parseProject(JSON.stringify({ ...emptyProject(), name: 'x'.repeat(64) })));
  assert.throws(() => parseProject(JSON.stringify({ ...emptyProject(), name: 'Ж'.repeat(32) })));
  const condition = emptyProject(); condition.transitions.push({ id: 'e', source: 'off', target: 'off', priority: 0, timing: 'after_sample', condition: { type: 123 } });
  assert.ok(validateProject(condition).some(error => error.code === 'schema'));
  assert.throws(() => parseProject(JSON.stringify(condition)));
});

test('JSON roundtrip preserves functional data and UI metadata; deleting entry is guarded', () => {
  const graph = dieselTemplate();
  graph.editor.viewport = { x: -123.5, y: 44.2, zoom: 0.35 };
  assert.deepEqual(parseProject(JSON.stringify(graph)), graph);
  assert.throws(() => deleteState(clone(graph), 'off'), /вход/);
  const copy = clone(graph); deleteState(copy, 'run1');
  assert.ok(!copy.states.some(state => state.id === 'run1'));
  assert.ok(!copy.transitions.some(edge => edge.source === 'run1' || edge.target === 'run1'));
  assert.equal(copy.editor.positions.run1, undefined);
});

test('IDs, priorities, references, byte names, audible/silent capacity and capabilities', () => {
  const graph = emptyProject();
  graph.states.push({ ...graph.states[0], id: 'off' });
  assert.ok(validateProject(graph).some(error => error.code === 'duplicate'));
  const limited = { ...DEFAULT_LIMITS, states: 1 };
  assert.ok(validateProject(dieselTemplate(), limited).some(error => error.code === 'schema'));
  const reference = emptyProject(); reference.engine.entry = 'missing';
  assert.ok(validateProject(reference).some(error => error.code === 'entry'));
  const json = emptyProject(); json.id = '__proto__'; json.editor.positions = JSON.parse('{"__proto__":{"x":0,"y":0}}');
  assert.deepEqual(validateProject(json).filter(error => error.severity !== 'warning'), []);
  assert.equal(positionOf(emptyProject(), '__proto__'), undefined);
  setPosition(json, '__proto__', { x: 10, y: 20 });
  assert.deepEqual(positionOf(json, '__proto__'), { x: 10, y: 20 });
  assert.equal(Object.getPrototypeOf(json.editor.positions), Object.prototype);
});
