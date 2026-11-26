/**
 * BalloonView — V12 high-altitude balloons inside the decoder box (DecoderPanel): the web client's list (main.ts
 * renderBalloon), from the same pure words (services/balloons.ts).
 *
 *   one row per payload heard, newest first — callsign, position, height and what else it sent, when, and the sentence
 *   itself (a Horus packet written as the UKHAS sentence it stands for) — then the SSDV pictures, each drawn from the JPEG
 *   the server has built so far, so it fills in as its packets arrive; under them, the report log.
 * ★ The decoder font throughout (DECODER_FONT) — never the display font.
 */
import React, { useMemo } from 'react';
import { Image, ScrollView, StyleSheet, Text, View, type StyleProp, type ViewStyle } from 'react-native';
import { DECODER_FONT, useDecoderStyles, useDecoderTokens, type DecoderTokens } from './DecoderShell';
import { scrollLane } from '../constants/popupTokens';
import { balloonLine, balloonPictureText, balloonStatus, balloonWhen, type BalloonReport } from '../services/balloons';

/** A picture as the screen keeps it: the JPEG as a data URI, and its size. */
export interface BalloonPic { key: string; uri: string; w: number; h: number; text: string }

export interface BalloonViewProps {
  report: BalloonReport | null;
  pictures: BalloonPic[];
  log: string;
  bodySize: StyleProp<ViewStyle>;
  scrollRef?: React.RefObject<ScrollView | null>;
  /** ★★ beta96 iPhone Duo half-open BIG: 'upper' = the payloads heard, 'lower' = the pictures and the report log. */
  part?: 'upper' | 'lower';
}

export default function BalloonView({ report, pictures, log, bodySize, scrollRef, part }: BalloonViewProps) {
  const tk = useDecoderTokens();
  const cs = useDecoderStyles(makeCs);
  const lines = useMemo(() => log.split('\n').filter((l) => l.trim()).slice(-8).reverse(), [log]);
  return (
    <ScrollView ref={scrollRef as any} style={[cs.body, bodySize]} contentContainerStyle={[cs.content, scrollLane]} showsVerticalScrollIndicator>
      {part !== 'lower' && (!report || !report.payloads.length) && <Text style={[cs.idle, { color: tk.muted }]}>{balloonStatus(report)}</Text>}
      {part !== 'lower' && report?.payloads.map((p) => (
        <View key={p.call} style={[cs.row, { borderTopColor: tk.divider }]} accessibilityLabel={`${p.call}: ${balloonLine(p)}`}>
          <Text style={[cs.call, { color: tk.accent }]} numberOfLines={1}>{p.call}</Text>
          <Text style={[cs.nums, { color: tk.value }]}>{balloonLine(p)}</Text>
          <Text style={[cs.when, { color: tk.muted }]}>{balloonWhen(p, report.now)}</Text>
          {!!p.sentence && <Text style={[cs.sent, { color: tk.muted }]} numberOfLines={2}>{p.sentence}</Text>}
        </View>
      ))}
      {part === 'lower' && !pictures.length && !lines.length && (
        <Text style={[cs.idle, { color: tk.muted }]}>SSDV pictures and the report log appear here</Text>
      )}
      {part !== 'upper' && pictures.map((pic) => {
        const info = report?.images.find((i) => i.key === pic.key);
        return (
          <View key={pic.key} style={cs.pic}>
            <Image source={{ uri: pic.uri }} style={[cs.img, { aspectRatio: pic.w > 0 && pic.h > 0 ? pic.w / pic.h : 4 / 3 }]} resizeMode="contain"
                   accessibilityLabel={`SSDV picture ${pic.key}`} />
            <Text style={[cs.when, { color: tk.muted }]}>{info ? balloonPictureText(info) : pic.text}</Text>
          </View>
        );
      })}
      {part !== 'upper' && lines.map((l, i) => <Text key={i} style={[cs.log, { color: tk.value }]} numberOfLines={2}>{l}</Text>)}
    </ScrollView>
  );
}

const makeCs = (_T: DecoderTokens) => StyleSheet.create({
  body:    {},
  content: { paddingHorizontal: 8, paddingVertical: 6 },
  idle:    { fontFamily: DECODER_FONT, fontSize: 11, padding: 10, textAlign: 'center' },
  row:     { paddingVertical: 4, borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: 'transparent' },
  call:    { fontFamily: DECODER_FONT, fontSize: 13, fontWeight: '700', letterSpacing: 0.8 },
  nums:    { fontFamily: DECODER_FONT, fontSize: 12, fontVariant: ['tabular-nums'] },
  when:    { fontFamily: DECODER_FONT, fontSize: 10, marginTop: 1 },
  sent:    { fontFamily: DECODER_FONT, fontSize: 9, opacity: 0.8, marginTop: 1 },
  pic:     { marginTop: 8 },
  img:     { width: '100%', maxWidth: 480, backgroundColor: '#000', borderRadius: 3 },
  log:     { fontFamily: DECODER_FONT, fontSize: 11, letterSpacing: 0.3, marginTop: 4 },
});
