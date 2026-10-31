/**
 * ★★★ V12 SYNCHRONOUS AM — ONE RULE, TWO READERS (the app and the VibeServer web client import this file).
 *
 * The VibeServer demodulators that start from a carrier PLL (vibedsp SyncAm, docs/v12/AM-SYNC.md):
 *   sam    SAM      — synchronous AM, both sidebands (weighted toward the cleaner one when they differ)
 *   sau    SAM-U    — synchronous AM, upper sideband only
 *   sal    SAM-L    — synchronous AM, lower sideband only
 *   ecssu  ECSS-U   — exalted-carrier selectable sideband, upper: the same carrier lock with an SSB-shaped,
 *                     one-sided passband (a DXer's way of zero-beating a station and taking one side)
 *   ecssl  ECSS-L   — the same, lower
 *   amst   AM-ST    — AM stereo, C-QUAM compatible. ★ "C-QUAM" is Motorola's trademark: always "C-QUAM
 *                     compatible", never "C-QUAM".
 *
 * ★★ OFFERED ONLY WHERE THE SERVER LISTS THEM (vibeserver.json `modes`). An older VibeServer accepted "sam" and played
 *    it as plain AM — a key that changes nothing — so it must never be drawn there; nor on any other backend (Kiwi,
 *    OpenWebRX and UberSDR offer only their own modes); nor on an Icom (the radio demodulates).
 */

export const AM_SYNC_IDS = ['sam', 'sau', 'sal', 'ecssu', 'ecssl', 'amst'] as const;
export type AmSyncId = typeof AM_SYNC_IDS[number];

/** Key legends (short, as the other demodulators'). */
export const AM_SYNC_LABEL: Record<AmSyncId, string> = {
  sam: 'SAM', sau: 'SAM-U', sal: 'SAM-L', ecssu: 'ECSS-U', ecssl: 'ECSS-L', amst: 'AM-ST',
};

/** Spoken names (VoiceOver / TalkBack) and the long form for tooltips and menus. */
export const AM_SYNC_NAME: Record<AmSyncId, string> = {
  sam:   'Synchronous AM, both sidebands',
  sau:   'Synchronous AM, upper sideband',
  sal:   'Synchronous AM, lower sideband',
  ecssu: 'ECSS, upper sideband — carrier-locked single sideband',
  ecssl: 'ECSS, lower sideband — carrier-locked single sideband',
  amst:  'AM Stereo (C-QUAM compatible)',
};

/** One-line explanations for a tooltip / hint. */
export const AM_SYNC_HINT: Record<AmSyncId, string> = {
  sam:   'Locks to the station\'s carrier and demodulates both sidebands coherently — no distortion when selective fading takes the carrier down.',
  sau:   'Synchronous AM on the upper sideband only — dodges a neighbour splattering on the lower side.',
  sal:   'Synchronous AM on the lower sideband only — dodges a neighbour splattering on the upper side.',
  ecssu: 'Carrier-locked upper sideband with an SSB-style filter: set the width as you would for USB.',
  ecssl: 'Carrier-locked lower sideband with an SSB-style filter: set the width as you would for LSB.',
  amst:  'AM stereo, compatible with C-QUAM broadcasts: stereo while the 25 Hz pilot is received, mono otherwise.',
};

export function isAmSync(mode: string | null | undefined): mode is AmSyncId {
  return !!mode && (AM_SYNC_IDS as readonly string[]).includes(mode);
}

/** The whole AM family (plain AM included) — where AM AUTO BW acts. */
export function isAmFamily(mode: string | null | undefined): boolean {
  return mode === 'am' || isAmSync(mode);
}

/** The synchronous AM modes THIS server offers, in key order. */
export function amSyncOffered(modes: readonly string[] | undefined | null): AmSyncId[] {
  if (!modes) return [];
  return AM_SYNC_IDS.filter((m) => modes.includes(m));
}

/** A demodulator's key legend — the synchronous AM family's own, everything else upper-cased as before. */
export function modeLegend(mode: string): string {
  return isAmSync(mode) ? AM_SYNC_LABEL[mode] : mode.toUpperCase();
}

/** Default passbands (Hz, relative to the dial): AM-shaped ±5 kHz; ECSS one-sided like SSB, 4.5 kHz. */
export const AM_SYNC_PASSBAND: Record<AmSyncId, [number, number]> = {
  sam: [-5000, 5000], sau: [-5000, 5000], sal: [-5000, 5000],
  ecssu: [50, 4500], ecssl: [-4500, -50], amst: [-5000, 5000],
};

/** The widest the server builds (mirrors RxPipeline::maxBwHz): AM-shaped 48 kHz, ECSS as SSB 12 kHz. */
export const AM_SYNC_MAX_BW: Record<AmSyncId, number> = {
  sam: 48_000, sau: 48_000, sal: 48_000, ecssu: 12_000, ecssl: 12_000, amst: 48_000,
};

/** The server's `amsync` state (sendAmSync in the shim). Unknown fields are ignored; a malformed message is null. */
export interface AmSyncState {
  lock: boolean;      // carrier PLL locked
  offHz: number;      // carrier offset from the dial (Hz)
  q: number;          // lock quality 0..1
  loHz: number;       // applied audio edge, lower sideband (0 = not reproduced)
  hiHz: number;       // applied audio edge, upper sideband
  auto: boolean;      // AUTO BW on
  cut: boolean;       // auto bandwidth is narrower than the listener's width on some side
  pilot: boolean;     // AM stereo pilot
  notchHz: number;    // heterodyne notch (audio Hz; negative = lower sideband), 0 = none
}
export function parseAmSync(m: unknown): AmSyncState | null {
  const o = m as Record<string, unknown> | null;
  if (!o || o.type !== 'amsync') return null;
  const num = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : 0);
  return {
    lock: o.lock === true, offHz: num(o.off), q: Math.max(0, Math.min(1, num(o.q))),
    loHz: Math.max(0, num(o.lo)), hiHz: Math.max(0, num(o.hi)),
    auto: o.auto === true, cut: o.cut === true, pilot: o.pilot === true, notchHz: num(o.notch),
  };
}

/** The readout line: "SYNC +3.2 Hz" / "SYNC …" while acquiring, then the applied width where AUTO BW has cut. */
export function amSyncReadout(s: AmSyncState | null, mode: string): string {
  if (!s || !isAmFamily(mode)) return '';
  const parts: string[] = [];
  if (isAmSync(mode)) {
    parts.push(s.lock ? `SYNC ${s.offHz >= 0 ? '+' : '−'}${Math.abs(s.offHz).toFixed(1)} Hz` : 'SYNC …');
  }
  if (s.cut) {
    const k = (hz: number) => (hz / 1000).toFixed(1);
    if (s.loHz > 0 && s.hiHz > 0) parts.push(s.loHz === s.hiHz ? `AUTO BW ±${k(s.hiHz)}k` : `AUTO BW L ${k(s.loHz)}k · U ${k(s.hiHz)}k`);
    else if (s.hiHz > 0) parts.push(`AUTO BW ${k(s.hiHz)}k`);
    else if (s.loHz > 0) parts.push(`AUTO BW ${k(s.loHz)}k`);
  }
  if (s.notchHz) parts.push(`NOTCH ${Math.abs(s.notchHz).toFixed(0)} Hz`);
  return parts.join(' · ');
}
