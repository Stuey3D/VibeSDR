/**
 * OWRX S-METER CALIBRATION — turning OpenWebRX's `smeter` number into something an S-meter can show.
 *
 * ★★★ THE WIRE VALUE IS NOT dBm. `smeter` is the channel's linear power relative to the server's own
 *  full scale (csdr squelch_and_smeter_cc); 10·log10 of it is dB below an arbitrary reference that moves
 *  with the dongle, its gain, the server's input scaling and the filter. Measured 2026-10-13 on 12
 *  public servers: noise floors from −35 to −74 — and we printed that as dBm against S9 = −73, so every
 *  OWRX server read S9 to S9+38 on NOISE. OWRX's own page never prints S-units for this reason (Marat:
 *  "I simply cannot guarantee that correct values are displayed").
 *
 * Two sources of a correction, best first:
 *   1. THE OWNER'S OWN CALIBRATION. The OpenWebRX+ `smeter` plugin (DL1HQH, 0xAF/openwebrxplus-plugins)
 *      is configured in the server's public `/static/plugins/receiver/init.js` as
 *      `window.smeter_config_global = { calibration_offset_hf, linearity_hf, …_vhf }`, applied as
 *      dBm = raw·linearity + offset, HF/VHF split at a 30 MHz centre frequency. 56 of 440 public servers
 *      load the plugin; ~30 carry real figures (HF typically −33…−55 dB, VHF −50…−82). Where the owner
 *      has measured, we show what their own page shows.
 *   2. OTHERWISE, ANCHOR THE NOISE AT S2 — the plugin's own "Auto S0" rule. The floor is learned from
 *      the quiet end of what arrives and placed at S2 (−115 dBm); a signal then reads its real height
 *      above the band's noise, which is all an S-reading can mean on an uncalibrated receiver.
 *
 * ✗ The plugin's IARU VHF scale (S9 = −93) is NOT followed: Stuart found it read too high on his own
 *   server and runs HF's S9 = −73 on both; our meter uses −73 everywhere (meters.ts S9_DB).
 *
 * Pure — no React Native — so scripts/test_owrxSmeterCal.ts runs it under plain node.
 */

/** Where an uncalibrated noise floor is placed: S2 on the HF scale (S9 −73, 6 dB per S-unit). */
export const OWRX_NOISE_ANCHOR_DBM = -73 - 7 * 6;

export interface BandCal { cal: number; lin: number }
export interface OwnerCal { hf: BandCal | null; vhf: BandCal | null }

/** Strip JS comments without touching `//` inside strings (URLs in Plugins.load lines). */
function stripComments(js: string): string {
  let out = '', i = 0, q = '';
  while (i < js.length) {
    const c = js[i], n = js[i + 1];
    if (q) {
      out += c;
      if (c === '\\') { out += n ?? ''; i += 2; continue; }
      if (c === q) q = '';
      i++; continue;
    }
    if (c === '"' || c === "'" || c === '`') { q = c; out += c; i++; continue; }
    if (c === '/' && n === '/') { while (i < js.length && js[i] !== '\n') i++; continue; }
    if (c === '/' && n === '*') { const e = js.indexOf('*/', i + 2); i = e < 0 ? js.length : e + 2; continue; }
    out += c; i++;
  }
  return out;
}

/**
 * The owner's calibration from their init.js, or null when there is none worth using.
 * ★ Read as DATA, never evaluated: only numeric fields are taken, and only within sane limits — this is
 *   text off somebody else's server. A band left at the plugin defaults (offset 0, linearity 1) is
 *   "not calibrated" (null), so it falls through to the S2 anchor instead of reading raw as dBm.
 */
export function parseOwnerCal(initJs: string): OwnerCal | null {
  const js = stripComments(initJs);
  const m = /smeter_config_global\s*=\s*\{/.exec(js);
  if (!m) return null;
  const start = m.index + m[0].length;
  const end = js.indexOf('}', start);
  if (end < 0) return null;
  const body = js.slice(start, end);
  const num = (k: string): number | undefined => {
    const r = new RegExp(`\\b${k}\\s*:\\s*(-?\\d+(?:\\.\\d+)?)`).exec(body);
    if (!r) return undefined;
    const v = parseFloat(r[1]);
    return Number.isFinite(v) ? v : undefined;
  };
  const band = (sfx: 'hf' | 'vhf'): BandCal | null => {
    const cal = num(`calibration_offset_${sfx}`) ?? 0;
    const lin = num(`linearity_${sfx}`) ?? 1;
    if (cal === 0 && lin === 1) return null;                      // defaults = never measured
    if (cal < -200 || cal > 100 || lin < 0.1 || lin > 10) return null;   // not a calibration
    return { cal, lin };
  };
  const hf = band('hf'), vhf = band('vhf');
  return hf || vhf ? { hf, vhf } : null;
}

/**
 * Learns the quiet end of a stream of raw readings: the 10th percentile of the last ~5 s.
 * ★ Falls at once (a quieter floor is the truth arriving); RISES SLOWLY (~20 min at OWRX's ~4–5 readings a second), so a carrier sitting
 *   in the channel cannot drag the floor up to itself, while a genuine change — the owner turning the
 *   gain up, the band waking at dawn — is still followed.
 */
export class FloorTracker {
  private win: number[] = [];
  private floor: number | null;
  private readonly winLen: number;
  constructor(initial: number | null = null, winLen = 50) { this.floor = initial; this.winLen = winLen; }

  /** Feed one raw reading (dB); returns the current floor estimate. */
  push(raw: number): number {
    this.win.push(raw);
    if (this.win.length > this.winLen) this.win.shift();
    const s = [...this.win].sort((a, b) => a - b);
    const p10 = s[Math.floor((s.length - 1) * 0.1)];
    if (this.floor === null || p10 < this.floor) this.floor = p10;
    else if (this.win.length >= this.winLen) this.floor += 0.0002 * (p10 - this.floor);
    return this.floor;
  }

  get value(): number | null { return this.floor; }
}

/** raw ↔ display for one band, as the S-meter and squelch both need it. */
export interface SmeterMap { toDisplay(raw: number): number; toRaw(disp: number): number }

export function ownerMap(c: BandCal): SmeterMap {
  return { toDisplay: (r) => r * c.lin + c.cal, toRaw: (d) => (d - c.cal) / c.lin };
}

export function anchorMap(floorRaw: number): SmeterMap {
  const off = OWRX_NOISE_ANCHOR_DBM - floorRaw;
  return { toDisplay: (r) => r + off, toRaw: (d) => d - off };
}
