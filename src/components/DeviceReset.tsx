/**
 * ★★★ beta96 RESET ALL SETTINGS AND START AGAIN — at the very bottom of the device's own server screen (Stuart, 2026-11-26).
 *
 * Two deliberate steps so a slow TV cannot do it by accident: the key opens a box asking for the ADMIN PASSWORD (or the
 * word RESET where none was ever set — and an owner who forgot the password uses "Change admin password" first, which
 * needs no old one), then an "Are you sure?" listing exactly what goes. Device only: the /setup page never offers it and
 * the server refuses it there.
 * ★ The gallery and stored decodes are KEPT unless the owner ticks "also delete" (off by default) — they are the owner's
 *   catches, not settings, and a reset is about starting the setup again.
 */
import React, { useState } from 'react';
import { Alert, Switch, Text, View } from 'react-native';
import TextInput from './TvTextInput';
import ServerKey from './ServerKey';
import type { CredInk } from './DeviceCredentials';
import { resetAllServerSettings } from '../services/serverChoiceStore';

export const RESET_WIPES = 'Every server setting goes: the radio settings (gain, bias-T, calibration, SDRplay and Icom cards), '
  + 'the admin password and PIN, the server name and address, public listing on vibesdr.net, the community map and feeds, '
  + 'remote access to /setup, the landing station, background decoding and the DX scanner, and the server type you chose. '
  + 'Your listening favourites and the app’s own settings are kept.';

type Props = { ink: CredInk; hasPassword: boolean; danger: string; onDone: () => void };

export default function DeviceReset({ ink, hasPassword, danger, onDone }: Props) {
  const [open, setOpen] = useState(false);
  const [pw, setPw] = useState('');
  const [wipeData, setWipeData] = useState(false);
  const [msg, setMsg] = useState('');
  const F = ink.fontFamily;
  if (!open) {
    return (
      <ServerKey color={danger} dim={ink.textDim} border={danger} fontFamily={F} testID="reset-open" style={{ marginTop: 28 }}
        title="RESET ALL SETTINGS AND START AGAIN" sub="Back to a first-time setup. Asks for the admin password."
        onPress={() => { setOpen(true); setMsg(''); setPw(''); }} />
    );
  }
  const go = () => {
    if (!pw) { setMsg(hasPassword ? 'Type the admin password.' : 'Type RESET.'); return; }
    Alert.alert('Are you sure?', 'This erases every setting. ' + RESET_WIPES
      + (wipeData ? '\n\nThe gallery, the DX log and the learned bookmarks are deleted too.' : '\n\nThe gallery and stored decodes are kept.'),
      [{ text: 'Cancel', style: 'cancel' },
       { text: 'Erase everything', style: 'destructive', onPress: async () => {
           try {
             await resetAllServerSettings(hasPassword ? pw : '', hasPassword ? '' : pw.trim().toUpperCase(), wipeData);
             setOpen(false); onDone();
           } catch (e: any) { setMsg(e?.message ?? String(e)); }
         } }]);
  };
  return (
    <View style={{ borderWidth: 1.5, borderColor: danger, borderRadius: 14, padding: 14, marginTop: 28 }}>
      <Text style={{ color: danger, fontFamily: F, fontSize: 16 }}>Reset all settings and start again</Text>
      <Text style={{ color: ink.textDim, fontFamily: F, fontSize: 12, lineHeight: 17, marginTop: 6 }}>{RESET_WIPES}</Text>
      <TextInput value={pw} onChangeText={setPw} secureTextEntry={hasPassword}
        placeholder={hasPassword ? 'Admin password' : 'Type RESET'} placeholderTextColor={ink.placeholder}
        style={{ color: ink.text, borderColor: ink.border, borderWidth: 1, borderRadius: 10, padding: 12, fontFamily: F, fontSize: 16, marginTop: 10 }} />
      {/* nav-registered: a native Switch takes the TV remote's own focus; Enter on a pad reaches the keys below */}
      <View style={{ flexDirection: 'row', alignItems: 'center', marginTop: 10 }}>
        <Text style={{ color: ink.text, fontFamily: F, fontSize: 14, flex: 1 }}>Also delete the gallery and stored data</Text>
        <Switch value={wipeData} onValueChange={setWipeData} />
      </View>
      {!!msg && <Text style={{ color: ink.bad, fontFamily: F, fontSize: 13, marginTop: 8 }}>{msg}</Text>}
      <ServerKey color={danger} dim={ink.textDim} border={danger} fontFamily={F} testID="reset-go" title="Reset…" onPress={go} />
      <ServerKey color={ink.key} dim={ink.textDim} border={ink.border} fontFamily={F} title="Cancel" onPress={() => setOpen(false)} />
    </View>
  );
}
