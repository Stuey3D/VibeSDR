/**
 * "Share a station" in the canned chat — src/services/chatShare.ts (app AND web client).
 *
 * Proves:
 *   • ★★★ NO LABEL LEAKS: a bookmark named something offensive, with a group and a comment, shares as
 *     numbers and ids only — the payload's keys are a fixed whitelist and its JSON never contains the
 *     label. Same for every bookmark the pickers can offer, DAB ones included;
 *   • the payload is what the server's parser accepts (kind, hz, closed-list mode, passband; DAB:
 *     block + sid + eid with the table's centre as hz, so an old server still relays "check out … DAB");
 *   • what is drawn comes from the SERVER's line: name/ensemble/text as sent, the old pre-kind line
 *     still drawable, a DAB line with no usable block dropped;
 *   • [TUNE] asks first on a shared dial somebody else is on, is refused on a spectator dial (unless
 *     admin), never offered for DAB where the receiver cannot play it, and just tunes otherwise;
 *   • source checks: ChatDrawer offers the Share chip only in canned mode, SDRScreen sends the share
 *     built by chatShare (never a bookmark's name), and neither client tunes on RECEIVING a share.
 *
 * Run: node --no-warnings scripts/test_chat_share.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import { registerHooks } from 'node:module';

// ★ src/ imports siblings without an extension (Metro and tsc resolve them); node's ESM loader does
//   not, so resolve './dabBlocks' -> './dabBlocks.ts' for this run only.
registerHooks({
  resolve(spec: string, ctx: unknown, next: (s: string, c: unknown) => unknown) {
    try { return next(spec, ctx); }
    catch (e) {
      if (spec.startsWith('.') && !/\.[cm]?[jt]sx?$/.test(spec)) return next(spec + '.ts', ctx);
      throw e;
    }
  },
} as never);

const cs = await import('../src/services/chatShare.ts');
const {
  shareFromBookmark, shareDab, shareFromTuned, parseShared, sharedStationText, sharedLineText,
  shareTuneStep, shareFreqText, shareSummary,
} = cs;

let fails = 0, passes = 0;
function ok(what: string, cond: boolean, detail = '') {
  if (cond) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}${detail ? `\n   ${detail}` : ''}`);
}

const ALLOWED_KEYS = new Set(['type', 'id', 'kind', 'hz', 'mode', 'bwLo', 'bwHi', 'block', 'sid', 'eid']);
const BAD = 'You Are All Idiots';

// ── ★★★ No label leakage ────────────────────────────────────────────────────────────────────────
{
  const bm = {
    name: BAD, frequency: 96_600_000, mode: 'wfm', group: BAD, comment: BAD, extension: BAD,
    bandwidth_low: -100_000, bandwidth_high: 100_000, scope: '', synced: true,
  };
  const out = shareFromBookmark(bm);
  ok('a bookmark shares', !!out);
  const json = JSON.stringify(out);
  ok('the label is not in the payload', !json.includes(BAD), json);
  ok('the payload keys are the whitelist only',
     Object.keys(out!).every((k) => ALLOWED_KEYS.has(k)), Object.keys(out!).join(','));
  ok('frequency, mode, passband carried', out!.hz === 96_600_000 && out!.mode === 'wfm'
     && out!.bwLo === -100_000 && out!.bwHi === 100_000 && out!.kind === 'bookmark' && out!.id === 'check_out');

  // A DAB bookmark (the server's learnt kind carries sid/eid) — still no label.
  const dabBm = { name: BAD, frequency: 225_648_000, mode: 'dab', sid: 0xc0d2, eid: 0xcc15, comment: BAD };
  const d = shareFromBookmark(dabBm);
  const dj = JSON.stringify(d);
  ok('DAB bookmark: no label', !!d && !dj.includes(BAD), dj);
  ok('DAB bookmark: block, sid, eid, centre hz, mode dab', d!.kind === 'dab' && d!.block === '12B'
     && d!.sid === 0xc0d2 && d!.eid === 0xcc15 && d!.hz === 225_648_000 && d!.mode === 'dab');
  ok('DAB keys whitelisted', Object.keys(d!).every((k) => ALLOWED_KEYS.has(k)));

  // Fuzz: names with every kind of character never appear.
  for (const name of ['<script>x</script>', '"quoted"', 'Ünïcödé 📻', 'a'.repeat(300)]) {
    const s = JSON.stringify(shareFromBookmark({ name, frequency: 7_040_000, mode: 'usb', comment: name } as never));
    ok(`no leak for ${name.slice(0, 20)}`, !s.includes(name));
  }
  // A mode outside the closed list is left out, not sent as text.
  const odd = shareFromBookmark({ frequency: 7_040_000, mode: 'my custom mode' });
  ok('an unknown mode is not sent', !!odd && odd.mode === undefined);
  ok('no frequency = no share', shareFromBookmark({ frequency: 0, mode: 'am' }) === null);
  ok('a DAB bookmark off-block = no share', shareFromBookmark({ frequency: 226_500_000, mode: 'dab' }) === null);
  ok('an inverted passband is dropped', shareFromBookmark({ frequency: 1e6, mode: 'am', bandwidth_low: 5, bandwidth_high: -5 })!.bwLo === undefined);
}

// ── What is playing now ─────────────────────────────────────────────────────────────────────────
{
  const a = shareFromTuned({ frequency: 96_600_000, mode: 'wfm', bandwidthLow: -1e5, bandwidthHigh: 1e5 }, null);
  ok('tuned analogue', a!.kind === 'bookmark' && a!.hz === 96_600_000 && a!.bwHi === 1e5);
  const b = shareFromTuned({ frequency: 96_600_000, mode: 'wfm' }, { channel: '11D', sid: 0xc1ce, eid: 0xce15 });
  ok('tuned DAB = the service, not the analogue dial underneath', b!.kind === 'dab' && b!.block === '11D' && b!.sid === 0xc1ce);
  ok('shareDab: unknown block', shareDab('12Z') === null);
  ok('shareDab: multiplex only', shareDab('12b')!.sid === undefined && shareDab('12b')!.block === '12B');
}

// ── Drawing what the server relayed ─────────────────────────────────────────────────────────────
{
  const s = parseShared({ type: 'said', from: 3, id: 'check_out', hz: 96_600_000, mode: 'wfm', kind: 'bookmark',
                          name: 'Heart', nameSrc: 'station', text: 'shared 96.600 MHz WFM — Heart' });
  ok('parse analogue', !!s && s.kind === 'bookmark' && s.name === 'Heart');
  ok('station text', sharedStationText(s!) === '96.600 MHz WFM — Heart', sharedStationText(s!));
  ok('line text', sharedLineText(s!) === '📻 shared 96.600 MHz WFM — Heart');
  const bare = parseShared({ hz: 96_600_000, mode: 'wfm' });
  ok('an old (pre-kind) line is still drawable, with no name', !!bare && sharedStationText(bare) === '96.600 MHz WFM');
  const d = parseShared({ hz: 225_648_000, mode: 'dab', kind: 'dab', block: '12B', sid: 49362, name: 'Heart', ensemble: 'D1 National' });
  ok('DAB station text', sharedStationText(d!) === 'DAB Heart — 12B (225.648 MHz)', sharedStationText(d!));
  const m = parseShared({ hz: 225_648_000, mode: 'dab', kind: 'dab', block: '12B', ensemble: 'D1 National' });
  ok('DAB multiplex text', sharedStationText(m!) === 'DAB 12B (225.648 MHz) — D1 National', sharedStationText(m!));
  ok('an old server\'s "check out 225.65 DAB" is a block', parseShared({ hz: 225_650_000, mode: 'dab' })!.block === '12B');
  ok('a DAB line with no block is dropped', parseShared({ kind: 'dab', hz: 1 }) === null);
  ok('an analogue line with no frequency is dropped', parseShared({ kind: 'bookmark' }) === null);
  ok('unknown mode drawn without a label', sharedStationText(parseShared({ hz: 1e6, mode: 'zzz' })!) === '1.000 MHz');
}

// ── [TUNE] ──────────────────────────────────────────────────────────────────────────────────────
{
  const fm = parseShared({ hz: 96_600_000, mode: 'wfm' })!;
  const dab = parseShared({ kind: 'dab', block: '12B', hz: 225_648_000 })!;
  ok('ordinary receiver: tune', shareTuneStep(fm, null) === 'tune');
  ok('exclusive: tune', shareTuneStep(fm, { mode: 'exclusive', listeners: 1, mine: false }) === 'tune');
  ok('shared, alone: tune', shareTuneStep(fm, { mode: 'shared', listeners: 1, mine: false }) === 'tune');
  ok('shared, somebody else on it: ASK', shareTuneStep(fm, { mode: 'shared', listeners: 3, mine: false }) === 'ask');
  ok('shared, you tuned last: tune', shareTuneStep(fm, { mode: 'shared', listeners: 3, mine: true }) === 'tune');
  ok('spectator: refused', shareTuneStep(fm, { mode: 'spectator', listeners: 2, mine: false }) === 'refused');
  ok('spectator, admin: as open', shareTuneStep(fm, { mode: 'spectator', listeners: 1, mine: false }, { admin: true }) === 'tune');
  ok('DAB where DAB is not possible: no-dab', shareTuneStep(dab, null, { dabCapable: false }) === 'no-dab');
}

// ── Formatting parity with the server (vibe_chat_share.h freqText) ──────────────────────────────
ok('96.600 MHz', shareFreqText(96_600_000) === '96.600 MHz');
ok('198 kHz', shareFreqText(198_000) === '198 kHz');
ok('7.5 kHz', shareFreqText(7_500) === '7.5 kHz');
ok('summary', shareSummary(shareDab('12B', 0xc0d2)!) === 'DAB 12B (225.648 MHz) · C0D2');

// ── Source checks: the clients use this, and never tune on receipt ──────────────────────────────
{
  const drawer = readFileSync(new URL('../src/components/ChatDrawer.tsx', import.meta.url), 'utf8');
  const screen = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
  const web = readFileSync(new URL('../web/client/src/chat.ts', import.meta.url), 'utf8');
  ok('ChatDrawer: a Share chip in canned mode', /Share a station/.test(drawer) && /isCanned && !!shareItems/.test(drawer));
  ok('ChatDrawer: TUNE on a shared line calls onShareTune', /onShareTune\?\.\(m\.share!?\)/.test(drawer));
  ok('SDRScreen: shares are built by chatShare', /shareFromBookmark\(/.test(screen) && /shareFromTuned\(/.test(screen));
  ok('SDRScreen: the received share is parsed, not tuned', /parseShared\(/.test(screen)
     && !/onSaid[\s\S]{0,1500}(onSearchTune|dabGoTo)\(/.test(screen.slice(screen.indexOf('onSaid: (from'), screen.indexOf('onSaid: (from') + 1600)));
  ok('web: shares built by chatShare', /shareFromBookmark\(/.test(web) && /shareFromTuned\(/.test(web));
  ok('web: received share parsed', /parseShared\(/.test(web));
  // The picker shows the user's own label locally; the SEND path must take the built share, never `.name`.
  const sendFn = web.slice(web.indexOf('function sendShare'), web.indexOf('function sendShare') + 600);
  ok('web: sendShare sends the ShareOut only', sendFn.length > 0 && !/\.name/.test(sendFn), sendFn);
}

console.log(`test_chat_share: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
