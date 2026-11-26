/**
 * keepRun.ts — THE LANDING MAP AND ITS KEEP-RUNNING MODES on the app's own server screen (V12 beta96; the server's rule is
 * vibe_keep_run.h / vibe_keep_store.h, the Setup page's is keepGate in vibe_setup_page.h — the same cases are tested in all
 * three: vibeserver/test-keep-run.cpp, scripts/test-setup-keep-run.mjs, scripts/test_keep_run.ts).
 *
 * ★ LOCAL HARDWARE: this screen sets up the app's OWN server for its OWN radio, so the facts come from the radio the app
 *   has (its driver, the rate the owner picked, the modes ticked off) — the one place the app may have its own settings
 *   (feedback_app_no_hardcoded_settings). The engine checks every one again at the moment it enters a mode.
 */
export type KeepModeId = 'adsb' | 'vdl2' | 'acars' | 'vor' | 'ais' | 'ism';
export type KeepMap = 'avmar' | 'amateur';
/** ★★ The Amateur map's ISM (Stuart 11-26: 868 MHz all day for MeshCore repeaters) — its band from ISM's own plan, carried in
 *  the token "ism:868". LoRa coverage follows ISM's own "every channel" setting. No IQ capture of ISM bursts, ever. */
export const KEEP_MODES: { id: KeepModeId; name: string; group: 'aviation' | 'maritime' | 'ism'; block: string; map?: KeepMap }[] = [
  { id: 'adsb',  name: 'ADS-B',         group: 'aviation', block: 'adsb' },
  { id: 'vdl2',  name: 'VDL2',          group: 'aviation', block: 'avdata' },
  { id: 'acars', name: 'ACARS',         group: 'aviation', block: 'avdata' },
  { id: 'vor',   name: 'VOR / ILS map', group: 'aviation', block: 'vormap' },
  { id: 'ais',   name: 'AIS',           group: 'maritime', block: 'ais' },
  { id: 'ism',   name: 'ISM',           group: 'ism',      block: 'ism', map: 'amateur' },
];
export const KEEP_GROUP_WORDS: Record<string, string> = { aviation: 'AVIATION', maritime: 'MARITIME', ism: 'ISM \u00b7 sensors and LoRa (MeshCore, Meshtastic, LoRaWAN)' };
export const MAP_AMATEUR = 'amateur';
export const keepMapOf = (id: string): KeepMap => (KEEP_MODES.find((m) => m.id === keepBase(id))?.map ?? 'avmar');
export const keepBase = (t: string) => String(t).split(':')[0];
/** ACARS + VDL2 together: the datalink's wide band (vibeavi minRateHz, ~6.1 MS/s). */
export const KEEP_COMBO_MIN = 6_083_334;
export const MAP_AVMAR = 'avmar';

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
export function keepGate(avail: KeepAvail[], comboOk: boolean, comboWhy: string, rateHz: number, chosenIn: string[], map: KeepMap = 'avmar'): KeepOffer[] {
  const av = (id: string) => avail.find((a) => a.id === id);
  let toks = keepParse(chosenIn.join(',')).filter((t) => { const a = av(keepBase(t)); return keepMapOf(t) === map && !!a && a.hw && !a.why; });
  let chosen = toks.map(keepBase);
  if (chosen.length > 1 && !(keepIsCombo(chosen) && comboOk)) { chosen = chosen.slice(0, 1); toks = toks.slice(0, 1); }
  return KEEP_MODES.filter((m) => (m.map ?? 'avmar') === map).map((m) => {
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
}): { avail: KeepAvail[]; comboOk: boolean; comboWhy: string } {
  const blocks = new Set(f.blockedModes.split(/[,;\s]+/).map((t) => t.trim().toLowerCase()).filter(Boolean));
  const hf = f.driver === 'airspyhf';
  const avail: KeepAvail[] = KEEP_MODES.map((m) => {
    let hw = !f.isIcom && !(hf && (m.id === 'adsb' || m.id === 'vor'));
    if (m.id === 'ism') hw = !f.isIcom && (f.ismBands ?? []).length > 0;
    if (f.lockedOwnVfos) hw = false;   // ★ a locked centre with its own-VFO listeners: a whole-radio mode would move the window
    let why = '';
    if (blocks.has(m.block)) why = `${m.name} is switched off for this radio (modes offered to listeners).`;
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

/** The ADS-B pair an older engine reads, mirrored from the map (vibekeep::migrate). */
export function keepMirror(map: boolean, modes: string, keep: boolean): { landingAdsb: boolean; adsbKeepRunning: boolean } {
  const ids = keepParse(modes);
  const landingAdsb = map && ids.length === 1 && ids[0] === 'adsb';   // ★ ADS-B is an Aviation & Maritime mode, so this is that map
  return { landingAdsb, adsbKeepRunning: landingAdsb && keep };
}
/** beta95's stored pair → the map (only when the map has never been stored). */
export function keepMigrate(stored: { map: string | null; landingAdsb: boolean; adsbKeep: boolean }): { map: boolean; modes: string; keep: boolean } | null {
  if (stored.map != null) return null;
  if (!stored.landingAdsb) return { map: false, modes: '', keep: false };
  return { map: true, modes: 'adsb', keep: stored.adsbKeep };
}
/** The satellites beside a kept mode — the server's satKeepWarn, said before Start. */
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
