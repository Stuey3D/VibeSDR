/**
 * ★★★ WHICH LITE RELEASE IS NEWER — the one decision in "Check for updates" (2026-10-10), kept pure so it is
 * tested (scripts/test_lite_update.ts). The native side (LiteUpdateModule, Lite only) lists the releases and
 * downloads; this picks.
 *
 * Versions as they are written:
 *   release tag     lite-v11.0.0-rc37     lite-v11.0.0      lite-v11.0.0-b5
 *   installed build 11.0.0~rc37-lite      11.0.0-lite       11.0.0~rc34test1-lite
 * Order: the numbers, then a final release above any pre-release, then b (beta) below rc, then the
 * pre-release number. ★ A TEST build (rc34test1) sits just below its own rc, and test releases are never
 * OFFERED — they are handed to one person for one fault, not to everyone who presses the button.
 */
export interface LiteVersion { nums: number[]; pre: '' | 'b' | 'rc'; preN: number; test: boolean }

export function parseLiteVersion(s: string): LiteVersion | null {
  const t = String(s || '').trim().replace(/^lite-v/i, '').replace(/-lite$/i, '');
  const m = /^(\d+)\.(\d+)\.(\d+)(?:[~-](b|rc)(\d+)(test\d*)?)?$/i.exec(t);
  if (!m) return null;
  return {
    nums: [Number(m[1]), Number(m[2]), Number(m[3])],
    pre: (m[4] ? m[4].toLowerCase() : '') as LiteVersion['pre'],
    preN: m[5] ? Number(m[5]) : 0,
    test: !!m[6],
  };
}

/** < 0 if a is older than b, 0 if the same, > 0 if a is newer. */
export function compareLite(a: LiteVersion, b: LiteVersion): number {
  for (let i = 0; i < 3; i++) if (a.nums[i] !== b.nums[i]) return a.nums[i] - b.nums[i];
  const rank = (v: LiteVersion) => (v.pre === '' ? 2 : v.pre === 'rc' ? 1 : 0);
  if (rank(a) !== rank(b)) return rank(a) - rank(b);
  if (a.preN !== b.preN) return a.preN - b.preN;
  if (a.test !== b.test) return a.test ? -1 : 1;
  return 0;
}

export interface LiteRelease { tag: string; apkName: string; apkUrl: string; size: number; published: string }

/** The newest offerable release, if it is newer than `installed`; otherwise null. */
export function newerLiteRelease(releases: LiteRelease[], installed: string): (LiteRelease & { v: LiteVersion }) | null {
  const mine = parseLiteVersion(installed);
  let best: (LiteRelease & { v: LiteVersion }) | null = null;
  for (const r of releases) {
    const v = parseLiteVersion(r.tag);
    if (!v || v.test || !r.apkUrl || !(r.size > 0)) continue;
    if (!best || compareLite(v, best.v) > 0) best = { ...r, v };
  }
  if (!best) return null;
  // ★ An installed version we cannot read (a hand-built one) is not offered a "newer" guess.
  if (!mine || compareLite(best.v, mine) <= 0) return null;
  return best;
}

/** How a version is said to a person: "RC37", "11.0.0", "beta 5". */
export function liteVersionLabel(v: LiteVersion): string {
  if (v.pre === 'rc') return `RC${v.preN}${v.test ? ' (test)' : ''}`;
  if (v.pre === 'b') return `beta ${v.preN}${v.test ? ' (test)' : ''}`;
  return v.nums.join('.');
}
