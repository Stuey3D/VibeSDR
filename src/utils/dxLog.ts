/**
 * ★★★ THE DX LOG — one set of rules for the app and the web client (V12, 2026-11-04). docs/v12/DX-SCANNER.md.
 *
 * The server's idle DX scanner (vibe_dxscan.h) keeps what it hears on Broadcast FM and DAB in a log of its own, apart from
 * the bookmarks. Both clients show it the same way: DX only by default (a toggle shows Locals and Suspects), sorted by last
 * heard / first heard / peak signal / times heard, a LIVE NOW badge on what the current or last sweep heard, tap to tune,
 * PROMOTE (the owner) copies it into the normal bookmarks.
 *
 * ★ Nothing here is a setting of the server's: what the scanner sweeps comes back with the list (`scanner`), and whether
 *   the log exists at all is hwinfo `dxLog` — absent on an older server, which then draws no DX LOG key.
 * ★ Text from the air (PS, labels) is only ever put in the page as text, never as HTML.
 */

export type DxClass = 'dx' | 'local' | 'suspect' | 'provisional';
export type DxSort = 'last' | 'first' | 'peak' | 'times';

export interface DxOpening { start: number; end: number; detections: number; peakSnrDb: number; peakLevelDb?: number; services?: string[]; proof?: string }
export interface DxEntry {
  id: number; band: 'FM' | 'DAB'; key: string; class: DxClass; suspectWhy?: string;
  freqKhz?: number; pi?: string; ps?: string; psVariants?: string[];
  block?: string; eid?: string; ecc?: number; ensemble?: string; services?: { sid: number; label: string }[];
  tii?: { main: number; sub: number };
  firstHeard: number; lastHeard: number; timesHeard: number; presence: number; openings: number; live: boolean;
  peakLevelDb?: number; peakSnrDb: number; promoted?: string;
  openingList?: DxOpening[];
}
export interface DxPage {
  ok: boolean; total: number; offset: number; limit: number;
  counts: { dx: number; local: number; suspect: number; provisional: number };
  bootstrap: boolean; bootstrapEnds: number; sweeps: { fm: number; dab: number }; alertSeq: number; now: number;
  entries: DxEntry[];
  scanner?: { on: boolean; bands: string; waitMin: number; raster: number; viewAdmin: boolean };
  canPromote?: boolean; error?: string;
}
export interface DxAlert { seq: number; utc: number; id: number; kind: 'new' | 'opening' | string; text: string }

export const DX_PAGE = 25;
export const DX_SORTS: { id: DxSort; label: string }[] = [
  { id: 'last', label: 'LAST HEARD' }, { id: 'first', label: 'FIRST HEARD' },
  { id: 'peak', label: 'PEAK SIGNAL' }, { id: 'times', label: 'TIMES HEARD' },
];

export interface DxQuery { band?: '' | 'FM' | 'DAB'; showLocal?: boolean; showSuspect?: boolean; sort?: DxSort; page?: number }
/** The query string for GET /vibeserver/dxlog.json (page is 1-based). */
export function dxQuery(q: DxQuery): string {
  const p: string[] = [];
  if (q.band) p.push(`band=${q.band}`);
  if (q.showLocal) p.push('local=1');
  if (q.showSuspect) p.push('suspect=1');
  if (q.sort && q.sort !== 'last') p.push(`sort=${q.sort}`);
  p.push(`offset=${Math.max(0, ((q.page || 1) - 1) * DX_PAGE)}`, `limit=${DX_PAGE}`);
  return p.join('&');
}
export function dxPageCount(total: number): number { return Math.max(1, Math.ceil(total / DX_PAGE)); }

/** "12:04 UTC" today, else "3 Nov 12:04 UTC" — the log is UTC, as the server keeps it. */
export function dxWhen(t: number, now = Date.now() / 1000): string {
  const d = new Date(t * 1000), n = new Date(now * 1000);
  const p2 = (x: number) => String(x).padStart(2, '0');
  const hm = `${p2(d.getUTCHours())}:${p2(d.getUTCMinutes())} UTC`;
  if (d.getUTCFullYear() === n.getUTCFullYear() && d.getUTCMonth() === n.getUTCMonth() && d.getUTCDate() === n.getUTCDate()) return hm;
  const mon = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'][d.getUTCMonth()];
  return `${d.getUTCDate()} ${mon} ${hm}`;
}
/** How long ago, briefly: "now", "4 min", "3 h", "2 d". */
export function dxAgo(t: number, now = Date.now() / 1000): string {
  const s = Math.max(0, now - t);
  if (s < 90) return 'now';
  if (s < 5400) return `${Math.round(s / 60)} min`;
  if (s < 172800) return `${Math.round(s / 3600)} h`;
  return `${Math.round(s / 86400)} d`;
}

/** The row's left column: "98.8" (MHz) or "12D". */
export function dxWhere(e: DxEntry): string {
  return e.band === 'FM' ? ((e.freqKhz || 0) / 1000).toFixed(e.freqKhz && e.freqKhz % 100 ? 2 : 1) : (e.block || '');
}
/** The row's title: the PS (or the PI) on FM; the ensemble (or its EId) on DAB. */
export function dxTitle(e: DxEntry): string {
  if (e.band === 'FM') return (e.ps || '').trim() || `PI ${e.pi}`;
  return (e.ensemble || '').trim() || `EId ${e.eid}`;
}
/** The tags beside the title, in order: LIVE NOW, the class (never "DX" — that is the default), PROMOTED. */
export function dxTags(e: DxEntry): string[] {
  const t: string[] = [];
  if (e.live) t.push('LIVE NOW');
  if (e.class === 'local') t.push('LOCAL');
  else if (e.class === 'suspect') t.push('SUSPECT');
  else if (e.class === 'provisional') t.push('BASELINE');
  if (e.promoted) t.push('BOOKMARKED');
  return t;
}
/** The row's second line: identity, how often, when. */
export function dxSubline(e: DxEntry, now = Date.now() / 1000): string {
  const id = e.band === 'FM'
    ? [`PI ${e.pi}`, e.psVariants && e.psVariants.length > 1 ? `${e.psVariants.length} PS` : ''].filter(Boolean).join(' · ')
    : [`EId ${e.eid}`, e.services && e.services.length ? `${e.services.length} service${e.services.length === 1 ? '' : 's'}` : '',
       e.tii ? `TII ${e.tii.main}/${e.tii.sub}` : ''].filter(Boolean).join(' · ');
  /* ★ "heard in 3443 scans" (Stuart, 2026-11-14: "heard 3443× in 2 openings" was confusing; "the scans completed at the
   *  bottom and heard in x amount of scans is enough"). Openings are still counted and still sort the log. */
  const heard = `heard in ${e.timesHeard} scan${e.timesHeard === 1 ? '' : 's'}`;
  const snr = `peak ${Math.round(e.peakSnrDb)} dB`;
  return [id, heard, snr, `last ${dxWhen(e.lastHeard, now)}`].join(' · ');
}
/** Why it is greyed (a Suspect) — the server's own words. */
export function dxSuspectLine(e: DxEntry): string {
  return e.class === 'suspect' && e.suspectWhy ? `Possibly not a real station: ${e.suspectWhy}` : '';
}

/** What tapping an entry tunes: FM → frequency + WFM; DAB → the block's first service (the block alone when it named none). */
export interface DxTune { kind: 'fm'; hz: number; mode: 'wfm' }
export interface DxTuneDab { kind: 'dab'; block: string; hz: number; sid: number }
export function dxTuneTarget(e: DxEntry): DxTune | DxTuneDab | null {
  if (e.band === 'FM' && e.freqKhz) return { kind: 'fm', hz: e.freqKhz * 1000, mode: 'wfm' };
  // ★ The server sends the block's centre (freqKhz) — no Band III table of our own
  if (e.band === 'DAB' && e.block && e.freqKhz) return { kind: 'dab', block: e.block, hz: e.freqKhz * 1000, sid: e.services && e.services.length ? e.services[0].sid : -1 };
  return null;
}

/** The footer: what is shown, and the baseline while a new server builds it. */
export function dxFooter(p: DxPage, shown: number): string {
  const bits: string[] = [];
  bits.push(`${p.total} shown`);
  const c = p.counts;
  bits.push(`${c.dx + c.provisional} DX · ${c.local} local · ${c.suspect} suspect`);
  if (p.sweeps) bits.push(`${p.sweeps.fm} FM · ${p.sweeps.dab} DAB scans completed`);   // ★ the same word as each row's "heard in N scans"
  let s = bits.join(' · ');
  if (p.bootstrap) s += ` — building this server's baseline until ${dxWhen(p.bootstrapEnds, p.now)}: everything is marked BASELINE and nothing alerts until then.`;
  void shown;
  return s;
}
/** The empty list, in words. */
export function dxEmptyLine(p: DxPage | null, showLocal: boolean): string {
  if (!p) return 'Could not reach the server.';
  if (p.scanner && !p.scanner.on) return 'The DX scanner is switched off on this radio — its owner can switch it on in Setup.';
  if (!showLocal && p.counts.local > 0) return `No DX yet — ${p.counts.local} local station${p.counts.local === 1 ? ' is' : 's are'} hidden (SHOW LOCALS).`;
  return 'Nothing heard yet — the scanner sweeps while nobody is listening.';
}

/** ★ Per-user alerts (opt-in, this device): which of the server's alerts are new since the last one shown. */
export function dxNewAlerts(alerts: DxAlert[], lastSeen: number): DxAlert[] {
  return alerts.filter((a) => a.seq > lastSeen).sort((a, b) => a.seq - b.seq);
}
