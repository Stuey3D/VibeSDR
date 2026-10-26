/**
 * ★★★ BACK FROM A RADIO LANDS WHERE YOU LEFT (Stuart, 2026-10-27: "in the main app when you back out of a radio it
 * always takes you back to the top level rather than back to the directory you were in"). The pure part, tested by
 * scripts/test_picker_return.ts; InstancePickerScreen wires it up.
 *
 * ★★★ THE CAUSE. The server list keeps "which directory, what search, which groups open" in its own component state,
 *     and a native stack keeps the screen MOUNTED under the radio, so for most receivers Back found it intact. A
 *     VIBESERVER is opened differently: connectVibeServer lands with `navigation.reset` to [InstancePicker, SDR]
 *     (the watch's background connect needs that — see its note), and a reset that names the picker WITHOUT its key
 *     builds a NEW picker. New picker, new state: selectedDir null — the top level. The VibeServer directory is the
 *     first one listed and the one Stuart uses most, so to him it was "always".
 * ★★ THE FIX IS NOT TO KEEP THE KEY. With the old picker kept in the reset, RN Screens sees a new top on an existing
 *    stack and PUSHES it, animated — the exact transition UIKit defers in the background and never applies (the black
 *    watch-connect screen that reset was written to cure). So the reset stays as it is, and the place is CARRIED:
 *    saved here as the picker leaves, adopted by the new picker when the reset says `restorePlace`.
 * ★★ ONE OWNER. A place is restored only by the picker that saved it, or by the one a reset handed it to. A deep link,
 *    the watch or the crash guard building a fresh picker never says `restorePlace`, so it lands at the top as before
 *    — a link is somebody else's choice of where to be, not a return.
 * ★ In memory only, never persisted: a cold launch starts at the top, as it always has.
 */

export interface PickerPlace<I = unknown> {
  /** The picker instance that left (or was handed the place by a reset). */
  owner: number;
  /** The open directory, or null for the chooser (favourites + directory cards). */
  dir: string | null;
  filter: string;
  openGroups: string[];
  showRestricted: boolean;
  /** The directory's rows as they were — restored as-is, the same list a picker that stayed mounted would show. */
  instances: I[];
  /** The list's scroll offset when it was left. */
  scrollY: number;
  /** The URL of the receiver that was opened, or null (a discovered server, local hardware… — rows with no list key). */
  anchor: string | null;
  /** Where that receiver sat in the list then (-1 if not in it). */
  anchorIndex: number;
}

let place: PickerPlace | null = null;

export const pickerPlace = {
  save(p: PickerPlace): void { place = p; },
  peek(): PickerPlace | null { return place; },
  /** Hand the place to another picker instance (the one a reset just built). Returns it, or null. */
  adopt(newOwner: number): PickerPlace | null {
    if (!place) return null;
    place = { ...place, owner: newOwner };
    return place;
  },
  /** The place, if `owner` holds it — consumed: a return happens once. */
  take(owner: number): PickerPlace | null {
    if (!place || place.owner !== owner) return null;
    const p = place; place = null;
    return p;
  },
  clear(): void { place = null; },
};

let nextOwner = 1;
/** A fresh owner id per picker instance. */
export function newPickerOwner(): number { return nextOwner++; }

/** A directory that is no longer offered is not a place: the chooser instead. Never anything above the picker. */
export function restorableDir(dir: string | null, known: readonly string[]): string | null {
  return dir !== null && known.includes(dir) ? dir : null;
}

/** The groups to open on return: the ones that were open, plus the opened receiver's own (so it is on screen). */
export function groupsToRestore(saved: readonly string[], anchorGroup: string | null): string[] {
  const out = [...saved];
  if (anchorGroup && !out.includes(anchorGroup)) out.push(anchorGroup);
  return out;
}

export type ReturnScroll =
  | { kind: 'stay' }                       // the list never went anywhere: leave it alone
  | { kind: 'offset'; y: number }          // the same place as before, to the point
  | { kind: 'index'; index: number }       // the receiver moved in the list: centre it
  | { kind: 'top' };                       // the receiver is gone: this list's top, never the app's

export interface ReturnPlan {
  scroll: ReturnScroll;
  /** The list row to highlight (and give the pad's focus), or -1. */
  focus: number;
}

/**
 * Where the list goes and what is highlighted, given the place and the list as it is NOW.
 * `urls` is the list's rows in order (null for a header). `remounted` = this picker is new, so its scroll is at 0.
 *
 * ★ The receiver still where it was → the same offset (or nothing at all, on a picker that kept its scroll).
 * ★ It moved (a Most-Used favourite rises to the top the moment it is opened) → centre it where it is now.
 * ★ It is gone (unfavourited from the radio's menu, the directory changed) → the top of the list it was in.
 * ★ No receiver to find (a discovered server, local hardware) → the offset alone.
 */
export function planReturn(p: Pick<PickerPlace, 'anchor' | 'anchorIndex' | 'scrollY'>,
                           urls: readonly (string | null)[], remounted: boolean): ReturnPlan {
  const kept = (): ReturnScroll => (remounted ? { kind: 'offset', y: Math.max(0, p.scrollY) } : { kind: 'stay' });
  if (!p.anchor) return { scroll: kept(), focus: -1 };
  const at = urls.indexOf(p.anchor);
  if (at < 0) return { scroll: { kind: 'top' }, focus: -1 };
  if (at === p.anchorIndex) return { scroll: kept(), focus: at };
  return { scroll: { kind: 'index', index: at }, focus: at };
}
