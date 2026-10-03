/**
 * dabShareServices — keep the SAME services array (and the same per-service objects) across DAB
 * reports when the list has not changed.
 *
 * ★★ WHY (efficiency audit 2026-10-03). The server sends the whole multiplex state about once a second
 *   and every report is a freshly parsed object, so `dabState.services` was a new array of new objects
 *   every second even when the list was identical — which defeats any memo keyed on it (React.memo on
 *   a row, a useMemo over the list). Structural sharing: an unchanged service keeps its old object,
 *   and an unchanged list keeps its old array. Pure (no React, no RN) so it can be tested in node.
 * ★ Cost: one shallow compare per service per report (~20 services x ~25 fields, once a second).
 */

type Svc = Record<string, unknown>;

function sameValue(a: unknown, b: unknown): boolean {
  if (a === b) return true;
  if (Array.isArray(a) && Array.isArray(b)) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!sameValue(a[i], b[i])) return false;
    return true;
  }
  if (a && b && typeof a === 'object' && typeof b === 'object') return sameShallow(a as Svc, b as Svc);
  return false;
}

function sameShallow(a: Svc, b: Svc): boolean {
  const ka = Object.keys(a), kb = Object.keys(b);
  if (ka.length !== kb.length) return false;
  for (const k of ka) if (!sameValue(a[k], b[k])) return false;
  return true;
}

/** `next` with every service that equals its predecessor (by sid) replaced by the predecessor's object,
 *  and `prev` itself returned when nothing changed. Order is `next`'s. */
export function shareDabServices<T extends { sid: number }>(prev: T[] | undefined, next: T[]): T[] {
  if (!prev || prev.length === 0 || prev === next) return next;
  const bySid = new Map<number, T>();
  for (const s of prev) bySid.set(s.sid, s);
  let allSame = prev.length === next.length;
  const out = new Array<T>(next.length);
  for (let i = 0; i < next.length; i++) {
    const n = next[i];
    const p = bySid.get(n.sid);
    const keep = p !== undefined && sameShallow(p as unknown as Svc, n as unknown as Svc);
    out[i] = keep ? p : n;
    if (!keep || prev[i] !== p) allSame = false;
  }
  return allSame ? prev : out;
}
