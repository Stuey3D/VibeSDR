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
import React, { useEffect, useMemo, useState } from 'react';
import { ScrollView, StyleSheet, Text, TouchableOpacity, View } from 'react-native';
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
  /** The list's footprint changed (its width, 0 as a pill) — the page keeps the attribution clear of it. */
  onFootprint?: (w: number) => void;
  /** The map's colours as rgb triplets (MapOverlay `ink`): text, controls, controls-as-legend. */
  ink: { x: string; k: string; kt: string };
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
  useEffect(() => { p.onFootprint?.(open ? width : 0); }, [open, width]);   // eslint-disable-line react-hooks/exhaustive-deps

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
        </NavRow>
        {open && (
          <ScrollView {...scrollProps} style={{ maxHeight: shown * LIST_ROW_H }} showsVerticalScrollIndicator
                      keyboardShouldPersistTaps="handled">
            {rows.length ? rows.map((r) => (
              <NavRow key={r.hex}><Row s={s} r={r} onPress={() => p.onFocus(r.hex)} /></NavRow>
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

function Row({ s, r, onPress }: { s: S; r: AdsbListRow; onPress: () => void }) {
  const { focused, viewRef } = useNavButton(onPress);
  const { ring } = useNavFocus();
  return (
    <TouchableOpacity ref={viewRef as never} style={[s.row, !r.placed && s.rowUnplaced]} onPress={onPress}
                      disabled={!r.placed} activeOpacity={0.7} accessibilityRole="button"
                      accessibilityLabel={`${r.label}${r.alt ? ', ' + r.alt : ''}${r.kt ? ', ' + r.kt : ''}${r.range ? ', ' + r.range : ''}`}>
      <Text style={[s.call, r.emergency && s.emerg]} numberOfLines={1}>{r.label}</Text>
      <Text style={[s.cell, s.alt]} numberOfLines={1}>{r.alt}</Text>
      <Text style={[s.cell, s.kt]} numberOfLines={1}>{r.kt}</Text>
      <Text style={[s.cell, s.rng]} numberOfLines={1}>{r.range}</Text>
      <NavRing on={focused} color={ring} radius={4} />
    </TouchableOpacity>
  );
}

const F = DECODER_FONT;
/* ★ The page's own chrome values (MapOverlay #topbar / #legbtn): the amber-black at 0.92, a controls-colour border at
 *  0.30, text in the text colour — so the list reads as one more piece of the map's furniture. */
const makeStyles = (ink: { x: string; k: string; kt: string }) => StyleSheet.create({
  wrap:   { position: 'absolute', backgroundColor: 'rgba(9,6,2,0.92)', borderWidth: 1, borderColor: `rgba(${ink.k},0.30)`,
            borderRadius: 10, overflow: 'hidden' },
  head:   { height: LIST_HEAD_H, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 10,
            borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: `rgba(${ink.k},0.25)` },
  pill:   { borderBottomWidth: 0, paddingHorizontal: 12 },
  headTxt:{ fontFamily: F, fontSize: 11, letterSpacing: 1.5, color: `rgba(${ink.x},0.75)` },
  headCnt:{ fontFamily: F, fontSize: 12, color: `rgba(${ink.x},0.95)` },
  chev:   { fontFamily: F, fontSize: 12, color: `rgba(${ink.kt},0.80)` },
  // ★ Sized for 240 pt (an SE in Display Zoom): 8 + 4·3 + 50 + 40 + 60 + 8 leaves the callsign ~62 pt, 8–9 characters.
  row:    { height: LIST_ROW_H, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 8, gap: 4 },
  rowUnplaced: { opacity: 0.5 },
  call:   { fontFamily: F, fontSize: 12, color: `rgba(${ink.x},0.95)`, flex: 1, minWidth: 0 },
  emerg:  { color: '#ff6a50' },
  cell:   { fontFamily: F, fontSize: 11, color: `rgba(${ink.x},0.70)`, textAlign: 'right' },
  alt:    { width: 50 },
  kt:     { width: 40 },
  rng:    { width: 60 },
  note:   { fontFamily: F, fontSize: 12, color: `rgba(${ink.x},0.60)`, padding: 10, lineHeight: 17 },
});
