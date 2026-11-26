/**
 * ★★★ beta96 REMOTE SETUP — "SET UP ON ANOTHER DEVICE", FIRST-TIME ONLY (Stuart, 2026-11-26).
 *
 * Made for TVs, where typing with a remote is the worst of it. ONE screen with a big BACK:
 *   • the server is started on this network (as VibeServer Simple, so /setup is served and nothing leaves the LAN);
 *   • a ONE-TIME CODE, big (ABCD-EFGH), with the address to open — http://<ip>:<port>/setup and <name>.local:<port>/setup —
 *     and a countdown (five minutes);
 *   • the laptop types the code; its first and only step is to SET the admin password (twice); then it is an ordinary
 *     admin on /setup. This screen shows "Setup in progress on <address>" meanwhile;
 *   • when the page presses Finish, this screen hands over to the server's own status screen (VibeServer Full — the
 *     owner set every setting there). Remote access to /setup later is the owner's explicit choice at Finish.
 * The code dies after five minutes, on BACK (the code AND the window), once used, or after five wrong tries (said here).
 * Not offered — and refused by the server — once an admin password exists.
 */
import React, { useCallback, useEffect, useRef, useState } from 'react';
import { ActivityIndicator, NativeModules, ScrollView, StyleSheet, Text, View } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import AsyncStorage from '@react-native-async-storage/async-storage';
import ServerKey, { useKeyList } from '../components/ServerKey';
import { themeFor } from '../constants/theme';
import { serverPalette } from '../constants/serverPalette';
import type { PopupTokens } from '../constants/popupTokens';
import { NAME_KEY } from '../services/rtlTcpServer';
import { getConnectedRadio, getVibeServerStatus, stopVibeServer, fpsForTier, DEFAULT_FPS_TIER } from '../services/vibeServer';
import {
  adoptRemoteAdminPassword, getMdnsName, setChoice, setupCodeCancel, setupCodeIssue, setupCodeState, startSimpleServer,
  type SetupCodeState,
} from '../services/serverChoiceStore';
import { mdnsLabel } from '../utils/serverChoice';
import { RATE_OPTIONS_AHF, RATE_OPTIONS_ASP, RATE_OPTIONS_HRF, RATE_OPTIONS_RSP, RATE_OPTIONS_RTL } from './ServerModeScreen';

const Local: any = (NativeModules as any).VibeLocalSDR;

/** ABCD-EFGH */
const pretty = (c: string) => (c.length === 8 ? c.slice(0, 4) + '-' + c.slice(4) : c);

type Props = { navigation: any; route: any; tokens?: PopupTokens };

export default function SetupElsewhereScreen({ navigation, tokens }: Props) {
  const { font: F } = themeFor();
  const C = serverPalette(tokens);
  const [code, setCode] = useState('');
  const [addr, setAddr] = useState<{ ip: string; port: number; host: string } | null>(null);
  const [st, setSt] = useState<SetupCodeState | null>(null);
  const [error, setError] = useState<string | null>(null);
  const startedHere = useRef(false);
  const doneRef = useRef(false);

  useEffect(() => { adoptRemoteAdminPassword(); }, []);

  const issue = useCallback(async () => {
    setError(null);
    try { setCode(await setupCodeIssue()); }
    catch (e: any) { setError(e?.message ?? String(e)); }
  }, []);

  // Start (or adopt) the server on this network, then a code.
  useEffect(() => {
    let gone = false;
    (async () => {
      try {
        let s = await getVibeServerStatus();
        if (!s?.running) {
          const [v, mdns, radio] = await Promise.all([AsyncStorage.getItem(NAME_KEY).catch(() => null), getMdnsName(), getConnectedRadio()]);
          const d = (radio as any)?.driver;
          const list = d === 'airspyhf' ? RATE_OPTIONS_AHF : d === 'sdrplay' ? RATE_OPTIONS_RSP : d === 'hackrf' ? RATE_OPTIONS_HRF
                     : d === 'airspy' ? RATE_OPTIONS_ASP : RATE_OPTIONS_RTL;
          await startSimpleServer({ name: v || 'VibeSDR', mdnsName: mdns, rate: 0,
                                    topRate: list.find((o) => o.value > 0)?.value ?? 0, fps: fpsForTier(DEFAULT_FPS_TIER) });
          startedHere.current = true;
          s = await getVibeServerStatus();
        }
        if (gone) return;
        let host = '';
        try { host = String((await Local?.getMdnsHostname?.()) || ''); } catch {}
        setAddr({ ip: s?.ip ?? '', port: s?.port ?? 0, host: host || mdnsLabel((await getMdnsName()) || 'VibeSDR') });
        await issue();
      } catch (e: any) { if (!gone) setError(e?.message ?? 'The server could not start. Is a radio plugged in?'); }
    })();
    return () => { gone = true; };
  }, [issue]);

  // The countdown and the session, once a second.
  useEffect(() => {
    const t = setInterval(async () => {
      const r = await setupCodeState();
      if (!r) return;
      setSt(r.state);
      if (r.state.finished && !doneRef.current) {
        // ★ Finished on the page: hand over to the server's own status screen.
        doneRef.current = true;
        await setupCodeCancel();       // closes the window; /setup goes back to the owner's switch
        await setChoice('full');
        navigation.replace('ServerMode', { vibeServerOnly: true, keepOnExit: true });
      }
    }, 1000);
    return () => clearInterval(t);
  }, [navigation]);

  // ★ Leaving by any road kills the code and the window.
  useEffect(() => () => { if (!doneRef.current) void setupCodeCancel(); }, []);

  const back = useCallback(async () => {
    doneRef.current = true;
    await setupCodeCancel();
    const pwSet = !!st?.passwordSet;
    // ★ A server started only to show this code goes away again if nothing was set up; once a password exists it is a
    //   real server — keep it running and show its own screen.
    if (pwSet) { await setChoice('full'); navigation.replace('ServerMode', { vibeServerOnly: true, keepOnExit: true }); return; }
    if (startedHere.current) await stopVibeServer();
    navigation.replace('ServerChoice', {});
  }, [navigation, st]);

  const dead = st && !st.active && !st.inProgress && !st.passwordSet && !!code;
  const keys: Array<() => void> = [() => { void back(); }, ...(dead ? [() => { void issue(); }] : [])];
  const [hl, setHl] = useKeyList(keys.length, (i) => keys[i]?.(), () => { void back(); });

  const mm = Math.floor((st?.secondsLeft ?? 300) / 60), ss = (st?.secondsLeft ?? 300) % 60;
  const ink = { dim: C.textDim, fontFamily: F };
  return (
    <SafeAreaView style={[s.root, { backgroundColor: C.bg }]}>
      <ScrollView contentContainerStyle={{ padding: 22, paddingBottom: 40 }}>
        <ServerKey {...ink} big first color={C.keyAct} border={C.borderBright} focused={hl === 0} onFocusHere={() => setHl(0)}
          testID="elsewhere-back" title="‹ BACK" sub="Cancels the code." onPress={() => { void back(); }} />
        <Text style={[s.h1, { color: C.text, fontFamily: F }]}>Set up on another device</Text>
        {!!error && <Text style={[s.line, { color: C.bad, fontFamily: F }]}>{error}</Text>}
        {!error && !code && <ActivityIndicator color={C.keyAct} style={{ marginTop: 30 }} />}
        {!!code && addr && (<>
          <Text style={[s.line, { color: C.textDim, fontFamily: F }]}>1. On a laptop or phone on the same Wi-Fi, open</Text>
          <Text style={[s.addr, { color: C.text, fontFamily: F }]} testID="elsewhere-url">http://{addr.ip}:{addr.port}/setup</Text>
          <Text style={[s.line, { color: C.textDim, fontFamily: F }]}>or {addr.host}.local:{addr.port}/setup</Text>
          <Text style={[s.line, { color: C.textDim, fontFamily: F, marginTop: 18 }]}>2. Type this code there:</Text>
          {st?.inProgress ? (
            <View style={[s.card, { borderColor: C.ok }]}>
              <Text style={{ color: C.ok, fontFamily: F, fontSize: 22 }} testID="elsewhere-progress">
                Setup in progress{st.peer ? ' on ' + st.peer : ''}
              </Text>
              <Text style={{ color: C.textDim, fontFamily: F, fontSize: 14, marginTop: 8 }}>
                {st.passwordSet ? 'The admin password is set. Finish on the other device to come back here.'
                                : 'First step there: set the admin password.'}
              </Text>
            </View>
          ) : st && !st.active ? (
            <View style={[s.card, { borderColor: C.bad }]}>
              <Text style={{ color: C.bad, fontFamily: F, fontSize: 20 }}>
                {st.ended === 'too_many_wrong' ? 'Too many wrong codes — that code was cancelled.'
                 : st.ended === 'expired' ? 'The code ran out after five minutes.'
                 : st.ended === 'password_exists' ? 'This server already has an admin password — sign in with it at /setup.'
                 : 'The code is no longer valid.'}
              </Text>
            </View>
          ) : (<>
            <Text style={[s.code, { color: C.keyAct, fontFamily: F }]} testID="elsewhere-code">{pretty(code)}</Text>
            <Text style={[s.line, { color: C.textDim, fontFamily: F }]}>
              Works once, for {mm}:{String(ss).padStart(2, '0')} more{st?.wrong ? ` — ${st.wrong} wrong ${st.wrong === 1 ? 'try' : 'tries'} so far (5 cancel it)` : ''}.
            </Text>
          </>)}
          {dead && (
            <ServerKey {...ink} color={C.keyAct} border={C.borderBright} focused={hl === 1} onFocusHere={() => setHl(1)}
              title="New code" onPress={() => { void issue(); }} />
          )}
          <Text style={[s.small, { color: C.textDim, fontFamily: F }]}>
            The first thing the other device asks for is a new admin password, typed there twice. The code itself is
            never a password. This works only until this server has an admin password, and only on this network. The
            local network has no HTTPS, so the code and the password cross it unencrypted: use it on your own Wi-Fi.
          </Text>
        </>)}
      </ScrollView>
    </SafeAreaView>
  );
}

const s = StyleSheet.create({
  root: { flex: 1 },
  h1: { fontSize: 26, marginTop: 18, marginBottom: 8 },
  line: { fontSize: 16, lineHeight: 22, marginTop: 6 },
  addr: { fontSize: 26, marginTop: 6 },
  code: { fontSize: 64, letterSpacing: 6, marginTop: 10 },
  card: { borderWidth: 2, borderRadius: 14, padding: 18, marginTop: 12 },
  small: { fontSize: 12, lineHeight: 17, marginTop: 24 },
});
