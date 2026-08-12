// ★★★ ONE VISIT IS ONE ROW, AND IT NAMES EVERY RADIO THEY TRIED.
//
// Each radio is its own process and logs its own connection, so a person looking at two receivers
// arrived in the admin log as two connections. With a visit now carrying ONE session id across
// radios (visitSessionId in main.ts), those rows fold back into the single event they describe.
// Stuart, 2026-08-11: "if I go into the Airspy and then use the back button and then switch to the
// SDRPlay that should only count 1 session... also if they do use multiple radios then show both."
//
// ★★ The last two cases are the ones that make this safe rather than merely tidy: session-less
//    refusals must NEVER be folded (two of them from one address is what a scan looks like), and
//    two genuine visits from one address must stay two rows.
//
// ★ This mirrors groupVisits() in web/client/src/admin.ts. Keep them in step.
/**
 * ★★★ A SESSION ID IS NOT A VISIT, AND A GAP IS WHAT SEPARATES THEM.
 *
 *     This merged every leg sharing a session id however far apart they were, and the visit's
 *     duration is the SPAN from first open to last close — so a browser tab left open, reconnecting
 *     for a minute every few minutes, became ONE visit whose clock ran across all the idle time
 *     between. Measured on the demo, 2026-08-12: 195.180.34.4 logged twelve legs of 20-61 seconds
 *     over an hour and forty; about NINE MINUTES on the radio, displayed as 1h39 and still rising
 *     while every leg read "closed". Stuart saw 2h15 on a server with a 30-MINUTE LIMIT, which
 *     makes the limit itself look broken — and it was working perfectly.
 *     ★ It also explains rows "disappearing and reappearing": each new leg rewrites the visit it
 *       is folded into, so a row moves and its times change under you.
 *
 * ★★ THE SPAN IS STILL RIGHT INSIDE A VISIT — do not be tempted to sum the legs. A visit is
 *    normally TWO CONCURRENT SOCKETS (spectrum and audio); summing them would double every
 *    duration, which is the bug this rule was written to avoid in the first place.
 *
 * ★ So: same session AND resuming within kVisitGapSec of the last leg ending. A reconnect across a
 *   network blip comes back in seconds; two minutes of nothing is someone who left and returned.
 *   The server's own idle grace is 300 s, so this is deliberately tighter — it describes a PERSON
 *   coming back, not a radio being released.
 */
const VISIT_GAP_SEC = 120;

function groupVisits(list) {
  const bySession = new Map();
  const out = [];
  for (const c of list) {
    const sess = String(c.session || '');
    if (!sess) { out.push({ ...c, radios: c.radio ? [c.radio] : [] }); continue; }
    const had = bySession.get(sess);
    // ★★★ TESTED IN BOTH DIRECTIONS, BECAUSE THE LOG ARRIVES NEWEST FIRST. My first cut only asked
    //     "does this leg START long after the visit ENDED" — which is never true when the list runs
    //     backwards in time, so the rule silently did nothing and the 2h32 row survived the fix
    //     (Stuart: "nope, rebooted and reloaded"). ConnLog::json() says "Newest first, capped" and
    //     it means it. The min/max merge below is order-agnostic; the gap test has to be too.
    // ★ `!had.end` is a LIVE leg — never split from it: the visit is still happening.
    const gapped = had && (
         (!!had.end && (c.at || 0) - had.end > VISIT_GAP_SEC)     // arrives long after it ended
      || (!!c.end && (had.at || 0) - c.end > VISIT_GAP_SEC));     // ended long before it began
    if (!had || gapped) {
      const v = { ...c, radios: c.radio ? [c.radio] : [] };
      bySession.set(sess, v);
      out.push(v);
      continue;
    }
    had.at = Math.min(had.at || 0, c.at || 0);
    // ★ A still-open leg (end 0) wins: the visit is LIVE, however many closed legs precede it.
    had.end = (!had.end || !c.end) ? 0 : Math.max(had.end, c.end);
    had.bytes = (had.bytes || 0) + (c.bytes || 0);
    // The most recent ending is the one worth showing — "banned" after two clean legs is the news.
    if (c.end && c.end >= (had.end || 0)) had.reason = c.reason;
    if (c.radio && !had.radios.includes(c.radio)) had.radios.push(c.radio);
  }
  return out;
}
let fail = 0;
const ok = (c, w) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${w}`); if (!c) fail++; };

// One person: landing -> Airspy (60s) -> back -> RSP (120s). One session id.
let g = groupVisits([
  { session:'v1', at:100, end:160, radio:'Airspy HF+',   reason:'closed' },
  { session:'v1', at:170, end:290, radio:'SDRplay RSP1B', reason:'closed' },
]);
ok(g.length === 1, 'two radios in one visit collapse to ONE row');
ok(g[0].radios.join(' -> ') === 'Airspy HF+ -> SDRplay RSP1B', 'both radios shown, in visit order');
ok(g[0].at === 100 && g[0].end === 290, 'span is first-open to last-close (190s), not the sum');

// A still-open leg means the visit is live, even after a closed one.
g = groupVisits([
  { session:'v2', at:10, end:20, radio:'A', reason:'closed' },
  { session:'v2', at:25, end:0,  radio:'B', reason:'' },
]);
ok(g.length === 1 && g[0].end === 0, 'a live leg keeps the whole visit live');

// Session-less refusals are NEVER folded — two of them is what a scan looks like.
g = groupVisits([
  { session:'', at:1, end:1, ip:'8.8.8.8', reason:'banned' },
  { session:'', at:2, end:2, ip:'8.8.8.8', reason:'banned' },
]);
ok(g.length === 2, 'two session-less refusals stay two rows');

// Separate visits from one address stay separate.
g = groupVisits([
  { session:'a', at:1, end:9, radio:'A', reason:'closed' },
  { session:'b', at:20, end:30, radio:'A', reason:'closed' },
]);
ok(g.length === 2, 'two visits with different session ids stay two rows');

// ★★★ THE ONE THAT WAS SHIPPING WRONG. Real data from the demo, 2026-08-12: one session id, legs
//     of 20-61 seconds, minutes of nothing between them. Grouped by session ALONE they became a
//     single visit spanning 1h39 and still growing on a server with a 30-minute limit — which
//     reads as the limit being broken, when it was the LOG that was wrong.
g = groupVisits([
  { at: 100, end: 160, session: 'S', radio: 'a' },      // one minute
  { at: 1200, end: 1260, session: 'S', radio: 'a' },    // back 17 minutes later
  { at: 2400, end: 2460, session: 'S', radio: 'a' },    // and again
]);
ok(g.length === 3, 'legs separated by long gaps are SEPARATE visits, not one long one');
ok(g.every((v) => v.end - v.at === 60), 'each visit shows its own minute, not the span of all three');

// ★ And a reconnect across a blip is still ONE visit — the rule must not split a genuine session.
g = groupVisits([
  { at: 100, end: 160, session: 'T', radio: 'a' },
  { at: 165, end: 400, session: 'T', radio: 'a' },      // five seconds later
]);
ok(g.length === 1, 'a reconnect within the gap stays one visit');
ok(g[0].end - g[0].at === 300, 'and spans first-open to last-close');


// ★★★ THE ORDER THE SERVER ACTUALLY SENDS: NEWEST FIRST. ConnLog::json() is documented "Newest
//     first", and a gap rule that only looks forwards does nothing at all against it — which is
//     exactly how the 2h32 row survived being "fixed" once already.
g = groupVisits([
  { at: 2400, end: 2460, session: 'S', radio: 'a' },   // newest
  { at: 1200, end: 1260, session: 'S', radio: 'a' },
  { at: 100,  end: 160,  session: 'S', radio: 'a' },   // oldest
]);
ok(g.length === 3, 'newest-first legs with long gaps are still THREE visits');
ok(g.every((v) => v.end - v.at === 60), 'and each keeps its own minute');

console.log(fail ? `\n${fail} failed` : '\npassed');
process.exit(fail ? 1 : 0);
