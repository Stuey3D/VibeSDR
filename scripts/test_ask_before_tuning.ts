// ★★★ ASK BEFORE TUNING — the clients' half (src/services/dialChat.ts, read by the app AND the browser). Pure: no
// server, no network. The rule itself (ranks, the 15 s window, the 5-minute NO) is the server's and is pinned by
// vibeserver/test-tune-etiquette.cpp; end to end through a real vibeserver: scripts/test-ask-before-tuning.mjs.
//   1. the dial message is READ, never inferred — an old server's silence means "not locked";
//   2. the banner says what to do now, and never runs longer than today's widest banner;
//   3. both web and app gate their tuning sends on the same reader.
import fs from 'node:fs';
import { parseAsk, askLocked, askBannerText, ASK_NUDGE_TEXT, dialSummary, DIAL_PHRASES, bannerLine } from '../src/services/dialChat.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };

// ── 1. reading the dial message ──
{
  const old = parseAsk({ type: 'dial', mode: 'open', tuner: 0, listeners: 3 });
  ok(!askLocked(old) && old.askFirst === false, 'an OLD server (no ask fields): not locked — a new client dims nothing');
  ok(!askLocked(parseAsk({ type: 'dial', tuneLocked: true })), 'tuneLocked without askFirst is ignored (the lock is not in force)');
  const t0 = 1_000_000;
  const open = parseAsk({ type: 'dial', askFirst: true, tuneLocked: true, askState: 'open', askSecs: 12 }, t0);
  ok(askLocked(open) && open.askState === 'open' && open.askUntil === t0 + 12_000, 'asked: locked, waiting, and when the wait ends');
  const no = parseAsk({ type: 'dial', askFirst: true, tuneLocked: true, askState: 'no', askSecs: 300 }, t0);
  ok(no.askState === 'no' && no.askUntil === t0 + 300_000, 'told no: locked until the server\'s five minutes are up');
  const free = parseAsk({ type: 'dial', askFirst: true, tuneLocked: false }, t0);
  ok(!askLocked(free) && free.askFirst === true, 'let in (or the longest-connected): in force, not locked');
  ok(parseAsk({ type: 'dial', askFirst: true, tuneLocked: true, askState: '<b>x</b>' }).askState === '', 'an unknown askState is dropped, not shown');
}

// ── 2. the banner ──
{
  const widestToday = 'SHARED TUNER · ASK TO TUNE · 10/10 👤'.length;
  const base = { askFirst: true, tuneLocked: true } as const;
  const lines = [
    askBannerText({ ...base }, false),
    askBannerText({ ...base, askState: 'open', askUntil: 1 }, false),
    askBannerText({ ...base, askState: 'no', askUntil: Date.now() + 300_000 }, false),
  ];
  ok(lines[0] === 'SHARED TUNER · ASK IN CHAT TO TUNE', `locked: "${lines[0]}"`);
  ok(lines[1] === 'SHARED TUNER · AWAITING A REPLY', `asked: "${lines[1]}"`);
  ok(/^SHARED TUNER · YOURS TO TUNE \d\d:\d\d$/.test(lines[2] ?? ''), `told no: "${lines[2]}" — a clock time, so nothing has to tick`);
  ok(lines.every((l) => (l ?? '').length <= widestToday), `no ask banner is wider than today's widest (${widestToday})`);
  ok(askBannerText({ ...base }, true) === 'ASK IN CHAT TO TUNE', 'tight (SE in Display Zoom): the prefix goes, the words stay');
  ok(askBannerText({ askFirst: true, tuneLocked: false }, false) === null, 'not locked: the ordinary banner');
  ok(ASK_NUDGE_TEXT.length <= widestToday, `the nudge fits: "${ASK_NUDGE_TEXT}"`);
  const d = { mode: 'open', tuner: 0, mine: false, you: 2, listeners: 2, decoding: false };
  ok(dialSummary(d).includes('nobody is tuning — go ahead'), 'unlocked strip unchanged');
  ok(!dialSummary({ ...d, ...base }).includes('go ahead'), 'locked strip never says "go ahead"');
}

// ── 3. the vocabulary and the gates ──
{
  const p = DIAL_PHRASES.find((x) => x.id === 'listening_few_min');
  ok(!!p && /listening/i.test(p.text) && !/do you mind/i.test(p.text), `the gentle NO: "${p?.text}"`);
  ok((bannerLine(9, 'listening_few_min') ?? '').length <= 28, 'it fits the banner when it arrives');
  const app = fs.readFileSync(new URL('../src/services/VibeServerWsClient.ts', import.meta.url), 'utf8');
  const web = fs.readFileSync(new URL('../web/client/src/spectrum.ts', import.meta.url), 'utf8');
  for (const [name, src] of [['app', app], ['web', web]] as const) {
    ok(/this\.tuneLocked = askLocked\(ask\)/.test(src), `${name}: the lock comes from the dial message, through parseAsk`);
    const gated = ['tune(', 'setMode(', 'setBandwidth('].every((m) => {
      const i = src.indexOf(`\n  ${m}`); return i >= 0 && src.slice(i, i + 900).includes('askBlocked()');
    });
    ok(gated, `${name}: tune / setMode / setBandwidth all ask the gate first`);
  }
}

console.log(`test_ask_before_tuning: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
