/**
 * AprsView — the V12 APRS decoder's body inside the decoder box (DecoderPanel): the web client's APRS box (main.ts
 * renderAprs) — the stations heard, newest first (a swatch in the station's colour, the call, the readable line, the
 * distance and age), then MESSAGES (when there are any) and PACKETS as they arrive.
 *
 * ★ A row press opens the station map on that station (the web's: "a row in the box opens the map on that station"). A
 *   keyboard / pad reaches every station row (DecoderPanel's list zone; `focusIdx`); Enter does the same.
 * ★ What it says is src/services/tracks.ts (aprsBoxRows / aprsMessageLines / aprsLogTime) — one rule with the web.
 * ★ The decoder font throughout and the decoder box's own tokens, so it follows the faceplate (default / silver / black,
 *   transparency on or off) as every other decoder body does. The station colours are the map's — they mean a kind.
 */
import React, { useEffect, useRef } from 'react';
import { ScrollView, StyleSheet, Text, TouchableOpacity, View, type StyleProp, type ViewStyle } from 'react-native';
import { DECODER_FONT, useDecoderStyles, useDecoderTokens, type DecoderTokens } from './DecoderShell';
import { scrollLane } from '../constants/popupTokens';
import { aprsLogTime, type AprsBoxRow } from '../services/tracks';

export interface AprsViewProps {
  rows: AprsBoxRow[];
  messages: string[];
  log: { t: number; line: string }[];
  baud: 1200 | 300;
  bodySize: StyleProp<ViewStyle>;
  /** The row the keyboard / pad is on, -1 for none. */
  focusIdx: number;
  /** A row pressed: the station map, on it. Null where there is no map (Apple TV) — the rows are then read, not pressed. */
  onMap: ((key: string) => void) | null;
  scrollRef?: React.RefObject<ScrollView | null>;
  onScrollY?: (y: number) => void;
}

export default function AprsView({ rows, messages, log, baud, bodySize, focusIdx, onMap, scrollRef, onScrollY }: AprsViewProps) {
  const tk = useDecoderTokens();
  const s = useDecoderStyles(makeS);
  const own = useRef<ScrollView | null>(null);
  const sv = scrollRef ?? own;
  const rowY = useRef<number[]>([]);
  useEffect(() => {
    if (focusIdx < 0) return;
    const y = rowY.current[focusIdx];
    if (y == null) return;
    try { sv.current?.scrollTo({ y: Math.max(0, y - 40), animated: false }); } catch { /* detached */ }
  }, [focusIdx]);   // eslint-disable-line react-hooks/exhaustive-deps
  const ring = (i: number) => (i === focusIdx ? { borderColor: tk.focus, borderWidth: 2, borderRadius: 6 } : null);

  return (
    <ScrollView ref={sv as any} style={[s.body, bodySize]} contentContainerStyle={[s.content, scrollLane]}
                showsVerticalScrollIndicator scrollEventThrottle={32}
                onScroll={(e) => onScrollY?.(e.nativeEvent.contentOffset.y)}>
      {!rows.length && (
        <Text style={[s.idle, { color: tk.muted }]}>
          {`listening — ${baud === 300 ? 'HF packet, 300 Bd' : 'APRS, 1200 Bd'}; stations appear with their first packet`}
        </Text>
      )}
      {rows.map((r, i) => {
        const body = (
          <>
            <View style={s.line}>
              <View style={[s.sw, { backgroundColor: r.colour }]} />
              <Text style={[s.cell, s.call, { color: tk.accent }]} numberOfLines={1}>{r.call}</Text>
              <Text style={[s.cell, s.right, { color: tk.muted }]} numberOfLines={1}>{r.right}</Text>
            </View>
            <Text style={[s.tx, { color: tk.value }]} numberOfLines={2}>{r.line}</Text>
          </>
        );
        const style = [s.row, { borderBottomColor: tk.divider }, r.noPos && s.noPos, ring(i)];
        const label = `${r.call}, ${r.line}, ${r.right}${onMap ? (r.noPos ? ' — no position yet' : ' — show on the map') : ''}`;
        return onMap ? (
          <TouchableOpacity key={r.key} activeOpacity={0.6} style={style} accessibilityRole="button" accessibilityLabel={label}
            onLayout={(e) => { rowY.current[i] = e.nativeEvent.layout.y; }} onPress={() => onMap(r.key)}>{body}</TouchableOpacity>
        ) : (
          <View key={r.key} style={style} accessibilityLabel={label}
            onLayout={(e) => { rowY.current[i] = e.nativeEvent.layout.y; }}>{body}</View>
        );
      })}
      {messages.length > 0 && (
        <View style={[s.head, { borderBottomColor: tk.divider }]}><Text style={[s.headTxt, { color: tk.label }]}>MESSAGES</Text></View>
      )}
      {messages.map((m, i) => <Text key={i} style={[s.msg, { color: tk.value }]} selectable>{m}</Text>)}
      <View style={[s.head, { borderBottomColor: tk.divider }]}><Text style={[s.headTxt, { color: tk.label }]}>PACKETS</Text></View>
      {!log.length && <Text style={[s.idle, { color: tk.muted }]}>no packets yet</Text>}
      {log.slice(0, 40).map((p, i) => (
        <Text key={`${p.t}-${i}`} style={[s.pk, { color: tk.value }]} selectable>
          <Text style={{ color: tk.muted }}>{aprsLogTime(p.t)} </Text>{p.line}
        </Text>
      ))}
    </ScrollView>
  );
}

const makeS = (T: DecoderTokens) => StyleSheet.create({
  body:    {},
  content: { paddingHorizontal: 8, paddingVertical: 6 },
  idle:    { fontFamily: DECODER_FONT, fontSize: 11, padding: 10, textAlign: 'center' },
  // ★ 44 pt: a row is a target (the map)
  row:     { minHeight: 44, justifyContent: 'center', paddingHorizontal: 6, paddingVertical: 4,
             borderBottomWidth: StyleSheet.hairlineWidth, borderWidth: 0, borderColor: 'transparent' },
  noPos:   { opacity: 0.6 },
  line:    { flexDirection: 'row', alignItems: 'center', gap: 6 },
  sw:      { width: 8, height: 8, borderRadius: 4 },
  cell:    { fontFamily: DECODER_FONT, fontSize: 11, letterSpacing: 0.3, fontVariant: ['tabular-nums'] },
  call:    { flex: 1, fontSize: 13, fontWeight: '700' },
  right:   { textAlign: 'right' },
  tx:      { fontFamily: DECODER_FONT, fontSize: 12, letterSpacing: 0.4, lineHeight: 17, marginTop: 2,
             textShadowColor: T.outputGlow, textShadowOffset: { width: 0, height: 0 }, textShadowRadius: 3 },
  head:    { paddingTop: 10, paddingBottom: 4, paddingHorizontal: 6, borderBottomWidth: StyleSheet.hairlineWidth },
  headTxt: { fontFamily: DECODER_FONT, fontSize: 10, fontWeight: '700', letterSpacing: 1.4 },
  msg:     { fontFamily: DECODER_FONT, fontSize: 12, lineHeight: 17, paddingHorizontal: 6, paddingVertical: 2 },
  pk:      { fontFamily: DECODER_FONT, fontSize: 11, lineHeight: 16, paddingHorizontal: 6, paddingVertical: 1 },
});
