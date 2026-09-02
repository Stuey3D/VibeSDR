import React, { useCallback, useEffect, useRef, useState } from 'react';
import { NavigationContainer, createNavigationContainerRef } from '@react-navigation/native';
import { createNativeStackNavigator } from '@react-navigation/native-stack';
import { StatusBar } from 'expo-status-bar';
import { Animated, ActivityIndicator, AppState, LogBox, NativeModules, Text, View } from 'react-native';
import { GestureHandlerRootView } from 'react-native-gesture-handler';
import { TouchableOpacity } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { useFonts } from 'expo-font';
LogBox.ignoreAllLogs();

import { watchProvider } from './src/services/watchProvider';
import { fetchFrontDoor, radioBaseUrl, isSharedDial,
         radioOccupancy, radioLimits } from './src/services/vibeserverRadios';
import { favouritesCollection, getFavourites, getTcpFavs, setFavouriteServerType } from './src/services/favourites';
import { bookmarksCollection } from './src/services/bookmarksSync';
import { registerCollection, registerSyncHook, startCloudSync } from './src/services/cloudSync';
import {
  syncGlobalDisplayPrefs, syncLastTune, syncServerDisplayPrefs,
} from './src/services/perServerSync';
import { syncFmdxDials } from './src/services/dialSync';
import { detectServerType } from './src/services/sdrTypes';
import { newLocalSession } from './src/services/localSession';
import { getViewMode } from './src/services/viewMode';
import { getDefaultInstance } from './src/services/defaultInstance';
import { watchTargetPending } from './src/services/watchBoot';
import { fetchDirectory, type DirectoryId } from './src/services/directories';
import type { SDRInstance } from './src/services/instancesApi';

// The phone is the single source of truth for the server list — Buddy MIRRORS it and connects by
// referencing these objects (never a list of its own). Filled when the watch browses a directory, so
// a later connect resolves to the exact server the phone fetched, with its real type/params.
const watchServerCache = new Map<string, SDRInstance>();
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
import { crumb }            from './src/services/crumbs';

/* ★★★ THE VERY FIRST JS CRUMB, at module scope — BEFORE any component mounts. Paired with the
 *   native launch crumb it brackets the whole of React's startup, which is the half of the black
 *   screen we have never once measured: if this line is absent from a session, the bundle never
 *   evaluated; if it is present and no render crumb follows, React started and never drew. */
crumb('bundle evaluated');

export type RootStackParamList = {
  // autoSpy: set by an `sdr://host:port` deep link → the picker auto-runs
  // connectSpy() once, then clears the param (see InstancePickerScreen).
  InstancePicker: {
    autoSpy?: { host: string; port: number };
    /** ★★★ A VIBESERVER THE WATCH CHOSE. A VibeServer is NOT connected by navigating to SDR with
     *  serverType 'vibeserver' — it has its own route (PIN probe, saved-PIN store, resolveVibeAuth)
     *  and that route lives in the PICKER, as openDirectoryVibeServer. applyInstance was sending it
     *  down the generic goTo instead, so Buddy's radio choice landed on a screen that never
     *  connected: the wrist waited for rows for ever and the phone sat there doing nothing
     *  (Stuart, 2026-08-28). Same hand-off shape as autoSpy: the picker owns the connect. */
    autoVibe?: { url: string; name: string };
    /** Sit in the stack WITHOUT auto-connecting to the default.
     *
     *  A watch-driven boot resets to [InstancePicker, target] so that BACK still has
     *  somewhere to go — but the picker auto-connects to the default the moment it
     *  mounts, which would drag the user straight back off the server the watch just
     *  chose. This says "be here, but don't take over". */
    noAutoConnect?: boolean;
    /** ★ Land on the picker with a DIRECTORY already open. Used by Server mode's "stop and back to
     *  the directory": somebody who has just stopped serving is usually going to go and listen to
     *  somebody else's receiver, and making them tap through the chooser to the same list every
     *  time is a step with no decision in it. Implies noAutoConnect — arriving at a directory is
     *  a choice in progress, so the default instance must not take over underneath it. */
    openDir?: string;
  } | undefined;
  SDR: {
    baseUrl:         string;
    password?:       string;
    instanceName?:   string;
    viewMode:        ViewMode;
    serverLongitude?: number | null;
    serverType?:     'ubersdr' | 'kiwi' | 'web888' | 'owrx';   // v3 multi-backend; default ubersdr
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
    /** ★★★ The owner does not permit third-party apps (`ext_api === 0` in the kiwisdr.com
     *  directory). Go STRAIGHT to compatibility mode — the receiver's own web page, which is
     *  public — instead of connecting, being admitted, streaming audio and being dropped at ~10 s
     *  with no explanation. Trying first costs the owner a slot and the user ten seconds to reach
     *  the same place. See memory/kiwi_ext_api_10s_kick.md. */
    compatOnly?:     boolean;
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
  /* ★★ App() RUNNING is not the same as a FRAME EXISTING — but the pair of them, against the
   *  scene crumbs either side, says which of those the black screen is. */
  crumb('App() render');
  useEffect(() => { crumb('App mounted'); }, []);

  // Install the global JS crash guard once — flaky SDR servers must never abort
  // the whole app; recover to the picker with a server-attributed message.
  useEffect(() => { installCrashGuard(navigationRef); }, []);

  // ── iCloud sync ────────────────────────────────────────────────────────────
  // Registered here, at the app level, rather than inside the screens that own
  // each store: a per-server dial or a favourites list must still merge when
  // the app cold-launches headless for the watch and no screen ever mounts.
  useEffect(() => {
    registerCollection(favouritesCollection);
    registerCollection(bookmarksCollection);
    registerSyncHook('displayPrefs',       syncGlobalDisplayPrefs);
    registerSyncHook('serverDisplayPrefs', syncServerDisplayPrefs);
    registerSyncHook('lastTune',           syncLastTune);
    registerSyncHook('fmdxDials',          syncFmdxDials);
    return startCloudSync();
  }, []);

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
      // The watch mirrors the phone's FULL list — network favourites (incl. spyserver:// entries)
      // PLUS RTL-TCP favs, which live in a separate store (host:port). Synthesise a url for the
      // TCP ones so the wrist has a stable key to send back (applyInstance matches it).
      Promise.all([getFavourites(), getTcpFavs()])
        .then(([favs, tcpFavs]) => watchProvider.sendFavourites([
          ...favs.map((f) => ({ name: f.name, url: f.url, type: f.serverType })),
          ...tcpFavs.map((t) => ({
            name: t.name, url: `${t.proto ?? 'rtltcp'}://${t.host}:${t.port}`, type: t.proto ?? 'rtltcp',
          })),
        ]))
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

    const goTo = async (
      f: { name: string; url: string; serverType?: string },
      viewMode: ViewMode,
      extra?: Record<string, unknown>,   // local-shim params (RTL-TCP / SpyServer)
    ) => {
      if (!(await whenNavReady())) { watchTargetPending.claimed = false; watchProvider.holdAwake(false); return; }
      const target = f.serverType === 'fmdx'
        ? { name: 'Tuner', params: { baseUrl: f.url, instanceName: f.name, viewMode } }
        : { name: 'SDR', params: {
              baseUrl: f.url, instanceName: f.name, viewMode,
              serverType: (f.serverType ?? 'ubersdr') as 'ubersdr' | 'kiwi' | 'web888' | 'owrx',
              ...(extra ?? {}),
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
      // ★ The session is up: real audio takes over from here, so put the keep-alive down.
      watchProvider.holdAwake(false);
      // We got where we were going — the picker is free to behave normally again.
      watchTargetPending.claimed = false;
    };

    // RTL-TCP / SpyServer: spin up the on-device shim (the phone does the same in the
    // picker's connectTcp/connectSpy), then open the local WS it hands back. PIN-less, so
    // safe to drive headless from the wrist. host:port is parsed from the fav url.
    const connectLocal = async (
      type: 'rtltcp' | 'spyserver', host: string, port: number, name: string, viewMode: ViewMode,
    ) => {
      const Local = (NativeModules as { VibeLocalSDR?: {
        startTcp?: (o: object) => Promise<{ port: number; wsBaseUrl: string }>;
        startSpyServer?: (o: object) => Promise<{ port: number; wsBaseUrl: string }>;
      } }).VibeLocalSDR;
      const start = type === 'rtltcp' ? Local?.startTcp : Local?.startSpyServer;
      if (!start) { watchTargetPending.claimed = false; watchProvider.holdAwake(false);
                    watchProvider.setPhoneStatus('setup'); return; }
      const opts = type === 'rtltcp'
        ? { host, port, centerFreq: 14_100_000, sampleRate: 2_400_000, fftSize: 8192, fftRate: 10, mode: 'usb' }
        : { host, port, centerFreq: 100_000_000, fftSize: 8192, fftRate: 10, mode: 'wfm' };
      const res = await start(opts);
      return goTo(
        { name: name || `${host}:${port}`, url: res.wsBaseUrl, serverType: 'ubersdr' },
        viewMode,
        { isLocal: true, isTcp: true, localPort: res.port, tcpHost: host, tcpPort: port,
          localGen: newLocalSession() },
      );
    };

    const applyInstance = (urlIn: string, wtype?: string, wname?: string) => {
      let url = urlIn;
      crumb(`applyInstance url=${urlIn} type=${wtype ?? '-'}`);
      if (!url) { crumb('applyInstance: EMPTY URL, giving up'); return; }
      watchTargetPending.claimed = true;   // stop the picker auto-connecting past us
      watchProvider.setPhoneStatus('starting');
      // ★★★ AND KEEP US ALIVE LONG ENOUGH TO FINISH — see watchProvider.holdAwake. From cold this
      //   whole function runs inside the few seconds iOS grants a watch-woken app; without this it
      //   is suspended part-way and the wrist waits on a phone that has stopped running.
      watchProvider.holdAwake(true);
      Promise.all([getFavourites(), getTcpFavs(), getViewMode()])
        .then(async ([favs, tcpFavs, viewMode]) => {
          const f = favs.find((x) => x.url === url);
          // The phone's OWN directory object, cached when the watch browsed it. This is the mirror:
          // Buddy referenced a server, we connect the exact one WE fetched (right type + longitude).
          const cached = watchServerCache.get(url);
          // RTL-TCP favs live in their own store (host:port, no url) — the wrist row's url is
          // synthesised as <proto>://host:port; match that back.
          const tcp = tcpFavs.find((t) => `${t.proto ?? 'rtltcp'}://${t.host}:${t.port}` === url);
          let type = f?.serverType ?? cached?.serverType ?? wtype ?? (tcp ? (tcp.proto ?? 'rtltcp') : undefined);
          // RE-DETECT the HTTP backend, exactly as the phone's own connectFav does — a favourite saved
          // with the wrong type (e.g. an OWRX added via the custom URL box, which defaults to UberSDR)
          // must still connect right from the watch. fmdx/spyserver/vibeserver/rtl aren't HTTP-sniffable.
          if (type !== 'fmdx' && type !== 'spyserver' && type !== 'vibeserver' && type !== 'rtltcp'
              && /^https?:\/\//i.test(url)) {
            const detected = await detectServerType(url);
            if (detected) {
              if (f && detected !== f.serverType) setFavouriteServerType(url, detected).catch(() => {});
              type = detected as typeof type;
            }
          }
          /* ★★★ A VIBESERVER FRONT DOOR IS NOT A RECEIVER, and this is the one place that can
           *   know it. Jr resolves the door where the connection is MADE rather than in its
           *   picker, and says why: a server is reached from several directions — a favourite,
           *   the last one on launch, a directory row — and a check on one path silently misses
           *   the rest. Buddy's connection is made HERE, on the phone, so here is the equivalent
           *   spot; putting it in browseForWatch covered the directory and nothing else.
           * ★★ ONE RADIO IS NOT A CHOICE — prefixed silently, and the watch sees what it always
           *    did. SEVERAL is a question only the wearer can answer, so the list goes to the
           *    wrist and this connect stops here; picking one comes back as an ordinary `inst`
           *    with the radio's own address, and lands in this same function with nothing left
           *    to resolve.
           * ★ Skipped when the address already names a radio (…/r/<id>), which is exactly what
           *   the wrist sends back — otherwise choosing would ask the question again. */
          if (type === 'vibeserver' && /^https?:\/\//i.test(url) && !/\/r\/[^/]+$/.test(url)) {
            const door = await fetchFrontDoor(url).catch(() => null);
            if (door && door.radios.length > 1) {
              watchProvider.sendRadios(url, door.name || wname || 'VibeServer',
                door.radios.map((r) => ({
                  id: radioBaseUrl(url, r.id),
                  name: r.label || r.driver || 'radio',
                  users: r.users ?? 0,
                  shared: isSharedDial(r),
                })));
              // ★ Let go of the picker: we are not connecting, we are asking. Without this the
              //   phone would sit "starting" behind a question nobody had answered yet.
              /* ★★★ HELD, NOT RELEASED — the wearer is choosing RIGHT NOW. Letting go here was
               *   the obvious reading ("we are only asking a question") and it was wrong: it put
               *   the phone to sleep while they read the list, so their answer landed on a cold
               *   phone and took the slow queued-wake path all over again. Stuart: "as long as it
               *   accounts for someone slowly browsing the list too."
               * ★ Bounded by holdAwake's own 60 s deadline and refreshed by each further tap, so
               *   somebody actively using Buddy keeps a live phone under them, and somebody who
               *   walks away mid-list is let go of within the minute. */
              watchTargetPending.claimed = false;
              watchProvider.holdAwake(true);
              watchProvider.setPhoneStatus('idle');
              return;
            }
            if (door && door.radios.length === 1) url = radioBaseUrl(url, door.radios[0].id);
          }
          if (type === 'rtltcp' || type === 'spyserver') {
            // Parse host:port off the url (rtltcp://h:p or spyserver://h:p).
            const m = url.match(/^[a-z]+:\/\/([^:/]+):(\d+)/i);
            const host = m?.[1] ?? tcp?.host ?? '';
            const port = m ? Number(m[2]) : (tcp?.port ?? 0);
            if (host && port) return connectLocal(type, host, port, f?.name ?? wname ?? tcp?.name ?? '', viewMode);
          }
          /* ★★★ HAND A VIBESERVER TO THE PICKER — it owns that connect (PIN probe, saved PIN,
           *   resolveVibeAuth). Everything above has already resolved the address, including the
           *   /r/<id> the wrist sends back after choosing a radio; all that is left is to connect
           *   it the ONE way this app connects a VibeServer. Going through goTo instead reached a
           *   screen that does not speak the protocol, which is why choosing a radio on Buddy hung
           *   with the phone still unconnected. See RootStackParamList.autoVibe. */
          if (type === 'vibeserver') {
            crumb(`vibeserver branch — waiting for nav ready, url=${url}`);
            if (!(await whenNavReady())) {
              /* ★★★ A SILENT GIVE-UP — the attempt ends here with nothing on screen and nothing
               *   said. Worth a crumb because whenNavReady POLLS on a 50 ms setInterval, and a
               *   watch-woken app is headless, where iOS throttles JS timers: a gate that normally
               *   clears instantly can take arbitrarily long in the background.
               * ★★ NOT by itself the UberSDR/VibeServer differential — goTo() waits on this same
               *   gate, so it cannot explain why only VibeServer needs several attempts. What it
               *   CAN explain is an attempt that silently evaporates. Let the crumbs say which
               *   attempts reach here. */
              crumb('vibeserver branch — NAV NEVER READY, attempt abandoned');
              watchTargetPending.claimed = false; watchProvider.holdAwake(false); return;
            }
            crumb('vibeserver branch — nav ready, resetting to picker with autoVibe');
            /* ★★★ `index` IS NOT OPTIONAL, and leaving it out is why the phone went BLACK. goTo()
             *   three hundred lines up passes `index: 1` with its two routes; this reset shipped
             *   with a routes array and no index at all. A navigation state React Navigation
             *   cannot resolve renders NOTHING — no screen mounts, so the picker's autoVibe effect
             *   never runs, so a VibeServer never connects. Stuart, 2026-08-28: "i can start an
             *   UberSDR session but vibeserver will not connect", and "when opening the app the
             *   screen comes on black even though i can see the spectrum on the watch" — one fault,
             *   both symptoms, and the black screen was the one that named it.
             * ★★ UberSDR was unaffected because it goes through goTo, which has always passed an
             *   index. The two paths differed by one line and I copied the wrong half. */
            navigationRef.reset({
              index: 0,
              routes: [{ name: 'InstancePicker',
                         params: { noAutoConnect: true,
                                   autoVibe: { url, name: f?.name ?? cached?.name ?? wname ?? url } } }],
            } as never);
            splashBridge.dismiss();
            crumb('vibeserver branch — reset dispatched, splash dismissed');
            return;
          }
          if (f) return goTo({ ...f, serverType: (type ?? f.serverType) as typeof f.serverType }, viewMode);
          // A browsed directory server the phone holds — connect it with its real name/type/longitude.
          if (cached) return goTo({ name: cached.name, url: cached.url, serverType: (type ?? cached.serverType) },
                                  viewMode, { serverLongitude: cached.longitude ?? null });
          if (type) return goTo({ name: wname || url, url, serverType: type }, viewMode);
          // ★ Nothing to connect to: end the attempt AND put the keep-alive down with it.
          watchTargetPending.claimed = false;   // unknown URL, no type — don't hold the picker
          watchProvider.holdAwake(false);
        })
        .catch(() => { watchTargetPending.claimed = false; watchProvider.holdAwake(false); });
    };
    watchProvider.setInstanceHandler(applyInstance);

    // The watch asked to browse a directory. The PHONE fetches it (its own service), caches the full
    // server objects (so a later connect resolves to them), and sends the watch a light display list.
    const browseForWatch = (dirId: string) => {
      /* ★★★ A BROWSE CAN BE WHAT WOKE US, and a woken picker must not connect past the wearer.
       *   The watch's browse is durable (transferUserInfo), so it launches this app headless when
       *   it has been suspended — and InstancePicker's mount effect then auto-connects to the
       *   DEFAULT instance, i.e. audio pours out of a phone whose owner only asked to see a list.
       *   `claimed` is exactly the "the watch is deciding where we go, stand down" flag; a browse
       *   is that decision beginning. Cleared by applyInstance when the choice actually lands. */
      crumb(`browseForWatch ${dirId} — fetching`);
      watchTargetPending.claimed = true;
      /* ★★ AND STAY AWAKE WHILE THEY BROWSE. Fetching a directory means the wearer is mid-task and
       *  their next tap is seconds away — but nothing was playing, so iOS would suspend us between
       *  the list arriving and them choosing from it, making every choice a cold start. Same 60 s
       *  deadline, refreshed by each browse, so reading a long list keeps the phone under them and
       *  putting the watch down lets it go. */
      watchProvider.holdAwake(true);
      fetchDirectory(dirId as DirectoryId)
        .then((all) => {
          /* ★★★ HIDE RECEIVERS THAT REFUSE THIRD-PARTY APPS — the watch has no way to recover from
           *   one. On the phone an `extApi === 0` receiver is drawn in red and can still be opened
           *   in compatibility mode, so showing it is a choice the user can act on. On the WRIST
           *   there is no such screen: tapping it hands the phone a receiver that will refuse, and
           *   what the user sees is our app failing. Stuart: "dont want to have users attempting
           *   to connect to Kiwi's directly from buddy and failing."
           * ★★ `0` is the OWNER'S PUBLISHED POLICY, not a guess — and `undefined` (unknown) is
           *   deliberately NOT treated as blocked, so an un-joined receiver still appears rather
           *   than vanishing on missing data.
           * ★ Filtered HERE, not on the watch: Buddy's list comes from this handler, not from its
           *   own Directories.fetch, so the watch never sees what it is not sent. Jr does the same
           *   filtering in its own fetch because Jr fetches for itself. */
          crumb(`browseForWatch ${dirId} — directory returned ${all.length} servers`);
          const filtered = all.filter((s) => s.extApi !== 0);

          /* ★★★ CAP THE LIST — `sendMessage` HAS A ~65 KB CEILING and drops the message whole.
           *   A row is ~139 bytes, so KiwiSDR and Receiverbook (~800-850 servers, ~115 KB) sail
           *   past it: the send fails, the watch never gets a payload, and it spins to its 20s
           *   timeout — the SAME symptom as the dropped `dir` field, from a different cause.
           *   That is why this is capped here and not left to be discovered later.
           * ★★ NEAREST-FIRST, because a cap has to drop something and on a wrist the useful
           *   receivers are the local ones. Unknown distance sorts LAST rather than being
           *   filtered — missing data must not delete a server (same rule as `extApi` above).
           * ★ Cached BEFORE the cap? No — deliberately AFTER, so the cache holds exactly what
           *   the watch was shown. Caching a server the wrist cannot see just grows the map. */
          const CAP = 300;   // ~42 KB, comfortably inside the ceiling
          const near = (s: { distance?: number | null }) =>
            s.distance == null ? Number.POSITIVE_INFINITY : s.distance;
          const list = filtered.length > CAP
            ? [...filtered].sort((a, b) => near(a) - near(b)).slice(0, CAP)
            : filtered;
          list.forEach((s) => { if (s.url) watchServerCache.set(s.url, s); });
          watchProvider.sendDirectory(dirId, list.map((s) => ({
            id: s.url,
            name: s.name,
            type: s.serverType ?? 'ubersdr',
            country: s.countryCode ?? null,
            users: s.users ?? 0,
            full: !!s.full,
            dist: s.distance ?? null,
            /* ★★★ THE RADIOS, SO THE WRIST NEVER HAS TO ASK THE DOOR. This is the whole of Stuart's
             *   change: a VibeServer was the one backend needing a SECOND step — pick the server,
             *   then open its landing page and pick a radio — and that second step needs the phone
             *   awake a second time, which is where "VibeServer needs several attempts" lived.
             *   The listing already carries the radios, so they travel with the row and the wearer
             *   picks a radio in the directory, in place.
             * ★★ ONLY WHERE THERE IS A CHOICE TO MAKE. One radio is not a choice: that row stays a
             *   plain server row and connects as it always has, at the door.
             * ★★★ AND ONLY WHERE THEY CAN BE ADDRESSED. A radio with no `id` has no /r/<id> to be
             *   sent to — the older Android publisher omitted it — so offering it on the wrist
             *   would draw a row that cannot be tapped. Those servers keep the door route, which
             *   still works. Missing data must not delete a server.
             * ★ FINISHED TEXT, not fields: the phone owns the wording (radioOccupancy /
             *   radioLimits) so there is one formatter, not one per platform, and no Hz arithmetic
             *   happens on arm64_32 where Swift's Int is 32-bit below watchOS 27.
             * ★ `id` is the FULL address, already /r/<id>-prefixed — the same rule sendRadios and
             *   the directory rows follow: the watch is handed addresses, never taught to build
             *   them. */
            radios: (s.radios && s.radios.length > 1 && s.radios.every((r) => !!r.id))
              ? s.radios.map((r) => ({
                  id: radioBaseUrl(s.url, r.id),
                  name: r.label,
                  occupancy: radioOccupancy(r),
                  limits: radioLimits(r),
                  /* ★★ SAID BY THE PHONE, NOT INFERRED FROM THE TEXT. Buddy dims and disables a
                   *  full radio, and parsing "3/3 full" back out of a sentence to decide that
                   *  would be a second reader of a rule the phone already knows. Unknown
                   *  occupancy is NOT full — absent data must never lock somebody out. */
                  full: r.listeners != null && r.users > 0 && r.listeners >= r.users,
                }))
              : undefined,
          })));
        })
        .catch((e) => {
          /* ★★★ THE DIRECTORY FETCH FAILED, and until now that was indistinguishable on the wrist
           *   from a phone that never answered — both draw an empty list. Naming the error is the
           *   difference between "the phone is asleep" and "vibeserver.vibesdr.net timed out". */
          crumb(`browseForWatch ${dirId} — FETCH FAILED: ${String(e?.message ?? e).slice(0, 120)}`);
          watchProvider.sendDirectory(dirId, []);   // empty list = "couldn't load" on the wrist
        });
    };
    watchProvider.setBrowseHandler(browseForWatch);

    // Decide what to connect to with no user to ask: default → connect, else picker/setup.
    // Shared by the headless boot AND the watch's Reopen, so both behave identically.
    const decideAndConnect = () => {
      // ★★★ IF THE PHONE IS ALREADY ON A RECEIVER, DO NOTHING. This is shared by the
      // headless cold boot and the watch's Reopen, and for a COLD phone "go to the
      // default" is right. On a LIVE one it is a hijack: Buddy showed its Start screen
      // while the phone was happily on VibeServer, and pressing Start tore that session
      // down and connected to the default UberSDR instead (Stuart, 2026-07-29).
      // ★ The watch showing Start while the phone is live is a SEPARATE state desync —
      // this guard only makes pressing it harmless, which is the half that loses work.
      const route = navigationRef.isReady() ? navigationRef.getCurrentRoute()?.name : undefined;
      if (route === 'SDR' || route === 'Tuner') {
        watchTargetPending.claimed = false;
        watchProvider.setPhoneStatus('live');
        return;
      }
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
    };

    // The wrist's "Reopen" after a deliberate close: the flag is why we DIDN'T auto-connect
    // (see below), so clearing it is the whole point — then behave like a fresh boot.
    watchProvider.setReopenHandler(() => {
      watchProvider.clearClosedByUser();
      decideAndConnect();
    });

    // The wrist's "Stop": disconnect and return the phone to the server list to WAIT. Resetting to the
    // picker unmounts the SDR/Tuner screen — which destroys the client (audio + sockets stop) — and the
    // noAutoConnect param + the claimed flag keep it from reconnecting. The watch follows via 'pick'.
    watchProvider.setStopHandler(() => {
      watchTargetPending.claimed = true;
      whenNavReady().then((ready) => {
        // ★★★ index: 0 — THE SAME OMISSION AS THE VIBESERVER PATH ABOVE, and the same consequence:
        //     a state React Navigation cannot resolve renders NOTHING, so the watch's Stop would
        //     leave the phone on a black screen. crashGuard.ts writes this reset correctly; these
        //     two did not. Found while fixing the other one — one rule, three writers, two wrong.
        if (ready) navigationRef.reset({ index: 0,
          routes: [{ name: 'InstancePicker', params: { noAutoConnect: true } }] } as never);
        watchProvider.setPhoneStatus('pick');
      });
    });

    // Headless boot: the watch heartbeat launched us in the background.
    if (startedInBackground) {
      watchProvider.setPhoneStatus('starting');
      // ANTI-HIJACK. The user swiped us closed; a heartbeat has now relaunched us headless.
      // Do NOT auto-connect + start audio — that dumped SDR audio out the speaker mid-call.
      // Tell the wrist we're closed and wait for an explicit Reopen (which clears the flag).
      watchProvider.wasClosedByUser().then((closed) => {
        if (closed) {
          watchProvider.setPhoneStatus('closed');
          watchTargetPending.claimed = false;
        } else {
          decideAndConnect();
        }
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
      // Foregrounded by a real user tap — no longer "closed by user", so the next
      // headless boot is free to auto-connect again.
      watchProvider.clearClosedByUser();
      pushFavs();
    });
    return () => sub.remove();
  }, []);

  const [fontsLoaded, fontError] = useFonts({
    'Nixie One':              require('./assets/fonts/NixieOne-Regular.ttf'),
    'Atkinson Hyperlegible':  require('./assets/fonts/AtkinsonHyperlegible-Regular.ttf'),
  });
  /** ★★★ NEVER BLOCK THE WHOLE APP ON FONTS FOREVER. `if (!fontsLoaded) return null` renders
   *  NOTHING until they load — so any font failure is a permanent BLACK SCREEN with a running
   *  process, no error on screen and nothing in the log. That is the worst shape a fault can take,
   *  and it is exactly what the first Apple TV build showed (2026-08-04).
   *  ★ useFonts returns an ERROR as its second value and it was being discarded, so a genuine
   *    failure was indistinguishable from "still loading".
   *  ★ Now: proceed after 3 s regardless. Text falls back to the system font — ugly on one screen,
   *    where before the whole app was invisible. */
  const [fontWaitOver, setFontWaitOver] = useState(false);
  useEffect(() => {
    const t = setTimeout(() => setFontWaitOver(true), 3000);
    /* ★★★ A TIMER IS NOT A GUARANTEE IN THE BACKGROUND, which is how the escape hatch above came
     *   to have a hole in it. iOS throttles and suspends JS timers in a backgrounded app — and
     *   this app is now routinely LAUNCHED backgrounded, headless, by a watch message. So on a
     *   Buddy-driven cold start the 3 s never elapses in any useful sense: fonts may not resolve
     *   either, the guard below renders null, no frame is ever drawn, and iOS goes on showing its
     *   own launch image. Which is systemBackgroundColor — PURE BLACK in dark mode, with nothing
     *   on it and nothing in the log, exactly as the note above predicted.
     * ★★ SO ALSO GIVE UP WAITING THE MOMENT THE APP COMES FORWARD. Foregrounding is the only
     *   instant that matters here: it is when somebody is actually looking, and it cannot be
     *   throttled away. Whatever the fonts are doing, they have had all the time they are getting.
     * ★ Both paths lead to the same flag, so this can only ever shorten the wait. */
    const sub = AppState.addEventListener('change', (st) => {
      if (st === 'active') setFontWaitOver(true);
    });
    return () => { clearTimeout(t); sub.remove(); };
  }, []);
  useEffect(() => {
    if (fontError) console.warn('[fonts] failed to load, continuing with system fonts:', fontError);
  }, [fontError]);

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

  /* ★★★ NEVER RENDER NOTHING. `return null` here was the last place in the app that could draw
   *   NO FRAME AT ALL, and a frame is what tells iOS to take its launch image down. Until one is
   *   drawn the user is looking at the system's black launch screen with a perfectly healthy app
   *   behind it — running, responsive, playing audio, feeding the watch, and showing nothing.
   *   That is the black screen Stuart has been reporting for weeks, and it is why there was never
   *   any text on it: CrashBoundary lives INSIDE the tree below, so returning early means even a
   *   real error has nowhere to be displayed.
   * ★★ THE FRAME COSTS US NOTHING. It is the app's own background colour — the same one the tree
   *   below paints — so a viewer sees no difference, but the launch image goes and we own the
   *   screen from the first moment. The font wait keeps its purpose (no flash of fallback type)
   *   and loses only its ability to leave the app invisible.
   * ★ Deliberately NOT the splash: this must not depend on state, fonts, or anything that can
   *   itself fail to resolve. A plain View is the one thing that cannot. */
  if (!fontsLoaded && !fontError && !fontWaitOver) {
    crumb('render: FONT GATE (plain frame)');
    return <View style={{ flex: 1, backgroundColor: '#080601' }} />;
  }
  crumb(`render: full tree (fonts=${fontsLoaded} err=${!!fontError} waitOver=${fontWaitOver})`);

  return (
    <GestureHandlerRootView style={{ flex: 1 }}>
      <ThemeProvider>
      <View style={{ flex: 1, backgroundColor: '#080601' }}>
        <CrashBoundary>
        <NavigationContainer
          ref={navigationRef}
          /* ★★★ WHAT THE NAVIGATOR ACTUALLY THINKS IT IS SHOWING, on every change.
           *   The native tree dump says the screen stack holds a screen with ~6 views in it where a
           *   healthy one has hundreds, and SDRScreen has no early return that could produce that —
           *   so the question is no longer "what is drawn" but "what route is mounted at all".
           *   This answers it in one line and costs nothing: navigation state changes are rare.
           * ★ Route NAMES only. Params carry a PIN on the VibeServer path and must never reach a
           *   log file the user may send us. */
          onStateChange={(st) => {
            try {
              const names = (st?.routes ?? []).map((r: { name: string }) => r.name).join(' > ');
              crumb(`nav: [${names}] index=${st?.index ?? '?'}`);
            } catch {}
          }}>
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
            backgroundColor: '#0A0A12', zIndex: 9999, opacity: splashOpacity,
          }}>
            {/* Centred block takes the space the notice doesn't need, so on short
                screens (e.g. 320x569dp FWVGA) it shrinks instead of overlapping. */}
            <View style={{ flex: 1, alignItems: 'center', justifyContent: 'center' }}>
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
            </View>

            {/* pointerEvents none: this block must never swallow taps meant for CONTINUE. */}
            <View pointerEvents="none" style={{ paddingBottom: 64, paddingTop: 8, paddingHorizontal: 28 }}>
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
