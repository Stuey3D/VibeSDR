/**
 * SDRBackend — the adapter contract for multi-backend support (v3 brief §2).
 *
 * The internal model stays UberSDR-shaped: SDRStatus/SDRCallbacks/SDRMode are
 * the lingua franca, and foreign protocols (KiwiSDR, OpenWebRX) translate INTO
 * them. The method surface below mirrors what the UI actually calls on
 * UberSDRClient today — including the view-prediction pair zoom()/pan()
 * rather than the brief's setView(centerHz,bwHz), which is expressible
 * through them (Kiwi quantises inside its adapter; OWRX slices locally).
 *
 * UI gating rule: capability-driven, same pattern as the skin's extension
 * probing — profile UI only when caps.profiles, chat only when caps.chat, etc.
 */

import type { SDRStatus, SDRMode, SDRCallbacks } from './UberSDRClient';

// 'web888' is KiwiAdapter too — a Web-888 / RaspSDR speaks the Kiwi protocol at a different URL,
// nothing more. See isKiwiProtocol() in sdrTypes; anything gating Kiwi behaviour must accept both.
export type BackendKind = 'ubersdr' | 'kiwi' | 'web888' | 'owrx' | 'fmdx';

/** FM-DX transmitter identification (from the server's maps.fmdx.org lookup).
 *  Distances/azimuths are relative to the SERVER's QTH, not the listener. */
export interface FmdxTxInfo {
  tx?:   string;   // transmitter/station name
  city?: string;
  itu?:  string;   // ITU country prefix
  erp?:  number;   // kW
  pol?:  string;   // polarisation (h/v)
  dist?: number;   // km from server QTH
  azi?:  number;   // degrees from server QTH
}

/** Whole-state snapshot from an FM-DX Webserver `/text` frame, normalised for
 *  the tuner screen. Delivered via BackendCallbacks.onFmdxState. */
export interface FmdxState {
  freqHz:  number;
  sig:     number;    // dBf (TEF scale)
  stereo:  boolean;
  rds:     boolean;
  pi:      string;    // hex PI code ('' when none)
  ps:      string;    // programme service (station) name
  rt:      string;    // current RadioText bank
  pty:     number;    // programme type (European RDS table)
  tp:      boolean;
  ta:      boolean;
  af:      number[];  // alternative frequencies, Hz
  users:   number;
  tx?:     FmdxTxInfo;
  countryIso?: string;  // RDS ECC-derived ISO country (e.g. 'GB') — disambiguates logos
  eq?:     boolean;   // cEQ filter on
  ims?:    boolean;   // iMS filter on
  ant?:    number;    // current antenna index
}

/** One-shot FM-DX server capabilities (from /static_data) — drives the
 *  demodulator options sheet (antenna list, whether IF-bandwidth is switchable). */
export interface FmdxServerInfo {
  antennas: { id: number; name: string }[];
  bwSwitch: boolean;
  /** The RECEIVER's name/location, from /static_data `tunerName`. Needed wherever a
   *  txInfo distance is shown: `dist` is measured from the SERVER's QTH, so "46 km"
   *  is meaningless without saying 46 km from WHERE. */
  tunerName?: string;
}

export interface ProfileInfo {
  id:        string;
  name:      string;
  centerHz?: number;  // learned lazily on OWRX (config follows selection)
  bwHz?:     number;
}

/** One aircraft from an OWRX ADS-B list.
 *
 *  Fields are the server's own (verified on-air against a live 1090MHz profile).
 *  `country`/`ccode` come FROM THE SERVER — no ICAO-range table needed — and are the
 *  aircraft's REGISTRY, not where the flight departed: a Ryanair 737 is Irish wherever
 *  it took off from. */
export interface Aircraft {
  icao:      string;      // 24-bit address, hex — the only field always present
  flight?:   string;      // callsign, e.g. RYR4KL
  reg?:      string;      // registration, e.g. D-AENE (the server calls it `aircraft`)
  ccode?:    string;      // ISO country of REGISTRY
  country?:  string;
  altitude?: number;      // ft
  speed?:    number;      // kt (ground)
  vspeed?:   number;      // ft/min — climbing/descending
  course?:   number;      // degrees
  squawk?:   string;
  rssi?:     number;      // dB
  msgs?:     number;
  lat?:      number;
  lon?:      number;
  /** Great-circle km from the RECEIVER (computed here — the server sends position,
   *  not distance). Absent when the aircraft hasn't sent a position yet: plenty of
   *  records carry altitude and speed with no lat/lon at all. */
  distKm?:   number;
  bearing?:  number;      // degrees from the receiver
}

/** A demodulator the backend offers (OWRX reports these; UI gates the picker). */
export interface BackendMode {
  id:    string;            // wire modulation name (usb, wfm, dmr, dab…)
  label: string;            // display name ("Broadcast FM")
  digital?: boolean;        // true for DMR/DStar/DAB/etc. (no SSB sideband concept)
}

export interface BackendCapabilities {
  /** OWRX: true (profile pill). UberSDR/Kiwi: false. */
  profiles: boolean;
  /** UberSDR/Kiwi: true. OWRX: false — client slices the full-profile row. */
  serverSideZoom: boolean;
  /** Where the S-meter comes from: Kiwi SND header | OWRX JSON | spectrum-derived. */
  smeter: 'header' | 'message' | 'derived';
  /** Absolute tunable range in Hz (per active profile on OWRX). */
  freqRange: [number, number];
  /** Kiwi: 15 quantised zoom levels (0–14). Undefined elsewhere. */
  zoomSteps?: number;
  /** UberSDR only. */
  chat: boolean;
  /** UberSDR only. */
  serverNR: boolean;
  /**
   * Per-edge passband half-width cap in Hz, keyed by mode, with a `default`
   * fallback. Drives the bandwidth sliders' range — never hardcode it, since
   * it varies by demodulator and server (e.g. OWRX broadcast FM wants ±96 kHz
   * = 192 kHz wide, while UberSDR narrow modes cap at 6 kHz).
   */
  maxBandwidth: { default: number } & Partial<Record<SDRMode, number>>;
}

/** Per-edge passband half-width cap (Hz) for the active mode. */
export function filterEdgeMax(caps: BackendCapabilities, mode: SDRMode): number {
  return caps.maxBandwidth[mode] ?? caps.maxBandwidth.default;
}

export interface SDRBackend {
  readonly kind: BackendKind;
  readonly caps: BackendCapabilities;
  /** Session id shared with the native audio engine. */
  readonly uuid: string;

  /** `allowServerDefault` — the caller had NO remembered tune for this instance, so the backend
   *  may land on the receiver's own published default instead of the frequency passed here.
   *  Backends that publish no such thing (OWRX profiles, FM-DX) ignore it. Precedence is always
   *  saved tune > server default > our default, so this is only ever set on a first visit. */
  connect(frequency?: number, mode?: SDRMode, opts?: { allowServerDefault?: boolean }): Promise<void>;
  destroy(): void;
  /** Pause-disconnect: close the connection but keep the native audio session
   *  (lock-screen card) intact. OWRX/Kiwi only; UberSDR uses pauseSpectrum. */
  disconnectSocket?(): void;

  /** recenter:true forces a one-shot view recentre even when unlocked (discrete
   *  jumps: freq modal, skip, Siri). Omitted/false = respect the follow flag. */
  tune(frequency: number, mode?: SDRMode, opts?: { recenter?: boolean }): void;
  /** Adopt an externally-confirmed tune (native audio WS echo) without re-sending. */
  syncFrequency(frequency: number, mode?: SDRMode): void;

  /** Locked (true) = view follows the VFO (today's behaviour). Unlocked = the
   *  waterfall pans independently and tune() leaves the view where it is. */
  setFollowMode(follow: boolean): void;

  /** Inclusive Hz range the visible window may pan across with no retune /
   *  profile change. movable=false → hard wall (band edge, OWRX profile span,
   *  Kiwi rx range). movable=true → wall can be pushed by retuning (local
   *  RTL-SDR Fs window). */
  panSpan(): { loHz: number; hiHz: number; movable: boolean };
  setMode(mode: SDRMode): void;
  setBandwidth(low: number, high: number): void;

  zoom(frequency: number, binBandwidth: number): void;
  pan(frequency: number): void;
  resetView(): void;

  setRate(divisor: number): void;
  pauseSpectrum(): void;
  resumeSpectrum(): void;
  /** UberSDR only: the spectrum WS URL (with auth) for the native locked-pocket
   *  forwarder. Absent on adapters whose spectrum isn't a native-openable UberSDR WS. */
  watchSpectrumUrl?(): string;
  /** Rebuild the spectrum socket NOW — a half-open zombie never fires onclose, so
   *  nothing else will. Driven by the client's own starvation watchdog, by the
   *  native network-path monitor, and by the watch when its rows dry up.
   *  UberSDR only; the other adapters have different pause/socket semantics and
   *  their own recovery (see BRIEF-watch-fixes §A7). */
  forceResubscribe?(reason: string): void;

  getStatus(): SDRStatus;
  getView(): SDRStatus;

  // Profiles — present only when caps.profiles (OWRX)
  getProfiles?(): ProfileInfo[];
  selectProfile?(id: string): void;

  /** OWRX: the active secondary decoder id (SSTV/Fax/…) running on top of the
   *  analog carrier, or null. Lets the UI highlight the carrier AND the decoder. */
  getSecondaryDecoder?(): string | null;
  /** OWRX: send a chat message on the main WS (basic text chat, no tune/zoom sync).
   *  name = the user's chosen handle, prefixed to each message server-side. */
  sendChat?(text: string, name: string): void;
  /** OWRX: squelch level in dB (−150 = off/open). */
  setSquelch?(level: number): void;
  /** OWRX: noise reduction — threshold ≤ 0 = off, higher = more NR. */
  setNr?(threshold: number): void;
  /** Kiwi: server-side noise filter / blanker, modelled as DSP descriptors.
   *  setDsp = enable+filter+params; setDspFilter/setDspParams update them. */
  setDsp?(enabled: boolean, filter: string, params: Record<string, string>): void;
  setDspFilter?(filter: string, params: Record<string, string>): void;
  setDspParams?(params: Record<string, string>): void;
  /** DAB: switch the audio service (programme) within the tuned ensemble. */
  setAudioServiceId?(id: number): void;
  /** DAB: speed-correction factor for the dablin chipmunk (1 = off). */
  setDabAudioScale?(scale: number): void;

  /**
   * ★★ Carry an admin credential on every socket this backend opens.
   *
   * ★★★ DECLARED HERE SO THE COMPILER CHECKS THE CALL. SDRScreen called this for weeks as
   *     `(c as any).setAdminAuth?.(…)` against a method that did not exist — and optional chaining
   *     on an `any` is silence, not an error, so an owner unlocking at the radio picker was still
   *     refused by a full radio (2026-08-13). Optional because Kiwi and OWRX have no such concept;
   *     typed, so a rename or a typo fails the build instead of quietly doing nothing.
   */
  setAdminAuth?(q: string): void;

  // ── Broadcast-FM treatments (VibeServer 3.1) ────────────────────────────────────────────────
  // ★★★ DECLARED HERE, AND TYPED, so a rename fails the BUILD. The `(c as any).setAdminAuth?.()`
  //     lesson: an `any` cast plus optional chaining makes a missing method indistinguishable from
  //     a working one — not an error, not a warning, just nothing happening for ever.
  // ★ Optional because only VibeServer has them: a Kiwi, an OWRX or an FM-DX backend has no such
  //   DSP, and the UI hides the controls rather than offering ones that cannot work.
  setWeakProc?(on: boolean): void;
  setIms?(on: boolean): void;
  setCeq?(on: boolean): void;
  setNoiseBlanker?(on: boolean): void;

}

/** A DAB programme (audio service) within the tuned ensemble. */
export interface DabProgramme { id: number; name: string; }

/**
 * Live station metadata decoded by the backend — RDS on broadcast FM and the
 * DAB ensemble/programme labels. Normalised so the UI shows ONE station name
 * uniformly, whether it came from RDS, DAB, or a bookmark match.
 */
export interface StationMeta {
  /** WFM RDS programme service (station) name, or the selected DAB programme. */
  stationName?: string;
  /** WFM RDS radiotext (scrolling now-playing/info), if any. */
  text?: string;
  /** DAB ensemble (multiplex) label. */
  ensemble?: string;
  /** DAB programmes in the ensemble — drives the programme picker. */
  programmes?: DabProgramme[];
  /** Short source tag for the UI badge (e.g. 'RDS', 'DMR') — live server data. */
  badge?: string;
  /** WFM stereo pilot detected (V4 local hardware) — drives the stereo icon. */
  stereo?: boolean;
  /** RDS PI code (hex, e.g. 'C712') — station identity for logo/country lookup. */
  pi?: string;
  /** ISO country (RDS ECC-derived, or receiver-location fallback) — flag + logo. */
  countryIso?: string;
}

/** Additive callbacks for v3 backends — UI ignores when absent. */
export interface BackendCallbacks extends SDRCallbacks {
  /** Direct S-meter reading in dBm (Kiwi: every SND header; OWRX: smeter msgs). */
  onSMeter?: (rssiDbm: number) => void;
  /** OWRX: profile list arrived/changed. */
  onProfiles?: (list: ProfileInfo[]) => void;
  /** OWRX: per-SDR usage, keyed by sdrId (the `sdrId|profileId` prefix). name =
   *  the SDR's display name; inUse = an active source (live user or background
   *  task) is on it (from /status.json polling). Lets the profile picker group
   *  by SDR and badge in-use SDRs so a user doesn't disturb another user. */
  onSdrUsage?: (sdrs: Record<string, { name: string; inUse: boolean; activeProfileId?: string }>) => void;
  /** OWRX: live user count (the WS `clients` broadcast = real connected users,
   *  NOT background tasks). Pairs with onSdrUsage so the user can tell whether an
   *  in-use SDR has real listeners before switching its profile. */
  onClients?: (count: number) => void;
  /** OWRX: an inbound chat message (server broadcasts incl. our own echo). */
  onChatMessage?: (name: string, text: string, color?: string) => void;
  /** OWRX: whether the server has chat enabled (config `allow_chat`). */
  onChatEnabled?: (enabled: boolean) => void;
  /** OWRX: server software name (OpenWebRX / OpenWebRX+) + version, for the menu. */
  onServerInfo?: (info: { name: string; version: string }) => void;
  /** OWRX: server's available demodulators — gate the mode picker to these. */
  onModes?: (list: BackendMode[]) => void;
  /** OWRX: live RDS (FM) / DAB station metadata. Cleared with empty fields. */
  onMetadata?: (meta: StationMeta) => void;
  /** FM-DX: whole-state telemetry per `/text` frame (freq, sig, RDS, txInfo, AF,
   *  users). Drives the tuner screen; supersedes onMetadata for that backend. */
  onFmdxState?: (state: FmdxState) => void;
  /** FM-DX: one-shot server capabilities (antennas, bw switch) from /static_data. */
  onFmdxInfo?: (info: FmdxServerInfo) => void;
  /** UberSDR: a spectrum recovery is in flight — the socket has been torn down and
   *  the first frame on its replacement has not arrived yet. The WATCH needs this:
   *  from the wrist, a phone healing its server link and a phone that has died look
   *  identical, and only the phone knows which it is. */
  onReconnecting?: (busy: boolean) => void;
  /** OWRX: the WS closed UNEXPECTEDLY (server crash/restart — common on OWRX),
   *  as opposed to a user pause/navigation. The UI keeps the session alive and
   *  shows a "server stopped responding" prompt instead of silently dropping. */
  onServerLost?: () => void;
  /** Kiwi: the receiver is full (all channels in use) — `MSG too_busy`. Distinct
   *  from onServerLost so the UI can say "server full, try another". */
  onServerBusy?: () => void;
  /** The receiver's own longitude, if the server advertises it (OWRX /status.json
   *  receiver.gps.lon). Drives the ITU region (MW 9/10 kHz) for custom/OWRX hosts
   *  that don't come through the directory with a known longitude. */
  onReceiverLon?: (lon: number) => void;
  /** Full receiver lat/lon (Kiwi `gps=(lat,lon)`) — drives FT8 spot distances and
   *  the on-device-decoder map for Local/Kiwi. */
  onReceiverLoc?: (lat: number, lon: number) => void;
  /** OWRX: server bookmarks + dial-frequency markers arrive over the WS (no REST
   *  endpoint like UberSDR). Feeds the VTS station readout + the search bar. */
  onBookmarks?: (list: { name: string; frequency: number; mode?: string; repeater?: boolean }[]) => void;
  /** OWRX: server-side secondary-decoder image output (SSTV/Fax). The server
   *  decodes and streams scanlines; the UI just paints them on the decoder
   *  canvas — SSTV pixels are RGB triplets, Fax pixels are greyscale bytes. */
  onDecoderImage?: (ev:
    | { phase: 'start'; kind: 'sstv' | 'fax'; width: number; height: number }
    | { phase: 'line';  kind: 'sstv' | 'fax'; line: number; width: number; pixels: Uint8Array }
    | { phase: 'done';  kind: 'sstv' | 'fax' }) => void;
  /** OWRX: server-side TEXT-decoder output (Packet/POCSAG/FLEX/DSC/ISM/HFDL/
   *  ACARS/ADSB/WSJT…), one formatted line per record. `replace` true = a live
   *  snapshot (ADS-B list) that supersedes the previous block rather than appends. */
  onDecoderText?: (line: string, replace?: boolean) => void;

  /** OWRX ADS-B: the live aircraft table, STRUCTURED.
   *
   *  The same records also become a text blob for the decoder panel — but flattening
   *  them to text on arrival is why nothing else in the app could ever use them. The
   *  server sends a rich record (registration country, altitude, speed, vertical
   *  rate, position, squawk); keep it. */
  onAircraft?: (list: Aircraft[]) => void;
  /** OWRX: the server/profile's own preset DSP defaults (config `initial_squelch_level`
   *  / `initial_nr_level`), pushed on connect and every profile switch. The adapter
   *  has already applied them to the demod; this just lets the UI sliders reflect the
   *  server's preset (e.g. an NFM 2 m profile that ships with a −65 dB squelch). A
   *  field is omitted when the server doesn't preset it. squelchDb −150 = off;
   *  nrThreshold 0 (with nrEnabled false) = NR off. */
  onServerDspDefaults?: (d: { squelchDb?: number; nrEnabled?: boolean; nrThreshold?: number }) => void;
}

export type { SDRStatus, SDRMode, SDRCallbacks };
