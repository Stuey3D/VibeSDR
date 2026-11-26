/**
 * ★★★ beta96 START / STOP AT THE TOP of the Full server screen (Stuart, 2026-11-26: "opening the app shows START SERVER
 * (or STOP SERVER when running) as the FIRST thing at the top, so nobody scrolls to the bottom"), with the line that says
 * how to change settings from another device, and the way back to the three server types.
 *
 * ★ A separate component so ServerModeScreen gains one line per view (its sections are b96-setupux's). The START here
 *   runs exactly what the bottom START runs — the screen hands in one `onStart` that does the same checks.
 * ★ The TV remote's focus starts on the big key (hasTVPreferredFocus), so OK on a fresh open starts or stops the server.
 * Lite-safe: React Native only.
 */
import React from 'react';
import { ActivityIndicator, Text, View } from 'react-native';
import ServerKey, { useKeyList } from './ServerKey';
import { setupAddressLine } from '../services/serverChoiceStore';

type Props = {
  C: any; F?: string;
  running: { ip: string; port: number } | null;
  mdnsHost: string;
  starting: boolean;
  /** The screen's own refusal (no admin password) — the key says it, in red, rather than starting. */
  startBlockedBy?: string;
  onStart: () => void;
  onStop: () => void;
  onServerType?: () => void;
  hasAdminPassword: boolean;
  remoteOn: boolean;
};

/** The status words, one rule for every server screen (Stuart, 2026-11-26 — shown only when remote access is on). */
export function remoteSettingsLine(o: { running: { ip: string; port: number } | null; mdnsHost: string;
                                        hasAdminPassword: boolean; remoteOn: boolean }): string {
  if (!o.running) return '';
  if (o.remoteOn && o.hasAdminPassword) return setupAddressLine(o.running.ip, o.running.port, o.mdnsHost);
  return 'Remote settings: off (turn on here to change settings from another device).';
}

export default function ServerTopBar(p: Props) {
  const { C, F } = p;
  const keys: Array<() => void> = [p.running ? p.onStop : p.onStart, ...(p.onServerType && !p.running ? [p.onServerType] : [])];
  const [hl, setHl] = useKeyList(keys.length, (i) => keys[i]?.());
  const line = remoteSettingsLine(p);
  return (
    <View testID="server-topbar">
      {p.running ? (
        <ServerKey big first color={C.danger} dim={C.textDim} border={C.danger} fill={C.dangerBg} fontFamily={F}
          focused={hl === 0} onFocusHere={() => setHl(0)} testID="topbar-stop"
          title="■ STOP SERVER" sub={`Serving at http://${p.running.ip}:${p.running.port}`} onPress={p.onStop} />
      ) : p.starting ? (
        <View style={{ padding: 22, alignItems: 'center' }}><ActivityIndicator color={C.ok} /></View>
      ) : (
        <ServerKey big first color={p.startBlockedBy ? C.danger : C.ok} dim={C.textDim}
          border={p.startBlockedBy ? C.danger : C.ok} fill={p.startBlockedBy ? C.dangerBg : C.okBg} fontFamily={F}
          focused={hl === 0} onFocusHere={() => setHl(0)} testID="topbar-start"
          title={p.startBlockedBy ? p.startBlockedBy : '▶ START SERVER'} onPress={p.onStart} />
      )}
      {!!line && (
        <Text testID="topbar-remote-line" style={{ color: C.textDim, fontFamily: F, fontSize: 13, lineHeight: 18, marginTop: 8 }}>{line}</Text>
      )}
      {!p.running && p.onServerType && (
        <ServerKey color={C.key} dim={C.textDim} border={C.border} fontFamily={F} focused={hl === 1} onFocusHere={() => setHl(1)}
          title="‹ Server type" sub="VibeServer Simple, Full or RTL-TCP." onPress={p.onServerType} />
      )}
    </View>
  );
}
