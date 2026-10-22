/**
 * icomRadio.ts — an ICOM TRANSCEIVER as the server describes it (driver "icom", RECEIVE ONLY), and the
 * decisions every client makes from that description. Shared by the app (sdrProtocol / SDRScreen / the
 * Icom panel) and the web client (web/client/src/main.ts imports this file directly), so the two can
 * never disagree about what an Icom shows — ONE RULE, TWO READERS.
 *
 * ★★★ THE CONTRACT IS THE SERVER'S, not ours: docs/ICOM-SUPPORT-PLAN.md "hwinfo + wire contract for
 *     clients (built)", built by radioCapsJsonCore()'s icom branch and icomStatusJson() in
 *     android/app/src/main/cpp/local_sdr_shim.cpp. Every list here (modes, FIL widths, ATT steps, preamp,
 *     AGC, spans, scope speeds) is the RADIO's, read off hwinfo.radio — never a constant of ours
 *     (feedback_app_no_hardcoded_settings). A field the server did not send stays ABSENT; it is not
 *     invented as "off".
 * ★★★ AN IC-7300 IS NOT AN IQ RADIO. What it gives a computer is a 475-point scope at a low rate, the
 *     radio's own demodulated audio and CI-V control. So everything the IQ path powers (sample rate, the
 *     gain table, VibeAGC, the IF ladder, RDS, DAB, raw IQ, multi-VFO, fps tiers, Spatialise, binaural,
 *     digital zoom …) does not exist here and must not be DRAWN (AGENTS.md: "a control that only works on
 *     one radio should not be there") — see ICOM_HIDDEN_CONTROLS.
 * ★★ RECEIVE ONLY. The server reports `canTransmit:false` unconditionally and there is no message a client
 *    can send that keys the radio. Nothing in this file builds one; icomControlMsg() only ever emits the
 *    receive-side fields the server's icom_control accepts.
 * ★ No imports, no React Native, no DOM: node runs this file directly for scripts/test_icom_radio.ts.
 */

export interface IcomPreflightItem {
  name: string; value: string; ok: boolean; gate: boolean;
  /** ★ The exact radio menu path to change it ("MENU > SET > Connectors > …"). */
  where: string;
}
export interface IcomPreflight {
  passed: boolean;
  /** false = the menu item NUMBERS are not yet confirmed on a real radio (plan "Open for Stuart"). */
  verified: boolean;
  summary: string;
  items: IcomPreflightItem[];
}
export interface IcomScope {
  points: number;
  /** TOTAL widths, Hz — the radio's span ladder (5 kHz … 1 MHz on a 7300). */
  spans: number[];
  span: number;
  modes: string[];
  mode: string;          // 'centre' | 'fixed'
  speeds: string[];
  /** Sweeps per second the server MEASURES (~19 at 115200 baud, ~3 at 19200). 0 = none yet. */
  fps: number;
}
/** hwinfo.radio for driver "icom". */
export interface IcomRadio {
  driver: 'icom';
  model: string;
  civ: string;
  link: string;
  transport: string;
  linesHeldLow: boolean;
  baud: number;
  noIq: true;
  sharedOnly: true;
  canTransmit: false;
  modes: string[];
  modeNow: string;
  filters: number[];
  filNow: number;
  filterWidths: Record<string, number[]>;
  att: number[]; attNow: number;
  preamp: number[]; preampNow: number;
  agc: string[]; agcNow: string;
  /** [min, max] — 0..255 on a 7300; null = this radio publishes no RF gain. */
  rfGain: [number, number] | null; rfGainNow: number;
  nb: boolean; nbNow: boolean;
  nr: boolean; nrNow: boolean;
  anf: boolean; anfNow: boolean;
  scope: IcomScope;
  ranges: [number, number][];
  tx: boolean;
  linkUp: boolean;
  ownerLock: boolean;
  rfOpen: boolean;
  audio: string;
  audioProblem: string;
  preflight: IcomPreflight | null;
  refusedFrames: number;
}
/** The `{"type":"icom", …}` status line — on join, on every change, the S-meter ≤ 4/s. */
export interface IcomStatus {
  freq: number; mode: string; fil: number;
  /** Raw 0-255 meter; < 0 = not read yet. */
  sRaw: number;
  /** dB relative to S9 (0 = S9, −54 = S0, +60 = S9+60). null when the meter has not been read. */
  sDbOverS9: number | null;
  tx: boolean; link: boolean; fps: number; span: number; scopeMode: string;
}

const num = (v: unknown, d = 0): number => (typeof v === 'number' && Number.isFinite(v) ? v : d);
const str = (v: unknown, d = ''): string => (typeof v === 'string' ? v : d);
const bool = (v: unknown): boolean => v === true;
const nums = (v: unknown): number[] =>
  Array.isArray(v) ? v.filter((x): x is number => typeof x === 'number' && Number.isFinite(x)) : [];
const strs = (v: unknown): string[] =>
  Array.isArray(v) ? v.filter((x): x is string => typeof x === 'string' && x.length > 0) : [];

/** True when hwinfo.radio (or anything carrying a `driver`) is an Icom. ★ Branch on THIS before any
 *  default — "else means dongle" has produced a bug every time a new radio arrived. */
export function isIcom(radio: unknown): boolean {
  return !!radio && typeof radio === 'object' && (radio as { driver?: unknown }).driver === 'icom';
}

/** hwinfo.radio → IcomRadio, defensively (a field of the wrong type is dropped, never trusted). null =
 *  not an Icom. */
export function parseIcomRadio(raw: unknown): IcomRadio | null {
  if (!isIcom(raw)) return null;
  const r = raw as Record<string, unknown>;
  const sc = (r.scope && typeof r.scope === 'object' ? r.scope : {}) as Record<string, unknown>;
  const fw: Record<string, number[]> = {};
  if (r.filterWidths && typeof r.filterWidths === 'object')
    for (const [k, v] of Object.entries(r.filterWidths as Record<string, unknown>)) {
      const w = nums(v);
      if (w.length) fw[k] = w;
    }
  const rg = nums(r.rfGain);
  const ranges: [number, number][] = Array.isArray(r.ranges)
    ? (r.ranges as unknown[]).map(nums).filter((p) => p.length === 2 && p[1] > p[0]).map((p) => [p[0], p[1]] as [number, number])
    : [];
  let preflight: IcomPreflight | null = null;
  if (r.preflight && typeof r.preflight === 'object') {
    const p = r.preflight as Record<string, unknown>;
    preflight = {
      passed: bool(p.passed), verified: bool(p.verified), summary: str(p.summary),
      items: Array.isArray(p.items) ? (p.items as unknown[]).filter((i) => i && typeof i === 'object').map((i) => {
        const it = i as Record<string, unknown>;
        return { name: str(it.name), value: str(it.value), ok: bool(it.ok), gate: bool(it.gate), where: str(it.where) };
      }) : [],
    };
  }
  return {
    driver: 'icom',
    model: str(r.model, 'Icom'), civ: str(r.civ), link: str(r.link), transport: str(r.transport),
    linesHeldLow: bool(r.linesHeldLow), baud: num(r.baud),
    noIq: true, sharedOnly: true, canTransmit: false,   // ★ unconditional in this phase, whatever arrives
    modes: strs(r.modes), modeNow: str(r.modeNow),
    filters: nums(r.filters), filNow: num(r.filNow),
    filterWidths: fw,
    att: nums(r.att), attNow: num(r.attNow),
    preamp: nums(r.preamp), preampNow: num(r.preampNow),
    agc: strs(r.agc), agcNow: str(r.agcNow),
    rfGain: rg.length === 2 && rg[1] > rg[0] ? [rg[0], rg[1]] : null, rfGainNow: num(r.rfGainNow),
    nb: bool(r.nb), nbNow: bool(r.nbNow), nr: bool(r.nr), nrNow: bool(r.nrNow), anf: bool(r.anf), anfNow: bool(r.anfNow),
    scope: {
      points: num(sc.points), spans: nums(sc.spans).filter((s) => s > 0), span: num(sc.span),
      modes: strs(sc.modes), mode: str(sc.mode, 'centre'), speeds: strs(sc.speeds), fps: num(sc.fps),
    },
    ranges,
    tx: bool(r.tx),
    // ★ ABSENT is not DOWN: only an explicit false says the radio stopped answering.
    linkUp: r.linkUp !== false,
    // ★ The server's default is ON (plan §3.4); an absent field must not unlock the radio in the UI.
    ownerLock: r.ownerLock !== false,
    rfOpen: bool(r.rfOpen),
    audio: str(r.audio), audioProblem: str(r.audioProblem),
    preflight,
    refusedFrames: num(r.refusedFrames),
  };
}

/** The `{"type":"icom"}` line → IcomStatus. null = not one. */
export function parseIcomStatus(raw: unknown): IcomStatus | null {
  if (!raw || typeof raw !== 'object' || (raw as { type?: unknown }).type !== 'icom') return null;
  const m = raw as Record<string, unknown>;
  const sRaw = num(m.sRaw, -1);
  const sDb = num(m.sDbOverS9, -999);
  return {
    freq: num(m.freq), mode: str(m.mode), fil: num(m.fil), sRaw,
    sDbOverS9: sRaw >= 0 && sDb > -999 ? sDb : null,
    tx: bool(m.tx), link: m.link !== false, fps: num(m.fps), span: num(m.span), scopeMode: str(m.scopeMode, 'centre'),
  };
}

/** ★ The status line is NEWER than the last hwinfo for the fields it carries (mode, FIL, TX, link, span,
 *  scope mode, rate) — the server sends hwinfo only on RF/scope/TX/link changes, the line on every one.
 *  Returns a new object; never mutates. */
export function mergeIcomStatus(radio: IcomRadio, st: IcomStatus | null): IcomRadio {
  if (!st) return radio;
  return {
    ...radio,
    modeNow: st.mode || radio.modeNow,
    filNow: st.fil > 0 ? st.fil : radio.filNow,
    tx: st.tx,
    linkUp: st.link,
    scope: {
      ...radio.scope,
      span: st.span > 0 ? st.span : radio.scope.span,
      mode: st.scopeMode || radio.scope.mode,
      fps: st.fps > 0 ? st.fps : radio.scope.fps,
    },
  };
}

// ── What does NOT apply (plan §2.3, contract "Client controls that do NOT apply on an Icom") ──────────

/** ★★★ Every IQ-side control a client must HIDE on an Icom — not disable, hide (a greyed control still
 *  reads as an offer, and an inert one teaches the listener the FEATURE is broken). Each id names a
 *  control family both clients draw; tests assert the list against the contract. */
export const ICOM_HIDDEN_CONTROLS = [
  'sampleRate',        // the rate picker — the only rate on an Icom is its audio's
  'gainSlider',        // the dongle gain slider / gain table
  'vibeAgc',           // VibeAGC and its lock
  'ifFilter',          // the IF filter ladder + Auto IF
  'directSampling',    // manual and auto
  'biasT',
  'ppm',               // ppm / ppb
  'radioPanels',       // the dongle / RSP / HF+ / Airspy / HackRF panels
  'squelch',           // OUR squelch
  'noiseBlanker',      // OUR blanker (the radio's NB is drawn instead)
  'rds',               // RDS, Advanced RDS, MPX, stereo pilot
  'stereo', 'deemphasis', 'nfmVoice',
  'dab',               // DAB and the DAB landing
  'rawIq', 'iqRecording', 'vibeIq',
  'multiVfo',          // multi-VFO / locked centre / per-listener VFO
  'fpsTiers',          // spectrum fps tiers + the governor display
  'spatialise', 'binaural',
  'digitalZoom',       // any zoom past the span ladder
  'iqDecoders',        // HFDL and other IQ decoders (audio decoders DO work)
  'wfmMode',           // WFM (and DAB) are not modes this radio has
] as const;
export type IcomHiddenControl = typeof ICOM_HIDDEN_CONTROLS[number];

/** True when `control` must not be drawn for this radio. Anything that is not an Icom hides nothing here. */
export function icomHides(radio: unknown, control: IcomHiddenControl | string): boolean {
  return isIcom(radio) && (ICOM_HIDDEN_CONTROLS as readonly string[]).includes(control);
}

// ── Who may do what (the OWNER LOCK and RF-admin, plan §3.4 / decision 6) ───────────────────────────

/** ★ Tune, mode, FIL, span and scope mode: anyone unless the owner lock is on, then a SIGNED-IN admin. */
export function icomMayTune(radio: IcomRadio | null, admin: boolean): boolean {
  return !!radio && (admin || !radio.ownerLock);
}
/** ★ ATT, preamp, AGC, RF gain, NB/NR/ANF, scope speed: admin-only unless the owner opened them. */
export function icomMayRf(radio: IcomRadio | null, admin: boolean): boolean {
  return !!radio && (admin || radio.rfOpen);
}
/** The note a listener sees under the owner lock ('' = none to show). */
export function icomFollowNote(radio: IcomRadio | null, admin: boolean): string {
  if (!radio || admin || !radio.ownerLock) return '';
  return 'Everyone hears what the radio is tuned to — the owner tunes it.';
}

// ── Labels ───────────────────────────────────────────────────────────────────────────────────────

/** The radio's mode as the radio's own panel writes it: usbd → USB-D, cwr → CW-R, rttyr → RTTY-R. */
export function icomModeLabel(m: string): string {
  const s = String(m || '').toLowerCase();
  if (!s) return '';
  if (s.endsWith('d') && (s === 'usbd' || s === 'lsbd' || s === 'amd' || s === 'fmd')) return `${s.slice(0, -1).toUpperCase()}-D`;
  if (s === 'cwr') return 'CW-R';
  if (s === 'rttyr') return 'RTTY-R';
  return s.toUpperCase();
}

/** The radio mode → the client vocabulary (the server's own icomToClientMode, for the passband). */
export function icomClientMode(m: string): string {
  switch (String(m || '').toLowerCase()) {
    case 'usb': case 'usbd': case 'rttyr': return 'usb';
    case 'lsb': case 'lsbd': case 'rtty': return 'lsb';
    case 'am': return 'am';
    case 'cw': return 'cwu';
    case 'cwr': return 'cwl';
    case 'fm': return 'nfm';
  }
  return 'usb';
}

/** The FIL width family a mode belongs to — the server's icom::widthFamily(), letter for letter. */
export function icomWidthFamily(mode: string): string {
  switch ((String(mode || 'usb').toLowerCase())[0]) {
    case 'l': case 'u': return 'ssb';
    case 'c': return 'cw';
    case 'r': return 'rtty';
    case 'a': return 'am';
    case 'f': return 'fm';
  }
  return 'ssb';
}

/** FIL1/2/3 widths (Hz) for a mode — the radio's FACTORY widths; the owner may have changed them in the
 *  radio and CI-V does not report that, so a passband drawn from these is approximate. [] = unknown. */
export function icomFilterWidths(radio: IcomRadio | null, mode?: string): number[] {
  if (!radio) return [];
  return radio.filterWidths[icomWidthFamily(mode ?? radio.modeNow)] ?? [];
}

/** 2400 → "2.4k", 500 → "500", 15000 → "15k". */
export function fmtWidth(hz: number): string {
  if (!(hz > 0)) return '';
  if (hz < 1000) return String(Math.round(hz));
  const k = hz / 1000;
  return `${Number.isInteger(k) ? k : k.toFixed(1).replace(/\.0$/, '')}k`;
}

/** A span rung as the radio labels it: the HALF width, "±25k" for a 50 kHz span. */
export function fmtSpan(totalHz: number): string {
  if (!(totalHz > 0)) return '';
  const half = totalHz / 2;
  return half >= 1e6 ? `±${half / 1e6}M` : `±${fmtWidth(half)}`;
}

/** The index of the rung nearest `totalHz` (log distance, like the server's nearestSpan). -1 = no ladder. */
export function nearestSpanIndex(spans: number[], totalHz: number): number {
  let best = -1, bd = Infinity;
  for (let i = 0; i < spans.length; i++) {
    const d = Math.abs(Math.log(spans[i] / Math.max(1, totalHz)));
    if (d < bd) { bd = d; best = i; }
  }
  return best;
}

/** ★ ZOOM IS THE SPAN LADDER. One step narrower (dir > 0 = zoom in) or wider from `current`; the same
 *  rung back when there is none further. 0 = no ladder. */
export function spanStep(spans: number[], current: number, dir: number): number {
  if (!spans.length) return 0;
  const sorted = [...spans].sort((a, b) => a - b);
  const i = nearestSpanIndex(sorted, current);
  const j = Math.max(0, Math.min(sorted.length - 1, i + (dir > 0 ? -1 : dir < 0 ? 1 : 0)));
  return sorted[j];
}

/** ATT step label: 0 → "OFF", 20 → "20 dB". */
export function fmtAtt(db: number): string { return db > 0 ? `${db} dB` : 'OFF'; }
/** Preamp label as the radio writes it: 0 → OFF, 1 → P.AMP1, 2 → P.AMP2. */
export function fmtPreamp(n: number): string { return n > 0 ? `P.AMP${n}` : 'OFF'; }
/** RF gain 0-255 as a percentage, the way the radio's own knob reads. */
export function rfGainPct(v: number, range: [number, number] | null): number {
  if (!range) return 0;
  return Math.round((100 * (v - range[0])) / (range[1] - range[0]));
}

// ── The S-meter (from the status line — NEVER from SPEC bins, which are relative to REF) ──────────────

export interface SMeterReading {
  /** "S7", "S9", "S9+20". */
  text: string;
  /** 0..1 along a bar S0 … S9+60. */
  frac: number;
  /** True above S9 (the red half of a real meter). */
  over: boolean;
}
/** dB-over-S9 → S-units: S9 + dB/6 below S9 (6 dB per S-unit, the IARU convention the server's
 *  calibration uses), "S9+N" (dB) above. null = the meter has not been read. */
export function sMeter(sDbOverS9: number | null | undefined): SMeterReading | null {
  if (sDbOverS9 == null || !Number.isFinite(sDbOverS9)) return null;
  const db = Math.max(-54, Math.min(60, sDbOverS9));
  // ★ The bar is a real meter's: S0-S9 fills the first 60 % (54 dB), S9+60 the rest (60 dB).
  const frac = db <= 0 ? ((db + 54) / 54) * 0.6 : 0.6 + (db / 60) * 0.4;
  if (db >= 1) return { text: `S9+${Math.round(db)}`, frac, over: true };
  const s = Math.max(0, Math.min(9, Math.round(9 + db / 6)));
  return { text: `S${s}`, frac, over: false };
}

// ── The baud note (setup) ────────────────────────────────────────────────────────────────────────

/** ★★ The scope shares the CI-V serial line with everything else, so the BAUD RATE IS THE WATERFALL'S
 *  SPEED: about 19 sweeps a second at 115200, about 3 at 19200 (measured against the fake radio; the
 *  setup page says the same). A rough linear estimate for the others. */
export function baudSweepsEstimate(baud: number): number {
  if (!(baud > 0)) return 0;
  if (baud >= 115200) return 19;
  if (baud <= 19200) return Math.max(0.5, Math.round((3 * baud) / 19200 * 10) / 10);
  return Math.round(3 + ((baud - 19200) / (115200 - 19200)) * 16);
}
export function baudNote(baud: number): string {
  const est = baudSweepsEstimate(baud);
  const base = 'The scope shares this serial line with tuning, so the baud rate IS the waterfall’s speed: '
             + '115200 gives about 19 lines a second, 19200 only about 3.';
  if (!est) return base;
  return baud >= 115200 ? base : `${base} At ${baud} expect about ${est} a second — set the radio and the server to 115200.`;
}

/** The transport in plain words (the setup card's line). */
export function transportText(radio: IcomRadio): string {
  return radio.linesHeldLow
    ? 'USB chip driven directly — DTR/RTS held low'
    : 'serial device — DTR/RTS raised for an instant on open';
}

// ── What a client SENDS ────────────────────────────────────────────────────────────────────────────

export interface IcomControlFields {
  mode?: string; fil?: number; span?: number; scopeMode?: string; scopeSpeed?: string;
  att?: number; preamp?: number; agc?: string; rfGain?: number; nb?: boolean; nr?: boolean; anf?: boolean;
}
/** ★ Build `{"type":"icom_control", …}` — ONLY values the radio's own lists contain; anything else is
 *  dropped here so a stale UI can never send a value the server would have to refuse. null = nothing left.
 *  ★★ Receive-side fields only: there is no TX field to build, by design. */
export function icomControlMsg(radio: IcomRadio | null, f: IcomControlFields): ({ type: 'icom_control' } & IcomControlFields) | null {
  if (!radio) return null;
  const out: IcomControlFields = {};
  if (f.mode !== undefined && radio.modes.includes(f.mode)) out.mode = f.mode;
  if (f.fil !== undefined && radio.filters.includes(f.fil)) out.fil = f.fil;
  if (f.span !== undefined && radio.scope.spans.includes(f.span)) out.span = f.span;
  if (f.scopeMode !== undefined && radio.scope.modes.includes(f.scopeMode)) out.scopeMode = f.scopeMode;
  if (f.scopeSpeed !== undefined && radio.scope.speeds.includes(f.scopeSpeed)) out.scopeSpeed = f.scopeSpeed;
  if (f.att !== undefined && radio.att.includes(f.att)) out.att = f.att;
  if (f.preamp !== undefined && radio.preamp.includes(f.preamp)) out.preamp = f.preamp;
  if (f.agc !== undefined && radio.agc.includes(f.agc)) out.agc = f.agc;
  if (f.rfGain !== undefined && radio.rfGain && Number.isFinite(f.rfGain))
    out.rfGain = Math.round(Math.max(radio.rfGain[0], Math.min(radio.rfGain[1], f.rfGain)));
  if (f.nb !== undefined && radio.nb) out.nb = !!f.nb;
  if (f.nr !== undefined && radio.nr) out.nr = !!f.nr;
  if (f.anf !== undefined && radio.anf) out.anf = !!f.anf;
  return Object.keys(out).length ? { type: 'icom_control', ...out } : null;
}

// ── The banner (TX / link down), one wording for every client ─────────────────────────────────────

/** What the top-of-screen banner says, or '' for none. TX outranks link-down (a transmitting radio is
 *  answering). ★ The server has ALREADY muted the audio and frozen the waterfall during TX — the
 *  silence is not a fault, and the banner is what says so. */
export function icomBanner(radio: IcomRadio | null): string {
  if (!radio) return '';
  if (radio.tx) return 'The radio is transmitting — receive paused';
  if (!radio.linkUp) return 'The radio is not answering — VibeServer is retrying';
  return '';
}

// ── The owner's SETUP (the server's config, per radio — the same fields the setup page's Icom card saves) ──
/* ★★ ONE SET OF FIELDS, THREE EDITORS: the server's setup page (vibe_setup_page.h icomRender / collect()), the
 *  app's Icom setup card for a remote VibeServer (GET/POST /vibeserver/config at the door, with the admin
 *  ticket), and the app's own server screen (ServerModeScreen, Android Phase 4). Names and defaults are the
 *  setup page's, letter for letter, so a save from any of them reads back the same in the others. */

/** The radio's own CI-V USB baud menu (MENU > SET > Connectors > CI-V > CI-V USB Baud Rate). */
export const ICOM_BAUDS = [115200, 57600, 38400, 19200, 9600, 4800] as const;
/** Scope speed as the CONFIG stores it (0 fast, 1 mid, 2 slow) — the live control uses the names. */
export const ICOM_SCOPE_SPEED_NAMES = ['Fast', 'Mid', 'Slow'] as const;

export interface IcomSettings {
  icomBaud: number;
  /** 0 = the radio's own (0x94 on an IC-7300). */
  icomCiv: number;
  icomScopeSpeed: number;
  /** ★ Default ON — "listeners follow the radio" (plan §3.4). */
  icomOwnerLock: boolean;
  icomRfOpen: boolean;
  /** 'auto' finds the radio's USB audio; 'none' = waterfall and dial only; or a device name. */
  icomAudio: string;
  /** 'usb' = the radio's USB port by its own name, or a path. */
  icomPort: string;
  /** Allow a /dev/… serial device (DTR/RTS raised for an instant on open). */
  icomTtyOk: boolean;
}

/** A radio's config entry → its Icom settings, with the setup page's defaults. */
export function icomSettingsFrom(r: unknown): IcomSettings {
  const o = (r && typeof r === 'object' ? r : {}) as Record<string, unknown>;
  const baud = num(o.icomBaud, 115200);
  return {
    icomBaud: baud > 0 ? baud : 115200,
    icomCiv: Math.max(0, Math.min(0xff, Math.round(num(o.icomCiv, 0)))),
    icomScopeSpeed: [0, 1, 2].includes(num(o.icomScopeSpeed, 0)) ? num(o.icomScopeSpeed, 0) : 0,
    icomOwnerLock: o.icomOwnerLock !== false,
    icomRfOpen: o.icomRfOpen === true,
    icomAudio: (str(o.icomAudio) || 'auto').trim() || 'auto',
    icomPort: (str(o.icomPort) || 'usb').trim() || 'usb',
    icomTtyOk: o.icomTtyOk === true,
  };
}

/** "94" → 0x94, "" → 0 (the radio's own), anything that is not one or two hex digits → null. */
export function parseCivHex(t: string): number | null {
  const s = String(t ?? '').trim();
  if (!s) return 0;
  if (!/^[0-9a-f]{1,2}$/i.test(s)) return null;
  return parseInt(s, 16);
}
export function fmtCivHex(n: number): string { return n > 0 ? n.toString(16).toUpperCase().padStart(2, '0') : ''; }

/** ★ WHICH RADIO in the server's config is the Icom we are listening to. A radio behind a front door is
 *  `/r/<serial>`, so the serial from the connect address wins; on a single-radio server, the one Icom in the
 *  list. null = cannot tell (more than one Icom and no serial) — the card then sends the owner to the setup
 *  page rather than guess which transceiver to reconfigure. */
export function pickIcomSerial(cfg: unknown, connectPath?: string): string | null {
  const radios = (cfg && typeof cfg === 'object' && Array.isArray((cfg as { radios?: unknown }).radios)
    ? (cfg as { radios: unknown[] }).radios : []) as Record<string, unknown>[];
  const m = /\/r\/([^/?#]+)/.exec(connectPath || '');
  if (m) {
    const want = decodeURIComponent(m[1]);
    return radios.some((r) => r && r.serial === want) ? want : null;
  }
  const icoms = radios.filter((r) => r && r.driver === 'icom' && typeof r.serial === 'string');
  return icoms.length === 1 ? String(icoms[0].serial) : null;
}

/** The whole config back, with ONE radio's Icom fields replaced — everything else exactly as read (the POST
 *  replaces the whole file, as the setup page's does). Returns null when the serial is not in it. */
export function withIcomSettings(cfg: unknown, serial: string, s: IcomSettings): Record<string, unknown> | null {
  if (!cfg || typeof cfg !== 'object') return null;
  const c = cfg as Record<string, unknown>;
  const radios = Array.isArray(c.radios) ? (c.radios as Record<string, unknown>[]) : [];
  const i = radios.findIndex((r) => r && r.serial === serial);
  if (i < 0) return null;
  const next = radios.slice();
  next[i] = { ...radios[i], ...s };
  return { ...c, radios: next };
}

/** The preflight in plain lines for the setup card: ✓/✗, the item, its value and — when it fails — the exact
 *  radio menu path to change it. */
export function preflightLines(p: IcomPreflight | null): { ok: boolean; text: string }[] {
  if (!p) return [];
  return p.items.map((it) => ({
    ok: it.ok,
    text: `${it.name}: ${it.value || '?'}${it.ok ? '' : ` — change it at ${it.where || 'the radio’s menu'}`}`,
  }));
}

/* ── A PHONE SERVING ITS OWN ICOM (ServerModeScreen, Android Phase 4) ─────────────────────────────────────────── */

/** ★★ The keys Android's server reads from the start config (VibeIcom.prepareStart: optInt/optBoolean with these
 *  names and defaults). scripts/check-icom-servermode.mjs holds this list against the Kotlin. The phone's audio is
 *  AudioRecord on the radio's USB input and its port is the device the owner allowed, so icomAudio / icomPort /
 *  icomTtyOk are not settings there. */
export const ICOM_PHONE_KEYS = ['icomBaud', 'icomCiv', 'icomScopeSpeed', 'icomOwnerLock', 'icomRfOpen'] as const;
export type IcomPhoneFields = Pick<IcomSettings, (typeof ICOM_PHONE_KEYS)[number]>;

/** The five fields for the phone's start config, at definite values (so a crash-restore starts the radio exactly as
 *  the screen did), from whatever was stored — a missing or damaged store gives the setup page's defaults, owner
 *  lock ON. ★ The baud is one the radio's menu offers, or 115200. */
export function icomPhoneFields(stored: unknown): IcomPhoneFields {
  const s = icomSettingsFrom(stored);
  return {
    icomBaud: (ICOM_BAUDS as readonly number[]).includes(s.icomBaud) ? s.icomBaud : 115200,
    icomCiv: s.icomCiv,
    icomScopeSpeed: s.icomScopeSpeed,
    icomOwnerLock: s.icomOwnerLock,
    icomRfOpen: s.icomRfOpen,
  };
}

/** The connected radio as the phone reports it before anything is opened (getConnectedRadio) → what the server
 *  screen says about it. 'none' = not an Icom; 'maybe' = a USB serial chip Android will not name until the owner
 *  allows it (Android 10+); 'icom' = named by its own USB strings. */
export function icomPresence(radio: { driver?: string; icomConfirmed?: boolean } | null | undefined): 'none' | 'maybe' | 'icom' {
  if (!radio || radio.driver !== 'icom') return 'none';
  return radio.icomConfirmed === false ? 'maybe' : 'icom';
}

// ── BUDDY (the iPhone's watch remote) — what the phone tells the wrist about an Icom ─────────────────
/* ★★★ BUDDY ONLY MIRRORS THE PHONE, so it gets every Icom decision READY-MADE: the radio's mode names already
 *  labelled, FIL1/2/3 with this mode's factory widths, the span label, the banner words and the owner-lock
 *  verdict. It draws them and computes nothing — the rules stay here, ONE RULE, ONE MORE READER (the app, the
 *  web, Jr, and now the wrist through the phone). Sent by watchProvider.sendIcom; drawn by Buddy's ControlMenu
 *  and ContentView (ios/VibeSDRWatch). Its commands come back as `icomMode` / `icomFil` and go through the
 *  phone's own client (icomControl → icomControlMsg), so a value the radio did not list can never reach it. */
export interface IcomWatchState {
  model: string;
  /** The radio's own modes: id is what `icom_control` takes, label is how the radio's panel writes it. */
  modes: { id: string; label: string }[];
  mode: string;
  modeLabel: string;
  /** FIL1/2/3 with this mode's factory width ('' when the radio published none). */
  fils: { n: number; width: string }[];
  fil: number;
  /** The scope span as the radio labels it ("±25k"), '' unknown. */
  span: string;
  /** "The radio is transmitting — …" / "… not answering …", '' none. TX outranks link-down. */
  banner: string;
  tx: boolean;
  link: boolean;
  /** This listener may change mode/FIL/span (owner lock off, or a signed-in admin). */
  mayTune: boolean;
  /** What a listener under the owner lock is told, '' when they may tune. */
  follow: string;
}

/** The radio → Buddy's state; null = not an Icom (the wrist drops every Icom branch). */
export function icomWatchState(radio: IcomRadio | null, admin: boolean): IcomWatchState | null {
  if (!radio) return null;
  const widths = icomFilterWidths(radio, radio.modeNow);
  const filters = radio.filters.length ? radio.filters : [];
  return {
    model: radio.model,
    modes: radio.modes.map((id) => ({ id, label: icomModeLabel(id) })),
    mode: radio.modeNow,
    modeLabel: icomModeLabel(radio.modeNow),
    fils: filters.map((n) => ({ n, width: fmtWidth(widths[n - 1] ?? 0) })),
    fil: radio.filNow,
    span: fmtSpan(radio.scope.span),
    banner: icomBanner(radio),
    tx: radio.tx,
    link: radio.linkUp,
    mayTune: icomMayTune(radio, admin),
    // ★ The SAME words the app's banner and the client's 4 s refusal use — icomFollowNote, not a copy.
    follow: icomFollowNote(radio, admin),
  };
}
