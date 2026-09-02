/**
 * converter.ts — up-converters and down-converters in front of LOCAL hardware.
 *
 * ★★★ ONE AFFINE MAP, NOT TWO FEATURES. An up-converter is a down-converter with a negative LO,
 *   so both are the same transform between what the TUNER is set to and what the user is
 *   listening to:
 *
 *       display = k · hw + offset          k ∈ {+1, −1}
 *       hw      = k · (display − offset)   (k² = 1, so the inverse is the same shape)
 *
 *   Two stored fields — a SIGNED LO offset and an inversion flag — cover a Ham It Up, a QO-100
 *   LNB and a high-side transverter block alike. GQRX has shipped exactly this as a single signed
 *   "LNB LO" field for years, so the convention is already familiar to the people who ask for it.
 *
 * ★★★ THE USER NEVER ENTERS A FREQUENCY RANGE. This looked like the hard part and isn't: the
 *   converter's usable display range is DERIVED from the dongle's existing tuning range under the
 *   same transform (see displayRange). Band edges, VFO clamping, drum limits and pan walls then
 *   fall out of caps.freqRange for free.
 *
 * ★★★ WHERE IT IS ALLOWED TO APPLY, and this is a correctness gate rather than a preference:
 *
 *   ✓ Local USB hardware, rtl_tcp, SpyServer — the listener owns the dongle AND the converter, and
 *     nothing in either wire protocol has a notion of an LO offset, so the client is the only
 *     place the correction can happen. One client sees this radio; there is nobody to disagree with.
 *   ✗ VibeServer — an owner bolts a converter on once and never touches it again, so the SERVER
 *     corrects it and publishes true frequencies (see its radio setup page). Correcting again here
 *     would double it. Note this is NOT the same as "not local": a VibeServer session arrives with
 *     isLocal true because it reuses the local protocol path — see the note at SDRScreen's
 *     `isVibeServer`, and gate on that, never on bare isLocal.
 *   ✗ UberSDR / KiwiSDR / Web-888 / OpenWebRX / FM-DX — if the operator has a converter their
 *     server already reports corrected frequencies, for the same reason. Double-correcting.
 *
 *   The rule underneath all three: THE CORRECTION BELONGS WHEREVER THE CONVERTER IS KNOWN ABOUT
 *   ONCE. Put it in a client that shares a receiver with other listeners and only that client is
 *   right — every other visitor, the web client and the watch included, reads 125 MHz off, and two
 *   listeners on a shared dial tune each other into the weeds.
 */

export interface ConverterProfile {
  /** Preset id, or 'custom'. 'none' is the identity. */
  id:       string;
  /** What the row says when collapsed — always names the profile, so the toggle is never a
   *  mystery to someone who set it up weeks ago. */
  label:    string;
  /** SIGNED. Negative for an up-converter (a Ham It Up is −125 MHz), positive for a
   *  down-converter/LNB. The editor asks for a positive LO plus a direction and applies the sign
   *  here, because "enter −125000000" is a sign error waiting to happen. */
  offsetHz: number;
  /** High-side injection: the spectrum is genuinely flipped, not merely relabelled. */
  inverted: boolean;
  /** ★★ THE CONVERTER'S OWN LO ERROR, AND IT IS NOT PPM. PPM scales the HARDWARE frequency
   *  (crystal error); the converter's LO has an independent error that does not scale with the
   *  tuner at all. Left to correct LO drift with the PPM control, a user drags PPM to an absurd
   *  value that then mis-tunes everything else on the dongle. Its own field, always. */
  trimHz:   number;
  /** ★★★ WHAT THE CONVERTER ACTUALLY CONVERTS — its INPUT passband, in true RF Hz, and the reason
   *  the tunable range is not simply the dongle's range shifted.
   *
   *  An up-converter is not a bare mixer: it carries a low-pass filter on its input, so only HF
   *  ever reaches the mixer at all — which is what stops the whole of VHF folding in on top of the
   *  HF you are trying to hear. So while the converter is inline, HF is ALL the receiver can hear:
   *  the aerial is behind the converter, and the dongle's own VHF/UHF reception is gone until the
   *  lead is physically moved (which is what the engage toggle is for).
   *
   *  ★★★ WHICH IS WHY THIS CLAMPS RATHER THAN WARNS. Derived from the dongle alone, a 125 MHz
   *   up-converter on an RTL-SDR gives a display range of −101…1641 MHz: arithmetically right and
   *   practically useless, because above the passband there is nothing to hear and can be nothing.
   *   A warning there would be a notice the user can tune straight past. Intersecting the range
   *   instead means the VFO simply cannot leave what the converter serves, and band edges, the
   *   drum limits and the pan walls all inherit that for free — the same way they inherit the
   *   shifted range. It also disposes of the negative lower bound.
   *
   *  ★ NOT A PRODUCT SPEC WE HAVE VERIFIED. The presets default to the app's own HF ceiling
   *    (30 MHz), which is the definitional purpose of an up-converter rather than a claim about
   *    anyone's hardware; Custom overrides it. Someone who owns the box can tell us the real
   *    figure — see the README's rule about not claiming what isn't there. */
  inputLoHz: number;
  inputHiHz: number;
  /** ★★★ CONFIGURATION vs STATE, and keeping them apart is the whole design. The three fields
   *  above are set once per dongle; this is flipped whenever the cable moves. An up-converter is
   *  inline and users genuinely swap between HF-through-the-converter and direct VHF by moving a
   *  lead — while a dish-mounted LNB is never bypassed. Both have to work. */
  engaged:  boolean;
}

export const NO_CONVERTER: ConverterProfile = {
  id: 'none', label: 'None', offsetHz: 0, inverted: false, trimHz: 0,
  inputLoHz: 0, inputHiHz: 0, engaged: false,
};

/** The app's own HF ceiling, and the default a converter presents to the dongle. Not a claim
 *  about any particular converter — see inputHiHz. */
export const HF_CEILING_HZ = 30_000_000;

/** ★ A bypassed converter IS the identity transform. Resolved ONCE, here, so that no call site
 *  below ever has to remember to check `engaged` — the class of bug where one path honours the
 *  toggle and another quietly doesn't. */
export const active = (c: ConverterProfile | null | undefined): ConverterProfile =>
  c && c.engaged && c.id !== 'none' ? c : NO_CONVERTER;

export const isIdentity = (c: ConverterProfile): boolean =>
  c.offsetHz === 0 && c.trimHz === 0 && !c.inverted;

export function toDisplay(hwHz: number, c: ConverterProfile): number {
  return (c.inverted ? -hwHz : hwHz) + c.offsetHz + c.trimHz;
}

export function toHardware(dispHz: number, c: ConverterProfile): number {
  const d = dispHz - c.offsetHz - c.trimHz;
  return c.inverted ? -d : d;
}

/** The tunable range the user actually gets, in display Hz.
 *
 * ★★ INVERSION SWAPS THE ENDS. With k = −1 the bottom of the hardware range maps to the TOP of
 *  the display range, and a [hi, lo] pair handed to a clamp silently forbids everything.
 * ★★★ THEN INTERSECTED WITH WHAT THE CONVERTER PASSES — see inputLoHz/inputHiHz. Without this the
 *  VFO ranges over a hundreds-of-MHz span that the converter's input filter guarantees is silent.
 * ★ A converter declaring no passband (inputHiHz 0) is taken at its word and only shifted, so an
 *   older stored profile keeps working rather than being clamped to nothing. */
export function displayRange(hw: readonly [number, number], c: ConverterProfile): [number, number] {
  const a = toDisplay(hw[0], c), b = toDisplay(hw[1], c);
  let lo = a <= b ? a : b, hi = a <= b ? b : a;
  if (c.inputHiHz > c.inputLoHz) {
    const clampLo = Math.max(lo, c.inputLoHz);
    const clampHi = Math.min(hi, c.inputHiHz);
    /* ★★★ AN EMPTY INTERSECTION MUST NOT BE RETURNED AS A RANGE. A dongle that starts above the
     *   converter's output — say a 200 MHz-and-up receiver behind a 125 MHz up-converter, whose
     *   output stops at 155 MHz — has no overlap at all, and [75 MHz, 30 MHz] is a lo GREATER than
     *   its hi: handed to a clamp that forbids every frequency and freezes the VFO solid, with
     *   nothing on screen to say why. That pairing cannot work in the real world either, but the
     *   app's job is to stay usable and let the user see the mistake, not to seize up. So the
     *   passband is ignored when it would leave nothing, and the shifted range stands. */
    if (clampLo < clampHi) { lo = clampLo; hi = clampHi; }
  }
  return [lo, hi];
}

/** A preset the picker offers.
 *
 *  ★★★ KEYED ON LO FREQUENCY, NOT PRODUCT NAME. A converter needs no driver code — unlike the SDR
 *   sources, where "we support four" is a compatibility claim backed by work — so here Custom IS
 *   the feature and the presets only save typing. A list reading "Bullseye BE01, Nooelec
 *   SAWbird…" would imply those were tested, invite "why isn't mine in the list?", and sit badly
 *   against the README's promise that nothing is claimed that isn't there. LO labels claim
 *   nothing and cover strictly more hardware: an unknown converter works on day one and a model
 *   released tomorrow never needs an app update.
 *  ★★ AND THE LOs ARE NOT GUESSABLE FROM THE NAMES, which is the evidence for keying on them:
 *   Ham It Up is 125 MHz (100 before v1.2), SpyVerter is 120 MHz, and the RTL-SDR Blog V4's is
 *   28.8 MHz and internal. Three well-known products, three different LOs. Product names go on
 *   the secondary line, where they read as examples rather than as a supported-hardware list.
 */
export interface ConverterPreset {
  id: string; label: string; hint: string; offsetHz: number; inverted: boolean;
  inputLoHz: number; inputHiHz: number;
  /** ★★ THE SECOND LINE THE NOTE ABOVE ALREADY ASKS FOR. The LO stays the button's title — it is
   *  the ground truth, it is what is printed on the box, and it covers converters nobody has heard
   *  of — but an LO alone is unreadable to somebody who has just bought a thing with a NAME on it
   *  and no idea what 125 MHz means. Stuart: "so a newbie user can quickly identify their model."
   *  ★ Kept to a few words and deliberately NOT exhaustive: these read as EXAMPLES of what uses
   *    this LO, never as a supported-hardware list — same rule as the hint. Absent where there is
   *    no product to name (None, Manual). */
  model?: string;
}

export const CONVERTER_PRESETS: ConverterPreset[] = [
  { id: 'none',       label: 'None',        hint: 'No converter',
    offsetHz: 0, inverted: false, inputLoHz: 0, inputHiHz: 0 },

  // ── Up-converters: HF in, VHF out. The input range is what the low-pass filter passes. ──
  { id: 'up-100',     label: '100 MHz up',  hint: 'Ham It Up v1.1 and earlier · 100 kHz\u201330 MHz',
    model: 'Ham It Up v1.1',
    offsetHz: -100_000_000, inverted: false, inputLoHz: 100_000, inputHiHz: 30_000_000 },
  { id: 'up-120',     label: '120 MHz up',  hint: 'SpyVerter, SpyVerter R2 · 100 kHz\u201360 MHz',
    model: 'SpyVerter',
    offsetHz: -120_000_000, inverted: false, inputLoHz: 100_000, inputHiHz: 60_000_000 },
  { id: 'up-125',     label: '125 MHz up',  hint: 'Ham It Up v1.2+, Ham It Up Plus, most clones \u00b7 100 kHz\u201330 MHz',
    model: 'Ham It Up v1.2+',
    offsetHz: -125_000_000, inverted: false, inputLoHz: 100_000, inputHiHz: 30_000_000 },

  /* ── Down-converters: satellite in, UHF out. ──
   * ★★★ THESE DECLARE NO INPUT RANGE, AND THAT IS DELIBERATE — the advertised one would lock out
   *   the very thing most people buy them for. A "universal" Ku LNB is specified as 10.7-11.7 GHz
   *   on its low band, and QO-100's narrowband transponder sits at 10489.5 MHz, BELOW that: the
   *   published figure excludes the most popular use of the hardware. It works anyway, which is
   *   why QO-100 listeners use these LNBs. Clamping to the datasheet would have offered a dial
   *   that refuses the one frequency the user came for.
   * ★★ So the range is merely SHIFTED for these, and anyone who wants it narrowed sets it by hand
   *   in Custom, where they can enter what their own dish actually reaches.
   * ★ NO C-BAND (5150 MHz) AND NO OTHER HIGH-SIDE BLOCK, because those INVERT: the output is
   *   LO minus RF and the spectrum comes out mirrored. `inverted` exists in the model but nothing
   *   implements it, and shipping a preset that needs it would mis-tune and break every decoder.
   *   See the note below. */
  { id: 'dn-9750',    label: '9750 MHz down', hint: 'Ku LNB low band \u00b7 QO-100 narrowband',
    model: 'Ku LNB low',
    offsetHz: 9_750_000_000, inverted: false, inputLoHz: 0, inputHiHz: 0 },
  { id: 'dn-10600',   label: '10600 MHz down', hint: 'Ku LNB high band',
    model: 'Ku LNB high',
    offsetHz: 10_600_000_000, inverted: false, inputLoHz: 0, inputHiHz: 0 },
  { id: 'dn-10750',   label: '10750 MHz down', hint: 'Single-band Ku LNB',
    model: 'Single-band LNB',
    offsetHz: 10_750_000_000, inverted: false, inputLoHz: 0, inputHiHz: 0 },

  { id: 'custom',     label: 'Manual\u2026',  hint: 'Enter the LO and the range it converts',
    offsetHz: 0, inverted: false, inputLoHz: 0, inputHiHz: HF_CEILING_HZ },
];

/** The output band a converter produces from its input range — what the TUNER has to cover.
 *  Shown in the manual editor so the numbers can be checked against the hardware: enter a Ham It
 *  Up (100 kHz-30 MHz, LO 125) and it reads 125.1-155.0 MHz, which is what the dongle must reach.
 *  ★ Empty when no input range is declared, because then there is nothing to say. */
export function outputRange(c: ConverterProfile): [number, number] | null {
  if (c.inputHiHz <= c.inputLoHz) return null;
  const a = toHardware(c.inputLoHz, c), b = toHardware(c.inputHiHz, c);
  return a <= b ? [a, b] : [b, a];
}

/* ★★★ WHAT IS STILL NOT SHIPPED, AND WHY — INVERTING CONVERTERS.
 *   A high-side block (C-band's 5150 MHz LO, some transverters) produces LO minus RF, so the
 *   spectrum comes out MIRRORED. `inverted` exists in the model and the maths above honours it,
 *   but nothing conjugates the IQ stream, and that is the half that matters: flip only the display
 *   and USB/LSB swap, CW lands the wrong side of the carrier, FM de-rotation runs backwards so RDS
 *   never locks, and every decoder — RTTY, NAVTEX, WEFAX, SSTV, FT8 — fails. So no preset sets it,
 *   and the manual editor does not offer it. A control that mis-tunes is worse than a missing one.
 *
 * ★★ THE uint32 CONCERN THAT ONCE BLOCKED THE LNB PRESETS IS CLOSED. The native bridge is uint32
 *   throughout (vibe_localsdr_jni.cpp, RtlTcpServer::start, all of spyserver_protocol.h) and a
 *   10.489 GHz DISPLAY frequency would silently wrap if it ever reached one. It cannot: every
 *   frequency crossing to native is converted to HARDWARE Hz first — which is always under 2 GHz
 *   — and the one path that broke that rule (WatchSpectrumForwarder, which opens its own socket)
 *   was found and fixed. The remaining native call that carries a raw display frequency,
 *   startRecording, takes a Double on both platforms. ★ CHECK THIS AGAIN before adding any new
 *   native call that takes a frequency. */

/** ★★ THE V4 MUST NOT BE GIVEN AN OFFSET — the likeliest support ticket this feature generates.
 *  The RTL-SDR Blog V4 replaced the V3's direct-sampling circuit with an INTERNAL up-converter
 *  (28.8 MHz LO) behind its triplexer, resolved below the driver: the dongle already reports true
 *  frequencies across its whole range. An owner who reads "upconverter" in their own datasheet and
 *  enters 28.8 MHz here lands 28.8 MHz off with no idea why. Shown in the Custom editor. */
export const V4_WARNING =
  'If you have an RTL-SDR Blog V4, leave this set to None — its up-converter is internal and the '
  + 'dongle already reports true frequencies.';

export function presetById(id: string): ConverterPreset | undefined {
  return CONVERTER_PRESETS.find(p => p.id === id);
}

/** Build a stored profile from a preset choice, preserving trim and engaged state. */
export function fromPreset(p: ConverterPreset, prev?: ConverterProfile): ConverterProfile {
  return {
    id: p.id, label: p.label, offsetHz: p.offsetHz, inverted: p.inverted,
    inputLoHz: p.inputLoHz, inputHiHz: p.inputHiHz,
    trimHz: prev?.trimHz ?? 0,
    engaged: p.id === 'none' ? false : (prev?.engaged ?? true),
  };
}

/** What to show under the VFO while a converter is engaged — the single most useful thing when
 *  someone reports "it tunes to the wrong place". Empty when there is nothing to say. */
export function hardwareReadout(dispHz: number, c: ConverterProfile): string {
  const a = active(c);
  if (isIdentity(a)) return '';
  return `RF ${(toHardware(dispHz, a) / 1e6).toFixed(6).replace(/0+$/, '').replace(/\.$/, '')} MHz`;
}
