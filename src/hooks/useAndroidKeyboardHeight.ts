/**
 * useAndroidKeyboardHeight — the on-screen keyboard's height on ANDROID (0 elsewhere, and while hidden).
 *
 * ★★ For a small box in a <Modal> that must sit above the keyboard. An Android Modal is its own window,
 *  which adjustResize does not shrink, and a KeyboardAvoidingView with behavior="height" inside it chases
 *  its own tail: it shrinks, the layout changes, it re-measures against the keyboard, and grows back —
 *  the Kiwi callsign box "flickering about like mad" behind the keyboard (Stuart, Moto, 2026-10-13).
 *  The fix FreqModal already uses: measure the keyboard ourselves and pad the overlay by it, once.
 *  iOS keeps its KeyboardAvoidingView (padding), which has never misbehaved there.
 */
import { useEffect, useState } from 'react';
import { Keyboard, Platform } from 'react-native';

export function useAndroidKeyboardHeight(): number {
  const [h, setH] = useState(0);
  useEffect(() => {
    if (Platform.OS !== 'android') return;
    const show = Keyboard.addListener('keyboardDidShow', e => setH(e.endCoordinates.height));
    const hide = Keyboard.addListener('keyboardDidHide', () => setH(0));
    return () => { show.remove(); hide.remove(); };
  }, []);
  return h;
}
