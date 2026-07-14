/**
 * SDRScreen — main receiver screen for VibeSDR v2.
 *
 * Hierarchy:
 *   SDRScreen
 *   ├── WaterfallView         (GPU Skia waterfall + spectrum, fills full screen)
 *   ├── ControlsBar           (drums, sig-frame, freq/mode pill, step, menu — absolute overlay)
 *   ├── MenuSheet             (slide-up panel)
 *   ├── StepPicker            (bottom-sheet step selector)
 *   ├── ModeSelector          (bottom-sheet demodulator selector)
 *   ├── FreqModal             (numpad frequency entry)
 *   ├── ChatDrawer            (slide-up chat)
 *   ├── DecoderPanel          (floating above pill)
 *   └── AudioPlayer           (renderless; plays Opus stream)
 */

import React, {
  useCallback, useEffect, useMemo, useRef, useState,
} from 'react';
import {
  Alert,
  AppState,
  BackHandler,
  ActivityIndicator,
  Dimensions,
  NativeEventEmitter,
  NativeModules,
  Platform,
  Pressable,
  Share,
  StatusBar,
  StyleSheet,
  Text,
  TouchableOpacity,
  View,
} from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { useKeepAwake }       from 'expo-keep-awake';
import type { NativeStackScreenProps } from '@react-navigation/native-stack';
import type { RootStackParamList }     from '../../App';
import { splashBridge }                 from '../../App';

import { MODE_BANDWIDTHS, type SDRStatus, type SDRMode } from '../services/UberSDRClient';
import { buildShareLink } from '../linking/DeepLinkHandler';
import { createBackend } from '../services/UberSDRAdapter';
import { KiwiAdapter } from '../services/KiwiAdapter';
import { localSessionGen } from '../services/localSession';
import { startBookmarkAutosave, stopBookmarkAutosave,
         getLearnedBookmarksNow } from '../services/vibeServer';
import { setReceiverIso } from '../services/rdsCountry';
import { watchProvider } from '../services/watchProvider';

/** FFT rate divisor while the phone is backgrounded but the watch is watching.
 *
 *  ONE — i.e. don't throttle at all.
 *
 *  Half rate looked like free battery: the watch only draws ~10fps, so why send
 *  20? Because a 10fps SOURCE cannot reliably yield 10fps of ROWS. Every frame
 *  then has to survive the send gate, the JS thread's background scheduling and
 *  WCSession's own jitter — and iOS throttles a backgrounded JS thread, so frames
 *  slip. Miss one and the next row is 200ms late. The result was a ragged feed
 *  full of holes, which the jitter buffer and the trace's EMA smoothed over: on
 *  the wrist it read as the averaging being cranked right up the moment the phone
 *  locked.
 *
 *  A 20fps source gives the gate a frame to choose from whenever it opens, so the
 *  watch gets a STEADY 10fps locked or awake. Headroom is what buys steadiness
 *  here; the frames we drop cost nothing, and the ones we keep are on time. */
const WATCH_BG_DIVISOR = 1;
import { filterEdgeMax, type SDRBackend, type ProfileInfo, type BackendMode, type DabProgramme, type Aircraft } from '../services/SDRBackend';
import { DecoderClient, RTTY_PRESETS,
         type RttySettings, type MorseQuality,
         type SpotRow, type SpotsKind,
         type ChatUserRow }                            from '../services/DecoderClient';
import { type DecoderImageHandle }                     from '../components/DecoderImageCanvas';
import { MIN_HZ, MAX_HZ, STEPS, stepsForFreq }         from '../services/sdrTypes';
import { v4 as uuidv4 }                                from 'uuid';
import AsyncStorage                                    from '@react-native-async-storage/async-storage';
import { setDefaultInstance, getDefaultInstance,
         clearDefaultInstance }                        from '../services/defaultInstance';
import { getFavourites, toggleFavourite }              from '../services/favourites';
import { useTheme }                                     from '../contexts/ThemeContext';

import WaterfallView   from '../components/WaterfallView';
import ControlsBar, { createMeterBus, meterText } from '../components/ControlsBar';
import { setDrumHaptics } from '../components/DrumWheel';
import MenuSheet, { type DspFilterDesc } from '../components/MenuSheet';
import { useCoachmarkTour, tourRef } from '../components/Coachmark';
import AudioPlayer, { VibePowerModule } from '../components/AudioPlayer';
import LocalAudioPlayer from '../components/LocalAudioPlayer';
import LocalHardwarePanel from '../components/LocalHardwarePanel';
import FreqModal       from '../components/FreqModal';
import ModeSelector    from '../components/ModeSelector';
import AudioSheet      from '../components/AudioSheet';
import StepPicker      from '../components/StepPicker';
import ChatDrawer,
  { type ChatMessage } from '../components/ChatDrawer';
import DecoderPanel,
  { type DecoderType } from '../components/DecoderPanel';
import SpecRatioOverlay  from '../components/SpecRatioOverlay';
import MapOverlay, { type MapKind } from '../components/MapOverlay';
import CityPickerModal from '../components/CityPickerModal';
import BrowserOverlay from '../components/BrowserOverlay';
import AboutOverlay from '../components/AboutOverlay';
import RecordingsOverlay from '../components/RecordingsOverlay';
import VTSBar, { type VtsNotifData } from '../components/VTSBar';
import { resolveStationLogo } from '../services/stationLogoCache';
import { tidyStationName } from '../services/stationLogo';
import { isWholeProfileMode } from '../services/dataModes';
import { isoToFlag, validIso } from '../services/rdsCountry';
import CenterVfoButton from '../components/CenterVfoButton';
import PasswordModal from '../components/PasswordModal';
import {
  fetchBookmarks, findNearest, findNextBookmark,
  fmtBandFreq, deriveItuRegion, refreshBandSnr, getBandSnrDb, propCondition,
  fetchUiConfig, fetchReceiverInfo,
  VTS_ON_HZ, searchStations, type ServerBookmark, type ServerBand,
  type ServerUiConfig, type ReceiverInfo,
} from '../services/stations';
import {
  loadUserBookmarks, saveUserBookmarks, bookmarksForInstance, withoutInstance,
  exportBookmarksJSON, parseBookmarksAny, mergeBookmarks, type UserBookmark,
} from '../services/userBookmarks';
import { getBandsAtRegion, bandTuneDefaults, BAND_PLAN, type Band } from '../constants/bandPlan';
import { fmNowPlaying } from '../services/nowPlaying';
import CarFmFace, { type CarFmPreset } from '../components/CarFmFace';
import { loadActiveEibi } from '../services/eibi';
import { getUserLocation } from '../services/instancesApi';
import { distanceKmToGrid } from '../services/grid';
import { countryForCallsign } from '../services/callsignCountry';
import * as DocumentPicker from 'expo-document-picker';
// SDK 56 moved readAsStringAsync to the legacy entry (new File API otherwise).
import * as FileSystem from 'expo-file-system/legacy';

// ── Constants ──────────────────────────────────────────────────────────────────

// Drum sensitivity (skin SENS_TABLE parity): px of travel per tune step /
// zoom octave. PRECISE doubles travel for everything (22→44, 40→77 ≈ skin's
// zoom 30→58 ratio).
const DRUM_SENS = {
  normal:  { vfo: 22, zoomOctave: 40 },
  precise: { vfo: 44, zoomOctave: 77 },
};
// Velocity-adaptive VFO (beyond skin): a slow deliberate thumb gets up to
// FINE_MULT× more travel per step (fine tuning), a fast spin stays at 1×.
// Mapped continuously, so decelerating onto a signal gains precision mid-drag.
const VFO_FINE_MULT  = 4;    // sensitivity multiplier at the slow end
const VFO_VEL_FINE   = 40;   // px/s and below → fully fine
const VFO_VEL_FAST   = 350;  // px/s and above → full speed

// SNR-bar compression — the skin's sigNorm curve (30/60@0.8/80) shifted down
// 30dB: upstream UberSDR's S-meter reads radiod's raw audio-stream SNR which
// FLOORS at ~30dB with no signal (madpsy/ka9q_ubersdr#77); ours comes from
// spectrum bins (the correct source), so no-signal ≈ 0-5dB. Same shape: 30dB
// of span to the knee at 0.8 fill, top fifth compressed for 45-55dB monsters.
const SIG_FLOOR = 5, SIG_KNEE = 35, SIG_KNEE_FILL = 0.8, SIG_CEIL = 55;
function sigNorm(v: number): number {
  if (v <= SIG_FLOOR) return 0;
  if (v >= SIG_CEIL)  return 1;
  if (v <= SIG_KNEE)
    return SIG_KNEE_FILL * (v - SIG_FLOOR) / (SIG_KNEE - SIG_FLOOR);
  return SIG_KNEE_FILL +
         (1 - SIG_KNEE_FILL) * (v - SIG_KNEE) / (SIG_CEIL - SIG_KNEE);
}


// ── Types ──────────────────────────────────────────────────────────────────────

type Props = NativeStackScreenProps<RootStackParamList, 'SDR'>;

// ── Helpers ───────────────────────────────────────────────────────────────────

function nowUTCStr() {
  const n = new Date();
  return String(n.getUTCHours()).padStart(2,'0') + String(n.getUTCMinutes()).padStart(2,'0') + 'z';
}
let _msgId = 0;
function mkMsg(type: ChatMessage['type'], text: string, user?: string): ChatMessage {
  return { id: String(++_msgId), type, text, user, ts: nowUTCStr() };
}

// ── Voice (Siri) query resolution ───────────────────────────────────────────
// Parse a spoken frequency: "7150", "7.15 MHz", "648 khz", "7150 usb". Bare
// numbers ≥ 30 are kHz (7150 → 7150 kHz), < 30 are MHz (7.15 / 14 → MHz).
function parseVoiceFreq(q: string): { hz: number; mode?: string } | null {
  const lower = q.toLowerCase();
  const modeM = lower.match(/\b(usb|lsb|sam|am|nfm|fm|cwu|cwl|cw)\b/);
  const mode  = modeM ? (modeM[1] === 'cw' ? 'cwu' : modeM[1]) : undefined;
  // NB: only spell out units (mhz/khz/hz). A bare "m"/"k" is NOT treated as a
  // unit — "20m"/"40m" are ham *bands* (metres), not 20 MHz; band routing below.
  const numM  = lower.match(/(\d+(?:[.,]\d+)?)\s*(mhz|khz|hz)?/);
  if (!numM) return null;
  const n = parseFloat(numM[1].replace(',', '.'));
  if (Number.isNaN(n)) return null;
  const unit = numM[2];
  let hz: number;
  if (unit === 'mhz')      hz = n * 1e6;
  else if (unit === 'khz') hz = n * 1e3;
  else if (unit === 'hz')                  hz = n;
  else                                     hz = n < 30 ? n * 1e6 : n * 1e3;
  hz = Math.round(hz);
  if (hz < MIN_HZ || hz > MAX_HZ) return null;
  return { hz, mode };
}

/** Resolve a spoken query to a tune. Frequency first, else the bookmark/band
 *  search (reusing searchStations). Band synonyms (amateur/voice → ham) are
 *  normalised so "40m ham", "40m amateur", "40m voice" all hit the 40m band. */
function resolveVoiceQuery(
  q: string, bms: ServerBookmark[], bands: ServerBand[],
): { hz: number; mode: string | null; isBand: boolean } | null {
  const lower = q.toLowerCase();
  // "N metre" amateur band — match the band by its wavelength label directly,
  // ahead of the fuzzy search. This is robust to Siri mishearing "ham" as "hand"
  // (we key off the number) and stops a "20 MHz" bookmark like WWV from winning
  // over the 20m band (bookmarks otherwise always rank before bands). Excludes a
  // bare "20 MHz"/"20 mhz" (the m must be metres, not a MHz unit prefix).
  // A spoken mode anywhere in the phrase ("7150 lower sideband", "40m upper
  // side band") — parseVoiceMode knows every synonym; it overrides band defaults.
  const spokenMode = parseVoiceMode(q);
  // CB / "citizens band" — the server's band label is usually just "11m", so the
  // word "CB" alone finds nothing. Map it onto the 11m band (by label or the
  // 26.9–27.4 MHz range) so "CB band" and "11m band" both work.
  if (/\b(c\.?\s?b\.?|citizens?\s*band)\b/i.test(q)) {
    const band = bands.find((b) => {
      const l = (b.label || '').toLowerCase();
      return l.includes('cb') || l.includes('11m') ||
             ((b.start || 0) <= 27000000 && (b.end || 0) >= 26960000);
    });
    if (band) return { hz: band.start, mode: spokenMode ?? band.mode ?? null, isBand: true };
  }
  // "N metre" amateur band — match the band by its wavelength label directly,
  // ahead of the fuzzy search. This is robust to Siri mishearing "ham" as "hand"
  // (we key off the number) and stops a "20 MHz" bookmark like WWV from winning
  // over the 20m band (bookmarks otherwise always rank before bands). Excludes a
  // bare "20 MHz"/"20 mhz" (the m must be metres, not a MHz unit prefix).
  const meterM = lower.match(/\b(\d{1,4})\s*m(?:et(?:er|re)s?)?\b/);
  if (meterM && !/\b\d+\s*mhz\b/.test(lower)) {
    const n = meterM[1];
    const band = bands.find((b) => {
      const l = (b.label || '').toLowerCase().replace(/\s+/g, '');
      return l === `${n}m` || l.startsWith(`${n}m`);
    });
    if (band) return { hz: band.start, mode: spokenMode ?? band.mode ?? null, isBand: true };
  }
  const norm = q.replace(/\b(amateur|voice|ssb|phone)\b/gi, 'ham');
  const bandSearch = () => {
    const res = searchStations(bms, bands, norm, 1);
    if (!res.length) return null;
    const r = res[0];
    if (r.isBand && r.band) return { hz: r.band.start, mode: spokenMode ?? r.band.mode ?? null, isBand: true };
    if (r.bm)               return { hz: r.bm.frequency, mode: spokenMode ?? r.bm.mode ?? null, isBand: false };
    return null;
  };
  // Only treat the phrase as a numeric tune when it's *purely* a frequency:
  // a number + optional unit + optional mode words, nothing else. Strip the freq
  // units AND every spoken-mode phrase — crucially WITHOUT word boundaries and
  // with \s* between words, so Siri's mashed forms ("lowersideband", "side band")
  // are all removed; otherwise "7150 lowersideband" keeps letters, gets routed to
  // the band search, and 7150 (inside 40m) snaps to the band start 7000. If any
  // letters survive it's a name ("BBC Radio 5", "20m ham band") → search first.
  const residue = lower
    // Remove numbers AND any trailing unit first — even glued ("909000hz",
    // "7150khz"), which a \b-anchored unit strip would miss.
    .replace(/\d+(?:[.,]\d+)?\s*(?:mhz|khz|hz)?/g, ' ')
    .replace(/(?:upper|lower)\s*side\s*band|side\s*band|synchron\w*(?:\s*(?:a\.?m|amplitude))?|amplitude\s*modulation|frequency\s*modulation|narrow\s*f\.?m|continuous\s*wave|\b(?:usb|lsb|sam|am|nfm|fm|cwu|cwl|cw|morse)\b/g, ' ')
    .replace(/[^a-z]/g, '');
  if (residue.length === 0) {
    const f = parseVoiceFreq(q);
    if (f) return { hz: f.hz, mode: spokenMode ?? f.mode ?? null, isBand: false };
    return bandSearch();
  }
  const b = bandSearch();
  if (b) return b;
  const f = parseVoiceFreq(q);
  return f ? { hz: f.hz, mode: spokenMode ?? f.mode ?? null, isBand: false } : null;
}

/** Spoken demodulator with synonyms → SDRMode. "synchronous AM"/"SAM"→sam,
 *  "lower side band"/"LSB"→lsb, "amplitude modulation"/"AM"→am, etc. Ordered so
 *  the specific phrases win (sam before am, nfm before fm, sideband before am). */
function parseVoiceMode(q: string): SDRMode | null {
  const s = q.toLowerCase();
  const map: [RegExp, SDRMode][] = [
    [/synchron\w*\s*(a\.?m|amplitude)|\bsync\s*am\b|\bsam\b/, 'sam'],
    [/upper\s*side\s*?band|\busb\b/, 'usb'],
    [/lower\s*side\s*?band|\blsb\b/, 'lsb'],
    [/narrow\w*\s*(f\.?m|frequency)|\bnfm\b/, 'nfm'],
    [/frequency\s*modulation|\bf\.?m\b/, 'fm'],
    [/amplitude\s*modulation|\ba\.?m\b/, 'am'],
    [/\bcwl\b|cw\s*lower/, 'cwl'],
    [/\bcwu\b|cw\s*upper|\bcw\b|morse|continuous\s*wave/, 'cwu'],
  ];
  for (const [re, m] of map) if (re.test(s) && m in MODE_BANDWIDTHS) return m as SDRMode;
  return null;
}

/** Spoken step → nearest supported step (Hz). "100Hz"→100, "1kHz"/"1000"→1000. */
function parseVoiceStep(q: string): number | null {
  const f = parseVoiceFreq(q.toLowerCase().replace(/\bstep\b/g, ''));
  // parseVoiceFreq clamps to the tuning range; a bare "100"/"500" wouldn't pass,
  // so parse a plain Hz/kHz value here too.
  let hz: number | null = null;
  const m = q.toLowerCase().match(/(\d+(?:[.,]\d+)?)\s*(khz|hz|k)?/);
  if (m) {
    const n = parseFloat(m[1].replace(',', '.'));
    if (!Number.isNaN(n)) hz = (m[2] === 'khz' || m[2] === 'k') ? n * 1000 : n;
  }
  if (hz == null && f) hz = f.hz;
  if (hz == null) return null;
  // snap to the nearest supported step
  let best = STEPS[0], bestD = Infinity;
  for (const s of STEPS) { const d = Math.abs(s - hz); if (d < bestD) { bestD = d; best = s; } }
  return best;
}

/** RFC3339 server timestamp → "HHMMz" (falls back to now) */
function chatTs(rfc: string): string {
  const d = rfc ? new Date(rfc) : new Date();
  if (isNaN(d.getTime())) return nowUTCStr();
  return `${String(d.getUTCHours()).padStart(2, '0')}${String(d.getUTCMinutes()).padStart(2, '0')}z`;
}

// ── Component ──────────────────────────────────────────────────────────────────


// V4 local hardware (RTL-SDR) demodulator list — includes WFM (broadcast FM),
// which HF UberSDR servers don't offer.
// SAM omitted — the on-device DSP (VibeDSP) has no synchronous-AM demodulator yet.
const LOCAL_MODES: { id: string; label: string }[] = [
  { id: 'wfm', label: 'WFM' }, { id: 'nfm', label: 'NFM' }, { id: 'am', label: 'AM' },
  { id: 'cwu', label: 'CW' },
  // LSB + USB last so they're the two large bottom buttons (the SSB pair),
  // with USB as the final option (sits below LSB in the grid).
  { id: 'lsb', label: 'LSB' }, { id: 'usb', label: 'USB' },
];

export default function SDRScreen({ route, navigation }: Props) {
  const { baseUrl, instanceName, password } = route.params;
  useKeepAwake();

  // V4 local hardware: tear down the on-device shim (closes the RTL-SDR + the
  // localhost server) when leaving the screen — BUT only if this is still the
  // latest local session. The shim is a singleton; when switching instances a new
  // session may already be running by the time this stale screen unmounts, and an
  // unguarded stopSpectrum() would kill it (V5's fast native start re-exposed this).
  const myLocalGen = useRef(route.params.localGen ?? 0).current;
  useEffect(() => {
    if (!route.params.isLocal) return;
    return () => {
      if (localSessionGen() === myLocalGen) (NativeModules as any).VibeLocalSDR?.stopSpectrum?.();
    };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // ── V4 local hardware controls (RTL-SDR) ──────────────────────────────────
  const isLocal = !!route.params.isLocal;
  // rtl_tcp network health, polled from the shim's jitter buffer. 3 = good (also the
  // resting value on the USB path, where it never clamps anything).
  const netLinkRef = useRef<0|1|2|3>(3);
  // SpyServer with canControl=0: another client holds the tuner, so tuning would
  // silently do nothing. Show it rather than letting the user fight a dead dial.
  const [readOnly, setReadOnly] = useState(false);
  // True when the local session's IQ comes from a SpyServer: most RTL-specific
  // hardware controls then belong to the server operator, not us.
  const [isSpy, setIsSpy] = useState(false);
  // Session limit (minutes) from the directory. The server enforces it; we just
  // warn up front and count down, rather than letting it look like a crash.
  const sessionLimitMins: number = route.params.sessionLimitMins ?? 0;
  const [sessionEndsAt, setSessionEndsAt] = useState<number | null>(null);
  const [sessionLeftMs, setSessionLeftMs] = useState<number | null>(null);
  const noticeShownRef = useRef(false);
  // Per-device persistence suffix so each local source keeps its OWN remembered
  // setup (frequency/mode/step + hardware config). RTL-TCP is keyed by host:port,
  // so UberSDR-over-RTL-TCP and a real-hardware RTL-TCP server never share state;
  // a USB dongle uses ':usb'. The old single 'lsv_local_hw' / ':local' keys let
  // every local device clobber each other — and could restore an out-of-band
  // frequency (e.g. 96.6 MHz WFM onto an HF-only UberSDR RTL-TCP).
  const localDeviceKey = route.params.isTcp
    ? `tcp:${route.params.tcpHost ?? ''}:${route.params.tcpPort ?? ''}`
    : 'usb';
  const localHwKey = `lsv_local_hw:${localDeviceKey}`;
  const LocalHw = (NativeModules as any).VibeLocalSDR;
  const [hwOpen,        setHwOpen]        = useState(false);
  const [hwGains,       setHwGains]       = useState<number[]>([]);
  const [hwServerRates, setHwServerRates] = useState<number[] | null>(null);  // VibeServer-offered rates
  const [hwGain,        setHwGain]        = useState(0);     // tenths of dB
  const [hwAutoGain,    setHwAutoGain]    = useState(true);
  const [hwPpm,         setHwPpm]         = useState(0);
  const [hwSampleRate,  setHwSampleRate]  = useState(2_400_000);
  const [hwBiasTee,     setHwBiasTee]     = useState(false);
  const [hwAgc,         setHwAgc]         = useState(false);
  const [hwDirectSamp,  setHwDirectSamp]  = useState(0);
  const [hwDeemph,      setHwDeemph]      = useState(50e-6);  // FM de-emphasis tau (0/50µs/75µs)
  const [hwStereo,      setHwStereo]      = useState(true);   // WFM stereo on / forced mono (local)
  const [hwSquelch,     setHwSquelch]     = useState(-100);   // audio squelch dBFS (-100 = off)
  const [hwNrLevel,     setHwNrLevel]     = useState(0);      // audio NR strength 0=off..20 (÷15 → native 0..1.33)
  const [hwNotch,       setHwNotch]       = useState(false);  // auto notch — LOCAL (shim)
  const [netNotch,      setNetNotch]      = useState(false);  // auto notch — NETWORK (UberSDR/OWRX/Kiwi)

  // Load saved RTL-SDR hardware settings and apply them to the running session,
  // so gain/bias-T/PPM/etc. persist across connections.
  const hwLoaded = useRef(false);
  useEffect(() => {
    if (!isLocal) return;
    let cancelled = false;
    (async () => {
      let prefs: any = {};
      try {
        // Per-device key first; migrate the old global blob on first connect so a
        // single existing dongle keeps its gain/rate/etc.
        let j = await AsyncStorage.getItem(localHwKey);
        if (j == null) j = await AsyncStorage.getItem('lsv_local_hw');
        if (j) prefs = JSON.parse(j);
      } catch {}
      if (cancelled) return;
      const auto = prefs.autoGain ?? true;
      const ppm  = typeof prefs.ppm === 'number' ? prefs.ppm : 0;
      let rate = typeof prefs.sampleRate === 'number' ? prefs.sampleRate : 2_400_000;
      // Local USB needs >=1 MHz (a dongle is sluggish/underfiltered lower); only
      // RTL-TCP may sit low. Clamp a stale/low saved rate for USB.
      if (!route.params.isTcp && rate < 1_000_000) rate = 2_400_000;
      const bias = !!prefs.biasTee;
      const agc  = !!prefs.agc;
      const ds   = typeof prefs.directSampling === 'number' ? prefs.directSampling : 0;
      const deemph = typeof prefs.deemph === 'number' ? prefs.deemph : 50e-6;
      const stereo = prefs.stereo !== false;   // default on
      // Squelch / NR / Notch are session-scoped DSP — NEVER restored, so a new
      // connection always starts clean (no surprise muted/“funny” audio carried
      // over from a previous session). Device config (gain/ppm/etc.) still persists.
      const sql = -100, nrLvl = 0, notch = false;
      setHwAutoGain(auto); setHwPpm(ppm); setHwSampleRate(rate);
      setHwBiasTee(bias); setHwAgc(agc); setHwDirectSamp(ds); setHwDeemph(deemph); setHwStereo(stereo); setHwSquelch(sql); setHwNrLevel(nrLvl); setHwNotch(notch);
      if (typeof prefs.gain === 'number') setHwGain(prefs.gain);
      // Re-apply to the native session (already running from startSpectrum).
      LocalHw?.setPpm?.(ppm);
      LocalHw?.setBiasTee?.(bias);
      LocalHw?.setAgc?.(agc);
      LocalHw?.setDirectSampling?.(ds);
      LocalHw?.setDeemphasis?.(deemph);
      LocalHw?.setStereoEnabled?.(stereo);
      LocalHw?.setSquelch?.(sql > -100, sql);
      LocalHw?.setNrStrength?.(nrLvl / 15);
      LocalHw?.setNR?.(nrLvl > 0);
      LocalHw?.setNotch?.(notch);
      if (rate !== 2_400_000) LocalHw?.setSampleRate?.(rate);
      LocalHw?.setGain?.(auto ? -1 : (typeof prefs.gain === 'number' ? prefs.gain : 0));
      try {
        const g = await LocalHw?.getTunerGains?.();
        if (!cancelled && Array.isArray(g) && g.length) {
          setHwGains(g);
          if (typeof prefs.gain !== 'number') setHwGain(g[Math.floor(g.length / 2)]);
        }
      } catch {}
      hwLoaded.current = true;
    })();
    return () => { cancelled = true; };
  }, [isLocal, LocalHw, localHwKey]);

  // Background-restriction nudge (local hardware only). Aggressive OEMs
  // (Motorola/Lenovo, some others) ship apps "Restricted" by default, which makes
  // Android strip our mediaPlayback foreground service in the background → the
  // process is demoted to a cached/little-core state → the local-SDR DSP thread
  // starves the audio writer → background audio breaks up. We can't clear the
  // restriction programmatically (user-only), so detect it once per session and
  // point the user at the Settings toggle. Shown at most once (until they act or
  // permanently dismiss). Network backends don't need this — only local hardware
  // runs a heavy in-process DSP thread that the demotion starves.
  useEffect(() => {
    if (!isLocal || !LocalHw?.isBackgroundRestricted) return;
    let cancelled = false;
    (async () => {
      try {
        const restricted = await LocalHw.isBackgroundRestricted();
        if (cancelled) return;
        if (!restricted) {
          // Not restricted → re-arm the prompt. If the OS later re-restricts the
          // app (an OEM battery-manager clamp, a system update, etc.), we want to
          // warn again even if the user previously tapped "Don't ask again" — that
          // dismissal only suppresses the CURRENT restricted episode, not forever.
          AsyncStorage.removeItem('lsv_bg_restrict_dismissed_v1').catch(() => {});
          return;
        }
        if ((await AsyncStorage.getItem('lsv_bg_restrict_dismissed_v1')) === '1') return;
        Alert.alert(
          'Allow background audio',
          "This device restricts VibeSDR when it isn't on screen, which breaks up audio in the background.\n\n" +
          "To fix it:\n" +
          "1. Tap “Open Settings” below.\n" +
          "2. Open “App battery usage” (or “Battery”) and turn ON “Allow background usage” (some phones instead call it “Unrestricted” / “Don't optimise”).\n" +
          "3. Then fully close VibeSDR (swipe it away from the recent-apps list) and open it again so the change takes effect.",
          [
            { text: 'Not now', style: 'cancel' },
            { text: "Don't ask again", style: 'destructive',
              onPress: () => { AsyncStorage.setItem('lsv_bg_restrict_dismissed_v1', '1').catch(() => {}); } },
            { text: 'Open Settings', onPress: () => { LocalHw?.openAppSettings?.(); } },
          ],
        );
      } catch {}
    })();
    return () => { cancelled = true; };
  }, [isLocal, LocalHw]);

  // Persist hardware settings whenever they change (after the initial load).
  useEffect(() => {
    if (!isLocal || !hwLoaded.current) return;
    AsyncStorage.setItem(localHwKey, JSON.stringify({
      autoGain: hwAutoGain, gain: hwGain, ppm: hwPpm, sampleRate: hwSampleRate,
      biasTee: hwBiasTee, agc: hwAgc, directSampling: hwDirectSamp, deemph: hwDeemph, stereo: hwStereo,
    })).catch(() => {});
    // NB: squelch / nrLevel / notch are intentionally NOT saved (session-scoped).
  }, [isLocal, localHwKey, hwAutoGain, hwGain, hwPpm, hwSampleRate, hwBiasTee, hwAgc, hwDirectSamp, hwDeemph, hwStereo]);

  // VibeServer (remote shim): hardware controls ride the WS to the serving device
  // instead of the (non-existent) local dongle. localHost set = remote session.
  const isRemoteShim = isLocal && !!route.params.localHost;

  // Tell the RDS decoder where the RECEIVER is, so it can VALIDATE a station's PI
  // country nibble instead of the app inventing a country. It has to be the ANTENNA's
  // country: a phone in London listening to a German UberSDR hears German stations, so
  // the phone's own locale would be actively wrong. Blank when we don't know, which
  // just falls back to ECC-only (i.e. the old behaviour) rather than to a bad guess.
  useEffect(() => {
    let cancelled = false;
    (async () => {
      if (isRemoteShim && route.params.localHost) {
        // A remote VibeServer publishes its own location, including the country.
        try {
          const r = await fetch(`http://${route.params.localHost}:${route.params.localPort}/location`);
          const j = await r.json();
          if (!cancelled) setReceiverIso(typeof j?.iso === 'string' ? j.iso : '');
        } catch { if (!cancelled) setReceiverIso(''); }
        return;
      }
      if (isLocal) {
        // The dongle is on THIS device, so this device's region is the aerial's region.
        try {
          const loc = Intl.DateTimeFormat().resolvedOptions().locale || '';
          const region = loc.split('-')[1] || '';
          if (!cancelled) setReceiverIso(/^[A-Za-z]{2}$/.test(region) ? region : '');
        } catch { if (!cancelled) setReceiverIso(''); }
        return;
      }
      if (!cancelled) setReceiverIso('');   // network instance: we don't know where it is
    })();
    return () => { cancelled = true; setReceiverIso(''); };
  }, [isLocal, isRemoteShim]);

  // The shim learns station names from RDS whenever it runs — serving OR listening —
  // but it has no storage, so something has to write the list down. On a REMOTE shim
  // (VibeServer) the SERVING phone owns that; here we only do it for a shim running
  // on THIS device.
  useEffect(() => {
    if (!isLocal || isRemoteShim) return;
    startBookmarkAutosave();
    return () => stopBookmarkAutosave();
  }, [isLocal, isRemoteShim]);
  const hwClient = useCallback(() => (isRemoteShim
    ? (client.current as {
        setHwGain?: (t: number, a: boolean) => void; setHwBiasT?: (on: boolean) => void;
        setHwAgc?: (on: boolean) => void; setHwPpm?: (n: number) => void;
        setHwSampleRate?: (r: number) => void;
      } | null)
    : null), [isRemoteShim]);

  const onHwAuto = useCallback((auto: boolean) => {
    setHwAutoGain(auto);
    const rc = hwClient();
    if (rc) rc.setHwGain?.(hwGain, auto); else LocalHw?.setGain?.(auto ? -1 : hwGain);
  }, [LocalHw, hwGain, hwClient]);
  // Gain reaches the dongle as a USB CONTROL TRANSFER, on the same bus carrying the
  // bulk IQ stream — so a slider drag firing one per step (~10 in 200ms) elbows the
  // sample flow aside, and you hear it as breakup while you drag. Coalesce to one
  // per 120ms, trailing edge always delivered so the gain you release on is the gain
  // the radio ends up at.
  const gainSendAt = useRef(0);
  const gainTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const gainPending = useRef<number | null>(null);
  const flushGain = useCallback(() => {
    const tenthDb = gainPending.current;
    if (tenthDb == null) return;
    gainPending.current = null;
    gainSendAt.current = Date.now();
    const rc = hwClient();
    if (rc) rc.setHwGain?.(tenthDb, false); else LocalHw?.setGain?.(tenthDb);
  }, [LocalHw, hwClient]);
  const onHwGain = useCallback((tenthDb: number) => {
    setHwAutoGain(false); setHwGain(tenthDb);
    gainPending.current = tenthDb;
    const wait = gainSendAt.current + 120 - Date.now();
    if (wait <= 0) { flushGain(); return; }
    if (!gainTimer.current) {
      gainTimer.current = setTimeout(() => { gainTimer.current = null; flushGain(); }, wait);
    }
  }, [flushGain]);
  const onHwPpm = useCallback((ppm: number) => {
    const v = Math.max(-200, Math.min(200, ppm)); setHwPpm(v);
    const rc = hwClient();
    if (rc) rc.setHwPpm?.(v); else LocalHw?.setPpm?.(v);
  }, [LocalHw, hwClient]);
  const onHwSampleRate = useCallback((rate: number) => {
    setHwSampleRate(rate);
    const rc = hwClient();
    // VibeServer: ask the server to change its capture rate (= the spectrum span
    // it sends). Useful to ease a struggling remote link without touching the host.
    if (rc) rc.setHwSampleRate?.(rate); else LocalHw?.setSampleRate?.(rate);
  }, [LocalHw, hwClient]);
  const onHwBiasTee = useCallback((on: boolean) => {
    setHwBiasTee(on);
    const rc = hwClient();
    if (rc) rc.setHwBiasT?.(on); else LocalHw?.setBiasTee?.(on);
  }, [LocalHw, hwClient]);
  const onHwAgc = useCallback((on: boolean) => {
    setHwAgc(on);
    const rc = hwClient();
    if (rc) rc.setHwAgc?.(on); else LocalHw?.setAgc?.(on);
  }, [LocalHw, hwClient]);
  const onHwDirectSamp = useCallback((mode: number) => { setHwDirectSamp(mode); LocalHw?.setDirectSampling?.(mode); }, [LocalHw]);
  const onHwDeemph = useCallback((tau: number) => { setHwDeemph(tau); LocalHw?.setDeemphasis?.(tau); }, [LocalHw]);
  const onHwStereo = useCallback((on: boolean) => { setHwStereo(on); LocalHw?.setStereoEnabled?.(on); }, [LocalHw]);
  const onLocalSquelch = useCallback((db: number) => {
    setHwSquelch(db); LocalHw?.setSquelch?.(db > -100, db);
  }, [LocalHw]);
  const onLocalNR = useCallback((level: number) => {
    setHwNrLevel(level);
    LocalHw?.setNrStrength?.(level / 15);
    LocalHw?.setNR?.(level > 0);
  }, [LocalHw]);
  const onLocalNotch = useCallback((on: boolean) => {
    setHwNotch(on); LocalHw?.setNotch?.(on);
  }, [LocalHw]);
  // Network auto notch (UberSDR/OWRX/Kiwi): client-side, applied in the audio
  // engine (iOS VibePowerModule / Android VibeStreamService). Persisted globally
  // and (re)applied whenever the connection comes up — see the effect below.
  // Session-scoped: NOT persisted, so it reverts to Off on a server change. It
  // only survives pause/resume because the screen stays mounted (re-applied on
  // reconnect by the effect below).
  const onNetNotch = useCallback((on: boolean) => {
    setNetNotch(on);
  }, []);

  const insets = useSafeAreaInsets();
  const { width: screenW, height: screenH } = Dimensions.get('window');
  const isLandscape = screenW > screenH;
  // Tablets (iPad) have room for the decoder panel in landscape; phones don't.
  const isTablet = Math.min(screenW, screenH) >= 768;

  // ── Spec ratio (portrait + landscape stored separately) ───────────────────
  const [specRatioPortrait,  setSpecRatioPortrait]  = useState(0.28);
  const [specRatioLandscape, setSpecRatioLandscape] = useState(0.20);
  const [ratioOverlayOpen,   setRatioOverlayOpen]   = useState(false);
  const specFrac = isLandscape ? specRatioLandscape : specRatioPortrait;

  // ── Client ────────────────────────────────────────────────────────────────

  const client    = useRef<SDRBackend | null>(null);
  const destroyed = useRef(false);
  // Bumping connEpoch mints a fresh session uuid and re-runs the whole connect
  // path (spectrum client + native audio engine + decoder) from scratch — used
  // to recover from a data-saver disconnect, where reopening the old session's
  // sockets lands in a broken half-state (frozen waterfall/zoom, no audio).
  const [connEpoch, setConnEpoch] = useState(0);
  const lastReconnectAt = useRef(0);
  const fullReconnect = useCallback(() => {
    const now = Date.now();
    if (now - lastReconnectAt.current < 2000) return;  // debounce double-triggers
    lastReconnectAt.current = now;
    setConnEpoch((e: number) => e + 1);
    // If we don't connect within ~12s (server full / rate-limited), flag failure
    // so the lock-screen card + banner tell the user to open the app.
    setTimeout(() => {
      if (!connectedRef.current) {
        VibePowerModule?.setReconnectFailed?.(true);
        setReconnectFailedUi(true);
      }
    }, 12000);
  }, []);
  const sessionUuid = useMemo(() => uuidv4(), [baseUrl, connEpoch]);

  // ── SDR state ─────────────────────────────────────────────────────────────

  const [connected, setConnected] = useState(false);
  const [serverLost, setServerLost] = useState(false);   // OWRX server crashed/restarted
  const [serverBusy, setServerBusy] = useState(false);   // Kiwi receiver full (too_busy)
  const [connLost,   setConnLost]   = useState(false);   // UberSDR link down — auto-reconnecting
  const [connTimedOut, setConnTimedOut] = useState(false); // initial connect never completed
  const connLostTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  // Initialised from AppState.currentState — a cold launch INTO THE BACKGROUND (the
  // watch waking the phone) fires no `change` event, so assuming foreground here made
  // the app behave as though someone were looking at it.
  const appActiveRef  = useRef(AppState.currentState === 'active');
  // Returning from the background: the spectrum was deliberately paused, so the
  // link reads 0 for a moment while the waterfall re-subscribes. Show a calm
  // "reinitialising" notice instead of the alarming "connection lost" one, and
  // only fall back to the real disconnect popup if it doesn't recover in time.
  const [reinit, setReinit] = useState(false);
  const reinitTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const resumingRef = useRef(false);   // true during the post-background reinit window
  // Audio came back fine but the spectrum/waterfall never re-subscribed — give
  // the user a way out (reconnect / instance list) instead of a stuck notice.
  const [specFailed, setSpecFailed] = useState(false);
  const [profiles, setProfiles]   = useState<ProfileInfo[]>([]);  // OWRX only
  const [activeProfileId, setActiveProfileId] = useState<string | undefined>(undefined);
  const [sdrUsage, setSdrUsage] = useState<Record<string, { name: string; inUse: boolean; activeProfileId?: string }>>({});  // OWRX: per-SDR usage
  const [clientCount, setClientCount] = useState(0);  // OWRX: live user count
  const [serverModes, setServerModes] = useState<BackendMode[]>([]);  // OWRX gated demod list
  // OWRX: server/profile preset DSP defaults (initial_squelch_level / initial_nr_level)
  // pushed on connect + every profile switch; seeds the menu's squelch/NR sliders so
  // they reflect the owner's preset (e.g. an NFM 2 m profile with a fixed squelch).
  const [owrxDspDefaults, setOwrxDspDefaults] =
    useState<{ squelchDb?: number; nrEnabled?: boolean; nrThreshold?: number; seq: number }>({ seq: 0 });
  // Live RDS (FM) / DAB station metadata (OWRX). liveStationRef mirrors the name
  // for the VTS resolver (reads in a debounced callback, avoids stale closures).
  const [dabProgrammes, setDabProgrammes] = useState<DabProgramme[]>([]);  // OWRX DAB ensemble
  const [activeDabId, setActiveDabId] = useState<number>(0);
  const [dabEnsemble, setDabEnsemble] = useState('');
  /** OWRX ADS-B: the live aircraft table. Structured — it used to be flattened to
   *  text on arrival, which is why nothing but the decoder panel could use it. */
  const [aircraft, setAircraft] = useState<Aircraft[]>([]);
  // DAB speed correction (dablin chipmunk workaround) — 1 = off; persisted.
  const [dabSpeed, setDabSpeed] = useState<number>(1);
  const [liveStation, setLiveStation] = useState<{ name?: string; text?: string; badge?: string; countryIso?: string; pi?: string }>({});
  const liveBadgeRef = useRef<string | undefined>(undefined);
  const liveStationRef = useRef<string>('');
  const [liveLogo, setLiveLogo] = useState<string | null>(null);   // WFM RDS station favicon
  const lastLiveLogoKey = useRef('');
  const [fmStereo, setFmStereo] = useState(false);   // WFM stereo pilot (local hardware)

  // CarFM: the FM-only face covers the full SDR UI when active. "Advanced" lets
  // the normal SDR UI (waterfall/decoders/all modes) back in without leaving.
  const carFm = !!route.params.carFm;
  const [advancedOpen, setAdvancedOpen] = useState(false);
  const fmFaceActive = carFm && !advancedOpen;
  const [fmSignalDb, setFmSignalDb] = useState<number | null>(null);

  // DAB speed correction is remembered PER STATION (ensemble + programme), since
  // the chipmunk is per-service: you set ×0.67 on a bad station once and it
  // auto-applies every time you return, while good stations stay Off. dabSpeed
  // is the CURRENT station's factor (for the menu highlight); the map is the store.
  const dabSpeedMapRef = useRef<Record<string, number>>({});
  const dabKeyRef = useRef<string>('');   // "<ensemble>|<programme>" of the tuned service
  useEffect(() => {
    AsyncStorage.getItem('owrx_dab_speed_map').then((j: string | null) => {
      if (!j) return;
      try { const m = JSON.parse(j); if (m && typeof m === 'object') dabSpeedMapRef.current = m; } catch {}
    }).catch(() => {});
  }, []);
  // Menu speed buttons + fine slider set the factor for the CURRENTLY tuned
  // station. Applied live; the storage write is debounced so dragging the slider
  // doesn't hammer AsyncStorage (it fires onValueChange continuously).
  const dabSaveTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const onDabSpeed = useCallback((scale: number) => {
    setDabSpeed(scale);
    client.current?.setDabAudioScale?.(scale);
    const key = dabKeyRef.current;
    if (!key) return;
    dabSpeedMapRef.current = { ...dabSpeedMapRef.current, [key]: scale };
    if (dabSaveTimer.current) clearTimeout(dabSaveTimer.current);
    dabSaveTimer.current = setTimeout(() => {
      AsyncStorage.setItem('owrx_dab_speed_map', JSON.stringify(dabSpeedMapRef.current)).catch(() => {});
    }, 400);
  }, []);
  // Called from the DAB metadata handler when the tuned service changes: look up
  // its saved correction (default Off) and apply it automatically.
  const applyDabStation = useCallback((ensemble: string, programme: string) => {
    const key = ensemble + '|' + programme;
    if (key === dabKeyRef.current) return;
    dabKeyRef.current = key;
    const saved = dabSpeedMapRef.current[key] ?? 1;
    setDabSpeed(saved);
    client.current?.setDabAudioScale?.(saved);
  }, []);
  const [status, setStatus]       = useState<SDRStatus>({
    frequency: 14_074_000, mode: 'usb',
    bandwidthLow: -3000, bandwidthHigh: 3000,
    binCount: 1024, binBandwidth: 0, centerHz: 0, bwHz: 0,
  });
  // Hot-path frame sink — WaterfallView registers its imperative frame handler
  // here; spectrum frames bypass React state entirely (CPU audit 2026-06-11:
  // setState per 10–20Hz frame re-rendered the whole tree ≈ a full core).
  const wfFrameSink = useRef<((b: Float32Array, s: SDRStatus) => void) | null>(null);
  // Muted via media controls (AirPods squeeze → pause = mute) — native emits
  // VibeMuted so the UI can show a tap-to-unmute banner.
  const [isMuted, setIsMuted] = useState(false);
  const isMutedRef = useRef(false);
  useEffect(() => { isMutedRef.current = isMuted; }, [isMuted]);
  /** The iPhone's real SYSTEM volume (0…1), kept current by the VibeVolume KVO event.
   *  The watch sends DELTAS against it — it never sends an absolute, because the phone
   *  owns the value and the wrist is only allowed to nudge it. */
  const sysVolRef = useRef(1);
  // True when the data saver has dropped the SDR stream after a muted spell.
  const [dataSaverOff, setDataSaverOff] = useState(false);
  const dataSaverOffRef = useRef(false);
  useEffect(() => { dataSaverOffRef.current = dataSaverOff; }, [dataSaverOff]);
  const unmute = useCallback(() => {
    (NativeModules.VibePowerModule as { setMuted?: (m: boolean) => void })?.setMuted?.(false);
    setIsMuted(false);
  }, []);

  // Full-screen waterfall: hide the controls bar, floating chevron restores.
  const [controlsHidden, setControlsHidden] = useState(false);
  const onHideControls = useCallback(() => { setControlsHidden(true); setMenuOpen(false); }, []);

  // Centre the spectrum view on the tuned frequency at the current zoom
  // (reference-skin parity).
  const onCentreVfo = useCallback(() => {
    const c = client.current; if (!c) return;
    const v = c.getView();
    if (v.binBandwidth > 0) c.zoom(c.getStatus().frequency, v.binBandwidth);
  }, []);

  // ── VFO lock / waterfall panning (BRIEF-vfo-lock-and-panning) ───────────────
  // Default locked = today's behaviour (view follows the VFO). Unlocked lets the
  // waterfall pan freely. Persisted in lsv_vfo_lock; mirrored to the client as
  // followVfo. Disabled (but shown) on local hardware until Phase 2.
  const [vfoLocked, setVfoLocked] = useState(true);
  const vfoLockedRef = useRef(true);
  useEffect(() => { vfoLockedRef.current = vfoLocked; }, [vfoLocked]);

  useEffect(() => {
    AsyncStorage.getItem('lsv_vfo_lock')
      .then(v => {
        const locked = v == null ? true : v === '1';
        setVfoLocked(locked);
        client.current?.setFollowMode(locked);
      })
      .catch(() => {});
  }, []);

  // Local hardware: keep the client's Fs window in sync with the live sample
  // rate so panSpan()'s movable wall matches the real capture bandwidth.
  useEffect(() => {
    if (!isLocal) return;
    (client.current as { setLocalSampleRate?: (hz: number) => void } | null)
      ?.setLocalSampleRate?.(hwSampleRate);
  }, [isLocal, hwSampleRate]);

  const onToggleVfoLock = useCallback(() => {
    setVfoLocked(prev => {
      const next = !prev;
      client.current?.setFollowMode(next);
      if (next) onCentreVfo();                  // re-locking snaps back to the VFO
      AsyncStorage.setItem('lsv_vfo_lock', next ? '1' : '0').catch(() => {});
      return next;
    });
  }, [onCentreVfo]);

  // Boundary walls for the waterfall (unlocked only).
  //  • Remote (UberSDR/Kiwi/OWRX): hard walls at the band/profile/rx edges.
  //  • Local/RTL-TCP: the dongle's captured Fs window edges (centre ± Fs/2) —
  //    these are the real "you can pan/tune this far" boundaries; the spectrum
  //    ends there. They move as the dongle re-tunes.
  // Local RF-centre (dongle) — derived to mirror the shim. Drives the RF-centre
  // marker (which can sit off-screen once the dongle locks and the view pans on)
  // and the capture-window walls (rfCenter ± Fs/2).
  const localRf = useMemo(() => {
    if (!isLocal) return null;
    const c = client.current as
      { rfCenterHz?: () => number; captureBandwidth?: () => number } | null;
    const fs = c?.captureBandwidth?.() || hwSampleRate;
    const rf = c?.rfCenterHz?.();
    if (rf == null || !(fs > 0)) return null;
    return { rf, fs };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [isLocal, status.centerHz, status.frequency, status.bwHz, hwSampleRate, connEpoch]);

  const walls = useMemo(() => {
    if (vfoLocked) return null;
    if (isLocal) {
      // Hard walls at the captured-band edges (dongle ± Fs/2) — these become
      // visible as you scroll the view across the band.
      if (!localRf) return null;
      const half = localRf.fs / 2;
      return { loHz: localRf.rf - half, hiHz: localRf.rf + half };
    }
    const s = client.current?.panSpan();
    return s ? { loHz: s.loHz, hiHz: s.hiHz } : null;
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [vfoLocked, isLocal, localRf, status.bwHz, connEpoch]);

  // VFO has panned outside the visible span → show the floating recentre button.
  // (No toast hint — the floating button itself is the affordance; VTS pop-ups
  // caused more trouble than they solved on the original skin.)
  const vfoOffscreen = !vfoLocked && status.bwHz > 0 &&
    (status.frequency < status.centerHz - status.bwHz / 2 ||
     status.frequency > status.centerHz + status.bwHz / 2);

  // ── Step ──────────────────────────────────────────────────────────────────

  const [step,      setStep]      = useState(1000);
  const [stepOpen,  setStepOpen]  = useState(false);
  const stepRef = useRef(step);
  useEffect(() => { stepRef.current = step; }, [step]);

  // ── Display settings ──────────────────────────────────────────────────────

  const [dbMin,         setDbMin]         = useState(-120);
  const [dbMax,         setDbMax]         = useState(-20);
  const [colormap,      setColormap]      = useState('Jet');       // production default
  const [nr,            setNr]            = useState(false);
  const [nb,            setNb]            = useState(false);
  // NR cycle: off → nr → nr2. SERV state controlled by server DSP section.
  const [nrMode,        setNrMode]        = useState<'off'|'nr'|'nr2'>('off');
  // Waterfall / spectrum display settings
  const [specShow,      setSpecShow]      = useState(true);
  const [specSmoothing, setSpecSmoothing] = useState(5);
  const [specFloor,     setSpecFloor]     = useState(0);
  const [specPeakScale, setSpecPeakScale] = useState(10);
  const [peakHold,      setPeakHold]      = useState(true);
  const [wfBrightness,  setWfBrightness]  = useState(0);
  const [wfContrast,    setWfContrast]    = useState(0);
  const [wfSharpness,   setWfSharpness]   = useState(5);
  // UberSDR auto-range symmetric contrast (0–20). Web client calibration = 10.
  const [hwLockedRate, setHwLockedRate] = useState(0);   // >0 = server pinned the rate
  const [autoContrast,  setAutoContrast]  = useState(5);  // production default (10 too dark)
  // M9PSY 5-tap spatial waterfall smooth
  const [spatialSmooth, setSpatialSmooth] = useState(true);
  const [wfCoarse,      setWfCoarse]      = useState<'auto'|'manual'>('auto');
  const [frameRate,     setFrameRate]     = useState<'native'|'20fps'|'30fps'>('20fps');
  // Smooth tune: 120Hz interpolated scroll while interacting; discrete row
  // steps + ~30fps spectrum tween once settled (ProMotion idles → battery).
  const [smoothTune,    setSmoothTune]    = useState(true);
  // Idle saver: after 30s without touch, ask the server for ⅓ frame rate
  // (set_rate 3 — skin default-waterfall parity). Meters/waterfall/spectrum
  // all slow with the data; any touch restores full rate instantly.
  const [idleSlow,      setIdleSlow]      = useState(true);
  const [vfoNeedle,     setVfoNeedle]     = useState('#ffffff');   // production default
  // Needle/glow brightness 1-10 (5 = original look) — bright palettes can
  // swallow the needle whatever colour it is (Stuart 2026-06-12 eve)
  const [vfoIntensity,  setVfoIntensity]  = useState(5);
  // Frost 0-10 (0 = off): smoked-glass band over the passband
  const [vfoFrost,      setVfoFrost]      = useState(5);           // production default
  // Instance spectrum backdrop (/api/spectrum-bg-image) + opacity 0-10
  // (3 = web default 0.30); follows the server's configured opacity until
  // the user moves the slider (or a saved pref exists)
  const [bgImageUrl,    setBgImageUrl]    = useState<string | null>(null);
  const [bgOpacity,     setBgOpacity]     = useState(3);
  const bgOpacityUserSet = useRef(false);
  // Station-ID overlay (web drawStationIdOverlay parity)
  const [stationId,     setStationId]     = useState<{ line1: string; line2?: string; color: string } | null>(null);
  // Server software version (menu footer — identifies the backend type)
  const [serverVersion, setServerVersion] = useState<string | null>(null);
  const [serverLabel,   setServerLabel]   = useState<string | null>(null);  // OWRX: OpenWebRX/+
  const [aboutOpen,     setAboutOpen]     = useState(false);
  const [recordingsOpen, setRecordingsOpen] = useState(false);
  // Mute the live SDR while a recording plays so they don't fight over the audio
  // session; restore the prior mute state when the browser closes.
  const preRecMuteRef = useRef(false);
  const onRecordingsActive = useCallback((active: boolean) => {
    const VM = NativeModules.VibePowerModule as { setMuted?: (m: boolean) => void };
    if (active) { preRecMuteRef.current = isMutedRef.current; VM?.setMuted?.(true); }
    else        { VM?.setMuted?.(preRecMuteRef.current); }
  }, []);

  useEffect(() => {
    let cancelled = false;
    fetchUiConfig(baseUrl).then((cfg: ServerUiConfig | null) => {
      if (cancelled) return;
      if (cfg?.spectrum_bg_image) {
        const raw = cfg.spectrum_bg_image;
        const abs = raw.startsWith('http')
          ? raw
          : baseUrl.replace(/\/+$/, '') + (raw.startsWith('/') ? raw : '/' + raw);
        // Cache-bust like the web client — a freshly uploaded image always loads
        setBgImageUrl(abs + (abs.includes('?') ? '&' : '?') + 't=' + Date.now());
      } else {
        setBgImageUrl(null);
      }
      if (!bgOpacityUserSet.current && typeof cfg?.spectrum_bg_opacity === 'number') {
        setBgOpacity(Math.round(Math.max(0, Math.min(1, cfg.spectrum_bg_opacity)) * 10));
      }
      const overlayOff = cfg?.station_id_overlay === false;
      if (overlayOff) setStationId(null);
      const idColor = /^#[0-9a-fA-F]{6}$/.test((cfg?.station_id_color ?? '').trim())
        ? (cfg!.station_id_color as string).trim() : '#ffffff';
      fetchReceiverInfo(baseUrl).then((r: ReceiverInfo | null) => {
        if (cancelled || !r) return;
        if (r.serverVersion) setServerVersion(r.serverVersion);
        if (overlayOff) return;
        const callsign = (r.callsign ?? '').trim();
        const name     = (r.name ?? '').trim();
        if (!callsign && !name) return;
        setStationId({
          line1: callsign && name ? `${callsign} - ${name}` : (callsign || name),
          line2: (r.location ?? '').trim() || undefined,
          color: idColor,
        });
      }).catch(() => {});
    }).catch(() => {});
    return () => { cancelled = true; };
  }, [baseUrl]);

  // SNR squelch (audio gate) — value ≤ -999 = open/disabled
  const [snrSquelch,    setSnrSquelch]    = useState(-999);
  // FM squelch — value ≤ -999 = open. Only active on fm/nfm modes.
  const [fmSquelch,     setFmSquelch]     = useState(-999);
  // Server-side NR (DSP insert) — filter list + param descriptors arrive via
  // the native audio WS (get_dsp_filters → dsp_filters); params are STRINGS
  // on the wire (server paramInfo is all string-typed).
  const [serverDspEnabled, setServerDspEnabled] = useState(false);
  const [serverDspFilter,  setServerDspFilter]  = useState('');
  const [serverDspParams,  setServerDspParams]  = useState<Record<string,string>>({});
  const [dspFilters,       setDspFilters]       = useState<DspFilterDesc[]>([]);
  // Kiwi squelch is a CLIENT-SIDE dBFS gate (the server SNR-based squelch is
  // unreliable). Threshold in dBm: −130 = Off (open), up to −20. Driven from the
  // S-meter dBm in onSMeter → native setSquelchOpen, with a short release tail.
  const [kiwiSquelch,      setKiwiSquelch]      = useState(-130); // dBm threshold (−130 = off)
  const kiwiSqDbmRef  = useRef(-130);
  const kiwiSqOpenRef = useRef(true);
  const kiwiSqAboveAt = useRef(0);
  const onKiwiSquelch = useCallback((db: number) => {
    setKiwiSquelch(db); kiwiSqDbmRef.current = db;
    if (db <= -130) {  // Off → force the gate open immediately
      kiwiSqOpenRef.current = true;
      (NativeModules.VibePowerModule as { setSquelchOpen?: (o: boolean) => void })?.setSquelchOpen?.(true);
    }
  }, []);
  // Evaluate the Kiwi squelch gate against a fresh S-meter reading (dBm).
  const evalKiwiSquelch = useCallback((dbm: number) => {
    const thr = kiwiSqDbmRef.current;
    if (thr <= -130) return;                       // Off — handled in onKiwiSquelch
    const now = Date.now();
    if (dbm >= thr) kiwiSqAboveAt.current = now;
    const open = (now - kiwiSqAboveAt.current) < 350;  // 350 ms release tail
    if (open !== kiwiSqOpenRef.current) {
      kiwiSqOpenRef.current = open;
      (NativeModules.VibePowerModule as { setSquelchOpen?: (o: boolean) => void })?.setSquelchOpen?.(open);
    }
  }, []);
  const [dspError,         setDspError]         = useState<string | null>(null);

  // ── UI overlay state ──────────────────────────────────────────────────────

  const [menuOpen,      setMenuOpen]      = useState(false);
  const [freqModalOpen, setFreqModalOpen] = useState(false);

  // Server map overlays (HFDL / Digital spots / CW spots — skin parity)
  const [mapKind, setMapKind] = useState<MapKind | null>(null);
  // On-device FT8 spots map (Local/Kiwi) + its no-GPS city-picker fallback.
  const [localMapOpen, setLocalMapOpen]   = useState(false);
  const [cityPickerOpen, setCityPickerOpen] = useState(false);

  // Admin pages (skin menu Admin section) — in-app browser overlay
  const [adminPage, setAdminPage] = useState<{ url: string; title: string } | null>(null);
  const onAdminLink = useCallback((path: string, title: string) => {
    if (!baseUrl) return;
    setMenuOpen(false);
    setAdminPage({ url: baseUrl.replace(/\/+$/, '') + path, title });
  }, [baseUrl]);

  // Frequency display unit — chosen in FreqModal, drives the main readout too.
  const [freqUnit, setFreqUnit] = useState<'hz' | 'khz' | 'mhz'>('khz');
  useEffect(() => {
    AsyncStorage.getItem('lsv_fq_unit').then((u: string | null) => {
      if (u === 'hz' || u === 'khz' || u === 'mhz') setFreqUnit(u);
    }).catch(() => {});
    // Smooth tune is always on now (no toggle) — don't restore an old saved "off".
    AsyncStorage.getItem('lsv_idle_slow').then((v: string | null) => {
      if (v !== null) setIdleSlow(v === '1');
    }).catch(() => {});
    AsyncStorage.getItem('lsv_frame_rate').then((v: string | null) => {
      if (v === 'native' || v === '20fps' || v === '30fps') setFrameRate(v);
    }).catch(() => {});
  }, []);
  const [modeSelOpen,   setModeSelOpen]   = useState(false);
  const [audioSheetOpen, setAudioSheetOpen] = useState(false);

  // ── Signal / SNR ──────────────────────────────────────────────────────────

  // Meter values bypass React state entirely (full-tree re-render per update
  // was ~a third of all JS time in the CPU profile) — leaf widgets subscribe.
  const meterBus    = useRef(createMeterBus());
  const meterSmooth = useRef({ level: 0, peak: 0, hold: 0 });
  // SNR from radiod's channel status (basebandPower − noiseDensity), pushed by
  // native per audio packet. This is the demodulator's own measurement (zoom-
  // independent, unlike the spectrum). −30 corrects radiod's known +30 dB
  // audio-stream floor offset (madpsy/ka9q_ubersdr#77) so it's honest 0–50 dB,
  // NOT the buggy 30–80 dB UberSDR shows. null until the first reading arrives.
  const audioSnrRef = useRef<number | null>(null);
  // Last time an audio packet was heard (VibeSignal fires ~5×/s while audio
  // flows). Used to tell a slow spectrum re-subscribe (audio still alive → keep
  // the calm "reinitialising" notice) from a genuine drop (audio dead too).
  const lastAudioAtRef = useRef(0);
  // OWRX reports a real channel S-meter (dBm) over the control WS — the
  // demodulator's own level reading, zoom-independent like UberSDR's SNR. We
  // store the latest value and let it drive the absolute (S-meter/dBFS) meter
  // for OWRX, where there's no native VibeSignal feed. null until first reading.
  const owrxSmeterRef = useRef<number | null>(null);
  const [signalMode,   setSignalMode]   = useState<'snr' | 'smeter' | 'dbfs'>('snr');
  const signalModeRef = useRef<'snr' | 'smeter' | 'dbfs'>('snr');
  useEffect(() => { signalModeRef.current = signalMode; }, [signalMode]);

  // ── Display prefs persistence — every waterfall/spectrum/display setting in
  // one blob, restored on launch, saved debounced (sliders fire per-tick).
  const prefsLoaded = useRef(false);
  // Save scope: 'server' when a per-instance override exists (saved via the
  // display panel's THIS SERVER button) — auto-save then targets that key so
  // later tweaks stick to this instance instead of silently reverting.
  const prefsTarget = useRef<'global' | 'server'>('global');
  const latestPrefsJson = useRef('');
  useEffect(() => {
    (async () => {
      let j: string | null = null;
      try {
        j = await AsyncStorage.getItem('lsv_display_prefs:' + baseUrl);
        if (j) prefsTarget.current = 'server';
        else j = await AsyncStorage.getItem('lsv_display_prefs');
      } catch {}
      applyPrefs(j);
    })();
    function applyPrefs(j: string | null) {
      if (j) {
        try {
          const p = JSON.parse(j) as Record<string, unknown>;
          const num  = (k: string, set: (v: number) => void)  => { const v = p[k]; if (typeof v === 'number' && isFinite(v)) set(v); };
          const bool = (k: string, set: (v: boolean) => void) => { const v = p[k]; if (typeof v === 'boolean') set(v); };
          num('dbMin', setDbMin);                 num('dbMax', setDbMax);
          num('specSmoothing', setSpecSmoothing); num('specFloor', setSpecFloor);
          num('specPeakScale', setSpecPeakScale); num('wfBrightness', setWfBrightness);
          num('wfContrast', setWfContrast);       num('wfSharpness', setWfSharpness);
          num('autoContrast', setAutoContrast);   num('step', setStep);
          num('specRatioPortrait', setSpecRatioPortrait);
          num('specRatioLandscape', setSpecRatioLandscape);
          bool('specShow', setSpecShow);          bool('peakHold', setPeakHold);
          bool('spatialSmooth', setSpatialSmooth);
          if (p.wfCoarse === 'auto' || p.wfCoarse === 'manual') setWfCoarse(p.wfCoarse);
          if (p.signalMode === 'snr' || p.signalMode === 'smeter' || p.signalMode === 'dbfs') setSignalMode(p.signalMode);
          if (typeof p.colormap === 'string')  setColormap(p.colormap);
          if (typeof p.vfoNeedle === 'string') setVfoNeedle(p.vfoNeedle);
          num('vfoIntensity', setVfoIntensity);
          num('vfoFrost', setVfoFrost);
          num('bgOpacity', (v: number) => { setBgOpacity(v); bgOpacityUserSet.current = true; });
        } catch {}
      }
      prefsLoaded.current = true;
    }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [baseUrl]);

  useEffect(() => {
    if (!prefsLoaded.current) return; // don't clobber the blob with defaults pre-load
    const json = JSON.stringify({
      dbMin, dbMax, colormap, specShow, specSmoothing, specFloor,
      specPeakScale, peakHold, wfBrightness, wfContrast, wfSharpness,
      autoContrast, spatialSmooth, wfCoarse, vfoNeedle, vfoIntensity, vfoFrost, bgOpacity, signalMode, step,
      specRatioPortrait, specRatioLandscape,
    });
    latestPrefsJson.current = json;
    const key = prefsTarget.current === 'server'
      ? 'lsv_display_prefs:' + baseUrl : 'lsv_display_prefs';
    const t = setTimeout(() => {
      AsyncStorage.setItem(key, json).catch(() => {});
    }, 500);
    return () => clearTimeout(t);
  }, [dbMin, dbMax, colormap, specShow, specSmoothing, specFloor,
      specPeakScale, peakHold, wfBrightness, wfContrast, wfSharpness,
      autoContrast, spatialSmooth, wfCoarse, vfoNeedle, vfoIntensity, vfoFrost, bgOpacity, signalMode, step,
      specRatioPortrait, specRatioLandscape, baseUrl]);

  // Display-panel save row (skin parity): RESET = defaults + drop the server
  // override; THIS SERVER = per-instance override; GLOBAL = the shared blob.
  const onDispReset = useCallback(() => {
    AsyncStorage.removeItem('lsv_display_prefs:' + baseUrl).catch(() => {});
    prefsTarget.current = 'global';
    setDbMin(-120); setDbMax(-20); setColormap('Jet');
    setSpecShow(true); setSpecSmoothing(5); setSpecFloor(0);
    setSpecPeakScale(10); setPeakHold(true);
    setWfBrightness(0); setWfContrast(0); setWfSharpness(5);
    setAutoContrast(5); setSpatialSmooth(true); setWfCoarse('auto');
    setVfoNeedle('#ffffff'); setVfoIntensity(5); setVfoFrost(5); setBgOpacity(3); setSignalMode('snr'); setStep(1000);
    setSpecRatioPortrait(0.28); setSpecRatioLandscape(0.20);
    Alert.alert('Display Reset', 'Display settings restored to defaults.');
  }, [baseUrl]);

  const onDispSaveServer = useCallback(() => {
    prefsTarget.current = 'server';
    AsyncStorage.setItem('lsv_display_prefs:' + baseUrl, latestPrefsJson.current)
      .catch(() => {});
    Alert.alert('Saved', 'Display settings saved for this server.');
  }, [baseUrl]);

  const onDispSaveGlobal = useCallback(() => {
    prefsTarget.current = 'global';
    AsyncStorage.removeItem('lsv_display_prefs:' + baseUrl).catch(() => {});
    AsyncStorage.setItem('lsv_display_prefs', latestPrefsJson.current)
      .catch(() => {});
    Alert.alert('Saved', 'Display settings saved as the global default.');
  }, [baseUrl]);

  // ── Recording ─────────────────────────────────────────────────────────────

  const [isRecording, setIsRecording] = useState(false);
  const [recSeconds,  setRecSeconds]  = useState(0);
  const recTimerRef = useRef<ReturnType<typeof setInterval> | null>(null);
  // iOS: the native share sheet (UIActivityViewController) must NOT present while
  // the AudioSheet Modal is up — it presents OVER the modal and RN loses track,
  // wedging all touch/render on dismiss. So on stop we stash the path, close the
  // sheet, and fire the share from the sheet's onDismiss (nothing modal up).
  const pendingRecShare = useRef<string | null>(null);

  const toggleRecording = useCallback(() => {
    if (!isRecording) {
      // Pass the LIVE freq/mode for the filename — native currentFreq is only
      // tracked on UberSDR's audio WS, so OWRX would otherwise show a stale freq.
      (VibePowerModule as any)?.startRecording(Math.round(status.frequency || 0), String(status.mode || ''))
        .then(() => {
          setRecSeconds(0);
          recTimerRef.current = setInterval(() => setRecSeconds((s: number) => s + 1), 1000);
          setIsRecording(true);
        })
        .catch((e: Error) => Alert.alert('Recording', `Could not start recording: ${e.message}`));
    } else {
      if (recTimerRef.current) { clearInterval(recTimerRef.current); recTimerRef.current = null; }
      setRecSeconds(0);
      setIsRecording(false);
      VibePowerModule?.stopRecording()
        .then(async (path: string | null) => {
          // Half-height native share sheet; the file also stays in app storage
          // (iOS Documents / Android filesDir) and is reachable via the
          // Recordings browser. Android needs an Expo content URI to share.
          if (!path) { setAudioSheetOpen(false); return; }
          if (Platform.OS === 'android') {
            try {
              const cu = await FileSystem.getContentUriAsync(
                path.startsWith('file://') ? path : 'file://' + path);
              VibePowerModule?.shareRecording(cu);
            } catch {}
            setAudioSheetOpen(false);
          } else {
            // Defer the share to AudioSheet's onDismiss (see pendingRecShare).
            pendingRecShare.current = path;
            setAudioSheetOpen(false);
          }
        })
        .catch(() => setAudioSheetOpen(false));
    }
  }, [isRecording, status.frequency, status.mode]);

  useEffect(() => () => {
    if (recTimerRef.current) clearInterval(recTimerRef.current);
  }, []);

  // ── Chat ──────────────────────────────────────────────────────────────────

  const [chatOpen,     setChatOpen]     = useState(false);
  const [chatUnread,   setChatUnread]   = useState(false);
  const [chatMessages, setChatMessages] = useState<ChatMessage[]>([]);
  const [myCallsign,   setMyCallsign]   = useState<string | null>(null);
  const [chatMuted,    setChatMuted]    = useState(false);
  const [chatUsers,    setChatUsers]    = useState<ChatUserRow[]>([]);
  const [chatEnabled,  setChatEnabled]  = useState(false);   // OWRX: from config allow_chat
  const [syncedUser,   setSyncedUser]   = useState<string | null>(null);
  const [zoomSync,     setZoomSync]     = useState(false);
  const myCallsignRef = useRef<string | null>(null);
  const chatMutedRef  = useRef(false);
  const syncedUserRef = useRef<string | null>(null);
  const zoomSyncRef   = useRef(false);
  useEffect(() => { myCallsignRef.current = myCallsign; }, [myCallsign]);
  useEffect(() => { chatMutedRef.current = chatMuted; },   [chatMuted]);
  useEffect(() => { syncedUserRef.current = syncedUser; }, [syncedUser]);
  useEffect(() => { zoomSyncRef.current = zoomSync; },     [zoomSync]);

  /** quiet=true (history replay / muted) — render without the unread pulse */
  const addChatMsg = useCallback((msg: ChatMessage, quiet = false) => {
    setChatMessages((prev: ChatMessage[]) => [...prev.slice(-99), msg]);
    if (quiet || chatMutedRef.current) return;
    setChatOpen((open: boolean) => {
      if (!open) setChatUnread(true);
      return open;
    });
  }, []);

  // Username rules (server SetUsername): 1–15 chars, letters/digits plus
  // - _ / inside; NO spaces; case preserved (need not be capitals).
  const sanitizeCallsign = useCallback((raw: string): string =>
    raw.replace(/[^A-Za-z0-9\-_\/]/g, '').replace(/^[-_\/]+|[-_\/]+$/g, '').slice(0, 15), []);

  const isOwrx = route.params.serverType === 'owrx';
  const isKiwi = route.params.serverType === 'kiwi';
  // Kiwi exposes its noise filters/blanker as DSP descriptors → reuse the
  // UberSDR server-DSP menu UI (filter selector + param sliders).
  useEffect(() => {
    if (isKiwi) setDspFilters(KiwiAdapter.DSP_FILTERS as DspFilterDesc[]);
  }, [isKiwi]);
  // OWRX and Kiwi have no SNR feed (radiod-only) — default to the S-meter
  // (the 'snr' mode reads dead on those backends).
  useEffect(() => { if ((isOwrx || isKiwi) && signalMode === 'snr') setSignalMode('smeter'); }, [isOwrx, isKiwi, signalMode]);
  const handleChatJoin = useCallback((cs: string) => {
    const clean = sanitizeCallsign(cs);
    if (!clean) return;
    setMyCallsign(clean);
    // OWRX has no join handshake — the name rides on each message; UberSDR joins.
    if (!isOwrx) decoderClient.current?.joinChat(clean);
    AsyncStorage.setItem('lsv_chat_callsign:' + baseUrl, clean).catch(() => {});
  }, [sanitizeCallsign, baseUrl, isOwrx]);

  const handleChatSend = useCallback((text: string) => {
    if (!myCallsign) return;
    if (isOwrx) client.current?.sendChat?.(text, myCallsign);
    else decoderClient.current?.sendChat(text);
    // Own messages echo back via the broadcast — rendered then (deduped),
    // matching the skin: what you see is what the server accepted.
  }, [myCallsign, isOwrx]);

  // Tune/zoom sync OUT: report our freq/mode/BW edges/zoom to chat so other
  // users can see and sync to us (debounced 1s — the drum emits fast)
  useEffect(() => {
    if (!myCallsign || !status.frequency || isOwrx) return;   // OWRX = basic text chat, no sync
    const t = setTimeout(() => {
      const view = client.current?.getView();
      decoderClient.current?.sendChatStatus({
        frequency: status.frequency,
        mode:      status.mode,
        bw_low:    status.bandwidthLow,
        bw_high:   status.bandwidthHigh,
        zoom_bw:   view?.binBandwidth ?? 0,
      });
    }, 1000);
    return () => clearTimeout(t);
  }, [myCallsign, status.frequency, status.mode, status.bandwidthLow, status.bandwidthHigh]);

  // Tune/zoom sync IN: follow another user's tune (skin syncToUser)
  const applyChatSync = useCallback((u: ChatUserRow) => {
    if (!u.frequency || !u.mode) return;
    onTuneHzRef.current?.(u.frequency);
    const m = u.mode.toLowerCase();
    if (m in MODE_BANDWIDTHS) onModeRef.current?.(m as SDRMode);
    if (typeof u.bw_low === 'number' && typeof u.bw_high === 'number') {
      onFilterBothRef.current?.(u.bw_low, u.bw_high);
    }
    if (zoomSyncRef.current && u.zoom_bw && u.zoom_bw > 0) {
      client.current?.zoom(u.frequency, u.zoom_bw);
    }
  }, []);

  const toggleUserSync = useCallback((username: string) => {
    setSyncedUser((prev: string | null) => {
      const next = prev === username ? null : username;
      if (next) {
        const u = chatUsersRef.current.find((x: ChatUserRow) => x.username === next);
        if (u) applyChatSync(u);
      }
      return next;
    });
  }, [applyChatSync]);

  // One-shot: tap a user row to jump to their frequency without following
  const chatUserTap = useCallback((u: ChatUserRow) => {
    applyChatSync(u);
  }, [applyChatSync]);

  const chatUsersRef = useRef<ChatUserRow[]>([]);
  useEffect(() => { chatUsersRef.current = chatUsers; }, [chatUsers]);
  const chatIdRef = useRef(0);

  // chat_user_update broadcasts arrive whenever ANY user on the instance
  // retunes — on a busy instance that's several per second, and each
  // setChatUsers re-renders the entire screen tree (the historic CPU
  // killer). Maintain the list in the ref always; only touch React state
  // while the drawer is actually open (synced on open).
  const updateChatUsers = useCallback((fn: (prev: ChatUserRow[]) => ChatUserRow[]) => {
    chatUsersRef.current = fn(chatUsersRef.current);
    if (chatOpenRef.current) setChatUsers(chatUsersRef.current);
  }, []);

  // One-time heads-up about OWRX's profile model: pausing disconnects, and a
  // later reconnect resets the receiver to its server-side default profile/freq
  // (we can't persist server profile state across a fresh session without
  // hijacking it). Shown once per install when first connected to an OWRX server.
  useEffect(() => {
    if (!connected || (route.params.serverType ?? 'ubersdr') !== 'owrx') return;
    AsyncStorage.getItem('owrx_pause_warning_seen').then((seen: string | null) => {
      if (seen) return;
      AsyncStorage.setItem('owrx_pause_warning_seen', '1').catch(() => {});
      Alert.alert(
        'OpenWebRX — note on pausing',
        'OpenWebRX receivers use server-side profiles. If you pause from the lock screen, VibeSDR disconnects to free the receiver — and reconnecting resets it to the server’s default profile and frequency. (Locking the screen while playing keeps audio going; this only applies to an explicit pause.)',
        [{ text: 'Got it' }],
      );
    }).catch(() => {});
  }, [connected]);

  // Handler refs — the decoder-client effect below builds its callbacks once
  // per connect, but tune/mode/filter handlers are declared later in the file
  /** Did WE close the spectrum WS on background? False when the watch kept it
   *  alive, so the foreground path knows not to re-open a live socket. */
  const specPausedByBgRef = useRef(false);

  /** Re-open the spectrum for a watch that is demonstrably there.
   *
   *  Called on every watch message (it heartbeats every 4s), so a spectrum that was
   *  paused while the watch was actually watching heals itself rather than staying
   *  dead until some flag happens to change. */
  const wakeSpectrumForWatch = useCallback(() => {
    if (appActiveRef.current) return;         // phone's own screen governs
    const c = client.current; if (!c) return;
    if (specPausedByBgRef.current) {
      specPausedByBgRef.current = false;
      c.resumeSpectrum();
      c.setRate(WATCH_BG_DIVISOR);
      watchProvider.setSpecPaused(false);
    }
  }, []);

  /** Late-bound: zoomBy is declared further down. */
  const zoomByRef = useRef<((factor: number) => void) | null>(null);

  const onTuneHzRef    = useRef<((hz: number) => void) | null>(null);
  const onModeRef      = useRef<((m: SDRMode) => void) | null>(null);
  const onFilterBothRef = useRef<((low: number, high: number) => void) | null>(null);
  const onVtsJumpRef   = useRef<((d: 'left' | 'right') => void) | null>(null);
  const onSearchTuneRef = useRef<((hz: number, mode?: string | null, isBand?: boolean, voiceStep?: boolean) => void) | null>(null);

  // ── Media skip mode: lock-screen ⏮⏭ tune by step or jump bookmarks ───────
  // CarFM defaults to bookmark stepping so steering-wheel / ESP32 ⏮⏭ move
  // between presets (spec §5b); a stored user choice below still overrides it.
  const [mediaSkip, setMediaSkip] = useState<'step' | 'bookmark'>(
    route.params.carFm ? 'bookmark' : 'step');
  const mediaSkipRef = useRef(mediaSkip);
  useEffect(() => { mediaSkipRef.current = mediaSkip; }, [mediaSkip]);
  // Lock-screen ⏮⏭ step-tune for backends whose tuning lives in JS (OWRX/Kiwi):
  // native delegates via VibeSkip rather than tuning its own WS. Snaps to the
  // step grid (matching the native UberSDR path + the VFO drum). Registered in a
  // ref so the once-mounted native event listener calls the latest closure.
  // DAB mode: ⏮⏭ cycle the ensemble's programmes instead of tuning (VFO locked).
  // Reassigned each render so it sees the current programme list + selection.
  const dabSkipRef = useRef<((dir: 'left' | 'right') => void) | null>(null);
  dabSkipRef.current = (dir: 'left' | 'right') => {
    const c = client.current; if (!c || dabProgrammes.length === 0) return;
    const idx = dabProgrammes.findIndex((p) => p.id === activeDabId);
    const next = dir === 'right'
      ? (idx + 1) % dabProgrammes.length
      : (idx - 1 + dabProgrammes.length) % dabProgrammes.length;
    const id = dabProgrammes[next].id;
    c.setAudioServiceId?.(id);
    setActiveDabId(id);
  };
  const mediaStepSkipRef = useRef<((dir: 'left' | 'right') => void) | null>(null);
  mediaStepSkipRef.current = (dir: 'left' | 'right') => {
    const c = client.current; if (!c) return;
    // Whole-profile data modes (DAB, ADS-B, ISM…) have nothing to tune — the only
    // thing a VFO can do is drag you OFF the block and kill the decode.
    if (isWholeProfileMode(String(c.getStatus().mode))) return;
    const s = stepRef.current; if (!(s > 0)) return;
    const cur = c.getStatus().frequency;
    const snapped = dir === 'right'
      ? (Math.floor(cur / s) + 1) * s
      : (Math.ceil(cur / s) - 1) * s;
    const [loHz, hiHz] = c.caps.freqRange;
    const newHz = Math.max(loHz, Math.min(hiHz, snapped));
    if (newHz === cur) return;
    c.tune(newHz, undefined, { recenter: true });   // media-control skip = discrete jump
    setStatus((prev: SDRStatus) => ({ ...prev, frequency: newHz }));
  };
  useEffect(() => {
    AsyncStorage.getItem('lsv_media_skip').then((v: string | null) => {
      if (v === 'bookmark' || v === 'step') setMediaSkip(v);
    }).catch(() => {});
  }, []);
  const onMediaSkip = useCallback((m: 'step' | 'bookmark') => {
    setMediaSkip(m);
    AsyncStorage.setItem('lsv_media_skip', m).catch(() => {});
  }, []);
  // Push to native; re-push on reconnect (the Android service can be recreated)
  useEffect(() => {
    VibePowerModule?.setMediaSkipMode(mediaSkip);
  }, [mediaSkip, connected]);

  // ── Pause = disconnect / Play = reconnect ─────────────────────────────────
  // Pause drops the SDR (the server lets it go on suspend anyway) and Play does
  // a full reconnect. If that reconnect doesn't land within a few seconds (server
  // full / rate-limited) we flag it so the lock-screen card + an in-app banner
  // tell the user to open the app.
  const [reconnectFailedUi, setReconnectFailedUi] = useState(false);
  const connectedRef = useRef(false);
  useEffect(() => {
    connectedRef.current = connected;
    if (connected) { VibePowerModule?.setReconnectFailed?.(false); setReconnectFailedUi(false); }
    // Siri: when live, the intent emits the command now; otherwise it stashes it.
    VibePowerModule?.setVoiceConnected?.(connected);
  }, [connected]);

  // (Re)apply the network notch to the audio engine whenever the connection is up
  // or the toggle changes. Local sources are notched in the shim, not here.
  useEffect(() => {
    if (!isLocal && connected) VibePowerModule?.setNotch?.(netNotch);
  }, [connected, netNotch, isLocal]);

  // The squelch gate is a persistent native flag (iOS VibePowerModule is a
  // singleton). Make sure non-Kiwi sessions start open, and always release the
  // gate on unmount so a closed Kiwi squelch can't silence the next session.
  useEffect(() => {
    const setOpen = (NativeModules.VibePowerModule as { setSquelchOpen?: (o: boolean) => void })?.setSquelchOpen;
    if (!isKiwi) setOpen?.(true);
    return () => { kiwiSqOpenRef.current = true; setOpen?.(true); };
  }, [isKiwi]);

  // Car-connected flag (iOS car-audio route / Android Auto client), updated by
  // the VibeCarConnected native event. Band-aware auto mode/step no longer gates
  // on this (it now fires for all non-hands-on tuning — see vtsCheck); kept for
  // potential car-specific behaviour later.
  const carConnected = useRef(false);

  // Chat drawer doesn't fit landscape even on a 17 Pro Max (let alone SE) —
  // the button stays live for the unread pulse, but opening demands portrait.
  const [chatRotateHint, setChatRotateHint] = useState(false);
  const chatHintTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const showChatRotateHint = useCallback(() => {
    setChatRotateHint(true);
    if (chatHintTimer.current) clearTimeout(chatHintTimer.current);
    chatHintTimer.current = setTimeout(() => setChatRotateHint(false), 2500);
  }, []);
  useEffect(() => () => {
    if (chatHintTimer.current) clearTimeout(chatHintTimer.current);
  }, []);

  const openChat = useCallback(() => {
    if (isLandscape) { showChatRotateHint(); return; }
    // Prime the chat stream (history replay arrives quiet) even before join
    decoderClient.current?.subscribeChat();
    setChatUsers(chatUsersRef.current);  // ref is live; state only while open
    setChatOpen(true);
    setChatUnread(false);
  }, [isLandscape, showChatRotateHint]);

  const closeChat = useCallback(() => {
    setChatOpen(false);
  }, []);

  // Rotating to landscape with chat open → close it and explain why
  useEffect(() => {
    if (isLandscape && chatOpen) {
      setChatOpen(false);
      showChatRotateHint();
    }
  }, [isLandscape, chatOpen, showChatRotateHint]);

  // Android back gesture/button: CONSUME it on this screen (iOS parity —
  // gestureEnabled:false on the stack). Edge swipes while working the VFO
  // drum were popping to the picker / exiting the app. Close transient UI
  // if open; leaving the instance is the menu's ← BACK button. RN Modals
  // (menu, maps, browser) intercept back themselves before this fires.
  const chatOpenRef = useRef(false);
  useEffect(() => { chatOpenRef.current = chatOpen; }, [chatOpen]);
  useEffect(() => {
    if (Platform.OS !== 'android') return;
    const sub = BackHandler.addEventListener('hardwareBackPress', () => {
      if (chatOpenRef.current) setChatOpen(false);
      return true;  // consumed — never pop the screen from a gesture
    });
    return () => sub.remove();
  }, []);

  // ── Decoder ───────────────────────────────────────────────────────────────

  const [activeDecoder,  setActiveDecoder]  = useState<DecoderType>(null);
  const [decoderText,    setDecoderText]    = useState('');
  const [decoderStatus,  setDecoderStatus]  = useState('listening…');
  const [decoding,       setDecoding]       = useState(false);
  const [pillBottom,     setPillBottom]     = useState(200); // updated by pill layout
  const [rootH,          setRootH]          = useState(0);   // measured root height
  const pillYRef = useRef<number | null>(null);
  // Re-derive pillBottom once the root measures (or rotates) — the pill's
  // own onLayout may have fired first with a stale height
  useEffect(() => {
    if (rootH > 0 && pillYRef.current != null) setPillBottom(rootH - pillYRef.current);
  }, [rootH]);

  // Real decoders — UberSDR server audio extensions over /ws/dxcluster,
  // exactly as the confirmed-working skin wires them (see DecoderClient.ts).
  // Uses the SAME session uuid as audio so the extension taps this session's
  // demodulated stream server-side. DEC_SIM fake data is gone.
  const decoderClient   = useRef<DecoderClient | null>(null);
  const decoderImageRef = useRef<DecoderImageHandle | null>(null);
  const activeDecRef    = useRef<DecoderType>(null);

  // Decoder transport base. Local/UberSDR serve /ws/dxcluster themselves; Kiwi
  // (no dxcluster) gets a native decoder sidecar fed its audio, so we point the
  // DecoderClient at that localhost service instead.
  const [decoderBase, setDecoderBase] = useState<string | null>(isKiwi ? null : baseUrl);
  useEffect(() => {
    if (!isKiwi) { setDecoderBase(baseUrl); return; }
    let cancelled = false;
    (NativeModules as any).VibeLocalSDR?.startDecoderService?.()
      .then((port: number) => { if (!cancelled && port > 0) setDecoderBase(`ws://127.0.0.1:${port}`); })
      .catch(() => {});
    return () => {
      cancelled = true;
      (NativeModules as any).VibeLocalSDR?.stopDecoderService?.();
    };
  }, [isKiwi, baseUrl]);

  useEffect(() => {
    if (!decoderBase) return;
    const dc = new DecoderClient(decoderBase, sessionUuid, {
      onText: (text: string) => {
        setDecoding(true);
        setDecoderText((prev: string) => {
          const next = prev + text;
          return next.length > 3000 ? next.slice(next.length - 3000) : next;
        });
      },
      onStatus: (s: string)  => setDecoderStatus(s),
      onDot:    (d)          => setDecoding(d === 'active' || d === 'rx'),
      // WEFAX/SSTV — drive the panel's image canvas (skin canvas parity).
      // WEFAX lines are greyscale, SSTV lines are RGB; route by active decoder.
      onImageStart: (w: number, h: number) => decoderImageRef.current?.imageStart(w, h),
      onImageLine:  (ln: number, w: number, px: Uint8Array) => {
        if (activeDecRef.current === 'sstv') decoderImageRef.current?.sstvLine(ln, w, px);
        else                                 decoderImageRef.current?.wefaxLine(ln, w, px);
      },
      onImageDone: ()            => decoderImageRef.current?.imageDone(),
      onError:     (msg: string) => setDecoderStatus('error: ' + msg),
      onSpot: (s) => {
        if (s.kind !== spotsKindRef.current) return;
        spotBufRef.current.push(s); // flushed by the 400ms tick — no setState here
      },
      // ── Chat (same WS) — replayed history arrives quiet (no unread pulse),
      //    duplicates are dropped in DecoderClient before reaching here ──────
      onChatMessage: (user: string, text: string, ts: string, isHistory: boolean) => {
        const own = user === myCallsignRef.current;
        setChatMessages((prev: ChatMessage[]) => [
          ...prev.slice(-99),
          { id: 'c' + String(++chatIdRef.current),
            type: own ? 'own' : 'other', user, text, ts: chatTs(ts) },
        ]);
        if (!isHistory && !own && !chatMutedRef.current) {
          setChatOpen((open: boolean) => {
            if (!open) setChatUnread(true);
            return open;
          });
        }
      },
      onChatJoined: (username: string, isHistory: boolean) => {
        if (!isHistory) addChatMsg(mkMsg('system', `${username} joined the chat`), true);
        decoderClient.current?.requestChatUsers();
      },
      onChatLeft: (username: string, isHistory: boolean) => {
        if (!isHistory) addChatMsg(mkMsg('system', `${username} left the chat`), true);
        updateChatUsers((prev: ChatUserRow[]) =>
          prev.filter((u: ChatUserRow) => u.username !== username));
        setSyncedUser((prev: string | null) => (prev === username ? null : prev));
      },
      onChatUsers: (users: ChatUserRow[]) => updateChatUsers(() => users),
      onChatUserUpdate: (u: ChatUserRow) => {
        updateChatUsers((prev: ChatUserRow[]) => {
          const i = prev.findIndex((x: ChatUserRow) => x.username === u.username);
          if (i < 0) return [...prev, u];
          const next = [...prev];
          next[i] = { ...next[i], ...u };
          return next;
        });
        // Following this user → mirror their tune
        if (u.username === syncedUserRef.current) applyChatSync(u);
      },
      onChatError: (msg: string) => {
        addChatMsg(mkMsg('system', `⚠ ${msg}`), true);
        // Join rejected (taken/invalid/profane) → back to the join flow
        if (/username|callsign/i.test(msg)) {
          setMyCallsign(null);
          AsyncStorage.removeItem('lsv_chat_callsign:' + baseUrl).catch(() => {});
        }
      },
    }, password);
    decoderClient.current = dc;

    // Saved callsign → auto-join on connect (skin autoLogin parity); the
    // chat stream then stays live for unread pulses without opening the
    // drawer. Runs here so the client exists before joinChat fires.
    let cancelled = false;
    AsyncStorage.getItem('lsv_chat_callsign:' + baseUrl).then((cs: string | null) => {
      if (cancelled || !cs) return;
      setMyCallsign(cs);
      dc.joinChat(cs);
    }).catch(() => {});

    return () => { cancelled = true; dc.destroy(); decoderClient.current = null; };
  // decoderBase is async for Kiwi (null until the native decoder sidecar's port
  // arrives from startDecoderService) — it MUST be a dep, or the DecoderClient is
  // never (re)built when the port lands, leaving Kiwi decoders/spots with no output.
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [baseUrl, sessionUuid, decoderBase]);

  // Selected decoder mode — persists across stop/start (skin _mode vs _on)
  const [selDecoder, setSelDecoder] =
    useState<'rtty'|'navtex'|'wefax'|'sstv'|'morse'|'whisper'|null>(null);

  // Digital/CW spots — share the dxcluster WS; mutually exclusive with decoders.
  // Spots are BUFFERED in a ref and flushed to state on a 400ms tick: the
  // server replays its whole buffer on subscribe (hundreds of messages in a
  // burst) and a setState per spot re-renders the entire screen tree — that's
  // what stuttered the waterfall. The skin never had this because DOM rows
  // append incrementally.
  const [spotsKind, setSpotsKind] = useState<SpotsKind | null>(null);
  const [spots,     setSpots]     = useState<SpotRow[]>([]);
  const spotsKindRef  = useRef<SpotsKind | null>(null);
  const spotBufRef    = useRef<SpotRow[]>([]);
  const spotTimerRef  = useRef<ReturnType<typeof setInterval> | null>(null);
  // Receiver position for FT8 spot distances (+ the on-device-decoder map).
  // Kiwi: server gps=(lat,lon) via onReceiverLoc. Local hardware: the phone's GPS
  // (same permission as instance-list distance sorting); null until resolved.
  const recvLocRef = useRef<{ lat: number; lon: number } | null>(null);
  const [recvLoc, setRecvLoc] = useState<{ lat: number; lon: number } | null>(null);
  useEffect(() => {
    if (!isLocal) return;
    let cancelled = false;
    getUserLocation().then(loc => {
      if (cancelled || !loc) return;
      recvLocRef.current = loc; setRecvLoc(loc);
    }).catch(() => {});
    return () => { cancelled = true; };
  }, [isLocal]);

  const stopSpotFlush = useCallback(() => {
    if (spotTimerRef.current) { clearInterval(spotTimerRef.current); spotTimerRef.current = null; }
    spotBufRef.current = [];
  }, []);

  const startSpotFlush = useCallback(() => {
    stopSpotFlush();
    spotTimerRef.current = setInterval(() => {
      const buf = spotBufRef.current;
      if (buf.length === 0) return;
      spotBufRef.current = [];
      // On-device decoder (Local/Kiwi) sends a TX grid + callsign but no distance
      // or country — derive both here. UberSDR spots already carry them.
      const rx = recvLocRef.current;
      for (const s of buf) {
        if (s.distKm == null && s.grid && rx) s.distKm = distanceKmToGrid(rx, s.grid);
        if (!s.country && s.call) s.country = countryForCallsign(s.call);
      }
      buf.reverse(); // arrival order oldest→newest; display newest first
      setSpots((prev: SpotRow[]) => {
        const next = buf.concat(prev);
        return next.length > 200 ? next.slice(0, 200) : next;
      });
    }, 400);
  }, [stopSpotFlush]);

  useEffect(() => stopSpotFlush, [stopSpotFlush]); // clear on unmount

  const openDecoder = useCallback((type: DecoderType) => {
    setActiveDecoder(type);
    activeDecRef.current = type;
    setDecoderText('');
    setDecoderStatus('listening…');
    setDecoding(false);
    decoderImageRef.current?.reset();
    if (!type) return;
    if (type === 'ft8') {
      // FT8 is not an audio extension — it's served instance-wide via the
      // Digital Spots feed (decoder_feed / digi spots APIs), not per-session.
      setDecoderStatus('FT8 arrives via Digital Spots — see Server Extensions');
      return;
    }
    decoderClient.current?.start(type);
  }, []);

  const closeDecoder = useCallback(() => {
    decoderClient.current?.stop();
    decoderImageRef.current?.reset();
    setActiveDecoder(null);
    activeDecRef.current = null;
    setDecoding(false);
    setDecoderText('');
    setDecoderStatus('listening…');
  }, []);

  const stopSpots = useCallback(() => {
    decoderClient.current?.stopSpots();
    spotsKindRef.current = null;
    setSpotsKind(null);
    stopSpotFlush();
  }, [stopSpotFlush]);

  // Menu decoder toggle — skin semantics: same mode running → stop (selection
  // kept, settings stay visible); otherwise select + start. Menu stays open.
  // Spots and audio decoders share the panel — starting one stops the other.
  const onDecToggle = useCallback((m: 'rtty'|'navtex'|'wefax'|'sstv'|'morse'|'whisper') => {
    if (activeDecRef.current === m) {
      closeDecoder();
      setSelDecoder(m); // closeDecoder clears running state; keep selection
    } else {
      stopSpots();
      setSelDecoder(m);
      openDecoder(m);
    }
  }, [closeDecoder, openDecoder, stopSpots]);

  // Spots toggle (menu Server Extensions DIGITAL/CW — skin lsvSpots)
  const onSpotsToggle = useCallback((k: SpotsKind) => {
    if (spotsKindRef.current === k) {
      stopSpots();
    } else {
      closeDecoder();
      setSpots([]);
      spotsKindRef.current = k;
      setSpotsKind(k);
      startSpotFlush();
      decoderClient.current?.startSpots(k);
    }
  }, [closeDecoder, stopSpots, startSpotFlush]);

  // RTTY settings — applying requires a re-attach (server reads params at attach)
  const [rttySettings, setRttySettings] = useState<RttySettings>({ ...RTTY_PRESETS.ham });
  const onRttySettings = useCallback((s: RttySettings) => {
    setRttySettings(s);
    const dc = decoderClient.current;
    if (!dc) return;
    dc.rttySettings = { ...s };
    if (activeDecRef.current === 'rtty') {
      setDecoderStatus('re-attaching…');
      dc.start('rtty');
    }
  }, []);

  // Morse quality — client-side filter in DecoderClient, no re-attach needed
  const [morseQuality, setMorseQuality] = useState<MorseQuality>('all');
  const onMorseQuality = useCallback((q: MorseQuality) => {
    setMorseQuality(q);
    if (decoderClient.current) decoderClient.current.morseQuality = q;
  }, []);

  // WEFAX LPM — same re-attach rule
  const [wefaxLpm, setWefaxLpm] = useState(120);
  const onWefaxLpm = useCallback((lpm: number) => {
    setWefaxLpm(lpm);
    const dc = decoderClient.current;
    if (!dc) return;
    dc.wefaxLpm = lpm;
    if (activeDecRef.current === 'wefax') {
      setDecoderStatus('re-attaching…');
      dc.start('wefax');
    }
  }, []);

  // ── Display style — wired to ThemeContext so the whole app re-renders ────────
  const { themeName, setTheme } = useTheme();
  const displayStyle = themeName;
  const handleDisplayStyle = useCallback((s: 'amber' | 'white') => {
    setTheme(s);
  }, [setTheme]);

  // ── Media control tune events (iOS lock screen) ───────────────────────────

  const dspSeen = useRef(false);
  useEffect(() => {
    // Both platforms expose VibePowerModule with the same events now
    const emitter = new NativeEventEmitter(NativeModules.VibePowerModule);
    const sub = emitter.addListener('VibeTuned', (e: { frequency: number; mode: string }) => {
      const c = client.current;
      c?.syncFrequency(e.frequency, e.mode as SDRMode);
      setStatus((prev: SDRStatus) => ({ ...prev, frequency: e.frequency, ...(e.mode ? { mode: e.mode as SDRMode } : {}) }));
      // Media-control skips tune blind from the lock screen / car stereo —
      // recentre the view on EVERY skip so the VFO stays centred and the
      // waterfall moves around it (drum-style; Stuart's design). Skips made
      // while the spectrum WS is paused (locked) land in view.centerHz and
      // the onopen view-restore replays them on unlock.
      c?.pan(e.frequency);
    });
    const subMute = emitter.addListener('VibeMuted', (e: { muted: boolean }) => {
      setIsMuted(!!e.muted);
      // OWRX: pause releases the lock-screen controls (native) and disconnects —
      // there's no play-to-reconnect because an OWRX reconnect resets the server
      // profile. Close the WS and show the in-app reconnect prompt so the user
      // reconnects deliberately (the warning explains the reset).
      if (e.muted && (route.params.serverType ?? 'ubersdr') === 'owrx') {
        client.current?.disconnectSocket?.();
        setDataSaverOff(true);
      }
    });
    // radiod channel SNR (basebandPower − noiseDensity); −30 corrects the +30 dB
    // audio-stream floor offset so the meter reads honest dB.
    const subSig = emitter.addListener('VibeSignal', (e: { snr: number }) => {
      audioSnrRef.current = e.snr - 30;
      lastAudioAtRef.current = Date.now();
    });
    // Native ⏮⏭ defer to JS. Bookmark mode jumps the station list; step mode
    // (used by OWRX/Kiwi, whose tuning lives in JS) snaps by the tune step.
    const subSkip = emitter.addListener('VibeSkip', (e: { direction: string }) => {
      const dir = e.direction === 'prev' ? 'left' : 'right';
      // DAB: cycle programmes within the ensemble (the VFO is locked there).
      if (String(client.current?.getStatus().mode) === 'dab') { dabSkipRef.current?.(dir); return; }
      if (mediaSkipRef.current === 'bookmark') onVtsJumpRef.current?.(dir);
      else mediaStepSkipRef.current?.(dir);
    });
    // Car audio route / Android Auto client connect — gates band-aware auto
    // mode/step (handheld use is never auto-switched).
    const subCar = emitter.addListener('VibeCarConnected', (e: { connected: boolean }) => {
      carConnected.current = !!e.connected;
    });
    // Car browse list pick (Android Auto / CarPlay) — tune via the shared
    // onSearchTune path so band-aware mode/step + region logic stay in one place.
    const subCarTune = emitter.addListener('VibeCarTune',
      (e: { frequency: number; mode?: string | null; isBand?: boolean }) => {
        onSearchTuneRef.current?.(e.frequency, e.mode ?? null, !!e.isBand);
      });
    // Siri voice command — native passes the spoken text + kind; JS resolves and
    // applies (tune: frequency/bookmark/band via searchStations + band mode/step;
    // mode: synonyms; step: nearest supported rate).
    const subVoice = emitter.addListener('VibeVoiceQuery', (e: { query: string; kind?: string }) => {
      if (e.kind === 'step') { const s = parseVoiceStep(e.query); if (s != null) setStep(s); return; }
      if (e.kind === 'mode') { const m = parseVoiceMode(e.query); if (m) onModeRef.current?.(m); return; }
      const r = resolveVoiceQuery(e.query, vtsBookmarks.current, searchBandsRef.current);
      if (r) onSearchTuneRef.current?.(r.hz, r.mode, r.isBand, true);
    });
    // Data saver dropped the stream — tear down the spectrum too (native already
    // closed the audio WS) and surface the reconnect prompt.
    const subDsOff = emitter.addListener('VibeDataSaverDisconnect', () => {
      setDataSaverOff(true);
      // UberSDR: native already closed the audio WS; just pause the spectrum WS.
      // OWRX/Kiwi: close the WS to free the server slot but KEEP the native audio
      // session (so the lock-screen disconnect card shows); a fresh adapter is
      // built on resume via fullReconnect. (destroy() would drop the card.)
      if ((route.params.serverType ?? 'ubersdr') === 'ubersdr') client.current?.pauseSpectrum();
      else client.current?.disconnectSocket?.();
    });
    // Resume from a data-saver disconnect (Play / unmute / banner tap). Reopening
    // the old session's sockets lands in a broken half-state (frozen waterfall +
    // zoom, no audio), so do a FULL from-scratch reconnect with a fresh uuid.
    const subDsOn = emitter.addListener('VibeDataSaverResume', () => {
      setDataSaverOff(false);
      setIsMuted(false);
      fullReconnect();
    });
    // The OS says the network path moved under us (WiFi→cellular, or a cellular IP
    // change on cell handover). Neither sends a FIN or an RST, so every socket on
    // the old flow is now a zombie that will sit OPEN forever. Native has already
    // treated the audio WS as suspect; the spectrum WS is JS's to revive, and it
    // has the same zombie on the same dead flow. Rate-limited inside the client.
    const subPath = emitter.addListener('VibeNetworkPathChanged', () => {
      client.current?.forceResubscribe?.('network-path-change');
    });
    // The iPhone's SYSTEM volume changed — by the hardware buttons, Control Centre, a
    // headset's own rocker, or by the watch itself. Mirror it to the wrist so the two
    // can never disagree. (iOS quantises to 1/16 steps, so what arrives here after a
    // watch-initiated set is the SNAPPED value — which is the truth the watch adopts.)
    const subVol = emitter.addListener('VibeVolume', (e: { volume: number }) => {
      sysVolRef.current = e.volume;
      watchProvider.setVolume(e.volume);
    });
    // Seed it. The observer emits the current volume when it starts, but that can land
    // before this listener exists — and KVO only fires on CHANGE thereafter, so without
    // an explicit read the wrist would show a default until the user happened to touch
    // something. Which is the exact class of bug this whole part exists to kill.
    (NativeModules.VibePowerModule as { getSystemVolume?: () => Promise<number> })
      ?.getSystemVolume?.()
      .then((v) => { sysVolRef.current = v; watchProvider.setVolume(v); })
      .catch(() => {});
    // Server-NR protocol messages arrive as text on the native audio WS
    const subWs = emitter.addListener('VibeWsText', (e: { text: string }) => {
      let msg: { type?: string; info?: Record<string, unknown> };
      try { msg = JSON.parse(e.text); } catch { return; }
      if (!msg || typeof msg.type !== 'string') return;
      const info = (msg.info ?? msg) as Record<string, unknown>;
      if (msg.type === 'dsp_filters') {
        dspSeen.current = true;
        const filters = (info.available ? (info.filters as DspFilterDesc[] | undefined) : []) ?? [];
        setDspFilters(filters);
        if (filters.length) {
          const name = filters.some((f: DspFilterDesc) => f.name === dspFilterRef.current)
            ? dspFilterRef.current : filters[0].name;
          setServerDspFilter(name);
          if (Object.keys(dspParamsRef.current).length === 0) {
            applyDspParams(dspDefaults(filters.find((f: DspFilterDesc) => f.name === name)));
          }
        }
      } else if (msg.type === 'dsp_status') {
        setServerDspEnabled(!!info.enabled);
        if (typeof info.filter === 'string' && info.filter) setServerDspFilter(info.filter);
        if (info.enabled && info.params && typeof info.params === 'object') {
          const merged = { ...dspParamsRef.current };
          for (const [k, v] of Object.entries(info.params as Record<string, unknown>)) {
            merged[k] = String(v);
          }
          applyDspParams(merged);
        }
      } else if (msg.type === 'dsp_error') {
        setDspError(String(info.error ?? 'DSP error'));
        setTimeout(() => setDspError(null), 4000);
      }
    });
    return () => {
      sub.remove(); subMute.remove(); subSig.remove(); subSkip.remove(); subWs.remove();
      subCar.remove(); subCarTune.remove(); subVoice.remove(); subDsOff.remove(); subDsOn.remove(); subPath.remove(); subVol.remove();
    };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Discover server-NR filters once the native audio WS is up (it opens on
  // mount; retries cover slow connects). No dsp_filters reply / available:
  // false ⇒ section stays hidden.
  useEffect(() => {
    const tries = [2000, 6000, 12000].map((ms) => setTimeout(() => {
      if (!dspSeen.current) sendAudioCmd({ type: 'get_dsp_filters' });
    }, ms));
    return () => tries.forEach(clearTimeout);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // ── Connect ───────────────────────────────────────────────────────────────

  useEffect(() => {
    destroyed.current = false;
    const c = createBackend(route.params.serverType ?? 'ubersdr', baseUrl, sessionUuid, {
      // (callbacks below; bypass password rides every WS URL)
      onConnect:    () => { if (!destroyed.current) { setConnected(true); setServerLost(false); setServerBusy(false); setConnLost(false); if (connLostTimer.current) { clearTimeout(connLostTimer.current); connLostTimer.current = null; } resumingRef.current = false; if (reinitTimer.current) { clearTimeout(reinitTimer.current); reinitTimer.current = null; } setReinit(false); setSpecFailed(false); } },
      onDisconnect: () => { if (!destroyed.current) setConnected(false); },
      // VibeServer: the serving device's tuner gains → drive the gain slider (a
      // remote client can't query the hardware natively).
      onHwGains: (gains: number[]) => { if (!destroyed.current && gains.length) setHwGains(gains); },
      onHwRates: (rates: number[]) => { if (!destroyed.current && rates.length) setHwServerRates(rates); },
      onHwLockedRate: (r: number) => { if (!destroyed.current) setHwLockedRate(r); },
      onServerLost: () => {
        // OWRX server crashed/restarted. Keep the app alive, free the dead audio
        // engine, and surface the wait-and-reconnect prompt (no auto-reconnect —
        // the server is usually still restarting).
        if (destroyed.current) return;
        setServerLost(true);
        (VibePowerModule as any)?.stopExternalAudio?.();
      },
      onServerBusy: () => {
        if (destroyed.current) return;
        setServerBusy(true);
        (VibePowerModule as any)?.stopExternalAudio?.();
      },
      onReceiverLon: (lon) => { if (!destroyed.current) setRecvLon(lon); },
      onReceiverLoc: (lat, lon) => { recvLocRef.current = { lat, lon }; if (!destroyed.current) setRecvLoc({ lat, lon }); },
      onReconnecting: (busy: boolean) => {
        // Tell the WRIST a recovery is under way. There are two links in series and
        // they fail independently; from the watch, "the phone is healing its server
        // link" and "the phone is dead" look identical. Only the phone knows, so
        // only the phone can say — otherwise the watch throws a black overlay over
        // a recovery that is working perfectly well.
        watchProvider.setReconnecting(busy);
      },
      onLink: (q) => {
        if (destroyed.current) return;
        const b = meterBus.current;
        // On the rtl_tcp path the backend's FFT-timing quality is measured AFTER the
        // jitter buffer, so it reads green while the network is starving the buffer.
        // Clamp it with the real network health — a bad link can only make it worse.
        const eff = Math.min(q, netLinkRef.current) as 0|1|2|3;
        b.emit({ ...b.value, link: eff });
        // The PHONE↔SERVER hop's health, sent to the wrist so its warning pill can
        // name which of the two hops is rough rather than shrugging "LINK ROUGH".
        watchProvider.setLinkQuality(eff);
        // UberSDR auto-reconnects silently — without a cue the app just looks
        // frozen when the link drops (e.g. the instance reboots). But the spectrum
        // is deliberately paused on minimise/resume, which briefly starves the
        // link to 0 with audio still fine — so DEBOUNCE: only pop after a sustained
        // drop, and cancel the instant the link recovers. OWRX/Kiwi use serverLost.
        if ((route.params.serverType ?? 'ubersdr') === 'ubersdr' && appActiveRef.current) {
          if (q === 0) {
            // While reinitialising after a resume the "reinit" notice owns the
            // screen — don't arm the connection-lost popup underneath it.
            if (!connLostTimer.current && !resumingRef.current) {
              connLostTimer.current = setTimeout(() => {
                connLostTimer.current = null;
                if (!destroyed.current) setConnLost(true);
              }, 3000);
            }
          } else {
            // Frames flowing again — recovery. Clear both notices.
            if (connLostTimer.current) { clearTimeout(connLostTimer.current); connLostTimer.current = null; }
            setConnLost(false);
            if (resumingRef.current) {
              resumingRef.current = false;
              if (reinitTimer.current) { clearTimeout(reinitTimer.current); reinitTimer.current = null; }
              setReinit(false);
            }
            // Spectrum recovered on its own (e.g. user hit reconnect) → drop the
            // failure popup too.
            setSpecFailed(false);
          }
        }
      },
      onStatus:     (s) => { if (!destroyed.current) setStatus(s); },
      onSMeter:     (dbm) => { if (!destroyed.current) { owrxSmeterRef.current = dbm; if (isKiwi) evalKiwiSquelch(dbm); } },
      onProfiles:   (list) => { if (!destroyed.current) setProfiles(list); },
      onSdrUsage:   (m) => { if (!destroyed.current) setSdrUsage(m); },
      onClients:    (n) => { if (!destroyed.current) setClientCount(n); },
      onChatEnabled: (en) => { if (!destroyed.current) setChatEnabled(en); },
      onServerInfo: (info) => { if (!destroyed.current) { setServerLabel(info.name); setServerVersion(info.version || null); } },
      onChatMessage: (name, text) => {
        // OWRX basic text chat (name + text). Server echoes our own back, so
        // don't local-echo on send — render the broadcast and mark own by name.
        if (destroyed.current) return;
        const own = name === myCallsignRef.current;
        setChatMessages((prev: ChatMessage[]) => [
          ...prev.slice(-99),
          { id: 'c' + String(++chatIdRef.current), type: own ? 'own' : 'other', user: name, text, ts: chatTs(new Date().toISOString()) },
        ]);
        if (!own && !chatMutedRef.current && !chatOpenRef.current) setChatUnread(true);
      },
      onModes:      (list) => { if (!destroyed.current) setServerModes(list); },
      onServerDspDefaults: (d) => {
        // Adapter already applied these to the demod; bump seq so the menu re-syncs
        // its sliders even when the new profile presets the same value as before.
        if (!destroyed.current) setOwrxDspDefaults((p) => ({ ...d, seq: p.seq + 1 }));
      },
      onBookmarks:  (list) => {
        // OWRX server bookmarks/dial markers (over the WS) → same path as
        // UberSDR's fetched bookmarks: VTS station readout + search bar.
        if (!destroyed.current) setServerBookmarks(list.map((b) => ({ name: b.name, frequency: b.frequency, mode: b.mode, repeater: b.repeater, source: 'server' as const })));
      },
      onAircraft: (list) => { if (!destroyed.current) setAircraft(list); },

      onDecoderText: (line, replace) => {
        // OWRX server-side text decoders (Packet/POCSAG/ADSB/…) → the decoder
        // text panel. `replace` (ADS-B live list) supersedes the buffer.
        if (destroyed.current) return;
        // Auto-open the panel if decode output arrives without a manual pick
        // (e.g. a profile whose start_mod is a standalone decoder like ADSB).
        if (!activeDecRef.current) {
          const dec = (client.current as any)?.getSecondaryDecoder?.() ?? null;
          const dt: DecoderType = dec === 'sstv' ? 'sstv' : dec === 'fax' ? 'wefax' : dec ? (dec as unknown as DecoderType) : null;
          if (dt) { activeDecRef.current = dt; setActiveDecoder(dt); }
        }
        setDecoding(true);
        if (replace) { setDecoderText(line); return; }
        // Append raw — the adapter newline-terminates records and char-stream
        // decoders (RTTY/CW) carry their own line breaks.
        setDecoderText((prev: string) => {
          const next = prev + line;
          return next.length > 4000 ? next.slice(next.length - 4000) : next;
        });
      },
      onDecoderImage: (ev) => {
        // OWRX decodes SSTV/Fax server-side and streams scanlines — paint them
        // on the SAME decoder canvas UberSDR uses (Fax → 'wefax' greyscale path).
        if (destroyed.current) return;
        const dt: DecoderType = ev.kind === 'sstv' ? 'sstv' : 'wefax';
        if (activeDecRef.current !== dt) { activeDecRef.current = dt; setActiveDecoder(dt); }
        if (ev.phase === 'start') { decoderImageRef.current?.imageStart(ev.width, ev.height); setDecoderStatus(`receiving ${ev.width}x${ev.height}`); }
        else if (ev.phase === 'line') {
          if (ev.kind === 'sstv') decoderImageRef.current?.sstvLine(ev.line, ev.width, ev.pixels);
          else                    decoderImageRef.current?.wefaxLine(ev.line, ev.width, ev.pixels);
        } else { decoderImageRef.current?.imageDone(); }
      },
      onMetadata:   (meta) => {
        if (destroyed.current) return;
        // RDS (FM) / DAB labels feed the SAME station display as bookmarks (VTS),
        // so a live station name shows uniformly regardless of source.
        liveStationRef.current = meta.stationName ?? '';
        liveBadgeRef.current = meta.badge;
        setLiveStation({ name: meta.stationName, text: meta.text, badge: meta.badge, countryIso: meta.countryIso, pi: meta.pi });
        if (typeof meta.stereo === 'boolean') setFmStereo(meta.stereo);
        // meta.programmes is the full cached list (DAB) or [] (explicit clear);
        // RDS messages omit it entirely (undefined) → leave the picker untouched.
        if (meta.programmes) {
          setDabProgrammes(meta.programmes);
          if (meta.ensemble) setDabEnsemble(meta.ensemble);
          // Mirror the server's default (first programme) so the picker reflects
          // what's actually playing until the user picks another.
          setActiveDabId((cur) => meta.programmes!.some((p) => p.id === cur)
            ? cur : (meta.programmes![0]?.id ?? 0));
          // Auto-apply this station's remembered speed correction.
          if (meta.stationName) applyDabStation(meta.ensemble ?? '', meta.stationName);
        }
      },
      onSpectrum:   (newBins, s) => {
        if (destroyed.current) return;
        // Waterfall/spectrum render imperatively — no React state per frame.
        wfFrameSink.current?.(newBins, s);
        // The watch feeds off the RAW frame, not the waterfall component: the
        // primary use case is a locked phone in a pocket, and the Skia tree is
        // unmounted by then. Plain JS + one bridge call, so it's safe to keep
        // running while backgrounded. No-ops when no watch is watching.
        watchProvider.onSpectrum(newBins, {
          centerHz:   s.centerHz,
          bwHz:       s.bwHz,
          tuneHz:     s.frequency,
          filterLow:  s.bandwidthLow,
          filterHigh: s.bandwidthHigh,
        });
        // Geometry/status drives the React overlay (band plan, readouts) —
        // only update when something actually changed (settled frames don't).
        // Epsilon gate: radiod's per-frame frequency stamps can jitter ±1Hz —
        // exact comparison leaked ~3-5 full-tree renders/s while settled
        // (render-counter diagnostic 2026-06-11). Sub-2Hz wobble is invisible
        // at any usable span; real changes pass untouched.
        setStatus((prev: SDRStatus) =>
          Math.abs(prev.centerHz - s.centerHz) < 2 &&
          Math.abs(prev.bwHz - s.bwHz) < 2 &&
          prev.frequency === s.frequency && prev.mode === s.mode &&
          prev.bandwidthLow === s.bandwidthLow && prev.bandwidthHigh === s.bandwidthHigh &&
          prev.binCount === s.binCount &&
          Math.abs(prev.binBandwidth - s.binBandwidth) < 1e-6
            ? prev : s);
        // ── Derive signal level + SNR from bins ────────────────────────────
        // Full data rate (~10Hz) — updates only re-render the two meter leaf
        // widgets via the bus, so there's no need to throttle anymore.
        // Find peak bin power in the current bandwidth window
        if (newBins.length > 0) {
          const len = newBins.length;
          // Peak in the audio passband window — feeds the dBFS / S-meter modes.
          const bwFrac = Math.min(1, (s.bandwidthHigh - s.bandwidthLow) / Math.max(1, s.bwHz));
          const half = Math.floor((bwFrac * len) / 2);
          const mid = Math.floor(len / 2);
          let peak = -200;
          for (let i = Math.max(0, mid - half); i <= Math.min(len - 1, mid + half); i++) {
            if (newBins[i] > peak) peak = newBins[i];
          }
          // SNR comes from radiod's channel status (audioSnrRef), NOT the spectrum
          // — the demodulator's own measurement of the tuned channel, so it's
          // independent of zoom (this is how UberSDR's meter works). 0 until the
          // first reading lands.
          let snrDb = audioSnrRef.current ?? 0;
          // Local SDR has no radiod SNR feed, so derive it from the spectrum:
          // passband peak minus the noise floor (mean of all bins ≈ the floor
          // for a mostly-empty spectrum). Cheap (no per-frame sort) and gives a
          // meaningful, zoom-tolerant reading.
          if (isLocal) {
            let sum = 0;
            for (let i = 0; i < len; i++) sum += newBins[i];
            const floor = sum / len;
            snrDb = Math.max(0, peak - floor);
          }
          // OWRX exposes a real channel S-meter (dBm) over the control WS but no
          // SNR. When present it's the honest absolute level source (and, lacking
          // an SNR feed, drives the bar in every mode); otherwise fall back to
          // the spectrum-derived peak as before.
          const owrxDbm = owrxSmeterRef.current;
          const levelDbm = owrxDbm ?? peak;
          // Bar source follows the meter mode: SNR uses the compression curve
          // (sigNorm, calibrated for honest 0–50 dB); S-meter/dBFS use the
          // absolute level mapping off the dBm level. OWRX's smeter dB spans
          // roughly −110 (noise) … −10 (strong), a different scale to UberSDR's
          // spectrum, so it gets its own linear mapping.
          const norm = owrxDbm != null
            ? Math.max(0, Math.min(1, (owrxDbm + 110) / 100))
            : signalModeRef.current === 'snr'
              ? sigNorm(snrDb)
              : Math.max(0, Math.min(1, (peak + 130) / 90));
          // Skin-feel smoothing rescaled for 10Hz updates (the skin's 0.55/0.18
          // alphas assumed its ~60Hz rAF loop — at 10Hz they felt sluggish).
          const sm = meterSmooth.current;
          sm.level += (norm > sm.level ? 0.85 : 0.35) * (norm - sm.level);
          if (sm.level >= sm.peak)   { sm.peak = sm.level; sm.hold = 15; }
          else if (sm.hold > 0)      { sm.hold--; }
          else                       { sm.peak = Math.max(0, sm.peak - 0.02); }
          // Send the meter TEXT THE PHONE DRAWS, not a metric of the watch's choosing.
          // OWRX/Kiwi have no SNR (snrDb is hardcoded 0 on them), so a wrist that
          // rendered SNR showed a permanent "—" while its bar moved perfectly well.
          watchProvider.setSignal(
            snrDb, sm.level,
            meterText(signalModeRef.current, {
              level: sm.level, peak: sm.peak, snr: snrDb, dbfs: levelDbm,
              active: owrxDbm != null ? owrxDbm > -110 : snrDb > 6, link: 0,
            }),
          );
          // Backgrounded (watch-only) frames must NOT drive the meter bus: it
          // re-renders a React leaf per frame, and per-frame React commits in the
          // background are exactly what starved the audio DSP in v6. Nobody can
          // see the phone's meter anyway — the watch gets its level above.
          if (appActiveRef.current) {
            meterBus.current.emit({
              level: sm.level, peak: sm.peak, snr: snrDb, dbfs: levelDbm,
              active: owrxDbm != null ? owrxDbm > -110 : snrDb > 6,
              link: meterBus.current.value.link,
            });
          }
        }
      },
      onError: (msg) => {
        if (destroyed.current) return;
        // Rate-limited / blocked → straight to the bypass-password box (the
        // instance password gets around per-IP limits); other errors offer
        // both routes.
        if (/429|rate.?limit|too many|refused|denied|blocked|busy/i.test(msg)) {
          setPwPrompt(true);
        } else {
          Alert.alert('Connection Error', msg, [
            { text: 'Back to Instances', onPress: () => navigation.goBack() },
            { text: 'Enter Password', onPress: () => setPwPrompt(true) },
          ]);
        }
      },
    }, password, !!route.params.isLocal);
    client.current = c;
    // Apply the persisted VFO-lock follow mode to the fresh connection.
    c.setFollowMode(vfoLockedRef.current);
    // Local hardware: thread the live device sample rate for panSpan()'s window.
    if (route.params.isLocal) (c as { setLocalSampleRate?: (hz: number) => void }).setLocalSampleRate?.(hwSampleRate);
    // VibeServer PIN: append the auth suffix to the spectrum WS.
    if (route.params.authSuffix) (c as { setAuthSuffix?: (s: string) => void }).setAuthSuffix?.(route.params.authSuffix);
    // QoL: restore the last frequency/mode used on THIS instance before
    // connecting (the hardcoded default landed on the 20m FT8 squeal every
    // launch). Falls back to the default tune on first visit / bad data.
    let cancelled = false;
    const tuneKey = isLocal ? `lsv_last_tune:${localDeviceKey}` : 'lsv_last_tune:' + baseUrl;
    (async () => {
      let j = await AsyncStorage.getItem(tuneKey).catch(() => null);
      // Migrate the pre-per-device global local key on first per-device connect.
      if (j == null && isLocal) j = await AsyncStorage.getItem('lsv_last_tune:local').catch(() => null);
      return j;
    })().then((j: string | null) => {
      if (cancelled || destroyed.current) return;
      let f = status.frequency;
      let m: SDRMode = status.mode;
      if (j) {
        try {
          const p = JSON.parse(j) as { frequency?: unknown; mode?: unknown };
          // MAX_HZ (30 MHz) is the HF ceiling for network SDRs, but local RTL-SDR
          // hardware tunes VHF/UHF — so an FM/airband/etc. last-tune would fail
          // the guard and silently reset to the default. Use a wide hardware bound
          // for local (the per-device key only ever stores a freq that was
          // tunable on THIS device, so it's inherently valid).
          const hiHz = isLocal ? 2_000_000_000 : MAX_HZ;
          if (typeof p.frequency === 'number' && p.frequency >= MIN_HZ && p.frequency <= hiHz) {
            f = Math.round(p.frequency);
          }
          if (typeof p.mode === 'string' && p.mode in MODE_BANDWIDTHS) m = p.mode as SDRMode;
        } catch {}
      }
      // NB: no device-range clamp here — the per-device key already means each
      // source only ever restores ITS OWN last frequency (valid when saved), so
      // there's nothing to guard against, and c.caps.freqRange isn't reliable yet
      // at restore time (the local device's real caps land after connect), which
      // made it wrongly reset an in-range frequency to the default.
      // A vibesdr:// deep link's freq/mode override the persisted last-tune, but
      // only on the first connect of this screen (consumed via the ref) so a
      // reconnect/rotation later doesn't yank the user back to the link's freq.
      if (!deepLinkTuneApplied.current) {
        deepLinkTuneApplied.current = true;
        const df = route.params.initialFreq;
        const dm = route.params.initialMode;
        if (typeof df === 'number' && df >= MIN_HZ && df <= MAX_HZ) f = Math.round(df);
        if (typeof dm === 'string' && dm in MODE_BANDWIDTHS) m = dm as SDRMode;
      }
      const bw = MODE_BANDWIDTHS[m];
      setStatus((prev: SDRStatus) => ({
        ...prev, frequency: f, mode: m,
        ...(bw ? { bandwidthLow: bw[0], bandwidthHigh: bw[1] } : {}),
      }));
      lastTuneLoaded.current = true;
      setTuneLoaded(true);
      // A server crash/refused connection rejects this — swallow it (onDisconnect
      // drives the UI). An unhandled rejection here can escalate to a hard crash.
      c.connect(f, m).catch(() => {});
    }).catch(() => {
      if (cancelled || destroyed.current) return;
      lastTuneLoaded.current = true;
      setTuneLoaded(true);
      c.connect(status.frequency, status.mode).catch(() => {});
    });
    return () => { cancelled = true; destroyed.current = true; c.destroy(); client.current = null; };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [baseUrl, connEpoch]);

  // Persist the tune (debounced — the drum changes frequency rapidly) so the
  // next visit to this instance resumes where you left off.
  const lastTuneLoaded = useRef(false);
  // One-shot: a deep-link initial tune is applied on the first connect only.
  const deepLinkTuneApplied = useRef(false);
  // Start the session countdown once we're actually connected.
  useEffect(() => {
    if (!connected || !sessionLimitMins || sessionEndsAt) return;
    setSessionEndsAt(Date.now() + sessionLimitMins * 60_000);
  }, [connected, sessionLimitMins, sessionEndsAt]);

  useEffect(() => {
    if (!sessionEndsAt) return;
    const tick = () => setSessionLeftMs(Math.max(0, sessionEndsAt - Date.now()));
    tick();
    const t = setInterval(tick, 1000);
    return () => clearInterval(t);
  }, [sessionEndsAt]);

  // One combined notice covering BOTH constraints — a read-only, time-limited
  // receiver should not produce two popups in a row.
  useEffect(() => {
    if (noticeShownRef.current || !connected) return;
    if (!readOnly && !sessionLimitMins) return;
    noticeShownRef.current = true;
    const parts: string[] = [];
    if (readOnly) parts.push(
      'This receiver is listen-only — another user is controlling it, so tuning ' +
      'and mode controls are disabled.');
    if (sessionLimitMins) parts.push(
      `This receiver limits each listener to ${sessionLimitMins} minutes. ` +
      'A countdown is shown next to the clock, and it will disconnect you when the time is up.');
    Alert.alert(readOnly && sessionLimitMins ? 'Listen-only, and time limited'
                : readOnly ? 'Listen-only receiver' : 'Time-limited receiver',
                parts.join('\n\n'));
  }, [connected, readOnly, sessionLimitMins]);

  // rtl_tcp link meter: poll the shim's network-stall counter — periods where the
  // socket delivered nothing for >120 ms. That is the honest client-side view of
  // the link; the backend's own quality reading is FFT-frame timing measured after
  // the jitter buffer, so it stays green while the network is failing.
  useEffect(() => {
    if (!isLocal) return;
    let last = -1;
    let toldClosed = false;
    const t = setInterval(async () => {
      try {
        const s = await LocalHw?.getNetStatus?.();
        if (!s?.tcp) { netLinkRef.current = 3; return; }   // USB path: nothing to clamp

        // The SpyServer hung up. It is NOT a generic connection loss: public
        // servers enforce session limits (30 min – 24 h) and hand the single
        // tuner to whoever asks next. Say so, once.
        if (s.spy && s.closed && !toldClosed) {
          toldClosed = true;
          Alert.alert(
            'Receiver disconnected',
            'The SpyServer closed the connection. Public receivers often limit how ' +
            'long one listener can stay, and many allow only one at a time — someone ' +
            'else may now have the tuner.',
            [{ text: 'OK', onPress: () => navigation.goBack() }],
          );
          return;
        }
        // Another client owns the tuner: the dial would silently do nothing.
        setIsSpy(!!s.spy);
        if (s.spy) setReadOnly(!s.canControl);

        const n = s.stalls ?? 0;
        if (last < 0) { last = n; return; }                // first sample: no delta yet
        const delta = n - last;
        last = n;
        netLinkRef.current = delta === 0 ? 3 : delta <= 2 ? 2 : 1;
      } catch {}
    }, 2000);
    return () => clearInterval(t);
  }, [isLocal, navigation]);

  // Bypass-password prompt — rate-limited/blocked connections show this
  // directly (the instance password gets around per-IP limits); submitting
  // replaces the screen with a fresh session carrying the password on every
  // WS URL (audio, spectrum, dxcluster).
  const [pwPrompt, setPwPrompt] = useState(false);

  // Audio engine start is GATED on the restore (ms-fast): the engine used to
  // start with the default 14.074/USB in the audio-WS URL and the corrective
  // restore tune could lose the race against the WS handshake — server stayed
  // on 20m FT8/USB while the UI showed the restored station (sounded like
  // "broken AM"), and zoom anchored on the stale server frequency.
  const [tuneLoaded, setTuneLoaded] = useState(false);

  // Initial-connect timeout: if the link never comes up (e.g. a wedged local
  // shim, dead host, or USB not ready) there's no error event to surface an
  // escape — the screen just spins forever. After 15s with no connection, show
  // a "couldn't connect" card with an escape back to the instance list.
  useEffect(() => {
    if (connected) { setConnTimedOut(false); return; }
    const t = setTimeout(() => { if (!destroyed.current && !connected) setConnTimedOut(true); }, 15000);
    return () => clearTimeout(t);
  }, [connected]);

  useEffect(() => {
    if (!lastTuneLoaded.current || !status.frequency) return;
    const t = setTimeout(() => {
      // Local hardware's baseUrl has a per-session port → use a stable PER-DEVICE
      // key (usb / tcp:host:port) so the last tune restores and devices don't
      // clobber each other (otherwise it reverts to the 14 MHz default).
      AsyncStorage.setItem(isLocal ? `lsv_last_tune:${localDeviceKey}` : 'lsv_last_tune:' + baseUrl,
        JSON.stringify({ frequency: status.frequency, mode: status.mode })).catch(() => {});
    }, 1000);
    return () => clearTimeout(t);
  }, [status.frequency, status.mode, baseUrl]);

  useEffect(() => {
    let resumeTimer: ReturnType<typeof setTimeout> | null = null;
    const sub = AppState.addEventListener('change', (state: string) => {
      if (state !== 'active') {
        if (resumeTimer) { clearTimeout(resumeTimer); resumeTimer = null; }
        // Backgrounded: the spectrum pause starves the link to 0, but that's NOT
        // a disconnect (audio keeps playing). Suppress the connection-lost popup
        // while backgrounded and reset it so a long lock can't leave it armed.
        appActiveRef.current = false;
        if (connLostTimer.current) { clearTimeout(connLostTimer.current); connLostTimer.current = null; }
        setConnLost(false);
        resumingRef.current = false;
        if (reinitTimer.current) { clearTimeout(reinitTimer.current); reinitTimer.current = null; }
        setReinit(false);
        setSpecFailed(false);
        // Keep the spectrum alive if the watch is currently showing it — a
        // locked phone in a pocket IS the primary watch use case, so pausing
        // here would kill the feature exactly when it matters. Half rate: the
        // watch only draws ~10fps, so full rate is wasted bytes and wasted
        // parse on the thread that feeds the audio DSP. The Skia tree is still
        // unmounted and every animation driver still cancelled — only the plain
        // JS path stays alive.
        if (watchProvider.isActive) {
          specPausedByBgRef.current = false;
          client.current?.setRate(WATCH_BG_DIVISOR);
        } else {
          specPausedByBgRef.current = true;
          client.current?.pauseSpectrum();
        }
        watchProvider.setSpecPaused(specPausedByBgRef.current);
      } else if (dataSaverOffRef.current) {
        appActiveRef.current = true;
        // Opened the app after a data-saver disconnect (the Play event may not
        // survive suspension): do a full from-scratch reconnect.
        setDataSaverOff(false);
        setIsMuted(false);
        fullReconnect();
      } else {
        // Instant zombie-socket check — after a background suspension the
        // audio WS can be half-open (server reaped the session, socket never
        // errors) leaving audio+spectrum dead until relaunch. The native
        // watchdog also catches this within ~8s; this makes it immediate.
        // OWRX/Kiwi audio is JS-owned (no native WS) — revive() would resurrect a
        // UberSDR audio WS underneath the foreign stream, so only the native Opus
        // engine (ubersdr) is revived here.
        if ((route.params.serverType ?? 'ubersdr') === 'ubersdr') {
          (NativeModules.VibePowerModule as { revive?: () => void })?.revive?.();
        }
        // Reopen the spectrum only AFTER the audio session re-registers
        // server-side: the spectrum WS subscribes to that same session, so if it
        // reopens first it gets no frames and the waterfall stays frozen (the bug
        // where you had to back out to instances and reconnect). connect() uses
        // the same audio-first-then-1s ordering; mirror it here.
        appActiveRef.current = true;
        // Surface the calm "waterfall reinitialising" notice while the spectrum
        // re-subscribes. If frames return (onLink q>0) it clears itself. After a
        // long background the spectrum can take a while to come back even though
        // audio never stopped — so the watchdog only escalates to the real
        // "Connection lost" popup when AUDIO is also dead; while audio still
        // flows it keeps the calm notice and re-checks.
        if ((route.params.serverType ?? 'ubersdr') === 'ubersdr' && specPausedByBgRef.current) {
          resumingRef.current = true;
          setReinit(true);
          setSpecFailed(false);
          const resumeStartedAt = Date.now();
          const armReinitWatchdog = () => {
            if (reinitTimer.current) clearTimeout(reinitTimer.current);
            reinitTimer.current = setTimeout(() => {
              reinitTimer.current = null;
              if (destroyed.current || !resumingRef.current) return;
              if (Date.now() - lastAudioAtRef.current < 2000) {
                // Audio is still flowing → we're connected. If the spectrum has
                // been silent for a long while it has genuinely failed to
                // re-subscribe — surface an escape (reconnect / instance list)
                // rather than spin the calm notice forever. Otherwise keep
                // waiting; it's just slow to come back.
                if (Date.now() - resumeStartedAt > 10000) {
                  resumingRef.current = false;
                  setReinit(false);
                  setSpecFailed(true);
                  return;
                }
                armReinitWatchdog();
                return;
              }
              // Audio is dead too → genuine disconnect.
              resumingRef.current = false;
              setReinit(false);
              setConnLost(true);
            }, 3500);
          };
          armReinitWatchdog();
        }
        if (resumeTimer) clearTimeout(resumeTimer);
        resumeTimer = setTimeout(() => {
          resumeTimer = null;
          // If the watch kept the socket alive through the lock there is nothing
          // to re-subscribe — just restore full rate. Re-opening a live socket
          // would drop frames and flash the "reinitialising" notice for nothing.
          if (specPausedByBgRef.current) {
            specPausedByBgRef.current = false;
            client.current?.resumeSpectrum();
          }
          // ALWAYS restore full rate on wake, on every path. If the watch held the
          // socket open through the lock we dropped the feed to quarter rate for
          // it; failing to undo that anywhere leaves the phone's own waterfall
          // crawling at 5fps.
          idleActiveRef.current = false;
          client.current?.setRate(1);
        }, 1200);
      }
    });
    return () => {
      if (resumeTimer) clearTimeout(resumeTimer);
      if (reinitTimer.current) { clearTimeout(reinitTimer.current); reinitTimer.current = null; }
      sub.remove();
    };
  }, []);

  // ── Smooth tune / idle saver ──────────────────────────────────────────────
  // Touches on RNGH surfaces (waterfall, drums) bypass the JS responder chain,
  // so interaction is marked BOTH in the root capture handler (catches all
  // Pressable UI) and at the top of each gesture callback below.
  const IDLE_SLOW_MS = 30_000;
  const IDLE_DIVISOR = 3; // skin default-waterfall parity

  const lastInteractRef = useRef(Date.now());
  const idleActiveRef   = useRef(false);

  const markInteract = useCallback(() => {
    lastInteractRef.current = Date.now();
    if (idleActiveRef.current) {
      idleActiveRef.current = false;
      client.current?.setRate(1); // wake: full data rate immediately
    }
  }, []);

  useEffect(() => {
    if (!idleSlow) {
      if (idleActiveRef.current) {
        idleActiveRef.current = false;
        client.current?.setRate(1);
      }
      return;
    }
    idleActiveRef.current = false; // new client (baseUrl) starts at divisor 1
    const t = setInterval(() => {
      // A watch showing the waterfall is an active viewer even though nobody is
      // touching the phone — don't idle-slow the feed under it.
      if (watchProvider.isActive) return;
      if (!idleActiveRef.current &&
          Date.now() - lastInteractRef.current > IDLE_SLOW_MS) {
        idleActiveRef.current = true;
        client.current?.setRate(IDLE_DIVISOR);
      }
    }, 5000);
    return () => clearInterval(t);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [idleSlow, baseUrl]); // baseUrl: new client starts at divisor 1

  const onSmoothTune = useCallback((v: boolean) => {
    setSmoothTune(v);
    AsyncStorage.setItem('lsv_smooth_tune', v ? '1' : '0').catch(() => {});
  }, []);

  const onIdleSlow = useCallback((v: boolean) => {
    setIdleSlow(v);
    AsyncStorage.setItem('lsv_idle_slow', v ? '1' : '0').catch(() => {});
  }, []);

  const onFrameRate = useCallback((v: 'native'|'20fps'|'30fps') => {
    setFrameRate(v);
    AsyncStorage.setItem('lsv_frame_rate', v).catch(() => {});
  }, []);

  // ── Drum sensitivity (NORMAL / PRECISE) ──────────────────────────────────
  const [drumMode, setDrumMode] = useState<'normal'|'precise'>('normal');
  const drumModeRef = useRef<'normal'|'precise'>('normal');
  useEffect(() => {
    AsyncStorage.getItem('lsv_drum_sens').then((v: string | null) => {
      if (v === 'normal' || v === 'precise') { setDrumMode(v); drumModeRef.current = v; }
    }).catch(() => {});
  }, []);
  const onDrumMode = useCallback((m: 'normal'|'precise') => {
    setDrumMode(m);
    drumModeRef.current = m;
    AsyncStorage.setItem('lsv_drum_sens', m).catch(() => {});
  }, []);

  // ✦ HAPTICS toggle — was UI-only (props never passed from here, and the
  // drums ticked unconditionally). Module-level switch in DrumWheel.
  const [hapticsEnabled, setHapticsEnabled] = useState(true);
  useEffect(() => {
    AsyncStorage.getItem('lsv_haptics').then((v: string | null) => {
      if (v === '0') { setHapticsEnabled(false); setDrumHaptics(false); }
    }).catch(() => {});
  }, []);
  const onHaptics = useCallback((on: boolean) => {
    setHapticsEnabled(on);
    setDrumHaptics(on);
    AsyncStorage.setItem('lsv_haptics', on ? '1' : '0').catch(() => {});
  }, []);

  // Whether this device has a haptic motor at all — hide the HAPTICS toggle if
  // not (it's a dead button otherwise). iPads have no Taptic Engine; on Android
  // we ask the native Vibrator (some tablets genuinely have no motor).
  const [hapticsHardware, setHapticsHardware] = useState(true);
  useEffect(() => {
    if (Platform.OS === 'ios') {
      setHapticsHardware(!isTablet);   // no iPad has a Taptic Engine
      return;
    }
    const mod = NativeModules.VibePowerModule as
      | { hasVibrator?: () => Promise<boolean> } | undefined;
    mod?.hasVibrator?.()
      .then((has) => setHapticsHardware(has !== false))
      .catch(() => setHapticsHardware(true));
  }, [isTablet]);

  // ── VFO drum ──────────────────────────────────────────────────────────────
  // Skin-parity step tuning (vSendDelta + vDown from Scalable_Mobile_UI v6.3.1):
  //   - pending accumulates in Hz: px × step / pxPerStep (velocity-adaptive)
  //   - tunes ONLY in whole steps: steps = round(pending / step)
  //   - baseline snaps to the step grid, so frequency always lands on a
  //     multiple of the step rate (7,153,000 — never 7,153,437)
  const vfoPendingHz = useRef(0);
  const vfoVel = useRef({ t: 0, v: 0 }); // EMA thumb speed, px/s

  const onVfoDelta = useCallback((pxDelta: number) => {
    const c = client.current; if (!c) return;
    // Whole-profile data modes are locked to their block — VFO tuning just knocks
    // you off it (kills the decode, and the block is a nuisance to re-find). DAB had
    // this guard; ADS-B did NOT, so the drum would happily drag you off 1090 MHz and
    // stop every aircraft decoding. One predicate now, so the next data mode can't
    // fall through the same gap. Ignore drum input.
    if (isWholeProfileMode(String(c.getStatus().mode))) return;
    markInteract();
    const s = stepRef.current;
    // Velocity-adaptive sensitivity: EMA of |px|/dt. A gesture gap resets to
    // 0 so a fresh slow touch starts fully fine; a fast flick's EMA catches
    // up within 2–3 events. The fine↔fast blend is continuous, so easing off
    // mid-spin onto a signal tightens the rate immediately.
    const now = Date.now();
    const gap = now - vfoVel.current.t;
    vfoVel.current.t = now;
    if (gap > 300) {
      vfoVel.current.v = 0;
    } else {
      const inst = Math.abs(pxDelta) / (Math.max(8, gap) / 1000);
      vfoVel.current.v = vfoVel.current.v * 0.7 + inst * 0.3;
    }
    const k = Math.max(0, Math.min(1,
      (vfoVel.current.v - VFO_VEL_FINE) / (VFO_VEL_FAST - VFO_VEL_FINE)));
    const pxPerStep = DRUM_SENS[drumModeRef.current].vfo
      * (VFO_FINE_MULT - (VFO_FINE_MULT - 1) * k);
    vfoPendingHz.current += (pxDelta * s) / pxPerStep;
    const steps = Math.round(vfoPendingHz.current / s);
    if (!steps) return;
    vfoPendingHz.current -= steps * s;
    const cur     = c.getStatus().frequency;
    const snapped = Math.round(cur / s) * s;   // vDown grid snap
    const [loHz, hiHz] = c.caps.freqRange;     // backend range (OWRX VHF/UHF ≠ 0–30 MHz)
    const newHz   = Math.max(loHz, Math.min(hiHz, snapped + steps * s));
    if (newHz === cur) return;
    c.tune(newHz);
    setStatus((prev: SDRStatus) => ({ ...prev, frequency: newHz }));
  }, []);

  // ── BW drum ───────────────────────────────────────────────────────────────

  // Gesture accumulator: drum ticks arrive as small px deltas (rounding them
  // per-event gives factor 1 = no-op), and the server snaps binBandwidth to a
  // ladder (small factors snap back to the same step). So compound the whole
  // gesture from the bandwidth captured at gesture start.
  // VFO-anchored zoom: every zoom path (menu ±, zoom drum, pinch) anchors on
  // the tuned frequency when it's inside the current span — a fresh connect
  // sits on the server's default full-span view, so centre-anchored zooms
  // dove into mid-band (≈15MHz) instead of the restored station. Falls back
  // to the view centre when the VFO has been panned out of sight.
  const zoomAnchorHz = useCallback((s: SDRStatus): number => {
    const c = client.current; if (!c) return s.centerHz;
    const span  = s.binBandwidth * (s.binCount || 1024);
    const tuned = c.getStatus().frequency;
    return tuned && span > 0 && Math.abs(tuned - s.centerHz) < span / 2
      ? tuned : s.centerHz;
  }, []);

  const bwZoomAcc = useRef({ base: 0, px: 0, t: 0 });
  const onBwDelta = useCallback((pxDelta: number) => {
    const c = client.current; if (!c) return;
    markInteract();
    const s = c.getView(); // predicted view — getStatus() is one RTT stale mid-gesture
    if (!s.binBandwidth || !s.centerHz || !s.binCount) return;
    const a = bwZoomAcc.current;
    const now = Date.now();
    if (now - a.t > 400 || !a.base) { a.base = s.binBandwidth; a.px = 0; }
    a.t = now;
    a.px += pxDelta;
    // Drum px per zoom octave (2×) — PRECISE nearly doubles the travel
    c.zoom(zoomAnchorHz(s), Math.max(0.5,
      a.base * Math.pow(0.5, a.px / DRUM_SENS[drumModeRef.current].zoomOctave)));
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const zoomBy = useCallback((factor: number) => {
    const c = client.current; if (!c) return;
    const v = c.getView(); if (!v.binBandwidth || !v.centerHz) return;
    c.zoom(zoomAnchorHz(v), Math.max(1, v.binBandwidth * factor));
  }, [zoomAnchorHz]);
  const onZoomIn  = useCallback(() => zoomBy(0.5), [zoomBy]);
  const onZoomOut = useCallback(() => zoomBy(2),   [zoomBy]);
  // Zoom extremes — each adapter clamps internally (UberSDR to its 6 kHz max-zoom
  // floor / full-span cap, OWRX/Kiwi to their own limits), so a tiny bandwidth =
  // full zoom in and a huge one = full span out.
  const onZoomMax = useCallback(() => {       // MAX = zoom all the way in
    const c = client.current; if (!c) return;
    const v = c.getView(); if (!v.centerHz) return;
    c.zoom(zoomAnchorHz(v), 1);
  }, [zoomAnchorHz]);
  const onZoomMin = useCallback(() => {       // MIN = full span out
    const c = client.current; if (!c) return;
    const v = c.getView(); if (!v.centerHz) return;
    c.zoom(zoomAnchorHz(v), Number.MAX_SAFE_INTEGER);
  }, [zoomAnchorHz]);

  // Toggle: SET DEFAULT when this instance isn't the default, CLEAR when it is
  const [isDefault, setIsDefault] = useState(false);
  useEffect(() => {
    getDefaultInstance()
      .then((d) => setIsDefault(!!d && d.url === baseUrl))
      .catch(() => {});
  }, [baseUrl]);

  const onSetDefault = useCallback(() => {
    if (isDefault) {
      clearDefaultInstance()
        .then(() => {
          setIsDefault(false);
          Alert.alert('Default Cleared', 'No default instance is set.');
        })
        .catch(() => {});
    } else {
      setDefaultInstance({ name: instanceName ?? baseUrl, url: baseUrl })
        .then(() => {
          setIsDefault(true);
          Alert.alert('Default Set', `${instanceName ?? baseUrl} is now your default instance.`);
        })
        .catch(() => {});
    }
  }, [baseUrl, instanceName, isDefault]);

  // Favourite the current instance from the menu — so a good receiver you found
  // mid-session lands in the picker's favourites without hunting for it again.
  // Network receivers only (local USB / RTL-TCP / SpyServer wrap localhost and
  // favourite via the picker, so isLocal instances don't get the button).
  const [isFavourite, setIsFavourite] = useState(false);
  useEffect(() => {
    getFavourites()
      .then((favs) => setIsFavourite(favs.some((f) => f.url === baseUrl)))
      .catch(() => {});
  }, [baseUrl]);

  const onToggleFavourite = useCallback(() => {
    const st = route.params.serverType ?? 'ubersdr';
    getFavourites()
      .then((favs) => toggleFavourite({ name: instanceName ?? baseUrl, url: baseUrl, serverType: st }, favs))
      .then((next) => setIsFavourite(next.some((f) => f.url === baseUrl)))
      .catch(() => {});
  }, [baseUrl, instanceName, route.params.serverType]);

  // ── Waterfall gestures ────────────────────────────────────────────────────

  const onWfPanDelta = useCallback((dxPx: number) => {
    const c = client.current; if (!c) return;
    if (vfoLockedRef.current) return;                 // no free pan while locked
    markInteract();
    // Predicted view: pan() updates it synchronously, so successive deltas
    // compound correctly. Re-basing on getStatus() made every delta in an RTT
    // window re-apply from the same stale centre (rubber-banding).
    const s = c.getView(); if (!s.bwHz || !s.centerHz) return;
    const span = c.panSpan();
    const target = s.centerHz + Math.round((dxPx / screenW) * s.bwHz);
    // Silently clamp at the boundary walls (the visible walls show the limit;
    // no toast — per Stuart, VTS pop-ups caused more trouble than they solved).
    let clamped: number;
    if (span.movable) {
      // Local Fs window: span bounds the CENTRE directly (keeps the VFO inside
      // the capture window; the VFO itself may leave the visible view).
      clamped = Math.max(span.loHz, Math.min(span.hiHz, target));
    } else {
      // Hard walls (band edge / profile / rx range): keep the whole VIEW inside.
      const half = s.bwHz / 2;
      const loC = span.loHz + half, hiC = span.hiHz - half;
      clamped = loC <= hiC ? Math.max(loC, Math.min(hiC, target))
                           : Math.round((span.loHz + span.hiHz) / 2);
    }
    c.pan(clamped);
  }, [screenW]);

  // Same gesture-accumulator pattern as the BW drum (ladder snap-back).
  const wfZoomAcc = useRef({ base: 0, f: 1, t: 0 });
  const wfZoomBy = useCallback((factor: number) => {
    const c = client.current; if (!c) return;
    markInteract();
    const s = c.getView(); if (!s.binBandwidth || !s.centerHz) return;
    const a = wfZoomAcc.current;
    const now = Date.now();
    if (now - a.t > 400 || !a.base) { a.base = s.binBandwidth; a.f = 1; }
    a.t = now;
    a.f *= factor;
    c.zoom(zoomAnchorHz(s), Math.max(0.5, a.base * a.f));
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const onWfZoomDelta = useCallback((dyPx: number) => {
    wfZoomBy(Math.pow(0.985, dyPx));
  }, [wfZoomBy]);

  const onWfPinchZoom = useCallback((scaleDelta: number) => {
    wfZoomBy(1 / scaleDelta);
  }, [wfZoomBy]);

  const onWfTapTune = useCallback((hz: number) => {
    const c = client.current; if (!c) return;
    // Whole-profile data modes (DAB, ADS-B, ISM…) have nothing to tune — the only
    // thing a VFO can do is drag you OFF the block and kill the decode.
    if (isWholeProfileMode(String(c.getStatus().mode))) return;
    markInteract();
    const [loHz, hiHz] = c.caps.freqRange;
    const clamped = Math.max(loHz, Math.min(hiHz, hz));
    c.tune(clamped);
    setStatus((prev: SDRStatus) => ({ ...prev, frequency: clamped }));
  }, []);

  // ── Mode / filter / tune ──────────────────────────────────────────────────

  const onMode = useCallback((m: SDRMode) => {
    const c = client.current; if (!c) return;
    c.setMode(m); // client mirrors the server's per-mode bandwidth defaults
    setStatus({ ...c.getStatus() });
    if (m !== 'wfm') setFmStereo(false);  // stereo icon only applies to WFM
    // OWRX image decoders (SSTV/Fax) ride on top of the analog carrier — sync the
    // decoder canvas to the adapter's REAL decoder state (it auto-keeps/clears the
    // decoder when the carrier changes), so changing the carrier doesn't close it.
    if (route.params.serverType === 'owrx') {
      // Image decoders → the Skia canvas (sstv/wefax); any other secondary
      // decoder (packet/pocsag/adsb/…) → the text panel, titled by its mode id.
      const dec = c.getSecondaryDecoder?.() ?? null;
      const dt: DecoderType = dec === 'sstv' ? 'sstv'
        : dec === 'fax' ? 'wefax'
        : dec ? (dec as unknown as DecoderType) : null;
      if (dt !== activeDecRef.current) {
        if (dt) {
          decoderImageRef.current?.reset();
          setDecoderText('');
          activeDecRef.current = dt; setActiveDecoder(dt);
          setDecoderStatus('listening…');
        } else { activeDecRef.current = null; setActiveDecoder(null); }
      }
    }
  }, [route.params.serverType]);

  // Atomic both-edges setter — single setBandwidth, no stale-closure edge
  const onFilterBoth = useCallback((low: number, high: number) => {
    client.current?.setBandwidth(low, high);
    setStatus((prev: SDRStatus) => ({ ...prev, bandwidthLow: low, bandwidthHigh: high }));
  }, []);

  const onFilterLow  = useCallback((v: number) => { client.current?.setBandwidth(v, status.bandwidthHigh); setStatus((prev: SDRStatus) => ({ ...prev, bandwidthLow: v })); }, [status.bandwidthHigh]);
  const onFilterHigh = useCallback((v: number) => { client.current?.setBandwidth(status.bandwidthLow, v);  setStatus((prev: SDRStatus) => ({ ...prev, bandwidthHigh: v })); }, [status.bandwidthLow]);

  // ── Audio-WS commands (set_dsp / squelch / gate are AUDIO-WS message types;
  //    the spectrum WS doesn't know them — the old client.setNRMode/setDsp
  //    paths were sending into the void) ──────────────────────────────────────
  const sendAudioCmd = useCallback((obj: Record<string, unknown>) => {
    VibePowerModule?.sendAudioCommand(JSON.stringify(obj));
  }, []);

  // ── NR cycle: off → nr → nr2 — native Swift DSP (VibeDSP.swift skin ports)
  const onNrMode = useCallback((mode: 'off'|'nr'|'nr2') => {
    setNrMode(mode);
    VibePowerModule?.setNrMode(mode);  // Android: accepted no-op (port pending)
  }, []);

  // ── NB toggle — native Swift noise blanker ────────────────────────────────
  const onNb = useCallback((on: boolean) => {
    setNb(on);
    VibePowerModule?.setNoiseBlanker(on);  // Android: accepted no-op (port pending)
  }, []);

  // ── SNR squelch (audio gate) ──────────────────────────────────────────────
  // The slider/state are in OUR meter's units (spectrum-derived passband
  // SNR). The server gates on radiod's raw audio-stream SNR, which reads
  // ~30dB higher (floors at ~30 — madpsy/ka9q_ubersdr#77, same offset the
  // signal meter compensates for), so shift +30 on the wire.
  const onSnrSquelch = useCallback((minSnr: number) => {
    setSnrSquelch(minSnr);
    sendAudioCmd({ type: 'set_audio_gate', min_snr: minSnr <= -999 ? -999 : minSnr + 30 });
  }, [sendAudioCmd]);

  // ── FM squelch ────────────────────────────────────────────────────────────
  const onFmSquelch = useCallback((db: number) => {
    setFmSquelch(db);
    sendAudioCmd({ type: 'set_squelch', squelchOpen: db });
  }, [sendAudioCmd]);

  // radiod creates FM channels with its own DEFAULT squelch — entering
  // fm/nfm must re-assert the app's squelch state (default −999 = always
  // open), otherwise marginal signals cut in and out while the UI says
  // "Open". Delayed so the server has re-created the radiod channel after
  // the mode tune.
  const fmSquelchRef = useRef(fmSquelch);
  useEffect(() => { fmSquelchRef.current = fmSquelch; }, [fmSquelch]);
  useEffect(() => {
    if (status.mode !== 'fm' && status.mode !== 'nfm') return;
    const t = setTimeout(() => {
      sendAudioCmd({ type: 'set_squelch', squelchOpen: fmSquelchRef.current });
    }, 700);
    return () => clearTimeout(t);
  }, [status.mode, sendAudioCmd]);

  // ── Server-side NR (DSP insert) ───────────────────────────────────────────
  // Ref mirrors so the WS-event listener and debounced senders read current
  // values without re-subscribing.
  const dspFiltersRef       = useRef<DspFilterDesc[]>([]);
  const dspFilterRef        = useRef('');
  const dspParamsRef        = useRef<Record<string,string>>({});
  const serverDspEnabledRef = useRef(false);
  useEffect(() => { dspFiltersRef.current = dspFilters; },             [dspFilters]);
  useEffect(() => { dspFilterRef.current = serverDspFilter; },         [serverDspFilter]);
  useEffect(() => { serverDspEnabledRef.current = serverDspEnabled; }, [serverDspEnabled]);

  const dspDefaults = useCallback((f?: DspFilterDesc): Record<string,string> => {
    const out: Record<string,string> = {};
    for (const p of f?.params ?? []) {
      if (p.runtime_safe === false) continue;
      out[p.name] = p.default ?? p.min ?? '0';
    }
    return out;
  }, []);

  const applyDspParams = useCallback((p: Record<string,string>) => {
    dspParamsRef.current = p;
    setServerDspParams(p);
  }, []);

  const onServerDsp = useCallback((enabled: boolean) => {
    setServerDspEnabled(enabled);  // optimistic — dsp_status confirms
    if (isKiwi) {
      client.current?.setDsp?.(enabled, dspFilterRef.current, dspParamsRef.current);
      return;
    }
    if (enabled) {
      sendAudioCmd({ type: 'set_dsp', enabled: true,
                     filter: dspFilterRef.current, params: dspParamsRef.current });
      // Server NR replaces client NR — the menu NR button locks to SERV
      setNrMode('off');
      VibePowerModule?.setNrMode('off');
    } else {
      sendAudioCmd({ type: 'set_dsp', enabled: false });
    }
  }, [sendAudioCmd, isKiwi]);

  const onServerDspFilter = useCallback((name: string) => {
    setServerDspFilter(name);
    const defs = dspDefaults(dspFiltersRef.current.find((f: DspFilterDesc) => f.name === name));
    applyDspParams(defs);
    if (isKiwi) {
      if (serverDspEnabledRef.current) client.current?.setDspFilter?.(name, defs);
      return;
    }
    if (serverDspEnabledRef.current) {
      sendAudioCmd({ type: 'set_dsp', enabled: true, filter: name, params: defs });
    }
  }, [sendAudioCmd, dspDefaults, applyDspParams, isKiwi]);

  // Param edits send the FULL params map, debounced 120ms (skin parity)
  const dspParamTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const onServerDspParam = useCallback((name: string, value: string) => {
    const next = { ...dspParamsRef.current, [name]: value };
    applyDspParams(next);
    if (dspParamTimer.current) clearTimeout(dspParamTimer.current);
    dspParamTimer.current = setTimeout(() => {
      if (!serverDspEnabledRef.current) return;
      if (isKiwi) client.current?.setDspParams?.(dspParamsRef.current);
      else sendAudioCmd({ type: 'set_dsp_params', params: dspParamsRef.current });
    }, 120);
  }, [sendAudioCmd, applyDspParams, isKiwi]);
  useEffect(() => () => {
    if (dspParamTimer.current) clearTimeout(dspParamTimer.current);
  }, []);

  const onTuneHz = useCallback((hz: number) => {
    const c = client.current; if (!c) return;
    markInteract();
    const [loHz, hiHz] = c.caps.freqRange;
    const clamped = Math.max(loHz, Math.min(hiHz, hz));
    // Discrete jump (freq modal, bookmark/VTS, Siri, search) → always land
    // centred, regardless of the VFO lock.
    c.tune(clamped, undefined, { recenter: true });
    setStatus((prev: SDRStatus) => ({ ...prev, frequency: clamped }));
  }, []);

  // Late-bound handler refs for the chat-sync engine (declared above the
  // decoder-client effect, which captures them in its callbacks)
  useEffect(() => {
    onTuneHzRef.current    = onTuneHz;
    onModeRef.current      = onMode;
    zoomByRef.current      = zoomBy;
    onFilterBothRef.current = onFilterBoth;
    onVtsJumpRef.current   = onVtsJump;
    onSearchTuneRef.current = onSearchTune;
  });

  // ── Apple Watch: crown/menu commands in, tuned state back out ─────────────
  //    The watch sends DELTAS, never absolute frequencies — the phone stays the
  //    single source of truth for step size and band limits, and the watch just
  //    mirrors whatever we echo back.
  useEffect(() => {
    // CLAIM the watch. Two screens can be alive at once (the outgoing one unmounts
    // asynchronously), and a stale screen streaming rows drags the wrist back to a
    // waterfall it has already left. Only the owner may send.
    const token = watchProvider.claim('sdr');
    // A MOUNTED SDR SCREEN IS A LIVE SESSION — say so.
    //
    // phoneStatus was only ever set to 'ready' on the WATCH-DRIVEN launch path
    // (App.tsx goTo). Connect on the PHONE instead, and nothing ever retracted an
    // earlier 'pick'/'setup' — so the wrist kept telling the user to choose a server
    // while a waterfall drew and the crown tuned underneath the message. The status has
    // to be owned by the thing that KNOWS, and that is the screen that is running.
    watchProvider.setPhoneStatus('ready');
    // The watch knows when ITS rows dried up. That was only ever drawn on screen;
    // make it act. Last line of defence behind the client's own watchdog, and
    // rate-limited there — so a wedged link can't turn this into a reopen storm.
    watchProvider.setStaleHandler(() => {
      client.current?.forceResubscribe?.('watch-rows-idle');
    });
    watchProvider.attach({
      // No arming here: Kiwi/OWRX/UberSDR give every user their OWN VFO, so tuning
      // disturbs nobody. Arming is an FM-DX rule, not a "shared backend" rule.
      onTuneDelta: (delta: number) => {
        const c = client.current; if (!c || !delta) return;
        wakeSpectrumForWatch();   // a turning crown is proof the watch is watching
        if (isWholeProfileMode(String(c.getStatus().mode))) return;   // locked to its ensemble
        const s = stepRef.current; if (!(s > 0)) return;
        const cur = c.getStatus().frequency;
        // Snap to the step grid first, exactly like the media-control skip: a
        // crown detent should land on a channel, not offset the current fraction.
        const base = delta > 0 ? Math.floor(cur / s) : Math.ceil(cur / s);
        const [loHz, hiHz] = c.caps.freqRange;
        const newHz = Math.max(loHz, Math.min(hiHz, (base + delta) * s));
        if (newHz === cur) return;
        onTuneHzRef.current?.(newHz);
      },
      // Numpad entry. onTuneHz already clamps to the receiver's range, so a
      // fat-fingered 999 MHz lands on the band edge rather than nowhere.
      onTuneHz: (hz: number) => { if (hz > 0) onTuneHzRef.current?.(hz); },
      onMode: (m: string) => { if (m) onModeRef.current?.(m as SDRMode); },
      onStep: (hz: number) => { if (hz > 0) setStep(hz); },

      // Crown in ZOOM mode. Drives the SAME client.zoom() the phone's zoom drum
      // drives — so it moves the real waterfall, and the watch gets genuinely
      // finer bins rather than a magnified crop of coarse ones.
      // DAB: pick a service. NOT a tune — setAudioServiceId re-sends the demod
      // without touching the frequency, so the ensemble lock is never disturbed.
      onDabSelect: (id: number) => {
        const c = client.current; if (!c || !id) return;
        c.setAudioServiceId?.(id);
        setActiveDabId(id);
      },

      onZoomDelta: (delta: number) => {
        if (!delta) return;
        zoomByRef.current?.(Math.pow(2, -delta / 6));
      },

      // The crown, in volume mode. ONE detent = ONE 1/16 step, because 1/16 IS the
      // iPhone's volume quantisation — a finer step would round to the same value and
      // the crown would feel dead for a click or two at a time.
      //
      // Applied against the phone's own current volume (sysVolRef, kept true by the KVO
      // event), never against a value the watch sent us: the phone owns the knob. The
      // set then SNAPS to 1/16 and the KVO observer echoes the snapped truth back to the
      // wrist, which adopts it when the crown settles.
      onVolumeDelta: (delta: number) => {
        if (!delta) return;
        const next = Math.max(0, Math.min(1, sysVolRef.current + delta / 16));
        sysVolRef.current = next;
        (NativeModules.VibePowerModule as { setSystemVolume?: (v: number) => void })
          ?.setSystemVolume?.(next);
      },

      // A mute is NOT "volume to zero": that would destroy the level you were listening
      // at, so unmuting could not restore it. Route to the phone's existing mute, which
      // gates playback and leaves the volume where it was.
      onMute: (muted: boolean) => {
        (NativeModules.VibePowerModule as { setMuted?: (m: boolean) => void })
          ?.setMuted?.(muted);
        setIsMuted(muted);
        watchProvider.setMuted(muted);   // echo the truth back, don't leave the wrist guessing
      },

      // The watch said hello. Answer with the current state so its menu already
      // knows the mode and step BEFORE the user opens it. Read from the client,
      // not from React state, which can lag a frame behind the radio.
      onHello: () => {
        const c = client.current; if (!c) return;
        // ANY message from the watch is PROOF it is there and listening — better
        // proof than a flag, because recency cannot desync. The watch pings every 4s,
        // so this self-heals a paused spectrum within one heartbeat.
        //
        // It has to, because onReachableChange only fires on a CHANGE: if the flag
        // was already true when we backgrounded and we paused anyway (or the pause
        // and the flag ever disagree), NOTHING re-opens the socket. The wrist then
        // sits there tuning perfectly — uplink alive, downlink dead — showing "No
        // spectrum from iPhone" forever. Same class as the stale-isReachable bug;
        // the flag is not the truth, the message is.
        wakeSpectrumForWatch();
        const s = c.getStatus();
        // Answer EVERY ping with state — the watch uses the freshness of this to tell
        // "the phone is dead" apart from "the phone is fine but rows are being lost",
        // and those need completely different fixes.
        watchProvider.sendState(s.frequency, String(s.mode), stepRef.current);
      },

      // The watch app is usually opened AFTER the phone is already locked in a
      // pocket — by which point we have already closed the spectrum WS. So the
      // watch coming into view has to be able to REOPEN it, and its going away
      // has to close it again, entirely while the phone stays backgrounded.
      onReachableChange: (reachable: boolean) => {
        if (appActiveRef.current) return;   // phone's own screen governs
        const c = client.current; if (!c) return;
        if (reachable) {
          if (specPausedByBgRef.current) {
            specPausedByBgRef.current = false;
            c.resumeSpectrum();
          }
          c.setRate(WATCH_BG_DIVISOR);
        } else if (!specPausedByBgRef.current) {
          specPausedByBgRef.current = true;
          c.pauseSpectrum();
        }
        watchProvider.setSpecPaused(specPausedByBgRef.current);
      },
    });
    return () => {
      watchProvider.setStaleHandler(null);
      watchProvider.release(token);
      watchProvider.detach();
    };
  }, []);

  // ── DAB on the watch: a LIST, not a band ───────────────────────────────────
  //    A DAB multiplex is one wide block carrying a dozen services; there is nothing
  //    to hunt in it and nothing to tune (the phone already refuses to — a nudge
  //    knocks you off the ensemble and kills the decode). So the wrist gets the
  //    services, and its crown becomes a SELECTOR.
  useEffect(() => {
    watchProvider.sendDab({
      ensemble: dabEnsemble,
      active: activeDabId,
      list: dabProgrammes.map((p) => ({ id: p.id, name: p.name })),
    });
  }, [dabProgrammes, activeDabId, dabEnsemble]);

  // The playing service's logo, for the wrist. DAB is the EASY case for the logo
  // lookup: the label is a decoded station name rather than a truncated 8-character
  // RDS PS, and the country is simply where the receiver is. The one catch is that
  // the ensemble sends it UNSPACED ("BBC Radio2"), which matches nothing — hence
  // tidyStationName.
  useEffect(() => {
    const name = dabProgrammes.find((p) => p.id === activeDabId)?.name;
    if (!name) { watchProvider.sendLogo(''); return; }
    let cancelled = false;
    resolveStationLogo({ name: tidyStationName(name) })
      .then((path) => {
        if (cancelled) return;
        if (!path) { watchProvider.sendLogo(''); return; }
        return FileSystem.readAsStringAsync(path, { encoding: FileSystem.EncodingType.Base64 })
          .then((b64) => { if (!cancelled) watchProvider.sendLogo(b64); });
      })
      .catch(() => { if (!cancelled) watchProvider.sendLogo(''); });
    return () => { cancelled = true; };
  }, [dabProgrammes, activeDabId]);

  // ADS-B on the watch: aircraft, not a waterfall. 1090 MHz is a whole-profile mode —
  // there is nothing to tune, and its spectrum is a slab of noise.
  useEffect(() => { watchProvider.sendAircraft(aircraft); }, [aircraft]);

  // Mode/step: rare, so send immediately.
  useEffect(() => {
    watchProvider.sendState(status.frequency, String(status.mode), step);
  }, [status.mode, step]);   // eslint-disable-line react-hooks/exhaustive-deps

  // Frequency: the AUTHORITATIVE echo, throttled to 4/sec with the last value
  // always delivered. The watch no longer reads the frequency off the row stream —
  // rows are lossy and can back up in WCSession, so a busy link served the wrist a
  // frequency from seconds ago (it lurched backwards mid-tune, then crawled
  // forward as the queue drained). Throttled state messages can't build a backlog,
  // and the trailing edge means the wrist always lands on the truth.
  useEffect(() => {
    watchProvider.sendFreq(status.frequency, String(status.mode), step);
  }, [status.frequency]);    // eslint-disable-line react-hooks/exhaustive-deps

  // Mirror the phone's own render settings into the watch's processor, so the
  // wrist keeps looking like a shrunk phone waterfall (it runs its own
  // SignalProcessor precisely so it survives the Skia teardown on lock).
  useEffect(() => {
    watchProvider.setColormap(colormap);
    watchProvider.setNeedle(vfoNeedle, vfoIntensity);
    watchProvider.setSharpness(wfSharpness);
    watchProvider.setPeakHold(peakHold);
    watchProvider.setProcessorSettings({
      autoContrast, wfBrightness, wfContrast, wfSharpness,
      spatialSmooth, peakHold,
      manualRange: wfCoarse === 'manual' ? { minDb: dbMin, maxDb: dbMax } : null,
    });
  }, [colormap, autoContrast, wfBrightness, wfContrast, wfSharpness,
      spatialSmooth, peakHold, wfCoarse, dbMin, dbMax, vfoNeedle, vfoIntensity]);

  // ── Share — deep link into this station (web-UI URL params; skin parity:
  //    the skin shared window.location.href which carries the same params) ──
  const onShareStation = useCallback(async () => {
    const c = client.current;
    let url = `${baseUrl.replace(/\/+$/, '')}/?freq=${Math.round(status.frequency)}`
      + `&mode=${status.mode}`
      + `&bwl=${Math.round(status.bandwidthLow)}&bwh=${Math.round(status.bandwidthHigh)}`;
    const v = c?.getView();
    if (v && v.binBandwidth > 0) {
      const span = v.binBandwidth * (v.binCount || 1024);
      if (span < 29_000_000) {  // only when actually zoomed in
        url += `&zoom_freq=${Math.round(v.centerHz)}&zoom_bw=${v.binBandwidth.toFixed(1)}`;
      }
    }
    const label = `VibeSDR — ${(status.frequency / 1e3).toFixed(3)} kHz ${status.mode.toUpperCase()}`;
    // vibesdr:// app link — opens straight into VibeSDR (url-form, so it works
    // for any remote backend). Skip for Local Hardware / RTL-TCP (localhost).
    const st = route.params.serverType ?? 'ubersdr';
    const appLink = route.params.isLocal
      ? null
      : buildShareLink({ baseUrl, serverType: st, freq: status.frequency, mode: status.mode });
    try {
      // iOS shares a real URL object (tappable everywhere); Android targets
      // ignore the url field, so embed it in the message text instead
      await Share.share(Platform.OS === 'ios'
        ? { url, message: appLink ? `${label}\nOpen in VibeSDR: ${appLink}` : label }
        : { message: appLink ? `${label}\n${url}\nOpen in VibeSDR: ${appLink}` : `${label}\n${url}` });
    } catch {}
  }, [baseUrl, status.frequency, status.mode, status.bandwidthLow, status.bandwidthHigh]);

  // ── VTS (station/band steward — a11y popup bar only, no tuning guide) ─────
  // Stations come from /api/bookmarks (static config + live EiBi schedule);
  // popup shows the station name when within 150kHz (green when within 99Hz),
  // and band-plan info when crossing a band boundary. Menu arrows jump
  // bookmarks; an arrow jump defers any band notif 3s so the station name
  // shows first (skin VTS_ARROW_BOOKMARK_MS).
  // ITU region drives the MW channel step (9 kHz region 1, 10 kHz region 2/3).
  // Prefer the RECEIVER longitude (passed by the directory, which knows it); fall
  // back to the user's device longitude when we don't have it (default/favourite
  // reconnects, OWRX, custom URLs) so it isn't left at region 0 → wrong 10 kHz.
  // ITU region (MW 9/10 kHz) is a property of WHERE THE RECEIVER IS — not the
  // listener (a European on a US receiver wants 10 kHz). So use the receiver's
  // own longitude only: from the directory (serverLongitude) or the server's
  // status page (recvLon via onReceiverLon — OWRX/UberSDR /status.json,
  // KiwiSDR /status). NOT the device location.
  const [recvLon, setRecvLon] = useState<number | null>(null);
  const ituRegion = useMemo(
    () => deriveItuRegion(route.params.serverLongitude ?? recvLon),
    [recvLon],   // eslint-disable-line react-hooks/exhaustive-deps
  );
  // The WRIST needs it too — its band label reads off the same plan, and without the
  // region it was quoting the American 40m/41m border (7300) on a British receiver.
  useEffect(() => { watchProvider.setItuRegion(ituRegion); }, [ituRegion]);
  const vtsBookmarks = useRef<ServerBookmark[]>([]);
  const [searchBookmarks, setSearchBookmarks] = useState<ServerBookmark[]>([]);
  const [searchBands,     setSearchBands]     = useState<ServerBand[]>([]);
  const searchBandsRef = useRef<ServerBand[]>([]);
  useEffect(() => { searchBandsRef.current = searchBands; }, [searchBands]);
  // Cold-launch Siri ("open VibeSDR and tune to …"): once connected + bookmarks
  // loaded, apply the pending spoken query the native intent stashed.
  const pendingVoiceDone = useRef(false);
  useEffect(() => {
    if (!connected || pendingVoiceDone.current) return;
    pendingVoiceDone.current = true;
    VibePowerModule?.getPendingVoiceQuery?.().then((json: string | null) => {
      if (!json) return;
      let cmd: { kind?: string; query?: string };
      try { cmd = JSON.parse(json); } catch { return; }
      const q = cmd.query ?? '';
      setTimeout(() => {   // bookmarks land shortly after connect
        if (cmd.kind === 'step') { const s = parseVoiceStep(q); if (s != null) setStep(s); }
        else if (cmd.kind === 'mode') { const m = parseVoiceMode(q); if (m) onModeRef.current?.(m); }
        else {
          const r = resolveVoiceQuery(q, vtsBookmarks.current, searchBandsRef.current);
          if (r) onSearchTuneRef.current?.(r.hz, r.mode, r.isBand, true);
        }
      }, 1500);
    }).catch(() => {});
  }, [connected]);
  const [vtsNotif,        setVtsNotif]        = useState<VtsNotifData | null>(null);
  const vtsKey            = useRef(0);
  const vtsLastStation    = useRef('');
  const vtsBandKey        = useRef<string | null>(null);
  const vtsBandInit       = useRef(false);
  const vtsArrowJumpUntil = useRef(0);
  const vtsDeferredBand   = useRef<ReturnType<typeof setTimeout> | null>(null);
  const [vtsMenuName, setVtsMenuName] = useState('');
  const [vtsMenuFreq, setVtsMenuFreq] = useState<number | undefined>(undefined);

  const [serverBookmarks, setServerBookmarks] = useState<ServerBookmark[]>([]);
  const [userBookmarks,   setUserBookmarks]   = useState<UserBookmark[]>([]);
  // EiBi shortwave schedule — the on-device fallback bookmark set. Toggleable
  // (some people find it too busy); used only when the backend has no server
  // bookmarks of its own. Persisted in lsv_eibi_enabled.
  const [eibiEnabled,   setEibiEnabled]   = useState(true);
  const [eibiBookmarks, setEibiBookmarks] = useState<ServerBookmark[]>([]);
  useEffect(() => {
    AsyncStorage.getItem('lsv_eibi_enabled').then((v) => { if (v === '0') setEibiEnabled(false); }).catch(() => {});
  }, []);
  const onEibiToggle = useCallback((on: boolean) => {
    setEibiEnabled(on);
    AsyncStorage.setItem('lsv_eibi_enabled', on ? '1' : '0').catch(() => {});
  }, []);

  useEffect(() => {
    let cancelled = false;
    const st = route.params.serverType ?? 'ubersdr';
    // OUR band plan is ALWAYS the search bar's band list, on every backend (the
    // server /api/bands only exists on UberSDR; Kiwi/OWRX have none).
    setSearchBands(BAND_PLAN.map((b: Band) => ({
      label: b.bandLabel ?? b.name, start: b.lo, end: b.hi, group: b.type, mode: b.mode,
    })));
    // LOCAL / VibeServer: the shim learns stations from RDS as you tune, so local
    // hardware is no longer bookmark-less — it builds its own list of what this
    // aerial can actually hear. Poll it (the shim keeps it in memory; the autosave
    // effect above is what writes it down).
    if (isLocal) {
      const load = () => {
        getLearnedBookmarksNow()
          .then((b) => { if (!cancelled && b.length) setServerBookmarks(b); })
          .catch(() => {});
      };
      load();
      const iv = setInterval(load, 30_000);
      loadUserBookmarks().then((b: UserBookmark[]) => { if (!cancelled) setUserBookmarks(b); }).catch(() => {});
      return () => { cancelled = true; clearInterval(iv); };
    }

    // Server bookmarks: UberSDR via REST; OWRX/Kiwi arrive over the WS
    // (onBookmarks, tagged source='server' there).
    // Whatever a backend yields is preferred; if it yields nothing, the EiBi
    // fallback below fills in — that's how Kiwi gets a searchable list.
    if (!isLocal && st === 'ubersdr') {
      const load = () => {
        fetchBookmarks(baseUrl)
          .then((b: ServerBookmark[]) => { if (!cancelled) setServerBookmarks(b.map((x) => ({ ...x, source: 'server' as const }))); })
          .catch(() => { if (!cancelled) setServerBookmarks([]); });
      };
      load();
      refreshBandSnr(baseUrl);
      const iv = setInterval(load, 10 * 60_000);
      loadUserBookmarks().then((b: UserBookmark[]) => { if (!cancelled) setUserBookmarks(b); }).catch(() => {});
      return () => { cancelled = true; clearInterval(iv); };
    }
    // OWRX: the WS onBookmarks callback populates serverBookmarks. Kiwi/local:
    // none, so clear any stale set from a previous instance → EiBi takes over.
    if (st !== 'owrx') setServerBookmarks([]);
    loadUserBookmarks().then((b: UserBookmark[]) => { if (!cancelled) setUserBookmarks(b); }).catch(() => {});
    return () => { cancelled = true; };
  }, [baseUrl]);

  // EiBi fallback set — loaded when enabled, refreshed as the schedule rolls.
  // Used only when the backend gave us no server bookmarks (see the merge).
  useEffect(() => {
    if (!eibiEnabled) { setEibiBookmarks([]); return; }
    let cancelled = false;
    const load = () => { loadActiveEibi().then((b) => { if (!cancelled) setEibiBookmarks(b); }).catch(() => {}); };
    load();
    const iv = setInterval(load, 10 * 60_000);
    return () => { cancelled = true; clearInterval(iv); };
  }, [eibiEnabled]);

  // Server (or EiBi fallback) + user bookmarks merged — feeds the VTS lookups AND
  // the search bar identically. User entries win name+freq collisions. Each
  // carries a `source` so the VTS can show its origin icon.
  useEffect(() => {
    const mine = bookmarksForInstance(userBookmarks, baseUrl);
    const seen = new Set(mine.map((b: UserBookmark) => `${b.name}|${b.frequency}`));
    const fallback = serverBookmarks.length > 0 ? serverBookmarks : (eibiEnabled ? eibiBookmarks : []);
    const merged: ServerBookmark[] = [
      ...mine.map((b: UserBookmark) => ({
        name: b.name, frequency: b.frequency, mode: b.mode,
        group: b.group ?? undefined, comment: b.comment ?? undefined,
        bandwidth_low: b.bandwidth_low ?? undefined,
        bandwidth_high: b.bandwidth_high ?? undefined,
        source: 'user' as const,
      })),
      ...fallback.filter((b: ServerBookmark) => !seen.has(`${b.name}|${b.frequency}`)),
    ];
    vtsBookmarks.current = merged;
    setSearchBookmarks(merged);
    pushCarBrowse(merged);
  }, [serverBookmarks, eibiBookmarks, eibiEnabled, userBookmarks, baseUrl, ituRegion]);

  // Push the car browse tree (Bookmarks + Band Plan folders) to the native
  // media-browser service. Bookmarks come from the merged list; the band plan is
  // region-deduped. Native caches it and serves Android Auto / CarPlay; a no-op
  // until a car connects. mediaId encodes freq|mode|step|isBand for the tap.
  const pushCarBrowse = useCallback((bookmarks: ServerBookmark[]) => {
    const bandSeen = new Set<string>();
    const bands = BAND_PLAN.filter((b: Band) => {
      if (b.regions && b.regions.length && ituRegion && !b.regions.includes(ituRegion)) return false;
      if (bandSeen.has(b.name)) return false;
      bandSeen.add(b.name);
      return true;
    }).map((b: Band) => ({
      name: b.name, frequency: b.lo, mode: b.mode ?? null, step: b.step ?? 0,
    }));
    const payload = {
      bookmarks: bookmarks.map((b: ServerBookmark) => ({
        name: b.name, frequency: b.frequency, mode: b.mode ?? null,
      })),
      bands,
    };
    VibePowerModule?.setBrowseItems?.(JSON.stringify(payload));
  }, [ituRegion]);

  // ── User bookmark management (menu BOOKMARKS pane) ────────────────────────
  const persistUserBookmarks = useCallback((next: UserBookmark[]) => {
    setUserBookmarks(next);
    saveUserBookmarks(next).catch(() => {});
  }, []);

  const onAddBookmark = useCallback((name: string, allInstances: boolean) => {
    const clean = name.trim();
    if (!clean) return;
    const bm: UserBookmark = {
      name:           clean,
      frequency:      Math.round(status.frequency),
      mode:           status.mode,
      bandwidth_low:  status.bandwidthLow,
      bandwidth_high: status.bandwidthHigh,
      group:          null, comment: null, extension: null,
      scope:          allInstances ? '' : baseUrl,
    };
    persistUserBookmarks(mergeBookmarks(userBookmarks, [bm]));
  }, [status.frequency, status.mode, status.bandwidthLow, status.bandwidthHigh,
      baseUrl, userBookmarks, persistUserBookmarks]);

  // The menu's saved list should show only what applies to THIS instance —
  // global ('') + this-instance — not bookmarks scoped to OTHER instances (a
  // 'this instance only' bookmark was showing on every instance's list).
  const visibleBookmarks = useMemo(
    () => bookmarksForInstance(userBookmarks, baseUrl),
    [userBookmarks, baseUrl],
  );

  const onDeleteBookmark = useCallback((bm: UserBookmark) => {
    persistUserBookmarks(userBookmarks.filter(
      (b: UserBookmark) => !(b.name === bm.name && b.frequency === bm.frequency && b.scope === bm.scope),
    ));
  }, [userBookmarks, persistUserBookmarks]);

  const onExportBookmarks = useCallback(() => {
    const list = userBookmarks;
    if (!list.length) { Alert.alert('Bookmarks', 'No bookmarks to export.'); return; }
    // Plain-array JSON — directly importable by desktop UberSDR's
    // local-bookmarks Import (JSON). Share as text: save/airdrop/paste.
    Share.share({ message: exportBookmarksJSON(list) }).catch(() => {});
  }, [userBookmarks]);

  const onImportBookmarks = useCallback((text: string, allInstances: boolean): string => {
    try {
      const incoming = parseBookmarksAny(text, allInstances ? '' : baseUrl);
      if (!incoming.length) return 'No bookmarks found (JSON or YAML).';
      persistUserBookmarks(mergeBookmarks(userBookmarks, incoming));
      return `Imported ${incoming.length} bookmark${incoming.length !== 1 ? 's' : ''}.`;
    } catch {
      return 'Could not parse that file (need JSON or YAML).';
    }
  }, [baseUrl, userBookmarks, persistUserBookmarks]);

  // Pick a bookmark file (JSON/YAML) from the Files app and import it.
  const onPickImportFile = useCallback(async (allInstances: boolean): Promise<string> => {
    try {
      const res = await DocumentPicker.getDocumentAsync({ type: '*/*', copyToCacheDirectory: true });
      if (res.canceled || !res.assets?.length) return '';
      const text = await FileSystem.readAsStringAsync(res.assets[0].uri);
      return onImportBookmarks(text, allInstances);
    } catch {
      return 'Could not read that file.';
    }
  }, [onImportBookmarks]);

  const showBandNotif = useCallback((bands: Band[]) => {
    if (!bands.length) return;
    const primary = bands[0];
    const range = `${fmtBandFreq(primary.lo)}–${fmtBandFreq(primary.hi)}`;
    let cond: string | null = null;
    let color: string | undefined;
    // Band conditions come from UberSDR's /api/noisefloor/latest (ft8_snr); only
    // UberSDR serves it. Don't attempt it on OWRX/Kiwi — they 404 (and the cache
    // clear in refreshBandSnr would otherwise be the only thing stopping the
    // previous instance's numbers leaking through).
    if (primary.type === 'ham' && (route.params.serverType ?? 'ubersdr') === 'ubersdr') {
      const snr = getBandSnrDb(baseUrl, primary.bandLabel);
      cond = propCondition(snr);
      if (snr !== null) {
        color = snr >= 30 ? 'rgba(60,220,90,0.95)'
              : snr >= 20 ? 'rgba(140,220,90,0.95)'
              : snr >= 6  ? 'rgba(255,200,80,0.95)'
              :             'rgba(235,90,80,0.95)';
      }
    }
    const primaryMsg = `BAND: ${range} · ${primary.name}`
      + (cond ? ` · Conditions: ${cond}` : '')
      + (bands.length > 1 && ituRegion ? ` (ITU R${ituRegion})` : '');
    const secondary = bands.slice(1).map((b: Band) => b.name).join('  │  ');
    vtsKey.current++;
    setVtsNotif({
      key: vtsKey.current, name: primaryMsg,
      secondary: secondary || undefined, kind: 'band', color,
    });
  }, [baseUrl, ituRegion]);

  const vtsCheck = useCallback((hz: number) => {
    // Band crossing
    const order: Record<string, number> = { ham: 0, broadcast: 1, utility: 2 };
    const bands = getBandsAtRegion(hz, ituRegion)
      .sort((a: Band, b: Band) => (order[a.type] ?? 9) - (order[b.type] ?? 9));
    const key = bands.length ? bands.map((b: Band) => b.name).join('|') : null;
    if (!vtsBandInit.current) {
      vtsBandInit.current = true;
      vtsBandKey.current = key;
    } else if (key !== vtsBandKey.current) {
      vtsBandKey.current = key;
      if (vtsDeferredBand.current) { clearTimeout(vtsDeferredBand.current); vtsDeferredBand.current = null; }
      if (bands.length) {
        if (Date.now() < vtsArrowJumpUntil.current) {
          vtsDeferredBand.current = setTimeout(() => {
            vtsDeferredBand.current = null;
            showBandNotif(bands);
          }, 3000);
        } else {
          showBandNotif(bands);
        }
      }
      // Band-aware tuning on boundary crossing. Fires for any tuning that ISN'T
      // the user hands-on in the app — lock-screen / Apple Watch / headphone /
      // car media-control skips all trigger it. Suppressed only while the user
      // is actively tuning in-app (recent markInteract: VFO drum, waterfall tap,
      // any touch) so the demod/step they're dialling in isn't yanked away.
      // 1.5s window comfortably covers the drum's inertia glide after release.
      const handsOn = Date.now() - lastInteractRef.current < 1500;
      if (!handsOn) {
        const d = bandTuneDefaults(hz, ituRegion);
        if (d.mode && d.mode in MODE_BANDWIDTHS) onMode(d.mode);
        if (d.step) setStep(d.step);
      }
    }
    // A live RDS/DAB station name (OWRX) owns the station display — it's the
    // actual decode of what you're hearing, so it wins over a bookmark guess.
    // The liveStation effect drives the name + popup; just keep the menu freq
    // pointed at the VFO and skip the bookmark match.
    if (liveStationRef.current) { setVtsMenuFreq(hz); return; }
    // Nearest station
    const nearest = findNearest(vtsBookmarks.current, hz);
    if (!nearest) {
      setVtsMenuName('');
      setVtsMenuFreq(undefined);
      vtsLastStation.current = '';
      return;
    }
    setVtsMenuName(nearest.name);
    setVtsMenuFreq(nearest.hz);
    // Popup ONLY when ON a station (≤99Hz) — the off-tune offset-arrow
    // variant is the skin's tuning guide, which was erratic on the popup
    // bar and is intentionally not ported. Off-tune resets the latch so
    // re-landing on the same station pops again.
    const onTune = Math.abs(nearest.offset) <= VTS_ON_HZ;
    if (!onTune) {
      vtsLastStation.current = '';
    } else if (nearest.name !== vtsLastStation.current) {
      vtsLastStation.current = nearest.name;
      vtsKey.current++;
      // On a digital-voice mode (DMR/YSF/…), a repeater bookmark and the live
      // caller alternate — hold the bookmark too so the pair stays pinned while
      // the QSO is live (rather than the bookmark timing out under the caller).
      const voiceMode = ['dmr', 'ysf', 'dstar', 'nxdn', 'm17', 'radel', 'radeu']
        .includes(String(client.current?.getStatus().mode ?? ''));
      setVtsNotif({ key: vtsKey.current, name: nearest.name, kind: 'station-on', hold: voiceMode, source: nearest.source, flag: nearest.flag });
    }
  }, [ituRegion, showBandNotif, onMode]);

  // Watch the tuned frequency (debounced — the drum emits many per second)
  useEffect(() => {
    const hz = status.frequency;
    if (!hz) return;
    const t = setTimeout(() => vtsCheck(hz), 250);
    return () => clearTimeout(t);
  }, [status.frequency, vtsCheck]);

  // Live RDS/DAB station name arrives async (no frequency change to trigger
  // vtsCheck), so react to it directly: drive the VTS station readout + popup,
  // uniform with the bookmark-derived station-on notif. Cleared name hands the
  // display back to the bookmark resolver on the next tune.
  useEffect(() => {
    const name = liveStation.name;
    if (!name) {
      // Live data cleared (tuned away / mode change / voice idle) — dismiss the
      // held popup and re-evaluate bookmarks for the current spot, so a held RDS
      // name / DMR caller falls back to the channel's bookmark instead of nothing.
      if (vtsLastStation.current) {
        vtsLastStation.current = '';
        setVtsNotif(null);
        vtsCheck(status.frequency);
      }
      return;
    }
    setVtsMenuName(name);
    setVtsMenuFreq(status.frequency);
    // RDS: append the scrolling radiotext after the station name (the VTS bar
    // marquees overflow). e.g. "BBC Nhtn — BBC Radio Northampton …We love …".
    const display = liveStation.text ? `${name} — ${liveStation.text}` : name;
    // WFM broadcast FM: show the RDS country flag + station logo (from PI/ECC).
    const wfm = status.mode === 'wfm';
    const flag = wfm && validIso(liveStation.countryIso) ? isoToFlag(liveStation.countryIso) : undefined;
    const logoUrl = wfm ? (liveLogo ?? undefined) : undefined;
    const composite = `${display}|${flag ?? ''}|${logoUrl ?? ''}`;
    if (composite !== vtsLastStation.current) {
      vtsLastStation.current = composite;
      vtsKey.current++;
      // Live server data (RDS/DMR/DAB) holds on screen until it changes/clears
      // — only the static bookmark/band notifs time out. Badge flags the source.
      setVtsNotif({ key: vtsKey.current, name: display, kind: 'station-on', hold: true, badge: liveBadgeRef.current, flag, logoUrl });
    }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [liveStation.name, liveStation.text, liveStation.countryIso, liveLogo, status.mode]);

  // ── Station logo (radio-browser favicon) ────────────────────────────────────
  // NOT gated on WFM any more. The gate existed because a station name only ever
  // arrived via RDS, which is FM-only — but a name now also comes from a bookmark or
  // the EiBi schedule, so an AM or shortwave station has one too, and EiBi even states
  // the transmitter's country outright. Refusing to look it up outside WFM meant the
  // browser client showed logos for stations the app wouldn't.
  useEffect(() => {
    const name = liveStation.name?.trim();
    const iso = validIso(liveStation.countryIso) ? liveStation.countryIso!.toUpperCase() : '';
    const key = `${name ?? ''}|${iso}`;
    if (key === lastLiveLogoKey.current) return;
    lastLiveLogoKey.current = key;
    if (!name) { setLiveLogo(null); return; }
    setLiveLogo(null);
    resolveStationLogo({ pi: liveStation.pi, name, iso: iso || undefined }).then((url) => {
      if (!destroyed.current && lastLiveLogoKey.current === key) setLiveLogo(url);
    });
  }, [liveStation.name, liveStation.countryIso, liveStation.pi]);

  // ── VTS-aware media session ────────────────────────────────────────────────
  // Track  = freq (user's unit) + demod + tune step ("648 kHz AM · 9 kHz step")
  // Artist = "VibeSDR: Radio Caroline" on a station, else the band
  //          ("VibeSDR: 40m Ham Band"); art = app icon + server-type logo.
  useEffect(() => {
    const hz = status.frequency;
    if (!hz) return;
    const t = setTimeout(() => {
      // CarFM contract (spec §5b): on broadcast FM, map RDS the way the ESP32
      // display expects — RadioText -> TITLE, station name (PS) -> ARTIST,
      // frequency -> ALBUM. Gadgetbridge relays these three, so this branch is
      // the whole system contract; the general SDR mapping below is bypassed.
      if (route.params.carFm && status.mode === 'wfm') {
        const np = fmNowPlaying({ ps: liveStation.name, rt: liveStation.text, freqHz: hz });
        VibePowerModule?.setNowPlaying(np.title, np.artist);
        VibePowerModule?.setNowPlayingAlbum?.(np.album);
        VibePowerModule?.setArtwork(route.params.isTcp ? 'rtltcp' : 'local');
        return;
      }
      const trim = (v: number, dp: number) =>
        v.toFixed(dp).replace(/\.?0+$/, '');
      const fq = freqUnit === 'hz' ? `${Math.round(hz)} Hz`
        : freqUnit === 'mhz' ? `${trim(hz / 1e6, 4)} MHz`
        : `${trim(hz / 1e3, 3)} kHz`;
      const st = mediaSkip === 'bookmark'
        ? 'bookmark skip'
        : (step >= 1000 ? `${trim(step / 1e3, 1)} kHz step` : `${step} Hz step`);
      const fqLine = `${fq} ${status.mode.toUpperCase()}`;
      // A live RDS/DAB station name becomes the TITLE (so it's prominent AND so a
      // DAB programme skip — which doesn't change the frequency — still changes the
      // now-playing metadata, forcing the lock-screen card to refresh). Otherwise
      // keep the freq/step title with the band/bookmark as the artist.
      let title: string, artist: string;
      if (liveStationRef.current) {
        title = liveStationRef.current;
        artist = `VibeSDR · ${fqLine}`;
      } else {
        const nearest = findNearest(vtsBookmarks.current, hz);
        let context: string;
        if (nearest && Math.abs(nearest.offset) <= 1000) {
          context = nearest.name;
        } else {
          const order: Record<string, number> = { ham: 0, broadcast: 1, utility: 2 };
          const bands = getBandsAtRegion(hz, ituRegion)
            .sort((a: Band, b: Band) => (order[a.type] ?? 9) - (order[b.type] ?? 9));
          context = bands.length ? bands[0].name : 'HF Radio';
        }
        title = `${fqLine} · ${st}`;
        artist = `VibeSDR: ${context}`;
      }
      VibePowerModule?.setNowPlaying(title, artist);
      // Local hardware / RTL-TCP reuse serverType 'ubersdr' for the client, but get
      // their own album-art inset so the card is distinct from a network session.
      const artType = route.params.isTcp ? 'rtltcp'
                    : route.params.isLocal ? 'local'
                    : (route.params.serverType ?? 'ubersdr');
      VibePowerModule?.setArtwork(artType);  // native caches per type
    }, 300);
    return () => clearTimeout(t);
  }, [status.frequency, status.mode, step, freqUnit, ituRegion, mediaSkip,
      serverBookmarks, userBookmarks, liveStation.name, liveStation.text]);

  useEffect(() => () => {
    if (vtsDeferredBand.current) clearTimeout(vtsDeferredBand.current);
  }, []);

  // Menu arrows: jump to next/previous bookmark (sets the bookmark's mode too)
  const onVtsJump = useCallback((dir: 'left' | 'right') => {
    const c = client.current; if (!c) return;
    const bm = findNextBookmark(vtsBookmarks.current, c.getStatus().frequency, dir);
    if (!bm) return;
    vtsArrowJumpUntil.current = Date.now() + 3000;
    onTuneHz(bm.frequency);
    const m = bm.mode?.toLowerCase();
    if (m && m in MODE_BANDWIDTHS) onMode(m as SDRMode);
  }, [onTuneHz, onMode]);
  const onVtsPrev = useCallback(() => onVtsJump('left'),  [onVtsJump]);
  const onVtsNext = useCallback(() => onVtsJump('right'), [onVtsJump]);

  // Search result tap: tune (+mode when the bookmark has one) and close menu
  // Tune from a search/list tap. For an explicit BAND selection we also apply
  // that band's demodulator + tune step (band-aware tuning) — a deliberate user
  // action, so it applies handheld too. Bookmark taps keep the bookmark's own
  // mode and leave the step untouched.
  const onSearchTune = useCallback((hz: number, mode?: string | null, isBand?: boolean, voiceStep?: boolean) => {
    setMenuOpen(false);
    const target = Math.round(hz);
    onTuneHz(target);
    const d = bandTuneDefaults(target, ituRegion);
    const explicit = mode?.toLowerCase() as SDRMode | undefined;
    if (isBand) {
      const m = d.mode ?? explicit;
      if (m && m in MODE_BANDWIDTHS) onMode(m);
      if (d.step) setStep(d.step);
    } else if (voiceStep) {
      // Voice/bookmark tune: explicit (spoken) mode wins, else the band default;
      // and adopt the band step too (e.g. Radio Caroline → MW 9 kHz).
      const m = (explicit && explicit in MODE_BANDWIDTHS) ? explicit : d.mode;
      if (m && m in MODE_BANDWIDTHS) onMode(m);
      if (d.step) setStep(d.step);
    } else if (explicit && explicit in MODE_BANDWIDTHS) {
      onMode(explicit);  // plain bookmark tap — mode only, step untouched
    }
  }, [onTuneHz, onMode, ituRegion]);

  // Menu INSTANCE row — ← BACK returns to the instance picker (it previously
  // fell back to just closing the menu). The ⟳ RECONNECT button was removed
  // 2026-06-12: it only recycled the spectrum client while the native audio
  // WS kept the old session → frozen waterfall, and the zombie watchdog +
  // revive() already cover real reconnects.
  const onBackToPicker = useCallback(() => {
    setMenuOpen(false);
    navigation.goBack();
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Stable handlers — inline lambdas defeat the React.memo on ControlsBar.
  const onStepOpen  = useCallback(() => setStepOpen(true), []);
  const onMenuOpen  = useCallback(() => setMenuOpen(true), []);
  const onFreqOpen  = useCallback(() => setFreqModalOpen(true), []);
  const onModeOpen  = useCallback(() => setModeSelOpen(true), []);
  const onAudioOpen = useCallback(() => setAudioSheetOpen(true), []);

  // ── CarFM face wiring ─────────────────────────────────────────────────────
  // Sample the (ref-based) audio SNR on a timer while the FM face is up, so the
  // meter is reactive without re-rendering on every VibeSignal event.
  useEffect(() => {
    if (!fmFaceActive) return;
    const t = setInterval(() => {
      const v = audioSnrRef.current;
      setFmSignalDb(Number.isFinite(v) ? v : null);
    }, 500);
    return () => clearInterval(t);
  }, [fmFaceActive]);

  // Presets = this-instance FM bookmarks (broadcast band), nearest-first.
  const fmPresets = useMemo<CarFmPreset[]>(() => (
    visibleBookmarks
      .filter((b: UserBookmark) =>
        b.mode === 'wfm' || (b.frequency >= 87_000_000 && b.frequency <= 108_500_000))
      .map((b: UserBookmark) => ({ name: b.name, frequency: b.frequency }))
      .sort((a, b) => a.frequency - b.frequency)
  ), [visibleBookmarks]);

  // Tune by the broadcast-FM channel raster (200 kHz Region 2, else 100 kHz).
  const onFmStep = useCallback((dir: 1 | -1) => {
    const raster = ituRegion === 2 ? 200_000 : 100_000;
    const cur = client.current?.getStatus().frequency ?? status.frequency;
    onTuneHz(Math.round(cur / raster) * raster + dir * raster);
  }, [ituRegion, status.frequency, onTuneHz]);

  // Seek = jump to the previous/next preset (true signal-seek is a follow-up).
  const onFmSeekPreset = useCallback((dir: 1 | -1) => {
    if (fmPresets.length === 0) { onFmStep(dir); return; }
    const cur = client.current?.getStatus().frequency ?? status.frequency;
    const sorted = fmPresets;
    let idx = sorted.findIndex((p) => p.frequency > cur + 1);      // next above
    if (dir < 0) idx = idx <= 0 ? sorted.length - 1 : idx - 1;
    else if (idx < 0) idx = 0;
    onTuneHz(sorted[Math.max(0, Math.min(sorted.length - 1, idx))].frequency);
  }, [fmPresets, status.frequency, onTuneHz, onFmStep]);

  // Save the current station as a preset, named from RDS PS when we have it.
  const onFmSavePreset = useCallback(() => {
    const name = (liveStationRef.current || '').trim()
      || `FM ${(status.frequency / 1e6).toFixed(1)}`;
    onAddBookmark(name, false);
  }, [status.frequency, onAddBookmark]);

  // First-run guided tour (dismissable). Spotlights the drum, step rate, the
  // disabled back-gesture, and the menu — opening it to show the route back to
  // the instance list. Fail-safe: always skippable; a target that can't be
  // measured falls back to a centred card.
  // A small render of the menu's instance section — shown in the tour instead of
  // opening the real menu (a second Modal over the coachmark wedges iOS).
  const menuMock = (
    <View style={{ borderRadius: 9, borderWidth: 1, borderColor: 'rgba(255,229,102,0.30)', backgroundColor: 'rgba(0,0,0,0.45)', padding: 9, gap: 7 }}>
      <Text style={{ color: 'rgba(255,255,255,0.45)', fontFamily: 'Atkinson Hyperlegible', fontSize: 9.5, letterSpacing: 2, marginBottom: 1 }}>☰  MENU</Text>
      <View style={{ borderWidth: 1, borderColor: 'rgba(255,255,255,0.16)', borderRadius: 5, paddingVertical: 7, paddingHorizontal: 10 }}>
        <Text style={{ color: 'rgba(255,255,255,0.82)', fontFamily: 'Atkinson Hyperlegible', fontSize: 11.5, letterSpacing: 0.5 }}>☆  SET DEFAULT</Text>
      </View>
      <View style={{ borderWidth: 1.5, borderColor: '#ffe566', backgroundColor: 'rgba(255,229,102,0.12)', borderRadius: 5, paddingVertical: 7, paddingHorizontal: 10 }}>
        <Text style={{ color: '#ffe566', fontFamily: 'Atkinson Hyperlegible', fontSize: 11.5, fontWeight: 'bold', letterSpacing: 0.5 }}>←  BACK TO INSTANCE LIST</Text>
      </View>
      <View style={{ borderWidth: 1, borderColor: 'rgba(255,255,255,0.16)', borderRadius: 5, paddingVertical: 7, paddingHorizontal: 10 }}>
        <Text style={{ color: 'rgba(255,255,255,0.82)', fontFamily: 'Atkinson Hyperlegible', fontSize: 11.5, letterSpacing: 0.5 }}>❔  REPLAY TUTORIAL</Text>
      </View>
    </View>
  );

  const sdrTour = useCoachmarkTour([
    { id: 'freq', title: 'Set the frequency',
      body: 'Tap the frequency readout to type one in directly (kHz or MHz).',
      target: tourRef('freqBox') },
    { id: 'mode', title: 'Choose the demodulator',
      body: 'Tap the mode to pick how the signal is decoded — AM, SSB (USB/LSB), CW, NFM/WFM and more.',
      target: tourRef('modeBtn') },
    { id: 'drum', title: 'Fine-tune with the VFO drum',
      body: 'Spin the drum to move up and down the band. The right-hand wheel zooms the waterfall.',
      target: tourRef('vfoDrum') },
    { id: 'step', title: 'Step rate',
      body: 'Sets how far each drum move jumps — a small step for fine tuning, a large one to skip across bands. Tap it to change.',
      target: tourRef('stepBtn') },
    { id: 'back', title: 'No back-swipe here',
      body: "The phone's Back gesture is switched off over the tuning area so it can't fight the drum.",
      target: tourRef('vfoDrum') },
    { id: 'menu', title: 'Everything else: the menu',
      body: 'Bandwidth, modes, noise reduction, the auto notch, decoders, bookmarks and settings all live here. And since Back is off, this is where you return to the server list:',
      target: tourRef('menuBtn'), illustration: menuMock },
  ], { storageKey: 'lsv_tour_sdr_v3' });
  const onReplayTour = useCallback(() => {
    setMenuOpen(false);
    setTimeout(() => sdrTour.restart(), 320);
  }, [sdrTour]);
  // Auto-start once on the first successful connection, after the controls have
  // laid out (so the drum/step/menu can be measured).
  useEffect(() => {
    if (!connected) return;
    // Also wait for the launch splash to clear — a first-launch deep-link or
    // default-instance auto-connect can reach the SDR screen while the splash is
    // still holding on the CONTINUE notice; the tour must not draw over it.
    let t: ReturnType<typeof setTimeout>;
    const unsub = splashBridge.whenDismissed(() => {
      t = setTimeout(() => { sdrTour.maybeAutoStart(); }, 1500);
    });
    return () => { unsub(); clearTimeout(t); };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [connected]);

  // ── Layout ────────────────────────────────────────────────────────────────

  const bottomInset = insets.bottom;

  return (
    <View
      style={styles.root}
      // Capture-phase touch sniff (returns false — never steals the touch):
      // marks interaction for smooth tune / idle saver on any Pressable UI.
      onStartShouldSetResponderCapture={() => { markInteract(); return false; }}
      // Real layout height — Android's Dimensions window height disagrees
      // with the laid-out root (status/nav bar handling), which pushed every
      // pillBottom-anchored overlay (spec-ratio popup, VTS bar, decoder
      // panel) off the bottom on Android.
      onLayout={(e: { nativeEvent: { layout: { height: number } } }) =>
        setRootH(e.nativeEvent.layout.height)}
    >
      <StatusBar barStyle="light-content" backgroundColor="#000" translucent={false} />

      {/* Waterfall — fills screen below the status bar / Dynamic Island so the
          band plan strip is never hidden under the notch */}
      <View style={{ marginTop: insets.top }}>
      <WaterfallView
        frameSink={wfFrameSink}
        binCount={status.binCount}
        centerHz={status.centerHz}
        bwHz={status.bwHz}
        tuneHz={status.frequency}
        filterLow={status.bandwidthLow}
        filterHigh={status.bandwidthHigh}
        dbMin={dbMin}
        dbMax={dbMax}
        wfCoarse={wfCoarse}
        colormap={colormap}
        width={screenW}
        height={screenH - insets.top}
        // Block waterfall tune/pan/pinch in the bottom gap (home-indicator
        // zone): the whole strip below the pill when controls show, else just
        // the home bar. Preserves swipe-up-to-minimise + menu Modals.
        bottomGuard={controlsHidden ? bottomInset : bottomInset + 8}
        ituRegion={1}
        onPanDelta={onWfPanDelta}
        onZoomDelta={onWfZoomDelta}
        onTapTune={onWfTapTune}
        onPinchZoom={onWfPinchZoom}
        specShow={specShow}
        autoContrast={autoContrast}
        specSmoothing={specSmoothing}
        specFloor={specFloor}
        specPeakScale={specPeakScale}
        peakHold={peakHold}
        spatialSmooth={spatialSmooth}
        smoothTune={smoothTune}
        lastInteractAt={lastInteractRef}
        wfBrightness={wfBrightness}
        wfContrast={wfContrast}
        wfSharpness={wfSharpness}
        frameRate={frameRate}
        needleColor={vfoNeedle}
        needleIntensity={vfoIntensity}
        needleFrost={vfoFrost}
        bgImageUrl={bgImageUrl}
        bgOpacity={bgOpacity / 10}
        stationId={stationId}
        specFrac={specFrac}
        panLoHz={walls?.loHz}
        panHiHz={walls?.hiHz}
        showWalls={!!walls}
        // RF-centre marker = the dongle/RF centre (derived to mirror the shim).
        // Sits at the display centre until the dongle locks; then the view pans
        // on across the captured band and the marker slides off to the side.
        centerMarkerHz={localRf?.rf ?? status.centerHz}
        showCenterMarker={isLocal && !vfoLocked}
      />
      </View>

      {/* Spec ratio overlay — floats above pill */}
      <SpecRatioOverlay
        visible={ratioOverlayOpen}
        isLandscape={isLandscape}
        portraitRatio={specRatioPortrait}
        landscapeRatio={specRatioLandscape}
        bottomOffset={pillBottom + 8}
        onChange={(p, l) => { setSpecRatioPortrait(p); setSpecRatioLandscape(l); }}
        onClose={() => setRatioOverlayOpen(false)}
      />
      {/* Decoder panel needs vertical space phone landscape doesn't have (skin
          parity: panel is portrait-only) — decoder keeps running, banner
          tells the user where it went. Tablets (iPad) have the room, so the
          panel is allowed in landscape there. */}
      {isLandscape && !isTablet && (activeDecoder !== null || spotsKind !== null) ? (
        <View style={[styles.rotateBanner, { bottom: pillBottom + 8 }]}
              pointerEvents="none">
          <Text style={styles.rotateBannerText}>
            ⟳ ROTATE TO PORTRAIT TO VIEW DECODER
          </Text>
        </View>
      ) : (
        <DecoderPanel
          activeDecoder={activeDecoder}
          decoderText={decoderText}
          aircraft={aircraft}
          decoderStatus={decoderStatus}
          decoding={decoding}
          bottomOffset={pillBottom + 8}
          onClear={() => setDecoderText('')}
          onClose={closeDecoder}
          morseQuality={morseQuality}
          onMorseQuality={onMorseQuality}
          spotsKind={spotsKind}
          spots={spots}
          onTuneHz={onTuneHz}
          imageRef={decoderImageRef}
          onImageStatus={setDecoderStatus}
        />
      )}

      {/* Chat rotate hint — chat is portrait-only, button stays for unread */}
      {chatRotateHint && (
        <View style={[styles.rotateBanner, {
                bottom: pillBottom + 8 +
                  (isLandscape && !isTablet && (activeDecoder !== null || spotsKind !== null) ? 42 : 0),
              }]}
              pointerEvents="none">
          <Text style={styles.rotateBannerText}>
            ⟳ ROTATE TO PORTRAIT FOR CHAT
          </Text>
        </View>
      )}

      {/* Reconnect failed (server full / rate-limited) — tap retries */}
      {reconnectFailedUi && (
        <TouchableOpacity style={[styles.mutedBanner, { top: insets.top + 46 }]}
          onPress={() => { setReconnectFailedUi(false); setDataSaverOff(false); unmute(); fullReconnect(); }}
          activeOpacity={0.85}>
          <Text style={styles.mutedBannerText}>⚠️ RECONNECT FAILED — TAP TO RETRY</Text>
        </TouchableOpacity>
      )}

      {/* OWRX server crashed/restarted (common on OWRX). Keep the app alive and
          tell the user to wait before reconnecting (the server's still booting). */}
      {serverLost && (() => {
        const lostLabel = route.params.serverType === 'kiwi' ? 'KiwiSDR'
                        : route.params.serverType === 'owrx' ? 'OpenWebRX'
                        : 'SDR';
        return (
        <View style={styles.serverLostWrap} pointerEvents="box-none">
          <View style={styles.serverLostCard}>
            <Text style={styles.serverLostTitle}>{lostLabel} server stopped responding</Text>
            <Text style={styles.serverLostBody}>{route.params.serverType === 'kiwi'
              ? "The receiver dropped the connection. KiwiSDR owners with few slots often restrict access: some allow only their own web page, so apps like VibeSDR are refused the moment they connect; some block broadcast / commercial bands and disconnect you when you tune there. If reconnecting drops the same way it's likely an owner restriction — try another receiver. Otherwise it may just be busy or restarting: wait a minute and reconnect."
              : `The receiver dropped the connection — ${lostLabel} servers restart from time to time. Please wait a minute, then reconnect — or pick another from the list.`}</Text>
            <View style={styles.serverLostBtnRow}>
              <TouchableOpacity style={[styles.serverLostBtn, styles.serverLostBtnAlt]}
                onPress={() => navigation.goBack()} activeOpacity={0.85}>
                <Text style={[styles.serverLostBtnText, styles.serverLostBtnAltText]}>INSTANCE LIST</Text>
              </TouchableOpacity>
              <TouchableOpacity style={styles.serverLostBtn}
                onPress={() => { setServerLost(false); fullReconnect(); }} activeOpacity={0.85}>
                <Text style={styles.serverLostBtnText}>RECONNECT</Text>
              </TouchableOpacity>
            </View>
          </View>
        </View>
        );
      })()}

      {/* Kiwi receiver full (too_busy) — all channels in use. */}
      {/* Read-only: another client owns the tuner (public SpyServers are usually
          single-tuner). Passive strip, not a blocking card — you can still listen
          to whatever they have it tuned to. */}
      {readOnly && (
        <View pointerEvents="none" style={{
          position: 'absolute', top: 0, left: 0, right: 0, alignItems: 'center', zIndex: 40,
        }}>
          <View style={{
            marginTop: 6, paddingHorizontal: 12, paddingVertical: 5, borderRadius: 6,
            backgroundColor: 'rgba(0,0,0,0.75)', borderWidth: 1, borderColor: '#ffb84d',
          }}>
            <Text style={{ color: '#ffb84d', fontSize: 12, textAlign: 'center' }}>
              Listen-only — another user is controlling this receiver
            </Text>
          </View>
        </View>
      )}

      {serverBusy && (
        <View style={styles.serverLostWrap} pointerEvents="box-none">
          <View style={styles.serverLostCard}>
            <Text style={styles.serverLostTitle}>Receiver unavailable</Text>
            <Text style={styles.serverLostBody}>This KiwiSDR has no free channel for you right now — it may be full, or its channels may be password-protected or limited to local users (the directory's user count can be out of date). Pick another receiver, or try again shortly.</Text>
            <View style={styles.serverLostBtnRow}>
              <TouchableOpacity style={[styles.serverLostBtn, styles.serverLostBtnAlt]}
                onPress={() => navigation.goBack()} activeOpacity={0.85}>
                <Text style={[styles.serverLostBtnText, styles.serverLostBtnAltText]}>INSTANCE LIST</Text>
              </TouchableOpacity>
              <TouchableOpacity style={styles.serverLostBtn}
                onPress={() => { setServerBusy(false); fullReconnect(); }} activeOpacity={0.85}>
                <Text style={styles.serverLostBtnText}>RETRY</Text>
              </TouchableOpacity>
            </View>
          </View>
        </View>
      )}

      {/* Connection to an UberSDR instance dropped (e.g. it rebooted). It auto-
          reconnects, but show a clear popup so the app doesn't just look frozen. */}
      {/* Returning from the background — the spectrum was paused, so show a calm
          reinitialising notice while the waterfall re-subscribes (no buttons; it
          clears itself on the first frame, or escalates to "Connection lost"). */}
      {reinit && !connLost && !dataSaverOff && !serverLost && !serverBusy && (
        <View style={styles.serverLostWrap} pointerEvents="box-none">
          <View style={styles.serverLostCard}>
            <Text style={styles.serverLostTitle}>Reinitialising</Text>
            <Text style={styles.serverLostBody}>
              Resuming the waterfall and spectrum — this takes a second or two…
            </Text>
            <ActivityIndicator color="#ffb84d" style={{ marginBottom: 4 }} />
          </View>
        </View>
      )}

      {/* Audio resumed fine but the waterfall/spectrum never re-subscribed after
          a background — give the user an escape (the rest of the app is alive). */}
      {specFailed && !reinit && !connLost && !dataSaverOff && !serverLost && !serverBusy && (
        <View style={styles.serverLostWrap} pointerEvents="box-none">
          <View style={styles.serverLostCard}>
            <Text style={styles.serverLostTitle}>Waterfall didn’t resume</Text>
            <Text style={styles.serverLostBody}>
              Audio is still running, but the waterfall and spectrum didn’t restart. Reconnect to restore them, or pick another instance.
            </Text>
            <View style={styles.serverLostBtnRow}>
              <TouchableOpacity style={[styles.serverLostBtn, styles.serverLostBtnAlt]}
                onPress={() => navigation.goBack()} activeOpacity={0.85}>
                <Text style={[styles.serverLostBtnText, styles.serverLostBtnAltText]}>INSTANCE LIST</Text>
              </TouchableOpacity>
              <TouchableOpacity style={styles.serverLostBtn}
                onPress={() => { setSpecFailed(false); fullReconnect(); }} activeOpacity={0.85}>
                <Text style={styles.serverLostBtnText}>RECONNECT</Text>
              </TouchableOpacity>
            </View>
          </View>
        </View>
      )}

      {connLost && !reinit && !specFailed && !dataSaverOff && !serverLost && !serverBusy && (
        <View style={styles.serverLostWrap} pointerEvents="box-none">
          <View style={styles.serverLostCard}>
            <Text style={styles.serverLostTitle}>Connection lost</Text>
            <Text style={styles.serverLostBody}>
              Lost connection to {instanceName || 'the instance'} — trying to reconnect…
            </Text>
            <ActivityIndicator color="#ffb84d" style={{ marginBottom: 14 }} />
            <View style={styles.serverLostBtnRow}>
              <TouchableOpacity style={[styles.serverLostBtn, styles.serverLostBtnAlt]}
                onPress={() => navigation.goBack()} activeOpacity={0.85}>
                <Text style={[styles.serverLostBtnText, styles.serverLostBtnAltText]}>INSTANCE LIST</Text>
              </TouchableOpacity>
              <TouchableOpacity style={styles.serverLostBtn}
                onPress={() => { setConnLost(false); fullReconnect(); }} activeOpacity={0.85}>
                <Text style={styles.serverLostBtnText}>RECONNECT</Text>
              </TouchableOpacity>
            </View>
          </View>
        </View>
      )}

      {/* Initial connect never completed (wedged host/shim/USB). Escape hatch so
          the app can never be permanently stuck on the connecting spinner. */}
      {connTimedOut && !connected && !serverLost && !serverBusy && !connLost && !dataSaverOff && (
        <View style={styles.serverLostWrap} pointerEvents="box-none">
          <View style={styles.serverLostCard}>
            <Text style={styles.serverLostTitle}>Couldn’t connect</Text>
            <Text style={styles.serverLostBody}>
              No response from {instanceName || 'the receiver'}. {route.params.isLocal
                ? 'Check the SDR is plugged in and try again, or pick another instance.'
                : isKiwi
                  ? "It may be offline or a temporary network issue — but if a retry also fails, this KiwiSDR's owner likely only allows their own web page and blocks apps like VibeSDR. Try another, or use UberSDR / OpenWebRX."
                  : 'It may be offline or unreachable — try again or pick another instance.'}
            </Text>
            <View style={styles.serverLostBtnRow}>
              <TouchableOpacity style={[styles.serverLostBtn, styles.serverLostBtnAlt]}
                onPress={() => navigation.goBack()} activeOpacity={0.85}>
                <Text style={[styles.serverLostBtnText, styles.serverLostBtnAltText]}>INSTANCE LIST</Text>
              </TouchableOpacity>
              <TouchableOpacity style={styles.serverLostBtn}
                onPress={() => { setConnTimedOut(false); fullReconnect(); }} activeOpacity={0.85}>
                <Text style={styles.serverLostBtnText}>RETRY</Text>
              </TouchableOpacity>
            </View>
          </View>
        </View>
      )}

      {/* Paused → disconnected — tap does a full from-scratch reconnect */}
      {dataSaverOff && !reconnectFailedUi && (
        <TouchableOpacity style={[styles.mutedBanner, { top: insets.top + 46 }]}
          onPress={() => { setDataSaverOff(false); unmute(); fullReconnect(); }} activeOpacity={0.85}>
          <Text style={styles.mutedBannerText}>⏸ PAUSED — TAP TO RECONNECT</Text>
        </TouchableOpacity>
      )}

      {/* Local hardware muted (media-control pause): the RTL/waterfall keep running,
          only audio is muted. Tap unmutes + clears the media-control paused state.
          (Network instances disconnect on pause, so this is local-only.) */}
      {isLocal && isMuted && !dataSaverOff && !reconnectFailedUi && (
        <TouchableOpacity style={[styles.mutedBanner, { top: insets.top + 46 }]}
          onPress={unmute} activeOpacity={0.85}>
          <Text style={styles.mutedBannerText}>🔇 AUDIO MUTED — TAP TO UNMUTE</Text>
        </TouchableOpacity>
      )}

      {/* Restore chevron when controls are hidden (full-screen waterfall) */}
      {controlsHidden && (
        <TouchableOpacity
          style={[styles.restoreBtn, { bottom: bottomInset + 10 }]}
          onPress={() => setControlsHidden(false)} activeOpacity={0.8} hitSlop={12}>
          <Text style={styles.restoreBtnText}>▲</Text>
        </TouchableOpacity>
      )}

      {/* Controls pill — absolute overlay, margin 8px each side */}
      {!controlsHidden && <View
        style={[styles.pillWrap, { bottom: bottomInset + 8 }]}
        onLayout={(e: any) => {
          // Track pill top so bottom-anchored overlays can sit above it
          const { y } = e.nativeEvent.layout;
          pillYRef.current = y;
          setPillBottom((rootH > 0 ? rootH : screenH) - y);
        }}
      >
        <ControlsBar
          readOnly={readOnly}
          sessionLeft={sessionLeftMs == null ? null : {
            text: `${Math.floor(sessionLeftMs / 60000)}:${String(Math.floor((sessionLeftMs % 60000) / 1000)).padStart(2, '0')}`,
            urgent: sessionLeftMs < 120_000,
          }}
          frequency={status.frequency}
          mode={status.mode}
          step={step}
          connected={connected}
          bottomInset={0}
          instanceHost={instanceName ?? baseUrl}
          meterBus={meterBus.current}
          signalMode={signalMode}
          fmStereo={fmStereo}
          isRecording={isRecording}
          recSeconds={recSeconds}
          chatUnread={chatUnread}
          onVfoDelta={onVfoDelta}
          onBwDelta={onBwDelta}
          onMode={onMode}
          onStep={onStepOpen}
          onMenu={onMenuOpen}
          onChat={openChat}
          onAudio={onAudioOpen}
          onFreqTap={onFreqOpen}
          onModeTap={onModeOpen}
          freqUnit={freqUnit}
          chatShareDisabled={isLocal}
          chatDisabled={isKiwi}
        />
      </View>}

      {/* VTS popup — station / band-crossing notifications above the pill */}
      {!controlsHidden && <VTSBar notif={vtsNotif} bottom={pillBottom + 8} serverType={isLocal ? 'local' : route.params.serverType} />}

      {/* Floating CENTRE ON VFO — unlocked + VFO off-screen (BRIEF §5.8) */}
      <CenterVfoButton visible={vfoOffscreen && !controlsHidden} bottom={pillBottom + 56} onPress={onCentreVfo} />

      {/* Menu sheet */}
      <MenuSheet
        visible={menuOpen}
        serverType={route.params.serverType ?? 'ubersdr'}
        dabProgrammes={dabProgrammes}
        activeDabId={activeDabId}
        onSelectDab={(id) => { client.current?.setAudioServiceId?.(id); setActiveDabId(id); }}
        dabSpeed={dabSpeed}
        onDabSpeed={onDabSpeed}
        vtsName={vtsMenuName}
        vtsFreq={vtsMenuFreq}
        onVtsPrev={onVtsPrev}
        onVtsNext={onVtsNext}
        searchBookmarks={searchBookmarks}
        searchBands={searchBands}
        onSearchTune={onSearchTune}
        userBookmarks={visibleBookmarks}
        currentFreq={status.frequency}
        currentMode={status.mode}
        onAddBookmark={onAddBookmark}
        onDeleteBookmark={onDeleteBookmark}
        onExportBookmarks={onExportBookmarks}
        onImportBookmarks={onImportBookmarks}
        onPickImportFile={onPickImportFile}
        colormap={colormap}
        dbMin={dbMin}
        dbMax={dbMax}
        filterLow={status.bandwidthLow}
        filterHigh={status.bandwidthHigh}
        bwEdgeMax={client.current ? filterEdgeMax(client.current.caps, status.mode) : 6000}
        nr={nrMode !== 'off'}
        nb={nb}
        recording={isRecording}
        recSeconds={recSeconds}
        signalMode={signalMode}
        displayStyle={displayStyle}
        serverName={instanceName ?? ''}
        serverUrl={baseUrl}
        onClose={() => setMenuOpen(false)}
        onLocalHardware={isLocal ? () => { setMenuOpen(false); setHwOpen(true); } : undefined}
        isTcp={!!route.params.isTcp}
        onColormap={setColormap}
        onDbMin={setDbMin}
        onDbMax={setDbMax}
        onFilterLow={onFilterLow}
        onFilterHigh={onFilterHigh}
        onFilterBoth={onFilterBoth}
        onNr={onNrMode}
        onZoomIn={onZoomIn}
        onZoomOut={onZoomOut}
        onZoomMin={onZoomMin}
        onZoomMax={onZoomMax}
        onSetDefault={onSetDefault}
        isDefaultInstance={isDefault}
        isFavourite={isFavourite}
        onToggleFavourite={isLocal ? undefined : onToggleFavourite}
        decMode={selDecoder}
        decOn={activeDecoder !== null && activeDecoder === selDecoder}
        onDecToggle={onDecToggle}
        spotsKind={spotsKind}
        onSpotsToggle={onSpotsToggle}
        onServerMap={(k) => { setMenuOpen(false); setMapKind(k); }}
        onSpotsMap={() => {
          setMenuOpen(false);
          // The map plots the live Digital Spots feed — start it if it isn't on.
          if (spotsKindRef.current !== 'digi') onSpotsToggle('digi');
          setLocalMapOpen(true);
        }}
        rttySettings={rttySettings}
        onRttySettings={onRttySettings}
        wefaxLpm={wefaxLpm}
        onWefaxLpm={onWefaxLpm}
        onNb={onNb}
        onRec={toggleRecording}
        // OWRX/Kiwi have no SNR meter — skip 'snr' in the cycle, stay on S-meter/dBFS.
        onSignalMode={(m: 'snr' | 'smeter' | 'dbfs') => setSignalMode((isOwrx || isKiwi) && m === 'snr' ? 'smeter' : m)}
        onDisplayStyle={handleDisplayStyle}
        onBack={onBackToPicker}
        onAdminLink={onAdminLink}
        onReplayTour={onReplayTour}
        onResetSettings={() => {
          setDbMin(-120); setDbMax(-20); setColormap('Jet');
          setStep(1000);
          setSpecShow(true); setSpecSmoothing(5); setSpecFloor(0);
          setSpecPeakScale(10); setPeakHold(true);
          setWfBrightness(0); setWfContrast(0); setWfSharpness(5);
          setAutoContrast(5); setSpatialSmooth(true);
          setWfCoarse('auto'); setFrameRate('20fps'); setVfoNeedle('#ffffff'); setVfoIntensity(5); setVfoFrost(5); setBgOpacity(3);
          setSpecRatioPortrait(0.28); setSpecRatioLandscape(0.20);
          onNrMode('off'); onNb(false);
          onSnrSquelch(-999); onFmSquelch(-999);
          if (serverDspEnabled) onServerDsp(false);
          setMenuOpen(false);
          // Bookmarks are precious — never clear silently with a reset
          const instCount = bookmarksForInstance(userBookmarks, baseUrl)
            .filter((b: UserBookmark) => b.scope === baseUrl).length;
          if (instCount > 0) {
            Alert.alert(
              'Bookmarks',
              `Keep the ${instCount} bookmark${instCount !== 1 ? 's' : ''} saved for this instance?`,
              [
                { text: 'Keep', style: 'default' },
                { text: 'Clear', style: 'destructive',
                  onPress: () => persistUserBookmarks(withoutInstance(userBookmarks, baseUrl)) },
              ],
            );
          }
        }}
        onSpecRatio={() => { setMenuOpen(false); setRatioOverlayOpen(true); }}
        vfoNeedle={vfoNeedle}           onVfoNeedle={setVfoNeedle}
        vfoIntensity={vfoIntensity}       onVfoIntensity={setVfoIntensity}
        vfoFrost={vfoFrost}               onVfoFrost={setVfoFrost}
        bgOpacity={bgOpacity}             onBgOpacity={(v: number) => { bgOpacityUserSet.current = true; setBgOpacity(v); }}
        hasBgImage={bgImageUrl != null}
        wfCoarse={wfCoarse}             onWfCoarse={setWfCoarse}
        autoContrast={autoContrast}     onAutoContrast={setAutoContrast}
        spatialSmooth={spatialSmooth}   onSpatialSmooth={setSpatialSmooth}
        wfBrightness={wfBrightness}     onWfBrightness={setWfBrightness}
        wfContrast={wfContrast}         onWfContrast={setWfContrast}
        wfSharpness={wfSharpness}       onWfSharpness={setWfSharpness}
        specShow={specShow}             onSpecShow={setSpecShow}
        specSmoothing={specSmoothing}   onSpecSmoothing={setSpecSmoothing}
        specFloor={specFloor}           onSpecFloor={setSpecFloor}
        specPeakScale={specPeakScale}   onSpecPeakScale={setSpecPeakScale}
        peakHold={peakHold}             onPeakHold={setPeakHold}
        frameRate={frameRate}           onFrameRate={onFrameRate}
        smoothTune={smoothTune}         onSmoothTune={onSmoothTune}
        idleSlow={idleSlow}             onIdleSlow={onIdleSlow}
        drumMode={drumMode}             onDrumMode={onDrumMode}
        mediaSkip={mediaSkip}           onMediaSkip={onMediaSkip}
        hapticsEnabled={hapticsEnabled} onHaptics={onHaptics} hapticsHardware={hapticsHardware}
        onCentreVfo={onCentreVfo}       onHideControls={onHideControls}
        vfoLocked={vfoLocked}           onToggleVfoLock={onToggleVfoLock}
        onDispReset={onDispReset}       onDispSaveServer={onDispSaveServer}
        onDispSaveGlobal={onDispSaveGlobal}
        snrSquelch={snrSquelch}         onSnrSquelch={onSnrSquelch}
        fmSquelch={fmSquelch}           onFmSquelch={onFmSquelch}
        localSquelch={hwSquelch}        onLocalSquelch={isLocal ? onLocalSquelch : undefined}
        kiwiSquelch={kiwiSquelch}       onKiwiSquelch={isKiwi ? onKiwiSquelch : undefined}
        localNR={hwNrLevel}             onLocalNR={isLocal ? onLocalNR : undefined}
        notchOn={isLocal ? hwNotch : netNotch}   onNotch={isLocal ? onLocalNotch : onNetNotch}
        eibiEnabled={eibiEnabled}        onEibiToggle={onEibiToggle}
        isFmMode={status.mode === 'fm' || status.mode === 'nfm'}
        serverDspEnabled={serverDspEnabled}
        serverDspFilter={serverDspFilter}
        serverDspParams={serverDspParams}
        dspFilters={dspFilters}
        dspError={dspError}
        onServerDsp={onServerDsp}
        onServerDspFilter={onServerDspFilter}
        onServerDspParam={onServerDspParam}
        serverVersion={serverVersion}
        serverLabel={serverLabel}
        onOwrxSquelch={(db) => client.current?.setSquelch?.(db)}
        onOwrxNr={(th) => client.current?.setNr?.(th)}
        owrxDspDefaults={owrxDspDefaults}
        onAbout={() => { setMenuOpen(false); setAboutOpen(true); }}
        onRecordings={() => { setMenuOpen(false); setRecordingsOpen(true); }}
      />

      {/* About VibeSDR — V2 changes, credits, GPL-3.0 */}
      <AboutOverlay visible={aboutOpen} onClose={() => setAboutOpen(false)} />
      <RecordingsOverlay
        visible={recordingsOpen}
        onClose={() => setRecordingsOpen(false)}
        onActiveChange={onRecordingsActive}
      />

      {/* First-run guided tour (dismissable) — renders nothing until active */}
      {sdrTour.overlay}

      {/* Step picker — bottom sheet */}
      <StepPicker
        visible={stepOpen}
        currentStep={step}
        steps={stepsForFreq(status.frequency)}
        onSelect={hz => { setStep(hz); }}
        onClose={() => setStepOpen(false)}
      />

      {/* Mode selector */}
      <ModeSelector
        visible={modeSelOpen}
        gainControl={isLocal ? {
          gains: hwGains, gainTenthDb: hwGain, auto: hwAutoGain, onAuto: onHwAuto, onGain: onHwGain,
        } : undefined}
        current={status.mode}
        modes={isLocal ? LOCAL_MODES : route.params.serverType === 'owrx' ? serverModes : undefined}
        activeDecoder={route.params.serverType === 'owrx'
          ? (activeDecoder === 'sstv' ? 'sstv' : activeDecoder === 'wefax' ? 'fax' : undefined)
          : undefined}
        filterLow={status.bandwidthLow}
        filterHigh={status.bandwidthHigh}
        bwEdgeMax={client.current ? filterEdgeMax(client.current.caps, status.mode) : 6000}
        onFilterBoth={onFilterBoth}
        onSelect={onMode}
        onClose={() => setModeSelOpen(false)}
      />

      {/* Audio sheet — NR/NB/squelch/notch/REC + server NR */}
      <AudioSheet
        visible={audioSheetOpen}
        onClose={() => setAudioSheetOpen(false)}
        onDismiss={() => {
          const p = pendingRecShare.current;
          if (p) { pendingRecShare.current = null; VibePowerModule?.shareRecording(p); }
        }}
        serverType={route.params.serverType ?? 'ubersdr'}
        isLocal={isLocal}
        nr={nrMode !== 'off'}
        onNr={onNrMode}
        nb={nb}
        onNb={onNb}
        recording={isRecording}
        recSeconds={recSeconds}
        onRec={toggleRecording}
        onRecordings={() => { setAudioSheetOpen(false); setRecordingsOpen(true); }}
        snrSquelch={snrSquelch}          onSnrSquelch={onSnrSquelch}
        localSquelch={hwSquelch}         onLocalSquelch={isLocal ? onLocalSquelch : undefined}
        localNR={hwNrLevel}              onLocalNR={isLocal ? onLocalNR : undefined}
        kiwiSquelch={kiwiSquelch}        onKiwiSquelch={isKiwi ? onKiwiSquelch : undefined}
        fmSquelch={fmSquelch}            onFmSquelch={onFmSquelch}
        isFmMode={status.mode === 'fm' || status.mode === 'nfm'}
        notchOn={isLocal ? hwNotch : netNotch}   onNotch={isLocal ? onLocalNotch : onNetNotch}
        onOwrxSquelch={(db) => client.current?.setSquelch?.(db)}
        onOwrxNr={(th) => client.current?.setNr?.(th)}
        owrxDspDefaults={owrxDspDefaults}
        serverDspEnabled={serverDspEnabled}
        serverDspFilter={serverDspFilter}
        serverDspParams={serverDspParams}
        dspFilters={dspFilters}
        dspError={dspError}
        onServerDsp={onServerDsp}
        onServerDspFilter={onServerDspFilter}
        onServerDspParam={onServerDspParam}
      />

      {/* v4 local hardware: RTL-SDR controls submenu */}
      {isLocal ? (
        <LocalHardwarePanel
          isSpy={isSpy}
          visible={hwOpen}
          onClose={() => setHwOpen(false)}
          gains={hwGains}
          gainTenthDb={hwGain}
          autoGain={hwAutoGain}
          onAuto={onHwAuto}
          onGain={onHwGain}
          ppm={hwPpm}
          onPpm={onHwPpm}
          sampleRate={hwSampleRate}
          onSampleRate={onHwSampleRate}
          isTcp={!!route.params.isTcp}
          serverRates={hwServerRates}
          lockedRate={hwLockedRate}
          biasTee={hwBiasTee}
          onBiasTee={onHwBiasTee}
          agc={hwAgc}
          onAgc={onHwAgc}
          directSampling={hwDirectSamp}
          onDirectSampling={onHwDirectSamp}
          deemph={hwDeemph}
          onDeemph={onHwDeemph}
          stereo={hwStereo}
          onStereo={onHwStereo}
        />
      ) : null}

      {/* Server map overlay (HFDL / Digital / CW — full-screen WebView Leaflet) */}
      <MapOverlay
        visible={mapKind !== null}
        kind={mapKind}
        baseUrl={baseUrl}
        sessionUuid={sessionUuid}
        onClose={() => setMapKind(null)}
      />

      {/* On-device FT8 spots map (Local/Kiwi): RN-fed spots (each with a grid),
          receiver position from device GPS / Kiwi gps / a picked city. */}
      <MapOverlay
        visible={localMapOpen}
        kind="digi"
        local
        baseUrl={isLocal ? 'https://localhost' : baseUrl}
        wsBaseOverride={decoderBase}
        sessionUuid={sessionUuid}
        rxLat={recvLoc?.lat ?? 0}
        rxLon={recvLoc?.lon ?? 0}
        spots={spots}
        onPickCity={() => setCityPickerOpen(true)}
        onClose={() => setLocalMapOpen(false)}
        disconnected={serverLost || serverBusy || connTimedOut}
        onBackToList={() => { setLocalMapOpen(false); navigation.goBack(); }}
        onRetry={() => fullReconnect()}
      />

      <CityPickerModal
        visible={cityPickerOpen}
        onClose={() => setCityPickerOpen(false)}
        onPick={(c) => {
          recvLocRef.current = { lat: c.lat, lon: c.lon };
          setRecvLoc({ lat: c.lat, lon: c.lon });
          setCityPickerOpen(false);
        }}
      />

      {/* Admin pages — in-app browser with ← SDR bar */}
      <BrowserOverlay
        url={adminPage?.url ?? null}
        title={adminPage?.title}
        allowSave={!!adminPage?.url?.includes('/files')}
        injectCSS={adminPage?.url?.endsWith('/map')
          ? '.webrx-top-container{display:none!important}'   // OWRX map: hide header → full-screen map
          : undefined}
        onClose={() => setAdminPage(null)}
      />

      {/* Frequency modal */}
      <FreqModal
        visible={freqModalOpen}
        currentHz={status.frequency}
        onConfirm={onTuneHz}
        onClose={() => setFreqModalOpen(false)}
        unit={freqUnit}
        onUnit={setFreqUnit}
        minHz={client.current?.caps.freqRange[0]}
        maxHz={client.current?.caps.freqRange[1]}
        onShare={isLocal ? undefined : onShareStation}
        profiles={profiles}
        activeProfileId={activeProfileId}
        sdrUsage={sdrUsage}
        clientCount={clientCount}
        onSelectProfile={(id) => { client.current?.selectProfile?.(id); setActiveProfileId(id); }}
      />

      {/* Chat drawer */}
      <ChatDrawer
        visible={chatOpen}
        messages={chatMessages}
        myCallsign={myCallsign}
        onJoin={handleChatJoin}
        onSend={handleChatSend}
        onClose={closeChat}
        onMute={() => setChatMuted((p: boolean) => !p)}
        muted={chatMuted}
        users={chatUsers}
        syncedUser={syncedUser}
        zoomSync={zoomSync}
        onToggleSync={toggleUserSync}
        onToggleZoomSync={() => setZoomSync((p: boolean) => !p)}
        onUserTap={chatUserTap}
        textOnly={isOwrx}
        onChangeName={() => setMyCallsign(null)}
      />

      {/* Bypass password — rate-limit recovery (replaces the session) */}
      <PasswordModal
        visible={pwPrompt}
        serverUrl={baseUrl}
        onSubmit={(pw: string) => {
          setPwPrompt(false);
          navigation.replace('SDR', { ...route.params, password: pw });
        }}
        onCancel={() => { setPwPrompt(false); navigation.goBack(); }}
      />

      {/* Audio player (renderless) — held until the saved tune is restored
          so the audio WS opens on the CORRECT freq/mode (no race) */}
      <AudioPlayer
        // v3: the native UberSDR Opus engine only speaks UberSDR. OWRX/Kiwi audio
        // moves into their own native engines in a later phase — until then the
        // OWRX waterfall works but audio is off (don't point the Opus engine at it).
        // v4 local hardware (isLocal) uses LocalAudioPlayer below instead.
        baseUrl={tuneLoaded && !route.params.isLocal && (route.params.serverType ?? 'ubersdr') === 'ubersdr' ? baseUrl : null}
        password={password}
        frequency={status.frequency}
        mode={status.mode}
        step={step}
        instanceName={instanceName}
        uuid={sessionUuid}
      />
      {/* v4 local hardware: audio from the on-device shim's /ws/audio (PCM) */}
      {route.params.isLocal && route.params.localPort != null ? (
        <LocalAudioPlayer
          port={tuneLoaded ? route.params.localPort : null}
          frequency={status.frequency}
          mode={status.mode}
          bandwidthLow={status.bandwidthLow}
          bandwidthHigh={status.bandwidthHigh}
          instanceName={instanceName}
          host={route.params.localHost}
          authSuffix={route.params.authSuffix}
        />
      ) : null}

      {/* CarFM: the FM-only face over the live pipeline. Opaque, so the SDR UI
          and waterfall below are simply hidden while it's up (spec §5a). */}
      {fmFaceActive ? (
        <CarFmFace
          freqHz={status.frequency}
          stationName={liveStation.name}
          radioText={liveStation.text}
          stereo={fmStereo}
          signalDb={fmSignalDb}
          presets={fmPresets}
          region2={ituRegion === 2}
          onStep={onFmStep}
          onSeekPreset={onFmSeekPreset}
          onSelectPreset={onTuneHz}
          onSavePreset={onFmSavePreset}
          onEnterFreq={onFreqOpen}
          onOpenAdvanced={() => setAdvancedOpen(true)}
        />
      ) : null}

      {/* CarFM advanced view: a way back to the FM face (the full SDR UI shows) */}
      {carFm && advancedOpen ? (
        <Pressable
          onPress={() => setAdvancedOpen(false)}
          style={[styles.fmReturn, { top: insets.top + 8 }]}
          accessibilityRole="button"
          accessibilityLabel="Back to FM radio"
        >
          <Text style={styles.fmReturnText}>◂ FM</Text>
        </Pressable>
      ) : null}
    </View>
  );
}

const styles = StyleSheet.create({
  fmReturn: {
    position: 'absolute', left: 12, zIndex: 61,
    backgroundColor: 'rgba(5,7,10,0.92)', borderWidth: 1,
    borderColor: 'rgba(59,158,255,0.6)', borderRadius: 8,
    paddingHorizontal: 14, paddingVertical: 9,
  },
  fmReturnText: {
    color: '#3B9EFF', fontFamily: 'Atkinson Hyperlegible',
    fontSize: 15, fontWeight: '800', letterSpacing: 1,
  },
  rotateBanner: {
    position: 'absolute', alignSelf: 'center', zIndex: 55,
    backgroundColor: 'rgba(8,12,6,0.92)', borderWidth: 1,
    borderColor: 'rgba(120,240,120,0.45)', borderRadius: 8,
    paddingHorizontal: 14, paddingVertical: 7,
  },
  rotateBannerText: {
    color: 'rgba(140,255,140,0.9)', fontFamily: 'Atkinson Hyperlegible',
    fontSize: 12, fontWeight: '700', letterSpacing: 0.5,
  },
  mutedBanner: {
    position: 'absolute', alignSelf: 'center', zIndex: 60,
    backgroundColor: 'rgba(20,6,4,0.92)', borderWidth: 1,
    borderColor: 'rgba(220,60,60,0.8)', borderRadius: 8,
    paddingHorizontal: 14, paddingVertical: 8,
  },
  mutedBannerText: {
    color: '#ff7a7a', fontFamily: 'Atkinson Hyperlegible',
    fontSize: 13, fontWeight: '700', letterSpacing: 0.5,
  },
  serverLostWrap: {
    position: 'absolute', top: 0, left: 0, right: 0, bottom: 0, zIndex: 90,
    alignItems: 'center', justifyContent: 'center',
    backgroundColor: 'rgba(0,0,0,0.55)',
  },
  serverLostCard: {
    maxWidth: 360, marginHorizontal: 28, padding: 20, borderRadius: 14,
    backgroundColor: 'rgba(16,12,8,0.98)', borderWidth: 1, borderColor: 'rgba(255,184,77,0.55)',
    alignItems: 'center',
  },
  serverLostTitle: {
    color: '#ffb84d', fontFamily: 'Atkinson Hyperlegible', fontSize: 16,
    fontWeight: '700', textAlign: 'center', marginBottom: 8,
  },
  serverLostBody: {
    color: 'rgba(255,235,210,0.9)', fontFamily: 'Atkinson Hyperlegible',
    fontSize: 13, lineHeight: 19, textAlign: 'center', marginBottom: 16,
  },
  serverLostBtnRow: {
    flexDirection: 'row', alignItems: 'center', gap: 10,
  },
  serverLostBtn: {
    backgroundColor: '#ffb84d', borderRadius: 8, paddingHorizontal: 22, paddingVertical: 10,
  },
  serverLostBtnText: {
    color: '#1a1206', fontFamily: 'Atkinson Hyperlegible', fontSize: 14,
    fontWeight: '700', letterSpacing: 0.5,
  },
  serverLostBtnAlt: {
    backgroundColor: 'transparent', borderWidth: 1, borderColor: 'rgba(255,184,77,0.6)',
  },
  serverLostBtnAltText: { color: '#ffb84d' },
  restoreBtn: {
    position: 'absolute', alignSelf: 'center', zIndex: 60,
    backgroundColor: 'rgba(10,10,10,0.55)', borderWidth: 1,
    borderColor: 'rgba(255,255,255,0.30)', borderRadius: 16,
    paddingHorizontal: 18, paddingVertical: 4,
  },
  restoreBtnText: { color: 'rgba(255,255,255,0.85)', fontSize: 14 },
  root: {
    flex: 1,
    backgroundColor: '#000',
  },
  pillWrap: {
    position: 'absolute',
    left:  8,
    right: 8,
  },
});
