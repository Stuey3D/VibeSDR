/**
 * ★★★ THE SDRplay CARD on the Android server screen (Stuart, 2026-11-04: "since SDRPlay is now on android all the sdrplay
 * settings need porting over"; "all the ones currently in Linux go to android").
 *
 * The Setup page's card (vibe_setup_page.h, the `drv === "sdrplay"` branch of the radio's hardware section, plus the IF
 * AGC target rows and the aerial-socket card), with RadioConfig's keys and defaults (vibeserver/vibeserver_config.h) —
 * VibeServerBoot applies them through LocalSdrShim exactly as vibeserver/main.cpp does at start-up.
 * ★ Which rows a model has is `rspCaps` — the Setup page's own rule: the RADIO's answer (hwinfo `radio`, read while the
 *   server runs and remembered), else the model from its name, else everything (the radio refuses what it lacks).
 */
export type RspCard = {
  rfNotch: boolean; dabNotch: boolean; rspAmNotch: boolean; rspHdr: boolean; rspExtRef: boolean;
  rspDabDecim: boolean; rfAgc: boolean; autoNotch: boolean; userNotch: boolean;
  dabAgcOverride: boolean; dabAgcTarget: number; agcSet: number; agcSetLock: boolean;
  /** The aerial socket by band ("0-30MHz Antenna C, 30-150MHz Antenna A") and "Only I may change the aerial". */
  antennaMap: string; antennaPortLocked: boolean;
};
/** RadioConfig's defaults; agcSet -30 is SDRplay's own working point (the Setup page shows and saves it). */
export const RSP_DEFAULTS: RspCard = {
  rfNotch: false, dabNotch: false, rspAmNotch: false, rspHdr: false, rspExtRef: false,
  rspDabDecim: false, rfAgc: false, autoNotch: false, userNotch: true,
  dabAgcOverride: true, dabAgcTarget: -40, agcSet: -30, agcSetLock: false,
  antennaMap: '', antennaPortLocked: false,
};
const clampN = (v: unknown, lo: number, hi: number, d: number) => {
  const n = Number(v);
  return Number.isFinite(n) ? Math.max(lo, Math.min(hi, Math.round(n))) : d;
};
/** A stored card, every field checked; absent / unreadable = the defaults. */
export function readRspCard(json: string | null | undefined): RspCard {
  let j: any = null;
  try { j = json ? JSON.parse(json) : null; } catch { j = null; }
  if (!j || typeof j !== 'object') return { ...RSP_DEFAULTS };
  const b = (k: keyof RspCard) => (typeof j[k] === 'boolean' ? j[k] : RSP_DEFAULTS[k]) as boolean;
  return {
    rfNotch: b('rfNotch'), dabNotch: b('dabNotch'), rspAmNotch: b('rspAmNotch'), rspHdr: b('rspHdr'), rspExtRef: b('rspExtRef'),
    rspDabDecim: b('rspDabDecim'), rfAgc: b('rfAgc'), autoNotch: b('autoNotch'), userNotch: b('userNotch'),
    dabAgcOverride: b('dabAgcOverride'),
    dabAgcTarget: clampN(j.dabAgcTarget, -60, -10, RSP_DEFAULTS.dabAgcTarget),
    agcSet: j.agcSet != null && Number(j.agcSet) > -100 ? clampN(j.agcSet, -72, -10, -30) : -30,
    agcSetLock: b('agcSetLock'),
    antennaMap: typeof j.antennaMap === 'string' ? j.antennaMap.slice(0, 300) : '',
    antennaPortLocked: b('antennaPortLocked'),
  };
}
/** The keys the engine's config carries (VibeServerBoot reads them by these names). */
export function rspConfigKeys(c: RspCard) {
  return {
    rfNotch: c.rfNotch, dabNotch: c.dabNotch, rspAmNotch: c.rspAmNotch, rspHdr: c.rspHdr, rspExtRef: c.rspExtRef,
    rspDabDecim: c.rspDabDecim, rfAgc: c.rfAgc, autoNotch: c.autoNotch, userNotch: c.userNotch,
    dabAgcOverride: c.dabAgcOverride, dabAgcTarget: c.dabAgcTarget, agcSet: c.agcSet, agcSetLock: c.agcSetLock,
  };
}

export type RspCaps = { known: boolean; model: string; rfNotch: boolean; dabNotch: boolean; biasT: boolean;
                        hdr: boolean; amNotch: boolean; extRef: boolean; antennas: string[] };
/** ★ What THIS RSP has — the Setup page's rspCaps, line for line. `live` = hwinfo's `radio` object as last read while
 *  serving (null = never served yet); `name` = what the USB descriptor calls it ("SDRplay RSPdx-R2"). */
export function rspCaps(live: any, name: string): RspCaps {
  if (live && live.driver === 'sdrplay')
    return { known: true, model: String(live.model || ''), rfNotch: !!live.rfNotch, dabNotch: !!live.dabNotch,
             biasT: !!live.biasT, hdr: !!live.hdr, amNotch: !!live.amNotch, extRef: !!live.extRef,
             antennas: Array.isArray(live.antennas) ? live.antennas.filter((x: unknown) => typeof x === 'string') : [] };
  const n = String(name || '');
  const m = /RSP\s*dx-?R2/i.test(n) ? 'RSPdx-R2' : /RSP\s*dx/i.test(n) ? 'RSPdx'
          : /RSP\s*duo/i.test(n) ? 'RSPduo' : /RSP\s*2/i.test(n) ? 'RSP2'
          : /RSP\s*1\s*B/i.test(n) ? 'RSP1B' : /RSP\s*1\s*A/i.test(n) ? 'RSP1A'
          : /RSP\s*1(?![0-9AB])/i.test(n) ? 'RSP1' : '';
  if (!m) return { known: false, model: '', rfNotch: true, dabNotch: true, biasT: true, hdr: true, amNotch: true, extRef: true, antennas: [] };
  const dx = m === 'RSPdx' || m === 'RSPdx-R2';
  return { known: true, model: m,
           rfNotch: m !== 'RSP1', dabNotch: m !== 'RSP1' && m !== 'RSP2', biasT: m !== 'RSP1',
           hdr: dx, amNotch: m === 'RSPduo', extRef: m === 'RSP2' || m === 'RSPduo',
           antennas: m === 'RSP2' ? ['A', 'B', 'Hi-Z'] : dx ? ['A', 'B', 'C']
                   : m === 'RSPduo' ? ['Tuner 1 50Ω', 'Tuner 1 Hi-Z', 'Tuner 2 50Ω'] : [] };
}
