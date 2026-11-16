/**
 * NcdxfView — the V12 NCDXF / IARU beacon tracker's body inside the decoder box (DecoderPanel): the web client's grid
 * (main.ts renderNcdxf), from the same pure rows (services/ncdxf.ts ncdxfRows).
 *
 *   one row per beacon in the published order, one column per band (20 17 15 12 10 m): the steps heard (4 = down to
 *   0.1 W), coloured green → amber as fewer are heard; "—" tried and not heard; blank not tried yet; the beacon on the
 *   air now outlined; a cell heard more than an hour ago faded. Under it, the heard log (the server's lines).
 * ★ The decoder font throughout (DECODER_FONT) — never the display font.
 */
import React, { useMemo } from 'react';
import { ScrollView, StyleSheet, Text, View, type StyleProp, type ViewStyle } from 'react-native';
import { DECODER_FONT, useDecoderStyles, useDecoderTokens, type DecoderTokens } from './DecoderShell';
import { scrollLane } from '../constants/popupTokens';
import { NCDXF_BANDS, ncdxfRows, ncdxfStepColour, type NcdxfReport } from '../services/ncdxf';

export interface NcdxfViewProps {
  report: NcdxfReport | null;
  log: string;
  bodySize: StyleProp<ViewStyle>;
  scrollRef?: React.RefObject<ScrollView | null>;
}

export default function NcdxfView({ report, log, bodySize, scrollRef }: NcdxfViewProps) {
  const tk = useDecoderTokens();
  const cs = useDecoderStyles(makeCs);
  const rows = useMemo(() => (report ? ncdxfRows(report) : []), [report]);
  const lines = useMemo(() => log.split('\n').filter((l) => l.trim()).slice(-12).reverse(), [log]);
  return (
    <ScrollView ref={scrollRef as any} style={[cs.body, bodySize]} contentContainerStyle={[cs.content, scrollLane]} showsVerticalScrollIndicator>
      {!report && <Text style={[cs.idle, { color: tk.muted }]}>waiting for the first ten-second slot…</Text>}
      {!!report && (
        <View>
          <View style={cs.row}>
            <Text style={[cs.call, { color: tk.label }]} />
            {NCDXF_BANDS.map((b, k) => (
              <Text key={b.hz} style={[cs.head, { color: report.band === k ? tk.accent : tk.label }]}>{b.label.replace(' ', '')}</Text>
            ))}
          </View>
          {rows.map((r) => (
            <View key={r.call} style={[cs.row, { borderTopColor: tk.divider }]}>
              <Text style={[cs.call, { color: tk.accent }]} numberOfLines={1}>{r.call}</Text>
              {r.cells.map((c, k) => (
                <Text key={k} accessibilityLabel={`${r.call}, ${NCDXF_BANDS[k].label}: ${c.title}`}
                      style={[cs.cell, c.steps ? { backgroundColor: ncdxfStepColour(c.steps), color: '#0b1014' } : { color: tk.muted },
                              c.faded && cs.faded, c.now && { borderColor: tk.accent, borderWidth: 1 }]}>{c.text}</Text>
              ))}
            </View>
          ))}
        </View>
      )}
      {lines.map((l, i) => <Text key={i} style={[cs.log, { color: tk.value }]} numberOfLines={2}>{l}</Text>)}
    </ScrollView>
  );
}

const makeCs = (_T: DecoderTokens) => StyleSheet.create({
  body:    {},
  content: { paddingHorizontal: 8, paddingVertical: 6 },
  idle:    { fontFamily: DECODER_FONT, fontSize: 11, padding: 10, textAlign: 'center' },
  row:     { flexDirection: 'row', alignItems: 'center', gap: 3, paddingVertical: 1, borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: 'transparent' },
  call:    { flex: 1, fontFamily: DECODER_FONT, fontSize: 12, fontWeight: '700', letterSpacing: 0.6 },
  head:    { width: 38, fontFamily: DECODER_FONT, fontSize: 9, fontWeight: '700', letterSpacing: 1, textAlign: 'center' },
  cell:    { width: 38, fontFamily: DECODER_FONT, fontSize: 11, fontWeight: '700', textAlign: 'center', borderRadius: 3,
             overflow: 'hidden', paddingVertical: 1, borderWidth: 0, borderColor: 'transparent', fontVariant: ['tabular-nums'] },
  faded:   { opacity: 0.45 },
  log:     { fontFamily: DECODER_FONT, fontSize: 11, letterSpacing: 0.3, marginTop: 4 },
});
