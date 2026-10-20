/**
 * FreqDigit — one character drawn in the MAIN DISPLAY's own style, for the controller keyboard's numeric keycaps
 * (brief "Keycap fonts": the numeric pad's large keys show digits exactly as the frequency readout does).
 *
 *   nixie  → a tube (NixieKeyDigit — the readout's own sprites; always orange), `.` = the INS-1 bead
 *   seg    → a drawn 7-segment cell (SegDigits), `.` = its DP electrode
 *   dot    → Doto over the ghost-dot grid, as the readout
 *   hyper  → Atkinson Hyperlegible text
 *
 * ★ The readout itself is still drawn by ControlsBar's own switch; extracting it there too (so the two can never
 *   drift) is left for a device check, because that window's sizes were tuned on device — the pieces used here are
 *   the readout's own components, so the glyphs already match.
 * ★ The brief's "Hyperlegible keycaps" override: `plain` draws Atkinson whatever the display.
 */
import React from 'react';
import { Text, View } from 'react-native';
import { useFaceplate } from '../contexts/FaceplateContext';
import { NixieKeyDigit } from './NixieTubes';
import { SegDigits, GhostGrid } from './VfdParts';

export function FreqDigit({ ch, w, h, plain }: { ch: string; w: number; h: number; plain?: boolean }) {
  const dk = useFaceplate().deck;
  const style = plain ? 'hyper' : dk.style;
  if (style === 'nixie' && /^[\d.]$/.test(ch)) {
    const th = Math.round(h * 0.86), tw = Math.round(Math.min(w * 0.7, th * 0.55));
    return <NixieKeyDigit d={ch} w={tw} h={th} />;
  }
  if (style === 'seg' && /^[\d.]$/.test(ch)) {
    return <SegDigits text={ch} rgb={dk.rgb} core={dk.core} glow={dk.glow} designH={Math.round(h * 0.62)}
                      style={{ width: w, height: h }} align="center" />;
  }
  if (style === 'dot') {
    return (
      <View style={{ width: w, height: h, alignItems: 'center', justifyContent: 'center' }} pointerEvents="none">
        <GhostGrid rgb={dk.rgb} pitch={3.4} dot={0.8} />
        <Text style={{ color: dk.freq, fontFamily: dk.freqFont, fontSize: Math.round(h * 0.58), textShadowColor: dk.freqGlow,
                       textShadowRadius: 5, includeFontPadding: false }}>{ch}</Text>
      </View>
    );
  }
  return (
    <Text style={{ color: plain ? '#f2efe6' : dk.freq, fontFamily: 'Atkinson Hyperlegible', fontSize: Math.round(h * 0.5),
                   includeFontPadding: false }}>{ch}</Text>
  );
}
