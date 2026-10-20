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
import { sanitizeIdent } from '../services/kiwiIdent';
import { useAndroidKeyboardHeight } from '../hooks/useAndroidKeyboardHeight';
import { useFieldPopupNav, NavWrap } from './FieldPopupNav';
import { usePadField } from './ControllerKeyboard';

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
  // ★ Android: pad above the keyboard ourselves — see useAndroidKeyboardHeight (the flicker).
  const kbH = useAndroidKeyboardHeight();
  const styles = usePopupStyles(makeStyles);
  const pt = usePopupTheme();
  // ★★★ Transparency OFF: no full-screen dim (§10.2). ★ Silver / black: the brushed plate (§10.3).
  const dim = useSurface().scrimOpacity > 0;
  const metalFrame = usePopupFrame(16, false);
  const [name, setName] = useState(initial);

  // Re-seed the field whenever the modal is (re-)shown with a different saved value.
  React.useEffect(() => { if (visible) setName(initial); }, [visible, initial]);

  const submit = () => {
    const val = sanitizeIdent(name.trim());
    if (!val) return;                 // non-blank required — a blank ident is what gets refused
    onSubmit(val);
  };

  // ★ Pad and keyboard (controller audit group B): an overlay — Back = Cancel, nothing reaches the list beneath — over
  //   [field, Cancel, primary], starting on the field. Enter on the field puts the cursor in it.
  const fieldRef = React.useRef<TextInput | null>(null);
  // ★ A pad: Enter on the field opens the controller keyboard (↵ = Save & Connect); keyboard / touch: the field.
  const nameRef = React.useRef(name); nameRef.current = name;
  const identField = usePadField({
    ref: fieldRef, mode: 'text', title: 'Name or call sign',
    getValue: () => nameRef.current, setValue: (v) => setName(sanitizeIdent(v)),
    onSubmit: (v) => {
      const val = sanitizeIdent(v.trim());
      if (!val) return 'Enter a name or call sign.';
      onSubmit(val);
    },
  });
  const nav = useFieldPopupNav(visible, {
    name: 'IdentModal', fields: 1, keys: [onCancel, submit],
    onField: () => identField.open(), onBack: onCancel,
  });

  return (
    <Modal
      visible={visible}
      transparent
      animationType="fade"
      onRequestClose={onCancel}
    >
      <KeyboardAvoidingView
        style={[styles.overlay, dim && styles.overlayDim, kbH > 0 && { paddingBottom: 24 + kbH }]}
        behavior={Platform.OS === 'ios' ? 'padding' : undefined}
      >
        <View style={[styles.box, metalFrame]}>
          <PopupPlate radius={16} />
          <Text style={styles.title}>Name or Callsign</Text>
          <Text style={styles.sub} numberOfLines={3}>
            Some KiwiSDR receivers require a name or callsign before you can listen. It’s saved and
            sent automatically next time.
          </Text>
          <TextInput
            ref={fieldRef}
            style={[styles.input, nav.field(0) && { borderColor: nav.ring, borderWidth: 2 }]}
            placeholder="e.g. M0ABC or Stu"
            placeholderTextColor={pt.metal ? pt.winDim : pt.ink.textFaint}
            value={name}
            onChangeText={(t) => setName(sanitizeIdent(t))}
            maxLength={16}
            autoCapitalize="characters"
            autoCorrect={false}
            returnKeyType="go"
            onSubmitEditing={submit}
            {...identField.fieldProps}
          />
          {pt.metal ? (
            <View style={styles.row}>
              <PopupKey label="CANCEL" focused={nav.key(0)} onPress={onCancel} height={40} fontSize={12} style={{ minWidth: 96 }} />
              <PopupKey label="SAVE & CONNECT" primary focused={nav.key(1)} onPress={submit} height={40} fontSize={12} style={{ minWidth: 110 }} />
            </View>
          ) : (
          <View style={styles.row}>
<NavWrap on={nav.key(0)} color={nav.ring}>
                        <TouchableOpacity style={styles.btn} onPress={onCancel}>
              <Text style={styles.btnTxtCancel}>Cancel</Text>
            </TouchableOpacity>
            </NavWrap>
<NavWrap on={nav.key(1)} color={nav.ring}>
                        <TouchableOpacity style={[styles.btn, styles.btnPrimary]} onPress={submit}>
              <Text style={styles.btnTxtPrimary}>Save & Connect</Text>
            </TouchableOpacity>
            </NavWrap>
          </View>
          )}
        </View>
      </KeyboardAvoidingView>
    </Modal>
  );
}

/* ★★ The default skin follows the user's colours (Stuart, 2026-10-12: "yes colours apply everywhere including
 *  directory if possible"): text in the TEXT colour, keys and borders in the CONTROLS colour — was amber. */
const makeStyles = (pt: PopupTokens) => StyleSheet.create({
  overlay:     { flex: 1, justifyContent: 'center', padding: 24 },
  overlayDim:  { backgroundColor: 'rgba(0,0,0,0.75)' },
  box:         { backgroundColor: '#0A0804', borderRadius: 10, borderWidth: 1, borderColor: pt.ink.border, padding: 20, gap: 14 },
  title:       onMetal(pt, { fontFamily: 'Courier', fontSize: 16, fontWeight: 'bold', color: pt.ink.text, letterSpacing: 1 }, { ...engraveText(pt), letterSpacing: 1.5 }),
  sub:         onMetal(pt, { fontFamily: 'Courier', fontSize: 11, color: pt.ink.textDim, lineHeight: 16 }, engraveText(pt, pt.note)),
  input:       onMetal(pt, { height: 44, backgroundColor: 'rgba(20,10,0,0.80)', borderWidth: 1, borderColor: pt.ink.keyBorder, borderRadius: 6, paddingHorizontal: 12, fontFamily: 'Courier', fontSize: 14, color: pt.ink.text },
                        { ...windowStyle(pt), fontFamily: 'Atkinson Hyperlegible', color: pt.readout }),
  row:         { flexDirection: 'row', gap: 10, justifyContent: 'flex-end' },
  btn:         { paddingHorizontal: 18, paddingVertical: 10, borderRadius: 6, borderWidth: 1, borderColor: pt.ink.keyBorder },
  btnPrimary:  { borderColor: pt.gold.selBorder, backgroundColor: pt.ink.keyBgAct },
  btnTxtCancel:  { fontFamily: 'Courier', fontSize: 13, color: pt.ink.key },
  btnTxtPrimary: { fontFamily: 'Courier', fontSize: 13, color: pt.gold.sel, fontWeight: 'bold' },
});
