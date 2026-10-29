/**
 * JttyView — the V12 JTTY decoder's body inside the decoder box (DecoderPanel): the web client's JTTY box (jtty.ts
 * renderJttyRows / renderJttySpots, main.ts renderJtty).
 *
 *   one row per station, LOWEST FREQUENCY FIRST: frequency · call · SNR · confidence · last heard, then its recent
 *   messages coloured by the decoder's confidence (red poor, orange low, amber medium, green high — CW's colours)
 *   SPOTS: every call heard, newest first, one row per call — CQ (calling) or QSO (in an exchange) · country
 *
 * Tap a row or a spot = USB, the dial 1500 Hz below the station (jtty.ts jttyTuneFor — the caller tunes). A keyboard / pad
 * reaches every row (DecoderPanel's list zone; `focusIdx`). The view keeps its place while it updates (CW's pin,
 * useSpotsPin). ★ The decoder font throughout (DECODER_FONT) — never the display font.
 */
import React, { useMemo } from 'react';
import { ScrollView, StyleSheet, Text, TouchableOpacity, View, type StyleProp, type ViewStyle } from 'react-native';
import { DECODER_FONT, useDecoderStyles, useDecoderTokens, type DecoderTokens } from './DecoderShell';
import { scrollLane } from '../constants/popupTokens';
import { fmtSpotTime } from '../utils/spotTime';
import { CW_Q_COLOURS, cwAge, cwFreqText, cwSnrText } from '../services/cwList';
import { JTTY_IDLE_ROWS, JTTY_IDLE_SPOTS, JTTY_SPOTS_SHOWN, sortJtty, type JttySignal, type JttySpot } from '../services/jtty';
import { CallLink, Q_SHORT, useSpotsPin } from './CwView';

/** Every row a keyboard / pad can land on, in the order drawn: the stations, then the spots. `hz` is RF (0 = none). */
export function jttyItems(list: readonly JttySignal[], spots: readonly JttySpot[]): Array<{ hz: number }> {
  return [...sortJtty(list).map((s) => ({ hz: s.hz })), ...spots.slice(0, JTTY_SPOTS_SHOWN).map((s) => ({ hz: s.frequency }))];
}

export interface JttyViewProps {
  list: readonly JttySignal[];
  spots: readonly JttySpot[];
  bodySize: StyleProp<ViewStyle>;
  focusIdx: number;
  onTune: (rfHz: number) => void;
  scrollRef?: React.RefObject<ScrollView | null>;
  onScrollY?: (y: number) => void;
}

export default function JttyView({ list, spots, bodySize, focusIdx, onTune, scrollRef, onScrollY }: JttyViewProps) {
  const tk = useDecoderTokens();
  const cs = useDecoderStyles(makeCs);
  const sigs = useMemo(() => sortJtty(list), [list]);
  const shownSpots = spots.slice(0, JTTY_SPOTS_SHOWN);
  const now = Date.now();
  const { scrollProps, onHeadLayout, rowY } = useSpotsPin(scrollRef, onScrollY, focusIdx);
  const ring = (i: number) => (i === focusIdx ? { borderColor: tk.focus, borderWidth: 2, borderRadius: 6 } : null);

  return (
    <ScrollView {...scrollProps} style={[cs.body, bodySize]} contentContainerStyle={[cs.content, scrollLane]}
                showsVerticalScrollIndicator>
      {!sigs.length && <Text style={[cs.idle, { color: tk.muted }]}>{JTTY_IDLE_ROWS}</Text>}
      {sigs.map((sg, i) => {
        const qc = CW_Q_COLOURS[sg.q];
        return (
          <TouchableOpacity key={`s${sg.id}`} activeOpacity={0.6}
            onLayout={(e) => { rowY.current[i] = e.nativeEvent.layout.y; }}
            accessibilityLabel={`${sg.call || 'unidentified'}${sg.country ? ', ' + sg.country : ''}, ${cwFreqText(sg.hz, sg.audio)}`
              + `${sg.hz > 0 ? ' kilohertz' : ''}, ${cwSnrText(sg.snr)}, ${sg.frames} frame${sg.frames === 1 ? '' : 's'}, `
              + `${sg.q} confidence — tap to tune`}
            style={[cs.row, { borderBottomColor: tk.divider }, ring(i), !sg.active && cs.gone]}
            onPress={() => onTune(sg.hz)}>
            <View style={cs.line}>
              <Text style={[cs.cell, cs.freq, { color: tk.value }]} numberOfLines={1}>{cwFreqText(sg.hz, sg.audio)}</Text>
              <CallLink call={sg.call} style={[cs.cell, cs.call, { color: sg.call ? tk.accent : tk.muted }]}>{sg.call || '—'}</CallLink>
              <Text style={[cs.cell, cs.snr, { color: tk.value }]} numberOfLines={1}>{cwSnrText(sg.snr)}</Text>
              <Text style={[cs.cell, cs.q, { color: qc, borderColor: qc }]} numberOfLines={1}>{Q_SHORT[sg.q]}</Text>
              <Text style={[cs.cell, cs.age, { color: tk.muted }]} numberOfLines={1}>{cwAge(sg.last, now)}</Text>
            </View>
            {!!sg.text.trim() && <Text style={[cs.tx, { color: qc }]} numberOfLines={3}>{sg.text.trim()}</Text>}
          </TouchableOpacity>
        );
      })}
      <View onLayout={onHeadLayout} style={[cs.head, { borderBottomColor: tk.divider }]}>
        <Text style={[cs.headTxt, { color: tk.label }]}>SPOTS</Text>
      </View>
      {!shownSpots.length && <Text style={[cs.idle, { color: tk.muted }]}>{JTTY_IDLE_SPOTS}</Text>}
      {shownSpots.map((s, j) => {
        const i = sigs.length + j;
        return (
          <TouchableOpacity key={`p${s.callsign}`} activeOpacity={0.6}
            onLayout={(e) => { rowY.current[i] = e.nativeEvent.layout.y; }}
            accessibilityLabel={`${s.callsign}, ${s.how === 'CQ' ? 'calling CQ' : 'in an exchange'}, `
              + `${cwFreqText(s.frequency, s.audio)} — tap to tune`}
            style={[cs.row, { borderBottomColor: tk.divider }, ring(i)]}
            onPress={() => onTune(s.frequency)}>
            <View style={cs.line}>
              <Text style={[cs.cell, cs.time, { color: tk.muted }]} numberOfLines={1}>{fmtSpotTime(s.timestamp)}</Text>
              <Text style={[cs.cell, cs.freq, { color: tk.value }]} numberOfLines={1}>{cwFreqText(s.frequency, s.audio)}</Text>
              <CallLink call={s.callsign} style={[cs.cell, cs.call, { color: tk.accent }]} />
              {/* ★ CQ stands out (the web's .jts.cq): a station calling is the one to answer */}
              <Text style={[cs.cell, cs.how, { color: s.how === 'CQ' ? CW_Q_COLOURS.high : tk.muted }]} numberOfLines={1}>{s.how}</Text>
              <Text style={[cs.cell, cs.country, { color: tk.value }]} numberOfLines={1}>{s.country}</Text>
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
  idle:    { fontFamily: DECODER_FONT, fontSize: 11, padding: 10, textAlign: 'center' },
  // ★ 44 pt is Apple's touch minimum — a row is a tune target
  row:     { minHeight: 44, justifyContent: 'center', paddingHorizontal: 6, paddingVertical: 4,
             borderBottomWidth: StyleSheet.hairlineWidth, borderWidth: 0, borderColor: 'transparent' },
  gone:    { opacity: 0.55 },
  line:    { flexDirection: 'row', alignItems: 'center', gap: 5 },
  cell:    { fontFamily: DECODER_FONT, fontSize: 11, letterSpacing: 0.3, fontVariant: ['tabular-nums'] },
  freq:    { width: 58 },
  call:    { flex: 1, fontSize: 13, fontWeight: '700' },
  snr:     { width: 40, textAlign: 'right' },
  q:       { width: 38, fontSize: 9, letterSpacing: 0.8, textAlign: 'center', borderWidth: 1, borderRadius: 3,
             paddingVertical: 1, overflow: 'hidden' },
  age:     { width: 26, textAlign: 'right' },
  time:    { width: 52 },
  how:     { width: 30, fontSize: 10, fontWeight: '700', letterSpacing: 0.6 },
  country: { flex: 1, textAlign: 'right' },
  tx:      { fontFamily: DECODER_FONT, fontSize: 12, letterSpacing: 0.6, lineHeight: 17, marginTop: 2 },
  head:    { paddingTop: 10, paddingBottom: 4, paddingHorizontal: 6, borderBottomWidth: StyleSheet.hairlineWidth },
  headTxt: { fontFamily: DECODER_FONT, fontSize: 10, fontWeight: '700', letterSpacing: 1.4, color: T.label },
});
