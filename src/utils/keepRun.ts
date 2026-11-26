/**
 * keepRun.ts — THE LANDING MAP AND ITS KEEP-RUNNING MODES on the app's own server screen (V12 beta96; the server's rule is
 * vibe_keep_run.h / vibe_keep_store.h, the Setup page's is keepGate in vibe_setup_page.h — the same cases are tested in all
 * three: vibeserver/test-keep-run.cpp, scripts/test-setup-keep-run.mjs, scripts/test_keep_run.ts).
 *
 * ★ LOCAL HARDWARE: this screen sets up the app's OWN server for its OWN radio, so the facts come from the radio the app
 *   has (its driver, the rate the owner picked, the modes ticked off) — the one place the app may have its own settings
 *   (feedback_app_no_hardcoded_settings). The engine checks every one again at the moment it enters a mode.
 */
export type KeepModeId = 'adsb' | 'vdl2' | 'acars' | 'vor' | 'ais' | 'aisdsc' | 'ism';
export type KeepMap = 'avmar' | 'amateur';
/** ★★ The Amateur map's ISM (Stuart 11-26: 868 MHz all day for MeshCore repeaters) — its band from ISM's own plan, carried in
 *  the token "ism:868". LoRa coverage follows ISM's own "every channel" setting. No IQ capture of ISM bursts, ever. */
export const KEEP_MODES: { id: KeepModeId; name: string; group: 'aviation' | 'maritime' | 'ism'; block: string; map?: KeepMap }[] = [
  { id: 'adsb',  name: 'ADS-B',         group: 'aviation', block: 'adsb' },
  { id: 'vdl2',  name: 'VDL2',          group: 'aviation', block: 'avdata' },
  { id: 'acars', name: 'ACARS',         group: 'aviation', block: 'avdata' },
  { id: 'vor',   name: 'VOR / ILS map', group: 'aviation', block: 'vormap' },
  { id: 'ais',   name: 'AIS',           group: 'maritime', block: 'ais' },
  // ★★★ beta96 THE MARITIME COMBINATION (vibe_maritime_combo.h): AIS + DSC channel 70 from one wide capture — one choice, drawn
  //     only where the capture holds both (KEEP_MARITIME_MIN)
  { id: 'aisdsc', name: 'AIS + DSC (channel 70)', group: 'maritime', block: 'ais' },
  { id: 'ism',   name: 'ISM (choose a band)', group: 'ism', block: 'ism', map: 'amateur' },   // ★★ b96-bgsplit any band of ISM's plan
];
export const KEEP_GROUP_WORDS: Record<string, string> = { aviation: 'AVIATION', maritime: 'MARITIME', ism: 'ISM \u00b7 sensors and LoRa (MeshCore, Meshtastic, LoRaWAN)' };
export const MAP_AMATEUR = 'amateur';
export const keepMapOf = (id: string): KeepMap => (KEEP_MODES.find((m) => m.id === keepBase(id))?.map ?? 'avmar');
export const keepBase = (t: string) => String(t).split(':')[0];
/** ACARS + VDL2 together: the datalink's wide band (vibeavi minRateHz, ~6.1 MS/s). */
export const KEEP_COMBO_MIN = 6_083_334;
export const MAP_AVMAR = 'avmar';
/** ★★★ beta96 AIS + DSC together (vibemar::minRateHz, ~6.17 MS/s). */
export const KEEP_MARITIME_MIN = 6_166_667;

/** Tokens in table order, one per mode: "vdl2", "acars", "ism:868" (ISM only with its band). */
export function keepParse(csv: string | null | undefined): string[] {
  const got: string[] = [];
  for (let t of String(csv || '').split(/[,;\t\n]/)) {
    t = t.trim().toLowerCase();
    if (t === 'acars+vdl2') { got.push('acars', 'vdl2'); continue; }
    const b = keepBase(t), band = t.includes(':') ? t.slice(t.indexOf(':') + 1) : '';
    if (!KEEP_MODES.some((m) => m.id === b)) continue;
    if (b === 'ism' ? /^[a-z0-9]{1,8}$/.test(band) : t === b) got.push(t);
  }
  const out: string[] = [];
  for (const m of KEEP_MODES) { const g = got.find((t) => keepBase(t) === m.id); if (g) out.push(g); }
  return out;
}
export const keepIsCombo = (ids: string[]) => ids.length === 2 && ids.includes('acars') && ids.includes('vdl2');
export function keepRateWords(hz: number): string {
  return (hz / 1e6).toFixed(3).replace(/0+$/, '').replace(/\.$/, '') + ' MS/s';
}
export function keepName(ids: string[], ismWords?: Record<string, string>): string {
  if (keepIsCombo(ids)) return 'ACARS + VDL2';
  if (!ids.length) return '';
  if (keepBase(ids[0]) === 'ism') { const b = ids[0].slice(4); return 'ISM ' + (ismWords?.[b] ?? b + ' MHz'); }
  return KEEP_MODES.find((m) => m.id === ids[0])?.name ?? ids[0];
}

export type KeepAvail = { id: KeepModeId; hw: boolean; why: string };
export type KeepOffer = { id: KeepModeId; tok: string; name: string; group: string; drawn: boolean; enabled: boolean; on: boolean; why: string };

/** vibekeep::gate — one mode at a time at an ordinary rate (the rest greyed with the rate and the reason), ACARS + VDL2
 *  together only where the capture holds both. */
export function keepGate(avail: KeepAvail[], comboOk: boolean, comboWhy: string, rateHz: number, chosenIn: string[], map: KeepMap | '' = 'avmar'): KeepOffer[] {
  const av = (id: string) => avail.find((a) => a.id === id);
  // ★★★ b96-bgsplit map '' = every map's modes (the BACKGROUND DECODING card)
  let toks = keepParse(chosenIn.join(',')).filter((t) => { const a = av(keepBase(t)); return (!map || keepMapOf(t) === map) && !!a && a.hw && !a.why; });
  let chosen = toks.map(keepBase);
  if (chosen.length > 1 && !(keepIsCombo(chosen) && comboOk)) { chosen = chosen.slice(0, 1); toks = toks.slice(0, 1); }
  return KEEP_MODES.filter((m) => !map || (m.map ?? 'avmar') === map).map((m) => {
    const o: KeepOffer = { id: m.id, tok: toks.find((t) => keepBase(t) === m.id) ?? m.id, name: m.name, group: m.group, drawn: true, enabled: true, on: false, why: '' };
    const a = av(m.id);
    if (!a || !a.hw) { o.drawn = false; o.enabled = false; return o; }
    o.on = chosen.includes(m.id);
    if (a.why) { o.enabled = false; o.why = a.why; return o; }
    if (o.on || !chosen.length) return o;
    const partner = chosen.length === 1 && ((chosen[0] === 'acars' && m.id === 'vdl2') || (chosen[0] === 'vdl2' && m.id === 'acars'));
    if (partner && comboOk) return o;
    o.enabled = false;
    if (partner) o.why = comboWhy || `ACARS and VDL2 together need a capture of ${keepRateWords(Math.ceil(KEEP_COMBO_MIN / 1e5) * 1e5)} or more`;
    else if (chosen.length === 2) o.why = "ACARS + VDL2 already take this radio's capture";
    else o.why = (rateHz > 0 ? keepRateWords(rateHz) + ': ' : '') + `one mode at a time on this radio — ${keepName([chosen[0]])} is ticked`;
    return o;
  });
}

/** The app's own radio: what it could ever do (driver), what the owner's settings rule out now (blocks, the rate). */
export function keepAvailLocal(f: {
  driver: string; isIcom: boolean; blockedModes: string; rate: number; dabRateBoost: boolean; lockedOwnVfos: boolean;
  /** the ISM bands this radio reaches (the engine's caps.ismBands) — none = no ISM row */
  ismBands?: string[];
  /** ★★★ beta96 the radio's caps carry "ais+8m" (it lists 8 MS/s and reaches channel 70 + AIS) — an Airspy Mini does not */
  ais8mOk?: boolean;
}): { avail: KeepAvail[]; comboOk: boolean; comboWhy: string } {
  const blocks = new Set(f.blockedModes.split(/[,;\s]+/).map((t) => t.trim().toLowerCase()).filter(Boolean));
  const hf = f.driver === 'airspyhf';
  const avail: KeepAvail[] = KEEP_MODES.map((m) => {
    let hw = !f.isIcom && !(hf && (m.id === 'adsb' || m.id === 'vor'));
    if (m.id === 'ism') hw = !f.isIcom && (f.ismBands ?? []).length > 0;
    if (f.lockedOwnVfos) hw = false;   // ★ a locked centre with its own-VFO listeners: a whole-radio mode would move the window
    let why = '';
    // ★★★ beta96 AIS + DSC: only a wide radio whose rate holds both (an RSP at 8, an R2 at 10, a HackRF, a USRP) — not drawn elsewhere
    // ★★★ …or the owner's "ais+8m" (MARITIME may raise the capture to 8 MS/s — the radio's list has it: these four drivers)
    if (m.id === 'aisdsc') hw = hw && !hf && ['sdrplay', 'airspy', 'hackrf', 'usrp'].includes(f.driver)
                           && (f.rate >= KEEP_MARITIME_MIN || (blocks.has('ais+8m') && !!f.ais8mOk));
    if (m.id === 'aisdsc' && blocks.has('dsc')) why = 'DSC is switched off for this radio (modes offered to listeners).';
    else if (blocks.has(m.block)) why = `${m.name} is switched off for this radio (modes offered to listeners).`;
    else if (m.id === 'adsb' && f.rate > 0 && f.rate < 2_400_000 && !f.dabRateBoost)
      why = 'ADS-B needs a capture of 2.4 MS/s — set the rate to 2.4 MS/s or more, or let modes change the rate.';
    return { id: m.id, hw, why };
  });
  const wideRadio = ['sdrplay', 'airspy', 'hackrf', 'usrp'].includes(f.driver);
  const comboOk = !blocks.has('avdata') && (f.rate >= KEEP_COMBO_MIN || (blocks.has('avdata+8m') && wideRadio));
  const comboWhy = comboOk ? '' : f.rate > 0
    ? `ACARS and VDL2 together need a capture of ${keepRateWords(6_100_000)} or more — this radio runs at ${keepRateWords(f.rate)}` : '';
  return { avail, comboOk, comboWhy };
}

/** The ADS-B pair an older engine reads, mirrored from the map (vibekeep::migrate). `keep` = the background IS the landing's mode. */
export function keepMirror(map: boolean, modes: string, keep: boolean): { landingAdsb: boolean; adsbKeepRunning: boolean } {
  const ids = keepParse(modes);
  const landingAdsb = map && ids.length === 1 && ids[0] === 'adsb';   // ★ ADS-B is an Aviation & Maritime mode, so this is that map
  return { landingAdsb, adsbKeepRunning: landingAdsb && keep };
}
/* ── ★★★ b96-bgsplit BACKGROUND DECODING, separate from the landing (Stuart 11-26: "Listen-Amateur uses FM radio as his landing
 *  zone, so if he enabled background AIS that would break that"). '' = manual (the profiles); else ONE whole-radio mode of
 *  any map. vibekeep::bgNormalise / migrate, the same rules. ── */
export function keepBgNormal(csv: string | null | undefined): string {
  let ids = keepParse(csv);
  if (ids.length > 1 && !keepIsCombo(ids)) ids = ids.slice(0, 1);
  return ids.join(',');
}
/** The background as stored: a stored bgMode wins; never stored → b96-keeprun's keep (background = the landing map's mode). */
export function keepBgMigrate(stored: string | null, mapOn: boolean, modes: string, mapKeep: boolean): string {
  if (stored != null) return keepBgNormal(stored);
  return mapOn && mapKeep ? keepBgNormal(modes) : '';
}
/** b96-keeprun's mapKeep, now a MIRROR: the background is exactly the landing map's mode. */
export function keepMapKeepMirror(mapOn: boolean, modes: string, bg: string): boolean {
  const m = keepParse(modes).join(',');
  return mapOn && !!m && !!bg && keepBgNormal(bg) === m;
}
/** ★★ ISM IS WORLDWIDE: the bands this radio reaches, the server's region's own first (caps.ismOwn), then the rest. */
export function keepIsmOrder(reach: string[], own: string[]): { id: string; own: boolean }[] {
  const o = own.filter((b) => reach.includes(b));
  return [...o.map((id) => ({ id, own: true })), ...reach.filter((b) => !o.includes(b)).map((id) => ({ id, own: false }))];
}
export function keepIsmDefault(reach: string[], own: string[]): string { return keepIsmOrder(reach, own)[0]?.id ?? ''; }
/** beta95's stored pair → the map (only when the map has never been stored). */
export function keepMigrate(stored: { map: string | null; landingAdsb: boolean; adsbKeep: boolean }): { map: boolean; modes: string; keep: boolean } | null {
  if (stored.map != null) return null;
  if (!stored.landingAdsb) return { map: false, modes: '', keep: false };
  return { map: true, modes: 'adsb', keep: stored.adsbKeep };
}
/** The satellites beside a kept mode — the server's satKeepWarn, said before Start. */
/** The satellites beside the BACKGROUND whole-radio mode (b96-bgsplit: `modes` is the background, `keep` whether one is set). */
export function keepSatWords(modes: string, keep: boolean, ismWords?: Record<string, string>): string {
  const ids = keepParse(modes);
  if (!keep || !ids.length) return '';
  if (ids.length === 1 && ids[0] === 'vdl2')
    return 'VDL2 is kept running on this radio: a Meteor or NOAA pass SHARES its 137 MHz capture — VDL2 keeps decoding through '
      + 'the pass (a few seconds’ gap as it starts and ends), the tuner’s IF filter opens over 136.7–138.0 MHz for the pass and '
      + 'closes after. With ORBCOMM ticked, the sky plot keeps building beside VDL2 all day. (A radio whose capture cannot hold both '
      + '— an Airspy HF+ — pauses VDL2 for the pass instead.)';
  const n = keepName(ids, ismWords);
  return `${n} is kept running on this radio: a satellite pass takes the radio — ${n} pauses for the pass, about `
    + '10–15 min, and resumes after. ORBCOMM sky-plot monitoring runs only beside a pass while it is kept running.';
}

/** ★★★ beta96 the modes-card switch a kept / landing map mode stands on ("avdata" for VDL2 / ACARS, "vormap", "ais", "ism",
 *  "adsb") — what ALL OFF keeps on (b96-newmodes: a radio is never left with no modes, and never with its landing off). */
export function keepModeSwitch(modes: string): string {
  const ids = keepParse(modes);
  if (!ids.length) return '';
  return KEEP_MODES.find((m) => m.id === keepBase(ids[0]))?.block ?? '';
}

// ══ ★★★ beta96 THE SINGLE-MODE RADIO (the Setup page's singleOptions / singleFromTicks; vibe_keep_store.h singleOk) ═════════
const LISTEN_DEMODS = ['wfm', 'nfm', 'am', 'sam', 'sau', 'sal', 'ecssu', 'ecssl', 'amst', 'usb', 'lsb', 'cwu', 'cwl', 'drm', 'freedv', 'm17', 'dvsig'];
const WHOLE_MODES = ['dab', 'adsb', 'ism', 'ndb', 'ais', 'avdata', 'sondes', 'vormap'];
export type SingleOption = { tok: string; label: string; why: string };
/** What this radio can be single-mode in — no dead entries. */
export function singleOptionsLocal(f: { avail: KeepAvail[]; ismBands: string[]; ismWords?: Record<string, string>; dab: boolean;
  ndb: boolean; shown: (id: string) => boolean; sat: boolean }): SingleOption[] {
  const out: SingleOption[] = [];
  const av = (id: string) => f.avail.find((a) => a.id === id);
  const rateWhy = (id: string) => { const a = av(id); return a && a.why && !/switched off/.test(a.why) ? a.why : ''; };
  for (const id of ['adsb', 'vdl2', 'acars', 'ais', 'aisdsc', 'vor'])
    if (av(id)?.hw) out.push({ tok: id, label: id === 'vdl2' ? 'DATALINK: VDL2' : id === 'acars' ? 'DATALINK: ACARS' : id === 'vor' ? 'VOR MAP' : id === 'aisdsc' ? 'MARITIME: AIS + DSC (channel 70)' : keepName([id]), why: rateWhy(id) });
  if (av('ism')?.hw) for (const b of f.ismBands) out.push({ tok: 'ism:' + b, label: 'ISM ' + (f.ismWords?.[b] ?? b + ' MHz'), why: '' });
  if (f.dab) out.push({ tok: 'dab', label: 'DAB', why: '' });
  if (f.ndb) out.push({ tok: 'ndb', label: 'NDB', why: '' });
  if (f.shown('sondes')) out.push({ tok: 'sondes', label: 'SONDES', why: '' });
  for (const d of ['wfm', 'nfm', 'am', 'usb', 'lsb', 'drm']) if (f.shown(d)) out.push({ tok: d, label: d.toUpperCase() + ' listening only', why: '' });
  if (f.sat) out.push({ tok: 'sat', label: 'SATELLITES (the radio follows the passes; listeners watch)', why: '' });
  return out;
}
/** The token the owner's TICKS imply — Paul's ADS-B-only radio reads as "adsb" (shown, saved when the owner next changes it). */
export function singleFromTicks(blockedModes: string, landingModes: string, ismBands: string[]): string {
  const off = new Set(String(blockedModes || '').split(/[,;\s]+/).filter(Boolean).map((t) => t.toLowerCase()));
  if (!LISTEN_DEMODS.every((d) => off.has(d))) return '';
  const left = WHOLE_MODES.filter((w) => !off.has(w));
  if (left.length !== 1) return '';
  const w = left[0];
  if (w === 'avdata') return keepParse(landingModes)[0] === 'acars' ? 'acars' : 'vdl2';
  if (w === 'vormap') return 'vor';
  if (w === 'ism') return 'ism:' + (ismBands[0] ?? '868');
  return w;
}
/** The words for a token. */
export function singleWords(t: string, ismWords?: Record<string, string>): string {
  if (!t || t === 'general') return '';
  if (t === 'sat') return 'SATELLITES';
  const named: Record<string, string> = { dab: 'DAB', ndb: 'NDB', sondes: 'SONDES', wfm: 'WFM', nfm: 'NFM', am: 'AM', usb: 'USB', lsb: 'LSB', drm: 'DRM', aisdsc: 'MARITIME (AIS + DSC)' };
  return named[t] ?? keepName([t], ismWords);
}
