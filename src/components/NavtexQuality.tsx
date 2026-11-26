/**
 * NavtexQuality — beta96 (Stuart 11-26): a NAVTEX message's decode quality and SNR, drawn in the app the way the web
 * client draws them (web/client/src/navtexSnrDom.ts). The rules — colours, ranges, worst slice per line — are
 * src/utils/navtexQuality.ts, shared, so the two say the same thing. DISPLAY ONLY, nothing to press (no pad stop).
 *
 *  · SnrTimeline     — the thin HORIZONTAL timeline (gallery rows, the live status): a segment per slice.
 *  · QualityBar      — the small 3-colour clean / FEC / lost bar.
 *  · NavtexQualityRow — both, with their lines ("Decode quality: …", "SNR 8 dB avg · 22 % weak").
 *  · SnrStripText    — the message text with a 4 px strip + 6 px gap down its LEFT: each RENDERED line coloured by the
 *    worst slice it was decoded in. The lines come from onTextLayout at the width the text has NOW, so the strip
 *    follows re-wrapping (rotation, split screen, a fold).
 */
import React, { useMemo, useState } from 'react';
import { StyleSheet, Text, View, type NativeSyntheticEvent, type StyleProp, type TextLayoutEventData, type TextStyle } from 'react-native';
import { lineRangesOf, lineWorst, navtexQualityDetail, navtexQualityLine, navtexSnrLine, navtexSplitPct, snrBand, snrColour, SNR_COLOURS,
         type NavtexSnr, type NavtexSplit } from '../utils/navtexQuality';

export function SnrTimeline({ snr, height = 4, maxWidth }: { snr?: NavtexSnr | null; height?: number; maxWidth?: number }) {
  if (!snr || !snr.tl.length) return null;
  const red = snr.tl.filter((d) => snrBand(d) === 'red').length;
  return (
    <View accessible accessibilityRole="image" style={[st.tl, { height, maxWidth }]}
          accessibilityLabel={`SNR timeline: ${red} of ${snr.tl.length} segments under 5 dB`}>
      {snr.tl.map((db, i) => <View key={i} style={{ flex: 1, backgroundColor: snrColour(db) }} />)}
    </View>
  );
}
export function QualityBar({ split }: { split?: NavtexSplit | null }) {
  const p = navtexSplitPct(split);
  if (!p) return null;
  return (
    <View accessible accessibilityRole="image" style={st.qb}
          accessibilityLabel={`Decode quality: ${p.clean.toFixed(1)} % clean, ${p.fec.toFixed(1)} % FEC, ${p.lost.toFixed(1)} % lost`}>
      {p.clean > 0 && <View style={{ flex: p.clean, backgroundColor: SNR_COLOURS.green }} />}
      {p.fec > 0 && <View style={{ flex: p.fec, backgroundColor: SNR_COLOURS.amber }} />}
      {p.lost > 0 && <View style={{ flex: p.lost, backgroundColor: SNR_COLOURS.red }} />}
    </View>
  );
}
/** The quality bar + line and the SNR timeline + line — each only when measured; nothing at all for an older server. */
export function NavtexQualityRow({ split, snr, color, fontFamily, words = true }:
  { split?: NavtexSplit | null; snr?: NavtexSnr | null; color: string; fontFamily?: string; words?: boolean }) {
  const ql = navtexQualityLine(split), sl = navtexSnrLine(snr);
  if (!ql && !sl) return null;
  return (
    <View style={st.row}>
      {!!ql && <QualityBar split={split} />}
      {!!ql && words && <Text style={[st.words, { color, fontFamily }]}>{`${ql} · ${navtexQualityDetail(split)}`}</Text>}
      {!!sl && <View style={st.tlWrap}><SnrTimeline snr={snr} /></View>}
      {!!sl && words && <Text style={[st.words, { color: snrColour(snr!.avg), fontFamily }]}>{sl}</Text>}
    </View>
  );
}

/** ★ The text with its SNR strip. `ranges` are the slices as character ranges of `text` (snrRanges); null/empty → the
 *  plain text, no gutter. */
export function SnrStripText({ text, ranges, style, selectable }:
  { text: string; ranges: { start: number; end: number; db: number }[] | null; style: StyleProp<TextStyle>; selectable?: boolean }) {
  const [lines, setLines] = useState<{ y: number; h: number; start: number; end: number }[]>([]);
  const on = !!ranges && ranges.length > 0;
  const onLayout = (e: NativeSyntheticEvent<TextLayoutEventData>) => {
    if (!on) return;
    // the rendered lines' texts, in order, are the source cut where it wrapped (navtexQuality.ts lineRangesOf)
    const r = lineRangesOf(text, e.nativeEvent.lines);
    const out = e.nativeEvent.lines.map((l, i) => ({ y: l.y, h: l.height, start: r[i].start, end: r[i].end }));
    setLines((prev) => (prev.length === out.length && prev.every((p, i) => p.y === out[i].y && p.start === out[i].start && p.end === out[i].end) ? prev : out));
  };
  const worst = useMemo(() => (on ? lineWorst(lines, ranges!) : []), [on, lines, ranges]);
  if (!on) return <Text selectable={selectable} style={style}>{text}</Text>;
  return (
    <View style={st.stripWrap}>
      <View style={st.strip} pointerEvents="none">
        {lines.map((l, i) => (worst[i] == null ? null
          : <View key={i} style={{ position: 'absolute', left: 0, width: 4, top: l.y, height: Math.max(1, l.h), backgroundColor: snrColour(worst[i]!) }} />))}
      </View>
      <Text selectable={selectable} style={style} onTextLayout={onLayout}>{text}</Text>
    </View>
  );
}

const st = StyleSheet.create({
  row: { flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', columnGap: 8, rowGap: 3, marginBottom: 6 },
  words: { fontSize: 10, letterSpacing: 0.4 },
  qb: { flexDirection: 'row', width: 64, height: 6, borderRadius: 2, overflow: 'hidden', backgroundColor: 'rgba(127,127,127,0.25)' },
  tl: { flexDirection: 'row', borderRadius: 2, overflow: 'hidden', backgroundColor: 'rgba(127,127,127,0.25)', alignSelf: 'stretch' },
  tlWrap: { flexGrow: 1, flexBasis: 100, minWidth: 60, maxWidth: 220 },
  stripWrap: { position: 'relative', paddingLeft: 10 },
  strip: { position: 'absolute', left: 0, top: 0, bottom: 0, width: 4 },
});
