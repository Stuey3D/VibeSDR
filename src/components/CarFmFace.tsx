/**
 * CarFmFace — the stripped, glanceable FM-only face for the CarFM fork.
 *
 * Renders as a full-screen opaque layer OVER the existing SDR pipeline (spec §4:
 * a separate UI layer, not tangled into the radio logic). SDRScreen keeps doing
 * all the connect / audio / RDS work; this just presents an FM tuner and calls
 * back into SDRScreen's existing handlers. An "Advanced" button lets the full
 * SDR UI (waterfall, decoders, every mode) back in when needed.
 *
 * Accessibility (spec §6): high-contrast dark theme, big touch targets, and NO
 * state encoded by red-vs-green. State uses position, shape, labels, and a
 * blue/amber/neutral palette. Colour only ever reinforces a label, never
 * carries meaning alone.
 */

import React, { useEffect, useRef, useState } from 'react';
import {
  View, Text, Pressable, ScrollView, StyleSheet, Animated, Easing,
  type LayoutChangeEvent,
} from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';

export interface CarFmPreset {
  name: string;
  frequency: number;   // Hz
}

export interface CarFmFaceProps {
  freqHz: number;
  stationName?: string;     // RDS PS
  radioText?: string;       // RDS RT
  stereo: boolean;
  signalDb: number | null;
  presets: CarFmPreset[];
  region2: boolean;         // FM channel raster: 200 kHz (Region 2) vs 100 kHz
  onStep: (dir: 1 | -1) => void;
  onSeekPreset: (dir: 1 | -1) => void;
  onSelectPreset: (frequency: number) => void;
  onSavePreset: () => void;
  onEnterFreq: () => void;
  onOpenAdvanced: () => void;
}

// ── Colourblind-safe dark palette (no red/green carries state) ────────────────
const C = {
  bg:        '#05070A',
  panel:     '#0E141B',
  panelHi:   '#151D28',
  freq:      '#FFB833',   // amber — the frequency, the one "hot" element
  text:      '#F2F5F8',
  dim:       '#8A94A2',
  accent:    '#3B9EFF',   // blue — active/selected/stereo (safe for red-green CVD)
  border:    'rgba(255,255,255,0.14)',
  segOn:     '#FFB833',
  segOff:    'rgba(255,255,255,0.10)',
};

const FM_MIN_HZ = 87_000_000;
const FM_MAX_HZ = 108_500_000;

function fmtFreq(freqHz: number): string {
  return (freqHz / 1e6).toFixed(1);
}

// ── Scrolling RadioText ticker ────────────────────────────────────────────────
function RadioTextTicker({ text }: { text: string }) {
  const [containerW, setContainerW] = useState(0);
  const [textW, setTextW] = useState(0);
  const x = useRef(new Animated.Value(0)).current;

  useEffect(() => {
    if (!textW || !containerW || textW <= containerW) {
      x.setValue(0);
      return;
    }
    // Classic ticker: slide the text from the right edge fully off the left,
    // then loop. Speed ~60 px/s so it's readable at a glance while driving.
    const distance = containerW + textW;
    const duration = (distance / 60) * 1000;
    x.setValue(containerW);
    const anim = Animated.loop(
      Animated.timing(x, {
        toValue: -textW,
        duration,
        easing: Easing.linear,
        useNativeDriver: true,
      }),
    );
    anim.start();
    return () => anim.stop();
  }, [textW, containerW, text, x]);

  const scrolls = textW > containerW && containerW > 0;

  return (
    <View
      style={styles.rtWrap}
      onLayout={(e: LayoutChangeEvent) => setContainerW(e.nativeEvent.layout.width)}
    >
      {scrolls ? (
        <Animated.Text
          numberOfLines={1}
          style={[styles.rtText, { transform: [{ translateX: x }] }]}
          onLayout={(e: LayoutChangeEvent) => setTextW(e.nativeEvent.layout.width)}
        >
          {text}
        </Animated.Text>
      ) : (
        <Text
          numberOfLines={1}
          style={[styles.rtText, { textAlign: 'center' }]}
          onLayout={(e: LayoutChangeEvent) => setTextW(e.nativeEvent.layout.width)}
        >
          {text}
        </Text>
      )}
    </View>
  );
}

// ── Signal meter (position + number, never colour alone) ──────────────────────
function SignalMeter({ db }: { db: number | null }) {
  const SEGMENTS = 12;
  // FM listenable SNR runs ~0–40 dB; map to filled segments by position.
  const frac = db == null ? 0 : Math.max(0, Math.min(1, db / 40));
  const filled = Math.round(frac * SEGMENTS);
  return (
    <View style={styles.sigRow}>
      <Text style={styles.sigLabel}>SIGNAL</Text>
      <View style={styles.sigBar}>
        {Array.from({ length: SEGMENTS }).map((_, i) => (
          <View
            key={i}
            style={[
              styles.sigSeg,
              { backgroundColor: i < filled ? C.segOn : C.segOff },
            ]}
          />
        ))}
      </View>
      <Text style={styles.sigValue}>{db == null ? '—' : `${Math.round(db)} dB`}</Text>
    </View>
  );
}

export default function CarFmFace(props: CarFmFaceProps) {
  const {
    freqHz, stationName, radioText, stereo, signalDb, presets, region2,
    onStep, onSeekPreset, onSelectPreset, onSavePreset, onEnterFreq, onOpenAdvanced,
  } = props;
  const insets = useSafeAreaInsets();

  const rasterKHz = region2 ? 200 : 100;
  const inFmBand = freqHz >= FM_MIN_HZ && freqHz <= FM_MAX_HZ;
  const rt = (radioText ?? '').trim();
  const ps = (stationName ?? '').trim();

  return (
    <View style={[styles.root, { paddingTop: insets.top + 8, paddingBottom: insets.bottom + 8 }]}>
      {/* Top bar: mode badge + stereo state (shape + label), Advanced escape */}
      <View style={styles.topBar}>
        <View style={styles.badgeRow}>
          <View style={styles.fmBadge}><Text style={styles.fmBadgeText}>FM</Text></View>
          {/* Stereo/mono: glyph + word + blue tint, never colour alone */}
          <View style={[styles.stereoPill, stereo && styles.stereoPillOn]}>
            <Text style={[styles.stereoGlyph, stereo && styles.stereoGlyphOn]}>
              {stereo ? '◎' : '○'}
            </Text>
            <Text style={[styles.stereoText, stereo && styles.stereoTextOn]}>
              {stereo ? 'STEREO' : 'MONO'}
            </Text>
          </View>
          {!inFmBand && (
            <View style={styles.oobPill}><Text style={styles.oobText}>OUT OF FM BAND</Text></View>
          )}
        </View>
        <Pressable
          onPress={onOpenAdvanced}
          style={({ pressed }) => [styles.advBtn, pressed && styles.pressed]}
          accessibilityRole="button"
          accessibilityLabel="Advanced SDR view"
        >
          <Text style={styles.advText}>ADVANCED ▸</Text>
        </Pressable>
      </View>

      {/* Frequency + station name */}
      <View style={styles.center}>
        <Pressable
          onPress={onEnterFreq}
          style={styles.freqPress}
          accessibilityRole="button"
          accessibilityLabel={`Frequency ${fmtFreq(freqHz)} megahertz. Tap to enter a frequency.`}
        >
          <View style={styles.freqRow}>
            <Text style={styles.freq} allowFontScaling={false}>{fmtFreq(freqHz)}</Text>
            <Text style={styles.unit}>MHz</Text>
          </View>
        </Pressable>
        <Text style={styles.station} numberOfLines={1}>
          {ps || (inFmBand ? 'Tuning…' : '—')}
        </Text>
        <RadioTextTicker text={rt || 'Waiting for RadioText…'} />
      </View>

      <SignalMeter db={signalDb} />

      {/* Transport row: seek presets ⏮⏭, step tune ∓ (big targets) */}
      <View style={styles.transport}>
        <Pressable
          onPress={() => onSeekPreset(-1)}
          style={({ pressed }) => [styles.tBtn, pressed && styles.pressed]}
          accessibilityRole="button" accessibilityLabel="Previous preset"
        >
          <Text style={styles.tGlyph}>⏮</Text>
          <Text style={styles.tLabel}>PRESET</Text>
        </Pressable>
        <Pressable
          onPress={() => onStep(-1)}
          style={({ pressed }) => [styles.tBtn, pressed && styles.pressed]}
          accessibilityRole="button" accessibilityLabel={`Tune down ${rasterKHz} kilohertz`}
        >
          <Text style={styles.tGlyph}>−</Text>
          <Text style={styles.tLabel}>{rasterKHz} kHz</Text>
        </Pressable>
        <Pressable
          onPress={() => onStep(1)}
          style={({ pressed }) => [styles.tBtn, pressed && styles.pressed]}
          accessibilityRole="button" accessibilityLabel={`Tune up ${rasterKHz} kilohertz`}
        >
          <Text style={styles.tGlyph}>＋</Text>
          <Text style={styles.tLabel}>{rasterKHz} kHz</Text>
        </Pressable>
        <Pressable
          onPress={() => onSeekPreset(1)}
          style={({ pressed }) => [styles.tBtn, pressed && styles.pressed]}
          accessibilityRole="button" accessibilityLabel="Next preset"
        >
          <Text style={styles.tGlyph}>⏭</Text>
          <Text style={styles.tLabel}>PRESET</Text>
        </Pressable>
      </View>

      {/* Presets */}
      <View style={styles.presetHeader}>
        <Text style={styles.presetTitle}>PRESETS</Text>
        <Pressable
          onPress={onSavePreset}
          style={({ pressed }) => [styles.saveBtn, pressed && styles.pressed]}
          accessibilityRole="button" accessibilityLabel="Save current station as preset"
        >
          <Text style={styles.saveText}>＋ SAVE</Text>
        </Pressable>
      </View>
      <ScrollView
        horizontal
        showsHorizontalScrollIndicator={false}
        contentContainerStyle={styles.presetRow}
      >
        {presets.length === 0 ? (
          <Text style={styles.presetEmpty}>No presets yet — tune a station and tap SAVE.</Text>
        ) : (
          presets.map((p, i) => {
            const active = Math.abs(p.frequency - freqHz) < 50_000;   // within ½ FM channel
            return (
              <Pressable
                key={`${p.frequency}-${i}`}
                onPress={() => onSelectPreset(p.frequency)}
                style={({ pressed }) => [
                  styles.preset, active && styles.presetActive, pressed && styles.pressed,
                ]}
                accessibilityRole="button"
                accessibilityState={{ selected: active }}
                accessibilityLabel={`${p.name}, ${fmtFreq(p.frequency)} megahertz${active ? ', playing' : ''}`}
              >
                {/* Active marker is a filled dot (shape), not just colour */}
                <Text style={[styles.presetDot, active && styles.presetDotOn]}>
                  {active ? '▶' : '○'}
                </Text>
                <Text style={[styles.presetName, active && styles.presetNameOn]} numberOfLines={1}>
                  {p.name}
                </Text>
                <Text style={[styles.presetFreq, active && styles.presetFreqOn]}>
                  {fmtFreq(p.frequency)}
                </Text>
              </Pressable>
            );
          })
        )}
      </ScrollView>
    </View>
  );
}

const styles = StyleSheet.create({
  root: {
    position: 'absolute', top: 0, left: 0, right: 0, bottom: 0,
    backgroundColor: C.bg,
    zIndex: 60,
    paddingHorizontal: 16,
  },
  topBar: {
    flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between',
  },
  badgeRow: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  fmBadge: {
    backgroundColor: C.freq, borderRadius: 6, paddingHorizontal: 10, paddingVertical: 3,
  },
  fmBadgeText: { color: '#1A1300', fontWeight: '800', fontSize: 16, letterSpacing: 1 },
  stereoPill: {
    flexDirection: 'row', alignItems: 'center', gap: 5,
    borderWidth: 1, borderColor: C.border, borderRadius: 6,
    paddingHorizontal: 9, paddingVertical: 3,
  },
  stereoPillOn: { borderColor: C.accent, backgroundColor: 'rgba(59,158,255,0.12)' },
  stereoGlyph: { color: C.dim, fontSize: 14 },
  stereoGlyphOn: { color: C.accent },
  stereoText: { color: C.dim, fontSize: 12, fontWeight: '700', letterSpacing: 1 },
  stereoTextOn: { color: C.accent },
  oobPill: {
    borderWidth: 1, borderColor: C.freq, borderRadius: 6, paddingHorizontal: 9, paddingVertical: 3,
  },
  oobText: { color: C.freq, fontSize: 11, fontWeight: '700', letterSpacing: 0.5 },
  advBtn: {
    borderWidth: 1, borderColor: C.border, borderRadius: 8,
    paddingHorizontal: 14, paddingVertical: 8,
  },
  advText: { color: C.text, fontSize: 13, fontWeight: '700', letterSpacing: 1 },

  center: { flex: 1, justifyContent: 'center', alignItems: 'center' },
  freqPress: { alignItems: 'center' },
  freqRow: { flexDirection: 'row', alignItems: 'flex-end' },
  freq: {
    color: C.freq, fontSize: 104, fontWeight: '800', lineHeight: 108,
    fontVariant: ['tabular-nums'],
  },
  unit: { color: C.dim, fontSize: 26, fontWeight: '700', marginBottom: 18, marginLeft: 8 },
  station: { color: C.text, fontSize: 34, fontWeight: '700', marginTop: 4, maxWidth: '100%' },

  rtWrap: { height: 26, marginTop: 8, alignSelf: 'stretch', overflow: 'hidden', justifyContent: 'center' },
  rtText: { color: C.dim, fontSize: 18, fontWeight: '600' },

  sigRow: { flexDirection: 'row', alignItems: 'center', gap: 10, marginVertical: 8 },
  sigLabel: { color: C.dim, fontSize: 12, fontWeight: '700', letterSpacing: 1, width: 56 },
  sigBar: { flex: 1, flexDirection: 'row', gap: 3, height: 16, alignItems: 'stretch' },
  sigSeg: { flex: 1, borderRadius: 2 },
  sigValue: { color: C.text, fontSize: 14, fontWeight: '700', width: 56, textAlign: 'right' },

  transport: { flexDirection: 'row', gap: 10, marginTop: 6 },
  tBtn: {
    flex: 1, height: 76, borderRadius: 12, backgroundColor: C.panelHi,
    borderWidth: 1, borderColor: C.border, alignItems: 'center', justifyContent: 'center',
  },
  tGlyph: { color: C.text, fontSize: 30, fontWeight: '800', lineHeight: 34 },
  tLabel: { color: C.dim, fontSize: 11, fontWeight: '700', letterSpacing: 1, marginTop: 2 },

  presetHeader: {
    flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between',
    marginTop: 14, marginBottom: 8,
  },
  presetTitle: { color: C.dim, fontSize: 12, fontWeight: '700', letterSpacing: 1.5 },
  saveBtn: {
    borderWidth: 1, borderColor: C.accent, borderRadius: 8,
    paddingHorizontal: 14, paddingVertical: 7, backgroundColor: 'rgba(59,158,255,0.10)',
  },
  saveText: { color: C.accent, fontSize: 13, fontWeight: '800', letterSpacing: 0.5 },
  presetRow: { gap: 10, paddingRight: 8, alignItems: 'stretch' },
  presetEmpty: { color: C.dim, fontSize: 15, paddingVertical: 20 },
  preset: {
    minWidth: 118, borderRadius: 12, backgroundColor: C.panel,
    borderWidth: 1, borderColor: C.border, paddingHorizontal: 14, paddingVertical: 12,
    justifyContent: 'center',
  },
  presetActive: { borderColor: C.accent, borderWidth: 2, backgroundColor: 'rgba(59,158,255,0.14)' },
  presetDot: { color: C.dim, fontSize: 13, marginBottom: 2 },
  presetDotOn: { color: C.accent },
  presetName: { color: C.text, fontSize: 17, fontWeight: '700' },
  presetNameOn: { color: '#FFFFFF' },
  presetFreq: { color: C.dim, fontSize: 14, fontWeight: '600', marginTop: 1 },
  presetFreqOn: { color: C.accent },

  pressed: { opacity: 0.6 },
});
