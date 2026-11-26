/**
 * VibeServer Lite — the MAIN app's VibeServer setup + status screen, VERBATIM.
 *
 * Stuart, 2026-09-18: "when opened it literally shows the VibeServer setup screen from the current
 * android app verbatim with a start button, and when running the same status screen." So this file
 * renders ../../src/screens/ServerModeScreen.tsx itself — not a copy — and supplies the one thing
 * it expects from the app around it: a `navigation` object.
 *
 * ★ beta96: Lite now opens on the server ENTRY (see SCREENS below); before that each call landed here:
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

/* ★★★ beta96 LITE GETS THE SAME ENTRY AS THE APP (docs/v12/SERVER-CHOICE.md): first open = SET UP HERE / REMOTE SETUP;
 *  then VibeServer Simple or Full (no RTL-TCP — removed from Lite on purpose, Stuart 2026-09-18), the last one
 *  remembered. A tiny stack stands in for react-navigation: the same four calls the screens make, the same names.
 *  ★ Full is always VibeServer-only here (vibeServerOnly), as before. 'InstancePicker' (keep serving and browse) still
 *    steps the app into the background — Lite has no client. */
const SCREENS = {
  ServerChoice: require('../../src/screens/ServerChoiceScreen').default,
  SimpleServer: require('../../src/screens/SimpleServerScreen').default,
  SetupElsewhere: require('../../src/screens/SetupElsewhereScreen').default,
  ServerMode: ServerModeScreen,
};

function Lite() {
  const [stack, setStack] = useState([{ name: 'ServerChoice', params: {}, key: 0 }]);
  const seq = React.useRef(1);
  const top = stack[stack.length - 1];
  const navigation = useMemo(() => {
    const entry = (name, params) => {
      const n = SCREENS[name] ? name : 'ServerChoice';
      const p = n === 'ServerMode' ? { ...(params || {}), vibeServerOnly: true } : (params || {});
      return { name: n, params: p, key: seq.current++ };
    };
    return {
      navigate: (name, params) => {
        if (name === 'InstancePicker') { BackHandler.exitApp(); return; }
        setStack((st) => [...st, entry(name, params)]);
      },
      replace: (name, params) => setStack((st) => [...st.slice(0, -1), entry(name, params)]),
      // ★ Back on the bottom screen: a fresh copy of it (Lite's old "Cancel" behaviour), never a dead press.
      goBack: () => setStack((st) => st.length > 1 ? st.slice(0, -1) : [{ ...st[0], key: seq.current++ }]),
      addListener: () => () => {},
      setOptions: () => {},
      isFocused: () => true,
    };
  }, []);
  React.useEffect(() => {
    const sub = BackHandler.addEventListener('hardwareBackPress', () => {
      if (stack.length > 1) { navigation.goBack(); return true; }
      return false;   // ★ the system's own: Lite steps into the background, the server carries on
    });
    return () => sub.remove();
  }, [stack.length, navigation]);
  const Screen = SCREENS[top.name];
  return (
    <SafeAreaProvider>
      <StatusBar barStyle="light-content" backgroundColor="#000" />
      <Screen key={top.key} navigation={navigation} route={{ key: String(top.key), name: top.name, params: top.params }} />
    </SafeAreaProvider>
  );
}
AppRegistry.registerComponent('VibeServerLite', () => Lite);
