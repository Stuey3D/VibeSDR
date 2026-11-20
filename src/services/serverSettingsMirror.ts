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
import { NAME_KEY } from './rtlTcpServer';

const Local: any = (NativeModules as any).VibeLocalSDR;

export const remoteSettingsSupported = () =>
  Platform.OS === 'android' && typeof Local?.serverSettingsGet === 'function';

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
  { native: 'galleryMb',        storage: 'vs_gallerymb',       enc: 'num' },
  { native: 'galleryPublic',    storage: 'vs_gallerypublic',   enc: 'bool01' },
  { native: 'gallerySaveLive',  storage: 'vs_gallerysavelive', enc: 'bool01' },
  { native: 'batteryPauseAt',   storage: 'vs_batpause',        enc: 'num' },
  { native: 'radioLabel',       storage: 'vs_radiolabel',      enc: 'str' },
  { native: 'antenna',          storage: 'vs_antenna',         enc: 'str' },
  { native: 'maxUsers',         storage: 'vs_maxusers',        enc: 'num' },
  { native: 'askBeforeTuning',  storage: 'vs_askfirst',        enc: 'bool01' },
  { native: 'zoomSpectrum',     storage: 'vs_zoomspec',        enc: 'bool01' },
  { native: 'sessionLimitMin',  storage: 'vs_sessionlimit',    enc: 'num' },
  { native: 'sessionLimitSoft', storage: 'vs_limitsoft',       enc: 'bool01' },
  { native: 'idleKickMin',      storage: 'vs_idlekick',        enc: 'num' },
  { native: 'nbWide',           storage: 'vs_nbwide',          enc: 'num' },
  { native: 'vfoB',             storage: 'vs_vfob',            enc: 'bool01' },
  { native: 'idleGraceSec',     storage: 'vs_idlegrace',       enc: 'num' },
];

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

/**
 * The server's settings → the screen's stored strings, for the remote controls only. `curPin`/`curPinMode` are the
 * screen's stored PIN and its mode, so a PIN that did not change keeps its mode (a random PIN stays "random").
 */
export function storageFromSettings(s: Record<string, unknown>, curPin: string | null, curPinMode: string | null): [string, string][] {
  const out: [string, string][] = [];
  for (const k of REMOTE_KEYS) {
    if (!(k.native in s)) continue;
    const v = s[k.native];
    let text: string;
    switch (k.enc) {
      case 'bool01': text = (v === true || v === 1 || v === '1' || v === 'true') ? '1' : '0'; break;
      case 'num': { const n = Number(v); if (!Number.isFinite(n)) continue; text = String(Math.round(n)); break; }
      case 'uncomp': { const n = Number(v); text = n === 1 || n === 2 ? String(n) : '0'; break; }
      default: text = v == null ? '' : String(v);
    }
    out.push([k.storage, text]);
  }
  // ★ The landing: absent on the server = none chosen (the screen's 0).
  const hz = Number(s.centerFreq);
  out.push(['vs_landinghz', Number.isFinite(hz) && hz > 0 ? String(Math.round(hz)) : '0']);
  if (typeof s.mode === 'string' && s.mode) out.push(['vs_landingmode', s.mode]);
  // ★ The PIN: '' = none (mode off); a new one = typed (custom); the same one keeps its mode.
  if (typeof s.pin === 'string') {
    const pin = s.pin;
    if (!pin) out.push(['vs_pinmode', 'off']);
    else if (pin !== (curPin ?? '')) { out.push(['vs_pin', pin], ['vs_pinmode', 'custom']); }
    else if (curPinMode === 'off') out.push(['vs_pinmode', 'custom']);
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
