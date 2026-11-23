// logRadio.ts — V12 beta92 THE LOGS' RADIO FILTER (docs/v12/LOG-RADIO-FILTER.md): one rule for the web client
// (web/client/src/main.ts) and the app (GalleryOverlay, MapOverlay's digi modes map).
//
// ★★★ Stuart, 2026-11-22: the logs stay ONE PER SERVER ("we dont separate the digital spots, navtex etc") — "in the logs
//     themselves we could filter by sdr if multiple picked up the same signal". So the server tags each entry with the
//     radio(s) that heard it (vibe_log_radio.h) and names them (gallery.json / stations.json "radios"); this file decides
//     WHEN a filter is offered and WHAT passes it. Filtering is a view: nothing is split or deleted.
// ★ The filter appears ONLY when the log holds entries from two or more radios — a one-radio server, or a log only one
//   radio has written to, draws nothing (a control that cannot change anything is not offered).
// ★ An entry with no radio (written before beta92, or by a radio with no id) is UNKNOWN and shows under every radio.
// ★ Pure — scripts/test_log_radio.ts.

/** One radio as a log API lists it: its id (what links use — never a serial), its name as the landing page shows it,
 *  and how many entries of this log it holds. */
export type LogRadio = { id: string; name: string; n?: number };

const ID_RE = /^[0-9a-z]{1,16}$/;

/** The radios worth a filter: those with entries, when there are at least two of them. Otherwise null (draw nothing). */
export function logRadioChoices(radios: unknown): LogRadio[] | null {
  if (!Array.isArray(radios)) return null;
  const out: LogRadio[] = [];
  for (const r of radios) {
    if (!r || typeof r !== 'object') continue;
    const id = String((r as any).id || ''), name = String((r as any).name || '').trim();
    const n = Number((r as any).n);
    if (!ID_RE.test(id) || out.some((o) => o.id === id)) continue;
    if (Number.isFinite(n) && n <= 0) continue;
    out.push({ id, name: name || id, n: Number.isFinite(n) ? n : undefined });
  }
  return out.length >= 2 ? out : null;
}

/** The ids an entry carries: a gallery line's "ri" ("a1b2,c3d4") or a station's "radios" (["a1b2"]). [] = unknown. */
export function entryRadios(v: unknown): string[] {
  const list = Array.isArray(v) ? v.map(String) : typeof v === 'string' ? v.split(',') : [];
  return list.map((s) => s.trim()).filter((s, i, a) => ID_RE.test(s) && a.indexOf(s) === i);
}

/** Does an entry pass the filter? '' = All. An entry with no radio (unknown) passes every radio. */
export function radioPasses(v: unknown, pick: string): boolean {
  if (!pick) return true;
  const ids = entryRadios(v);
  return ids.length === 0 || ids.includes(pick);
}

/** The pick kept only while it is still one of the choices (a radio removed, or a log only one radio holds → All). */
export function keepRadioPick(pick: string, choices: LogRadio[] | null): string {
  return pick && choices && choices.some((c) => c.id === pick) ? pick : '';
}

/** "&radio=<id>" for a log API, '' for All. */
export function radioQuery(pick: string): string {
  return pick && ID_RE.test(pick) ? `&radio=${pick}` : '';
}

/** "Heard by RSP1A HF and RTL VHF" — for an entry more than one radio heard (null when one or none, or no names). */
export function heardByText(v: unknown, choices: LogRadio[] | null): string | null {
  const ids = entryRadios(v);
  if (!choices || ids.length < 2) return null;
  const names = ids.map((id) => choices.find((c) => c.id === id)?.name || id);
  return `Heard by ${names.length === 2 ? names.join(' and ') : `${names.slice(0, -1).join(', ')} and ${names[names.length - 1]}`}`;
}

/** The chip label: "RSP1A HF · 120" (the count when the server gave one). */
export function radioChipLabel(r: LogRadio): string {
  return typeof r.n === 'number' ? `${r.name} · ${r.n.toLocaleString('en-GB')}` : r.name;
}
