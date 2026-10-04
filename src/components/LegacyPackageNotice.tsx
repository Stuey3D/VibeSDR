/**
 * ★★★ "VibeSDR HAS A NEW HOME" — the LEGACY Android build only (BRIEF-android-package-migration, 2026-10-04).
 *
 * The Play listing moves to net.vibesdr.app and GitHub follows it; for one release GitHub also ships the old id,
 * com.vibesdr.app, labelled "VibeSDR (legacy)", as its FINAL update. That build says so: once, on the first launch of
 * this version, dismissible — and the menu keeps a quiet link back to it (openLegacyNotice).
 *  ★ It links straight to the bookmark EXPORT that already exists (the BOOKMARKS card's EXPORT JSON — the same
 *    exportBookmarksJSON), so moving across is one tap here and IMPORT in the new app. Other settings have no export.
 *  ★ The net.vibesdr.app build reads isLegacyPackage === false and never draws this. iOS never has the constant.
 */
import React, { useEffect, useState } from 'react';
import { Alert, DeviceEventEmitter, Modal, NativeModules, Platform, Share, StyleSheet, Text, TouchableOpacity, View } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { exportBookmarksJSON, loadUserBookmarks } from '../services/userBookmarks';
import { APP_VERSION } from '../constants/version';

export const isLegacyPackage: boolean =
  Platform.OS === 'android' && (NativeModules as any).VibeLocalSDR?.isLegacyPackage === true;

const SEEN_KEY = `lsv_legacy_notice_seen_${APP_VERSION}`;
const OPEN_EVENT = 'vibesdr.openLegacyNotice';

/** The menu's quiet link. */
export function openLegacyNotice(): void { DeviceEventEmitter.emit(OPEN_EVENT); }

export default function LegacyPackageNotice() {
  const [open, setOpen] = useState(false);
  useEffect(() => {
    if (!isLegacyPackage) return;
    AsyncStorage.getItem(SEEN_KEY).then((v) => { if (!v) setOpen(true); }).catch(() => setOpen(true));
    const sub = DeviceEventEmitter.addListener(OPEN_EVENT, () => setOpen(true));
    return () => sub.remove();
  }, []);
  if (!isLegacyPackage) return null;

  const close = () => { setOpen(false); AsyncStorage.setItem(SEEN_KEY, '1').catch(() => {}); };
  const exportBookmarks = async () => {
    const list = await loadUserBookmarks().catch(() => []);
    if (!list.length) { Alert.alert('Bookmarks', 'You have no bookmarks to move across.'); return; }
    Share.share({ message: exportBookmarksJSON(list) }).catch(() => {});
  };

  return (
    <Modal visible={open} transparent animationType="fade" onRequestClose={close}>
      <View style={st.backdrop}>
        <View style={st.card} accessibilityRole="alert">
          <Text style={st.title}>VibeSDR has a new home.</Text>
          <Text style={st.body}>
            Future updates come only to the new VibeSDR app (net.vibesdr.app), from GitHub and soon Google Play.
            This version won't be updated again.
          </Text>
          <Text style={st.body}>Install the new app, move your bookmarks across, then uninstall this one.</Text>
          <Text style={st.small}>
            EXPORT MY BOOKMARKS shares them as a file or text; in the new app open the frequency card, BOOKMARKS,
            and use IMPORT. Other settings start fresh.
          </Text>
          <TouchableOpacity style={[st.btn, st.primary]} onPress={exportBookmarks} accessibilityRole="button">
            <Text style={[st.btnTxt, st.primaryTxt]}>EXPORT MY BOOKMARKS</Text>
          </TouchableOpacity>
          <TouchableOpacity style={st.btn} onPress={close} accessibilityRole="button">
            <Text style={st.btnTxt}>CLOSE</Text>
          </TouchableOpacity>
        </View>
      </View>
    </Modal>
  );
}

const st = StyleSheet.create({
  backdrop:   { flex: 1, backgroundColor: 'rgba(0,0,0,0.78)', justifyContent: 'center', padding: 22 },
  card:       { backgroundColor: '#14110a', borderRadius: 14, borderWidth: 1, borderColor: '#5a4520', padding: 20,
                maxWidth: 520, width: '100%', alignSelf: 'center', gap: 10 },
  title:      { color: '#ffb833', fontFamily: 'Atkinson Hyperlegible', fontSize: 19, fontWeight: '700' },
  body:       { color: '#e8dcc0', fontFamily: 'Atkinson Hyperlegible', fontSize: 15, lineHeight: 21 },
  small:      { color: '#b8a885', fontFamily: 'Atkinson Hyperlegible', fontSize: 13, lineHeight: 18 },
  btn:        { borderWidth: 1, borderColor: '#5a4520', borderRadius: 10, paddingVertical: 12, alignItems: 'center' },
  btnTxt:     { color: '#ffb833', fontFamily: 'Atkinson Hyperlegible', fontSize: 15, fontWeight: '700', letterSpacing: 1.5 },
  primary:    { backgroundColor: '#ffb833', borderColor: '#ffb833' },
  primaryTxt: { color: '#14110a' },
});
