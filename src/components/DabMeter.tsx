/**
 * DabMeter — the DAB reception meter that stands in for the signal bar while DAB is on.
 *
 * ★★★ WHY (Stuart, 2026-10-06): a US listener sat on Coventry through the Pi 2 for an hour — station list
 *     loaded, never a sound — while the bar read S9. In DAB the bar measures power in the passband, which says
 *     nothing about whether the multiplex DECODES. This says that instead: three ascending bars, the verdict,
 *     what to expect to hear, and the error figure behind it when there is room —
 *         ▂▄▆  Multiplex weak · No or heavily broken audio · 14 % frames lost
 *     The verdict is src/utils/dabQuality.ts (shared with the web client); this file only draws it.
 *
 * ★★ IT TAKES THE SLOT OF THE BAR IT REPLACES, at that slot's height, so the deck does not move:
 *    • `line`    — the phone's thin meter under the frequency (portrait and landscape);
 *    • `housing` — the LED strip's / edgewise meter's black housing (two lines when it is tall enough);
 *    • `frame`   — the bottom of the tablet / Mac bar frame, under the centred pill.
 *
 * ★★ THE TYPE IS THE DISPLAY'S (fp.screen — faceplate.ts ScreenText, the one source for every big element, ★
 *    2026-10-06) in the mode box's reading colour, its nearest neighbour: Atkinson, Nixie One, Doto, or DSEG14
 *    cells on VCR (it was Atkinson there, beside a segment mode box), and on a lit display (dot / seg / Nixie) it
 *    glows like the rest of the glass. The bars are
 *    LinkBars' (the status row's link meter): the link colours on a plain display, the display's own colour
 *    with dim unlit segments on a VFD — one colour, as a VFD is.
 */
import React, { useState } from 'react';
import { View, type LayoutChangeEvent } from 'react-native';
import { useFaceplate } from '../contexts/FaceplateContext';
import { LED, NEON_TEXT, rgba, screenOneWeight } from '../constants/faceplate';
import { dabMeterType } from '../constants/meters';
import { screenString } from '../constants/displayText';
import SegLowerDText from './SegLowerDText';
import type { DabQuality } from '../utils/dabQuality';

export type DabMeterVariant = 'line' | 'housing' | 'frame';

export default function DabMeter({ q, height, variant, padH = 6 }: {
  q: DabQuality; height: number; variant: DabMeterVariant; padH?: number;
}) {
  const fp = useFaceplate();
  const dk = fp.deck;
  const ct = fp.chassis;
  const [w, setW] = useState(0);
  const screen = fp.screen;
  const lit = dk.style !== 'hyper';                      // dot / seg / nixie: a lit display
  const oneWeight = screen.oneWeight;                    // one-weight face: never bold
  // ★ DSEG14's cell is the whole em — scaled so its capitals stand as tall as the other faces' (ScreenText.sizeK).
  const k = screen.sizeK;
  // ── Sizes from the slot (meters.ts dabMeterType — ★ 2026-10-08: the two-line housing's pair now fills its height) ──
  // ★ 2026-10-07 (Stuart, Nixie on silver: "Clear audio … that line in this colour is difficult to read"): the
  //   advice line went from 82 % of the verdict's size to 90 % (DAB_TYPE.adviceK), and brighter (`dim` below).
  const { twoLines, font1, font2, lh1, lh2, barsH } = dabMeterType(height, variant);
  const barW = Math.max(2, Math.round(barsH * 0.24));
  const barGap = Math.max(1, Math.round(barW * 0.55));
  const barsW = 3 * barW + 2 * barGap;

  // ── Which parts fit (most important first: the verdict, then what you will hear, then the figure) ──
  // ★ The display font's character width (ScreenText.charEm) — enough to choose which parts fit before drawing.
  //   adjustsFontSizeToFit is the safety net under the estimate, never the plan (a shrunk sentence reads as a glitch).
  const cw = screen.charEm;
  const room = Math.max(0, w - 2 * padH - barsW - 6);
  const fits = (s: string, f: number) => s.length * f * k * cw <= room;
  const parts: string[] = [];
  let line2 = '';
  if (twoLines) {
    parts.push(q.label);
    const full = [q.advice, q.detail].filter(Boolean).join(' · ');
    line2 = !w || fits(full, font2) ? full : q.advice;
  } else {
    const all = [q.label, q.advice, q.detail].filter(Boolean) as string[];
    const noDetail = [q.label, q.advice];
    const withDetail = [q.label, q.detail].filter(Boolean) as string[];
    const pick = !w ? all
      : fits(all.join(' · '), font1) ? all
      : fits(noDetail.join(' · '), font1) ? noDetail
      // ★ A phone too narrow for the sentence: the verdict and its figure, or the short word and its figure.
      : fits(withDetail.join(' · '), font1) ? withDetail
      : [q.short, ...(q.detail ? [q.detail] : [])];
    parts.push(pick.join(' · '));
  }

  // ── Colours ──
  const levelColour = q.level === 3 ? ct.linkGood : q.level === 2 ? ct.linkFair : ct.linkBad;
  // ★ Nixie's glass is NEON whatever the text colour (§2: the rule outranks it) — the mode box's reading is.
  const nixie = dk.style === 'nixie';
  const litRgb = nixie ? LED.neon.rgb : dk.rgb;
  const barLit = lit ? (nixie ? NEON_TEXT.reading : dk.core) : levelColour;
  const barUnlit = lit ? rgba(litRgb, 0.28) : ct.linkUnlit;
  const glow = lit ? { textShadowColor: nixie ? NEON_TEXT.readingGlow : dk.glow, textShadowRadius: 4,
                       textShadowOffset: { width: 0, height: 0 } } : null;
  const txt = {
    color: dk.reading, fontFamily: screen.font, includeFontPadding: false,
    // ★ Doto / Nixie One / DSEG14 are ONE weight — asking for bold makes the platform substitute the system font.
    fontWeight: '700' as const, ...screenOneWeight(screen), letterSpacing: oneWeight ? 0.4 : 0.2,
    ...glow,
  };
  // ★ 2026-10-07: was .75 / .70. ★ 2026-10-08 — NIXIE: the reading's own hue a step down (NEON_TEXT.advice), not the
  //   neon base, whose deeper red blurred thin Nixie One strokes; VCR / DOT / Hyperlegible are as they were.
  const dim = nixie ? NEON_TEXT.advice : lit ? rgba(litRgb, 0.9) : 'rgba(255,255,255,0.86)';
  const sentence = [q.label, q.advice, q.detail].filter(Boolean).join('. ');

  return (
    <View
      onLayout={(e: LayoutChangeEvent) => { const nw = Math.round(e.nativeEvent.layout.width); setW(p => (p === nw ? p : nw)); }}
      accessible accessibilityRole="text" accessibilityLabel={`DAB reception: ${sentence}`}
      style={{
        height, alignSelf: 'stretch', flexDirection: 'row', alignItems: 'center', paddingHorizontal: padH, gap: 6,
        // ★ The track the line it replaces drew (SignalCanvas: ct.meterTrack); the housing and the frame have their own.
        ...(variant === 'line' ? { backgroundColor: ct.meterTrack, borderRadius: Math.min(7, height / 2) } : null),
        justifyContent: variant === 'line' ? 'center' : 'flex-start',
      }}>
      <View style={{ flexDirection: 'row', alignItems: 'flex-end', gap: barGap, height: barsH }}
            importantForAccessibility="no-hide-descendants">
        {[0, 1, 2].map(i => (
          <View key={i} style={{
            width: barW, height: Math.round(barsH * (0.45 + i * 0.275)), borderRadius: Math.min(1.5, barW / 2),
            backgroundColor: i < q.level ? barLit : barUnlit,
          }} />
        ))}
      </View>
      <View style={{ flex: 1, minWidth: 0, justifyContent: 'center' }} importantForAccessibility="no-hide-descendants">
        {/* ★ SegLowerDText (2026-10-06): on VCR "MER 12.3 dB" keeps a lower-case d (J + the centre bar). */}
        <SegLowerDText seg={screen.style === 'seg'} cells={screenString(screen.style, parts[0])}
              numberOfLines={1} adjustsFontSizeToFit minimumFontScale={0.8}
              style={[txt, { fontSize: font1 * k, lineHeight: lh1 }]} />
        {twoLines && !!line2 && (
          <SegLowerDText seg={screen.style === 'seg'} cells={screenString(screen.style, line2)}
                numberOfLines={1} adjustsFontSizeToFit minimumFontScale={0.8}
                style={[txt, { fontSize: font2 * k, lineHeight: lh2, color: dim, fontWeight: oneWeight ? 'normal' : '600' }]} />
        )}
      </View>
    </View>
  );
}
