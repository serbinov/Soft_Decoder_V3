<script lang="ts">
  import type { Project, EffectTable, EffectKind, Preset } from './model';
  import { EFFECT_KINDS, PRESETS } from './model';
  import type { Track } from './api';

  let { graph, mutate, tracks }: { graph: Project; mutate: (edit: (project: Project) => void) => void; tracks: Track[] } = $props();
  const files = $derived(tracks.map(track => track.file.replace(/^audio\//, '')).filter(file => /^[A-Za-z0-9_-]+\.wav$/.test(file)));
  const KIND_NAMES: Record<EffectKind, string> = { horn: 'Гудок', motor: 'Двигатель', brake: 'Тормоза', bell: 'Звонок', coupler: 'Сцепка', custom: 'Другой' };
  const PRESET_NAMES: Record<Preset, string> = { oneShot: 'Один раз', loopHeld: 'Цикл пока нажата', shortLong: 'Короткий/длинный', latched: 'Переключатель', random: 'Случайно', state: 'По состоянию' };
  let editing = $state('');
  const ROLES = ['init', 'loop', 'end', 'short'] as const;
  const ROLE_NAMES: Record<typeof ROLES[number], string> = { init: 'Начало', loop: 'Тело (цикл)', end: 'Конец', short: 'Короткий' };

  function field(id: string, key: keyof EffectTable, value: string | number) { mutate(project => { const table = project.effectTables.find(t => t.id === id); if (table) (table as unknown as Record<string, string | number>)[key] = value; }); }
  function add() { mutate(project => { let n = 1; while (project.effectTables.some(t => t.id === `fx_${n}`)) n++; project.effectTables.push({ id: `fx_${n}`, name: `Эффект ${n}`, kind: 'custom', preset: 'oneShot', init: '', loop: '', end: '', short: '', shortMs: 400, volume: 80, rate: 1000, behavior: '' }); }); }
  function remove(id: string) { mutate(project => { const blocks = new Set(project.blocks.filter(b => b.table === id).map(b => b.id)); project.effectTables = project.effectTables.filter(t => t.id !== id); project.blocks = project.blocks.filter(b => !blocks.has(b.id)); project.wires = project.wires.filter(w => !blocks.has(w.from) && !blocks.has(w.to)); }); }
</script>

<section class="library" aria-label="Библиотека эффектов">
  <header class="library-hd">
    <h2>Библиотека эффектов</h2>
    <button class="button" onclick={add}>+ Таблица</button>
  </header>
  <p class="hint">Эффект описывается один раз здесь, а в пульте используется готовым блоком.</p>
  <div class="cards">
    {#each graph.effectTables as table (table.id)}
      <article class="effect-card" class:open={editing === table.id}>
        <header>
          <strong>{table.name}</strong>
          <span class="role">{KIND_NAMES[table.kind]} · {PRESET_NAMES[table.preset]}</span>
        </header>
        <div class="summary">{table.init || table.loop || table.end || table.short ? [table.init && `init ${table.init}`, table.loop && `loop ${table.loop}`, table.end && `end ${table.end}`, table.short && `short ${table.short}`].filter(Boolean).join(' · ') : 'Файлы не заданы'}</div>
        <div class="card-actions">
          <button class="button sm" onclick={() => editing = editing === table.id ? '' : table.id}>{editing === table.id ? 'Свернуть' : 'Изменить'}</button>
          <button class="button sm danger" onclick={() => remove(table.id)}>Удалить</button>
        </div>
        {#if editing === table.id}
          <div class="edit">
            <label>Имя<input value={table.name} maxlength="63" onchange={e => field(table.id, 'name', e.currentTarget.value)} /></label>
            <div class="row">
              <label>Категория<select value={table.kind} onchange={e => field(table.id, 'kind', e.currentTarget.value)}>{#each EFFECT_KINDS as kind}<option value={kind}>{KIND_NAMES[kind]}</option>{/each}</select></label>
              <label>Поведение<select value={table.preset} onchange={e => field(table.id, 'preset', e.currentTarget.value)}>{#each PRESETS as preset}<option value={preset}>{PRESET_NAMES[preset]}</option>{/each}</select></label>
            </div>
            {#each ROLES as role}
              <label>{ROLE_NAMES[role]}
                <select value={table[role]} onchange={e => field(table.id, role, e.currentTarget.value)}>
                  <option value="">— нет —</option>
                  {#each files as file}<option value={file}>{file}</option>{/each}
                </select>
              </label>
            {/each}
            <div class="row">
              <label>Порог тапа, мс<input type="number" min="0" max="10000" value={table.shortMs} onchange={e => field(table.id, 'shortMs', Number(e.currentTarget.value))} /></label>
              <label>Громкость<input type="number" min="0" max="100" value={table.volume} onchange={e => field(table.id, 'volume', Number(e.currentTarget.value))} /></label>
              <label>Скорость ‰<input type="number" min="500" max="3000" step="50" value={table.rate} onchange={e => field(table.id, 'rate', Number(e.currentTarget.value))} /></label>
            </div>
          </div>
        {/if}
      </article>
    {/each}
  </div>
</section>
