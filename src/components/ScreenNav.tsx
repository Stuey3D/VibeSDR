/**
 * ScreenNav — the HIGHLIGHT on the receiver screen itself, for a hardware keyboard (Stuart, 2026-10-18).
 *
 * ★★★ ONE SCHEME, FULL PARITY with the game controller on the web page and the Apple TV remote: arrows / W A S D move
 *   a highlight box between the screen's controls (the frequency box, the mode, the step, audio, the menu, chat, the
 *   servers chip, the decoder box), Enter presses the highlighted one, < > tune and - + zoom from anywhere. This
 *   replaces the old letters (D demodulator, M menu, S step, A audio, C chat), Enter-opens-the-frequency-box, the
 *   arrows tuning and zooming, and Tab into the decoder box.
 *
 * ★ Direction is judged on the controls' boxes ON SCREEN (src/utils/gamepadNav.ts pickNext — the same picker the web
 *   page's pad uses), so it follows whatever layout is showing: portrait, landscape, iPad, Mac window, hidden controls.
 * ★ Controls join with `ref={screenNavRef('id', onPress)}` (mergeRefs with a tourRef where there is one). Only one of a
 *   portrait/landscape pair is ever mounted, so one id serves both — the Coachmark registry's rule.
 */
import React from 'react';
import { View } from 'react-native';
import { pickNext, type Box, type Dir } from '../utils/gamepadNav';

const nodes = new Map<string, any>();
const presses = new Map<string, () => void>();

/** Attach to a control the screen highlight can land on. `press` is what Enter does there. */
export function screenNavRef(id: string, press: (() => void) | undefined) {
  if (press) presses.set(id, press); else presses.delete(id);
  return (node: any) => {
    if (node) nodes.set(id, node);
    else if (nodes.has(id)) nodes.delete(id);
  };
}

function measure(node: any): Promise<Box | null> {
  return new Promise((resolve) => {
    if (!node || typeof node.measureInWindow !== 'function') { resolve(null); return; }
    let done = false;
    const t = setTimeout(() => { if (!done) { done = true; resolve(null); } }, 250);
    node.measureInWindow((x: number, y: number, w: number, h: number) => {
      if (done) return;
      done = true; clearTimeout(t);
      resolve(w > 2 && h > 2 ? { x, y, w, h } : null);
    });
  });
}

/** Where the highlight goes from `cur` in `dir` (null = start at the top-left control). Null if nowhere. */
export async function screenNavMove(cur: string | null, dir: Dir): Promise<{ id: string; box: Box } | null> {
  const ids = [...nodes.keys()].filter((id) => presses.has(id));
  const boxes = await Promise.all(ids.map((id) => measure(nodes.get(id))));
  const live = ids.map((id, i) => ({ id, box: boxes[i] })).filter((x): x is { id: string; box: Box } => !!x.box);
  if (!live.length) return null;
  const here = cur ? live.find((x) => x.id === cur) : undefined;
  const i = pickNext(here ? here.box : null, live.map((x) => x.box), dir);
  if (i >= 0) return live[i];
  return here ?? null;                        // nothing further that way: stay put
}

/** The highlighted control's box now (it may have moved — a rotation, a re-layout). */
export async function screenNavBox(id: string): Promise<Box | null> {
  return measure(nodes.get(id));
}

/** Enter on the highlighted control. False if it is no longer there. */
export function screenNavPress(id: string): boolean {
  const f = presses.get(id);
  if (!f || !nodes.has(id)) return false;
  f();
  return true;
}

/** The highlight box, drawn over the control (window coordinates; the screen's root fills the window). */
export function ScreenNavRing({ box, color }: { box: Box | null; color: string }) {
  if (!box) return null;
  const pad = 4;
  return (
    <View pointerEvents="none" style={{
      position: 'absolute', left: box.x - pad, top: box.y - pad, width: box.w + pad * 2, height: box.h + pad * 2,
      borderWidth: 3, borderColor: color, borderRadius: 10, zIndex: 9999, elevation: 30,
    }} />
  );
}
