/**
 * The B19 perf overlay (src/constants/perfOverlay.ts — THIS BUILD ONLY). Three lines, once a second:
 *   CPU %  — this process, 100 % = one core (iOS: every thread's cpu_usage; Android: /proc/self/stat).
 *   RAM    — iOS phys_footprint (Xcode's / Activity Monitor's figure); Android total PSS.
 *   UI     — the MAIN-thread frame clock over the last second (CADisplayLink / Choreographer): fps and the
 *            median and 90th-percentile frame intervals. A long interval = the main thread missed a vsync.
 *            ✗ No GPU figure: iOS has no public per-app GPU-utilisation API, and none is invented.
 *   TILT   — what drives the light (off / sensor / synthetic), light writes per second, and the Faceplate
 *            provider's React renders per second — tilt must cause 0.
 * ★ Cheap by construction: one native call a second; only this component re-renders; pointerEvents none.
 */
import React, { useEffect, useRef, useState } from 'react';
import { NativeModules, Platform, StyleSheet, Text, View } from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { perfLines } from '../constants/perfOverlay';
import { tiltProbe } from '../services/tiltLight';

type Stats = { cpuPct: number; footprintMB: number; uiFps: number; uiP50Ms: number; uiP90Ms: number };

export default function PerfOverlay() {
  const insets = useSafeAreaInsets();
  const [lines, setLines] = useState<string[]>([]);
  const last = useRef({ w: tiltProbe.writes, r: tiltProbe.providerRenders, t: Date.now() });
  useEffect(() => {
    const mod = NativeModules.VibePowerModule as { perfStats?: () => Promise<Stats> } | undefined;
    if (typeof mod?.perfStats !== 'function') { setLines(['perf: native stats missing in this binary']); return; }
    let live = true;
    const tick = async () => {
      let s: Stats;
      try { s = await mod.perfStats!(); } catch { return; }
      if (!live) return;
      const now = Date.now(), dt = Math.max(0.001, (now - last.current.t) / 1000);
      const w = (tiltProbe.writes - last.current.w) / dt, r = (tiltProbe.providerRenders - last.current.r) / dt;
      last.current = { w: tiltProbe.writes, r: tiltProbe.providerRenders, t: now };
      setLines(perfLines(s, { running: tiltProbe.running, writesPerSec: w, rendersPerSec: r }));
    };
    void tick();
    const id = setInterval(() => { void tick(); }, 1000);
    return () => { live = false; clearInterval(id); };
  }, []);
  if (!lines.length) return null;
  return (
    <View pointerEvents="none" style={[st.wrap, { top: insets.top + 44 }]}>
      <View style={st.box}>
        {lines.map((l, i) => <Text key={i} style={st.t}>{l}</Text>)}
      </View>
    </View>
  );
}

const st = StyleSheet.create({
  // ★ Top-centre, under the band strip and scale: clear of ‹ Servers (top-left) and SERVER HEALTH (top-right).
  wrap: { position: 'absolute', left: 0, right: 0, alignItems: 'center', zIndex: 9999, elevation: 50 },
  box: { backgroundColor: 'rgba(0,0,0,0.55)', borderRadius: 6, paddingHorizontal: 6, paddingVertical: 3 },
  t: { color: '#9cff9c', fontSize: 10, lineHeight: 13,
       fontFamily: Platform.OS === 'ios' ? 'Menlo' : 'monospace' },
});
