import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  View, Text, TextInput, TouchableOpacity, ScrollView, ActivityIndicator, StyleSheet, Alert,
  PermissionsAndroid, Platform, Switch,
} from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import type { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { themeFor, Colors } from '../constants/theme';
import { useGlassTheme } from '../components/PopupShell';
import { meaningText } from '../constants/decoderTokens';
import {
  startRtlTcpServer, stopRtlTcpServer, getServerStatus,
  getServerName, saveServerName,
  getServerPersist, saveServerPersist,
  type ServerInfo, type ServerStatus,
} from '../services/rtlTcpServer';
import { advertiseRtlTcp, stopAdvertiseRtlTcp } from '../services/mdns';
import ServerKey, { useKeyList } from '../components/ServerKey';   // ★★★ beta96 its own short setup screen
import { setChoice } from '../services/serverChoiceStore';

type Props = NativeStackScreenProps<RootStackParamList, 'RtlTcpServer'>;

// The RTL-TCP server control screen.
//
// Ad-hoc mode (default): owns the full lifecycle — starts the server + mDNS
// advert on mount, tears both down on unmount, so leaving frees the dongle.
//
// Persist mode ("keep sharing"): the SERVER owns its own lifecycle, held up by
// the foreground service. The screen starts it if it isn't running, ADOPTS it if
// it is, and never stops it on unmount — only the explicit STOP button does.
// This is what a set-and-forget node needs (phone + dongle at an antenna base).
export default function RtlTcpServerScreen({ navigation, route }: Props) {
  const { font: F } = themeFor();
  /* ★★ THE USER'S COLOURS HERE TOO (Stuart, 2026-10-12: "yes colours apply everywhere including directory if
   *  possible") — a screen of the directory's. Text in the TEXT colour, keys / selectors / borders in the
   *  CONTROLS colour (useGlassTheme: the glass skin whatever the chassis — this screen is never plated), meanings
   *  by the clash rule (meaningsFor); the link's green, the STOP key's red and the dark background stay fixed. */
  const pt = useGlassTheme();
  const C = useMemo(() => ({
    bg: Colors.bg, green: Colors.green, red: Colors.red,
    text: pt.ink.text, value: pt.gold.value, textDim: pt.ink.textDim, textFaint: pt.ink.textFaint,
    key: pt.gold.sel, keyIdle: pt.ink.key, keyFill: pt.gold.fill,
    border: pt.ink.border, borderBright: pt.gold.selBorder,
    bad: pt.ink.m.bad, warn: pt.ink.m.warn, m: pt.ink.m,
  }), [pt]);
  // The Server-mode picker owns the shared "auto-discovery" toggle; honour it
  // here (default on for a direct entry / legacy callers).
  const doAdvertise = route.params?.advertise !== false;
  const adv = useCallback((n: string, p: number) => { if (doAdvertise) advertiseRtlTcp(n, p); }, [doAdvertise]);
  const [name, setName]       = useState(route.params?.name ?? 'VibeSDR RTL-SDR');
  const [info, setInfo]       = useState<ServerInfo | null>(null);
  const [status, setStatus]   = useState<ServerStatus | null>(null);
  const [starting, setStarting] = useState(true);
  const [error, setError]     = useState<string | null>(null);
  const [dropping, setDropping] = useState(false);   // drops grew since the last poll
  const [persist, setPersist] = useState(false);     // keep sharing after leaving
  const startedRef = useRef(false);
  const lastDropRef = useRef(0);
  const lastIqRef = useRef(-1);
  const [iqStalled, setIqStalled] = useState(false);
  // The unmount cleanup closes over the first render, so it can't read `persist`
  // state. Mirror it into a ref that the cleanup can see.
  const persistRef = useRef(false);
  /* ★★★ beta96 RTL-TCP IS ITS OWN ENTRY ("Use as a server" → RTL-TCP), so it has its own short screen first: the name
   *  and START. Opened with `setup` (from the entry) and nothing already running; any other way in starts at once, as
   *  it always did (the Full screen's protocol path, a running server adopted). */
  const [setupPhase, setSetupPhase] = useState(false);

  // Start the server (once) + advertise — or ADOPT one already running (persist
  // mode left it up when the screen was last closed). Starting a second server on
  // the same dongle would fail on the USB open, so adopt must come first.
  useEffect(() => {
    if (startedRef.current) return;
    startedRef.current = true;
    let cancelled = false;
    (async () => {
      const persisted = await getServerPersist();
      if (cancelled) return;
      setPersist(persisted);
      persistRef.current = persisted;

      const existing = await getServerStatus();
      if (cancelled) return;
      if (existing?.running && existing.port > 0) {
        const adoptedName = await getServerName(route.params?.name ?? 'VibeSDR RTL-SDR');
        if (cancelled) return;
        setName(adoptedName);
        setInfo({ ip: existing.ip || '0.0.0.0', port: existing.port, name: adoptedName });
        setStatus(existing);
        lastDropRef.current = existing.drops ?? 0;
        setStarting(false);
        adv(adoptedName, existing.port);   // idempotent; IP may have changed
        return;
      }

      const initialName = await getServerName(route.params?.name ?? 'VibeSDR RTL-SDR');
      if (cancelled) return;
      setName(initialName);
      if ((route.params as any)?.setup) { setStarting(false); setSetupPhase(true); return; }
      await beginRef.current(initialName, () => cancelled);
    })();
    return () => { cancelled = true; };
  }, []);

  /** The start itself — from the effect above, or from the short setup screen's START. */
  const beginRef = useRef(async (initialName: string, isCancelled: () => boolean) => {
    {
      if (isCancelled()) return;
      // Android 13+: the FGS notification is suppressed without POST_NOTIFICATIONS.
      if (Platform.OS === 'android' && Platform.Version >= 33) {
        try { await PermissionsAndroid.request(PermissionsAndroid.PERMISSIONS.POST_NOTIFICATIONS); } catch {}
      }
      if (isCancelled()) return;
      try {
        const res = await startRtlTcpServer({ name: initialName });
        if (isCancelled()) return;
        setInfo(res);
        setStarting(false);
        adv(res.name, res.port);
      } catch (e: any) {
        if (isCancelled()) return;
        setStarting(false);
        // ★★ WRONG UNTIL 2026-11-25: this said an Airspy HF+ "shares over rtl_tcp too". It never did — the
        //    server opened it through librtlsdr, sent the header and then no IQ at all (Sloper). RTL-TCP is
        //    an RTL-SDR's; the native start now refuses anything else and its message says what to use.
        setError(e?.message ?? 'Could not start the server. Is a radio plugged in via USB OTG?');
      }
    }
  });

  // Teardown on unmount (any way of leaving the screen) — UNLESS persist mode is
  // on, in which case the server outlives the screen and only the explicit STOP
  // button ends it. Read the ref, not the state: this cleanup runs once, closed
  // over the first render's values.
  useEffect(() => () => {
    if (persistRef.current) return;   // keep sharing; FGS holds it up
    stopAdvertiseRtlTcp();
    stopRtlTcpServer();
  }, []);

  // Poll status for the live readout (client connected / actual bandwidth).
  useEffect(() => {
    if (starting || error) return;
    const t = setInterval(async () => {
      const s = await getServerStatus();
      if (!s) return;
      const dropped = s.drops ?? 0;
      setDropping(dropped > lastDropRef.current);
      lastDropRef.current = dropped;
      // ★ IQ FLOWING is its own test — "nothing dropped" alone read "Good" for a client getting nothing.
      const iq = s.iqBytes;
      setIqStalled(typeof iq === 'number' && iq === lastIqRef.current);
      lastIqRef.current = iq ?? -1;
      setStatus(s);
    }, 2000);
    return () => clearInterval(t);
  }, [starting, error]);

  const applyPersist = useCallback((on: boolean) => {
    setPersist(on);
    persistRef.current = on;
    saveServerPersist(on);
  }, []);


  const commitName = useCallback(() => {
    const n = name.trim() || 'VibeSDR RTL-SDR';
    setName(n);
    saveServerName(n);
    if (info) adv(n, info.port);
  }, [name, info]);

  const stopAndBack = useCallback(() => {
    stopAdvertiseRtlTcp();
    stopRtlTcpServer();
    navigation.goBack();
  }, [navigation]);

  const clientLine = status?.client
    ? (status.clientAddr ? `Connected: ${status.clientAddr}` : 'Client connected')
    : 'Waiting for a client to connect…';
  const mhz = (hz: number) => hz >= 1e6 ? `${(hz / 1e6).toFixed(3).replace(/\.?0+$/, '')} MS/s` : `${(hz / 1e3).toFixed(1).replace(/\.0$/, '')} kS/s`;
  /* ★ THE CLIENT'S RATE, and the radio's when it differs: the radio runs the next rate it has and the stream is resampled
   *  to exactly what the client asked for (LocalSdrShim::startRtlTcpDoor). */
  const rateLine = !status?.sampleRate ? '—'
    : status.clientRate && Math.abs(status.clientRate - status.sampleRate) > 1
      ? `${mhz(status.clientRate)} (radio at ${mhz(status.sampleRate)})`
      : mhz(status.sampleRate);
  /* ★★★ GAIN, AS THE DECK'S AGC CHIP SAYS IT (Stuart, 2026-11-25): "VibeAGC <dB>" while the client has asked for
   *  automatic gain on a radio that has VibeAGC, the radio's own "AGC" where it has only that, else the manual figure. */
  const gainDb = status && (status.gainTenthDb ?? -1) >= 0 ? `${((status.gainTenthDb ?? 0) / 10).toFixed(1)} dB` : '';
  const gainLine = !status ? '—'
    : status.agc ? `${status.agcName || 'AGC'}${gainDb ? ` ${gainDb}` : ''}`
    : status.driver === 'hackrf' && !gainDb ? 'Manual (this radio has no AGC)'
    : `Manual${gainDb ? ` ${gainDb}` : ''}`;

  const dropped = status?.drops ?? 0;
  /* ★★★ "Good" ONLY WHEN IQ IS MOVING (Sloper, 2026-11-25). This said Good whenever nothing was dropped, and
   *     nothing is dropped when nothing is sent: a server whose radio delivered no IQ at all showed
   *     "Connected … Good" over a client with no waterfall. */
  const noIq = !!status?.usbEnded || (!!status?.client && iqStalled);
  const linkLine = status?.usbEnded ? 'The radio stopped sending — press STOP and start again'
    : !status?.client ? '—'
    : iqStalled ? 'No IQ from the radio'
    : dropped === 0 ? 'Good'
    : `${dropped} block${dropped === 1 ? '' : 's'} dropped`;
  const linkColor = noIq ? C.bad
    : !status?.client ? C.textDim
    : dropping ? C.bad
    : dropped > 0 ? C.warn
    : C.green;

  const startFromSetup = () => {
    const n = name.trim() || 'VibeSDR RTL-SDR';
    setName(n); saveServerName(n); void setChoice('rtltcp');
    setSetupPhase(false); setStarting(true);
    void beginRef.current(n, () => false);
  };
  const toChoice = () => navigation.replace('ServerChoice' as any, { choose: true } as any);
  const setupKeys = [startFromSetup, toChoice];
  const [hl, setHl] = useKeyList(setupPhase ? setupKeys.length : 0, (i) => setupKeys[i]?.(), setupPhase ? () => navigation.goBack() : undefined);

  if (setupPhase) {
    return (
      <SafeAreaView style={[styles.root, { backgroundColor: C.bg }]}>
        <ScrollView contentContainerStyle={{ padding: 18, paddingBottom: 40 }}>
          <Text style={[styles.h1, { color: C.text, fontFamily: F }]}>RTL-TCP (Compatibility)</Text>
          <Text style={[styles.sub, { color: C.textDim, fontFamily: F }]}>
            The radio&rsquo;s raw IQ for SDR#, SDR++ or SDR Console, with VibeAGC. Local network only, one program at a
            time, no PIN. Any radio but an Icom.
          </Text>
          <ServerKey big first color={C.green} dim={C.textDim} border={C.green} fontFamily={F} focused={hl === 0}
            onFocusHere={() => setHl(0)} testID="rtltcp-start" title="▶ START RTL-TCP" onPress={startFromSetup} />
          <Text style={[styles.section, { color: C.textDim, fontFamily: F, marginTop: 18 }]}>LOCAL NAME</Text>
          <TextInput value={name} onChangeText={setName} placeholder="VibeSDR RTL-SDR" placeholderTextColor={C.textFaint}
            style={[styles.input, { color: C.value, borderColor: C.border, fontFamily: F }]} />
          <Text style={[styles.hint, { color: C.textDim, fontFamily: F }]}>Shown to programs discovering this server on the network.</Text>
          <ServerKey color={C.key} dim={C.textDim} border={C.border} fontFamily={F} focused={hl === 1}
            onFocusHere={() => setHl(1)} title="‹ Server type" sub="VibeServer Simple, Full or RTL-TCP." onPress={toChoice} />
        </ScrollView>
      </SafeAreaView>
    );
  }

  return (
    <SafeAreaView style={[styles.root, { backgroundColor: C.bg }]}>
      <ScrollView contentContainerStyle={{ padding: 18, paddingBottom: 40 }}>
        <Text style={[styles.h1, { color: C.text, fontFamily: F }]}>RTL-TCP Server</Text>
        <Text style={[styles.sub, { color: C.textDim, fontFamily: F }]}>
          {(persist
            ? "Sharing this phone's radio with SDR software on your network (SDR#, SDR++, SDR Console). Sharing continues in the background until you press STOP."
            : "Sharing this phone's radio with SDR software on your network (SDR#, SDR++, SDR Console). Leaving this screen stops the server and frees the radio for use on this device.")
           + ' Local network only: nothing outside your network can connect, and it is never listed or sent through the internet.'}
        </Text>

        {starting && (
          <View style={styles.center}>
            <ActivityIndicator color={C.key} />
            <Text style={{ color: C.textDim, fontFamily: F, marginTop: 10 }}>Starting server…</Text>
          </View>
        )}

        {error && (
          <View style={[styles.card, { borderColor: C.bad }]}>
            <Text style={{ ...meaningText(C.m, C.bad), fontFamily: F, fontSize: 14 }}>{error}</Text>
          </View>
        )}

        {info && !error && (
          <>
            {/* Live status */}
            <View style={[styles.card, { borderColor: C.borderBright }]}>
              <View style={styles.rowBetween}>
                <Text style={[styles.label, { color: C.textDim, fontFamily: F }]}>ADDRESS</Text>
                <Text style={[styles.value, { color: C.value, fontFamily: F }]}>{info.ip}:{info.port}</Text>
              </View>
              <View style={styles.rowBetween}>
                <Text style={[styles.label, { color: C.textDim, fontFamily: F }]}>SAMPLE RATE</Text>
                <Text style={[styles.value, { color: C.value, fontFamily: F }]}>{rateLine}</Text>
              </View>
              <View style={styles.rowBetween}>
                <Text style={[styles.label, { color: C.textDim, fontFamily: F }]}>GAIN</Text>
                <Text style={[styles.value, { color: C.value, fontFamily: F }]}>{gainLine}</Text>
              </View>
              <View style={styles.rowBetween}>
                <Text style={[styles.label, { color: C.textDim, fontFamily: F }]}>STATUS</Text>
                <Text style={[styles.value, { color: status?.client ? C.green : C.textDim, fontFamily: F }]}>
                  {clientLine}
                </Text>
              </View>
              <View style={styles.rowBetween}>
                <Text style={[styles.label, { color: C.textDim, fontFamily: F }]}>LINK</Text>
                <Text style={[styles.value, { color: linkColor, fontFamily: F }]}>{linkLine}</Text>
              </View>
              {dropping && (
                <Text style={[styles.hint, { ...meaningText(C.m, C.bad), fontFamily: F, marginTop: 6 }]}>
                  ⚠ The network can't keep up — choose a lower sample rate in your SDR program, or move
                  closer to the router.
                </Text>
              )}
              {(status?.refused ?? 0) > 0 && !!status?.lastRefusal && (
                <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
                  {`Turned away: ${status.lastRefusal}`}
                </Text>
              )}
            </View>

            {/* ★ LOCAL NAME here too, though RTL-TCP has no public listing: one field, one name
                across both server screens. A user switching protocol must not meet two labels for
                the same box. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>LOCAL NAME</Text>
            <TextInput
              value={name}
              onChangeText={setName}
              onBlur={commitName}
              onSubmitEditing={commitName}
              placeholder="VibeSDR RTL-SDR"
              placeholderTextColor={C.textFaint}
              style={[styles.input, { color: C.value, borderColor: C.border, fontFamily: F }]}
            />
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F }]}>
              Shown to clients discovering this server on the network.
            </Text>

            {/* Persist ("keep sharing") — off = ad-hoc share that ends with this screen. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>KEEP SHARING</Text>
            <View style={[styles.card, { borderColor: C.border }]}>
              <View style={styles.rowBetween}>
                <Text style={[styles.value, { color: C.text, fontFamily: F, flex: 1, paddingRight: 12 }]}>
                  Keep the server running
                </Text>
                <Switch
                  value={persist}
                  onValueChange={applyPersist}
                  trackColor={{ false: C.border, true: C.keyFill }}
                  thumbColor={C.key}
                />
              </View>
              <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 8 }]}>
                {persist
                  ? 'Sharing continues when you leave this screen. The radio stays busy, so this device can’t receive on it. Use STOP to end sharing.'
                  : 'Sharing stops when you leave this screen, freeing the radio for use on this device.'}
              </Text>
            </View>

          </>
        )}

        {/* Stop + back */}
        <TouchableOpacity style={[styles.stopBtn, { borderColor: C.red }]} onPress={stopAndBack}>
          <Text style={{ color: C.red, fontFamily: F, fontSize: 16 }}>
            {error ? '‹ Back to servers' : '■ Stop server & back to servers'}
          </Text>
        </TouchableOpacity>
      </ScrollView>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1 },
  h1: { fontSize: 24, marginBottom: 6 },
  sub: { fontSize: 13, lineHeight: 18, marginBottom: 16 },
  center: { alignItems: 'center', paddingVertical: 40 },
  card: { borderWidth: 1, borderRadius: 10, padding: 14, marginBottom: 16, gap: 10 },
  rowBetween: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  label: { fontSize: 11, letterSpacing: 1 },
  value: { fontSize: 15, flexShrink: 1, textAlign: 'right', marginLeft: 8 },
  section: { fontSize: 11, letterSpacing: 1, marginTop: 4, marginBottom: 6 },
  input: { borderWidth: 1, borderRadius: 8, paddingHorizontal: 12, paddingVertical: 10, fontSize: 16 },
  hint: { fontSize: 11.5, lineHeight: 16, marginTop: 6 },
  optRow: {
    borderWidth: 1, borderRadius: 8, paddingHorizontal: 14, paddingVertical: 12,
    marginBottom: 8, flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center',
  },
  stopBtn: {
    borderWidth: 1, borderRadius: 10, paddingVertical: 14, alignItems: 'center', marginTop: 20,
  },
});
