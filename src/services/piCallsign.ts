/**
 * RBDS PI code <-> callsign (North America), per NRSC-4-B.
 *
 * In North America the RBDS PI code is algorithmically derived from the
 * callsign, so the mapping runs both ways with no database and no network. PI is
 * in block 1 of EVERY RDS group, so it arrives almost immediately on tuning and
 * survives weak signal far better than PS text — decode PI -> callsign -> look up
 * the bundled station DB and you have the station's identity instantly, offline,
 * before RadioText even fills in.
 *
 * IMPORTANT (see addendum §6): a PI-derived callsign is a HINT, not truth.
 *  - Three-letter callsigns use a lookup table, NOT the formula (Annex D).
 *  - FM TRANSLATORS are not compatible with the formula at all (NRSC-G300) and
 *    dominate rural coverage — expect their PI decodes to be wrong/meaningless.
 *  - Some encoders transmit a default/incorrect PI, or none.
 *  - Canadian stations near the border sometimes use US codes with C for W.
 * Callers must prefer decoded PS text when present and cross-check the computed
 * callsign against the DB (full-power/LPFM vs translator) before trusting it.
 *
 * Verify against the reference calculators when changing this:
 *   https://caseymediallc.com/rds   and   https://db.wtfda.org/rds2.html
 * and against NRSC-4-B itself — not the prose summary above.
 */

// K/W block bases (decimal), NRSC-4-B.
const K_BASE = 4096; // 0x1000
const W_BASE = 21672; // 0x54A8
// Max offset a 4-letter suffix can encode: 25*676 + 25*26 + 25 = 17575.
const MAX_SUFFIX = 25 * 676 + 25 * 26 + 25; // 17575
// The "second nibble = 0" PI values are reassigned into the 0xA000 block by the
// standard; we detect but do not (yet) invert that remap — flagged low-confidence.
const A_BLOCK_LO = 0xa000;
const A_BLOCK_HI = 0xafff;

/**
 * Three-letter callsigns (e.g. KOB, WHO, WGN) are assigned fixed PI codes by a
 * lookup table in NRSC-4-B Annex D, NOT by the formula. This table is
 * intentionally empty: fill it from the authoritative Annex D during the DB
 * build (a handful of legacy stations). Until then, three-letter PIs simply
 * decode as "unknown" rather than guessing a wrong callsign.
 *
 * Keyed both ways: PI (decimal) -> callsign, and callsign -> PI.
 * TODO(db-build): populate from NRSC-4-B Annex D.
 */
export const THREE_LETTER_PI: ReadonlyArray<{ pi: number; callsign: string }> = [
  // { pi: 0x_____, callsign: 'KOB' }, ...
];
const PI_TO_THREE = new Map(THREE_LETTER_PI.map((e) => [e.pi, e.callsign]));
const THREE_TO_PI = new Map(THREE_LETTER_PI.map((e) => [e.callsign, e.pi]));

export interface PiDecode {
  /** Computed callsign (4- or 3-letter), or null if the PI can't be decoded. */
  callsign: string | null;
  /** How it was derived. */
  method: 'formula' | 'three-letter' | 'none';
  /**
   * True only for a clean formula/table decode. False when the PI is an encoder
   * default, out of the valid K/W range, or sits in the A-block remap we don't
   * invert — callers should treat these as unreliable and lean on PS text + the
   * DB cross-check.
   */
  confident: boolean;
  /** Human-readable reason when not confident. */
  note?: string;
}

const A = 'A'.charCodeAt(0);

function isValidCallsign(cs: string): boolean {
  return /^[KW][A-Z]{3}$/.test(cs);
}

/** Callsign -> PI (decimal). Returns null for inputs the formula can't encode. */
export function callsignToPi(callsignRaw: string): number | null {
  if (!callsignRaw) return null;
  // Normalise: uppercase, strip an -FM / -FM1 style suffix and any dashes.
  const cs = callsignRaw.toUpperCase().replace(/-.*$/, '').trim();

  if (cs.length === 3) {
    const pi = THREE_TO_PI.get(cs);
    return pi ?? null; // three-letter: table-only
  }
  if (!isValidCallsign(cs)) return null;

  const base = cs[0] === 'W' ? W_BASE : K_BASE;
  const v1 = cs.charCodeAt(1) - A;
  const v2 = cs.charCodeAt(2) - A;
  const v3 = cs.charCodeAt(3) - A;
  if (v1 < 0 || v1 > 25 || v2 < 0 || v2 > 25 || v3 < 0 || v3 > 25) return null;
  return base + v1 * 676 + v2 * 26 + v3;
}

/**
 * PI (decimal) -> callsign HINT. This is what the app calls on decoding block 1.
 * Always inspect `.confident` before showing the result as an identity.
 */
export function piToCallsign(pi: number): PiDecode {
  if (!Number.isFinite(pi) || pi <= 0) {
    return { callsign: null, method: 'none', confident: false, note: 'invalid/zero PI' };
  }
  const p = Math.floor(pi);

  // Three-letter table takes precedence (fixed assignments).
  const three = PI_TO_THREE.get(p);
  if (three) return { callsign: three, method: 'three-letter', confident: true };

  // Encoder defaults / reserved values that are never real US callsign PIs.
  if (p === 0xffff) {
    return { callsign: null, method: 'none', confident: false, note: 'default/unset PI (0xFFFF)' };
  }
  if (p >= A_BLOCK_LO && p <= A_BLOCK_HI) {
    // NRSC-4-B remaps "second nibble 0" callsign PIs into 0xA000; inverting that
    // is not implemented — surface as a hint at best, never authoritative.
    return { callsign: null, method: 'none', confident: false, note: 'A-block remapped PI (not inverted)' };
  }

  let letter: 'K' | 'W';
  let rem: number;
  if (p >= W_BASE) { letter = 'W'; rem = p - W_BASE; }
  else if (p >= K_BASE) { letter = 'K'; rem = p - K_BASE; }
  else {
    return { callsign: null, method: 'none', confident: false, note: 'below K block (not a US 4-letter PI)' };
  }

  if (rem > MAX_SUFFIX) {
    // Would overflow the three-letter suffix — likely a translator or a foreign
    // / default code, not a real US callsign.
    return { callsign: null, method: 'none', confident: false, note: 'suffix out of range (likely translator/foreign)' };
  }

  const v1 = Math.floor(rem / 676);
  const v2 = Math.floor((rem % 676) / 26);
  const v3 = rem % 26;
  const callsign = letter + String.fromCharCode(A + v1, A + v2, A + v3);
  return { callsign, method: 'formula', confident: true };
}

/** Convenience: uppercase 4-letter base (drop -FM etc.) used as the DB join key. */
export function callsignBase(callsign: string): string {
  return callsign.toUpperCase().replace(/-.*$/, '').trim();
}
