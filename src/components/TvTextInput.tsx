/**
 * ★★★ A TEXT FIELD A TV REMOTE CAN GET PAST (Kiko, 2026-10-03, an Android 6 TV box set up with its D-pad).
 *     On a TV, focus that lands in an Android EditText STAYS there: DPAD_DOWN moves the text cursor instead
 *     of the focus, so every setting below the first text box — and the Start button — was unreachable
 *     from the remote. Stuart's Android 9 Sony let focus out; an Android 6 box did not. Plug-and-play on a
 *     TV box means the remote alone must reach everything.
 *
 *  So ON A TV ONLY (see useTvFields below), a field is drawn as a focusable
 *  box showing its value. The D-pad moves past it like any other control; OK opens a small edit sheet with
 *  the field focused (and the on-screen keyboard), and DONE / BACK leave it. Phones and tablets get the
 *  plain TextInput, untouched.
 *  ★ The value is committed through the field's own onChangeText, then its onEndEditing / onSubmitEditing /
 *    onBlur — so callers that save on change or on blur behave exactly as they do with a keyboard.
 */
import React, { createContext, useContext, useState } from 'react';
import { Modal, NativeModules, Platform, Pressable, StyleSheet, Text, TextInput, View, type TextInputProps } from 'react-native';
import type { PopupTokens } from '../constants/popupTokens';

/* ★★★ LITE-SAFE: NO PopupShell / FaceplateContext / Reanimated / Skia HERE. ServerModeScreen imports this field,
 *   and VibeServer Lite bundles ServerModeScreen on RN 0.73 with neither library (its metro config cannot even
 *   see the main app's node_modules). So the colours come IN: the main app wraps the screen in
 *   <TvTextInputTokens value={tokens}> (or passes `tokens` on a field); Lite, with no colour settings, gets the
 *   field's PRE-2026-10-12 literals exactly (LITE_STYLES — Stuart: Lite has no colour UI and must look exactly as
 *   before). popupTokens.ts is pure (type imports and decoderTokens.ts only). */
const TvTokens = createContext<PopupTokens | null>(null);
/** Hands the user's colours (the main app's glass tokens) to every TV text field below it. */
export const TvTextInputTokens = TvTokens.Provider;

/* ★★ WHO GETS THE TV FIELD. Platform.isTV is the system's television UI MODE, and Kiko's Android TV box
 *  (2026-10-03) could not tab between fields with a hardware keyboard either — a box may not report that
 *  mode. So also: the device declares itself a television (android.hardware.type.television / leanback —
 *  the server screen's own test), or it has NO TOUCHSCREEN at all (VibeLocalSDR.noTouchscreen, from
 *  PackageManager). A phone or tablet always has a touchscreen, so neither ever changes there.
 *  ★ Decided once: none of these can change while the app runs. An APK older than the constant reads
 *    undefined, i.e. today's behaviour. */
const useTvFields: boolean = Platform.isTV || (Platform.OS === 'android' && (() => {
  const m = (NativeModules as any).VibeLocalSDR;
  return m?.isTv === true || m?.noTouchscreen === true;
})());

export default function TvTextInput(props: TextInputProps & { tokens?: PopupTokens }) {
  if (!useTvFields) { const { tokens: _t, ...rest } = props; return <TextInput {...rest} />; }
  return <TvField {...props} />;
}

function TvField({ tokens, ...props }: TextInputProps & { tokens?: PopupTokens }) {
  const ctx = useContext(TvTokens);
  const styles = tvStyles(tokens ?? ctx);
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

/* ★★ IN THE USER'S COLOURS (Stuart, 2026-10-12: "yes colours apply everywhere including directory if possible"):
 *   the field's label in the TEXT colour, DONE and the sheet's edge in the CONTROLS colour; the D-pad focus ring is
 *   the app's focus MEANING (green on every colour, PopupTokens ink.focus) — it was amber here alone. */
const tvCache = new WeakMap<PopupTokens, ReturnType<typeof makeTvStyles>>();
function tvStyles(pt: PopupTokens | null) {
  if (!pt) return LITE_STYLES;
  let s = tvCache.get(pt);
  if (!s) { s = makeTvStyles(pt); tvCache.set(pt, s); }
  return s;
}
/** The field's colours by role: the user's (main app) or the pre-2026-10-12 literals (Lite — TV_LITE below). */
export interface TvFieldColours { focus: string; border: string; label: string; doneBorder: string; done: string }
/** ★★ VibeServer Lite: EXACTLY the old literals (git 034143e6^ TvTextInput.tsx styles: focused #ffb833, sheet and
 *  DONE edge #5a4520, label #c9922e, DONE #ffb833) — scripts/test_server_colours.ts holds them. */
export const TV_LITE: TvFieldColours = { focus: '#ffb833', border: '#5a4520', label: '#c9922e', doneBorder: '#5a4520', done: '#ffb833' };
export const tvColoursFor = (pt: PopupTokens): TvFieldColours =>
  ({ focus: pt.ink.focus, border: pt.ink.border, label: pt.ink.textDim, doneBorder: pt.ink.keyBorder, done: pt.gold.sel });
const makeTvStyles = (pt: PopupTokens) => buildTvStyles(tvColoursFor(pt));
const buildTvStyles = (c: TvFieldColours) => StyleSheet.create({
  focused:  { borderColor: c.focus, borderWidth: 2 },
  backdrop: { flex: 1, backgroundColor: 'rgba(0,0,0,0.75)', justifyContent: 'center', padding: 40 },
  sheet:    { backgroundColor: '#14110a', borderRadius: 14, borderWidth: 1, borderColor: c.border, padding: 20,
              maxWidth: 720, width: '100%', alignSelf: 'center' },
  label:    { color: c.label, fontSize: 14, marginBottom: 10 },
  field:    { marginBottom: 14 },
  done:     { alignSelf: 'flex-end', paddingVertical: 10, paddingHorizontal: 22, borderRadius: 10,
              borderWidth: 1, borderColor: c.doneBorder },
  doneText: { color: c.done, fontSize: 16, fontWeight: 'bold', letterSpacing: 2 },
});
const LITE_STYLES = buildTvStyles(TV_LITE);
