/**
 * ★★★ V12 PAGING (POCSAG, AUTO) — ANDROID LOCAL HARDWARE ONLY: the phone's own plugged-in radio, in this app. The engine
 * runs in the phone's own VibeServer (android/app/src/main/cpp/vibe_paging_run.h) and offers itself in vibeserver.json
 * (`"paging": true` + `pagingModes`) only there; a server never does, so no other client draws the key.
 *
 * ★★ THE MANUAL CHOICES ARE THE SERVER'S DATA (`pagingModes`, AUTO first): one key per entry, so a FLEX receiver later is
 *    more entries and not a new client. Nothing here lists the rates.
 * ★ Nothing paging is kept: no gallery, no file — the text box is the only place a page appears.
 */
export interface PagingMode { id: string; label: string }

/** `pagingModes` from vibeserver.json, or [] (ids and labels are short words; anything else is dropped). */
export function parsePagingModes(v: unknown): PagingMode[] {
  if (!Array.isArray(v)) return [];
  const out: PagingMode[] = [];
  for (const m of v) {
    const id = typeof m?.id === 'string' ? m.id : '';
    const label = typeof m?.label === 'string' ? m.label : '';
    if (/^[a-z0-9]{1,24}$/.test(id) && /^[A-Za-z0-9 /.-]{1,24}$/.test(label)) out.push({ id, label });
  }
  return out;
}

export interface PagingStatus { mode: string; detected: string; pages: number; rate: number; short: boolean }
export function parsePagingStatus(d: any): PagingStatus | null {
  if (!d || typeof d !== 'object') return null;
  return {
    mode: typeof d.mode === 'string' ? d.mode : 'auto',
    detected: typeof d.detected === 'string' ? d.detected.slice(0, 32) : '',
    pages: Number.isFinite(d.pages) ? d.pages : 0,
    rate: Number.isFinite(d.rate) ? d.rate : 0,
    short: d.short === true,
  };
}
/** The status line: what is locked (or that it is listening), the page count, and the narrow-filter warning. */
export function pagingStatusText(s: PagingStatus | null): string {
  if (!s) return 'listening…';
  const head = s.detected ? s.detected : s.mode === 'auto' ? 'listening — AUTO (rate from the signal)' : 'listening';
  const n = s.pages ? ` · ${s.pages} page${s.pages === 1 ? '' : 's'}` : '';
  const w = s.short ? ' · filter narrow: widen NFM to 12.5 kHz or more' : '';
  return head + n + w;
}
export const PAGING_HINT =
  'Tune a paging channel in NFM with the filter at 12.5 kHz or wider. AUTO finds the rate (POCSAG 512, 1200 or 2400) ' +
  'and names it; pick a rate only if AUTO cannot lock. The speaker is muted while PAGE runs and comes back as it was ' +
  'when you close it. Pages are shown here only — never saved or sent anywhere.';
