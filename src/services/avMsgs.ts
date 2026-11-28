/**
 * avMsgs.ts — ★★★ beta97 hotfix THE DATALINK MESSAGES LIST, from the server's message log (Stuart: "is the list limited to
 * 200 messages only for VDL2?" — it showed each aircraft's LATEST message as it changed, a rolling 200, nothing from before
 * the map opened). Shared by the app and the web (one rule, one module):
 *
 *   · EVERY decoded message (ACARS, VDL2, HFDL, ADS-C), from GET /vibeserver/avmsgs.json — the server's own log
 *     (vibe_avi_service.h msgsJson: server-wide, bounded, each message numbered);
 *   · on open a BACKFILL within the map's time window (?window=s), then the newest only (?since=<last id>), de-duplicated
 *     by the message's own number;
 *   · a TIME WINDOW that follows the map's time filter (1 H / 24 H / ALL / THIS SESSION), and a MEMORY CEILING by device
 *     (≈ 500 on a 1 GB phone, more on a bigger device or a desktop);
 *   · the tab says what it shows: "last 1 h", "latest 500" when the ceiling bites, "since opened" on an older server
 *     (no log: the live feed only, as before).
 *
 * Pure: the callers fetch (the app with withReadAuth, the web with its own fetch) and keep the list.
 */
import type { LiveMsg } from './liveMaps';

/** One message from the server's log. */
export interface AvMsg { id: number; t: number; icao?: string; reg?: string; flight?: string; src: string; via?: string; label: string; name?: string;
  text?: string; snr?: number; snrMode?: string }
export interface AvMsgPage { seq: number; keep: number; trunc: boolean; msgs: AvMsg[] }

export const AV_MSGS_PATH = '/vibeserver/avmsgs.json';
/** How often the list asks for what is new while it is on screen (a floor: never faster). */
export const AV_MSGS_POLL_MS = 5000;
/** The most one call returns (the server's own cap is 1500). */
export const AV_MSGS_PAGE = 300;

const str = (v: unknown, max: number) => (typeof v === 'string' ? v.slice(0, max) : undefined);
const num = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : undefined);

/** The server's answer, checked (anything else from it is dropped, never trusted). null = not a log page. */
export function parseAvMsgs(raw: unknown): AvMsgPage | null {
  if (!raw || typeof raw !== 'object') return null;
  const o = raw as Record<string, unknown>;
  const seq = num(o.seq);
  if (seq == null || !Array.isArray(o.msgs)) return null;
  const msgs: AvMsg[] = [];
  for (const x of (o.msgs as unknown[]).slice(0, 1500)) {
    if (!x || typeof x !== 'object') continue;
    const m = x as Record<string, unknown>;
    const id = num(m.id), t = num(m.t);
    if (id == null || t == null || id <= 0) continue;
    const icao = str(m.icao, 8), reg = str(m.reg, 12), flight = str(m.flight, 12);
    msgs.push({ id, t, ...(icao ? { icao } : {}), ...(reg ? { reg } : {}), ...(flight ? { flight } : {}),
      src: str(m.src, 8) ?? 'ACARS', ...(str(m.via, 48) ? { via: str(m.via, 48) } : {}), label: str(m.label, 4) ?? '',
      ...(str(m.name, 48) ? { name: str(m.name, 48) } : {}), ...(str(m.text, 400) ? { text: str(m.text, 400) } : {}),
      ...(num(m.snr) != null && str(m.snrMode, 12) ? { snr: num(m.snr), snrMode: str(m.snrMode, 12) } : {}) });
  }
  return { seq, keep: num(o.keep) ?? 86400, trunc: o.trunc === true, msgs };
}

/** The list's window (seconds) from the map's time filter: 1 H, 24 H, ALL (the server keeps a day), THIS SESSION (since `t0`). */
export function avMsgWindowS(range: string | null | undefined, nowS: number, sessionT0?: number | null, keepS = 86400): number {
  const r = String(range ?? '').toLowerCase();
  if (r === '1h') return 3600;
  if (r === '6h') return 6 * 3600;
  if (r === '24h') return Math.min(keepS, 86400);
  if ((r === 'session' || r === 'sess') && sessionT0 && sessionT0 > 0) return Math.max(60, nowS - sessionT0);
  return keepS;   // ALL (and anything unknown): all the server keeps
}

/** The memory ceiling, by the device's memory (bytes; the web: navigator.deviceMemory GB × 2^30) — ≈ 500 on a 1 GB phone. */
export function avMsgCeiling(totalMemoryBytes: number | null | undefined): number {
  const gb = totalMemoryBytes && totalMemoryBytes > 0 ? totalMemoryBytes / 2 ** 30 : 0;
  if (!gb) return 500;          // unknown: the small device's figure
  if (gb < 2.5) return 500;     // the 1–2 GB phones (the 32-bit ones in the field)
  if (gb < 6) return 1000;
  return 1500;                  // the server's own ceiling
}

/** New messages merged in: de-duplicated by the server's number, newest first, at most `ceiling`. */
export function avMsgMerge(have: AvMsg[], add: AvMsg[], ceiling: number): AvMsg[] {
  if (!add.length) return have.length > ceiling ? have.slice(0, ceiling) : have;
  const seen = new Set(have.map((m) => m.id));
  const fresh = add.filter((m) => !seen.has(m.id) && (seen.add(m.id), true));
  if (!fresh.length) return have;
  return [...fresh, ...have].sort((a, b) => b.id - a.id).slice(0, Math.max(1, ceiling));
}

/** Inside the window (seconds back from `nowS`). */
export function avMsgInWindow(list: AvMsg[], windowS: number, nowS: number): AvMsg[] {
  const from = nowS - windowS;
  return list.filter((m) => m.t >= from);
}

/** The aircraft's key the map's rows use (the merged table: the ICAO address, else the registration, else the flight). */
export function avMsgKey(m: AvMsg): string { return `a:${m.icao || m.reg || m.flight || ''}`; }

/** As the list's existing rows (liveMaps LiveMsg): who, the source, the label and its words, the body where sent. */
export function avMsgToLive(m: AvMsg): LiveMsg {
  return { id: `log#${m.id}`, key: avMsgKey(m), t: Math.round(m.t), who: m.flight || m.reg || m.icao || 'aircraft', src: m.src,
           label: m.label, labelName: m.name ?? '', ...(m.text ? { text: m.text } : {}), ...(m.snr != null && m.snrMode ? { snr: m.snr, snrMode: m.snrMode } : {}) };
}

/** The tab's words: what the list holds. `capped`: the ceiling bit (more than it keeps). `log`: false on an older server. */
export function avMsgLabel(o: { windowS: number; keepS?: number; shown: number; capped: boolean; log: boolean; session?: boolean }): string {
  if (!o.log) return 'since opened';
  if (o.capped) return `latest ${o.shown}`;
  if (o.session) return 'this session';
  const h = o.windowS / 3600;
  if (o.windowS >= (o.keepS ?? 86400)) return 'last 24 h';
  return h >= 1 ? `last ${Math.round(h)} h` : `last ${Math.max(1, Math.round(o.windowS / 60))} min`;
}

/** The URL's query (the caller adds the base and its auth): a backfill (no cursor) or what came after `since`. */
export function avMsgsQuery(o: { since?: number; windowS: number; max?: number }): string {
  return `${AV_MSGS_PATH}?window=${Math.max(60, Math.round(o.windowS))}&max=${Math.max(1, Math.min(1500, o.max ?? AV_MSGS_PAGE))}${o.since ? `&since=${Math.floor(o.since)}` : ''}`;
}
