<script lang="ts">
  import type { Project, Source, Block, Sink, LogicOp, SinkOutput, RouteEvent, DirGate, StateGate } from './model';
  import { LOGIC_OPS, SINK_OUTPUTS, ROUTE_EVENTS } from './model';

  let { graph, mutate }: { graph: Project; mutate: (edit: (project: Project) => void) => void } = $props();

  const EVENT_NAMES: Record<string, string> = { '': 'без события', fn_press: 'нажатие', fn_release: 'отпускание', fn_on: 'удержание', fn_off: 'снятие', sample_done: 'граница сэмпла' };
  const OP_NAMES: Record<LogicOp, string> = { and: 'И', or: 'Или', not: 'Не', delay: 'Задержка', oneShot: 'Один раз', latch: 'Переключатель', speedRange: 'Скорость', dir: 'Направление', state: 'Состояние', random: 'Случайно', duck: 'Приглушить' };
  const DIR_NAMES: Record<DirGate, string> = { any: 'любое', fwd: 'вперёд', rev: 'назад' };
  const STATE_NAMES: Record<StateGate, string> = { any: 'любое', moving: 'едет', stopped: 'стоит' };

  const tableName = (id: string) => graph.effectTables.find(t => t.id === id)?.name ?? id;
  const sourceLabel = (s: Source) => s.role === 'fn' ? `F${s.fn}` : s.role === 'engine' ? 'Двигатель' : s.role === 'motion' ? 'Движение' : s.role === 'dir' ? 'Направление' : 'Состояние';
  const blockLabel = (b: Block) => b.kind === 'sound' ? tableName(b.table) : OP_NAMES[b.op];
  const sinkLabel = (s: Sink) => s.output === 'AUDIO' ? 'Звук (микшер)' : s.output;
  const refLabel = (id: string) => { const s = graph.sources.find(x => x.id === id); if (s) return sourceLabel(s); const b = graph.blocks.find(x => x.id === id); if (b) return blockLabel(b); const k = graph.sinks.find(x => x.id === id); return k ? sinkLabel(k) : id; };

  function nextId(prefix: string, ids: string[]) { let n = 1; while (ids.includes(`${prefix}_${n}`)) n++; return `${prefix}_${n}`; }
  function addSource() { mutate(project => { const used = project.sources.filter(s => s.role === 'fn').map(s => s.fn); let fn = 0; while (used.includes(fn) && fn < 28) fn++; project.sources.push({ id: nextId('src', project.sources.map(s => s.id)), role: 'fn', fn, label: '' }); }); }
  function addSoundBlock() { mutate(project => { const table = project.effectTables[0]?.id ?? ''; project.blocks.push({ id: nextId('blk', project.blocks.map(b => b.id)), kind: 'sound', table, op: 'and', min: 0, max: 0 }); }); }
  function addLogicBlock() { mutate(project => { project.blocks.push({ id: nextId('blk', project.blocks.map(b => b.id)), kind: 'logic', table: '', op: 'and', min: 0, max: 0 }); }); }
  function addSink() { mutate(project => { const used = project.sinks.map(s => s.output); const output = SINK_OUTPUTS.find(o => !used.includes(o)) ?? 'AUX1'; project.sinks.push({ id: nextId('sink', project.sinks.map(s => s.id)), output }); }); }
  function removeNode(id: string) { mutate(project => { project.sources = project.sources.filter(s => s.id !== id); project.blocks = project.blocks.filter(b => b.id !== id); project.sinks = project.sinks.filter(s => s.id !== id); project.wires = project.wires.filter(w => w.from !== id && w.to !== id); }); }
  function addWire() { mutate(project => { if (!project.sources.length || (!project.blocks.length && !project.sinks.length)) return; project.wires.push({ id: nextId('w', project.wires.map(w => w.id)), from: project.sources[0].id, to: (project.blocks[0] ?? project.sinks[0]).id, event: 'fn_press', dir: 'any', state: 'any' }); }); }
  function removeWire(id: string) { mutate(project => { project.wires = project.wires.filter(w => w.id !== id); }); }
  function sourceField(id: string, key: keyof Source, value: string | number) { mutate(project => { const s = project.sources.find(x => x.id === id); if (s) (s as unknown as Record<string, string | number>)[key] = value; }); }
  function blockField(id: string, key: keyof Block, value: string | number) { mutate(project => { const b = project.blocks.find(x => x.id === id); if (b) (b as unknown as Record<string, string | number>)[key] = value; }); }
  function sinkField(id: string, value: string) { mutate(project => { const s = project.sinks.find(x => x.id === id); if (s) s.output = value as SinkOutput; }); }
  function wireField(id: string, key: 'from' | 'to' | 'event' | 'dir' | 'state', value: string) { mutate(project => { const w = project.wires.find(x => x.id === id); if (w) (w as unknown as Record<string, string>)[key] = value; }); }
  const summary = (id: string) => { const w = graph.wires.find(x => x.id === id); if (!w) return ''; const event = w.event ? ` по «${EVENT_NAMES[w.event]}»` : ''; const gates = [w.dir !== 'any' && DIR_NAMES[w.dir], w.state !== 'any' && STATE_NAMES[w.state]].filter(Boolean).join(', '); return `${refLabel(w.from)}${event} → ${refLabel(w.to)}${gates ? ` (${gates})` : ''}`; };
</script>

<section class="patch" aria-label="Патч-пульт">
  <p class="hint">Слева — источники (F-клавиши и состояние), в центре — звук/логика, справа — выходы. Провода соединяют их слева направо.</p>
  <div class="columns">
    <div class="col">
      <header>Источники <button class="button sm" onclick={addSource}>+</button></header>
      {#each graph.sources as source (source.id)}
        <article class="node source">
          <div class="node-hd"><strong>{sourceLabel(source)}</strong><button class="x" aria-label="Удалить" onclick={() => removeNode(source.id)}>✕</button></div>
          {#if source.role === 'fn'}
            <label>Клавиша<select value={source.fn} onchange={e => sourceField(source.id, 'fn', Number(e.currentTarget.value))}>{#each Array.from({ length: 29 }, (_, i) => i) as n}<option value={n}>F{n}</option>{/each}</select></label>
          {/if}
          {#if source.role !== 'fn'}
            <label>Клавиша F0..28<select value={source.fn} onchange={e => sourceField(source.id, 'fn', Number(e.currentTarget.value))}>{#each Array.from({ length: 29 }, (_, i) => i) as n}<option value={n}>F{n}</option>{/each}</select></label>
          {/if}
        </article>
      {/each}
      {#if !graph.sources.length}<p class="empty">Нет источников</p>{/if}
    </div>
    <div class="col">
      <header>Блоки <span class="adds"><button class="button sm" onclick={addSoundBlock}>+ Звук</button><button class="button sm" onclick={addLogicBlock}>+ Логика</button></span></header>
      {#each graph.blocks as block (block.id)}
        <article class="node block">
          <div class="node-hd"><strong>{block.kind === 'sound' ? 'Звук' : 'Логика'}</strong><button class="x" aria-label="Удалить" onclick={() => removeNode(block.id)}>✕</button></div>
          {#if block.kind === 'sound'}
            <label>Таблица<select value={block.table} onchange={e => blockField(block.id, 'table', e.currentTarget.value)}>{#each graph.effectTables as t}<option value={t.id}>{t.name}</option>{/each}</select></label>
          {:else}
            <label>Операция<select value={block.op} onchange={e => blockField(block.id, 'op', e.currentTarget.value)}>{#each LOGIC_OPS as op}<option value={op}>{OP_NAMES[op]}</option>{/each}</select></label>
            {#if block.op === 'speedRange'}<div class="row"><label>Мин<input type="number" min="0" max="255" value={block.min} onchange={e => blockField(block.id, 'min', Number(e.currentTarget.value))} /></label><label>Макс<input type="number" min="0" max="255" value={block.max} onchange={e => blockField(block.id, 'max', Number(e.currentTarget.value))} /></label></div>{/if}
            {#if block.op === 'delay'}<label>Задержка, мс<input type="number" min="0" max="10000" value={block.min} onchange={e => blockField(block.id, 'min', Number(e.currentTarget.value))} /></label>{/if}
          {/if}
        </article>
      {/each}
      {#if !graph.blocks.length}<p class="empty">Нет блоков</p>{/if}
    </div>
    <div class="col">
      <header>Выходы <button class="button sm" onclick={addSink}>+</button></header>
      {#each graph.sinks as sink (sink.id)}
        <article class="node sink">
          <div class="node-hd"><strong>{sinkLabel(sink)}</strong><button class="x" aria-label="Удалить" onclick={() => removeNode(sink.id)}>✕</button></div>
          <label>Выход<select value={sink.output} onchange={e => sinkField(sink.id, e.currentTarget.value)}>{#each SINK_OUTPUTS as output}<option value={output}>{output === 'AUDIO' ? 'Звук (микшер)' : output}</option>{/each}</select></label>
        </article>
      {/each}
      {#if !graph.sinks.length}<p class="empty">Нет выходов</p>{/if}
    </div>
  </div>

  <div class="wires">
    <header><h3>Соединения</h3><button class="button sm" onclick={addWire}>+ Провод</button></header>
    {#each graph.wires as wire (wire.id)}
      <div class="wire">
        <div class="wire-row">
          <select value={wire.from} onchange={e => wireField(wire.id, 'from', e.currentTarget.value)}>
            {#each graph.sources as s}<option value={s.id}>{sourceLabel(s)}</option>{/each}
          </select>
          <select value={wire.event} onchange={e => wireField(wire.id, 'event', e.currentTarget.value as RouteEvent)}>{#each ROUTE_EVENTS as event}<option value={event}>{EVENT_NAMES[event]}</option>{/each}</select>
          <select value={wire.to} onchange={e => wireField(wire.id, 'to', e.currentTarget.value)}>
            {#each graph.blocks as b}<option value={b.id}>{blockLabel(b)}</option>{/each}
            {#each graph.sinks as k}<option value={k.id}>{sinkLabel(k)}</option>{/each}
          </select>
          <select value={wire.dir} onchange={e => wireField(wire.id, 'dir', e.currentTarget.value)}>{#each Object.keys(DIR_NAMES) as d}<option value={d}>{DIR_NAMES[d as DirGate]}</option>{/each}</select>
          <select value={wire.state} onchange={e => wireField(wire.id, 'state', e.currentTarget.value)}>{#each Object.keys(STATE_NAMES) as s}<option value={s}>{STATE_NAMES[s as StateGate]}</option>{/each}</select>
          <button class="x" aria-label="Удалить провод" onclick={() => removeWire(wire.id)}>✕</button>
        </div>
        <div class="wire-summary">{summary(wire.id)}</div>
      </div>
    {/each}
    {#if !graph.wires.length}<p class="empty">Нет соединений</p>{/if}
  </div>
</section>
