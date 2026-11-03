/**
 * fmDefaults.ts — the app's side of the owner's BROADCAST FM DEFAULTS (the engine's rules are
 * android/app/src/main/cpp/vibe_fm_defaults.h; the browser's Setup page has the same card).
 *
 * ★ The defaults here MUST match vibefm::Defaults: de-emphasis by region, NR off, ST/IMS/CEQ/AUTO BW/NB on,
 *   RETURN. They are what the engine runs with when a config says nothing, so the card shows the truth.
 * ★ On the wire to the engine they are the config's flat fm* keys — the one spelling every reader uses.
 */
export interface FmDefaults {
  /** -1 = by ITU region (75 µs in Region 2, 50 µs elsewhere), else 0 / 50 / 75. */
  deemphUs: -1 | 0 | 50 | 75;
  stereo: boolean; nr: boolean; ims: boolean; ceq: boolean; autobw: boolean; nb: boolean;
  /** false = RETURN to these for each new listener; true = REMEMBER the last listener's settings. */
  remember: boolean;
}

export const FM_DEFAULTS: FmDefaults = {
  deemphUs: -1, stereo: true, nr: false, ims: true, ceq: true, autobw: true, nb: true, remember: false,
};

/** A stored value back into FmDefaults — anything missing or malformed takes the default, never a guess. */
export function readFmDefaults(raw: string | null | undefined): FmDefaults {
  let o: Record<string, unknown> = {};
  try { const j = JSON.parse(raw || '{}'); if (j && typeof j === 'object') o = j as Record<string, unknown>; } catch { /* the defaults */ }
  const b = (k: keyof FmDefaults) => (typeof o[k] === 'boolean' ? (o[k] as boolean) : (FM_DEFAULTS[k] as boolean));
  const d = Number(o.deemphUs);
  return {
    deemphUs: d === 0 || d === 50 || d === 75 ? d : -1,
    stereo: b('stereo'), nr: b('nr'), ims: b('ims'), ceq: b('ceq'), autobw: b('autobw'), nb: b('nb'), remember: b('remember'),
  };
}

/** The engine config's keys (vibefm::fromJson reads exactly these). */
export function fmDefaultsKeys(d: FmDefaults) {
  return {
    fmDeemph: d.deemphUs, fmStereo: d.stereo, fmNr: d.nr, fmIms: d.ims, fmCeq: d.ceq,
    fmAutoBw: d.autobw, fmNb: d.nb, fmRemember: d.remember,
  };
}

/** ★ Exactly when "return" happens, in the words the browser's Setup page uses (fmDefHints). */
export function fmPolicyWords(remember: boolean, lockedMany: boolean, many: boolean): string {
  if (remember) return lockedMany
    ? 'Each new listener’s channel starts from the last settings anyone chose. After the server restarts, from these defaults.'
    : 'The radio keeps whatever the last listener left, the way a shared tuner does. After the server restarts, these defaults.';
  if (lockedMany) return 'Every new listener’s channel starts with these. Nobody else’s channel is ever changed. Reloading keeps your own settings.';
  if (many) return 'When someone connects and nobody else is listening, the radio goes back to these. Someone joining while others listen changes nothing. Reloading keeps your own settings.';
  return 'Each new listener starts with these. Reloading keeps your own settings.';
}
