/**
 * ★★★ beta96 "USE AS A SERVER" OFFERS THREE THINGS (Stuart, 2026-11-26) — the pure half, no React Native, so
 * scripts/test_server_choice.ts can prove it with plain node.
 *
 *   1. VIBESERVER SIMPLE — local network only. A name, the address on this network (.local), the capture width and the
 *      waterfall rate, then Start. "This is the thing we got praise for before for being simple and we need that back."
 *   2. VIBESERVER FULL — the full server screen (ServerModeScreen), every setting.
 *   3. RTL-TCP — the engine's raw-IQ door for SDR#, SDR++, SDR Console (local network only, every radio but an Icom).
 *
 * ★★ THE CHOICE IS REMEMBERED (CHOICE_KEY): opening "Use as a server" again goes straight to the last one, and every
 *    one of the three has a way back to the list.
 * ★★★ SIMPLE ↔ FULL KEEP EACH OTHER'S SETTINGS. They are one server with one settings store (VibeRemoteSetup.kt — the
 *     config the server is started and restored from). Simple writes only its own four (SIMPLE_KEYS) into that store and
 *     marks it `simple: true`; the Kotlin side then RUNS a view of it (simpleView: the radio's own front-end settings,
 *     Simple's four, defaults for everything else, and LAN_ONLY_OFF) while the store keeps every Full setting untouched
 *     for the day the owner goes back to Full. Full's Start writes its config whole, without the mark.
 *     ★ The same lists are in VibeServerBoot.kt — test_server_choice.ts reads that file and fails if they drift.
 */

export type ServerChoice = 'simple' | 'full' | 'rtltcp';

/** AsyncStorage: the last one used. Absent = never chosen → the list is shown. */
export const CHOICE_KEY = 'vs_serverchoice';
/** AsyncStorage: the address on this network, as typed ('' = made from the server name, as it always was). */
export const MDNS_KEY = 'vs_mdnsname';

export function parseChoice(v: unknown): ServerChoice | null {
  return v === 'simple' || v === 'full' || v === 'rtltcp' ? v : null;
}

/** The route each choice opens (App.tsx's stack names; Lite's own small router uses the same names). */
export function routeFor(c: ServerChoice): 'SimpleServer' | 'ServerMode' | 'RtlTcpServer' {
  return c === 'simple' ? 'SimpleServer' : c === 'full' ? 'ServerMode' : 'RtlTcpServer';
}

/**
 * What the list offers. ★ AGENTS.md: a choice that cannot work here is not drawn.
 *  • Lite: Simple and Full only — RTL-TCP was removed from Lite on purpose (Stuart, 2026-09-18: "rtl-tcp can be fully
 *    removed"), and Lite's native side has no RTL-TCP door.
 *  • An Icom: no RTL-TCP — it gives no raw IQ (the door refuses it; the Full screen says the same).
 *  • A build without the RTL-TCP door at all: no RTL-TCP.
 */
export function choicesFor(o: { lite: boolean; rtlTcpSupported: boolean; icom: boolean }): ServerChoice[] {
  const out: ServerChoice[] = ['simple', 'full'];
  if (!o.lite && o.rtlTcpSupported && !o.icom) out.push('rtltcp');
  return out;
}

/** The words on the list — one place, so the tour and About can quote them. */
export const CHOICE_WORDS: Record<ServerChoice, { title: string; tag: string; desc: string }> = {
  simple: { title: 'VibeServer Simple', tag: 'Local network only',
            desc: 'A name, a capture width and a waterfall rate, then Start. Everyone on your Wi-Fi can listen. '
                + 'No internet listing, no sharing, no feeds.' },
  full:   { title: 'VibeServer Full', tag: 'Every setting',
            desc: 'The full server screen: PIN and admin password, public listing on vibesdr.net, shared and locked '
                + 'ranges, decoders, background decoding, the community map and feeds.' },
  rtltcp: { title: 'RTL-TCP', tag: 'Raw IQ',
            desc: 'For SDR#, SDR++ or SDR Console, with VibeAGC. Local network only, one listener, no PIN.' },
};

/**
 * ★ The address on this network, made exactly as the server makes it (VibeServerOps.hostSlug): lower case, anything
 *   that is not a letter or a digit becomes one '-', no '-' at either end, at most 32 characters, "vibesdr" if nothing
 *   is left. It is a PREVIEW — two servers on one network cannot hold the same name, so the server may add "-2".
 */
export function mdnsLabel(s: string): string {
  const t = String(s || '').toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '');
  return t ? t.slice(0, 32) : 'vibesdr';
}

/** The keys Simple writes into the one settings store — and nothing else. */
export const SIMPLE_KEYS = ['name', 'mdnsName', 'sampleRate', 'lockedRate', 'rateLock', 'maxFftRate', 'simple'] as const;

/**
 * ★★★ WHAT A SIMPLE SERVER RUNS WITH BESIDES ITS FOUR: the RADIO's own settings — what the aerial and the hardware need,
 *     which are not "server" choices at all. Dropping them would leave a powered aerial without its bias-T, a converter
 *     uncorrected, an SDRplay card and an Icom's CI-V lost — a deaf server that looks broken. Kept from the store as the
 *     owner set them (in Full, or on the /setup page); a fresh install has none, and the radio's defaults apply.
 *  ★ adminPassword is kept too: a password the owner set still protects the controls. Simple never sets one.
 */
export const RADIO_KEYS = [
  'ppm', 'ppb', 'biasT', 'directSampling', 'autoDirectSampling', 'directSamplingBelowHz',
  'converterOffsetHz', 'converterInputLoHz', 'converterInputHiHz',
  'rtlAgc', 'tunerBwAuto', 'restGain', 'agcLock', 'gainLimits', 'gainLocks', 'gainSplits', 'gainCurves', 'ifBwLimits',
  'rspVibeAgc', 'nbWide',
  'rfNotch', 'dabNotch', 'rspAmNotch', 'rspHdr', 'rspExtRef', 'rspDabDecim', 'rfAgc', 'autoNotch', 'userNotch',
  'dabAgcOverride', 'dabAgcTarget', 'agcSet', 'agcSetLock',
  'icomBaud', 'icomCiv', 'icomScopeSpeed', 'icomTuneLock', 'icomPowerOn', 'icomAutoOff', 'icomOffDelayMin',
  'antennaSwitch', 'antennaMap', 'antennaLocked', 'antennaNames', 'radioLabel', 'keepRadioAlive',
  'adminPassword', 'autoRestore', 'advertise',
] as const;

/**
 * ★★★ LOCAL NETWORK ONLY — what a Simple server never does, set to a definite OFF so no stored Full setting can ride
 *     along: no community map, no PSK Reporter / WSPRnet, no DMR ID lookups, no ADS-B feed to flight trackers, no
 *     background decoding, no idle DX scanner. The public listing (the tunnel) is the fourth part of the rule and is
 *     not a config key: VibeServerOps / VibeServerRestore skip VibeTunnel.restoreIfWanted for a Simple server, and the
 *     /setup page refuses to list one.
 */
export const LAN_ONLY_OFF: Record<string, string | boolean> = {
  communityMap: false, pskrOn: false, wsprnetOn: false, dmrIdLookup: false,
  adsbFeed: '', bgDecode: '', dxScan: '',
};

/** What Simple sends to Start: its four, and the mark. The server merges them into the store (simpleMerge). */
export function simpleStartConfig(o: { name: string; mdnsName: string; rate: number; topRate: number; fps: number }) {
  const name = String(o.name || '').trim() || 'VibeSDR';
  const rate = Math.max(0, Math.round(o.rate || 0));
  return {
    simple: true,
    name,
    mdnsName: String(o.mdnsName || '').trim(),
    // ★ 0 = listeners choose their span: start at the radio's top rate (its own list), never pin.
    sampleRate: rate || Math.max(0, Math.round(o.topRate || 0)) || 2_400_000,
    lockedRate: rate,
    rateLock: rate > 0,
    maxFftRate: Math.max(0, Math.round(o.fps || 0)),
  };
}

/** The store after a Simple Start: everything kept, Simple's own keys replaced. (VibeServerOps.simpleMerge's twin.) */
export function simpleMerge(stored: Record<string, unknown>, incoming: Record<string, unknown>): Record<string, unknown> {
  const out: Record<string, unknown> = { ...(stored || {}) };
  for (const k of SIMPLE_KEYS) if (k in incoming) out[k] = incoming[k];
  out.simple = true;
  return out;
}

/** What the engine runs for a Simple server. (VibeServerBoot.simpleView's twin.) Absent keys take the engine's defaults. */
export function simpleView(cfg: Record<string, unknown>): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const k of [...SIMPLE_KEYS, ...RADIO_KEYS]) if (k in cfg) out[k] = cfg[k];
  Object.assign(out, LAN_ONLY_OFF);
  // ★ One listener's radio, as Simple always was; no PIN (local network only), the web page on.
  out.maxUsers = 1; out.advanced = false; out.pin = ''; out.webServer = true;
  return out;
}

/** Is this stored config a Simple server's? */
export const isSimpleConfig = (cfg: Record<string, unknown> | null | undefined) => !!cfg && cfg.simple === true;
