/**
 * ★★★ CHECK FOR UPDATES — VibeServer Lite only (Stuart, 2026-10-10).
 *
 * A quiet link beside "Export logs…" on the server screen, in both its views. It draws NOTHING unless the native
 * LiteUpdate module exists, which is only in Lite (Play forbids self-update, so the main app never has it).
 *
 *   Check for updates → "You have the latest (RC37)"  |  "RC38 is available — 45 MB"  [Update]
 *   Update → (first time on Android 8+: "Allow VibeServer Lite to install updates" → Android's own switch)
 *          → "Downloading… 12 of 45 MB" → Android asks "Install an update to this app?" → the new build starts,
 *            and VibeUpdateReceiver puts a running server back.
 * ★ Nothing on a timer, no data used until pressed. ★ Every step says what it is doing and every failure says why.
 */
import React, { useEffect, useRef, useState } from 'react';
import { AppState, DeviceEventEmitter, NativeModules, Text, TouchableOpacity, View } from 'react-native';
import { newerLiteRelease, liteVersionLabel, type LiteRelease } from '../services/liteUpdate';

const LU: any = (NativeModules as any).LiteUpdate;
const MB = (n: number) => (n / 1e6).toFixed(n >= 10e6 ? 0 : 1);

type St =
  | { k: 'idle' } | { k: 'checking' } | { k: 'latest'; label: string } | { k: 'error'; msg: string }
  | { k: 'available'; rel: LiteRelease; label: string } | { k: 'permission'; rel: LiteRelease; label: string }
  | { k: 'downloading'; got: number; total: number; label: string } | { k: 'installing'; label: string };

export default function LiteUpdate({ C, F }: { C: any; F?: string }) {
  const [st, setSt] = useState<St>({ k: 'idle' });
  const stRef = useRef(st); stRef.current = st;

  useEffect(() => {
    if (!LU) return;
    const p = DeviceEventEmitter.addListener('LiteUpdateProgress', (e: any) => {
      const s = stRef.current;
      if (s.k === 'downloading') setSt({ ...s, got: Number(e?.got) || 0, total: Number(e?.total) || s.total });
    });
    const d = DeviceEventEmitter.addListener('LiteUpdateStatus', (e: any) => {
      // ★ Success replaces this process, so what arrives here is a cancel or a failure.
      if (!e?.ok) setSt({ k: 'error', msg: e?.status === 3 ? 'The update was cancelled — nothing changed.'
                                                       : `Android did not install it${e?.message ? ': ' + e.message : '.'}` });
    });
    // ★ Back from Android's "Install unknown apps" switch: carry on if it was turned on.
    const a = AppState.addEventListener('change', async (s) => {
      const cur = stRef.current;
      if (s === 'active' && cur.k === 'permission' && await LU.canInstall().catch(() => false)) void startDownload(cur.rel, cur.label);
    });
    return () => { p.remove(); d.remove(); a.remove(); };
  }, []);

  if (!LU) return null;

  const check = async () => {
    setSt({ k: 'checking' });
    try {
      const cur = await LU.current();
      const list: LiteRelease[] = JSON.parse(await LU.check());
      const best = newerLiteRelease(list, String(cur?.versionName ?? ''));
      if (!best) {
        const mine = String(cur?.versionName ?? '').replace(/-lite$/, '').replace('~', ' ').toUpperCase();
        setSt({ k: 'latest', label: mine });
        return;
      }
      setSt({ k: 'available', rel: best, label: liteVersionLabel(best.v) });
    } catch (e: any) {
      setSt({ k: 'error', msg: `Could not reach GitHub — ${String(e?.message ?? e)}` });
    }
  };

  const startDownload = async (rel: LiteRelease, label: string) => {
    if (!(await LU.canInstall().catch(() => true))) { setSt({ k: 'permission', rel, label }); return; }
    setSt({ k: 'downloading', got: 0, total: rel.size, label });
    try {
      const path = await LU.download(rel.apkUrl, rel.size);
      setSt({ k: 'installing', label });
      await LU.install(path);
    } catch (e: any) {
      setSt({ k: 'error', msg: `The update did not download — ${String(e?.message ?? e)}` });
    }
  };

  const dim = { color: C.textDim, fontFamily: F, fontSize: 13 };
  const link = (label: string, onPress: () => void) => (
    <TouchableOpacity onPress={onPress} accessibilityRole="button" hitSlop={{ top: 10, bottom: 10, left: 12, right: 12 }}
                      style={{ alignSelf: 'center', paddingVertical: 8, paddingHorizontal: 12 }}>
      <Text style={[dim, { textDecorationLine: 'underline' }]}>{label}</Text>
    </TouchableOpacity>
  );
  const button = (label: string, onPress: () => void) => (
    <TouchableOpacity onPress={onPress} accessibilityRole="button"
                      style={{ alignSelf: 'center', marginTop: 8, paddingVertical: 10, paddingHorizontal: 22,
                               borderWidth: 1, borderColor: C.gold, borderRadius: 8 }}>
      <Text style={{ color: C.gold, fontFamily: F, fontSize: 15 }}>{label}</Text>
    </TouchableOpacity>
  );
  const line = (text: string) => <Text style={[dim, { textAlign: 'center', marginTop: 4, paddingHorizontal: 16 }]}>{text}</Text>;

  switch (st.k) {
    case 'idle': return link('Check for updates', check);
    case 'checking': return <View>{line('Checking GitHub for a newer VibeServer Lite…')}</View>;
    case 'latest': return <View>{line(`You have the latest${st.label ? ` (${st.label})` : ''}.`)}</View>;
    case 'error': return <View>{line(st.msg)}{link('Try again', check)}</View>;
    case 'available': return (
      <View style={{ marginTop: 6 }}>
        {line(`VibeServer Lite ${st.label} is available — ${MB(st.rel.size)} MB.`)}
        {line('Android asks before it installs. A running server restarts on the new version; listeners reconnect.')}
        {button(`Update to ${st.label}`, () => { void startDownload(st.rel, st.label); })}
      </View>);
    case 'permission': return (
      <View style={{ marginTop: 6 }}>
        {line('First, allow VibeServer Lite to install updates. Turn on the switch on the next screen, then come back here.')}
        {button('Allow updates', () => { LU.openInstallPermission().catch(() => {}); })}
      </View>);
    case 'downloading': return <View>{line(`Downloading ${st.label}… ${MB(st.got)} of ${MB(st.total)} MB`)}</View>;
    case 'installing': return <View>{line(`Installing ${st.label} — confirm on the screen Android shows.`)}</View>;
  }
}
