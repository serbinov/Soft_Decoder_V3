import { expect, test, type Page } from '@playwright/test';
import { DEFAULT_LIMITS, clone, emptyProject, type Project } from '../../src/model';

async function mockDevice(page: Page) {
  const device = { project: emptyProject('stored'), revision: 4, saveConflict: false, loseSave: false, offline: false, active: false, fault: false, legacyEmpty: false, legacyError: '', cancelSwitch: false, switchConfirmations: 0, switchRequests: [] as { method: string; body: string | null; query: string }[], switchDrafts: [] as Project[], pollTimes: [] as number[], writes: [] as string[], requests: [] as string[], errors: [] as string[] };
  device.project.name = 'Stored project';
  page.on('pageerror', error => device.errors.push(error.message));
  page.on('request', request => device.requests.push(request.url()));
  page.on('dialog', dialog => {
    const switching = /^Активировать (MDS|NONE)/.test(dialog.message());
    if (switching) { device.switchConfirmations++; expect(dialog.message()).toContain('Двигатель должен быть остановлен'); expect(dialog.message()).toContain('Команды двигателя/AUX не отправляются'); }
    void (switching && device.cancelSwitch ? dialog.dismiss() : dialog.accept());
  });
  await page.route('**/api/**', async route => {
    const request = route.request(), url = new URL(request.url()), path = url.pathname;
    const answer = (data: unknown, status = 200) => route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(data) });
    if (request.method() === 'POST') device.writes.push(path);
    if (device.offline) return route.abort('failed');
    if (path.endsWith('/capabilities')) return answer({ ok: true, format: 'sound-graph', schemaVersion: 1, limits: DEFAULT_LIMITS });
    if (path === '/api/sound/projects') return answer({ ok: true, active: device.active ? '' : 'original_mds', count: device.legacyEmpty ? 0 : 1, projects: device.legacyEmpty ? [] : [{ name: 'original_mds', active: !device.active }] });
    if (path.endsWith('/projects')) return answer({ ok: true, projects: [{ id: device.project.id, name: device.project.name, revision: device.revision }] });
    if (path.endsWith('/project')) return answer({ ok: true, project: device.project, revision: device.revision });
    if (path === '/api/audio/tracks') return answer({ ok: true, tracks: [{ slot: 1, file: 'audio/idle.wav', label: 'Idle WAV', enabled: true }, { slot: 2, file: 'audio/engine_start.wav', label: 'Start WAV', enabled: true }] });
    if (path.endsWith('/asset')) return answer({ ok: true, asset: { file: url.searchParams.get('file'), size: 44144, crc32: '12345678', sampleRate: 22050, channels: 1, bits: 16, durationMs: 1000 } });
    if (path.endsWith('/state')) { device.pollTimes.push(Date.now()); return answer({ ok: true, active: device.active, fault: device.fault, id: device.project.id, revision: device.revision, engine: false, armed: true, states: [device.project.states.length > 1 ? 1 : 0], failedChannels: 0, speed: 0 }); }
    if (path.endsWith('/validate')) return answer({ ok: true, valid: false, diagnostics: [{ code: 'test', id: 'state_1', field: 'volume', message: 'Mock server diagnostic for state volume' }] });
    if (path.endsWith('/save')) {
      if (device.loseSave) return route.abort('failed');
      if (device.saveConflict) return answer({ ok: false, error: 'Stale revision conflict' }, 409);
      device.project = request.postDataJSON() as Project;
      device.revision = Number(url.searchParams.get('expectedRevision')) + 1;
      return answer({ ok: true, revision: device.revision });
    }
    if (path.endsWith('/apply')) { expect(request.postData()).toBeNull(); device.active = true; return answer({ ok: true }); }
    if (path === '/api/sound/graph/legacy') {
      device.switchRequests.push({ method: request.method(), body: request.postData(), query: url.search });
      device.switchDrafts.push(await page.evaluate(() => new Promise<Project>((resolve, reject) => { const open = indexedDB.open('decoder-sound-editor', 1); open.onerror = () => reject(open.error); open.onsuccess = () => { const db = open.result, get = db.transaction('drafts').objectStore('drafts').get('current'); get.onsuccess = () => { resolve(get.result.project); db.close(); }; get.onerror = () => { reject(get.error); db.close(); }; }; })));
      if (device.legacyError) return answer({ ok: false, error: device.legacyError }, 409);
      device.active = false; device.fault = false; return answer({ ok: true });
    }
    if (path === '/api/sound/scheme') return answer({ ok: true, name: 'original_mds', type: 2, tables: [{ legacy: true }] });
    return answer({ ok: false, error: `Unexpected API ${path}` }, 404);
  });
  return device;
}

async function start(page: Page) {
  await page.goto('/sound-editor/');
  await expect(page.getByText(/Локальное восстановление (готово|сохранено)/)).toBeVisible();
}
async function panel(page: Page, name: 'palette' | 'inspector') {
  if ((page.viewportSize()?.width ?? 0) > 800) return;
  if (await page.locator(`aside.${name}.open`).isVisible()) return;
  const close = page.getByRole('button', { name: 'Закрыть', exact: true });
  if (await close.first().isVisible()) await close.first().click();
  await page.getByRole('button', { name: name === 'palette' ? 'Проект / палитра' : 'Инспектор', exact: true }).click();
}
async function closePanel(page: Page) {
  const close = page.locator('aside.open .drawer-heading button');
  if (await close.isVisible()) await close.click();
}
async function update(page: Page, label: string, value: string) { const input = page.getByRole('textbox', { name: label, exact: true }); await input.fill(value); await input.press('Tab'); }
function assertLocal(device: Awaited<ReturnType<typeof mockDevice>>) {
  expect(device.errors).toEqual([]);
  expect(device.requests.every(url => url.startsWith('http://127.0.0.1:4173/'))).toBeTruthy();
  expect(device.writes.every(path => ['/api/sound/graph/save', '/api/sound/graph/apply', '/api/sound/graph/validate', '/api/sound/graph/legacy'].includes(path))).toBeTruthy();
}

test('template, copy/delete undo, device WAV, alternative connection, Save then Apply and active state', async ({ page }) => {
  const device = await mockDevice(page); await start(page);
  await page.getByRole('button', { name: 'Шаблон «Тепловоз + F2»', exact: true }).click();
  await expect(page.locator('.canvas-info')).toContainText('13/57');
  await expect(page.getByRole('button', { name: 'Применить сохранённую версию', exact: true })).toBeDisabled();
  await panel(page, 'inspector');
  await page.getByRole('combobox', { name: 'Просмотр состояния', exact: true }).selectOption('start');
  await update(page, 'Имя состояния', 'Own engine start');
  await page.getByRole('button', { name: 'Копировать выбранное', exact: true }).click();
  await expect(page.locator('.canvas-info')).toContainText('14/57');
  await page.getByRole('button', { name: 'Удалить выбранное', exact: true }).click();
  await expect(page.locator('.canvas-info')).toContainText('13/57');
  await closePanel(page); await page.getByRole('button', { name: 'Отменить', exact: true }).click();
  await expect(page.locator('.canvas-info')).toContainText('14/57');
  await page.getByRole('button', { name: 'Повторить', exact: true }).click();
  await expect(page.locator('.canvas-info')).toContainText('13/57');
  await page.getByRole('button', { name: 'Новая', exact: true }).click();
  await panel(page, 'palette');
  await page.getByRole('combobox', { name: 'Функция двигателя', exact: true }).selectOption('28');
  await page.getByRole('button', { name: 'Добавить звуковое состояние', exact: true }).click();
  await panel(page, 'inspector');
  await page.getByRole('combobox', { name: 'WAV на устройстве', exact: true }).selectOption('idle.wav');
  await expect(page.locator('.manifest')).toContainText('CRC32 12345678');
  await panel(page, 'palette');
  await page.getByRole('combobox', { name: 'Источник соединения', exact: true }).selectOption('off');
  await page.getByRole('combobox', { name: 'Цель соединения', exact: true }).selectOption('state_1');
  await page.getByRole('button', { name: 'Добавить переход', exact: true }).click();
  await panel(page, 'inspector');
  await page.getByRole('combobox', { name: 'Условие', exact: true }).selectOption('fn_press');
  await page.getByRole('combobox', { name: 'Функция условия', exact: true }).selectOption('28');
  await closePanel(page);
  await page.getByRole('button', { name: 'Сохранить черновик', exact: true }).click();
  await expect(page.getByRole('status').filter({ hasText: 'Сохранена версия 1' })).toBeVisible();
  expect(device.project.engine.fn).toBe(28); expect(device.project.transitions[0].condition.fn).toBe(28);
  expect(device.project.assets[0].file).toBe('idle.wav');
  expect(device.writes).toEqual(['/api/sound/graph/save']);
  await page.getByRole('button', { name: 'Применить сохранённую версию', exact: true }).click();
  await expect(page.locator('.sound-node.live')).toHaveCount(1, { timeout: 6000 });
  expect(device.writes).toEqual(['/api/sound/graph/save', '/api/sound/graph/apply']);
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBeTruthy();
  assertLocal(device);
});

test('revision conflict preserves draft, blocks overwrite, explicit copy starts revision zero', async ({ page }) => {
  const device = await mockDevice(page); await start(page); await panel(page, 'palette');
  await page.getByRole('combobox', { name: 'Сохранённые проекты', exact: true }).selectOption('stored');
  await page.getByRole('button', { name: 'Загрузить выбранный', exact: true }).click();
  await update(page, 'Имя проекта', 'My unsaved edit'); await closePanel(page);
  device.saveConflict = true;
  await page.getByRole('button', { name: 'Сохранить черновик', exact: true }).click();
  await expect(page.getByRole('alert')).toContainText('Конфликт версий');
  await expect(page.getByRole('button', { name: 'Сохранить черновик', exact: true })).toBeDisabled();
  await expect(page.getByRole('button', { name: 'Применить сохранённую версию', exact: true })).toBeDisabled();
  await page.getByRole('button', { name: 'Сохранить как отдельную копию', exact: true }).click();
  device.saveConflict = false;
  await page.getByRole('button', { name: 'Сохранить черновик', exact: true }).click();
  await expect(page.getByRole('status').filter({ hasText: 'Сохранена версия 1' })).toBeVisible();
  expect(device.project.id).toBe('copy_1'); expect(device.project.name).toContain('My unsaved edit');
  assertLocal(device);
});

test('offline reload offers IndexedDB recovery, failed save is not retried', async ({ page }) => {
  const device = await mockDevice(page); await start(page); await panel(page, 'palette');
  await update(page, 'Имя проекта', 'Recover offline'); await closePanel(page);
  await expect(page.getByText(/Локальное восстановление сохранено/)).toBeVisible();
  device.loseSave = true; await page.getByRole('button', { name: 'Сохранить черновик', exact: true }).click();
  await expect(page.getByRole('alert')).toContainText('Результат записи неизвестен');
  await expect(page.getByRole('button', { name: 'Сохранить черновик', exact: true })).toBeDisabled();
  device.offline = true;
  await page.reload(); await expect(page.getByRole('button', { name: 'Восстановить черновик', exact: true })).toBeVisible();
  await page.getByRole('button', { name: 'Восстановить черновик', exact: true }).click();
  await panel(page, 'palette'); await expect(page.getByRole('textbox', { name: 'Имя проекта', exact: true })).toHaveValue('Recover offline');
  await closePanel(page); await expect(page.getByRole('button', { name: 'Сохранить черновик', exact: true })).toBeDisabled();
  expect(device.writes).toEqual(['/api/sound/graph/save']);
  assertLocal(device);
});

test('malformed imports never replace draft; missing WAV import and legacy remain explicitly unapplied', async ({ page }) => {
  const device = await mockDevice(page); await start(page);
  const file = page.locator('input[type=file][accept*="json"]');
  await file.setInputFiles({ name: 'bad.json', mimeType: 'application/json', buffer: Buffer.from('{"format":"sound-graph","format":"other"}') });
  await expect(page.getByRole('status').filter({ hasText: 'Дублирующийся ключ JSON' })).toBeVisible();
  const missing = clone(emptyProject('imported'));
  missing.states.push({ id: 'sample', name: 'Missing sample', file: 'missing.wav', loop: true, volume: 90, rate: 1000 });
  missing.transitions.push({ id: 'start', source: 'off', target: 'sample', priority: 1, timing: 'immediate', condition: { type: 'engine_on' } });
  await file.setInputFiles({ name: 'graph.json', mimeType: 'application/json', buffer: Buffer.from(JSON.stringify(missing)) });
  await expect(page.locator('.canvas-info')).toContainText('2/57');
  await page.getByRole('button', { name: 'Сохранить черновик', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Применить сохранённую версию', exact: true })).toBeDisabled();
  await panel(page, 'palette'); await page.getByRole('button', { name: 'Показать текущую схему', exact: true }).click();
  await expect(page.locator('.legacy-view')).toContainText('original_mds');
  await expect(page.getByText('Только чтение. Копия-шаблон не является точной миграцией. Исходный MDS и звуковые привязки остаются без изменений.')).toBeVisible();
  expect(device.writes).toEqual(['/api/sound/graph/save']); assertLocal(device);
});

test('server diagnostic maps to inspector, fullscreen drawer and polling are bounded', async ({ page }) => {
  const device = await mockDevice(page); await start(page); await panel(page, 'palette');
  await page.getByRole('button', { name: 'Добавить звуковое состояние', exact: true }).click(); await closePanel(page);
  await page.getByRole('button', { name: 'Проверить на устройстве', exact: true }).click();
  await panel(page, 'inspector'); await page.getByRole('button').filter({ hasText: 'Mock server diagnostic for state volume' }).click();
  await expect(page.getByRole('spinbutton', { name: 'Громкость состояния', exact: true })).toBeVisible(); await closePanel(page);
  await page.getByRole('button', { name: 'Полный экран', exact: true }).click();
  await page.getByRole('button', { name: 'Инспектор', exact: true }).click();
  await expect(page.locator('aside.inspector.open')).toBeVisible(); await closePanel(page);
  await page.getByRole('button', { name: 'Выйти из полного экрана', exact: true }).click();
  await expect.poll(() => device.pollTimes.length, { timeout: 7000 }).toBeGreaterThanOrEqual(3);
  for (let i = 1; i < device.pollTimes.length; i++) expect(device.pollTimes[i] - device.pollTimes[i - 1]).toBeGreaterThanOrEqual(1900);
  assertLocal(device);
});

test('drag/zoom update local editor metadata only, export contains no WAV body', async ({ page }) => {
  const device = await mockDevice(page); await start(page);
  const node = page.locator('.svelte-flow__node[data-id="off"]');
  const before = await node.getAttribute('style');
  const box = await node.boundingBox();
  if (!box) throw new Error('Node is missing');
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.mouse.down(); await page.mouse.move(box.x + box.width / 2 + 45, box.y + box.height / 2 + 70, { steps: 8 }); await page.mouse.up();
  await closePanel(page);
  await expect(node).not.toHaveAttribute('style', before!);
  await page.getByRole('button', { name: 'Zoom In', exact: true }).click();
  await page.getByRole('button', { name: 'Fit View', exact: true }).click();
  const downloadPromise = page.waitForEvent('download'); await page.getByRole('button', { name: 'Экспорт JSON', exact: true }).click();
  const download = await downloadPromise, stream = await download.createReadStream();
  if (!stream) throw new Error('Download failed');
  const chunks: Buffer[] = []; for await (const chunk of stream) chunks.push(chunk);
  const exported = JSON.parse(Buffer.concat(chunks).toString());
  expect(exported.editor.positions.off).not.toEqual({ x: 60, y: 80 });
  expect(exported.editor.viewport.zoom).not.toBe(0.8);
  expect(exported.states[0].file).toBe(''); expect(exported.assets).toEqual([]);
  expect(device.writes).toEqual([]); assertLocal(device);
});

test('hidden tabs pause telemetry and visible tabs restart no faster than two seconds', async ({ page }) => {
  const device = await mockDevice(page); await start(page);
  await expect.poll(() => device.pollTimes.length).toBeGreaterThanOrEqual(1);
  await page.evaluate(() => { Object.defineProperty(document, 'hidden', { configurable: true, value: true }); document.dispatchEvent(new Event('visibilitychange')); });
  const count = device.pollTimes.length;
  await page.waitForTimeout(2600);
  expect(device.pollTimes.length).toBe(count);
  const restored = Date.now();
  await page.evaluate(() => { Object.defineProperty(document, 'hidden', { configurable: true, value: false }); document.dispatchEvent(new Event('visibilitychange')); });
  await expect.poll(() => device.pollTimes.length, { timeout: 6000 }).toBe(count + 1);
  expect(device.pollTimes.at(-1)! - restored).toBeGreaterThanOrEqual(1900);
  assertLocal(device);
});

test('explicit legacy MDS and NONE switches preserve unsaved graph, persist first and send no body', async ({ page }) => {
  const device = await mockDevice(page); device.active = true; device.fault = true; await start(page);
  await panel(page, 'inspector');
  await page.getByRole('button', { name: 'Открыть действия восстановления MDS/NONE', exact: true }).click();
  await update(page, 'Имя проекта', 'Keep my unsaved graph');
  await page.getByRole('combobox', { name: 'Проекты MDS', exact: true }).selectOption('original_mds');
  await page.getByRole('button', { name: 'Активировать MDS', exact: true }).click();
  await expect(page.getByRole('status').filter({ hasText: 'Активировано: MDS' })).toBeVisible();
  await expect(page.getByRole('textbox', { name: 'Имя проекта', exact: true })).toHaveValue('Keep my unsaved graph');
  device.legacyEmpty = true;
  await page.getByRole('button', { name: 'Обновить', exact: true }).click();
  await expect(page.getByRole('combobox', { name: 'Проекты MDS', exact: true }).locator('option')).toHaveCount(1);
  await expect(page.getByRole('button', { name: 'Активировать MDS', exact: true })).toBeDisabled();
  await page.getByRole('button', { name: 'Отключить граф / NONE', exact: true }).click();
  await expect(page.getByRole('status').filter({ hasText: 'Активировано: NONE' })).toBeVisible();
  expect(device.switchRequests).toEqual([{ method: 'POST', body: null, query: '?id=original_mds' }, { method: 'POST', body: null, query: '?none=1' }]);
  expect(device.switchDrafts[0].name).toBe('Keep my unsaved graph'); expect(device.switchDrafts[1]).toEqual(device.switchDrafts[0]);
  expect(device.project.name).toBe('Stored project'); expect(device.revision).toBe(4);
  await expect(page.getByRole('textbox', { name: 'Имя проекта', exact: true })).toHaveValue('Keep my unsaved graph');
  expect(device.writes).toEqual(['/api/sound/graph/legacy', '/api/sound/graph/legacy']); assertLocal(device);
});

test('cancelled legacy and NONE confirmations make no writes or draft changes', async ({ page }) => {
  const device = await mockDevice(page); device.active = true; device.cancelSwitch = true; await start(page); await panel(page, 'palette');
  await update(page, 'Имя проекта', 'Unchanged after cancellation');
  await page.getByRole('combobox', { name: 'Проекты MDS', exact: true }).selectOption('original_mds');
  await page.getByRole('button', { name: 'Активировать MDS', exact: true }).click();
  await page.getByRole('button', { name: 'Отключить граф / NONE', exact: true }).click();
  expect(device.switchConfirmations).toBe(2); expect(device.writes).toEqual([]); expect(device.active).toBeTruthy();
  await expect(page.getByRole('textbox', { name: 'Имя проекта', exact: true })).toHaveValue('Unchanged after cancellation'); assertLocal(device);
});

test('moving motor rejects MDS and NONE without mutating graph, draft or active runtime', async ({ page }) => {
  const device = await mockDevice(page); device.active = true; device.legacyError = 'Motor is moving; motor must be stopped'; await start(page); await panel(page, 'palette');
  await update(page, 'Имя проекта', 'Keep rejected switch draft');
  await page.getByRole('combobox', { name: 'Проекты MDS', exact: true }).selectOption('original_mds');
  await page.getByRole('button', { name: 'Активировать MDS', exact: true }).click();
  await expect(page.getByRole('status').filter({ hasText: 'Motor is moving' })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Отключить граф / NONE', exact: true })).toBeEnabled();
  await page.getByRole('button', { name: 'Отключить граф / NONE', exact: true }).click();
  await expect.poll(() => device.switchDrafts.length).toBe(2);
  await expect(page.getByRole('status').filter({ hasText: 'Motor is moving' })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Отключить граф / NONE', exact: true })).toBeEnabled();
  expect(device.active).toBeTruthy(); expect(device.switchDrafts[1]).toEqual(device.switchDrafts[0]); expect(device.switchDrafts[0].name).toBe('Keep rejected switch draft');
  await expect(page.getByRole('textbox', { name: 'Имя проекта', exact: true })).toHaveValue('Keep rejected switch draft');
  await expect(page.getByRole('alert')).toHaveCount(0);
  expect(device.switchRequests.every(request => request.method === 'POST' && request.body === null)).toBeTruthy(); assertLocal(device);
});
