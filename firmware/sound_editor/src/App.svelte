<script lang="ts">
  import { onMount, untrack } from 'svelte';
  import { Background, Controls, MarkerType, SvelteFlow, type Connection, type Edge, type Node } from '@xyflow/svelte';
  import SoundNode from './SoundNode.svelte';
  import EffectsPanel from './EffectsPanel.svelte';
  import PatchPanel from './PatchPanel.svelte';
  import { CONDITIONS, DEFAULT_LIMITS, byteLength, clone, compileWires, conditionLabel, conditionName, deleteState, devicePayload, dieselTemplate, emptyProject, migrateV1toV2, nextId, parseProject, positionOf, setPosition, validFile, validateProject, type ConditionType, type Diagnostic, type Limits, type Project, type Viewport } from './model';
  import { ApiError, api, type Descriptor, type RuntimeStatus, type Track } from './api';
  import { readDraft, writeDraft, type Draft } from './draft';

  let graph = $state.raw<Project>(emptyProject());
  let saved = $state('');
  let savedId = $state('');
  let revision = $state(0);
  let uncertain = $state(false);
  let conflict = $state(false);
  let limits = $state.raw<Limits>(DEFAULT_LIMITS);
  let projects = $state.raw<Descriptor[]>([]);
  let tracks = $state.raw<Track[]>([]);
  let projectChoice = $state('');
  let selected = $state('');
  let selectedKind = $state<'state' | 'edge' | ''>('');
  let past = $state.raw<Project[]>([]), future = $state.raw<Project[]>([]);
  let nodes = $state.raw<Node[]>([]), edges = $state.raw<Edge[]>([]);
  let viewport = $state<Viewport>({ x: 0, y: 0, zoom: 0.8 });
  let interacting = $state(false);
  let runtime = $state.raw<RuntimeStatus | null>(null);
  let online = $state(false);
  let busy = $state('');
  let message = $state('Сохранение создаёт черновик. Применение явно активирует сохранённую версию, пока двигатель остановлен.');
  let serverErrors = $state.raw<Diagnostic[]>([]);
  let recovery = $state.raw<Draft | undefined>();
  let ready = $state(false);
  let compatible = $state(false);
  let draftStatus = $state('Загрузка локального восстановления...');
  let drawer = $state<'palette' | 'inspector' | ''>('');
  let fullscreen = $state(false);
  let from = $state(''), to = $state('');
  let tab = $state<'patch' | 'effects' | 'graph'>('patch');
  let theme = $state<'dark' | 'light'>(currentTheme());
  let uploadInput: HTMLInputElement;
  let importInput: HTMLInputElement;
  const nodeTypes = { sound: SoundNode };
  const functions = Array.from({ length: 29 }, (_, i) => i);
  const serialized = $derived(JSON.stringify(graph));
  const dirty = $derived(serialized !== saved);
  const effectiveRevision = $derived(graph.id === savedId ? revision : 0);
  const diagnostics = $derived([...validateProject(graph, limits), ...serverErrors]);
  const applyErrors = $derived(validateProject(graph, limits, true).filter(error => error.severity !== 'warning'));
  const selectedState = $derived(graph.states.find(state => selectedKind === 'state' && state.id === selected));
  const edge = $derived(graph.transitions.find(edge => selectedKind === 'edge' && edge.id === selected));
  const activeHere = $derived(runtime?.active && runtime.id === graph.id && runtime.revision === effectiveRevision && !dirty);
  const switchBlocked = $derived(!ready || !online || !compatible || !!busy || !!recovery || uncertain || conflict);
  const edgeStroke = $derived(theme === 'light' ? '#5b6478' : '#7395c8');
  const edgeLabel = $derived(theme === 'light' ? '#0f1420' : '#fff');
  const edgeLabelBg = $derived(theme === 'light' ? '#ffffff' : '#14171d');

  $effect(() => { document.documentElement.dataset.theme = theme; });

  $effect(() => {
    if (interacting) return;
    const activeIds = activeHere ? new Set(runtime!.states.filter(index => index < graph.states.length).map(index => graph.states[index].id)) : new Set();
    nodes = graph.states.map((state, index) => ({ id: state.id, type: 'sound', position: positionOf(graph, state.id) ?? { x: (index % 3) * 270, y: Math.floor(index / 3) * 180 }, selected: selectedKind === 'state' && selected === state.id, data: { ...state, label: state.name, active: activeIds.has(state.id), entry: graph.engine.entry === state.id || graph.effects.some(effect => effect.entry === state.id), invalid: diagnostics.some(error => error.id === state.id && error.severity !== 'warning') } }));
    edges = graph.transitions.map(edge => ({ id: edge.id, source: edge.source, target: edge.target, selected: selectedKind === 'edge' && selected === edge.id, label: `${conditionLabel(edge.condition)} / ${edge.priority}`, markerEnd: { type: MarkerType.ArrowClosed }, style: `stroke: ${edgeStroke};`, labelStyle: `fill: ${edgeLabel};`, labelBgStyle: `fill: ${edgeLabelBg};` }));
    const view = graph.editor.viewport;
    untrack(() => { if (viewport.x !== view.x || viewport.y !== view.y || viewport.zoom !== view.zoom) viewport = clone(view); });
  });

  $effect(() => {
    if (!ready || recovery) return;
    const draft: Draft = { project: clone(graph), revision: effectiveRevision, saved, time: Date.now(), uncertain: uncertain || conflict };
    let current = true;
    draftStatus = 'Сохранение локального восстановления...';
    writeDraft(draft).then(() => { if (current) draftStatus = 'Локальное восстановление сохранено'; }).catch(() => { if (current) draftStatus = 'Локальное восстановление недоступно. Экспортируйте JSON перед закрытием.'; });
    return () => { current = false; };
  });

  onMount(() => {
    let disposed = false, polling = false, timer: ReturnType<typeof setTimeout>;
    readDraft().then(draft => { if (!disposed) { recovery = draft; ready = true; draftStatus = draft ? 'Восстановление доступно' : 'Локальное восстановление готово'; } }).catch(() => { ready = true; draftStatus = 'IndexedDB недоступен. Экспортируйте JSON, чтобы сохранить правки.'; });
    void refresh();
    async function poll() {
      if (disposed || document.hidden || polling) return;
      polling = true;
      try { const status = await api.state(); if (!disposed) { runtime = status; online = true; } } catch { if (!disposed) { online = false; runtime = null; } }
      polling = false;
      clearTimeout(timer);
      if (!disposed && !document.hidden) timer = setTimeout(poll, 2000);
    }
    function visibility() { clearTimeout(timer); if (!document.hidden) timer = setTimeout(poll, 2000); }
    function unload(event: BeforeUnloadEvent) { if (dirty || uncertain) { event.preventDefault(); event.returnValue = ''; } }
    function syncTheme() { theme = currentTheme(); }
    document.addEventListener('visibilitychange', visibility); window.addEventListener('beforeunload', unload); window.addEventListener('storage', syncTheme); void poll();
    return () => { disposed = true; clearTimeout(timer); document.removeEventListener('visibilitychange', visibility); window.removeEventListener('beforeunload', unload); window.removeEventListener('storage', syncTheme); };
  });

  async function refresh() {
    const results = await Promise.allSettled([api.capabilities(), api.projects(), api.tracks()]);
    if (results[0].status === 'fulfilled') {
      const capabilities = results[0].value;
      if (capabilities.format !== 'sound-graph' || capabilities.schemaVersion !== 1) { message = 'Формат графа устройства несовместим. Сохранение/применение отключены.'; compatible = false; return; }
      const advertised = capabilities.limits;
      if (advertised && Object.keys(DEFAULT_LIMITS).every(key => Number.isInteger(advertised[key as keyof Limits]) && advertised[key as keyof Limits] > 0)) { limits = { ...DEFAULT_LIMITS, ...advertised }; compatible = true; }
      else { compatible = false; message = 'Возможности устройства некорректны. Сохранение/применение отключены.'; }
    } else { compatible = false; message = 'API устройства недоступен. Локальное редактирование/экспорт доступны; записи никогда не повторяются автоматически.'; }
    if (results[1].status === 'fulfilled') projects = results[1].value.projects;
    if (results[2].status === 'fulfilled') tracks = results[2].value.tracks;
  }

  function change(edit: (project: Project) => void) {
    if (busy || recovery || !ready) return;
    const next = clone(graph);
    try { edit(next); } catch (error) { message = String(error); return; }
    if (JSON.stringify(next) === serialized) return;
    past = [...past.slice(-49), clone(graph)]; future = []; graph = next; serverErrors = [];
  }
  function undo() { if (!past.length || busy) return; future = [clone(graph), ...future]; graph = past.at(-1)!; past = past.slice(0, -1); serverErrors = []; }
  function redo() { if (!future.length || busy) return; past = [...past, clone(graph)]; graph = future[0]; future = future.slice(1); serverErrors = []; }
  function canReplace() { return ready && !busy && !recovery && (!dirty || window.confirm('Заменить текущий локальный черновик? Сначала экспортируйте JSON, чтобы сохранить отдельную копию. Активный проект устройства не изменяется.')); }
  function toggleFullscreen() { drawer = ''; fullscreen = !fullscreen; }
  function replace(project: Project, rev = 0, persisted = false) { graph = clone(project); revision = rev; savedId = persisted ? project.id : ''; saved = persisted ? JSON.stringify(project) : ''; past = []; future = []; selected = ''; selectedKind = ''; serverErrors = []; conflict = false; uncertain = false; }
  function create(template = false) { if (!canReplace()) return; replace(template ? dieselTemplate(nextId('diesel', projects.map(project => project.id))) : emptyProject(nextId('graph', projects.map(project => project.id)))); message = template ? 'Имена WAV в шаблоне — заполнители. Выберите/проверьте настоящие WAV перед применением. Звуки и привязки не изменялись.' : 'Новый локальный граф. Добавьте состояния и типизированные переходы.'; }
  function restore() { if (!recovery) return; try { const project = migrateV1toV2(parseProject(JSON.stringify(recovery.project), limits)); graph = project; saved = recovery.saved; revision = recovery.revision; savedId = project.id; uncertain = recovery.uncertain; conflict = false; recovery = undefined; message = 'Локальный черновик восстановлен. Переподключитесь и проверьте текущую версию сервера перед записью.'; } catch (error) { message = String(error); } }
  function select(id: string, kind: 'state' | 'edge') { selected = id; selectedKind = kind; drawer = 'inspector'; }
  function addState(silent: boolean) {
    const id = nextId('state', graph.states.map(state => state.id));
    change(project => { if (project.states.length >= limits.states) throw new Error('Достигнут лимит состояний устройства.'); project.states.push({ id, name: silent ? 'Тихое состояние' : 'Новый звук', file: silent ? '' : 'select_sample.wav', loop: false, volume: 80, rate: 1000 }); project.editor.positions[id] = { x: (project.states.length % 3) * 270 + 60, y: Math.floor(project.states.length / 3) * 190 + 80 }; }); select(id, 'state');
  }
  function connect(connection: Pick<Connection, 'source' | 'target'>) {
    if (!connection.source || !connection.target) return;
    const id = nextId('edge', graph.transitions.map(edge => edge.id));
    change(project => {
      if (project.transitions.length >= limits.transitions) throw new Error('Достигнут лимит переходов устройства.');
      const used = project.transitions.filter(edge => edge.source === connection.source).map(edge => edge.priority);
      let priority = 0; while (used.includes(priority)) priority++;
      project.transitions.push({ id, source: connection.source, target: connection.target, priority, timing: 'immediate', condition: { type: 'sample_done' } });
    }); select(id, 'edge');
  }
  function copySelection() {
    if (selectedState) {
      const original = selectedState, id = nextId('state', graph.states.map(state => state.id));
      change(project => { if (project.states.length >= limits.states) throw new Error('Достигнут лимит состояний устройства.'); project.states.push({ ...original, id, name: original.name.slice(0, 45) + ' копия' }); const position = positionOf(project, original.id) ?? { x: 0, y: 0 }; setPosition(project, id, { x: position.x + 60, y: position.y + 120 }); }); select(id, 'state');
    } else if (edge) {
      const original = clone(edge), id = nextId('edge', graph.transitions.map(edge => edge.id));
      change(project => { if (project.transitions.length >= limits.transitions) throw new Error('Достигнут лимит переходов устройства.'); const used = project.transitions.filter(edge => edge.source === original.source).map(edge => edge.priority); let priority = 0; while (used.includes(priority)) priority++; project.transitions.push({ ...original, id, priority }); }); select(id, 'edge');
    }
  }
  function removeSelection() { change(project => { if (selectedKind === 'state') deleteState(project, selected); else project.transitions = project.transitions.filter(edge => edge.id !== selected); }); if (!graph.states.some(state => state.id === selected) && !graph.transitions.some(edge => edge.id === selected)) { selected = ''; selectedKind = ''; } }
  function currentTheme(): 'dark' | 'light' { return localStorage.getItem('aura_theme') === 'light' ? 'light' : 'dark'; }
  function input(event: Event) { return (event.currentTarget as HTMLInputElement).value; }
  function stateField(field: 'name' | 'file' | 'loop' | 'volume' | 'rate', value: string | number | boolean) { change(project => { const state = project.states.find(state => state.id === selected); if (state) Object.assign(state, { [field]: value }); }); }
  function edgeField(field: string, value: string | number) { change(project => { const edge = project.transitions.find(edge => edge.id === selected); if (edge) Object.assign(edge, { [field]: value }); }); }
  function conditionType(type: ConditionType) { change(project => { const edge = project.transitions.find(edge => edge.id === selected); if (!edge) return; edge.condition = type.startsWith('fn_') ? { type, fn: 2 } : ['speed', 'accel', 'decel'].includes(type) ? { type, min: 0, max: 255 } : { type }; }); }
  function conditionField(field: 'fn' | 'min' | 'max', value: string) { change(project => { const edge = project.transitions.find(edge => edge.id === selected); if (!edge) return; if (value === '') delete edge.condition[field]; else edge.condition[field] = Number(value); }); }
  function addEffect() { change(project => { if (project.effects.length >= limits.effects) throw new Error('Достигнут лимит эффектов.'); const id = nextId('effect', project.effects.map(effect => effect.id)), entry = nextId('entry', project.states.map(state => state.id)); if (project.states.length >= limits.states) throw new Error('Достигнут лимит состояний.'); project.states.push({ id: entry, name: `${id} тихий вход`, file: '', loop: false, volume: 100, rate: 1000 }); project.editor.positions[entry] = { x: 60, y: project.states.length * 100 }; project.effects.push({ id, entry, fn: 2 }); }); }
  function fork() { const id = nextId('copy', projects.map(project => project.id).concat(graph.id)); change(project => { project.id = id; project.name = project.name.slice(0, 45) + ' копия'; }); revision = 0; savedId = ''; saved = ''; conflict = false; uncertain = false; message = 'Отдельная копия. Исходный граф/MDS совместимости остаётся без изменений.'; }

  async function operation(label: string, task: () => Promise<void>, writing = false) {
    if (busy || recovery || !ready) return; busy = label; serverErrors = [];
    try { if (writing) await persist(); await task(); }
    catch (error) {
      if (error instanceof ApiError) { serverErrors = error.diagnostics; if (writing && error.status === 409 && label === 'Сохранение') conflict = true; if (writing && error.status === 0) uncertain = true; }
      message = error instanceof Error ? error.message : String(error);
    } finally { if (writing) { try { await persist(); } catch { draftStatus = 'Локальное восстановление недоступно. Экспортируйте JSON перед закрытием.'; } } busy = ''; }
  }
  async function persist() { await writeDraft({ project: clone(graph), revision: effectiveRevision, saved, time: Date.now(), uncertain: uncertain || conflict }); }
  async function load() {
    if (!projectChoice || !canReplace()) return;
    await operation('Загрузка', async () => { const response = await api.project(projectChoice); const project = migrateV1toV2(parseProject(JSON.stringify(response.project), limits)); replace(project, response.revision, true); message = `Загружено «${project.name}», версия ${response.revision}. Активное воспроизведение не изменялось.`; });
  }
  async function validate() { await operation('Проверка', async () => { const response = await api.validate(devicePayload(graph)); serverErrors = response.diagnostics; message = response.valid ? 'Проверка устройства пройдена. Требуемые WAV всё ещё проверяются при применении.' : 'Устройство отклонило этот граф. Выберите диагностику, чтобы просмотреть поля.'; }); }
  async function save() {
    if (!compatible || conflict || uncertain || validateProject(graph, limits).some(error => error.severity !== 'warning')) return;
    await operation('Сохранение', async () => { const snapshot = clone(graph); const response = await api.save(devicePayload(snapshot), effectiveRevision); revision = response.revision; savedId = snapshot.id; saved = JSON.stringify(snapshot); message = `Сохранена версия ${revision}. Не применена; активный проект не изменён.`; void refresh(); }, true);
  }
  async function apply() {
    if (!compatible || dirty || !effectiveRevision || applyErrors.length || conflict || uncertain) return;
    if (!window.confirm(`Применить сохранённую версию ${effectiveRevision}? Двигатель должен быть остановлен. Это изменяет только звуковой проект, но не команды двигателя/AUX.`)) return;
    await operation('Применение', async () => { await api.apply(graph.id, effectiveRevision); message = `Применена версия ${effectiveRevision}. Двигатель/эффекты ждут новых событий функций.`; runtime = null; }, true);
  }
  async function inspect(file: string, stateId?: string) {
    if (!validFile(file)) { message = 'Используйте простое ASCII-имя WAV-файла: буквы, цифры, подчёркивание или дефис, с окончанием .wav.'; return; }
    await operation('Проверка WAV', async () => { const response = await api.asset(file); const next = clone(graph); if (stateId) { const state = next.states.find(state => state.id === stateId); if (state) state.file = file; } next.assets = next.assets.filter(asset => asset.file !== file); next.assets.push(response.asset); past = [...past.slice(-49), clone(graph)]; future = []; graph = next; message = `Проверен ${file}: ${response.asset.sampleRate} Гц, каналов: ${response.asset.channels}, ${response.asset.durationMs} мс.`; });
  }
  async function upload(event: Event) {
    const file = (event.currentTarget as HTMLInputElement).files?.[0]; uploadInput.value = ''; if (!file) return;
    if (!validFile(file.name)) { message = 'Переименуйте WAV в ASCII-имя, используя буквы, цифры, подчёркивание или дефис.'; return; }
    if (tracks.some(track => track.file.replace(/^audio\//, '') === file.name) || graph.assets.some(asset => asset.file === file.name)) { message = 'Имя файла уже существует. Загрузите новое имя; существующие ресурсы здесь не перезаписываются.'; return; }
    if (!window.confirm(`Загрузить ${file.name} в свободный слот дорожки? Используйте собственное/лицензированное аудио. Воспроизведение не начнётся.`)) return;
    await operation('Загрузка WAV', async () => { await api.upload(file); message = 'WAV загружен. Обновите дорожки, затем выберите/проверьте его, чтобы привязать манифест.'; const response = await api.tracks(); tracks = response.tracks; }, true);
  }
  async function importJson(event: Event) {
    const file = (event.currentTarget as HTMLInputElement).files?.[0]; importInput.value = ''; if (!file || !canReplace()) return;
    if (file.size > limits.jsonBytes) { message = 'Импорт превышает лимит JSON устройства.'; return; }
    try { const project = migrateV1toV2(parseProject(await file.text(), limits)); replace(project); message = 'JSON импортирован как несохранённый черновик. Данные WAV не включены; проверьте файлы на этом устройстве. Сохранение/применение никогда не выполняются автоматически.'; }
    catch (error) { message = error instanceof Error ? error.message : String(error); }
  }
  function exportJson() { const blob = new Blob([JSON.stringify(graph, null, 2)], { type: 'application/json' }), url = URL.createObjectURL(blob); const anchor = document.createElement('a'); anchor.href = url; anchor.download = `${graph.id}.json`; anchor.click(); setTimeout(() => URL.revokeObjectURL(url), 1000); message = 'Экспортирован только JSON графа. Аудио WAV нужно передавать отдельно.'; }
  function key(event: KeyboardEvent) {
    if (event.target instanceof HTMLElement && (event.target.closest('input,select,textarea') || event.target.isContentEditable)) return;
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'z') { event.preventDefault(); event.shiftKey ? redo() : undo(); }
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'y') { event.preventDefault(); redo(); }
    if (event.key === 'Delete' && selected) { event.preventDefault(); removeSelection(); }
    if (event.key === 'Escape') { drawer = ''; fullscreen = false; }
  }
</script>

<svelte:window onkeydown={key} />
<svelte:head><title>Редактор звукового графа</title><meta name="description" content="Автономный редактор звукового графа декодера. Команды двигателя и AUX не отправляются." /></svelte:head>

<main class:fullscreen aria-busy={!!busy}>
  <header class="topbar">
    <div class="brand">AURA-X <span>ЗВУКОВОЙ ГРАФ</span></div>
    <div class="status"><i class:online></i>{online ? 'Подключено' : 'Нет связи / API недоступен'} <strong>{dirty ? 'Не сохранено' : `Сохранено r${effectiveRevision}`}</strong></div>
    <a class="button" href="/">Интерфейс декодера</a>
  </header>
  <nav class="toolbar" aria-label="Действия с проектом">
    <button class:active={tab === 'patch'} onclick={() => tab = 'patch'}>Пульт</button>
    <button class:active={tab === 'effects'} onclick={() => tab = 'effects'}>Эффекты</button>
    <button class:active={tab === 'graph'} onclick={() => tab = 'graph'}>Граф (эксперт)</button>
    <button disabled={!!busy || !!recovery} onclick={() => create()}>Новая</button>
    <button disabled={!!busy || !!recovery} onclick={() => create(true)}>Шаблон «Тепловоз + F2»</button>
    <button disabled={!past.length || !!busy} onclick={undo}>Отменить</button><button disabled={!future.length || !!busy} onclick={redo}>Повторить</button>
    <button disabled={!!busy || !!recovery} onclick={() => importInput.click()}>Импорт JSON</button><button onclick={exportJson}>Экспорт JSON</button>
    <button disabled={!!busy || !!recovery} onclick={validate}>Проверить на устройстве</button>
    <button class="primary" disabled={!ready || !compatible || !!busy || !!recovery || conflict || uncertain || diagnostics.some(error => error.severity !== 'warning')} onclick={save}>Сохранить черновик</button>
    <button class="apply" disabled={!ready || !compatible || !!busy || !!recovery || dirty || !effectiveRevision || conflict || uncertain || !!applyErrors.length} onclick={apply}>Применить сохранённую версию</button>
    <button class="mobile-action" onclick={() => drawer = drawer === 'palette' ? '' : 'palette'}>Проект / палитра</button>
    <button class="mobile-action" onclick={() => drawer = drawer === 'inspector' ? '' : 'inspector'}>Инспектор</button>
    <button onclick={toggleFullscreen}>{fullscreen ? 'Выйти из полного экрана' : 'Полный экран'}</button>
  </nav>
  <input class="hidden" type="file" accept=".json,application/json" bind:this={importInput} onchange={importJson} />
  <input class="hidden" type="file" accept=".wav,audio/wav" bind:this={uploadInput} onchange={upload} />
  {#if recovery}<div class="banner warning" role="status">Доступен локальный черновик от {new Date(recovery.time).toLocaleString()}. Записи на устройство не выполнялись.<button onclick={restore}>Восстановить черновик</button><button onclick={() => { recovery = undefined; message = 'Восстановление отменено явно.'; }}>Отменить восстановление</button></div>{/if}
  {#if conflict || uncertain}<div class="banner danger" role="alert">{conflict ? 'Конфликт версий: другая вкладка/операция на устройстве изменила этот проект.' : 'Результат записи неизвестен после потери связи.'} Экспортируйте этот черновик, перезагрузите версию сервера или сохраните отдельную копию. Автоматическая перезапись отсутствует.<button disabled={!!busy} onclick={fork}>Сохранить как отдельную копию</button></div>{/if}
  <div class="notice" role="status">{busy ? `${busy}... ` : ''}{message}</div>
  {#if tab === 'effects'}
    <div class="workspace single"><EffectsPanel {graph} mutate={change} {tracks} /></div>
  {:else if tab === 'patch'}
    <div class="workspace single"><PatchPanel {graph} mutate={change} /></div>
  {:else}
  <div class="workspace">
    <aside class="palette" class:open={drawer === 'palette'} aria-label="Проект и палитра">
      <div class="drawer-heading"><h2>Проект / палитра</h2><button onclick={() => drawer = ''}>Закрыть</button></div>
      <fieldset disabled={!ready || !!busy || !!recovery}>
        <h2>Проект</h2>
        <label>ID проекта<input aria-label="ID проекта" value={graph.id} maxlength="31" onchange={event => change(project => project.id = input(event))} /></label>
        <label>Имя проекта<input aria-label="Имя проекта" value={graph.name} onchange={event => change(project => project.name = input(event))} /></label>
        <small>Имена: 63 байта UTF-8. Изменение ID создаёт новую историю версий.</small>
        <label>Сохранённые проекты<select aria-label="Сохранённые проекты" bind:value={projectChoice}><option value="">Выбрать проект</option>{#each projects as project}<option value={project.id}>{project.name} / r{project.revision}</option>{/each}</select></label>
        <div class="row"><button disabled={!projectChoice} onclick={load}>Загрузить выбранный</button><button onclick={() => void refresh()}>Обновить</button></div>
        <button onclick={fork}>Копировать проект</button>
        <h2>Двигатель</h2>
        <label>Функция двигателя<select aria-label="Функция двигателя" value={graph.engine.fn} onchange={event => change(project => project.engine.fn = Number(input(event)))}>{#each functions as fn}<option value={fn}>F{fn}</option>{/each}</select></label>
        <label>Тихий вход двигателя<select aria-label="Тихий вход двигателя" value={graph.engine.entry} onchange={event => change(project => project.engine.entry = input(event))}>{#each graph.states.filter(state => !state.file) as state}<option value={state.id}>{state.name}</option>{/each}</select></label>
        <label>Гистерезис скорости<input aria-label="Гистерезис скорости" type="number" min="0" max="32" value={graph.hysteresis} onchange={event => change(project => project.hysteresis = Number(input(event)))} /></label>
        <small>Нормализованная скорость 0..255. Состояние меняется не чаще одного раза за тик 20 мс. Двигатель переключается по новому нажатию функции.</small>
        <h2>Палитра</h2><div class="row"><button onclick={() => addState(false)}>Добавить звуковое состояние</button><button onclick={() => addState(true)}>Добавить тихое состояние</button></div>
        <h2>Независимые эффекты</h2>
        {#each graph.effects as effect}
          <div class="effect-box">
            <strong>{effect.id}</strong>
            <label>Функция эффекта<select aria-label={`Функция эффекта ${effect.id}`} value={effect.fn} onchange={event => change(project => project.effects.find(item => item.id === effect.id)!.fn = Number(input(event)))}>{#each functions as fn}<option value={fn}>F{fn}</option>{/each}</select></label>
            <label>Тихий вход эффекта<select aria-label={`Тихий вход эффекта ${effect.id}`} value={effect.entry} onchange={event => change(project => project.effects.find(item => item.id === effect.id)!.entry = input(event))}>{#each graph.states.filter(state => !state.file) as state}<option value={state.id}>{state.name}</option>{/each}</select></label>
            <button onclick={() => change(project => project.effects = project.effects.filter(item => item.id !== effect.id))}>Удалить привязку эффекта</button>
          </div>
        {/each}
        <button onclick={addEffect}>Добавить эффект и вход</button>
        <small>Общий пул голосов; эффект никогда не забирает голос двигателя. Привязки OUTPUT/LOGIC не редактируются.</small>
        <h2>Соединить без перетаскивания</h2>
        <label>Исходное состояние<select aria-label="Источник соединения" bind:value={from}><option value="">Выбрать источник</option>{#each graph.states as state}<option value={state.id}>{state.name}</option>{/each}</select></label>
        <label>Целевое состояние<select aria-label="Цель соединения" bind:value={to}><option value="">Выбрать цель</option>{#each graph.states as state}<option value={state.id}>{state.name}</option>{/each}</select></label>
        <button disabled={!from || !to} onclick={() => connect({ source: from, target: to })}>Добавить переход</button>
        <h2>Библиотека WAV</h2><button onclick={() => uploadInput.click()}>Загрузить WAV</button>
        <small>Рекомендуется PCM16 моно 22050 Гц. Только собственное/лицензированное аудио. Загрузка не начинает воспроизведение.</small>
        {#each graph.assets.filter(asset => !graph.states.some(state => state.file === asset.file)) as asset}<button onclick={() => change(project => project.assets = project.assets.filter(item => item.file !== asset.file))}>Удалить неиспользуемый манифест: {asset.file}</button>{/each}
        <small>Удаление метаданных неиспользуемого манифеста не удаляет WAV-файлы с устройства.</small>
      </fieldset>
    </aside>
    <section class="canvas" aria-label="Холст звукового графа">
      <SvelteFlow bind:nodes bind:edges bind:viewport {nodeTypes} colorMode={theme} minZoom={0.1} maxZoom={4} nodesDraggable={!busy && !recovery} nodesConnectable={!busy && !recovery} deleteKey={null}
        onnodeclick={({ node }) => select(node.id, 'state')} onedgeclick={({ edge }) => select(edge.id, 'edge')}
        onconnect={connect} onnodedragstart={() => interacting = true} onnodedragstop={({ nodes: moved }) => { change(project => { for (const node of moved) setPosition(project, node.id, { x: node.position.x, y: node.position.y }); }); interacting = false; }}
        onmovestart={() => interacting = true} onmoveend={(_event, view) => { change(project => project.editor.viewport = { x: view.x, y: view.y, zoom: view.zoom }); interacting = false; }}>
        <Background gap={24} /><Controls />
      </SvelteFlow>
      <div class="canvas-info">{graph.states.length}/{limits.states} состояний / {graph.transitions.length}/{limits.transitions} переходов / {graph.effects.length}/{limits.effects} эффектов <span>{runtime ? `Скорость ${runtime.speed ?? '?'} / 255` : 'Нет телеметрии'}</span></div>
    </section>
    <aside class="inspector" class:open={drawer === 'inspector'} aria-label="Инспектор">
      <div class="drawer-heading"><h2>Инспектор</h2><button onclick={() => drawer = ''}>Закрыть</button></div>
      <fieldset disabled={!ready || !!busy || !!recovery}>
        <h2>Инспектор</h2>
        <label>Просмотр состояния<select aria-label="Просмотр состояния" value={selectedKind === 'state' ? selected : ''} onchange={event => select(input(event), 'state')}><option value="">Выбрать состояние</option>{#each graph.states as state}<option value={state.id}>{state.name} ({state.id})</option>{/each}</select></label>
        <label>Просмотр перехода<select aria-label="Просмотр перехода" value={selectedKind === 'edge' ? selected : ''} onchange={event => select(input(event), 'edge')}><option value="">Выбрать переход</option>{#each graph.transitions as edge}<option value={edge.id}>{edge.source} -&gt; {edge.target} ({edge.id})</option>{/each}</select></label>
        {#if selectedState}
          <h3>{selectedState.id}</h3><label>Имя состояния<input aria-label="Имя состояния" value={selectedState.name} onchange={event => stateField('name', input(event))} /></label>
          <label>Имя файла WAV<input aria-label="Имя файла WAV" value={selectedState.file} placeholder="Тишина (пусто) или sample.wav" onchange={event => stateField('file', input(event))} /></label>
          <label>WAV на устройстве<select aria-label="WAV на устройстве" value={tracks.some(track => track.file.replace(/^audio\//, '') === selectedState.file) ? selectedState.file : ''} onchange={event => { const file = input(event); if (file) void inspect(file, selectedState!.id); }}><option value="">Выбрать / проверить WAV</option>{#each tracks.filter(track => validFile(track.file.replace(/^audio\//, ''))) as track}<option value={track.file.replace(/^audio\//, '')}>{track.label || track.file}</option>{/each}</select></label>
          <button disabled={!selectedState.file} onclick={() => inspect(selectedState!.file, selectedState!.id)}>Проверить имя файла / обновить манифест</button>
          <label class="check"><input type="checkbox" aria-label="Зациклить сэмпл" checked={selectedState.loop} onchange={event => stateField('loop', event.currentTarget.checked)} />Зациклить сэмпл</label>
          <label>Громкость 0..100<input aria-label="Громкость состояния" type="number" min="0" max="100" value={selectedState.volume} onchange={event => stateField('volume', Number(input(event)))} /></label>
          <label>Скорость воспроизведения (‰) 500..3000<input aria-label="Скорость воспроизведения состояния" type="number" min="500" max="3000" step="50" value={selectedState.rate} onchange={event => stateField('rate', Number(input(event)))} /></label>
          {#each graph.assets.filter(asset => asset.file === selectedState?.file) as asset}<div class="manifest">{asset.sampleRate} Гц / {asset.channels} кан. / {asset.bits} бит<br />{asset.durationMs} мс / {asset.size} байт<br />CRC32 {asset.crc32}</div>{/each}
          <small>Пустой файл — намеренная тишина. Входные состояния должны оставаться тихими. Тихий sample_done переходит на следующий тик.</small>
        {:else if edge}
          <h3>{edge.id}</h3>
          <label>Источник перехода<select aria-label="Источник перехода" value={edge.source} onchange={event => edgeField('source', input(event))}>{#each graph.states as state}<option value={state.id}>{state.name}</option>{/each}</select></label>
          <label>Цель перехода<select aria-label="Цель перехода" value={edge.target} onchange={event => edgeField('target', input(event))}>{#each graph.states as state}<option value={state.id}>{state.name}</option>{/each}</select></label>
          <label>Условие<select aria-label="Условие" value={edge.condition.type} onchange={event => conditionType(input(event) as ConditionType)}>{#each CONDITIONS as condition}<option value={condition}>{conditionName(condition)}</option>{/each}</select></label>
          {#if edge.condition.type.startsWith('fn_')}<label>Функция условия<select aria-label="Функция условия" value={edge.condition.fn} onchange={event => conditionField('fn', input(event))}>{#each functions as fn}<option value={fn}>F{fn}</option>{/each}</select></label>{/if}
          {#if ['speed', 'accel', 'decel'].includes(edge.condition.type)}<div class="row"><label>Минимум<input aria-label="Минимум условия" type="number" min="0" max="255" value={edge.condition.min ?? ''} onchange={event => conditionField('min', input(event))} /></label><label>Максимум<input aria-label="Максимум условия" type="number" min="0" max="255" value={edge.condition.max ?? ''} onchange={event => conditionField('max', input(event))} /></label></div><small>Нормализованная скорость {runtime?.speed ?? '?'} / 255, гистерезис {graph.hysteresis}. Диапазон входа {edge.condition.min ?? 0}..{edge.condition.max ?? 255}; диапазон выхода {Math.max(0, (edge.condition.min ?? 0) - graph.hysteresis)}..{Math.min(255, (edge.condition.max ?? 255) + graph.hysteresis)}. Ускорение/замедление используют отфильтрованную величину.</small>{/if}
          <label>Приоритет (побеждает больший)<input aria-label="Приоритет перехода" type="number" min="0" max="255" value={edge.priority} onchange={event => edgeField('priority', Number(input(event)))} /></label>
          <label>Момент срабатывания<select aria-label="Момент срабатывания перехода" value={edge.timing} onchange={event => edgeField('timing', input(event))}><option value="immediate">Немедленно</option><option value="after_sample">По границе сэмпла</option></select></label>
          <small>Уникальный приоритет для каждого источника. Срабатывание по границе сэмпла перепроверяется на границе; нажатие/отпускание должно произойти на этом тике. Для удержания/отпускания предпочитайте fn вкл/выкл. Немедленное срабатывание освобождает только голос этого канала.</small>
        {:else}<p>Выберите состояние или переход. Используйте селекторы здесь, если управление холстом неудобно на телефоне.</p>{/if}
        <div class="row"><button disabled={!selectedState && !edge} onclick={copySelection}>Копировать выбранное</button><button class="danger-button" disabled={!selectedState && !edge} onclick={removeSelection}>Удалить выбранное</button></div>
      </fieldset>
      <h2>Проверка / Диагностика</h2>
      <p>{diagnostics.filter(error => error.severity !== 'warning').length} ошибок / {diagnostics.filter(error => error.severity === 'warning').length} предупреждений. Приоритет у устройства.</p>
      <div class="diagnostics" role="log">
        {#each diagnostics as error}<button class:warning={error.severity === 'warning'} onclick={() => { if (graph.states.some(state => state.id === error.id)) select(error.id, 'state'); else if (graph.transitions.some(edge => edge.id === error.id)) select(error.id, 'edge'); }}><strong>{error.id || 'Проект'} {error.field}</strong><span>{error.message}</span></button>{/each}
      </div>
      {#if runtime?.failedChannels}<p class="warning-text">Маска сбоев аудио/каналов во время выполнения: {runtime.failedChannels}. Канал безопасно остановлен; проверьте WAV/диагностику устройства.</p>{/if}
      {#if runtime?.fault}<p class="warning-text" role="alert">Выбранный граф устройства отсутствует/повреждён. Звук остаётся безопасно заблокированным. Примените допустимый сохранённый проект при остановленном двигателе.</p><button onclick={() => drawer = 'palette'}>Открыть действия восстановления</button>{/if}
      {#if runtime?.active && !activeHere}<small>Активный проект устройства: {runtime.id} r{runtime.revision}. Подсветка скрыта для другой/несохранённой версии.</small>{/if}
    </aside>
  </div>
  {/if}
  <footer><span>{draftStatus} / JSON {byteLength(serialized)} / {limits.jsonBytes} байт</span><span>Экспорт JSON не включает WAV / <a href="./THIRD_PARTY_NOTICES.txt">Сторонние лицензии</a></span></footer>
</main>
