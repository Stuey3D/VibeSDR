/**
 * ★★★ beta96 CHANGE ADMIN PASSWORD / PIN — ON THE DEVICE ITSELF, NO OLD PASSWORD (Stuart, 2026-11-26).
 *
 * Forgotten-password recovery: whoever holds the device holds the radio, so physical access is the proof and the old
 * password is never asked for HERE. The remote /setup page still needs the current password, and cannot change it
 * (vibe_android_setup.h never takes adminPassword from the page; VibeRemoteSetup.configSetPatch skips it).
 * ★ Typed by the owner, twice, at least 8 characters — never generated (feedback: admin passwords typed by the owner).
 * ★ The PIN half is drawn only where a PIN exists (Full); a Simple server has none (local network only).
 * Lite-safe: React Native only.
 */
import React, { useState } from 'react';
import { Text, View } from 'react-native';
import TextInput from './TvTextInput';
import ServerKey from './ServerKey';

export type CredInk = { text: string; textDim: string; key: string; keyAct: string; border: string; borderBright: string;
                        placeholder: string; bad: string; ok: string; fontFamily?: string };

/** The password rule, one place (the shim's setup-code step and this screen agree: 8–128, typed twice). */
export function passwordProblem(a: string, b: string): string {
  if (!a) return 'Type the new admin password.';
  if (a !== b) return 'The two passwords are not the same.';
  if (a.length < 8) return 'At least 8 characters.';
  if (a.length > 128) return 'At most 128 characters.';
  if (/[\u0000-\u001f\u007f]/.test(a)) return 'No control characters.';
  return '';
}

type Props = {
  ink: CredInk;
  /** Saves the new password (already checked). Throws with words on failure. */
  onSavePassword: (pw: string) => Promise<void>;
  /** Present only where a PIN exists: saves the new PIN ('' = no PIN). */
  onSavePin?: (pin: string) => Promise<void>;
  hasPassword: boolean;
};

export default function DeviceCredentials({ ink, onSavePassword, onSavePin, hasPassword }: Props) {
  const [open, setOpen] = useState(false);
  const [a, setA] = useState(''); const [b, setB] = useState('');
  const [pin, setPin] = useState('');
  const [msg, setMsg] = useState<{ t: string; bad: boolean } | null>(null);
  const F = ink.fontFamily;
  const inputStyle = { color: ink.text, borderColor: ink.border, borderWidth: 1, borderRadius: 10, padding: 12,
                       fontFamily: F, fontSize: 16, marginTop: 8 };
  if (!open) {
    return (
      <ServerKey color={ink.key} dim={ink.textDim} border={ink.border} fontFamily={F} testID="cred-open"
        title={onSavePin ? 'Change admin password / PIN' : 'Change admin password'}
        sub={(hasPassword ? 'Forgotten it? ' : '') + 'Set a new one here — no old password needed on this device.'}
        onPress={() => { setOpen(true); setMsg(null); }} />
    );
  }
  return (
    <View style={{ borderWidth: 1, borderColor: ink.borderBright, borderRadius: 14, padding: 14, marginTop: 12 }}>
      <Text style={{ color: ink.text, fontFamily: F, fontSize: 16 }}>New admin password</Text>
      <Text style={{ color: ink.textDim, fontFamily: F, fontSize: 12, marginTop: 4, lineHeight: 17 }}>
        It protects the radio&rsquo;s controls and the setup page at /setup. At least 8 characters, typed twice.
        Nobody else can change it from another device without the current one.
      </Text>
      <TextInput value={a} onChangeText={setA} secureTextEntry placeholder="New admin password"
        placeholderTextColor={ink.placeholder} style={inputStyle} />
      <TextInput value={b} onChangeText={setB} secureTextEntry placeholder="The same again"
        placeholderTextColor={ink.placeholder} style={inputStyle} />
      <ServerKey color={ink.keyAct} dim={ink.textDim} border={ink.borderBright} fontFamily={F} testID="cred-save-pw"
        title="Save admin password" onPress={async () => {
          const why = passwordProblem(a, b);
          if (why) { setMsg({ t: why, bad: true }); return; }
          try { await onSavePassword(a); setA(''); setB(''); setMsg({ t: 'Saved. It is in force now.', bad: false }); }
          catch (e: any) { setMsg({ t: e?.message ?? String(e), bad: true }); }
        }} />
      {onSavePin && (<>
        <Text style={{ color: ink.text, fontFamily: F, fontSize: 16, marginTop: 18 }}>New listener PIN</Text>
        <Text style={{ color: ink.textDim, fontFamily: F, fontSize: 12, marginTop: 4 }}>Digits only, up to 12. Empty = no PIN.</Text>
        <TextInput value={pin} onChangeText={(t) => setPin(t.replace(/[^0-9]/g, '').slice(0, 12))} keyboardType="number-pad"
          placeholder="PIN" placeholderTextColor={ink.placeholder} style={inputStyle} />
        <ServerKey color={ink.keyAct} dim={ink.textDim} border={ink.borderBright} fontFamily={F} testID="cred-save-pin"
          title="Save PIN" onPress={async () => {
            try { await onSavePin(pin); setMsg({ t: pin ? 'PIN saved.' : 'PIN removed — anyone who can reach the server may listen.', bad: false }); }
            catch (e: any) { setMsg({ t: e?.message ?? String(e), bad: true }); }
          }} />
      </>)}
      {!!msg && <Text style={{ color: msg.bad ? ink.bad : ink.ok, fontFamily: F, fontSize: 13, marginTop: 10 }}>{msg.t}</Text>}
      <ServerKey color={ink.key} dim={ink.textDim} border={ink.border} fontFamily={F} title="Done" onPress={() => setOpen(false)} />
    </View>
  );
}
