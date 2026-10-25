// ★★★ ASK BEFORE TUNING — the clients' half (src/services/dialChat.ts, read by the app AND the browser). Pure: no
// server, no network. The rule itself (ranks, the 15 s window, the 5-minute NO) is the server's and is pinned by
// vibeserver/test-tune-etiquette.cpp; end to end through a real vibeserver: scripts/test-ask-before-tuning.mjs.
//   1. the dial message is READ, never inferred — an old server's silence means "not locked";
//   2. the banner says what to do now, and never runs longer than today's widest banner;
//   3. both web and app gate their tuning sends on the same reader.
import fs from 'node:fs';
import { parseAsk, askLocked, askBannerText, ASK_NUDGE_TEXT, ASK_SHARED_SETTINGS, dialSummary, DIAL_PHRASES, bannerLine } from '../src/services/dialChat.ts';

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

// ── 4. "lock gain too" (Stuart, 2026-10-21): ONE list of the room's receiver settings, in three places ──
{
  const read = (f: string) => fs.readFileSync(new URL(`../${f}`, import.meta.url), 'utf8');
  const shim = read('android/app/src/main/cpp/local_sdr_shim.cpp');
  const s = shim.indexOf('kGated = {'), e = shim.indexOf('};', s);
  const server = [...shim.slice(s, e).matchAll(/"([A-Za-z_]+)"/g)].map((m) => m[1])
    // ★ 'adsb' and 'ism' (V12) are dial moves like 'dab': the client gates them in adsb()/ism() with askBlocked, not by this list.
    .filter((t) => !['tune', 'mode', 'bandwidth', 'dab', 'dab_service', 'adsb', 'ism', 'iqtune'].includes(t)).sort();
  const jr = read('spike/WristSDR/WristSDR/UberClient.swift');
  const js = jr.indexOf('askSharedSettings: Set<String> = ['), je = jr.indexOf(']', js + 40);
  const jrList = [...jr.slice(js, je).matchAll(/"([A-Za-z_]+)"/g)].map((m) => m[1]).sort();
  const app = [...ASK_SHARED_SETTINGS].sort();
  ok(server.length >= 20 && server.join() === app.join(), `the server and the app/web refuse the same ${app.length} settings`);
  ok(jrList.join() === app.join(), 'Jr refuses the same list');
  for (const t of ['gain', 'agc', 'rsp_control', 'ahf_control', 'airspy_control', 'hackrf_control', 'tunerbw', 'nr', 'notch', 'deemph', 'stereo'])
    if (!app.includes(t)) ok(false, `${t} is missing from the locked list`);
  for (const t of ['zoom', 'reset', 'squelch', 'fftRate', 'set_rate', 'rdsx', 'say', 'state', 'iqout'])
    ok(!app.includes(t) && !server.includes(t), `${t} stays free (a view, or this listener's own)`);
  const appWs = read('src/services/VibeServerWsClient.ts'), web = read('web/client/src/spectrum.ts');
  ok(/_sendCtl\(obj[^)]*\) \{\s*\/\/[^\n]*\n\s*if \(this\.tuneLocked && ASK_SHARED_SETTINGS\.has/.test(appWs), 'app: the control sender drops a locked setting');
  ok(/_send\(obj[^)]*\) \{[\s\S]{0,400}if \(this\.tuneLocked && ASK_SHARED_SETTINGS\.has/.test(web), 'web: the one sender drops a locked setting');
}

console.log(`test_ask_before_tuning: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
