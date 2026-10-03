/**
 * useDeepLinks — cold/warm-start wiring for `vibesdr://` links.
 *
 * Mounted once at the app root. Handles both entry points:
 *   - cold start: Linking.getInitialURL() (app launched by the link)
 *   - warm start: Linking 'url' event (app already running)
 *
 * A link that arrives before the app is ready (fonts/splash/init) is queued and
 * drained once `ready` flips true, so we never navigate mid-splash. Duplicate
 * deliveries (Android fires both getInitialURL and the event on some launch
 * paths) are de-duped within a short window.
 */

import { useEffect, useRef } from 'react';
import { Alert, Linking, ToastAndroid, Platform } from 'react-native';
import { CommonActions } from '@react-navigation/native';

import { navigationRef } from '../../App';
import { getViewMode } from '../services/viewMode';
import { parseVibeSdrUrl, resolveRequest, type ResolvedTarget } from './DeepLinkHandler';
import { parseSdrUrl } from './SdrLinkHandler';
import { markDeepLinkActive, markInitialLinkChecked } from './deepLinkState';
import { parseUrlStrict, SERVER_SCHEMES } from '../utils/safeUrl';

function toast(msg: string) {
  if (Platform.OS === 'android') ToastAndroid.show(msg, ToastAndroid.LONG);
  else Alert.alert('VibeSDR', msg);
}

/**
 * Reset the stack to the picker with an `autoSpy` param — the picker's effect
 * runs connectSpy() (which pushes the SDR screen itself once the native shim
 * answers). We deliberately do NOT synthesise a ResolvedTarget: the resolve
 * pipeline rejects non-URL backends, and the SpyServer nav params only exist
 * after the shim starts. Single-route reset so a failed connect leaves the user
 * cleanly on the picker.
 */
function goToSpy(host: string, port: number) {
  if (!navigationRef.isReady()) return;
  navigationRef.dispatch(CommonActions.reset({
    index: 0,
    routes: [{ name: 'InstancePicker', params: { autoSpy: { host, port } } }],
  }));
}

/** Reset the stack to the SDR screen for a resolved target (fresh mount). */
async function goToTarget(target: ResolvedTarget) {
  if (!navigationRef.isReady()) return;
  const viewMode = await getViewMode();
  navigationRef.dispatch(CommonActions.reset({
    index: 1,
    routes: [
      { name: 'InstancePicker' },
      {
        name: 'SDR',
        params: {
          baseUrl:      target.baseUrl,
          instanceName: target.instanceName,
          serverType:   target.serverType,
          viewMode,
          deepLink:     true,
          initialFreq:  target.freq,
          initialMode:  target.mode,
          initialZoom:  target.zoom,
        },
      },
    ],
  }));
}

export function useDeepLinks(ready: boolean) {
  const pending    = useRef<string | null>(null);
  const lastUrl    = useRef<string>('');
  const lastAt     = useRef<number>(0);
  const readyRef   = useRef(ready);
  readyRef.current = ready;

  // Parse → resolve → (confirm if needed) → navigate.
  const process = async (url: string) => {
    // sdr:// SpyServer links — arbitrary third-party host, opens a raw TCP
    // socket, so ALWAYS confirm (stricter than vibesdr://, which auto-connects).
    if (/^sdr:\/\//i.test(url)) {
      const t = parseSdrUrl(url);
      if (!t) { toast('Invalid SpyServer link'); return; }
      const onSDR = navigationRef.getCurrentRoute?.()?.name === 'SDR';
      Alert.alert(
        'Connect to SpyServer?',
        `${t.host}:${t.port}\n\nSpyServer links come from third-party sources — only connect to servers you trust.`
          + (onSDR ? '\nThis will disconnect your current session.' : ''),
        [
          { text: 'Cancel', style: 'cancel' },
          { text: 'Connect', onPress: () => goToSpy(t.host, t.port) },
        ],
      );
      return;
    }

    const req = parseVibeSdrUrl(url);
    if (!req) { toast('Invalid VibeSDR link'); return; }
    const res = await resolveRequest(req);
    if (!res.ok) { toast(res.reason); return; }

    // Cold start (nothing to interrupt) → connect directly. If we're already on
    // an SDR session, confirm before tearing it down.
    /* ★★★ A url= LINK ALWAYS ASKS, COLD START INCLUDED (security, 2026-10-03). It names an arbitrary
     *  host — a QR code, a forum post — and connecting tells that host our address and session. Only the
     *  uuid form, which can only name a server already in the directory, still connects straight away.
     *  The HOST is shown, not the link's own name for itself, as the sdr:// path already does. */
    const onSDR = navigationRef.getCurrentRoute?.()?.name === 'SDR';
    if (req.url) {
      const host = parseUrlStrict(res.target.baseUrl, SERVER_SCHEMES)?.host || res.target.baseUrl;
      Alert.alert(
        'Open server from link?',
        `${host}\n\nLinks can come from anyone — only connect to servers you trust.`
          + (onSDR ? '\nThis will disconnect your current session.' : ''),
        [
          { text: 'Cancel', style: 'cancel' },
          { text: 'Connect', onPress: () => { void goToTarget(res.target); } },
        ],
      );
    } else if (onSDR) {
      Alert.alert(
        'Open server from link?',
        `Connect to ${res.target.instanceName}?\nThis will disconnect your current session.`,
        [
          { text: 'Cancel', style: 'cancel' },
          { text: 'Connect', onPress: () => { void goToTarget(res.target); } },
        ],
      );
    } else {
      void goToTarget(res.target);
    }
  };

  const handle = (url: string | null) => {
    if (!url || !/^(vibesdr|sdr):\/\//i.test(url)) return;
    const now = Date.now();
    if (url === lastUrl.current && now - lastAt.current < 2000) return; // dedup
    lastUrl.current = url;
    lastAt.current  = now;
    // A deep link now owns this launch — stop the picker auto-connecting to the
    // user's default instance (which would otherwise stomp the link's target).
    markDeepLinkActive();
    if (!readyRef.current) { pending.current = url; return; } // queue until ready
    void process(url);
  };

  // Cold start + warm-start listener. The picker blocks its default-instance
  // auto-connect until this probe answers (link or not), so ALWAYS mark it
  // checked — including on null/throw, which is the common "no link" case.
  useEffect(() => {
    Linking.getInitialURL()
      .then(handle)
      .catch(() => {})
      .finally(markInitialLinkChecked);
    const sub = Linking.addEventListener('url', (e) => handle(e.url));
    return () => sub.remove();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Drain a queued link once the app reaches its ready state.
  useEffect(() => {
    if (ready && pending.current) {
      const url = pending.current;
      pending.current = null;
      void process(url);
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ready]);
}
