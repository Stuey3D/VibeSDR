import React, { useState } from 'react';
import {
  KeyboardAvoidingView,
  Modal,
  Platform,
  StyleSheet,
  Text,
  TextInput,
  TouchableOpacity,
  View,
} from 'react-native';
import { sanitizeIdent } from '../services/kiwiIdent';

interface Props {
  visible:  boolean;
  /** Pre-fill with the saved identity so it never has to be retyped — just confirmed or edited. */
  initial:  string;
  onSubmit: (ident: string) => void;
  onCancel: () => void;
}

/// "Name or callsign" entry for KiwiSDR — the same box the Kiwi web client shows. Saved and
/// remembered (see kiwiIdent), pre-filled here so re-opening it is confirm-or-edit, not retype.
/// It is IDENTITY, not a chat name (KiwiSDR has no chat) — worded accordingly.
export default function IdentModal({ visible, initial, onSubmit, onCancel }: Props) {
  const [name, setName] = useState(initial);

  // Re-seed the field whenever the modal is (re-)shown with a different saved value.
  React.useEffect(() => { if (visible) setName(initial); }, [visible, initial]);

  const submit = () => {
    const val = sanitizeIdent(name.trim());
    if (!val) return;                 // non-blank required — a blank ident is what gets refused
    onSubmit(val);
  };

  return (
    <Modal
      visible={visible}
      transparent
      animationType="fade"
      onRequestClose={onCancel}
    >
      <KeyboardAvoidingView
        style={styles.overlay}
        behavior={Platform.OS === 'ios' ? 'padding' : 'height'}
      >
        <View style={styles.box}>
          <Text style={styles.title}>Name or Callsign</Text>
          <Text style={styles.sub} numberOfLines={3}>
            Some KiwiSDR receivers require a name or callsign before you can listen. It’s saved and
            sent automatically next time.
          </Text>
          <TextInput
            style={styles.input}
            placeholder="e.g. M0ABC or Stu"
            placeholderTextColor="rgba(200,137,58,0.45)"
            value={name}
            onChangeText={(t) => setName(sanitizeIdent(t))}
            maxLength={16}
            autoCapitalize="characters"
            autoCorrect={false}
            returnKeyType="go"
            onSubmitEditing={submit}
          />
          <View style={styles.row}>
            <TouchableOpacity style={styles.btn} onPress={onCancel}>
              <Text style={styles.btnTxtCancel}>Cancel</Text>
            </TouchableOpacity>
            <TouchableOpacity style={[styles.btn, styles.btnPrimary]} onPress={submit}>
              <Text style={styles.btnTxtPrimary}>Save & Connect</Text>
            </TouchableOpacity>
          </View>
        </View>
      </KeyboardAvoidingView>
    </Modal>
  );
}

const styles = StyleSheet.create({
  overlay:     { flex: 1, backgroundColor: 'rgba(0,0,0,0.75)', justifyContent: 'center', padding: 24 },
  box:         { backgroundColor: '#0A0804', borderRadius: 10, borderWidth: 1, borderColor: 'rgba(255,160,0,0.40)', padding: 20, gap: 14 },
  title:       { fontFamily: 'Courier', fontSize: 16, fontWeight: 'bold', color: '#FFB833', letterSpacing: 1 },
  sub:         { fontFamily: 'Courier', fontSize: 11, color: 'rgba(200,137,58,0.70)', lineHeight: 16 },
  input:       { height: 44, backgroundColor: 'rgba(20,10,0,0.80)', borderWidth: 1, borderColor: 'rgba(255,160,0,0.35)', borderRadius: 6, paddingHorizontal: 12, fontFamily: 'Courier', fontSize: 14, color: '#FFB833' },
  row:         { flexDirection: 'row', gap: 10, justifyContent: 'flex-end' },
  btn:         { paddingHorizontal: 18, paddingVertical: 10, borderRadius: 6, borderWidth: 1, borderColor: 'rgba(255,160,0,0.30)' },
  btnPrimary:  { borderColor: 'rgba(255,160,0,0.60)', backgroundColor: 'rgba(255,160,0,0.12)' },
  btnTxtCancel:  { fontFamily: 'Courier', fontSize: 13, color: 'rgba(200,137,58,0.70)' },
  btnTxtPrimary: { fontFamily: 'Courier', fontSize: 13, color: '#FFB833', fontWeight: 'bold' },
});
