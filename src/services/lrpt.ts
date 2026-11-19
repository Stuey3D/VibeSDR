// lrpt.ts — METEOR-M LRPT weather-satellite pictures (V12, 2026-11-19; docs/v12/LRPT.md): what the app and the web page
// both need to start the decoder and read its status. The picture itself comes as the SSTV frames (0x07 start · 0x01
// line · 0x05 complete), a half-size colour preview growing during the pass; the full-size PNGs go to the gallery.

/** {"type":"lrpt_status","data":{…}} once a second (vibe_lrpt_run.h Run::status). */
export interface LrptStatus {
  carrier: boolean;      // the demodulator is locked to the satellite's carrier
  sync: boolean;         // frames are being found
  mode: string;          // "QPSK" | "OQPSK" (AUTO alternates until frames decode)
  diff: number;          // -1 not yet known, 0 plain, 1 differentially coded
  symbolRate: number;
  lock: number;          // constellation lock, 0–100 (≈ 64 on noise, ≥ 80 locked)
  offsetHz: number;      // the carrier's offset from the dial (the Doppler left over)
  frames: number;        // frames that passed Reed–Solomon
  bad: number;           // frames that did not
  packets: number;
  lines: number;         // picture lines so far
  channels: string[];    // live MSU-MR channels ("1 visible", "2 near-IR", "5 thermal" …)
  why?: string;          // a reason it cannot run here (the IQ slice's rate)
}

export function parseLrptStatus(d: unknown): LrptStatus | null {
  if (!d || typeof d !== 'object') return null;
  const o = d as Record<string, unknown>;
  const n = (k: string) => (typeof o[k] === 'number' && isFinite(o[k] as number) ? (o[k] as number) : 0);
  return {
    carrier: o.carrier === true,
    sync: o.sync === true,
    mode: typeof o.mode === 'string' ? (o.mode as string).slice(0, 8) : '',
    diff: n('diff'),
    symbolRate: n('symbolRate'),
    lock: n('lock'),
    offsetHz: n('offsetHz'),
    frames: Math.max(0, Math.round(n('frames'))),
    bad: Math.max(0, Math.round(n('bad'))),
    packets: Math.max(0, Math.round(n('packets'))),
    lines: Math.max(0, Math.round(n('lines'))),
    channels: Array.isArray(o.channels) ? (o.channels as unknown[]).filter((c) => typeof c === 'string').map((c) => (c as string).slice(0, 24)).slice(0, 6) : [],
    why: typeof o.why === 'string' && o.why ? (o.why as string).slice(0, 160) : undefined,
  };
}

/** The decoder box's status line. */
export function lrptStatusText(st: LrptStatus | null): string {
  if (!st) return 'waiting for a METEOR pass';
  if (st.why) return st.why;
  if (st.lines > 0) {
    const ch = st.channels.length ? ` · channels ${st.channels.map((c) => c.split(' ')[0]).join(', ')}` : '';
    return `${st.lines} lines${ch} · ${st.frames} frames${st.sync ? '' : ' · signal lost'}`;
  }
  if (st.sync) return `${st.mode} locked · frames ${st.frames}${st.bad ? ` (${st.bad} unreadable)` : ''}`;
  if (st.carrier) return `carrier found (${st.mode}, ${st.offsetHz >= 0 ? '+' : ''}${st.offsetHz} Hz) · looking for frames`;
  return 'listening for a METEOR satellite';
}

export const LRPT_HINT =
  'METEOR-M weather satellite pictures (LRPT): tune 137.100 or 137.900 MHz while a METEOR satellite passes over (a pass ' +
  'lasts up to 15 minutes; a 137 MHz QFH or V-dipole aerial helps a lot). The picture grows here during the pass; each ' +
  "pass's channels and colour composite are saved to the gallery as PNG.";

/** The attach parameters: the symbol rate is the satellite's (72000, or 80000 for the 80k mode); the rest AUTO. */
export function lrptAttachParams(symbolRate = 72000): Record<string, unknown> {
  return { symbol_rate: symbolRate >= 76000 ? 80000 : 72000, modulation: 'auto', diff: 'auto' };
}
