/**
 * "Hide this user" in the chat — src/services/chatHide.ts (app; 2026-10-05).
 *
 * Proves:
 *   • ★★★ a hidden sender's lines go — past ones already drawn AND new ones — while own and system
 *     lines never do, even when they carry the same name;
 *   • the key is the name EXACTLY as the server sent it: "user5" does not take "User5" or "User5 " along;
 *   • the option is absent on your own lines (by type AND by name) and on system lines;
 *   • nothing hidden = the SAME array back (no new FlatList data on every render);
 *   • the undo line reads "N hidden" and is empty at zero (no "0 hidden" dead control);
 *   • source checks: the drawer offers Hide only outside canned mode, draws "N hidden · Show", and both
 *     host screens key the set on the server connection and keep a hidden sender out of the unread pulse;
 *     nothing in the hide path touches a socket.
 *
 * Run: node --no-warnings scripts/test_chat_hide.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';

const h = await import('../src/services/chatHide.ts');
const { canHide, isHidden, visibleMessages, withHidden, hiddenSummary, hideLabel } = h;

let fails = 0, passes = 0;
function ok(what: string, cond: boolean, detail = '') {
  if (cond) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}${detail ? `\n   ${detail}` : ''}`);
}

type M = { id: string; type: 'own' | 'other' | 'system'; user?: string; text: string };
const msgs: M[] = [
  { id: '1', type: 'other',  user: 'User2381', text: 'abuse' },
  { id: '2', type: 'other',  user: 'G4ABC',    text: 'hello' },
  { id: '3', type: 'system',                   text: 'User2381 joined the chat' },
  { id: '4', type: 'own',    user: 'ME',       text: 'hi' },
  { id: '5', type: 'other',  user: 'User2381', text: 'more abuse' },
  { id: '6', type: 'other',  user: 'user2381', text: 'a different person' },
];

// ── Filtering ─────────────────────────────────────────────────────────────────────────────────
{
  const none = new Set<string>();
  ok('nothing hidden = the same array', visibleMessages(msgs, none) === msgs);

  const hid = withHidden(none, 'User2381', 'ME');
  const v = visibleMessages(msgs, hid).map(m => m.id);
  ok('past lines from the hidden sender go', !v.includes('1') && !v.includes('5'), v.join(','));
  ok('other senders stay', v.includes('2'));
  ok('system lines stay (even naming the hidden sender)', v.includes('3'));
  ok('own lines stay', v.includes('4'));
  ok('exact match: a different-case name is a different person', v.includes('6'));

  // a NEW line arriving after the hide is filtered the same way
  const later = [...msgs, { id: '7', type: 'other' as const, user: 'User2381', text: 'new' }];
  ok('new lines from the hidden sender go too', !visibleMessages(later, hid).some(m => m.id === '7'));

  ok('isHidden ignores an own line with the hidden name',
     !isHidden({ type: 'own', user: 'User2381' }, hid));
  ok('trailing space is not the same name', !withHidden(none, 'User2381 ', 'ME').has('User2381'));
}

// ── Who may be hidden ─────────────────────────────────────────────────────────────────────────
{
  ok('somebody else may be hidden', canHide({ type: 'other', user: 'G4ABC' }, 'ME'));
  ok('never your own line', !canHide({ type: 'own', user: 'ME' }, 'ME'));
  ok('never a line carrying your own name', !canHide({ type: 'other', user: 'ME' }, 'ME'));
  ok('never a system line', !canHide({ type: 'system' }, 'ME'));
  ok('never a nameless line', !canHide({ type: 'other' }, 'ME'));
  ok('not yet joined: others still hideable', canHide({ type: 'other', user: 'G4ABC' }, null));

  const none = new Set<string>();
  ok('hiding yourself is a no-op (same set)', withHidden(none, 'ME', 'ME') === none);
  ok('hiding an empty name is a no-op', withHidden(none, '', 'ME') === none);
  const one = withHidden(none, 'A', 'ME');
  ok('hiding is immutable (input set untouched)', none.size === 0 && one.size === 1);
  ok('hiding twice keeps one entry (same set)', withHidden(one, 'A', 'ME') === one);
  ok('two hidden', withHidden(one, 'B', 'ME').size === 2);
}

// ── Labels ────────────────────────────────────────────────────────────────────────────────────
ok('undo line', hiddenSummary(2) === '2 hidden');
ok('no undo line at zero', hiddenSummary(0) === '');
ok('the action names who', hideLabel('User2381') === 'Hide User2381');

// ── Source checks ─────────────────────────────────────────────────────────────────────────────
{
  const drawer = readFileSync('src/components/ChatDrawer.tsx', 'utf8');
  ok('drawer: Hide only outside canned mode', /hideOn = !!onHideUser && !isCanned/.test(drawer));
  ok('drawer: thread draws the filtered list', /data=\{shown\}/.test(drawer));
  ok('drawer: long-press opens the strip', /onLongPress=\{\(\) => setMenuFor\(m\.id\)\}/.test(drawer));
  ok('drawer: Show line', /hiddenSummary\(hiddenUsers\.size\)\} · /.test(drawer));
  ok('drawer: set is keyed on the session', /\[sessionKey\]/.test(drawer));
  ok('chatHide sends nothing', !/send|WebSocket|fetch|AsyncStorage/.test(
    readFileSync('src/services/chatHide.ts', 'utf8').replace(/\/\*[\s\S]*?\*\/|\/\/.*$/gm, '')));
  for (const [f, key] of [['src/screens/SDRScreen.tsx', 'connectBase'], ['src/screens/TunerScreen.tsx', 'baseUrl']]) {
    const s = readFileSync(f, 'utf8');
    ok(`${f}: hidden set per connection`, new RegExp(`useHiddenChatUsers\\(${key}\\)`).test(s));
    ok(`${f}: drawer wired`, /onHideUser=/.test(s) && /onShowHidden=/.test(s) && /hiddenUsers=/.test(s));
    ok(`${f}: hidden sender never lights unread`, /chatHiddenRef\.current\.has\(/.test(s));
    ok(`${f}: nothing persisted`, !/AsyncStorage[^\n]*[Hh]idden/.test(s));
  }
}

console.log(`chat hide: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
