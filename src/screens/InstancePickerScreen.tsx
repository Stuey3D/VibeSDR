import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useFocusEffect } from '@react-navigation/native';
import {
  ActivityIndicator,
  Alert,
  FlatList,
  Image,
  KeyboardAvoidingView,
  Modal,
  Platform,
  StyleSheet,
  Text,
  TextInput,
  TouchableOpacity,
  View,
  NativeModules,
} from 'react-native';
// safe-area-context SafeAreaView — RN's own is iOS-only, which put the
// header under the status bar on Android (G35: cog untappable)
import { SafeAreaView } from 'react-native-safe-area-context';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { splashBridge } from '../../App';
import type { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { newLocalSession } from '../services/localSession';
import UsbSdrIcon from '../components/UsbSdrIcon';
import { themeFor } from '../constants/theme';
import {
  SDRInstance,
  fetchInstances,
  getUserLocation,
  isVersionOld,
  MIN_RECOMMENDED_VERSION,
} from '../services/instancesApi';
import { checkConnection, detectServerType, probeServer, DEFAULT_PORT,
         type BackendType, type ServerType } from '../services/sdrTypes';
import { vibeServerNeedsPin } from '../services/vibeAuth';

/**
 * Pull a host and port out of anything a person might reasonably type:
 * a bare IP, "stuey3d.freemyip.com:8073", "http://host/path", "ws://host:1234".
 *
 * URL() is deliberately not used — it rejects a bare "host:8073" (it reads "host"
 * as the scheme), which is the single most likely thing to be typed into the box.
 * When no port is given we fall back to the backend's default, or 80/443.
 */
/** How each backend is named in the UI. 'auto' = let the probe decide. */
const PROTO_LABEL: Record<BackendType, string> = {
  vibeserver: 'VibeServer', ubersdr: 'UberSDR', owrx: 'OpenWebRX',
  kiwi: 'KiwiSDR', fmdx: 'FM-DX', rtltcp: 'rtl_tcp', spyserver: 'SpyServer',
};
/** Offered in the add-server modal. AUTO first — it's right almost always; the
 *  explicit choices exist for raw-TCP servers on non-standard ports, which cannot
 *  be auto-detected (no HTTP to sniff). */
const PROTO_CHOICES: Array<[BackendType | 'auto', string]> = [
  ['auto', 'Auto'], ['vibeserver', 'VibeServer'], ['rtltcp', 'rtl_tcp'],
  ['spyserver', 'SpyServer'], ['owrx', 'OpenWebRX'], ['kiwi', 'KiwiSDR'],
  ['ubersdr', 'UberSDR'], ['fmdx', 'FM-DX'],
];

function parseHostPort(raw: string, hint?: BackendType): { host: string; port: number } | null {
  let s = raw.trim()
    .replace(/^ws:\/\//i, 'http://').replace(/^wss:\/\//i, 'https://');
  const https = /^https:\/\//i.test(s);
  s = s.replace(/^https?:\/\//i, '').replace(/\/.*$/, '');   // drop scheme + path
  if (!s) return null;
  const m = /^(.+?)(?::(\d+))?$/.exec(s);
  if (!m) return null;
  const host = m[1];
  const port = m[2] ? parseInt(m[2], 10)
    : hint ? DEFAULT_PORT[hint]
    : https ? 443 : 80;
  return Number.isFinite(port) && port > 0 && port < 65536 ? { host, port } : null;
}
import {
  DefaultInstance,
  clearDefaultInstance,
  getDefaultInstance,
  setDefaultInstance,
} from '../services/defaultInstance';
import { isDeepLinkActive, whenInitialLinkChecked } from '../linking/deepLinkState';
import { parseSdrUrl } from '../linking/SdrLinkHandler';
import { watchTargetPending } from '../services/watchBoot';
import { Favourite, getFavourites, toggleFavourite, setFavouriteServerType,
         repairVibeserverFavourites,
         TcpFav, getTcpFavs, saveTcpFavs } from '../services/favourites';
import { loadUserBookmarks, saveUserBookmarks, type UserBookmark } from '../services/userBookmarks';
import { ViewMode, getViewMode, setViewMode } from '../services/viewMode';
import PasswordModal from '../components/PasswordModal';
import { VibePowerModule } from '../components/AudioPlayer';
import { useCoachmarkTour, tourRef } from '../components/Coachmark';
import { APP_VERSION } from '../constants/version';
import { DIRECTORIES, fetchDirectory, type DirectoryId } from '../services/directories';
import { startMdnsDiscovery, type DiscoveredServer } from '../services/mdns';
import { resolveVibeAuth } from '../services/vibeAuth';
import { rtlTcpServerSupported } from '../services/rtlTcpServer';

// Per-backend logo for the directory cards + per-instance type icon (receiverbook
// mixes OWRX + Kiwi, so the row icon tells them apart at a glance).
const TYPE_LOGOS: Record<string, any> = {
  ubersdr: require('../../assets/logo_ubersdr.png'),
  owrx:    require('../../assets/logo_owrx.png'),
  kiwi:    require('../../assets/logo_kiwi.png'),
  fmdx:    require('../../assets/logo_fmdx.png'),
};

/** ISO 3166-1 alpha-2 → 🇬🇧-style emoji flag (regional indicators, no assets). */
function flagEmoji(code: string | null): string {
  if (!code || code.length !== 2) return '';
  const cc = code.toUpperCase();
  if (!/^[A-Z]{2}$/.test(cc)) return '';
  return String.fromCodePoint(127397 + cc.charCodeAt(0), 127397 + cc.charCodeAt(1));
}

type SortMode = 'nearest' | 'snr';
type Props = NativeStackScreenProps<RootStackParamList, 'InstancePicker'>;

// A unified list item — either a real SDRInstance or a favourited custom URL
type ListItem =
  | { kind: 'instance'; data: SDRInstance }
  | { kind: 'custom';   fav: Favourite };

export default function InstancePickerScreen({ navigation, route }: Props) {
  const [instances,   setInstances]     = useState<SDRInstance[]>([]);
  const [loading,     setLoading]       = useState(true);
  const [error,       setError]         = useState<string | null>(null);
  const [customUrl,   setCustomUrl]     = useState('');
  const [connecting,  setConnecting]    = useState(false);
  const [filter,      setFilter]        = useState('');
  const [defaultInst, setDefaultInst]   = useState<DefaultInstance | null>(null);
  // Tell native the default-instance name (or '' = none) so the Siri "open and
  // tune" intent can auto-connect, or prompt the user to set a default.
  useEffect(() => { VibePowerModule?.setDefaultInstance?.(defaultInst?.name ?? ''); }, [defaultInst]);
  const [viewMode,    setViewModeState] = useState<ViewMode>('default');
  const [modeReady,   setModeReady]     = useState(false);
  const [sortMode,    setSortMode]      = useState<SortMode>('nearest');
  const [pwModal,     setPwModal]       = useState<{ url: string; name: string } | null>(null);
  const [favourites,  setFavourites]    = useState<Favourite[]>([]);
  // RTL-TCP named favourites (host:port + friendly name), persisted locally.
  const [tcpFavs,     setTcpFavs]       = useState<TcpFav[]>([]);
  const [tcpModal,    setTcpModal]      = useState(false);
  // Auto-discovered RTL-TCP servers (mDNS/Bonjour) — live while the picker is focused.
  const [discovered,  setDiscovered]    = useState<DiscoveredServer[]>([]);
  const [tcpName,     setTcpName]       = useState('');
  const [tcpHost,     setTcpHost]       = useState('');
  const [tcpPort,     setTcpPort]       = useState('1234');
  // Which protocol the manual-add modal speaks. rtl_tcp = raw full-rate IQ;
  // spyserver = server-side decimation (far less bandwidth, works over cellular).
  const [tcpProto,    setTcpProto]      = useState<BackendType | 'auto'>('auto');
  // null = directory CHOOSER (favourites + directory cards); set = that
  // directory's instance list.
  const [selectedDir, setSelectedDir]   = useState<DirectoryId | null>(null);

  // First-run tour on the instance list — welcome + the custom-server box.
  const pickerTour = useCoachmarkTour([
    { id: 'welcome', title: 'Welcome to VibeSDR',
      body: 'Browse public SDR servers below, or set a favourite as your default to skip straight in next time.' },
    { id: 'custom', title: 'Your own server',
      body: 'Got a private UberSDR, OpenWebRX or KiwiSDR? Enter its address here to connect to it directly.',
      target: tourRef('customUrl') },
  ], { storageKey: 'lsv_tour_picker_v1' });
  useEffect(() => {
    // Wait for the launch splash to fully dismiss before auto-starting the tour —
    // on first launch the splash holds open on the CONTINUE / power-saving notice,
    // and the tutorial must not draw on top of it. whenDismissed fires immediately
    // on later launches (splash already gone), preserving the original ~1.1s settle.
    let t: ReturnType<typeof setTimeout>;
    const unsub = splashBridge.whenDismissed(() => {
      t = setTimeout(() => { pickerTour.maybeAutoStart(); }, 1100);
    });
    return () => { unsub(); clearTimeout(t); };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  const userLocRef = useRef<{ lat: number; lon: number } | null>(null);

  const openDirectory = useCallback((id: DirectoryId) => {
    setSelectedDir(id);
    setInstances([]); setFilter(''); setError(null); setLoading(true);
    fetchDirectory(id, userLocRef.current?.lat, userLocRef.current?.lon)
      .then((list) => setInstances(list))
      .catch((e: any) => setError(e?.message || 'Failed to load this directory'))
      .finally(() => setLoading(false));
  }, []);

  // Assigned once connectLocal/tryUsbLaunch are defined below; the mount + focus
  // effects (declared above those callbacks) call it through this ref to avoid a
  // use-before-declaration cycle.
  const tryUsbLaunchRef = useRef<null | ((m?: typeof viewMode) => Promise<boolean>)>(null);

  useEffect(() => {
    let cancelled = false;

    async function loadAndInit() {
      let mode = await getViewMode();
      if (cancelled) return;
      const { width } = require('react-native').Dimensions.get('window');
      const isSmallScreen = width <= 390;
      if (!mode) {
        if (isSmallScreen) { await setViewMode('accessibility'); mode = 'accessibility'; }
        else { navigation.replace('InstancePicker'); return; }
      } else if (isSmallScreen && mode !== 'accessibility') {
        await setViewMode('accessibility'); mode = 'accessibility';
      }
      setViewModeState(mode);
      setModeReady(true);

      const favs = await getFavourites();
      if (!cancelled) setFavourites(favs);
      const tfavs = await getTcpFavs();
      if (!cancelled) setTcpFavs(tfavs);

      const dEarly = await getDefaultInstance();
      if (!cancelled && dEarly) {
        setDefaultInst(dEarly);
        splashBridge.updateLabel(dEarly.name || dEarly.url);
      }

      // Learn the user's location for distance sorting (used when a directory is
      // opened), but DON'T fetch any directory here — the landing view is the
      // chooser (favourites + directory cards). Directories load on tap.
      try { const loc = await getUserLocation(); if (!cancelled) userLocRef.current = loc; } catch {}
      if (!cancelled) { setLoading(false); }

      // Launched by plugging in an RTL-SDR? Go straight to Local Hardware and skip
      // the default-instance auto-connect below (which would otherwise win the
      // race and open the default server / leave us on the picker).
      if (!cancelled && await tryUsbLaunchRef.current?.(mode)) return;
      if (!cancelled) splashBridge.dismiss();

      // A default instance still auto-connects straight through — unless a
      // vibesdr:// deep link is driving this launch (it owns the session and
      // resets us to its target; auto-connecting to the default would stomp it).
      //
      // WAIT for the cold-start link probe before deciding. getInitialURL() is
      // async, so merely SAMPLING the flag here races it: on a QR cold start the
      // picker could win, auto-connect to the default, and the link would arrive
      // too late (it opened the default instance instead of the scanned one).
      await whenInitialLinkChecked();
      // The WATCH is driving this boot — it has already chosen (or is choosing) a
    // server, so auto-connecting to the default would drag the user straight back to
    // it. Stand down. (`noAutoConnect` is the durable form: we sit BENEATH the
    // watch's target so BACK works, but we must not take over.)
    if (watchTargetPending.claimed || route.params?.noAutoConnect) return;
    if (!cancelled && dEarly && !isDeepLinkActive()) {
        navigation.navigate('SDR', { baseUrl: dEarly.url, instanceName: dEarly.name, viewMode: mode, serverLongitude: null });
      }
    }

    loadAndInit();
    return () => { cancelled = true; };
  }, []);

  const firstFocusRef = useRef(true);
  useFocusEffect(useCallback(() => {
    getViewMode().then(mode => { if (mode) setViewModeState(mode); });
    // Re-read the default AND favourites on every focus — the SDR menu can
    // set/clear both, and returning here doesn't remount (stale star / missing
    // favourite otherwise).
    getDefaultInstance().then(d => setDefaultInst(d)).catch(() => {});
    // Undo the v8.0.0 mis-detection BEFORE reading them, or we'd show (and
    // connect with) the corrupted 'vibeserver' type for one more session.
    repairVibeserverFavourites()
      .then(getFavourites)
      .then(f => setFavourites(f))
      .catch(() => {});
    getTcpFavs().then(f => setTcpFavs(f)).catch(() => {});
    // Skip the initial focus (loadAndInit owns the launch-time USB check — running
    // it here too would race the read-and-clear flag). On LATER focuses (returning
    // from an SDR session), pick up an RTL-SDR that was plugged in while away.
    if (firstFocusRef.current) { firstFocusRef.current = false; return; }
    tryUsbLaunchRef.current?.();
  }, []));

  // mDNS/Bonjour: browse for RTL-TCP servers while the picker is on screen; stop
  // on blur so we don't hold the network browser open during an SDR session.
  useFocusEffect(useCallback(() => {
    setDiscovered([]);
    const stop = startMdnsDiscovery(setDiscovered);
    return () => { stop(); setDiscovered([]); };
  }, []));

  const { colors: C, font: F, scale } = themeFor(viewMode);

  const normalisedCustomUrl = useMemo(() => {
    let u = customUrl.trim().replace(/\/+$/, '');
    if (u && !u.startsWith('http://') && !u.startsWith('https://')) u = 'http://' + u;
    return u;
  }, [customUrl]);
  // A pasted sdr:// / spyserver:// link connects but isn't an http receiver — the
  // http favourite/default stars don't apply; it favourites into the SpyServer
  // list (tcpFavs) instead. Non-null = the custom box holds a valid spy link.
  const customSpyTarget = useMemo(
    () => parseSdrUrl(customUrl.trim().replace(/^spyserver:\/\//i, 'sdr://')),
    [customUrl],
  );
  const fs = (base: number) => Math.round(base * scale);

  const isFav = useCallback((url: string) => favourites.some(f => f.url === url), [favourites]);

  const handleToggleFav = useCallback(async (fav: Favourite) => {
    const next = await toggleFavourite(fav, favourites);
    setFavourites(next);
  }, [favourites]);

  const connect = useCallback(async (url: string, name: string, password?: string, serverLongitude?: number | null, serverType?: 'ubersdr' | 'kiwi' | 'owrx' | 'fmdx') => {
    if (!url) return;
    const cleaned = url.trim().replace(/\/$/, '');
    // FM-DX Webserver: distinct tuner screen, and checkConnection (UberSDR-shaped)
    // doesn't apply — the adapter opens the /text WS itself.
    if (serverType === 'fmdx') {
      navigation.navigate('Tuner', { baseUrl: cleaned, instanceName: name, viewMode });
      return;
    }
    setConnecting(true);
    try {
      const result = await checkConnection(cleaned, password);
      if (result.passwordRequired && !password) {
        setConnecting(false);
        setPwModal({ url: cleaned, name });
        return;
      }
      if (!result.allowed) {
        setConnecting(false);
        Alert.alert('Connection Refused', result.reason ?? 'Server refused connection');
        return;
      }
      setConnecting(false);
      navigation.navigate('SDR', { baseUrl: cleaned, instanceName: name, password, viewMode, serverLongitude, serverType });
    } catch (e: any) {
      setConnecting(false);
      Alert.alert('Connection Error', e.message ?? 'Could not reach server');
    }
  }, [navigation, viewMode]);

  // V4 local hardware (Android only): start the on-device shim (RTL-SDR over
  // USB OTG) and connect to it on localhost. Audio rides /ws/audio (external
  // PCM); spectrum/control reuse the UberSDR path against ws://127.0.0.1.
  const connectLocal = useCallback(async (modeOverride?: typeof viewMode) => {
    const Local = (NativeModules as any).VibeLocalSDR;
    if (!Local?.startSpectrum) { Alert.alert('Local Hardware', 'Not available on this build.'); return; }
    setConnecting(true);
    try {
      // NB: the shim's WS stays bound to localhost (default). VibeServer will later
      // opt into LAN serving behind the sharing screen + PIN; until that auth path
      // exists we must NOT call setServeOnLan(true) — it would expose unauthenticated
      // tuning control to the whole network.
      const res = await Local.startSpectrum({
        // fftSize 8192 over 2.4 MHz ≈ 293 Hz/bin (sharp AM/SSB); fftRate 10 to
        // match UberSDR's line cadence so the waterfall interpolation lines up.
        centerFreq: 100_000_000, sampleRate: 2_400_000, fftSize: 8192, fftRate: 10, mode: 'wfm',
      }) as { port: number; wsBaseUrl: string };
      setConnecting(false);
      navigation.navigate('SDR', {
        baseUrl: res.wsBaseUrl, instanceName: 'Local Hardware', viewMode: modeOverride ?? viewMode,
        serverType: 'ubersdr', isLocal: true, localPort: res.port,
        localGen: newLocalSession(), carFm: true,
      });
    } catch (e: any) {
      setConnecting(false);
      Alert.alert('Local Hardware', e?.message ?? 'Could not start local SDR. Is an RTL-SDR plugged in via USB OTG?');
    }
  }, [navigation, viewMode]);

  // Connect to a remote VibeServer (a shim shared over the LAN). Same client
  // wiring as Local Hardware (spectrum via /ws/user-spectrum, audio via /ws/audio)
  // but pointed at a LAN host, with the PIN resolved to an auth suffix. No local
  // shim is started — the radio lives on the serving phone.
  const connectVibeServer = useCallback(async (host: string, port: number, name: string, pin: string) => {
    setConnecting(true);
    const baseUrl = `http://${host}:${port}`;
    let authSuffix = '';
    try {
      authSuffix = await resolveVibeAuth(baseUrl, pin);
    } catch {
      setConnecting(false);
      Alert.alert('VibeServer', `Could not reach ${host}:${port}. Is it on the same network?`);
      return;
    }
    setConnecting(false);
    navigation.navigate('SDR', {
      baseUrl, instanceName: name, viewMode,
      serverType: 'ubersdr', isLocal: true, localPort: port,
      localHost: host, authSuffix,
      localGen: newLocalSession(),
    });
  }, [navigation, viewMode]);

  // Tap a discovered/typed VibeServer: prompt for the PIN if it needs one, with a
  // saved PIN pre-filled (per host:port) so the user need not retype it. "Save &
  // Connect" persists the entered PIN; "Connect" uses it just this once.
  const openVibeServer = useCallback(async (host: string, port: number, name: string, needsPin: boolean) => {
    if (!needsPin) { connectVibeServer(host, port, name, ''); return; }
    const key = `vs_pin:${host}:${port}`;
    let saved = '';
    try { saved = (await AsyncStorage.getItem(key)) ?? ''; } catch {}
    if (Platform.OS === 'ios' && (Alert as any).prompt) {
      (Alert as any).prompt(
        'VibeServer PIN', `Enter the PIN for ${name}`,
        [{ text: 'Cancel', style: 'cancel' },
         { text: 'Connect', onPress: (pin?: string) => connectVibeServer(host, port, name, pin || saved) },
         { text: 'Save & Connect', onPress: (pin?: string) => {
             const p = pin || saved;
             AsyncStorage.setItem(key, p).catch(() => {});
             connectVibeServer(host, port, name, p);
           } }],
        'plain-text', saved, 'number-pad');
    } else {
      // Android has no Alert.prompt — use the saved PIN if we have one.
      connectVibeServer(host, port, name, saved);
    }
  }, [connectVibeServer]);

  // Route straight into Local Hardware when the app was launched/resumed by
  // plugging in an RTL-SDR (USB_DEVICE_ATTACHED). Returns true if it claimed the
  // launch, so the caller skips the default-instance auto-connect. Native flag is
  // read-and-cleared, so it fires once per attach.
  const tryUsbLaunch = useCallback(async (modeArg?: typeof viewMode): Promise<boolean> => {
    const Local = (NativeModules as any).VibeLocalSDR;
    if (!Local?.consumeUsbLaunch) return false;
    let pending = false;
    try { pending = await Local.consumeUsbLaunch(); } catch { pending = false; }
    if (!pending) return false;
    splashBridge.dismiss();

    // Let the user pick how to use the just-plugged-in dongle: listen on this
    // device, or share it over the network as an RTL-TCP server. (Falls straight
    // through to listen if the server path isn't available on this build.)
    if (rtlTcpServerSupported) {
      Alert.alert(
        'RTL-SDR connected',
        'How would you like to use this dongle?',
        [
          { text: 'Listen on this device', onPress: () => { connectLocal(modeArg); } },
          { text: 'Share over network', onPress: () => navigation.navigate('ServerMode', {}) },
          { text: 'Cancel', style: 'cancel' },
        ],
        { cancelable: true },
      );
    } else {
      await connectLocal(modeArg);
    }
    return true;
  }, [connectLocal, navigation]);
  tryUsbLaunchRef.current = tryUsbLaunch;

  // RTL-TCP: connect to an rtl_tcp server (host:port) over the network and run the
  // same on-device shim against it — no USB, so this also works on iOS. Reuses the
  // local-SDR wiring (isLocal) with isTcp set for the RTL-TCP icon/labels.
  const connectTcp = useCallback(async (host: string, port: number, name: string) => {
    const Local = (NativeModules as any).VibeLocalSDR;
    if (!Local?.startTcp) { Alert.alert('RTL-TCP', 'Not available on this build.'); return; }
    setConnecting(true);
    try {
      // Default to 20m USB — most rtl_tcp sources people add are HF (e.g. UberSDR
      // 0–30 MHz); the persisted last-tune overrides this on the SDR screen.
      const res = await Local.startTcp({
        host, port,
        centerFreq: 14_100_000, sampleRate: 2_400_000, fftSize: 8192, fftRate: 10, mode: 'usb',
      }) as { port: number; wsBaseUrl: string };
      setConnecting(false);
      navigation.navigate('SDR', {
        baseUrl: res.wsBaseUrl, instanceName: name || `${host}:${port}`, viewMode,
        serverType: 'ubersdr', isLocal: true, isTcp: true, localPort: res.port,
        tcpHost: host, tcpPort: port, localGen: newLocalSession(), carFm: true,
      });
    } catch (e: any) {
      setConnecting(false);
      Alert.alert('RTL-TCP', e?.message ?? `Could not connect to ${host}:${port}. Is rtl_tcp running and reachable?`);
    }
  }, [navigation, viewMode]);

  // SpyServer-compatible: same wiring as connectTcp (network IQ -> on-device shim),
  // so demod/decoders/audio work unchanged, on iOS too. The server dictates the
  // sample rate, so we don't ask for one.
  const connectSpy = useCallback(async (host: string, port: number, name: string,
                                       sessionLimitMins?: number) => {
    const Local = (NativeModules as any).VibeLocalSDR;
    if (!Local?.startSpyServer) { Alert.alert('SpyServer', 'Not available on this build.'); return; }
    setConnecting(true);
    try {
      const res = await Local.startSpyServer({
        host, port,
        centerFreq: 100_000_000, fftSize: 8192, fftRate: 10, mode: 'wfm',
      }) as { port: number; wsBaseUrl: string };
      setConnecting(false);
      navigation.navigate('SDR', {
        baseUrl: res.wsBaseUrl, instanceName: name || `${host}:${port}`, viewMode,
        serverType: 'ubersdr', isLocal: true, isTcp: true, localPort: res.port,
        tcpHost: host, tcpPort: port, localGen: newLocalSession(),
        sessionLimitMins,
      });
    } catch (e: any) {
      setConnecting(false);
      Alert.alert('SpyServer', e?.message ?? `Could not connect to ${host}:${port}.`);
    }
  }, [navigation, viewMode]);

  /**
   * THE ROUTER. One place that turns a BackendType into the right connect call.
   *
   * Every backend used to be reached by its own hard-coded entry point, which is
   * why the Custom-server box could only ever produce an http receiver: there was
   * nowhere to say "this turned out to be a VibeServer, open it as one". Probe
   * once, route here, and a single typed address can reach anything we support.
   */
  const connectDetected = useCallback(async (
    type: BackendType, host: string, port: number, name: string,
  ) => {
    const label = name || `${host}:${port}`;
    switch (type) {
      case 'rtltcp':   connectTcp(host, port, label); return;
      case 'spyserver': connectSpy(host, port, label); return;
      case 'vibeserver': {
        // Ask the server itself whether it wants a PIN — a typed host has no mDNS
        // TXT record to tell us. See vibeServerNeedsPin().
        let needsPin = true;
        try { needsPin = await vibeServerNeedsPin(`http://${host}:${port}`); }
        catch {
          Alert.alert('VibeServer', `Could not reach ${host}:${port}. Is it on the same network?`);
          return;
        }
        openVibeServer(host, port, label, needsPin);
        return;
      }
      default: {
        // The HTTP receivers (ubersdr / kiwi / owrx / fmdx) all go through connect(),
        // which routes fmdx to the tuner screen and the rest to the waterfall.
        const url = `http://${host}:${port}`;
        connect(url, label, undefined, null, type);
      }
    }
  }, [connect, connectTcp, connectSpy, openVibeServer]);

  // sdr:// deep link → auto-connect once. Guard with a ref + clear the param so a
  // failed connect leaves the user on the picker (connectSpy's own Alert is the
  // error UX) instead of a retry loop. markDeepLinkActive() (set by useDeepLinks)
  // already suppresses the default-instance auto-connect.
  const autoSpyFired = useRef(false);
  const autoSpy = route.params?.autoSpy;
  useEffect(() => {
    if (!autoSpy) { autoSpyFired.current = false; return; }
    if (autoSpyFired.current || connecting) return;
    autoSpyFired.current = true;
    navigation.setParams({ autoSpy: undefined });
    connectSpy(autoSpy.host, autoSpy.port, `${autoSpy.host}:${autoSpy.port}`);
  }, [autoSpy, connecting, connectSpy, navigation]);

  // Parse the add-RTL-TCP modal: accept "host", "host:port", separate fields, or a
  // pasted "sdr://host:port" / "spyserver://host:port" (strips the scheme + flips
  // to the SpyServer proto — Airspy's map hands out copy-text, so paste must work
  // where tapping can't). Returns the detected proto so the connect routing below
  // doesn't depend on the async setTcpProto having landed yet.
  const parseTcpEntry = useCallback((): { host: string; port: number; proto: BackendType | 'auto' } | null => {
    let h = tcpHost.trim();
    let proto: BackendType | 'auto' = tcpProto;
    const schemeM = /^(sdr|spyserver):\/\//i.exec(h);
    if (schemeM) {
      h = h.slice(schemeM[0].length).replace(/[/?#].*$/, '');   // drop scheme + any path/query junk
      proto = 'spyserver';
      if (tcpProto !== 'spyserver') setTcpProto('spyserver');   // flip the toggle for feedback
    }
    // The port field is a fallback — a port typed into the HOST field wins, since
    // that's the natural way to paste "host:8073".
    let p = parseInt(tcpPort.trim(), 10);
    const u = parseHostPort(h, proto === 'auto' ? undefined : proto);
    if (!u) return null;
    h = u.host;
    if (/:\d+$/.test(tcpHost.trim()) || !Number.isFinite(p) || p <= 0 || p > 65535) p = u.port;
    return { host: h, port: p, proto };
  }, [tcpHost, tcpPort, tcpProto]);

  const tcpModalConnect = useCallback(async (save: boolean) => {
    const parsed = parseTcpEntry();
    if (!parsed) { Alert.alert('Custom server', 'Enter a host (and optional :port).'); return; }
    const name = tcpName.trim() || `${parsed.host}:${parsed.port}`;

    // On AUTO, probe before saving — so the favourite remembers what it actually is
    // and reconnects straight to the right backend next time, with no second probe.
    let type: BackendType | null = parsed.proto === 'auto' ? null : parsed.proto;
    if (!type) {
      setConnecting(true);
      type = await probeServer(parsed.host, parsed.port, null);
      setConnecting(false);
      if (!type) {
        Alert.alert('Custom server',
          `Nothing answered at ${parsed.host}:${parsed.port}.\n\nIf it's an rtl_tcp or SpyServer on a non-standard port, pick the type instead of Auto — raw TCP can't be detected.`);
        return;
      }
    }
    if (save) {
      const next = [...tcpFavs.filter(f => !(f.host === parsed.host && f.port === parsed.port)),
                    { name, host: parsed.host, port: parsed.port, proto: type }];
      setTcpFavs(next); saveTcpFavs(next).catch(() => {});
    }
    setTcpModal(false); setTcpName(''); setTcpHost(''); setTcpPort('');
    connectDetected(type, parsed.host, parsed.port, name);
  }, [parseTcpEntry, tcpName, tcpFavs, connectDetected]);

  const removeTcpFav = useCallback((fav: TcpFav) => {
    const next = tcpFavs.filter(f => !(f.host === fav.host && f.port === fav.port));
    setTcpFavs(next); saveTcpFavs(next).catch(() => {});
  }, [tcpFavs]);

  // Favourite a pasted SpyServer link into the SpyServer/RTL-TCP fav list (where
  // it becomes reconnectable), asking for a name on iOS. Toggles off if present.
  const toggleSpyFav = useCallback((host: string, port: number) => {
    const exists = tcpFavs.some(f => f.proto === 'spyserver' && f.host === host && f.port === port);
    if (exists) {
      const next = tcpFavs.filter(f => !(f.proto === 'spyserver' && f.host === host && f.port === port));
      setTcpFavs(next); saveTcpFavs(next).catch(() => {});
      return;
    }
    const fallback = `${host}:${port}`;
    const add = (name: string) => {
      const next = [...tcpFavs.filter(f => !(f.host === host && f.port === port)),
                    { name, host, port, proto: 'spyserver' as const }];
      setTcpFavs(next); saveTcpFavs(next).catch(() => {});
    };
    if (Platform.OS === 'ios' && (Alert as any).prompt) {
      (Alert as any).prompt('Name this SpyServer', fallback,
        [{ text: 'Cancel', style: 'cancel' },
         { text: 'Save', onPress: (t?: string) => add((t && t.trim()) || fallback) }],
        'plain-text', fallback);
    } else add(fallback);
  }, [tcpFavs]);

  // Pin a discovered (mDNS) server into the RTL-TCP favourites so it survives
  // even when it's not currently advertising.
  const saveDiscovered = useCallback((s: DiscoveredServer) => {
    const next = [...tcpFavs.filter(f => !(f.host === s.host && f.port === s.port)),
                  { name: s.name, host: s.host, port: s.port }];
    setTcpFavs(next); saveTcpFavs(next).catch(() => {});
  }, [tcpFavs]);

  // Discovered servers not already saved as a favourite (dedupe by host:port).
  const discoveredNew = useMemo(
    () => discovered.filter(s => !tcpFavs.some(f => f.host === s.host && f.port === s.port)),
    [discovered, tcpFavs]);

  // Connect a saved favourite: use its stored backend type, or detect it once
  // (and remember it) so an OpenWebRX/Kiwi favourite doesn't reconnect as UberSDR.
  const connectFav = useCallback(async (fav: Favourite) => {
    // FM-DX isn't sniffable by detectServerType (which only knows ubersdr/kiwi/
    // owrx) and letting it "detect" would mis-open an FM-DX fav as UberSDR
    // (waterfall) — trust the stored type and route straight to the tuner.
    if (fav.serverType === 'fmdx') { connect(fav.url, fav.name, undefined, null, 'fmdx'); return; }
    // SpyServer speaks a raw TCP protocol — detectServerType only sniffs HTTP
    // backends and would mis-open it as UberSDR. Route on the stored type.
    if (fav.serverType === 'spyserver') {
      const m = /^spyserver:\/\/([^:]+):(\d+)$/.exec(fav.url);
      if (m) connectSpy(m[1], parseInt(m[2], 10), fav.name);
      return;
    }
    // Re-detect on every connect. A SUCCESSFUL detection is authoritative and
    // self-heals a wrong stored type (e.g. an UberSDR-with-kiwi-emulation that a
    // previous build mis-saved as kiwi). Detection returns null only when the
    // host can't be reached — then keep the stored type rather than guessing.
    const detected = await detectServerType(fav.url);
    const type = detected ?? fav.serverType ?? 'ubersdr';
    if (type !== fav.serverType) setFavouriteServerType(fav.url, type).catch(() => {});
    // A favourite can now detect as a VibeServer (it serves a web page, so it IS
    // sniffable) — that must open through the router, not connect(), or it would be
    // opened as a plain UberSDR receiver with no PIN handshake.
    if (type === 'vibeserver') {
      const u = parseHostPort(fav.url, 'vibeserver');
      if (u) { connectDetected('vibeserver', u.host, u.port, fav.name); return; }
    }
    connect(fav.url, fav.name, undefined, null, type as ServerType | 'fmdx');
  }, [connect, connectDetected]);

  const connectCustom = useCallback(async () => {
    if (!customUrl.trim()) return;
    const raw = customUrl.trim();
    // A pasted SpyServer link (Airspy's directory only offers copy, no tappable
    // anchor) — this single-field box is where people naturally paste it. Route
    // it to the SpyServer path instead of treating it as an http(s) receiver.
    const spy = parseSdrUrl(raw.replace(/^spyserver:\/\//i, 'sdr://'));
    if (spy) { setCustomUrl(''); connectSpy(spy.host, spy.port, `${spy.host}:${spy.port}`); return; }

    const u = parseHostPort(raw);
    if (!u) { Alert.alert('Custom server', `Couldn't read an address from "${raw}".`); return; }

    // Probe, then route. Plain HTTP to public-internet servers IS allowed
    // (NSAllowsArbitraryLoads) — most Kiwi/OpenWebRX receivers are hobbyist HTTP
    // boxes with no TLS, so we don't block them.
    setConnecting(true);
    const type = await probeServer(u.host, u.port, null);
    setConnecting(false);
    if (!type) {
      Alert.alert('Custom server',
        `Nothing answered at ${u.host}:${u.port}.\n\nIf this is an rtl_tcp or SpyServer on a non-standard port, add it with the + button and pick the type — raw TCP servers can't be auto-detected.`);
      return;
    }
    connectDetected(type, u.host, u.port, raw);
  }, [customUrl, connectSpy, connectDetected]);

  const handleSetDefault = useCallback((inst: DefaultInstance) => {
    Alert.alert('Set Default', `Auto-connect to "${inst.name}" on startup?`, [
      { text: 'Cancel', style: 'cancel' },
      { text: 'Set Default', onPress: async () => { await setDefaultInstance(inst); setDefaultInst(inst); } },
    ]);
  }, []);

  const handleClearDefault = useCallback(() => {
    Alert.alert('Remove Default', 'Stop auto-connecting on startup?', [
      { text: 'Cancel', style: 'cancel' },
      { text: 'Remove', style: 'destructive', onPress: async () => { await clearDefaultInstance(); setDefaultInst(null); } },
    ]);
  }, []);

  // Master factory reset — wipes ALL settings and stored data (display
  // prefs, per-instance tunes, favourites, default instance, callsigns…),
  // asking about bookmarks first (never cleared silently).
  const handleMasterReset = useCallback(() => {
    Alert.alert(
      'Factory Reset',
      'Clears ALL settings and stored data and returns the app to a fresh-install state. Recordings already saved to your device are not affected.',
      [
        { text: 'Cancel', style: 'cancel' },
        { text: 'Reset Everything', style: 'destructive', onPress: async () => {
          const bms = await loadUserBookmarks().catch(() => [] as UserBookmark[]);
          const wipe = async (keepBms: boolean) => {
            await AsyncStorage.clear().catch(() => {});
            if (keepBms && bms.length) await saveUserBookmarks(bms).catch(() => {});
            navigation.replace('InstancePicker');
          };
          if (bms.length > 0) {
            Alert.alert(
              'Bookmarks',
              `Keep your ${bms.length} saved bookmark${bms.length !== 1 ? 's' : ''}?`,
              [
                { text: 'Keep', style: 'default', onPress: () => wipe(true) },
                { text: 'Clear All', style: 'destructive', onPress: () => wipe(false) },
              ],
            );
          } else {
            wipe(false);
          }
        } },
      ],
    );
  }, [navigation]);

  // Build list: favourites section (pinned top) then filtered instances
  const listData = useMemo((): ListItem[] => {
    const q = filter.toLowerCase().trim();

    // Favourited custom URLs (not in the instances list)
    const instanceUrls = new Set(instances.map(i => i.url));
    const customFavs: ListItem[] = favourites
      .filter(f => !instanceUrls.has(f.url))
      .filter(f => !q || f.name.toLowerCase().includes(q) || f.url.toLowerCase().includes(q))
      .map(f => ({ kind: 'custom', fav: f }));

    // Favourited instances (pinned to top)
    const favInstances: ListItem[] = instances
      .filter(i => isFav(i.url))
      .filter(i => !q || i.name.toLowerCase().includes(q) || (i.location ?? '').toLowerCase().includes(q) || (i.callsign ?? '').toLowerCase().includes(q))
      .map(i => ({ kind: 'instance', data: i }));

    // Remaining instances (not favourited)
    let rest = instances
      .filter(i => !isFav(i.url))
      .filter(i => !q || i.name.toLowerCase().includes(q) || (i.location ?? '').toLowerCase().includes(q) || (i.callsign ?? '').toLowerCase().includes(q));

    if (sortMode === 'snr') {
      rest = [...rest].sort((a, b) => (b.bestSnr ?? -Infinity) - (a.bestSnr ?? -Infinity));
    }

    return [
      ...customFavs,
      ...favInstances,
      ...rest.map(i => ({ kind: 'instance' as const, data: i })),
    ];
  }, [instances, favourites, filter, sortMode, isFav]);


  if (!modeReady) return <SafeAreaView style={{ flex: 1, backgroundColor: '#0A0A12' }} />;

  const renderItem = ({ item, index }: { item: ListItem; index: number }) => {
    // Section header before first non-favourite instance
    const favCount = listData.filter(d =>
      d.kind === 'custom' || (d.kind === 'instance' && isFav(d.data.url))
    ).length;
    const showFavHeader   = index === 0 && favCount > 0;
    const showOtherHeader = index === favCount && favCount > 0 && listData.length > favCount;

    if (item.kind === 'custom') {
      const fav = item.fav;
      const isDefault = defaultInst?.url === fav.url;
      return (
        <>
          {showFavHeader && <SectionHeader label="FAVOURITES" fs={fs} F={F} C={C} />}
          <TouchableOpacity
            style={[styles.row, { borderColor: C.borderBright, backgroundColor: 'rgba(255,100,100,0.06)' }]}
            onPress={() => connectFav(fav)}
            disabled={connecting}
          >
            <View style={styles.rowMain}>
              <View style={styles.nameRow}>
                <View style={fav.serverType === 'owrx' ? styles.logoChip : undefined}>
                  {TYPE_LOGOS[fav.serverType ?? 'ubersdr']
                    ? <Image source={TYPE_LOGOS[fav.serverType ?? 'ubersdr']} style={styles.typeLogo} resizeMode="contain" />
                    : <Text style={{ fontFamily: F, fontSize: fs(14), color: C.amber }}>📻</Text>}
                </View>
                <Text style={{ fontFamily: F, fontSize: fs(16), color: C.amber, flex: 1 }} numberOfLines={1}>
                  {isDefault ? '★ ' : ''}{fav.name}
                </Text>
              </View>
              <Text style={{ fontFamily: F, fontSize: fs(10), color: C.textDim }} numberOfLines={1}>{fav.url}</Text>
            </View>
            <View style={styles.rowRight}>
              <TouchableOpacity style={{ padding: 4 }}
                onPress={() => isDefault ? handleClearDefault() : handleSetDefault({ name: fav.name, url: fav.url })}>
                <Text style={{ fontSize: fs(18), color: isDefault ? C.amber : C.goldDim }}>{isDefault ? '★' : '☆'}</Text>
              </TouchableOpacity>
              <TouchableOpacity style={{ padding: 4 }} onPress={() => handleToggleFav(fav)}>
                <Text style={{ fontSize: fs(18), color: C.red }}>♥</Text>
              </TouchableOpacity>
            </View>
          </TouchableOpacity>
        </>
      );
    }

    const inst = item.data;
    const isDefault = defaultInst?.url === inst.url;
    const versionOld = isVersionOld(inst.version);
    const favoured = isFav(inst.url);
    // Full receiver — greyed out + unselectable. The two directories report the
    // count with OPPOSITE meaning: UberSDR's `users` = FREE slots (20/20 = empty,
    // 0/20 = full); KiwiSDR's `users` = IN-USE (4/4 = full). So full-ness flips.
    const st = inst.serverType ?? 'ubersdr';
    const isFull = inst.maxUsers > 0 && (st === 'ubersdr'
      ? inst.users <= 0              // ubersdr: 0 free slots = full
      : inst.users >= inst.maxUsers); // kiwi: all in use = full

    return (
      <>
        {showFavHeader  && <SectionHeader label="FAVOURITES" fs={fs} F={F} C={C} />}
        {showOtherHeader && <SectionHeader label="ALL INSTANCES" fs={fs} F={F} C={C} />}
        <TouchableOpacity
          style={[
            styles.row,
            { borderColor: C.border },
            isDefault && { borderColor: C.borderBright, backgroundColor: 'rgba(255,160,0,0.08)' },
            favoured && !isDefault && { borderColor: 'rgba(255,80,80,0.4)' },
            isFull && { opacity: 0.4 },
          ]}
          onPress={() => {
            // SpyServer isn't a web backend: its "url" is spyserver://host:port and
            // it runs through the on-device shim, not a WebSocket to a page.
            if (inst.serverType === 'spyserver') {
              const m = /^spyserver:\/\/([^:]+):(\d+)$/.exec(inst.url);
              if (m) connectSpy(m[1], parseInt(m[2], 10), inst.name, inst.sessionLimitMins);
              return;
            }
            connect(inst.url, inst.name, undefined, inst.longitude, inst.serverType);
          }}
          disabled={connecting || isFull}
        >
          <View style={styles.rowMain}>
            <View style={styles.nameRow}>
              <View style={(inst.serverType ?? 'ubersdr') === 'owrx' ? styles.logoChip : undefined}>
                {TYPE_LOGOS[inst.serverType ?? 'ubersdr']
                  ? <Image source={TYPE_LOGOS[inst.serverType ?? 'ubersdr']} style={styles.typeLogo} resizeMode="contain" />
                  : <Text style={{ fontFamily: F, fontSize: fs(14), color: C.amber }}>📻</Text>}
              </View>
              <Text style={{ fontFamily: F, fontSize: fs(16), color: C.amber, flex: 1 }} numberOfLines={1}>
                {isDefault ? '★ ' : ''}{flagEmoji(inst.countryCode) ? flagEmoji(inst.countryCode) + ' ' : ''}{inst.name}
              </Text>
              {inst.version ? (
                <Text style={{ fontFamily: F, fontSize: fs(11), color: versionOld ? C.red : C.textDim, marginLeft: 6 }}>
                  v{inst.version}
                </Text>
              ) : null}
            </View>
            {versionOld && (
              <Text style={{ fontFamily: F, fontSize: fs(11), color: C.red, marginTop: 2 }}>
                ⚠ Older than v{MIN_RECOMMENDED_VERSION} — may have visual glitches
              </Text>
            )}
            <View style={styles.metaRow}>
              {inst.location ? (
                <Text style={{ fontFamily: F, fontSize: fs(12.5), color: C.gold }} numberOfLines={1}>{inst.location}</Text>
              ) : null}
              {inst.callsign ? (
                <Text style={{ fontFamily: F, fontSize: fs(12.5), color: C.textDim }}>
                  {inst.location ? '  ·  ' : ''}{inst.callsign}
                </Text>
              ) : null}
              {inst.distance != null ? (
                <Text style={{ fontFamily: F, fontSize: fs(12.5), color: C.textDim }}>
                  {'  ·  '}{inst.distance < 1 ? '<1' : Math.round(inst.distance)} km
                </Text>
              ) : null}
            </View>
            {inst.bestSnr != null ? (
              <Text style={{ fontFamily: F, fontSize: fs(11), color: snrColor(inst.bestSnr, C), marginTop: 2 }}>
                {snrLabel(inst.bestSnr)}
              </Text>
            ) : null}
          </View>
          <View style={styles.rowRight}>
            {(inst.users != null && inst.maxUsers) ? (
              <Text style={{ fontFamily: F, fontSize: fs(12), color: isFull ? C.red : C.textDim, fontWeight: isFull ? 'bold' : 'normal' }}>
                {isFull ? 'FULL ' : ''}{inst.users}/{inst.maxUsers}
              </Text>
            ) : null}
            <TouchableOpacity style={{ padding: 4 }}
              onPress={() => isDefault ? handleClearDefault() : handleSetDefault({ name: inst.name, url: inst.url })}>
              <Text style={{ fontSize: fs(18), color: isDefault ? C.amber : C.goldDim }}>{isDefault ? '★' : '☆'}</Text>
            </TouchableOpacity>
            <TouchableOpacity style={{ padding: 4 }}
              onPress={() => handleToggleFav({ name: inst.name, url: inst.url, serverType: inst.serverType })}>
              <Text style={{ fontSize: fs(18), color: favoured ? C.red : C.textDim }}>{favoured ? '♥' : '♡'}</Text>
            </TouchableOpacity>
          </View>
        </TouchableOpacity>
      </>
    );
  };

  return (
    <SafeAreaView style={[styles.safe, { backgroundColor: C.bg }]}>
      <PasswordModal
        visible={!!pwModal}
        serverUrl={pwModal?.url ?? ''}
        onSubmit={pw => { const m = pwModal; setPwModal(null); if (m) connect(m.url, m.name, pw); }}
        onCancel={() => setPwModal(null)}
      />

      {/* Add RTL-TCP server: friendly name + host:port; Connect, or Save & Connect. */}
      <Modal visible={tcpModal} transparent animationType="fade" onRequestClose={() => setTcpModal(false)}>
        <View style={styles.tcpBackdrop}>
          <View style={[styles.tcpCard, { borderColor: C.amber }]}>
            <Text style={{ fontFamily: F, fontSize: fs(16), color: C.amber, letterSpacing: 1, marginBottom: 10 }}>CUSTOM SERVER</Text>
            {/* AUTO is the default and handles almost everything — the probe reads the
                server's own landing page. The explicit choices exist for rtl_tcp and
                SpyServer on NON-STANDARD ports: they're raw TCP with no HTTP to sniff,
                so nothing can identify them but the user. */}
            <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 6, marginBottom: 10 }}>
              {PROTO_CHOICES.map(([id, label]) => (
                <TouchableOpacity key={id} onPress={() => {
                    setTcpProto(id);
                    if (id !== 'auto') setTcpPort(String(DEFAULT_PORT[id as BackendType]));
                  }}
                  style={[styles.tcpBtnAlt, { alignItems: 'center',
                          borderColor: tcpProto === id ? C.amber : C.border,
                          backgroundColor: tcpProto === id ? C.amber + '22' : 'transparent' }]}>
                  <Text style={{ fontFamily: F, fontSize: fs(12), color: tcpProto === id ? C.amber : C.textDim }}>{label}</Text>
                </TouchableOpacity>
              ))}
            </View>
            <Text style={{ fontFamily: F, fontSize: fs(11), color: C.textDim, marginBottom: 10 }}>
              {tcpProto === 'auto'
                ? 'We probe the address and work out what it is. Leave this on Auto unless you\'re adding an rtl_tcp or SpyServer on an unusual port — those speak raw TCP and can\'t be detected.'
                : tcpProto === 'spyserver'
                ? 'Low bandwidth — works over hotspots and mobile data. Speaks the SpyServer protocol used by SDR# and SDR++.'
                : tcpProto === 'rtltcp'
                ? 'Raw full-rate IQ — needs a fast local network. Works with virtually all SDR software.'
                : `Connect as ${PROTO_LABEL[tcpProto as BackendType]}, without probing.`}
            </Text>
            <TextInput style={[styles.tcpInput, { color: C.gold, borderColor: C.border, fontFamily: F }]}
              placeholder="Name (e.g. Shack Pi)" placeholderTextColor={C.textDim}
              value={tcpName} onChangeText={setTcpName} autoCorrect={false} />
            <TextInput style={[styles.tcpInput, { color: C.gold, borderColor: C.border, fontFamily: F }]}
              placeholder="Host, IP or URL (e.g. stuey3d.freemyip.com)" placeholderTextColor={C.textDim}
              value={tcpHost} onChangeText={setTcpHost} autoCapitalize="none" autoCorrect={false} keyboardType="url" />
            <TextInput style={[styles.tcpInput, { color: C.gold, borderColor: C.border, fontFamily: F }]}
              placeholder={tcpProto === 'auto' ? 'Port' : `Port (default ${DEFAULT_PORT[tcpProto as BackendType]})`}
              placeholderTextColor={C.textDim}
              value={tcpPort} onChangeText={setTcpPort} keyboardType="number-pad" />
            <View style={{ flexDirection: 'row', justifyContent: 'flex-end', marginTop: 14, gap: 8, flexWrap: 'wrap' }}>
              <TouchableOpacity style={styles.tcpBtnAlt} onPress={() => setTcpModal(false)}>
                <Text style={{ fontFamily: F, fontSize: fs(13), color: C.textDim }}>Cancel</Text>
              </TouchableOpacity>
              <TouchableOpacity style={styles.tcpBtnAlt} onPress={() => tcpModalConnect(false)}>
                <Text style={{ fontFamily: F, fontSize: fs(13), color: C.gold }}>Connect</Text>
              </TouchableOpacity>
              <TouchableOpacity style={[styles.tcpBtn, { backgroundColor: C.amber }]} onPress={() => tcpModalConnect(true)}>
                <Text style={{ fontFamily: F, fontSize: fs(13), color: '#1a1205', fontWeight: '600' }}>Save & Connect</Text>
              </TouchableOpacity>
            </View>
          </View>
        </View>
      </Modal>
      <KeyboardAvoidingView style={styles.flex} behavior={Platform.OS === 'ios' ? 'padding' : undefined}>

        {/* Header */}
        <View style={[styles.header, { borderBottomColor: C.border }]}>
          <View style={styles.headerLeft}>
            <Text style={{ fontFamily: F, fontSize: fs(24), fontWeight: 'bold', color: C.amber, letterSpacing: 3,
              textShadowColor: C.amberGlow, textShadowOffset: { width: 0, height: 0 }, textShadowRadius: 8 }}>
              VibeSDR
            </Text>
            <Text style={{ fontFamily: F, fontSize: fs(10), color: C.textDim, letterSpacing: 1 }}>v{APP_VERSION}</Text>
          </View>
          {/* ⚙ = factory reset (the mode-change badge is gone — single skin now) */}
          <TouchableOpacity style={{ padding: 10 }} onPress={handleMasterReset} hitSlop={8}>
            <Text style={{ fontSize: fs(22), color: C.textDim }}>⚙</Text>
          </TouchableOpacity>
        </View>

        {/* Default banner */}
        {defaultInst && (
          <View style={[styles.defaultBanner, { borderBottomColor: C.borderBright }]}>
            <Text style={{ fontFamily: F, fontSize: fs(9), color: C.amber, letterSpacing: 2, flexShrink: 0 }}>★ AUTO-CONNECT</Text>
            <Text style={{ fontFamily: F, fontSize: fs(12), color: C.amber, flex: 1 }} numberOfLines={1}>{defaultInst.name}</Text>
            <TouchableOpacity onPress={handleClearDefault} style={{ paddingHorizontal: 8, paddingVertical: 4 }}>
              <Text style={{ fontFamily: F, fontSize: fs(14), color: C.red }}>✕</Text>
            </TouchableOpacity>
          </View>
        )}

        {/* Directory back row — only inside an open directory */}
        {selectedDir !== null && (
          <TouchableOpacity
            style={[styles.backRow, { borderBottomColor: C.border }]}
            onPress={() => { setSelectedDir(null); setInstances([]); setFilter(''); setError(null); setLoading(false); }}
          >
            <Text style={{ fontFamily: F, fontSize: fs(13), color: C.amber }}>‹ Directories</Text>
            <Text style={{ fontFamily: F, fontSize: fs(11), color: C.textDim, letterSpacing: 1 }}>
              {DIRECTORIES.find(d => d.id === selectedDir)?.name ?? ''}
            </Text>
          </TouchableOpacity>
        )}

        {/* Custom URL — chooser only */}
        {selectedDir === null && (
        <View ref={tourRef('customUrl')} collapsable={false} style={styles.customRow}>
          <TextInput
            style={[styles.urlInput, { fontFamily: F, fontSize: fs(12), color: C.amber, borderColor: C.border }]}
            placeholder="Custom URL  e.g. sdr.example.com"
            placeholderTextColor={C.textDim}
            value={customUrl}
            onChangeText={setCustomUrl}
            autoCapitalize="none"
            autoCorrect={false}
            keyboardType="url"
            returnKeyType="go"
            onSubmitEditing={connectCustom}
          />
          {customSpyTarget ? (
            /* SpyServer link pasted — favourite into the SpyServer list (no http
               default-star; SpyServer isn't a default-instance auto-connect). */
            <TouchableOpacity
              style={[styles.connectBtn, { borderColor: C.border, paddingHorizontal: 10 }]}
              onPress={() => toggleSpyFav(customSpyTarget.host, customSpyTarget.port)}
            >
              {(() => {
                const fav = tcpFavs.some(f => f.proto === 'spyserver'
                  && f.host === customSpyTarget.host && f.port === customSpyTarget.port);
                return <Text style={{ fontSize: fs(18), color: fav ? C.red : C.textDim }}>{fav ? '♥' : '♡'}</Text>;
              })()}
            </TouchableOpacity>
          ) : normalisedCustomUrl ? (
            <>
              {/* Heart — favourite this custom URL */}
              <TouchableOpacity
                style={[styles.connectBtn, { borderColor: C.border, paddingHorizontal: 10 }]}
                onPress={() => {
                  const url     = normalisedCustomUrl;
                  const fallback = url.replace(/^https?:\/\//, '');
                  // Already a favourite → just un-favourite. Otherwise ask for a name.
                  if (isFav(url)) { handleToggleFav({ name: fallback, url }); return; }
                  if (Platform.OS === 'ios' && (Alert as any).prompt) {
                    (Alert as any).prompt(
                      'Name this favourite',
                      url,
                      [
                        { text: 'Cancel', style: 'cancel' },
                        { text: 'Save', onPress: (text?: string) =>
                            handleToggleFav({ name: (text && text.trim()) || fallback, url }) },
                      ],
                      'plain-text',
                      fallback,
                    );
                  } else {
                    handleToggleFav({ name: fallback, url });
                  }
                }}
              >
                <Text style={{ fontSize: fs(18), color: isFav(normalisedCustomUrl) ? C.red : C.textDim }}>
                  {isFav(normalisedCustomUrl) ? '♥' : '♡'}
                </Text>
              </TouchableOpacity>
              {/* Star — set as default */}
              <TouchableOpacity
                style={[styles.connectBtn, { borderColor: C.border, paddingHorizontal: 10 }]}
                onPress={() => {
                  const url  = normalisedCustomUrl;
                  const name = url.replace(/^https?:\/\//, '');
                  if (defaultInst?.url === url) {
                    handleClearDefault();
                  } else {
                    Alert.alert('Set as Default', `Auto-connect to "${name}" on every startup?`, [
                      { text: 'Cancel', style: 'cancel' },
                      { text: 'Set Default', onPress: async () => {
                          await setDefaultInstance({ name, url });
                          setDefaultInst({ name, url });
                        },
                      },
                    ]);
                  }
                }}
              >
                <Text style={{ fontSize: fs(18), color: defaultInst?.url === normalisedCustomUrl ? C.amber : C.goldDim }}>
                  {defaultInst?.url === normalisedCustomUrl ? '★' : '☆'}
                </Text>
              </TouchableOpacity>
            </>
          ) : null}
          <TouchableOpacity
            style={[styles.connectBtn, { borderColor: C.borderBright }, connecting && { opacity: 0.5 }]}
            onPress={connectCustom}
            disabled={connecting}
          >
            <Text style={{ fontFamily: F, fontSize: fs(11), fontWeight: 'bold', color: C.amber, letterSpacing: 1 }}>
              {connecting ? '...' : 'CONNECT'}
            </Text>
          </TouchableOpacity>
        </View>
        )}

        {/* Filter + Sort toggle — directory view only */}
        {selectedDir !== null && (
        <View style={styles.filterRow}>
          <TextInput
            style={[styles.filterInput, { fontFamily: F, fontSize: fs(11), color: C.amber, borderColor: C.border, flex: 1 }]}
            placeholder="Search name, location or callsign…"
            placeholderTextColor={C.textDim}
            value={filter}
            onChangeText={setFilter}
            autoCapitalize="none"
            autoCorrect={false}
          />
          <TouchableOpacity
            style={[styles.sortBtn, { borderColor: sortMode === 'snr' ? C.borderBright : C.border }]}
            onPress={() => setSortMode(m => m === 'nearest' ? 'snr' : 'nearest')}
          >
            <Text style={{ fontFamily: F, fontSize: fs(9), color: sortMode === 'snr' ? C.amber : C.textDim, letterSpacing: 1 }}>
              {sortMode === 'snr' ? '📶 SNR' : '📍 NEAR'}
            </Text>
          </TouchableOpacity>
        </View>
        )}

        {/* Body: directory list when one is open, else the chooser */}
        {selectedDir !== null ? (
          loading ? (
            <View style={styles.centred}>
              <Text style={{ fontFamily: F, fontSize: fs(12), color: C.textDim }}>Loading…</Text>
            </View>
          ) : error ? (
            <View style={styles.centred}>
              <Text style={{ fontFamily: F, fontSize: fs(12), color: C.red, textAlign: 'center' }}>{error}</Text>
              <TouchableOpacity onPress={() => openDirectory(selectedDir)}>
                <Text style={{ fontFamily: F, fontSize: fs(12), color: C.amber, textDecorationLine: 'underline' }}>Retry</Text>
              </TouchableOpacity>
            </View>
          ) : (
            <FlatList
              data={listData}
              keyExtractor={item => item.kind === 'custom' ? 'custom:' + item.fav.url : item.data.url}
              renderItem={renderItem}
              contentContainerStyle={{ paddingHorizontal: 12, paddingBottom: 40 }}
              ItemSeparatorComponent={() => <View style={{ height: 6 }} />}
              ListEmptyComponent={
                <Text style={{ fontFamily: F, fontSize: fs(12), color: C.textDim, textAlign: 'center', marginTop: 40 }}>
                  No instances found
                </Text>
              }
            />
          )
        ) : (
          <FlatList
            data={listData}
            keyExtractor={item => item.kind === 'custom' ? 'custom:' + item.fav.url : item.data.url}
            renderItem={renderItem}
            contentContainerStyle={{ paddingHorizontal: 12, paddingBottom: 40 }}
            ItemSeparatorComponent={() => <View style={{ height: 6 }} />}
            ListHeaderComponent={
              <View style={{ marginBottom: 4 }}>
                {/* RTL-SDR — the dongle plugged into THIS phone. Two things you can do
                    with it, so it reads as one heading with two choices rather than a
                    "Local Hardware" row with a share action bolted underneath.
                    Android only: iOS has no USB host SDR. */}
                {Platform.OS === 'android' && (<>
                  <SectionHeader label="RTL-SDR" fs={fs} F={F} C={C} />
                  <TouchableOpacity
                    style={[styles.row, { borderColor: C.amber }]}
                    onPress={() => connectLocal()}
                  >
                    <View style={styles.rowMain}>
                      <Text style={{ fontFamily: F, fontSize: fs(16), color: C.amber }} numberOfLines={1}>Listen</Text>
                      <Text style={{ fontFamily: F, fontSize: fs(11.5), color: C.textDim, marginTop: 2 }} numberOfLines={1}>
                        tune the dongle plugged into this phone (USB-C OTG)
                      </Text>
                    </View>
                    <View style={{ marginLeft: 4 }}><UsbSdrIcon size={26} color={C.amber} strokeWidth={2.4} /></View>
                    <Text style={{ fontFamily: F, fontSize: fs(20), color: C.goldDim, marginLeft: 8 }}>›</Text>
                  </TouchableOpacity>
                  {rtlTcpServerSupported && (
                    <TouchableOpacity
                      style={[styles.row, { borderColor: C.amber }]}
                      onPress={() => navigation.navigate('ServerMode', {})}
                    >
                      <View style={styles.rowMain}>
                        <Text style={{ fontFamily: F, fontSize: fs(16), color: C.amber }} numberOfLines={1}>⇆ Use as server</Text>
                        <Text style={{ fontFamily: F, fontSize: fs(11.5), color: C.textDim, marginTop: 2 }} numberOfLines={1}>
                          serve this dongle to other devices on your Wi-Fi
                        </Text>
                      </View>
                      <Text style={{ fontFamily: F, fontSize: fs(20), color: C.goldDim, marginLeft: 8 }}>›</Text>
                    </TouchableOpacity>
                  )}
                </>)}

                {/* CUSTOM SERVER — any address you type. The probe works out what's
                    actually listening (VibeServer, OWRX, Kiwi, UberSDR, FM-DX,
                    rtl_tcp, SpyServer), so one box reaches every backend. */}
                <View style={{ marginTop: Platform.OS === 'android' ? 10 : 0 }}>
                  <SectionHeader label="CUSTOM SERVER" fs={fs} F={F} C={C} />
                  {tcpFavs.map((f) => (
                    <TouchableOpacity key={`${f.host}:${f.port}`}
                      style={[styles.row, { borderColor: C.amber }]}
                      onPress={() => connectDetected(
                        (f.proto ?? 'rtltcp') as BackendType, f.host, f.port, f.name)}
                      onLongPress={() => Alert.alert(f.name, `${f.host}:${f.port}`, [
                        { text: 'Cancel', style: 'cancel' },
                        { text: 'Delete', style: 'destructive', onPress: () => removeTcpFav(f) },
                      ])}
                    >
                      <Image source={require('../../assets/rtltcp.png')}
                        style={{ width: 26, height: 26, tintColor: C.amber, marginRight: 8 }} resizeMode="contain" />
                      <View style={styles.rowMain}>
                        <Text style={{ fontFamily: F, fontSize: fs(16), color: C.amber }} numberOfLines={1}>{f.name}</Text>
                        <Text style={{ fontFamily: F, fontSize: fs(11.5), color: C.textDim, marginTop: 2 }} numberOfLines={1}>
                          {PROTO_LABEL[(f.proto ?? 'rtltcp') as BackendType] ?? 'auto'} · {f.host}:{f.port}
                        </Text>
                      </View>
                      <TouchableOpacity hitSlop={{ top: 10, bottom: 10, left: 10, right: 10 }}
                        onPress={() => Alert.alert('Delete', `Remove "${f.name}"?`, [
                          { text: 'Cancel', style: 'cancel' },
                          { text: 'Delete', style: 'destructive', onPress: () => removeTcpFav(f) },
                        ])}>
                        <Text style={{ fontFamily: F, fontSize: fs(18), color: C.goldDim, paddingHorizontal: 8 }}>✕</Text>
                      </TouchableOpacity>
                    </TouchableOpacity>
                  ))}
                  <TouchableOpacity
                    style={[styles.row, { borderColor: C.goldDim, borderStyle: 'dashed' }]}
                    onPress={() => setTcpModal(true)}
                  >
                    <View style={styles.rowMain}>
                      <Text style={{ fontFamily: F, fontSize: fs(15), color: C.gold }} numberOfLines={1}>+ Add custom server</Text>
                      <Text style={{ fontFamily: F, fontSize: fs(11.5), color: C.textDim, marginTop: 2 }} numberOfLines={1}>
                        name + address of any SDR server — we work out the type
                      </Text>
                    </View>
                  </TouchableOpacity>
                </View>

                {/* DISCOVERED — RTL-TCP servers found automatically on the local
                    network via mDNS/Bonjour. Only shown when something advertises. */}
                {discoveredNew.length > 0 && (
                  <View style={{ marginTop: 10 }}>
                    <SectionHeader label="DISCOVERED" fs={fs} F={F} C={C} />
                    {discoveredNew.map((s) => (
                      <TouchableOpacity key={`disc-${s.host}:${s.port}`}
                        style={[styles.row, { borderColor: C.amber }]}
                        onPress={() => s.proto === 'vibeserver'
                          ? openVibeServer(s.host, s.port, s.name, s.pin)
                          : connectTcp(s.host, s.port, s.name)}
                      >
                        <Image source={require('../../assets/rtltcp.png')}
                          style={{ width: 26, height: 26, tintColor: C.amber, marginRight: 8 }} resizeMode="contain" />
                        <View style={styles.rowMain}>
                          <Text style={{ fontFamily: F, fontSize: fs(16), color: C.amber }} numberOfLines={1}>{s.name}</Text>
                          <Text style={{ fontFamily: F, fontSize: fs(11.5), color: C.textDim, marginTop: 2 }} numberOfLines={1}>
                            {s.proto === 'vibeserver'
                              ? `VibeServer · ${s.host}:${s.port}${s.pin ? ' · 🔒' : ''}`
                              : `rtl_tcp · ${s.host}:${s.port} · on your network`}
                          </Text>
                        </View>
                        <TouchableOpacity hitSlop={{ top: 10, bottom: 10, left: 10, right: 10 }}
                          onPress={() => saveDiscovered(s)}>
                          <Text style={{ fontFamily: F, fontSize: fs(20), color: C.goldDim, paddingHorizontal: 8 }}>☆</Text>
                        </TouchableOpacity>
                      </TouchableOpacity>
                    ))}
                  </View>
                )}
              </View>
            }
            ListFooterComponent={
              <View style={{ marginTop: 14 }}>
                <SectionHeader label="DIRECTORIES" fs={fs} F={F} C={C} />
                {DIRECTORIES.map(d => (
                  <TouchableOpacity
                    key={d.id}
                    style={[styles.row, { borderColor: C.border, marginBottom: 6 }]}
                    onPress={() => openDirectory(d.id)}
                  >
                    <View style={styles.rowMain}>
                      <Text style={{ fontFamily: F, fontSize: fs(16), color: C.amber }} numberOfLines={1}>{d.name}</Text>
                      <Text style={{ fontFamily: F, fontSize: fs(11.5), color: C.textDim, marginTop: 2 }} numberOfLines={1}>{d.desc}</Text>
                    </View>
                    <View style={styles.rowRight}>
                      {d.kinds.map(k => (
                        <View key={k} style={k === 'owrx' ? styles.logoChip : undefined}>
                          {TYPE_LOGOS[k]
                            ? <Image source={TYPE_LOGOS[k]} style={styles.typeLogo} resizeMode="contain" />
                            : <Text style={{ fontFamily: F, fontSize: fs(18), color: C.amber }}>📻</Text>}
                        </View>
                      ))}
                      <Text style={{ fontFamily: F, fontSize: fs(20), color: C.goldDim, marginLeft: 4 }}>›</Text>
                    </View>
                  </TouchableOpacity>
                ))}
                <Text style={{ fontFamily: F, fontSize: fs(10.5), color: C.textDim, lineHeight: fs(15),
                               paddingHorizontal: 6, paddingTop: 10, paddingBottom: 4 }}>
                  ⚠ KiwiSDRs have very few listening slots, so owners choose who connects. Some
                  allow only their own web page and refuse apps like VibeSDR; some block broadcast
                  or commercial bands and disconnect you the moment you tune there. A refusal or
                  sudden drop is the owner's restriction — not a fault in VibeSDR. For unrestricted
                  access, use UberSDR or OpenWebRX.
                </Text>
              </View>
            }
          />
        )}
      </KeyboardAvoidingView>

      {/* First-run guided tour (dismissable) */}
      {pickerTour.overlay}
    </SafeAreaView>
  );
}

function SectionHeader({ label, fs, F, C }: { label: string; fs: (n: number) => number; F: string; C: any }) {
  return (
    <View style={{ paddingHorizontal: 4, paddingTop: 10, paddingBottom: 4 }}>
      <Text style={{ fontFamily: F, fontSize: fs(9), color: C.textDim, letterSpacing: 2 }}>{label}</Text>
    </View>
  );
}

function snrLabel(snr: number): string {
  if (snr >= 30) return '▲ Excellent conditions';
  if (snr >= 20) return '▲ Good conditions';
  if (snr >= 6)  return '△ Fair conditions';
  return '▽ Poor conditions';
}
function snrColor(snr: number, C: any): string {
  if (snr >= 30) return C.green;
  if (snr >= 20) return '#88cc44';
  if (snr >= 6)  return C.amber;
  return C.red;
}

const styles = StyleSheet.create({
  safe:          { flex: 1 },
  flex:          { flex: 1 },
  header:        { flexDirection: 'row', alignItems: 'center', padding: 16, paddingBottom: 8, borderBottomWidth: 1 },
  headerLeft:    { flex: 1, flexDirection: 'row', alignItems: 'baseline', gap: 8 },
  defaultBanner: { flexDirection: 'row', alignItems: 'center', backgroundColor: 'rgba(255,160,0,0.10)', borderBottomWidth: 1, paddingHorizontal: 12, paddingVertical: 8, gap: 8 },
  customRow:     { flexDirection: 'row', gap: 8, padding: 12, paddingBottom: 8 },
  urlInput:      { flex: 1, height: 44, backgroundColor: 'rgba(20,10,0,0.75)', borderWidth: 1, borderRadius: 6, paddingHorizontal: 12 },
  connectBtn:    { height: 44, backgroundColor: 'rgba(20,10,0,0.75)', borderWidth: 1, borderRadius: 6, paddingHorizontal: 12, justifyContent: 'center' },
  filterRow:     { flexDirection: 'row', gap: 8, paddingHorizontal: 12, marginBottom: 8, alignItems: 'center' },
  filterInput:   { height: 40, backgroundColor: 'rgba(10,8,4,0.60)', borderWidth: 1, borderRadius: 6, paddingHorizontal: 12 },
  sortBtn:       { height: 40, borderWidth: 1, borderRadius: 6, paddingHorizontal: 10, justifyContent: 'center', backgroundColor: 'rgba(10,8,4,0.60)' },
  row:           { flexDirection: 'row', alignItems: 'flex-start', paddingVertical: 10, paddingHorizontal: 12, backgroundColor: 'rgba(20,10,0,0.55)', borderWidth: 1, borderRadius: 6 },
  rowMain:       { flex: 1 },
  tcpBackdrop:   { flex: 1, backgroundColor: 'rgba(0,0,0,0.65)', justifyContent: 'center', padding: 22 },
  tcpCard:       { backgroundColor: 'rgba(16,10,2,0.98)', borderWidth: 1, borderRadius: 12, padding: 18 },
  tcpInput:      { height: 44, backgroundColor: 'rgba(8,6,2,0.7)', borderWidth: 1, borderRadius: 6, paddingHorizontal: 12, marginBottom: 8 },
  tcpBtn:        { borderRadius: 8, paddingHorizontal: 16, paddingVertical: 10 },
  tcpBtnAlt:     { borderRadius: 8, paddingHorizontal: 14, paddingVertical: 10, borderWidth: 1, borderColor: 'rgba(255,184,77,0.45)' },
  rowRight:      { alignItems: 'center', gap: 6, flexDirection: 'row', marginLeft: 8 },
  nameRow:       { flexDirection: 'row', alignItems: 'center', gap: 6 },
  typeLogo:      { width: 20, height: 20 },
  logoChip:      { backgroundColor: 'rgba(235,235,235,0.92)', borderRadius: 5, padding: 2 },
  backRow:       { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', paddingHorizontal: 14, paddingVertical: 10, borderBottomWidth: 1 },
  metaRow:       { flexDirection: 'row', flexWrap: 'wrap', marginTop: 2 },
  centred:       { flex: 1, alignItems: 'center', justifyContent: 'center', gap: 12, padding: 20 },
});
