export const DEFAULT_LIMITS = { states: 57, soundStates: 31, transitions: 128, effects: 24, effectTables: 16, blocks: 64, sinks: 9, assets: 31, jsonBytes: 131072 };
export type Limits = typeof DEFAULT_LIMITS;
export const CONDITIONS = ['fn_press', 'fn_release', 'fn_on', 'fn_off', 'engine_on', 'engine_off', 'speed', 'accel', 'decel', 'sample_done'] as const;
export type ConditionType = typeof CONDITIONS[number];
export type Condition = { type: ConditionType; fn?: number; min?: number; max?: number };
export type SoundState = { id: string; name: string; file: string; loop: boolean; volume: number; rate: number };
export type Transition = { id: string; source: string; target: string; priority: number; timing: 'immediate' | 'after_sample'; condition: Condition };
export type Effect = { id: string; entry: string; fn: number };
export type Asset = { file: string; size: number; crc32: string; sampleRate: number; channels: number; bits: number; durationMs: number };
export type Point = { x: number; y: number };
export type Viewport = Point & { zoom: number };

/* ---- v2 authoring layer: effect-table library + patch-panel routing ---- */

export const EFFECT_KINDS = ['horn', 'motor', 'brake', 'bell', 'coupler', 'custom'] as const;
export type EffectKind = typeof EFFECT_KINDS[number];
export const PRESETS = ['oneShot', 'loopHeld', 'shortLong', 'latched', 'random', 'state'] as const;
export type Preset = typeof PRESETS[number];
export type EffectTable = {
  id: string; name: string; kind: EffectKind; preset: Preset;
  init: string; loop: string; end: string; short: string;
  shortMs: number; volume: number; rate: number;
  behavior: string;
};
export const LOGIC_OPS = ['and', 'or', 'not', 'delay', 'oneShot', 'latch', 'speedRange', 'dir', 'state', 'random', 'duck'] as const;
export type LogicOp = typeof LOGIC_OPS[number];
export type SourceRole = 'fn' | 'engine' | 'motion' | 'dir' | 'state';
export type Source = { id: string; role: SourceRole; fn: number; label: string };
export type Block = { id: string; kind: 'sound' | 'logic'; table: string; op: LogicOp; min: number; max: number };
export const SINK_OUTPUTS = ['F0F', 'F0R', 'AUX1', 'AUX2', 'AUX3', 'AUX4', 'AUX5', 'AUX6', 'AUX7', 'AUDIO'] as const;
export type SinkOutput = typeof SINK_OUTPUTS[number];
export type Sink = { id: string; output: SinkOutput };
export const ROUTE_EVENTS = ['fn_press', 'fn_release', 'fn_on', 'fn_off', 'sample_done'] as const;
export type RouteEvent = '' | typeof ROUTE_EVENTS[number];
export type DirGate = 'any' | 'fwd' | 'rev';
export type StateGate = 'any' | 'moving' | 'stopped';
export type Wire = { id: string; from: string; to: string; event: RouteEvent; dir: DirGate; state: StateGate };

export type Project = {
  format: 'sound-graph'; schemaVersion: 1 | 2; id: string; name: string;
  engine: { entry: string; fn: number }; hysteresis: number;
  effectTables: EffectTable[]; sources: Source[]; blocks: Block[]; sinks: Sink[]; wires: Wire[];
  states: SoundState[]; transitions: Transition[]; effects: Effect[]; assets: Asset[];
  editor: { positions: Record<string, Point>; viewport: Viewport };
};

export type Diagnostic = { code: string; id: string; field: string; message: string; severity?: 'error' | 'warning' };
export const byteLength = (value: string) => new TextEncoder().encode(value).length;
export const validId = (id: string) => /^[A-Za-z0-9_-]{1,31}$/.test(id);
export const validFile = (file: string) => /^[A-Za-z0-9_-]+\.wav$/.test(file) && byteLength(file) <= 63;
export const clone = <T>(value: T): T => structuredClone(value);
export function positionOf(graph: Project, id: string): Point | undefined { return Object.hasOwn(graph.editor.positions, id) ? graph.editor.positions[id] : undefined; }
export function setPosition(graph: Project, id: string, point: Point): void { graph.editor.positions = { ...graph.editor.positions, [id]: point }; }
const record = (value: unknown): value is Record<string, unknown> => typeof value === 'object' && value !== null && !Array.isArray(value);
const uint = (value: unknown, max: number) => typeof value === 'number' && Number.isInteger(value) && value >= 0 && value <= max;
const text = (value: unknown, max: number) => typeof value === 'string' && byteLength(value) <= max && !/[\u0000\uD800-\uDFFF]/u.test(value.replace(/[\uD800-\uDBFF][\uDC00-\uDFFF]/g, ''));

/* Seed the effect-table library with the five starter presets. */
export function seedEffectTables(): EffectTable[] {
  const table = (id: string, name: string, kind: EffectKind, preset: Preset): EffectTable =>
    ({ id, name, kind, preset, init: '', loop: '', end: '', short: '', shortMs: 400, volume: 80, rate: 1000, behavior: '' });
  return [
    table('horn', 'Гудок', 'horn', 'shortLong'),
    table('motor', 'Мотор', 'motor', 'loopHeld'),
    table('brake', 'Тормоза', 'brake', 'state'),
    table('bell', 'Звонок', 'bell', 'random'),
    table('coupler', 'Сцепка', 'coupler', 'oneShot'),
  ];
}

export function emptyProject(id = 'new_graph'): Project {
  return { format: 'sound-graph', schemaVersion: 2, id, name: 'Новый звуковой граф', engine: { entry: 'off', fn: 8 }, hysteresis: 3,
    effectTables: seedEffectTables(), sources: [], blocks: [], sinks: [], wires: [],
    states: [{ id: 'off', name: 'Выкл / тихий вход', file: '', loop: false, volume: 100, rate: 1000 }], transitions: [], effects: [], assets: [],
    editor: { positions: { off: { x: 60, y: 80 } }, viewport: { x: 0, y: 0, zoom: 0.8 } } };
}

export function dieselTemplate(id = 'diesel_graph'): Project {
  const graph = emptyProject(id); graph.name = 'Тепловоз + удержание F2 (гудок)';
  graph.effectTables = graph.effectTables.map(t => t.id === 'motor' ? { ...t, init: 'engine_start.wav', loop: 'engine_idle.wav', end: 'engine_stop.wav' } : t.id === 'horn' ? { ...t, init: 'horn_start.wav', loop: 'horn_hold.wav', end: 'horn_end.wav', short: 'horn_short.wav' } : t);
  const add = (id: string, name: string, file: string, loop: boolean, x: number, y: number) => { graph.states.push({ id, name, file, loop, volume: 80, rate: 1000 }); graph.editor.positions[id] = { x, y }; };
  add('start', 'Запуск двигателя', 'engine_start.wav', false, 330, 80);
  add('idle', 'Холостой ход', 'engine_idle.wav', true, 600, 80);
  for (let i = 1; i <= 5; i++) add(`run${i}`, `Движение ${i}`, `engine_run${i}.wav`, true, 600 + ((i - 1) % 3) * 270, 280 + Math.floor((i - 1) / 3) * 200);
  add('shutdown', 'Остановка двигателя', 'engine_stop.wav', false, 330, 480);
  add('horn_off', 'Гудок / тихий вход', '', false, 60, 760);
  add('horn_start', 'Атака гудка', 'horn_start.wav', false, 330, 760);
  add('horn_hold', 'Удержание гудка', 'horn_hold.wav', true, 600, 760);
  add('horn_end', 'Отпускание гудка', 'horn_end.wav', false, 870, 760);
  graph.effects.push({ id: 'horn', entry: 'horn_off', fn: 2 });
  graph.effectTables = graph.effectTables.map(t => t.id === 'horn' ? { ...t, behavior: 'horn_off' } : t);
  graph.sources = [{ id: 'src_engine', role: 'engine', fn: 8, label: 'Двигатель' }, { id: 'src_horn', role: 'fn', fn: 2, label: 'F2 Гудок' }];
  graph.blocks = [{ id: 'blk_motor', kind: 'sound', table: 'motor', op: 'and', min: 0, max: 0 }, { id: 'blk_horn', kind: 'sound', table: 'horn', op: 'and', min: 0, max: 0 }];
  graph.sinks = [];
  graph.wires = [{ id: 'w_engine_motor', from: 'src_engine', to: 'blk_motor', event: 'fn_on', dir: 'any', state: 'any' }, { id: 'w_horn', from: 'src_horn', to: 'blk_horn', event: 'fn_press', dir: 'any', state: 'any' }];
  graph.editor.positions['blk_motor'] = { x: 380, y: 80 }; graph.editor.positions['blk_horn'] = { x: 380, y: 300 };
  const edge = (source: string, target: string, condition: Condition, priority = 10, timing: Transition['timing'] = 'immediate') => graph.transitions.push({ id: `t${graph.transitions.length + 1}`, source, target, condition, priority, timing });
  edge('off', 'start', { type: 'engine_on' }); edge('start', 'idle', { type: 'sample_done' }); edge('shutdown', 'off', { type: 'sample_done' });
  const modes = ['idle', 'run1', 'run2', 'run3', 'run4', 'run5'];
  for (const source of ['start', ...modes]) edge(source, 'shutdown', { type: 'engine_off' }, 255);
  for (const source of modes) for (let i = 0; i < modes.length; i++) if (source !== modes[i]) edge(source, modes[i], { type: 'speed', min: i === 0 ? 0 : 1 + (i - 1) * 51, max: i === 0 ? 0 : i * 51 }, i + 1, 'after_sample');
  edge('horn_off', 'horn_start', { type: 'fn_press', fn: 2 });
  edge('horn_start', 'horn_end', { type: 'fn_off', fn: 2 }, 200);
  edge('horn_start', 'horn_hold', { type: 'sample_done' });
  edge('horn_hold', 'horn_end', { type: 'fn_off', fn: 2 });
  edge('horn_end', 'horn_off', { type: 'sample_done' });
  return graph;
}

/* Migrate a v1 graph (effects-as-F-entry) into the v2 authoring layer. */
export function migrateV1toV2(input: Project): Project {
  if (input.schemaVersion === 2 && Array.isArray(input.effectTables)) return input;
  const graph = { ...clone(input), schemaVersion: 2 as const };
  graph.effectTables = graph.effectTables ?? [];
  graph.sources = graph.sources ?? []; graph.blocks = graph.blocks ?? []; graph.sinks = graph.sinks ?? []; graph.wires = graph.wires ?? [];
  for (const effect of graph.effects ?? []) {
    const tableId = `fx_${effect.id}`;
    const state = graph.states.find(s => s.id === effect.entry);
    graph.effectTables.push({ id: tableId, name: effect.id, kind: 'custom', preset: 'oneShot', init: state?.file ?? '', loop: '', end: '', short: '', shortMs: 400, volume: state?.volume ?? 80, rate: state?.rate ?? 1000, behavior: effect.entry });
    const sourceId = `src_${effect.id}`; const blockId = `blk_${effect.id}`;
    graph.sources.push({ id: sourceId, role: 'fn', fn: effect.fn, label: `F${effect.fn} ${effect.id}` });
    graph.blocks.push({ id: blockId, kind: 'sound', table: tableId, op: 'and', min: 0, max: 0 });
    graph.wires.push({ id: `w_${effect.id}`, from: sourceId, to: blockId, event: 'fn_press', dir: 'any', state: 'any' });
  }
  if (!graph.sources.some(s => s.role === 'engine')) graph.sources.push({ id: 'src_engine', role: 'engine', fn: graph.engine.fn, label: 'Двигатель' });
  return graph;
}

/* Expand one effect table into a behaviour subgraph rooted at its entry. */
export function expandTable(table: EffectTable, graph: Project): { entry: string; states: SoundState[]; transitions: Transition[] } {
  const entry = table.behavior || `${table.id}_off`;
  const states: SoundState[] = []; const transitions: Transition[] = [];
  const state = (id: string, name: string, file: string, loop: boolean) => { if (file) states.push({ id, name, file, loop, volume: table.volume, rate: table.rate }); };
  const edge = (source: string, target: string, condition: Condition, priority = 10, timing: Transition['timing'] = 'immediate') => transitions.push({ id: `${table.id}_${transitions.length + 1}`, source, target, condition, priority, timing });
  if (!graph.states.some(s => s.id === entry)) states.push({ id: entry, name: `${table.name} / тихий вход`, file: '', loop: false, volume: 100, rate: 1000 });
  state(`${table.id}_init`, `${table.name} init`, table.init, false);
  state(`${table.id}_loop`, `${table.name} loop`, table.loop, true);
  state(`${table.id}_end`, `${table.name} end`, table.end, false);
  state(`${table.id}_short`, `${table.name} short`, table.short, false);
  const first = table.init ? `${table.id}_init` : table.loop ? `${table.id}_loop` : table.end ? `${table.id}_end` : '';
  if (table.preset === 'shortLong' && table.short) {
    if (first) edge(entry, first, { type: 'fn_press' });
    edge(entry, `${table.id}_short`, { type: 'fn_press' }, 1);
    edge(`${table.id}_short`, entry, { type: 'sample_done' }, 1);
    edge(first || entry, `${table.id}_end`, { type: 'fn_release' }, 5);
    if (table.end) edge(`${table.id}_end`, entry, { type: 'sample_done' });
  } else if (table.preset === 'loopHeld' && table.loop) {
    edge(entry, `${table.id}_loop`, { type: 'fn_press' });
    edge(`${table.id}_loop`, entry, { type: 'fn_release' }, 5);
  } else if (first) {
    edge(entry, first, { type: 'fn_press' });
    if (table.init && table.loop) edge(`${table.id}_init`, `${table.id}_loop`, { type: 'sample_done' });
    const last = table.loop ? `${table.id}_loop` : table.end ? `${table.id}_end` : `${table.id}_init`;
    edge(last, entry, { type: 'sample_done' });
  }
  return { entry, states, transitions };
}

/* Compile the routing layer into canonical F bindings (outputs/logic/sound). */
export type CompiledBinding = { fn: number; target: 'OUTPUT' | 'SOUND' | 'LOGIC'; id: string; dir: DirGate; state: StateGate };
export function compileWires(graph: Project): CompiledBinding[] {
  const out: CompiledBinding[] = [];
  const byId = new Map(graph.sources.map(s => [s.id, s]));
  const blocks = new Map(graph.blocks.map(b => [b.id, b]));
  const sinks = new Map(graph.sinks.map(s => [s.id, s]));
  for (const wire of graph.wires) {
    const from = byId.get(wire.from); const to = blocks.get(wire.to) ?? sinks.get(wire.to);
    if (!from || !to) continue;
    if ('output' in to) { out.push({ fn: from.fn, target: 'OUTPUT', id: to.output, dir: wire.dir, state: wire.state }); continue; }
    if (to.kind === 'sound') out.push({ fn: from.fn, target: 'SOUND', id: to.table, dir: wire.dir, state: wire.state });
    else out.push({ fn: from.fn, target: 'LOGIC', id: to.op, dir: wire.dir, state: wire.state });
  }
  return out;
}

/* Strip the v2 authoring layer for the schema-v1 device payload. The device
 * runs the compiled states/transitions/effects; the library/routing stays
 * editor-side until the device schema grows to understand it. */
export function devicePayload(graph: Project): Project {
  const payload = clone(graph) as Project;
  payload.schemaVersion = 1;
  payload.effectTables = []; payload.sources = []; payload.blocks = []; payload.sinks = []; payload.wires = [];
  return payload;
}

const FN_NAMES: Record<string, string> = { fn_press: 'нажатие', fn_release: 'отпускание', fn_on: 'удержание', fn_off: 'снятие' };
export function conditionName(type: ConditionType): string {
  switch (type) {
    case 'fn_press': return 'Нажатие функции';
    case 'fn_release': return 'Отпускание функции';
    case 'fn_on': return 'Удержание функции';
    case 'fn_off': return 'Снятие функции';
    case 'engine_on': return 'Двигатель включён';
    case 'engine_off': return 'Двигатель выключен';
    case 'speed': return 'Скорость';
    case 'accel': return 'Ускорение';
    case 'decel': return 'Замедление';
    case 'sample_done': return 'Граница сэмпла';
  }
}
export function conditionLabel(c: Condition): string {
  if (c.type.startsWith('fn_')) return `F${c.fn} ${FN_NAMES[c.type]}`;
  if (['speed', 'accel', 'decel'].includes(c.type)) return `${conditionName(c.type).toLowerCase()} ${c.min ?? 0}..${c.max ?? 255}`;
  return conditionName(c.type);
}

/* Human-readable summary of an effect table's behaviour. */
export function tableSummary(t: EffectTable): string {
  const files = [t.init && 'начало', t.loop && 'тело', t.end && 'конец', t.short && 'короткий'].filter(Boolean).join(', ');
  return files || 'файлы не заданы';
}

export function validateProject(input: unknown, limits: Limits = DEFAULT_LIMITS, forApply = false): Diagnostic[] {
  const errors: Diagnostic[] = [];
  const issue = (code: string, id: string, field: string, message: string, severity: 'error' | 'warning' = 'error') => errors.push({ code, id, field, message, severity });
  const object = (value: unknown, allowed: string[], id: string, field: string): value is Record<string, unknown> => {
    if (!record(value) || Object.keys(value).some(key => !allowed.includes(key))) {
      issue('schema', id, field, `Недопустимый объект или неизвестное поле: ${field}`); return false;
    }
    return true;
  };
  if (!object(input, ['format', 'schemaVersion', 'id', 'name', 'engine', 'hysteresis', 'effectTables', 'sources', 'blocks', 'sinks', 'wires', 'states', 'transitions', 'effects', 'assets', 'editor'], '', 'project')) return errors;
  if (input.format !== 'sound-graph' || (input.schemaVersion !== 1 && input.schemaVersion !== 2)) issue('schema', '', 'format', 'Поддерживается только схема sound-graph версии 1 или 2. MDS — не JSON.');
  if (typeof input.id !== 'string' || !validId(input.id)) issue('schema', '', 'id', 'ID: 1..31 символов ASCII — буквы, цифры, подчёркивание или дефис.');
  if (!text(input.name, 63)) issue('schema', '', 'name', 'Имя должно быть корректным UTF-8, не более 63 байт.');
  if (!uint(input.hysteresis, 32)) issue('schema', '', 'hysteresis', 'Гистерезис должен быть 0..32.');
  if (object(input.engine, ['entry', 'fn'], '', 'engine')) {
    if (typeof input.engine.entry !== 'string' || !validId(input.engine.entry) || !uint(input.engine.fn, 28)) issue('schema', '', 'engine', 'Двигателю требуются ID входа и F0..F28.');
  }
  for (const [key, max] of [['states', limits.states], ['transitions', limits.transitions], ['effects', limits.effects], ['effectTables', limits.effectTables], ['blocks', limits.blocks], ['sinks', limits.sinks], ['assets', limits.assets]] as const) {
    const value = (input as Record<string, unknown>)[key];
    if (value !== undefined && (!Array.isArray(value) || value.length > max)) issue('schema', '', key, `Поле ${key} должно быть массивом не более чем из ${max} элементов.`);
  }
  if (errors.length) return errors;
  const graph = input as unknown as Project;
  const seen = new Set<string>();
  for (const state of graph.states ?? []) {
    if (!object(state, ['id', 'name', 'file', 'loop', 'volume', 'rate'], '', 'state')) continue;
    const id = typeof state.id === 'string' ? state.id : '';
    if (!validId(id) || !text(state.name, 63) || typeof state.file !== 'string' || (state.file !== '' && !validFile(state.file)) || typeof state.loop !== 'boolean' || !uint(state.volume, 100) || !uint(state.rate, 3000) || state.rate < 500) issue('schema', id, 'state', 'Недопустимый ID/имя/имя WAV-файла/цикл/громкость/скорость состояния.');
    if (seen.has(id)) issue('duplicate', id, 'id', 'Дублирующийся ID состояния.'); seen.add(id);
  }
  if (errors.some(error => error.code === 'schema')) return errors;
  if (!graph.states.length) issue('capacity', '', 'states', 'Требуется хотя бы одно тихое входное состояние.');
  if (graph.states.filter(state => state.file).length > limits.soundStates || graph.states.filter(state => !state.file).length > 26) issue('capacity', '', 'states', 'Не более 31 звукового и 26 тихих состояний.');
  for (const edge of graph.transitions ?? []) {
    if (!object(edge, ['id', 'source', 'target', 'priority', 'timing', 'condition'], '', 'transition')) continue;
    const id = typeof edge.id === 'string' ? edge.id : '';
    if (!validId(id) || typeof edge.source !== 'string' || typeof edge.target !== 'string' || !uint(edge.priority, 255) || !['immediate', 'after_sample'].includes(edge.timing)) issue('schema', id, 'transition', 'Недопустимый ID/концы/приоритет/момент перехода.');
    if (object(edge.condition, ['type', 'fn', 'min', 'max'], id, 'condition')) {
      const c = edge.condition;
      if (!CONDITIONS.includes(c.type)) issue('schema', id, 'condition', 'Неизвестное типизированное условие.');
      const fn = typeof c.type === 'string' && c.type.startsWith('fn_'), range = ['speed', 'accel', 'decel'].includes(c.type);
      if (fn ? !uint(c.fn, 28) : c.fn !== undefined) issue('schema', id, 'fn', 'Условие функции требует F0..F28; у других условий fn отсутствует.');
      if (range ? (c.min === undefined && c.max === undefined) || (c.min !== undefined && !uint(c.min, 255)) || (c.max !== undefined && !uint(c.max, 255)) : c.min !== undefined || c.max !== undefined) issue('schema', id, 'range', 'Диапазон скорости/ускорения требует min или max в пределах 0..255.');
      if (c.min !== undefined && c.max !== undefined && c.min > c.max) issue('range', id, 'min', 'Минимум диапазона превышает максимум.');
    }
  }
  for (const effect of graph.effects ?? []) {
    if (!object(effect, ['id', 'entry', 'fn'], '', 'effect')) continue;
    if (typeof effect.id !== 'string' || !validId(effect.id) || typeof effect.entry !== 'string' || !validId(effect.entry) || !uint(effect.fn, 28)) issue('schema', '', 'effects', 'Эффекту требуются ID, тихий вход и F0..F28.');
  }
  /* v2 authoring layer */
  const tables = new Set<string>();
  for (const table of graph.effectTables ?? []) {
    if (!object(table, ['id', 'name', 'kind', 'preset', 'init', 'loop', 'end', 'short', 'shortMs', 'volume', 'rate', 'behavior'], '', 'effectTable')) continue;
    const id = typeof table.id === 'string' ? table.id : '';
    if (!validId(id) || !text(table.name, 63) || !uint(table.shortMs, 10000) || !uint(table.volume, 100) || !uint(table.rate, 3000) || table.rate < 500 || !EFFECT_KINDS.includes(table.kind as EffectKind) || !PRESETS.includes(table.preset as Preset)) issue('schema', id, 'effectTable', 'Недопустимая таблица эффекта: ID/имя/категория/пресет/порог/громкость/скорость.');
    if (tables.has(id)) issue('duplicate', id, 'id', 'Дублирующийся ID таблицы эффекта.'); tables.add(id);
    for (const role of ['init', 'loop', 'end', 'short'] as const) {
      const file = (table as Record<string, unknown>)[role];
      if (file !== '' && (typeof file !== 'string' || !validFile(file))) issue('schema', id, role, `Недопустимое имя WAV роли ${role}.`);
    }
    if (typeof table.behavior !== 'string' || (table.behavior !== '' && !validId(table.behavior))) issue('schema', id, 'behavior', 'Недопустимая ссылка на поведение.');
  }
  const refs = new Set<string>();
  for (const source of graph.sources ?? []) {
    if (!object(source, ['id', 'role', 'fn', 'label'], '', 'source')) continue;
    const id = typeof source.id === 'string' ? source.id : '';
    if (!validId(id) || !text(source.label, 63) || !['fn', 'engine', 'motion', 'dir', 'state'].includes(source.role) || !uint(source.fn, 28)) issue('schema', id, 'source', 'Недопустимый источник: ID/роль/имя/F0..F28.');
    if (refs.has(id)) issue('duplicate', id, 'id', 'Дублирующийся ID узла.'); refs.add(id);
  }
  for (const block of graph.blocks ?? []) {
    if (!object(block, ['id', 'kind', 'table', 'op', 'min', 'max'], '', 'block')) continue;
    const id = typeof block.id === 'string' ? block.id : '';
    if (!validId(id) || !['sound', 'logic'].includes(block.kind) || !uint(block.min, 255) || !uint(block.max, 255)) issue('schema', id, 'block', 'Недопустимый блок.');
    if (block.kind === 'sound' && !tables.has(block.table)) issue('reference', id, 'table', 'Звуковой блок ссылается на несуществующую таблицу эффекта.');
    if (block.kind === 'logic' && !LOGIC_OPS.includes(block.op as LogicOp)) issue('schema', id, 'op', 'Неизвестная логическая операция.');
    if (refs.has(id)) issue('duplicate', id, 'id', 'Дублирующийся ID узла.'); refs.add(id);
  }
  const referenced = new Set((graph.blocks ?? []).filter(block => block.kind === 'sound').map(block => block.table));
  for (const table of graph.effectTables ?? []) {
    if (!referenced.has(table.id)) continue;
    if (table.preset === 'shortLong' && !table.short && !(table.init && table.end)) issue('table', table.id, 'short', 'Для «короткий/длинный» нужен short или пара init+end.');
    if (table.preset === 'loopHeld' && !table.loop) issue('table', table.id, 'loop', 'Для «цикл пока нажата» нужен Loop-файл.');
  }
  for (const sink of graph.sinks ?? []) {
    if (!object(sink, ['id', 'output'], '', 'sink')) continue;
    const id = typeof sink.id === 'string' ? sink.id : '';
    if (!validId(id) || !SINK_OUTPUTS.includes(sink.output as SinkOutput)) issue('schema', id, 'sink', 'Недопустимый выходной узел.');
    if (refs.has(id)) issue('duplicate', id, 'id', 'Дублирующийся ID узла.'); refs.add(id);
  }
  for (const wire of graph.wires ?? []) {
    if (!object(wire, ['id', 'from', 'to', 'event', 'dir', 'state'], '', 'wire')) continue;
    const id = typeof wire.id === 'string' ? wire.id : '';
    if (!validId(id) || !refs.has(wire.from) || !refs.has(wire.to) || !['any', 'fwd', 'rev'].includes(wire.dir) || !['any', 'moving', 'stopped'].includes(wire.state) || (wire.event !== '' && !ROUTE_EVENTS.includes(wire.event as typeof ROUTE_EVENTS[number]))) issue('schema', id, 'wire', 'Недопустимый провод или его концы.');
  }
  for (const asset of graph.assets ?? []) {
    if (!object(asset, ['file', 'size', 'crc32', 'sampleRate', 'channels', 'bits', 'durationMs'], '', 'asset')) continue;
    if (typeof asset.file !== 'string' || !validFile(asset.file) || typeof asset.crc32 !== 'string' || !/^[a-fA-F0-9]{8}$/.test(asset.crc32) || ![asset.size, asset.sampleRate, asset.channels, asset.bits].every(value => uint(value, 0xffffffff) && value > 0) || !uint(asset.durationMs, 0xffffffff)) issue('schema', '', 'assets', 'Недопустимые размеры манифеста WAV или CRC32.');
  }
  if (object(graph.editor, ['positions', 'viewport'], '', 'editor')) {
    if (!record(graph.editor.positions)) issue('schema', '', 'positions', 'Позиции редактора должны быть объектом.');
    else for (const [id, position] of Object.entries(graph.editor.positions)) {
      if (!validId(id) || !object(position, ['x', 'y'], id, 'position') || ![position.x, position.y].every(value => typeof value === 'number' && Number.isFinite(value))) issue('schema', id, 'position', 'Недействительные координаты редактора.');
    }
    const view = graph.editor.viewport;
    if (!object(view, ['x', 'y', 'zoom'], '', 'viewport') || ![view.x, view.y, view.zoom].every(value => typeof value === 'number' && Number.isFinite(value)) || view.zoom < 0.1 || view.zoom > 4) issue('schema', '', 'viewport', 'Недопустимая область просмотра; масштаб должен быть 0.1..4.');
  }
  if (errors.some(error => error.code === 'schema')) return errors;
  const states = new Map(graph.states.map(state => [state.id, state]));
  for (const [entry, id] of [[graph.engine.entry, ''], ...graph.effects.map(effect => [effect.entry, effect.id])]) {
    if (!states.has(entry) || states.get(entry)!.file) issue('entry', id, 'entry', 'Каждый вход двигателя/эффекта должен ссылаться на тихое состояние.');
  }
  const unique = (items: { id: string }[], field: string) => {
    const label = field === 'transition' ? 'перехода' : 'эффекта';
    const ids = new Set<string>(); for (const item of items) { if (ids.has(item.id)) issue('duplicate', item.id, field, `Дублирующийся ID ${label}.`); ids.add(item.id); }
  };
  unique(graph.transitions, 'transition'); unique(graph.effects, 'effect');
  const priorities = new Set<string>();
  for (const edge of graph.transitions) {
    if (!states.has(edge.source) || !states.has(edge.target)) issue('reference', edge.id, 'target', 'Конечная точка перехода не существует.');
    const key = `${edge.source}:${edge.priority}`;
    if (priorities.has(key)) issue('priority', edge.id, 'priority', 'Приоритеты исходящих переходов должны быть уникальны; побеждает больший.'); priorities.add(key);
  }
  const files = new Set<string>();
  for (const asset of graph.assets) { if (files.has(asset.file)) issue('duplicate', '', 'assets', `Дублирующийся WAV в манифесте: ${asset.file}`); files.add(asset.file); }
  const required = new Set<string>(graph.states.filter(state => state.file).map(state => state.file));
  for (const file of required) if (!files.has(file)) issue('asset', '', 'file', `Выберите/проверьте требуемый WAV: ${file}. JSON не содержит аудио.`, forApply ? 'error' : 'warning');
  const immediate = graph.transitions.filter(edge => edge.timing === 'immediate' && !states.get(edge.source)?.file && !states.get(edge.target)?.file && !['fn_press', 'fn_release', 'sample_done'].includes(edge.condition.type));
  const visiting = new Set<string>(), visited = new Set<string>();
  function visit(id: string): boolean {
    if (visiting.has(id)) return true; if (visited.has(id)) return false;
    visiting.add(id);
    for (const edge of immediate.filter(edge => edge.source === id)) if (visit(edge.target)) return true;
    visiting.delete(id); visited.add(id); return false;
  }
  for (const state of graph.states) if (visit(state.id)) { issue('cycle', state.id, 'transitions', 'Немедленный тихий цикл без нового события или границы сэмпла.'); break; }
  const reachable = new Set([graph.engine.entry, ...graph.effects.map(effect => effect.entry)]);
  for (let changed = true; changed;) {
    changed = false;
    for (const edge of graph.transitions) if (reachable.has(edge.source) && !reachable.has(edge.target)) { reachable.add(edge.target); changed = true; }
  }
  for (const state of graph.states) if (!reachable.has(state.id)) issue('unreachable', state.id, 'state', 'Состояние недостижимо из объединения входов двигателя/эффектов.');
  if (byteLength(JSON.stringify(graph)) > limits.jsonBytes) issue('capacity', '', 'project', 'JSON превышает лимит тела запроса устройства.');
  return errors;
}

// Detect duplicate JSON keys before JSON.parse can silently discard them.
export function parseProject(json: string, limits: Limits = DEFAULT_LIMITS): Project {
  if (byteLength(json) > limits.jsonBytes) throw new Error('JSON превышает лимит 128 КиБ/устройства.');
  let pos = 0, tokens = 0;
  function value(depth: number): void {
    if (depth > 16 || ++tokens > 8192) throw new Error('Превышен лимит вложенности/токенов JSON.');
    whitespace(); const char = json[pos];
    if (char === '{' || char === '[') {
      const object = char === '{', close = object ? '}' : ']'; pos++; whitespace();
      const keys = new Set<string>();
      if (json[pos] !== close) for (;;) {
        if (object) {
          whitespace(); const key = string();
          if (keys.has(key)) throw new Error(`Дублирующийся ключ JSON: ${key}`); keys.add(key); tokens++;
          whitespace(); if (json[pos++] !== ':') throw new Error('Ожидалось двоеточие JSON.');
        }
        value(depth + 1); whitespace();
        if (json[pos] !== ',') break; pos++;
      }
      if (json[pos++] !== close) throw new Error('Недопустимый контейнер JSON.');
    } else if (char === '"') string();
    else {
      const token = /^(?:true|false|null|-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?)/.exec(json.slice(pos));
      if (!token) throw new Error('Недопустимое значение JSON.'); pos += token[0].length;
    }
  }
  function whitespace() { while (/\s/.test(json[pos] ?? '') && pos < json.length) pos++; }
  function string(): string {
    if (json[pos] !== '"') throw new Error('Ожидалась строка JSON.');
    const start = pos++; let escaped = false;
    while (pos < json.length) { const char = json[pos++]; if (char === '"' && !escaped) { const decoded: unknown = JSON.parse(json.slice(start, pos)); if (!text(decoded, 1024)) throw new Error('Недопустимая/слишком длинная строка JSON.'); return decoded as string; } if (char === '\\' && !escaped) escaped = true; else escaped = false; }
    throw new Error('Незакрытая строка JSON.');
  }
  value(0); whitespace(); if (pos !== json.length) throw new Error('Лишние данные после JSON.');
  const result: unknown = JSON.parse(json);
  const structural = validateProject(result, limits).filter(error => error.code === 'schema');
  if (structural.length) throw new Error(structural.map(error => error.message).join(' '));
  return result as Project;
}

export function nextId(prefix: string, ids: string[]): string {
  for (let i = 1; ; i++) if (!ids.includes(`${prefix}_${i}`)) return `${prefix}_${i}`;
}

export function deleteState(graph: Project, id: string): void {
  if (graph.engine.entry === id || graph.effects.some(effect => effect.entry === id)) throw new Error('Переназначьте вход двигателя/эффекта перед удалением.');
  graph.states = graph.states.filter(state => state.id !== id);
  graph.transitions = graph.transitions.filter(edge => edge.source !== id && edge.target !== id);
  delete graph.editor.positions[id];
}
