/**
 * PadNotice — the short notice when HOLD START switches a game controller's controls (src/utils/padActions.ts):
 * which controls are now in use, and "hold START to switch back". The web page's toast, in the same words and colours.
 *
 * ★ Mounted once, at the root (App.tsx), and draws nothing until a pad switches.
 * ★ On Android the system toast is used instead: it is drawn above everything, the frequency card and every other
 *   pop-up included (a React Native Modal is a window of its own, and a view at the root sits under it).
 */
import React, { useEffect, useRef, useState } from 'react';
import { Platform, Text, ToastAndroid, View } from 'react-native';
import { addPadNoticeListener } from '../utils/hardwareKeys';

const SHOW_MS = 2600;

export default function PadNotice() {
  const [text, setText] = useState<string | null>(null);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  useEffect(() => {
    const sub = addPadNoticeListener((t) => {
      if (Platform.OS === 'android') { ToastAndroid.show(t, ToastAndroid.LONG); return; }
      setText(t);
      if (timer.current) clearTimeout(timer.current);
      timer.current = setTimeout(() => setText(null), SHOW_MS);
    });
    return () => { sub.remove(); if (timer.current) clearTimeout(timer.current); };
  }, []);
  if (!text) return null;
  return (
    <View pointerEvents="none" style={{ position: 'absolute', left: 16, right: 16, bottom: 40, alignItems: 'center', zIndex: 9600 }}>
      <View accessibilityLiveRegion="polite" style={{
        maxWidth: 560, backgroundColor: 'rgba(40,10,0,0.94)', borderColor: 'rgba(255,120,0,0.6)', borderWidth: 1,
        borderRadius: 8, paddingVertical: 8, paddingHorizontal: 14,
      }}>
        <Text style={{ color: '#ffb833', fontSize: 13, fontFamily: Platform.OS === 'ios' ? 'Menlo' : 'monospace', textAlign: 'center' }}>
          {text}
        </Text>
      </View>
    </View>
  );
}
