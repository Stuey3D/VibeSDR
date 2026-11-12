/**
 * ★★★ V12 BACKGROUND DECODING — the model behind the Android server screen's editor (components/BackgroundDecodingSetup).
 *
 * The SAME setting the Linux/Mac Setup page edits (vibe_setup_page.h bgLoad / bgCollect / bgRender) and the engine reads
 * (vibe_bgdecode_config.h parse): ONE JSON string per radio, RadioConfig::bgDecode —
 *   {"on":true,"waitMin":5,"keepInRange":false?,"profiles":[{"centre":500000,"from":"00:00","to":"00:00",
 *     "vfos":[{"hz":517500,"mode":"usb","dec":"navtex"}]} | {"ism":"433",…} | {"ais":true,…}]}
 * ★ The DECODERS are not listed here: they come from the engine on this phone (VibeLocalSDR.backgroundCaps →
 *   LocalSdrShim::backgroundCapsJson, from vibe::bg::decoders()), so a decoder added to the engine reaches this screen
 *   by construction (feedback: no hardcoded settings against a server). What lives here is the PAGE's arithmetic — the
 *   window, the NDB segment, the Band reach, the DSC watch — mirrored from the Setup page so both draw the same lines.
 */

export type BgVfo = { hz: number; mode: string; dec: string };
export type BgProfile = { centre: number; from: string; to: string; vfos: BgVfo[]; ism?: string; ais?: boolean };
export type BgModel = { on: boolean; waitMin: number; keepInRange: boolean; profiles: BgProfile[] };

/** One decoder as the engine lists it (backgroundCapsJson "decoders"). */
export type BgDecoderDef = { id: string; label: string; modes: string[]; tone: number; band?: boolean; ndb?: boolean };
/** One Band the engine's Band digital spots can sit on (the hwinfo `spotBands` shape). */
export type BgSpotBand = { id: string; name: string; hz: number; lo: number; hi: number; bandLo: number; bandHi: number; modes?: string[] };
export type BgCaps = {
  decoders: BgDecoderDef[];
  ismBands: string[];
  spotBands: BgSpotBand[];
  maxVfos: number;
  maxProfiles: number;
  decoderSlotsDefault: number;
};

/** The editor's demodulators — only those some background decoder works on (the Setup page's BG_DEMODS). */
export const BG_DEMODS: Record<string, string> = { usb: 'USB', lsb: 'LSB', nfm: 'NFM' };
/** ★ Words for the ISM band ids the ENGINE lists (vibe_ism_bands.h) — the Setup page's labels. An id the engine adds
 *  that has no words here is still offered, under its own id. */
export const BG_ISM_WORDS: Record<string, string> = {
  '315': '315 MHz (US / Japan: tyre sensors, key fobs, remotes)',
  '433': '433.92 MHz (weather stations, sensors, doorbells, tyre sensors)',
  '868': '868 MHz SRD — sensors and meters (EU: LaCrosse, Bresser, Fine Offset; water, heat and gas meters)',
  '907': '907 MHz — Meshtastic US (LongFast 906.875)',
  '910': '910 MHz — MeshCore US / Canada (910.525)',
  '915': '915 MHz (US: LaCrosse, Ambient, Fine Offset; MeshCore AU, LoRaWAN AU915)',
  '920': '920 MHz — Meshtastic ANZ (LongFast 919.875), MeshCore Vietnam / Thailand',
  '923': '923 MHz — MeshCore Brazil / Australia (923.125), AS923, Meshtastic JP / KR / TW',
};
export const BG_WAIT_CHOICES = [1, 2, 5, 10, 15, 30, 60];
export const BG_NDB_LO = 190000, BG_NDB_HI = 535000, BG_NDB_MIN = 100000, BG_NDB_LEGACY_CENTRE = 362750;
export const DSC_HF = [2187500, 4207500, 6312000, 8414500, 12577000, 16804500];
export const DSC_VHF = 156525000;

export const emptyBgModel = (): BgModel => ({ on: false, waitMin: 5, keepInRange: true, profiles: [] });
export const bgNewProfile = (): BgProfile => ({ centre: 0, from: '00:00', to: '00:00', vfos: [{ hz: 0, mode: 'usb', dec: 'navtex' }] });

/** Read a stored bgDecode string — the Setup page's bgLoad. An unreadable one is off with no profiles. */
export function parseBg(json: string | null | undefined): BgModel {
  const m = emptyBgModel();
  try {
    const j = json ? JSON.parse(json) : null;
    if (j && typeof j === 'object') {
      m.on = !!j.on;
      m.waitMin = j.waitMin > 0 ? +j.waitMin : 5;
      m.keepInRange = j.keepInRange !== false;
      m.profiles = Array.isArray(j.profiles) ? j.profiles.map((p: any): BgProfile => {
        const q: BgProfile = {
          centre: +p.centre || 0, from: p.from || '00:00', to: p.to || '00:00',
          vfos: (Array.isArray(p.vfos) ? p.vfos : []).map((v: any) => ({
            hz: +v.hz || 0, mode: BG_DEMODS[v.mode] ? v.mode : 'usb', dec: v.dec || 'navtex' })),
        };
        if (typeof p.ism === 'string' && p.ism) { q.ism = p.ism; q.centre = 0; q.vfos = []; }
        else if (p.ais === true) { q.ais = true; q.centre = 0; q.vfos = []; }
        // ★ an older whole-radio NDB profile is the VFO profile it now is (the engine converts it the same way)
        else if (p.ndb === true) { q.centre = BG_NDB_LEGACY_CENTRE; q.vfos = [{ hz: 0, mode: 'usb', dec: 'ndb-skim' }]; }
        return q;
      }) : [];
    }
  } catch { /* off, no profiles — the next save writes a good one */ }
  return m;
}

/** The string to store — the Setup page's bgCollect. Off with nothing set up stores nothing at all (''). */
export function collectBg(m: BgModel): string {
  const anything = m.profiles.some((p) => p.ism || p.ais || p.centre > 0 || p.vfos.some((v) => v.hz > 0 || v.dec === 'ndb-skim'));
  if (!m.on && !anything) return '';
  const out: any = { on: m.on, waitMin: m.waitMin, profiles: m.profiles.map((p) => {
    const q: any = { centre: p.centre, from: p.from, to: p.to, vfos: p.vfos.map((v) => ({ hz: v.hz, mode: v.mode, dec: v.dec })) };
    if (p.ism) q.ism = p.ism;
    if (p.ais) q.ais = true;
    return q;
  }) };
  if (!m.keepInRange) out.keepInRange = false;   // ★ absent = on, as the engine reads it
  return JSON.stringify(out);
}

/** Read the engine's answer (backgroundCapsJson). null = this build's engine has none (an older native side). */
export function parseBgCaps(json: string | null | undefined): BgCaps | null {
  try {
    const j = json ? JSON.parse(json) : null;
    if (!j || !Array.isArray(j.decoders) || !j.decoders.length) return null;
    return {
      decoders: j.decoders.filter((d: any) => d && typeof d.id === 'string').map((d: any) => ({
        id: d.id, label: String(d.label || d.id), modes: Array.isArray(d.modes) ? d.modes : ['usb'], tone: +d.tone || 0,
        band: !!d.band, ndb: !!d.ndb })),
      ismBands: Array.isArray(j.ismBands) ? j.ismBands.map(String) : [],
      spotBands: Array.isArray(j.spotBands) ? j.spotBands : [],
      maxVfos: j.maxVfos > 0 ? j.maxVfos : 16,
      maxProfiles: j.maxProfiles > 0 ? j.maxProfiles : 4,
      decoderSlotsDefault: j.decoderSlotsDefault > 0 ? j.decoderSlotsDefault : 0,
    };
  } catch { return null; }
}

/** ★ A row whose DECODER sets the frequency — its frequency and demodulator are greyed out (Stuart, 2026-10-27). */
export const bgDecoderSetsFreq = (dec: string) => dec === 'spots-band' || dec === 'ndb-skim';
/** The half-window every background VFO must sit inside, kept clear of the capture's edge (vibe::bg::halfWindowHz). */
export const bgHalfWindow = (rate: number) => (rate > 0 ? rate / 2 - Math.min(50000, rate * 0.15) - 3000 : 0);

export function bgNdbSegment(centre: number, rate: number): { lo: number; hi: number; ok: boolean } | null {
  const half = bgHalfWindow(rate);
  if (!(centre > 0) || !(rate > 0)) return null;
  const lo = Math.max(BG_NDB_LO, centre - half), hi = Math.min(BG_NDB_HI, centre + half);
  return { lo, hi, ok: hi - lo >= BG_NDB_MIN };
}

/** The line under an NDB skimmer VFO — the Setup page's bgNdbHint. */
export function bgNdbHint(ndbOk: boolean, ndbBlocked: boolean, centre: number, rate: number, locked: boolean): { bad: boolean; text: string } {
  if (!ndbOk) return { bad: true, text: 'This radio cannot skim NDBs' + (ndbBlocked ? ' — NDB is switched off on it (Modes and decoders).' : ' (an RTL needs direct sampling).') + ' Choose another decoder or remove this VFO.' };
  const seg = bgNdbSegment(centre, rate);
  const what = ' The frequency and demodulator are set by the decoder. Every beacon heard goes to the gallery: the day’s heard log and map, the last 10 minutes and a card for every UTC hour.';
  if (!seg) return { bad: false, text: 'Every NDB beacon in as much of the 190–535 kHz band as the RF centre holds.' + what };
  if (!seg.ok) return { bad: true, text: 'At ' + (centre / 1e3).toFixed(1) + ' kHz and ' + Math.round(rate / 1e3) + ' kS/s this radio sees '
    + (seg.hi > seg.lo ? 'only ' + ((seg.hi - seg.lo) / 1e3).toFixed(0) + ' kHz' : 'none') + ' of the NDB band (190–535 kHz) — the skimmer needs at least 100 kHz. '
    + (locked ? 'On a locked radio the lock must cover the band.' : 'Move the RF centre nearer the band (e.g. 362.75 kHz).') };
  return { bad: false, text: 'Covers ' + (seg.lo / 1e3).toFixed(0) + '–' + (seg.hi / 1e3).toFixed(0) + ' kHz of the NDB band at this RF centre.' + what };
}

export const bgSpotBand = (bands: BgSpotBand[], hz: number) =>
  hz > 0 ? bands.find((b) => hz >= b.bandLo && hz <= b.bandHi) || null : null;
export function bgBandReach(b: BgSpotBand, centre: number, rate: number): boolean {
  if (!(centre > 0) || !(rate > 0)) return true;   // ★ no centre or rate yet: every band (the save checks)
  const half = bgHalfWindow(rate) + 3000;
  return Math.max(Math.abs(b.lo - centre), Math.abs(b.hi - centre)) <= half;
}
/** The line under a Band VFO — the Setup page's bgBandHint. */
export function bgBandHint(bandOk: boolean, bands: BgSpotBand[], hz: number, centre: number, rate: number): { bad: boolean; text: string } {
  if (!bandOk) return { bad: true, text: 'Band digital spots are switched off on this radio (Modes and decoders) — choose another decoder or switch Band back on.' };
  if (!bands.length) return { bad: false, text: 'The server’s band list has not arrived — enter the band’s FT8 dial (7074 for 40 m, 14074 for 20 m). Every digital mode on it is decoded into one summary card an hour.' };
  const b = bgSpotBand(bands, hz);
  if (!b) return { bad: false, text: bands.some((x) => bgBandReach(x, centre, rate))
    ? 'Choose the band. Every digital mode on it is decoded into one summary card an hour; the decoder sets the frequency and demodulator.'
    : 'No band’s digital modes fit inside this radio’s window at this RF centre — move the RF centre onto a band (e.g. 1840 for 160 m).' };
  if (!bgBandReach(b, centre, rate)) return { bad: true, text: b.name + '’s digital modes (' + (b.lo / 1e6).toFixed(4) + '–' + (b.hi / 1e6).toFixed(4)
    + ' MHz) are outside this radio’s window at this RF centre — choose a band it holds, or move the RF centre.' };
  return { bad: false, text: b.name + ': ' + (b.modes || []).join(', ') + ' on ' + (b.lo / 1e6).toFixed(4) + '–' + (b.hi / 1e6).toFixed(4)
    + ' MHz — one summary card an hour; quiet modes cost nothing (the Band governor). The decoder sets the frequency and demodulator.' };
}

/** ★★ DSC WATCH — the Setup page's bgDscWatch: every GMDSS DSC frequency this window hears becomes a VFO at once. Mutates
 *  the profile and returns the sentence to show under it. */
export function bgDscWatch(p: BgProfile, rate: number, locked: boolean, lockFreq: number, cap: number, driver: string, maxFreq = 0): string {
  const half = bgHalfWindow(rate) > 0 ? bgHalfWindow(rate) : 0;
  const all = DSC_HF.concat([DSC_VHF]);
  let centre = locked ? lockFreq : p.centre;
  if (!(centre > 0) && half > 0) {
    let best = 0, bestN = -1;
    for (const a of all) for (const b of all) {
      if (b < a || b - a > 2 * half) continue;
      const c = (a + b) / 2, n = all.filter((f) => Math.abs(f - c) <= half).length;
      if (n > bestN) { bestN = n; best = c; }
    }
    centre = Math.round(best / 1000) * 1000;
    if (!locked) p.centre = centre;
  }
  const inWin = (f: number) => half > 0 && centre > 0 && Math.abs(f - centre) <= half;
  const added: number[] = [], skipped: number[] = [];
  for (const f of all) {
    const hz = f === DSC_VHF ? f : f - 1700;
    if (p.vfos.some((v) => Math.abs(v.hz - hz) < 50 && (v.dec === 'dsc' || v.dec === 'dsc-vhf'))) continue;
    if (!inWin(f)) { skipped.push(f); continue; }
    if (p.vfos.length >= cap) { skipped.push(f); continue; }
    const blank = p.vfos.findIndex((v) => !(v.hz > 0));
    const v: BgVfo = f === DSC_VHF ? { hz, mode: 'nfm', dec: 'dsc-vhf' } : { hz, mode: 'usb', dec: 'dsc' };
    if (blank >= 0) p.vfos[blank] = v; else p.vfos.push(v);
    added.push(f);
  }
  const name = (f: number) => (f === DSC_VHF ? 'Ch 70' : (f / 1e3).toFixed(1));
  let msg = added.length ? 'Added DSC on ' + added.map(name).join(', ') + (added.some((f) => f !== DSC_VHF) ? ' kHz' : '') + '.' : '';
  const out = skipped.filter((f) => f !== DSC_VHF || (driver !== 'airspyhf' && !(maxFreq > 0 && maxFreq < DSC_VHF)));
  if (out.length) msg += (msg ? ' ' : '') + name(out[0]) + (out.length > 1 ? ' and ' + (out.length - 1) + ' more' : '')
    + (half > 0 ? ' fall outside this radio’s ±' + (half / 1e6).toFixed(2) + ' MHz window — give them a profile of their own (another time of day).' : ' need the radio’s sample rate to place.');
  if (!added.length && !out.length) msg = 'Every DSC frequency this window can hear is already here.';
  return msg;
}

/** "13:00" from "12:00" and an offset in minutes — the server's local time beside the UTC times (the phone IS the server). */
export function bgLocal(hhmm: string, offsetMin: number): string {
  const m = /^(\d{1,2}):(\d{2})$/.exec(hhmm || '');
  if (!m) return '';
  const t = (((+m[1] * 60 + +m[2] + offsetMin) % 1440) + 1440) % 1440;
  return String(Math.floor(t / 60)).padStart(2, '0') + ':' + String(t % 60).padStart(2, '0');
}
/** "HH:MM" as typed, or null when it is not a time of day. */
export function bgTime(t: string): string | null {
  const m = /^(\d{1,2}):?(\d{2})$/.exec((t || '').trim());
  if (!m || +m[1] > 23 || +m[2] > 59) return null;
  return String(+m[1]).padStart(2, '0') + ':' + m[2];
}

/* ★★ WHAT THIS RADIO MAY DO IN THE BACKGROUND — by its driver, the rules vibeserver/main.cpp's save applies (the screen is
 *  drawn while the server is stopped, so there is no running radio to ask). AGENTS.md: a profile kind this radio cannot
 *  run is not offered. */
export function bgReach(driver: string, autoDs: boolean, directSampling: number) {
  const ismOk = driver !== 'airspyhf' && driver !== 'icom';
  const aisOk = driver !== 'airspyhf' && driver !== 'icom';
  const rtlLike = !['airspyhf', 'sdrplay', 'icom', 'hackrf', 'airspy', 'usrp'].includes(driver);
  const ndbOk = driver === 'airspyhf' || driver === 'sdrplay' || (rtlLike && (autoDs || directSampling > 0));
  return { ismOk, aisOk, ndbOk };
}
