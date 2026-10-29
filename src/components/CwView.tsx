/**
 * CwView — the V12 CW decoder's body inside the decoder box (DecoderPanel): the web client's CW panel (main.ts renderCw).
 * ★ V12 PSK31/63 uses it too (`psk`): the web draws both in one list box — PSK shows its mode where CW shows its speed.
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
import { fmtSpotTime } from '../utils/spotTime';   // ★ HH:MM:SS UTC (one helper for every spot list)
import { plainEnglish } from '../utils/cwVocab';
import { cwHasWpmRange, qrzCallUrl } from '../utils/cwFormat';
import { openWebPage } from '../utils/openWebPage';
import {
  CW_Q_COLOURS, cwAge, cwFreqText, cwPinFromScroll, cwPinRestore, cwSnrText, kbIdleText, kbRateText, kbSpotRateText,
  sortCwSignals, type CwList, type CwSignal, type CwSpot,
} from '../services/cwList';

/** ★★ V12 (2026-10-29) A CALLSIGN OPENS ITS QRZ.COM PAGE — in the same in-app browser as the Signal ID wiki
 *  (utils/openWebPage). A nested Text with its own onPress takes the touch, so the row's own tap (tune) does not fire.
 *  Not a callsign (qrzCallUrl null: "—", a garbled call) = plain text. Used by every list with callsigns (CW, PSK,
 *  JTTY, the digital spots). Nothing is fetched from QRZ: it is a link. */
export function CallLink({ call, style, children }: { call: string; style: any; children?: React.ReactNode }) {
  const url = qrzCallUrl(call);
  if (!url) return <Text style={style} numberOfLines={1}>{children ?? call}</Text>;
  return (
    <Text style={[style, { textDecorationLine: 'underline' }]} numberOfLines={1} suppressHighlighting={false}
      accessibilityRole="link" accessibilityLabel={`${call} on QRZ.com`}
      onPress={(e: any) => { e?.stopPropagation?.(); openWebPage(url); }}>
      {children ?? call}
    </Text>
  );
}

/** The spots drawn (the web keeps 200 and draws 60). */
export const CW_SPOTS_SHOWN = 60;

/** Every row a keyboard / pad can land on, in the order drawn: the signals, then the spots. */
export function cwItems(list: CwList | null, spots: readonly CwSpot[]): Array<{ hz: number }> {
  const sigs = sortCwSignals(list?.signals ?? []);
  return [...sigs.map((s) => ({ hz: s.hz })), ...spots.slice(0, CW_SPOTS_SHOWN).map((s) => ({ hz: s.frequency }))];
}

/** ★ The confidence marker, short enough for an SE in Display Zoom; the colour carries it as much as the word. */
export const Q_SHORT: Record<CwSignal['q'], string> = { high: 'HIGH', medium: 'MED', low: 'LOW', poor: 'POOR' };

/** ★★ THE VIEW STAYS WHERE THE LISTENER PUT IT (cwList cwPinFromScroll / cwPinRestore) — the CW list's scroll pin as a hook,
 *  shared by every list-with-SPOTS view (CW, PSK, JTTY): scrolled down at all, the SPOTS heading keeps its place on screen
 *  while rows come and go above it; at the top, the view stays at the top. A keyboard / pad row is brought into view. */
export function useSpotsPin(scrollRef: React.RefObject<ScrollView | null> | undefined, onScrollY: ((y: number) => void) | undefined,
                            focusIdx: number) {
  const own = useRef<ScrollView | null>(null);
  const sv = scrollRef ?? own;
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
  const scrollProps = {
    ref: sv as any,
    scrollEventThrottle: 32,
    onScroll,
    onScrollBeginDrag: () => { userScrolling.current = true; },
    onScrollEndDrag: (e: any) => { settle(e); },
    onMomentumScrollBegin: () => { userScrolling.current = true; },
    onMomentumScrollEnd: (e: any) => { userScrolling.current = false; settle(e); },
    onTouchEnd: () => { setTimeout(() => { userScrolling.current = false; }, 400); },
  };
  return { scrollProps, onHeadLayout, rowY };
}


export interface CwViewProps {
  skim: boolean;
  /** ★ V12: the PSK decoder's list (its mode where CW has its speed). */
  psk?: boolean;
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
  /** ★★ V12 (2026-10-29) THE PLAIN ENGLISH KEY (DecoderPanel's header): every row's text, and ONE SIGNAL's, read
   *  through utils/cwVocab plainEnglish — codes in plain words, callsigns / names / numbers as sent. CW only. */
  plain?: boolean;
}

export default function CwView({ skim, psk = false, list, spots, text, bodySize, focusIdx, onTune, scrollRef, onScrollY,
                                 plain = false }: CwViewProps) {
  const tk = useDecoderTokens();
  const cs = useDecoderStyles(makeCs);
  const sigs = useMemo(() => sortCwSignals(list?.signals ?? []), [list]);
  const shownSpots = spots.slice(0, CW_SPOTS_SHOWN);
  const now = Date.now();
  const { scrollProps, onHeadLayout, rowY } = useSpotsPin(scrollRef, onScrollY, focusIdx);

  const ring = (i: number) => (i === focusIdx ? { borderColor: tk.focus, borderWidth: 2, borderRadius: 6 } : null);
  const read = (t: string) => (plain && !psk ? plainEnglish(t) : t);
  // ★ A speed RANGE ("17–21 wpm") needs a wider column — only while a row shows one, so an older server's list keeps
  //   its call column as wide as before.
  const wideRows = !psk && sigs.some(cwHasWpmRange);
  const wideSpots = !psk && shownSpots.some(cwHasWpmRange);

  return (
    <ScrollView
      {...scrollProps}
      style={[cs.body, bodySize]}
      contentContainerStyle={[cs.content, scrollLane]}
      showsVerticalScrollIndicator
    >
      {!skim && !!text && (
        <Text style={[cs.primary, { color: tk.value }]} selectable>{read(text)}</Text>
      )}
      {!sigs.length && (
        <Text style={[cs.idle, { color: tk.muted }]}>{kbIdleText(psk, skim)}</Text>
      )}
      {sigs.map((sg: CwSignal, i: number) => {
        const primary = !!list && !list.skimmer && sg.id === list.primary;
        const qc = CW_Q_COLOURS[sg.q];
        return (
          <TouchableOpacity key={`s${sg.id}`} activeOpacity={0.6}
            onLayout={(e) => { rowY.current[i] = e.nativeEvent.layout.y; }}
            accessibilityLabel={`${sg.call || 'unidentified'} ${cwFreqText(sg.hz, sg.audio)}${sg.hz > 0 ? ' kilohertz' : ''}, `
              + `${sg.baud !== undefined ? `PSK${sg.baud}` : `${Math.round(sg.wpm)} words a minute`}, ${cwSnrText(sg.snr)}, `
              + `${sg.q} confidence — tap to tune`}
            style={[cs.row, { borderBottomColor: tk.divider }, primary && { backgroundColor: tk.selectedBg }, ring(i),
                    !sg.active && cs.gone]}
            onPress={() => onTune(sg.hz)}>
            <View style={cs.line}>
              <Text style={[cs.cell, cs.freq, { color: tk.value }]} numberOfLines={1}>{cwFreqText(sg.hz, sg.audio)}</Text>
              <CallLink call={sg.call} style={[cs.cell, cs.call, { color: sg.call ? tk.accent : tk.muted }]}>{sg.call || '—'}</CallLink>
              <Text style={[cs.cell, cs.wpm, wideRows && cs.wpmWide, { color: tk.value }]} numberOfLines={1}>{kbRateText(sg)}</Text>
              <Text style={[cs.cell, cs.snr, { color: tk.value }]} numberOfLines={1}>{cwSnrText(sg.snr)}</Text>
              <Text style={[cs.cell, cs.q, { color: qc, borderColor: qc }]} numberOfLines={1}>{Q_SHORT[sg.q]}</Text>
              <Text style={[cs.cell, cs.age, { color: tk.muted }]} numberOfLines={1}>{cwAge(sg.last, now)}</Text>
            </View>
            {!!sg.text.trim() && (
              <Text style={[cs.tx, { color: qc }]} numberOfLines={2}>{read(sg.text.trim())}</Text>
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
              <Text style={[cs.cell, cs.time, { color: tk.muted }]} numberOfLines={1}>{fmtSpotTime(s.timestamp)}</Text>
              <Text style={[cs.cell, cs.freq, { color: tk.value }]} numberOfLines={1}>{cwFreqText(s.frequency, s.audio)}</Text>
              <CallLink call={s.callsign} style={[cs.cell, cs.call, { color: tk.accent }]} />
              <Text style={[cs.cell, cs.country, { color: tk.value }]} numberOfLines={1}>{s.country}</Text>
              <Text style={[cs.cell, cs.wpm, wideSpots && cs.wpmWide, { color: tk.value }]} numberOfLines={1}>{kbSpotRateText(s)}</Text>
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
  wpmWide: { width: 64 },   // ★ "17–21 wpm"
  snr:     { width: 40, textAlign: 'right' },
  q:       { width: 38, fontSize: 9, letterSpacing: 0.8, textAlign: 'center', borderWidth: 1, borderRadius: 3,
             paddingVertical: 1, overflow: 'hidden' },
  age:     { width: 26, textAlign: 'right' },
  time:    { width: 52 },   // ★ HH:MM:SS (was HH:MM, 38)
  country: { flex: 1, textAlign: 'right' },
  tx:      { fontFamily: DECODER_FONT, fontSize: 12, letterSpacing: 0.6, lineHeight: 17, marginTop: 2 },
  head:    { paddingTop: 10, paddingBottom: 4, paddingHorizontal: 6, borderBottomWidth: StyleSheet.hairlineWidth },
  headTxt: { fontFamily: DECODER_FONT, fontSize: 10, fontWeight: '700', letterSpacing: 1.4 },
});
