/**
 * ★★★ THE LAST INPUT SOURCE — touch, a physical keyboard, or a game controller / TV remote (controller keyboard brief,
 * "Core rule", decided 2026-10-20). Pure: no React Native, so the key bus, PanelNav and the tests share one copy.
 *
 * Stuart's scope rule: decided by the source AT THE MOMENT Enter or a tap opens a field, and followed LIVE while
 * editing — touch → the system keyboard, always (accessibility, other languages); a physical keyboard → types in
 * place, never an on-screen keyboard; a pad or remote → ControllerKeyboard. An addition, never a replacement.
 *
 * ★ Set centrally: src/utils/hardwareKeys.ts on every key it dispatches (a pad report counts as 'pad' even while a
 *   field has the cursor — that is how a pad takes over from the system keyboard), and noteTouch() from the ROOT touch
 *   capture in App.tsx plus every pop-up's own onTouchStart (a Modal is its own root). PanelNav's old `keyboardMode`
 *   is now just `source !== 'touch'`.
 */
export type InputSource = 'touch' | 'keyboard' | 'pad';

export class InputSourceTracker {
  private src: InputSource = 'touch';
  private subs = new Set<(s: InputSource) => void>();
  get(): InputSource { return this.src; }
  set(s: InputSource) {
    if (s === this.src) return;
    this.src = s;
    this.subs.forEach((f) => { try { f(s); } catch { /* a listener's fault */ } });
  }
  subscribe(f: (s: InputSource) => void): () => void {
    this.subs.add(f);
    return () => { this.subs.delete(f); };
  }
}

/** The app's one tracker. */
export const inputSource = new InputSourceTracker();
export const inputSourceNow = (): InputSource => inputSource.get();
