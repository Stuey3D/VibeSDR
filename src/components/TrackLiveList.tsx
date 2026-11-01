/**
 * TrackLiveList — the AIS / APRS map's live list, in the bottom-left corner (V12, docs/v12/AIS-APRS.md): the web's
 * TrackView list (web/client/src/tracks.ts) in React Native, beside AdsbLiveList and built the same way.
 *
 * ★★★ DRAWN BY REACT NATIVE OVER THE WEBVIEW (AdsbLiveList's note): a pad and the keyboard reach the app through
 *     hardwareKeys → PanelNav, never the page — rows registered here walk with the D-pad, Enter finds the ship / station
 *     on the map, Back passes on to the map's overlay (EXIT AIS / CLOSE MAP).
 * ★★ What it says and in what order is src/services/tracks.ts (aisListRows / aprsListRows — the web's aisRows / aprsRows):
 *    ships nearest first, stations newest first; a row with no position is dimmed and says so instead of finding nothing.
 * ★ The status line (heard · on the map · msg/min, the capture when short) sits under the header — the web's top-bar line,
 *   moved here because the map page's top bar has no room for it on a phone. The safety strip (AIS types 12/14) above the
 *   rows, amber. Collapses to a pill; remembered per device. The map's colours, never the faceplate's (AdsbLiveList's
 *   rule: it is part of the map). The decoder font.
 */
import React, { useEffect, useMemo, useRef, useState } from 'react';
import { ScrollView, StyleSheet, Text, TouchableOpacity, View } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { NavCtx, NavRow, NavRing, useNavButton, useNavFocus, usePanelNav, noteTouchInteraction } from './PanelNav';
import { DECODER_FONT } from './DecoderShell';
import { scrollLane } from '../constants/popupTokens';
import { adsbListRowsFit, adsbListWidth, parseListOpen, LIST_HEAD_H, LIST_ROW_H, EDGE } from '../services/adsbList';
import type { TrackRow } from '../services/tracks';

/** ★ The status line and each safety line under the header. */
const STATUS_H = 20;
const SAFETY_LINE_H = 18;
const OPEN_KEY = 'lsv_trk_list_open';   // ★ the web's vibesdr.trkListMin — one choice for AIS and APRS

export interface TrackLiveListProps {
  /** "SHIPS" / "STATIONS". */
  title: string;
  rows: TrackRow[];
  status: string;
  /** Shown while there are no rows (aisEmpty / aprsEmpty, or "taking the receiver to 162 MHz…" before the first report). */
  empty: string;
  safety?: string[];
  /** A row was chosen — find it on the map (the layer's focus). Only rows with a position are sent. */
  onFocus: (key: string) => void;
  /** The map's selection (an icon tapped, or a row) — its row is highlighted. */
  selected?: string | null;
  onFootprint?: (w: number, h: number) => void;
  ink: { x: string; k: string; kt: string };
  areaW: number;
  areaH: number;
  insets: { top: number; bottom: number; left: number; right: number };
  /** ★★ V12 2026-11-01 THE TIME FILTER (Stuart: "filter the map based on last hour last day last week last month all
   *  time"): a row of keys under the status line, walked by the pad like any row. The screen filters. */
  ranges?: Array<{ id: string; label: string; words: string }>;
  range?: string;
  onRange?: (id: string) => void;
  /** What ALL (or the ranges offered) mean here — said to a screen reader on each key. */
  rangeHint?: string;
  /** ★★ V12 2026-11-01 THE NAVTEX MAP's layers (STATIONS · WARNINGS · WEATHER): on/off keys under the ranges. */
  toggles?: Array<{ id: string; label: string; words: string }>;
  toggled?: Record<string, boolean>;
  onToggle?: (id: string, on: boolean) => void;
}
const RANGE_H = 30;

export default function TrackLiveList(p: TrackLiveListProps) {
  const [open, setOpen] = useState(true);
  useEffect(() => { AsyncStorage.getItem(OPEN_KEY).then((v) => setOpen(parseListOpen(v))).catch(() => {}); }, []);
  const setOpenKept = (v: boolean) => { setOpen(v); AsyncStorage.setItem(OPEN_KEY, v ? '1' : '0').catch(() => {}); };
  const [note, setNote] = useState('');
  const noteT = useRef<ReturnType<typeof setTimeout> | null>(null);
  useEffect(() => () => { if (noteT.current) clearTimeout(noteT.current); }, []);
  const pick = (r: TrackRow) => {
    if (!r.noPos) { p.onFocus(r.key); return; }
    // ★ the web's "Not on the map — no position heard yet." (a row press that found nothing must say why)
    setNote('Not on the map — no position heard yet.');
    if (noteT.current) clearTimeout(noteT.current);
    noteT.current = setTimeout(() => setNote(''), 2500);
  };

  const innerW = p.areaW - p.insets.left - p.insets.right;
  const innerH = p.areaH - p.insets.top - p.insets.bottom;
  const width = adsbListWidth(innerW);
  const safety = p.safety ?? [];
  const extra = STATUS_H + safety.length * SAFETY_LINE_H + (p.ranges?.length && open ? RANGE_H : 0) + (p.toggles?.length && open ? RANGE_H : 0);
  const fit = adsbListRowsFit(innerH - extra);
  const shown = Math.max(1, Math.min(fit, p.rows.length || 1));
  const height = LIST_HEAD_H + extra + shown * LIST_ROW_H;
  useEffect(() => { p.onFootprint?.(open ? width : 0, open ? height : 0); }, [open, width, height]);   // eslint-disable-line react-hooks/exhaustive-deps

  const { navCtx, scrollProps } = usePanelNav(true, { flat: true });
  const s = useMemo(() => makeStyles(p.ink), [p.ink]);
  const placed = p.rows.filter((r) => !r.noPos).length;
  const pos = { left: EDGE + p.insets.left, bottom: EDGE + p.insets.bottom };
  return (
    <NavCtx.Provider value={navCtx}>
      <View style={[s.wrap, pos, open && { width }]} onTouchStart={noteTouchInteraction}>
        <NavRow>
          <HeadKey s={s} open={open} onPress={() => setOpenKept(!open)} title={p.title} count={p.rows.length} placed={placed} />
        </NavRow>
        {open && (
          <Text style={[s.status, !!note && s.noteTxt]} numberOfLines={1}>{note || p.status}</Text>
        )}
        {open && !!p.ranges?.length && (
          <NavRow>
            <View style={s.ranges} accessibilityRole="radiogroup">
              {p.ranges.map((x) => <RangeKey key={x.id} s={s} label={x.label} words={`${x.words}${p.rangeHint ? '. ' + p.rangeHint : ''}`}
                                             on={p.range === x.id} onPress={() => p.onRange?.(x.id)} />)}
            </View>
          </NavRow>
        )}
        {open && !!p.toggles?.length && (
          <NavRow>
            <View style={s.ranges}>
              {p.toggles.map((x) => <ToggleKey key={x.id} s={s} label={x.label} words={x.words}
                                               on={p.toggled?.[x.id] !== false} onPress={() => p.onToggle?.(x.id, p.toggled?.[x.id] === false)} />)}
            </View>
          </NavRow>
        )}
        {open && safety.map((l, i) => (
          <Text key={i} style={s.safety} numberOfLines={1}>{l}</Text>
        ))}
        {open && (
          <ScrollView {...scrollProps} style={{ maxHeight: shown * LIST_ROW_H }} contentContainerStyle={scrollLane}
                      showsVerticalScrollIndicator keyboardShouldPersistTaps="handled">
            {p.rows.length ? p.rows.map((r) => (
              <NavRow key={r.key}><Row s={s} r={r} sel={!!p.selected && p.selected === r.key} onPress={() => pick(r)} /></NavRow>
            )) : (
              <Text style={s.empty} numberOfLines={3}>{p.empty}</Text>
            )}
          </ScrollView>
        )}
      </View>
    </NavCtx.Provider>
  );
}

type S = ReturnType<typeof makeStyles>;

function HeadKey({ s, open, onPress, title, count, placed }: {
  s: S; open: boolean; onPress: () => void; title: string; count: number; placed: number;
}) {
  const { focused, viewRef } = useNavButton(onPress);
  const { ring } = useNavFocus();
  const word = title.toLowerCase();
  return (
    <TouchableOpacity ref={viewRef as never} style={[s.head, !open && s.pill]} onPress={onPress} activeOpacity={0.75}
                      accessibilityRole="button"
                      accessibilityLabel={open ? `${title} list, ${count} heard. Hide the list` : `Show the ${word} list, ${count} heard`}>
      <Text style={s.headTxt}>{title}</Text>
      <Text style={s.headCnt}>{count}{open && placed < count ? ` · ${placed} on map` : ''}</Text>
      {open && <View style={{ flex: 1 }} />}
      <Text style={s.chev}>{open ? '▾' : '▸'}</Text>
      <NavRing on={focused} color={ring} radius={open ? 8 : 15} />
    </TouchableOpacity>
  );
}

function RangeKey({ s, label, words, on, onPress }: { s: S; label: string; words: string; on: boolean; onPress: () => void }) {
  const { focused, viewRef } = useNavButton(onPress);
  const { ring } = useNavFocus();
  return (
    <TouchableOpacity ref={viewRef as never} style={[s.rKey, on && s.rKeyOn]} onPress={onPress} activeOpacity={0.7}
                      accessibilityRole="radio" accessibilityState={{ checked: on }} accessibilityLabel={words}>
      <Text style={[s.rTxt, on && s.rTxtOn]}>{label}</Text>
      <NavRing on={focused} color={ring} radius={5} />
    </TouchableOpacity>
  );
}

function ToggleKey({ s, label, words, on, onPress }: { s: S; label: string; words: string; on: boolean; onPress: () => void }) {
  const { focused, viewRef } = useNavButton(onPress);
  const { ring } = useNavFocus();
  return (
    <TouchableOpacity ref={viewRef as never} style={[s.rKey, on && s.tKeyOn]} onPress={onPress} activeOpacity={0.7}
                      accessibilityRole="switch" accessibilityState={{ checked: on }} accessibilityLabel={words}>
      <Text style={[s.rTxt, on && s.rTxtOn]}>{label}</Text>
      <NavRing on={focused} color={ring} radius={5} />
    </TouchableOpacity>
  );
}

function Row({ s, r, sel, onPress }: { s: S; r: TrackRow; sel: boolean; onPress: () => void }) {
  const { focused, viewRef } = useNavButton(onPress);
  const { ring } = useNavFocus();
  const where = r.range && r.range !== 'no position' ? `${r.range}${r.brg ? ' ' + r.brg : ''}` : r.range;
  return (
    <TouchableOpacity ref={viewRef as never} style={[s.row, r.noPos && s.rowNoPos, sel && s.rowSel, r.em && s.rowEm]}
                      onPress={onPress} activeOpacity={0.7} accessibilityRole="button"
                      accessibilityLabel={`${r.name}, ${r.second}${r.sp ? ', ' + r.sp : ''}${where ? ', ' + where : ''}${r.age ? ', heard ' + r.age + ' ago' : ''}`}>
      <View style={s.idCol}>
        <View style={s.nameBox}>
          <View style={[s.sw, { backgroundColor: r.colour }]} />
          {!!r.flag && <Text style={s.flag}>{r.flag}</Text>}
          <Text style={[s.name, r.em && s.emName]} numberOfLines={1}>{r.name}</Text>
        </View>
        <Text style={s.second} numberOfLines={1}>{r.second}</Text>
      </View>
      <Text style={[s.cell, s.sp]} numberOfLines={1}>
        {r.sp}{r.trend === 1 ? <Text style={s.up}> ▲</Text> : r.trend === -1 ? <Text style={s.dn}> ▼</Text> : null}
      </Text>
      <Text style={[s.cell, s.rng, r.noPos && s.dim]} numberOfLines={1}>
        {where}{r.age ? <Text style={s.dim}>{`\n${r.age}`}</Text> : null}
      </Text>
      <NavRing on={focused} color={ring} radius={4} />
    </TouchableOpacity>
  );
}

const F = DECODER_FONT;
/* ★ AdsbLiveList's furniture values: the map's dark chrome at 0.92, the controls colour for the border, text in the text
 *  colour — so the ship list reads as part of the map on every faceplate, transparency on or off. The type colours
 *  (the swatch) and the ▲/▼ greens and reds are the web's, kept: they MEAN something. */
const makeStyles = (ink: { x: string; k: string; kt: string }) => StyleSheet.create({
  wrap:   { position: 'absolute', backgroundColor: 'rgba(9,6,2,0.92)', borderWidth: 1, borderColor: `rgba(${ink.k},0.30)`,
            borderRadius: 10, overflow: 'hidden' },
  head:   { height: LIST_HEAD_H, flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 10,
            borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: `rgba(${ink.k},0.25)` },
  pill:   { borderBottomWidth: 0, paddingHorizontal: 12 },
  headTxt:{ fontFamily: F, fontSize: 11, letterSpacing: 1.5, color: `rgba(${ink.x},0.75)` },
  headCnt:{ fontFamily: F, fontSize: 12, color: `rgba(${ink.x},0.95)` },
  chev:   { fontFamily: F, fontSize: 12, color: `rgba(${ink.kt},0.80)` },
  status: { height: STATUS_H, lineHeight: STATUS_H, paddingHorizontal: 10, fontFamily: F, fontSize: 10.5,
            color: `rgba(${ink.x},0.62)`, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: `rgba(${ink.k},0.18)` },
  noteTxt:{ color: `rgba(${ink.x},0.95)` },
  // ★ the web's .safety: amber on brown
  safety: { height: SAFETY_LINE_H, lineHeight: SAFETY_LINE_H, paddingHorizontal: 10, fontFamily: F, fontSize: 10.5,
            backgroundColor: '#3a2a10', color: '#ffe2a8' },
  row:    { height: LIST_ROW_H, flexDirection: 'row', alignItems: 'center', paddingHorizontal: 8, gap: 6 },
  rowSel: { backgroundColor: 'rgba(255,229,102,0.12)', borderLeftWidth: 2, borderLeftColor: 'rgba(255,229,102,0.7)', paddingLeft: 6 },
  rowNoPos: { opacity: 0.65 },
  rowEm:  { backgroundColor: 'rgba(120,20,20,0.75)' },
  idCol:  { flex: 1, minWidth: 0, justifyContent: 'center' },
  nameBox:{ minWidth: 0, flexDirection: 'row', alignItems: 'center' },
  sw:     { width: 8, height: 8, borderRadius: 4, marginRight: 5 },
  flag:   { fontSize: 11, marginRight: 4 },
  name:   { fontFamily: F, fontSize: 12, color: `rgba(${ink.x},0.95)`, flexShrink: 1, minWidth: 0 },
  emName: { color: '#ffb0a0' },
  second: { fontFamily: F, fontSize: 9.5, color: `rgba(${ink.x},0.62)`, letterSpacing: 0.3, marginTop: 1 },
  cell:   { fontFamily: F, fontSize: 10.5, color: `rgba(${ink.x},0.78)`, textAlign: 'right', fontVariant: ['tabular-nums'] },
  sp:     { width: 62 },
  rng:    { width: 62, lineHeight: 13 },
  dim:    { color: `rgba(${ink.x},0.50)` },
  up:     { color: '#6dd36d' },
  dn:     { color: '#ff8a6b' },
  ranges: { height: RANGE_H, flexDirection: 'row', alignItems: 'center', gap: 4, paddingHorizontal: 8,
            borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: `rgba(${ink.k},0.18)` },
  rKey:   { height: 22, justifyContent: 'center', paddingHorizontal: 7, borderRadius: 5, borderWidth: 1, borderColor: `rgba(${ink.k},0.35)` },
  rKeyOn: { backgroundColor: '#ffd34d', borderColor: '#ffd34d' },
  tKeyOn: { backgroundColor: '#7fc8ff', borderColor: '#7fc8ff' },
  rTxt:   { fontFamily: F, fontSize: 10, letterSpacing: 0.8, color: `rgba(${ink.x},0.85)` },
  rTxtOn: { color: '#111' },
  empty:  { fontFamily: F, fontSize: 12, color: `rgba(${ink.x},0.60)`, padding: 10, lineHeight: 17 },
});
