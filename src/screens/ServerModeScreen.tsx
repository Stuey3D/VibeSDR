import React, { useCallback, useEffect, useRef, useState } from 'react';
import {
  View, Text, TextInput, TouchableOpacity, ScrollView, ActivityIndicator,
  StyleSheet, Platform, PermissionsAndroid, Switch, Alert, NativeModules,
} from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import AsyncStorage from '@react-native-async-storage/async-storage';
import type { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { themeFor } from '../constants/theme';
import { getServerName, saveServerName } from '../services/rtlTcpServer';
import {
  startVibeServer, stopVibeServer, getVibeServerStatus, setVibeServerCompressAudio, getConnectedRadio,
  setVibeServerAdminSecret, setVibeServerUncompressedAudio, setVibeServerSessionLimit,
  vibeServerSupported, randomPin, fmtRate, FPS_TIERS, fpsForTier,
  getServerLocationMode, setServerLocationMode, getManualServerLocation,
  setManualServerLocation, resolveLocation,
  getLearnedBookmarksNow, importServerBookmarks, clearServerBookmarks,
  type FpsTier, type VibeServerInfo, type VibeServerStatus, type LocationMode,
} from '../services/vibeServer';
import { advertiseServer, stopAdvertiseRtlTcp } from '../services/mdns';
import * as DocumentPicker from 'expo-document-picker';
import * as FileSystem from 'expo-file-system';

type Props = NativeStackScreenProps<RootStackParamList, 'ServerMode'>;

// Server-mode picker + VibeServer control screen.
//   • VibeServer (default) — server-side DSP, compressed audio + waterfall,
//     ~25x lighter than raw IQ, HMAC PIN. Handled inline here.
//   • RTL-TCP — raw IQ, maximum compatibility. Delegates to RtlTcpServerScreen.
// The auto-discovery (mDNS) toggle and the advertised name are shared by both.

type Proto = 'vibeserver' | 'rtltcp';
type PinMode = 'random' | 'custom' | 'off';

// 0 = CLIENT-CONTROLLED: the client picks the span live (the same convention as the
// RTL-TCP server's overrideRate). Anything else PINS the rate — the client's picker
// is then hidden and told the server set it, because a rate it can't change is a
// rate it shouldn't offer.
// ★★ PER RADIO. These were a dongle's rates offered to whatever was plugged in, so an Airspy
// HF+ — which tops out near 912 kHz — was shown 2.4 MHz, 1.2 MHz and 960 kHz: every option
// impossible (Stuart, 2026-07-27). The shim would snap a pinned rate to the nearest real one,
// so the menu was lying rather than breaking, which is worse.
// ★ FIFTH time today the same shape has bitten: a list written when there was one radio.
const RATE_OPTIONS_RTL = [
  { label: 'Client-controlled', value: 0 },
  { label: 'Full · 2.4 MHz',  value: 2_400_000 },
  { label: '1.2 MHz',         value: 1_200_000 },
  { label: '960 kHz (light)', value: 960_000 },
];
// Airspy HF+ Discovery / Dual Port. ★ The radio's own list still wins once it is running —
// this is which menu to draw, not a claim about the hardware.
const RATE_OPTIONS_AHF = [
  { label: 'Client-controlled', value: 0 },
  { label: 'Full · 912 kHz',  value: 912_000 },
  { label: '768 kHz',         value: 768_000 },
  { label: '384 kHz (light)', value: 384_000 },
];

const K = {
  proto: 'vs_proto', advertise: 'vs_advertise', pinMode: 'vs_pinmode',
  pin: 'vs_pin', rate: 'vs_rate', fps: 'vs_fps', compress: 'vs_compress',
  webServer: 'vs_webserver', autoRestore: 'vs_autorestore',
  adminPw: 'vs_adminpw', uncomp: 'vs_uncompressed', limitMin: 'vs_sessionlimit',
};

export default function ServerModeScreen({ navigation, route }: Props) {
  const { colors: C, font: F } = themeFor();

  const [proto, setProto]         = useState<Proto>('vibeserver');
  const [name, setName]           = useState(route.params?.name ?? 'VibeSDR');
  const [advertise, setAdvertise] = useState(true);
  const [pinMode, setPinMode]     = useState<PinMode>('random');
  const [pin, setPin]             = useState(() => randomPin(Date.now()));
  const [rate, setRate]           = useState(0);          // 0 = client-controlled
  const [fps, setFps]             = useState<FpsTier>('full');
  const [compress, setCompress]   = useState(true);
  // ★ Admin password — protects the CONTROLS (bias-T, direct sampling, calibration), not
  // access. Deliberately separate from the listening PIN: Hans's server is open to everyone
  // and still must not let a visitor put DC on the feedline.
  const [adminPw, setAdminPw]     = useState('');
  const [showAdminPw, setShowAdminPw] = useState(false);
  // 0 = off, 1 = listener's choice, 2 = compatibility only.
  const [uncomp, setUncomp]       = useState<0 | 1 | 2>(0);
  // ★ Per-listener time limit, minutes. 0 = unlimited — right for a private receiver.
  const [limitMin, setLimitMin]   = useState(0);
  // ★ Which radio is attached, so the menus match it. VID/PID only — see getConnectedRadio.
  const [radio, setRadio] = useState<{ driver: string; model: string } | null>(null);
  useEffect(() => { void getConnectedRadio().then(setRadio); }, []);
  const rateOptions = radio?.driver === 'airspyhf' ? RATE_OPTIONS_AHF : RATE_OPTIONS_RTL;
  // The rate ceiling to quote in prose, so the hint cannot drift from the list above it.
  const topRateLabel = radio?.driver === 'airspyhf' ? '912 kHz' : '2.4 MHz';
  const [webServer, setWebServer] = useState(true);
  const [autoRestore, setAutoRestore] = useState(true);
  const [bmCount, setBmCount] = useState<number | null>(null);
  const [bmMsg, setBmMsg]     = useState('');
  const [locMode, setLocMode]     = useState<LocationMode>('off');
  const [locCity, setLocCity]     = useState('');

  const [running, setRunning] = useState<VibeServerInfo | null>(null);
  const [status, setStatus]   = useState<VibeServerStatus | null>(null);
  /** The mDNS hostname the responder actually TOOK — "vibesdr-moto-g35", or with a "-2"
   *  suffix if another phone on the LAN already had the name. Asked for after the server
   *  is up, because the name isn't settled until the responder has probed for a clash. */
  const [mdnsHost, setMdnsHost] = useState('');
  const [starting, setStarting] = useState(false);
  const [error, setError]     = useState<string | null>(null);
  const runningRef = useRef(false);

  // Load saved preferences + name.
  useEffect(() => {
    (async () => {
      const n = await getServerName(route.params?.name ?? 'VibeSDR');
      setName(n);
      try {
        const [p, a, pm, sp, r, fp, cp, ws, ar, apw, unc, lim] = await Promise.all([
          AsyncStorage.getItem(K.proto), AsyncStorage.getItem(K.advertise),
          AsyncStorage.getItem(K.pinMode), AsyncStorage.getItem(K.pin),
          AsyncStorage.getItem(K.rate), AsyncStorage.getItem(K.fps),
          AsyncStorage.getItem(K.compress), AsyncStorage.getItem(K.webServer),
          AsyncStorage.getItem(K.autoRestore),
          AsyncStorage.getItem(K.adminPw), AsyncStorage.getItem(K.uncomp),
          AsyncStorage.getItem(K.limitMin),
        ]);
        if (p === 'rtltcp' || p === 'vibeserver') setProto(p);
        if (a != null) setAdvertise(a !== '0');
        if (ws != null) setWebServer(ws !== '0');
        if (ar != null) setAutoRestore(ar !== '0');
        if (apw != null) setAdminPw(apw);
        if (unc === '1' || unc === '2') setUncomp(unc === '1' ? 1 : 2);
        if (lim != null) setLimitMin(Number(lim) || 0);
        setLocMode(await getServerLocationMode());
        setLocCity((await getManualServerLocation())?.label ?? '');
        if (pm === 'random' || pm === 'custom' || pm === 'off') setPinMode(pm);
        // Restore the saved PIN for BOTH modes so re-opening the server keeps the
        // same code — it only changes when the user taps refresh (↻) or edits it.
        if (sp) setPin(sp);
        else AsyncStorage.setItem(K.pin, pin);   // first run: persist the generated default
        // NB: 0 is a REAL value here (client-controlled), so no `if (r)` / `|| default`
        // — both would silently turn "client-controlled" back into a pinned 2.4 MHz.
        if (r != null && Number.isFinite(Number(r))) setRate(Number(r));
        if (fp === 'full' || fp === 'half' || fp === 'quarter') setFps(fp);
        if (cp != null) setCompress(cp !== '0');
      } catch {}
    })();
  }, []);

  // Stop the server when leaving unless it's running (VibeServer is ad-hoc: a
  // single remote client, so we tear down on exit to free the dongle).
  useEffect(() => () => {
    if (runningRef.current) { stopAdvertiseRtlTcp(); stopVibeServer(); }
  }, []);

  // Poll live status once serving.
  useEffect(() => {
    if (!running) return;
    const t = setInterval(async () => {
      const s = await getVibeServerStatus();
      if (s) setStatus(s);
    }, 1500);
    return () => clearInterval(t);
  }, [running]);

  // The NAME the mDNS responder took. Asked for once the server is up, and RETRIED for a
  // few seconds: the responder probes the network for a clash before it commits to a
  // name (that's how a second phone ends up as "-2"), so the answer is not ready the
  // instant the server starts.
  useEffect(() => {
    if (!running) { setMdnsHost(''); return; }
    let cancelled = false;
    let tries = 0;
    const ask = async () => {
      const Local = (NativeModules as any).VibeLocalSDR;
      try {
        const h = await Local?.getMdnsHostname?.();
        if (cancelled) return;
        if (h) { setMdnsHost(String(h)); return; }
      } catch { /* not serving, or no responder on this platform */ }
      if (!cancelled && ++tries < 8) setTimeout(ask, 700);
    };
    void ask();
    return () => { cancelled = true; };
  }, [running]);

  // The bookmark count grows on its own as clients tune, so refresh it rather than
  // compute it once.
  const refreshBmCount = useCallback(async () => {
    const list = await getLearnedBookmarksNow();
    setBmCount(list.length);
  }, []);
  useEffect(() => { void refreshBmCount(); }, [refreshBmCount, running]);

  const onImportBookmarks = useCallback(async () => {
    try {
      const res = await DocumentPicker.getDocumentAsync({ type: '*/*', copyToCacheDirectory: true });
      if (res.canceled || !res.assets?.length) return;
      const text = await FileSystem.readAsStringAsync(res.assets[0].uri);
      const n = await importServerBookmarks(text);
      setBmMsg(`Imported ${n} bookmark${n === 1 ? '' : 's'}`);
      void refreshBmCount();
    } catch (e: any) {
      setBmMsg(e?.message ?? 'Could not read that file');
    }
  }, [refreshBmCount]);

  const onResetBookmarks = useCallback(() => {
    Alert.alert(
      'Reset bookmarks',
      'Delete every station this server has learned or had imported? Clients will see an empty list. This cannot be undone.',
      [
        { text: 'Cancel', style: 'cancel' },
        { text: 'Reset', style: 'destructive', onPress: async () => {
            await clearServerBookmarks();
            setBmMsg('Bookmarks cleared');
            void refreshBmCount();
          } },
      ],
    );
  }, [refreshBmCount]);

  const effectivePin = pinMode === 'off' ? '' : pin;

  /**
   * Warn if the OS has BACKGROUND-RESTRICTED us, before we start serving.
   *
   * SDRScreen already checks this — but only for local LISTENING, and it never mounts
   * when you go straight to "Use as server". So a fresh install on a phone that
   * restricts by default (Motorola does) starts a server the OS then quietly starves,
   * with nothing on screen to explain it. That's worse for a server than for audio:
   * nobody is watching it, so the only symptom is "my receiver stopped answering".
   *
   * Resolves true if we should go ahead and start.
   */
  const checkBackgroundAllowed = useCallback(async (): Promise<boolean> => {
    const Local = (NativeModules as any).VibeLocalSDR;
    if (!Local?.isBackgroundRestricted) return true;
    let restricted = false;
    try { restricted = await Local.isBackgroundRestricted(); } catch { return true; }
    if (!restricted) return true;

    return new Promise<boolean>(resolve => {
      Alert.alert(
        'Allow background usage',
        "This phone restricts VibeSDR when it isn't on screen, which will starve the server — clients drop out or stop connecting.\n\n" +
        "To fix it:\n" +
        "1. Tap \u201cOpen Settings\u201d.\n" +
        "2. Open \u201cApp battery usage\u201d (or \u201cBattery\u201d) and turn ON \u201cAllow background usage\u201d (some phones call it \u201cUnrestricted\u201d / \u201cDon't optimise\u201d).\n" +
        "3. Come back and start the server.",
        [
          { text: 'Open Settings', onPress: () => { Local?.openAppSettings?.(); resolve(false); } },
          { text: 'Start anyway', style: 'destructive', onPress: () => resolve(true) },
        ],
        { cancelable: true, onDismiss: () => resolve(false) },
      );
    });
  }, []);

  const start = useCallback(async () => {
    if (!(await checkBackgroundAllowed())) return;
    setError(null);
    setStarting(true);
    const n = name.trim() || 'VibeSDR';
    saveServerName(n);
    await AsyncStorage.multiSet([
      [K.proto, proto], [K.advertise, advertise ? '1' : '0'],
      [K.pinMode, pinMode], [K.pin, pin], [K.rate, String(rate)],
      [K.fps, fps], [K.compress, compress ? '1' : '0'],
      [K.webServer, webServer ? '1' : '0'],
      [K.autoRestore, autoRestore ? '1' : '0'],
      [K.adminPw, adminPw], [K.uncomp, String(uncomp)], [K.limitMin, String(limitMin)],
    ]);
    if (Platform.OS === 'android' && Platform.Version >= 33) {
      try { await PermissionsAndroid.request(PermissionsAndroid.PERMISSIONS.POST_NOTIFICATIONS); } catch {}
    }

    // Resolve the location BEFORE starting, so the very first client to connect
    // already sees it. A typed city is geocoded once, here — never per client.
    if (locMode === 'manual') {
      const city = locCity.trim();
      const known = await getManualServerLocation();
      if (city && known?.label !== city) {
        const geo = await resolveLocation(city);
        if (!geo) {
          setStarting(false);
          setError(`Couldn't find "${city}". Check the spelling, or enter a Maidenhead locator (e.g. IO92nh) — that needs no internet.`);
          return;
        }
        await setManualServerLocation(geo);
      }
    }
    await setServerLocationMode(locMode);

    try {
      const info = await startVibeServer({
        name: n,
        // rate 0 = client-controlled: start at the full span and let the client
        // narrow it. Anything else both starts AND pins there.
        sampleRate: rate || 2_400_000,
        lockedRate: rate,
        pin: effectivePin,
        maxFftRate: fpsForTier(fps),
        compressAudio: compress,
        adminPassword: adminPw,
        uncompressedAudio: uncomp,
        sessionLimitMin: limitMin,
        webServer,
        advertise,
        autoRestore,
      });
      setRunning(info);
      runningRef.current = true;
      setStarting(false);
      if (advertise) advertiseServer(n, info.port, 'vibeserver', effectivePin !== '');
    } catch (e: any) {
      setStarting(false);
      setError(e?.message ?? 'Could not start VibeServer. Is a supported SDR plugged in via USB OTG?');
    }
  // ★★★ EVERY PIECE OF STATE THIS READS MUST BE LISTED. adminPw, uncomp and limitMin were
  // missing, so `start` was frozen with their INITIAL values — '' and 0 — and no amount of
  // typing changed what it sent. The server dutifully applied an empty password and no limit,
  // and reported exactly that.
  // ★★ IT ALSO ATE THE SETTINGS. This callback persists what it is about to send, so every
  // Start wrote the STALE '' over the real password in storage — which is why the field came
  // back BLANK next time and made it look like a save bug. One stale closure, two symptoms,
  // and an evening of looking at the server for a fault that was three lines above it
  // (Stuart, 2026-07-27).
  }, [name, proto, advertise, pinMode, pin, rate, fps, compress, effectivePin,
      webServer, autoRestore, locMode, locCity, checkBackgroundAllowed,
      adminPw, uncomp, limitMin]);

  const stopAndBack = useCallback(() => {
    stopAdvertiseRtlTcp();
    stopVibeServer();
    runningRef.current = false;
    navigation.goBack();
  }, [navigation]);

  const toggleCompress = useCallback((on: boolean) => {
    setCompress(on);
    if (runningRef.current) setVibeServerCompressAudio(on);   // live toggle
  }, []);

  const regenPin = useCallback(() => {
    const p = randomPin(Date.now());
    setPin(p);
    AsyncStorage.setItem(K.pin, p);   // persist immediately so it survives re-open
  }, []);

  if (Platform.OS !== 'android' || !vibeServerSupported) {
    return (
      <SafeAreaView style={[styles.root, { backgroundColor: C.bg }]}>
        <ScrollView contentContainerStyle={{ padding: 18 }}>
          <Text style={[styles.h1, { color: C.amber, fontFamily: F }]}>Server mode</Text>
          <Text style={[styles.sub, { color: C.textDim, fontFamily: F }]}>
            Sharing a local USB dongle is only available on Android.
          </Text>
          <TouchableOpacity style={[styles.stopBtn, { borderColor: C.border }]} onPress={() => navigation.goBack()}>
            <Text style={{ color: C.gold, fontFamily: F, fontSize: 16 }}>‹ Back</Text>
          </TouchableOpacity>
        </ScrollView>
      </SafeAreaView>
    );
  }

  // ── Running view (live telemetry) ─────────────────────────────────────────
  if (running) {
    const spec = status?.specBytesPerSec ?? 0;
    const aud = status?.audioBytesPerSec ?? 0;
    const client = status?.client;
    return (
      <SafeAreaView style={[styles.root, { backgroundColor: C.bg }]}>
        <ScrollView contentContainerStyle={{ padding: 18, paddingBottom: 40 }}>
          <Text style={[styles.h1, { color: C.amber, fontFamily: F }]}>VibeServer</Text>
          <Text style={[styles.sub, { color: C.textDim, fontFamily: F }]}>
            {`Serving this phone's ${radio?.model ?? 'SDR'} with server-side DSP.`} Leaving this screen
            stops the server and frees the dongle.
          </Text>

          <View style={[styles.card, { borderColor: C.borderBright }]}>
            {/* BOTH WAYS IN. The IP is the one that always works; the NAME is the one a
                human can actually retype, and it survives the router handing this phone a
                different address tomorrow — which the IP does not. Showing only the IP
                hid the better of the two. */}
            <Row C={C} F={F} k="ADDRESS" v={`${running.ip}:${running.port}`} vc={C.amber} />
            {!!mdnsHost && (
              <Row C={C} F={F} k="NAME" v={`${mdnsHost}.local:${running.port}`} vc={C.amber} />
            )}
            <Row C={C} F={F} k="ACCESS" v={effectivePin ? `PIN ${effectivePin}` : 'Open (no PIN)'} vc={effectivePin ? C.green : C.amber} />
            <Row C={C} F={F} k="STATUS"
              v={client ? (status?.clientAddr ? `Connected: ${status.clientAddr}` : 'Client connected') : 'Waiting for a client…'}
              vc={client ? C.green : C.goldDim} />
            <Row C={C} F={F} k="WATERFALL" v={`${fmtRate(spec)}`} vc={client ? C.amber : C.goldDim} />
            <Row C={C} F={F} k="AUDIO" v={`${fmtRate(aud)}${status?.compressed ? '' : ' (raw)'}`} vc={client ? C.amber : C.goldDim} />
            {/* The client drives the capture rate (and the frame rate) live. Showing
                them here is how the HOST sees the server answering the client —
                otherwise a remote change is invisible from this end. */}
            <Row C={C} F={F} k="SAMPLE RATE"
              v={status?.sampleRate ? `${(status.sampleRate / 1e6).toFixed(3).replace(/0+$/, '').replace(/\.$/, '')} MS/s` : '—'}
              vc={client ? C.amber : C.goldDim} />
            <Row C={C} F={F} k="FRAME RATE"
              v={status?.fftRate ? `${Math.round(status.fftRate)} fps` : '—'}
              vc={client ? C.amber : C.goldDim} />
            {/* CPU — a phone serving on a shelf for hours invites "what is this costing me",
                and the honest answer beats guessing from how warm it feels. Percent of ONE
                core, so it stays comparable with the DSP benchmark figures; the core count is
                shown alongside so 120% reads as "1.2 of 8" rather than as an error. */}
            <Row C={C} F={F} k="CPU"
              v={status?.cpu ? `${status.cpu.toFixed(0)}% of 1 core${status?.cores ? ` (of ${status.cores})` : ''}` : '—'}
              vc={client ? C.amber : C.goldDim} />
          </View>

          <Text style={[styles.hint, { color: C.textDim, fontFamily: F }]}>
            The PIN protects tuning control — it is not audio encryption. Use a VPN
            for privacy on untrusted networks.
          </Text>

          {/* Live compressed-audio fallback toggle */}
          <View style={[styles.card, { borderColor: C.border, marginTop: 14 }]}>
            <View style={styles.rowBetween}>
              <Text style={[styles.value, { color: C.amber, fontFamily: F, flex: 1, paddingRight: 12 }]}>
                Compressed audio
              </Text>
              <Switch value={compress} onValueChange={toggleCompress}
                trackColor={{ false: C.border, true: C.green }} thumbColor={C.amber} />
            </View>
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 8 }]}>
              Turn off only if a client has audio trouble (falls back to raw PCM).
            </Text>
          </View>

          <TouchableOpacity style={[styles.stopBtn, { borderColor: C.red }]} onPress={stopAndBack}>
            <Text style={{ color: C.red, fontFamily: F, fontSize: 16 }}>■ Stop server & back to servers</Text>
          </TouchableOpacity>
        </ScrollView>
      </SafeAreaView>
    );
  }

  // ── Config view ───────────────────────────────────────────────────────────
  return (
    <SafeAreaView style={[styles.root, { backgroundColor: C.bg }]}>
      <ScrollView contentContainerStyle={{ padding: 18, paddingBottom: 40 }}>
        <Text style={[styles.h1, { color: C.amber, fontFamily: F }]}>Server mode</Text>
        <Text style={[styles.sub, { color: C.textDim, fontFamily: F }]}>
          {`Share this phone's ${radio?.model ?? 'SDR'} over your network.`}
        </Text>

        {/* Protocol picker */}
        <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>PROTOCOL</Text>
        <ProtoCard C={C} F={F} active={proto === 'vibeserver'} onPress={() => setProto('vibeserver')}
          title="VibeServer" tag="Recommended"
          desc="More secure, less data. Server-side DSP, compressed audio + waterfall, PIN protected." />
        <ProtoCard C={C} F={F} active={proto === 'rtltcp'} onPress={() => setProto('rtltcp')}
          title="RTL-TCP" tag="Compatible"
          desc="Raw IQ, maximum compatibility. Needs a fast, stable network. No PIN." />

        {/* Shared: advertised name */}
        <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>ADVERTISED NAME</Text>
        <TextInput value={name} onChangeText={setName}
          placeholder="VibeSDR" placeholderTextColor={C.goldDim}
          style={[styles.input, { color: C.amber, borderColor: C.border, fontFamily: F }]} />

        {/* Shared: auto-discovery */}
        <View style={[styles.card, { borderColor: C.border, marginTop: 14 }]}>
          <View style={styles.rowBetween}>
            <Text style={[styles.value, { color: C.amber, fontFamily: F, flex: 1, paddingRight: 12 }]}>
              Server auto-discovery
            </Text>
            <Switch value={advertise} onValueChange={setAdvertise}
              trackColor={{ false: C.border, true: C.green }} thumbColor={C.amber} />
          </View>
          <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 8 }]}>
            {advertise
              ? 'This server appears automatically on other VibeSDR devices on the network.'
              : "Hidden — clients must enter this phone's address by hand. Good on a public hotspot" +
                (proto === 'rtltcp' ? ' (RTL-TCP has no PIN).' : '.')}
          </Text>
        </View>

        {proto === 'vibeserver' ? (
          <>
            {/* ★★ SECURITY — the PIN and the admin password TOGETHER, because they are two
                secrets with two different jobs and having them in separate parts of the screen
                invited the reading that one replaces the other (Stuart, 2026-07-27).
                The distinction in one line each: the PIN is the DOOR, the password is the
                CONTROLS INSIDE. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>SECURITY</Text>
            <View style={[styles.card, { borderColor: C.border }]}>
              <Text style={[styles.value, { color: C.amber, fontFamily: F, fontSize: 14 }]}>
                Two separate protections
              </Text>
              <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 8 }]}>
                <Text style={{ color: C.gold }}>PIN — protects the CONNECTION.</Text>{'\n'}
                Decides who may connect and listen at all. Without it, anyone who can reach this
                server can use the radio.{'\n\n'}
                <Text style={{ color: C.gold }}>Password — protects the SERVER and HARDWARE.</Text>{'\n'}
                Anyone already listening can still tune and set the gain — that is what a
                receiver is for. The password guards the few settings that can damage equipment
                or leave the radio broken for the next person: bias-T, direct sampling and
                calibration.{'\n\n'}
                They are independent on purpose. A public receiver has NO PIN so everyone can
                listen, and a password so no visitor can put DC on the feedline.
              </Text>
            </View>

            {/* PIN */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>PIN — WHO MAY CONNECT</Text>
            <View style={styles.pillRow}>
              {(['random', 'custom', 'off'] as PinMode[]).map(m => (
                <Pill key={m} C={C} F={F} active={pinMode === m}
                  label={m === 'random' ? 'Random' : m === 'custom' ? 'Custom' : 'No PIN'}
                  onPress={() => setPinMode(m)} />
              ))}
            </View>
            {pinMode !== 'off' && (
              <View style={styles.rowBetween}>
                <TextInput value={pin}
                  onChangeText={t => { setPin(t.replace(/[^0-9]/g, '').slice(0, 12)); if (pinMode === 'random') setPinMode('custom'); }}
                  editable={pinMode === 'custom'}
                  keyboardType="number-pad"
                  style={[styles.input, { flex: 1, color: C.amber, borderColor: C.border, fontFamily: F }]} />
                {pinMode === 'random' && (
                  <TouchableOpacity onPress={regenPin} style={[styles.regen, { borderColor: C.border }]}>
                    <Text style={{ color: C.gold, fontFamily: F, fontSize: 18 }}>↻</Text>
                  </TouchableOpacity>
                )}
              </View>
            )}
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F }]}>
              {pinMode === 'off'
                ? 'Anyone on the network can connect and tune. Use only on a trusted LAN.'
                : 'Clients enter this PIN once. It authenticates control without ever crossing the wire (HMAC challenge-response).'}
            </Text>

            {/* Bookmarks. The server LEARNS stations from RDS as clients tune, so the
                list grows on its own — which means it also needs a way to be emptied,
                and a way to be seeded from a list you already have. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>BOOKMARKS</Text>
            <View style={[styles.card, { borderColor: C.border }]}>
              <Text style={[styles.hint, { color: C.textDim, fontFamily: F }]}>
                {bmCount === null
                  ? 'Stations this receiver hears are added automatically and shared with every client.'
                  : `${bmCount} station${bmCount === 1 ? '' : 's'} — learned from RDS as clients tune, plus anything imported. Shared with every client.`}
              </Text>
              <View style={[styles.rowBetween, { marginTop: 10 }]}>
                <TouchableOpacity onPress={onImportBookmarks}
                  style={[styles.regen, { borderColor: C.border, flex: 1, marginRight: 8, alignItems: 'center' }]}>
                  <Text style={{ color: C.gold, fontFamily: F, fontSize: 13 }}>IMPORT LIST</Text>
                </TouchableOpacity>
                <TouchableOpacity onPress={onResetBookmarks}
                  style={[styles.regen, { borderColor: C.border, flex: 1, alignItems: 'center' }]}>
                  <Text style={{ color: C.amber, fontFamily: F, fontSize: 13 }}>RESET</Text>
                </TouchableOpacity>
              </View>
              {bmMsg ? (
                <Text style={[styles.hint, { color: C.amber, fontFamily: F, marginTop: 8 }]}>{bmMsg}</Text>
              ) : null}
            </View>

            {/* Crash recovery. The foreground service is already START_STICKY, so Android
                brings the SERVICE back by itself — but the radio died with the process,
                so without rebuilding it you'd be left with a notification claiming the
                server is up and nothing behind it. Switchable, because a shim that
                crashes REPEATEDLY would otherwise crash-loop. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>RECOVERY</Text>
            <View style={[styles.card, { borderColor: C.border }]}>
              <View style={styles.rowBetween}>
                <Text style={[styles.value, { color: C.amber, fontFamily: F, flex: 1, paddingRight: 12 }]}>
                  Restart if it crashes
                </Text>
                <Switch value={autoRestore} onValueChange={setAutoRestore}
                  trackColor={{ false: C.border, true: C.green }} thumbColor={C.amber} />
              </View>
              <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 8 }]}>
                {autoRestore
                  ? 'If the app is killed while serving, the server rebuilds itself and carries on. The dongle is never unplugged, so nothing is lost.'
                  : 'A crash stops the server for good until you start it again. Turn this on for a receiver left running unattended.'}
              </Text>
              <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 8 }]}>
                This does not survive a REBOOT — Android will not detect a dongle that was
                plugged in while the phone was off, and no app can change that. Replug it.
              </Text>
              {/* ★★ LEAVING IT UP FOR WEEKS IS A DIFFERENT PROBLEM FROM SURVIVING A CRASH, and the
                  phone is the thing that breaks it. The start-up check asks for background usage,
                  which covers being throttled — it does NOT cover the phone deciding to reboot on a
                  schedule. Samsung ships "Auto restart" ON by default (every few days, ~3am), and a
                  reboot is exactly the case the line above says we cannot recover from: the dongle
                  needs replugging by hand. Somebody running a public receiver will otherwise find
                  it dead every few days with no idea why. */}
              <Text style={[styles.hint, { color: C.amber, fontFamily: F, marginTop: 10 }]}>
                Leaving this running long term? Turn OFF battery optimisation for VibeSDR, and turn
                OFF any scheduled auto-restart — Samsung phones restart every few days by default,
                and a reboot needs the dongle replugged by hand.
              </Text>
            </View>

            {/* Web server. Turning this OFF means a browser gets nothing — only the
                VibeSDR app can connect. It's the blunt lock for a server you don't
                want a stranger stumbling into via a URL. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>WEB CLIENT</Text>
            <View style={[styles.card, { borderColor: C.border }]}>
              <View style={styles.rowBetween}>
                <Text style={[styles.value, { color: C.amber, fontFamily: F, flex: 1, paddingRight: 12 }]}>
                  Serve the web client
                </Text>
                <Switch value={webServer} onValueChange={setWebServer}
                  trackColor={{ false: C.border, true: C.green }} thumbColor={C.amber} />
              </View>
              <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 8 }]}>
                {webServer
                  ? 'Anyone on the network can open this server in a browser (the PIN still applies).'
                  : 'Browsers get nothing — only the VibeSDR app can connect.'}
              </Text>
            </View>

            {/* Bandwidth (sample rate) */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>BANDWIDTH</Text>
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginBottom: 8 }]}>
              {rate === 0
                ? `Clients choose their own span, up to the full ${topRateLabel}.`
                : 'Pinned — clients cannot change the span. Lower it to save processing power on a low-end phone.'}
            </Text>
            {rateOptions.map(o => (
              <OptRow key={o.value} C={C} F={F} active={rate === o.value} label={o.label} onPress={() => setRate(o.value)} />
            ))}

            {/* Receiver location. A SEPARATE consent from the app's own location
                permission: granting location to sort the instance list by distance is
                not consent to BROADCAST that position to every client. So this is
                opt-in, and 'off' is the default. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>RECEIVER LOCATION</Text>
            <View style={styles.pillRow}>
              {(['off', 'device', 'manual'] as LocationMode[]).map(m => (
                <Pill key={m} C={C} F={F} active={locMode === m}
                  label={m === 'off' ? 'Not set' : m === 'device' ? 'Use device' : 'Enter city'}
                  onPress={() => setLocMode(m)} />
              ))}
            </View>
            {locMode === 'manual' && (
              <TextInput value={locCity} onChangeText={setLocCity}
                placeholder="Town, or grid locator (Northampton / IO92nh)" placeholderTextColor={C.textDim}
                style={[styles.input, { color: C.amber, borderColor: C.border, fontFamily: F }]} />
            )}
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F }]}>
              {locMode === 'off'
                ? 'No location is published. Clients show "receiver location not set" and go without spot distances, map centring and the regional band plan.'
                : locMode === 'device'
                ? "This phone's coarse position (~1 km) is published to every client that connects."
                : 'A town or city needs an internet connection when you press Start (looked up once, then stored). A Maidenhead locator works OFFLINE — use it if this server has no internet. Published to every client; set it if the receiver lives somewhere other than where you are.'}
            </Text>
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
              Distances and band edges are properties of the ANTENNA, not the listener —
              80m is 3.5–3.8 MHz in Region 1 but 3.5–4.0 in Region 2.
            </Text>

            {/* Waterfall frame rate */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>WATERFALL RATE</Text>
            {FPS_TIERS.map(t => (
              <OptRow key={t.key} C={C} F={F} active={fps === t.key} label={t.label} onPress={() => setFps(t.key)} />
            ))}

            {/* Compressed audio. ★ The codec is OPUS now — this said IMA-ADPCM long after
                Opus replaced it, which is worse than saying nothing: it names the wrong
                format to anyone deciding whether their client can cope. */}
            <View style={[styles.card, { borderColor: C.border, marginTop: 14 }]}>
              <View style={styles.rowBetween}>
                <Text style={[styles.value, { color: C.amber, fontFamily: F, flex: 1, paddingRight: 12 }]}>
                  Compressed audio
                </Text>
                <Switch value={compress} onValueChange={setCompress}
                  trackColor={{ false: C.border, true: C.green }} thumbColor={C.amber} />
              </View>
              <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 8 }]}>
                Opus — about 20x lighter than raw audio. Turn off only if a listener's
                client cannot decode it.
              </Text>
            </View>

            {/* ★★ UNCOMPRESSED AUDIO — the owner's UPLINK policy, three-way.
                Raw is 48 kHz stereo int16: ~187 KB/s per listener, which is why this is not
                a plain switch. Hans asked for it after hearing Opus on a good system. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>UNCOMPRESSED AUDIO</Text>
            {([
              [0, 'Off', 'Never send raw audio. Listeners who cannot decode Opus are turned away with a reason.'],
              [1, "Listener's choice", 'An UNCOMPRESSED button appears in each listener\u2019s audio menu. Defaults to Opus.'],
              [2, 'Compatibility only', 'Raw only as a fallback for a client that cannot decode Opus — no user-facing switch.'],
            ] as const).map(([v, label, hint]) => (
              <OptRow key={v} C={C} F={F} active={uncomp === v} label={label} hint={hint}
                onPress={() => { setUncomp(v as 0 | 1 | 2); AsyncStorage.setItem(K.uncomp, String(v));
                                 if (runningRef.current) setVibeServerUncompressedAudio(v as 0 | 1 | 2); }} />
            ))}
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
              This phone always gets uncompressed audio from its own server — the setting rations
              your UPLINK, and listening on the same device never touches it.
            </Text>

            {/* ★★ TIME LIMIT — only earns its place on a PUBLIC receiver. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>TIME LIMIT PER LISTENER</Text>
            {([[0, 'Unlimited'], [15, '15 minutes'], [30, '30 minutes'],
               [60, '1 hour'], [120, '2 hours']] as const).map(([v, label]) => (
              <OptRow key={v} C={C} F={F} active={limitMin === v} label={label}
                onPress={() => { setLimitMin(v); AsyncStorage.setItem(K.limitMin, String(v));
                                 if (runningRef.current) setVibeServerSessionLimit(v); }} />
            ))}
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
              For a receiver you have put on the internet. This server serves ONE listener at a
              time, so without a limit the first person to connect can hold it all evening and
              everyone else just sees IN USE.{'\n\n'}
              A listener is warned at two minutes and again at thirty seconds, then disconnected
              with an explanation. Their address is then held off for two minutes — otherwise
              their client would simply reconnect and carry on, and the limit would achieve
              nothing.{'\n\n'}
              You are not affected: listening on this phone is exempt, and so is any session
              unlocked with the admin password. Leave it Unlimited for a private receiver.
            </Text>

            {/* ★★ ADMIN PASSWORD — control, not access. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>PASSWORD — WHO MAY CHANGE THINGS</Text>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
              <TextInput value={adminPw}
                onChangeText={(v) => { setAdminPw(v); AsyncStorage.setItem(K.adminPw, v);
                                       if (runningRef.current) setVibeServerAdminSecret(v); }}
                placeholder="none — controls are open" placeholderTextColor={C.goldDim}
                autoCapitalize="none" autoCorrect={false}
                // ★ Dots by default: this gets typed at a rally or a club night with people
                // stood behind you (Stuart, 2026-07-27).
                secureTextEntry={!showAdminPw}
                style={[styles.input, { color: C.amber, borderColor: C.border, fontFamily: F, flex: 1 }]} />
              <TouchableOpacity onPress={() => setShowAdminPw(v => !v)}
                style={[styles.card, { borderColor: C.border, paddingVertical: 10, paddingHorizontal: 12 }]}>
                <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12 }}>
                  {showAdminPw ? 'HIDE' : 'SHOW'}
                </Text>
              </TouchableOpacity>
            </View>
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
              Protects the settings that can damage hardware or spoil the band for everyone —
              bias-T, direct sampling, frequency calibration. Separate from the listening PIN:
              a receiver can be open to every listener and still refuse a visitor putting DC on
              the feedline. Leave empty and nothing is protected.
            </Text>

            {/* ★★★ WARN WHILE SERVING, AND ONLY THEN. The phone is a SIMPLE-mode server: plug in
                and share, which is the behaviour people like, so a password is NOT forced here.
                But an owner who never opened this screen met the consequence as a stranger's
                demand for a password they had never set (issue #19, on the Mac, where one was
                being generated silently). Nothing is generated anywhere now — so the honest thing
                is to say plainly, at the moment it is true, that this receiver is unprotected.
                ★ Advice, not an error: it is a perfectly reasonable way to run a radio on your own
                  network, and the colour says "read this", not "you did something wrong". */}
            {running && !adminPw.trim() && (
              <View style={[styles.card, { borderColor: C.goldDim, marginTop: 14 }]}>
                <Text style={{ color: C.amber, fontFamily: F, fontSize: 13 }}>
                  No admin password set
                </Text>
                <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
                  Recommended if you are on a PUBLIC NETWORK, or intend to port forward this and
                  serve it to the internet — anyone who can reach the server can otherwise change
                  the gain, switch on the bias-T or alter the calibration. On your own home network
                  that is usually nobody but you, and no password is needed.{'\n\n'}
                  A listening PIN above is the other half: the PIN decides who may LISTEN, the
                  password decides who may CHANGE the radio.
                </Text>
              </View>
            )}

            {error && (
              <View style={[styles.card, { borderColor: C.red, marginTop: 14 }]}>
                <Text style={{ color: C.red, fontFamily: F, fontSize: 14 }}>{error}</Text>
              </View>
            )}

            {/* ★ SAY THERE IS NO SAVE BUTTON. Every setting here persists the moment it is
                changed and START applies them — but a settings screen with no Save invites the
                reasonable worry that nothing was kept (Stuart: "there is no save button but I
                assumed pressing start server was the save button", 2026-07-27). He was right,
                and having to assume is the problem. */}
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 16, textAlign: 'center' }]}>
              Settings are saved as you change them. Starting the server applies them.
            </Text>
            <TouchableOpacity style={[styles.startBtn, { borderColor: C.green, backgroundColor: C.green + '18' }]}
              onPress={start} disabled={starting}>
              {starting
                ? <ActivityIndicator color={C.green} />
                : <Text style={{ color: C.green, fontFamily: F, fontSize: 16 }}>▶ Start VibeServer</Text>}
            </TouchableOpacity>
          </>
        ) : (
          <TouchableOpacity style={[styles.startBtn, { borderColor: C.amber, backgroundColor: C.amber + '18' }]}
            onPress={() => navigation.replace('RtlTcpServer', { name: name.trim() || 'VibeSDR RTL-SDR', advertise })}>
            <Text style={{ color: C.amber, fontFamily: F, fontSize: 16 }}>▶ Start RTL-TCP server</Text>
          </TouchableOpacity>
        )}

        <TouchableOpacity style={[styles.stopBtn, { borderColor: C.border }]} onPress={() => navigation.goBack()}>
          <Text style={{ color: C.gold, fontFamily: F, fontSize: 15 }}>‹ Cancel</Text>
        </TouchableOpacity>
      </ScrollView>
    </SafeAreaView>
  );
}

function Row({ C, F, k, v, vc }: any) {
  return (
    <View style={styles.rowBetween}>
      <Text style={[styles.label, { color: C.textDim, fontFamily: F }]}>{k}</Text>
      <Text style={[styles.value, { color: vc, fontFamily: F }]}>{v}</Text>
    </View>
  );
}

function ProtoCard({ C, F, active, onPress, title, tag, desc }: any) {
  return (
    <TouchableOpacity onPress={onPress}
      style={[styles.protoCard, { borderColor: active ? C.amber : C.border, backgroundColor: active ? C.amber + '14' : 'transparent' }]}>
      <View style={styles.rowBetween}>
        <Text style={{ color: active ? C.amber : C.gold, fontFamily: F, fontSize: 17 }}>{title}</Text>
        <Text style={{ color: active ? C.amber : C.goldDim, fontFamily: F, fontSize: 11 }}>{tag}</Text>
      </View>
      <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12, lineHeight: 16, marginTop: 6 }}>{desc}</Text>
    </TouchableOpacity>
  );
}

function Pill({ C, F, active, label, onPress }: any) {
  return (
    <TouchableOpacity onPress={onPress}
      style={[styles.pill, { borderColor: active ? C.amber : C.border, backgroundColor: active ? C.amber + '22' : 'transparent' }]}>
      <Text style={{ color: active ? C.amber : C.gold, fontFamily: F, fontSize: 14 }}>{label}</Text>
    </TouchableOpacity>
  );
}

function OptRow({ C, F, active, label, hint, onPress }: any) {
  return (
    <TouchableOpacity style={[styles.optRow, { borderColor: active ? C.amber : C.border }]} onPress={onPress}>
      {/* ★ The label column must FLEX and the tick stay fixed — a three-way policy choice
          needs a sentence to be choosable at all, and without the flex a long hint pushes
          the tick off the row. */}
      <View style={{ flex: 1, paddingRight: 10 }}>
        <Text style={{ color: active ? C.amber : C.gold, fontFamily: F, fontSize: 15 }}>{label}</Text>
        {!!hint && (
          <Text style={{ color: C.textDim, fontFamily: F, fontSize: 11, marginTop: 3, lineHeight: 15 }}>
            {hint}
          </Text>
        )}
      </View>
      {active && <Text style={{ color: C.amber, fontFamily: F }}>✓</Text>}
    </TouchableOpacity>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1 },
  h1: { fontSize: 24, marginBottom: 6 },
  sub: { fontSize: 13, lineHeight: 18, marginBottom: 16 },
  card: { borderWidth: 1, borderRadius: 10, padding: 14, marginBottom: 8, gap: 10 },
  protoCard: { borderWidth: 1, borderRadius: 10, padding: 14, marginBottom: 10 },
  rowBetween: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  label: { fontSize: 11, letterSpacing: 1 },
  value: { fontSize: 15, flexShrink: 1, textAlign: 'right', marginLeft: 8 },
  section: { fontSize: 11, letterSpacing: 1, marginTop: 16, marginBottom: 6 },
  input: { borderWidth: 1, borderRadius: 8, paddingHorizontal: 12, paddingVertical: 10, fontSize: 16 },
  regen: { borderWidth: 1, borderRadius: 8, paddingHorizontal: 14, paddingVertical: 8, marginLeft: 8 },
  hint: { fontSize: 11.5, lineHeight: 16, marginTop: 6 },
  pillRow: { flexDirection: 'row', gap: 8, marginBottom: 8 },
  pill: { borderWidth: 1, borderRadius: 8, paddingHorizontal: 16, paddingVertical: 9 },
  optRow: {
    borderWidth: 1, borderRadius: 8, paddingHorizontal: 14, paddingVertical: 12,
    marginBottom: 8, flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center',
  },
  startBtn: { borderWidth: 1, borderRadius: 10, paddingVertical: 15, alignItems: 'center', marginTop: 20 },
  stopBtn: { borderWidth: 1, borderRadius: 10, paddingVertical: 14, alignItems: 'center', marginTop: 12 },
});
