/**
 * AircraftDataCredit — V12 beta91 the Mictronics ODC-By notice as one small line at the foot of an aircraft list (the
 * ADS-B map's AdsbLiveList, the aviation map's TrackLiveList). ODC-By 1.0 §4.3: the notice goes with the produced work
 * wherever the Mictronics fields show; the words and links are the ADS-B layer card's (adsbInfo.ts ADSB_DATA_CREDIT).
 * Links open by touch; a screen reader reads the whole line. Its height is AIRCRAFT_CREDIT_H — the list counts it.
 */
import React from 'react';
import { Linking, StyleSheet, Text } from 'react-native';
import { DECODER_FONT } from './DecoderShell';
import { ADSB_DATA_CREDIT } from '../services/adsbInfo';

export const AIRCRAFT_CREDIT_H = 26;   // ★ two lines of 9 px text

export default function AircraftDataCredit({ ink }: { ink: { x: string; k: string; kt: string } }) {
  const c = ADSB_DATA_CREDIT;
  const link = (t: string, u: string) => (
    <Text style={{ color: `rgba(${ink.kt},0.85)`, textDecorationLine: 'underline' }} accessibilityRole="link"
          onPress={() => { Linking.openURL(u).catch(() => {}); }}>{t}</Text>
  );
  return (
    <Text style={[st.line, { color: `rgba(${ink.x},0.55)`, borderTopColor: `rgba(${ink.k},0.18)` }]} numberOfLines={2}
          accessibilityLabel={`${c.lead}${c.mic}${c.mid}${c.odc}${c.tail}${c.vrs}${c.end}`}>
      {c.lead}{link(c.mic, c.micUrl)}{c.mid}{link(c.odc, c.odcUrl)}{c.tail}{link(c.vrs, c.vrsUrl)}{c.end}
    </Text>
  );
}

const st = StyleSheet.create({
  line: { height: AIRCRAFT_CREDIT_H, paddingHorizontal: 10, paddingTop: 2, fontFamily: DECODER_FONT, fontSize: 9, lineHeight: 11.5,
          borderTopWidth: StyleSheet.hairlineWidth },
});
