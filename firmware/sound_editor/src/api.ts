import type { Asset, Diagnostic, Limits, Project } from './model';

export type Descriptor = { id: string; name: string; revision: number };
export type Track = { slot: number; file: string; label: string; enabled: boolean };
export type Binding = { fn: number; type: number; id: number; dir?: number; state?: number };
export type RuntimeStatus = { active: boolean; fault?: boolean; id: string; revision: number; engine: boolean; armed: boolean; states: number[]; failedChannels: number; speed?: number };
export class ApiError extends Error {
  status: number;
  diagnostics: Diagnostic[];
  constructor(message: string, status: number, diagnostics: Diagnostic[] = []) { super(message); this.status = status; this.diagnostics = diagnostics; }
}

export async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), init.body instanceof File ? 300000 : 15000);
  try {
    const response = await fetch(path, { ...init, signal: controller.signal, cache: 'no-store' });
    const raw = await response.text();
    let data: { ok?: boolean; error?: string; diagnostics?: Diagnostic[] };
    try { data = JSON.parse(raw); } catch { throw new ApiError(`Устройство вернуло не-JSON (${response.status}). Изменения не применены.`, response.status); }
    if (!response.ok || data.ok !== true) throw new ApiError(data.error ?? `Запрос к устройству завершился ошибкой (${response.status})`, response.status, data.diagnostics);
    return data as T;
  } catch (error) {
    if (error instanceof ApiError) throw error;
    throw new ApiError('Соединение потеряно или истёк тайм-аут. Результат записи неизвестен: перезагрузите версию перед повтором. Локальный черновик сохранён.', 0);
  } finally { clearTimeout(timeout); }
}

const query = (params: Record<string, string | number>) => new URLSearchParams(Object.entries(params).map(([key, value]) => [key, String(value)])).toString();
export const api = {
  capabilities: () => request<{ limits: Limits; format: string; schemaVersion: number }>('/api/sound/graph/capabilities'),
  projects: () => request<{ projects: Descriptor[] }>('/api/sound/graph/projects'),
  project: (id: string) => request<{ project: Project; revision: number }>(`/api/sound/graph/project?${query({ id, revision: 0 })}`),
  tracks: () => request<{ tracks: Track[] }>('/api/audio/tracks'),
  asset: (file: string) => request<{ asset: Asset }>(`/api/sound/graph/asset?${query({ file })}`),
  state: () => request<RuntimeStatus>('/api/sound/graph/state'),
  validate: (project: Project) => request<{ valid: boolean; diagnostics: Diagnostic[] }>('/api/sound/graph/validate', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(project) }),
  save: (project: Project, revision: number) => request<{ revision: number }>(`/api/sound/graph/save?${query({ id: project.id, expectedRevision: revision })}`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(project) }),
  apply: (id: string, revision: number) => request(`/api/sound/graph/apply?${query({ id, revision })}`, { method: 'POST' }),
  upload: (file: File) => request<{ slot: number }>('/api/audio/upload?slot=0&enabled=1', { method: 'POST', headers: { 'Content-Type': 'audio/wav', 'X-File-Name': encodeURIComponent(file.name) }, body: file }),
  bindings: () => request<{ binds: Binding[] }>('/api/func-map?view=bind'),
  bindAdd: (fn: number, type: number, id: number, dir: number, state: number) => request(`/api/func-map?${query({ bind: 1, fn, type, id, dir, state })}`, { method: 'POST' }),
  bindRemove: (idx: number) => request(`/api/func-map?${query({ bind: 1, remove: 1, idx })}`, { method: 'POST' }),
};
