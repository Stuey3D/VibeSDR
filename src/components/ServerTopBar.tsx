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
import { remoteSettingsLine } from '../utils/serverChoice';

type Props = {
  C: any; F?: string;
  running: { ip: string; port: number } | null;
  mdnsHost: string;
  starting: boolean;
  /** ★★★ beta96 a START that was just refused: the one line naming what is missing, under the key (red). Never shown
   *  while browsing — only after the owner pressed START (Stuart 11-26). The key itself stays green. */
  refusal?: string;
  onStart: () => void;
  onStop: () => void;
  onServerType?: () => void;
  hasAdminPassword: boolean;
  remoteOn: boolean;
};

export { remoteSettingsLine };

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
        <ServerKey big first color={C.ok} dim={C.textDim} border={C.ok} fill={C.okBg} fontFamily={F}
          focused={hl === 0} onFocusHere={() => setHl(0)} testID="topbar-start"
          title="▶ START SERVER" onPress={p.onStart} />
      )}
      {!p.running && !!p.refusal && (
        <Text testID="topbar-refusal" style={{ color: C.danger, fontFamily: F, fontSize: 14, lineHeight: 19, marginTop: 8 }}>{p.refusal}</Text>
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
