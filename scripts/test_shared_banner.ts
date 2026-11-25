/**
 * The SHARED TUNER banner carries the head-count in EVERY state (src/services/dialChat.ts sharedBannerWords — Stuart,
 * 2026-11-25: "the user count is missing on the ask in chat to tune message"), and never truncates mid-word: the
 * "SHARED TUNER · " prefix goes first, on a tight layout and whenever the line would outgrow the fitted one.
 *
 * Run: node --no-warnings scripts/test_shared_banner.ts   (run-tests.sh does)
 */
import { sharedBannerWords, bannerCount, BANNER_FITTED_LEN, type AskState } from '../src/services/dialChat.ts';

let fails = 0;
function ok(cond: boolean, what: string) {
  if (cond) console.log(`  ✓ ${what}`);
  else { fails++; console.log(`  ✗ ${what}`); }
}
const room = { listeners: 3, max: 5 };
const ask = (askState: string, extra: Partial<AskState> = {}): AskState => ({ askFirst: true, tuneLocked: true, askState, ...extra } as any);
const clock = () => '14:05';

const plain = sharedBannerWords({ ...room }, false);
ok(plain === 'SHARED TUNER · ASK TO TUNE · 3/5 👤', `plain: "${plain}" (unchanged)`);
for (const [state, words, extra] of [['none', 'ASK IN CHAT TO TUNE', {}], ['open', 'AWAITING A REPLY', {}],
                                      ['no', 'YOURS TO TUNE 14:05', { askUntil: Date.now() + 60000 }]] as const) {
  for (const tight of [false, true]) {
    const t = sharedBannerWords({ ...room, ask: ask(state, extra as any) }, tight, clock);
    ok(t.includes(words) && t.endsWith('3/5 👤'), `${state}${tight ? ' (tight)' : ''}: "${t}" carries the count`);
    if (tight) {
      ok(!t.startsWith('SHARED TUNER'), `${state} (tight): the prefix goes first`);
      ok(t.length <= BANNER_FITTED_LEN, `${state} (tight): no longer than the line the SE in Display Zoom was fitted to`);
    } else ok(t.startsWith('SHARED TUNER · '), `${state}: the prefix stays where there is room`);
  }
}
const fm = sharedBannerWords({ listeners: 4, max: 0, youPlus: true, ask: ask('none') }, false);
ok(fm.endsWith('(You+3)') && fm.startsWith('SHARED TUNER · '), `FM-DX style: "${fm}"`);
ok(bannerCount({ listeners: 1, max: 1 }) === '1 👤', 'no cap: just the count');
ok(sharedBannerWords({ ...room, said: 'hello' }, false) === 'hello', 'a chat line still wins');

if (fails) { console.log(`\nshared banner: ${fails} FAILED`); process.exit(1); }
console.log('\nshared banner: all passed');
