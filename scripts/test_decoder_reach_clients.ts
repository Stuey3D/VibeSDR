// test_decoder_reach_clients.ts — V12 (2026-11-03) the CLIENT half of vibe_decoder_reach.h.
//   REACH: a decoder the radio cannot tune comes back `"<id>": false, "<id>Why": "…"` — the app and the web client draw it
//          GREYED with that reason (never hidden, never live): a press says why and starts nothing.
//   REGION: the Setup page and ServerModeScreen read `regionDefaults` — a default of the owner's switch, the reason beside
//          it, the owner's tap recorded either way ("+same" on / "same" off).
//   npx tsx scripts/test_decoder_reach_clients.ts
import { readFileSync } from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { parseDecoderReach, REACH_DECODERS } from '../src/services/sdrTypes';

const root = path.resolve(__dirname, '..');
const read = (p: string) => readFileSync(path.join(root, p), 'utf8');
let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };

// ── the app's parser (sdrTypes.ts) ──
{
  ok(JSON.stringify(parseDecoderReach({ same: false, sameWhy: 'Outside this radio’s locked range' })) === JSON.stringify({ same: 'Outside this radio’s locked range' }),
     'false + a reason: greyed with it');
  ok(Object.keys(parseDecoderReach({ same: false })).length === 0, 'false with no reason (the owner’s block): no reach entry — the key stays hidden');
  ok(Object.keys(parseDecoderReach({ same: true, sameWhy: 'x' })).length === 0, 'true: live, whatever else is sent');
  ok(Object.keys(parseDecoderReach({})).length === 0, 'an older server: nothing greyed');
  ok(Object.keys(parseDecoderReach(null)).length === 0, 'no answer: nothing greyed');
  ok(parseDecoderReach({ navtex: false, navtexWhy: 'a'.repeat(500) }).navtex.length <= 200, 'a reason is clamped');
  for (const id of ['same', 'dsc', 'aprs', 'navtex', 'selcal', 'cw', 'psk', 'jtty'])
    ok((REACH_DECODERS as readonly string[]).includes(id), `the app reads ${id}Why`);
}

// ── the server's table and the clients' lists agree ──
{
  const h = read('android/app/src/main/cpp/vibe_decoder_reach.h');
  const ids = [...h.matchAll(/(?:Entry e\{ |v\.push_back\(\{ )"([a-z]+)"/g)].map((m) => m[1]);
  ok(ids.length >= 8, `the server's table: ${ids.join(', ')}`);
  for (const id of ids) ok((REACH_DECODERS as readonly string[]).includes(id), `the app reads the server's ${id}`);
  const web = read('web/client/src/main.ts');
  const wl = web.match(/const REACH_DECODERS = \[([^\]]+)\]/);
  for (const id of ids) ok(!!wl && wl[1].includes(`'${id}'`), `the web client reads the server's ${id}`);
}

// ── the app draws it greyed, a press says why (ModeSelector + SDRScreen) ──
{
  const ms = read('src/components/ModeSelector.tsx');
  const srv = ms.slice(ms.indexOf('const srvKey = '), ms.indexOf('const adsbOff'));
  const cli = ms.slice(ms.indexOf('const clientKey = '), ms.indexOf('const scannerKey'));
  for (const [name, body] of [['server decoder key', srv], ['client decoder key (NAVTEX)', cli]] as const) {
    ok(/decWhy\?\.\[/.test(body), `${name}: reads decWhy`);
    ok(/why \? \(\) => [^:]*onDecWhy\?\.\(/.test(body), `${name}: a press with a reason goes to onDecWhy, not the decoder`);
    ok(/<NavItem key=\{[a-z]+\} onPress=\{press\}>/.test(body), `${name}: the pad / keyboard press is the same press (NavItem)`);
    ok(/UNREACH/.test(body) && /unavailable: \$\{why\}/.test(body), `${name}: greyed, and says why to a screen reader`);
    ok(/const active = !why &&/.test(body), `${name}: never lit while out of reach`);
  }
  const sd = read('src/screens/SDRScreen.tsx');
  ok(/decWhy: \(client\.current as any\)\?\.isVibe \? decWhy : undefined/.test(sd), 'SDRScreen hands decWhy to the grid');
  ok(/onDecWhy: \(label: string, why: string\) => \{ markInteract\(\); appAlert\(/.test(sd), 'SDRScreen: a press shows the reason (appAlert)');
  for (const id of ['same', 'dsc', 'cw', 'psk', 'jtty', 'selcal']) ok(new RegExp(`o\\?\\.${id} === true \\|\\| !!dw\\.${id}`).test(sd), `SDRScreen: ${id} stays drawn when out of reach`);
  ok(/aprsAvail: \(aprsCap\.on \|\| !!decWhy\.aprs\)/.test(sd), 'SDRScreen: APRS stays drawn when out of reach');
}

// ── the web client draws it greyed, a press says why ──
{
  const web = read('web/client/src/main.ts');
  const click = web.slice(web.indexOf("const mode = b.dataset.dec as 'rtty'"), web.indexOf('decoders!.attach(decExt(mode)'));
  ok(/if \(decWhy\[mode\]\) \{ showPill\(/.test(click) && /return; \}/.test(click), 'web: a press on an out-of-reach key shows the reason and attaches nothing');
  const paint = web.slice(web.indexOf('function decWhyPaint'), web.indexOf('function syncDecButtons'));
  ok(/classList\.toggle\('unreach'/.test(paint) && /aria-disabled/.test(paint) && /b\.title = why/.test(paint), 'web: greyed, aria-disabled, the reason as the title (hover, focus)');
  ok(!/\.hidden\s*=/.test(paint), 'web: never hidden by reach');
  for (const id of ['same', 'dsc', 'cw', 'psk', 'jtty', 'selcal', 'aprs']) ok(new RegExp(`${id}Cap = j\\.${id} === true \\|\\| !!decWhy\\.${id}`).test(web), `web: ${id} stays drawn when out of reach`);
  ok(/#decodersPanel \.btn\.unreach \{ opacity: 0\.45; border-style: dashed; \}/.test(read('web/client/index.html')), 'web: the refused-key look');
}

// ── the Setup page: the region default is a DEFAULT, the owner's tick recorded either way (its real functions) ──
{
  const page = read('android/app/src/main/cpp/vibe_setup_page.h');
  const a = page.indexOf('function modeOwnerSet()'), b = page.indexOf('/** ★★★ CAN THIS MACHINE DECODE AAC AT ALL?');
  ok(a > 0 && b > a, 'the page’s list functions found');
  const ctx: any = { radio: () => ctx.R, R: { blockedModes: '' } };
  vm.createContext(ctx);
  vm.runInContext(page.slice(a, b) + '\nthis.modeBlockSet = modeBlockSet; this.modeRegionOff = modeRegionOff; this.setRD = (o) => { REGION_DEFAULTS = o; };', ctx);
  const off = () => [...ctx.modeBlockSet()].sort().join(',');
  ctx.setRD({ same: 'Off by default here' });
  ctx.R.blockedModes = '';           ok(off() === 'same', 'Region 1, nothing said: SAME off');
  ctx.R.blockedModes = '+same';      ok(off() === '', 'the owner ticked SAME: on (and "+same" is not an off)');
  ctx.R.blockedModes = 'same,rtty';  ok(off() === 'rtty,same', 'the owner unticked SAME: off, their own list kept');
  ctx.R.blockedModes = 'lora+all';   ok(off() === 'lora+all,same', 'a "+" option inside a token is kept');
  ctx.setRD({});
  ctx.R.blockedModes = '+same';      ok(off() === '', 'region unknown: nothing off by default');
  const change = page.slice(page.indexOf('const set = modeOwnerSet();\n      // ★ CHECKED means OFFERED'), page.indexOf('radio().blockedModes = [...set].join(",");\n      modeBlockSummary();\n      if (!el.dataset.on)'));
  ok(/else if \(el\.checked\) \{ set\.delete\(id\); if \(REGION_DEFAULTS\[id\]\) set\.add\("\+" \+ id\); \}/.test(change), 'ticking a region-default switch records "+id"');
  ok(/else \{ set\.delete\("\+" \+ id\); set\.add\(id\); \}/.test(change), 'unticking records "id" (and drops "+id")');
  ok(/data-region-why=/.test(page), 'the reason is drawn beside the switch');
  const sm = read('src/screens/ServerModeScreen.tsx');
  ok(/getRegionDefaults/.test(sm) && /set\.add\('\+' \+ m\.id\)/.test(sm) && /set\.delete\('\+' \+ m\.id\); set\.add\(m\.id\)/.test(sm), 'ServerModeScreen: the same rule, from the engine');
  ok(/\{rdWhy\}<\/Text>/.test(sm), 'ServerModeScreen: the reason beside the switch');
}

console.log(`\ntest_decoder_reach_clients: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
