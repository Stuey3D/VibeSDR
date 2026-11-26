/**
 * SnrChip — beta96 (Stuart 11-26): a decoded message's SNR as a small filled chip at the end of its row — the number
 * always, the band colour behind it (src/utils/snrColour.ts: ONE scale, the colour is the margin over THAT mode's decode
 * threshold). The web draws the same from snrChipHtml. Nothing to press (no pad stop). Draws nothing without a number.
 * ★ Decoder-box text: the standard (Hyperlegible) decoder font, never the display font (feedback_display_font_everywhere).
 */
import React from 'react';
import { StyleSheet, Text } from 'react-native';
import { DECODER_FONT } from './DecoderShell';
import { snrChip, type SnrMode } from '../utils/snrColour';

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
  bare: { marginLeft: 0 },   // in a column of its own (the spot table)
});
