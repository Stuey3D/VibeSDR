/**
 * ★★★ THE RECEIVER'S UTC OFFSET, FROM THE CLIENT'S OWN TZ RULES WHEN IT CAN (2026-11-12).
 *
 * A VibeServer sends `tzOffsetMin` + `tzAbbr` worked out from ITS device's rule table, and since
 * this change also `tz` — the IANA name ("America/Sao_Paulo"). Kiko's receiver is an old Android TV
 * box in Paraná whose tzdata predates Brazil abolishing DST (2019): in November it sent -120 "BRST",
 * and listeners saw 20:20 when it was 19:20 at the aerial. The NAME is never stale; the box's rules
 * are. So when `tz` is present and this client's Intl knows it, the offset is computed here, from a
 * browser's or phone's current tzdata — and where the two disagree, ours wins: the receiver is the
 * likelier one to be years behind.
 *
 * ★ Falls back to the server's offset/abbreviation exactly as before when there is no `tz` (an older
 *   server, or a machine that does not name its zone), when Intl or its timeZone option is missing
 *   (an old Hermes), or when this client's database does not know the name.
 * ★ The offset is derived from formatToParts (wall-clock fields read back as UTC), not from
 *   timeZoneName 'longOffset', which older engines lack. Pure and shared by the app and the web page.
 */

export interface ServerZone {
  /** Signed minutes from UTC at `now`. */
  offsetMin: number;
  /** The label to show: a zone abbreviation ("BST", "CET") or a signed offset ("-03", "+05:30"). */
  abbr: string;
  /** True when the offset came from this client's rules for `tz`, not the server's figure. */
  fromClientRules: boolean;
}

/** "-03", "+05:30", "UTC" — the shape tzdata's own %Z uses for zones with no letters. */
export function offsetLabel(min: number): string {
  if (min === 0) return 'UTC';
  const a = Math.abs(min);
  const p = (n: number) => String(n).padStart(2, '0');
  return (min > 0 ? '+' : '-') + p(Math.floor(a / 60)) + (a % 60 ? ':' + p(a % 60) : '');
}

type Fmts = { parts: Intl.DateTimeFormat; name: Intl.DateTimeFormat | null } | null;
const cache = new Map<string, Fmts>();

/** The two formatters for a zone, built once (they are not cheap on Hermes); null = unknown zone /
 *  no Intl timeZone support — remembered too, so a bad name costs one throw, not one a tick. */
function fmtsFor(tz: string): Fmts {
  if (cache.has(tz)) return cache.get(tz)!;
  let f: Fmts = null;
  try {
    if (typeof Intl !== 'undefined' && typeof Intl.DateTimeFormat === 'function') {
      const parts = new Intl.DateTimeFormat('en-GB', {
        timeZone: tz, hourCycle: 'h23',
        year: 'numeric', month: '2-digit', day: '2-digit',
        hour: '2-digit', minute: '2-digit', second: '2-digit',
      });
      // ★ An engine with no zone data that IGNORES timeZone (rather than throwing) reports no
      //   resolved zone — refuse it, or it would hand back the device's own time under any name.
      //   Aliases are fine ("US/Eastern" may resolve to "America/New_York"), so no name match.
      if (parts.resolvedOptions().timeZone) {
        let name: Intl.DateTimeFormat | null = null;
        try { name = new Intl.DateTimeFormat('en-GB', { timeZone: tz, timeZoneName: 'short' }); } catch { name = null; }
        f = { parts, name };
      }
    }
  } catch { f = null; }   // RangeError: this client's database does not know the zone
  if (cache.size > 32) cache.clear();
  cache.set(tz, f);
  return f;
}

/** The offset of `tz` at `now` by this client's rules, in minutes — or null if it cannot say. */
export function clientOffsetMin(tz: string, now: Date): number | null {
  const f = fmtsFor(tz);
  if (!f) return null;
  try {
    const v: Record<string, number> = {};
    for (const p of f.parts.formatToParts(now)) if (p.type !== 'literal') v[p.type] = Number(p.value);
    if (![v.year, v.month, v.day, v.hour, v.minute, v.second].every(Number.isFinite)) return null;
    const wall = Date.UTC(v.year, v.month - 1, v.day, v.hour % 24, v.minute, v.second);
    const off = Math.round((wall - Math.floor(now.getTime() / 1000) * 1000) / 60_000);
    return off >= -720 && off <= 840 ? off : null;
  } catch { return null; }
}

/** The zone's short name at `now` by this client's rules, if it is letters ("BST", "CET"); a bare
 *  "GMT-3" style answer is replaced by the signed offset ("-03") to match tzdata's own convention. */
function clientAbbr(tz: string, now: Date, offsetMin: number): string {
  const f = fmtsFor(tz);
  let n = '';
  try { n = f?.name?.formatToParts(now).find((p) => p.type === 'timeZoneName')?.value ?? ''; } catch { n = ''; }
  if (/^[A-Za-z]{2,6}$/.test(n) && !(/^(GMT|UTC)$/.test(n) && offsetMin !== 0)) return n;
  return offsetLabel(offsetMin);
}

/**
 * ★ What to show for the receiver's local time at `now`.
 * @param tz           the server's IANA name, or ''/undefined when it did not send one
 * @param srvOffsetMin the server's own offset (its device's rules)
 * @param srvAbbr      the server's own abbreviation ('' = none)
 */
export function serverZoneAt(now: Date, tz: string | null | undefined,
                             srvOffsetMin: number, srvAbbr: string): ServerZone {
  if (tz) {
    const off = clientOffsetMin(tz, now);
    if (off !== null) {
      // ★ The rules agree: the server's own abbreviation is right and often richer ("EST" where
      //   en-GB Intl would say "GMT-5") — keep it. They disagree: the server's is stale; ours.
      const abbr = off === srvOffsetMin && srvAbbr ? srvAbbr : clientAbbr(tz, now, off);
      return { offsetMin: off, abbr, fromClientRules: true };
    }
  }
  return { offsetMin: srvOffsetMin, abbr: srvAbbr, fromClientRules: false };
}
