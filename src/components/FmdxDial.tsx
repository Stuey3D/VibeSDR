import React, { useState, useMemo, useRef } from 'react';
import { View, Text, StyleSheet, type LayoutChangeEvent } from 'react-native';
import { Gesture, GestureDetector } from 'react-native-gesture-handler';
import type { ThemeTokens } from '../contexts/ThemeContext';

// Vintage analogue tuning dial for the FM-DX tuner (v7, plan §2c). A horizontal
// FM-band scale with a moving needle at the current frequency and station-name
// labels at the frequencies where we've decoded RDS (client-learned map). Tap to
// tune, PINCH to zoom into a clustered part of the band, PAN to scroll, and
// double-tap to reset to the full band. No plugin required.

export interface DialStation {
  freqHz: number;
  name:   string;
  /** When this name was last confirmed by an RDS decode (ms).
   *  ★ Drives 30-day expiry: a Sporadic-E catch heard once and never again
   *  should give its slot back, because a dial cluttered with one-off openings
   *  claims the band is busier than it really is.
   *  Optional only for entries written before the field existed — the loader
   *  stamps those, so nothing in storage stays undated. */
  lastHeard?: number;
  /** RDS Programme Identification. A DIFFERENT broadcaster on a frequency is a
   *  replacement, not a rename, and only the PI can tell the two apart. */
  pi?:    string;
}

/** A station unheard for this long gives up its slot. Matches the rule
 *  VibeServer's RDS bookmarks already use — one behaviour for "what this aerial
 *  can hear", however it was learned. */
export const DIAL_EXPIRY_MS = 30 * 24 * 3600_000;
/** Keep the dial bounded: 300 entries ≈ 12 KB, which is what makes per-server
 *  iCloud sync fit inside the key-value store's 1 MB. */
export const DIAL_MAX = 300;

// Match the VFO drum's LED palette: green digits (hue 120), warm red needle
// (hue 4), on a near-black face.
const GREEN      = 'hsl(120,100%,45%)';
const GREEN_DIM  = 'hsla(120,100%,45%,0.28)';
const GREEN_SOFT = 'hsla(120,100%,55%,0.92)';
const RED        = 'hsl(4,95%,52%)';
const FACE       = '#070806';

interface Props {
  freqHz:  number;
  loHz:    number;
  hiHz:    number;
  stations: DialStation[];
  onTune:  (hz: number) => void;
  theme:   ThemeTokens;
  height?: number;
  /** Controlled zoom/pan window (so the zoom drum can drive it too). */
  view:         { lo: number; hi: number };
  onViewChange: (v: { lo: number; hi: number }) => void;
}

const MIN_SPAN = 2_000_000;   // max zoom-in = 2 MHz visible

function FmdxDial({ freqHz, loHz, hiHz, stations, onTune, theme, height = 158, view, onViewChange }: Props) {
  const [w, setW] = useState(0);
  const onLayout = (e: LayoutChangeEvent) => setW(e.nativeEvent.layout.width);
  const fullSpan = Math.max(1, hiHz - loHz);
  const t = theme;

  // Controlled zoom/pan window. viewRef mirrors the prop so gesture callbacks
  // read the live value without stale closures.
  const viewRef = useRef(view);
  viewRef.current = view;
  const startView = useRef(view);
  const vLo = view.lo, vHi = view.hi, vSpan = Math.max(1, vHi - vLo);
  const x = (hz: number) => ((hz - vLo) / vSpan) * w;

  const gesture = useMemo(() => {
    const clampWin = (lo: number, sp: number) => {
      let l = lo, h = lo + sp;
      if (l < loHz) { l = loHz; h = l + sp; }
      if (h > hiHz) { h = hiHz; l = h - sp; }
      return { lo: l, hi: h };
    };
    const pan = Gesture.Pan().runOnJS(true).activeOffsetX([-8, 8]).failOffsetY([-12, 12])
      .onBegin(() => { startView.current = viewRef.current; })
      .onUpdate((e) => {
        if (!w) return;
        const s = startView.current; const sp = s.hi - s.lo;
        onViewChange(clampWin(s.lo - (e.translationX / w) * sp, sp));
      });
    const pinch = Gesture.Pinch().runOnJS(true)
      .onBegin(() => { startView.current = viewRef.current; })
      .onUpdate((e) => {
        if (!w) return;
        const s = startView.current; const sp0 = s.hi - s.lo;
        const sp = Math.max(MIN_SPAN, Math.min(fullSpan, sp0 / e.scale));
        const focalHz = s.lo + (Math.max(0, Math.min(w, e.focalX)) / w) * sp0;
        onViewChange(clampWin(focalHz - (e.focalX / w) * sp, sp));
      });
    const tap = Gesture.Tap().runOnJS(true).maxDistance(10).onEnd((e) => {
      if (!w) return;
      const v = viewRef.current;
      onTune(v.lo + (Math.max(0, Math.min(w, e.x)) / w) * (v.hi - v.lo));
    });
    const doubleTap = Gesture.Tap().runOnJS(true).numberOfTaps(2).onEnd(() => onViewChange({ lo: loHz, hi: hiHz }));
    return Gesture.Exclusive(doubleTap, Gesture.Race(tap, Gesture.Simultaneous(pan, pinch)));
  }, [w, loHz, hiHz, fullSpan, onTune, onViewChange]);

  // Scale sits at the TOP; station names cascade DOWN one side (vintage-radio
  // style). Rows step down below the MHz numbers.
  const SCALE_Y = 20;
  const ROWS = useMemo(() => {
    const out: number[] = [];
    for (let y = SCALE_Y + 24; y <= height - 12; y += 15) out.push(y);
    return out;
  }, [height]);

  // Which learned station are we tuned to (for the highlight)?
  const curKey = useMemo(() => {
    let best: number | null = null, bestD = 60_000;   // within ~60 kHz
    for (const s of stations) { const d = Math.abs(s.freqHz - freqHz); if (d < bestD) { bestD = d; best = s.freqHz; } }
    return best != null ? `${best}` : null;
  }, [stations, freqHz]);

  // Adaptive scale: pick the finest label step that keeps ≤ ~12 labels across
  // the visible span (2 MHz → 1 → 0.5 → 0.2 → 0.1 as you zoom in). Minor ticks
  // subdivide it. Integer Hz math avoids float drift.
  const ticks = useMemo(() => {
    if (!Number.isFinite(vLo) || !Number.isFinite(vHi) || vHi <= vLo) return [] as { hz: number; major: boolean }[];
    const span = vHi - vLo;
    const ladder = [100_000, 200_000, 500_000, 1_000_000, 2_000_000];
    let labelStep = 2_000_000;
    for (const s of ladder) { if (span / s <= 12) { labelStep = s; break; } }
    const minorStep = labelStep >= 500_000 ? labelStep / 5 : labelStep / 2;
    const start = Math.ceil(vLo / minorStep) * minorStep;
    const out: { hz: number; major: boolean }[] = [];
    for (let f = start; f <= vHi; f += minorStep) {
      out.push({ hz: f, major: f % labelStep === 0 });
    }
    return out;
  }, [vLo, vHi]);

  // Every in-range station gets a tick (proof it was saved). NAME labels use
  // collision avoidance (two rows, min gap) so dense city bands stay readable —
  // the current station always wins a label; others fill remaining space.
  const { ticks: staTicks, labels } = useMemo(() => {
    const empty = { ticks: [] as { key: string; px: number }[], labels: [] as { key: string; px: number; name: string; top: number }[] };
    if (!w) return empty;
    const MIN_GAP = 46;
    const inRange = [...stations].filter(s => s.freqHz >= vLo && s.freqHz <= vHi && s.name)
      .sort((a, b) => a.freqHz - b.freqHz);
    const ticks = inRange.map(s => ({ key: `${s.freqHz}`, px: x(s.freqHz) }));
    // Place the current station first so it's never dropped, then nearest-out.
    const ordered = [...inRange].sort((a, b) =>
      Math.abs(a.freqHz - freqHz) - Math.abs(b.freqHz - freqHz));
    const rowsX: number[][] = ROWS.map(() => []);
    const labels: { key: string; px: number; name: string; top: number }[] = [];
    for (const s of ordered) {
      const px = x(s.freqHz);
      let placed = -1;
      for (let r = 0; r < ROWS.length; r++) {
        if (rowsX[r].every(p => Math.abs(p - px) >= MIN_GAP)) { placed = r; break; }
      }
      if (placed < 0) continue;
      rowsX[placed].push(px);
      labels.push({ key: `${s.freqHz}`, px, name: s.name, top: ROWS[placed] });
    }
    return { ticks, labels };
  }, [stations, w, vLo, vHi, freqHz, ROWS]);

  const needleX = w ? x(freqHz) : 0;

  return (
    <GestureDetector gesture={gesture}>
      <View
        onLayout={onLayout}
        style={[styles.wrap, { height, backgroundColor: FACE, borderColor: t.barBorder }]}
      >
        {/* Baseline (near the top) */}
        <View style={[styles.baseline, { top: SCALE_Y, backgroundColor: GREEN_DIM }]} />

        {/* Ticks (point down from the scale) + MHz labels beneath */}
        {w > 0 && ticks.map(({ hz, major }) => {
          if (!Number.isFinite(hz)) return null;
          const px = x(hz);
          if (px < -18 || px > w + 18) return null;
          const mhz = hz / 1e6;
          const lbl = Number.isInteger(mhz) ? String(mhz) : mhz.toFixed(1);
          return (
            <React.Fragment key={`m${hz}`}>
              <View style={{ position: 'absolute', left: px, top: SCALE_Y, width: 1, height: major ? 12 : 6, backgroundColor: GREEN_DIM }} />
              {major && (
                <Text style={[styles.tickLbl, { left: px - 16, width: 32, top: SCALE_Y + 13, color: GREEN, fontFamily: t.font }]}>{lbl}</Text>
              )}
            </React.Fragment>
          );
        })}

        {/* A tick for every saved station, hanging off the scale */}
        {staTicks.map(({ key, px }) => (
          <View key={`t${key}`} style={{ position: 'absolute', left: px, top: SCALE_Y - 6, width: 1, height: 12, backgroundColor: key === curKey ? RED : GREEN_SOFT }} />
        ))}

        {/* Station name labels — cascading down one side; tuned station in red */}
        {labels.map(({ key, px, name, top }) => (
          <Text
            key={`l${key}`}
            numberOfLines={1}
            style={[styles.staLbl, {
              left: px - 30, width: 60, fontFamily: t.font, top, textAlign: 'center',
              color: key === curKey ? RED : GREEN_SOFT,
              fontWeight: key === curKey ? 'bold' : 'normal',
            }]}
          >{name}</Text>
        ))}

        {/* Current-frequency needle (warm red, matches the drum) */}
        {w > 0 && needleX >= 0 && needleX <= w && (
          <>
            <View style={{ position: 'absolute', left: needleX, top: 2, width: 2, height: height - 4, backgroundColor: RED }} />
            <View style={{ position: 'absolute', left: needleX - 5, top: 0, width: 0, height: 0, borderLeftWidth: 5, borderRightWidth: 5, borderTopWidth: 7, borderLeftColor: 'transparent', borderRightColor: 'transparent', borderTopColor: RED }} />
          </>
        )}
      </View>
    </GestureDetector>
  );
}

const styles = StyleSheet.create({
  wrap: { borderRadius: 14, borderWidth: 1, overflow: 'hidden' },
  baseline: { position: 'absolute', left: 0, right: 0, height: 1 },
  tickLbl: { position: 'absolute', width: 20, textAlign: 'center', fontSize: 10, fontWeight: 'bold' },
  staLbl: { position: 'absolute', fontSize: 9 },
});

export default React.memo(FmdxDial);
