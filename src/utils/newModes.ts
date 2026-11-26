/* ★★★ beta96 NEW MODES — the app's copy of the server's rule (vibe_decoder_reach.h newModesState; the Setup page's
 * newModesState in vibe_setup_page.h). docs/v12/NEW-MODES-OFF.md.
 *
 * Stuart 11-26: Paul set his radio up ADS-B only; later betas added modes his BLOCKED list could not name, they came in
 * ON, and listeners could leave ADS-B again. Each switch has the generation it first appeared in (the table comes from
 * this phone's own engine: VibeLocalSDR.getRegionDefaults → modeGen / modeGens). The owner's list carries "seen@N" and
 * "newmodes@on" / "newmodes@off". A switch newer than seen is NEW, and OFF until the owner ticks it ("+id") when the
 * radio is single-mode by their ticks (always), or they said "off", or (automatic) they had blocked anything at all.
 * No table (an older engine) = nothing is new. Pure functions only — scripts/test_new_modes_clients.ts drives them. */

export type ModeGens = { now: number; gens: Record<string, number>; unstamped?: number };
export const NO_MODE_GENS: ModeGens = { now: 0, gens: {} };

/** The options' generations — evidence of when a list was written, never a NEW switch. Same as the server's table. */
const OPTION_GENS: Record<string, number> = {
  'lora+all': 63, 'tones+digits': 80, 'avdata+text': 80, 'dvsig+ids': 87, 'ale+amd': 87,
  'ft8+deep': 91, 'ft8-deep': 91, 'avdata+8m': 93,
};
export const LISTEN_DEMODS = ['wfm', 'nfm', 'am', 'sam', 'sau', 'sal', 'ecssu', 'ecssl', 'amst', 'usb', 'lsb', 'cwu', 'cwl',
  'drm', 'freedv', 'm17', 'dvsig'];
export const WHOLE_MODES = ['dab', 'adsb', 'ism', 'ndb', 'ais', 'avdata', 'sondes', 'vormap'];

/** The engine's `{modeGen, modeGens}` (getRegionDefaults JSON), cleaned; anything odd = no table. */
export function parseModeGens(j: any): ModeGens {
  if (!j || typeof j.modeGen !== 'number' || !j.modeGens || typeof j.modeGens !== 'object') return NO_MODE_GENS;
  const gens: Record<string, number> = {};
  for (const [k, v] of Object.entries(j.modeGens)) {
    const id = String(k).toLowerCase().replace(/[^0-9a-z-]/g, '').slice(0, 16);
    if (id && typeof v === 'number' && v >= 0) gens[id] = Math.floor(v);
  }
  const unstamped = typeof j.modeGenUnstamped === 'number' ? Math.max(0, Math.floor(j.modeGenUnstamped)) : undefined;
  return { now: Math.max(0, Math.floor(j.modeGen)), gens, ...(unstamped === undefined ? {} : { unstamped }) };
}

export const ownSet = (csv: string) => new Set(String(csv || '').split(/[,;\s]+/).filter(Boolean).map((t) => t.toLowerCase()));
export const isNewModesWord = (t: string) => t.startsWith('seen@') || t.startsWith('newmodes@');

export type NewModesState = { seen: number; stamped: boolean; restricted: boolean; single: boolean; policy: number; off: boolean };

export function newModesState(own: Set<string>, mg: ModeGens): NewModesState {
  let seen = mg.now, stamped = false, restricted = false, policy = 0, evidence = 0;
  for (const t of own) {
    if (t.startsWith('seen@')) { seen = Math.max(0, Math.min(mg.now, parseInt(t.slice(5), 10) || 0)); stamped = true; }
    else if (t === 'newmodes@on') policy = 1;
    else if (t === 'newmodes@off') policy = -1;
    else {
      const id = t[0] === '+' ? t.slice(1) : t;
      const g = id in mg.gens ? mg.gens[id] : id in OPTION_GENS ? OPTION_GENS[id] : -1;
      if (g < 0) continue;
      evidence = Math.max(evidence, g);
      if (t[0] !== '+' && id in mg.gens) restricted = true;
    }
  }
  const singleAt = (g: number) => {
    const seenOff = (id: string) => !(id in mg.gens) || mg.gens[id] > g || own.has(id);
    return LISTEN_DEMODS.every(seenOff) && WHOLE_MODES.filter((w) => !seenOff(w)).length <= 1;
  };
  // ★★ no "seen@": a single-mode list is dated by its newest token; any other restricted list counts as having seen
  //    mg.unstamped (the server's kUnstampedRestrictedSeen) — "dab" alone must not switch off everything since 11.x
  if (!stamped) seen = !restricted ? mg.now : singleAt(evidence) ? evidence : Math.max(evidence, Math.min(mg.now, mg.unstamped ?? mg.now - 1));
  const single = singleAt(seen);
  return { seen, stamped, restricted, single, policy, off: single || policy < 0 || (policy === 0 && restricted) };
}
export const modeIsNew = (id: string, own: Set<string>, mg: ModeGens) => id in mg.gens && mg.gens[id] > newModesState(own, mg).seen;
export function newModeOff(id: string, own: Set<string>, mg: ModeGens): boolean {
  if (!modeIsNew(id, own, mg) || own.has(id) || own.has('+' + id)) return false;
  return newModesState(own, mg).off;
}
export const newModeIds = (own: Set<string>, mg: ModeGens) => {
  const s = newModesState(own, mg);
  return Object.keys(mg.gens).filter((id) => mg.gens[id] > s.seen);
};
/** The list as the engine ENFORCES it: the owner's, plus region defaults and new-mode defaults, minus their words. */
export function enforcedOff(own: Set<string>, regionDefaults: Record<string, string>, mg: ModeGens): Set<string> {
  const off = new Set([...own].filter((t) => t[0] !== '+' && !isNewModesWord(t)));
  for (const id of Object.keys(regionDefaults)) if (!own.has(id) && !own.has('+' + id)) off.add(id);
  for (const id of Object.keys(mg.gens)) if (newModeOff(id, own, mg)) off.add(id);
  return off;
}
/** A list with no "seen@" gets the one derived from it — the same enforcement, and editing cannot move it. */
export function freeze(csv: string, mg: ModeGens): string {
  const own = ownSet(csv);
  if (!mg.now) return csv;
  const s = newModesState(own, mg);
  if (s.stamped) return csv;
  own.add('seen@' + s.seen);
  return [...own].join(',');
}
/** The owner has seen every switch: each new one keeps the state it has, then "seen@<now>". */
export function stamp(csv: string, mg: ModeGens, regionDefaults: Record<string, string>): string {
  const own = ownSet(csv);
  if (!mg.now) return csv;
  const s = newModesState(own, mg);
  if (s.stamped && s.seen >= mg.now) return csv;
  for (const id of Object.keys(mg.gens)) {
    if (mg.gens[id] <= s.seen) continue;
    const mine = own.has(id), plus = own.has('+' + id);
    const off = mine || (!plus && s.off);
    own.delete(id); own.delete('+' + id);
    if (off) own.add(id); else if (plus && regionDefaults[id]) own.add('+' + id);
  }
  for (const t of [...own]) if (t.startsWith('seen@')) own.delete(t);
  own.add('seen@' + mg.now);
  return [...own].join(',');
}
export function setPolicy(csv: string, v: 'on' | 'off'): string {
  const own = ownSet(csv);
  own.delete('newmodes@on'); own.delete('newmodes@off'); own.add('newmodes@' + v);
  return [...own].join(',');
}
/** Every listening demodulator and whole-radio mode off — a server nobody can listen to. */
export function noneLeft(csv: string, regionDefaults: Record<string, string>, mg: ModeGens): boolean {
  const off = enforcedOff(ownSet(csv), regionDefaults, mg);
  return [...LISTEN_DEMODS, ...WHOLE_MODES].every((id) => off.has(id));
}
/** ALL ON / ALL OFF over `ids` (the plain switches; never a "+" option). ALL OFF keeps `keep` (the landing mode) on;
 *  null = refused (no landing mode among `ids`), the caller says why. */
export function allSet(csv: string, on: boolean, ids: string[], keep: string, regionDefaults: Record<string, string>,
                       mg: ModeGens): string | null {
  const own = ownSet(csv);
  const tick = (id: string) => { own.delete(id); if (regionDefaults[id] || modeIsNew(id, own, mg)) own.add('+' + id); };
  if (on) { for (const id of ids) tick(id); return [...own].join(','); }
  if (!keep || !ids.includes(keep)) return null;
  for (const id of ids) {
    if (id === keep) { tick(id); continue; }
    own.delete('+' + id); own.add(id);
  }
  return [...own].join(',');
}
