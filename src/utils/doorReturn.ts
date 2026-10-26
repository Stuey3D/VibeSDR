/**
 * ★★★ BACK FROM A RADIO BEHIND A FRONT DOOR LANDS ON THAT DOOR'S RADIO LIST — and Back again on the server list.
 * (Stuart, 2026-10-27, the same report as pickerReturn.ts: back out of a radio and you were taken past the door you
 * came through.) The pure part, tested by scripts/test_picker_return.ts.
 *
 * ★★ HOW. The chooser is SDRScreen itself, waiting for a radio (`awaitingRadio`). Back from a radio chosen there
 *    REPLACES the screen with a fresh one at the same door — a clean unmount ends the session the way leaving always
 *    has, and the new screen asks the door again, so the list is current. Un-choosing in place would mean every
 *    session effect on a 13 000-line screen learning to tear down and wait again; one remount does it by construction
 *    (the same `replace` TRY AGAIN and the password prompt already use).
 * ★★ THE VISIT IS CARRIED, NOT RE-ASKED. A radio's PIN and the admin credential typed at the door are "in memory only,
 *    for this visit" (SDRScreen) — and stepping back to the same door's list is still the visit. They ride across the
 *    replace HERE, never in route params (params reach the navigation log), consumed by the first screen that asks
 *    for that door, and dropped if nobody does.
 */

export interface DoorVisit {
  /** The door's address — a visit is only handed to a screen on the same door. */
  base: string;
  /** The radio being left: the chooser starts on it. */
  radioId: string;
  /** PIN per radio this visit unlocked. */
  pins: Record<string, string>;
  /** The admin credential proved at the door ('' if none). */
  adminAuthQ: string;
}

let visit: DoorVisit | null = null;

export const doorVisit = {
  stash(v: DoorVisit): void { visit = { ...v, pins: { ...v.pins } }; },
  /** The visit for this door, consumed. Anything else (another door, nothing stashed) → null, and a stale one goes. */
  take(base: string): DoorVisit | null {
    const v = visit; visit = null;
    return v && v.base === base ? v : null;
  },
};

/**
 * Should Back from this radio go to the door's list rather than the server list?
 * Only when the listener CHOSE it there: a door of one adopted silently, an address that names its radio (/r/<id>),
 * or a session that never had a door all go straight back to the servers, as before.
 */
export function backToDoor(o: { hasDoor: boolean; choseFromList: boolean; radioId: string | null }): boolean {
  return o.hasDoor && o.choseFromList && o.radioId !== null;
}

/** The route params for the screen that shows the door again: one-shot arrivals do not fire twice. */
export function doorReturnParams<P extends Record<string, unknown>>(params: P, freshLocalGen: () => number): P {
  const out: Record<string, unknown> = { ...params, doorReturn: true };
  // ★ A link's tune, and a takeover, belonged to the first connect — never to a radio chosen afterwards.
  for (const k of ['deepLink', 'initialFreq', 'initialMode', 'initialZoom', 'autoTakeover']) delete out[k];
  // ★ A NEW local generation, so the screen being left cannot stop the shim under the one replacing it
  //   (services/localSession.ts — the same guard switching servers relies on).
  if (typeof params.localGen === 'number') out.localGen = freshLocalGen();
  return out as P;
}
