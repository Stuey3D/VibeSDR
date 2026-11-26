/**
 * ★★★ beta96 VIBESERVER SIMPLE — LOCAL NETWORK ONLY (Stuart, 2026-11-26: "This is the thing we got praise for before for
 * being simple and we need that back").
 *
 * One short screen, the spirit of the July 2026 server screen (name, capture width, waterfall rate, Start):
 *   START (first, so nobody scrolls for it) · SERVER NAME · ADDRESS ON THIS NETWORK (.local) · CAPTURE WIDTH · WATERFALL
 * Running: one status block — serving / the address to open / listeners — and the /setup line once a password exists.
 *
 * ★★ Everything else takes the server's defaults — the radio's own rates from its own list (the Full screen's, per
 *    driver; an Icom has neither width nor rate), landing where the radio starts, no background decoding — and it is
 *    LOCAL NETWORK ONLY: no listing on vibesdr.net, no community map, no PSK Reporter / WSPRnet, no ADS-B feed, no DMR ID
 *    lookups. The server enforces it (VibeServerBoot.effective / src/utils/serverChoice.ts simpleView), not this screen.
 * ★★ Settings made in Full are KEPT in the server's one store and come back when the owner returns to Full; this
 *    screen writes only its four (name, .local name, width, rate) — which Full shows too.
 * ★ D-pad: big keys, TV remote focus + the pad / keyboard bus. Back = the same as leaving (see the teardown note).
 */
import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { ActivityIndicator, Alert, NativeModules, PermissionsAndroid, Platform, ScrollView, StyleSheet, Text, View } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import AsyncStorage from '@react-native-async-storage/async-storage';
import TextInput from '../components/TvTextInput';
import ServerKey, { useKeyList } from '../components/ServerKey';
import DeviceCredentials from '../components/DeviceCredentials';
import DeviceReset from '../components/DeviceReset';
import { themeFor } from '../constants/theme';
import { serverPalette } from '../constants/serverPalette';
import type { PopupTokens } from '../constants/popupTokens';
import { NAME_KEY } from '../services/rtlTcpServer';
import {
  getConnectedRadio, getVibeServerStatus, stopVibeServer, FPS_TIERS, fpsForTier, DEFAULT_FPS_TIER,
  type FpsTier, type VibeServerStatus,
} from '../services/vibeServer';
import { icomPresence } from '../services/icomRadio';
import {
  getMdnsName, setMdnsName, setChoice, startSimpleServer, storeFacts, saveAdminPasswordOnDevice, setupAddressLine,
  adoptRemoteAdminPassword,
} from '../services/serverChoiceStore';
import { mdnsLabel, remoteSettingsLine } from '../utils/serverChoice';
import { RATE_OPTIONS_AHF, RATE_OPTIONS_ASP, RATE_OPTIONS_HRF, RATE_OPTIONS_RSP, RATE_OPTIONS_RTL } from './ServerModeScreen';

const Local: any = (NativeModules as any).VibeLocalSDR;
const isLite = !!Local?.isLite;

type Props = { navigation: any; route: any; tokens?: PopupTokens };

export default function SimpleServerScreen({ navigation, route, tokens }: Props) {
  const { font: F } = themeFor();
  const C = serverPalette(tokens);
  const [loaded, setLoaded] = useState(false);
  const [name, setName] = useState('VibeSDR');
  const [mdns, setMdns] = useState('');
  const [rate, setRate] = useState(0);
  const [fps, setFps] = useState<FpsTier>(DEFAULT_FPS_TIER);
  const [radio, setRadio] = useState<any>(null);
  const [running, setRunning] = useState<{ ip: string; port: number } | null>(null);
  const [status, setStatus] = useState<VibeServerStatus | null>(null);
  const [notSimple, setNotSimple] = useState(false);
  const [facts, setFacts] = useState<{ hasAdminPassword: boolean; remote: boolean; hasStore: boolean } | null>(null);
  const [mdnsHost, setMdnsHost] = useState('');
  const [starting, setStarting] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const runningRef = useRef(false);
  const keepRef = useRef(!!route?.params?.keepOnExit);

  useEffect(() => { adoptRemoteAdminPassword(); }, []);

  // Load: the shared name, this screen's .local name, the width and rate Full also reads; adopt a running server.
  useEffect(() => {
    let gone = false;
    (async () => {
      const [v, m, r, st, f] = await Promise.all([
        AsyncStorage.multiGet([NAME_KEY, 'vs_rate', 'vs_fps']).catch(() => [] as any),
        getMdnsName(), getConnectedRadio(), getVibeServerStatus(), storeFacts(),
      ]);
      if (gone) return;
      const g = Object.fromEntries(v as [string, string | null][]);
      if (g[NAME_KEY]) setName(String(g[NAME_KEY]));
      const rt = Number(g['vs_rate']); if (Number.isFinite(rt) && rt >= 0) setRate(rt);
      if (g['vs_fps'] === 'full' || g['vs_fps'] === 'half' || g['vs_fps'] === 'quarter') setFps(g['vs_fps']);
      setMdns(m); setRadio(r); setFacts(f);
      if (st?.running && st.port > 0) {
        setRunning({ ip: st.ip, port: st.port }); setStatus(st); runningRef.current = true;
        setNotSimple(!!f && !f.simple);   // ★ started from Full: not local-network-only until restarted here
      }
      setLoaded(true);
    })();
    return () => { gone = true; };
  }, []);

  // The status, once a second and a half while serving.
  useEffect(() => {
    if (!running) return;
    let gone = false;
    const tick = async () => {
      const st = await getVibeServerStatus();
      if (gone) return;
      if (st && !st.running) { setRunning(null); runningRef.current = false; return; }
      if (st) setStatus(st);
      if (!mdnsHost) { try { const h = await Local?.getMdnsHostname?.(); if (h && !gone) setMdnsHost(String(h)); } catch {} }
    };
    void tick();
    const t = setInterval(tick, 1500);
    return () => { gone = true; clearInterval(t); };
  }, [running, mdnsHost]);
  useEffect(() => { if (running) void storeFacts().then(setFacts); }, [running]);

  /* ★★★ LEAVING STOPS THE SERVER — unless asked not to ("Keep serving"), exactly as the Full screen does: walking
   *  away from the screen that started a radio normally means you are done with it, and a server left running with
   *  nothing on screen is how a phone sat at 3x its CPU for an hour (2026-09-02). */
  useEffect(() => () => { if (runningRef.current && !keepRef.current) { void stopVibeServer(); } }, []);

  const isIcom = icomPresence(radio) !== 'none';
  const rateOptions = useMemo(() => {
    const d = radio?.driver;
    return d === 'airspyhf' ? RATE_OPTIONS_AHF : d === 'sdrplay' ? RATE_OPTIONS_RSP : d === 'hackrf' ? RATE_OPTIONS_HRF
         : d === 'airspy' ? RATE_OPTIONS_ASP : RATE_OPTIONS_RTL;
  }, [radio]);
  const topRate = rateOptions.find((o) => o.value > 0)?.value ?? 0;

  const start = useCallback(async () => {
    setError(null);
    if (Local?.isBackgroundRestricted) {
      try {
        if (await Local.isBackgroundRestricted()) {
          const go = await new Promise<boolean>((res) => Alert.alert('Allow background usage',
            'This device restricts VibeSDR when it is not on screen, which starves the server. Open Settings → App battery usage and allow background usage, then start again.',
            [{ text: 'Open Settings', onPress: () => { Local?.openAppSettings?.(); res(false); } },
             { text: 'Start anyway', style: 'destructive', onPress: () => res(true) }], { cancelable: true, onDismiss: () => res(false) }));
          if (!go) return;
        }
      } catch { /* go on */ }
    }
    if (Platform.OS === 'android' && Platform.Version >= 33) {
      try { await PermissionsAndroid.request(PermissionsAndroid.PERMISSIONS.POST_NOTIFICATIONS); } catch {}
    }
    setStarting(true);
    const n = name.trim() || 'VibeSDR';
    try {
      await AsyncStorage.multiSet([[NAME_KEY, n], ['vs_rate', String(isIcom ? 0 : rate)], ['vs_fps', fps]]);
      await setMdnsName(mdns.trim());
      await setChoice('simple');
      if (runningRef.current) { await stopVibeServer(); runningRef.current = false; }
      const info = await startSimpleServer({ name: n, mdnsName: mdns.trim(), rate: isIcom ? 0 : rate, topRate,
                                             fps: fpsForTier(fps) });
      setRunning({ ip: info.ip, port: info.port }); runningRef.current = true; setNotSimple(false); setMdnsHost('');
    } catch (e: any) {
      setError(e?.message ?? 'Could not start VibeServer. Is a supported radio plugged in?');
    } finally { setStarting(false); }
  }, [name, mdns, rate, fps, isIcom, topRate]);

  const stop = useCallback(async () => {
    await stopVibeServer(); runningRef.current = false; setRunning(null); setStatus(null);
  }, []);
  const keepServing = useCallback(() => {
    keepRef.current = true;
    if (isLite) { Local?.minimise?.(); return; }
    navigation.navigate('InstancePicker', { noAutoConnect: true });
  }, [navigation]);
  const toFull = useCallback(async () => {
    // ★★ Simple → Full: the same server and the same store. Running, it is handed over (Full adopts it, and it stays
    //    local-network-only until it is started again from Full); stopped, Full shows these four with the rest.
    keepRef.current = true;
    navigation.replace('ServerMode', { vibeServerOnly: true, keepOnExit: !!runningRef.current });
  }, [navigation]);
  const toChoice = useCallback(() => {
    keepRef.current = !!runningRef.current;
    navigation.replace('ServerChoice', { choose: true });
  }, [navigation]);
  const back = useCallback(() => {
    if (isLite) { if (running) keepServing(); else Local?.minimise?.(); return; }
    navigation.goBack();
  }, [navigation, running, keepServing]);

  const ink = { color: C.keyAct, dim: C.textDim, border: C.borderBright, fontFamily: F };
  const credInk = { text: C.text, textDim: C.textDim, key: C.key, keyAct: C.keyAct, border: C.border, borderBright: C.borderBright,
                    placeholder: C.placeholder, bad: C.bad, ok: C.ok, fontFamily: F };

  // ── The keys, in screen order, for the pad / keyboard (fields are reached with the TV remote's own focus) ──
  const keys: Array<() => void> = running
    ? [() => { void stop(); }, keepServing, toFull, ...(notSimple ? [() => { void start(); }] : []), toChoice]
    : [() => { void start(); }, toFull, toChoice];
  const [hl, setHl] = useKeyList(keys.length, (i) => keys[i]?.(), back);
  useEffect(() => { setHl(0); }, [!!running]);

  if (!loaded) return <SafeAreaView style={[s.root, { backgroundColor: C.bg }]}><ActivityIndicator color={C.keyAct} style={{ marginTop: 60 }} /></SafeAreaView>;

  if (running) {
    const listeners = status?.listeners ?? 0, maxUsers = status?.maxUsers ?? 1;
    const host = mdnsHost || mdnsLabel(mdns || name);
    return (
      <SafeAreaView style={[s.root, { backgroundColor: C.bg }]}>
        <ScrollView contentContainerStyle={{ padding: 18, paddingBottom: 40 }}>
          <Text style={[s.h1, { color: C.text, fontFamily: F }]}>VibeServer Simple</Text>
          {/* ★ THE ONE-LINE STATUS — running, where to open it, who is on. */}
          <View style={[s.card, { borderColor: C.ok }]}>
            <Text style={{ color: C.ok, fontFamily: F, fontSize: 18 }}>● Serving on this network</Text>
            <Text style={{ color: C.text, fontFamily: F, fontSize: 20, marginTop: 8 }} testID="simple-address">
              http://{running.ip}:{running.port}
            </Text>
            <Text style={{ color: C.text, fontFamily: F, fontSize: 16, marginTop: 4 }}>{host}.local:{running.port}</Text>
            <Text style={{ color: C.textDim, fontFamily: F, fontSize: 14, marginTop: 8 }}>
              {listeners}/{maxUsers} listening{status?.radioProblem ? ' — ' + status.radioProblem : ''}
            </Text>
            <Text style={{ color: C.textDim, fontFamily: F, fontSize: 13, marginTop: 8, lineHeight: 18 }} testID="simple-setup-line">
              {remoteSettingsLine({ running, mdnsHost: host, hasAdminPassword: !!facts?.hasAdminPassword, remoteOn: !!facts?.remote })}
            </Text>
          </View>
          {notSimple && (
            <Text style={[s.hint, { color: C.warn, fontFamily: F }]}>
              This server was started from VibeServer Full, with its settings — it is not limited to this network yet.
              &ldquo;Restart as Simple&rdquo; below makes it local-network-only.
            </Text>
          )}
          <ServerKey {...ink} big first color={C.danger} border={C.danger} focused={hl === 0} onFocusHere={() => setHl(0)}
            testID="simple-stop" title="■ STOP SERVER" onPress={() => { void stop(); }} />
          <ServerKey {...ink} color={C.ok} border={C.ok} focused={hl === 1} onFocusHere={() => setHl(1)}
            title={isLite ? '▶ Keep serving — minimise' : '▶ Keep serving and go back'} onPress={keepServing} />
          <ServerKey {...ink} focused={hl === 2} onFocusHere={() => setHl(2)} testID="simple-to-full"
            title="Full settings ›" sub="The same server with every setting. It keeps running; it stays local-network-only until started from Full."
            onPress={() => { void toFull(); }} />
          {notSimple && (
            <ServerKey {...ink} focused={hl === 3} onFocusHere={() => setHl(3)} title="Restart as Simple" onPress={() => { void start(); }} />
          )}
          <ServerKey {...ink} focused={hl === keys.length - 1} onFocusHere={() => setHl(keys.length - 1)}
            title="‹ Server type" onPress={toChoice} />
          <DeviceCredentials ink={credInk} hasPassword={!!facts?.hasAdminPassword}
            onSavePassword={async (pw) => { await saveAdminPasswordOnDevice(pw); setFacts(await storeFacts()); }} />
        <DeviceReset ink={credInk} hasPassword={!!facts?.hasAdminPassword} danger={C.danger}
          onDone={() => { runningRef.current = false; navigation.replace('ServerChoice', {}); }} />
        </ScrollView>
      </SafeAreaView>
    );
  }

  const inputStyle = [s.input, { color: C.text, borderColor: C.border, fontFamily: F }];
  return (
    <SafeAreaView style={[s.root, { backgroundColor: C.bg }]}>
      <ScrollView contentContainerStyle={{ padding: 18, paddingBottom: 40 }}>
        <Text style={[s.h1, { color: C.text, fontFamily: F }]}>VibeServer Simple</Text>
        <Text style={[s.sub, { color: C.textDim, fontFamily: F }]}>
          Share {radio?.model ? 'the ' + radio.model : 'the radio'} with everyone on this Wi-Fi. Local network only.
        </Text>
        {/* ★★ START FIRST (Stuart, 2026-11-26: nobody scrolls to the bottom for it). */}
        <ServerKey {...ink} big first color={C.ok} border={C.ok} fill={C.okBg} focused={hl === 0} onFocusHere={() => setHl(0)}
          testID="simple-start" title={starting ? 'Starting…' : '▶ START SERVER'} onPress={() => { if (!starting) void start(); }} />
        {!!error && <Text style={[s.hint, { color: C.bad, fontFamily: F }]}>{error}</Text>}

        <Text style={[s.section, { color: C.section, fontFamily: F }]}>SERVER NAME</Text>
        <TextInput value={name} onChangeText={setName} placeholder="VibeSDR" placeholderTextColor={C.placeholder} style={inputStyle} />

        <Text style={[s.section, { color: C.section, fontFamily: F }]}>ADDRESS ON THIS NETWORK</Text>
        <TextInput value={mdns} onChangeText={setMdns} placeholder={mdnsLabel(name)} placeholderTextColor={C.placeholder}
          autoCapitalize="none" autoCorrect={false} style={inputStyle} />
        <Text style={[s.hint, { color: C.textDim, fontFamily: F }]}>
          Other devices can open {mdnsLabel(mdns || name)}.local. Empty = made from the server name.
        </Text>

        {!isIcom && (<>
          <Text style={[s.section, { color: C.section, fontFamily: F }]}>CAPTURE WIDTH</Text>
          <View style={s.wrap}>
            {rateOptions.map((o) => (
              <ServerKey key={o.value} {...ink} title={(rate === o.value ? '✓ ' : '') + o.label}
                color={rate === o.value ? C.keyAct : C.key} border={rate === o.value ? C.borderBright : C.border}
                style={s.chip} onPress={() => setRate(o.value)} />
            ))}
          </View>
          <Text style={[s.section, { color: C.section, fontFamily: F }]}>WATERFALL</Text>
          <View style={s.wrap}>
            {FPS_TIERS.map((t) => (
              <ServerKey key={t.key} {...ink} title={(fps === t.key ? '✓ ' : '') + t.label}
                color={fps === t.key ? C.keyAct : C.key} border={fps === t.key ? C.borderBright : C.border}
                style={s.chip} onPress={() => setFps(t.key)} />
            ))}
          </View>
        </>)}

        <Text style={[s.hint, { color: C.textDim, fontFamily: F, marginTop: 16 }]}>
          Local network only: no listing on vibesdr.net, no community map, no feeds, no background decoding.
          {facts?.hasStore ? ' Settings you made in VibeServer Full are kept for when you switch back.' : ''}
        </Text>
        <ServerKey {...ink} focused={hl === 1} onFocusHere={() => setHl(1)} testID="simple-full"
          title="Full settings ›" sub="PIN, public listing, shared ranges, decoders, feeds — and these four." onPress={() => { void toFull(); }} />
        <ServerKey {...ink} focused={hl === 2} onFocusHere={() => setHl(2)} title="‹ Server type" onPress={toChoice} />
        <DeviceCredentials ink={credInk} hasPassword={!!facts?.hasAdminPassword}
          onSavePassword={async (pw) => { await saveAdminPasswordOnDevice(pw); setFacts(await storeFacts()); }} />
        <DeviceReset ink={credInk} hasPassword={!!facts?.hasAdminPassword} danger={C.danger}
          onDone={() => { runningRef.current = false; navigation.replace('ServerChoice', {}); }} />
      </ScrollView>
    </SafeAreaView>
  );
}

const s = StyleSheet.create({
  root: { flex: 1 },
  h1: { fontSize: 24, marginBottom: 6 },
  sub: { fontSize: 14, lineHeight: 20 },
  section: { fontSize: 12, letterSpacing: 1, marginTop: 20, marginBottom: 2 },
  hint: { fontSize: 12, lineHeight: 17, marginTop: 8 },
  input: { borderWidth: 1, borderRadius: 10, padding: 12, fontSize: 18, marginTop: 6 },
  card: { borderWidth: 1.5, borderRadius: 14, padding: 16, marginTop: 12 },
  wrap: { flexDirection: 'row', flexWrap: 'wrap', gap: 8 },
  chip: { marginTop: 8, paddingVertical: 10, paddingHorizontal: 14 },
});
