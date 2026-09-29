/**
 * airband.ts — the VHF aeronautical COM band (118.000–136.990 MHz) behaving like an aviation radio,
 * and the step arithmetic that makes a 25/3 kHz step land on true channel frequencies.
 *
 * ONE MODULE, BOTH CLIENTS. The app (src/) and the web client (web/client/src/, which imports this
 * file directly) must agree to the hertz on what a channel is called and where a step lands — two
 * copies of this rule would be two answers to "what is 118.010?".
 *
 * ── THE 8.33 kHz STEP ─────────────────────────────────────────────────────────────────────────
 * 8.33 kHz is 25 kHz / 3 = 8333.333… Hz, which is not an integer. The step is therefore carried
 * everywhere as the integer SENTINEL `STEP_833` (8333) — prefs, the ladder, the watch link and the
 * labels all see an ordinary number — and every piece of arithmetic that moves a frequency by a
 * step goes through stepIndex()/stepHz() below, which use the EXACT 25000/3 and round only the
 * final answer. Channel n is always computed from its index (round(n × 25000 / 3)), never by adding
 * 8333 n times, so there is no drift: 1000 steps up from 118.000 is 126.33333 MHz, not 126.333.
 *
 * ── CHANNEL NAMES (ICAO Annex 10 Vol V §4.1.2.4, Table 4-1 (bis); EUROCONTROL / UK CAA) ────────
 * Every 25 kHz block starting at X.X00 / X.X25 / X.X50 / X.X75 holds three 8.33 kHz channels.
 * For the block whose 25 kHz channel is B (e.g. B = 118.000):
 *
 *      frequency (MHz)    25 kHz channel name    8.33 kHz channel name
 *      B + 0              B        (118.000)     B + .005   (118.005)
 *      B + 25/3 kHz       —                      B + .010   (118.010)   → 118.0083 MHz
 *      B + 50/3 kHz       —                      B + .015   (118.015)   → 118.0167 MHz
 *
 * so names ending .x20 / .x45 / .x70 / .x95 are never used, and the 25 kHz channel B and the
 * 8.33 kHz channel B+.005 are the SAME frequency with different names (and, on a real radio,
 * a different IF filter). Ofcom's worked example: "Channel 126.855 / 126.8500 MHz". The top of
 * the band is 136.975 (25 kHz) and 136.990 (8.33 kHz, 136.99167 MHz).
 *
 * ★★ WHICH NAME THE READOUT SHOWS ON A SHARED FREQUENCY. A receiver cannot tell a 25 kHz
 *    transmission on 118.000 from an 8.33 kHz one — it is the same carrier. So the name is a
 *    property of how the listener SELECTED the channel, exactly as on an 8.33-capable radio:
 *      1. the name they last chose at this frequency (typed "118.005", or stepped onto it), else
 *      2. the step mode: in 8.33 mode the 8.33 name (118.005), in any other mode the 25 kHz
 *         name (118.000) — a 25 kHz radio has no other name for it.
 *    The two frequencies in the middle of a block have only an 8.33 name, whatever the mode.
 *
 * ★★ STEPPING IN 8.33 MODE WALKS NAMES, NOT FREQUENCIES — as the inner knob of an 8.33 set does
 *    (Trig / Garmin / the UK CAA SafetySense leaflet: 118.000, 118.005, 118.010, 118.015, 118.025…).
 *    Four names per 25 kHz block, two of which share a frequency, so one step in four changes only
 *    the name (and, when the passband is still at a default, the filter width — see below).
 *
 * ── PASSBAND DEFAULTS (ETSI EN 300 676-1 V1.5.1, §8.5 "effective acceptance bandwidth") ────────
 *    25 kHz receivers:   ±8.5 kHz  (wide on purpose: it admits the offset carriers of climax
 *                                   multi-carrier stations)
 *    8.33 kHz receivers: ±2.8 kHz  (audio response 350 Hz–2.5 kHz, §8.3.3)
 *    These are only DEFAULTS. A passband is re-chosen only while it is still sitting at a default
 *    (the AM mode default or one of these two); a width the listener or a bookmark chose is never
 *    touched.
 */

/** The 8.33 kHz step's integer stand-in. See the header: arithmetic uses 25000/3 exactly. */
export const STEP_833 = 8333;
const EXACT_833 = 25000 / 3;

export const AIR_COM_LO = 118_000_000;
/** Exclusive. The last channels are 136.975 (25 kHz) and 136.990 → 136.99167 MHz (8.33 kHz). */
export const AIR_COM_HI = 137_000_000;

/** Passband defaults, ±Hz, per channel spacing — ETSI EN 300 676-1 §8.5.3. */
export const AIR_PB_25 = 8500;
export const AIR_PB_833 = 2800;
/** The AM mode default everywhere else (MODE_BANDWIDTHS.am, both clients). */
const AM_DEFAULT_HALF = 5000;

/** Half a hertz either side is rounding; this is how far off a channel still counts as ON it. */
const ON_CHANNEL_TOL_HZ = 2;

export type Spacing = 25 | 833;

/** The channel NAME the listener last selected, pinned to the frequency they selected it at. */
export interface AirDesig { hz: number; spacing: Spacing }

export interface AirChannel {
  /** e.g. "118.005" — what an aviation radio displays. */
  name: string;
  nameKhz: number;
  spacing: Spacing;
  /** The true carrier frequency, Hz (integer, rounded from the exact value). */
  hz: number;
  /** The true frequency the way licences print it: MHz to 4 decimals, "118.0083". */
  trueText: string;
}

export function isAirbandCom(hz: number): boolean {
  return hz >= AIR_COM_LO - ON_CHANNEL_TOL_HZ && hz < AIR_COM_HI;
}

/** Aviation voice is AM. Channel names and passband defaults only apply to the AM family. */
export function isAirVoiceMode(mode: string | null | undefined): boolean {
  const m = String(mode ?? '').toLowerCase();
  return m === 'am' || m === 'sam';
}

// ── Step arithmetic ───────────────────────────────────────────────────────────────────────────

export function stepExactHz(step: number): number {
  return step === STEP_833 ? EXACT_833 : step;
}

/** Index of `hz` on the step's grid. A frequency within ON_CHANNEL_TOL_HZ of a grid point IS that
 *  point, whatever `how` says — 118008333 must floor to 118.00833's own index, not the one below
 *  (it sits a third of a hertz under the exact value). */
export function stepIndex(hz: number, step: number, how: 'round' | 'floor' | 'ceil' = 'round'): number {
  const s = stepExactHz(step);
  const near = Math.round(hz / s);
  if (Math.abs(near * s - hz) <= ON_CHANNEL_TOL_HZ) return near;
  const x = hz / s;
  return how === 'floor' ? Math.floor(x) : how === 'ceil' ? Math.ceil(x) : near;
}

/** Grid point `idx` in integer Hz, computed from the index — never accumulated. */
export function stepHz(idx: number, step: number): number {
  return Math.round(idx * stepExactHz(step));
}

export function snapToStep(hz: number, step: number): number {
  return step > 0 ? stepHz(stepIndex(hz, step, 'round'), step) : hz;
}

/** `n` steps from `hz`. `dir`: the first step up goes to the next grid point ABOVE (floor + 1),
 *  the first step down to the next BELOW (ceil − 1) — the tuner keys' rule. `round`: snap to the
 *  nearest grid point, then move — the drum's rule. */
export function stepFrom(hz: number, step: number, n: number, snap: 'dir' | 'round' = 'dir'): number {
  if (!(step > 0) || !n) return snap === 'round' ? snapToStep(hz, step) : hz;
  const base = snap === 'round' ? stepIndex(hz, step, 'round')
    : n > 0 ? stepIndex(hz, step, 'floor') : stepIndex(hz, step, 'ceil');
  return stepHz(base + n, step);
}

/** Human label for a step, shared by every step button and menu: the sentinel reads "8.33". */
export function stepLabelKhz(step: number): string | null {
  return step === STEP_833 ? '8.33' : null;
}

// ── Channel names ─────────────────────────────────────────────────────────────────────────────

const fmtName = (khz: number) => (khz / 1000).toFixed(3);
const fmtTrue = (hz: number) => (hz / 1e6).toFixed(4);

/** The 8.33 grid position of `hz` inside the COM band, or null when it is not on a channel. */
function gridPos(hz: number): { block: number; sub: number; hz: number } | null {
  if (!isAirbandCom(hz)) return null;
  const idx = Math.round((hz - AIR_COM_LO) / EXACT_833);
  const exact = AIR_COM_LO + idx * EXACT_833;
  if (Math.abs(exact - hz) > ON_CHANNEL_TOL_HZ || idx < 0) return null;
  const block = Math.floor(idx / 3);
  const trueHz = Math.round(exact);
  if (trueHz >= AIR_COM_HI) return null;
  return { block, sub: idx - 3 * block, hz: trueHz };
}

function makeChannel(block: number, sub: number, spacing: Spacing, hz: number): AirChannel {
  const baseKhz = 118_000 + 25 * block;
  const nameKhz = spacing === 25 ? baseKhz : baseKhz + 5 * (sub + 1);
  return { name: fmtName(nameKhz), nameKhz, spacing, hz, trueText: fmtTrue(hz) };
}

/** The channel at `hz`, named by the rule in the header. `desig` is the name last selected (only
 *  honoured when it is pinned to this same frequency); `step` decides otherwise. */
export function channelAt(hz: number, step: number, desig?: AirDesig | null): AirChannel | null {
  const g = gridPos(hz);
  if (!g) return null;
  let spacing: Spacing = 833;
  if (g.sub === 0) {
    spacing = desig && Math.abs(desig.hz - hz) <= ON_CHANNEL_TOL_HZ ? desig.spacing
      : step === STEP_833 ? 833 : 25;
  }
  return makeChannel(g.block, g.sub, spacing, g.hz);
}

/** What the readout should show: the channel, only in the COM band, only in AM. */
export function airbandChannel(hz: number, mode: string | null | undefined, step: number,
                               desig?: AirDesig | null): AirChannel | null {
  if (!isAirVoiceMode(mode)) return null;
  return channelAt(hz, step, desig);
}

export type ChannelLookup =
  | { ok: true; hz: number; spacing: Spacing; name: string }
  | { ok: false; name: string; message: string; lower: string; upper: string };

/** A channel NAME, in kHz (118005), to its frequency. null when the number is not in the COM band
 *  or not on the 5 kHz grid that names live on — i.e. it is not a name at all. */
export function channelNameToHz(nameKhz: number): ChannelLookup | null {
  if (!Number.isInteger(nameKhz) || nameKhz < 118_000 || nameKhz > 136_995 || nameKhz % 5 !== 0) return null;
  const r = nameKhz % 25;
  const baseKhz = nameKhz - r;
  const name = fmtName(nameKhz);
  if (r === 20) {
    // 136.995 has no channel above it — the band ends at 136.990.
    const lower = fmtName(nameKhz - 5), upper = nameKhz + 5 <= 136_990 ? fmtName(nameKhz + 5) : '';
    return { ok: false, name, lower, upper,
      message: `${name} is not an aviation channel — 8.33 kHz channel names never end in .x20, .x45, .x70 or .x95. `
             + (upper ? `Nearest: ${lower} or ${upper}.` : `Nearest: ${lower}.`) };
  }
  const base = baseKhz * 1000;
  if (r === 0) return { ok: true, hz: base, spacing: 25, name };
  const sub = r / 5 - 1;                       // .x05 → 0, .x10 → 1, .x15 → 2
  return { ok: true, hz: Math.round(base + sub * EXACT_833), spacing: 833, name };
}

/** ★ PILOT-STYLE ENTRY. A typed value in the COM band that lands exactly on a 5 kHz boundary is a
 *  channel NAME, as it is on an aviation radio: 118.010 means the channel that tunes 118.0083 MHz,
 *  121.5 means 121.500. Anything finer (118.0083, 128.5928) is a plain frequency and returns null
 *  — so a VOLMET off the raster still tunes exactly where it was typed. Names that do not exist
 *  (.x20/.x45/.x70/.x95) are REFUSED with the nearest two, not snapped: guessing which neighbour
 *  was meant would be tuning somewhere the listener did not ask for. */
export function airbandEntry(typedHz: number): ChannelLookup | null {
  const hz = Math.round(typedHz);
  if (hz < AIR_COM_LO || hz >= AIR_COM_HI || hz % 5000 !== 0) return null;
  return channelNameToHz(hz / 1000);
}

/** One knob click in 8.33 mode: the next NAME in the sequence .x00 .x05 .x10 .x15 | .x25 … */
function walkName(nameKhz: number, dir: 1 | -1): number {
  const r = nameKhz % 25;
  if (dir > 0) return r === 15 ? nameKhz + 10 : nameKhz + 5;     // .x15 → next block's .x00
  return r === 0 ? nameKhz - 10 : nameKhz - 5;                    // .x00 → previous block's .x15
}

/**
 * `n` steps from `hz`, the way the dial should move there. Outside the COM band, or in any mode
 * other than AM, or on any step other than 8.33, this is exactly stepFrom(). In the COM band in AM
 * with the 8.33 step, it walks channel NAMES (see the header) — and returns the name it landed on,
 * so the caller can pin it (`desig`). A frequency that is not on a channel first moves onto the
 * grid like any other step; only then does the walk begin.
 */
export function airbandStepFrom(hz: number, step: number, n: number, snap: 'dir' | 'round',
                                mode: string | null | undefined, desig?: AirDesig | null,
): { hz: number; desig: AirDesig | null } {
  const plain = () => {
    const to = stepFrom(hz, step, n, snap);
    const ch = step === STEP_833 && isAirVoiceMode(mode) ? channelAt(to, step, null) : null;
    return { hz: to, desig: ch ? { hz: ch.hz, spacing: ch.spacing } : null };
  };
  if (step !== STEP_833 || !isAirVoiceMode(mode) || !n) return plain();
  const cur = channelAt(hz, step, desig);
  if (!cur) return plain();
  let k = cur.nameKhz;
  const dir: 1 | -1 = n > 0 ? 1 : -1;
  for (let i = 0; i < Math.abs(n); i++) {
    const nx = walkName(k, dir);
    if (nx < 118_000 || nx > 136_990) break;       // the ends of the band: stay on the last channel
    k = nx;
  }
  const to = channelNameToHz(k);
  if (!to || !to.ok) return plain();              // cannot happen for a walked name; never strand the dial
  return { hz: to.hz, desig: { hz: to.hz, spacing: to.spacing } };
}

/**
 * The passband to switch to after a tune, or null to leave it alone.
 *  - On a COM channel in AM, with the passband still at a DEFAULT (±5 kHz AM, ±2.8 or ±8.5): the
 *    default for the channel's spacing.
 *  - In the COM band but between channels (a VOLMET on a 100 Hz boundary, a fine step): unchanged —
 *    the filter must not flap while somebody walks past the raster.
 *  - Outside the COM band in AM, if the passband is still one of the two AIRBAND defaults: the
 *    ordinary AM default back, so leaving the airband changes nothing outside it.
 *  - A passband anybody chose (a bookmark's ±3 kHz, a dragged edge) is never touched.
 */
export function airbandPassband(hz: number, mode: string | null | undefined, ch: AirChannel | null,
                                low: number, high: number): [number, number] | null {
  if (!isAirVoiceMode(mode)) return null;
  const sym = Math.round(-low) === Math.round(high) ? Math.round(high) : -1;
  const isAirDefault = sym === AIR_PB_25 || sym === AIR_PB_833;
  if (ch) {
    if (!(isAirDefault || sym === AM_DEFAULT_HALF)) return null;
    const want = ch.spacing === 833 ? AIR_PB_833 : AIR_PB_25;
    return want === sym ? null : [-want, want];
  }
  if (isAirbandCom(hz)) return null;
  return isAirDefault ? [-AM_DEFAULT_HALF, AM_DEFAULT_HALF] : null;
}
