/**
 * The AIRCRAFT DATALINK box on a VibeServer (V12, docs/v12/AVIATION.md) — ACARS and VDL2, a whole-radio mode, in ISM's
 * box (IsmPanel): over the lower waterfall, which stays live (the bursts on it are half the point).
 *
 *   header: ● DATALINK · the band (VDL2 136.8625 MHz) · ACARS / VDL2 (the two bands, the current one lit) · MAP · EXIT
 *   status: "5 aircraft · 33 messages · gain 43.4 dB auto"
 *   list:   newest first (the server's order): heard … ago · who (flight · registration · ICAO address) — what was last
 *           heard (the label's plain name); under it the route and last OUT / OFF / ON / IN, the position's altitude and
 *           fix time (when the server could read one — services/aviation.ts avDetail), and how it was heard (VDL2 136.975)
 *   ★ MAP (V12 2026-11-16) opens MapOverlay 'avdata': the aircraft that reported a position; CLOSE MAP comes back here.
 *
 * ★★ THE SERVER OWNS THE BAND (the shared-dial contract): a band key sends the band, the next report says where the radio
 *    is. ★★ MESSAGE TEXT appears only where the server sent it — the owner's opt-in, to the owner's own session; this
 *    box never asks for it. ★ No ✕ (the ISM box's reason): EXIT leaves the mode and the box together; after a refusal the
 *    key reads CLOSE. ★ The decoder font throughout (DecoderShell). Pure logic: services/aviation.ts.
 */
import React, { useEffect, useState } from 'react';
import { ScrollView, StyleSheet, Text, View, useWindowDimensions } from 'react-native';
import { DecoderShell, DecoderHeader, DecoderTitle, DecoderKey, DecoderBody, DECODER_FONT, decoderBodyInset,
         useDecoderTokens, useDecoderStyles, engraveStyle, type DecoderTokens } from './DecoderShell';
import { scrollLane } from '../constants/popupTokens';
import { avAge, avAppsText, avDetail, avLine, type AvBand, type AvReport } from '../services/aviation';

const AV_MAX_W = 760;

export interface AvdataPanelProps {
  report: AvReport | null;
  /** The server's refusal (`avdata_error`), shown in the body. */
  error?: string;
  /** True while the receiver is in the mode; false after a refusal (the exit key then only closes). */
  on: boolean;
  onExit: () => void;
  /** The bands this radio reaches (vibeserver.json avdataBands), in step order. */
  bands: AvBand[];
  onPickBand: (id: string) => void;
  /** ★ The aircraft with a position on a map (MapOverlay 'avdata'); absent on Apple TV (no map there). */
  onMap?: () => void;
  bottomOffset: number;
  topLimit?: number;
}

export default function AvdataPanel(p: AvdataPanelProps) {
  const s = useDecoderStyles(makeStyles);
  const tk = useDecoderTokens();
  const { height: winH } = useWindowDimensions();
  const HEADER_H = 46 + 22;
  const bodyH = Math.round(p.topLimit != null
    ? Math.max(90, winH - p.bottomOffset - p.topLimit - HEADER_H - decoderBodyInset(tk)) : 200);
  // ★ between reports (one a second) the ages tick on here
  const [arrived, setArrived] = useState(0);
  const [, setTick] = useState(0);
  useEffect(() => { setArrived(Date.now()); }, [p.report]);
  useEffect(() => { const t = setInterval(() => setTick((n) => n + 1), 1000); return () => clearInterval(t); }, []);
  const since = arrived ? Math.floor((Date.now() - arrived) / 1000) : 0;
  const r = p.report;
  const band = p.error ? 'not available' : r ? `${r.bandName} ${(r.hz / 1e6).toFixed(4).replace(/0+$/, '')} MHz` : 'finding the band…';
  const status = r ? `${r.aircraft.length} aircraft · ${r.msgs} message${r.msgs === 1 ? '' : 's'}`
    + (avAppsText(r) ? ` · ${avAppsText(r)}` : '')
    + (r.gain != null ? ` · gain ${r.gain.toFixed(1)} dB${r.gainAuto ? ' auto' : ''}` : '') + (r.dropped ? ' · fell behind' : '') : '';

  return (
    <DecoderShell bottom={p.bottomOffset} maxWidth={AV_MAX_W}>
      <DecoderHeader>
        <View style={[s.dot, p.on && !!r?.aircraft.length && s.dotOn]} />
        <DecoderTitle>DATALINK</DecoderTitle>
        {/* ★ The band name gives way, never the keys: EXIT must always be reachable (the DAB box's SE lesson). */}
        <View style={s.bandRow}>
          <Text style={s.band} numberOfLines={1}>{band}</Text>
        </View>
        {p.on && p.bands.length > 1 && p.bands.map((b) => (
          <DecoderKey key={b.id} active={r?.band === b.id} label={b.name} onPress={() => { if (r?.band !== b.id) p.onPickBand(b.id); }}
                      accessibilityLabel={`${b.name}: ${b.region}`} />
        ))}
        {p.on && !!p.onMap && <DecoderKey label="MAP" onPress={p.onMap} accessibilityLabel="The aircraft that reported a position, on a map" />}
        <DecoderKey onPress={p.onExit} label={p.on ? 'EXIT' : 'CLOSE'}
                    accessibilityLabel={p.on ? 'Leave the aircraft datalink — the receiver goes back to where it was' : 'Close'} />
      </DecoderHeader>
      {!!r && !p.error && <Text style={s.status} numberOfLines={1}>{status}</Text>}
      <DecoderBody>
        <ScrollView style={{ maxHeight: bodyH }} contentContainerStyle={scrollLane} showsVerticalScrollIndicator>
          {p.error ? (
            <Text style={s.note}>{p.error}</Text>
          ) : !r ? (
            <Text style={s.note}>Taking the receiver to the aircraft datalink…</Text>
          ) : r.aircraft.length ? r.aircraft.map((a, i) => (
            <View key={(a.icao || a.reg || a.flight || '') + i} style={[s.row, i % 2 === 0 && s.rowAlt]}
                  accessibilityLabel={`${avLine(a)}, heard ${avAge(a.age + since)}, ${avDetail(a)}`}>
              <Text style={s.ago} numberOfLines={1}>{avAge(a.age + since).replace(' ago', '')}</Text>
              <View style={s.txtCol}>
                <Text style={s.txt}>{avLine(a)}</Text>
                <Text style={s.detail} numberOfLines={2}>{avDetail(a)}</Text>
                {!!a.text && <Text style={s.body}>{a.text}</Text>}
              </View>
            </View>
          )) : (
            <Text style={s.note}>
              Listening on every {r.bandName} channel. Aircraft appear here as they are heard — ACARS and VDL2 are bursts, so
              a quiet minute is normal; a receiver near an airport or under a flight path hears the most.
            </Text>
          )}
        </ScrollView>
      </DecoderBody>
    </DecoderShell>
  );
}

const F = DECODER_FONT;
const makeStyles = (T: DecoderTokens) => StyleSheet.create({
  dot:     { width: 8, height: 8, borderRadius: 4, backgroundColor: T.muted, opacity: 0.6 },
  dotOn:   { backgroundColor: T.good, opacity: 1 },
  bandRow: { flex: 1, flexShrink: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 4 },
  band:    { flexShrink: 1, minWidth: 0, fontFamily: F, fontSize: 13, letterSpacing: 1, color: T.hdrValue, textAlign: 'center',
             ...engraveStyle(T) },
  status:  { fontFamily: F, fontSize: 11, color: T.muted, paddingHorizontal: 12, paddingBottom: 4 },
  row:     { flexDirection: 'row', gap: 10, paddingHorizontal: 10, paddingVertical: 4 },
  rowAlt:  { backgroundColor: T.rowActive },
  ago:     { width: 54, textAlign: 'right', fontFamily: F, fontSize: 12, color: T.muted, fontVariant: ['tabular-nums'] },
  txt:     { fontFamily: F, fontSize: 12, lineHeight: 17, color: T.value },
  txtCol:  { flex: 1, minWidth: 0 },
  detail:  { fontFamily: F, fontSize: 11, lineHeight: 15, color: T.muted, marginTop: 1 },
  body:    { fontFamily: F, fontSize: 11, lineHeight: 15, color: T.value, marginTop: 2 },
  note:    { fontFamily: F, fontSize: 13, color: T.muted, padding: 12, lineHeight: 18 },
});
