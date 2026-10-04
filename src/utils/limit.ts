/**
 * limit — at most `n` of these promises in flight; the rest wait their turn, in order.
 *
 * ★★ WHY: a DAB multiplex's service list asks the server for ~20 RadioDNS logos at once, and the server
 *    answers "no logo" past its own small cap (LogoLookupSlot, audit 2026-10-03) — so most services fell
 *    back to a name-search guess and kept it. Shared by the app and the web client (one rule, one place).
 */
export function limiter(n: number) {
  let active = 0;
  const queue: Array<() => void> = [];
  const next = () => {
    if (active >= n || !queue.length) return;
    active++;
    queue.shift()!();
  };
  return function run<T>(fn: () => Promise<T>): Promise<T> {
    return new Promise<T>((resolve, reject) => {
      queue.push(() => {
        fn().then(resolve, reject).finally(() => { active--; next(); });
      });
      next();
    });
  };
}
