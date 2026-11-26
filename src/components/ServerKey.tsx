/**
 * ★★★ beta96 the server entry's keys — BIG, and reachable from a TV remote, a game controller and a keyboard.
 *
 * Two roads to the same highlight, because the devices differ:
 *   • a TV REMOTE moves Android's own focus (the Sony, Kiko's box) — onFocus / onBlur light the ring;
 *   • a GAME CONTROLLER or KEYBOARD arrives on the app's key bus (src/utils/hardwareKeys.ts — the pad is consumed
 *     natively, so Android's focus never sees it): useKeyList moves an index up / down, Enter presses, Back goes back.
 * Whichever moved last owns the ring. Lite-safe: React Native only (no Reanimated, no faceplate context).
 */
import React, { useEffect, useRef, useState } from 'react';
import { Text, TouchableOpacity, View, type StyleProp, type ViewStyle } from 'react-native';
import { addKeyListener } from '../utils/hardwareKeys';

/** The ring: white on any skin — the one colour that reads from the sofa on every controls colour. */
export const KEY_RING = '#ffffff';

/** Up / down / Enter / Back over `count` keys. `onEnter(i)` presses key i; `onBack` leaves (undefined = not ours). */
export function useKeyList(count: number, onEnter: (i: number) => void, onBack?: () => void, start = 0) {
  const [idx, setIdx] = useState(start);
  const ref = useRef({ idx, count, onEnter, onBack });
  ref.current = { idx, count, onEnter, onBack };
  useEffect(() => { if (idx >= count && count > 0) setIdx(count - 1); }, [count, idx]);
  useEffect(() => {
    const sub = addKeyListener('down', (e: any) => {
      if (e?.typing) return;
      const r = ref.current;
      if (e.key === 'ArrowDown' || e.key === 'ArrowRight') { setIdx((i) => Math.min(r.count - 1, i + 1)); return true; }
      if (e.key === 'ArrowUp' || e.key === 'ArrowLeft') { setIdx((i) => Math.max(0, i - 1)); return true; }
      if (e.key === 'Enter') { r.onEnter(r.idx); return true; }
      if (e.key === 'Back' && r.onBack) { r.onBack(); return true; }
    });
    return () => sub.remove();
  }, []);
  return [idx, setIdx] as const;
}

type Props = {
  title: string; sub?: string; tag?: string;
  onPress: () => void;
  /** The key bus's highlight is on this key. */
  focused?: boolean;
  /** Take the TV remote's focus first (one per screen). */
  first?: boolean;
  /** Told when the TV remote's focus lands here, so the key bus agrees. */
  onFocusHere?: () => void;
  color: string; dim: string; border: string; fill?: string;
  big?: boolean;
  style?: StyleProp<ViewStyle>;
  fontFamily?: string;
  testID?: string;
};

export default function ServerKey(p: Props) {
  const [tvFocus, setTvFocus] = useState(false);
  const on = tvFocus || !!p.focused;
  return (
    <TouchableOpacity
      testID={p.testID}
      onPress={p.onPress}
      hasTVPreferredFocus={!!p.first}
      onFocus={() => { setTvFocus(true); p.onFocusHere?.(); }}
      onBlur={() => setTvFocus(false)}
      activeOpacity={0.7}
      style={[{
        borderWidth: on ? 3 : 1.5, borderColor: on ? KEY_RING : p.border, borderRadius: 14,
        paddingVertical: p.big ? 22 : 14, paddingHorizontal: 18, marginTop: 12,
        backgroundColor: p.fill ?? 'transparent',
      }, p.style]}>
      <View style={{ flexDirection: 'row', alignItems: 'baseline', flexWrap: 'wrap' }}>
        <Text style={{ color: p.color, fontFamily: p.fontFamily, fontSize: p.big ? 22 : 18, flexShrink: 1 }}>{p.title}</Text>
        {!!p.tag && <Text style={{ color: p.dim, fontFamily: p.fontFamily, fontSize: 13, marginLeft: 10 }}>{p.tag}</Text>}
      </View>
      {!!p.sub && <Text style={{ color: p.dim, fontFamily: p.fontFamily, fontSize: 13, lineHeight: 18, marginTop: 6 }}>{p.sub}</Text>}
    </TouchableOpacity>
  );
}
