/**
 * ★★★ ONE OVERLAY RULE (controller audit, root causes A and B — 2026-10-20). The pure part; PanelNav `useOverlay` wires
 * it to React and the key bus. Tested by scripts/test_overlay_stack.ts.
 *
 * Before this, each screen kept a hand-written list of what was "open" (SDRScreen `anyPanelOpen`, the picker
 * `listNavActive`) and the two ways it went wrong were the same mistake from either side:
 *   A. a surface IN the list but with no key handling — the screen stopped listening and nobody started (the radio
 *      chooser: no highlight, no Enter, no way back);
 *   B. a surface NOT in the list — arrows and Enter went on pressing the controls BEHIND it (the tour cards, the
 *      connection cards, the password boxes, the map).
 *
 * The rule: whatever is on top owns the keys. A surface that draws over a screen registers here while it is visible.
 *   - Any overlay open → the screen hides its highlight and stops acting on movement and Enter (`count() > 0`).
 *   - The TOP overlay hears what its own nav did not take: Back runs its `onBack`, Enter its `onEnter` (a tour card's
 *     NEXT), and arrows are swallowed so nothing beneath moves. Tune and zoom pass (they work from anywhere, as on the
 *     web page) unless the overlay says `blockTuneZoom`.
 *   - Its own `useListNav` / `usePanelNav` still runs first (priority 0), so a sub-page or a slider in adjust mode
 *     takes Back before the overlay closes — one layer at a time.
 *   - A surface cannot suppress the screen without also owning Back: `onBack` is how it registers.
 */
export type OverlayKey = 'ArrowUp' | 'ArrowDown' | 'ArrowLeft' | 'ArrowRight' | 'Enter' | 'Back'
  | 'TuneDown' | 'TuneUp' | 'ZoomOut' | 'ZoomIn' | string;

export interface OverlayOpts {
  /** Back with nothing left to peel inside the surface: close it / go back. */
  onBack?: () => void;
  /** Enter that the surface's own nav did not take (a card with one obvious action — the tour's NEXT). */
  onEnter?: () => void;
  /** Tune and zoom do nothing while this is on top (a card where retuning underneath makes no sense). */
  blockTuneZoom?: boolean;
  /** Still hears Back while a third-party page has every shortcut off (BrowserOverlay: the way out is ours). */
  backWhenSuppressed?: boolean;
  /** ★ Stays above level-0 overlays opened after it, and hears keys before any panel (PanelNav gives it a higher
   *  priority): a tour card that opens the menu behind it to point at something must keep Enter and Back. */
  level?: number;
  /** For diagnostics only. */
  name?: string;
}

export type OverlayDecision = 'back' | 'enter' | 'swallow' | 'pass';

interface Entry { tok: object; opts: OverlayOpts; seq: number }

export class OverlayStack {
  private list: Entry[] = [];
  private seq = 0;
  private subs = new Set<(n: number) => void>();
  push(tok: object, opts: OverlayOpts) {
    this.remove(tok, true);
    this.list.push({ tok, opts, seq: ++this.seq });
    this.emit();
  }
  /** Replace the options of a registered overlay without moving it (its handlers changed). */
  update(tok: object, opts: OverlayOpts) {
    const e = this.list.find((x) => x.tok === tok);
    if (e) e.opts = opts;
  }
  remove(tok: object, quiet = false) {
    const i = this.list.findIndex((x) => x.tok === tok);
    if (i < 0) return;
    this.list.splice(i, 1);
    if (!quiet) this.emit();
  }
  count(): number { return this.list.length; }
  /** The top: the highest level, newest within it. */
  private top(): Entry | undefined {
    let best: Entry | undefined;
    for (const e of this.list) {
      if (!best || (e.opts.level ?? 0) > (best.opts.level ?? 0)
          || ((e.opts.level ?? 0) === (best.opts.level ?? 0) && e.seq > best.seq)) best = e;
    }
    return best;
  }
  isTop(tok: object): boolean { return this.top()?.tok === tok; }
  topName(): string | undefined { return this.top()?.opts.name; }
  subscribe(f: (n: number) => void): () => void {
    this.subs.add(f);
    return () => { this.subs.delete(f); };
  }
  /** What the overlay `tok` does with a key its own nav passed on. */
  decide(tok: object, key: OverlayKey, ev: { typing?: boolean }, suppressed: boolean): OverlayDecision {
    const e = this.list.find((x) => x.tok === tok);
    if (!e || !this.isTop(tok)) return 'pass';
    // A field's own Enter (commit) and Escape (let go of the cursor) are the field's, not ours.
    if (ev.typing) return 'pass';
    if (suppressed) return key === 'Back' && e.opts.backWhenSuppressed ? (e.opts.onBack ? 'back' : 'swallow') : 'pass';
    if (key === 'Back') return e.opts.onBack ? 'back' : 'swallow';
    if (key === 'Enter') return e.opts.onEnter ? 'enter' : 'swallow';
    if (key === 'ArrowUp' || key === 'ArrowDown' || key === 'ArrowLeft' || key === 'ArrowRight') return 'swallow';
    if (key === 'TuneDown' || key === 'TuneUp' || key === 'ZoomOut' || key === 'ZoomIn') return e.opts.blockTuneZoom ? 'swallow' : 'pass';
    return 'pass';
  }
  private emit() {
    const n = this.list.length;
    this.subs.forEach((f) => { try { f(n); } catch { /* a subscriber's fault */ } });
  }
}
