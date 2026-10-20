/**
 * appAlert — Alert.alert's exact shape, answerable by a game controller (controller audit root cause D, 2026-10-20).
 *
 * ★★★ An iOS native alert is a dead end for a pad (UIAlertController in React Native's own window: GameController
 *   events never reach its buttons) AND a hazard (they DO reach JS, so the screen underneath kept moving and Enter
 *   pressed things behind the alert). So:
 *   - TOUCH in use → the native alert, unchanged (VoiceOver, sizing, the platform's look). While it is up an overlay
 *     is registered (PanelNav pushOverlay) so a pad picked up mid-alert cannot press anything behind it.
 *   - PAD or KEYBOARD in use → an in-app dialog (AppAlertHost, mounted once in App.tsx): the same title, message and
 *     buttons, the highlight on the safe default (src/utils/appAlertModel.ts — never a destructive key), arrows move,
 *     Enter presses, Back = the cancel button. It is a level-2 overlay: above tours and every panel.
 * ★ Alert.prompt is NOT covered — the iOS PIN prompt takes VibePinPrompt when a pad is in use instead (brief).
 */
import React, { useEffect, useState } from 'react';
import { Alert, Modal, StyleSheet, Text, TouchableOpacity, View, type AlertButton, type AlertOptions } from 'react-native';
import { keyboardModeNow, pushOverlay, useNavFocus, useOverlay, useRepeatingKeys, NAV_REPEAT_KEYS } from './PanelNav';
import { alertBack, alertButtons, alertMove, alertStartIndex } from '../utils/appAlertModel';
import { usePopupStyles, usePopupTheme, usePopupFrame, onMetal, engraveText, PopupKey, PopupPlate, type PopupTokens } from './PopupShell';

interface Pending { title: string; message?: string; buttons: AlertButton[]; options?: AlertOptions }

let queue: Pending[] = [];
const subs = new Set<(q: Pending[]) => void>();
const emit = () => subs.forEach((f) => f(queue));

/** Alert.alert, but answerable by a pad when a pad or keyboard is driving. Same arguments, same behaviour. */
export function appAlert(title: string, message?: string, buttons?: AlertButton[], options?: AlertOptions): void {
  const btns = alertButtons(buttons) as AlertButton[];
  if (keyboardModeNow() && subs.size > 0) {
    queue = [...queue, { title, message, buttons: btns, options }];
    emit();
    return;
  }
  // ★ The native path: swallow pad keys while it is up, released by whichever way it closes.
  const release = pushOverlay({ name: 'native alert', level: 2 });
  const wrapped: AlertButton[] = btns.map((b) => ({ ...b, onPress: (...a: unknown[]) => { release(); (b.onPress as ((...x: unknown[]) => void) | undefined)?.(...a); } }));
  Alert.alert(title, message, wrapped, {
    ...options,
    onDismiss: () => { release(); options?.onDismiss?.(); },
  });
}

function finish(p: Pending, press?: AlertButton) {
  queue = queue.filter((x) => x !== p);
  emit();
  // After the dialog has gone, as the native alert does — a handler that raises another alert gets a clean slate.
  setTimeout(() => {
    if (press) (press.onPress as (() => void) | undefined)?.();
    else p.options?.onDismiss?.();
  }, 0);
}

/** Mount once, near the root (App.tsx), inside the faceplate provider. */
export function AppAlertHost() {
  const [q, setQ] = useState<Pending[]>(queue);
  useEffect(() => { subs.add(setQ); setQ(queue); return () => { subs.delete(setQ); }; }, []);
  const cur = q[0];
  return cur ? <AppAlertDialog key={queue.indexOf(cur) + ':' + cur.title} p={cur} /> : null;
}

function AppAlertDialog({ p }: { p: Pending }) {
  const styles = usePopupStyles(makeStyles);
  const pt = usePopupTheme();
  const metalFrame = usePopupFrame(16, false);
  const ring = useNavFocus().ring;
  const [hl, setHl] = useState(() => alertStartIndex(p.buttons));
  const back = () => {
    const b = alertBack(p.buttons, !!p.options?.cancelable);
    if ('press' in b) finish(p, p.buttons[b.press]);
    else if ('dismiss' in b) finish(p);
  };
  // Registered for the screen's sake (it yields) — the keys themselves are taken just above, at a priority over
  // every overlay, panel and tour, and consumed: nothing behind the dialog hears anything while it is up.
  useOverlay(true, { name: 'app alert', level: 2, blockTuneZoom: true });
  useRepeatingKeys(true, (k) => {
    const n = p.buttons.length;
    if (k === 'ArrowLeft' || k === 'ArrowUp') setHl((i) => alertMove(i, -1, n));
    else if (k === 'ArrowRight' || k === 'ArrowDown') setHl((i) => alertMove(i, 1, n));
    else if (k === 'Enter') finish(p, p.buttons[hl]);
    else if (k === 'Back') back();
    return true;
  }, NAV_REPEAT_KEYS, { priority: 60 });
  const vertical = p.buttons.length > 2;
  return (
    <Modal visible transparent animationType="fade" onRequestClose={back}
           supportedOrientations={['portrait', 'portrait-upside-down', 'landscape', 'landscape-left', 'landscape-right']}>
      <View style={styles.overlay}>
        <View style={[styles.box, metalFrame]}>
          <PopupPlate radius={16} />
          <Text style={styles.title}>{p.title}</Text>
          {!!p.message && <Text style={styles.msg}>{p.message}</Text>}
          <View style={[styles.row, vertical && styles.col]}>
            {p.buttons.map((b, i) => {
              const label = (b.text ?? 'OK');
              const danger = b.style === 'destructive';
              const primary = i === alertStartIndex(p.buttons) && !danger;
              return pt.metal ? (
                <PopupKey key={i} label={label.toUpperCase()} focused={hl === i} danger={danger} primary={primary}
                          onPress={() => finish(p, b)} height={40} fontSize={12}
                          style={vertical ? { alignSelf: 'stretch' } : { minWidth: 96 }} />
              ) : (
                <TouchableOpacity key={i} onPress={() => finish(p, b)} activeOpacity={0.85}
                  style={[styles.btn, primary && styles.btnPrimary, vertical && { alignSelf: 'stretch' },
                          hl === i && { borderColor: ring, borderWidth: 3, margin: -2 }]}>
                  <Text style={[styles.btnTxt, primary && styles.btnTxtPrimary, danger && styles.btnTxtDanger]}>{label}</Text>
                </TouchableOpacity>
              );
            })}
          </View>
        </View>
      </View>
    </Modal>
  );
}

const makeStyles = (pt: PopupTokens) => StyleSheet.create({
  overlay: { flex: 1, justifyContent: 'center', padding: 24, backgroundColor: 'rgba(0,0,0,0.6)' },
  box:     { width: '100%', maxWidth: 440, alignSelf: 'center', backgroundColor: '#0A0804', borderRadius: 10,
             borderWidth: 1, borderColor: pt.ink.border, padding: 20, gap: 14 },
  title:   onMetal(pt, { fontFamily: 'Atkinson Hyperlegible', fontSize: 17, fontWeight: 'bold', color: pt.ink.text },
                   { ...engraveText(pt), fontFamily: 'Atkinson Hyperlegible', fontSize: 17 }),
  msg:     onMetal(pt, { fontFamily: 'Atkinson Hyperlegible', fontSize: 14, lineHeight: 20, color: pt.ink.textSoft },
                   { ...engraveText(pt, pt.note), fontFamily: 'Atkinson Hyperlegible', fontSize: 14, lineHeight: 20 }),
  row:     { flexDirection: 'row', flexWrap: 'wrap', gap: 10, justifyContent: 'flex-end' },
  col:     { flexDirection: 'column', alignItems: 'stretch' },
  btn:     { paddingHorizontal: 18, paddingVertical: 10, borderRadius: 6, borderWidth: 1, borderColor: pt.ink.keyBorder, alignItems: 'center' },
  btnPrimary: { borderColor: pt.gold.selBorder, backgroundColor: pt.ink.keyBgAct },
  btnTxt:  { fontFamily: 'Atkinson Hyperlegible', fontSize: 14, color: pt.ink.key },
  btnTxtPrimary: { color: pt.gold.sel, fontWeight: 'bold' },
  btnTxtDanger:  { color: pt.ink.m.bad },
});
