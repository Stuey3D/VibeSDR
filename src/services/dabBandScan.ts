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

export interface DabScanService { sid: number; label: string }
export interface DabScanHit {
  block: string; channel: number; hz: number; ensemble: string;
  fib: number; mer: number; complete: boolean; services: DabScanService[];
}
export interface DabScanState {
  running: boolean; by: string; done: number; total: number; current: string;
  added: number; cancelled: boolean; finished: boolean; secs: number; seq: number;
  why?: string; error?: string; found: DabScanHit[];
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
    why: str(m.why) || undefined, error: str(m.error) || undefined, found,
  };
}

/** Block order (the server walks upward; a client may get a late message out of order). */
export function dabScanRows(s: DabScanState | null): DabScanHit[] {
  return s ? s.found.slice().sort((a, b) => a.channel - b.channel) : [];
}

/** "222.064 MHz" */
export function dabScanMHz(hz: number): string { return hz > 0 ? `${(hz / 1e6).toFixed(3)} MHz` : ''; }

/** FIC quality in words a listener reads at a glance — the FIB rate, the same measure the ensemble memory uses. */
export function dabScanQuality(h: DabScanHit): { pct: number; word: 'strong' | 'good' | 'weak' } {
  const pct = Math.round(h.fib * 100);
  return { pct, word: h.fib > 0.9 ? 'strong' : h.fib > 0.6 ? 'good' : 'weak' };
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
