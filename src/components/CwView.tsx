/**
 * CwView — the V12 CW decoder's body inside the decoder box (DecoderPanel): the web client's CW panel (main.ts renderCw).
 *
 *   ONE SIGNAL: the primary signal's text (as RTTY's) · the signals list · SPOTS
 *   SKIMMER:    the signals list across the segment · SPOTS
 *
 * Each row: frequency · call · wpm · SNR · confidence · age, and its recent text under it in its confidence's colour (the
 * health pill's: red poor, orange low, amber medium, green high — cwList CW_Q_COLOURS). Lowest frequency first, always.
 * Tap a row or a spot = tune to it in CW. A keyboard / pad reaches every row (DecoderPanel's list zone; `focusIdx`).
 *
 * ★★ THE VIEW STAYS WHERE THE LISTENER PUT IT (cwList cwPinFromScroll / cwPinRestore): scrolled down at all, the SPOTS
 *    heading keeps its place on screen while rows come and go above it; at the top, the view stays at the top.
 * ★ The decoder font throughout (DECODER_FONT) — never the display font (memory: display font on the main controls + VTS
 *   only).
 */
import React, { useCallback, useEffect, useMemo, useRef } from 'react';
import { ScrollView, StyleSheet, Text, TouchableOpacity, View, type StyleProp, type ViewStyle } from 'react-native';
import { DECODER_FONT, useDecoderStyles, useDecoderTokens, type DecoderTokens } from './DecoderShell';
import { scrollLane } from '../constants/popupTokens';
import {
  CW_Q_COLOURS, cwAge, cwFreqText, cwPinFromScroll, cwPinRestore, cwSnrText, sortCwSignals,
  type CwList, type CwSignal, type CwSpot,
} from '../services/cwList';

/** The spots drawn (the web keeps 200 and draws 60). */
export const CW_SPOTS_SHOWN = 60;

/** Every row a keyboard / pad can land on, in the order drawn: the signals, then the spots. */
export function cwItems(list: CwList | null, spots: readonly CwSpot[]): Array<{ hz: number }> {
  const sigs = sortCwSignals(list?.signals ?? []);
  return [...sigs.map((s) => ({ hz: s.hz })), ...spots.slice(0, CW_SPOTS_SHOWN).map((s) => ({ hz: s.frequency }))];
}

/** ★ The confidence marker, short enough for an SE in Display Zoom; the colour carries it as much as the word. */
const Q_SHORT: Record<CwSignal['q'], string> = { high: 'HIGH', medium: 'MED', low: 'LOW', poor: 'POOR' };

function hhmm(t: number): string {
  const d = new Date(t);
  return String(d.getUTCHours()).padStart(2, '0') + ':' + String(d.getUTCMinutes()).padStart(2, '0');
}

export interface CwViewProps {
  skim: boolean;
  list: CwList | null;
  spots: readonly CwSpot[];
  /** ONE SIGNAL's primary text (the decoder text); not drawn for the skimmer. */
  text: string;
  bodySize: StyleProp<ViewStyle>;
  /** The row the keyboard / pad is on, -1 for none. */
  focusIdx: number;
  onTune: (hz: number) => void;
  /** The panel's scroll hooks (bodyScrollY for the arrows) — merged with the pin's own. */
  scrollRef?: React.RefObject<ScrollView | null>;
  onScrollY?: (y: number) => void;
}

export default function CwView({ skim, list, spots, text, bodySize, focusIdx, onTune, scrollRef, onScrollY }: CwViewProps) {
  const tk = useDecoderTokens();
  const cs = useDecoderStyles(makeCs);
  const own = useRef<ScrollView | null>(null);
  const sv = scrollRef ?? own;
  const sigs = useMemo(() => sortCwSignals(list?.signals ?? []), [list]);
  const shownSpots = spots.slice(0, CW_SPOTS_SHOWN);
  const now = Date.now();

  /* ── The pin ── */
  const scrollY = useRef(0);
  const headY = useRef(0);
  const pinAt = useRef<number | null>(null);
  const userScrolling = useRef(false);
  const noteUser = (y: number) => { pinAt.current = cwPinFromScroll(y, headY.current); };
  const onScroll = (e: any) => {
    const y = e?.nativeEvent?.contentOffset?.y ?? 0;
    scrollY.current = y;
    onScrollY?.(y);
    if (userScrolling.current) noteUser(y);
  };
  const settle = (e: any) => {
    const y = e?.nativeEvent?.contentOffset?.y ?? scrollY.current;
    scrollY.current = y; onScrollY?.(y);
    noteUser(y);
  };
  /** The heading moved in the content (rows came or went above it): put it back where the listener left it. */
  const onHeadLayout = useCallback((e: any) => {
    headY.current = e?.nativeEvent?.layout?.y ?? 0;
    if (userScrolling.current) return;   // ★ never fight a finger
    const want = cwPinRestore(pinAt.current, headY.current, scrollY.current);
    if (want === null) return;
    scrollY.current = want;
    try { sv.current?.scrollTo({ y: want, animated: false }); } catch { /* detached */ }
  }, [sv]);

  /* ── The keyboard / pad's row: brought into view ── */
  const rowY = useRef<number[]>([]);
  useEffect(() => {
    if (focusIdx < 0) return;
    const y = rowY.current[focusIdx];
    if (y == null) return;
    try { sv.current?.scrollTo({ y: Math.max(0, y - 40), animated: false }); } catch { /* detached */ }
    // ★ a row reached by the keyboard is where the listener is looking — pin from there
    scrollY.current = Math.max(0, y - 40);
    noteUser(scrollY.current);
  }, [focusIdx]);   // eslint-disable-line react-hooks/exhaustive-deps

  const ring = (i: number) => (i === focusIdx ? { borderColor: tk.focus, borderWidth: 2, borderRadius: 6 } : null);

  return (
    <ScrollView
      ref={sv as any}
      style={[cs.body, bodySize]}
      contentContainerStyle={[cs.content, scrollLane]}
      showsVerticalScrollIndicator
      scrollEventThrottle={32}
      onScroll={onScroll}
      onScrollBeginDrag={() => { userScrolling.current = true; }}
      onScrollEndDrag={(e) => { settle(e); }}
      onMomentumScrollBegin={() => { userScrolling.current = true; }}
      onMomentumScrollEnd={(e) => { userScrolling.current = false; settle(e); }}
      onTouchEnd={() => { setTimeout(() => { userScrolling.current = false; }, 400); }}
    >
      {!skim && !!text && (
        <Text style={[cs.primary, { color: tk.value }]} selectable>{text}</Text>
      )}
      {!sigs.length && (
        <Text style={[cs.idle, { color: tk.muted }]}>
          {skim ? 'listening across the segment — each CW signal appears here as it is decoded'
                : 'listening — tune a CW signal into the passband'}
        </Text>
      )}
      {sigs.map((sg: CwSignal, i: number) => {
        const primary = !!list && !list.skimmer && sg.id === list.primary;
        const qc = CW_Q_COLOURS[sg.q];
        return (
          <TouchableOpacity key={`s${sg.id}`} activeOpacity={0.6}
            onLayout={(e) => { rowY.current[i] = e.nativeEvent.layout.y; }}
            accessibilityLabel={`${sg.call || 'unidentified'} ${cwFreqText(sg.hz, sg.audio)}${sg.hz > 0 ? ' kilohertz' : ''}, `
              + `${Math.round(sg.wpm)} words a minute, ${cwSnrText(sg.snr)}, ${sg.q} confidence — tap to tune`}
            style={[cs.row, { borderBottomColor: tk.divider }, primary && { backgroundColor: tk.selectedBg }, ring(i),
                    !sg.active && cs.gone]}
            onPress={() => onTune(sg.hz)}>
            <View style={cs.line}>
              <Text style={[cs.cell, cs.freq, { color: tk.value }]} numberOfLines={1}>{cwFreqText(sg.hz, sg.audio)}</Text>
              <Text style={[cs.cell, cs.call, { color: sg.call ? tk.accent : tk.muted }]} numberOfLines={1}>{sg.call || '—'}</Text>
              <Text style={[cs.cell, cs.wpm, { color: tk.value }]} numberOfLines={1}>{`${Math.round(sg.wpm)} wpm`}</Text>
              <Text style={[cs.cell, cs.snr, { color: tk.value }]} numberOfLines={1}>{cwSnrText(sg.snr)}</Text>
              <Text style={[cs.cell, cs.q, { color: qc, borderColor: qc }]} numberOfLines={1}>{Q_SHORT[sg.q]}</Text>
              <Text style={[cs.cell, cs.age, { color: tk.muted }]} numberOfLines={1}>{cwAge(sg.last, now)}</Text>
            </View>
            {!!sg.text.trim() && (
              <Text style={[cs.tx, { color: qc }]} numberOfLines={2}>{sg.text.trim()}</Text>
            )}
          </TouchableOpacity>
        );
      })}
      <View onLayout={onHeadLayout} style={[cs.head, { borderBottomColor: tk.divider }]}>
        <Text style={[cs.headTxt, { color: tk.label }]}>SPOTS</Text>
      </View>
      {!shownSpots.length && (
        <Text style={[cs.idle, { color: tk.muted }]}>a callsign heard calling (CQ, DE, TEST) twice is spotted here</Text>
      )}
      {shownSpots.map((s, j) => {
        const i = sigs.length + j;
        return (
          <TouchableOpacity key={`p${s.callsign}`} activeOpacity={0.6}
            onLayout={(e) => { rowY.current[i] = e.nativeEvent.layout.y; }}
            accessibilityLabel={`${s.callsign} ${s.how} ${cwFreqText(s.frequency, s.audio)} — tap to tune`}
            style={[cs.row, { borderBottomColor: tk.divider }, ring(i)]}
            onPress={() => onTune(s.frequency)}>
            <View style={cs.line}>
              <Text style={[cs.cell, cs.time, { color: tk.muted }]} numberOfLines={1}>{hhmm(s.timestamp)}</Text>
              <Text style={[cs.cell, cs.freq, { color: tk.value }]} numberOfLines={1}>{cwFreqText(s.frequency, s.audio)}</Text>
              <Text style={[cs.cell, cs.call, { color: tk.accent }]} numberOfLines={1}>{s.callsign}</Text>
              <Text style={[cs.cell, cs.country, { color: tk.value }]} numberOfLines={1}>{s.country}</Text>
              <Text style={[cs.cell, cs.wpm, { color: tk.value }]} numberOfLines={1}>{`${Math.round(s.wpm)} wpm`}</Text>
            </View>
          </TouchableOpacity>
        );
      })}
    </ScrollView>
  );
}

const makeCs = (T: DecoderTokens) => StyleSheet.create({
  body:    {},
  content: { paddingHorizontal: 8, paddingVertical: 6 },
  primary: { fontFamily: DECODER_FONT, fontSize: 12, letterSpacing: 0.8, lineHeight: 20, marginBottom: 6,
             textShadowColor: T.outputGlow, textShadowOffset: { width: 0, height: 0 }, textShadowRadius: 4 },
  idle:    { fontFamily: DECODER_FONT, fontSize: 11, padding: 10, textAlign: 'center' },
  // ★ 44 pt is Apple's touch minimum — a row is a tune target
  row:     { minHeight: 44, justifyContent: 'center', paddingHorizontal: 6, paddingVertical: 4,
             borderBottomWidth: StyleSheet.hairlineWidth, borderWidth: 0, borderColor: 'transparent' },
  gone:    { opacity: 0.55 },
  line:    { flexDirection: 'row', alignItems: 'center', gap: 5 },
  // ★ tabular-nums: frequency, speed, SNR and age are columns and line up down the list
  cell:    { fontFamily: DECODER_FONT, fontSize: 11, letterSpacing: 0.3, fontVariant: ['tabular-nums'] },
  freq:    { width: 58 },
  call:    { flex: 1, fontSize: 13, fontWeight: '700' },
  wpm:     { width: 44, textAlign: 'right' },
  snr:     { width: 40, textAlign: 'right' },
  q:       { width: 38, fontSize: 9, letterSpacing: 0.8, textAlign: 'center', borderWidth: 1, borderRadius: 3,
             paddingVertical: 1, overflow: 'hidden' },
  age:     { width: 26, textAlign: 'right' },
  time:    { width: 38 },
  country: { flex: 1, textAlign: 'right' },
  tx:      { fontFamily: DECODER_FONT, fontSize: 12, letterSpacing: 0.6, lineHeight: 17, marginTop: 2 },
  head:    { paddingTop: 10, paddingBottom: 4, paddingHorizontal: 6, borderBottomWidth: StyleSheet.hairlineWidth },
  headTxt: { fontFamily: DECODER_FONT, fontSize: 10, fontWeight: '700', letterSpacing: 1.4 },
});
