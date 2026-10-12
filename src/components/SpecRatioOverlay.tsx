/**
 * SpecRatioOverlay — floating pill above the control island.
 *
 * Lets the user drag a slider to set the spectrum/waterfall height split.
 * Portrait and landscape ratios are stored independently — on rotation the
 * correct ratio is restored automatically.
 *
 * The pill appears above the control bar (positioned via `bottomOffset` prop),
 * slides in with a spring, and has a ✓ tick button to dismiss.
 *
 * Matches the VibeSDR mockup spec-ratio-overlay design exactly.
 */

import React, { useCallback, useEffect, useRef, useState } from 'react';
import {
  Animated,
  StyleSheet,
  Text,
  TouchableOpacity,
  View,
} from 'react-native';
import Slider from '@react-native-community/slider';
import { useTheme } from '../contexts/ThemeContext';
import { useGlassTheme } from './PopupShell';

// ── Types ──────────────────────────────────────────────────────────────────────

export interface SpecRatioOverlayProps {
  visible:         boolean;
  isLandscape:     boolean;
  portraitRatio:   number;         // 0.05 – 0.65
  landscapeRatio:  number;
  bottomOffset:    number;         // px from bottom of screen (pillTop - 10)
  onChange:        (portrait: number, landscape: number) => void;
  onClose:         () => void;
}

// ── Constants ──────────────────────────────────────────────────────────────────

const FONT  = 'Atkinson Hyperlegible';
const MIN   = 0.05;
const MAX   = 0.65;

// ── Component ──────────────────────────────────────────────────────────────────

export default function SpecRatioOverlay({
  visible, isLandscape,
  portraitRatio, landscapeRatio,
  bottomOffset, onChange, onClose,
}: SpecRatioOverlayProps) {

  const { theme: t } = useTheme();
  /* ★★ IN THE USER'S COLOURS (Stuart, 2026-10-12: "yes colours apply everywhere including directory if possible").
   *   The old amber / white split followed the retired theme name (always 'white' since the faceplates). Now:
   *   the read-out and labels in the TEXT colour, the ✓ key, the slider and the panel's edge in the CONTROLS
   *   colour. Dark glass on every chassis (useGlassTheme). The split bar's yellow / blue stand for the spectrum
   *   and the waterfall themselves and stay. */
  const pt = useGlassTheme();
  const value  = pt.gold.value;          // the read-out
  const label  = pt.ink.textDim;         // title, badge, scale labels
  const bdr    = pt.ink.border;          // the panel's edge
  const badgeBdr = pt.ink.keyBorder;
  const badgeBg  = 'rgba(255,255,255,0.06)';
  const trackMax = 'rgba(255,255,255,0.15)';

  const translateY = useRef(new Animated.Value(20)).current;
  const opacity    = useRef(new Animated.Value(0)).current;

  const currentRatio = isLandscape ? landscapeRatio : portraitRatio;
  const [sliderVal, setSliderVal] = useState(currentRatio);

  useEffect(() => {
    setSliderVal(isLandscape ? landscapeRatio : portraitRatio);
  }, [isLandscape, landscapeRatio, portraitRatio, visible]);

  useEffect(() => {
    if (visible) {
      Animated.parallel([
        Animated.spring(translateY, { toValue: 0, damping: 22, stiffness: 220, useNativeDriver: true }),
        Animated.timing(opacity, { toValue: 1, duration: 180, useNativeDriver: true }),
      ]).start();
    } else {
      Animated.parallel([
        Animated.timing(translateY, { toValue: 20, duration: 200, useNativeDriver: true }),
        Animated.timing(opacity, { toValue: 0, duration: 160, useNativeDriver: true }),
      ]).start();
    }
  }, [visible, translateY, opacity]);

  const handleChange = useCallback((val: number) => {
    const clamped = Math.max(MIN, Math.min(MAX, val));
    setSliderVal(clamped);
    if (isLandscape) onChange(portraitRatio, clamped);
    else             onChange(clamped, landscapeRatio);
  }, [isLandscape, portraitRatio, landscapeRatio, onChange]);

  if (!visible) return null;

  const specPct = Math.round(sliderVal * 100);
  const wfPct   = 100 - specPct;

  return (
    <Animated.View
      style={[sro.wrap, { bottom: bottomOffset, borderColor: bdr, opacity, transform: [{ translateY }] }]}
      pointerEvents="auto"
    >
      {/* Header */}
      <View style={sro.header}>
        <Text style={[sro.title, { color: label, fontFamily: t.font }]}>
          SPECTRUM / WATERFALL RATIO
        </Text>
        <View style={[sro.badge, { backgroundColor: badgeBg, borderColor: badgeBdr }]}>
          <Text style={[sro.badgeTxt, { color: label, fontFamily: t.font }]}>
            {isLandscape ? 'LANDSCAPE' : 'PORTRAIT'}
          </Text>
        </View>
        <TouchableOpacity
          style={[sro.tick, { backgroundColor: pt.ink.keyBgAct, borderColor: pt.ink.keyBorder }]}
          onPress={onClose} hitSlop={8} activeOpacity={0.75}
        >
          <Text style={[sro.tickTxt, { color: pt.gold.sel }]}>✓</Text>
        </TouchableOpacity>
      </View>

      {/* Value readout */}
      <Text style={[sro.val, { color: value, fontFamily: t.font }]}>
        Spectrum {specPct}% · Waterfall {wfPct}%
      </Text>

      {/* Slider */}
      <Slider
        style={sro.slider}
        minimumValue={MIN}
        maximumValue={MAX}
        step={0.01}
        value={sliderVal}
        onValueChange={handleChange}
        minimumTrackTintColor={pt.gold.fill}
        maximumTrackTintColor={trackMax}
        thumbTintColor={pt.gold.thumb}
      />

      {/* Labels */}
      <View style={sro.labels}>
        <Text style={[sro.labelTxt, { color: label, fontFamily: t.font }]}>◀ more waterfall</Text>
        <Text style={[sro.labelTxt, { color: label, fontFamily: t.font }]}>50/50</Text>
        <Text style={[sro.labelTxt, { color: label, fontFamily: t.font }]}>more spectrum ▶</Text>
      </View>

      {/* Visual split bar */}
      <View style={sro.splitBar}>
        <View style={[sro.splitSpec, { flex: specPct }]} />
        <View style={[sro.splitWf,   { flex: wfPct   }]} />
      </View>
    </Animated.View>
  );
}

// ── Styles ────────────────────────────────────────────────────────────────────

const sro = StyleSheet.create({
  wrap: {
    position: 'absolute', left: 8, right: 8,
    backgroundColor: 'rgba(10,8,4,0.84)',
    borderWidth: 1,
    borderRadius: 18,
    paddingHorizontal: 16, paddingTop: 12, paddingBottom: 14,
    gap: 10,
    shadowColor: '#000', shadowOffset: { width: 0, height: 4 },
    shadowOpacity: 0.85, shadowRadius: 12, elevation: 12,
    zIndex: 200,
  },
  header:    { flexDirection: 'row', alignItems: 'center', gap: 8 },
  title:     { flex: 1, fontFamily: FONT, fontSize: 10, letterSpacing: 2 },
  badge:     { borderWidth: 1, borderRadius: 10, paddingHorizontal: 8, paddingVertical: 2 },
  badgeTxt:  { fontFamily: FONT, fontSize: 9, letterSpacing: 1 },
  tick: {
    width: 32, height: 32, borderRadius: 16,
    borderWidth: 1,
    alignItems: 'center', justifyContent: 'center', flexShrink: 0,
  },
  tickTxt:   { fontSize: 16 },
  val:       { fontFamily: FONT, fontSize: 14, textAlign: 'center', letterSpacing: 1 },
  slider:    { width: '100%', height: 36 },
  labels:    { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  labelTxt:  { fontFamily: FONT, fontSize: 9, letterSpacing: 0.5 },
  splitBar:  { height: 8, borderRadius: 4, flexDirection: 'row', gap: 2, overflow: 'hidden' },
  splitSpec: { backgroundColor: 'rgba(255,200,50,0.35)', borderRadius: 4, minWidth: 2 },
  splitWf:   { backgroundColor: 'rgba(0,140,255,0.25)', borderRadius: 4, minWidth: 2 },
});
