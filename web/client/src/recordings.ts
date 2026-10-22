/**
 * recordings.ts — saved recordings for the web client.
 *
 * Mirrors the app's in-app recordings browser (list / play / share / delete)
 * rather than just firing a download and forgetting: a recording you can't find
 * again isn't much of a feature.
 *
 * Stored in IndexedDB, not localStorage — WAV blobs are megabytes and localStorage
 * is a ~5 MB string store. IndexedDB holds Blobs natively and survives a reload.
 */

export interface Recording {
  id: string;
  name: string;
  frequency: number;
  mode: string;
  createdAt: number;
  seconds: number;
  bytes: number;
  blob: Blob;
}

const DB = 'vibesdr';
const STORE = 'recordings';

function open(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB, 1);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains(STORE)) db.createObjectStore(STORE, { keyPath: 'id' });
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

function tx<T>(mode: IDBTransactionMode, fn: (s: IDBObjectStore) => IDBRequest<T>): Promise<T> {
  return open().then(db => new Promise<T>((resolve, reject) => {
    const t = db.transaction(STORE, mode);
    const req = fn(t.objectStore(STORE));
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
    t.oncomplete = () => db.close();
  }));
}

export async function saveRecording(r: Omit<Recording, 'id'>): Promise<void> {
  // Timestamp + frequency is unique enough, and sorts naturally.
  const id = `${r.createdAt}-${Math.round(r.frequency)}`;
  await tx('readwrite', s => s.put({ ...r, id }));
}

export async function listRecordings(): Promise<Recording[]> {
  try {
    const all = await tx<Recording[]>('readonly', s => s.getAll() as IDBRequest<Recording[]>);
    return all.sort((a, b) => b.createdAt - a.createdAt);   // newest first
  } catch {
    return [];   // private browsing, or IndexedDB unavailable
  }
}

export async function deleteRecording(id: string): Promise<void> {
  await tx('readwrite', s => s.delete(id) as unknown as IDBRequest<undefined>);
}

export function formatSize(bytes: number): string {
  // ★ Under 1 KB in BYTES (the gallery showed a 181-byte NAVTEX message as "0 KB", 2026-10-22).
  return bytes >= 1e6 ? `${(bytes / 1e6).toFixed(1)} MB` : bytes < 1000 ? `${Math.max(0, Math.round(bytes))} B` : `${Math.round(bytes / 1e3)} KB`;
}

export function formatDuration(sec: number): string {
  const s = Math.round(sec);
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
}
