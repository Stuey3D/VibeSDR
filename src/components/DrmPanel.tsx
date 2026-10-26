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
 * ★ Over the lower waterfall where the decoder box sits (it stands down while a decoder or the spots are open). Tap the
 *   header to fold it to its title line; the waterfall stays live above it. The decoder font throughout (DecoderShell).
 * ★ No dead controls: the box has no keys but MIN — DRM has nothing to set (the server cuts its own channel, so the
 *   width control is not offered in DRM either).
 */
import React, { useMemo, useState } from 'react';
import { StyleSheet, Text, View } from 'react-native';
import { DecoderShell, DecoderHeader, DecoderTitle, DecoderKey, DecoderBody, DECODER_FONT,
         useDecoderTokens, useDecoderStyles, type DecoderTokens } from './DecoderShell';
import { drmView, drmViewText, DRM_LIGHT_ON, DRM_LIGHT_OFF, DRM_SNR_COLOURS, type DrmState } from '../services/drm';

/** Wide enough for the signal line on one row on a tablet; a phone's window narrows it (DecoderShell). */
const DRM_MAX_W = 560;

export interface DrmPanelProps {
  state: DrmState | null;
  bottomOffset: number;
}

export default function DrmPanel({ state, bottomOffset }: DrmPanelProps) {
  const tk = useDecoderTokens();
  const s = useDecoderStyles(makeStyles);
  const [min, setMin] = useState(false);
  const v = useMemo(() => drmView(state), [state]);
  const playing = !!state?.msc && !!state?.playable;

  return (
    <DecoderShell bottom={bottomOffset} maxWidth={DRM_MAX_W}>
      <DecoderHeader onPress={() => setMin((m) => !m)}>
        <View style={[s.dot, playing && s.dotOn]} />
        <DecoderTitle minimised={min}>DRM</DecoderTitle>
        <Text style={[s.sub, { color: tk.muted }]} numberOfLines={1}>
          {min && v.label ? v.label : 'Digital Radio Mondiale'}
        </Text>
        <DecoderKey label={min ? 'SHOW' : 'MIN'} onPress={() => setMin((m) => !m)}
                    accessibilityLabel={min ? 'Show the DRM box' : 'Fold the DRM box to its title'} />
      </DecoderHeader>
      {!min && (
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
});
