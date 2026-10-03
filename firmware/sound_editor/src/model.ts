export const DEFAULT_LIMITS = { states: 57, soundStates: 31, transitions: 128, effects: 24, assets: 31, jsonBytes: 131072 };
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
export type Project = {
  format: 'sound-graph'; schemaVersion: 1; id: string; name: string;
  engine: { entry: string; fn: number }; hysteresis: number;
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

export function validateProject(input: unknown, limits: Limits = DEFAULT_LIMITS, forApply = false): Diagnostic[] {
  const errors: Diagnostic[] = [];
  const issue = (code: string, id: string, field: string, message: string, severity: 'error' | 'warning' = 'error') => errors.push({ code, id, field, message, severity });
  const object = (value: unknown, allowed: string[], id: string, field: string): value is Record<string, unknown> => {
    if (!record(value) || Object.keys(value).some(key => !allowed.includes(key))) {
      issue('schema', id, field, `Недопустимый объект или неизвестное поле: ${field}`); return false;
    }
    return true;
  };
  if (!object(input, ['format', 'schemaVersion', 'id', 'name', 'engine', 'hysteresis', 'states', 'transitions', 'effects', 'assets', 'editor'], '', 'project')) return errors;
  if (input.format !== 'sound-graph' || input.schemaVersion !== 1) issue('schema', '', 'format', 'Поддерживается только схема sound-graph версии 1. MDS — не JSON.');
  if (typeof input.id !== 'string' || !validId(input.id)) issue('schema', '', 'id', 'ID: 1..31 символов ASCII — буквы, цифры, подчёркивание или дефис.');
  if (!text(input.name, 63)) issue('schema', '', 'name', 'Имя должно быть корректным UTF-8, не более 63 байт.');
  if (!uint(input.hysteresis, 32)) issue('schema', '', 'hysteresis', 'Гистерезис должен быть 0..32.');
  if (object(input.engine, ['entry', 'fn'], '', 'engine')) {
    if (typeof input.engine.entry !== 'string' || !validId(input.engine.entry) || !uint(input.engine.fn, 28)) issue('schema', '', 'engine', 'Двигателю требуются ID входа и F0..F28.');
  }
  for (const [key, max] of [['states', limits.states], ['transitions', limits.transitions], ['effects', limits.effects], ['assets', limits.assets]] as const) {
    if (!Array.isArray(input[key]) || input[key].length > max) issue('schema', '', key, `Поле ${key} должно быть массивом не более чем из ${max} элементов.`);
  }
  if (errors.length) return errors;
  const graph = input as unknown as Project;
  const seen = new Set<string>();
  for (const state of graph.states) {
    if (!object(state, ['id', 'name', 'file', 'loop', 'volume', 'rate'], '', 'state')) continue;
    const id = typeof state.id === 'string' ? state.id : '';
    if (!validId(id) || !text(state.name, 63) || typeof state.file !== 'string' || (state.file !== '' && !validFile(state.file)) || typeof state.loop !== 'boolean' || !uint(state.volume, 100) || !uint(state.rate, 3000) || state.rate < 500) issue('schema', id, 'state', 'Недопустимый ID/имя/имя WAV-файла/цикл/громкость/скорость состояния.');
    if (seen.has(id)) issue('duplicate', id, 'id', 'Дублирующийся ID состояния.'); seen.add(id);
  }
  if (errors.some(error => error.code === 'schema')) return errors;
  if (!graph.states.length) issue('capacity', '', 'states', 'Требуется хотя бы одно тихое входное состояние.');
  if (graph.states.filter(state => state.file).length > limits.soundStates || graph.states.filter(state => !state.file).length > 26) issue('capacity', '', 'states', 'Не более 31 звукового и 26 тихих состояний.');
  for (const edge of graph.transitions) {
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
  for (const effect of graph.effects) {
    if (!object(effect, ['id', 'entry', 'fn'], '', 'effect')) continue;
    if (typeof effect.id !== 'string' || !validId(effect.id) || typeof effect.entry !== 'string' || !validId(effect.entry) || !uint(effect.fn, 28)) issue('schema', '', 'effects', 'Эффекту требуются ID, тихий вход и F0..F28.');
  }
  for (const asset of graph.assets) {
    if (!object(asset, ['file', 'size', 'crc32', 'sampleRate', 'channels', 'bits', 'durationMs'], '', 'asset')) continue;
    if (typeof asset.file !== 'string' || !validFile(asset.file) || typeof asset.crc32 !== 'string' || !/^[a-fA-F0-9]{8}$/.test(asset.crc32) || ![asset.size, asset.sampleRate, asset.channels, asset.bits].every(value => uint(value, 0xffffffff) && value > 0) || !uint(asset.durationMs, 0xffffffff)) issue('schema', '', 'assets', 'Недопустимые размеры манифеста WAV или CRC32.');
  }
  if (object(graph.editor, ['positions', 'viewport'], '', 'editor')) {
    if (!record(graph.editor.positions)) issue('schema', '', 'positions', 'Позиции редактора должны быть объектом.');
    else for (const [id, position] of Object.entries(graph.editor.positions)) {
      if (!validId(id) || !object(position, ['x', 'y'], id, 'position') || ![position.x, position.y].every(value => typeof value === 'number' && Number.isFinite(value))) issue('schema', id, 'position', 'Недопустимые координаты редактора.');
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
  for (const state of graph.states) if (state.file && !files.has(state.file)) issue('asset', state.id, 'file', `Выберите/проверьте требуемый WAV: ${state.file}. JSON не содержит аудио.`, forApply ? 'error' : 'warning');
  // Event edges advance on a fresh event; silent sample_done still advances next tick.
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

export function emptyProject(id = 'new_graph'): Project {
  return { format: 'sound-graph', schemaVersion: 1, id, name: 'Новый звуковой граф', engine: { entry: 'off', fn: 8 }, hysteresis: 3,
    states: [{ id: 'off', name: 'Выкл / тихий вход', file: '', loop: false, volume: 100, rate: 1000 }], transitions: [], effects: [], assets: [], editor: { positions: { off: { x: 60, y: 80 } }, viewport: { x: 0, y: 0, zoom: 0.8 } } };
}

export function dieselTemplate(id = 'diesel_graph'): Project {
  const graph = emptyProject(id); graph.name = 'Тепловоз + удержание F2 (гудок)';
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

export function deleteState(graph: Project, id: string): void {
  if (graph.engine.entry === id || graph.effects.some(effect => effect.entry === id)) throw new Error('Переназначьте вход двигателя/эффекта перед удалением.');
  graph.states = graph.states.filter(state => state.id !== id);
  graph.transitions = graph.transitions.filter(edge => edge.source !== id && edge.target !== id);
  delete graph.editor.positions[id];
}
