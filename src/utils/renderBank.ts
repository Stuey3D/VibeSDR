/**
 * ★★★ SLOTS CLAIMED DURING RENDER, PUBLISHED ONLY ON A REAL COMMIT (V12 489, 2026-10-20 — Stuart's pad on the Mac:
 * "the green box doesn't keep up with the screen scrolling … I cannot select anything").
 *
 * The server list, the bookmarks pane, the decoder box's header and the USB hardware panel each build ONE focus order by
 * claiming a slot per control while they render, in JSX order — so the order is whatever is on screen and cannot drift
 * from it. The bank used to be a plain ref, emptied at the top of the owner's render and read by the key handlers.
 *
 * ★★ THAT IS NOT SAFE IN REACT, and today's work made it bite every time. When a component calls setState with the value
 *    it already holds (here: `setRowSub(0)` after every highlight move), React may still CALL THE COMPONENT to check,
 *    then bail out WITHOUT RENDERING ITS CHILDREN and without committing. The owner's own render emptied the bank and
 *    claimed its own slots again (the RECORDINGS key, the cog, the URL box …) — but the rows, which claim from CHILD
 *    components (ChooserRow, ChooserKey, BmBtn, HBtn), never ran. Every key handler then saw a bank of 5 where the
 *    screen had 21: the highlight drawn on the third DISCOVERED server was treated as list row 1, the list scrolled to
 *    a row that was not lit (the ring "fell behind", then vanished), and Enter pressed a different row from the one
 *    lit — or nothing (the footer bank was empty, so the DIRECTORIES did not open). Reproduced in Node with the real
 *    React (scripts/test_render_bank.ts) — a cold start does not cure it, the first highlight move breaks it.
 *
 * The rule now: claim into `stage` while rendering; read `committed` from anything that runs outside render (a key
 * handler, an effect, a timer). `committed` is replaced in a layout effect, which React runs only for a render it
 * actually commits — a bailed-out render leaves the last good bank in place. `onCommit` is told each committed bank
 * (publish its length to state there, so the focus range follows the rows).
 */
import { useLayoutEffect, useRef } from 'react';

export interface RenderBank<T> {
  /** This render's bank: claim into it while rendering, in JSX order. Never read it from a key handler. */
  readonly stage: { current: T[] };
  /** The last COMMITTED bank — what is on screen. Key handlers, effects and timers read this one. */
  readonly committed: { current: T[] };
}

export function useRenderBank<T>(onCommit?: (bank: readonly T[]) => void): RenderBank<T> {
  const stage = useRef<T[]>([]);
  const committed = useRef<T[]>([]);
  // Emptied for every render — including one React then throws away; that is why nothing outside render reads it.
  stage.current = [];
  const cb = useRef(onCommit);
  cb.current = onCommit;
  useLayoutEffect(() => {
    // A copy: a child re-rendering on its own later must not grow the bank the handlers read.
    committed.current = stage.current.slice();
    cb.current?.(committed.current);
  });
  return useRef<RenderBank<T>>({ stage, committed }).current;
}
