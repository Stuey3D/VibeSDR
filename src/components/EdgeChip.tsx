/**
 * ★★★ A CARD THAT TUCKS INTO THE SCREEN EDGE (Stuart, 2026-10-03, after the SERVER HEALTH and TIME cards had fouled
 * the decoder boxes "for ages"): flick it to the right and it slides off into a small tab on the edge — the way
 * picture-in-picture video tucks away — with its icon and a ‹. Tap the tab (or drag it left) and the card comes back.
 *
 *  ★ The tab carries the card's state at a glance: the health tab is the pill's own colour (HealthPill
 *    healthSummary), the time tab turns yellow, then red, as the countdown does.
 *  ★ `hint`: when a card comes out BY ITSELF over an open decoder box (the server in the red, the countdown
 *    running out), it says "Slide to dismiss ›" for a moment first, so nobody has to guess it can go.
 *  ★ Tucked or shown is the CALLER's state (SDRScreen decides it from the boxes and the urgency); this only
 *    animates between them and reports the user's flick / tap.
 */
import React, { useEffect, useRef, useState } from 'react';
import { Animated, Easing, PanResponder, StyleSheet, Text, TouchableOpacity, View } from 'react-native';
import Svg, { Circle, Path } from 'react-native-svg';

const TAB_W = 26;
const TAB_H = 52;
const HINT_MS = 2500;

export default function EdgeChip({ top, right, tucked, onTuck, onShow, hint = false, tabColour, tabIcon, label,
                                   children }: {
  top: number; right: number;
  tucked: boolean;
  /** The user flicked the card away. */
  onTuck: () => void;
  /** The user asked for it back (tapped / dragged the tab). */
  onShow: () => void;
  /** Show "Slide to dismiss ›" for a moment when the card appears. */
  hint?: boolean;
  tabColour: string;
  tabIcon: React.ReactNode;
  /** What the card is, for the tab's accessibility label ("Server health", "Time remaining"). */
  label: string;
  children: React.ReactNode;
}) {
  const [w, setW] = useState(160);
  const off = w + right + 12;                       // fully past the screen edge
  const x = useRef(new Animated.Value(tucked ? off : 0)).current;
  const first = useRef(true);
  useEffect(() => {
    if (first.current) { first.current = false; x.setValue(tucked ? off : 0); return; }
    Animated.timing(x, { toValue: tucked ? off : 0, duration: 260, easing: Easing.out(Easing.cubic),
                         useNativeDriver: true }).start();
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [tucked]);

  // The hint, shown each time the card comes out with `hint` set.
  const [hinting, setHinting] = useState(false);
  useEffect(() => {
    if (tucked || !hint) { setHinting(false); return; }
    setHinting(true);
    const t = setTimeout(() => setHinting(false), HINT_MS);
    return () => clearTimeout(t);
  }, [tucked, hint]);

  const offRef = useRef(off); offRef.current = off;
  const pan = useRef(PanResponder.create({
    onMoveShouldSetPanResponder: (_e, g) => g.dx > 6 && Math.abs(g.dx) > Math.abs(g.dy),
    onPanResponderMove: (_e, g) => x.setValue(Math.max(0, g.dx)),
    onPanResponderRelease: (_e, g) => {
      if (g.dx > 50 || g.vx > 0.5) {
        Animated.timing(x, { toValue: offRef.current, duration: 180, useNativeDriver: true }).start(() => onTuckRef.current());
      } else {
        Animated.spring(x, { toValue: 0, useNativeDriver: true, bounciness: 6 }).start();
      }
    },
    onPanResponderTerminate: () => Animated.spring(x, { toValue: 0, useNativeDriver: true }).start(),
  })).current;
  const onTuckRef = useRef(onTuck); onTuckRef.current = onTuck;

  // The tab is the card's mirror: in when the card is out, out when it is in.
  const tabX = x.interpolate({ inputRange: [0, off], outputRange: [TAB_W + 4, 0], extrapolate: 'clamp' });
  const tabPan = useRef(PanResponder.create({
    onMoveShouldSetPanResponder: (_e, g) => g.dx < -6 && Math.abs(g.dx) > Math.abs(g.dy),
    onPanResponderRelease: (_e, g) => { if (g.dx < -20) onShowRef.current(); },
  })).current;
  const onShowRef = useRef(onShow); onShowRef.current = onShow;

  return (
    <>
      <Animated.View {...pan.panHandlers} pointerEvents={tucked ? 'none' : 'auto'}
        onLayout={(e) => { const v = Math.ceil(e.nativeEvent.layout.width); if (v > 0 && v !== w) setW(v); }}
        style={[ec.card, { top, right, transform: [{ translateX: x }] }]}>
        <View style={hinting ? ec.hidden : undefined}>{children}</View>
        {hinting && (
          <View pointerEvents="none" style={ec.hintWrap}>
            <Text style={ec.hint} numberOfLines={1}>Slide to dismiss ›</Text>
          </View>
        )}
      </Animated.View>
      <Animated.View {...tabPan.panHandlers} pointerEvents={tucked ? 'auto' : 'none'}
        style={[ec.tabWrap, { top, transform: [{ translateX: tabX }] }]}>
        <TouchableOpacity onPress={onShow} activeOpacity={0.7} hitSlop={{ top: 6, bottom: 6, left: 10, right: 4 }}
          accessibilityRole="button" accessibilityLabel={`Show ${label}`}
          style={[ec.tab, { borderColor: tabColour }]}>
          {tabIcon}
          <Text style={[ec.chev, { color: tabColour }]}>‹</Text>
        </TouchableOpacity>
      </Animated.View>
    </>
  );
}

const ec = StyleSheet.create({
  card:     { position: 'absolute', zIndex: 250 },
  hidden:   { opacity: 0 },
  hintWrap: { position: 'absolute', left: 0, right: 0, top: 0, bottom: 0, alignItems: 'center', justifyContent: 'center',
              borderRadius: 8, backgroundColor: 'rgba(8,6,2,0.92)', borderWidth: 1, borderColor: 'rgba(255,184,51,0.55)' },
  hint:     { color: '#ffb833', fontFamily: 'Atkinson Hyperlegible', fontSize: 13, fontWeight: '700', letterSpacing: 0.5 },
  tabWrap:  { position: 'absolute', right: 0, zIndex: 251 },
  tab:      { width: TAB_W, height: TAB_H, borderTopLeftRadius: 12, borderBottomLeftRadius: 12,
              borderWidth: 1.5, borderRightWidth: 0, backgroundColor: 'rgba(14,12,8,0.88)',
              alignItems: 'center', justifyContent: 'center', gap: 2 },
  chev:     { fontSize: 20, fontWeight: '700', lineHeight: 20, marginTop: -2 },
});

/** The health tab's glyph: a cross, in the pill's colour. */
export function HealthTabIcon({ colour }: { colour: string }) {
  return <Text style={{ color: colour, fontSize: 18, fontWeight: '800', lineHeight: 18 }}>+</Text>;
}

/** The time tab's glyph: a clock face. */
export function ClockTabIcon({ colour }: { colour: string }) {
  return (
    <Svg width={15} height={15} viewBox="0 0 24 24">
      <Circle cx={12} cy={12} r={9} fill="none" stroke={colour} strokeWidth={2.2} />
      <Path d="M12 7v5l3.5 2" fill="none" stroke={colour} strokeWidth={2.2} strokeLinecap="round" strokeLinejoin="round" />
    </Svg>
  );
}
