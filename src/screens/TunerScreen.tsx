import { useEffect, useRef, useState, useCallback, useMemo } from 'react';
import { AppState, View, Text, TouchableOpacity, ScrollView, Image, ActivityIndicator, StyleSheet, Modal, Pressable, NativeEventEmitter, NativeModules, Alert, Platform } from 'react-native';
import * as FileSystem from 'expo-file-system/legacy';
import RecordingsOverlay from '../components/RecordingsOverlay';
import AudioSheet from '../components/AudioSheet';
import { SafeAreaView, useSafeAreaInsets } from 'react-native-safe-area-context';
import AsyncStorage from '@react-native-async-storage/async-storage';
import StepPicker from '../components/StepPicker';
import { useIsFocused } from '@react-navigation/native';
import { shortcutsSuppressed } from '../components/PanelNav';
import { v4 as uuidv4 } from 'uuid';
import type { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { createBackend } from '../services/UberSDRAdapter';
import type { SDRBackend, FmdxState, FmdxServerInfo } from '../services/SDRBackend';
import { resolveStationLogo } from '../services/stationLogoCache';
import { getFavourites, toggleFavourite } from '../services/favourites';
import { isoToFlag, ituToIso, validIso } from '../services/rdsCountry';
import { useTheme, type ThemeTokens } from '../contexts/ThemeContext';
import ControlsBar, { createMeterBus } from '../components/ControlsBar';
import { VibePowerModule } from '../components/AudioPlayer';
import { watchProvider } from '../services/watchProvider';
import ChatDrawer, { type ChatMessage } from '../components/ChatDrawer';
import FreqModal from '../components/FreqModal';
import FmdxDial, { type DialStation } from '../components/FmdxDial';

// FM-DX Webserver tuner screen (v7). Single shared hardware tuner: server-side
// demod + RDS, native MP3 audio. No waterfall — station/RDS panels fill the top,
// and the app's real control island (single VFO drum, no bandwidth) sits at the
// bottom so it reads as native VibeSDR. Chat is first-class (shared tuning).

type Props = NativeStackScreenProps<RootStackParamList, 'Tuner'>;

/** What the WATCH gets. One builder, so the live frame and the hello reply can
 *  never drift apart. `dBf` is FM-DX's own unit — the watch prints whatever string
 *  we hand it and so can never disagree with the phone about the signal. */
function watchFmdxPayload(s: FmdxState, level: number, rx: string,
                         antennas: { id: number; name: string }[] = []) {
  const iso = ituToIso(s.tx?.itu) || s.countryIso;
  return {
    eq: !!(s as any).eq,
    ims: !!(s as any).ims,
    ant: (s as any).ant ?? 0,
    antennas,
    freq: s.freqHz,
    ps: s.ps ?? '',
    rt: s.rt ?? '',
    pi: s.pi ?? '',
    sig: s.sig,
    users: s.users ?? 0,
    stereo: !!s.stereo,
    tx: s.tx?.tx ?? '',
    city: s.tx?.city ?? '',
    dist: Math.round(s.tx?.dist ?? 0),      // km from the SERVER's QTH, not ours
    rx,                                     // ...which is HERE. A distance needs an origin.
    pty: PTY[s.pty] ?? '',
    flag: validIso(iso) ? isoToFlag(iso) : '',
    meter: `${Math.round(s.sig)} dBf`,
    level,
  };
}

const PTY = [
  'None', 'News', 'Current Affairs', 'Information', 'Sport', 'Education', 'Drama',
  'Culture', 'Science', 'Varied', 'Pop Music', 'Rock Music', 'Easy Listening',
  'Light Classical', 'Serious Classical', 'Other Music', 'Weather', 'Finance',
  'Children', 'Social Affairs', 'Religion', 'Phone In', 'Travel', 'Leisure',
  'Jazz Music', 'Country Music', 'National Music', 'Oldies Music', 'Folk Music',
  'Documentary', 'Alarm Test', 'Alarm',
];

const FM_LO = 87_500_000, FM_HI = 108_000_000;
const clampFm = (hz: number) => Math.min(FM_HI, Math.max(FM_LO, hz));
/** Best country for flag/logo: transmitter ITU (reliable) → RDS country_iso
 *  (only if a real code, not 'UN'/blank). Returns ISO alpha-2 or ''. */
function countryOf(st: FmdxState | null): string {
  return ituToIso(st?.tx?.itu) || (validIso(st?.countryIso) ? st!.countryIso!.trim().toUpperCase() : '');
}
// FM step ladder (Hz) — server accepts any kHz via T<kHz>, so we lock the STEP
// button to broadcast-FM-sensible values (1 kHz DX → 1 MHz coarse).
const FM_STEPS = [1_000, 10_000, 100_000, 1_000_000];
// VFO drum feel — ported from SDRScreen's velocity-adaptive tuning.
const DRUM_VFO_SENS = 22, VFO_FINE_MULT = 4, VFO_VEL_FINE = 40, VFO_VEL_FAST = 350;
const pad2 = (n: number) => String(n).padStart(2, '0');
const zulu = () => { const d = new Date(); return `${pad2(d.getUTCHours())}${pad2(d.getUTCMinutes())}z`; };

// Shared-tuner notice: remind ONCE PER APP RUN (listen session), not once per
// install and not on every server (re)connect. A module-level flag resets on a
// cold start, so each session the user is reminded a single time.
let fmdxNoticeShownThisSession = false;

export default function TunerScreen({ route, navigation }: Props) {
  const { baseUrl, instanceName } = route.params;
  const { theme } = useTheme();
  const insets = useSafeAreaInsets();
  const styles = useMemo(() => makeStyles(theme), [theme]);

  const backendRef = useRef<SDRBackend | null>(null);
  const pausedRef = useRef(false);   // power-saving pause — freezes the meter + SNR
  // The /text stream pushes state many times a second (signal meter etc.). A full
  // React re-render per frame pegs the JS thread → iOS kills the app for exceeding
  // its background-CPU limit. So the live meter is driven imperatively (meterBus,
  // no re-render) and the React state (panels/dial) is committed at ~5 Hz only.
  const latestStRef = useRef<FmdxState | null>(null);
  const stThrottleRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const destroyed = useRef(false);

  const [st, setSt] = useState<FmdxState | null>(null);
  const [connected, setConnected] = useState(false);
  const [paused, setPaused] = useState(false);   // power-saving pause (media controls) → disconnected
  const [error, setError] = useState<string | null>(null);
  const [logo, setLogo] = useState<string | null>(null);
  const lastLogoName = useRef<string>('');

  // Client-learned dial map: every RDS name we decode is pinned to its frequency
  // on the vintage dial, persisted per server. Accumulate in a ref, flush to state
  // + storage debounced.
  const [dialStations, setDialStations] = useState<DialStation[]>([]);
  const dialMapRef = useRef<Map<number, string>>(new Map());
  const dialFlushTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const DIAL_KEY = `lsv_fmdx_dial:${baseUrl}`;
  const learnStation = useCallback((freqHz: number, name: string) => {
    const n = name.trim();
    if (n.length < 2) return;                          // wait for a real PS lock
    if (dialMapRef.current.get(freqHz) === n) return;  // unchanged — no-op (no spam while parked)
    dialMapRef.current.set(freqHz, n);
    // Update the dial LIVE as you tune; persist to storage debounced.
    const arr = Array.from(dialMapRef.current, ([f, nm]) => ({ freqHz: f, name: nm })).slice(-300);
    setDialStations(arr);
    if (dialFlushTimer.current) clearTimeout(dialFlushTimer.current);
    dialFlushTimer.current = setTimeout(() => {
      AsyncStorage.setItem(DIAL_KEY, JSON.stringify(arr)).catch(() => {});
    }, 800);
  }, [DIAL_KEY]);

  // Tuning: displayFreq is what the pill/drum show; while dragging we update it
  // locally and only COMMIT (tune the shared radio) on settle so a drum spin
  // doesn't spam retunes for everyone. When not dragging, the server frame drives it.
  const [displayFreq, setDisplayFreq] = useState(95_000_000);
  const [step, setStep] = useState(100_000);
  /** Mirror of `step` for callbacks that outlive a render (the watch handlers are
   *  attached once). */
  const stepRef = useRef(step);
  /** The iPhone's real SYSTEM volume (0…1). The watch sends DELTAS against it — never
   *  an absolute — because the phone owns the value and the wrist only nudges it. */
  const sysVolRef = useRef(1);
  useEffect(() => { stepRef.current = step; }, [step]);
  /** Where the RECEIVER is (from /static_data). Every txInfo distance is measured
   *  from here, so the watch needs it to make "46 km" mean anything. */
  const rxNameRef = useRef('');
  const [freqModalOpen, setFreqModalOpen] = useState(false);
  const [dialView, setDialView] = useState({ lo: FM_LO, hi: FM_HI });
  const [bottomH, setBottomH] = useState(0);   // measured VTS+island height → ScrollView bottom padding
  const [forcedMono, setForcedMono] = useState(false);
  const [demodOpen, setDemodOpen] = useState(false);
  const [stepOpen, setStepOpen] = useState(false);

  // ★★ THE SAME CONTROLS AS THE REST OF THE APP. FM-DX hardcoded the drums, so a user who had
  // chosen the tuner keys got drums back on this one backend — Stuart: "controls being the
  // same as the rest". Read from the same two keys SDRScreen persists, so the choice follows
  // the user rather than the screen they happen to be on.
  const [vfoKeys, setVfoKeys]   = useState(false);
  const [zoomKeys, setZoomKeys] = useState(false);
  useEffect(() => {
    AsyncStorage.getItem('lsv_vfo_keys').then((v: string | null) => { if (v === '1') setVfoKeys(true); }).catch(() => {});
    AsyncStorage.getItem('lsv_zoom_keys').then((v: string | null) => { if (v === '1') setZoomKeys(true); }).catch(() => {});
  }, []);   // S — reuses StepPicker, so it is
                                                     // navigable by keyboard for free
  const [isRecording, setIsRecording] = useState(false);
  const [recSeconds, setRecSeconds] = useState(0);
  const recTimerRef = useRef<ReturnType<typeof setInterval> | null>(null);
  const [recordingsOpen, setRecordingsOpen] = useState(false);
  const [audioSheetOpen, setAudioSheetOpen] = useState(false);
  // iOS: defer the native share sheet to the AudioSheet's onDismiss (see SDRScreen).
  const pendingRecShare = useRef<string | null>(null);
  const [serverInfo, setServerInfo] = useState<FmdxServerInfo | null>(null);
  const serverInfoRef = useRef<FmdxServerInfo | null>(null);   // for the watch payload (static per server)
  const [showNotice, setShowNotice] = useState(false);   // first-connect shared-tuner notice

  // Meter bus — carries the signal fill AND the 3-bar server-connection link
  // quality (derived from /text frame arrival, since FM-DX has no FFT frames).
  const meterBus = useMemo(() => createMeterBus(), []);
  const lastFrameAt = useRef(Date.now());
  const lastSigNorm = useRef(0);
  const dragFreqRef = useRef<number | null>(null);
  const commitTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const vfoPendingHz = useRef(0);
  const vfoVel = useRef({ t: 0, v: 0 });   // EMA thumb speed, px/s
  // After we command a tune, the server keeps streaming the OLD freq for a beat
  // before it retunes. Hold our target and ignore mismatching frames until it
  // converges (or a grace timeout — a locked/spectator server never will), so
  // the display doesn't bounce back to the old frequency.
  const targetFreqRef = useRef<number | null>(null);
  const convergeTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const armTarget = useCallback((f: number) => {
    targetFreqRef.current = f;
    if (convergeTimer.current) clearTimeout(convergeTimer.current);
    convergeTimer.current = setTimeout(() => { targetFreqRef.current = null; }, 3000);
  }, []);

  // Chat
  const [chatOpen, setChatOpen] = useState(false);
  const [chatUnread, setChatUnread] = useState(false);
  const [chatMessages, setChatMessages] = useState<ChatMessage[]>([]);
  const [myCallsign, setMyCallsign] = useState<string | null>(null);
  const myCallsignRef = useRef<string | null>(null);
  const chatOpenRef = useRef(false);
  const msgId = useRef(0);
  const lastNpTitle = useRef('');   // dedupe lock-screen now-playing pushes
  useEffect(() => { myCallsignRef.current = myCallsign; }, [myCallsign]);
  useEffect(() => { chatOpenRef.current = chatOpen; }, [chatOpen]);

  const CALLSIGN_KEY = `lsv_chat_callsign:${baseUrl}`;
  const dismissNotice = useCallback(() => { setShowNotice(false); fmdxNoticeShownThisSession = true; }, []);

  // ── Connect / teardown ──────────────────────────────────────────────────────
  useEffect(() => {
    destroyed.current = false;
    AsyncStorage.getItem(CALLSIGN_KEY).then((cs) => { if (!destroyed.current && cs) setMyCallsign(cs); });
    // The shared-tuner notice must be SEEN, not merely fired.
    //
    // The watch can boot this screen HEADLESSLY (phone asleep in a pocket), and the
    // notice was shown on mount and immediately marked as shown-this-session — so it
    // was used up with nobody looking, and the user never got the one warning that
    // says "retuning this server moves the frequency for everyone else on it". A
    // notice nobody sees is worse than no notice, because we then believe they've had
    // it. Wait until the app is actually IN FRONT OF THEM.
    let noticeSub: { remove(): void } | null = null;
    if (!fmdxNoticeShownThisSession) {
      if (AppState.currentState === 'active') {
        fmdxNoticeShownThisSession = true;
        setShowNotice(true);
      } else {
        noticeSub = AppState.addEventListener('change', (st) => {
          if (st !== 'active' || fmdxNoticeShownThisSession || destroyed.current) return;
          fmdxNoticeShownThisSession = true;
          setShowNotice(true);
          noticeSub?.remove();
          noticeSub = null;
        });
      }
    }
    AsyncStorage.getItem(DIAL_KEY).then((raw) => {
      if (destroyed.current || !raw) return;
      try {
        const arr: DialStation[] = JSON.parse(raw);
        dialMapRef.current = new Map(arr.map((s) => [s.freqHz, s.name]));
        setDialStations(arr);
      } catch {}
    });

    const uuid = uuidv4();
    const backend = createBackend('fmdx', baseUrl, uuid, {
      onSpectrum: () => {},
      onStatus:   () => {},
      onError:    (m) => { if (!destroyed.current) setError(m); },
      onConnect:  () => { if (!destroyed.current) { setConnected(true); setError(null); } },
      onDisconnect: () => { if (!destroyed.current) setConnected(false); },
      onServerLost: () => { if (!destroyed.current) setError('Server stopped responding'); },
      onFmdxInfo: (info) => {
        if (destroyed.current) return;
        setServerInfo(info);
        serverInfoRef.current = info;
        rxNameRef.current = info.tunerName ?? '';
      },
      onFmdxState: (s) => {
        if (destroyed.current) return;
        // Commit to React state at ~5 Hz (trailing) — NOT per frame. The dial +
        // panels don't need 20–30 Hz; the live meter is imperative (below).
        latestStRef.current = s;
        if (!stThrottleRef.current) {
          stThrottleRef.current = setTimeout(() => {
            stThrottleRef.current = null;
            if (!destroyed.current && latestStRef.current) setSt(latestStRef.current);
          }, 200);
        }
        // Data flowing → link good; feed the signal fill too.
        lastFrameAt.current = Date.now();
        const sn = Math.min(1, Math.max(0, s.sig / 70));
        lastSigNorm.current = sn;
        meterBus.emit({ level: sn, peak: sn, snr: 0, dbfs: s.sig, active: true, link: 3 });
        // The wrist gets the same frame. Throttled inside the provider (4/sec) —
        // RDS RadioText changes constantly and WCSession queues rather than drops.
        // `dBf` is FM-DX's own unit; the watch prints whatever string we send, so
        // it can never disagree with us about the signal.
        watchProvider.sendFmdx(watchFmdxPayload(s, sn, rxNameRef.current, serverInfoRef.current?.antennas ?? []));
        if (s.rds && s.ps) learnStation(s.freqHz, s.ps);  // pin RDS name to the dial
        // Lock-screen card: "STATION · 89.2" (freq beside the RDS name), or just
        // the frequency until RDS locks. Deduped so we don't spam the card.
        const mhz = (s.freqHz / 1e6).toFixed(1);
        const psName = s.ps?.trim();
        const npTitle = psName ? `${psName} · ${mhz}` : `${mhz} MHz`;
        if (npTitle !== lastNpTitle.current) {
          lastNpTitle.current = npTitle;
          VibePowerModule?.setNowPlaying?.(npTitle, instanceName ?? 'FM-DX');
        }
        if (dragFreqRef.current != null) return;          // dragging — drum owns the display
        const target = targetFreqRef.current;
        if (target != null) {
          if (s.freqHz === target) {                       // server caught up
            targetFreqRef.current = null;
            if (convergeTimer.current) clearTimeout(convergeTimer.current);
            setDisplayFreq(s.freqHz);
          }
          // else: still the stale old freq — hold the target, don't bounce
        } else {
          setDisplayFreq(s.freqHz);                        // idle — server drives
        }
      },
      onChatMessage: (name, text) => {
        if (destroyed.current) return;
        const own = name === myCallsignRef.current;
        setChatMessages((prev) => [...prev.slice(-99), {
          id: `m${msgId.current++}`, type: own ? 'own' : 'other', user: name, text, ts: zulu(),
        }]);
        if (!chatOpenRef.current) setChatUnread(true);
      },
    });
    backendRef.current = backend;
    backend.connect().catch((e) => { if (!destroyed.current) setError(String(e?.message ?? e)); });
    // FM-DX lock-screen card: neutral FM artwork + server name (the native side
    // also disables skip + owns reconnect for the shared tuner).
    VibePowerModule?.setInstanceName?.(instanceName ?? 'FM-DX');
    (VibePowerModule as any)?.setArtwork?.('fmdx');

    return () => {
      destroyed.current = true;
      noticeSub?.remove();
      if (commitTimer.current) clearTimeout(commitTimer.current);
      if (convergeTimer.current) clearTimeout(convergeTimer.current);
      if (dialFlushTimer.current) clearTimeout(dialFlushTimer.current);
      if (stThrottleRef.current) clearTimeout(stThrottleRef.current);
      backendRef.current?.destroy();
      backendRef.current = null;
    };
  }, [baseUrl]);

  // ── Connection-link watchdog: degrade the 3-bar meter when /text frames go
  //    stale (green → yellow → red → down), independent of a single frame. ──
  useEffect(() => {
    const id = setInterval(() => {
      if (pausedRef.current) {   // paused for power saving — meter flat, SNR frozen
        meterBus.emit({ level: 0, peak: 0, snr: 0, dbfs: 0, active: false, link: 0 });
        return;
      }
      const gap = Date.now() - lastFrameAt.current;
      const link: 0 | 1 | 2 | 3 = gap < 2000 ? 3 : gap < 4000 ? 2 : gap < 8000 ? 1 : 0;
      const sn = link > 0 ? lastSigNorm.current : 0;
      meterBus.emit({ level: sn, peak: sn, snr: 0, dbfs: 0, active: link > 0, link });
    }, 1000);
    return () => clearInterval(id);
  }, [meterBus]);

  // ── Power-saving pause (lock-screen pause / AirPods out / Bluetooth off) ──────
  //    Native emits VibeMuted + stops the /audio stream; we drop the /text + /chat
  //    control sockets too so the whole FM-DX session disconnects (no background
  //    battery drain, SNR freezes) — a true disconnect like UberSDR, not a mute.
  //    ▶ (VibeMuted false) reopens everything.
  useEffect(() => {
    const emitter = new NativeEventEmitter(NativeModules.VibePowerModule);
    const sub = emitter.addListener('VibeMuted', (e: { muted: boolean }) => {
      pausedRef.current = !!e.muted;
      setPaused(!!e.muted);
      const b = backendRef.current as any;
      if (e.muted) {
        b?.pauseForPower?.();
        meterBus.emit({ level: 0, peak: 0, snr: 0, dbfs: 0, active: false, link: 0 });
      } else {
        b?.resumeFromPower?.();
      }
    });
    // The iPhone's SYSTEM volume — mirrored to the wrist so its meter shows the truth
    // rather than a knob of its own. See VibePowerModule's volume section.
    const subVol = emitter.addListener('VibeVolume', (e: { volume: number }) => {
      sysVolRef.current = e.volume;
      watchProvider.setVolume(e.volume);
    });
    (NativeModules.VibePowerModule as { getSystemVolume?: () => Promise<number> })
      ?.getSystemVolume?.()
      .then((v) => { sysVolRef.current = v; watchProvider.setVolume(v); })
      .catch(() => {});
    return () => { sub.remove(); subVol.remove(); };
  }, [meterBus]);

  // ── Station logo (radio-browser, EXACT-name match only so we never show the
  //    wrong station's logo). Use the transmitter's full station name — far
  //    better than the truncated RDS PS. Monogram when there's no confident hit. ──
  const logoName = st?.tx?.tx?.trim() || st?.ps?.trim() || '';
  const logoIso = countryOf(st);
  useEffect(() => {
    const key = `${logoName}|${logoIso}`;
    if (!logoName || key === lastLogoName.current) return;
    lastLogoName.current = key;
    setLogo(null);
    resolveStationLogo({ pi: st?.pi, name: logoName, iso: logoIso || undefined }).then((url) => {
      if (!destroyed.current && lastLogoName.current === key) setLogo(url);
    });
  }, [logoName, logoIso]);

  // Inlay the resolved station logo on the lock-screen artwork.
  useEffect(() => { (VibePowerModule as any)?.setStationLogo?.(logo ?? ''); }, [logo]);

  // …and send the SAME image to the watch, as bytes. The phone has already resolved
  // it to a local file, so the wrist shows exactly what the phone shows rather than
  // fetching a URL it has no network path to. Empty = no logo, and the watch then
  // falls back to the app icon (glass over nothing reads as a broken grey box).
  useEffect(() => {
    let cancelled = false;
    if (!logo) { watchProvider.sendLogo(''); return; }
    FileSystem.readAsStringAsync(logo, { encoding: FileSystem.EncodingType.Base64 })
      .then((b64) => { if (!cancelled) watchProvider.sendLogo(b64); })
      .catch(() => { if (!cancelled) watchProvider.sendLogo(''); });
    return () => { cancelled = true; };
  }, [logo]);

  // ── Favourite this instance ────────────────────────────────────────────────
  //    The SDR screen offers this from its menu sheet; FM-DX has no menu, so the
  //    heart lives in the header — otherwise a good receiver you found mid-session
  //    can only be favourited by going back and hunting for it in the picker.
  //
  //    serverType MUST be 'fmdx'. FM-DX isn't sniffable by detectServerType, and the
  //    picker trusts the stored type — an untyped favourite would be re-detected and
  //    mis-opened as an UberSDR waterfall (see InstancePickerScreen.connectFav).
  const [isFavourite, setIsFavourite] = useState(false);
  useEffect(() => {
    getFavourites()
      .then((favs) => setIsFavourite(favs.some((f) => f.url === baseUrl)))
      .catch(() => {});
  }, [baseUrl]);

  const onToggleFavourite = useCallback(() => {
    getFavourites()
      .then((favs) => toggleFavourite(
        { name: instanceName ?? baseUrl, url: baseUrl, serverType: 'fmdx' }, favs))
      .then((next) => setIsFavourite(next.some((f) => f.url === baseUrl)))
      .catch(() => {});
  }, [baseUrl, instanceName]);

  // The wrist draws the SAME dial, from the same memory.
  useEffect(() => { watchProvider.sendStations(dialStations); }, [dialStations]);

  // ── Apple Watch: FM-DX is its own screen on the wrist (no spectrum to show). ──
  //    The crown is DISARMED there by default — this server has ONE receiver and
  //    retuning it moves the frequency for EVERY listener, so tuning must be a
  //    deliberate act, not a wrist twitch. The watch owns that latch; by the time a
  //    tune command reaches us the user has already armed it.
  useEffect(() => {
    const token = watchProvider.claim('fmdx');
    // A mounted tuner screen is a live session — see the same note in SDRScreen. Without
    // this, a stale 'pick'/'setup' from a watch-driven boot never gets retracted when the
    // user connects on the phone, and the wrist reports "choose a server" over a radio
    // that is plainly playing.
    watchProvider.setPhoneStatus('ready');
    watchProvider.attach({
      onTuneDelta: (delta: number, armed: boolean) => {
        // A SHARED tuner: retuning moves the frequency for EVERY listener on this
        // server. The watch disarms its crown by default for exactly that reason —
        // but that gate must not live in the watch's UI alone, or any other watch
        // screen (or a stale one, after a navigation bug) can tune the receiver out
        // from under everyone. The phone REQUIRES the assertion.
        if (!armed) return;
        if (!delta) return;
        const cur = latestStRef.current?.freqHz ?? 0;
        const st0 = stepRef.current;
        if (!(cur > 0) || !(st0 > 0)) return;
        // Snap to the step grid first, like the phone's own drum: a detent should
        // land on a channel, not offset the current fraction.
        const base = delta > 0 ? Math.floor(cur / st0) : Math.ceil(cur / st0);
        const f = (base + delta) * st0;
        armTarget(f);
        backendRef.current?.tune(f);
      },
      onTuneHz: (hz: number) => {
        if (hz > 0) { armTarget(hz); backendRef.current?.tune(hz); }
      },
      onMode: () => {},          // FM-DX is WFM only — no demod choice to make
      onStep: (hz: number) => { if (hz > 0) setStep(hz); },
      onZoomDelta: () => {},     // no spectrum, nothing to zoom
      // ── Thin-remote FM-DX server controls (the watch relayed the tap) ──
      onFmdxEq:      (on) => (backendRef.current as any)?.setEq?.(on),
      onFmdxIms:     (on) => (backendRef.current as any)?.setIms?.(on),
      onFmdxAntenna: (id) => (backendRef.current as any)?.setAntenna?.(id),
      // Volume is the SYSTEM's, not the backend's — FM-DX audio comes out of the same
      // speaker as everything else, so the wrist controls it here exactly as it does on
      // the SDR screen. (Unlike tuning, this disturbs nobody: a shared tuner is shared,
      // but the loudness in YOUR ear is yours.)
      onVolumeDelta: (delta: number) => {
        if (!delta) return;
        const next = Math.max(0, Math.min(1, sysVolRef.current + delta / 16));
        sysVolRef.current = next;
        (NativeModules.VibePowerModule as { setSystemVolume?: (v: number) => void })
          ?.setSystemVolume?.(next);
      },
      onMute: (muted: boolean) => {
        (NativeModules.VibePowerModule as { setMuted?: (m: boolean) => void })
          ?.setMuted?.(muted);
        watchProvider.setMuted(muted);
      },
      onReachableChange: () => {},
      onHello: () => {
        const s0 = latestStRef.current;
        if (s0) {
          watchProvider.sendFmdx(
            watchFmdxPayload(s0, Math.min(1, Math.max(0, s0.sig / 70)),
                             rxNameRef.current, serverInfoRef.current?.antennas ?? []),
          );
        }
      },
    });
    return () => { watchProvider.release(token); watchProvider.detach(); };
  }, [armTarget]);

  // ── Drum tuning: velocity-adaptive accumulator, snapped to the step grid,
  //    committed once on settle (shared tuner — don't spam retunes). ───────────
  const commitTune = useCallback(() => {
    const f = dragFreqRef.current;
    dragFreqRef.current = null;
    if (f != null) { armTarget(f); backendRef.current?.tune(f); }
  }, [armTarget]);

  const onVfoDelta = useCallback((pxDelta: number) => {
    const s = step;
    const now = Date.now();
    const gap = now - vfoVel.current.t;
    vfoVel.current.t = now;
    if (gap > 300) vfoVel.current.v = 0;
    else {
      const inst = Math.abs(pxDelta) / (Math.max(8, gap) / 1000);
      vfoVel.current.v = vfoVel.current.v * 0.7 + inst * 0.3;
    }
    const k = Math.max(0, Math.min(1, (vfoVel.current.v - VFO_VEL_FINE) / (VFO_VEL_FAST - VFO_VEL_FINE)));
    const pxPerStep = DRUM_VFO_SENS * (VFO_FINE_MULT - (VFO_FINE_MULT - 1) * k);
    vfoPendingHz.current += (pxDelta * s) / pxPerStep;
    const steps = Math.round(vfoPendingHz.current / s);
    if (!steps) return;
    vfoPendingHz.current -= steps * s;
    const cur = dragFreqRef.current ?? displayFreq;
    const snapped = Math.round(cur / s) * s;              // lock to the step grid
    const newHz = clampFm(snapped + steps * s);
    if (newHz === cur) return;
    dragFreqRef.current = newHz;
    setDisplayFreq(newHz);
    if (commitTimer.current) clearTimeout(commitTimer.current);
    commitTimer.current = setTimeout(commitTune, 220);
  }, [displayFreq, step, commitTune]);

  const onConfirmFreq = useCallback((hz: number) => {
    const f = clampFm(hz);
    dragFreqRef.current = null;
    setDisplayFreq(f);
    armTarget(f);
    backendRef.current?.tune(f);
  }, [armTarget]);

  // Stable callback so <FmdxDial> (React.memo) isn't re-rendered every parent
  // render — the dial only needs to re-render when its own props change.
  const onDialTune = useCallback((hz: number) => {
    onConfirmFreq(Math.round(hz / 100_000) * 100_000);
  }, [onConfirmFreq]);

  // Zoom drum → zoom the dial (FM-DX has no bandwidth). Octave zoom anchored on
  // the tuned frequency, clamped to [2 MHz, full band].
  const onDialZoom = useCallback((px: number) => {
    setDialView((v) => {
      const sp0 = v.hi - v.lo;
      const full = FM_HI - FM_LO;
      const sp = Math.max(2_000_000, Math.min(full, sp0 * Math.pow(0.5, px / 90)));
      const anchor = clampFm(displayFreq);
      const rel = sp0 > 0 ? (anchor - v.lo) / sp0 : 0.5;
      let lo = anchor - rel * sp, hi = lo + sp;
      if (lo < FM_LO) { lo = FM_LO; hi = lo + sp; }
      if (hi > FM_HI) { hi = FM_HI; lo = hi - sp; }
      return { lo, hi };
    });
  }, [displayFreq]);

  // ── Chat handlers ───────────────────────────────────────────────────────────
  const onJoin = useCallback((cs: string) => {
    const clean = cs.trim().replace(/[^A-Za-z0-9\-_/]/g, '').slice(0, 20);
    if (!clean) return;
    setMyCallsign(clean);
    AsyncStorage.setItem(CALLSIGN_KEY, clean).catch(() => {});
  }, [CALLSIGN_KEY]);
  const onSend = useCallback((text: string) => {
    if (myCallsignRef.current) (backendRef.current as any)?.sendChat?.(text, myCallsignRef.current);
  }, []);
  const openChat = useCallback(() => { setChatOpen(true); setChatUnread(false); }, []);

  // ── Hardware keyboard ───────────────────────────────────────────────────────
  //
  // ★ FM-DX runs on THIS screen rather than SDRScreen, which is why it had no keyboard
  // support at all: every capability added to the main screen has to be added here again,
  // deliberately (see BRIEF-fmdx-backend-adapter.md). Stuart scoped what it actually needs —
  // Enter, C, Esc, D, S, R — rather than the whole SDRScreen scheme, because there is no
  // waterfall to pan and no zoom to drive here.
  //
  // ★ The drums/keys toggle is deliberately NOT here: if you are on a keyboard you are using
  // neither control, so a shortcut for switching between them would be answering a question
  // nobody has asked.
  const kbRefs = useRef({ openChat, toggleRecording: (() => {}) as () => void });
  kbRefs.current.openChat = openChat;
  const screenFocused = useIsFocused();
  const screenFocusedRef = useRef(screenFocused);
  useEffect(() => { screenFocusedRef.current = screenFocused; }, [screenFocused]);

  const panelsOpenRef = useRef(false);
  useEffect(() => {
    panelsOpenRef.current = freqModalOpen || demodOpen || stepOpen || audioSheetOpen
                         || chatOpen || recordingsOpen;
  }, [freqModalOpen, demodOpen, stepOpen, audioSheetOpen, chatOpen, recordingsOpen]);

  useEffect(() => {
    const emitter = new NativeEventEmitter(NativeModules.VibePowerModule);
    const sub = emitter.addListener('VibeKeyDown', (e: { key: string }) => {
      const k = e?.key;
      if (!k) return;
      // Not the screen on top, or a page we did not write is showing — same rules as SDRScreen.
      if (!screenFocusedRef.current || shortcutsSuppressed()) return;
      if (k === 'Escape') {
        // One rule: something open closes, nothing open goes back to the server list.
        if (panelsOpenRef.current) {
          setFreqModalOpen(false); setDemodOpen(false); setStepOpen(false);
          setAudioSheetOpen(false); setChatOpen(false); setRecordingsOpen(false);
        } else {
          navigation.goBack();
        }
        return;
      }
      // A panel that is open owns the rest — its own navigation is already listening.
      if (panelsOpenRef.current) return;
      switch (k) {
        case 'Enter': setFreqModalOpen(true); break;
        case 'C': kbRefs.current.openChat(); break;
        case 'D': setDemodOpen(true); break;
        case 'S': setStepOpen(true); break;
        case 'R': kbRefs.current.toggleRecording(); break;
        default: break;
      }
    });
    return () => sub.remove();
  }, [navigation]);

  // ── Recording (REC + Recordings live in the AUDIO sheet — control island) ────
  const toggleRecording = useCallback(() => {
    if (!isRecording) {
      (VibePowerModule as any)?.startRecording(Math.round(displayFreq || 0), 'wfm')
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
          // Present the native share sheet only once the AudioSheet Modal is gone
          // (else it presents over the modal and wedges touch handling) — iOS
          // defers to the sheet's onDismiss; Android has no such conflict.
          if (!path) { setAudioSheetOpen(false); return; }
          if (Platform.OS === 'android') {
            try {
              const cu = await FileSystem.getContentUriAsync(path.startsWith('file://') ? path : 'file://' + path);
              VibePowerModule?.shareRecording(cu);
            } catch {}
            setAudioSheetOpen(false);
          } else {
            pendingRecShare.current = path;
            setAudioSheetOpen(false);
          }
        })
        .catch(() => setAudioSheetOpen(false));
    }
  }, [isRecording, displayFreq]);
  kbRefs.current.toggleRecording = toggleRecording;   // R — see the keyboard block above
  useEffect(() => () => { if (recTimerRef.current) clearInterval(recTimerRef.current); }, []);

  // Playing a saved recording (expo-audio) fights the live native engine for the
  // audio session — mute FM-DX (disconnects) while the browser is active, resume
  // on close. Reuses the VibeMuted path (native closes/reopens its own WS).
  const onRecordingsActive = useCallback((active: boolean) => {
    (NativeModules.VibePowerModule as any)?.setMuted?.(active);
  }, []);

  const ps = st?.ps?.trim() || (paused ? 'Paused' : connected ? '' : 'Connecting…');
  const resumeFromPause = useCallback(() => { (VibePowerModule as any)?.setMuted?.(false); }, []);
  const monogram = (st?.ps?.trim() || '?').slice(0, 3).toUpperCase();
  const sigNorm = Math.min(1, Math.max(0, (st?.sig ?? 0) / 70));

  return (
    <SafeAreaView style={styles.root} edges={['top']}>
      {/* Header (Back lives in the control island's menu slot) */}
      <View style={[styles.header, { paddingLeft: 16 + insets.left, paddingRight: 16 + insets.right }]}>
        <Text style={styles.title} numberOfLines={1}>{instanceName ?? 'FM-DX'}</Text>
        {/* Favourite this receiver. Same ♥/♡ convention as the instance picker —
            the app has no icon library, it uses Unicode glyphs throughout. */}
        <TouchableOpacity
          style={styles.favBtn}
          onPress={onToggleFavourite}
          accessibilityLabel={isFavourite ? 'Remove from favourites' : 'Add to favourites'}
          hitSlop={{ top: 8, bottom: 8, left: 8, right: 8 }}
          activeOpacity={0.7}
        >
          <Text style={[styles.fav, isFavourite && styles.favOn]}>
            {isFavourite ? '♥' : '♡'}
          </Text>
        </TouchableOpacity>
        {/* REC + recordings library moved into the AUDIO sheet (control island). */}
        {!!st && !paused && <Text style={styles.users}>{st.users} 👤</Text>}
      </View>

      {/* Power-saving pause: FM-DX disconnects (frees the shared tuner) — mirror the
          other backends' "PAUSED — TAP TO RECONNECT" pill (▶ or a tap resumes). */}
      {paused && (
        <TouchableOpacity style={[styles.pausedBanner, { top: insets.top + 46 }]}
          onPress={resumeFromPause} activeOpacity={0.85}>
          <Text style={styles.pausedBannerText}>⏸ PAUSED — TAP TO RECONNECT</Text>
        </TouchableOpacity>
      )}

      <ScrollView style={{ flex: 1 }} contentContainerStyle={{ paddingTop: 14, paddingBottom: 14 + bottomH, paddingLeft: 14 + insets.left, paddingRight: 14 + insets.right, gap: 12 }}>
        {error && <Text style={styles.err}>{error}</Text>}

        {/* Vintage tuning dial — every RDS name we decode is pinned to its freq */}
        <FmdxDial
          freqHz={displayFreq}
          loHz={FM_LO}
          hiHz={FM_HI}
          stations={dialStations}
          onTune={onDialTune}
          theme={theme}
          view={dialView}
          onViewChange={setDialView}
        />

        {/* PI (signal reading lives under the mode label; station name + RDS
            RadioText moved to the VTS strip above the island) */}
        <View style={styles.panel}>
          <Text style={styles.metaLabel}>PI CODE</Text>
          <Text style={styles.metaVal}>{st?.pi || '––––'}</Text>
        </View>

        {/* Transmitter (relative to the RECEIVER's location) */}
        {st?.tx?.tx && (
          <View style={styles.panel}>
            <Text style={styles.metaLabel}>TRANSMITTER</Text>
            <Text style={styles.txName}>{st.tx.tx}{st.tx.city ? ` · ${st.tx.city}` : ''}</Text>
            <Text style={styles.txMeta}>
              {[st.tx.erp ? `${st.tx.erp} kW` : '', st.tx.pol ? st.tx.pol.toUpperCase() : '',
                Number.isFinite(st.tx.dist as number) ? `${st.tx.dist} km` : '',
                Number.isFinite(st.tx.azi as number) ? `${st.tx.azi}°` : ''].filter(Boolean).join(' · ')}
              {'   (from receiver)'}
            </Text>
          </View>
        )}

        {/* Alternative frequencies — tap to tune (same station elsewhere) */}
        {!!st?.af?.length && (
          <View style={styles.panel}>
            <Text style={styles.metaLabel}>AF · TAP TO TUNE</Text>
            <View style={styles.afRow}>
              {st.af.map((h, i) => (
                <TouchableOpacity key={`${h}-${i}`} style={styles.afChip} onPress={() => onConfirmFreq(h)} activeOpacity={0.7}>
                  <Text style={styles.afChipTxt}>{(h / 1e6).toFixed(1)}</Text>
                </TouchableOpacity>
              ))}
            </View>
          </View>
        )}

        {!connected && !error && (
          paused
            ? <View style={{ alignItems: 'center', padding: 20, gap: 6 }}>
                <Text style={{ fontSize: 30 }}>⏸</Text>
                <Text style={{ color: theme.btnText, fontFamily: theme.font, fontSize: 15, opacity: 0.8 }}>Paused — press ▶ to resume</Text>
              </View>
            : <View style={{ alignItems: 'center', padding: 20 }}><ActivityIndicator color={theme.btnActiveText} /></View>
        )}
      </ScrollView>

      {/* VTS + control island float absolutely over the scroll content — the SDR
          controls were built to overlay a fixed-fill area, not sit in flex flow
          (which was clipping the island to 59px). onLayout feeds the ScrollView's
          bottom padding so nothing hides behind them. */}
      <View
        onLayout={(e) => setBottomH(e.nativeEvent.layout.height)}
        style={{ position: 'absolute', left: 0, right: 0, bottom: 0 }}
      >
      <View style={[styles.vts, { marginLeft: 14 + insets.left, marginRight: 14 + insets.right }]}>
        <View style={styles.vtsLogo}>
          {logo
            ? <Image source={{ uri: logo }} style={styles.vtsLogoImg} resizeMode="contain" />
            : <Text style={styles.vtsMono}>{monogram}</Text>}
        </View>
        <View style={{ flex: 1 }}>
          <View style={styles.vtsTopRow}>
            {!!isoToFlag(countryOf(st)) && <Text style={styles.vtsFlag}>{isoToFlag(countryOf(st))}</Text>}
            <Text style={styles.vtsName} numberOfLines={1}>{ps || '—'}</Text>
            {st?.stereo && <Pill label="ST" on styles={styles} />}
            {st?.tp && <Pill label="TP" on styles={styles} />}
            {st?.ta && <Pill label="TA" on styles={styles} />}
          </View>
          {!!st?.rt?.trim() && (
            <Text style={styles.vtsRt} numberOfLines={1}>{st.rt.replace(/\s{2,}/g, ' ').trim()}</Text>
          )}
        </View>
      </View>

      {/* The app's real control island — wrapped exactly like SDRScreen's
          pillWrap (inset 8px each side, bottom = safe-area + 8; bar's own
          bottomInset is 0 so the rounded corners aren't clipped). */}
      <View style={{ marginHorizontal: 8, marginBottom: insets.bottom + 8 }}>
      <ControlsBar
        frequency={displayFreq}
        mode="wfm"
        step={step}
        connected={connected}
        meterBus={meterBus}
        signalActive={connected}
        fmStereo={!!st?.stereo && !forcedMono}
        freqUnit="mhz"
        bottomInset={0}
        onVfoDelta={onVfoDelta}
        onBwDelta={onDialZoom}
        onMode={() => {}}
        onStep={setStep}
        onMenu={() => navigation.goBack()}
        onChat={openChat}
        onAudio={() => setAudioSheetOpen(true)}
        audioAsRecord
        isRecording={isRecording}
        recSeconds={recSeconds}
        onFreqTap={() => setFreqModalOpen(true)}
        onModeTap={() => setDemodOpen(true)}
        chatUnread={chatUnread}
        instanceHost={instanceName ?? 'FM-DX'}
        vfoNoInertia
        menuAsBack
        stepList={FM_STEPS}
        vfoKeys={vfoKeys}
        zoomKeys={zoomKeys}
        meterLabel={st ? `${Math.round(st.sig)} dBf` : ''}
        freqFormat={(hz) => (hz / 1e6).toFixed(3)}
      />
      </View>
      </View>

      {/* First-connect shared-tuner notice */}
      <Modal visible={showNotice} transparent animationType="fade" onRequestClose={dismissNotice}>
        <View style={styles.noticeBackdrop}>
          <View style={styles.noticeCard}>
            <Text style={styles.noticeTitle}>SHARED TUNER</Text>
            <Text style={styles.noticeBody}>
              This is one radio shared by everyone connected — <Text style={styles.noticeBold}>tuning changes the frequency for all listeners</Text>.
            </Text>
            <Text style={styles.noticeItem}>💬  Please ask in chat before you retune.</Text>
            <Text style={styles.noticeItem}>🔒  Lock-screen / headphone skip is disabled here, so you can't retune everyone by accident.</Text>
            <Text style={styles.noticeItem}>👤  The counter at the top shows how many people are listening.</Text>
            <TouchableOpacity style={styles.noticeBtn} onPress={dismissNotice}>
              <Text style={styles.noticeBtnTxt}>GOT IT</Text>
            </TouchableOpacity>
          </View>
        </View>
      </Modal>

      {/* Demodulator options sheet (mode-pill tap) — mono/stereo, cEQ, iMS, antenna */}
      <Modal visible={demodOpen} transparent animationType="fade" onRequestClose={() => setDemodOpen(false)}>
        <Pressable style={styles.sheetBackdrop} onPress={() => setDemodOpen(false)}>
          <Pressable style={styles.sheet} onPress={() => {}}>
            <Text style={styles.sheetTitle}>DEMODULATOR</Text>
            <OptToggle label="Stereo" on={!forcedMono} styles={styles}
              onPress={() => { const m = !forcedMono; setForcedMono(m); (backendRef.current as any)?.forceMono?.(m); }} />
            <OptToggle label="cEQ" on={!!st?.eq} styles={styles}
              onPress={() => (backendRef.current as any)?.setEq?.(!st?.eq)} />
            <OptToggle label="iMS" on={!!st?.ims} styles={styles}
              onPress={() => (backendRef.current as any)?.setIms?.(!st?.ims)} />
            {(serverInfo?.antennas.length ?? 0) > 1 && (
              <View style={{ marginTop: 6 }}>
                <Text style={styles.sheetLabel}>ANTENNA</Text>
                <View style={styles.antRow}>
                  {serverInfo!.antennas.map((a, i) => (
                    <TouchableOpacity key={`ant${i}`}
                      style={[styles.antBtn, st?.ant === a.id && styles.antBtnOn]}
                      onPress={() => (backendRef.current as any)?.setAntenna?.(a.id)}>
                      <Text style={[styles.antBtnTxt, st?.ant === a.id && styles.antBtnTxtOn]} numberOfLines={1}>{a.name}</Text>
                    </TouchableOpacity>
                  ))}
                </View>
              </View>
            )}
            <TouchableOpacity style={styles.sheetClose} onPress={() => setDemodOpen(false)}>
              <Text style={styles.sheetCloseTxt}>CLOSE</Text>
            </TouchableOpacity>
          </Pressable>
        </Pressable>
      </Modal>

      {/* ★ The same StepPicker the rest of the app uses, so S gets arrow navigation, Enter
          and Backspace for free rather than a bespoke path that would drift from the others. */}
      <StepPicker
        visible={stepOpen}
        currentStep={step}
        steps={FM_STEPS}
        onSelect={setStep}
        onClose={() => setStepOpen(false)}
      />
      <RecordingsOverlay visible={recordingsOpen} onClose={() => setRecordingsOpen(false)} onActiveChange={onRecordingsActive} />

      {/* Audio sheet — FM-DX has only REC + Recordings (no client DSP / squelch) */}
      <AudioSheet
        visible={audioSheetOpen}
        onClose={() => setAudioSheetOpen(false)}
        onDismiss={() => {
          const p = pendingRecShare.current;
          if (p) { pendingRecShare.current = null; VibePowerModule?.shareRecording(p); }
        }}
        recordingOnly
        recording={isRecording}
        recSeconds={recSeconds}
        onRec={toggleRecording}
        onRecordings={() => { setAudioSheetOpen(false); setRecordingsOpen(true); }}
      />

      <FreqModal
        visible={freqModalOpen}
        currentHz={displayFreq}
        onConfirm={onConfirmFreq}
        onClose={() => setFreqModalOpen(false)}
        unit="mhz"
        lockUnit
        minHz={FM_LO}
        maxHz={FM_HI}
      />

      <ChatDrawer
        visible={chatOpen}
        messages={chatMessages}
        myCallsign={myCallsign}
        onJoin={onJoin}
        onSend={onSend}
        onClose={() => setChatOpen(false)}
        onChangeName={() => setMyCallsign(null)}
        textOnly
      />
    </SafeAreaView>
  );
}

function OptToggle({ label, on, onPress, styles }: { label: string; on: boolean; onPress: () => void; styles: any }) {
  return (
    <TouchableOpacity style={styles.optRow} onPress={onPress} activeOpacity={0.7}>
      <Text style={styles.optLabel}>{label}</Text>
      <View style={[styles.optSwitch, on && styles.optSwitchOn]}>
        <Text style={[styles.optSwitchTxt, on && styles.optSwitchTxtOn]}>{on ? 'ON' : 'OFF'}</Text>
      </View>
    </TouchableOpacity>
  );
}

function Pill({ label, on, styles }: { label: string; on?: boolean; styles: any }) {
  return (
    <View style={[styles.pill, on && styles.pillOn]}>
      <Text style={[styles.pillTxt, on && styles.pillTxtOn]}>{label}</Text>
    </View>
  );
}

function makeStyles(t: ThemeTokens) {
  const F = t.font;
  return StyleSheet.create({
    root: { flex: 1, backgroundColor: '#080601' },
    header: { flexDirection: 'row', alignItems: 'center', paddingHorizontal: 16, paddingVertical: 10, gap: 12, borderBottomWidth: 1, borderBottomColor: t.barBorder },
    back: { paddingVertical: 2, paddingRight: 4 },
    backTxt: { color: t.btnActiveText, fontFamily: F, fontSize: 15 },
    title: { flex: 1, color: t.freqColor, fontFamily: F, fontSize: 18, fontWeight: 'bold', letterSpacing: 1 },
    users: { color: t.snrColor, fontFamily: F, fontSize: 13 },
    favBtn: { padding: 4 },
    fav:   { color: t.sectionColor, fontSize: 20 },
    favOn: { color: '#e5484d' },
    err: { color: '#ff8a8a', fontFamily: F, fontSize: 13, textAlign: 'center' },
    pausedBanner: {
      position: 'absolute', alignSelf: 'center', zIndex: 60,
      backgroundColor: 'rgba(20,6,4,0.92)', borderWidth: 1,
      borderColor: 'rgba(220,60,60,0.8)', borderRadius: 8,
      paddingHorizontal: 14, paddingVertical: 8,
    },
    pausedBannerText: { color: '#ff7a7a', fontFamily: F, fontSize: 13, fontWeight: '700', letterSpacing: 0.5 },
    panel: { backgroundColor: t.barBg, borderRadius: 14, borderWidth: 1, borderColor: t.barBorder, padding: 14 },
    stationRow: { flexDirection: 'row', alignItems: 'center', gap: 14 },
    logoBox: { width: 72, height: 72, borderRadius: 10, backgroundColor: t.pillBg, borderWidth: 1, borderColor: t.barBorder, alignItems: 'center', justifyContent: 'center', overflow: 'hidden' },
    logo: { width: 68, height: 68 },
    monogram: { color: t.btnActiveText, fontFamily: F, fontSize: 22, fontWeight: 'bold' },
    station: { color: t.freqColor, fontFamily: F, fontSize: 22, fontWeight: 'bold' },
    pills: { flexDirection: 'row', flexWrap: 'wrap', gap: 6, marginTop: 8 },
    pill: { borderColor: t.btnBorder, borderWidth: 1, borderRadius: 5, paddingHorizontal: 8, paddingVertical: 4, backgroundColor: t.btnBg },
    pillOn: { backgroundColor: t.btnActiveBg, borderColor: t.btnActiveBdr },
    pillTxt: { color: t.unitColor, fontFamily: F, fontSize: 10, fontWeight: 'bold', letterSpacing: 1 },
    pillTxtOn: { color: t.btnActiveText },
    metaRow: { flexDirection: 'row', gap: 12 },
    metaCell: { flex: 1 },
    metaLabel: { color: t.sectionColor, fontFamily: F, fontSize: 11, fontWeight: 'bold', letterSpacing: 2, marginBottom: 4 },
    metaVal: { color: t.freqColor, fontFamily: F, fontSize: 26, fontWeight: 'bold' },
    metaUnit: { fontSize: 14, color: t.unitColor, fontWeight: 'normal' },
    sigBarBg: { height: 6, backgroundColor: t.pillBg, borderRadius: 3, marginTop: 8, overflow: 'hidden' },
    sigBarFill: { height: 6, backgroundColor: t.btnActiveText },
    rt: { color: t.freqColor, fontFamily: F, fontSize: 15, marginTop: 4 },
    txName: { color: t.freqColor, fontFamily: F, fontSize: 15, fontWeight: 'bold', marginTop: 2 },
    txMeta: { color: t.unitColor, fontFamily: F, fontSize: 12, marginTop: 3 },
    af: { color: t.freqColor, fontFamily: F, fontSize: 15, marginTop: 4, letterSpacing: 1 },
    vts: { flexDirection: 'row', alignItems: 'center', gap: 10, marginHorizontal: 14, marginBottom: 6, paddingHorizontal: 12, paddingVertical: 8, backgroundColor: t.barBg, borderRadius: 12, borderWidth: 1, borderColor: t.barBorder },
    vtsLogo: { width: 40, height: 40, borderRadius: 8, backgroundColor: t.pillBg, borderWidth: 1, borderColor: t.barBorder, alignItems: 'center', justifyContent: 'center', overflow: 'hidden' },
    vtsLogoImg: { width: 38, height: 38 },
    vtsMono: { color: t.btnActiveText, fontFamily: F, fontSize: 14, fontWeight: 'bold' },
    vtsTopRow: { flexDirection: 'row', alignItems: 'center', gap: 6 },
    noticeBackdrop: { flex: 1, backgroundColor: 'rgba(0,0,0,0.7)', justifyContent: 'center', padding: 26 },
    noticeCard: { backgroundColor: '#14141f', borderRadius: 16, borderWidth: 1, borderColor: t.btnActiveBdr, padding: 22 },
    noticeTitle: { color: t.btnActiveText, fontFamily: F, fontSize: 14, fontWeight: 'bold', letterSpacing: 2, marginBottom: 12, textAlign: 'center' },
    noticeBody: { color: t.freqColor, fontFamily: F, fontSize: 15, lineHeight: 21, marginBottom: 14 },
    noticeBold: { fontWeight: 'bold', color: t.btnActiveText },
    noticeItem: { color: t.unitColor, fontFamily: F, fontSize: 14, lineHeight: 20, marginBottom: 10 },
    noticeBtn: { marginTop: 8, alignItems: 'center', paddingVertical: 13, borderRadius: 8, backgroundColor: t.btnActiveBg, borderWidth: 1, borderColor: t.btnActiveBdr },
    noticeBtnTxt: { color: t.btnActiveText, fontFamily: F, fontSize: 15, fontWeight: 'bold', letterSpacing: 1 },
    sheetBackdrop: { flex: 1, backgroundColor: 'rgba(0,0,0,0.55)', justifyContent: 'flex-end' },
    sheet: { backgroundColor: '#101018', borderTopLeftRadius: 18, borderTopRightRadius: 18, borderWidth: 1, borderColor: t.barBorder, padding: 18, paddingBottom: 30, gap: 8 },
    sheetTitle: { color: t.sectionColor, fontFamily: F, fontSize: 12, fontWeight: 'bold', letterSpacing: 2, marginBottom: 6 },
    sheetLabel: { color: t.sectionColor, fontFamily: F, fontSize: 11, fontWeight: 'bold', letterSpacing: 1, marginBottom: 6 },
    optRow: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', paddingVertical: 10, borderBottomWidth: 1, borderBottomColor: 'rgba(255,255,255,0.08)' },
    optLabel: { color: t.freqColor, fontFamily: F, fontSize: 16 },
    optSwitch: { minWidth: 52, alignItems: 'center', paddingVertical: 6, paddingHorizontal: 12, borderRadius: 6, borderWidth: 1, borderColor: t.btnBorder, backgroundColor: t.btnBg },
    optSwitchOn: { backgroundColor: t.btnActiveBg, borderColor: t.btnActiveBdr },
    optSwitchTxt: { color: t.unitColor, fontFamily: F, fontSize: 12, fontWeight: 'bold' },
    optSwitchTxtOn: { color: t.btnActiveText },
    antRow: { flexDirection: 'row', flexWrap: 'wrap', gap: 8 },
    antBtn: { paddingVertical: 8, paddingHorizontal: 12, borderRadius: 6, borderWidth: 1, borderColor: t.btnBorder, backgroundColor: t.btnBg },
    antBtnOn: { backgroundColor: t.btnActiveBg, borderColor: t.btnActiveBdr },
    antBtnTxt: { color: t.unitColor, fontFamily: F, fontSize: 13 },
    antBtnTxtOn: { color: t.btnActiveText, fontWeight: 'bold' },
    sheetClose: { marginTop: 14, alignItems: 'center', paddingVertical: 12, borderRadius: 8, borderWidth: 1, borderColor: t.btnBorder },
    sheetCloseTxt: { color: t.freqColor, fontFamily: F, fontSize: 14, fontWeight: 'bold', letterSpacing: 1 },
    vtsFlag: { fontSize: 18 },
    vtsName: { color: t.freqColor, fontFamily: F, fontSize: 17, fontWeight: 'bold', flexShrink: 1 },
    vtsRt: { color: t.unitColor, fontFamily: F, fontSize: 12, marginTop: 1 },
    afRow: { flexDirection: 'row', flexWrap: 'wrap', gap: 8, marginTop: 6 },
    afChip: { backgroundColor: t.btnBg, borderWidth: 1, borderColor: t.btnBorder, borderRadius: 6, paddingHorizontal: 12, paddingVertical: 7 },
    afChipTxt: { color: t.btnActiveText, fontFamily: F, fontSize: 15, fontWeight: 'bold' },
  });
}
