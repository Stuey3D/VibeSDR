/**
 * The DV ID box on a VibeServer (V12, docs/v12/DV-SIGNALLING.md) — DMR, D-STAR, System Fusion, NXDN and dPMR signalling,
 * NEVER THE VOICE, while the listener's mode is DV ID. The web client's #dvBox (web/client/src/main.ts dvBoxSync), drawn
 * from the same rule module (services/dvsig.ts).
 *
 *   header: ● DV ID · AUTO / DMR / D-STAR / YSF / NXDN / dPMR (the manual fallback) · MAP · EXIT
 *   note:   "70 cm amateur: callsigns and IDs are shown" — or, off the amateur bands, "who is talking is not shown"
 *   list:   newest first: time · system · CC/slot · who → whom · alias · text · position
 *
 * ★★★ WHAT IS SHOWN IS THE SERVER'S DECISION (UK WTA 2006 s.48): an identities copy only on an amateur band (or to the
 *     signed-in owner who opted in); this box draws what it was sent. MAP only for amateur positions (dvsigMapPoints).
 * ★ No dead controls: every key does something — a pin is sent to the server, the next report lights it.
 */
import React from 'react';
import { ScrollView, StyleSheet, Text, View, useWindowDimensions } from 'react-native';
import { DecoderShell, DecoderHeader, DecoderTitle, DecoderKey, DecoderBody, DECODER_FONT, decoderBodyInset,
         useDecoderTokens, useDecoderStyles, type DecoderTokens } from './DecoderShell';
import { scrollLane } from '../constants/popupTokens';
import { DV_PINS, dvRow, dvTime, dvsigMapPoints, type DvPin, type DvState } from '../services/dvsig';

const DV_MAX_W = 640;

export interface DvsigPanelProps {
  state: DvState | null;
  onPin: (p: DvPin) => void;
  onExit: () => void;
  /** Opens the map with the amateur positions (absent = no map here). */
  onMap?: () => void;
  bottomOffset: number;
  topLimit?: number;
}

export default function DvsigPanel(p: DvsigPanelProps) {
  const s = useDecoderStyles(makeStyles);
  const tk = useDecoderTokens();
  const { height: winH, width: winW } = useWindowDimensions();
  const HEADER_H = 46 + 40;
  const bodyH = Math.round(p.topLimit != null ? Math.max(90, winH - p.bottomOffset - p.topLimit - HEADER_H - decoderBodyInset(tk)) : 200);
  const st = p.state;
  const pin = st?.pin ?? 'auto';
  const note = st ? (st.amateur ? `${st.band}: callsigns and IDs are shown` : st.ids ? 'Not an amateur frequency: IDs shown to you, the owner, only'
                                 : 'Not an amateur frequency: who is talking is not shown') : 'Listening…';
  const pts = dvsigMapPoints(st);
  const live = !!st?.log.some((e) => e.open);
  const narrow = winW < 520;
  return (
    <DecoderShell bottom={p.bottomOffset} maxWidth={DV_MAX_W}>
      <DecoderHeader>
        <View style={[s.dot, live && s.dotOn]} />
        <DecoderTitle>DV ID</DecoderTitle>
        <View style={{ flex: 1 }} />
        {!!p.onMap && pts.length > 0 && <DecoderKey label="MAP" onPress={p.onMap} accessibilityLabel="Amateur stations that sent a position, on the map" />}
        <DecoderKey label="EXIT" onPress={p.onExit} accessibilityLabel="Leave DV ID and return to NFM" />
      </DecoderHeader>
      <View style={s.pins}>
        {DV_PINS.map((x) => (
          <DecoderKey key={x.id} active={pin === x.id} label={narrow && x.id === 'dstar' ? 'D★' : x.label}
                      onPress={() => { if (pin !== x.id) p.onPin(x.id); }}
                      accessibilityLabel={x.id === 'auto' ? 'Every system at once' : `Only ${x.label}`} />
        ))}
      </View>
      <Text style={s.status} numberOfLines={2}>{note}</Text>
      <DecoderBody>
        <ScrollView style={{ maxHeight: bodyH }} contentContainerStyle={scrollLane} showsVerticalScrollIndicator>
          {!st || !st.log.length ? (
            <Text style={s.note}>Nothing heard yet. DMR, D-STAR, System Fusion, NXDN and dPMR are listened for at once. The voice is never decoded.</Text>
          ) : st.log.slice(0, 40).map((e, i) => (
            <View key={e.id} style={[s.row, i % 2 === 0 && s.rowAlt]} accessibilityLabel={`${e.sys} ${dvRow(e)}`}>
              <Text style={s.ago} numberOfLines={1}>{dvTime(e.t)}</Text>
              <Text style={s.sys} numberOfLines={1}>{e.sys}</Text>
              <Text style={[s.txt, e.open && s.open]}>{dvRow(e)}</Text>
            </View>
          ))}
        </ScrollView>
      </DecoderBody>
    </DecoderShell>
  );
}

const F = DECODER_FONT;
const makeStyles = (T: DecoderTokens) => StyleSheet.create({
  dot:    { width: 8, height: 8, borderRadius: 4, backgroundColor: T.muted, opacity: 0.6 },
  dotOn:  { backgroundColor: T.good, opacity: 1 },
  pins:   { flexDirection: 'row', flexWrap: 'wrap', gap: 4, paddingHorizontal: 10, paddingBottom: 4 },
  status: { fontFamily: F, fontSize: 11, color: T.muted, paddingHorizontal: 12, paddingBottom: 4 },
  row:    { flexDirection: 'row', gap: 8, paddingHorizontal: 10, paddingVertical: 4 },
  rowAlt: { backgroundColor: T.rowActive },
  ago:    { width: 62, fontFamily: F, fontSize: 11, color: T.muted, fontVariant: ['tabular-nums'] },
  sys:    { width: 50, fontFamily: F, fontSize: 11, color: T.hdrValue },
  txt:    { flex: 1, minWidth: 0, fontFamily: F, fontSize: 12, lineHeight: 17, color: T.value },
  open:   { color: T.good },
  note:   { fontFamily: F, fontSize: 13, color: T.muted, padding: 12, lineHeight: 18 },
});
