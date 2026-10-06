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
 * ★★ THE TYPE IS THE MODE BOX'S READING (fp.deck: modeFont, reading colour), its nearest neighbour — so it
 *    follows the display font the user picked (Doto on the dot-matrix display, Atkinson elsewhere) and the
 *    text colour, and on a lit display (dot / seg / Nixie) it glows like the rest of the glass. The bars are
 *    LinkBars' (the status row's link meter): the link colours on a plain display, the display's own colour
 *    with dim unlit segments on a VFD — one colour, as a VFD is.
 */
import React, { useState } from 'react';
import { Text, View, type LayoutChangeEvent } from 'react-native';
import { useFaceplate } from '../contexts/FaceplateContext';
import { FONT_DOTO, LED, NEON_TEXT, rgba } from '../constants/faceplate';
import type { DabQuality } from '../utils/dabQuality';

export type DabMeterVariant = 'line' | 'housing' | 'frame';

/** ★ Character width as a share of the font size — enough to choose which parts fit before drawing.
 *  Doto is a wide dot-matrix face; Atkinson Hyperlegible an ordinary proportional one. adjustsFontSizeToFit
 *  is the safety net under the estimate, never the plan (a shrunk sentence reads as a glitch). */
const CHAR_W = { doto: 0.66, hyper: 0.56 };

export default function DabMeter({ q, height, variant, padH = 6 }: {
  q: DabQuality; height: number; variant: DabMeterVariant; padH?: number;
}) {
  const fp = useFaceplate();
  const dk = fp.deck;
  const ct = fp.chassis;
  const [w, setW] = useState(0);
  const lit = dk.style !== 'hyper';                      // dot / seg / nixie: a lit display
  const dot = dk.modeFont === FONT_DOTO;
  const twoLines = variant === 'housing' && height >= 26;

  // ── Sizes from the slot ──
  const font1 = Math.max(7, Math.min(twoLines ? 11 : 10, Math.floor((twoLines ? height * 0.40 : height - 2))));
  const font2 = Math.max(7, Math.round(font1 * 0.82));
  const barsH = Math.max(5, Math.min(twoLines ? height - 10 : height - 2, Math.round(font1 * 1.3)));
  const barW = Math.max(2, Math.round(barsH * 0.24));
  const barGap = Math.max(1, Math.round(barW * 0.55));
  const barsW = 3 * barW + 2 * barGap;

  // ── Which parts fit (most important first: the verdict, then what you will hear, then the figure) ──
  const cw = (dot ? CHAR_W.doto : CHAR_W.hyper);
  const room = Math.max(0, w - 2 * padH - barsW - 6);
  const fits = (s: string, f: number) => s.length * f * cw <= room;
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
    color: dk.reading, fontFamily: dk.modeFont, includeFontPadding: false,
    // ★ Doto is ONE weight (the Black cut is the file) — asking it for bold falls back to the system font.
    fontWeight: dot ? 'normal' as const : '700' as const, letterSpacing: dot ? 0.4 : 0.2,
    ...glow,
  };
  const dim = lit ? rgba(litRgb, 0.75) : 'rgba(255,255,255,0.70)';
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
        <Text numberOfLines={1} adjustsFontSizeToFit minimumFontScale={0.8}
              style={[txt, { fontSize: font1, lineHeight: Math.round(font1 * 1.2) }]}>
          {parts[0]}
        </Text>
        {twoLines && !!line2 && (
          <Text numberOfLines={1} adjustsFontSizeToFit minimumFontScale={0.8}
                style={[txt, { fontSize: font2, lineHeight: Math.round(font2 * 1.2), color: dim, fontWeight: dot ? 'normal' : '600' }]}>
            {line2}
          </Text>
        )}
      </View>
    </View>
  );
}
