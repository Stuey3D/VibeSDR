/**
 * DrmPanel — V12 DRM in the app (docs/v12/DRM.md): the box a listener reads while their mode is DRM, the web client's
 * #drmBox (web/client/src/drm.ts drmRender). The server receives DRM (vibe_drm_listener.h) and reports about once per
 * 1.2 s super frame; this only draws the report — services/drm.ts drmView says what, in the web's words.
 *
 *   the station — its label (and service id), programme · language · flag country, its text message
 *   six lights in the order a receiver gains them: Time · Freq · Frame · FAC · SDC · MSC
 *   the SNR (green / amber / red) and one plain line of what the signal is
 *   the Audio light and the codec ("via FAAD2" where the server says which decoder plays it)
 *   the server's own sentence, prominently, when it cannot play the codec
 *
 * ★★ V12 THE SIGNAL PANE (2026-10-26): DAB's kind of advanced pane as a second view — LOCK, SIGNAL, QUALITY (delay
 *    spread and Doppler among them), ERRORS, AUDIO, STATION — services/drm.ts drmSignalRows, the web's very rows. The
 *    compact box stays the default; SIGNAL in the header swaps to the pane and says STATION to come back (it names where
 *    it GOES — DabPanel's rule for its STATIONS/SIGNAL key).
 * ★ Over the lower waterfall where the decoder box sits (it stands down while a decoder or the spots are open). Tap the
 *   header to fold it to its title line; the waterfall stays live above it. The decoder font throughout (DecoderShell).
 * ★ BY PAD AND KEYBOARD (IsmPanel's pattern): the screen's highlight can land on the box ('drmBox'); Enter takes the keys,
 *   the arrows walk SIGNAL · MIN and then, in the Signal pane, every row (each brought into view, so the whole pane can be
 *   read without a touch), Back hands the keys back. It does NOT take the keys by itself as DAB's box does: in DRM the
 *   dial is still the radio's, and the box is something to read.
 * ★ No dead controls: no key here sets anything on the radio — DRM has nothing to set (the server cuts its own channel,
 *   so the width control is not offered in DRM either).
 */
import React, { useEffect, useMemo, useRef, useState } from 'react';
import { ScrollView, StyleSheet, Text, View, useWindowDimensions } from 'react-native';
import { DecoderShell, DecoderHeader, DecoderTitle, DecoderKey, DecoderBody, DECODER_FONT, decoderBodyInset,
         useDecoderTokens, useDecoderStyles, type DecoderTokens } from './DecoderShell';
import { scrollLane } from '../constants/popupTokens';
import { captureRegion, capturedRegion, useListNav, useNavFocus } from './PanelNav';
import { screenNavRef } from './ScreenNav';
import { drmView, drmViewText, drmSignalRows, DRM_LIGHT_ON, DRM_LIGHT_OFF, DRM_SNR_COLOURS,
         type DrmState, type DrmTone } from '../services/drm';

/** Wide enough for the signal line on one row on a tablet; a phone's window narrows it (DecoderShell). */
const DRM_MAX_W = 560;

export interface DrmPanelProps {
  state: DrmState | null;
  bottomOffset: number;
  /** ★ The highest the box may reach (the screen's boxTopLimit, as the DAB and ISM boxes get it) — sizes the Signal pane. */
  topLimit?: number;
}

type NavKey = { key: string; row: number };

export default function DrmPanel({ state, bottomOffset, topLimit }: DrmPanelProps) {
  const tk = useDecoderTokens();
  const s = useDecoderStyles(makeStyles);
  const { height: winH } = useWindowDimensions();
  const [min, setMin] = useState(false);
  const [pane, setPane] = useState<'station' | 'signal'>('station');
  const v = useMemo(() => drmView(state), [state]);
  const rows = useMemo(() => (pane === 'signal' ? drmSignalRows(state) : []), [state, pane]);
  const playing = !!state?.msc && !!state?.playable;
  const sig = pane === 'signal' && !min;
  const togglePane = () => { setMin(false); setPane((p) => (p === 'signal' ? 'station' : 'signal')); };

  // ★ The Signal pane's height: up to the screen's limit (the DAB/ISM boxes' arithmetic), else a fixed 200 like ISM's
  const HEADER_H = 46;
  const bodyH = Math.round(topLimit != null
    ? Math.max(120, winH - bottomOffset - topLimit - HEADER_H - decoderBodyInset(tk)) : 200);

  // ── pad / keyboard ──
  const ring = useNavFocus().ring;
  const [grab, setGrab] = useState(false);
  useEffect(() => { if (grab) captureRegion('drm'); else if (capturedRegion() === 'drm') captureRegion(null); }, [grab]);
  useEffect(() => () => { if (capturedRegion() === 'drm') captureRegion(null); }, []);
  const keys: NavKey[] = [{ key: 'pane', row: 0 }, { key: 'min', row: 0 }];
  if (sig) rows.forEach((r, i) => { if (!('h' in r)) keys.push({ key: `r:${i}`, row: keys.length }); });
  const nav = useRef(keys); nav.current = keys;
  const listRef = useRef<ScrollView | null>(null);
  const rowY = useRef<Record<number, number>>({});
  const focus = useListNav(
    grab, keys.length,
    (i) => { const k = nav.current[i]?.key; if (k === 'pane') togglePane(); else if (k === 'min') setMin((m) => !m); },
    (i) => {
      const k = nav.current[i]?.key;
      if (!k || !k.startsWith('r:')) return;
      const y = rowY.current[Number(k.slice(2))];
      if (y != null) listRef.current?.scrollTo({ y: Math.max(0, y - 40), animated: false });
    },
    undefined,
    () => { setGrab(false); return true; },
    { rowOf: (i) => nav.current[i]?.row ?? 0, startAt: 0 },
  );
  const hl = (k: string) => (grab && keys[focus]?.key === k ? { borderColor: ring, borderWidth: 2 } : null);
  const tone = (t?: DrmTone) => (t === 'ok' ? tk.good : t === 'warn' ? tk.warn : t === 'bad' ? tk.bad : tk.value);

  return (
    <DecoderShell bottom={bottomOffset} maxWidth={DRM_MAX_W} borderColor={grab ? ring : undefined}>
      {/* ★ A place the screen's highlight can land: Enter takes the keys into the box. */}
      <View pointerEvents="none" style={StyleSheet.absoluteFill} ref={screenNavRef('drmBox', () => setGrab(true))} />
      <DecoderHeader onPress={() => setMin((m) => !m)}>
        <View style={[s.dot, playing && s.dotOn]} />
        <DecoderTitle minimised={min}>DRM</DecoderTitle>
        <Text style={[s.sub, { color: tk.muted }]} numberOfLines={1}>
          {min && v.label ? v.label : 'Digital Radio Mondiale'}
        </Text>
        <DecoderKey active={pane === 'signal'} onPress={togglePane} style={hl('pane')}
                    label={pane === 'signal' ? 'STATION' : 'SIGNAL'}
                    accessibilityLabel={pane === 'signal' ? 'Back to the station box' : 'Advanced signal information'} />
        <DecoderKey label={min ? 'SHOW' : 'MIN'} onPress={() => setMin((m) => !m)} style={hl('min')}
                    accessibilityLabel={min ? 'Show the DRM box' : 'Fold the DRM box to its title'} />
      </DecoderHeader>
      {!min && pane === 'station' && (
        <DecoderBody>
          <View style={s.body} accessible accessibilityLabel={drmViewText(v).join('. ')}>
            {!!v.label && (
              <View style={s.head}>
                <Text style={[s.label, { color: tk.value }]} numberOfLines={2}>{v.label}</Text>
                {!!v.sid && <Text style={[s.sid, { color: tk.muted, borderColor: tk.divider }]}>{v.sid}</Text>}
              </View>
            )}
            {!!v.meta && <Text style={[s.meta, { color: tk.muted }]}>{v.meta}</Text>}
            {!!v.text && <Text style={[s.text, { color: tk.value }]}>{v.text}</Text>}
            {!!v.lights && (
              <View style={s.lights}>
                {v.lights.map((l) => (
                  <Text key={l.name} style={[s.light, { borderColor: l.on ? DRM_LIGHT_ON : tk.divider, color: l.on ? '#fff' : tk.muted },
                                            l.on && { backgroundColor: DRM_LIGHT_ON }]}>{l.name}</Text>
                ))}
              </View>
            )}
            {!!v.sig && (
              <View style={s.sigRow}>
                <Text style={[s.snr, { color: DRM_SNR_COLOURS[v.sig.q] }]}>{v.sig.snr}</Text>
                <Text style={[s.sig, { color: tk.value }]}>{v.sig.line}</Text>
              </View>
            )}
            {!!v.codec && (
              <View style={s.sigRow}>
                <Text style={[s.light, { color: '#fff', borderColor: v.codec.playable ? DRM_LIGHT_ON : DRM_LIGHT_OFF,
                                         backgroundColor: v.codec.playable ? DRM_LIGHT_ON : DRM_LIGHT_OFF }]}>Audio</Text>
                <Text style={[s.sig, { color: tk.value }]}>{v.codec.line}</Text>
                {!!v.codec.via && <Text style={[s.via, { color: tk.muted }]}>{v.codec.via}</Text>}
              </View>
            )}
            {!!v.why && <Text style={[s.why, { color: tk.value }]} accessibilityRole="alert">{v.why}</Text>}
            {!!v.small && <Text style={[s.small, { color: tk.muted }]}>{v.small}</Text>}
            {!!v.wait && <Text style={[s.wait, { color: tk.muted }]}>{v.wait}</Text>}
          </View>
        </DecoderBody>
      )}
      {sig && (
        <DecoderBody>
          {/* ★ The web's #drmSignal: a small heading per group, amber label, the value in its traffic-light colour where it
              has a nominal range. Each row carries its plain-English explanation as its accessibility hint (the web's tooltip). */}
          <ScrollView ref={listRef} style={{ maxHeight: bodyH }} contentContainerStyle={[scrollLane, s.pane]} showsVerticalScrollIndicator>
            {rows.map((r, i) => ('h' in r
              ? <Text key={`h${i}`} style={[s.section, { color: tk.label }]}>{r.h}</Text>
              : (
                <View key={`r${i}`} style={[s.row, hl(`r:${i}`)]} onLayout={(e) => { rowY.current[i] = e.nativeEvent.layout.y; }}
                      accessible accessibilityLabel={`${r.k}: ${r.v}`} accessibilityHint={r.tip}>
                  <Text style={[s.lbl, { color: tk.rowLabel }]} numberOfLines={2}>{r.k}</Text>
                  <Text style={[s.val, { color: tone(r.tone) }, r.tone === 'bad' && tk.badBold && s.valBad]}>{r.v}</Text>
                </View>
              )))}
          </ScrollView>
        </DecoderBody>
      )}
    </DecoderShell>
  );
}

const makeStyles = (_T: DecoderTokens) => StyleSheet.create({
  dot:    { width: 7, height: 7, borderRadius: 4, backgroundColor: 'rgba(128,128,128,0.45)', marginRight: 6 },
  dotOn:  { backgroundColor: DRM_LIGHT_ON },
  sub:    { flex: 1, fontFamily: DECODER_FONT, fontSize: 10, letterSpacing: 0.6, marginLeft: 8 },
  body:   { paddingHorizontal: 10, paddingVertical: 8, gap: 5 },
  head:   { flexDirection: 'row', alignItems: 'center', gap: 8, flexWrap: 'wrap' },
  label:  { fontFamily: DECODER_FONT, fontSize: 15, fontWeight: '700', letterSpacing: 0.5, flexShrink: 1 },
  sid:    { fontFamily: DECODER_FONT, fontSize: 10, borderWidth: 1, borderRadius: 3, paddingHorizontal: 4 },
  meta:   { fontFamily: DECODER_FONT, fontSize: 11, letterSpacing: 0.3 },
  text:   { fontFamily: DECODER_FONT, fontSize: 12, lineHeight: 17 },
  lights: { flexDirection: 'row', flexWrap: 'wrap', gap: 4 },
  light:  { fontFamily: DECODER_FONT, fontSize: 10, fontWeight: '700', letterSpacing: 0.4, borderWidth: 1, borderRadius: 4,
            paddingHorizontal: 6, paddingVertical: 1, overflow: 'hidden' },
  sigRow: { flexDirection: 'row', alignItems: 'center', gap: 8, flexWrap: 'wrap' },
  snr:    { fontFamily: DECODER_FONT, fontSize: 12, fontWeight: '700', fontVariant: ['tabular-nums'] },
  sig:    { fontFamily: DECODER_FONT, fontSize: 11, flexShrink: 1 },
  via:    { fontFamily: DECODER_FONT, fontSize: 10, fontStyle: 'italic' },
  // ★ the web's .drmWhy: an amber rule and wash — it must be read, silence with no reason reads as a broken receiver
  why:    { fontFamily: DECODER_FONT, fontSize: 12, lineHeight: 17, borderLeftWidth: 3, borderLeftColor: '#E8C547',
            backgroundColor: 'rgba(255,184,51,0.08)', paddingHorizontal: 8, paddingVertical: 4 },
  small:  { fontFamily: DECODER_FONT, fontSize: 10, fontVariant: ['tabular-nums'] },
  wait:   { fontFamily: DECODER_FONT, fontSize: 11, paddingVertical: 4 },
  // ── the Signal pane: DabPanel's rows (124 pt label, value beside it), its section headings ──
  pane:    { paddingHorizontal: 12, paddingVertical: 6, gap: 3 },
  section: { fontFamily: DECODER_FONT, fontSize: 10, letterSpacing: 2, marginTop: 10, marginBottom: 2 },
  row:     { flexDirection: 'row', alignItems: 'flex-start', gap: 8, borderWidth: 2, borderColor: 'transparent', borderRadius: 4 },
  lbl:     { fontFamily: DECODER_FONT, fontSize: 12, letterSpacing: 1, width: 124 },
  val:     { fontFamily: DECODER_FONT, fontSize: 14, flex: 1, fontVariant: ['tabular-nums'] },
  valBad:  { fontWeight: '700' },
});
