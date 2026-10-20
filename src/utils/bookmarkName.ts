/**
 * ★★ THE BOOKMARK NAME, PRE-FILLED (Stuart, decided 2026-10-20 — brief "Decided 10-20" §3): from the RDS name, then the
 * station name, then the frequency — so SAVE works straight away (it is disabled while the name is blank, and a pad
 * had no way to type one) and the name stays editable. Pure; tested by scripts/test_bookmark_name.ts.
 */
export function bookmarkDefaultName(o: { rds?: string | null; station?: string | null; hz: number }): string {
  const tidy = (s?: string | null) => (s ?? '').replace(/\s+/g, ' ').trim();
  const rds = tidy(o.rds);
  if (rds) return rds.slice(0, 60);
  const st = tidy(o.station);
  if (st) return st.slice(0, 60);
  return bookmarkFreqLabel(o.hz);
}

/** "99.8 MHz", "198 kHz", "7.074 MHz" — the frequency as a name: no trailing zeros, a unit people say. */
export function bookmarkFreqLabel(hz: number): string {
  if (!(hz > 0)) return '';
  const trim = (s: string) => (s.includes('.') ? s.replace(/0+$/, '').replace(/\.$/, '') : s);
  return hz >= 1e6 ? `${trim((hz / 1e6).toFixed(4))} MHz` : `${trim((hz / 1e3).toFixed(3))} kHz`;
}
