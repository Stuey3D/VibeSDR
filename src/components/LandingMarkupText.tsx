/**
 * ★★★ THE OWNER'S LANDING MESSAGE WITH ITS **bold** AND *italic* (Stuart, 2026-11-12) — the app's half of
 * src/utils/landingMarkup.ts, which the web client and the directory read too. The text is split into spans and each
 * span is drawn as TEXT in a nested <Text>; nothing the owner types is ever interpreted, so there is nothing here to
 * escape and no way for a span to become a link or a tag.
 *
 *  ★★ iOS DRAWS NO STYLE A FAMILY DOES NOT HAVE. RCTFont looks for a face in the family that matches the weight and
 *     the slant and, finding only Atkinson's regular face, returns it unchanged — `fontWeight: 'bold'` and
 *     `fontStyle: 'italic'` are silently ignored. So on iOS bold uses the bundled bold face (registered in App.tsx as
 *     ATKINSON_BOLD) and italic uses the system font's real italic; Android synthesises both from the family.
 */
import React from 'react';
import { Platform, Text, type StyleProp, type TextStyle } from 'react-native';
import { landingMarkupSpans } from '../utils/landingMarkup';

/** The family name App.tsx registers AtkinsonHyperlegible-Bold.ttf under. */
export const ATKINSON_BOLD = 'Atkinson Hyperlegible Bold';

function spanStyle(b: boolean, i: boolean): TextStyle | undefined {
  if (!b && !i) return undefined;
  if (Platform.OS !== 'ios') return { fontWeight: b ? 'bold' : undefined, fontStyle: i ? 'italic' : undefined };
  if (i) return { fontFamily: 'System', fontStyle: 'italic', fontWeight: b ? 'bold' : undefined };
  return { fontFamily: ATKINSON_BOLD };
}

export default function LandingMarkupText({ text, style }: { text: string; style?: StyleProp<TextStyle> }) {
  const spans = landingMarkupSpans(text);
  return (
    <Text style={style}>
      {spans.map((sp, k) => {
        const st = spanStyle(sp.b, sp.i);
        return st ? <Text key={k} style={st}>{sp.text}</Text> : sp.text;
      })}
    </Text>
  );
}
