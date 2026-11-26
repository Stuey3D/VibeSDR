/**
 * ★★★ THE PHONE'S SERVER SETTINGS, MIRRORED WITH THE /setup PAGE (beta88, "Remote access to server settings").
 *
 * Stuart: "When a setting is changed in the remote setup page the setting changes on the device menu too so its mirrored
 * and both views show the current status."
 *
 * ★★ ONE TRUTH: the server process keeps the settings the server starts and restores from (VibeRemoteSetup.kt). This
 *    screen still reads and writes AsyncStorage as it always has — that is its own working copy — but:
 *      • on opening, it first takes the server's values for every setting the page can change (syncFromServer), so a
 *        change made on the page while nobody had the app open is what the screen shows;
 *      • as the owner edits, it sends the config its Start would send to the server (putServerSettings), so the page
 *        shows the phone's current settings even before the next Start;
 *      • while it is open, the server TELLS it when the page saved (onServerSettings), and it redraws.
 * ★ Only the controls the page can change are taken from the server (REMOTE_KEYS — the same list as
 *   vibe_android_setup.h supportedControls(); scripts/check-remote-keys.mjs keeps the two in step). Everything else is the
 *   screen's alone, exactly as before.
 * ★ Android only: no other platform runs this server.
 */
import { DeviceEventEmitter, NativeModules, Platform } from 'react-native';
import { NAME_KEY, PUBLIC_NAME_KEY } from './rtlTcpServer';
import { icomPhoneFields } from './icomRadio';
import { readFmDefaults, type FmDefaults } from './fmDefaults';
import { readRspCard, type RspCard } from './rspCard';
import { parseSwitchCfg, serverFields, type AntennaSwitchCfg } from './antennaSwitch';

const Local: any = (NativeModules as any).VibeLocalSDR;

export const remoteSettingsSupported = () =>
  Platform.OS === 'android' && typeof Local?.serverSettingsGet === 'function';

/* ★★★ beta88 (Stuart, 2026-11-20: "all settings for android need to be on the remote setup page") — EVERY setting on the
 *  phone's server screen is now on the page, so every one of them comes back here. Three kinds:
 *    • REMOTE_KEYS — one server key, one storage key, a plain encoding;
 *    • SPECIAL — a storage value the screen keeps in its own shape (the capture width, the waterfall tier, the converter
 *      in MHz, the FM defaults / Icom / SDRplay card / antenna switch as one JSON value each, the location, the PIN);
 *    • DEVICE_ONLY / NOT_SETTINGS — what the page may only SEE, or what is not a setting at all (a cache, the admin
 *      password). scripts/check-remote-keys.mjs fails if a key of the screen's K table is in none of them. */

/** ★ The server's key → the screen's AsyncStorage key, and how the screen writes its value. Kept beside the screen's own
 *  key table (ServerModeScreen K) by name: a rename there must be made here too. */
export type RemoteKey = { native: string; storage: string; enc: 'str' | 'num' | 'bool01' | 'uncomp' };
export const REMOTE_KEYS: RemoteKey[] = [
  { native: 'name',             storage: NAME_KEY,             enc: 'str' },
  { native: 'advertise',        storage: 'vs_advertise',       enc: 'bool01' },
  { native: 'landingMessage',   storage: 'vs_landingmsg',      enc: 'str' },
  { native: 'landingLinkUrl',   storage: 'vs_landingurl',      enc: 'str' },
  { native: 'landingLinkLabel', storage: 'vs_landinglbl',      enc: 'str' },
  { native: 'uncompressedAudio', storage: 'vs_uncompressed',   enc: 'uncomp' },
  { native: 'trustedProxies',   storage: 'vs_proxies',         enc: 'str' },
  { native: 'oneRadioPerIp',    storage: 'vs_oneradioperip',   enc: 'bool01' },
  { native: 'decoderMax',       storage: 'vs_decodermax',      enc: 'num' },
  { native: 'rttySkimWorkers',  storage: 'vs_rttyskimworkers', enc: 'num' },
  { native: 'galleryMb',        storage: 'vs_gallerymb',       enc: 'num' },
  { native: 'galleryPublic',    storage: 'vs_gallerypublic',   enc: 'bool01' },
  { native: 'gallerySaveLive',  storage: 'vs_gallerysavelive', enc: 'bool01' },
  { native: 'communityMap',     storage: 'vs_communitymap',    enc: 'bool01' },   // ★★ V12 beta94 the community map
  { native: 'communityOff',     storage: 'vs_communityoff',    enc: 'str' },
  // ★ V12 beta90 PSK Reporter / WSPRnet / the DMR ID lookup
  { native: 'pskrOn',           storage: 'vs_pskron',          enc: 'bool01' },
  { native: 'pskrCall',         storage: 'vs_pskrcall',        enc: 'str' },
  { native: 'pskrAntenna',      storage: 'vs_pskrantenna',     enc: 'str' },
  { native: 'wsprnetOn',        storage: 'vs_wsprneton',       enc: 'bool01' },
  { native: 'wsprnetCall',      storage: 'vs_wsprnetcall',     enc: 'str' },
  { native: 'dmrIdLookup',      storage: 'vs_dmridlookup',     enc: 'bool01' },
  { native: 'batteryPauseAt',   storage: 'vs_batpause',        enc: 'num' },
  { native: 'webServer',        storage: 'vs_webserver',       enc: 'bool01' },
  { native: 'compressAudio',    storage: 'vs_compress',        enc: 'bool01' },
  { native: 'advanced',         storage: 'vs_advanced',        enc: 'bool01' },
  { native: 'publicName',       storage: PUBLIC_NAME_KEY,      enc: 'str' },
  { native: 'radioLabel',       storage: 'vs_radiolabel',      enc: 'str' },
  { native: 'antenna',          storage: 'vs_antenna',         enc: 'str' },
  { native: 'antennaIcon',      storage: 'vs_antennaicon',     enc: 'str' },
  { native: 'antennaRanges',    storage: 'vs_antranges',       enc: 'str' },
  { native: 'antennaFilters',   storage: 'vs_antfilters',      enc: 'str' },
  { native: 'antennaNames',     storage: 'vs_antnames',        enc: 'str' },
  { native: 'maxUsers',         storage: 'vs_maxusers',        enc: 'num' },
  { native: 'askBeforeTuning',  storage: 'vs_askfirst',        enc: 'bool01' },
  { native: 'zoomSpectrum',     storage: 'vs_zoomspec',        enc: 'bool01' },
  { native: 'radioUse',         storage: 'vs_radiouse',        enc: 'str' },
  { native: 'lockedCentre',     storage: 'vs_lockedcentre',    enc: 'num' },
  { native: 'spectrogram',      storage: 'vs_spectrogram',     enc: 'bool01' },
  { native: 'sessionLimitMin',  storage: 'vs_sessionlimit',    enc: 'num' },
  { native: 'sessionLimitSoft', storage: 'vs_limitsoft',       enc: 'bool01' },
  { native: 'idleKickMin',      storage: 'vs_idlekick',        enc: 'num' },
  { native: 'nbWide',           storage: 'vs_nbwide',          enc: 'num' },
  { native: 'vfoB',             storage: 'vs_vfob',            enc: 'bool01' },
  { native: 'idleGraceSec',     storage: 'vs_idlegrace',       enc: 'num' },
  { native: 'blockedModes',     storage: 'vs_blockedmodes',    enc: 'str' },
  { native: 'dabRateBoost',     storage: 'vs_dabboost',        enc: 'bool01' },
  { native: 'adsb256',          storage: 'vs_adsb256',         enc: 'bool01' },
  { native: 'ism256',           storage: 'vs_ism256',          enc: 'bool01' },
  { native: 'dabScanLabels',    storage: 'vs_dabscan',         enc: 'num' },
  { native: 'gainLimits',       storage: 'vs_gainlimits',      enc: 'str' },
  { native: 'gainLocks',        storage: 'vs_gainlocks',       enc: 'str' },
  { native: 'gainSplits',       storage: 'vs_gainsplits',      enc: 'str' },
  { native: 'gainCurves',       storage: 'vs_gaincurves',      enc: 'str' },
  { native: 'ifBwLimits',       storage: 'vs_ifbwlimits',      enc: 'str' },
  { native: 'restGain',         storage: 'vs_restgain',        enc: 'num' },
  { native: 'agcLock',          storage: 'vs_agclock',         enc: 'bool01' },
  { native: 'rtlAgc',           storage: 'vs_rtlagc',          enc: 'bool01' },
  { native: 'tunerBwAuto',      storage: 'vs_tunerbwauto',     enc: 'bool01' },
  { native: 'allowRanges',      storage: 'vs_allow',           enc: 'str' },
  { native: 'blockRanges',      storage: 'vs_block',           enc: 'str' },
  { native: 'landingDabChannel', storage: 'vs_landingdabch',   enc: 'num' },
  { native: 'landingDabSid',    storage: 'vs_landingdabsid',   enc: 'num' },
  { native: 'landingDabService', storage: 'vs_landingdabsvc',  enc: 'str' },
  { native: 'landingAdsb',      storage: 'vs_landingadsb',     enc: 'bool01' },
  { native: 'adsbKeepRunning',  storage: 'vs_adsbkeep',        enc: 'bool01' },
  { native: 'landingMap',       storage: 'vs_landingmap',      enc: 'str' },      // ★★★ beta96 the landing map
  { native: 'landingModes',     storage: 'vs_landingmodes',    enc: 'str' },
  { native: 'mapKeep',          storage: 'vs_mapkeep',         enc: 'bool01' },
  { native: 'bgMode',           storage: 'vs_bgmode',          enc: 'str' },      // ★★★ b96-bgsplit the background whole-radio mode ('' = manual) — the page always sends a definite value
  { native: 'singleMode',       storage: 'vs_singlemode',      enc: 'str' },      // ★★★ beta96 the single-mode radio
  { native: 'biasT',            storage: 'vs_biast',           enc: 'bool01' },
  { native: 'autoDirectSampling', storage: 'vs_autods',        enc: 'bool01' },
  { native: 'bgDecode',         storage: 'vs_bgdecode',        enc: 'str' },
  { native: 'dxScan',           storage: 'vs_dxscan',          enc: 'str' },
  { native: 'adsbFeed',         storage: 'vs_adsbfeed',        enc: 'str' },   // ★★ V12 beta96 flight trackers
  { native: 'aircraftFeeds',    storage: 'vs_aircraftfeeds',   enc: 'str' },   // ★★ V12 beta96 server-level feed sources
  { native: 'mapViewers',       storage: 'vs_mapviewers',      enc: 'num' },   // ★★ V12 beta96 the standalone AIR & SEA map's limits
  { native: 'mapTimeMin',       storage: 'vs_maptimemin',      enc: 'num' },
  { native: 'rawIq',            storage: 'vs_rawiq',           enc: 'num' },
  { native: 'rawIqMax',         storage: 'vs_rawiqmax',        enc: 'num' },
  { native: 'rawIqLanMaxHz',    storage: 'vs_rawiqlanmaxhz',   enc: 'num' },
  { native: 'rspVibeAgc',       storage: 'vs_rspvibeagc',      enc: 'num' },
  { native: 'ppb',              storage: 'vs_ppb',             enc: 'num' },
];

/** ★ The server keys the screen takes back in its OWN shape (storageFromSettings below), each → the storage key(s). */
export const SPECIAL_KEYS: Record<string, string[]> = {
  lockedRate: ['vs_rate'], sampleRate: [], rateLock: [], maxFftRate: ['vs_fps'],
  centerFreq: ['vs_landinghz'], mode: ['vs_landingmode'], pin: ['vs_pin', 'vs_pinmode'],
  batteryResumeAt: [],   // derived: pause + 20
  ppm: ['vs_ppm'], directSampling: ['vs_directsampling'], directSamplingBelowHz: ['vs_autodsmhz'],
  converterOffsetHz: ['vs_convoffset', 'vs_convdown'], converterInputLoHz: ['vs_convlo'], converterInputHiHz: ['vs_convhi'],
  fmDeemph: ['vs_fmdefaults'], fmStereo: ['vs_fmdefaults'], fmNr: ['vs_fmdefaults'], fmIms: ['vs_fmdefaults'],
  fmCeq: ['vs_fmdefaults'], fmAutoBw: ['vs_fmdefaults'], fmNb: ['vs_fmdefaults'], fmRemember: ['vs_fmdefaults'],
  icomBaud: ['vs_icom'], icomCiv: ['vs_icom'], icomScopeSpeed: ['vs_icom'], icomTuneLock: ['vs_icom'],
  icomPowerOn: ['vs_icom'], icomAutoOff: ['vs_icom'], icomOffDelayMin: ['vs_icom'],
  rfNotch: ['vs_rsp'], dabNotch: ['vs_rsp'], rspAmNotch: ['vs_rsp'], rspHdr: ['vs_rsp'], rspExtRef: ['vs_rsp'],
  rspDabDecim: ['vs_rsp'], rfAgc: ['vs_rsp'], autoNotch: ['vs_rsp'], userNotch: ['vs_rsp'], dabAgcOverride: ['vs_rsp'],
  dabAgcTarget: ['vs_rsp'], agcSet: ['vs_rsp'], agcSetLock: ['vs_rsp'],
  antennaSwitch: ['vs_antswitch'], antennaMap: ['vs_antswitch', 'vs_rsp'], antennaLocked: ['vs_antswitch', 'vs_rsp'],
  serverLocation: ['lsv_server_location', 'vs_locmode', 'vs_loc_moving'],
  publicShareSec: [],    // ★ the page's share length; the phone's own comes from the running tunnel (tunnelStatus.until)
};

/** ★★ WHAT THE PAGE MAY ONLY SEE (or not even that) — the screen's keys that are NOT remote, each with why. Shown read-only
 *  on the page's "This phone" card where it makes sense (docs/v12/ANDROID-SERVER-PROCESS.md, "Device-only"). */
export const DEVICE_ONLY: Record<string, string> = {
  vs_proto: 'rtl_tcp or VibeServer — rtl_tcp has no web page, so switching from the page would cut the page off with no way back',
  vs_keepalive: 'keep radio alive — the phone holds the USB radio; Android decides what may hold it (shown read-only)',
  vs_adminpw: 'the admin password — never sent to the page and never set from it (security rule)',
  vs_mdnsname: 'the address on this network (.local) — set on the device (VibeServer Simple / Full); the page shows the address the server took',
};
/** The screen's keys that are not settings at all. */
export const NOT_SETTINGS: Record<string, string> = {
  vs_startonboot: 'retired 2026-09-28 — kept only so an old value can be removed',
  vs_antports: 'the ports the radio last listed — a cache of the hardware, not a choice',
  vs_rspcaps: 'the RSP’s capabilities as last served — a cache of the hardware, not a choice',
};

/** Every storage key the mirror may write — read first, so an equal value is never written back. */
export const MIRROR_STORAGE_KEYS: string[] = [...new Set([
  ...REMOTE_KEYS.map((k) => k.storage), ...Object.values(SPECIAL_KEYS).flat(),
])];

export type ServerSettings = {
  settings: Record<string, unknown>;
  remote: boolean;
  hasAdminPassword: boolean;
  /** False until the server has kept a copy (the first Start, or the first edit on this build). */
  hasStore: boolean;
};

export async function getServerSettings(): Promise<ServerSettings | null> {
  if (!remoteSettingsSupported()) return null;
  try {
    const raw = await Local.serverSettingsGet();
    const j = JSON.parse(raw);
    return { settings: j.settings ?? {}, remote: j.remote === true, hasAdminPassword: j.hasAdminPassword === true,
             hasStore: j.hasStore === true };
  } catch { return null; }
}

/** The config the screen's Start would send — the server keeps it as the truth the page shows. `dropLanding`: the owner
 *  chose no landing frequency, so the server's copy must not keep an old one. */
export async function putServerSettings(native: Record<string, unknown>, dropLanding: boolean): Promise<void> {
  if (!remoteSettingsSupported()) return;
  try { await Local.serverSettingsPut(JSON.stringify(native), dropLanding); } catch { /* the next edit or Start sends it */ }
}

/** The owner's switch. Resolves with the server's answer; refuses (with words) without an admin password. */
export async function setRemoteAccess(on: boolean): Promise<{ ok: boolean; message?: string }> {
  if (!remoteSettingsSupported()) return { ok: false, message: 'Not available on this device' };
  try { await Local.setRemoteAccess(on); return { ok: true }; }
  catch (e: any) { return { ok: false, message: e?.message ?? String(e) }; }
}

/** ★ A number written back that says what is already stored ('' and '0', '125' and '125.0') is not written, so nothing
 *  churns; text must match exactly. */
const same = (a: string | null | undefined, b: string, numeric: boolean) => {
  const A = a ?? '';
  if (A === b) return true;
  if (!numeric) return false;
  const x = Number(A.trim() === '' ? 0 : A), y = Number(b.trim() === '' ? 0 : b);
  return Number.isFinite(x) && Number.isFinite(y) && x === y;
};
const mhz = (hz: number) => String(Number((hz / 1e6).toFixed(6)));
const has = (s: Record<string, unknown>, ...k: string[]) => k.some((x) => x in s);
const b01 = (v: unknown) => (v === true || v === 1 || v === '1' || v === 'true');

/** ★ The switch the page saved (antennaSwitch JSON + the per-band map + the lock) back into the screen's own shape. The
 *  screen's copy is kept untouched when it already says the same; otherwise each antenna becomes a relay of its own. */
export function switchFromServer(s: Record<string, unknown>, cur: AntennaSwitchCfg): AntennaSwitchCfg {
  const want = { antennaSwitch: String(s.antennaSwitch ?? ''), antennaMap: String(s.antennaMap ?? ''), antennaLocked: b01(s.antennaLocked) };
  const now = serverFields(cur);
  if (now.antennaSwitch === want.antennaSwitch && (!want.antennaSwitch || (now.antennaMap === want.antennaMap && now.antennaLocked === want.antennaLocked))) return cur;
  if (!want.antennaSwitch) return { ...cur, enabled: false };
  let j: any = null;
  try { j = JSON.parse(want.antennaSwitch); } catch { return cur; }
  const ants: any[] = Array.isArray(j?.antennas) ? j.antennas.slice(0, 16) : [];
  const map = want.antennaMap.split(',').map((t) => t.trim()).filter(Boolean);
  const bandsFor = (name: string) => map.filter((e) => e.endsWith(' ' + name)).map((e) => e.slice(0, e.length - name.length - 1).trim()).join(', ');
  return {
    enabled: j?.enabled !== false, broker: `${String(j?.host ?? '')}:${Number(j?.port) || 1883}`,
    user: String(j?.user ?? ''), pass: String(j?.pass ?? ''), deviceName: cur.deviceName,
    relays: ants.map((a) => ({ label: String(a?.name ?? ''), cmd: String(a?.cmd ?? ''), state: String(a?.state ?? ''),
                               on: String(a?.on ?? 'ON'), off: String(a?.off ?? 'OFF') })),
    antennas: ants.map((a, i) => ({ name: String(a?.name ?? ''), relay: i, bands: bandsFor(String(a?.name ?? '')) })),
    locked: want.antennaLocked,
  };
}

/**
 * The server's settings → the screen's stored strings, for every remote setting. `cur` = the screen's stored values for
 * MIRROR_STORAGE_KEYS (and the PIN's): a value that already says the same is not written, and a PIN that did not change
 * keeps its mode (a random PIN stays "random").
 */
export function storageFromSettings(s: Record<string, unknown>, cur: Record<string, string | null>): [string, string][] {
  const out: [string, string][] = [];
  const put = (k: string, v: string, numeric = false) => { if (!same(cur[k], v, numeric)) out.push([k, v]); };
  for (const k of REMOTE_KEYS) {
    if (!(k.native in s)) continue;
    const v = s[k.native];
    let text: string;
    switch (k.enc) {
      case 'bool01': text = b01(v) ? '1' : '0'; break;
      case 'num': { const n = Number(v); if (!Number.isFinite(n)) continue; text = String(Math.round(n)); break; }
      case 'uncomp': { const n = Number(v); text = n === 1 || n === 2 ? String(n) : '0'; break; }
      default: text = v == null ? '' : String(v);
    }
    put(k.storage, text, k.enc === 'num' || k.enc === 'uncomp');
  }
  // ★ The landing: absent on the server = none chosen (the screen's 0).
  const hz = Number(s.centerFreq);
  put('vs_landinghz', Number.isFinite(hz) && hz > 0 ? String(Math.round(hz)) : '0', true);
  if (typeof s.mode === 'string' && s.mode) put('vs_landingmode', s.mode);
  // ★ The PIN: '' = none (mode off); a new one = typed (custom); the same one keeps its mode.
  if (typeof s.pin === 'string') {
    const pin = s.pin, curPin = cur.vs_pin ?? '', curMode = cur.vs_pinmode ?? null;
    if (!pin) put('vs_pinmode', 'off');
    else if (pin !== curPin) { out.push(['vs_pin', pin]); put('vs_pinmode', 'custom'); }
    else if (curMode === 'off') put('vs_pinmode', 'custom');
  }
  // ★ The capture width: the screen's 0 = the listener's choice, else the pinned rate.
  if ('lockedRate' in s) put('vs_rate', String(Math.max(0, Math.round(Number(s.lockedRate) || 0))), true);
  if ('maxFftRate' in s) { const f = Number(s.maxFftRate); put('vs_fps', f >= 15 ? 'full' : f > 0 && f <= 7 ? 'quarter' : 'half'); }
  // ★ ppm: absent = leave the radio alone (the screen's empty box).
  if ('ppm' in s) { const n = Number(s.ppm); put('vs_ppm', Number.isFinite(n) ? String(Math.round(n)) : ''); }
  if ('directSampling' in s) { const d = Number(s.directSampling); put('vs_directsampling', d === 1 ? 'i' : d === 2 ? 'q' : 'off'); }
  if ('directSamplingBelowHz' in s) { const n = Number(s.directSamplingBelowHz); if (n > 0) put('vs_autodsmhz', mhz(n), true); }
  // ★ The converter: the screen keeps MHz and a direction; the server a signed offset (an up-converter NEGATIVE).
  if ('converterOffsetHz' in s) {
    const o = Number(s.converterOffsetHz) || 0;
    put('vs_convoffset', o ? mhz(Math.abs(o)) : '', true);
    if (o) put('vs_convdown', o > 0 ? '1' : '0');
  }
  if ('converterInputLoHz' in s) { const n = Number(s.converterInputLoHz) || 0; put('vs_convlo', n ? mhz(n) : '', true); }
  if ('converterInputHiHz' in s) { const n = Number(s.converterInputHiHz) || 0; put('vs_convhi', n ? mhz(n) : '', true); }
  // ★ The FM defaults, one JSON value (services/fmDefaults.ts).
  if (has(s, 'fmDeemph', 'fmStereo', 'fmNr', 'fmIms', 'fmCeq', 'fmAutoBw', 'fmNb', 'fmRemember')) {
    const c = readFmDefaults(cur.vs_fmdefaults);
    const d = Number(s.fmDeemph);
    const next: FmDefaults = {
      deemphUs: 'fmDeemph' in s ? (d === 0 || d === 50 || d === 75 ? d : -1) : c.deemphUs,
      stereo: 'fmStereo' in s ? b01(s.fmStereo) : c.stereo, nr: 'fmNr' in s ? b01(s.fmNr) : c.nr,
      ims: 'fmIms' in s ? b01(s.fmIms) : c.ims, ceq: 'fmCeq' in s ? b01(s.fmCeq) : c.ceq,
      autobw: 'fmAutoBw' in s ? b01(s.fmAutoBw) : c.autobw, nb: 'fmNb' in s ? b01(s.fmNb) : c.nb,
      remember: 'fmRemember' in s ? b01(s.fmRemember) : c.remember,
    };
    if (JSON.stringify(next) !== JSON.stringify(c) || cur.vs_fmdefaults == null) put('vs_fmdefaults', JSON.stringify(next));
  }
  // ★ An Icom's seven, one JSON value (icomPhoneFields).
  if (has(s, 'icomBaud', 'icomCiv', 'icomScopeSpeed', 'icomTuneLock', 'icomPowerOn', 'icomAutoOff', 'icomOffDelayMin')) {
    let stored: any = null; try { stored = cur.vs_icom ? JSON.parse(cur.vs_icom) : null; } catch { stored = null; }
    const c = icomPhoneFields(stored);
    const next = icomPhoneFields({ ...c, ...Object.fromEntries(['icomBaud', 'icomCiv', 'icomScopeSpeed', 'icomTuneLock', 'icomPowerOn',
      'icomAutoOff', 'icomOffDelayMin'].filter((k) => k in s).map((k) => [k, s[k]])) });
    if (JSON.stringify(next) !== JSON.stringify(c)) put('vs_icom', JSON.stringify(next));
  }
  // ★ The antenna switch (the screen's own shape) — and, with no switch, an RSP's per-band sockets live in its card.
  const sw = parseSwitchCfg(cur.vs_antswitch);
  if (has(s, 'antennaSwitch')) {
    const next = switchFromServer(s, sw);
    if (next !== sw) put('vs_antswitch', JSON.stringify(next));
  }
  const rspKeys = ['rfNotch', 'dabNotch', 'rspAmNotch', 'rspHdr', 'rspExtRef', 'rspDabDecim', 'rfAgc', 'autoNotch', 'userNotch',
                   'dabAgcOverride', 'dabAgcTarget', 'agcSet', 'agcSetLock'];
  const noSwitch = !String(s.antennaSwitch ?? '');
  if (has(s, ...rspKeys) || (noSwitch && has(s, 'antennaMap', 'antennaLocked'))) {
    const c: RspCard = readRspCard(cur.vs_rsp);
    const merged: Record<string, unknown> = { ...c };
    for (const k of rspKeys) if (k in s) merged[k] = s[k];
    if (noSwitch) {
      if ('antennaMap' in s) merged.antennaMap = String(s.antennaMap ?? '');
      if ('antennaLocked' in s) merged.antennaPortLocked = b01(s.antennaLocked);
    }
    const next = readRspCard(JSON.stringify(merged));
    if (JSON.stringify(next) !== JSON.stringify(c)) put('vs_rsp', JSON.stringify(next));
  }
  // ★ Where the receiver is — the screen's three keys (vibeServer.ts LOC_KEY / LOCMODE_KEY / LOC_MOVING_KEY).
  const loc = s.serverLocation as any;
  if (loc && typeof loc === 'object') {
    const manual = loc.mode === 'manual' && Number.isFinite(Number(loc.lat)) && Number.isFinite(Number(loc.lon));
    if (manual) {
      const point = { lat: Number(loc.lat), lon: Number(loc.lon), label: String(loc.label ?? '') };
      let c: any = null; try { c = cur.lsv_server_location ? JSON.parse(cur.lsv_server_location) : null; } catch { c = null; }
      if (!c || c.lat !== point.lat || c.lon !== point.lon || (c.label ?? '') !== point.label)
        out.push(['lsv_server_location', JSON.stringify(point)]);
    }
    put('vs_locmode', manual ? 'manual' : 'off');
    put('vs_loc_moving', loc.moving === true ? '1' : '0');
  }
  return out;
}

/** Hear the page's saves while the screen is open. Returns the unsubscribe. */
export function onServerSettings(cb: (settings: Record<string, unknown>) => void): () => void {
  if (!remoteSettingsSupported()) return () => {};
  const sub = DeviceEventEmitter.addListener('VibeServerEvent', (e: any) => {
    if (!e || e.type !== 'settings') return;
    try { const j = JSON.parse(e.json); if (j && j.settings) cb(j.settings); } catch { /* not ours */ }
  });
  return () => sub.remove();
}
