/**
 * ★★★ appAlert's rules — the pure part (controller audit root cause D, 2026-10-20). Tested by scripts/test_app_alert.ts.
 *
 * An iOS native alert (UIAlertController) cannot be answered by a game controller, and the UI underneath it keeps
 * hearing the pad — Enter pressed things BEHIND the alert. `appAlert()` (src/components/AppAlert.tsx) keeps Alert.alert's
 * exact shape; with a pad or keyboard in use it draws an in-app dialog instead, and these are its rules:
 *   - the keys are the buttons in the order given;
 *   - the highlight starts on the last button that is neither `cancel` nor `destructive` (the platform's "default"
 *     side), else on the cancel button, else the first — **a destructive button is never where it starts**, so a
 *     stray Enter can never take over a receiver or delete something;
 *   - Back presses the `cancel` button; with none, a lone button (an "OK" notice) is Back too; otherwise Back
 *     dismisses only when the alert is cancelable (Alert.alert's own rule), else it does nothing.
 */
export type AlertStyle = 'default' | 'cancel' | 'destructive';
export interface AlertButtonLike { text?: string; style?: AlertStyle }

/** Alert.alert with no buttons shows one "OK". */
export function alertButtons<T extends AlertButtonLike>(buttons: T[] | undefined): Array<T | { text: string }> {
  return buttons && buttons.length ? buttons : [{ text: 'OK' }];
}

export function alertStartIndex(buttons: readonly AlertButtonLike[]): number {
  for (let i = buttons.length - 1; i >= 0; i--) {
    const st = buttons[i].style;
    if (st !== 'cancel' && st !== 'destructive') return i;
  }
  const c = buttons.findIndex((b) => b.style === 'cancel');
  if (c >= 0) return c;
  return 0;   // all destructive: nothing safe to start on; the first one, which Enter must still be pressed to fire
}

/** What Back does: press button `index`, dismiss with no button, or nothing. */
export type AlertBack = { press: number } | { dismiss: true } | { none: true };

export function alertBack(buttons: readonly AlertButtonLike[], cancelable: boolean): AlertBack {
  const c = buttons.findIndex((b) => b.style === 'cancel');
  if (c >= 0) return { press: c };
  if (buttons.length === 1 && buttons[0].style !== 'destructive') return { press: 0 };
  return cancelable ? { dismiss: true } : { none: true };
}

/** One step of the highlight (left/up = -1, right/down = +1), clamped. */
export function alertMove(cur: number, dir: -1 | 1, n: number): number {
  if (n <= 0) return -1;
  return Math.max(0, Math.min(n - 1, cur + dir));
}
