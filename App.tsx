import React, { useCallback, useEffect, useRef, useState } from 'react';
import { NavigationContainer, createNavigationContainerRef } from '@react-navigation/native';
import { createNativeStackNavigator } from '@react-navigation/native-stack';
import { StatusBar } from 'expo-status-bar';
import { Animated, ActivityIndicator, AppState, LogBox, Text, View } from 'react-native';
import { GestureHandlerRootView } from 'react-native-gesture-handler';
import { TouchableOpacity } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { useFonts } from 'expo-font';
LogBox.ignoreAllLogs();

import { watchProvider } from './src/services/watchProvider';
import { getFavourites } from './src/services/favourites';
import { getViewMode } from './src/services/viewMode';
import { getDefaultInstance } from './src/services/defaultInstance';
import { watchTargetPending } from './src/services/watchBoot';
import InstancePickerScreen from './src/screens/InstancePickerScreen';
import SDRScreen            from './src/screens/SDRScreen';
import RtlTcpServerScreen   from './src/screens/RtlTcpServerScreen';
import ServerModeScreen     from './src/screens/ServerModeScreen';
import TunerScreen          from './src/screens/TunerScreen';
import CrashBoundary        from './src/components/CrashBoundary';
import { installCrashGuard } from './src/services/crashGuard';
import { ThemeProvider }    from './src/contexts/ThemeContext';
import type { ViewMode }    from './src/services/viewMode';
import type { SDRMode }     from './src/services/UberSDRClient';
import { useDeepLinks }     from './src/linking/useDeepLinks';

export type RootStackParamList = {
  // autoSpy: set by an `sdr://host:port` deep link → the picker auto-runs
  // connectSpy() once, then clears the param (see InstancePickerScreen).
  InstancePicker: {
    autoSpy?: { host: string; port: number };
    /** Sit in the stack WITHOUT auto-connecting to the default.
     *
     *  A watch-driven boot resets to [InstancePicker, target] so that BACK still has
     *  somewhere to go — but the picker auto-connects to the default the moment it
     *  mounts, which would drag the user straight back off the server the watch just
     *  chose. This says "be here, but don't take over". */
    noAutoConnect?: boolean;
  } | undefined;
  SDR: {
    baseUrl:         string;
    password?:       string;
    instanceName?:   string;
    viewMode:        ViewMode;
    serverLongitude?: number | null;
    serverType?:     'ubersdr' | 'kiwi' | 'owrx';   // v3 multi-backend; default ubersdr
    // NB FM-DX servers route to the 'Tuner' screen instead (see below), not here.
    // V4 local hardware (Android): connect to the on-device shim on localhost.
    // Audio comes from its /ws/audio (external-PCM engine), not the UberSDR /ws.
    isLocal?:        boolean;
    localPort?:      number;
    // VibeServer (remote shim): the LAN host serving /ws/audio + /ws/user-spectrum,
    // and the PIN auth query suffix ("&vs_nonce=&vs_auth="). Absent for a local
    // (loopback) session, which stays on 127.0.0.1 with no auth.
    localHost?:      string;
    authSuffix?:     string;
    // RTL-TCP: same on-device shim but fed IQ from an rtl_tcp server over the
    // network (no USB → works on iOS). Reuses the isLocal wiring; isTcp drives the
    // RTL-TCP icon/labels, tcpHost/tcpPort allow reconnect.
    // Some public receivers cap how long one listener may stay (SpyServer's
    // maxSessionDuration; other directories expose the same idea). 0/undefined =
    // unlimited. Drives the on-connect warning and the countdown by the clock.
    sessionLimitMins?: number;
    isTcp?:          boolean;
    tcpHost?:        string;
    tcpPort?:        number;
    // Local-session generation (see services/localSession): the unmount cleanup
    // only stops the shim if this is still the latest session, so a stale screen
    // can't tear down a newer one when switching instances.
    localGen?:       number;
    // vibesdr:// deep link: connect and optionally apply an initial tune. These
    // override the persisted last-tune for this instance on first connect only.
    deepLink?:       boolean;
    initialFreq?:    number;
    initialMode?:    SDRMode;
    initialZoom?:    number;
    // CarFM fork: this is the car FM-radio use (local USB dongle or rtl_tcp dev
    // loop). Switches the MediaSession to the RDS->MediaMetadata mapping the
    // ESP32 display expects (RT->title, PS->artist, freq->album) and defaults
    // media ⏮/⏭ to stepping presets. Non-car SDR sessions leave it unset.
    carFm?:          boolean;
  };
  // Server mode (Android): pick a sharing protocol (VibeServer / RTL-TCP) for
  // this device's USB dongle, with shared PIN + auto-discovery options.
  ServerMode: { name?: string } | undefined;
  // RTL-TCP server (Android): share this device's USB dongle over the network.
  // `advertise` (default true) lets the Server-mode picker honour the shared
  // auto-discovery toggle.
  RtlTcpServer: { name?: string; advertise?: boolean } | undefined;
  // FM-DX Webserver (v7): single shared FM tuner, server-side demod + RDS, MP3
  // audio. Distinct tuner UI (no waterfall) — see TunerScreen.
  Tuner: {
    baseUrl:       string;
    instanceName?: string;
    viewMode:      ViewMode;
    initialFreq?:  number;   // deep-link retune (retunes the shared tuner for all)
  };
};

export const splashBridge = {
  dismiss:     (_target?: string) => {},
  updateLabel: (_label: string)   => {},
  // True once the splash overlay has fully faded away. On FIRST launch the splash
  // is held open until the user taps CONTINUE on the power-saving notice, so any
  // first-run coachmark tour must wait for this — otherwise the tutorial draws
  // ON TOP of the splash (bug present since the info splash was added). Screens
  // subscribe via whenDismissed().
  dismissed: false,
  _waiters: [] as Array<() => void>,
  whenDismissed(cb: () => void): () => void {
    if (this.dismissed) { cb(); return () => {}; }
    this._waiters.push(cb);
    return () => { this._waiters = this._waiters.filter((w) => w !== cb); };
  },
  _notifyDismissed() {
    if (this.dismissed) return;
    this.dismissed = true;
    const w = this._waiters; this._waiters = [];
    w.forEach((fn) => { try { fn(); } catch { /* ignore */ } });
  },
};

// Decorative spectrum + waterfall graphic for the splash heading. Purely
// cosmetic — a static synthesised trace so the launch screen reads as an SDR.
function SplashSpectrum() {
  // A peaky spectrum envelope (0..1), centre-weighted with a couple of signals.
  const bars = [
    0.10, 0.14, 0.12, 0.18, 0.55, 0.22, 0.16, 0.20, 0.30, 0.85,
    0.95, 0.78, 0.32, 0.22, 0.18, 0.40, 0.25, 0.16, 0.62, 0.70,
    0.38, 0.20, 0.15, 0.24, 0.12, 0.16, 0.10, 0.14, 0.11, 0.09,
  ];
  const W = 220, SPEC_H = 46, WF_H = 30, GAP = 2;
  const bw = (W - GAP * (bars.length - 1)) / bars.length;
  // Three waterfall rows fading downward — older lines dimmer.
  const wfRows = [0.85, 0.55, 0.3];
  return (
    <View style={{ width: W, marginBottom: 22, alignItems: 'center' }}>
      {/* Spectrum bars */}
      <View style={{ flexDirection: 'row', alignItems: 'flex-end', height: SPEC_H, width: W }}>
        {bars.map((v, i) => (
          <View key={i} style={{
            width: bw, marginRight: i === bars.length - 1 ? 0 : GAP,
            height: Math.max(2, v * SPEC_H),
            backgroundColor: `rgba(255,184,51,${0.35 + v * 0.6})`,
            borderTopLeftRadius: 1, borderTopRightRadius: 1,
          }} />
        ))}
      </View>
      {/* Waterfall — a few rows of the same envelope, fading down */}
      <View style={{ width: W, height: WF_H, marginTop: 3, borderRadius: 2, overflow: 'hidden' }}>
        {wfRows.map((alpha, r) => (
          <View key={r} style={{ flexDirection: 'row', height: WF_H / wfRows.length, width: W }}>
            {bars.map((v, i) => (
              <View key={i} style={{
                width: bw, marginRight: i === bars.length - 1 ? 0 : GAP,
                flex: undefined,
                backgroundColor: `rgba(255,${120 + v * 90},${20 + v * 30},${v * alpha})`,
              }} />
            ))}
          </View>
        ))}
      </View>
    </View>
  );
}

const Stack = createNativeStackNavigator<RootStackParamList>();
export const navigationRef = createNavigationContainerRef<RootStackParamList>();

export default function App() {
  // Install the global JS crash guard once — flaky SDR servers must never abort
  // the whole app; recover to the picker with a server-attributed message.
  useEffect(() => { installCrashGuard(navigationRef); }, []);

  // ── Apple Watch: run HEADLESS ──────────────────────────────────────────────
  //
  //    The watch can wake the phone by sending it a message — iOS cold-launches the
  //    app straight INTO THE BACKGROUND. That is the whole point (phone in a pocket,
  //    waterfall on your wrist) and it is not something we could switch off even if
  //    we wanted to: WCSession wakes the counterpart app, full stop.
  //
  //    What was broken is that the app didn't KNOW. Every background gate defaulted to
  //    "foreground" (a `change` event only fires on a TRANSITION, and a launch straight
  //    into the background produces none), so the renderer mounted its whole Skia tree
  //    and animation drivers with nobody looking — the stutter, and the audio-DSP
  //    starvation the renderer itself warns about. Those gates now read
  //    AppState.currentState, so a headless launch mounts no renderer at all and the
  //    wrist is fed by the cheap raw-spectrum path built for the locked phone.
  //
  //    This handler is the other half: WHAT to connect to, with nobody to ask.
  //      1. the instance the WATCH asked for   (explicit beats everything)
  //      2. the DEFAULT instance               (the user's standing answer)
  //      3. FAVOURITES → ask the wrist to pick (we have candidates; let them choose)
  //      4. neither → tell them to open the phone and save one. Honest, not a fault.
  useEffect(() => {
    const startedInBackground = AppState.currentState !== 'active';

    // THE LINK IS THE APP'S, NOT A SCREEN'S. Start it before anything else: on a cold
    // boot with no default instance NO screen ever mounts, and reachability used to be
    // established inside a screen's attach() — so the wrist was told to choose a server
    // and then shown an empty list, because nothing could be sent to it.
    watchProvider.startLink();

    const pushFavs = () => {
      getFavourites()
        .then((favs) => watchProvider.sendFavourites(
          favs.map((f) => ({ name: f.name, url: f.url, type: f.serverType })),
        ))
        .catch(() => {});
    };
    pushFavs();

    /** Navigation does not exist for the first moment of a cold boot — we hold render
     *  until the fonts load. Every reset() fired before that was silently DISCARDED,
     *  which is why the headless boot resolved a default instance and then did nothing
     *  at all: no screen, no connection, no audio. Wait for the navigator. */
    const whenNavReady = () => new Promise<boolean>((resolve) => {
      if (navigationRef.isReady()) { resolve(true); return; }
      const t0 = Date.now();
      const iv = setInterval(() => {
        if (navigationRef.isReady()) { clearInterval(iv); resolve(true); }
        else if (Date.now() - t0 > 15000) { clearInterval(iv); resolve(false); }
      }, 50);
    });

    const goTo = async (f: { name: string; url: string; serverType?: string }, viewMode: ViewMode) => {
      if (!(await whenNavReady())) { watchTargetPending.claimed = false; return; }
      const target = f.serverType === 'fmdx'
        ? { name: 'Tuner', params: { baseUrl: f.url, instanceName: f.name, viewMode } }
        : { name: 'SDR', params: {
              baseUrl: f.url, instanceName: f.name, viewMode,
              serverType: (f.serverType ?? 'ubersdr') as 'ubersdr' | 'kiwi' | 'owrx',
            } };
      // RESET, never navigate() — navigate PUSHES and leaves the old screen mounted
      // and streaming (that's what made the wrist flash between the waterfall and the
      // FM-DX screen).
      //
      // The picker sits BENEATH the target so BACK still goes somewhere: resetting to
      // the target alone left the user stranded on a screen with nothing to pop to,
      // and "Back to Instances" did nothing at all. It is passed noAutoConnect,
      // because it auto-connects to the DEFAULT on mount and would otherwise drag us
      // straight back off the server the watch just chose.
      navigationRef.reset({
        index: 1,
        routes: [{ name: 'InstancePicker', params: { noAutoConnect: true } }, target],
      } as never);

      // DISMISS THE SPLASH. InstancePicker is the ONLY thing that ever dismissed it
      // (splashBridge.dismiss() in its mount effect) — and a watch-driven boot goes
      // STRAIGHT to the target and never mounts the picker at all. So the splash sat
      // there spinning forever, with the FM-DX shared-tuner warning stacked on top of
      // it. Whoever decides where we're going owns dismissing it.
      splashBridge.dismiss();

      watchProvider.setPhoneStatus('ready');
      // We got where we were going — the picker is free to behave normally again.
      watchTargetPending.claimed = false;
    };

    const applyInstance = (url: string) => {
      if (!url) return;
      watchTargetPending.claimed = true;   // stop the picker auto-connecting past us
      watchProvider.setPhoneStatus('starting');
      Promise.all([getFavourites(), getViewMode()])
        .then(([favs, viewMode]) => {
          const f = favs.find((x) => x.url === url);
          if (f) return goTo(f, viewMode);
          watchTargetPending.claimed = false;   // unknown URL — don't hold the picker
        })
        .catch(() => { watchTargetPending.claimed = false; });
    };
    watchProvider.setInstanceHandler(applyInstance);

    // Headless boot: decide what to connect to, with no user to ask.
    if (startedInBackground) {
      watchTargetPending.claimed = true;      // the picker must not race us
      watchProvider.setPhoneStatus('starting');
      Promise.all([getDefaultInstance(), getFavourites(), getViewMode()])
        .then(([def, favs, viewMode]) => {
          if (def) {
            // DefaultInstance stores no serverType (the picker navigates without one).
            // If the same server is also FAVOURITED we know its type — use it, so an
            // OWRX or FM-DX default doesn't get mis-opened as UberSDR.
            const known = favs.find((f) => f.url === def.url);
            goTo({ name: def.name, url: def.url, serverType: known?.serverType }, viewMode);
          } else if (favs.length) {
            // We have candidates but no standing answer — let the wrist choose rather
            // than picking one for them.
            watchProvider.setPhoneStatus('pick');
            watchTargetPending.claimed = false;
          } else {
            // Nothing to connect to. Say so plainly instead of showing a dead screen.
            watchProvider.setPhoneStatus('setup');
            watchTargetPending.claimed = false;
          }
        })
        .catch(() => {
          watchProvider.setPhoneStatus('setup');
          watchTargetPending.claimed = false;
        });
    }

    // Favourites change on the picker, not here — re-read on foreground rather than
    // trying to observe a store we don't own.
    const sub = AppState.addEventListener('change', (st) => {
      if (st !== 'active') return;
      // The user has the phone in their hand — THEY drive now. Never let a stale
      // watch claim keep the picker from auto-connecting to their default: that made
      // the app stop connecting on a normal open, which is far worse than the bug it
      // was guarding against.
      watchTargetPending.claimed = false;
      pushFavs();
    });
    return () => sub.remove();
  }, []);

  const [fontsLoaded] = useFonts({
    'Nixie One':              require('./assets/fonts/NixieOne-Regular.ttf'),
    'Atkinson Hyperlegible':  require('./assets/fonts/AtkinsonHyperlegible-Regular.ttf'),
  });

  const [splashDone, setSplashDone]   = useState(false);
  const [splashLabel, setSplashLabel] = useState('CONNECTING TO INSTANCE LIST');
  const splashOpacity = useRef(new Animated.Value(1)).current;

  // First launch shows the power-saving info and waits for the user to tap
  // CONTINUE (so they can actually read it); every later launch reverts to the
  // brief connecting splash that auto-dismisses once the picker/instance is up.
  // `undefined` while we read the flag — keeps the splash from flashing the
  // wrong variant before AsyncStorage resolves.
  const SPLASH_SEEN_KEY = 'lsv_splash_info_seen_v1';
  const [firstOpen, setFirstOpen] = useState<boolean | undefined>(undefined);
  const firstOpenRef = useRef(false);
  useEffect(() => {
    AsyncStorage.getItem(SPLASH_SEEN_KEY).then((v) => {
      const first = v !== '1';
      firstOpenRef.current = first;
      setFirstOpen(first);
    }).catch(() => { firstOpenRef.current = false; setFirstOpen(false); });
  }, []);

  const fadeSplash = useCallback(() => {
    Animated.timing(splashOpacity, { toValue: 0, duration: 450, useNativeDriver: true })
      .start(() => { setSplashDone(true); splashBridge._notifyDismissed(); });
  }, [splashOpacity]);

  splashBridge.dismiss = useCallback((target?: string) => {
    if (target) setSplashLabel(`CONNECTING TO:\n${target.toUpperCase()}`);
    // On first launch hold the splash open — the user dismisses it with the
    // CONTINUE button so the power-saving notice is actually read.
    if (firstOpenRef.current) return;
    fadeSplash();
  }, [fadeSplash]);
  splashBridge.updateLabel = (label: string) => setSplashLabel(label.toUpperCase());

  const handleContinue = useCallback(() => {
    firstOpenRef.current = false;
    AsyncStorage.setItem(SPLASH_SEEN_KEY, '1').catch(() => {});
    fadeSplash();
  }, [fadeSplash]);

  // vibesdr:// deep links — drain once fonts are loaded and the first-open flag
  // has resolved (so we don't navigate before the nav container is mounted).
  useDeepLinks(fontsLoaded && firstOpen !== undefined);

  // Hold splash until fonts are ready — prevents flash of Courier New fallback
  if (!fontsLoaded) return null;

  return (
    <GestureHandlerRootView style={{ flex: 1 }}>
      <ThemeProvider>
      <View style={{ flex: 1, backgroundColor: '#080601' }}>
        <CrashBoundary>
        <NavigationContainer ref={navigationRef}>
          <StatusBar style="light" />
          <Stack.Navigator
            initialRouteName="InstancePicker"
            screenOptions={{
              headerStyle:      { backgroundColor: '#0A0A12' },
              headerTintColor:  '#FFB833',
              headerTitleStyle: { fontFamily: 'Courier' },
              contentStyle:     { backgroundColor: '#0A0A12' },
              animation:        'fade',
            }}
          >
            <Stack.Screen name="InstancePicker" component={InstancePickerScreen} options={{ headerShown: false }} />
            <Stack.Screen name="SDR"            component={SDRScreen}            options={{ headerShown: false, gestureEnabled: false }} />
            <Stack.Screen name="ServerMode"     component={ServerModeScreen}     options={{ headerShown: false }} />
            <Stack.Screen name="RtlTcpServer"   component={RtlTcpServerScreen}   options={{ headerShown: false }} />
            <Stack.Screen name="Tuner"          component={TunerScreen}          options={{ headerShown: false }} />
          </Stack.Navigator>
        </NavigationContainer>
        </CrashBoundary>

        {!splashDone && (
          <Animated.View style={{
            position: 'absolute', top: 0, left: 0, right: 0, bottom: 0,
            backgroundColor: '#0A0A12', alignItems: 'center', justifyContent: 'center',
            zIndex: 9999, opacity: splashOpacity,
          }}>
            <SplashSpectrum />
            <Text style={{ color: '#FFB833', fontSize: 22, fontFamily: 'Courier', fontWeight: 'bold' }}>
              VibeSDR
            </Text>
            <Text style={{ color: 'rgba(255,184,51,0.6)', fontSize: 11, fontFamily: 'Courier', marginTop: 12, textAlign: 'center' }}>
              {splashLabel}
            </Text>
            {firstOpen ? (
              <TouchableOpacity
                onPress={handleContinue}
                activeOpacity={0.85}
                style={{
                  marginTop: 26, paddingVertical: 10, paddingHorizontal: 30,
                  borderRadius: 8, borderWidth: 1, borderColor: '#FFB833',
                  backgroundColor: 'rgba(255,184,51,0.12)',
                }}>
                <Text style={{ color: '#FFB833', fontSize: 13, fontFamily: 'Courier', fontWeight: 'bold', letterSpacing: 1 }}>
                  CONTINUE
                </Text>
              </TouchableOpacity>
            ) : (
              <ActivityIndicator color="#FFB833" style={{ marginTop: 28 }} />
            )}

            <View style={{ position: 'absolute', bottom: 40, left: 28, right: 28 }}>
              <Text style={{ color: 'rgba(255,184,51,0.9)', fontSize: 11, fontFamily: 'Courier', fontWeight: 'bold', textAlign: 'center', marginBottom: 10, letterSpacing: 1 }}>
                POWER-SAVING BEHAVIOUR
              </Text>
              <Text style={{ color: 'rgba(255,184,51,0.55)', fontSize: 10.5, fontFamily: 'Courier', textAlign: 'center', lineHeight: 16 }}>
                When you switch away from VibeSDR the waterfall and spectrum fully freeze to save power. They take a second or two to resume when you return — this is normal.{'\n\n'}
                After 30 seconds the waterfall and spectrum slow down to save power. This can be turned off in the menu.{'\n\n'}
                Full pausing in the background is by design and cannot be disabled.
              </Text>
            </View>
          </Animated.View>
        )}
      </View>
      </ThemeProvider>
    </GestureHandlerRootView>
  );
}
