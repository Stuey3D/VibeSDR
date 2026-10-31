/**
 * ★★★ V12 PAGING (POCSAG and FLEX, AUTO) — ANDROID LOCAL HARDWARE ONLY: the phone's own plugged-in radio, in this app. The engine
 * runs in the phone's own VibeServer (android/app/src/main/cpp/vibe_paging_run.h) and offers itself in vibeserver.json
 * (`"paging": true` + `pagingModes`) only there; a server never does, so no other client draws the key.
 *
 * ★★ THE MANUAL CHOICES ARE THE SERVER'S DATA (`pagingModes`, AUTO first): one key per entry, so a FLEX receiver later is
 *    more entries and not a new client. Nothing here lists the rates.
 * ★ Nothing paging is kept: no gallery, no file — the text box is the only place a page appears.
 */
import { parseGroupedModes, type GroupedMode } from './autoModes';
export type PagingMode = GroupedMode;

/** `pagingModes` from vibeserver.json, or [] — each {id, label, group} (autoModes.ts: PROTOCOL then RATE rows). */
export function parsePagingModes(v: unknown): PagingMode[] { return parseGroupedModes(v, 16); }

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
  'Tune a paging channel in NFM with the filter at 12.5 kHz or wider. AUTO finds the protocol and the rate — POCSAG ' +
  '512, 1200 or 2400, or FLEX 1600, 3200 or 6400 — and names it; if AUTO cannot lock, pick the protocol (and for POCSAG ' +
  'the rate) below. FLEX names its own speed in every frame. The speaker is muted while PAGE runs and comes back as it was ' +
  'when you close it. Pages are shown here only — never saved or sent anywhere.';
