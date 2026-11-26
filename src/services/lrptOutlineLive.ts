/**
 * lrptOutlineLive.ts — the app's METEOR LRPT MAP OUTLINE state (V12, 2026-11-24): the live picture's outline as the
 * server last sent it ({"type":"lrpt_geo"}, DecoderClient), and the viewer's MAP OUTLINE switch (one choice for the live
 * picture, the SATELLITE box and the gallery — remembered on this device; OFF until switched, OUTLINE_DEFAULT_ON).
 * A tiny store rather than props through SDRScreen: the decoder box, the SATELLITE box and the gallery each read it with
 * useLrptOutline(). The rule and the parsing are src/utils/lrptOutline.ts (shared with the web page).
 */
import { useEffect, useState } from 'react';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { OUTLINE_KEY, outlineFromStored, outlineToStored, type Outline } from '../utils/lrptOutline';
import { sameGaps, type LrptGap } from '../utils/lrptGaps';   // ★ beta96 the live picture's gaps (squashed bands)

interface State { outline: Outline | null; why: string; on: boolean; gaps: LrptGap[] }
let state: State = { outline: null, why: '', on: outlineFromStored(null), gaps: [] };
const subs = new Set<(s: State) => void>();
const emit = () => { for (const f of subs) f(state); };
let loaded = false;
function load() {
  if (loaded) return;
  loaded = true;
  AsyncStorage.getItem(OUTLINE_KEY).then((v) => { state = { ...state, on: outlineFromStored(v) }; emit(); }).catch(() => {});
}

/** DecoderClient: the server's live outline (or why there is none). */
export function setLiveOutline(outline: Outline | null, why: string) { state = { ...state, outline, why }; emit(); }
/** A new picture (or the decoder changed): the last outline does not belong to it. */
export function clearLiveOutline() { if (state.outline || state.why || state.gaps.length) { state = { ...state, outline: null, why: '', gaps: [] }; emit(); } }
/** ★★★ beta96 DecoderClient: the live picture's gaps from lrpt_status (src/utils/lrptGaps.ts) — emitted only when changed. */
export function setLiveGaps(gaps: LrptGap[]) { if (!sameGaps(gaps, state.gaps)) { state = { ...state, gaps }; emit(); } }
export function setOutlineOn(on: boolean) {
  state = { ...state, on }; emit();
  AsyncStorage.setItem(OUTLINE_KEY, outlineToStored(on)).catch(() => {});
}
export function useLrptOutline(): State & { toggle: () => void } {
  const [s, set] = useState(state);
  useEffect(() => { load(); subs.add(set); set(state); return () => { subs.delete(set); }; }, []);
  return { ...s, toggle: () => setOutlineOn(!state.on) };
}
