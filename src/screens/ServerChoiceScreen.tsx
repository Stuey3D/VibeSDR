/**
 * ★★★ beta96 THE WAY INTO "USE AS A SERVER" (Stuart, 2026-11-26) — the main app's and VibeServer Lite's.
 *
 *   FIRST OPEN (nothing set up yet): two big keys — SET UP HERE and REMOTE SETUP.
 *     • SET UP HERE  → the list: VibeServer Simple · VibeServer Full · RTL-TCP (Lite: Simple and Full — RTL-TCP was
 *                      removed from Lite on purpose, Stuart 2026-09-18).
 *     • REMOTE SETUP → "Set up on another device": a one-time code on this screen, the owner sets it up from a laptop.
 *                      FIRST-TIME ONLY — not drawn at all once an admin password exists (it would be a dead key: the
 *                      server refuses the code then too). Owners with a password open /setup and sign in with it.
 *   AFTER THAT: straight to the last one used (CHOICE_KEY), which has its own "‹ Server type" key back to this list.
 *
 * ★ A server already running decides where to go: RTL-TCP running → its screen; a VibeServer → Simple or Full as last
 *   chosen (Full by default), which ADOPT it rather than starting a second one.
 * ★ D-pad: every key is a ServerKey (TV remote focus + the pad / keyboard bus, useKeyList). Back leaves.
 */
import React, { useCallback, useEffect, useMemo, useState } from 'react';
import { ActivityIndicator, Alert, NativeModules, Platform, ScrollView, StyleSheet, Text, View } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import { themeFor } from '../constants/theme';
import { serverPalette } from '../constants/serverPalette';
import type { PopupTokens } from '../constants/popupTokens';
import ServerKey, { useKeyList } from '../components/ServerKey';
import { CHOICE_WORDS, choicesFor, routeFor, type ServerChoice } from '../utils/serverChoice';
import { getChoice, setChoice, storeFacts, setupCodeSupported, adoptRemoteAdminPassword } from '../services/serverChoiceStore';
import { rtlTcpServerSupported, getServerStatus, stopRtlTcpServer } from '../services/rtlTcpServer';
import { getConnectedRadio, getVibeServerStatus, stopVibeServer } from '../services/vibeServer';
import { icomPresence } from '../services/androidIcom';

const isLite = !!(NativeModules as any).VibeLocalSDR?.isLite;

/** The params each screen is opened with. Full is opened VibeServer-only: RTL-TCP is its own entry now. */
export function paramsFor(c: ServerChoice, keepOnExit: boolean): Record<string, unknown> {
  if (c === 'full') return { vibeServerOnly: true, keepOnExit };
  if (c === 'simple') return { keepOnExit };
  return { setup: true };
}

type Props = { navigation: any; route: any; tokens?: PopupTokens };

export default function ServerChoiceScreen({ navigation, route, tokens }: Props) {
  const { font: F } = themeFor();
  const C = serverPalette(tokens);
  const choose = !!route?.params?.choose;
  const [phase, setPhase] = useState<'loading' | 'first' | 'list'>('loading');
  const [noPassword, setNoPassword] = useState(false);
  const [icom, setIcom] = useState(false);
  const [running, setRunning] = useState<'vibeserver' | 'rtltcp' | null>(null);

  useEffect(() => { adoptRemoteAdminPassword(); }, []);

  useEffect(() => {
    let gone = false;
    (async () => {
      const [choice, facts, radio, rtl, vs] = await Promise.all([
        getChoice(), storeFacts(), getConnectedRadio(), rtlTcpServerSupported ? getServerStatus() : Promise.resolve(null),
        getVibeServerStatus(),
      ]);
      if (gone) return;
      const run = rtl?.running ? 'rtltcp' : vs?.running ? 'vibeserver' : null;
      setRunning(run);
      setIcom(icomPresence(radio as any) !== 'none');
      setNoPassword(!facts?.hasAdminPassword);
      if (!choose) {
        if (run === 'rtltcp') { navigation.replace('RtlTcpServer', {}); return; }
        if (run === 'vibeserver') {
          const c: ServerChoice = choice === 'simple' ? 'simple' : 'full';
          navigation.replace(routeFor(c), paramsFor(c, true)); return;
        }
        if (choice) { navigation.replace(routeFor(choice), paramsFor(choice, false)); return; }
      }
      // ★ FIRST OPEN = never chosen and no admin password: the two big keys. Otherwise the list.
      setPhase(!choose && !choice && !facts?.hasAdminPassword ? 'first' : 'list');
    })();
    return () => { gone = true; };
  }, []);

  const options = useMemo(() => choicesFor({ lite: isLite, rtlTcpSupported, icom }), [icom]);
  const remoteOk = noPassword && setupCodeSupported();

  const open = useCallback(async (c: ServerChoice) => {
    const go = async () => {
      await setChoice(c);
      navigation.replace(routeFor(c), paramsFor(c, running === 'vibeserver' && c !== 'rtltcp'));
    };
    // ★ One radio, one server: switching between a VibeServer and RTL-TCP stops the one running first — said, not done silently.
    const clash = (running === 'vibeserver' && c === 'rtltcp') || (running === 'rtltcp' && c !== 'rtltcp');
    if (!clash) { void go(); return; }
    Alert.alert('Stop the running server?',
      running === 'rtltcp' ? 'RTL-TCP is serving this radio. It stops before VibeServer can start.'
                           : 'VibeServer is serving this radio. It stops before RTL-TCP can start, and its listeners are disconnected.',
      [{ text: 'Cancel', style: 'cancel' },
       { text: 'Stop and switch', style: 'destructive', onPress: async () => {
           if (running === 'rtltcp') await stopRtlTcpServer(); else await stopVibeServer();
           setRunning(null); void go(); } }]);
  }, [navigation, running]);

  const leave = useCallback(() => {
    if (phase === 'list' && !choose && remoteOk && options.length) { setPhase('first'); return; }
    if (isLite) { (NativeModules as any).VibeLocalSDR?.minimise?.(); return; }
    navigation.goBack();
  }, [navigation, phase, choose, remoteOk, options.length]);

  // The keys on screen, in order, for the pad / keyboard.
  const keys: Array<() => void> = phase === 'first'
    ? [() => setPhase('list'), ...(remoteOk ? [() => navigation.replace('SetupElsewhere', {})] : [])]
    : [...options.map((c) => () => { void open(c); }), ...(remoteOk ? [() => navigation.replace('SetupElsewhere', {})] : [])];
  const [hl, setHl] = useKeyList(keys.length, (i) => keys[i]?.(), leave);
  useEffect(() => { setHl(0); }, [phase]);

  if (phase === 'loading') {
    return (
      <SafeAreaView style={[s.root, { backgroundColor: C.bg }]}>
        <ActivityIndicator color={C.keyAct} style={{ marginTop: 60 }} />
      </SafeAreaView>
    );
  }
  const keyInk = { color: C.keyAct, dim: C.textDim, border: C.borderBright, fontFamily: F };
  return (
    <SafeAreaView style={[s.root, { backgroundColor: C.bg }]}>
      <ScrollView contentContainerStyle={{ padding: 18, paddingBottom: 40 }}>
        <Text style={[s.h1, { color: C.text, fontFamily: F }]}>{isLite ? 'VibeServer Lite' : 'Use as a server'}</Text>
        {phase === 'first' ? (<>
          <Text style={[s.sub, { color: C.textDim, fontFamily: F }]}>
            Share the radio plugged into this {Platform.isTV ? 'TV' : 'device'} with other devices. How would you like to set it up?
          </Text>
          <ServerKey {...keyInk} big first testID="choice-here" focused={hl === 0} onFocusHere={() => setHl(0)}
            title="SET UP HERE" sub="On this screen, with the remote or the touchscreen." onPress={() => setPhase('list')} />
          {remoteOk && (
            <ServerKey {...keyInk} big testID="choice-remote" focused={hl === 1} onFocusHere={() => setHl(1)}
              title="REMOTE SETUP" sub="Show a one-time code here, then finish setting up from a laptop or phone on the same Wi-Fi — no typing with a remote."
              onPress={() => navigation.replace('SetupElsewhere', {})} />
          )}
        </>) : (<>
          <Text style={[s.sub, { color: C.textDim, fontFamily: F }]}>
            Choose how to serve this radio. You can change it later with &ldquo;Server type&rdquo; on its screen.
          </Text>
          {options.map((c, i) => (
            <ServerKey key={c} {...keyInk} first={i === 0} testID={'choice-' + c} focused={hl === i} onFocusHere={() => setHl(i)}
              title={CHOICE_WORDS[c].title} tag={CHOICE_WORDS[c].tag} sub={CHOICE_WORDS[c].desc} onPress={() => { void open(c); }} />
          ))}
          {icom && !isLite && rtlTcpServerSupported && (
            <Text style={[s.hint, { color: C.textDim, fontFamily: F }]}>
              RTL-TCP is not offered: it sends a radio&rsquo;s raw IQ, and an Icom has none.
            </Text>
          )}
          {remoteOk && (
            <ServerKey {...keyInk} testID="choice-remote" focused={hl === options.length} onFocusHere={() => setHl(options.length)}
              title="Set up on another device" sub="First-time setup from a laptop, with a one-time code."
              onPress={() => navigation.replace('SetupElsewhere', {})} />
          )}
        </>)}
        <Text style={[s.hint, { color: C.textDim, fontFamily: F, marginTop: 18 }]}>
          {isLite ? 'Back minimises the app.' : 'Back returns to the server list.'}
        </Text>
      </ScrollView>
    </SafeAreaView>
  );
}

const s = StyleSheet.create({
  root: { flex: 1 },
  h1: { fontSize: 24, marginBottom: 6 },
  sub: { fontSize: 14, lineHeight: 20, marginBottom: 4 },
  hint: { fontSize: 12, lineHeight: 17, marginTop: 10 },
});
