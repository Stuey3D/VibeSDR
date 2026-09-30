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
import {
  usePopupStyles, usePopupTheme, usePopupFrame, onMetal, engraveText, windowStyle,
  PopupKey, PopupPlate, type PopupTokens,
} from './PopupShell';
import { useSurface } from '../contexts/FaceplateContext';

interface Props {
  visible:    boolean;
  serverUrl:  string;
  onSubmit:   (password: string) => void;
  onCancel:   () => void;
}

export default function PasswordModal({ visible, serverUrl, onSubmit, onCancel }: Props) {
  const styles = usePopupStyles(makeStyles);
  const pt = usePopupTheme();
  // ★★★ Transparency OFF: no full-screen dim (§10.2). ★ Silver / black: the brushed plate (§10.3).
  const dim = useSurface().scrimOpacity > 0;
  const metalFrame = usePopupFrame(16, false);
  const [pw, setPw] = useState('');

  const submit = () => {
    const val = pw.trim();
    setPw('');
    onSubmit(val);
  };

  const cancel = () => {
    setPw('');
    onCancel();
  };

  return (
    <Modal
      visible={visible}
      transparent
      animationType="fade"
      onRequestClose={() => {/* intentionally not dismissible via system gesture */}}
    >
      <KeyboardAvoidingView
        style={[styles.overlay, dim && styles.overlayDim]}
        behavior={Platform.OS === 'ios' ? 'padding' : 'height'}
      >
        <View style={[styles.box, metalFrame]}>
          <PopupPlate radius={16} />
          <Text style={styles.title}>Password Required</Text>
          <Text style={styles.sub} numberOfLines={2}>{serverUrl}</Text>
          <TextInput
            style={styles.input}
            placeholder="Enter password"
            placeholderTextColor={pt.metal ? pt.winDim : "rgba(200,137,58,0.45)"}
            value={pw}
            onChangeText={setPw}
            secureTextEntry
            autoFocus={false}
            returnKeyType="go"
            onSubmitEditing={submit}
          />
          {pt.metal ? (
            <View style={styles.row}>
              <PopupKey label="CANCEL" onPress={cancel} height={40} fontSize={12} style={{ minWidth: 96 }} />
              <PopupKey label="CONNECT" primary onPress={submit} height={40} fontSize={12} style={{ minWidth: 110 }} />
            </View>
          ) : (
          <View style={styles.row}>
            <TouchableOpacity style={styles.btn} onPress={cancel}>
              <Text style={styles.btnTxtCancel}>Cancel</Text>
            </TouchableOpacity>
            <TouchableOpacity style={[styles.btn, styles.btnPrimary]} onPress={submit}>
              <Text style={styles.btnTxtPrimary}>Connect</Text>
            </TouchableOpacity>
          </View>
          )}
        </View>
      </KeyboardAvoidingView>
    </Modal>
  );
}

const makeStyles = (pt: PopupTokens) => StyleSheet.create({
  overlay:     { flex: 1, justifyContent: 'center', padding: 24 },
  overlayDim:  { backgroundColor: 'rgba(0,0,0,0.75)' },
  box:         { backgroundColor: '#0A0804', borderRadius: 10, borderWidth: 1, borderColor: 'rgba(255,160,0,0.40)', padding: 20, gap: 14 },
  title:       onMetal(pt, { fontFamily: 'Courier', fontSize: 16, fontWeight: 'bold', color: '#FFB833', letterSpacing: 1 }, { ...engraveText(pt), letterSpacing: 1.5 }),
  sub:         onMetal(pt, { fontFamily: 'Courier', fontSize: 11, color: 'rgba(200,137,58,0.70)' }, engraveText(pt, pt.note)),
  input:       onMetal(pt, { height: 44, backgroundColor: 'rgba(20,10,0,0.80)', borderWidth: 1, borderColor: 'rgba(255,160,0,0.35)', borderRadius: 6, paddingHorizontal: 12, fontFamily: 'Courier', fontSize: 14, color: '#FFB833' },
                        { ...windowStyle(pt), fontFamily: 'Atkinson Hyperlegible', color: pt.readout }),
  row:         { flexDirection: 'row', gap: 10, justifyContent: 'flex-end' },
  btn:         { paddingHorizontal: 18, paddingVertical: 10, borderRadius: 6, borderWidth: 1, borderColor: 'rgba(255,160,0,0.30)' },
  btnPrimary:  { borderColor: 'rgba(255,160,0,0.60)', backgroundColor: 'rgba(255,160,0,0.12)' },
  btnTxtCancel:  { fontFamily: 'Courier', fontSize: 13, color: 'rgba(200,137,58,0.70)' },
  btnTxtPrimary: { fontFamily: 'Courier', fontSize: 13, color: '#FFB833', fontWeight: 'bold' },
});
