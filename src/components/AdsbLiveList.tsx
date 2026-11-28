/**
 * AdsbLiveList — the ADS-B live list (V12; Stuart, 2026-10-24: "the full-screen map is on … a live list in the bottom
 * corner"). What it says and how big it is: src/services/adsbList.ts. ★★★ beta96: no longer a corner of its own — its
 * rows (useAdsbBody) head the AIR & SEA map's AIRCRAFT tab while ADS-B is live. ★★★ beta97 THE MAP CARDS (MapChrome):
 * the LIST drawer owns the corner, the chip and the pad's walk; NEWEST · FURTHEST · LONGEST TRACKED went to STATS and the
 * GAIN key to TUNE's one-row gain — those chips and the GAIN card were DELETED here. What is left is the rows.
 *
 * ★★★ DRAWN BY REACT NATIVE OVER THE WEBVIEW, NOT INSIDE THE MAP PAGE — because of the controller. A game pad and the
 *     keyboard reach the app through hardwareKeys → PanelNav, never through the page: on iOS and the Mac a pad is a
 *     GCController the WebView never sees. Rows registered here walk with the D-pad (up/down), Enter finds the
 *     aircraft, Back passes on to the map (gamepad_mapping: D-pad highlights, back leaves).
 * ★ The map's colours, not the app theme's: it sits on the map's own dark chrome (MapOverlay's `ink`), so it looks
 *   the same on every faceplate, with transparency on or off — it is part of the map, not a panel of the receiver.
 * ★ The decoder font — the display font is for the main controls only.
 */
import React, { useEffect, useMemo, useRef, useState } from 'react';
import { AccessibilityInfo, Animated, Easing, Image, StyleSheet, Text, TouchableOpacity, View, useWindowDimensions } from 'react-native';
import { NavRow, NavRing, useNavButton, useNavFocus } from './PanelNav';
import { DECODER_FONT } from './DecoderShell';
import { SnrChip } from './SnrChip';
import { chromeScale } from '../utils/mapChrome';   // ★ beta96 the reply SNR, colour-coded (replaced the 3-bar meter)
import type { Aircraft } from '../services/SDRBackend';
import { adsbListRows, adsbRowLayout, adsbRowSizes, COL_ALT, COL_KT, COL_RNG, ROW_GAP, type AdsbRowSizes,
         type AdsbListRow, type AdsbRowLayout } from '../services/adsbList';

/** ★★★ beta96 (stage 6, docs/v12/ONE-MAP.md): ADS-B mode's map IS the AIR & SEA map — this list no longer has a corner
 *  of its own. Its body (the chips, the GAIN key and card, the rows, the data credit) heads the AIR & SEA map's AIRCRAFT
 *  tab while ADS-B is live (TrackLiveList `adsb`), exactly as the web's adsbListHtml heads asRender's AIRCRAFT rows. */
export interface AdsbBodyInput {
  /** The report in the app's shape, ranged from the receiver (SDRScreen's `aircraft`). */
  aircraft: Aircraft[];
  /** False until the first report: "taking the receiver to 1090 MHz" rather than "no aircraft". */
  started: boolean;
  /** A row (or a chip) was chosen — find it on the map. */
  onFocus: (hex: string) => void;
  /** ★ The map's selected aircraft (an icon tapped, or a row) — its row is highlighted, as on the web list. */
  selected?: string | null;
  /** ★ V12: where the server serves the owner's operator logos ("http(s)://host/adsb/"), or null — the row then shows
   *  the operator's ICAO code as a badge. The server names a logo only when the file exists. */
  logoBase?: string | null;
  /* ★★★ beta97 THE MAP CARDS: NEWEST · FURTHEST · LONGEST TRACKED are the STATS card's now (every source merged) and the
   *  GAIN key is the TUNE card's one-row gain (MapChrome) — this body is the rows alone, like every mode's list. */
}
export interface AdsbBody {
  count: number;
  placed: number;
  credit: boolean;
  rows: React.ReactNode[];
  empty: string;
}

/** ★★ The ADS-B list's body as a hook, so the host (TrackLiveList) can size itself around it. Always called (hooks);
 *  `p` null = nothing (ADS-B not live, or another tab shown). ★★★ beta96 `listW`: the list's width (pt) — the rows are laid
 *  out from it (adsbRowLayout: speed goes, then the chip moves under the callsign, then altitude over range), so the
 *  callsign is never what gives (Stuart 11-26: "the signal hides the aircraft details"). */
export function useAdsbBody(p: AdsbBodyInput | null, ink: { x: string; k: string; kt: string }, listW = 300): AdsbBody {
  const rows = useMemo(() => (p ? adsbListRows(p.aircraft) : []), [p?.aircraft]);   // eslint-disable-line react-hooks/exhaustive-deps
  // ★★★ beta97 hotfix the whole row at the chrome's scale (MapChrome's k): the layout judged in base points, drawn × k
  const win = useWindowDimensions();
  const z = useMemo(() => adsbRowSizes(chromeScale(win.width, win.height)), [win.width, win.height]);
  const s = useMemo(() => makeStyles(ink, z), [ink, z]);
  const lay = useMemo(() => adsbRowLayout(listW / z.k), [listW, z.k]);
  if (!p) return { count: 0, placed: 0, credit: false, rows: [], empty: '' };
  /* ★★ V12 beta91 ODC-By 1.0 §4.3: while a row shows the server's aircraft data (its second line, the operator badge) the
   *  Mictronics notice sits at the list's foot (AircraftDataCredit — the card's words) */
  const credit = p.aircraft.some((a) => !!(a.info || a.op));
  return {
    count: rows.length, placed: rows.filter((r) => r.placed).length, credit,
    rows: rows.map((r) => (
      <NavRow key={`adsb:${r.hex}`}><Row s={s} z={z} r={r} lay={lay} sel={!!p.selected && p.selected === r.hex} logoBase={p.logoBase ?? null} onPress={() => p.onFocus(r.hex)} /></NavRow>
    )),
    empty: p.started ? 'Listening on 1090 MHz — no aircraft yet.' : 'Taking the receiver to 1090 MHz…',
  };
}

type S = ReturnType<typeof makeStyles>;

function Row({ s, z, r, lay, sel, onPress, logoBase }: { s: S; z: AdsbRowSizes; r: AdsbListRow; lay: AdsbRowLayout; sel: boolean; onPress: () => void; logoBase: string | null }) {
  const { focused, viewRef } = useNavButton(onPress);
  const { ring } = useNavFocus();
  // ★ the SNR chip and a MIL / emergency tag: beside the callsign, or (a narrow list) at the head of the second line
  const ex = (
    <>
      <SnrChip mode="adsb" db={r.snr} size={z.snr} />   {/* ★★★ the subtitle's size: never taller than the callsign's line */}
      {r.badge ? <Text style={[s.tag, s.tagEm]} numberOfLines={1}>{r.badge}</Text>
        : r.military ? <Text style={[s.tag, s.tagMil]}>MIL</Text> : null}
    </>
  );
  const alt = <Text style={[s.cell, lay.stacked ? s.stackTxt : s.alt, !!r.altCol && { color: r.altCol }]} numberOfLines={1}>{r.alt}{r.trend ? ` ${r.trend}` : ''}</Text>;
  const rng = <Text style={[s.cell, lay.stacked ? s.stackTxt : s.rng]} numberOfLines={1}>{r.range}</Text>;
  return (
    <TouchableOpacity ref={viewRef as never} style={[s.row, !r.placed && s.rowUnplaced, sel && s.rowSel]} onPress={onPress}
                      disabled={!r.placed} activeOpacity={0.7} accessibilityRole="button"
                      accessibilityLabel={`${r.label}${r.info ? ', ' + r.info : ''}${r.alt ? ', ' + r.alt : ''}${r.kt ? ', ' + r.kt : ''}${r.range ? ', ' + r.range : ''}`}>
      {r.emergency && <EmBreath />}
      <View style={s.callCol}>
        <View style={s.callBox}>
          {!!r.flag && <Text style={s.flag}>{r.flag}</Text>}
          <Text style={[s.call, r.emergency && s.emerg, r.military && !r.emergency && s.mil]} numberOfLines={1}>{r.label}</Text>
          {!lay.chipUnder && ex}
        </View>
        {/* ★ V12 aircraft data: operator (the owner's logo, else its code as a badge) · type · registration · route */}
        {(!!r.info || lay.chipUnder) && <InfoLine s={s} r={r} logoBase={logoBase} lead={lay.chipUnder ? ex : null} />}
      </View>
      {lay.stacked ? <View style={s.stack}>{alt}{rng}</View> : alt}
      {lay.kt && <Text style={[s.cell, s.kt]} numberOfLines={1}>{r.kt}</Text>}
      {!lay.stacked && rng}
      <NavRing on={focused} color={ring} radius={4} />
    </TouchableOpacity>
  );
}

function InfoLine({ s, r, logoBase, lead }: { s: S; r: AdsbListRow; logoBase: string | null; lead?: React.ReactNode }) {
  const [logoOk, setLogoOk] = useState(true);
  const head = r.op && r.info.startsWith(r.op) ? r.op : '';
  const rest = head ? r.info.slice(head.length).replace(/^ \u00b7 /, '') : r.info;
  const uri = logoBase && r.logo && logoOk ? logoBase + 'logo/' + encodeURIComponent(r.logo) : '';
  return (
    <View style={s.infoBox}>
      {lead ? <View style={s.lead}>{lead}</View> : null}
      {uri ? <Image source={{ uri }} style={s.logo} resizeMode="contain" onError={() => setLogoOk(false)} accessibilityLabel={r.op} />
        : head ? <Text style={s.opBadge}>{head}</Text> : null}
      {!!rest && <Text style={s.info} numberOfLines={1}>{rest}</Text>}
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


const F = DECODER_FONT;
/* ★ The page's own chrome values (MapOverlay #topbar / #legbtn): the amber-black at 0.92, a controls-colour border at
 *  0.30, text in the text colour — so the list reads as one more piece of the map's furniture. */
const makeStyles = (ink: { x: string; k: string; kt: string }, z: AdsbRowSizes) => StyleSheet.create({
  // ★★★ beta96 the columns come from adsbRowLayout (the list's width): at 240 pt the old fixed set left the callsign 62 pt,
  //     and the SNR chip beside it took two thirds of that.
  row:    { height: z.rowH, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 8 * z.k, gap: ROW_GAP * z.k },
  // ★ the web list's .adsbRow.sel: a faint amber wash and a bar on the left
  rowSel: { backgroundColor: 'rgba(255,229,102,0.12)', borderLeftWidth: 2, borderLeftColor: 'rgba(255,229,102,0.7)', paddingLeft: 6 * z.k },
  rowUnplaced: { opacity: 0.5 },
  // ★★★ the callsign's line at the same height on every row (with or without a second line), the cells level with it
  callCol:{ flex: 1, minWidth: 0, alignSelf: 'stretch', justifyContent: 'flex-start', paddingTop: (z.rowH - z.callLine - z.subLine - 1 * z.k) / 2 },
  callBox:{ minWidth: 0, flexDirection: 'row', alignItems: 'center', height: z.callLine, gap: 5 * z.k },
  flag:   { fontSize: z.flag, lineHeight: z.callLine },
  infoBox:{ minWidth: 0, flexDirection: 'row', alignItems: 'center', marginTop: 1 * z.k, height: z.subLine },
  info:   { fontFamily: F, fontSize: z.sub, lineHeight: z.subLine, color: `rgba(${ink.x},0.62)`, flexShrink: 1, minWidth: 0, letterSpacing: 0.3 },
  opBadge:{ fontFamily: F, fontSize: z.op, fontWeight: '700', letterSpacing: 0.6, color: `rgba(${ink.x},0.80)`, borderWidth: StyleSheet.hairlineWidth,
            borderColor: `rgba(${ink.x},0.55)`, borderRadius: 2, paddingHorizontal: 3 * z.k, marginRight: 4 * z.k, overflow: 'hidden' },
  logo:   { width: 40 * z.k, height: 10 * z.k, marginRight: 4 * z.k },
  call:   { fontFamily: F, fontSize: z.call, lineHeight: z.callLine, color: `rgba(${ink.x},0.95)`, flexShrink: 1, minWidth: 0 },
  emerg:  { color: '#ff6a50' },
  mil:    { color: 'rgb(140,195,255)' },
  tag:    { paddingHorizontal: 4 * z.k, borderRadius: 3, fontFamily: F, fontSize: z.tag, fontWeight: '700', letterSpacing: 0.8,
            overflow: 'hidden', flexShrink: 0 },
  tagEm:  { backgroundColor: '#e04040', color: '#fff' },
  tagMil: { backgroundColor: 'rgba(90,170,255,0.85)', color: '#06121f' },
  // ★★★ the cells on the CALLSIGN's line (the row's top line), not floated to its middle
  cell:   { fontFamily: F, fontSize: z.cell, lineHeight: z.callLine, color: `rgba(${ink.x},0.70)`, textAlign: 'right', alignSelf: 'flex-start', marginTop: (z.rowH - z.callLine - z.subLine - 1 * z.k) / 2 },
  alt:    { width: COL_ALT * z.k },
  kt:     { width: COL_KT * z.k },
  rng:    { width: COL_RNG * z.k },
  // ★ a narrow list: altitude over range in one column at the end (each on its own line, the two lines' heights)
  stack:  { width: Math.max(COL_ALT, COL_RNG) * z.k, alignSelf: 'stretch', justifyContent: 'flex-start', paddingTop: (z.rowH - 2 * z.subLine) / 2 },
  stackTxt: { lineHeight: z.subLine, marginTop: 0, alignSelf: 'auto' },
  // ★ the chip (and a tag) at the head of the second line when the list is too narrow for it beside the callsign
  lead:   { flexDirection: 'row', alignItems: 'center', gap: 5 * z.k, marginRight: 5 * z.k, flexShrink: 0 },
});
