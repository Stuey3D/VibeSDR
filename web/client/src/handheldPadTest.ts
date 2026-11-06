/**
 * ★★ V12 R36S — CONTROLLER TEST (R36S HARDWARE → CONTROLLER). What the pad REALLY reports, live: every button by its
 * Gamepad-API number and standard name, the four axes, the pad's id and mapping.
 *
 * WHY (2026-11-06, the first card on Stuart's R36S): in SET FROM MAP neither L1/R1 nor L2/R2 zoomed and the D-pad
 * "snapped oddly" — yet the page's tests drive a standard-layout pad and pass. The image has no SSH (by design), so
 * the only way to see what libmanette hands WebKit for `r36s_Gamepad` is on the screen itself. Clones of this console
 * are wired differently, so it stays as a user tool, not a one-off.
 *
 * ★ The whole pad is taken (setPadCapture) so nothing behind the overlay moves. Leave: HOLD ANY BUTTON 2 s (a tap
 *   only shows the button). Each change is also written to the console, which Cog sends to the journal.
 */
import { setPadCapture } from './gamepad';

const STD = ['B0 bottom', 'B1 right', 'B2 left', 'B3 top', 'B4 L1', 'B5 R1', 'B6 L2', 'B7 R2', 'B8 SELECT', 'B9 START',
             'B10 L3', 'B11 R3', 'B12 D-UP', 'B13 D-DOWN', 'B14 D-LEFT', 'B15 D-RIGHT', 'B16 HOME'];
const HOLD_MS = 2000;

let open = false;

export function openPadTest(): void {
  if (open) return;
  open = true;
  const root = document.createElement('div');
  root.id = 'hhPadTest';
  root.setAttribute('role', 'dialog');
  root.setAttribute('aria-label', 'Controller test');
  root.style.cssText = 'position:fixed;inset:0;z-index:9700;background:#050805;color:#cfe;padding:10px 14px;'
    + 'font:13px ui-monospace,monospace;white-space:pre;overflow:hidden';
  document.body.appendChild(root);
  const release = setPadCapture({ act: () => { /* ★ swallowed: the test reads the pad itself */ } });

  const seen = new Set<number>();
  let lastLog = '', heldSince = 0, raf = 0;
  const close = () => {
    if (!open) return;
    open = false;
    cancelAnimationFrame(raf);
    release();
    root.remove();
  };
  const frame = () => {
    if (!open) return;
    const pads = navigator.getGamepads ? [...navigator.getGamepads()].filter(Boolean) as Gamepad[] : [];
    const lines: string[] = ['CONTROLLER TEST', 'Hold any button 2 s to leave', ''];
    if (!pads.length) lines.push('No pad reported to the page — press a button.');
    let anyDown = false;
    for (const gp of pads) {
      const down: number[] = [];
      gp.buttons.forEach((b, i) => { if (b?.pressed) { down.push(i); seen.add(i); } });
      if (down.length) anyDown = true;
      lines.push(`pad ${gp.index}: ${gp.id}`);
      lines.push(`mapping "${gp.mapping}" · ${gp.buttons.length} buttons · ${gp.axes.length} axes`);
      lines.push('');
      lines.push('DOWN NOW: ' + (down.length ? down.map((i) => STD[i] ?? `B${i}`).join(', ') : '—'));
      lines.push('');
      lines.push('AXES: ' + gp.axes.map((a, i) => `A${i} ${a >= 0 ? '+' : ''}${a.toFixed(2)}`).join('  '));
      lines.push('');
      lines.push('SEEN SO FAR: ' + ([...seen].sort((a, b) => a - b).map((i) => STD[i] ?? `B${i}`).join(', ') || '—'));
      const log = `[pad test] ${gp.id} map=${gp.mapping} down=${down.join(',')} axes=${gp.axes.map((a) => a.toFixed(2)).join(',')}`;
      if (log !== lastLog) { lastLog = log; console.log(log); }
    }
    root.textContent = lines.join('\n');
    const now = performance.now();
    if (!anyDown) heldSince = 0;
    else if (!heldSince) heldSince = now;
    else if (now - heldSince >= HOLD_MS) { close(); return; }
    raf = requestAnimationFrame(frame);
  };
  raf = requestAnimationFrame(frame);
}
