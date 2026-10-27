/**
 * ★★ V12 TWO SERVERS AT ONCE — THE BACKGROUND SERVER'S CARD (Stuart's design, GitHub discussion #57, 2026-10-27):
 *
 *   "just under the < Servers button is a little pop out card the same as the health and timer chips and when that pops
 *    out you see a little Frequency Readout with a little Icon to show which ear the audio is in >< (speaker icons
 *    left/right with the left or right icon brighter to indicate the ear it is in) if the user has chosen mono audio
 *    between both servers so no stereo separation then just a simple speaker that illuminates and fades out depending on
 *    audio (thinking squelch) tap the card to quickly switch back to the other server and hold for balance of the audio
 *    If the server in the background has a time limit then that is also shown in this card, and if the backgrounded
 *    servers time limit is approaching the card pops out to show you"
 *
 * ★ The health and time cards' furniture, mirrored onto the LEFT edge under ‹ Servers: the same frame, the same dark
 *   glass, the same arrow pointing at the edge it tucks into, the same tab, the same 260 ms slide (EdgeChip.tsx). One
 *   geometry rule (edgeChipGeometry) read with the left inset, so a rotation, an iPad split or a Mac window resize
 *   re-places it exactly as it does the right-hand cards. Colours are the time card's own (gold → yellow → red), not the
 *   skin's — the same as the cards it sits opposite, on every skin.
 * ★ Tucked or out: out by default; the user's flick is respected until the server has something to SAY (a time stage
 *   rising, STILL LISTENING?, an ending) — dualMix.shouldPopOut.
 * ★ TAP = swap to that server. HOLD = the balance between the two (a slider appears in the card; it closes itself).
 */
import React, { useEffect, useMemo, useRef, useState } from 'react';
import { Animated, AppState, Easing, PanResponder, Pressable, StyleSheet, Text, TouchableOpacity, View,
         useWindowDimensions } from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import Svg, { G, Path } from 'react-native-svg';
import { EDGE_TAB_W, edgeChipGeometry } from './edgeChipGeometry';
import { useIslandSide } from '../hooks/useIslandSide';
import { screenNavRef } from './ScreenNav';
import { bookmarkFreqLabel } from '../utils/bookmarkName';
import { answerStillHere, dualLevels, setDualBalance, useDual } from '../services/dualServer';
import { bgSide, countdownStage, earIcon, fmtLeft, shouldPopOut } from '../services/dualMix';

/** A server's name at a slider's end: the left end is server 1 (the left ear), the right end server 2. */
const short = (n?: string) => { const t = (n ?? '').trim(); return t.length > 9 ? `${t.slice(0, 8)}…` : t || '—'; };

const STAGE_TAB   = ['#ffb833', '#ffe14d', '#ff6b6b'];                                        // = SDRScreen TIME_TAB
const STAGE_FRAME = ['rgba(255,160,0,0.55)', 'rgba(255,225,77,0.7)', 'rgba(255,90,90,0.75)']; // = the time card's

/** A speaker; `flip` faces it left. */
function Speaker({ colour, opacity, flip, waves = true }: { colour: string; opacity: number; flip?: boolean; waves?: boolean }) {
  return (
    <Svg width={17} height={17} viewBox="0 0 24 24" style={{ opacity }}>
      <G transform={flip ? 'translate(24,0) scale(-1,1)' : undefined}>
        <Path d="M3 9v6h4l5 4V5L7 9H3z" fill={colour} />
        {waves && <Path d="M15.5 8.5a5 5 0 0 1 0 7M18.3 5.7a9 9 0 0 1 0 12.6" fill="none" stroke={colour}
                         strokeWidth={2} strokeLinecap="round" />}
      </G>
    </Svg>
  );
}

export default function DualServerCard({ myId, top, onSwap, boxOpen = false }: {
  /** This screen's side of the pairing — the card shows the OTHER one, and only while this screen is in front. */
  myId: string;
  /** Under ‹ Servers. */
  top: number;
  /** TAP: bring the background server to the front (SDRScreen navigates). */
  onSwap: () => void;
  /** A decoder / DAB / Advanced RDS box is open: tuck, as the health and time cards do (they fouled the boxes). */
  boxOpen?: boolean;
}) {
  const st = useDual();
  const bg = st.fg === myId ? bgSide(st) : null;
  const index = bg ? (st.sides.indexOf(bg) as 0 | 1) : 0;
  const icon = earIcon(st.output, index);

  /* ── the clock ── */
  const [now, setNow] = useState(() => Date.now());
  const ticking = !!bg && (bg.endsAt != null || bg.asking != null);
  useEffect(() => {
    if (!ticking) return;
    const t = setInterval(() => { if (AppState.currentState === 'active') setNow(Date.now()); }, 1000);
    return () => clearInterval(t);
  }, [ticking]);
  const left = bg?.endsAt != null ? bg.endsAt - now : null;
  const stage = bg ? countdownStage(left, bg.soft) : 0;
  const asking = !!bg?.asking && bg.asking > now;
  const ended = !!bg?.ended;

  /* ── tucked / out ── */
  const [tucked, setTucked] = useState(false);
  const prev = useRef({ stage: 0, asking: false, ended: false });
  useEffect(() => {
    const cur = { stage, asking, ended };
    if (shouldPopOut(prev.current, cur)) setTucked(false);
    prev.current = cur;
  }, [stage, asking, ended]);
  // ★ A box opening tucks it (out again from the tab, or by itself when the server has something to say).
  useEffect(() => { if (boxOpen) setTucked(true); }, [boxOpen]);

  /* ── the MONO speaker follows this server's level (squelch opening and closing) ── */
  const lamp = useRef(new Animated.Value(0.35)).current;
  const wantLamp = !!bg && icon === 'mono' && !tucked && !ended;
  useEffect(() => {
    if (!wantLamp || !bg) return;
    let live = true;
    const which = bg.transport;
    const t = setInterval(() => {
      if (AppState.currentState !== 'active') return;
      dualLevels().then((l) => {
        if (!live) return;
        const v = which === 'native' ? l.native : l.aux;
        // ~ −46 dBFS dark … −10 dBFS full; a lamp, not a meter.
        const db = 20 * Math.log10(Math.max(1e-6, v));
        const k = Math.max(0, Math.min(1, (db + 46) / 36));
        Animated.timing(lamp, { toValue: 0.25 + 0.75 * k, duration: 140, useNativeDriver: true }).start();
      });
    }, 150);
    return () => { live = false; clearInterval(t); };
  }, [wantLamp, bg?.transport]);   // eslint-disable-line react-hooks/exhaustive-deps

  /* ── HOLD: the balance ── */
  const [balOpen, setBalOpen] = useState(false);
  const balTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const armBalClose = () => {
    if (balTimer.current) clearTimeout(balTimer.current);
    balTimer.current = setTimeout(() => setBalOpen(false), 4000);
  };
  useEffect(() => () => { if (balTimer.current) clearTimeout(balTimer.current); }, []);
  const trackW = useRef(1);
  const balRef = useRef(st.balance); balRef.current = st.balance;
  const balPan = useMemo(() => PanResponder.create({
    onStartShouldSetPanResponder: () => true,
    onMoveShouldSetPanResponder: () => true,
    onPanResponderGrant: (e) => {
      if (balTimer.current) clearTimeout(balTimer.current);
      setDualBalance((e.nativeEvent.locationX / trackW.current) * 2 - 1, false);
    },
    onPanResponderMove: (e) => setDualBalance((e.nativeEvent.locationX / trackW.current) * 2 - 1, false),
    onPanResponderRelease: () => { setDualBalance(balRef.current, true); armBalClose(); },
    onPanResponderTerminate: () => { setDualBalance(balRef.current, true); armBalClose(); },
  }), []);   // eslint-disable-line react-hooks/exhaustive-deps

  /* ── the left edge (EdgeChip, mirrored) ── */
  const insets = useSafeAreaInsets();
  const { width: windowW, height: windowH } = useWindowDimensions();
  const island = useIslandSide();
  const mirrored = island === 'left' ? 'right' : island === 'right' ? 'left' : island;
  const [w, setW] = useState(170);
  const [h, setH] = useState(0);
  const geo = edgeChipGeometry({ windowW, windowH, cardW: w, cardH: h, top, islandSide: mirrored,
                                 insets: { top: insets.top, bottom: insets.bottom, right: insets.left, left: insets.right } });
  const off = geo.cardOff;
  const x = useRef(new Animated.Value(0)).current;
  const first = useRef(true);
  useEffect(() => {
    if (first.current) { first.current = false; x.setValue(tucked ? off : 0); return; }
    Animated.timing(x, { toValue: tucked ? off : 0, duration: 260, easing: Easing.out(Easing.cubic),
                         useNativeDriver: true }).start();
  }, [tucked]);   // eslint-disable-line react-hooks/exhaustive-deps
  const lastOff = useRef(off);
  useEffect(() => {
    if (lastOff.current === off) return;
    lastOff.current = off;
    x.stopAnimation();
    x.setValue(tucked ? off : 0);
  }, [off]);   // eslint-disable-line react-hooks/exhaustive-deps
  const offRef = useRef(off); offRef.current = off;
  // Flick LEFT to tuck (the edge is on the left); drag the tab RIGHT to bring it back.
  const pan = useMemo(() => PanResponder.create({
    onMoveShouldSetPanResponder: (_e, g) => g.dx < -6 && Math.abs(g.dx) > Math.abs(g.dy),
    onPanResponderMove: (_e, g) => x.setValue(Math.min(0, g.dx) * -1),
    onPanResponderRelease: (_e, g) => {
      if (g.dx < -50 || g.vx < -0.5) {
        Animated.timing(x, { toValue: offRef.current, duration: 180, useNativeDriver: true }).start(() => setTucked(true));
      } else Animated.spring(x, { toValue: 0, useNativeDriver: true, bounciness: 6 }).start();
    },
    onPanResponderTerminate: () => Animated.spring(x, { toValue: 0, useNativeDriver: true }).start(),
  }), []);   // eslint-disable-line react-hooks/exhaustive-deps
  const tabPan = useMemo(() => PanResponder.create({
    onMoveShouldSetPanResponder: (_e, g) => g.dx > 6 && Math.abs(g.dx) > Math.abs(g.dy),
    onPanResponderRelease: (_e, g) => { if (g.dx > 20) setTucked(false); },
  }), []);
  // x runs 0 (out) … off (tucked) as EdgeChip's does; on this edge "out of sight" is NEGATIVE translateX.
  const cardX = Animated.multiply(x, -1);
  const tabX = x.interpolate({ inputRange: [0, Math.max(1, off)], outputRange: [-geo.tabOff, 0], extrapolate: 'clamp' });

  if (!bg) return null;

  const colour = STAGE_TAB[stage];
  const frame = ended ? 'rgba(255,90,90,0.75)' : asking ? STAGE_FRAME[1] : STAGE_FRAME[stage];
  const tabColour = ended ? '#ff6b6b' : asking ? STAGE_TAB[1] : colour;
  const freq = bg.freqHz > 0 ? bookmarkFreqLabel(bg.freqHz) : '';
  const mode = (bg.mode || '').toUpperCase();
  const earA11y = icon === 'mono' ? 'in both ears' : icon === 'L' ? 'in your left ear' : 'in your right ear';

  const onPress = () => {
    if (ended) return;
    if (asking) { answerStillHere(bg.id); return; }
    onSwap();
  };

  return (
    <>
      <Animated.View {...pan.panHandlers} pointerEvents={tucked ? 'none' : 'box-none'}
        onLayout={(e) => {
          const v = Math.ceil(e.nativeEvent.layout.width); if (v > 0 && v !== w) setW(v);
          const hv = Math.ceil(e.nativeEvent.layout.height); if (hv > 0 && hv !== h) setH(hv);
        }}
        style={[s.card, { top, left: 0, maxWidth: Math.min(geo.maxCardW, 300), transform: [{ translateX: cardX }] }]}>
        <View style={[s.row, { borderColor: frame, paddingLeft: geo.padRight }]}>
          {/* ★ The pad / keyboard reach it like every other screen control (ScreenNav): Enter = the tap. */}
          <Pressable ref={screenNavRef('dualCard', tucked ? undefined : onPress)}
            onPress={onPress} onLongPress={() => { if (!ended) { setBalOpen(true); armBalClose(); } }}
            delayLongPress={450} style={({ pressed }) => [s.body, { opacity: pressed ? 0.8 : 1 }]}
            accessibilityRole="button"
            accessibilityLabel={`${bg.name}${freq ? `, ${freq} ${mode}` : ''}, ${earA11y}`}
            accessibilityHint={ended ? undefined : asking ? 'Tap to tell the server you are still listening'
                                                          : 'Tap to switch to this server. Hold for the balance between the two.'}>
            <View style={s.top}>
              {icon === 'mono'
                ? <Animated.View style={{ opacity: lamp }}><Speaker colour={colour} opacity={1} /></Animated.View>
                : <View style={s.ears}>
                    <Speaker colour={colour} opacity={icon === 'L' ? 1 : 0.22} flip waves={icon === 'L'} />
                    <Speaker colour={colour} opacity={icon === 'R' ? 1 : 0.22} waves={icon === 'R'} />
                  </View>}
              <Text style={[s.name, { color: colour }]} numberOfLines={1}>{bg.name}</Text>
            </View>
            {ended ? (
              <Text style={[s.line, { color: '#ff8c8c' }]} numberOfLines={2}>Ended — {bg.ended}</Text>
            ) : asking ? (
              <Text style={[s.line, { color: STAGE_TAB[1] }]} numberOfLines={2}>
                Still listening? Tap — {fmtLeft((bg.asking ?? now) - now)}
              </Text>
            ) : (
              <View style={s.top}>
                {!!freq && <Text style={[s.freq, { color: colour }]}>{freq}</Text>}
                {!!mode && <Text style={[s.mode, { color: colour }]}>{mode}</Text>}
                {left != null && (
                  <Text style={[s.clock, { color: colour }]}>
                    {bg.soft && left <= 0 ? 'yours for now' : `⏳ ${fmtLeft(left)}`}
                  </Text>
                )}
              </View>
            )}
            {balOpen && !ended && (
              <View style={s.balRow}>
                <Text style={[s.balEnd, { color: colour }]} numberOfLines={1}>{short(st.sides[0]?.name)}</Text>
                <View style={s.track} {...balPan.panHandlers}
                      onLayout={(e) => { trackW.current = Math.max(1, e.nativeEvent.layout.width); }}
                      accessibilityRole="adjustable" accessibilityLabel="Balance between the two servers">
                  <View style={[s.rail, { backgroundColor: colour }]} />
                  <View style={[s.mid, { backgroundColor: colour }]} />
                  <View style={[s.knob, { borderColor: colour, left: `${((st.balance + 1) / 2) * 100}%` }]} />
                </View>
                <Text style={[s.balEnd, { color: colour }]} numberOfLines={1}>{short(st.sides[1]?.name)}</Text>
              </View>
            )}
          </Pressable>
          <TouchableOpacity onPress={() => Animated.timing(x, { toValue: offRef.current, duration: 200, useNativeDriver: true })
                                          .start(() => setTucked(true))}
            hitSlop={{ top: 8, bottom: 8, left: 2, right: 8 }} accessibilityRole="button"
            accessibilityLabel={`Hide ${bg.name}`} style={s.arrowHit}>
            <Text style={[s.arrow, { color: tabColour }]}>‹</Text>
          </TouchableOpacity>
        </View>
      </Animated.View>
      <Animated.View {...tabPan.panHandlers} pointerEvents={tucked ? 'auto' : 'none'}
        style={[s.tabWrap, { top, left: 0, transform: [{ translateX: tabX }] }]}>
        <TouchableOpacity onPress={() => setTucked(false)} activeOpacity={0.7} hitSlop={{ top: 6, bottom: 6, left: 4, right: 10 }}
          accessibilityRole="button" accessibilityLabel={`Show ${bg.name}`}
          style={[s.tab, { borderColor: tabColour, width: geo.tabW, paddingLeft: geo.tabW - EDGE_TAB_W }]}>
          <Text style={[s.chev, { color: tabColour }]}>›</Text>
          <Speaker colour={tabColour} opacity={1} waves={false} />
        </TouchableOpacity>
      </Animated.View>
    </>
  );
}

const s = StyleSheet.create({
  card:    { position: 'absolute', zIndex: 250 },
  // ONE frame: open and flush on the LEFT — the card joins that edge (EdgeChip's, mirrored).
  row:     { flexDirection: 'row', alignItems: 'stretch', backgroundColor: 'rgba(8,10,8,0.86)',
             borderWidth: 1.5, borderLeftWidth: 0, borderTopRightRadius: 12, borderBottomRightRadius: 12 },
  body:    { paddingVertical: 5, paddingLeft: 8, paddingRight: 2, gap: 2, flexShrink: 1 },
  top:     { flexDirection: 'row', alignItems: 'center', gap: 6 },
  ears:    { flexDirection: 'row', alignItems: 'center', gap: 1 },
  name:    { fontFamily: 'Atkinson Hyperlegible', fontSize: 12, letterSpacing: 0.3, flexShrink: 1, maxWidth: 190 },
  freq:    { fontFamily: 'Atkinson Hyperlegible', fontSize: 15, fontWeight: '700', fontVariant: ['tabular-nums'] },
  mode:    { fontFamily: 'Atkinson Hyperlegible', fontSize: 10, letterSpacing: 0.8, opacity: 0.85 },
  clock:   { fontFamily: 'Atkinson Hyperlegible', fontSize: 11, fontVariant: ['tabular-nums'], opacity: 0.95 },
  line:    { fontFamily: 'Atkinson Hyperlegible', fontSize: 12, maxWidth: 220 },
  balRow:  { flexDirection: 'row', alignItems: 'center', gap: 6, marginTop: 4, marginBottom: 2 },
  balEnd:  { fontFamily: 'Atkinson Hyperlegible', fontSize: 10, opacity: 0.85, maxWidth: 64 },
  track:   { width: 120, height: 28, justifyContent: 'center' },
  rail:    { height: 2, borderRadius: 1, opacity: 0.5 },
  mid:     { position: 'absolute', left: '50%', width: 1, height: 10, marginLeft: -0.5, opacity: 0.6 },
  knob:    { position: 'absolute', width: 16, height: 16, marginLeft: -8, borderRadius: 8, borderWidth: 2,
             backgroundColor: 'rgba(8,10,8,0.95)' },
  arrowHit: { justifyContent: 'center', paddingLeft: 1, paddingRight: 7 },
  arrow:   { fontSize: 20, fontWeight: '700', lineHeight: 22 },
  tabWrap: { position: 'absolute', zIndex: 251 },
  tab:     { height: 52, borderTopRightRadius: 12, borderBottomRightRadius: 12, borderWidth: 1.5, borderLeftWidth: 0,
             backgroundColor: 'rgba(14,12,8,0.88)', alignItems: 'center', justifyContent: 'center', gap: 2 },
  chev:    { fontSize: 20, fontWeight: '700', lineHeight: 20, marginTop: -2 },
});
