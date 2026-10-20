/**
 * Coachmark — a first-run guided tour: a dimmed spotlight over a target element
 * with a speech bubble, a breathing highlight, and Next / Skip controls.
 *
 * Targets are plain React Native views; we read their on-screen rectangle with
 * measureInWindow, so the same overlay can point at anything (the VFO drum, the
 * step control, a button inside the open menu) without those screens knowing
 * about the tour. A step may run an onEnter action first (e.g. open the menu),
 * then we wait enterDelay ms for layout before measuring.
 *
 * Usage:
 *   const drumRef = useRef(null);
 *   const tour = useCoachmarkTour(steps, { storageKey: 'tour_sdr_v1' });
 *   return (<>… <View ref={drumRef} collapsable={false}/> … {tour.overlay}</>);
 *   // tour.maybeAutoStart() on first layout; tour.restart() from a Replay button.
 */
import React, { useCallback, useEffect, useRef, useState } from 'react';
import {
  Animated, Dimensions, Easing, Pressable, StyleSheet, Text, View,
} from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { useGlassTheme, type PopupTokens } from './PopupShell';
import { useOverlay, useKeyboardMode, useNavFocus } from './PanelNav';

export interface TourStep {
  id: string;
  body: string;
  title?: string;
  target?: React.RefObject<any>;   // element to spotlight; omit → centred card
  onEnter?: () => void;            // run before measuring (e.g. open the menu)
  enterDelay?: number;             // ms to wait for layout before measuring
  padding?: number;                // spotlight padding around the target
  illustration?: React.ReactNode;  // optional mock/render shown inside the bubble
}

interface Rect { x: number; y: number; width: number; height: number; }

// ── Tour-target registry ──────────────────────────────────────────────────
// Components attach `ref={tourRef('id')}` (or mergeRefs(existing, tourRef('id')))
// to the view a step points at — no prop-drilling from the screen that owns the
// tour. Only one of a portrait/landscape pair is ever mounted, so the same id is
// safe in both. Steps reference targets via tourRef('id').
const registry: Record<string, React.MutableRefObject<any>> = {};
export function tourRef(id: string): React.MutableRefObject<any> {
  if (!registry[id]) registry[id] = { current: null };
  return registry[id];
}
export function mergeRefs(...refs: any[]) {
  return (node: any) => refs.forEach((r) => {
    if (typeof r === 'function') r(node);
    else if (r && typeof r === 'object') r.current = node;
  });
}

/* ★★ THE TOUR CARD TAKES THE USER'S COLOURS (Stuart, 2026-10-12: "yes colours apply everywhere including
 *   directory if possible"). It was gold (#ffe566) on every faceplate. Now, via useGlassTheme (the dark glass
 *   skin in the user's colours, whatever the chassis — the card is never plated): the title and body in the
 *   TEXT colour, the spotlight ring, the card's edge and the NEXT key in the CONTROLS colour. */
const DIM  = 'rgba(0,0,0,0.80)';
const F    = 'Atkinson Hyperlegible';

function measure(ref?: React.RefObject<any>): Promise<Rect | null> {
  return new Promise((resolve) => {
    const node = ref?.current;
    if (!node || typeof node.measureInWindow !== 'function') { resolve(null); return; }
    node.measureInWindow((x: number, y: number, width: number, height: number) => {
      if (width > 0 && height > 0) resolve({ x, y, width, height });
      else resolve(null);
    });
  });
}

export function useCoachmarkTour(
  steps: TourStep[],
  opts: { storageKey: string },
) {
  const [active, setActive] = useState(false);
  const [idx, setIdx]       = useState(0);
  const [rect, setRect]     = useState<Rect | null>(null);
  const startedRef = useRef(false);   // guard maybeAutoStart against re-fires

  // Re-measure the current step's target (after its onEnter + layout settle).
  const remeasure = useCallback(async (i: number) => {
    const step = steps[i];
    if (!step) return;
    const r = await measure(step.target);
    setRect(r);
  }, [steps]);

  // Drive a step: run onEnter, wait, measure.
  const goTo = useCallback((i: number) => {
    const step = steps[i];
    if (!step) return;
    setIdx(i);
    setRect(null);
    step.onEnter?.();
    const delay = step.enterDelay ?? (step.onEnter ? 380 : 60);
    setTimeout(() => remeasure(i), delay);
  }, [steps, remeasure]);

  const finish = useCallback(() => {
    setActive(false);
    AsyncStorage.setItem(opts.storageKey, '1').catch(() => {});
  }, [opts.storageKey]);

  const next = useCallback(() => {
    if (idx + 1 >= steps.length) finish();
    else goTo(idx + 1);
  }, [idx, steps.length, finish, goTo]);

  const start = useCallback(() => {
    if (!steps.length) return;
    setActive(true);
    goTo(0);
  }, [steps.length, goTo]);

  // Auto-start once if never seen. Call from the screen after first layout.
  const maybeAutoStart = useCallback(async () => {
    if (startedRef.current) return;
    startedRef.current = true;
    try {
      const seen = await AsyncStorage.getItem(opts.storageKey);
      if (seen !== '1') start();
    } catch { /* ignore */ }
  }, [opts.storageKey, start]);

  const restart = useCallback(() => { startedRef.current = true; start(); }, [start]);

  // Re-measure on rotation / size change.
  useEffect(() => {
    if (!active) return;
    const sub = Dimensions.addEventListener('change', () => {
      setTimeout(() => remeasure(idx), 250);
    });
    return () => sub.remove();
  }, [active, idx, remeasure]);

  const overlay = active ? (
    <CoachmarkOverlay
      step={steps[idx]} rect={rect} index={idx} count={steps.length}
      onNext={next} onSkip={finish}
    />
  ) : null;

  return { overlay, start, restart, maybeAutoStart, active };
}

function CoachmarkOverlay({
  step, rect, index, count, onNext, onSkip,
}: {
  step: TourStep; rect: Rect | null; index: number; count: number;
  onNext: () => void; onSkip: () => void;
}) {
  const styles = useCoachStyles();
  // ★★ A PAD OR KEYBOARD ANSWERS THE CARD (controller audit group B, 2026-10-20). Before this the card had no keys
  //    and was in no yield list, so Enter pressed the control UNDER the card. Now it is a raised overlay (level 1: it
  //    stays on top of a menu a step opens behind it, and hears keys before any panel): Enter = Next, Back = Skip,
  //    arrows and tune/zoom do nothing beneath it. The ring sits on Next, the key Enter presses.
  useOverlay(true, { name: 'tour', level: 1, onEnter: onNext, onBack: onSkip, blockTuneZoom: true });
  const kbInUse = useKeyboardMode();
  const navRing = useNavFocus().ring;
  const win = Dimensions.get('window');
  const pad = step.padding ?? 8;
  const breathe = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    const loop = Animated.loop(Animated.sequence([
      Animated.timing(breathe, { toValue: 1, duration: 900, easing: Easing.inOut(Easing.quad), useNativeDriver: false }),
      Animated.timing(breathe, { toValue: 0, duration: 900, easing: Easing.inOut(Easing.quad), useNativeDriver: false }),
    ]));
    loop.start();
    return () => loop.stop();
  }, [breathe]);

  // Spotlight rect (padded + clamped to screen).
  const sx = rect ? Math.max(0, rect.x - pad) : 0;
  const sy = rect ? Math.max(0, rect.y - pad) : 0;
  const sw = rect ? Math.min(win.width - sx, rect.width + pad * 2) : 0;
  const sh = rect ? Math.min(win.height - sy, rect.height + pad * 2) : 0;

  // Bubble vertical placement: below the target if it's in the top ~55%, else above.
  const below   = rect ? (rect.y + rect.height / 2) < win.height * 0.55 : true;
  const BUBBLE_W = Math.min(320, win.width - 32);
  const bubbleTop = rect
    ? (below ? sy + sh + 14 : undefined)
    : win.height / 2 - 80;
  const bubbleBottom = rect && !below ? (win.height - sy + 14) : undefined;
  // Horizontal: centre on the target, clamped on-screen.
  const bubbleLeft = rect
    ? Math.max(16, Math.min(win.width - BUBBLE_W - 16, rect.x + rect.width / 2 - BUBBLE_W / 2))
    : (win.width - BUBBLE_W) / 2;

  const haloOpacity = breathe.interpolate({ inputRange: [0, 1], outputRange: [0.35, 1] });
  const haloScale   = breathe.interpolate({ inputRange: [0, 1], outputRange: [1, 1.04] });

  return (
    // ★★★ NOT a <Modal>. It was, and that made the hole a LIE: a Modal is a
    // full-screen view that swallows every touch inside it, including the gap the
    // four dim bands leave. So the control the tour spotlighted, breathed a gold
    // ring around and told you to press could not be pressed — only Next/Skip
    // worked. On first run that reads as "the new buttons don't work", which is
    // exactly how it was reported (Stuart, 2026-07-29, on the Mac).
    // ★★ It is the v9.0.2 splash bug again (98261a3): an overlay that LOOKS
    // transparent is not. Same lesson, second visit — so it is written down here.
    // An in-tree absolute overlay lets the hole be a real hole: taps land on the
    // control underneath, while the dim bands still block everything else so the
    // tour keeps its focus. ★ Safe only because no step uses onEnter to open a
    // Modal — a step pointing INSIDE a menu would need the old behaviour back.
    <View style={[StyleSheet.absoluteFill, { zIndex: 9999, elevation: 9999 }]}
          pointerEvents="box-none">
      {/* Dim everything; if there's a target, leave a hole via 4 surrounding bands. */}
      {rect ? (
        <>
          <View style={[styles.dim, { top: 0, left: 0, right: 0, height: sy }]} />
          <View style={[styles.dim, { top: sy + sh, left: 0, right: 0, bottom: 0 }]} />
          <View style={[styles.dim, { top: sy, left: 0, width: sx, height: sh }]} />
          <View style={[styles.dim, { top: sy, left: sx + sw, right: 0, height: sh }]} />
          {/* Breathing highlight ring around the target. */}
          <Animated.View pointerEvents="none" style={{
            position: 'absolute', left: sx, top: sy, width: sw, height: sh,
            borderRadius: 12, borderWidth: 2, borderColor: styles.ring.borderColor,
            opacity: haloOpacity, transform: [{ scale: haloScale }],
          }} />
        </>
      ) : (
        <View style={[styles.dim, StyleSheet.absoluteFill]} />
      )}

      {/* Speech bubble */}
      <View style={[styles.bubble, { width: BUBBLE_W, left: bubbleLeft, top: bubbleTop as number, bottom: bubbleBottom as number }]}>
        {!!step.title && <Text style={styles.title}>{step.title}</Text>}
        <Text style={styles.body}>{step.body}</Text>
        {!!step.illustration && <View style={styles.illo}>{step.illustration}</View>}
        <View style={styles.row}>
          <Text style={styles.count}>{index + 1} / {count}</Text>
          <View style={styles.btns}>
            <Pressable /* nav-skip: Back = Skip (the overlay) */ onPress={onSkip} hitSlop={10} style={styles.skip}>
              <Text style={styles.skipText}>Skip</Text>
            </Pressable>
            <Pressable /* nav-registered: Enter = Next, ringed */ onPress={onNext} hitSlop={10}
                       style={[styles.next, kbInUse && { borderWidth: 3, borderColor: navRing, margin: -3 }]}>
              <Text style={styles.nextText}>{index + 1 >= count ? 'Got it' : 'Next'}</Text>
            </Pressable>
          </View>
        </View>
      </View>
    </View>
  );
}

/** Built once per token object (PopupTokens are memoised per setting). */
const styleCache = new WeakMap<PopupTokens, ReturnType<typeof makeStyles>>();
function useCoachStyles() {
  const pt = useGlassTheme();
  let s = styleCache.get(pt);
  if (!s) { s = makeStyles(pt); styleCache.set(pt, s); }
  return s;
}
const makeStyles = (pt: PopupTokens) => StyleSheet.create({
  dim: { position: 'absolute', backgroundColor: DIM },
  ring: { borderColor: pt.gold.thumb },
  bubble: {
    position: 'absolute', backgroundColor: 'rgba(14,12,10,0.98)', borderRadius: 14,
    borderWidth: 1, borderColor: pt.gold.selBorder, padding: 14,
    shadowColor: '#000', shadowOpacity: 0.5, shadowRadius: 12, shadowOffset: { width: 0, height: 4 },
  },
  title: { color: pt.ink.text, fontFamily: F, fontSize: 14, fontWeight: 'bold', marginBottom: 5, letterSpacing: 0.5 },
  body:  { color: pt.ink.textSoft, fontFamily: F, fontSize: 13.5, lineHeight: 19 },
  illo:  { marginTop: 12 },
  row:   { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginTop: 14 },
  count: { color: pt.ink.textDim, fontFamily: F, fontSize: 11, letterSpacing: 1 },
  btns:  { flexDirection: 'row', alignItems: 'center', gap: 10 },
  skip:  { paddingVertical: 6, paddingHorizontal: 10 },
  skipText: { color: pt.ink.key, fontFamily: F, fontSize: 13 },
  next:  { backgroundColor: pt.gold.thumb, borderRadius: 8, paddingVertical: 7, paddingHorizontal: 18 },
  nextText: { color: '#0a0805', fontFamily: F, fontSize: 13, fontWeight: 'bold' },
});
