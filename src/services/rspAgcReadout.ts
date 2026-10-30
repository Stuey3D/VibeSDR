/**
 * ★★★ THE SDRplay AGC READOUT — what the gain loop is doing, and whether it has got stuck (2026-10-30, V12).
 *
 * Stuart: "do we get to see a readout of what the AGC is doing for both the RF and IF so I can see if it is working
 * or got stuck". Two places, one copy of the rules (the app's SDRScreen and the web client's main.ts both use this):
 *   · the STATUS LINE: "RF 3 · IF 42" — the LNA state and the IF gain reduction in dB — SDRplay radios only;
 *   · the RADIO PANEL: one line — VibeAGC's state and why, the ADC peak against its target, gain writes in the last
 *     minute; or, with VibeAGC off, which AGC is running instead.
 *
 * ★★★ AND IT TURNS AMBER WHEN THE LOOP IS STUCK — with hysteresis, because the last warning built for this radio
 *     flickered on a healthy receiver and taught people to ignore it (memory: rsp_frozen_readout_warning_design —
 *     "a warning that comes and goes on a working receiver is worse than no warning"). So:
 *       · RAISED on: VibeAGC handed back to the old system (and why) · the IF sat at a rail for WARN_RAIL_S while
 *         the RF stage had room to take the load off it (the server only counts it then — see rspstat `vrail`) · a
 *         gain write rejected / not honoured / not sent in the last minute · no fresh ADC level for WARN_BLIND_S;
 *       · HELD for at least HOLD_MS once raised, and CLEARED only after the cause has been absent for CLEAR_MS —
 *         never following the condition sample by sample.
 *
 * Pure: no clock (the caller passes `now`), no React, no DOM — scripts/test_rsp_agc_readout.ts drives it.
 * ★ The server's fields are optional: a server too old to send them gets the plain "RF n · IF nn" and no warning.
 */

/** One rspstat, the fields this needs (the server's names). */
export interface RspAgcSample {
  lna: number; ifgr: number;
  /** IF AGC switched on (the panel's switch / hwinfo) — off = manual gain, nothing automatic to report. */
  ifAgc: boolean;
  /** Our RF loop (RF AGC) on. */
  rfAgc?: boolean;
  adcPeak?: number; agcSet?: number; settling?: boolean; agcInit?: boolean;
  vagc?: number; vst?: number; vrs?: number; vtgt?: number; vwpm?: number; vmiss?: number; vblind?: number; vrail?: number;
}

export interface RspAgcReadout {
  /** The status-line fragment: "RF 3 · IF 42", plus a short reason while amber. Empty on manual gain. */
  status: string;
  /** The radio panel's line. */
  line: string;
  /** Amber. */
  warn: boolean;
  /** The reason in words while amber, '' otherwise. */
  why: string;
}

export const WARN_RAIL_S = 10;     // the IF at a rail with RF to spare this long = stuck
export const WARN_BLIND_S = 3;     // VibeAGC with no fresh ADC window this long = blind
export const HOLD_MS = 30_000;     // once amber, at least this long
export const CLEAR_MS = 10_000;    // …and until the cause has been gone this long

/** VibeAGC's states (rspstat `vst`). */
export const VST_OFF = 0, VST_SETTLING = 1, VST_HOLDING = 2, VST_CUT = 3, VST_ADD = 4, VST_HANDED_BACK = 5;

/** rspstat `vrs` in words — vibe_rsp_vibeagc.h VagcAct (1-7) and the shim's trip codes (11-14). Append only. */
export function vagcReasonText(code: number | undefined): string {
  switch (code) {
    case 1: return 'the IF was at its 59 dB rail and the converter still hot';
    case 2: return 'the converter was clipping with the IF nearly spent';
    case 3: return 'the IF reduction sat over the front-end load line';
    case 4: return 'the IF had room for a whole rung';
    case 11: return 'the SDRplay API reported a device failure';
    case 12: return 'the RSP service stopped answering';
    case 13: return 'the driver rejected 3 gain writes';
    case 14: return '3 RF gain writes in a row were not honoured';
    default: return '';
  }
}

const dbfs = (v: number) => `${v < 0 ? '−' : ''}${Math.abs(Math.round(v))}`;

/** What is wrong right now, before any hysteresis: [status word, sentence] or null. Priority: hand-back, a missed
 *  write, blind, the rail. */
export function rspAgcCause(s: RspAgcSample): [string, string] | null {
  if (!s.ifAgc) return null;                                   // manual gain: nothing automatic to be stuck
  if (s.vst === VST_HANDED_BACK) {
    const r = vagcReasonText(s.vrs);
    return ['AGC HANDED BACK', `VibeAGC handed back to the SDRplay's own IF AGC${r ? ` — ${r}` : ''}`];
  }
  if ((s.vmiss ?? 0) > 0)
    return ['WRITE MISSED', `${s.vmiss} gain write${s.vmiss === 1 ? '' : 's'} did not land in the last minute`];
  if ((s.vagc ?? 0) === 1 && (s.vblind ?? 0) >= WARN_BLIND_S)
    return ['NO ADC LEVEL', `No fresh ADC level for ${s.vblind} s — the loop cannot see the signal`];
  const rail = s.vrail ?? 0;
  if (rail >= WARN_RAIL_S) return ['IF RAIL', `The IF has sat at its 59 dB rail for ${rail} s with RF gain still to cut`];
  if (-rail >= WARN_RAIL_S) return ['IF FLOOR', `The IF has sat at its floor for ${-rail} s with RF gain still to add`];
  return null;
}

/** The panel line without the warning. */
export function rspAgcLine(s: RspAgcSample): string {
  if (!s.ifAgc) return 'Manual gain — no AGC is running.';
  const peak = typeof s.adcPeak === 'number' && s.adcPeak > -98 ? dbfs(s.adcPeak) : '—';
  if ((s.vagc ?? 0) === 1) {
    const r = vagcReasonText(s.vrs);
    const st = s.vst === VST_SETTLING ? 'settling'
             : s.vst === VST_CUT ? `cutting RF${r ? ` (${r})` : ''}`
             : s.vst === VST_ADD ? `adding RF${r ? ` (${r})` : ''}`
             : s.vst === VST_HOLDING ? 'holding'
             : 'running';                                          // a beta44 server: on, no detail
    const tgt = typeof s.vtgt === 'number' ? ` · target ${dbfs(s.vtgt)}` : '';
    const w = typeof s.vwpm === 'number' ? ` · ${s.vwpm} gain write${s.vwpm === 1 ? '' : 's'} in the last minute` : '';
    return `VibeAGC: ${st} · ADC ${peak} dBFS${tgt}${w}`;
  }
  const who = `the SDRplay's own IF AGC${s.rfAgc !== false ? ' + our RF walker' : ''}`;
  const tgt = typeof s.agcSet === 'number' ? ` · target ${dbfs(s.agcSet)}` : '';
  // ★ After a hand-back the line names what runs NOW; the hand-back and its reason are the amber line under it (why).
  return `${who[0].toUpperCase()}${who.slice(1)} · ADC ${peak} dBFS${tgt}`;
}

/** ★ The status fragment without the warning: "RF 3 · IF 42". Empty on manual gain (no AGC has chosen anything). */
export function rspAgcStatus(s: RspAgcSample): string {
  return s.ifAgc ? `RF ${Math.round(s.lna)} · IF ${Math.round(s.ifgr)}` : '';
}

/**
 * ★★ The latch. Feed every rspstat (cheap — no rendering here); read `read()` when you want to draw.
 * Raised the moment a cause appears; held HOLD_MS at least; cleared once the cause has been absent CLEAR_MS. While
 * held, the reason shown is the newest cause seen (or the one that raised it, if the cause has gone).
 */
export class RspAgcWatch {
  private warnSince = -1;
  private quietSince = -1;
  private cause: [string, string] | null = null;
  private last: RspAgcSample | null = null;

  /** Forget everything (a new server, a different radio). */
  reset() { this.warnSince = -1; this.quietSince = -1; this.cause = null; this.last = null; }

  push(s: RspAgcSample, now: number): void {
    this.last = s;
    const c = rspAgcCause(s);
    if (!s.ifAgc) { this.warnSince = -1; this.quietSince = -1; this.cause = null; return; }  // manual: the owner's word
    if (c) {
      if (this.warnSince < 0) this.warnSince = now;
      this.cause = c; this.quietSince = -1;
      return;
    }
    if (this.warnSince < 0) return;
    if (this.quietSince < 0) this.quietSince = now;
    if (now - this.warnSince >= HOLD_MS && now - this.quietSince >= CLEAR_MS) {
      this.warnSince = -1; this.quietSince = -1; this.cause = null;
    }
  }

  get warn(): boolean { return this.warnSince >= 0; }

  read(): RspAgcReadout {
    const s = this.last;
    if (!s) return { status: '', line: '', warn: false, why: '' };
    const warn = this.warnSince >= 0 && !!this.cause;
    const base = rspAgcStatus(s);
    return {
      status: base && warn ? `${base} · ${this.cause![0]}` : base,
      line: rspAgcLine(s),
      warn,
      why: warn ? this.cause![1] : '',
    };
  }
}

/** ★ A rspstat message (either client's parse) → the sample. `ifAgc` comes from the caller: neither client takes it
 *  from rspstat (it rides hwinfo / the panel's own switch). */
export function rspAgcSampleOf(m: any, ifAgc: boolean): RspAgcSample {
  const n = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : undefined);
  return {
    lna: Number(m?.lna) || 0, ifgr: Number(m?.ifgr) || 0, ifAgc,
    rfAgc: m?.rfAgc === undefined ? undefined : (m.rfAgc === true || Number(m.rfAgc) === 1),
    adcPeak: n(m?.adcPeak), agcSet: n(m?.agcSet),
    vagc: n(m?.vagc === true ? 1 : m?.vagc === false ? 0 : m?.vagc), vst: n(m?.vst), vrs: n(m?.vrs), vtgt: n(m?.vtgt),
    vwpm: n(m?.vwpm), vmiss: n(m?.vmiss), vblind: n(m?.vblind), vrail: n(m?.vrail),
  };
}

/* ── ★★★ THE AGC CHOICE (the radio menu's AGC row, 2026-10-30) ─────────────────────────────────────────────────────
 * Stuart, on his unlocked Lenovo RSP1A with VibeAGC running: "i assume this isnt vibeagc" / "no option for it here
 * either". The menu said IF AGC AUTO (which reads as the SDRplay's own) and an AGC TARGET of −30 "not in use" while
 * VibeAGC held −20 — and the only switch was on the setup page. One set of words for both clients, here:
 *   · hwinfo radio.vagcMode — the owner's choice, rsp_control `vagcmode`: 0 AUTO · 1 VIBEAGC · 2 SDRPLAY;
 *   · hwinfo radio.vagcAuto — what AUTO resolves to with the driver the server loaded (VibeAGC with OpenRSP);
 *   · rspstat vagc — whether VibeAGC is RUNNING now (it may have handed back, or manual gain may be on).
 * ★ The AGC TARGET is NOT settable under VibeAGC: its targets are per profile (−20 dBFS peak; DAB's freeze band
 *   around it) and tuned on air, and the controller has no setter — so it is shown read-only, "set by VibeAGC". */
export const VAGC_AUTO = 0, VAGC_ON = 1, VAGC_SDRPLAY = 2;
export const VAGC_KEYS: { mode: number; label: string }[] = [
  { mode: VAGC_AUTO, label: 'AUTO' }, { mode: VAGC_ON, label: 'VIBEAGC' }, { mode: VAGC_SDRPLAY, label: 'SDRPLAY' },
];
/** Does this choice mean VibeAGC (with AUTO resolved for this server's driver)? */
export function vagcResolved(mode: number, auto: boolean): boolean {
  return mode === VAGC_ON || (mode === VAGC_AUTO && auto);
}
/** The owner's row, beside the keys: what AUTO means here. '' unless AUTO is chosen. */
export function vagcAutoNote(mode: number, auto: boolean): string {
  if (mode !== VAGC_AUTO) return '';
  return auto ? '→ VibeAGC (OpenRSP driver)' : "→ SDRplay's own (official API)";
}
/** A listener's read-only row: the choice in words. */
export function vagcChoiceText(mode: number, auto: boolean): string {
  const who = vagcResolved(mode, auto) ? 'VibeAGC' : "SDRplay's own";
  return mode === VAGC_AUTO ? `${who} (auto)` : who;
}
/** Who runs the IF stage while the IF AGC is on: VibeAGC if it is running, otherwise the SDRplay's own AGC. */
export function vagcIfOwner(running: boolean): string {
  return running ? 'VibeAGC' : 'SDRplay';
}
/** The AGC TARGET while VibeAGC runs: its real target, read-only. */
export function vagcTargetText(vtgt: number | undefined): string {
  return typeof vtgt === 'number' ? `${dbfs(vtgt)} dBFS · set by VibeAGC` : 'set by VibeAGC';
}
