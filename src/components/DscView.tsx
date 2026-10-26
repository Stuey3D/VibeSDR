/**
 * DscView — the V12 DSC decoder's body inside the decoder box (DecoderPanel): the web client's DSC box (dsc.ts
 * renderDscRows, main.ts renderDsc).
 *
 *   each call the server's own readable line ("DISTRESS · MMSI 235012345 (United Kingdom) · Sinking · 50°12'N …"),
 *   newest first, coloured by what it is — distress red, urgency orange, safety amber, routine plain; a transmission's
 *   repeats are one row "×5"; "corrected" / "unconfirmed" when the check character had work to do; MAP on a position.
 *
 * ★ READABLE, NEVER A SYMBOL DUMP. A keyboard / pad reaches every row (DecoderPanel's list zone; `focusIdx`); Enter on a
 *   row with a position is its MAP. ★ The decoder font throughout (DECODER_FONT).
 */
import React, { useEffect, useRef } from 'react';
import { ScrollView, StyleSheet, Text, TouchableOpacity, View, type StyleProp, type ViewStyle } from 'react-native';
import { DECODER_FONT, useDecoderStyles, useDecoderTokens, type DecoderTokens } from './DecoderShell';
import { scrollLane } from '../constants/popupTokens';
import { DSC_IDLE, DSC_LEVEL_BG, DSC_LEVEL_BORDER, DSC_LEVEL_NAMES, DSC_LEVEL_TEXT, dscFreqText, dscTimeText, type DscCall } from '../services/dsc';

export interface DscViewProps {
  calls: readonly DscCall[];
  bodySize: StyleProp<ViewStyle>;
  focusIdx: number;
  /** MAP for a call that sent a position; absent = no MAP key (never a dead control). */
  onMap?: (c: DscCall) => void;
  scrollRef?: React.RefObject<ScrollView | null>;
  onScrollY?: (y: number) => void;
}

export const dscHasPos = (c: DscCall) => c.lat !== null && c.lon !== null;

export default function DscView({ calls, bodySize, focusIdx, onMap, scrollRef, onScrollY }: DscViewProps) {
  const tk = useDecoderTokens();
  const cs = useDecoderStyles(makeCs);
  const own = useRef<ScrollView | null>(null);
  const sv = scrollRef ?? own;
  const rowY = useRef<number[]>([]);
  useEffect(() => {
    if (focusIdx < 0) return;
    const y = rowY.current[focusIdx];
    if (y == null) return;
    try { sv.current?.scrollTo({ y: Math.max(0, y - 40), animated: false }); } catch { /* detached */ }
  }, [focusIdx, sv]);
  const ring = (i: number) => (i === focusIdx ? { borderColor: tk.focus, borderWidth: 2, borderRadius: 6 } : null);

  return (
    <ScrollView ref={sv as any} style={[cs.body, bodySize]} contentContainerStyle={[cs.content, scrollLane]}
                showsVerticalScrollIndicator scrollEventThrottle={32}
                onScroll={(e) => onScrollY?.(e?.nativeEvent?.contentOffset?.y ?? 0)}>
      {!calls.length && <Text style={[cs.idle, { color: tk.muted }]}>{DSC_IDLE}</Text>}
      {calls.map((c, i) => {
        const rule = DSC_LEVEL_BORDER[c.level];
        const colour = DSC_LEVEL_TEXT[c.level] ?? tk.value;
        const wash = DSC_LEVEL_BG[c.level];
        const pos = dscHasPos(c) && !!onMap;
        const level = DSC_LEVEL_NAMES[c.level] ?? 'routine';
        const head = (
          <View style={cs.line}>
            <Text style={[cs.cell, cs.time, { color: tk.muted }]} numberOfLines={1}>{dscTimeText(c.utc)}</Text>
            <Text style={[cs.cell, cs.freq, { color: tk.value }]} numberOfLines={1}>{dscFreqText(c.rf)}</Text>
            {c.repeat > 1 && <Text style={[cs.cell, cs.rep, { color: tk.value }]}>{`×${c.repeat}`}</Text>}
            {c.check !== 'ok' && <Text style={[cs.cell, cs.ck, { color: tk.muted, borderColor: tk.divider }]}>{c.check}</Text>}
            <View style={{ flex: 1 }} />
            {pos && (
              <TouchableOpacity accessibilityRole="button" accessibilityLabel={`${c.posText || 'position'} — show on the map`}
                                hitSlop={8} style={[cs.map, { borderColor: tk.accent }]} onPress={() => onMap!(c)}>
                <Text style={[cs.mapTxt, { color: tk.accent }]}>MAP</Text>
              </TouchableOpacity>
            )}
          </View>
        );
        return (
          <View key={`${c.id}:${c.rf}`} onLayout={(e) => { rowY.current[i] = e.nativeEvent.layout.y; }}
                accessible accessibilityLabel={`${level}${c.repeat > 1 ? `, sent ${c.repeat} times` : ''}: ${c.line}`}
                style={[cs.row, { borderBottomColor: tk.divider }, !!rule && { borderLeftColor: rule, borderLeftWidth: 3 }, !!wash && { backgroundColor: wash },
                        ring(i), c.check === 'unconfirmed' && cs.unconf]}>
            {head}
            <Text style={[cs.tx, { color: colour, fontWeight: c.level >= 3 ? '700' : '400' }]} selectable>{c.line}</Text>
          </View>
        );
      })}
    </ScrollView>
  );
}

const makeCs = (_T: DecoderTokens) => StyleSheet.create({
  body:    {},
  content: { paddingHorizontal: 8, paddingVertical: 6 },
  idle:    { fontFamily: DECODER_FONT, fontSize: 11, padding: 10, textAlign: 'center' },
  row:     { minHeight: 44, justifyContent: 'center', paddingHorizontal: 6, paddingVertical: 5,
             borderBottomWidth: StyleSheet.hairlineWidth, borderWidth: 0, borderColor: 'transparent' },
  unconf:  { opacity: 0.7 },
  line:    { flexDirection: 'row', alignItems: 'center', gap: 6 },
  cell:    { fontFamily: DECODER_FONT, fontSize: 11, letterSpacing: 0.3, fontVariant: ['tabular-nums'] },
  time:    { width: 66 },
  freq:    { minWidth: 70 },
  rep:     { fontWeight: '700' },
  ck:      { fontSize: 9, letterSpacing: 0.6, borderWidth: 1, borderRadius: 3, paddingHorizontal: 3, overflow: 'hidden' },
  // ★ 44 pt tall with its hitSlop — MAP is a target of its own inside the row
  map:     { borderWidth: 1, borderRadius: 4, paddingHorizontal: 8, paddingVertical: 4, minWidth: 44, alignItems: 'center' },
  mapTxt:  { fontFamily: DECODER_FONT, fontSize: 10, fontWeight: '700', letterSpacing: 1 },
  tx:      { fontFamily: DECODER_FONT, fontSize: 12, letterSpacing: 0.4, lineHeight: 17, marginTop: 3 },
});
