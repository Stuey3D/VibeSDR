/**
 * PsStabiliser — turns the raw RDS Programme Service name into something that can be DISPLAYED.
 *
 * ★★★ THE VTS FLICKERED LIKE A BROKEN ELEMENT ON BRAZILIAN RDS (Stuart, 2026-09-29).
 *   PS is eight characters. In Europe it is (mostly) a fixed name, so it arrives, settles, and the
 *   station bar goes still. Brazilian stations use it as a MARQUEE — Kiko's 94.5 rotates
 *   "UMUARAMA" / "MASSA", his 93.7 "UMUARAMA" / "ALINE" / "RADIO", one or two seconds apart, for
 *   ever — so every consumer keyed on the name re-fired every rotation: the VTS faded out and back
 *   in, its RadioText scroll restarted from the left, and the logo lookup (keyed on the name)
 *   blanked the picture and put it back. Nothing was broken; everything was re-triggered.
 *   The server met the same broadcasters when learning RDS bookmarks (local_sdr_shim.cpp: "a
 *   station that marquees its PS can never settle one") and stopped waiting for the text to settle.
 *   The display cannot do that — it has to show SOMETHING — so it shows the rotation AS one name.
 *
 * What it does, per station (the caller's `key` — the PI when there is one):
 *   1. NEVER BLANKS. An empty PS on the same station holds the last name; only a new key resets.
 *   2. A static PS is passed through untouched and at once — the European case does not change.
 *   3. A ROTATING PS (several changes in a short window, or a segment coming round again) is shown
 *      as the whole cycle joined into one stable string — "UMUARAMA ALINE RADIO" — which the VTS
 *      then scrolls like any long name. The cycle is put in a CANONICAL order (starting from the
 *      segment heard first), so the string does not change as the rotation moves round.
 *      A character-by-character scroller ("RADIO MA", "ADIO MAS", …) is re-assembled by overlap.
 *   4. A MINIMUM DWELL bounds the update rate: a new value never replaces one shown less than
 *      MIN_DWELL_MS ago (the newer one waits, see nextDueIn/tick), and a rotating name, once
 *      shown, is replaced at most every ROTATING_DWELL_MS.
 *
 * ★ Pure (no React Native) so scripts/test_psStabiliser.ts can drive it with a fake clock.
 */

export const MIN_DWELL_MS = 3000;
export const ROTATING_DWELL_MS = 10000;
/** A PS that changed this many times inside ROT_WINDOW_MS is rotating. */
const ROT_MIN_CHANGES = 3;
const ROT_WINDOW_MS = 30000;
/** How far back a cycle is looked for — a character scroller can take a minute to come round. */
const HISTORY_MS = 120000;
const HISTORY_MAX = 200;
const MAX_PERIOD = 64;
/** The fallback when no clean period is found (e.g. dynamic PS carrying song titles). */
const FALLBACK_SEGMENTS = 32;

type Seg = { ps: string; t: number };

export class PsStabiliser {
  private key: string | null = null;
  private hist: Seg[] = [];
  private firstSeen = new Map<string, number>();
  private shown = '';
  private shownAt = 0;
  private shownRotating = false;
  private lastRaw = '';

  /** Forget everything — live RDS has ended (retune, mode change). */
  reset(): void {
    this.key = null; this.hist = []; this.firstSeen.clear();
    this.shown = ''; this.shownAt = 0; this.shownRotating = false; this.lastRaw = '';
  }

  /** True while the station's PS is being treated as a rotating marquee. */
  get rotating(): boolean { return this.shownRotating; }

  /**
   * Feed one PS observation for station `key`. Returns the name to DISPLAY — never '' once a name
   * has been seen for this key.
   */
  feed(key: string, psRaw: string | undefined | null, now: number): string {
    if (key !== this.key) { this.reset(); this.key = key; }
    const ps = (psRaw ?? '').replace(/\s+/g, ' ').trim();
    if (ps && ps !== this.lastRaw) {
      this.lastRaw = ps;
      this.hist.push({ ps, t: now });
      if (!this.firstSeen.has(ps)) this.firstSeen.set(ps, now);
    }
    this.prune(now);
    return this.evaluate(now);
  }

  /** Re-evaluate without a new observation — call when nextDueIn() elapses. */
  tick(now: number): string {
    this.prune(now);
    return this.evaluate(now);
  }

  /** ms until the display may change without a new observation, or null if nothing is waiting. */
  nextDueIn(now: number): number | null {
    const want = this.target(now);
    if (!want || !this.shown || want.name === this.shown) return null;
    return Math.max(0, this.dueAt(want) - now);
  }

  // ── internals ──────────────────────────────────────────────────────────────

  private prune(now: number) {
    while (this.hist.length > HISTORY_MAX
           || (this.hist.length > 1 && now - this.hist[0].t > HISTORY_MS)) this.hist.shift();
  }

  /** What we would show if no dwell applied. */
  private target(now: number): { name: string; rotating: boolean } | null {
    if (!this.lastRaw) return null;
    const recent = this.hist.filter(s => now - s.t <= ROT_WINDOW_MS);
    // changes = segments after the first in the window; plus a RECURRENCE (A … B … A) is proof.
    const changes = recent.length - 1;
    const seen = new Set<string>();
    let recurs = false;
    for (let i = 0; i < recent.length; i++) {
      if (seen.has(recent[i].ps) && i > 0 && recent[i - 1].ps !== recent[i].ps) { recurs = true; break; }
      seen.add(recent[i].ps);
    }
    const rotating = changes >= ROT_MIN_CHANGES || recurs;
    if (!rotating) return { name: this.lastRaw, rotating: false };
    return { name: this.cycleName(), rotating: true };
  }

  private evaluate(now: number): string {
    const want = this.target(now);
    if (!want) return this.shown;
    if (!this.shown) { this.adopt(want, now); return this.shown; }
    if (want.name === this.shown) { this.shownRotating = want.rotating; return this.shown; }
    if (now >= this.dueAt(want)) this.adopt(want, now);
    return this.shown;
  }

  /** When `want` may replace what is shown.
   *  ★ A plain (non-rotating) change that arrives after the shown name has sat still for the dwell
   *    is due AT ONCE — that is the European case, a station changing its PS once, unchanged.
   *  ★★ One that arrives INSIDE the dwell is the start of a possible rotation, so it must itself
   *     hold still for MIN_DWELL_MS before it is shown. Otherwise the second segment of a marquee
   *     would flash up for a moment before the rotation had been recognised. */
  private dueAt(want: { name: string; rotating: boolean }): number {
    const base = this.shownAt + (this.shownRotating ? ROTATING_DWELL_MS : MIN_DWELL_MS);
    if (want.rotating) return base;
    const since = this.hist[this.hist.length - 1]?.t ?? this.shownAt;
    return since - this.shownAt >= MIN_DWELL_MS ? base : Math.max(base, since + MIN_DWELL_MS);
  }

  private adopt(want: { name: string; rotating: boolean }, now: number) {
    this.shown = want.name; this.shownAt = now; this.shownRotating = want.rotating;
  }

  /** The rotation as one canonical string. */
  private cycleName(): string {
    const seq = this.hist.map(s => s.ps);
    let cycle: string[] | null = null;
    // Smallest period p such that the last p segments repeat the p before them.
    for (let p = 2; p <= MAX_PERIOD && 2 * p <= seq.length; p++) {
      let ok = true;
      for (let i = seq.length - p; i < seq.length; i++) {
        if (seq[i] !== seq[i - p]) { ok = false; break; }
      }
      if (ok) { cycle = seq.slice(seq.length - p); break; }
    }
    if (!cycle) {
      // No clean period yet: the distinct segments in the order they were first heard.
      const distinct = [...new Set(seq)];
      return trimWrap(mergeSegments(distinct.slice(-FALLBACK_SEGMENTS)));
    }
    return trimWrap(mergeSegments(this.canonicalRotation(cycle)));
  }

  /** Rotate the cycle so it starts from the segment heard FIRST — the rotation's position must
   *  not change the string. Ties (a segment repeated within the cycle) are broken by comparing the
   *  whole rotation's first-seen times, so the choice is deterministic. */
  private canonicalRotation(cycle: string[]): string[] {
    const fs = (s: string) => this.firstSeen.get(s) ?? Number.MAX_SAFE_INTEGER;
    let best = 0;
    for (let r = 1; r < cycle.length; r++) {
      for (let i = 0; i < cycle.length; i++) {
        const a = fs(cycle[(r + i) % cycle.length]), b = fs(cycle[(best + i) % cycle.length]);
        if (a !== b) { if (a < b) best = r; break; }
      }
    }
    return cycle.slice(best).concat(cycle.slice(0, best));
  }
}

/** A merged CYCLE of a character scroller ends by starting again ("RADIO MASSA FM RADIO") —
 *  drop the repeated head from the tail. Needs the same substantial overlap as the merge. */
function trimWrap(s: string): string {
  for (let o = Math.floor(s.length / 2); o >= 3; o--) {
    if (s.endsWith(s.slice(0, o))) return s.slice(0, s.length - o).trim();
  }
  return s;
}

/** Join rotating segments into one string. A segment that overlaps the end of what we have (a
 *  character scroller: "RADIO MA" then "ADIO MAS") is merged; one already contained is skipped;
 *  otherwise segments are separated by a space. The overlap must be substantial — at least 3
 *  characters and half the shorter segment — so "UMUARAMA" + "ALINE" is not fused on one 'A'. */
export function mergeSegments(segs: string[]): string {
  let acc = '';
  for (const s of segs) {
    if (!s) continue;
    if (!acc) { acc = s; continue; }
    if (acc.includes(s) && s.length >= 3) continue;
    let o = Math.min(acc.length, s.length);
    const need = Math.max(3, Math.ceil(Math.min(acc.length, s.length) / 2));
    for (; o >= need; o--) if (acc.endsWith(s.slice(0, o))) break;
    acc = o >= need ? acc + s.slice(o) : `${acc} ${s}`;
  }
  return acc;
}
