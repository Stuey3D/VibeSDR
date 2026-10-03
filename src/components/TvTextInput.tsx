/**
 * ★★★ A TEXT FIELD A TV REMOTE CAN GET PAST (Kiko, 2026-10-03, an Android 6 TV box set up with its D-pad).
 *     On a TV, focus that lands in an Android EditText STAYS there: DPAD_DOWN moves the text cursor instead
 *     of the focus, so every setting below the first text box — and the Start button — was unreachable
 *     from the remote. Stuart's Android 9 Sony let focus out; an Android 6 box did not. Plug-and-play on a
 *     TV box means the remote alone must reach everything.
 *
 *  So ON A TV ONLY (Platform.isTV — the system's own television UI mode), a field is drawn as a focusable
 *  box showing its value. The D-pad moves past it like any other control; OK opens a small edit sheet with
 *  the field focused (and the on-screen keyboard), and DONE / BACK leave it. Phones and tablets get the
 *  plain TextInput, untouched.
 *  ★ The value is committed through the field's own onChangeText, then its onEndEditing / onSubmitEditing /
 *    onBlur — so callers that save on change or on blur behave exactly as they do with a keyboard.
 */
import React, { useState } from 'react';
import { Modal, Platform, Pressable, StyleSheet, Text, TextInput, View, type TextInputProps } from 'react-native';

export default function TvTextInput(props: TextInputProps) {
  if (!Platform.isTV) return <TextInput {...props} />;
  return <TvField {...props} />;
}

function TvField(props: TextInputProps) {
  const [open, setOpen] = useState(false);
  const [draft, setDraft] = useState('');
  const [focused, setFocused] = useState(false);
  const value = props.value ?? '';
  const shown = value === '' ? (props.placeholder ?? '') : props.secureTextEntry ? '•'.repeat(Math.min(12, value.length)) : value;
  const flat = StyleSheet.flatten(props.style) ?? {};
  const color = value === '' ? (props.placeholderTextColor ?? '#888') : (flat.color ?? '#fff');

  const begin = () => { if (props.editable === false) return; setDraft(value); setOpen(true); };
  const commit = () => {
    setOpen(false);
    if (draft !== value) props.onChangeText?.(draft);
    const ev: any = { nativeEvent: { text: draft } };
    props.onEndEditing?.(ev);
    props.onSubmitEditing?.(ev);
    props.onBlur?.(ev);
  };

  return (
    <>
      <Pressable focusable onPress={begin} onFocus={() => setFocused(true)} onBlur={() => setFocused(false)}
        accessibilityRole="button" accessibilityLabel={props.accessibilityLabel ?? props.placeholder}
        style={[props.style, { justifyContent: 'center' }, focused && styles.focused]}>
        <Text numberOfLines={props.multiline ? 3 : 1}
          style={{ color, fontFamily: flat.fontFamily, fontSize: flat.fontSize ?? 15 }}>{shown}</Text>
      </Pressable>
      <Modal visible={open} transparent animationType="fade" onRequestClose={commit}>
        <View style={styles.backdrop}>
          <View style={styles.sheet}>
            {!!props.placeholder && <Text style={styles.label}>{props.placeholder}</Text>}
            <TextInput {...props} value={draft} onChangeText={setDraft} autoFocus
              onBlur={undefined} onEndEditing={undefined} onFocus={undefined}
              onSubmitEditing={props.multiline ? undefined : commit}
              style={[props.style, styles.field]} />
            <Pressable focusable onPress={commit} style={({ focused: f }: any) => [styles.done, f && styles.focused]}>
              <Text style={styles.doneText}>DONE</Text>
            </Pressable>
          </View>
        </View>
      </Modal>
    </>
  );
}

const styles = StyleSheet.create({
  focused:  { borderColor: '#ffb833', borderWidth: 2 },
  backdrop: { flex: 1, backgroundColor: 'rgba(0,0,0,0.75)', justifyContent: 'center', padding: 40 },
  sheet:    { backgroundColor: '#14110a', borderRadius: 14, borderWidth: 1, borderColor: '#5a4520', padding: 20,
              maxWidth: 720, width: '100%', alignSelf: 'center' },
  label:    { color: '#c9922e', fontSize: 14, marginBottom: 10 },
  field:    { marginBottom: 14 },
  done:     { alignSelf: 'flex-end', paddingVertical: 10, paddingHorizontal: 22, borderRadius: 10,
              borderWidth: 1, borderColor: '#5a4520' },
  doneText: { color: '#ffb833', fontSize: 16, fontWeight: 'bold', letterSpacing: 2 },
});
