// test_av_msgs.ts — ★★★ beta97 hotfix THE DATALINK MESSAGES LIST from the server's log (src/services/avMsgs.ts; the server's
// half: vibeserver/test-avi-msglog.cpp). Every message counted (two from one aircraft between updates → two rows), the
// window following the map's time filter, the ceiling by device memory, the backfill + live de-duplicated, the tab's words,
// and an older server's fallback (no log → the live feed, "since opened").   npx tsx scripts/test_av_msgs.ts
import { readFileSync } from 'node:fs';
import { parseAvMsgs, avMsgMerge, avMsgCeiling, avMsgWindowS, avMsgInWindow, avMsgToLive, avMsgLabel, avMsgsQuery, avMsgKey, type AvMsg } from '../src/services/avMsgs';
import { msgFeedUpdate, type LiveMsg } from '../src/services/liveMaps';

let pass = 0, fail = 0;
const ok = (c: unknown, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const now = 1_800_000_000;
const msg = (id: number, ago: number, extra: Partial<AvMsg> = {}): AvMsg => ({ id, t: now - ago, icao: '4CA1B2', reg: '.EI-DCL', flight: 'RYR4KD', src: 'VDL2', label: 'H1', name: 'message to/from crew or ops', ...extra });

console.log('── every message, not each aircraft\'s latest ──');
{
  // the server's page as msgsJson writes it: two messages from one aircraft between two of the app's updates
  const page = parseAvMsgs({ seq: 2, keep: 86400, max: 1500, trunc: false, msgs: [{ id: 2, t: now - 1, icao: '4CA1B2', reg: '.EI-DCL', flight: 'RYR4KD', src: 'VDL2', label: 'QA', name: 'out of gate' },
                                                                                  { id: 1, t: now - 2, icao: '4CA1B2', reg: '.EI-DCL', flight: 'RYR4KD', src: 'VDL2', label: 'H1', text: 'FIRST' }] })!;
  const rows = avMsgMerge([], page.msgs, 500);
  ok(rows.length === 2 && rows[0].id === 2 && rows[1].text === 'FIRST', 'two messages from one aircraft between updates → TWO rows, newest first');
  // the old feed: the same two, seen as one aircraft's count going 0 → 2 between reports — ONE row (why the list showed so few)
  const old = msgFeedUpdate([], new Map(), [{ key: 'a:4CA1B2', who: 'RYR4KD', src: 'VDL2', n: 2, age: 1, label: 'QA' }], now * 1000);
  ok(old.length === 1, `the old feed made ${old.length} row of them (each aircraft's latest on change)`);
  const live: LiveMsg = avMsgToLive(rows[1]);
  ok(live.key === 'a:4CA1B2' && live.who === 'RYR4KD' && live.label === 'H1' && live.text === 'FIRST', 'as the list\'s rows (the aircraft\'s key, so tapping an aircraft still filters to it)');
  ok(avMsgKey(msg(9, 1, { icao: undefined })) === 'a:.EI-DCL', 'no ICAO address: keyed by the registration, as the merged table');
}

console.log('── the window follows the map\'s time filter ──');
{
  ok(avMsgWindowS('1h', now) === 3600 && avMsgWindowS('24h', now) === 86400 && avMsgWindowS('all', now) === 86400, '1 H · 24 H · ALL (all the server keeps: a day)');
  ok(avMsgWindowS('session', now, now - 900) === 900, 'THIS SESSION: since it began');
  const list = [msg(3, 100), msg(2, 4000), msg(1, 90000)];
  ok(avMsgInWindow(list, 3600, now).length === 1 && avMsgInWindow(list, 86400, now).length === 2, 'the list holds only what falls inside the window');
}

console.log('── the ceiling, by the device ──');
{
  ok(avMsgCeiling(1 * 2 ** 30) === 500 && avMsgCeiling(null) === 500, '≈ 500 on a 1 GB phone (and when the memory is not known)');
  ok(avMsgCeiling(4 * 2 ** 30) === 1000 && avMsgCeiling(16 * 2 ** 30) === 1500, 'more on a bigger device; a desktop the server\'s own ceiling');
  const many = Array.from({ length: 800 }, (_, i) => msg(i + 1, 800 - i));
  const kept = avMsgMerge([], many, 500);
  ok(kept.length === 500 && kept[0].id === 800 && kept[499].id === 301, 'the newest 500 kept, the oldest dropped');
}

console.log('── backfill, then live, de-duplicated ──');
{
  const back = Array.from({ length: 5 }, (_, i) => msg(i + 1, 60 - i));
  const live = [msg(4, 56), msg(5, 55), msg(6, 2), msg(7, 1)];   // the first live call overlaps the backfill's newest
  const list = avMsgMerge(avMsgMerge([], back, 500), live, 500);
  ok(list.length === 7 && new Set(list.map((m) => m.id)).size === 7 && list[0].id === 7, 'backfill 1–5 + live 4–7 → 7 messages, each once, newest first');
  ok(avMsgsQuery({ windowS: 3600 }) === '/vibeserver/avmsgs.json?window=3600&max=300' && avMsgsQuery({ windowS: 3600, since: 42 }).endsWith('&since=42'), 'the backfill asks for the window; then only what came after the last number');
}

console.log('── the tab says what it shows ──');
{
  ok(avMsgLabel({ windowS: 3600, shown: 120, capped: false, log: true }) === 'last 1 h', '"MESSAGES · last 1 h"');
  ok(avMsgLabel({ windowS: 86400, shown: 500, capped: true, log: true }) === 'latest 500', '"· latest 500" when the ceiling bites');
  ok(avMsgLabel({ windowS: 86400, shown: 40, capped: false, log: true }) === 'last 24 h', 'ALL: the day the server keeps');
  ok(avMsgLabel({ windowS: 900, shown: 9, capped: false, log: true, session: true }) === 'this session', 'THIS SESSION');
  ok(avMsgLabel({ windowS: 3600, shown: 200, capped: false, log: false }) === 'since opened', 'an older server: "since opened" (the live feed)');
}

console.log('── an older server degrades to the live feed ──');
{
  ok(parseAvMsgs('<html>404</html>') === null && parseAvMsgs({ msgs: [] }) === null, 'not a log page → null (never trusted)');
  ok(parseAvMsgs({ seq: 3, msgs: [{ id: 'x' }, { id: 2, t: now }, null] })!.msgs.length === 1, 'bad entries dropped');
  const sdr = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
  ok(/if \(!r\.ok\) \{ if \(r\.status === 404\) setAvLogOk\(false\); return; \}/.test(sdr) && /const msgsAll = msgFilter\(logMsgs \?\? asMsgs\.current, asMsgKey\);/.test(sdr),
     'a 404 (no log on that server) → the live feed, as before');
  ok(/label: t === 'messages' \? `\$\{TAB_LABEL\[t\]\} \\u00b7 \$\{msgWords\}` : TAB_LABEL\[t\]/.test(sdr), 'the tab: "MESSAGES · <what it shows>"');
  ok(/setInterval\(\(\) => \{ void tick\(\); \}, AV_MSGS_POLL_MS\)/.test(sdr) && /const avLogWanted = airSeaUp && isVibeServer && /.test(sdr), 'asked every 5 s only while a map that can hold datalink messages is up');
}

console.log(`test_av_msgs: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
