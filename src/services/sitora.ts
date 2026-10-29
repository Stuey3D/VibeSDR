/**
 * SITOR-A (AMTOR mode A, the maritime ARQ teleprinter) — the server's status for the decoder box, shared by the web
 * client (web/client/src/main.ts) and the app (DecoderPanel). The server is android/app/src/main/cpp/vibe_sitora_run.h:
 *   0x01 text   — the printed text; each passage starts "[n · IDENT] " (the sending station, its identity once heard),
 *                 protocol events are lines in [ ];
 *   0x03 state  — the link (SITORA_LINKS below);
 *   {"type":"sitora_status","data":{link,linkName,iss,master,heard,ids,numbers,idChecked,blocks,repeats,lost,
 *                                   combined,snr,offset,inverted}}
 */

/** The link states, by the 0x03 byte (decoders/sitor_a.h Link). "phasing" is the call-up. */
export const SITORA_LINKS = ['no signal', 'searching', 'phasing', 'identification', 'traffic', 'idle', 'change-over', 'ended'] as const;

export interface SitorStatus {
  link: number;
  iss: number;          // the station sending now (0/1), -1 unknown
  master: number;       // the calling station (0/1), -1 not yet known
  heard: [boolean, boolean];
  ids: [string, string];
  numbers: [string, string];
  idChecked: boolean;
  blocks: number;
  repeats: number;      // the RQ count: blocks asked for again
  lost: number;
  combined: number;
  snr: [number, number];
}

const str = (v: unknown, max: number) => (typeof v === 'string' ? v.replace(/[^A-Z0-9]/g, '').slice(0, max) : '');
const num = (v: unknown, d = 0) => (typeof v === 'number' && Number.isFinite(v) ? v : d);
const pair = <T,>(v: unknown, f: (x: unknown) => T): [T, T] => (Array.isArray(v) ? [f(v[0]), f(v[1])] : [f(undefined), f(undefined)]);

/** The server's status, checked field by field (nothing is trusted to be the right type). */
export function parseSitorStatus(d: unknown): SitorStatus | null {
  if (!d || typeof d !== 'object') return null;
  const o = d as Record<string, unknown>;
  return {
    link: Math.max(0, Math.min(SITORA_LINKS.length - 1, Math.round(num(o.link)))),
    iss: Math.round(num(o.iss, -1)),
    master: Math.round(num(o.master, -1)),
    heard: pair(o.heard, (x) => x === true),
    ids: pair(o.ids, (x) => str(x, 7)),
    numbers: pair(o.numbers, (x) => str(x, 9)),
    idChecked: o.idChecked === true,
    blocks: Math.max(0, Math.round(num(o.blocks))),
    repeats: Math.max(0, Math.round(num(o.repeats))),
    lost: Math.max(0, Math.round(num(o.lost))),
    combined: Math.max(0, Math.round(num(o.combined))),
    snr: pair(o.snr, (x) => num(x)),
  };
}

/** "QCXT (32610)", "station 2" — a station as the box names it. */
export function sitorStation(st: SitorStatus, i: number): string {
  if (i < 0 || i > 1) return '';
  const id = st.ids[i], n = st.numbers[i];
  const role = st.master === i ? 'caller' : st.master === 1 - i ? 'called' : '';
  const name = id ? (n && n !== id ? `${id} (${n})` : id) : `station ${i + 1}`;
  return role ? `${name}, ${role}` : name;
}

/** The box's status line: the link state, who is printing, the RQ count — e.g.
 *  "TRAFFIC · printing PEARDBY (364775427), caller · RQ 12 · 2 lost". */
export function sitorStatusText(st: SitorStatus | null, state?: number): string {
  const link = st ? st.link : Math.max(0, Math.min(SITORA_LINKS.length - 1, state ?? 0));
  const parts: string[] = [SITORA_LINKS[link].toUpperCase()];
  if (st) {
    if (st.iss >= 0 && link >= 2 && link <= 6) parts.push(`printing ${sitorStation(st, st.iss)}`);
    if (st.blocks || st.repeats) parts.push(`RQ ${st.repeats}`);
    if (st.lost) parts.push(`${st.lost} lost`);
    if (st.idChecked) parts.push('identity confirmed');
  }
  return parts.join(' · ');
}

/** The idle hint: where SITOR-A is and how to tune it. */
export const SITORA_HINT = 'SITOR-A (ARQ) — maritime teleprinter, 100 Bd, 170 Hz shift. Tune USB so the two tones sit around 1000 Hz; both stations of a link are printed, each under its own label.';
