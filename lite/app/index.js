/**
 * VibeServer Lite — the MAIN app's VibeServer setup + status screen, VERBATIM.
 *
 * Stuart, 2026-09-18: "when opened it literally shows the VibeServer setup screen from the current
 * android app verbatim with a start button, and when running the same status screen." So this file
 * renders ../../src/screens/ServerModeScreen.tsx itself — not a copy — and supplies the one thing
 * it expects from the app around it: a `navigation` object.
 *
 * ★ Lite HAS no other screens, so each navigation call lands somewhere honest:
 *   goBack()                     — "Stop & back" / "Cancel": back to a fresh setup form (remount).
 *   navigate('InstancePicker')   — "keep serving and browse": there is no client here, so the app
 *                                   steps into the background and the server carries on.
 * ★ Lite is a VibeServer-ONLY build: `vibeServerOnly` removes the protocol picker from the screen,
 *   so its rtl_tcp branch (navigation.replace) is unreachable and needs no destination.
 */
import React, { useMemo, useState } from 'react';
import { AppRegistry, BackHandler, StatusBar } from 'react-native';
import { SafeAreaProvider } from 'react-native-safe-area-context';
import ServerModeScreen from '../../src/screens/ServerModeScreen';
import { recordCrash } from '../../src/services/crashGuard';

/* ★★ LITE RECORDS ITS OWN JS ERRORS TOO (2026-10-07). The main app's crashGuard is installed from App.tsx,
 *  which Lite never runs, so Export logs' "last JS error" was always "none recorded" here. Lite has no
 *  picker to recover to, so this only RECORDS and then hands the error to React Native's own handler —
 *  behaviour is unchanged; the report gains the message and stack. */
{
  const EU = global.ErrorUtils;
  const prev = EU?.getGlobalHandler?.();
  EU?.setGlobalHandler?.((error, isFatal) => {
    try { recordCrash(error, 'ServerMode (Lite)'); } catch {}
    if (prev) prev(error, isFatal);
  });
}

function Lite() {
  const [mount, setMount] = useState(0);
  const navigation = useMemo(() => ({
    goBack: () => setMount((n) => n + 1),
    navigate: () => BackHandler.exitApp(),
    replace: () => {},
    addListener: () => () => {},
    setOptions: () => {},
    isFocused: () => true,
  }), []);
  return (
    <SafeAreaProvider>
      <StatusBar barStyle="light-content" backgroundColor="#000" />
      <ServerModeScreen key={mount} navigation={navigation} route={{ key: 'ServerMode', name: 'ServerMode', params: { vibeServerOnly: true } }} />
    </SafeAreaProvider>
  );
}
AppRegistry.registerComponent('VibeServerLite', () => Lite);
