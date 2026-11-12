/**
 * The receiver's local time from the CLIENT's tz rules (src/utils/serverZone.ts, 2026-11-12).
 *
 * Kiko's receiver (an old Android TV box in Paraná) sent tzOffsetMin -120 "BRST" in November: its tzdata predates
 * Brazil abolishing DST in 2019. With the zone name ("tz") the client works the offset out from its own rules and
 * shows UTC-3. Silent — nothing played.
 *
 * Run: node --no-warnings scripts/test_server_zone.ts
 */
import { serverZoneAt, clientOffsetMin, offsetLabel } from '../src/utils/serverZone.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
function ok(what: string, c: boolean, detail = '') {
  if (c) { passes++; return; }
  fails++;
  console.error(`FAIL ${what} ${detail}`);
}

const NOV = new Date(Date.UTC(2026, 10, 12, 22, 20, 0));   // 22:20 UTC — the evening of the report
const JUL = new Date(Date.UTC(2026, 6, 1, 12, 0, 0));

// ── Kiko: stale server rules, current client rules ────────────────────────────────────────────
{
  const z = serverZoneAt(NOV, 'America/Sao_Paulo', -120, 'BRST');
  eq('Sao_Paulo in November is UTC-3 whatever the server says', z.offsetMin, -180);
  ok('…and the stale "BRST" is not shown', z.abbr !== 'BRST', z.abbr);
  ok('…the label is "-03" or "BRT"', z.abbr === '-03' || z.abbr === 'BRT', z.abbr);
  eq('…from the client\'s rules', z.fromClientRules, true);
  const wall = new Date(NOV.getTime() + z.offsetMin * 60_000).toISOString().slice(11, 16);
  eq('…so the receiver\'s clock reads 19:20', wall, '19:20');
}

// ── Rules agree: the server's own abbreviation is kept ────────────────────────────────────────
eq('London in summer, agreeing, keeps "BST"', serverZoneAt(JUL, 'Europe/London', 60, 'BST'),
   { offsetMin: 60, abbr: 'BST', fromClientRules: true });
eq('New York in winter, agreeing, keeps "EST"', serverZoneAt(NOV, 'America/New_York', -300, 'EST'),
   { offsetMin: -300, abbr: 'EST', fromClientRules: true });
eq('India +5:30 (a half-hour zone)', clientOffsetMin('Asia/Kolkata', NOV), 330);
eq('Chathams +13:45 in November (DST, three-quarter hour)', clientOffsetMin('Pacific/Chatham', NOV), 825);
eq('Etc/UTC is 0', clientOffsetMin('Etc/UTC', NOV), 0);
{
  // A server that agreed but sent no abbreviation: ours, never a "GMT-5" style string.
  const z = serverZoneAt(NOV, 'America/New_York', -300, '');
  ok('no server abbreviation -> letters or signed offset', /^[A-Za-z]{2,6}$/.test(z.abbr) || z.abbr === '-05', z.abbr);
  ok('…never "GMT-5"', !/^GMT[+-]/.test(z.abbr), z.abbr);
}

// ── No tz: the old behaviour, exactly ─────────────────────────────────────────────────────────
eq('no tz (older server) -> the server\'s figures', serverZoneAt(NOV, undefined, -120, 'BRST'),
   { offsetMin: -120, abbr: 'BRST', fromClientRules: false });
eq('empty tz -> the server\'s figures', serverZoneAt(NOV, '', 60, ''),
   { offsetMin: 60, abbr: '', fromClientRules: false });

// ── A zone this client does not know: the fallback ────────────────────────────────────────────
eq('unknown zone -> the server\'s figures', serverZoneAt(NOV, 'Mars/Olympus_Mons', -120, 'BRST'),
   { offsetMin: -120, abbr: 'BRST', fromClientRules: false });
eq('…asked again (cached refusal) -> the same', serverZoneAt(NOV, 'Mars/Olympus_Mons', -120, 'BRST').fromClientRules, false);
eq('garbage zone -> fallback', serverZoneAt(NOV, '../../etc/passwd', 0, 'UTC').fromClientRules, false);

// ── The label helper ──────────────────────────────────────────────────────────────────────────
eq('offsetLabel -180', offsetLabel(-180), '-03');
eq('offsetLabel +330', offsetLabel(330), '+05:30');
eq('offsetLabel 0', offsetLabel(0), 'UTC');

console.log(`server zone: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
