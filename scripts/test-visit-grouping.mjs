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
function groupVisits(list) {
  const bySession = new Map(); const out = [];
  for (const c of list) {
    const sess = String(c.session || '');
    if (!sess) { out.push({ ...c, radios: c.radio ? [c.radio] : [] }); continue; }
    const had = bySession.get(sess);
    if (!had) { const v = { ...c, radios: c.radio ? [c.radio] : [] }; bySession.set(sess, v); out.push(v); continue; }
    had.at = Math.min(had.at || 0, c.at || 0);
    had.end = (!had.end || !c.end) ? 0 : Math.max(had.end, c.end);
    had.bytes = (had.bytes || 0) + (c.bytes || 0);
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
console.log(fail ? `\n${fail} failed` : '\npassed');
process.exit(fail ? 1 : 0);
