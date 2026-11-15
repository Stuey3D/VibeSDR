/**
 * dabBlockView.ts — DAB BLOCK LABELS AND THE BLOCK SWAPPER on a wide spectrum. ONE pure module, shared by the app
 * (WaterfallView.tsx, fed by SDRScreen) and the web client (web/client/src/waterfall.ts, fed by main.ts). Tested by
 * scripts/test_dab_block_view.ts.
 *
 * ★★★ STUART'S BRIEF (2026-11-15, the Almere USRP B210 capturing 56 MS/s — all of Band III on one screen, DAB on 7D):
 *     "For DAB on a super wide radio like this add the spectrum labels to show the blocks and, if in the memory, show the
 *     MUX name next to it — DAB 7D – MTVNL, DAB 8A – 8A regio Alphen. And allow clicking the waterfall to tune to the
 *     specific DAB block (no option to unlock waterfall click from step rate; this is simply a block swapper without
 *     having to walk up all blocks). Maybe on radios with sample rates of 4 MS/s and above which can show 2 or more
 *     multiplexes side by side."
 *
 *  ★ WHEN: in DAB, and only while the span ACTUALLY SHOWN holds two whole blocks side by side (DAB_WIDE_MIN_SPAN_HZ) —
 *    so a narrow radio never changes, and zooming in on a wide one drops the labels and the swapper together.
 *  ★ NAMES: only what the receiver has told us — the live ensemble's own label for the block it is on, the server's
 *    block memory (DabState.blocks), the last band scan's list (MULTIPLEXES), a DAB bookmark that carries an ensemble
 *    name. Never invented: a block nobody has named is plain "DAB 5A".
 *  ★ THE CLICK: inside a block's bandwidth (±DAB_BLOCK_HALF_HZ of its centre) = go to that multiplex, by the SAME
 *    function the MULTIPLEXES list calls (dabGoTo in both clients), so ask-before-tuning, the shared dial, the server's
 *    block change and its service auto-select all apply. Outside every block: nothing — DAB has no dial.
 *  ★ LAYOUT: the labels are KnownSignal rows, laid out by knownSignals.ts labelLayout with everything else on those two
 *    rows (LISTEN, RF CENTRE, the signal labels), so they never overlap. Named blocks outrank bare ones; the block being
 *    received outranks both and is drawn in the tuned colour.
 */
import { DAB_BLOCKS } from '../services/dabBlocks.ts';
import type { KnownSignal } from './knownSignals.ts';

/** Half a DAB ensemble's occupied bandwidth (1.536 MHz / 2). */
export const DAB_BLOCK_HALF_HZ = 768_000;
/** ★ Two whole blocks side by side need one block spacing (1.712 MHz) plus one block's width (1.536 MHz) = 3.248 MHz.
 *  A 3.2 MS/s RTL never reaches it (one multiplex at a time); a 4 MS/s RSP and up does. */
export const DAB_WIDE_MIN_SPAN_HZ = 3_300_000;

/** Collision priority (knownSignals.ts PRIO_*: safety is 7) — the block labels are the reason for the view in DAB. */
export const PRIO_DAB_CURRENT = 10;
export const PRIO_DAB_NAMED = 9;
export const PRIO_DAB_BLOCK = 8;

/** The longest ensemble name drawn beside a block (characters, an ellipsis included). */
export const DAB_MUX_NAME_MAX = 18;

/** True when the span shown can hold two whole multiplexes side by side. */
export function dabWideView(spanHz: number): boolean {
  return Number.isFinite(spanHz) && spanHz >= DAB_WIDE_MIN_SPAN_HZ;
}

/** Where a block's name can come from — each optional, all what the server has said. */
export interface DabNameSources {
  /** The ensemble being received: its block name ("7D") and its own label. Outranks everything. */
  live?: { channel: string; label: string } | null;
  /** The server's block memory (DabState.blocks: block name → ensemble label). */
  memory?: Readonly<Record<string, string>> | null;
  /** The last band scan's hits (dab_scan `found`). */
  scan?: readonly { block: string; ensemble: string }[] | null;
  /** Bookmarks — only a DAB one that carries an ensemble name counts (most carry only the service's). */
  bookmarks?: readonly { frequency: number; mode?: string | null; ensemble?: string | null }[] | null;
}

const clean = (s: unknown) => (typeof s === 'string' ? s.replace(/\s+/g, ' ').trim() : '');

/** Block name → ensemble name, from what the receiver has told us; highest-ranked source wins. */
export function dabMuxNames(src: DabNameSources): Map<string, string> {
  const out = new Map<string, string>();
  const put = (block: string, name: unknown, force = false) => {
    const n = clean(name);
    if (!block || !n) return;
    if (force || !out.has(block)) out.set(block, n);
  };
  // lowest first, each later source overwriting: bookmarks < scan < memory < live
  for (const b of src.bookmarks ?? []) {
    if (String(b.mode ?? '').toLowerCase() !== 'dab') continue;
    const i = DAB_BLOCKS.findIndex((k) => Math.abs(k.hz - b.frequency) < 50_000);
    if (i >= 0) put(DAB_BLOCKS[i].name, b.ensemble, true);
  }
  for (const h of src.scan ?? []) put(h.block, h.ensemble, true);
  for (const [k, v] of Object.entries(src.memory ?? {})) put(k, v, true);
  if (src.live) put(src.live.channel, src.live.label, true);
  return out;
}

/** "DAB 7D — MTVNL", or "DAB 5A" with no name. A long name is shortened with an ellipsis. */
export function dabBlockLabel(block: string, mux?: string): string {
  let n = clean(mux);
  if (n.length > DAB_MUX_NAME_MAX) n = n.slice(0, DAB_MUX_NAME_MAX - 1).trimEnd() + '…';
  return n ? `DAB ${block} — ${n}` : `DAB ${block}`;
}

/** A block label row for the shared layout: which block, and whether it is the one being received. */
export interface DabBlockSignal extends KnownSignal { dabIdx: number; dabCurrent: boolean }

/** Every Band III block as a label (sorted by frequency — labelLayout takes the ones in view). `current` = index into
 *  DAB_BLOCKS of the block being received, −1 = none. */
export function dabBlockSignals(names: ReadonlyMap<string, string>, current: number): DabBlockSignal[] {
  return DAB_BLOCKS.map((b, i) => {
    const mux = names.get(b.name);
    return {
      hz: b.hz, lo: b.hz - DAB_BLOCK_HALF_HZ, hi: b.hz + DAB_BLOCK_HALF_HZ,
      label: dabBlockLabel(b.name, mux), dec: '', dialHz: b.hz, mode: 'dab',
      prio: i === current ? PRIO_DAB_CURRENT : mux ? PRIO_DAB_NAMED : PRIO_DAB_BLOCK,
      src: 'dab', dabIdx: i, dabCurrent: i === current,
    } as DabBlockSignal;
  });
}

/** True for a label row made by dabBlockSignals. */
export function isDabBlockSignal(s: KnownSignal): s is DabBlockSignal {
  return (s as DabBlockSignal).dabIdx !== undefined;
}

/** The blocks whose centre is in [lo, hi] (indices into DAB_BLOCKS). */
export function dabBlocksInView(lo: number, hi: number): number[] {
  const out: number[] = [];
  for (let i = 0; i < DAB_BLOCKS.length; i++) if (DAB_BLOCKS[i].hz >= lo && DAB_BLOCKS[i].hz <= hi) out.push(i);
  return out;
}

/** The labels to lay out in DAB on this view: the signal labels already in force with every block merged in (sorted),
 *  or the signal labels alone when the view cannot hold two blocks. */
export function withDabBlocks(known: readonly KnownSignal[] | null | undefined, blocks: readonly DabBlockSignal[] | null | undefined,
                              spanHz: number): readonly KnownSignal[] | null {
  if (!blocks || !blocks.length || !dabWideView(spanHz)) return known && known.length ? known : null;
  if (!known || !known.length) return blocks;
  return [...known, ...blocks].sort((a, b) => a.hz - b.hz);
}

/** ★★ THE CLICK: the block a click at `hz` lands in (index into DAB_BLOCKS), or −1 — outside every block, or a view too
 *  narrow to swap blocks on. Where two blocks overlap (10A/10N, 11A/11N, 12A/12N sit 160 kHz apart) the nearer centre. */
export function dabBlockHit(hz: number, spanHz: number): number {
  if (!Number.isFinite(hz) || !dabWideView(spanHz)) return -1;
  let best = -1, bestD = Infinity;
  for (let i = 0; i < DAB_BLOCKS.length; i++) {
    const d = Math.abs(DAB_BLOCKS[i].hz - hz);
    if (d <= DAB_BLOCK_HALF_HZ && d < bestD) { bestD = d; best = i; }
  }
  return best;
}

/** The hover hint on the web: "go to 8A — 8A regio Alphen", "go to 5A", "on 7D — MTVNL" for the block already playing. */
export function dabBlockHint(idx: number, names: ReadonlyMap<string, string>, current: number): string {
  const b = DAB_BLOCKS[idx];
  if (!b) return '';
  const n = clean(names.get(b.name));
  return `${idx === current ? 'on' : 'go to'} ${b.name}${n ? ' — ' + n : ''}`;
}
