/**
 * ★★★ A CARD THAT TUCKS INTO THE SCREEN EDGE (Stuart, 2026-10-03, after the SERVER HEALTH and TIME cards had fouled
 * the decoder boxes "for ages"): flick it to the right and it slides off into a small tab on the edge — the way
 * picture-in-picture video tucks away — with its icon and a ‹. Tap the tab (or drag it left) and the card comes back.
 *
 *  ★ The tab carries the card's state at a glance: the health tab is the pill's own colour (HealthPill
 *    healthSummary), the time tab turns yellow, then red, as the countdown does.
 *  ★ No "Slide to dismiss" hint: the card joined to the edge with a › pointing at it says so (Stuart, 2026-10-03).
 *  ★ Tucked or shown is the CALLER's state (SDRScreen decides it from the boxes and the urgency); this only
 *    animates between them and reports the user's flick / tap.
 */
import React, { useEffect, useRef, useState } from 'react';
import { Animated, Easing, PanResponder, StyleSheet, Text, TouchableOpacity, View, useWindowDimensions } from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import Svg, { Circle, Path } from 'react-native-svg';
import { EDGE_TAB_H, EDGE_TAB_W, edgeChipGeometry } from './edgeChipGeometry';

export default function EdgeChip({ top, tucked, onTuck, onShow, tabColour, tabIcon, label,
                                   frameColour, children }: {
  /** The card's frame (it draws ONE frame round the arrow and the content; the content is drawn bare).
   *  Default: the tab's colour. */
  frameColour?: string;
  /** ★ No `right` any more (2026-10-06): the card and tab sit on the PHYSICAL edge and pad their content by the
   *  safe-area inset themselves — edgeChipGeometry, the one rule both of them read. */
  top: number;
  tucked: boolean;
  /** The user flicked the card away. */
  onTuck: () => void;
  /** The user asked for it back (tapped / dragged the tab). */
  onShow: () => void;
  tabColour: string;
  tabIcon: React.ReactNode;
  /** What the card is, for the tab's accessibility label ("Server health", "Time remaining"). */
  label: string;
  children: React.ReactNode;
}) {
  const [w, setW] = useState(160);
  /* ★★★ ONE GEOMETRY, FROM THE WINDOW (Stuart, 2026-10-06, RC15 landscape: the chips sat ~43 pt inside the glass,
   *  and once an open card ran off the edge beside tabs that were still inset). The card and the tab were both
   *  placed at the caller's safe-area inset — the safe area's edge, not the screen's — and each worked out its
   *  own slide distance from it. Now both are at right 0 with their content padded in by the inset, and everything
   *  comes from edgeChipGeometry, read here from the live window + insets so a rotation, an iPad split or a Mac
   *  window resize re-places both at once. */
  const insets = useSafeAreaInsets();
  const { width: windowW } = useWindowDimensions();
  const geo = edgeChipGeometry({ windowW, insets, cardW: w });
  const off = geo.cardOff;                          // fully past the screen edge
  const x = useRef(new Animated.Value(tucked ? off : 0)).current;
  const first = useRef(true);
  useEffect(() => {
    if (first.current) { first.current = false; x.setValue(tucked ? off : 0); return; }
    Animated.timing(x, { toValue: tucked ? off : 0, duration: 260, easing: Easing.out(Easing.cubic),
                         useNativeDriver: true }).start();
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [tucked]);

  /* ★★ ROTATION (Stuart, 2026-10-04: "the health and clock anchors at the edge of the screen are not lining up upon
   *  rotation to landscape"). `off` depends on `right` (the notch inset — 0 in portrait, ~45 pt in landscape) and
   *  the card's width, and the card was only ever placed when tucked/shown CHANGED. After a rotation a tucked card
   *  and its tab sat at the portrait distances. Re-place them, without animating, whenever `off` moves. */
  const lastOff = useRef(off);
  useEffect(() => {
    if (lastOff.current === off) return;
    lastOff.current = off;
    x.stopAnimation();
    x.setValue(tucked ? off : 0);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [off]);

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
  // ★ "Out" is PAST THE SCREEN EDGE: the tab is anchored at `right` (the notch inset), so sliding it only its own
  //   width left it showing in the inset — beside the open card, in landscape (Stuart's screenshot, 2026-10-04).
  //   ★ 2026-10-06: the tab now reaches the glass itself (geo.tabW includes the inset), so `tabOff` is its whole width.
  const tabX = x.interpolate({ inputRange: [0, off], outputRange: [geo.tabOff, 0], extrapolate: 'clamp' });
  const tabPan = useRef(PanResponder.create({
    onMoveShouldSetPanResponder: (_e, g) => g.dx < -6 && Math.abs(g.dx) > Math.abs(g.dy),
    onPanResponderRelease: (_e, g) => { if (g.dx < -20) onShowRef.current(); },
  })).current;
  const onShowRef = useRef(onShow); onShowRef.current = onShow;

  return (
    <>
      <Animated.View {...pan.panHandlers} pointerEvents={tucked ? 'none' : 'auto'}
        onLayout={(e) => { const v = Math.ceil(e.nativeEvent.layout.width); if (v > 0 && v !== w) setW(v); }}
        style={[ec.card, { top, right: geo.right, maxWidth: geo.maxCardW, transform: [{ translateX: x }] }]}>
        {/* ★ JOINED TO THE EDGE, with a › pointing at it (Stuart, 2026-10-03: "move them to join the edge of the
            screen and then put an arrow to the left of the content pointing to the edge of the screen to show the
            card can be collapsed away"). The child squares its right side (see the callers); the arrow sits inside
            its frame, on the left, and tapping it tucks the card too. */}
        <View style={[ec.row, { borderColor: frameColour ?? tabColour, paddingRight: geo.padRight }]}>
          <TouchableOpacity onPress={() => Animated.timing(x, { toValue: offRef.current, duration: 200, useNativeDriver: true })
                                          .start(() => onTuckRef.current())}
            hitSlop={{ top: 8, bottom: 8, left: 8, right: 2 }} accessibilityRole="button" accessibilityLabel={`Hide ${label}`}
            style={ec.arrowHit}>
            <Text style={[ec.arrow, { color: tabColour }]}>›</Text>
          </TouchableOpacity>
          {children}
        </View>
      </Animated.View>
      <Animated.View {...tabPan.panHandlers} pointerEvents={tucked ? 'auto' : 'none'}
        style={[ec.tabWrap, { top, right: geo.right, transform: [{ translateX: tabX }] }]}>
        <TouchableOpacity onPress={onShow} activeOpacity={0.7} hitSlop={{ top: 6, bottom: 6, left: 10, right: 4 }}
          accessibilityRole="button" accessibilityLabel={`Show ${label}`}
          style={[ec.tab, { borderColor: tabColour, width: geo.tabW, paddingRight: geo.padRight }]}>
          {tabIcon}
          <Text style={[ec.chev, { color: tabColour }]}>‹</Text>
        </TouchableOpacity>
      </Animated.View>
    </>
  );
}

const ec = StyleSheet.create({
  card:     { position: 'absolute', zIndex: 250 },
  // ONE frame: rounded on the left, open and flush on the right — the card JOINS the edge.
  row:      { flexDirection: 'row', alignItems: 'stretch', backgroundColor: 'rgba(8,10,8,0.86)',
              borderWidth: 1.5, borderRightWidth: 0, borderTopLeftRadius: 12, borderBottomLeftRadius: 12 },
  arrowHit: { justifyContent: 'center', paddingLeft: 7, paddingRight: 1 },
  arrow:    { fontSize: 20, fontWeight: '700', lineHeight: 22 },
  tabWrap:  { position: 'absolute', zIndex: 251 },
  tab:      { width: EDGE_TAB_W, height: EDGE_TAB_H, borderTopLeftRadius: 12, borderBottomLeftRadius: 12,
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
