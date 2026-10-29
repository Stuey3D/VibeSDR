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
 * ★★ V12 (Stuart, 2026-10-29: "most people tune DSC wrong") THE BOX CARRIES ITS OWN CHANNEL KEYS — the band the dial is on
 *   (its distress channel and its ITU-R M.541 routine ones), else the distress set — and, when the dial sits ON an assigned
 *   frequency (4208.5 typed straight in, where the tones fall at 0 Hz), a one-tap "TUNE 4206.8 USB for 4208.5 kHz" above
 *   them. They come first in the list zone: `focusIdx` counts the keys, then the calls.
 */
import React, { useEffect, useRef } from 'react';
import { ScrollView, StyleSheet, Text, TouchableOpacity, View, type StyleProp, type ViewStyle } from 'react-native';
import { DECODER_FONT, useDecoderStyles, useDecoderTokens, type DecoderTokens } from './DecoderShell';
import { scrollLane } from '../constants/popupTokens';
import { DSC_IDLE, DSC_LEVEL_BG, DSC_LEVEL_BORDER, DSC_LEVEL_NAMES, DSC_LEVEL_TEXT, dscFreqText, dscTimeText, type DscCall } from '../services/dsc';

/** A key at the top of the box: the tune offer (`offer`), or a channel (lit while the dial is on it). */
export interface DscBoxKey { id: string; label: string; a11y: string; on: boolean; offer?: boolean; distress?: boolean; press: () => void }

export interface DscViewProps {
  calls: readonly DscCall[];
  /** The box's keys, drawn above the calls (the list zone reaches them first). Absent / empty = none. */
  keys?: readonly DscBoxKey[];
  bodySize: StyleProp<ViewStyle>;
  focusIdx: number;
  /** MAP for a call that sent a position; absent = no MAP key (never a dead control). */
  onMap?: (c: DscCall) => void;
  scrollRef?: React.RefObject<ScrollView | null>;
  onScrollY?: (y: number) => void;
}

export const dscHasPos = (c: DscCall) => c.lat !== null && c.lon !== null;

export default function DscView({ calls, keys = [], bodySize, focusIdx, onMap, scrollRef, onScrollY }: DscViewProps) {
  const tk = useDecoderTokens();
  const cs = useDecoderStyles(makeCs);
  const own = useRef<ScrollView | null>(null);
  const sv = scrollRef ?? own;
  const rowY = useRef<number[]>([]);
  const nk = keys.length;
  useEffect(() => {
    if (focusIdx < 0) return;
    if (focusIdx < nk) { try { sv.current?.scrollTo({ y: 0, animated: false }); } catch { /* detached */ } return; }
    const y = rowY.current[focusIdx - nk];
    if (y == null) return;
    try { sv.current?.scrollTo({ y: Math.max(0, y - 40), animated: false }); } catch { /* detached */ }
  }, [focusIdx, sv, nk]);
  const ring = (i: number) => (i + nk === focusIdx ? { borderColor: tk.focus, borderWidth: 2, borderRadius: 6 } : null);
  const offer = keys.find((k) => k.offer);
  const chans = keys.filter((k) => !k.offer);

  return (
    <ScrollView ref={sv as any} style={[cs.body, bodySize]} contentContainerStyle={[cs.content, scrollLane]}
                showsVerticalScrollIndicator scrollEventThrottle={32}
                onScroll={(e) => onScrollY?.(e?.nativeEvent?.contentOffset?.y ?? 0)}>
      {!!offer && (
        <TouchableOpacity accessibilityRole="button" accessibilityLabel={offer.a11y} onPress={offer.press}
                          style={[cs.offer, { borderColor: tk.chipBorderAct, backgroundColor: tk.chipBgAct }, keys.indexOf(offer) === focusIdx && { borderColor: tk.focus, borderWidth: 2 }]}>
          <Text style={[cs.offerTxt, { color: tk.accent }]}>{offer.label}</Text>
          <Text style={[cs.offerWhy, { color: tk.muted }]}>A listed DSC frequency is the assigned one — the dial goes 1.7 kHz below it in USB.</Text>
        </TouchableOpacity>
      )}
      {chans.length > 0 && (
        <View style={cs.keys}>
          {chans.map((k) => {
            const i = keys.indexOf(k);
            return (
              <TouchableOpacity key={k.id} accessibilityRole="button" accessibilityLabel={k.a11y} accessibilityState={{ selected: k.on }}
                                hitSlop={4} onPress={k.press}
                                style={[cs.key, { borderColor: k.on ? tk.chipBorderAct : tk.chipBorder }, k.on && { backgroundColor: tk.chipBgAct },
                                        !!k.distress && !k.on && { borderColor: DSC_LEVEL_BORDER[3] ?? tk.divider },
                                        i === focusIdx && { borderColor: tk.focus, borderWidth: 2 }]}>
                <Text style={[cs.keyTxt, { color: k.on ? tk.selected : tk.value }]}>{k.label}</Text>
              </TouchableOpacity>
            );
          })}
        </View>
      )}
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
  // ★ The channel keys: 44 pt targets with their hitSlop, wrapped; a distress channel's rule in the distress red.
  keys:    { flexDirection: 'row', flexWrap: 'wrap', gap: 6, paddingVertical: 4 },
  key:     { borderWidth: 1, borderRadius: 5, paddingHorizontal: 10, paddingVertical: 7, minWidth: 64, alignItems: 'center' },
  keyTxt:  { fontFamily: DECODER_FONT, fontSize: 12, fontWeight: '700', letterSpacing: 0.4, fontVariant: ['tabular-nums'] },
  offer:   { borderWidth: 1, borderRadius: 6, paddingHorizontal: 10, paddingVertical: 8, marginVertical: 4 },
  offerTxt: { fontFamily: DECODER_FONT, fontSize: 13, fontWeight: '700', letterSpacing: 0.6 },
  offerWhy: { fontFamily: DECODER_FONT, fontSize: 10, marginTop: 2 },
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
