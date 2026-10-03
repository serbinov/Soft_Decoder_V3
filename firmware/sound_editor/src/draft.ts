import type { Project } from './model';

export type Draft = { project: Project; revision: number; saved: string; time: number; uncertain: boolean };
async function database(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open('decoder-sound-editor', 1);
    request.onupgradeneeded = () => request.result.createObjectStore('drafts');
    request.onerror = () => reject(request.error);
    request.onsuccess = () => resolve(request.result);
  });
}
export async function readDraft(): Promise<Draft | undefined> {
  const db = await database();
  try { return await new Promise((resolve, reject) => { const request = db.transaction('drafts').objectStore('drafts').get('current'); request.onsuccess = () => resolve(request.result); request.onerror = () => reject(request.error); }); }
  finally { db.close(); }
}
async function storeDraft(draft: Draft): Promise<void> {
  const db = await database();
  try {
    await new Promise<void>((resolve, reject) => { const transaction = db.transaction('drafts', 'readwrite'); transaction.objectStore('drafts').put(draft, 'current'); transaction.oncomplete = () => resolve(); transaction.onerror = () => reject(transaction.error); transaction.onabort = () => reject(transaction.error); });
  } finally { db.close(); }
}
let pending: Promise<void> = Promise.resolve();
export function writeDraft(draft: Draft): Promise<void> {
  const next = pending.catch(() => {}).then(() => storeDraft(draft));
  pending = next;
  return next;
}
