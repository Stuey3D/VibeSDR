/**
 * AdsbLiveList — the ADS-B map's live list, in the bottom-left corner (V12; Stuart, 2026-10-24: "the full-screen map
 * is on … a live list in the bottom corner"). What it says and how big it is: src/services/adsbList.ts.
 *
 * ★★★ DRAWN BY REACT NATIVE OVER THE WEBVIEW, NOT INSIDE THE MAP PAGE — because of the controller. A game pad and the
 *     keyboard reach the app through hardwareKeys → PanelNav, never through the page: on iOS and the Mac a pad is a
 *     GCController the WebView never sees, and on Android / Android TV the page cannot know which surface owns the
 *     D-pad. Rows registered here walk with the D-pad (up/down), Enter finds the aircraft, Back passes on to the map's
 *     overlay and leaves ADS-B — the scheme every other surface uses (gamepad_mapping: D-pad highlights, back leaves).
 *     Drawn in the page, the same rows would be touch-only. ★ Touch is unchanged either way: RN views over a WebView
 *     take their own taps and the rest (`box-none`) fall through to the map.
 * ★★ THE BOTTOM-LEFT CORNER, because the others are taken: the layer's AUTO · FIT · RINGS · ⌖ top-right, the +/− stack
 *    and the attribution ⓘ bottom-right (the page narrows the attribution's text to the room left of the list — see
 *    __adsbListW in MapOverlay), and the page moves its own ⓘ legend to the top-left in ADS-B.
 * ★★ COLLAPSES TO A PILL (✈ count) so it hides next to nothing of the map; remembered per device.
 * ★ The map's colours, not the app theme's: it sits on the map's own dark chrome (MapOverlay's `ink`), so it looks
 *   the same on every faceplate, with transparency on or off — it is part of the map, not a panel of the receiver.
 * ★ The decoder font — the display font is for the main controls only.
 */
import { scrollLane } from '../constants/popupTokens';
import React, { useEffect, useMemo, useRef, useState } from 'react';
import { AccessibilityInfo, Animated, Easing, Image, ScrollView, StyleSheet, Text, TouchableOpacity, View } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { NavCtx, NavRow, NavRing, useNavButton, useNavFocus, usePanelNav, noteTouchInteraction } from './PanelNav';
import { DECODER_FONT } from './DecoderShell';
import type { Aircraft } from '../services/SDRBackend';
import { adsbListRows, adsbListRowsFit, adsbListWidth, parseListOpen, LIST_HEAD_H, LIST_ROW_H, EDGE,
         type AdsbListRow } from '../services/adsbList';

const OPEN_KEY = 'lsv_adsb_list_open';

export interface AdsbLiveListProps {
  /** The report in the app's shape, ranged from the receiver (SDRScreen's `aircraft`). */
  aircraft: Aircraft[];
  /** False until the first report: "taking the receiver to 1090 MHz" rather than "no aircraft". */
  started: boolean;
  /** A row was chosen — find it on the map (the layer's focus(hex)). */
  onFocus: (hex: string) => void;
  /** The (i) in the header: the map layer's colour key, opened above the list (toggleLegend). */
  onLegend?: () => void;
  /** The list's footprint changed (width and height, 0 × 0 as a pill) — the page keeps the attribution clear of it,
   *  and the map layer keeps aircraft clear of it (setObstacle). */
  onFootprint?: (w: number, h: number) => void;
  /** The map's colours as rgb triplets (MapOverlay `ink`): text, controls, controls-as-legend. */
  ink: { x: string; k: string; kt: string };
  /** ★ V12: where the server serves the owner's operator logos ("http(s)://host/adsb/"), or null — the row then shows
   *  the operator's ICAO code as a badge. The server names a logo only when the file exists. */
  logoBase?: string | null;
  /** The modal's window and the parent's safe-area insets (the hook reads 0 inside a Modal — BrowserOverlay's note). */
  areaW: number;
  areaH: number;
  insets: { top: number; bottom: number; left: number; right: number };
}

export default function AdsbLiveList(p: AdsbLiveListProps) {
  const [open, setOpen] = useState(true);
  useEffect(() => { AsyncStorage.getItem(OPEN_KEY).then((v) => setOpen(parseListOpen(v))).catch(() => {}); }, []);
  const setOpenKept = (v: boolean) => { setOpen(v); AsyncStorage.setItem(OPEN_KEY, v ? '1' : '0').catch(() => {}); };

  const rows = useMemo(() => adsbListRows(p.aircraft), [p.aircraft]);
  const innerW = p.areaW - p.insets.left - p.insets.right;
  const innerH = p.areaH - p.insets.top - p.insets.bottom;
  const width = adsbListWidth(innerW);
  const fit = adsbListRowsFit(innerH);
  const shown = Math.max(1, Math.min(fit, rows.length || 1));
  const height = LIST_HEAD_H + shown * LIST_ROW_H;
  useEffect(() => { p.onFootprint?.(open ? width : 0, open ? height : 0); }, [open, width, height]);   // eslint-disable-line react-hooks/exhaustive-deps

  // ★ Flat: up/down (and left/right) walk pill/header → rows. Back is not taken here, so it reaches the map's overlay.
  const { navCtx, scrollProps } = usePanelNav(true, { flat: true });
  const s = useMemo(() => makeStyles(p.ink), [p.ink]);
  const placed = rows.filter((r) => r.placed).length;

  const pos = { left: EDGE + p.insets.left, bottom: EDGE + p.insets.bottom };
  return (
    <NavCtx.Provider value={navCtx}>
      <View style={[s.wrap, pos, open && { width }]} onTouchStart={noteTouchInteraction}>
        <NavRow>
          <HeadKey s={s} open={open} onPress={() => setOpenKept(!open)}
                   label={open ? 'AIRCRAFT' : '✈'} count={rows.length} placed={placed} />
          {open && p.onLegend && <LegendKey s={s} onPress={p.onLegend} />}
        </NavRow>
        {open && (
          <ScrollView {...scrollProps} style={{ maxHeight: shown * LIST_ROW_H }} contentContainerStyle={scrollLane} showsVerticalScrollIndicator
                      keyboardShouldPersistTaps="handled">
            {rows.length ? rows.map((r) => (
              <NavRow key={r.hex}><Row s={s} r={r} logoBase={p.logoBase ?? null} onPress={() => p.onFocus(r.hex)} /></NavRow>
            )) : (
              <Text style={s.note} numberOfLines={2}>
                {p.started ? 'Listening on 1090 MHz — no aircraft yet.' : 'Taking the receiver to 1090 MHz…'}
              </Text>
            )}
          </ScrollView>
        )}
      </View>
    </NavCtx.Provider>
  );
}

type S = ReturnType<typeof makeStyles>;

function HeadKey({ s, open, onPress, label, count, placed }: {
  s: S; open: boolean; onPress: () => void; label: string; count: number; placed: number;
}) {
  const { focused, viewRef } = useNavButton(onPress);
  const { ring } = useNavFocus();
  return (
    <TouchableOpacity ref={viewRef as never} style={[s.head, !open && s.pill]} onPress={onPress} activeOpacity={0.75}
                      accessibilityRole="button"
                      accessibilityLabel={open ? `Aircraft list, ${count} aircraft. Hide the list` : `Show the aircraft list, ${count} aircraft`}>
      <Text style={s.headTxt}>{label}</Text>
      <Text style={s.headCnt}>{count}{open && placed < count ? ` · ${placed} on map` : ''}</Text>
      {open && <View style={{ flex: 1 }} />}
      <Text style={s.chev}>{open ? '▾' : '▸'}</Text>
      <NavRing on={focused} color={ring} radius={open ? 8 : 15} />
    </TouchableOpacity>
  );
}

function LegendKey({ s, onPress }: { s: S; onPress: () => void }) {
  const { focused, viewRef } = useNavButton(onPress);
  const { ring } = useNavFocus();
  return (
    <TouchableOpacity ref={viewRef as never} style={s.legKey} onPress={onPress} activeOpacity={0.7}
                      accessibilityRole="button" accessibilityLabel="What the colours mean">
      <Text style={s.legTxt}>i</Text>
      <NavRing on={focused} color={ring} radius={12} />
    </TouchableOpacity>
  );
}

function Row({ s, r, onPress, logoBase }: { s: S; r: AdsbListRow; onPress: () => void; logoBase: string | null }) {
  const { focused, viewRef } = useNavButton(onPress);
  const { ring } = useNavFocus();
  return (
    <TouchableOpacity ref={viewRef as never} style={[s.row, !r.placed && s.rowUnplaced]} onPress={onPress}
                      disabled={!r.placed} activeOpacity={0.7} accessibilityRole="button"
                      accessibilityLabel={`${r.label}${r.info ? ', ' + r.info : ''}${r.alt ? ', ' + r.alt : ''}${r.kt ? ', ' + r.kt : ''}${r.range ? ', ' + r.range : ''}`}>
      {r.emergency && <EmBreath />}
      <View style={s.callCol}>
        <View style={s.callBox}>
          {!!r.flag && <Text style={s.flag}>{r.flag}</Text>}
          <Text style={[s.call, r.emergency && s.emerg, r.military && !r.emergency && s.mil]} numberOfLines={1}>{r.label}</Text>
          {r.bars > 0 && <SigBars n={r.bars} />}
          {r.badge ? <Text style={[s.tag, s.tagEm]} numberOfLines={1}>{r.badge}</Text>
            : r.military ? <Text style={[s.tag, s.tagMil]}>MIL</Text> : null}
        </View>
        {/* ★ V12 aircraft data: operator (the owner's logo, else its code as a badge) · type · registration · route */}
        {!!r.info && <InfoLine s={s} r={r} logoBase={logoBase} />}
      </View>
      <Text style={[s.cell, s.alt, !!r.altCol && { color: r.altCol }]} numberOfLines={1}>{r.alt}{r.trend ? ` ${r.trend}` : ''}</Text>
      <Text style={[s.cell, s.kt]} numberOfLines={1}>{r.kt}</Text>
      <Text style={[s.cell, s.rng]} numberOfLines={1}>{r.range}</Text>
      <NavRing on={focused} color={ring} radius={4} />
    </TouchableOpacity>
  );
}

function InfoLine({ s, r, logoBase }: { s: S; r: AdsbListRow; logoBase: string | null }) {
  const [logoOk, setLogoOk] = useState(true);
  const head = r.op && r.info.startsWith(r.op) ? r.op : '';
  const rest = head ? r.info.slice(head.length).replace(/^ \u00b7 /, '') : r.info;
  const uri = logoBase && r.logo && logoOk ? logoBase + 'logo/' + encodeURIComponent(r.logo) : '';
  return (
    <View style={s.infoBox}>
      {uri ? <Image source={{ uri }} style={s.logo} resizeMode="contain" onError={() => setLogoOk(false)} accessibilityLabel={r.op} />
        : head ? <Text style={s.opBadge}>{head}</Text> : null}
      <Text style={s.info} numberOfLines={1}>{rest}</Text>
    </View>
  );
}
/** ★★ An emergency row breathes red (the map's 1.2 s breath — never a hard blink); still when the system asks for less
 *  motion. Drawn behind the row's text. */
function EmBreath() {
  const v = useRef(new Animated.Value(0)).current;
  const [still, setStill] = useState(false);
  useEffect(() => { AccessibilityInfo.isReduceMotionEnabled().then(setStill).catch(() => {}); }, []);
  useEffect(() => {
    if (still) return;
    const loop = Animated.loop(Animated.sequence([
      Animated.timing(v, { toValue: 1, duration: 600, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
      Animated.timing(v, { toValue: 0, duration: 600, easing: Easing.inOut(Easing.sin), useNativeDriver: true }),
    ]));
    loop.start();
    return () => loop.stop();
  }, [still, v]);
  return <Animated.View pointerEvents="none" style={[StyleSheet.absoluteFill, { backgroundColor: 'rgb(224,64,64)',
    opacity: still ? 0.22 : v.interpolate({ inputRange: [0, 1], outputRange: [0.08, 0.30] }) }]} />;
}

/** ★ Three ascending bars, the lit ones in the link colours (LinkBars / DabMeter's) — adsbBars decides how many. */
function SigBars({ n }: { n: number }) {
  const lit = n >= 3 ? '#33cc44' : n === 2 ? '#e0b020' : '#e04040';
  return (
    <View style={sig.box} accessibilityLabel={`signal ${n} of 3`}>
      {[0.4, 0.7, 1].map((h, i) => (
        <View key={i} style={[sig.bar, { height: 9 * h, backgroundColor: i < n ? lit : 'rgba(255,184,51,0.22)' }]} />
      ))}
    </View>
  );
}
const sig = StyleSheet.create({
  box: { flexDirection: 'row', alignItems: 'flex-end', gap: 1.5, height: 9, marginLeft: 5 },
  bar: { width: 2.5, borderRadius: 1 },
});

const F = DECODER_FONT;
/* ★ The page's own chrome values (MapOverlay #topbar / #legbtn): the amber-black at 0.92, a controls-colour border at
 *  0.30, text in the text colour — so the list reads as one more piece of the map's furniture. */
const makeStyles = (ink: { x: string; k: string; kt: string }) => StyleSheet.create({
  wrap:   { position: 'absolute', backgroundColor: 'rgba(9,6,2,0.92)', borderWidth: 1, borderColor: `rgba(${ink.k},0.30)`,
            borderRadius: 10, overflow: 'hidden' },
  head:   { height: LIST_HEAD_H, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 10,
            borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: `rgba(${ink.k},0.25)` },
  pill:   { borderBottomWidth: 0, paddingHorizontal: 12 },
  legKey: { position: 'absolute', right: 30, top: 3, width: 24, height: 24, borderRadius: 12, alignItems: 'center',
            justifyContent: 'center', borderWidth: 1, borderColor: `rgba(${ink.k},0.35)` },
  legTxt: { fontFamily: 'Georgia', fontStyle: 'italic', fontWeight: '600', fontSize: 12, color: `rgba(${ink.x},0.8)` },
  headTxt:{ fontFamily: F, fontSize: 11, letterSpacing: 1.5, color: `rgba(${ink.x},0.75)` },
  headCnt:{ fontFamily: F, fontSize: 12, color: `rgba(${ink.x},0.95)` },
  chev:   { fontFamily: F, fontSize: 12, color: `rgba(${ink.kt},0.80)` },
  // ★ Sized for 240 pt (an SE in Display Zoom): 8 + 4·3 + 50 + 40 + 60 + 8 leaves the callsign ~62 pt, 8–9 characters.
  row:    { height: LIST_ROW_H, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 8, gap: 4 },
  rowUnplaced: { opacity: 0.5 },
  callCol:{ flex: 1, minWidth: 0, justifyContent: 'center' },
  callBox:{ minWidth: 0, flexDirection: 'row', alignItems: 'center' },
  flag:   { fontSize: 11, marginRight: 4 },
  infoBox:{ minWidth: 0, flexDirection: 'row', alignItems: 'center', marginTop: 1 },
  info:   { fontFamily: F, fontSize: 9.5, color: `rgba(${ink.x},0.62)`, flexShrink: 1, minWidth: 0, letterSpacing: 0.3 },
  opBadge:{ fontFamily: F, fontSize: 8, fontWeight: '700', letterSpacing: 0.6, color: `rgba(${ink.x},0.80)`, borderWidth: StyleSheet.hairlineWidth,
            borderColor: `rgba(${ink.x},0.55)`, borderRadius: 2, paddingHorizontal: 3, marginRight: 4, overflow: 'hidden' },
  logo:   { width: 40, height: 10, marginRight: 4 },
  call:   { fontFamily: F, fontSize: 12, color: `rgba(${ink.x},0.95)`, flexShrink: 1, minWidth: 0 },
  emerg:  { color: '#ff6a50' },
  mil:    { color: 'rgb(140,195,255)' },
  tag:    { marginLeft: 5, paddingHorizontal: 4, borderRadius: 3, fontFamily: F, fontSize: 8.5, fontWeight: '700', letterSpacing: 0.8,
            overflow: 'hidden' },
  tagEm:  { backgroundColor: '#e04040', color: '#fff' },
  tagMil: { backgroundColor: 'rgba(90,170,255,0.85)', color: '#06121f' },
  cell:   { fontFamily: F, fontSize: 11, color: `rgba(${ink.x},0.70)`, textAlign: 'right' },
  alt:    { width: 50 },
  kt:     { width: 40 },
  rng:    { width: 60 },
  note:   { fontFamily: F, fontSize: 12, color: `rgba(${ink.x},0.60)`, padding: 10, lineHeight: 17 },
});
