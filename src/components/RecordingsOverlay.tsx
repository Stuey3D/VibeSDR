/**
 * RecordingsOverlay — in-app browser for saved audio recordings.
 *
 * Recordings are written by the native engine as VibeSDR_<date>_<freq>_<mode>.m4a
 * into the app's document directory (iOS: Documents, visible in Files; Android:
 * app filesDir — see VibeStreamService). They used to be unreachable once you
 * dismissed the share sheet — this screen lists them so you can listen / share /
 * delete.
 *
 * Playback uses expo-audio. The live SDR is paused while this is open (see
 * onActiveChange) so the two don't fight over the audio session — the parent
 * mutes/pauses on open and resumes on close.
 */
import React, { useCallback, useEffect, useState, useRef } from 'react';
import {
  ActivityIndicator, Alert, FlatList, Modal, NativeModules, Platform,
  Pressable, StyleSheet, Text, TouchableOpacity, View,
} from 'react-native';
import { useRepeatingKeys, NAV_REPEAT_KEYS, useKeyboardMode } from './PanelNav';
import { SafeAreaProvider, SafeAreaView } from 'react-native-safe-area-context';
import * as FileSystem from 'expo-file-system/legacy';
import { useAudioPlayer, useAudioPlayerStatus, setAudioModeAsync } from 'expo-audio';

const REC = NativeModules.VibePowerModule as { shareRecording?: (path: string) => void };

interface Rec {
  name: string;
  uri: string;        // file:// uri
  path: string;       // bare filesystem path (for native share)
  size: number;       // bytes
  mtime: number;      // epoch seconds
  freq: string;       // "7.1500MHz"
  mode: string;       // "LSB"
  when: string;       // "23 Jun 2026 14:02"
}

const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

// VibeSDR_2026-06-23T14-02-11_7.1500MHz_LSB.m4a
function parseRec(name: string, uri: string, size: number, mtime: number): Rec {
  const base = name.replace(/\.m4a$/i, '');
  const parts = base.split('_');
  const freq = parts[2] ?? '';
  const mode = (parts[3] ?? '').toUpperCase();
  let when = '';
  const d = parts[1];                       // 2026-06-23T14-02-11
  const m = d?.match(/^(\d{4})-(\d{2})-(\d{2})T(\d{2})-(\d{2})/);
  if (m) when = `${+m[3]} ${MONTHS[+m[2] - 1]} ${m[1]} ${m[4]}:${m[5]}`;
  return { name, uri, path: uri.replace(/^file:\/\//, ''), size, mtime, freq, mode, when };
}

function fmtSize(b: number): string {
  if (b >= 1_048_576) return (b / 1_048_576).toFixed(1) + ' MB';
  if (b >= 1024) return Math.round(b / 1024) + ' KB';
  return b + ' B';
}
function fmtTime(s: number): string {
  if (!isFinite(s) || s < 0) s = 0;
  const m = Math.floor(s / 60);
  const sec = Math.floor(s % 60);
  return `${m}:${sec.toString().padStart(2, '0')}`;
}

export interface RecordingsOverlayProps {
  visible: boolean;
  onClose: () => void;
  /** Fired true on open / false on close so the parent can pause+mute the live
   *  SDR while a recording plays, then resume. */
  onActiveChange?: (active: boolean) => void;
}

export default function RecordingsOverlay({ visible, onClose, onActiveChange }: RecordingsOverlayProps) {
  const [recs, setRecs] = useState<Rec[] | null>(null);
  const [sel, setSel] = useState<string | null>(null);    // uri of the selected/playing rec
  const [trackW, setTrackW] = useState(0);                // seek-bar width of the open row
  const player = useAudioPlayer();
  const status = useAudioPlayerStatus(player);

  // Play even when the ringer switch is silent (it's a deliberate action).
  useEffect(() => { setAudioModeAsync({ playsInSilentMode: true }).catch(() => {}); }, []);

  const load = useCallback(async () => {
    setRecs(null);
    try {
      const dir = FileSystem.documentDirectory ?? '';
      const names = await FileSystem.readDirectoryAsync(dir);
      const list: Rec[] = [];
      for (const n of names) {
        if (!/^VibeSDR_.*\.m4a$/i.test(n)) continue;
        const uri = dir + n;
        const info = await FileSystem.getInfoAsync(uri);
        if (!info.exists) continue;
        list.push(parseRec(n, uri, (info as any).size ?? 0, (info as any).modificationTime ?? 0));
      }
      list.sort((a, b) => b.mtime - a.mtime);   // newest first
      setRecs(list);
    } catch {
      setRecs([]);
    }
  }, []);

  // Open/close lifecycle: tell the parent to pause the SDR, (re)load the list.
  useEffect(() => {
    if (visible) { onActiveChange?.(true); load(); }
    else { try { player.pause(); } catch {} setSel(null); onActiveChange?.(false); }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [visible]);

  const togglePlay = useCallback((rec: Rec) => {
    if (sel === rec.uri) {
      if (status.playing) player.pause();
      else player.play();
      return;
    }
    setSel(rec.uri);
    try {
      player.replace({ uri: rec.uri });
      player.seekTo(0);
      player.play();
    } catch {}
  }, [sel, status.playing, player]);

  const share = useCallback(async (rec: Rec) => {
    try {
      // Android: bare filesDir paths aren't shareable — wrap in Expo's content
      // provider. iOS: the native share takes the Documents path directly.
      if (Platform.OS === 'android') {
        const cu = await FileSystem.getContentUriAsync(rec.uri);
        REC.shareRecording?.(cu);
      } else {
        REC.shareRecording?.(rec.path);
      }
    } catch {}
  }, []);

  const remove = useCallback((rec: Rec) => {
    Alert.alert('Delete recording', `Delete "${rec.freq} ${rec.mode}" (${rec.when})?`, [
      { text: 'Cancel', style: 'cancel' },
      { text: 'Delete', style: 'destructive', onPress: async () => {
        try {
          if (sel === rec.uri) { try { player.pause(); } catch {} setSel(null); }
          await FileSystem.deleteAsync(rec.uri, { idempotent: true });
        } catch {}
        load();
      } },
    ]);
  }, [sel, player, load]);

  // ── Keyboard ────────────────────────────────────────────────────────────────
  //
  // ★ Its own handler rather than the shared list navigation, because the key map here is
  // deliberately different: SPACE plays, and BACKSPACE deletes. Enter is not used at all —
  // it opens the frequency box everywhere else in the app, and a list where Enter meant
  // "play" would be the same context-dependent key that has caused most of the trouble.
  //
  // ★ Delete is accepted alongside Backspace: a Mac keyboard has no forward-delete key, so
  // offering only one of the two is unusable on half the hardware.
  //
  // ★ SHARE is deliberately absent. It hands off to the system share sheet, which is not ours
  // to drive — the same reason share is left off the keyboard path in the frequency card.
  const kbInUse = useKeyboardMode();
  const [navIdx, setNavIdx] = useState(0);
  const listRef = useRef<FlatList<Rec> | null>(null);
  const kb = useRef({ recs, navIdx, togglePlay: (_r: Rec) => {}, remove: (_r: Rec) => {}, onClose });
  kb.current = { recs, navIdx, togglePlay, remove, onClose };

  useEffect(() => { if (visible) setNavIdx(0); }, [visible]);

  useRepeatingKeys(visible, (k: string) => {
    const { recs: all, navIdx: i } = kb.current;
    const rs = all ?? [];
    if (k === 'Escape') { kb.current.onClose(); return; }
    if (!rs.length) return;
    if (k === 'ArrowUp' || k === 'ArrowDown') {
      const next = Math.max(0, Math.min(rs.length - 1, i + (k === 'ArrowDown' ? 1 : -1)));
      setNavIdx(next);
      try { listRef.current?.scrollToIndex({ index: next, viewPosition: 0.5, animated: true }); } catch {}
      return;
    }
    if (k === 'Space') { const r = rs[i]; if (r) kb.current.togglePlay(r); return; }
    if (k === 'Backspace' || k === 'Delete') { const r = rs[i]; if (r) kb.current.remove(r); }
  }, NAV_REPEAT_KEYS);

  const renderItem = useCallback(({ item, index }: { item: Rec; index: number }) => {
    const navOn = index === navIdx;
    const isSel = sel === item.uri;
    const playing = isSel && status.playing;
    const dur = isSel ? status.duration : 0;
    const cur = isSel ? status.currentTime : 0;
    const frac = isSel && dur > 0 ? Math.min(1, cur / dur) : 0;
    return (
      <View style={[styles.row, isSel && styles.rowSel,
                    navOn && { borderColor: '#7CFF9B', borderWidth: 2 }]}>
        <View style={styles.rowTop}>
          <TouchableOpacity style={styles.playBtn} onPress={() => togglePlay(item)} hitSlop={8}>
            <Text style={styles.playIcon}>{playing ? '❚❚' : '▶'}</Text>
          </TouchableOpacity>
          <View style={styles.meta}>
            <Text style={styles.freq} numberOfLines={1}>{item.freq}  <Text style={styles.mode}>{item.mode}</Text></Text>
            <Text style={styles.sub} numberOfLines={1}>{item.when} · {fmtSize(item.size)}</Text>
          </View>
          <TouchableOpacity style={styles.actBtn} onPress={() => share(item)} hitSlop={6}>
            <Text style={styles.actIcon}>⤴</Text>
          </TouchableOpacity>
          <TouchableOpacity style={styles.actBtn} onPress={() => remove(item)} hitSlop={6}>
            <Text style={[styles.actIcon, styles.del]}>🗑</Text>
          </TouchableOpacity>
        </View>
        {isSel && (
          <View style={styles.progWrap}>
            <Pressable
              style={styles.progTrack}
              onLayout={(e) => setTrackW(e.nativeEvent.layout.width)}
              onPress={(e) => {
                if (dur > 0 && trackW > 0) {
                  player.seekTo((e.nativeEvent.locationX / trackW) * dur);
                }
              }}
            >
              <View style={[styles.progFill, { width: `${frac * 100}%` }]} />
            </Pressable>
            <Text style={styles.time}>{fmtTime(cur)} / {fmtTime(dur)}</Text>
          </View>
        )}
      </View>
    );
  }, [sel, status.playing, status.duration, status.currentTime, trackW, togglePlay, share, remove, player]);

  return (
    <Modal visible={visible} animationType="slide" onRequestClose={onClose} transparent={false}>
      {/* Modals render outside the app's SafeAreaProvider, so SafeAreaView reads
          zero insets (title drew under the status bar / notch). Own provider fixes it. */}
      <SafeAreaProvider>
      <SafeAreaView style={styles.root} edges={['top', 'bottom']}>
        <View style={styles.bar}>
          <Text style={styles.title}>RECORDINGS</Text>
          <TouchableOpacity onPress={onClose} hitSlop={10}><Text style={styles.close}>✕</Text></TouchableOpacity>
        </View>
        {/* ★ Keyboard hint in the header, only while a keyboard is in use — the same shape as
            the server list's subtitle. Share is absent because it hands off to the system
            sheet, which is not ours to drive. */}
        {kbInUse && (
          <Text style={styles.kbHint}>
            ↑↓ select · space play/pause · ⌫ delete · esc close
          </Text>
        )}
        {recs == null ? (
          <View style={styles.center}><ActivityIndicator color="#3ddc84" /></View>
        ) : recs.length === 0 ? (
          <View style={styles.center}>
            <Text style={styles.empty}>No saved recordings</Text>
            <Text style={styles.emptySub}>Recordings you make with the ⏺ button appear here.</Text>
          </View>
        ) : (
          <FlatList
            ref={listRef}
            data={recs}
            extraData={navIdx}
            keyExtractor={(r) => r.uri}
            renderItem={renderItem}
            contentContainerStyle={{ paddingVertical: 8 }}
          />
        )}
      </SafeAreaView>
      </SafeAreaProvider>
    </Modal>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#0a0d0b' },
  bar: {
    flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between',
    paddingHorizontal: 16, paddingVertical: 12,
    borderBottomWidth: 1, borderBottomColor: 'rgba(61,220,132,0.25)',
  },
  title: { color: '#3ddc84', fontSize: 16, fontWeight: '800', letterSpacing: 2 },
  close: { color: '#ddd', fontSize: 20, fontWeight: '700' },
  center: { flex: 1, alignItems: 'center', justifyContent: 'center', padding: 24 },
  empty: { color: '#ccc', fontSize: 16, fontWeight: '700' },
  kbHint: { color: '#8fa', fontSize: 10, letterSpacing: 0.5, opacity: 0.85,
            paddingHorizontal: 16, paddingBottom: 8 },
  emptySub: { color: '#888', fontSize: 13, marginTop: 8, textAlign: 'center' },
  row: {
    marginHorizontal: 12, marginVertical: 4, padding: 10,
    backgroundColor: 'rgba(255,255,255,0.04)', borderRadius: 10,
    borderWidth: 1, borderColor: 'transparent',
  },
  rowSel: { borderColor: 'rgba(61,220,132,0.6)', backgroundColor: 'rgba(61,220,132,0.07)' },
  rowTop: { flexDirection: 'row', alignItems: 'center' },
  playBtn: {
    width: 38, height: 38, borderRadius: 19, marginRight: 10,
    alignItems: 'center', justifyContent: 'center',
    backgroundColor: 'rgba(61,220,132,0.15)', borderWidth: 1, borderColor: '#3ddc84',
  },
  playIcon: { color: '#3ddc84', fontSize: 14, fontWeight: '800' },
  meta: { flex: 1, minWidth: 0 },
  freq: { color: '#fff', fontSize: 15, fontWeight: '700' },
  mode: { color: '#ffb833', fontSize: 13, fontWeight: '700' },
  sub: { color: '#9aa', fontSize: 12, marginTop: 2 },
  actBtn: { paddingHorizontal: 8, paddingVertical: 6, marginLeft: 2 },
  actIcon: { color: '#bcd', fontSize: 18 },
  del: { color: '#ff7676' },
  progWrap: { marginTop: 10, flexDirection: 'row', alignItems: 'center' },
  progTrack: {
    flex: 1, height: 6, borderRadius: 3, marginRight: 10,
    backgroundColor: 'rgba(255,255,255,0.12)', overflow: 'hidden',
  },
  progFill: { height: 6, backgroundColor: '#3ddc84' },
  time: { color: '#9aa', fontSize: 11, width: 78, textAlign: 'right' },
});
