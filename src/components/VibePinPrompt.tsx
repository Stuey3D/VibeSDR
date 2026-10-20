import React, { useEffect, useRef, useState } from 'react';
import {
  KeyboardAvoidingView,
  Modal,
  Platform,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  TouchableOpacity,
  View,
} from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import {
  usePopupStyles, usePopupTheme, usePopupFrame, onMetal, engraveText, windowStyle,
  PopupKey, PopupPlate, type PopupTokens,
} from './PopupShell';
import { useSurface } from '../contexts/FaceplateContext';
import { useFieldPopupNav, NavWrap } from './FieldPopupNav';
import { usePadField } from './ControllerKeyboard';
import { inputSourceNow } from '../utils/inputSource';
import { useAndroidKeyboardHeight } from '../hooks/useAndroidKeyboardHeight';

/**
 * ★★★ THE VIBESERVER PIN BOX FOR ANDROID (2026-10-16).
 *
 *  Stuart: "Android devices cannot connect to a PIN protected server as no popup for it shows."
 *  The picker asked for the PIN with Alert.prompt, which is iOS-only — on Android it quietly
 *  connected with the saved PIN, or with NONE, and the server refused the socket. Android never
 *  had anywhere to type a PIN at all.
 *
 *  ★★ Says and does what the iOS prompt does: "VibeServer PIN", "Enter the PIN for <name>", a
 *     number pad, pre-filled with the saved PIN, and Cancel / Connect / Save & Connect. iOS keeps
 *     its native Alert.prompt; this is the same question in the app's own popup skin.
 *  ★ `note` carries the reason we are asking AGAIN ("That PIN was not accepted."), so a wrong PIN
 *    is a retry, not a dead end.
 *  ★ Enter on a hardware keyboard = Connect (onSubmitEditing). Back / Esc = Cancel.
 *  ★ Built like PasswordModal: the Android keyboard is measured and padded (a Modal is its own
 *    window, which adjustResize does not shrink — see useAndroidKeyboardHeight), the box is capped
 *    in width for tablets and landscape, and it scrolls when a landscape phone's keyboard leaves
 *    less height than the box needs.
 */
interface Props {
  visible:  boolean;
  /** The server's name as the list shows it. */
  name:     string;
  /** Pre-fill: the saved PIN for this host:port, or '' when none (or it was just refused). */
  initial:  string;
  /** Why we are asking again, if we are. */
  note?:    string;
  onSubmit: (pin: string, save: boolean) => void;
  onCancel: () => void;
}

export default function VibePinPrompt({ visible, name, initial, note, onSubmit, onCancel }: Props) {
  const kbH = useAndroidKeyboardHeight();
  const insets = useSafeAreaInsets();
  const styles = usePopupStyles(makeStyles);
  const pt = usePopupTheme();
  const dim = useSurface().scrimOpacity > 0;
  const metalFrame = usePopupFrame(16, false);
  const [pin, setPin] = useState(initial);
  const inputRef = useRef<TextInput>(null);

  // ★ Re-seed on every opening: a re-ask after a refused PIN must not show the refused one.
  useEffect(() => { if (visible) setPin(initial); }, [visible, initial]);

  const submit = (save: boolean) => {
    const p = pin.trim();
    onSubmit(p, save);
  };

  // ★ Pad and keyboard (controller audit group B): an overlay — Back = Cancel — over [PIN, Cancel, Connect, Save &
  //   Connect], starting on the field. Its keys were unreachable and Back only let go of the field.
  // ★ A pad: Enter on the field opens the numeric pad in PIN mode (digits only; ↵ = Connect); keyboard / touch: the field.
  const pinRef = useRef(pin); pinRef.current = pin;
  const pinField = usePadField({
    ref: inputRef, mode: 'pin', title: `PIN for ${name}`,
    getValue: () => pinRef.current, setValue: setPin,
    onSubmit: (v) => { onSubmit(v.trim(), false); },
  });
  const nav = useFieldPopupNav(visible, {
    name: 'VibeServer PIN', fields: 1, keys: [onCancel, () => submit(false), () => submit(true)],
    onField: () => pinField.open(), onBack: onCancel,
  });

  return (
    <Modal
      visible={visible}
      transparent
      animationType="fade"
      supportedOrientations={['portrait', 'portrait-upside-down', 'landscape', 'landscape-left', 'landscape-right']}
      onRequestClose={onCancel}
      // ★ Focus once the window is up: autoFocus inside an Android Modal can fire before the
      //   window takes input, and then no keyboard comes up.
      // ★ Not with a pad: the cursor would raise a system keyboard the pad cannot type into — the highlight starts on
      //   the field instead, and Enter opens the controller keyboard.
      onShow={() => { if (inputSourceNow() !== 'pad') setTimeout(() => inputRef.current?.focus(), 50); }}
    >
      <KeyboardAvoidingView
        style={[styles.flex, dim && styles.overlayDim]}
        behavior={Platform.OS === 'ios' ? 'padding' : undefined}
      >
        {/* ★ No indicator: this scroller exists only so the box still fits a short landscape phone
            with the keyboard up — it scrolls a single small card, and a bar would sit on its keys. */}
        <ScrollView
          showsVerticalScrollIndicator={false}
          keyboardShouldPersistTaps="handled"
          contentContainerStyle={[
            styles.overlay,
            {
              paddingTop: 24 + insets.top,
              paddingBottom: 24 + Math.max(insets.bottom, kbH),
              paddingLeft: 24 + insets.left,
              paddingRight: 24 + insets.right,
            },
          ]}
        >
          <View style={[styles.box, metalFrame]}>
            <PopupPlate radius={16} />
            <Text style={styles.title}>VibeServer PIN</Text>
            {!!note && <Text style={styles.note}>{note}</Text>}
            <Text style={styles.sub} numberOfLines={3}>Enter the PIN for {name}</Text>
            <TextInput
              ref={inputRef}
              style={[styles.input, nav.field(0) && { borderColor: nav.ring, borderWidth: 2 }]}
              placeholder="PIN"
              placeholderTextColor={pt.metal ? pt.winDim : pt.ink.textFaint}
              value={pin}
              onChangeText={setPin}
              keyboardType="number-pad"
              secureTextEntry
              autoCapitalize="none"
              autoCorrect={false}
              spellCheck={false}
              autoComplete="off"
              textContentType="none"
              importantForAutofill="no"
              returnKeyType="go"
              blurOnSubmit={false}
              onSubmitEditing={() => submit(false)}
              accessibilityLabel={`PIN for ${name}`}
              {...pinField.fieldProps}
            />
            {pt.metal ? (
              <View style={styles.row}>
                <PopupKey label="CANCEL" focused={nav.key(0)} onPress={onCancel} height={40} fontSize={12} />
                <PopupKey label="CONNECT" focused={nav.key(1)} onPress={() => submit(false)} height={40} fontSize={12} />
                <PopupKey label="SAVE & CONNECT" primary focused={nav.key(2)} onPress={() => submit(true)} height={40} fontSize={12} />
              </View>
            ) : (
              <View style={styles.row}>
                <NavWrap on={nav.key(0)} color={nav.ring}>
                <TouchableOpacity style={styles.btn} onPress={onCancel} accessibilityRole="button">
                  <Text style={styles.btnTxtCancel}>Cancel</Text>
                </TouchableOpacity>
                </NavWrap>
                <NavWrap on={nav.key(1)} color={nav.ring}>
                <TouchableOpacity style={styles.btn} onPress={() => submit(false)} accessibilityRole="button">
                  <Text style={styles.btnTxt}>Connect</Text>
                </TouchableOpacity>
                </NavWrap>
                <NavWrap on={nav.key(2)} color={nav.ring}>
                <TouchableOpacity style={[styles.btn, styles.btnPrimary]} onPress={() => submit(true)} accessibilityRole="button">
                  <Text style={styles.btnTxtPrimary}>Save & Connect</Text>
                </TouchableOpacity>
                </NavWrap>
              </View>
            )}
          </View>
        </ScrollView>
      </KeyboardAvoidingView>
    </Modal>
  );
}

/* Same skin as PasswordModal: the user's TEXT and CONTROLS colours on the default look, the
 * brushed plate on silver / black. */
const makeStyles = (pt: PopupTokens) => StyleSheet.create({
  flex:        { flex: 1 },
  overlay:     { flexGrow: 1, justifyContent: 'center' },
  overlayDim:  { backgroundColor: 'rgba(0,0,0,0.75)' },
  box:         { width: '100%', maxWidth: 440, alignSelf: 'center', backgroundColor: '#0A0804', borderRadius: 10, borderWidth: 1, borderColor: pt.ink.border, padding: 20, gap: 14 },
  title:       onMetal(pt, { fontFamily: 'Courier', fontSize: 16, fontWeight: 'bold', color: pt.ink.text, letterSpacing: 1 }, { ...engraveText(pt), letterSpacing: 1.5 }),
  note:        onMetal(pt, { fontFamily: 'Courier', fontSize: 12, color: pt.gold.sel }, engraveText(pt, pt.note)),
  sub:         onMetal(pt, { fontFamily: 'Courier', fontSize: 11, color: pt.ink.textDim }, engraveText(pt, pt.note)),
  input:       onMetal(pt, { height: 44, backgroundColor: 'rgba(20,10,0,0.80)', borderWidth: 1, borderColor: pt.ink.keyBorder, borderRadius: 6, paddingHorizontal: 12, fontFamily: 'Courier', fontSize: 14, color: pt.ink.text },
                        { ...windowStyle(pt), fontFamily: 'Atkinson Hyperlegible', color: pt.readout }),
  // ★ Wraps: three keys do not fit one row on an SE-width phone in portrait.
  row:         { flexDirection: 'row', flexWrap: 'wrap', gap: 10, justifyContent: 'flex-end' },
  btn:         { paddingHorizontal: 14, paddingVertical: 10, borderRadius: 6, borderWidth: 1, borderColor: pt.ink.keyBorder },
  btnPrimary:  { borderColor: pt.gold.selBorder, backgroundColor: pt.ink.keyBgAct },
  btnTxtCancel:  { fontFamily: 'Courier', fontSize: 13, color: pt.ink.key },
  btnTxt:        { fontFamily: 'Courier', fontSize: 13, color: pt.ink.key },
  btnTxtPrimary: { fontFamily: 'Courier', fontSize: 13, color: pt.gold.sel, fontWeight: 'bold' },
});
