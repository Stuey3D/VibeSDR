// TranscriptText.tsx — the TRANSCRIBE captions with every callsign heard as a QRZ.com link (V12, 2026-10-30). Stuart:
// "when using the transcription … detect a callsign and put a link to it".
//
// ★★ WHAT IS DRAWN: a call SPELLED OUT ("Golf Four Tango Papa Juliet", "Hotel Boston 9 Charlie Zebra Kilo") is REPLACED
//    in the caption by the call itself — G4TPJ, underlined, in the spots' link colour (CwView CallLink, the same tap →
//    qrz.com/db/<CALL> in the in-app browser). The words that were heard are kept as the link's accessibility label
//    ("G4TPJ, heard as Golf Four Tango Papa Juliet, on QRZ.com"), so a screen reader still says what was spoken; a call
//    written plainly (G4TPJ) is simply made a link where it stands.
// ★ TRANSLATED CAPTIONS ("   → …" under the source line): callsigns are found on the SOURCE line — a translator may
//    turn "Golf" into a word — and any the translation lost are added at its end ("· G4TPJ") so the link still shows.
// ★ CHEAP: the transcript is a growing string; each LINE is parsed once (a bounded cache keyed by the line) and lines
//    with no call stay joined in one plain string. Partials are never parsed — they live in the status line, not here.
import React, { useMemo } from 'react';
import { Text } from 'react-native';
import { transcriptPieces } from '../utils/spokenCallsign';
import { CallLink } from './CwView';

export default function TranscriptText({ text, linkStyle }: { text: string; linkStyle: any }) {
  const pieces = useMemo(() => transcriptPieces(text), [text]);
  return (
    <>
      {pieces.map((p, i) => p.call
        ? <CallLink key={i} inline call={p.call} style={linkStyle}
            label={p.spoken && p.spoken !== p.call ? `${p.call}, heard as ${p.spoken}, on QRZ.com` : undefined}>{p.text}</CallLink>
        : <Text key={i}>{p.text}</Text>)}
    </>
  );
}
