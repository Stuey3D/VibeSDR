// The server screen's settings are read in ONE store call (src/services/serverPrefs.ts, 2026-10-06).
// run: node --no-warnings scripts/test_server_prefs.ts
import { prefsFromPairs, readPrefs } from '../src/services/serverPrefs.ts';
let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) passes++; else { fails++; console.log(`✗ ${what}: got ${g} want ${w}`); }
};

// A stand-in for AsyncStorage that counts its calls and answers like the real one: [key, value|null] pairs.
function fakeStore(data: Record<string, string>, fail?: Error) {
  const calls: string[][] = [];
  return {
    calls,
    async multiGet(keys: readonly string[]) {
      calls.push([...keys]);
      if (fail) throw fail;
      return keys.map((k) => [k, k in data ? data[k] : null] as const);
    },
  };
}

(async () => {
  // --- ONE round trip for every key — the whole point
  {
    const s = fakeStore({ vs_pin: '4321', vs_adminpw: 'pw', vs_rate: '0' });
    const keys = ['vs_pin', 'vs_adminpw', 'vs_rate', 'vs_landingmsg', 'vs_idlegrace'];
    const p = await readPrefs(s, keys);
    eq('one store call', s.calls.length, 1);
    eq('every key asked for at once', s.calls[0], keys);
    eq('stored values come back', [p.vs_pin, p.vs_adminpw], ['4321', 'pw']);
    // ★ "0" is a REAL value (client-controlled rate) and must not read as absent.
    eq('"0" survives', p.vs_rate, '0');
    // ★ Absent is null — "never set" — so the screen can apply its default-ON rules.
    eq('absent is null, not undefined', p.vs_landingmsg, null);
    eq('every key is present in the snapshot', Object.keys(p).sort(), [...keys].sort());
  }

  // --- duplicates asked for once (the name key is shared by two readers)
  {
    const s = fakeStore({ a: '1' });
    await readPrefs(s, ['a', 'b', 'a']);
    eq('duplicates de-duplicated', s.calls[0], ['a', 'b']);
  }

  // --- an empty string is a stored value, not an absent one
  eq('empty string kept', prefsFromPairs(['x'], [['x', '']]).x, '');

  // --- a rejection is passed through, so the screen can retry it (not mistaken for "no settings")
  {
    const s = fakeStore({}, new Error('database is locked'));
    let msg = '';
    try { await readPrefs(s, ['a']); } catch (e: any) { msg = e.message; }
    eq('rejection passes through', msg, 'database is locked');
  }

  // --- robustness against a store that answers oddly
  eq('null pairs', prefsFromPairs(['a'], null), { a: null });
  eq('unasked key ignored', prefsFromPairs(['a'], [['a', '1'], ['zz', '2']]), { a: '1' });
  eq('null value', prefsFromPairs(['a'], [['a', null]]), { a: null });

  console.log(fails ? `✗ server prefs: ${fails} failed, ${passes} passed` : `✓ server prefs: ${passes} passed`);
  process.exit(fails ? 1 : 0);
})();
