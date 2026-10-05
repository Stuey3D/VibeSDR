// test_navtex.ts — NAVTEX message blocks from the decoder's character stream (src/utils/navtex.ts, 2026-10-05).
// Real-shaped traffic: CR CR LF line ends, '_' for a lost character (fsk_decoder.cpp), chunks split anywhere.
import { NavtexAssembler, NAVTEX_END_LOST_MS, navtexTitle, navtexLostPct, navtexBody, navtexFileName, navtexFileText }
  from '../src/utils/navtex.ts';
let pass = 0, fail = 0;
const ok = (c: boolean, m: string) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m); } };

const T0 = Date.UTC(2026, 9, 5, 16, 1, 30);
const GALE = 'ZCZC GB98\r\r\nWZ 1234\r\r\nGALE WARNING\r\r\nVIKING NORTH UTSIRE SOUTHWEST 7 TO SEVERE GALE 9\r\r\nNNNN\r\r\n';

/** Feed `s` in chunks of `n` characters, one second apart. */
function feed(a: NavtexAssembler, s: string, n: number, t0 = T0): number {
  let t = t0;
  for (let i = 0; i < s.length; i += n) { a.push(s.slice(i, i + n), t); t += 1000; }
  return t;
}

// 1. Standing by: nothing yet.
{
  const a = new NavtexAssembler();
  ok(a.live === null && a.prev === null && !a.receiving, 'empty assembler is standing by');
}

// 2. A clean message, every chunk size (a header or trailer split across chunks must still be found).
for (const n of [1, 2, 3, 5, 7, 13, 1000]) {
  const a = new NavtexAssembler();
  feed(a, GALE, n);
  const m = a.live;
  ok(!!m && m.done && !m.endLost && !m.startLost, `clean message complete (chunk ${n})`);
  ok(m?.id === 'GB98' && m.station === 'G' && m.subject === 'B' && m.serial === '98', `header parsed (chunk ${n})`);
  ok(m?.text === 'WZ 1234\nGALE WARNING\nVIKING NORTH UTSIRE SOUTHWEST 7 TO SEVERE GALE 9', `body, CR CR LF folded (chunk ${n}): ${JSON.stringify(m?.text)}`);
  ok(!a.receiving, `standing by after NNNN (chunk ${n})`);
  ok(a.prev === null, `no PREV after the first message (chunk ${n})`);
}

// 3. Title, quality, body, file.
{
  const a = new NavtexAssembler();
  feed(a, GALE, 4);
  const m = a.live!;
  ok(navtexTitle(m) === 'Station G · Met warning · #98', 'title: ' + navtexTitle(m));
  ok(navtexLostPct(m) === 0, 'no characters lost');
  ok(navtexBody(m).startsWith('ZCZC GB98\n') && navtexBody(m).endsWith('\nNNNN'), 'body framed by ZCZC / NNNN');
  ok(navtexFileName(m) === 'navtex_GB98_2026-10-05T1601Z.txt', 'file name: ' + navtexFileName(m));
  ok(navtexFileText(m).startsWith('NAVTEX · Station G · Met warning · #98\nReceived 2026-10-05 16:01 UTC'), 'file header');
}

// 4. While a message is arriving it is LIVE and grows; the next message moves the last one to PREV.
{
  const a = new NavtexAssembler();
  let t = feed(a, GALE, 6);
  a.push('ZCZC EA12\r\r\nNAV WARNING 345', t);
  ok(a.receiving, 'second message is arriving');
  ok(a.live?.id === 'EA12' && a.live.text === 'NAV WARNING 345' && !a.live.done, 'LIVE shows the growing message');
  ok(a.prev?.id === 'GB98', 'the finished one moved to PREV');
  t = feed(a, ' WRECK IN POSITION\r\r\nNNNN\r\r\n', 3, t + 1000);
  ok(a.live?.id === 'EA12' && a.live.done && a.prev?.id === 'GB98', 'after NNNN: LIVE = the new one, PREV = the old');
  feed(a, 'ZCZC KE01\r\r\nTEST\r\r\nNNNN\r\r\n', 5, t + 1000);
  ok(a.live?.id === 'KE01' && a.prev?.id === 'EA12', 'only ONE previous is kept, like WEFAX');
  ok(navtexTitle(a.live!) === 'Station K · Weather forecast · #01', 'E = forecast: ' + navtexTitle(a.live!));
}

// 5. Damaged header: one ZCZC letter lost, and lost id characters.
for (const hdr of ['ZC_C GB98', '_CZC GB98', 'ZCZ_ GB98', 'ZCZC G_98', 'ZCZC _B98', 'ZCZC GB9_', 'ZCZCGB98', 'ZCZC  GB98']) {
  const a = new NavtexAssembler();
  feed(a, hdr + '\r\r\nGALE WARNING FORTIES\r\r\nNNNN\r\r\n', 4);
  ok(!!a.live && !a.live.startLost && a.live.done, `damaged header "${hdr}" still starts a message`);
}
{
  const a = new NavtexAssembler();
  feed(a, 'ZCZC G_98\r\r\nGALE\r\r\nNNNN\r\r\n', 3);
  ok(a.live?.subject === null && navtexTitle(a.live!) === 'Station G · Subject ? · #98', 'lost subject shown as ?: ' + navtexTitle(a.live!));
  ok(navtexFileName(a.live!).startsWith('navtex_Gx98_'), 'lost id char → x in the file name: ' + navtexFileName(a.live!));
}
// …but two lost ZCZC letters, a space in it, or a mostly-lost id is NOT a header.
for (const bad of ['Z__C GB98', 'ZC C GB98', 'ZCZC ____', 'ZCZC _ _9', 'ZCZE GB98X']) {
  const a = new NavtexAssembler();
  feed(a, bad + '\r\r\n', 3);
  ok(a.live === null, `"${bad}" is not a header`);
}

// 6. Damaged trailer: one N lost still ends it; another letter does not.
{
  const a = new NavtexAssembler();
  feed(a, 'ZCZC GB98\r\r\nGALE\r\r\nNN_N\r\r\n', 2);
  ok(!!a.live?.done && !a.live.endLost && !a.receiving, 'NN_N ends the message');
  const b = new NavtexAssembler();
  feed(b, 'ZCZC GB98\r\r\nPENNNE ISLAND\r\r\n', 2);
  ok(b.receiving && !b.live?.done, 'NNN inside a word does not end it');
}

// 7. Trailer lost: the next header ends it ("[end lost]"), or 75 s of silence does.
{
  const a = new NavtexAssembler();
  let t = feed(a, 'ZCZC GB98\r\r\nGALE WARNING\r\r\n', 4);
  t = feed(a, 'ZCZC GA99\r\r\nFOG\r\r\nNNNN\r\r\n', 4, t + 5000);
  ok(a.prev?.id === 'GB98' && a.prev.endLost && navtexBody(a.prev).endsWith('[end lost]'), 'next header ends a message whose NNNN was lost');
  ok(a.live?.id === 'GA99' && a.live.done && !a.live.endLost, 'and the new one is clean');
}
{
  const a = new NavtexAssembler();
  const t = feed(a, 'ZCZC GB98\r\r\nGALE WARNING\r\r\n', 4);
  ok(!a.tick(t + NAVTEX_END_LOST_MS - 2000) && a.receiving, 'still receiving before the timeout');
  ok(a.tick(t + NAVTEX_END_LOST_MS + 1000), 'timeout fires');
  ok(!a.receiving && a.live?.id === 'GB98' && a.live.endLost && a.live.done, 'silence ends it, end lost');
  ok(!a.tick(t + 3 * NAVTEX_END_LOST_MS), 'a second tick changes nothing');
}

// 8. Joined mid-message: "[start lost]", then the next header moves it to PREV.
{
  const a = new NavtexAssembler();
  let t = feed(a, 'SOUTHWEST 7 TO SEVERE GALE 9 LATER\r\r\nNNNN\r\r\n', 5);
  ok(!!a.live?.startLost && a.live.done && navtexBody(a.live).startsWith('[start lost]'), 'text before any ZCZC is a [start lost] message');
  ok(navtexTitle(a.live!) === 'Start lost' && navtexFileName(a.live!).startsWith('navtex_partial_'), 'start-lost title / file name');
  feed(a, GALE, 7, t + 1000);
  ok(a.live?.id === 'GB98' && !!a.prev?.startLost, 'the fragment becomes PREV');
}
{
  const a = new NavtexAssembler();
  feed(a, 'SOUTHWEST 7 TO SEVERE GALE 9 LATER', 4);
  ok(a.receiving && !!a.live?.startLost && !a.live.done, 'a growing fragment shows live');
}
// A few characters of noise between broadcasts are not a message.
{
  const a = new NavtexAssembler();
  feed(a, GALE + 'E_T 3\r\r\n', 3);
  ok(a.live?.id === 'GB98' && !a.receiving, 'noise after NNNN is not a message');
  feed(a, 'ZCZC GB99\r\r\nX\r\r\nNNNN\r\r\n', 3, T0 + 100_000);
  ok(a.prev?.id === 'GB98', 'and is not kept as PREV');
}

// 9. Quality: the share of lost characters.
{
  const a = new NavtexAssembler();
  feed(a, 'ZCZC GB98\r\r\nGA_E WA_NING\r\r\nNNNN\r\r\n', 4);
  ok(navtexLostPct(a.live!) === 18, 'lost % = 2 of 11: ' + navtexLostPct(a.live!));
}

// 9b. The server's own FEC count (VibeServer op 0x07, 2026-10-05), sent behind the text with its NNNN.
{
  const a = new NavtexAssembler();
  feed(a, 'ZCZC GB98\r\r\nGA_E WARNING\r\r\nNNNN\r\r\n', 4);
  ok(navtexTitle(a.live!) === 'Station G · Met warning · #98', 'no count yet: the plain title');
  const v = a.version;
  ok(a.setFec({ clean: 14, repaired: 2, lost: 1 }) && a.version > v, 'the count attaches to the message NNNN closed');
  ok(navtexTitle(a.live!) === 'Station G · Met warning · #98 · 2 repaired · 1 lost', 'title: ' + navtexTitle(a.live!));
  ok(navtexLostPct(a.live!) === null, 'and the _-derived % steps aside (one answer, not two)');
  ok(navtexFileText(a.live!).startsWith('NAVTEX · Station G · Met warning · #98 · 2 repaired · 1 lost\nReceived '),
     'the saved file carries it: ' + JSON.stringify(navtexFileText(a.live!).split('\n').slice(0, 2)));
  ok(!a.setFec({ clean: 1, repaired: 0, lost: 0 }), 'never twice for one message');
  // The next message arrives: the count is its own, and PREV keeps the old one.
  feed(a, 'ZCZC GA01\r\r\nNO MESSAGES\r\r\nNNNN\r\r\n', 5, T0 + 600_000);
  ok(a.setFec({ clean: 20, repaired: 0, lost: 0 }), 'a second message takes a second count');
  ok(navtexTitle(a.live!) === 'Station G · Nav warning · #01 · no errors', 'all clean says so: ' + navtexTitle(a.live!));
  ok(navtexTitle(a.prev!).endsWith('2 repaired · 1 lost'), 'PREV keeps its own count');
}
{
  // A message closed "[end lost]" (no NNNN) is not the server's message: no count lands on it.
  const a = new NavtexAssembler();
  feed(a, 'ZCZC GB98\r\r\nGALE\r\r\nZCZC GB99\r\r\nX', 4);
  ok(a.prev?.endLost === true && !a.setFec({ clean: 5, repaired: 0, lost: 0 }), 'no count on an [end lost] message');
  // Nor on a "[start lost]" fragment: the server counted from a boundary this box never saw.
  const b = new NavtexAssembler();
  feed(b, 'VIKING NORTH UTSIRE SOUTHWEST 7 TO SEVERE GALE 9\r\r\nNNNN\r\r\n', 4);
  ok(b.live?.startLost === true && !b.setFec({ clean: 5, repaired: 0, lost: 0 }), 'no count on a [start lost] fragment');
  ok(!new NavtexAssembler().setFec({ clean: 1, repaired: 0, lost: 0 }), 'nothing to attach to: ignored');
}

// 10. reset() clears everything (CLR / reopen).
{
  const a = new NavtexAssembler();
  feed(a, GALE + 'ZCZC GA01\r\r\nX', 4);
  const v = a.version;
  a.reset();
  ok(a.live === null && a.prev === null && !a.receiving && a.version > v, 'reset clears and bumps the version');
}

console.log(`navtex: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
