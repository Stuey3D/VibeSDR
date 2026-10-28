/**
 * ★★ V12 WHICH SERVERS SEND THE FULL-CAPTURE ROW (SPWB — src/utils/wfWide.ts, docs/v12/WATERFALL-FULL-CAPTURE.md §1).
 *
 * The app asks for it with `&wide=1` on /ws/user-spectrum ONLY for a server whose `hwinfo` said `"wide": 2` (the format
 * version) or more. An older server never sees the parameter: its URL, its SPEC stream and its rate are exactly as before.
 *
 * ★★ THE ORDER PROBLEM: hwinfo is a MESSAGE on the spectrum socket — it can only arrive after the URL was chosen. So the
 *    answer is REMEMBERED per server (by its base URL) and used from the next socket on: a resume from the background, an
 *    auto-reconnect, the next session. Reopening the socket just to ask would be an extra connection (and a re-subscribe
 *    pause) on every first visit — not done (feedback: no extra requests). A server that stops advertising it (a
 *    downgrade) is forgotten at its next hwinfo. The /connection preflight reply is read too (`wide` there, if a server
 *    ever sends it, decides the FIRST socket) — see the report for the server change that would make the first visit work.
 * Pure (no React Native imports) so scripts/test_wide_caps.ts can run it under Node; wideCaps.ts wires the store.
 */

export interface CapsStore {
  get(key: string): Promise<string | null>;
  set(key: string, value: string): Promise<void>;
}

export const WIDE_CAPS_KEY = 'vsWideServers';
/** The SPWB format this app reads. */
export const WIDE_FORMAT = 2;
const MAX_SERVERS = 64;

/** The server's key: its base URL without a trailing slash, lower-cased scheme + host. */
export function serverKey(baseUrl: string): string {
  const s = baseUrl.trim().replace(/\/+$/, '');
  const m = /^([a-z]+:\/\/[^/]+)(.*)$/i.exec(s);
  return m ? m[1].toLowerCase() + m[2] : s;
}

export class WideCapsCore {
  private set: string[] = [];
  private loading: Promise<void> | null = null;
  private store: CapsStore;
  constructor(store: CapsStore) { this.store = store; }

  /** Load once (single flight). A storage that fails leaves the list empty for this run — the old behaviour. */
  load(): Promise<void> {
    if (!this.loading) {
      this.loading = (async () => {
        try {
          const raw = await this.store.get(WIDE_CAPS_KEY);
          const v = raw ? JSON.parse(raw) : [];
          if (Array.isArray(v)) this.set = v.filter((x) => typeof x === 'string').slice(-MAX_SERVERS);
        } catch { /* keep empty */ }
      })();
    }
    return this.loading;
  }

  /** `&wide=1` for a server known to send SPWB v2, else ''. */
  suffix(baseUrl: string): string { return this.set.includes(serverKey(baseUrl)) ? '&wide=1' : ''; }

  /** What the server said (hwinfo / the preflight): `wide` ≥ WIDE_FORMAT = remember it; anything else (absent, older) =
   *  forget it. Returns true when the answer changed. */
  note(baseUrl: string, wide: unknown): boolean {
    const k = serverKey(baseUrl), yes = typeof wide === 'number' && wide >= WIDE_FORMAT, had = this.set.includes(k);
    if (yes === had) return false;
    this.set = yes ? [...this.set.filter((x) => x !== k), k].slice(-MAX_SERVERS) : this.set.filter((x) => x !== k);
    this.store.set(WIDE_CAPS_KEY, JSON.stringify(this.set)).catch(() => {});
    return true;
  }
}
