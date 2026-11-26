/**
 * SnrChip — beta96 (Stuart 11-26): a decoded message's SNR as a small filled chip at the end of its row — the number
 * always, the band colour behind it (src/utils/snrColour.ts: ONE scale, the colour is the margin over THAT mode's decode
 * threshold). The web draws the same from snrChipHtml. Nothing to press (no pad stop). Draws nothing without a number.
 * ★ Decoder-box text: the standard (Hyperlegible) decoder font, never the display font (feedback_display_font_everywhere).
 */
import React from 'react';
import { StyleSheet, Text, type StyleProp, type TextStyle } from 'react-native';
import { DECODER_FONT } from './DecoderShell';
import { snrChip, snrTokenParts, SNR_LOG_MAX_CHARS, SNR_LOG_MODE, type SnrMode } from '../utils/snrColour';

export function SnrChip({ mode, db, small, bare }: { mode: SnrMode; db: number | null | undefined; small?: boolean; bare?: boolean }) {
  const c = snrChip(mode, db);
  if (!c) return null;
  return (
    <Text accessible accessibilityRole="text" accessibilityLabel={c.title} numberOfLines={1}
          style={[st.chip, small && st.small, bare && st.bare, { backgroundColor: c.bg, color: c.fg }]}>{bare ? c.text.replace(/ dB$/, '') : c.text}</Text>
  );
}

const st = StyleSheet.create({
  chip: { fontFamily: DECODER_FONT, fontSize: 10, lineHeight: 13, fontWeight: '700', paddingHorizontal: 4, borderRadius: 3,
          overflow: 'hidden', marginLeft: 5, flexShrink: 0, fontVariant: ['tabular-nums'], alignSelf: 'center' },
  small: { fontSize: 9, lineHeight: 12, paddingHorizontal: 3 },
  bare: { marginLeft: 0 },
  inline: { fontWeight: '700' },   // inside a log's text: its font and size, the chip's colours   // in a column of its own (the spot table)
});

/** ★ beta96 a day log's text with its kept SNRs (" · SNR 18 dB" at a line's end) as inline chips — AIS, APRS, ISM,
 *  ORBCOMM (src/utils/snrColour.ts SNR_LOG_MODE); anything else, a translation, or a log over SNR_LOG_MAX_CHARS: plain. */
export function SnrLogText({ text, kind, style, selectable }: { text: string; kind?: string; style: StyleProp<TextStyle>; selectable?: boolean }) {
  const mode = kind ? SNR_LOG_MODE[kind] : undefined;
  if (!mode || text.length > SNR_LOG_MAX_CHARS || !text.includes(' · SNR ')) return <Text selectable={selectable} style={style}>{text}</Text>;
  return (
    <Text selectable={selectable} style={style}>
      {snrTokenParts(text).map((p, i) => {
        if (typeof p === 'string') return p;
        const c = snrChip(mode, p.db);
        return c ? <Text key={i} accessibilityLabel={c.title} style={[st.inline, { backgroundColor: c.bg, color: c.fg }]}>{` ${c.text} `}</Text> : null;
      })}
    </Text>
  );
}
