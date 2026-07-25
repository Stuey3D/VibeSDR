/**
 * BrowserOverlay — full-screen in-app browser for the server's admin pages
 * (ADMIN / NOISE / CONDITIONS / LISTENERS — skin menu's Admin section).
 * Native "← SDR" bar with browser ‹ › history arrows (the admin pages are
 * multi-level); iOS also keeps edge-swipe back/forward inside the page, and
 * the Android back gesture navigates page history before closing the modal.
 * The pages are arbitrary server HTML, so unlike MapOverlay no chrome is
 * injected into the WebView itself.
 */

import React, { useRef, useState } from 'react';
import { NativeEventEmitter, NativeModules } from 'react-native';
import { useSuppressShortcuts } from './PanelNav';
import {
  ActivityIndicator, Modal, Share, StyleSheet, Text, TouchableOpacity, View,
} from 'react-native';
import { WebView } from 'react-native-webview';
import { SafeAreaView } from 'react-native-safe-area-context';

export interface BrowserOverlayProps {
  url:     string | null;
  title?:  string;
  onClose: () => void;
  /** Show a Save/Share button (OWRX files gallery — save SSTV/WEFAX images). */
  allowSave?: boolean;
  /** CSS injected into the page (e.g. hide the OWRX header to enlarge the map). */
  injectCSS?: string;
  /** Back-bar label (default "← SDR"; compatibility mode uses "← VibeSDR"). */
  backLabel?: string;
}

export default function BrowserOverlay({ url, title, onClose, allowSave, injectCSS, backLabel }: BrowserOverlayProps) {
  // ★★ THIS IS SOMEONE ELSE'S PAGE. Every VibeSDR keyboard and controller shortcut is
  // switched off for as long as it is SHOWING — see PanelNav. Firing our own actions under a
  // page we did not write is worse than doing nothing, because the user is looking at that
  // page and will read whatever happens as the page's doing.
  //
  // ★★★ GATED ON `url`, and that is the whole point. This component is rendered
  // UNCONDITIONALLY by SDRScreen for the admin pages and merely returns null when it has no
  // url (see below) — so an ungated call suppressed every shortcut in the app, permanently,
  // the moment the screen mounted. MOUNTED IS NOT THE SAME AS VISIBLE, and the failure mode
  // was silent and total: the keyboard simply stopped working everywhere. (2026-07-25.)
  useSuppressShortcuts(!!url);

  // ★ Say so, rather than being silently dead. Shown only if a key is actually pressed:
  // someone driving by touch never sees it, and someone reaching for the keyboard gets an
  // answer at the exact moment they ask the question. (Stuart, 2026-07-25.)
  const [keyNote, setKeyNote] = React.useState(false);
  React.useEffect(() => {
    if (!url) { setKeyNote(false); return; }   // same reason: no page, no listener
    const emitter = new NativeEventEmitter(NativeModules.VibePowerModule);
    const sub = emitter.addListener('VibeKeyDown', () => setKeyNote(true));
    return () => sub.remove();
  }, [url]);

  const webRef = useRef<WebView>(null);
  const [canBack, setCanBack] = useState(false);
  const [canFwd,  setCanFwd]  = useState(false);
  const [curUrl,  setCurUrl]  = useState(url);
  const [progress, setProgress] = useState(0);   // 0..1 page-load progress (Safari-style bar)
  const [loading,  setLoading]  = useState(true); // spinner while the page loads (slow-first-byte)
  if (!url) return null;
  // Hand the currently-open URL to the OS share sheet — for an image (a tapped
  // file in the OWRX gallery) iOS/Android offer "Save Image" / "Save to Files".
  const onSave = () => { Share.share({ url: curUrl ?? url }).catch(() => {}); };
  return (
    <Modal
      visible
      animationType="slide"
      supportedOrientations={['portrait', 'landscape']}
      // Android back gesture/button: walk page history first, close last
      onRequestClose={() => {
        if (canBack) webRef.current?.goBack();
        else onClose();
      }}
    >
      {/* SafeAreaView (native, measures the modal's own window) — the
          useSafeAreaInsets hook returns 0 inside an RN Modal, which clipped
          the bar under the Dynamic Island. */}
      <SafeAreaView style={styles.root} edges={['top']}
                    onTouchStart={() => setKeyNote(false)}>
        {/* ★ Only when a key is pressed — see keyNote. Sits under the bar so it reads as
            part of this page's chrome rather than an alert over the receiver's own UI. */}
        {keyNote && (
          <View style={styles.keyNote}>
            <Text style={styles.keyNoteText}>
              This is the receiver's own web page, so VibeSDR's keyboard and controller
              shortcuts are switched off here. To use compatibility mode please use the
              touchscreen — or the trackpad if you're on a Mac.
            </Text>
          </View>
        )}
        <View style={styles.bar}>
          <TouchableOpacity onPress={onClose} hitSlop={12} activeOpacity={0.7}>
            <Text style={styles.back}>{backLabel ?? '← SDR'}</Text>
          </TouchableOpacity>
          <Text style={styles.title} numberOfLines={1}>{title ?? url}</Text>
          {allowSave && (
            <TouchableOpacity onPress={onSave} hitSlop={10} activeOpacity={0.7}>
              <Text style={styles.save}>⤓ Save</Text>
            </TouchableOpacity>
          )}
          {/* Browser history arrows — multi-level admin pages */}
          <TouchableOpacity
            onPress={() => webRef.current?.goBack()}
            hitSlop={10} activeOpacity={0.7} disabled={!canBack}
          >
            <Text style={[styles.navArrow, !canBack && styles.navArrowDim]}>‹</Text>
          </TouchableOpacity>
          <TouchableOpacity
            onPress={() => webRef.current?.goForward()}
            hitSlop={10} activeOpacity={0.7} disabled={!canFwd}
          >
            <Text style={[styles.navArrow, !canFwd && styles.navArrowDim]}>›</Text>
          </TouchableOpacity>
        </View>
        {/* Safari-style load bar — on a slow receiver a blank WebView reads as "hung". */}
        {progress < 1 && (
          <View style={styles.progressTrack}>
            <View style={[styles.progressFill, { width: `${Math.max(3, progress * 100)}%` }]} />
          </View>
        )}
        <WebView
          ref={webRef}
          source={{ uri: url }}
          style={styles.web}
          allowsBackForwardNavigationGestures
          onLoadStart={() => { setProgress(0); setLoading(true); }}
          onLoadProgress={({ nativeEvent }) => setProgress(nativeEvent.progress)}
          // Inject after the page loads (injectedJavaScript-prop timing was
          // unreliable on the OWRX map). A MutationObserver re-applies it in case
          // the header mounts after load.
          onLoadEnd={() => {
            setProgress(1);
            setLoading(false);
            if (!injectCSS) return;
            const css = JSON.stringify(injectCSS);
            webRef.current?.injectJavaScript(
              `(function(){function a(){var id='vibe-inj';if(!document.getElementById(id)){var s=document.createElement('style');s.id=id;s.textContent=${css};(document.head||document.documentElement).appendChild(s);}}a();new MutationObserver(a).observe(document.documentElement,{childList:true,subtree:true});})();true;`,
            );
          }}
          onNavigationStateChange={(nav: { canGoBack: boolean; canGoForward: boolean; url: string }) => {
            setCanBack(nav.canGoBack);
            setCanFwd(nav.canGoForward);
            setCurUrl(nav.url);
          }}
        />
        {loading && (
          <View style={styles.spinnerWrap} pointerEvents="none">
            <ActivityIndicator size="large" color="#FFB833" />
            <Text style={styles.spinnerText}>Loading…</Text>
          </View>
        )}
      </SafeAreaView>
    </Modal>
  );
}

const styles = StyleSheet.create({
  // Amber, matching the app's own notices, and full-width so it cannot be mistaken for
  // part of the receiver's page underneath.
  keyNote: {
    backgroundColor: 'rgba(60,40,0,0.96)', paddingHorizontal: 12, paddingVertical: 8,
    borderBottomWidth: 1, borderBottomColor: 'rgba(255,200,0,0.45)',
  },
  keyNoteText: { color: '#ffe566', fontSize: 12, lineHeight: 17 },
  root:  { flex: 1, backgroundColor: '#000' },
  progressTrack: { height: 3, backgroundColor: 'rgba(255,160,0,0.12)' },
  progressFill:  { height: 3, backgroundColor: '#FFB833' },
  spinnerWrap:   { position: 'absolute', top: 60, left: 0, right: 0, bottom: 0, alignItems: 'center', justifyContent: 'center', gap: 12 },
  spinnerText:   { fontFamily: 'Courier', fontSize: 13, color: 'rgba(255,184,51,0.9)', letterSpacing: 1 },
  bar:   {
    flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between',
    paddingHorizontal: 14, paddingTop: 6, paddingBottom: 8, backgroundColor: '#0a0a0a',
    borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: 'rgba(255,255,255,0.18)',
    gap: 6,
  },
  back:  { color: '#ffe566', fontFamily: 'Atkinson Hyperlegible', fontSize: 16 },
  save:  { color: '#ffe566', fontFamily: 'Atkinson Hyperlegible', fontSize: 14, paddingHorizontal: 6 },
  title: {
    flex: 1, textAlign: 'center', paddingHorizontal: 8,
    color: 'rgba(255,255,255,0.85)', fontFamily: 'Atkinson Hyperlegible', fontSize: 15,
  },
  navArrow: {
    color: '#ffe566', fontSize: 26, lineHeight: 28,
    paddingHorizontal: 8, fontFamily: 'Atkinson Hyperlegible',
  },
  navArrowDim: { color: 'rgba(255,255,255,0.22)' },
  web:   { flex: 1, backgroundColor: '#000' },
});
