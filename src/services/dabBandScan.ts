// dabBandScan.ts — the DAB BAND SCAN's results as the clients show them (V12, 2026-10-26). Pure, shared by the web DAB box
// (web/client/src/main.ts) and the app (src/components/DabPanel.tsx); scripts/test_dab_band_scan.ts drives it.
//
// ★★★ M7EIW on Discord: "a scan function for DAB which also shows the results". The SERVER scans (local_sdr_shim.cpp,
//     dabBandScanRun) and publishes `{"type":"dab_scan",…}` as it goes; this turns that into what a person reads:
//     progress ("12 / 41 blocks"), one row per ensemble found — block, frequency, name, how many stations, the FIC
//     quality — each expandable to its station list, and the closing line ("3 ensembles · 27 stations added to
//     bookmarks"). Stuart's rule decides who sees the SCAN key at all: hwinfo `dabScan` (a single-user radio, or the
//     admin) — never inferred here (the app has no hardcoded settings vs a server).
//  ★ Text from the air is shown through the caller's own escaping; nothing here builds HTML.

import { FIB_STRONG, FIB_WEAK, MER_STRONG, MER_WEAK } from '../utils/dabQuality';
export interface DabScanService { sid: number; label: string }
export interface DabScanHit {
  block: string; channel: number; hz: number; ensemble: string;
  fib: number; mer: number; complete: boolean; services: DabScanService[];
}
export interface DabScanState {
  running: boolean; by: string; done: number; total: number; current: string;
  added: number; cancelled: boolean; finished: boolean; secs: number; seq: number;
  why?: string; error?: string; found: DabScanHit[];
  /** ★ V12: an OWNER's scan holding the radio — listeners locked out while it runs (absent on an older server). */
  lock?: boolean;
}

const num = (v: unknown, d = 0) => (typeof v === 'number' && Number.isFinite(v) ? v : d);
const str = (v: unknown) => (typeof v === 'string' ? v : '');

/** Parse a `dab_scan` message defensively. null when it is not one. */
export function parseDabScan(m: any): DabScanState | null {
  if (!m || m.type !== 'dab_scan') return null;
  const found: DabScanHit[] = Array.isArray(m.found) ? m.found.map((h: any) => ({
    block: str(h?.block), channel: num(h?.channel, -1), hz: num(h?.hz), ensemble: str(h?.ensemble).trim(),
    fib: Math.max(0, Math.min(1, num(h?.fib))), mer: num(h?.mer), complete: h?.complete === true,
    services: Array.isArray(h?.services)
      ? h.services.filter((s: any) => num(s?.sid) > 0 && str(s?.label).trim()).map((s: any) => ({ sid: num(s.sid), label: str(s.label).trim() }))
      : [],
  })).filter((h: DabScanHit) => h.block && h.channel >= 0) : [];
  return {
    running: m.running === true, by: str(m.by), done: num(m.done), total: num(m.total), current: str(m.current),
    added: num(m.added), cancelled: m.cancelled === true, finished: m.finished === true, secs: num(m.secs), seq: num(m.seq),
    why: str(m.why) || undefined, error: str(m.error) || undefined, found, lock: m.lock === true,
  };
}

/** Block order (the server walks upward; a client may get a late message out of order). */
export function dabScanRows(s: DabScanState | null): DabScanHit[] {
  return s ? s.found.slice().sort((a, b) => a.channel - b.channel) : [];
}

/** "222.064 MHz" */
export function dabScanMHz(hz: number): string { return hz > 0 ? `${(hz / 1e6).toFixed(3)} MHz` : ''; }

/** FIC quality in words a listener reads at a glance — the FIB rate, the same measure the ensemble memory uses. */
/** ★★ V12 2026-11-04 THE LIVE PANE'S RULE, NOT A RULE OF ITS OWN (Stuart: "it is incorrectly listing some as strong when
 *  they are moderate" — Herts Beds Bucks scanned STRONG, then read "Multiplex moderate · MER 12 dB" when tuned). The scan
 *  graded on the FIB rate alone (> 90 % = strong), and a multiplex decodes its FIC perfectly at an MER that the receiver
 *  itself calls moderate. Now the worse of the FIB rate and the MER, on dabQuality's own thresholds and in its own words:
 *  FIB ≥ 99 % / ≥ 90 %, MER ≥ 16 dB / ≥ 10 dB → strong / moderate / weak. A scan hit without an MER is judged on the FIB. */
export function dabScanQuality(h: DabScanHit): { pct: number; word: 'strong' | 'moderate' | 'weak' } {
  const pct = Math.round(h.fib * 100);
  const byFib = h.fib >= FIB_STRONG ? 3 : h.fib >= FIB_WEAK ? 2 : 1;
  const byMer = h.mer > 0 ? (h.mer >= MER_STRONG ? 3 : h.mer >= MER_WEAK ? 2 : 1) : 3;
  const lvl = Math.min(byFib, byMer);
  return { pct, word: lvl === 3 ? 'strong' : lvl === 2 ? 'moderate' : 'weak' };
}

const plural = (n: number, one: string, many = one + 's') => `${n} ${n === 1 ? one : many}`;

/** "3 stations" — the row's count. */
export function dabScanCount(h: DabScanHit): string { return plural(h.services.length, 'station'); }

/** The progress / summary line. */
export function dabScanLine(s: DabScanState | null): string {
  if (!s) return '';
  if (s.error) return s.error;
  const ens = s.found.length, st = s.found.reduce((a, h) => a + h.services.length, 0);
  if (s.running) {
    return `Scanning ${s.done} / ${s.total} blocks${s.current ? ` · ${s.current}` : ''}`
         + (ens ? ` — ${plural(ens, 'ensemble')} so far` : '');
  }
  if (!s.finished) return '';
  const tail = `${plural(s.added, 'station')} added to bookmarks`;
  if (s.cancelled) return `Scan stopped${s.why ? ` (${s.why.replace(/^stopped — /, '')})` : ''} at ${s.done} / ${s.total} — `
                        + `${plural(ens, 'ensemble')}, ${tail}`;
  return ens ? `${plural(ens, 'ensemble')}, ${plural(st, 'station')} — ${tail}`
             : `No DAB found on any of the ${s.total} blocks${s.why ? ` (${s.why})` : ''}. Check the aerial is up and connected.`;
}

/** What somebody ELSE on the radio is told while the owner scans (a shared radio's admin scan). */
export function dabScanOthersLine(s: DabScanState | null): string {
  return s && s.running && s.by === 'admin' ? `The owner is scanning the DAB band (${s.done} / ${s.total}) — the radio is back when it ends.` : '';
}

/** Expanded rows: a Set of block names the listener opened. Toggling one keeps the others. */
export function dabScanToggle(open: ReadonlySet<string>, block: string): Set<string> {
  const n = new Set(open);
  if (n.has(block)) n.delete(block); else n.add(block);
  return n;
}

/* ── ★★★ V12 2026-11-04: THE KEY, THE LIST, AND THE OWNER'S SCAN LOCK — one rule for both clients ──────────────────────
 *  Stuart: "change the button's name from scan to multiplexes or ensembles after the initial scan is complete that way it
 *  can be shown on shared VFO servers too". The server keeps the last scan's list and gives it to anyone who asks
 *  (`dab_scan_get`); whether this listener may START one is still the server's `dabScan` answer, never inferred here. */

/** The `lock` flag the server sets while an OWNER's scan holds the radio (absent on an older server = never locked). */
export function scanLocked(m: unknown): boolean {
  const x = m as { running?: unknown; lock?: unknown } | null;
  return !!x && x.running === true && x.lock === true;
}

/** What the DAB box's scan key is, for this listener:
 *    'multiplexes' — the last scan found at least one ensemble and none is running: opens the kept list, for ANYONE
 *                    (a shared radio's listeners too, who cannot scan);
 *    'scan'        — no list yet, and this listener may scan (hwinfo `dabScan`);
 *    null          — not drawn (no list and no permission, or a scan is running — its own pane has CANCEL). */
export function dabScanKey(s: DabScanState | null, mayScan: boolean): 'multiplexes' | 'scan' | null {
  if (s?.running) return null;
  if (s && s.found.length > 0) return 'multiplexes';
  return mayScan ? 'scan' : null;
}

/** ★★ V12 2026-11-12: THE "NO SCAN YET" LINE. The server opens DAB on 12B when it has nothing better — no block remembered,
 *  no scan list, no national multiplex for its country (vibe_dab_landing.h) — and says so in the report (`unscanned`).
 *  Outside the UK 12B is often silence, so the box says why, plainly. ★ SCAN is suggested ONLY to someone who can press it
 *  (Stuart: "Scan only unlocks on single user radios and admin mode") — the same `mayScan` (hwinfo `dabScan`) that draws
 *  the key; everybody else is told where the owner's scan lives, with no control offered. Nothing while a scan runs or
 *  once a list exists. */
export function dabUnscannedLine(unscanned: boolean, s: DabScanState | null, mayScan: boolean): string {
  if (!unscanned || s?.running || (s && s.found.length > 0)) return '';
  return mayScan ? 'No DAB scan on this radio yet, so it opened on 12B — press SCAN to find the multiplexes here.'
                 : 'No DAB scan on this radio yet, so it opened on 12B — the owner can run one from SERVER ADMIN → DAB SCAN.';
}

/** May a tap on a row of the list TUNE for this listener? The server's own answers only: not on a listen-only
 *  (spectator) dial unless they are its unlocked admin, not while ask-before-tuning holds them, not while an owner's
 *  scan has the radio. Where it may not, the list is shown READ-ONLY (no tune affordance at all). */
export function dabScanMayTune(o: { dialMode: string; admin: boolean; askLocked: boolean; scanLocked: boolean }): boolean {
  if (o.admin) return true;
  if (o.scanLocked || o.askLocked) return false;
  return o.dialMode !== 'spectator';
}

/** ★★ THE SHARED-TUNER SLOT'S WORDS WHILE AN OWNER'S SCAN HOLDS THE RADIO (Stuart: "it should say ADMIN Triggered SCAN
 *  In Progress, Please Wait" — then: not a separate banner, it ALTERNATES in the shared-tuner message slot). `phase`
 *  flips every SCAN_LOCK_FLIP_MS; the progress rides on the second line ("PLEASE WAIT · 12 / 41"). */
export const SCAN_LOCK_FLIP_MS = 2000;
export function scanLockLine(phase: number, done = 0, total = 0): string {
  return phase % 2 === 0 ? 'ADMIN TRIGGERED A SCAN' : `PLEASE WAIT${total > 0 ? ` · ${done} / ${total}` : ''}`;
}

/* ── ★★ V12: THE OWNER'S FM SCAN (`fm_scan`) — the admin page's progress and result line ─────────────────────────────── */
export interface FmScanHit { khz: number; pi: string; ps: string; psComplete: boolean; snr: number }
export interface FmScanState {
  running: boolean; by: string; done: number; total: number; current: string; added: number;
  cancelled: boolean; finished: boolean; secs: number; seq: number; lock: boolean; why?: string; error?: string; found: FmScanHit[];
}
export function parseFmScan(m: any): FmScanState | null {
  if (!m || m.type !== 'fm_scan') return null;
  const found: FmScanHit[] = Array.isArray(m.found) ? m.found.map((h: any) => ({
    khz: num(h?.khz), pi: str(h?.pi), ps: str(h?.ps).trim(), psComplete: h?.psComplete === true, snr: num(h?.snr),
  })).filter((h: FmScanHit) => h.khz > 0 && h.pi) : [];
  return {
    running: m.running === true, by: str(m.by), done: num(m.done), total: num(m.total), current: str(m.current),
    added: num(m.added), cancelled: m.cancelled === true, finished: m.finished === true, secs: num(m.secs), seq: num(m.seq),
    lock: m.lock === true, why: str(m.why) || undefined, error: str(m.error) || undefined, found,
  };
}
/** "Scanning FM 3 / 12 hops · 92.1–94.3 MHz — 7 stations so far" / "14 FM stations — 9 added to bookmarks". */
export function fmScanLine(s: FmScanState | null): string {
  if (!s) return '';
  if (s.error) return s.error;
  const n = s.found.length;
  if (s.running) return `Scanning FM ${s.done} / ${s.total} hops${s.current ? ` · ${s.current}` : ''}`
                      + (n ? ` — ${plural(n, 'station')} so far` : '');
  if (!s.finished) return '';
  const tail = `${plural(s.added, 'station')} added to bookmarks`;
  if (s.cancelled) return `FM scan stopped${s.why ? ` (${s.why.replace(/^stopped — /, '')})` : ''} at ${s.done} / ${s.total} — `
                        + `${plural(n, 'station')}, ${tail}`;
  return n ? `${plural(n, 'FM station')} with RDS — ${tail}`
           : `No FM station with RDS found across the band${s.why ? ` (${s.why})` : ''}. Check the aerial is up and connected.`;
}
