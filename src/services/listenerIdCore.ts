/**
 * ★★★ THE APP'S LISTENER ID — the app's copy of the web client's `browserId()` (web/client/src/main.ts).
 *
 * Stuart, 2026-10-27: "2 radios one server is allowed unless server owner blocks the same connection using both radios
 * exactly how the web client handles it now".
 *
 * ★★ WHAT THE SERVER DOES WITH IT. Every VibeServer spectrum and audio socket the web opens carries `&bid=<uuid>` — one
 *    random id per BROWSER (localStorage), shared by its tabs. The radio process keys the "radios per listener" rule
 *    (g_vsMaxRadiosPerIp — the owner's "Allow several connections" setting) by `b:<bid>` when one is sent, and by the
 *    ADDRESS only when none is (occKeyFor, local_sdr_shim.cpp). The app sent none, so it was judged by its address: a
 *    phone was one listener with every other device in the house, and loopback/tunnel arrivals were a different rule
 *    again. Sending the same kind of id the same way puts the app under EXACTLY the web's rule — no more, no less.
 * ★★ IT IDENTIFIES NOTHING. A random UUID made once per install, never derived from the device, the person or the
 *    account; it only groups this install's connections, and it grants nothing (the server says so beside `bid`).
 * ★ An older server never reads `bid` — an unknown query parameter is ignored — so it is harmless there.
 * ★ The server accepts letters, digits and '-' up to 64 characters and treats anything else as NO id; a UUID fits.
 *
 * Pure (no React Native imports) so `scripts/test_listener_id.ts` can run it under Node. listenerId.ts wires the store.
 */

export interface IdStore {
  get(key: string): Promise<string | null>;
  set(key: string, value: string): Promise<void>;
}

/** ★ Not the web's key name (`vsBrowserId`) on purpose: there is no localStorage here, and nothing reads both. */
export const LISTENER_ID_KEY = 'vsListenerId';

/** The server's own acceptance rule for `bid` (local_sdr_shim.cpp) — anything else it would silently discard. */
export function validListenerId(s: unknown): s is string {
  return typeof s === 'string' && s.length > 0 && s.length <= 64 && /^[A-Za-z0-9-]+$/.test(s);
}

export class ListenerIdCore {
  private id: string | null = null;
  private loading: Promise<string> | null = null;
  private store: IdStore;
  private make: () => string;
  // ★ Plain fields, not parameter properties: Node's type stripping (the test runner) cannot run those.
  constructor(store: IdStore, make: () => string) { this.store = store; this.make = make; }

  /** ★ Load once (single flight), making and saving one on the first launch. A storage that fails still gives an id
   *  for this run — the web's own fallback (a tab id where storage is unavailable): the rule then applies per run,
   *  which is better than not at all. */
  load(): Promise<string> {
    if (this.id) return Promise.resolve(this.id);
    if (!this.loading) {
      this.loading = (async () => {
        let had: string | null = null;
        try { had = await this.store.get(LISTENER_ID_KEY); } catch { had = null; }
        if (validListenerId(had)) { this.id = had; return had; }
        const made = this.make();
        this.id = made;
        try { await this.store.set(LISTENER_ID_KEY, made); } catch { /* this run still has it */ }
        return made;
      })().finally(() => { this.loading = null; });
    }
    return this.loading;
  }

  /** The id if loaded, else '' (send nothing — the server falls back to the address, as it does for older apps). */
  now(): string { return this.id ?? ''; }

  /** `&bid=<id>` for a socket URL — the web's exact parameter — or '' before the id is loaded. */
  query(): string { return this.id ? `&bid=${encodeURIComponent(this.id)}` : ''; }

  /** ★ Factory reset: the storage has just been wiped, so the next connection must not carry the old id — a fresh
   *  install has a fresh one. (The web has no reset of its own; clearing site data is its equivalent.) */
  forget(): void { this.id = null; this.loading = null; }
}
