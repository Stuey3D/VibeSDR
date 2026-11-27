/**
 * The SONDES box on a VibeServer (V12, docs/v12/RS41.md) — RS41 radiosondes, a whole-radio mode, in the aircraft
 * datalink's box (AvdataPanel): over the lower waterfall, which stays live (the sondes' bursts are on it).
 *
 *   header: ● SONDES · the band (402–404 MHz) · the bands (the current one lit) · MAP · EXIT
 *   status: "3 sondes · listening on 4 signals · gain 43.4 dB auto"
 *   list:   newest first (the server's order): heard … ago · serial · height · climb — where heard · the temperature, or
 *           "not yet calibrated (n of 51)"
 *
 * ★★ THE SERVER OWNS THE BAND (the shared-dial contract): a band key sends the band, the next report says where the radio
 *    is. ★★ NEVER A GUESSED PTU (services/sondes.ts sondeTemp). ★ MAP opens MapOverlay 'sondes' (the sondes at their last
 *    position, a chosen one's track); CLOSE MAP comes back here. ★ No ✕: EXIT leaves the mode and the box together;
 *    after a refusal the key reads CLOSE. ★ The decoder font throughout (DecoderShell).
 */
import React, { useEffect, useState } from 'react';
import { ScrollView, StyleSheet, Text, View, useWindowDimensions } from 'react-native';
import { DecoderShell, DecoderHeader, DecoderTitle, DecoderKey, DecoderBody, DECODER_FONT, decoderBodyInset,
         useDecoderTokens, useDecoderStyles, engraveStyle, type DecoderTokens } from './DecoderShell';
import { scrollLane } from '../constants/popupTokens';
import { sondeAge, sondeDetail, sondeEmpty, sondeLine, sondeSnrMode, type SondeBand, type SondeReport } from '../services/sondes';
import { SnrChip } from './SnrChip';

const SD_MAX_W = 760;

export interface SondesPanelProps {
  report: SondeReport | null;
  /** The server's refusal (`sondes_error`), shown in the body. */
  error?: string;
  /** True while the receiver is in the mode; false after a refusal (the exit key then only closes). */
  on: boolean;
  onExit: () => void;
  /** ★★ V12 THE ONLY MODE: the note ("ADS-B only on this receiver") when this mode is the receiver's only one — EXIT is
   *  not drawn (CLOSE puts the box away; the mode carries on). */
  only?: string;
  /** The bands this radio runs (vibeserver.json sondesBands), in step order. */
  bands: SondeBand[];
  onPickBand: (id: string) => void;
  /** Opens the map (absent = no map here: Apple TV). */
  onMap?: () => void;
  bottomOffset: number;
  topLimit?: number;
}

export default function SondesPanel(p: SondesPanelProps) {
  const s = useDecoderStyles(makeStyles);
  const tk = useDecoderTokens();
  const { height: winH, width: winW } = useWindowDimensions();
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
  const band = p.error ? 'not available' : r ? r.bandName : 'finding the band…';
  const n = r?.sondes.length ?? 0;
  const status = r ? `${n} sonde${n === 1 ? '' : 's'} · listening on ${r.chans.length} signal${r.chans.length === 1 ? '' : 's'}`
    + (r.gain != null ? ` · gain ${r.gain.toFixed(1)} dB${r.gainAuto ? ' auto' : ''}` : '') + (r.dropped ? ' · fell behind' : '') : '';
  // ★ a narrow phone: the band keys drop their " MHz" (the box's title says it), never EXIT
  const short = winW < 520;

  return (
    <DecoderShell bottom={p.bottomOffset} maxWidth={SD_MAX_W}>
      <DecoderHeader>
        <View style={[s.dot, p.on && n > 0 && s.dotOn]} />
        <DecoderTitle>SONDES</DecoderTitle>
        {/* ★ The band name gives way, never the keys: EXIT must always be reachable (the DAB box's SE lesson). */}
        <View style={s.bandRow}>
          <Text style={s.band} numberOfLines={1}>{short && p.on && p.bands.length > 1 ? '' : band}</Text>
        </View>
        {p.on && p.bands.length > 1 && p.bands.map((b) => (
          <DecoderKey key={b.id} active={r?.band === b.id} label={short ? b.name.replace(' MHz', '') : b.name}
                      onPress={() => { if (r?.band !== b.id) p.onPickBand(b.id); }} accessibilityLabel={`Search ${b.name}`} />
        ))}
        {p.on && !!p.onMap && <DecoderKey label="MAP" onPress={p.onMap} accessibilityLabel="The sondes on a map" />}
        <DecoderKey onPress={p.onExit} label={p.on && !p.only ? 'EXIT' : 'CLOSE'}
                    accessibilityLabel={p.on && !p.only ? 'Leave the sondes mode — the receiver goes back to where it was' : 'Close'} />
      </DecoderHeader>
      {/* ★★ V12 THE ONLY MODE: no EXIT — say why once, so nobody hunts for it (CLOSE only puts the box away) */}
      {!!p.only && <Text style={{ color: '#ffffff', opacity: 0.75, fontSize: 11, paddingHorizontal: 10, paddingBottom: 4 }} accessibilityRole="text">{p.only}</Text>}
      {!!r && !p.error && <Text style={s.status} numberOfLines={1}>{status}</Text>}
      <DecoderBody>
        <ScrollView style={{ maxHeight: bodyH }} contentContainerStyle={scrollLane} showsVerticalScrollIndicator>
          {p.error ? (
            <Text style={s.note}>{p.error}</Text>
          ) : !r || !n ? (
            <Text style={s.note}>{sondeEmpty(r)}</Text>
          ) : r.sondes.map((x, i) => (
            <View key={x.serial} style={[s.row, i % 2 === 0 && s.rowAlt]}
                  accessibilityLabel={`${sondeLine(x)}, heard ${sondeAge(x.age + since)}, ${sondeDetail(x)}`}>
              <Text style={s.ago} numberOfLines={1}>{sondeAge(x.age + since).replace(' ago', '')}</Text>
              <View style={s.txtCol}>
                <Text style={[s.txt, x.climb != null && x.climb < -2 && s.falling]}>{sondeLine(x)}</Text>
                <Text style={s.detail} numberOfLines={2}>{sondeDetail(x)}</Text>
              </View>
              {/* ★ beta96 the last frame's SNR, colour-coded by its margin over this type's threshold */}
              {sondeSnrMode(x) ? <SnrChip mode={sondeSnrMode(x)!} db={x.snr} /> : null}
            </View>
          ))}
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
  falling: { color: T.warn },
  txtCol:  { flex: 1, minWidth: 0 },
  detail:  { fontFamily: F, fontSize: 11, lineHeight: 15, color: T.muted, marginTop: 1 },
  note:    { fontFamily: F, fontSize: 13, color: T.muted, padding: 12, lineHeight: 18 },
});
